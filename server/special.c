/* Special field types. */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <string.h>
#include <limits.h>
#include <pthread.h>

#include "error.h"
#include "parse.h"
#include "config_server.h"
#include "fields.h"
#include "locking.h"

#include "special.h"


struct xadc_state {
    int offset;                 // Offset to apply to raw value
    double scale;               // Scaling factor for formatted result
    char *raw_file;             // File name to read value from

    pthread_mutex_t mutex;
    int raw_value;              // Last value read
    uint64_t update_index;      // Timestamp of last update
};


static error__t xadc_class_init(
    const char **line, unsigned int count,
    struct hash_table *attr_map, void **class_data,
    struct indent_parser *parser)
{
    struct xadc_state *xadc = malloc(sizeof(struct xadc_state));
    *xadc = (struct xadc_state) {
        .mutex = PTHREAD_MUTEX_INITIALIZER,
        .update_index = 1,
    };
    *class_data = xadc;
    return TEST_OK_(count == 1, "Cannot have multiple instances of xadc field");
}


/* Reads formatted value from named file. */
static error__t read_file_value(
    const char *filename, const char *format, void *result)
{
    FILE *file;
    return
        TEST_OK(file = fopen(filename, "r"))  ?:
        DO_FINALLY(
            TEST_OK(fscanf(file, format, result) == 1),
        // finally
            fclose(file));
}


#define FORMAT_FILENAME(filename, base_path, suffix) \
    DO(snprintf(filename, sizeof(filename), "%s_%s", base_path, suffix))


static error__t xadc_parse_register(
    void *class_data, struct field *field, unsigned int block_base,
    const char **line)
{
    /* Treat entire line as path to sysfs node to read.  We'll add the _offset,
     * _scale, and _raw suffixes to find our nodes. */
    const char *base_path = skip_whitespace(*line);
    *line = strchr(base_path, '\0');

    struct xadc_state *xadc = class_data;
    char filename[PATH_MAX];
    return
        /* Read offset if present, otherwise defaults to 0. */
        FORMAT_FILENAME(filename, base_path, "offset")  ?:
        IF(access(filename, R_OK) == 0,
            read_file_value(filename, "%d", &xadc->offset))  ?:
        /* Read scale, must be present. */
        FORMAT_FILENAME(filename, base_path, "scale")  ?:
        read_file_value(filename, "%lg", &xadc->scale)  ?:
        /* Finally save copy of the full raw file name to read for value. */
        FORMAT_FILENAME(filename, base_path, "raw")  ?:
        DO(xadc->raw_file = strdup(filename));
}


static void xadc_destroy(void *class_data)
{
    struct xadc_state *xadc = class_data;
    free(xadc->raw_file);
    free(class_data);
}


static error__t xadc_raw_get(struct xadc_state *xadc)
{
    int raw_value;
    return
        read_file_value(xadc->raw_file, "%d", &raw_value)  ?:
        DO(
            if (raw_value != xadc->raw_value)
            {
                xadc->raw_value = raw_value;
                xadc->update_index = get_change_index();
            }
        );
}


static error__t xadc_get(
    void *class_data, unsigned int number, char result[], size_t length)
{
    struct xadc_state *xadc = class_data;
    return
        WITH_LOCK(xadc->mutex, xadc_raw_get(xadc))  ?:
        format_double(result, length,
            1e-3 * xadc->scale * (xadc->raw_value + xadc->offset));
}


static void xadc_change_set(
    void *class_data, const uint64_t report_index, bool changes[])
{
    struct xadc_state *xadc = class_data;
    LOCK(xadc->mutex);
    ERROR_REPORT(xadc_raw_get(xadc),
        "Unexpected error reading XADC value");
    changes[0] = xadc->update_index > report_index;
    UNLOCK(xadc->mutex);
}


const struct class_methods xadc_class_methods = {
    "xadc",
    .init = xadc_class_init,
    .parse_register = xadc_parse_register,
    .destroy = xadc_destroy,
    .get = xadc_get,
    .change_set = xadc_change_set,
    .change_set_index = CHANGE_IX_READ,
};
