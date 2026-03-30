// LinkFlow Phase 3: UDP Server (Receiver)
// File: src/server.cpp

#include <iostream>
#include <cstring>
#include <unistd.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <fstream>
#include <map>
#include <sys/stat.h>
#include "./headers/packet.h"
#include "./headers/arq.h"

using namespace std;

// Create directory if it doesn't exist
void create_received_dir() {
    struct stat st = {0};
    if (stat("received", &st) == -1) {
        mkdir("received");
    }
}

int main() {
    cout << "📡 LinkFlow Phase 3 Server Starting..." << endl;

    // Initialize Winsock
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        cerr << "❌ WSAStartup failed." << endl;
        return 1;
    }

    create_received_dir();

    // 1. Create UDP Socket
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        cerr << "❌ Socket creation failed." << endl;
        return 1;
    }
    cout << "✅ UDP socket created (fd=" << sockfd << ")" << endl;

    // 2. Server Address Setup
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    if (bind(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        cerr << "❌ Bind failed." << endl;
        return 1;
    }
    cout << "🎧 Listening on port " << PORT << "..." << endl;

    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);

    uint16_t expected_seq_num = 1;
    map<uint16_t, Packet> receive_buffer;
    bool is_receiving = true;
    string current_filename = "";

    while (is_receiving) {
        Packet pkt;
        memset(&pkt, 0, sizeof(pkt));

        // Wait for incoming packet
        int bytes_recv = recvfrom(sockfd, (char *)&pkt, sizeof(pkt), 0,
                                  (struct sockaddr *)&client_addr, &addr_len);

        if (bytes_recv > 0) {
            // Task: Packet Deserialization
            deserialize_packet(&pkt);

            // Task: CRC Validation
            // The client calculates CRC32 on `compressed_len`, which is not transmitted in the header.
            // Since DATA_SIZE is small (1000), we can brute-force the length to find the matching CRC 
            // and validate that the data is not corrupted.
            uint32_t expected_crc = pkt.crc32;
            bool crc_valid = false;
            size_t actual_compressed_len = DATA_SIZE;

            for (size_t len = 0; len <= DATA_SIZE; len++) {
                if (calculate_crc32((unsigned char*)pkt.data, len) == expected_crc) {
                    crc_valid = true;
                    actual_compressed_len = len;
                    break;
                }
            }

            char decompressed_data[DATA_SIZE];
            size_t decompressed_len = DATA_SIZE;

            if (crc_valid) {
                // Task: ACK Generation (Specialized ACK packet type=1)
                Packet ack_pkt;
                memset(&ack_pkt, 0, sizeof(ack_pkt));
                ack_pkt.type = 1; // ACK Type
                ack_pkt.ack_num = pkt.seq_num + 1; // Next expected
                serialize_packet(&ack_pkt);

                sendto(sockfd, (const char *)&ack_pkt, sizeof(ack_pkt), 0,
                       (struct sockaddr *)&client_addr, addr_len);

                cout << "✅ Received seq=" << pkt.seq_num << ", Sending ACK=" << ack_pkt.ack_num << endl;

                // Task: File Reassembly
                if (pkt.seq_num == expected_seq_num) {
                    current_filename = pkt.filename;
                    string out_filepath = "received/recv_" + current_filename;
                    
                    // We must determine the size to write. 
                    // If it's the last packet (type=3), we write the remaining file_size%DATA_SIZE 
                    // or just the decompressed size.
                    size_t bytes_to_write = DATA_SIZE;
                    // Let's attempt decompression because the client sent compressed data
                    int decomp_res = decompress_data(pkt.data, DATA_SIZE, decompressed_data, decompressed_len);
                    if (decomp_res == 0) {
                        cout << "📦 Decompressed " << decompressed_len << " bytes" << endl;
                    } else {
                        // Fallback to raw data if not compressed properly
                        memcpy(decompressed_data, pkt.data, DATA_SIZE);
                        decompressed_len = DATA_SIZE;
                    }
                    
                    if (pkt.type == 3) {
                        // Last packet
                        bytes_to_write = pkt.file_size - pkt.chunk_offset;
                        if (bytes_to_write > decompressed_len) bytes_to_write = decompressed_len;
                    } else {
                        bytes_to_write = decompressed_len;
                    }

                    ofstream outfile;
                    if (pkt.seq_num == 1) {
                        outfile.open(out_filepath, ios::binary | ios::trunc); // fresh file
                    } else {
                        outfile.open(out_filepath, ios::binary | ios::app); // append
                    }

                    if (outfile.is_open()) {
                        outfile.write(decompressed_data, bytes_to_write);
                        outfile.close();
                    }

                    expected_seq_num++;

                    // Check buffer for out-of-order packets that are now in sequence
                    while (receive_buffer.find(expected_seq_num) != receive_buffer.end()) {
                        Packet buffered_pkt = receive_buffer[expected_seq_num];
                        
                        size_t buf_decomp_len = DATA_SIZE;
                        char buf_decomp_data[DATA_SIZE];
                        decompress_data(buffered_pkt.data, DATA_SIZE, buf_decomp_data, buf_decomp_len);

                        size_t write_len = (buffered_pkt.type == 3) ? (buffered_pkt.file_size - buffered_pkt.chunk_offset) : buf_decomp_len;
                        
                        outfile.open(out_filepath, ios::binary | ios::app);
                        outfile.write(buf_decomp_data, write_len);
                        outfile.close();
                        
                        receive_buffer.erase(expected_seq_num);
                        expected_seq_num++;

                        if (buffered_pkt.type == 3) {
                            is_receiving = false;
                            cout << "🎉 File transfer completed successfully!" << endl;
                        }
                    }

                    if (pkt.type == 3) {
                        is_receiving = false;
                        cout << "🎉 File transfer completed successfully!" << endl;
                    }
                } else if (pkt.seq_num > expected_seq_num) {
                    // Buffer out-of-order packets (Though Go-Back-N usually discards them, 
                    // buffering is an optimization for Selective Repeat. We buffer them here).
                    receive_buffer[pkt.seq_num] = pkt;
                }
            } else {
                cout << "❌ CRC Validation failed for seq=" << pkt.seq_num << endl;
            }
        }
    }

    close(sockfd);
    WSACleanup();
    return 0;
}
