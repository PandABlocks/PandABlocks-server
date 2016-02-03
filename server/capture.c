/* Data capture control. */

#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#include "error.h"
#include "parse.h"
#include "buffered_file.h"
#include "config_server.h"
#include "data_server.h"
#include "classes.h"
#include "output.h"
#include "locking.h"
#include "hardware.h"

#include "capture.h"


struct data_capture {
};

static struct data_capture data_capture_state;


struct data_capture *prepare_data_capture(void)
{
    printf("Write capture masks\n");
    printf("Write capture set\n");
    return &data_capture_state;
}


error__t parse_data_options(const char *line, struct data_options *options)
{
    printf("parse_data_options: \"%s\"\n", line);
    return ERROR_OK;
}


bool send_data_header(
    struct data_capture *capture, struct data_options *options,
    struct buffered_file *file)
{
    write_string(file, "header\n", 7);
    return flush_out_buf(file);
}


size_t compute_output_data(
    struct data_capture *capture, struct data_options *options,
    const void *input, size_t input_length, size_t *input_consumed,
    void *output, size_t output_length)
{
    const uint32_t *buf = input;
#if 1
    /* Lazy, about to redo.  For the moment the out buf is long enough. */
    *input_consumed = input_length;
//     ASSERT_IO(usleep(1000000));
    return (size_t) sprintf(output, "%u..%u(%zu)\n",
        buf[0], buf[input_length/4 - 1], input_length);
#else
    size_t count = 0;
    for (size_t i = 0; i < in_length / 4; i ++)
        count += (size_t) sprintf(output + count, " %d", buf[i]);
    *input_consumed = input_length;
    usleep(100000);
    return count;
#endif
}
