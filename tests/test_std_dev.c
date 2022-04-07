/* Simple wrapper for calling std_dev with arguments. */

#include <stdbool.h>
#include <stdint.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>

#include "std_dev.h"


static uint64_t parse_uint64(const char *string)
{
    char *end;
    errno = 0;
    uint64_t result = strtoull(string, &end, 0);
    assert(end > string);
    assert(errno == 0);
    assert(*end == '\0');
    return result;
}


static uint32_t parse_uint32(const char *string)
{
    char *end;
    errno = 0;
    uint32_t result = (uint32_t) strtoul(string, &end, 0);
    assert(end > string);
    assert(errno == 0);
    assert(*end == '\0');
    return result;
}


int main(int argc, char **argv)
{
    assert(argc == 5);
    uint32_t n = parse_uint32(argv[1]);
    uint64_t b = parse_uint64(argv[2]);
    uint32_t ah = parse_uint32(argv[3]);
    uint64_t al = parse_uint64(argv[4]);

    printf("%u %"PRIu64" 0x%08"PRIx32"%016"PRIx64" => ", n, b, ah, al);

    unaligned_uint96_t a = { .low_word_64 = al };
    a.values[2] = ah;
    double s = compute_standard_deviation(n, (int64_t) b, &a);
    printf("%.16le\n", s);
    return 0;
}
