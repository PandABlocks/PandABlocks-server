#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>

#include "error.h"
#include "locking.h"

#include "buffer.h"


struct buffer {
    char dummy[1024];
};


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

void *get_write_block(struct buffer *buffer, size_t *block_size)
{
    *block_size = sizeof(buffer->dummy);
    return buffer->dummy;
}


void release_write_block(struct buffer *buffer, size_t written, bool data_end)
{
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

struct buffer *create_buffer(size_t block_size, size_t block_count)
{
    struct buffer *buffer = malloc(sizeof(struct buffer));
    *buffer = (struct buffer) {
    };
    return buffer;
}


void destroy_buffer(struct buffer *buffer)
{
    free(buffer);
}
