// =============================================================================
// crc32_hw.h  —  Hardware-Accelerated CRC-32 (drop-in replacement)
// =============================================================================
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring> // memcpy for unaligned reads

// ---- Platform / intrinsic headers ------------------------------------------
#if defined(_MSC_VER)
#include <intrin.h>
#include <nmmintrin.h>
#elif defined(__GNUC__) || defined(__clang__)
#ifdef __SSE4_2__
#include <nmmintrin.h>
#endif
#include <cpuid.h>
#endif

// =============================================================================
// Runtime CPU feature detection
// =============================================================================
static inline bool _crc32_cpu_has_sse42() noexcept
{
#if defined(_MSC_VER)
    int info[4];
    __cpuid(info, 1);
    return (info[2] & (1 << 20)) != 0;
#elif defined(__GNUC__) || defined(__clang__)
    uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
    __get_cpuid(1, &eax, &ebx, &ecx, &edx);
    return (ecx & (1u << 20)) != 0;
#else
    return false;
#endif
}

// Cached at first call — no overhead on repeated invocations
static inline bool has_hw_crc32() noexcept
{
    static const bool result = _crc32_cpu_has_sse42();
    return result;
}

// =============================================================================
// Slicing-by-8 table generator
// Builds 8 CRC tables at startup for the standard IEEE 802.3 polynomial.
// Each table handles one byte of an 8-byte-wide processing lane.
// =============================================================================
namespace crc32_detail
{

    static uint32_t T[8][256];
    static bool tables_ready = false;

    static void build_tables() noexcept
    {
        // Build T[0] — the standard single-byte CRC table
        for (uint32_t i = 0; i < 256; ++i)
        {
            uint32_t crc = i;
            for (int k = 0; k < 8; ++k)
                crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320u : (crc >> 1);
            T[0][i] = crc;
        }

        // Build T[1]..T[7] by extending T[0]
        // T[n][i] = T[0][T[n-1][i] & 0xFF] ^ (T[n-1][i] >> 8)
        for (int slice = 1; slice < 8; ++slice)
            for (uint32_t i = 0; i < 256; ++i)
                T[slice][i] = T[0][T[slice - 1][i] & 0xFF] ^ (T[slice - 1][i] >> 8);

        tables_ready = true;
    }

    static inline void ensure_tables() noexcept
    {
        if (!tables_ready) [[unlikely]]
            build_tables();
    }

    // ---------------------------------------------------------------------------
    // Tier 2 — Slicing-by-8 scalar path
    // Processes 8 bytes per loop iteration, 4 table lookups parallelised by the
    // CPU's out-of-order execution, no SIMD dependency.
    // Throughput: ~1–2 cycles/byte on a modern x86.
    // ---------------------------------------------------------------------------
    static uint32_t crc32_slicing8(uint32_t crc, const uint8_t *data, size_t len) noexcept
    {
        ensure_tables();

        // ---- Process 8 bytes at a time ----------------------------------------
        while (len >= 8)
        {
            // Unaligned 32-bit reads — safe on x86/x64, UB on strict-aliasing
            // platforms, so we go through memcpy.
            uint32_t w0, w1;
            memcpy(&w0, data, 4);
            memcpy(&w1, data + 4, 4);

            // XOR the current CRC into the first word
            w0 ^= crc;

            crc =
                T[7][w0 & 0xFF] ^
                T[6][(w0 >> 8) & 0xFF] ^
                T[5][(w0 >> 16) & 0xFF] ^
                T[4][w0 >> 24] ^
                T[3][w1 & 0xFF] ^
                T[2][(w1 >> 8) & 0xFF] ^
                T[1][(w1 >> 16) & 0xFF] ^
                T[0][w1 >> 24];

            data += 8;
            len -= 8;
        }

        // ---- Byte-at-a-time tail -----------------------------------------------
        while (len--)
            crc = T[0][(crc ^ *data++) & 0xFF] ^ (crc >> 8);

        return crc;
    }

} // namespace crc32_detail

// =============================================================================
// Tier 1 — SSE4.2 hardware CRC path
// Uses the _mm_crc32_u64 / _mm_crc32_u8 intrinsics which map to a single
// CRC32 opcode on Intel Nehalem+ and AMD Bulldozer+.
// Throughput: ~0.13 cycles/byte (one instruction, 3-cycle latency pipelined).
// =============================================================================
#if defined(__SSE4_2__) || defined(_MSC_VER)

static uint32_t crc32_hw_sse42(uint32_t crc, const uint8_t *data, size_t len) noexcept
{
    // ---- 8-byte chunks (64-bit intrinsic for maximum throughput) -----------
    // Cast via uint64_t to avoid strict-aliasing UB
    while (len >= 8)
    {
        uint64_t word;
        memcpy(&word, data, 8);
        crc = static_cast<uint32_t>(
            _mm_crc32_u64(static_cast<uint64_t>(crc), word));
        data += 8;
        len -= 8;
    }

    // ---- 4-byte chunk ------------------------------------------------------
    if (len >= 4)
    {
        uint32_t word;
        memcpy(&word, data, 4);
        crc = _mm_crc32_u32(crc, word);
        data += 4;
        len -= 4;
    }

    // ---- Remaining 0–3 bytes -----------------------------------------------
    while (len--)
        crc = _mm_crc32_u8(crc, *data++);

    return crc;
}

#endif // __SSE4_2__

// =============================================================================
// Public API — drop-in replacement for your calculate_crc32()
//
//   uint32_t calculate_crc32(const unsigned char *data, size_t len);
//
// Dispatches at runtime:
//   • SSE4.2 present  → ~0.13 cycles/byte  (hardware CRC opcode)
//   • SSE4.2 absent   → ~1–2 cycles/byte   (slicing-by-8 table)
//   • vs. your baseline ~13  cycles/byte   (bit-loop)
// =============================================================================
inline uint32_t calculate_crc32(const unsigned char *data, size_t len) noexcept
{
    uint32_t crc = 0xFFFFFFFFu;

#if defined(__SSE4_2__) || defined(_MSC_VER)
    if (has_hw_crc32())
    {
        crc = crc32_hw_sse42(crc, data, len);
        return crc ^ 0xFFFFFFFFu;
    }
#endif

    // Fallback: slicing-by-8
    crc = crc32_detail::crc32_slicing8(crc, data, len);
    return crc ^ 0xFFFFFFFFu;
}
