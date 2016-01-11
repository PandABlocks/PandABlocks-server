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


/* Short tables are written as a burst: first write to the reset register to
 * start the write, then to the fill register. */
void hw_write_short_table(
    unsigned int block_base, unsigned int block_number,
    unsigned int reset_reg, unsigned int fill_reg, unsigned int length_reg,
    const uint32_t data[], size_t length)
{
    register_map[make_offset(block_base, block_number, reset_reg)] = 1;
    for (size_t i = 0; i < length; i ++)
        register_map[make_offset(block_base, block_number, fill_reg)] = data[i];
    register_map[make_offset(block_base, block_number, length_reg)] =
        (uint32_t) length;
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


void hw_write_bit_capture(uint32_t capture_mask)
{
    *named_register(BIT_CAPTURE_MASK) = capture_mask;
}

void hw_write_position_capture_masks(
    uint32_t capture_mask, uint32_t framed_mask, uint32_t extended_mask)
{
    *named_register(POS_CAPTURE_MASK) = capture_mask;
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Long table support */

struct hw_long_table {
};


error__t hw_open_long_table(
    unsigned int block_base, unsigned int count, unsigned int order,
    struct hw_long_table **table, size_t *length)
{
    *length = 0;
    return ERROR_OK;
}


void hw_read_long_table_area(
    struct hw_long_table *table, unsigned int number, uint32_t **data)
{
}


void hw_write_long_table_length(
    struct hw_long_table *table, unsigned int number, size_t length)
{
}


void hw_close_long_table(struct hw_long_table *table)
{
}



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */


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
