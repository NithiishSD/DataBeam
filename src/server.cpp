#include <iostream>
#include <cstring>
#include <fstream>
#include <sstream>
#include <map>
#include <vector>
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#include <sys/stat.h>
#include <queue>
#include <pthread.h>
#include "./headers/packet.h"
#include "./headers/arq.h"
#include "./headers/compress.h"
#include "./headers/crchw.h"
#include "./headers/crypto.h"
using namespace std;

// Work queue — holds CRC-verified raw packets waiting to be decompressed
struct WorkItem
{
    Packet pkt;
    size_t compressed_len;
};

queue<WorkItem> work_queue;
pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t queue_cond = PTHREAD_COND_INITIALIZER;
volatile bool server_done = false;

void create_received_dir()
{
    struct stat st = {0};
    if (stat("received", &st) == -1)
        mkdir("received");
}

void save_checkpoint(const string &filename, uint32_t total_chunks, uint16_t expected_seq, const bool* slot_occupied)
{
    ofstream out("resume.json");
    if (out.is_open())
    {
        out << "{\n";
        out << "  \"filename\": \"" << filename << "\",\n";
        out << "  \"total_chunks\": " << total_chunks << ",\n";
        out << "  \"last_seq\": " << (expected_seq > 0 ? expected_seq - 1 : 0) << ",\n";
        out << "  \"received_chunks\": [";
        bool first = true;
        if (slot_occupied != nullptr) {
            for (int i = 0; i < 1024; i++) {
                if (slot_occupied[i]) {
                    if (!first) out << ", ";
                    uint16_t dist = (i - (expected_seq % 1024) + 1024) % 1024;
                    out << (expected_seq + dist);
                    first = false;
                }
            }
        }
        out << "]\n}\n";
    }
}

uint16_t load_checkpoint(const string &target_filename)
{
    ifstream in("resume.json");
    if (!in.is_open())
        return 1;
    stringstream buf;
    buf << in.rdbuf();
    string content = buf.str();
    size_t f_pos = content.find("\"filename\"");
    size_t s_pos = content.find("\"last_seq\"");
    if (f_pos == string::npos || s_pos == string::npos)
        return 1;
    size_t f_start = content.find("\"", f_pos + 10) + 1;
    size_t f_end = content.find("\"", f_start);
    string saved = content.substr(f_start, f_end - f_start);
    if (saved != target_filename)
        return 1;
    size_t s_start = content.find(":", s_pos) + 1;
    while (isspace(content[s_start]))
        s_start++;
    size_t s_end = content.find_first_of(",}", s_start);
    while (s_end > s_start && isspace(content[s_end - 1]))
        s_end--;
    return (uint16_t)stoi(content.substr(s_start, s_end - s_start)) + 1;
}

vector<uint32_t> compute_file_hashes(const string &filepath)
{
    vector<uint32_t> hashes;
    ifstream file(filepath, ios::binary);
    if (!file.is_open())
        return hashes;
    char buffer[DATA_SIZE];
    while (file.read(buffer, DATA_SIZE) || file.gcount() > 0)
    {
        size_t n = file.gcount();
        hashes.push_back(calculate_crc32(
            reinterpret_cast<unsigned char *>(buffer), n));
    }
    return hashes;
}

// =============================================================================
// Decoded packet — stores decompressed payload so the buffer drain loop
// never has to decompress or CRC-check twice
// =============================================================================
struct DecodedPacket
{
    char data[DATA_SIZE + 1];
    size_t data_len;
    uint8_t type;
    uint32_t file_size;
    uint32_t chunk_offset;
};

