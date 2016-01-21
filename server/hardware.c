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
#ifdef SIM_HARDWARE
#include "sim_hardware.h"
#endif

#include "hardware.h"


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Register map access. */


#ifndef SIM_HARDWARE

static volatile uint32_t *register_map;
static uint32_t register_map_size;


static unsigned int make_offset(
    unsigned int block_base, unsigned int block_number, unsigned int reg)
{
    return
        ((block_base & 0x1f) << 10) |
        ((block_number & 0xf) << 6) |
        (reg & 0x3f);
}


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

#endif



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Named register support. */

/* Named registers. */

#define BIT_READ_RST            0
#define BIT_READ_VALUE          1
#define POS_READ_RST            2
#define POS_READ_VALUE          3
#define POS_READ_CHANGES        4
#define BIT_CAPTURE_MASK        5
#define POS_CAPTURE_MASK        6

struct named_register {
    const char *name;
    unsigned int offset;
};

#define NAMED_REGISTER(name) \
    [name] = { #name, UNASSIGNED_REGISTER }

static struct named_register named_registers[] = {
    NAMED_REGISTER(BIT_READ_RST),
    NAMED_REGISTER(BIT_READ_VALUE),
    NAMED_REGISTER(POS_READ_RST),
    NAMED_REGISTER(POS_READ_VALUE),
    NAMED_REGISTER(POS_READ_CHANGES),
    NAMED_REGISTER(BIT_CAPTURE_MASK),
    NAMED_REGISTER(POS_CAPTURE_MASK),
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


static inline void write_named_register(unsigned int name, uint32_t value)
{
    hw_write_register(
        reg_block_base, 0, named_registers[name].offset, value);
}


static inline uint32_t read_named_register(unsigned int name)
{
    return hw_read_register(reg_block_base, 0, named_registers[name].offset);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */


/* The bit updates interface consists of a burst read of 8 16-bit pairs (packed
 * into 32-bit words).  The upper 16-bits record the current bit value, the
 * bottom 16-bits whether the value has changed. */
void hw_read_bits(bool bits[BIT_BUS_COUNT], bool changes[BIT_BUS_COUNT])
{
    write_named_register(BIT_READ_RST, 1);
    for (unsigned int i = 0; i < BIT_BUS_COUNT / 16; i ++)
    {
        uint32_t word = read_named_register(BIT_READ_VALUE);
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
    write_named_register(POS_READ_RST, 1);
    for (unsigned int i = 0; i < POS_BUS_COUNT; i ++)
        positions[i] = read_named_register(POS_READ_VALUE);
    uint32_t word = read_named_register(POS_READ_CHANGES);
    for (unsigned int i = 0; i < POS_BUS_COUNT; i ++)
        changes[i] = (word >> i) & 1;
}


void hw_write_capture_masks(
    uint32_t bit_capture, uint32_t pos_capture,
    uint32_t framed_mask, uint32_t extended_mask)
{
}


/******************************************************************************/
/* Table API. */


struct short_table {
    unsigned int reset_reg;     // Starts write
    unsigned int fill_reg;      // Writes one word
    unsigned int length_reg;    // Completes write
};


struct long_table {
    unsigned int base_reg;      // Physical base address
    unsigned int length_reg;    // Sets memory area length
    void **table_ids;           // Id for table
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


static error__t create_short_table(
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
    return ERROR_OK;
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
    const uint32_t *data = table->data[number];

    hw_write_register(table->block_base, number, short_table->reset_reg, 1);
    for (size_t i = 0; i < length; i ++)
        hw_write_register(
            table->block_base, number, short_table->fill_reg, data[i]);
    hw_write_register(
        table->block_base, number, short_table->length_reg, (uint32_t) length);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Long table support */

#ifndef SIM_HARDWARE
static error__t hw_long_table_allocate(
    unsigned int order,
    uint32_t **data, uint32_t *physical_addr, void **table_id)
{
    // Use ioctl on device to allocate block
    size_t length = 1U << order;
    *data = malloc(length * sizeof(uint32_t));
    *physical_addr = 0;
    *table_id = NULL;
    return FAIL_("Not implemented");
}

static void hw_long_table_release(void *table_id)
{
    // Use ioctl on device to release table
}

static void hw_long_table_flush(
    void *table_id, size_t length,
    unsigned int block_base, unsigned int number)
{
    // Use ioctl on device to flush cache
}
#endif


static error__t create_long_table(
    struct hw_table *table, unsigned int order,
    unsigned int base_reg, unsigned int length_reg)
{
    table->long_table = (struct long_table) {
        .base_reg = base_reg,
        .length_reg = length_reg,
        .table_ids = malloc(table->count * sizeof(void *)),
    };

    error__t error = ERROR_OK;
    for (unsigned int i = 0; !error  &&  i < table->count; i ++)
    {
        uint32_t physical_addr;
        error = hw_long_table_allocate(
            order, &table->data[i], &physical_addr,
            &table->long_table.table_ids[i]);
        if (!error)
            hw_write_register(table->block_base, i, base_reg, physical_addr);
    }
    return error;
}


static void destroy_long_table(struct hw_table *table)
{
    for (unsigned int i = 0; i < table->count; i ++)
        hw_long_table_release(table->long_table.table_ids[i]);
    free(table->long_table.table_ids);
}


static void start_long_table_write(struct hw_table *table, unsigned int number)
{
    /* Invalidate entire data area. */
    hw_write_register(
        table->block_base, number, table->long_table.length_reg, 0);
}

static void complete_long_table_write(
    struct hw_table *table, unsigned int number, size_t length)
{
    /* Flush cache and update length register. */
    hw_long_table_flush(
        table->long_table.table_ids[number], length, table->block_base, number);
    hw_write_register(
        table->block_base, number, table->long_table.length_reg,
        (uint32_t) length);
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
    return create_short_table(
        *table, max_length, reset_reg, fill_reg, length_reg);
}


error__t hw_open_long_table(
    unsigned int block_base, unsigned int block_count, unsigned int order,
    unsigned int base_reg, unsigned int length_reg,
    struct hw_table **table, size_t *length)
{
    *length = 1U << order;
    *table = create_hw_table(block_base, block_count, LONG_TABLE);
    return create_long_table(*table, order, base_reg, length_reg);
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


#ifndef SIM_HARDWARE

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

#endif
