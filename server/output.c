#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "error.h"
#include "hardware.h"
#include "hashtable.h"
#include "parse.h"
#include "config_server.h"
#include "data_server.h"
#include "fields.h"
#include "attributes.h"
#include "types.h"
#include "enums.h"
#include "time.h"
#include "pos_mux.h"
#include "pos_out.h"
#include "ext_out.h"
#include "bit_out.h"

#include "output.h"


/* Common internal interface to capture sources. */

enum output_type {
    OUTPUT_POS,
    OUTPUT_EXT,
};

struct output_field {
    enum output_type output_type;
    union {
        void *output;
        struct ext_out *ext_out;
        struct pos_out *pos_out;
    };
    unsigned int number;
    char *field_name;
};


/*****************************************************************************/
/* Output type dependent functionality. */


static bool get_capture_enabled(
    struct output_field *field, const char **capture)
{
    switch (field->output_type)
    {
        case OUTPUT_POS:
            return get_pos_out_capture(field->pos_out, field->number, capture);
        case OUTPUT_EXT:
            return get_ext_out_capture(field->ext_out, capture);
        default:
            ASSERT_FAIL();
    }
}


static void reset_output_capture(struct output_field *field)
{
    switch (field->output_type)
    {
        case OUTPUT_POS:
            reset_pos_out_capture(field->pos_out, field->number);
            break;
        case OUTPUT_EXT:
            reset_ext_out_capture(field->ext_out);
            break;
    }
}


static unsigned int get_capture_info(
    struct output_field *field, struct capture_info capture_info[])
{
    switch (field->output_type)
    {
        case OUTPUT_POS:
            return get_pos_out_capture_info(
                field->pos_out, field->number, capture_info);
        case OUTPUT_EXT:
            return get_ext_out_capture_info(field->ext_out, capture_info);
        default:
            ASSERT_FAIL();
    }
}


/*****************************************************************************/

#define MAX_OUTPUT_COUNT    (POS_BUS_COUNT + EXT_BUS_COUNT)

static struct output_field output_fields[MAX_OUTPUT_COUNT];
static unsigned int output_field_count = 0;


static error__t register_one_field(
    enum output_type output_type, void *output,
    struct field *field, unsigned int number)
{
    char name[MAX_NAME_LENGTH];
    format_field_name(name, sizeof(name), field, NULL, number, '\0');

    /* There's a funny little bug in gcc (Bug 10676, fixed in gcc 4.6) which
     * means we can't initialise .output statically, so we need to do this in
     * two bites here. */
    struct output_field output_field = {
        .output_type = output_type,
//         .output = output,           // Need gcc > 4.6 for this!
        .number = number,
        .field_name = strdup(name),
    };
    output_field.output = output;

    return
        TEST_OK_(output_field_count < MAX_OUTPUT_COUNT,
            "Too many capture fields specifed!")  ?:
        DO(output_fields[output_field_count++] = output_field);
}


static error__t register_output_fields(
    enum output_type output_type, void *output,
    struct field *field, unsigned int count)
{
    error__t error = ERROR_OK;
    for (unsigned int i = 0; !error  &&  i < count; i ++)
        error = register_one_field(output_type, output, field, i);
    return error;
}


error__t register_ext_out(struct ext_out *ext_out, struct field *field)
{
    return register_output_fields(OUTPUT_EXT, ext_out, field, 1);
}


error__t register_pos_out(
    struct pos_out *pos_out, struct field *field, unsigned int count)
{
    return register_output_fields(OUTPUT_POS, pos_out, field, count);
}


void reset_capture_list(void)
{
    for (unsigned int i = 0; i < output_field_count; i ++)
        reset_output_capture(&output_fields[i]);
}


void report_capture_list(struct connection_result *result)
{
    for (unsigned int i = 0; i < output_field_count; i ++)
    {
        struct output_field *field = &output_fields[i];
        const char *capture;
        if (get_capture_enabled(field, &capture))
            format_many_result(result, "%s %s", field->field_name, capture);
    }
}


void report_capture_labels(struct connection_result *result)
{
    for (unsigned int i = 0; i < output_field_count; i ++)
    {
        struct output_field *field = &output_fields[i];
        result->write_many(result->write_context, field->field_name);
    }
}


bool iterate_captured_values(
    unsigned int *ix, unsigned int *captured,
    struct capture_info capture_info[])
{
    if (*ix < output_field_count)
    {
        struct output_field *field = &output_fields[*ix];
        *captured = get_capture_info(field, capture_info);
        /* Fill in the one field that we manage. */
        for (unsigned int i = 0; i < *captured; i ++)
            capture_info[i].field_name = field->field_name;
        *ix += 1;
        return true;
    }
    else
        return false;
}


/*****************************************************************************/
/* Startup and shutdown. */


error__t initialise_output(void)
{
    return
        initialise_pos_mux()  ?:
        initialise_pos_out()  ?:
        initialise_bit_out()  ?:
        initialise_ext_out();
}


void terminate_output(void)
{
    terminate_ext_out();
    terminate_bit_out();
    terminate_pos_out();
    terminate_pos_mux();

    for (unsigned int i = 0; i < output_field_count; i ++)
        free(output_fields[i].field_name);
}
