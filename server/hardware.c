/* Hardware interface for PandA. */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "panda_device.h"

#include "error.h"
#include "hardware.h"


static volatile uint32_t *register_map;
static uint32_t register_map_size;

#define CONTROL_BASE            0x1000



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Named register support. */

/* Named registers. */

#define BIT_READ_RESET          0
#define BIT_READ_VALUE          1
#define POS_READ_RESET          2
#define POS_READ_VALUE          3
#define POS_READ_CHANGES        4
#define BIT_CAPTURE_MASK        5
#define POS_CAPTURE_MASK        6
#define FRAMED_MASK             7
#define EXTENDED_MASK           8

struct named_register {
    const char *name;
    unsigned int offset;
};

#define NAMED_REGISTER(name) \
    [name] = { #name, UNASSIGNED_REGISTER }

static struct named_register named_registers[] = {
    NAMED_REGISTER(BIT_READ_RESET),
    NAMED_REGISTER(BIT_READ_VALUE),
    NAMED_REGISTER(POS_READ_RESET),
    NAMED_REGISTER(POS_READ_VALUE),
    NAMED_REGISTER(POS_READ_CHANGES),
    NAMED_REGISTER(BIT_CAPTURE_MASK),
    NAMED_REGISTER(POS_CAPTURE_MASK),
    NAMED_REGISTER(FRAMED_MASK),
    NAMED_REGISTER(EXTENDED_MASK),
};

static unsigned int reg_block_base = UNASSIGNED_REGISTER;


void hw_set_block_base(unsigned int reg)
{
    reg_block_base = reg;
}


error__t hw_set_named_register(const char *name, unsigned int reg)
{
    for (unsigned int i = 0; i < ARRAY_SIZE(named_registers); i ++)
    {
        if (strcmp(named_registers[i].name, name) == 0)
            return
                TEST_OK_(named_registers[i].offset == UNASSIGNED_REGISTER,
                    "Register already assigned")  ?:
                DO(named_registers[i].offset = reg);
    }
    return FAIL_("Invalid register name");
}

error__t hw_validate(void)
{
    for (unsigned int i = 0; i < ARRAY_SIZE(named_registers); i ++)
        if (named_registers[i].offset == UNASSIGNED_REGISTER)
            return FAIL_("Register %s unassigned", named_registers[i].name);
    return ERROR_OK;
}


static unsigned int make_offset(
    unsigned int block_base, unsigned int block_number, unsigned int reg)
{
    return
        ((block_base & 0x1f) << 10) |
        ((block_number & 0xf) << 6) |
        (reg & 0x3f);
}


static volatile uint32_t *named_register(unsigned char name)
{
    return &register_map[
        make_offset(reg_block_base, 0, named_registers[name].offset)];
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */


void hw_write_register(
    unsigned int block_base, unsigned int block_number, unsigned int reg,
    uint32_t value)
{
    register_map[make_offset(block_base, block_number, reg)] = value;
}

uint32_t hw_read_register(
    unsigned int block_base, unsigned int block_number, unsigned int reg)
{
    uint32_t result = register_map[make_offset(block_base, block_number, reg)];
    return result;
}


/* The bit updates interface consists of a burst read of 8 16-bit pairs (packed
 * into 32-bit words).  The upper 16-bits record the current bit value, the
 * bottom 16-bits whether the value has changed. */
void hw_read_bits(bool bits[BIT_BUS_COUNT], bool changes[BIT_BUS_COUNT])
{
    *named_register(BIT_READ_RESET) = 1;
    for (unsigned int i = 0; i < BIT_BUS_COUNT / 16; i ++)
    {
        uint32_t word = *named_register(BIT_READ_VALUE);
        for (unsigned int j = 0; j < 16; j ++)
        {
            bits[16*i + j] = (word >> (16 + j)) & 1;
            changes[16*i + j] = (word >> j) & 1;
        }
    }
}


/* The position updates interface is a burst read of 32 position values followed
 * by a separate read of the changes flag register.  Note that the changes
 * register must be read after reading all positions. */
void hw_read_positions(
    uint32_t positions[POS_BUS_COUNT], bool changes[POS_BUS_COUNT])
{
    *named_register(POS_READ_RESET) = 1;
    for (unsigned int i = 0; i < POS_BUS_COUNT; i ++)
        positions[i] = *named_register(POS_READ_VALUE);
    uint32_t word = *named_register(POS_READ_CHANGES);
    for (unsigned int i = 0; i < POS_BUS_COUNT; i ++)
        changes[i] = (word >> i) & 1;
}


void hw_write_capture_masks(
    uint32_t bit_capture, uint32_t pos_capture,
    uint32_t framed_mask, uint32_t extended_mask)
{
    *named_register(BIT_CAPTURE_MASK) = bit_capture;
    *named_register(POS_CAPTURE_MASK) = pos_capture;
    *named_register(FRAMED_MASK)      = framed_mask;
    *named_register(EXTENDED_MASK)    = extended_mask;
}


/******************************************************************************/
/* Table API. */


struct short_table {
    unsigned int reset_reg;
    unsigned int fill_reg;
    unsigned int length_reg;
};


struct long_table {
};


struct hw_table {
    uint32_t **data;
    unsigned int count;
    unsigned int block_base;

    /* The two table types have different resource requirements. */
    enum table_type { SHORT_TABLE, LONG_TABLE } table_type;
    union {
        struct short_table short_table;
        struct long_table long_table;
    };
};


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Short table support */


static void create_short_table(
    struct hw_table *table, size_t max_length,
    unsigned int reset_reg, unsigned int fill_reg, unsigned int length_reg)
{
    table->short_table = (struct short_table) {
        .reset_reg = reset_reg,
        .fill_reg = fill_reg,
        .length_reg = length_reg,
    };
    for (unsigned int i = 0; i < table->count; i ++)
        table->data[i] = malloc(max_length * sizeof(uint32_t));
}


static void destroy_short_table(struct hw_table *table)
{
    for (unsigned int i = 0; i < table->count; i ++)
        free(table->data[i]);
}


/* Short tables are written as a burst: first write to the reset register to
 * start the write, then to the fill register. */
static void write_short_table(
    struct hw_table *table, unsigned int number, size_t length)
{
    struct short_table *short_table = &table->short_table;
    unsigned int reset_reg =
        make_offset(table->block_base, number, short_table->reset_reg);
    unsigned int fill_reg =
        make_offset(table->block_base, number, short_table->fill_reg);
    unsigned int length_reg =
        make_offset(table->block_base, number, short_table->length_reg);

    const uint32_t *data = table->data[number];
    register_map[reset_reg] = 1;
    for (size_t i = 0; i < length; i ++)
        register_map[fill_reg] = data[i];
    register_map[length_reg] = (uint32_t) length;
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Long table support */


static error__t create_long_table(struct hw_table *table)
{
    return ERROR_OK;
}


static void destroy_long_table(struct hw_table *table)
{
}


static void start_long_table_write(struct hw_table *table, unsigned int number)
{
    // Write to start write register to invalidate data area
}

static void complete_long_table_write(
    struct hw_table *table, unsigned int number, size_t length)
{
    // Write length written to length register.
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Common */


static struct hw_table *create_hw_table(
    unsigned int block_base, unsigned int count, enum table_type table_type)
{
    struct hw_table *table = malloc(sizeof(struct hw_table));
    *table = (struct hw_table) {
        .data = malloc(count * sizeof(uint32_t *)),
        .count = count,
        .block_base = block_base,
        .table_type = table_type,
    };
    return table;
}


error__t hw_open_short_table(
    unsigned int block_base, unsigned int block_count,
    unsigned int reset_reg, unsigned int fill_reg, unsigned int length_reg,
    size_t max_length, struct hw_table **table)
{
    *table = create_hw_table(block_base, block_count, SHORT_TABLE);
    create_short_table(*table, max_length, reset_reg, fill_reg, length_reg);
    return ERROR_OK;
}


error__t hw_open_long_table(
    unsigned int block_base, unsigned int block_count, unsigned int order,
    struct hw_table **table, size_t *length)
{
    *length = 1U << order;
    *table = create_hw_table(block_base, block_count, LONG_TABLE);
    return create_long_table(*table);
}


const uint32_t *hw_read_table_data(struct hw_table *table, unsigned int number)
{
    return table->data[number];
}


void hw_write_table(
    struct hw_table *table, unsigned int number,
    size_t offset, const uint32_t data[], size_t length)
{
    if (table->table_type == LONG_TABLE)
        start_long_table_write(table, number);

    memcpy(table->data[number] + offset, data, length * sizeof(uint32_t));

    /* Now inform the hardware as appropriate. */
    switch (table->table_type)
    {
        case SHORT_TABLE:
            write_short_table(table, number, offset + length);
            break;
        case LONG_TABLE:
            complete_long_table_write(table, number, offset + length);
            break;
    }
}


void hw_close_table(struct hw_table *table)
{
    switch (table->table_type)
    {
        case SHORT_TABLE:   destroy_short_table(table);     break;
        case LONG_TABLE:    destroy_long_table(table);      break;
    }

    free(table->data);
    free(table);
}


/******************************************************************************/


static int map;

error__t initialise_hardware(void)
{
    return
        TEST_IO_(map = open("/dev/panda.map", O_RDWR | O_SYNC),
            "Unable to open PandA device")  ?:
        TEST_IO(ioctl(map, PANDA_MAP_SIZE, &register_map_size))  ?:
        TEST_IO(register_map = mmap(
            0, register_map_size, PROT_READ | PROT_WRITE, MAP_SHARED, map, 0));
}


void terminate_hardware(void)
{
    ERROR_REPORT(
        TEST_IO(munmap(
            CAST_FROM_TO(volatile uint32_t *, void *, register_map),
            register_map_size))  ?:
        TEST_IO(close(map)),
        "Calling terminate_hardware");
}