void *worker_thread(void *arg)
{
    // All file state lives here — only this thread touches the file
    DecodedPacket *memory_pool = new DecodedPacket[1024];

    // Track which slots are currently "occupied" with data
    bool slot_occupied[1024];
    memset(slot_occupied, false, sizeof(slot_occupied));
    ofstream outfile;
    string out_filepath = "";
    string current_filename = "";
    uint16_t expected_seq_num = 1;
    bool connection_init = false;

    while (!server_done)
    {
        // ---- Pop from queue ------------------------------------------------
        WorkItem item;
        {
            pthread_mutex_lock(&queue_mutex);
            while (work_queue.empty() && !server_done)
                pthread_cond_wait(&queue_cond, &queue_mutex);
            if (server_done && work_queue.empty())
            {
                pthread_mutex_unlock(&queue_mutex);
                break;
            }
            item = work_queue.front();
            work_queue.pop();
            pthread_mutex_unlock(&queue_mutex);
        }

        Packet &pkt = item.pkt;

        // ---- File init (mirrors network thread logic) ----------------------
        if (!connection_init || string(pkt.filename) != current_filename)
        {
            if (outfile.is_open())
            {
                outfile.flush();
                outfile.close();
            }
            current_filename = pkt.filename;
            out_filepath = "received/recv_" + current_filename;
            
            // Check for checkpoint
            expected_seq_num = load_checkpoint(current_filename);
            
            if (expected_seq_num > 1) {
                outfile.open(out_filepath, ios::binary | ios::out | ios::app);
                cout << "[WORKER] Resuming receiving " << current_filename << " from seq=" << expected_seq_num << endl;
            } else {
                outfile.open(out_filepath, ios::binary | ios::out | ios::trunc);
            }
            
            if (!outfile.is_open())
            {
                cerr << "[WORKER] Cannot create/open: " << out_filepath << endl;
                continue;
            }
            // Instead of receive_buffer.clear();
            memset(slot_occupied, false, sizeof(slot_occupied));
            connection_init = true;
        }

        // ---- Decrypt -------------------------------------------------------
        char decrypted_data[DATA_SIZE + 1];
        if (!aes_decrypt((const uint8_t*)pkt.data, item.compressed_len, SHARED_SECRET_KEY, pkt.iv, (uint8_t*)decrypted_data)) {
            cerr << "[WORKER] Decrypt failed seq=" << pkt.seq_num << endl;
            continue;
        }

        // ---- Decompress ----------------------------------------------------
        DecodedPacket decoded;
        decoded.data_len = sizeof(decoded.data);
        int ret = decompress_data(decrypted_data, item.compressed_len,
                                  decoded.data, decoded.data_len);
        if (ret != 0)
        {
            // This should never happen — CRC passed in network thread
            // meaning data arrived intact. If decompress fails here
            // it means compress_data produced data that decompress_data
            // can't handle — that's a compress.h bug, not a network bug.
            cerr << "[WORKER] Decomp failed seq=" << pkt.seq_num
                 << " err=" << ret << endl;
            continue;
        }

        decoded.type = pkt.type;
        decoded.file_size = pkt.file_size;
        decoded.chunk_offset = pkt.chunk_offset;

        // ---- Buffer or write (same logic as before) ------------------------
        if (pkt.seq_num == expected_seq_num)
        {
            size_t remaining = (size_t)decoded.file_size - decoded.chunk_offset;
            size_t write_len = min(decoded.data_len, remaining);

            outfile.write(decoded.data, (streamsize)write_len);
            if (outfile.fail())
            {
                cerr << "[WORKER] Write failed seq=" << pkt.seq_num << endl;
                outfile.clear();
            }

            expected_seq_num++;
            if (expected_seq_num % 100 == 0)
            {
                uint32_t tc = (decoded.file_size + DATA_SIZE - 1) / DATA_SIZE;
                save_checkpoint(current_filename, tc, expected_seq_num, slot_occupied);
            }
            // save_checkpoint(current_filename, expected_seq_num);

            // Drain buffer
            while (slot_occupied[expected_seq_num % 1024])
            {
                int idx = expected_seq_num % 1024;
                DecodedPacket &bp = memory_pool[idx];

                // 1. Calculate length and write
                size_t bp_remaining = (size_t)bp.file_size - bp.chunk_offset;
                size_t bp_write_len = min(bp.data_len, bp_remaining);

                outfile.write(bp.data, (streamsize)bp_write_len);

                if (outfile.fail())
                {
                    save_checkpoint(current_filename, expected_seq_num);
                    cerr << "[WORKER] Write failed buffered seq=" << expected_seq_num << endl;
                    outfile.clear();
                    break;
                }

                // 2. Performance: Batch Checkpointing (Crucial for 1GB!)
                // Only write to disk (resume.json) every 100 chunks to save I/O time
                if (expected_seq_num % 100 == 0)
                {
                    uint32_t tc = (bp.file_size + DATA_SIZE - 1) / DATA_SIZE;
                    save_checkpoint(current_filename, tc, expected_seq_num, slot_occupied);
                }

                // 3. Mark the end condition
                bool is_end = (bp.type == 3);

                // 4. Free the slot for the next "lap" of the circular buffer
                slot_occupied[idx] = false;
                expected_seq_num++;

                if (is_end)
                {
                    outfile.flush();
                    outfile.close();
                    connection_init = false;
                    remove("resume.json");
                    cout << "[WORKER] Transfer complete: " << out_filepath << endl;
                    server_done = true;
                    break;
                }
            }

            if (decoded.type == 3 && outfile.is_open())
            {
                outfile.flush();
                outfile.close();
                connection_init = false;
                remove("resume.json");
                cout << "[WORKER] Transfer complete: " << out_filepath << endl;
                server_done = true;
            }
        }
        else if (pkt.seq_num > expected_seq_num)
        {
            int idx = pkt.seq_num % 1024;

            // Safety: Only store if we aren't already holding this packet
            if (!slot_occupied[idx])
            {
                // Direct copy into the pre-allocated slot
                memory_pool[idx] = decoded;
                slot_occupied[idx] = true;
            }
        }
    }

    return nullptr;
}
int main()
{
    cout << " LinkFlow Phase 4 Server Starting..." << endl;
    SetConsoleOutputCP(CP_UTF8);

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
    {
        cerr << " WSAStartup failed." << endl;
        return 1;
    }

    create_received_dir();

    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0)
    {
        cerr << " Socket failed." << endl;
        return 1;
    }

    // Large socket buffers — absorb bursts without dropping
    int buf_size = 32 * 1024 * 1024;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVBUF, (char *)&buf_size, sizeof(buf_size));
    setsockopt(sockfd, SOL_SOCKET, SO_SNDBUF, (char *)&buf_size, sizeof(buf_size));

    // FIX: 1ms recv timeout — server must be responsive for ACK sending
    struct timeval tv = {0, 1000};
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));

    struct sockaddr_in server_addr = {};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    if (bind(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        cerr << " Bind failed." << endl;
        return 1;
    }
    cout << " Listening on port " << PORT << "..." << endl;

    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);

    uint16_t expected_seq_num = 1;
    bool is_receiving = true;
    bool connection_initialized = false;
    string current_filename = "";
    string out_filepath = ""; // FIX: single declaration, no shadowing

    // FIX: buffer stores DecodedPacket — already CRC-checked and decompressed
    // No duplicate work when draining
    DecodedPacket *fast_buffer[512] = {nullptr};

    ofstream outfile;
    pthread_t t_worker;
    pthread_create(&t_worker, nullptr, worker_thread, nullptr);
    while (is_receiving)
    {
        Packet pkt;
        memset(&pkt, 0, sizeof(pkt));

        int bytes_recv = recvfrom(sockfd, (char *)&pkt, sizeof(pkt), 0,
                                  (struct sockaddr *)&client_addr, &addr_len);

        if (bytes_recv <= 0)
            continue; // timeout or error — loop again
            
        // ----- HMAC Verification -----
        uint8_t received_hmac[32];
        memcpy(received_hmac, pkt.hmac, 32);
        memset(pkt.hmac, 0, 32); // zero for verification
        
        if (!verify_hmac((const uint8_t*)&pkt, sizeof(pkt), SHARED_SECRET_KEY, received_hmac)) {
            cerr << "[SERVER] HMAC VERIFICATION FAILED! Tampering detected. seq=" << ntohs(pkt.seq_num) << endl;
            continue; // drop packet silently
        }
        
        memcpy(pkt.hmac, received_hmac, 32); // restore

        deserialize_packet(&pkt);

        // ---- Validate data_len --------------------------------------------
        size_t compressed_len = pkt.data_len;
        if (compressed_len == 0 || compressed_len > (size_t)(DATA_SIZE + 1))
        {
            cerr << "[SERVER] Bad data_len=" << compressed_len
                 << " seq=" << pkt.seq_num << endl;
            continue;
        }

        // ---- Send ACK immediately after length check (before heavy work) --
        // FIX: ACK sent as small struct, not full Packet — reduces ACK latency
        // FIX: ACK sent BEFORE decode so the client's RTO doesn't expire
        //      while we decompress
        // ---- CRC check — must pass before ACK ------------------------------------
        uint32_t computed = calculate_crc32(
            reinterpret_cast<const unsigned char *>(pkt.data),
            compressed_len);

        if (computed != pkt.crc32)
        {
            cerr << "[SERVER] CRC FAIL seq=" << pkt.seq_num
                 << " expected=0x" << hex << pkt.crc32
                 << " got=0x" << computed << dec << endl;
            continue; // don't ACK — client will retransmit
        }
        {
            Packet ack_pkt;
            memset(&ack_pkt, 0, sizeof(ack_pkt));
            ack_pkt.type = 1;
            ack_pkt.ack_num = pkt.seq_num;
            ack_pkt.bitmap = (uint16_t)(1u << (pkt.seq_num % 16));
            serialize_packet(&ack_pkt);
            sendto(sockfd, (const char *)&ack_pkt, sizeof(ack_pkt), 0,
                   (struct sockaddr *)&client_addr, addr_len);
        }
        {
            WorkItem item;
            item.pkt = pkt;
            item.compressed_len = compressed_len;

            pthread_mutex_lock(&queue_mutex);
            work_queue.push(item);
            pthread_cond_signal(&queue_cond); // wake worker thread
            pthread_mutex_unlock(&queue_mutex);
        }

        // pkt.seq_num < expected_seq_num → duplicate, already processed, ignore
    }
    server_done = true;
    pthread_cond_signal(&queue_cond); // wake worker if it's waiting
    pthread_join(t_worker, nullptr);
    outfile.flush();
    outfile.close();
    closesocket(sockfd);
    WSACleanup();
    return 0;
}
