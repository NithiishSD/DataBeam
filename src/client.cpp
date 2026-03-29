// LinkFlow Phase 1.2: UDP Client Implementation with File Transfer
// File: src/client.cpp
// Purpose: Send file in 1000-byte chunks with ARQ and CRC32 validation

#include <iostream>
#include <cstring>
#include <iomanip>
#include <unistd.h>
#include <ws2tcpip.h>
#include <winsock2.h>
#include <fstream>
#include <chrono>
#include "./headers/packet.h"

using namespace std;

const int TIMEOUT_MS = 500; // 500ms timeout for ARQ
const int MAX_RETRIES = 3;  // Max retransmission attempts

// Function to read a file and return its size
long get_file_size(const char *filename)
{
    ifstream file(filename, ios::binary | ios::ate);
    if (!file.is_open())
        return -1;
    return file.tellg();
}

// Function to read file chunk at given offset
bool read_file_chunk(const char *filename, uint32_t offset, char *buffer, size_t &bytes_read)
{
    ifstream file(filename, ios::binary);
    if (!file.is_open())
        return false;

    file.seekg(offset);
    file.read(buffer, DATA_SIZE);
    bytes_read = file.gcount();
    return true;
}

int main(int argc, char *argv[])
{
    cout << "📡 LinkFlow Client Starting..." << endl;

    // Validate command line arguments
    if (argc < 2)
    {
        cerr << "Usage: " << argv[0] << " <filename>" << endl;
        return 1;
    }

    const char *filename = argv[1];
    long file_size = get_file_size(filename);

    if (file_size < 0)
    {
        cerr << "❌ Cannot open file: " << filename << endl;
        return 1;
    }

    cout << "📁 File: " << filename << " (" << file_size << " bytes)" << endl;

    // STEP 1: Create UDP Socket
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0)
    {
        cerr << "❌ Socket creation failed: " << strerror(errno) << endl;
        return 1;
    }
    cout << "✅ UDP socket created (fd=" << sockfd << ")" << endl;

    // STEP 2: Set socket timeout to 500ms for recvfrom
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = TIMEOUT_MS * 1000; // 500ms
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));

    // STEP 3: Server Address Setup
    struct sockaddr_in server_addr;
    socklen_t addr_len = sizeof(server_addr);
    memset(&server_addr, 0, sizeof(server_addr));

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);

    if (inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr) <= 0)
    {
        cerr << "❌ Invalid address" << endl;
        close(sockfd);
        return 1;
    }
    cout << "🔗 Server address configured: 127.0.0.1:" << PORT << endl;

    // STEP 4: Send file in chunks with Stop-and-Wait ARQ
    uint16_t seq_num = 1;
    uint32_t chunk_offset = 0;
    int total_chunks = (file_size + DATA_SIZE - 1) / DATA_SIZE;
    int chunks_sent = 0;

    cout << "\n📤 Starting file transmission (" << total_chunks << " chunks)...\n"
         << endl;

    while (chunk_offset < file_size)
    {
        // Read chunk from file
        char chunk_data[DATA_SIZE];
        memset(chunk_data, 0, DATA_SIZE);
        size_t bytes_read = 0;

        if (!read_file_chunk(filename, chunk_offset, chunk_data, bytes_read))
        {
            cerr << "❌ Error reading file at offset " << chunk_offset << endl;
            close(sockfd);
            return 1;
        }

        // Create packet
        struct Packet pkt;
        memset(&pkt, 0, sizeof(pkt));

        // Core fields
        pkt.seq_num = seq_num;
        pkt.ack_num = 0;
        pkt.type = (chunk_offset + bytes_read >= file_size) ? 3 : 0; // 3=END, 0=DATA

        // File transfer info
        strcpy(pkt.filename, filename);
        pkt.file_size = file_size;
        pkt.chunk_offset = chunk_offset;

        // Calculate CRC32 of data payload
        pkt.crc32 = calculate_crc32((unsigned char *)chunk_data, bytes_read);

        // Copy payload
        memcpy(pkt.data, chunk_data, bytes_read);

        // Metadata
        strcpy(pkt.username, "client_user");
        pkt.stream_id = 0;
        pkt.window_size = 1; // Stop-and-Wait = window of 1

        // Serialize packet (convert to network byte order)
        struct Packet pkt_copy = pkt;
        serialize_packet(&pkt_copy);

        // Stop-and-Wait ARQ with retransmission
        bool ack_received = false;
        int retry_count = 0;

        while (!ack_received && retry_count < MAX_RETRIES)
        {
            // Send packet
            cout << "📨 Sending packet " << seq_num << ": offset=" << chunk_offset
                 << " bytes=" << bytes_read;

            if (retry_count > 0)
                cout << " (retry " << retry_count << ")";
            cout << endl;

            int bytes_sent = sendto(sockfd, (const char *)&pkt_copy, sizeof(pkt_copy), 0,
                                    (struct sockaddr *)&server_addr, addr_len);

            if (bytes_sent < 0)
            {
                cerr << "❌ sendto failed: " << strerror(errno) << endl;
                close(sockfd);
                return 1;
            }

            // Wait for ACK with timeout
            struct Packet ack_pkt;
            memset(&ack_pkt, 0, sizeof(ack_pkt));

            int bytes_recv = recvfrom(sockfd, (char *)&ack_pkt, sizeof(ack_pkt), 0,
                                      (struct sockaddr *)&server_addr, &addr_len);

            if (bytes_recv < 0)
            {
                if (WSAGetLastError() == WSAETIMEDOUT)
                {
                    cout << "⏱️  Timeout! No ACK received within " << TIMEOUT_MS << "ms" << endl;
                    retry_count++;
                    continue;
                }
                else
                {
                    cerr << "❌ recvfrom failed: " << strerror(errno) << endl;
                    close(sockfd);
                    return 1;
                }
            }

            // Deserialize ACK packet
            deserialize_packet(&ack_pkt);

            // Verify ACK
            if (ack_pkt.type == 1 && ack_pkt.ack_num == seq_num) // 1=ACK
            {
                cout << "✅ ACK received for packet " << seq_num << endl;
                ack_received = true;
            }
            else
            {
                cout << "⚠️  Invalid ACK (expected seq=" << seq_num
                     << ", got ack_num=" << ack_pkt.ack_num << ")" << endl;
                retry_count++;
            }
        }

        if (!ack_received)
        {
            cerr << "❌ Failed to send packet " << seq_num << " after "
                 << MAX_RETRIES << " retries" << endl;
            close(sockfd);
            return 1;
        }

        // Update for next chunk
        chunk_offset += bytes_read;
        seq_num++;
        chunks_sent++;
        cout << endl;
    }

    close(sockfd);
    cout << "\n✅ File transfer COMPLETE!" << endl;
    cout << "📊 Summary:" << endl;
    cout << "   - Total chunks sent: " << chunks_sent << endl;
    cout << "   - Total bytes: " << file_size << endl;
    cout << "   - All ACKs received: SUCCESS" << endl;

    return 0;
}
