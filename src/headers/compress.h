
#ifndef COMPRESS_H
#define COMPRESS_H

#include <stdint.h>

using namespace std;

// Compression utilities
int compress_data(const char *input, size_t input_len, char *output, size_t &output_len);
int decompress_data(const char *input, size_t input_len, char *output, size_t &output_len);

#endif // COMPRESS_H
