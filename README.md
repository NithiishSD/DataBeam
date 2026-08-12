# DataBeam

DataBeam is a high-performance, UDP-based reliable file transfer project that implements ARQ (automatic repeat request) protocols, real-time compression, RTT-adaptive retransmission timers, and congestion control. It is designed as a pipelined client that can efficiently stream file chunks over lossy networks while minimizing latency and maximizing throughput.

This README explains the project's goals, architecture, important files, how to build and run the client, configuration options, testing guidance, performance characteristics, and next steps for future phases.

---

## What this is (short)

A high-throughput UDP file-transfer client that implements Selective Repeat and Go-Back-N ARQ techniques (phases evolved across the repo), zlib compression, microsecond RTT sampling, adaptive RTO (RFC 6298 style), and AIMD congestion control. Intended for developers and researchers building/experimenting with reliable transport over UDP.

### Stack
- Language(s): C++ (C++11 / C++17 features used)
- Primary runtime: Native CLI executable (Windows-focused code paths present; POSIX compatibility where feasible)
- Notable libraries and dependencies:
  - zlib (compression)
  - pthreads (threading) — used via POSIX APIs in the code
  - Winsock2/WSA extensions (Windows network APIs; WSASendMsg used when available)
  - AES/HMAC primitives (in repo headers for encryption/HMAC)

---

## How it's organized

Top-level important entries (annotated):

```
src/                       # Main C++ source
  client.cpp               # Client entrypoint: 3-stage pipeline, sender/receiver/timeout threads
  arq.cpp                  # GoBackNARQ implementation (RTT, RTO, AIMD) and helpers
  headers/                 # Header files for packet layout, ARQ declarations, constants
    arq.h                  # GoBackNARQ declaration (sent buffer, RTT vars, cwnd)
    packet.h               # Packet formats: SlimDataPacket, ACKPacket, StartPacket, serializers
    ...                    # other headers (compress.h, crypto.h, etc.) referenced by client
IMPLEMENTATION_COMPLETE.md  # Phase 3 implementation summary (detailed)
README.md                   # This file (overview and instructions)
QUICKSTART.md (may exist)    # Quickstart and usage notes (if present in repo)
```

How it fits together (runtime shape):
- client.cpp implements a 3-stage pipeline:
  1. Dispatcher: reads file chunks (memory-mapped on Windows) and produces RawChunk entries.
  2. Compressor/crypto pool: compresses (zlib), encrypts, computes CRC/HMAC and prepares ReadyPacket entries.
  3. Sender: reorders packets (zero-allocation ReorderBuf), enqueues to the ARQ manager and sends via UDP (batched when WSASendMsg is available).
- Receiver thread parses ACK/Start/Probe responses, updates ARQ state and RTT measurements.
- Timeout thread checks ARQ timers and triggers retransmissions when RTO expires.

Notes: The repository contains both a Go-Back-N implementation (src/arq.cpp / src/headers/arq.h) and a SelectiveRepeatARQ type referenced in src/client.cpp; the client code uses a Selective Repeat style SACK bitmap layout (see src/headers/packet.h), so the project has evolved from pure GBN to a Selective Repeat-capable pipeline in later phases.

---

## Protocol & Packet formats

Key packet structures (detailed in src/headers/packet.h):

- SlimDataPacket (data packet)
  - Fields: type, seq_num (32-bit), crc32, data_len (16-bit), flags, connection_id, chunk_offset, packet_iv, hmac[16], data[payload]
  - Data payload size governed by DataBeam::PACKET_DATA_SIZE constant in headers.
  - Serialization/byte-order helpers present (serialize_slim_packet / deserialize_slim_packet).

- ACKPacket (receiver → sender)
  - Cumulative ack_num (next expected seq number) plus SACK bitmap stored as multiple 64-bit chunks (full selective acknowledgements), type and connection id, CRC32.
  - compute_ack_crc() and byte-order helpers present.

