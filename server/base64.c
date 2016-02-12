/* Base 64 encode and decode functions. */

#include <stdlib.h>
#include <string.h>

#include "base64.h"

/* Encoding lookup table. */
static const char encode[64] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* Decode lookup table, setup during initialisation. */
static unsigned char decode[256];


size_t base64_encode(const void *data, size_t length, char out[])
{
    char *out_start = out;
    const unsigned char *data_in = data;
    for (; length >= 3; length -= 3)
    {
        unsigned char a = *data_in++;
        unsigned char b = *data_in++;
        unsigned char c = *data_in++;

        *out++ = encode[a >> 2];
        *out++ = encode[((a << 4) | (b >> 4)) & 0x3F];
        *out++ = encode[((b << 2) | (c >> 6)) & 0x3F];
        *out++ = encode[c & 0x3F];
    }
    switch (length)
    {
        case 2:
        {
            unsigned char a = *data_in++;
            unsigned char b = *data_in++;
            *out++ = encode[a >> 2];
            *out++ = encode[((a << 4) | (b >> 4)) & 0x3F];
            *out++ = encode[(b << 2) & 0x3F];
            *out++ = '=';
            break;
        }
        case 1:
        {
            unsigned char a = *data_in++;
            *out++ = encode[a >> 2];
            *out++ = encode[(a << 4) & 0x3F];
            *out++ = '=';
            *out++ = '=';
            break;
        }
    }
    *out++ = '\0';
    return (size_t) (out - out_start);
}


/* Converts a base64 string into binary, or returns an error code if the string
 * is malformed or the output buffer is overrun. */
enum base64_status base64_decode(
    const char *string, void *data, size_t max_length, size_t *converted)
{
    /* The format optionally allows the data to be padded to a multiple of 4
     * characters with up to two trailing = signs, so trim these if present. */
    size_t in_length = strlen(string);
    if ((in_length % 4) == 0)
    {
        if (string[in_length - 1] == '=')  in_length -= 1;
        if (string[in_length - 1] == '=')  in_length -= 1;
    }

    /* Compute the converted length and check for overrun. */
    *converted = 3 * (in_length / 4);
    if ((in_length % 4) > 0)
        *converted += in_length % 4 - 1;
    if (*converted > max_length)
        return BASE64_STATUS_OVERRUN;

    /* First convert the blocks of 4 characters. */
    unsigned char *out = data;
    for (; in_length >= 4; in_length -= 4)
    {
        unsigned char a = decode[(unsigned char) *string++];
        unsigned char b = decode[(unsigned char) *string++];
        unsigned char c = decode[(unsigned char) *string++];
        unsigned char d = decode[(unsigned char) *string++];
        if ((a | b | c | d) == 0xff)
            return BASE64_STATUS_MALFORMED;

        *out++ = (unsigned char) ((a << 2) | (b >> 4));
        *out++ = (unsigned char) ((b << 4) | (c >> 2));
        *out++ = (unsigned char) ((c << 6) | d);
    }

    /* Finish off the remaining 2 or 3 characters, if present. */
    switch (in_length)
    {
        case 3:
        {
            unsigned char a = decode[(unsigned char) *string++];
            unsigned char b = decode[(unsigned char) *string++];
            unsigned char c = decode[(unsigned char) *string++];
            if ((a | b | c) == 0xff)
                return BASE64_STATUS_MALFORMED;

            *out++ = (unsigned char) ((a << 2) | (b >> 4));
            *out++ = (unsigned char) ((b << 4) | (c >> 2));
            break;
        }
        case 2:
        {
            unsigned char a = decode[(unsigned char) *string++];
            unsigned char b = decode[(unsigned char) *string++];
            if ((a | b) == 0xff)
                return BASE64_STATUS_MALFORMED;

            *out++ = (unsigned char) ((a << 2) | (b >> 4));
            break;
        }
        case 1:
            return BASE64_STATUS_MALFORMED;
    }
    return BASE64_STATUS_OK;
}


void initialise_base64(void)
{
    memset(decode, 0xff, sizeof(decode));
    for (unsigned int i = 0; i < sizeof(encode); i ++)
        decode[(unsigned char) encode[i]] = (unsigned char) i;
}


const char *base64_error_string(enum base64_status status)
{
    const char *error_messages[] = {
        [BASE64_STATUS_OK]        = "OK",
        [BASE64_STATUS_MALFORMED] = "Malformed base64 string",
        [BASE64_STATUS_OVERRUN]   = "Input string too long for output buffer",
    };
    unsigned int count = sizeof(error_messages) / sizeof(error_messages[0]);
    if (status < count)
        return error_messages[status];
    else
        return "Unknown base64 status";
}
