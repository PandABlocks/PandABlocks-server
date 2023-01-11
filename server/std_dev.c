#include <stdbool.h>
#include <stdint.h>
#include <math.h>

#include "std_dev.h"


/* In this file we compute the standard deviation
 *
 *      std_dev = sqrt(sum(x^2)/n - (sum(x)/n)^2)
 *
 * given the following arguments
 *
 *  A = sum(x^2)    < 2^94 : uint96_t
 *  B = |sum(x)|    < 2^63 : uint64_t
 *  N = n           < 2^32 : uint32_t
 *
 * The bound on N is by definition, and as we know that x is of type int_32 and
 * thus satisfies |x| <= 2^31 we immediately get the bounds above.
 *
 * The main challenge to computing this is the large number of bits in the
 * intermediate result.  Writing S = std_dev we can start by rearranging to get
 * the calculation
 *
 *      N S^2 = A - B^2/N
 *
 * and then the key is to compute this difference without losing significant
 * bits.  We do this by splitting B^2/N into its integer and fractional parts by
 * division
 *
 *      B^2 = N Q + R  where  R < N
 *
 * and then we can safely compute
 *
 *      N S^2 ~~ float(A - Q) - float(R) / N
 *
 * without losing significant precision.
 *
 * Unfortunately even the integer part of this challenges our hardware
 * resources.
 */


/* We know that it is safe to use this on sum(x) as we know this can't be equal
 * to -2^63. */
static uint64_t abs_64(int64_t x)
{
    return (uint64_t) (x >= 0 ? x : -x);
}


/* Compute D, R from N, B such that B = N*D + R and R < N.  We're given
 * B < 2^64, R < 2^32, and we know and assume that D < 2^32. */
static bool div_rem_64_32(uint64_t b, uint32_t n, uint32_t *d, uint32_t *r)
{
    uint64_t div = b / n;
    /* Note: with the current gcc 4.9.1 compiler we need to compute rem like
     * this rather than the possibly more natural b % n form, to avoid the
     * division being computed more than once! */
    uint64_t rem = b - n * div;
    *d = (uint32_t) div;
    *r = (uint32_t) rem;
    /* Sanity check on division, fail if divider too large. */
    return (div >> 32) == 0;
}


/* Compute A - B*C for 96-bit A, 64-bit B, 32-bit C. */
static unaligned_uint96_t mul_sub_96_64_32(
    unaligned_uint96_t a, uint64_t b, uint32_t c)
{
    /* Compute low word and subtract from low word of result. */
    uint64_t bl_c = (b & 0xFFFFFFFF) * c;

#if __GNUC__ >= 5
    /* Use built-in carry propagation where available. */
    bool borrow = __builtin_sub_overflow(a.low_word_64, bl_c, &a.low_word_64);
    a.high_word_32 -= (uint32_t) borrow;

#elif defined(__ARM_ARCH) && __ARM_ARCH <= 7
    /* 32-bit ARM target with old compiler.  Do the add with carry by hand. */
    __asm__(
        "subs   %[all], %[all], %[bcl]" "\n\t"
        "sbcs   %[alh], %[alh], %[bch]" "\n\t"
        "sbc    %[ah], %[ah], #0"
        : [all] "+r" (a.values[0]),
          [alh] "+r" (a.values[1]),
          [ah]  "+r" (a.values[2])
        : [bcl] "r" (bl_c & 0xFFFFFFFF),
          [bch] "r" (bl_c >> 32)
        : "cc");

#else
    /* This will occur when building on RHEL7, this is stuck with gcc 4.8.5. */
    #pragma message "Building fallback option: check your compiler version!"

    /* This should only occur when building the simulation server with an old
     * gcc compiler.  We'll have to do the carry propagation the hard and
     * inefficent way.  Note that this generates pretty nasty code. */
    uint64_t difference = a.low_word_64 - bl_c;
    bool borrow = difference > a.low_word_64;
    a.low_word_64 = difference;
    a.high_word_32 -= (uint32_t) borrow;

#endif

    /* Subtract overlapping high word from the result. */
    uint64_t bh_c = (b >> 32) * c;
    a.high_word_64 -= bh_c;

    return a;
}


/* Convert 96-bit integer to double precision result. */
static double uint96_to_double(unaligned_uint96_t a)
{
    /* Work 32-bit word at a time to avoid possible loss of significant bits. */
    double result = (double) a.values[2];
    result = ldexp(result, 32) + (double) a.values[1];
    return ldexp(result, 32) + (double) a.values[0];
}


/* Computes the standard deviation from sum of values and sum of squares.  The
 * required calculation is:
 *
 *      std_dev = sqrt(sum(x^2)/n - (sum(x)/n)^2)
 *
 * The challenge is to perform the subtraction above without loss of precision,
 * as it is possible for both sides of this difference to be very large numbers,
 * and we may need all bits of the result.  Given that we only have 64-bit
 * arithmetic available this means we have some hoops to jump through.
 *
 * We are given the following arguments:
 *
 *  samples     : unsigned 32-bit   N
 *  sum_values  : signed 64-bit     B = |sum(x)|
 *  sum_squares : unsigned 96-bit   A = sum(x^2)
 *
 * These bounds can be tightened.  We know that |x| <= 2^31 and N < 2^32, so
 * straightaway we see that B < 2^63, which tells us that taking the magnitude
 * is safe.  Next we have x^2 <= 2^62, and therefore A < 2^94.
 *
 * We start by writing S = std_dev and using division to write B = D*N + R
 * (where R < N < 2^32, D <= 2^31), we can write
 *
 *      N S^2 = A - B^2/N = A - (B + R) * D - R^2/N
 *
 * We can compute A-(B+R)D using 96-bit arithmetic, and at this point it simply
 * remains to subtract R^2/N.  This residual part has no more than 32 integer
 * bits (R^2/N < 2^32), and so it should be safe to do this part using floating
 * point arithmetic. */
double compute_standard_deviation(
    uint32_t samples, int64_t raw_sum_values,
    const unaligned_uint96_t *raw_sum_squares)
{
    /* If the incoming data is invalid for some reason, return NAN. */
    if (samples == 0)
        return NAN;

    uint64_t sum_values = abs_64(raw_sum_values);

    /* First compute divison and remainder B = D*N + R. */
    uint32_t sum_mean, sum_rem;     // D, R
    if (!div_rem_64_32(sum_values, samples, &sum_mean, &sum_rem))
        return NAN;

    /* Now compute A - (B + R) D.  Note that computing B+R in 64-bits is safe as
     * we know that B is bounded by 2^63. */
    unaligned_uint96_t sum_squares = *raw_sum_squares;
    sum_squares = mul_sub_96_64_32(sum_squares, sum_values + sum_rem, sum_mean);

    double r2_n = (double) ((uint64_t) sum_rem * sum_rem) / samples;
    double n_s2 = uint96_to_double(sum_squares) - r2_n;
    if (n_s2 < 0)
        return NAN;

    return sqrt(n_s2 / samples);
}
