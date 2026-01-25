

#include <iostream>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include "packet.h"

using namespace std;

int main(int argc, char *argv[])
{
    cout << "🚀 LinkFlow Client Starting..." << endl;

    // STEP 2: Create UDP Socket
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0)
    {
        cerr << "❌ Socket creation failed: " << strerror(errno) << endl;
        return 1;
    }
    cout << "✅ UDP socket created (fd=" << sockfd << ")" << endl;

    // STEP 3: Server Address Setup
    struct sockaddr_in server_addr;
    socklen_t addr_len = sizeof(server_addr);

    // Zero out structure
    memset(&server_addr, 0, sizeof(server_addr));

    // Fill server details
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT); // From packet.h

    // Convert IP address from text to binary
    if (inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr) <= 0)
    {
        cerr << "❌ Invalid address" << endl;
        close(sockfd);
        return 1;
    }
    cout << "✅ Server address configured: 127.0.0.1:" << PORT << endl;

    // STEP 4: Create & Fill FIRST Packet
    struct Packet pkt;
    memset(&pkt, 0, sizeof(pkt));

    // Fill packet fields (matching your packet.h structure)
    pkt.seq_num = 1;                      // First packet
    pkt.type = 0;                         // Data packet (0=data, 1=ACK)
    strcpy(pkt.data, "Phase 1.2 Hello!"); // Test message
    strcpy(pkt.filename, "test.txt");     // Dummy filename

    cout << "📦 Packet prepared:" << endl;
    cout << "   - Sequence: " << pkt.seq_num << endl;
    cout << "   - Type: " << pkt.type << " (DATA)" << endl;
    cout << "   - Data: " << pkt.data << endl;
    cout << "   - Filename: " << pkt.filename << endl;

    // STEP 5: Send Packet
    cout << "\n📤 Sending packet to server..." << endl;
    int bytes_sent = sendto(sockfd, &pkt, sizeof(pkt), 0,
                            (struct sockaddr *)&server_addr, addr_len);

    if (bytes_sent < 0)
    {
        cerr << "❌ sendto failed: " << strerror(errno) << endl;
        close(sockfd);
        return 1;
    }
    cout << "✅ Sent " << bytes_sent << " bytes to server" << endl;

    // STEP 5: Receive ACK
    cout << "\n⏳ Waiting for ACK from server..." << endl;
    struct Packet ack_pkt;
    memset(&ack_pkt, 0, sizeof(ack_pkt));

    int bytes_recv = recvfrom(sockfd, &ack_pkt, sizeof(ack_pkt), 0,
                              (struct sockaddr *)&server_addr, &addr_len);

    if (bytes_recv < 0)
    {
        cerr << "❌ recvfrom failed: " << strerror(errno) << endl;
        close(sockfd);
        return 1;
    }

    cout << "✅ ACK received (" << bytes_recv << " bytes):" << endl;
    cout << "   - ACK Number: " << ack_pkt.ack_num << endl;
    cout << "   - Type: " << ack_pkt.type << endl;

    // Verify ACK matches sent packet
    if (ack_pkt.ack_num == pkt.seq_num)
    {
        cout << "✅ ACK verification PASSED (seq=" << pkt.seq_num << ")" << endl;
    }
    else
    {
        cout << "⚠️  ACK mismatch: expected " << pkt.seq_num
             << ", got " << ack_pkt.ack_num << endl;
    }

    // STEP 6: Cleanup
    close(sockfd);
    cout << "\n🎉 Phase 1.2 COMPLETE!" << endl;
    cout << "📊 Summary:" << endl;
    cout << "   - Socket operations: SUCCESS" << endl;
    cout << "   - Packet transmission: SUCCESS" << endl;
    cout << "   - ACK reception: SUCCESS" << endl;

    return 0;
}