- StartPacket (client → server handshake)
  - type=2, file_size, total_chunks, filename, username, initial window_size, connection_id.

CRC and HMAC
- The client computes crc32 over the packet struct and uses an HMAC (16 bytes) for packet authentication before sending. Packet serialization converts multi-byte fields to network byte order before HMAC is calculated.

Compression
- zlib compression is used for per-chunk compression; packets are marked compressed in the flags only if compression reduces size. Binary media (mp4, mkv, jpg, png, zip, gz...) are auto-detected and compression may be disabled.

---

## Key design points and algorithms

- 3-Stage Pipeline: dispatcher → compressors → sender. This allows file I/O, CPU-bound compression/crypto, and network I/O to run concurrently and scale with CPU cores.

- ARQ and Congestion Control:
  - The repo contains a Go-Back-N implementation (GoBackNARQ) with a WINDOW_SIZE constant (DataBeam::GBN_WINDOW_SIZE) and adaptive RTO using RFC 6298-style EWMA SRTT and RTTVAR (α=0.125, β=0.25).
  - AIMD congestion control implemented: slow-start (exponential) until ssthresh, then additive increase; multiplicative decrease (halve cwnd) on timeout/loss.
  - The client pipeline references a SelectiveRepeatARQ with SACK bitmap support and fast retransmit behaviors (the code handles SACK bitmaps, marking packets individually and scheduling fast retransmits).

- RTT & RTO:
  - RTT measured using std::chrono::high_resolution_clock with microsecond resolution.
  - SRTT/RTTVAR tracked in microseconds; RTO computed as SRTT + 4*RTTVAR and bounded.

- Performance tuning:
  - Uses WSASendMsg / UDP batching when available (USO) on Windows to reduce syscall overhead.
  - Zero-allocation ReorderBuf (power-of-two capacity) to avoid heap churn.
  - Memory-mapped file read on Windows (CreateFileMapping + MapViewOfFile) for fast sequential reads.

---

## Build instructions

Prerequisites
- C++ compiler with C++11 / C++17 support (g++ / MSVC). Example testing uses g++ on Windows (MinGW/MSYS).
- zlib development headers and library (--link -lz)
- Windows: Winsock2 is required (-lws2_32) and some Windows-specific APIs are used in the code.
- pthreads (POSIX) is used in the source; on Windows you may compile using pthreads-w32 or adapt to native threads.

Example (Windows MinGW/MSYS) build command shown in repo:

```bash
cd c:\path\to\DataBeam
g++ -fdiagnostics-color=always -g src/client.cpp src/arq.cpp -Isrc -lws2_32 -lz -o client.exe
```

Notes:
- If you compile on Linux, remove or adapt Winsock-specific calls and link with pthreads.
- Ensure the include path includes src/headers if your build layout differs.

---

## Minimal run (quickstart)

1. Start the receiver/server component (server binary must be present and listening on default port). The client currently sends to 127.0.0.1 by default in the code; change server_addr in client.cpp for remote hosts.

2. Run client:

```bash
./client.exe path/to/file.bin
```

What happens:
- Client sends StartPacket handshake and waits for server ACK.
- Client performs a probe (packet train) to estimate bandwidth and RTT, and initializes cwnd accordingly.
- The 3-stage pipeline dispatches chunks, compressors prepare wire packets, and sender transmits packets obeying ARQ and cwnd constraints.
- Receiver thread processes ACKs and SACK bitmaps, updates RTT and cwnd, and triggers fast retransmits when holes are observed.
- Timeout thread manages retransmissions when RTO expires.

---

## Configuration and constants

