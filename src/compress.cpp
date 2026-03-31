#include <zlib.h>
#include <cstring>
#include <iostream>
#include <cmath>
#include "./headers/packet.h"
#include "./headers/compress.h"
using namespace std;
// Compression using zlib
int compress_data(const char *input, size_t input_len, char *output, size_t &output_len)
{
    uLongf compressed_size = output_len;
    int ret = compress2((Bytef *)output, &compressed_size,
                        (const Bytef *)input, input_len, 6);

    if (ret == Z_OK)
    {
        output_len = compressed_size;
        return 0;
    }

    return ret;
}

// Decompression using zlib
int decompress_data(const char *input, size_t input_len, char *output, size_t &output_len)
{
    uLongf decompressed_size = output_len;
    int ret = uncompress((Bytef *)output, &decompressed_size,
                         (const Bytef *)input, input_len);

    if (ret == Z_OK)
    {
        output_len = decompressed_size;
        return 0;
    }

    return ret;
}
