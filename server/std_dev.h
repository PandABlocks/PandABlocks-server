/* Support for computing standard deviation. */


typedef union __attribute__((packed)) {
    uint32_t values[3];
    struct __attribute__((packed)) {
        uint64_t low_word_64;
        uint32_t high_word_32;
    };
    struct __attribute__((packed)) {
        uint32_t low_word_32;
        uint64_t high_word_64;
    };
} unaligned_uint96_t;


double compute_standard_deviation(
    uint32_t samples, int64_t sum_values,
    const unaligned_uint96_t *sum_squares);