Most runtime constants live in header files (DataBeam:: constants). Important ones you may want to tune:
- DataBeam::SR_WINDOW_SIZE — Selective Repeat window capacity (e.g., 4096)
- DataBeam::PACKET_DATA_SIZE — Payload per packet (default ~1000 in this codebase)
- DataBeam::GBN_WINDOW_SIZE / WINDOW_SIZE — GBN window size used in GBN implementation (8 in Phase 3 docs)
- INITIAL_RTO / GBN_INITIAL_RTO_MS — initial timeout (e.g., 500 ms)
- DataBeam::CLIENT_COMPRESSOR_THREADS — number of compressor threads in the pipeline
- DataBeam::RECV_TIMEOUT_MS — UDP socket receive timeout

See src/headers/constants.h (not fully listed here) for the definitive values and tuning knobs.

---

## Testing checklist

Use this checklist when validating the code and running experiments:

- [ ] Build: compiles without errors
- [ ] Handshake: client and server handshake successfully
- [ ] Pipeline: dispatcher, compressors, sender operate concurrently without deadlock
- [ ] Compression: text files are compressed and flagged; incompressible files are sent uncompressed
- [ ] ARQ mechanics:
  - [ ] Sender respects cwnd and ARQ window constraints
  - [ ] ACKs advance cumulative ack and SACK bits mark individual packets
  - [ ] Fast retransmit and timeout retransmissions act as expected
- [ ] RTT & RTO: RTT samples printed for ACKs; RTO adapts when network delays change
- [ ] Congestion Control: cwnd starts small and grows; cwnd halves on timeouts
- [ ] End-to-end: file transfer completes and final CRC/verification succeeds

---

## Performance characteristics (observed/design)

- Stop-and-wait baseline: ~1–5 Mbps (1 RTT per packet)
- Pipelined SR/GBN (windowing): ~3–15 Mbps depending on RTT and window
- Compression: large benefit for text/JSON (40–70% size reduction); little or negative benefit for pre-compressed binaries
- The pipeline and batching (WSASendMsg) reduce syscall overhead for high packet rates

Benchmarks and profiling hooks are available in the client (ClientPerf struct and print_client_report()). Use these to measure per-packet latencies (disk read, compression, crypto, send syscall, lock wait).

---

## Known limitations & notes

- The codebase contains both Go-Back-N and Selective Repeat code paths. client.cpp in current tree references SelectiveRepeatARQ; ensure the corresponding implementation header/source are present and compiled (repo evolves across phases).
- Some parts are Windows-specific (CreateFileMapping, WSASendMsg). Porting to Linux requires changes to file-mapping, socket APIs, and possibly replace WSASendMsg with sendmmsg or using batch syscalls.
- The code uses raw sockets/APIs and custom serialization; be careful with endianness and struct packing when interoperating across platforms.
- AES/HMAC and key handling are present in code: ensure keys are managed securely for production use. Currently keys may be hardcoded for testing.

---

## Next steps / roadmap (recommended)

- Phase 4 goals already planned in repo notes:
  - Implement Selective Repeat ARQ fully (if not already done) so retransmissions are per-packet instead of window-wide
  - Fast Retransmit on 3 duplicate ACKs
  - Multiple parallel streams (stream_id) for large, multi-core transfers
  - Dynamic window sizing based on measured bandwidth-delay product
  - Add FEC (forward error correction) for high-loss links
- Port and test server on Linux and do interop tests across platforms.
- Add end-to-end test harness and CI job to run transfer tests in a simulated loss environment (e.g., netem on Linux or Windows network emulator).

---

## Where to look next in the code

- src/client.cpp — start here to understand the pipeline, threads, and runtime flow
- src/headers/packet.h — definitive packet layout and serializers
- src/arq.cpp and src/headers/arq.h — Go-Back-N RTT/AIMD implementation; search for SelectiveRepeat* headers if you rely on SR functionality
- IMPLEMENTATION_COMPLETE.md — detailed implementation notes for Phase 3

---

## Contact / author

Author: NithiishSD

If you'd like the README to emphasize usage examples, diagrams, or a smaller "quickstart" README, tell me which sections to expand or remove and I will update the file accordingly.
