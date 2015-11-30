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

#define BIT_READ_RESET          (CONTROL_BASE + 0)
#define BIT_READ_VALUE          (CONTROL_BASE + 1)

#define POS_READ_RESET          (CONTROL_BASE + 2)
#define POS_READ_VALUE          (CONTROL_BASE + 3)
#define POS_READ_CHANGES        (CONTROL_BASE + 4)

#define BIT_CAPTURE_MASK        (CONTROL_BASE + 5)
#define POS_CAPTURE_MASK        (CONTROL_BASE + 6)


static unsigned int make_offset(
    unsigned int block_base, unsigned int block_number, unsigned int reg)
{
    return
        ((block_base & 0x1f) << 8) |
        ((block_number & 0xf) << 4) |
        (reg & 0xf);
}

void hw_write_register(
    unsigned int block_base, unsigned int block_number, unsigned int reg,
    uint32_t value)
{
    printf("hw_write_register %u:%u:%u <= %u\n",
        block_base, block_number, reg, value);
    register_map[make_offset(block_base, block_number, reg)] = value;
}

uint32_t hw_read_register(
    unsigned int block_base, unsigned int block_number, unsigned int reg)
{
    uint32_t result = register_map[make_offset(block_base, block_number, reg)];
    printf("hw_read_register %u:%u:%u => %u\n",
        block_base, block_number, reg, result);
    return result;
}


/* Short tables are written as a burst: first write to the reset register to
 * start the write, then to the fill register. */
void hw_write_short_table(
    unsigned int block_base, unsigned int block_number,
    unsigned int reset_reg, unsigned int fill_reg,
    const uint32_t data[], size_t length)
{
    register_map[make_offset(block_base, block_number, reset_reg)] = 1;
    for (size_t i = 0; i < length; i ++)
        register_map[make_offset(block_base, block_number, fill_reg)] = data[i];
}


/* The bit updates interface consists of a burst read of 8 16-bit pairs (packed
 * into 32-bit words).  The upper 16-bits record the current bit value, the
 * bottom 16-bits whether the value has changed. */
void hw_read_bits(bool bits[BIT_BUS_COUNT], bool changes[BIT_BUS_COUNT])
{
    register_map[BIT_READ_RESET] = 1;
    for (unsigned int i = 0; i < BIT_BUS_COUNT / 16; i ++)
    {
        uint32_t word = register_map[BIT_READ_VALUE];
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
    register_map[POS_READ_RESET] = 1;
    for (unsigned int i = 0; i < POS_BUS_COUNT; i ++)
        positions[i] = register_map[POS_READ_VALUE];
    uint32_t word = register_map[POS_READ_CHANGES];
    for (unsigned int i = 0; i < POS_BUS_COUNT; i ++)
        changes[i] = (word >> i) & 1;
}


void hw_write_bit_capture(uint32_t capture_mask)
{
    register_map[BIT_CAPTURE_MASK] = capture_mask;
}

void hw_write_position_capture(uint32_t capture_mask)
{
    register_map[POS_CAPTURE_MASK] = capture_mask;
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

struct hw_long_table {
};


error__t hw_open_long_table(
    unsigned int block_base, unsigned int number, unsigned int order,
    struct hw_long_table **table, uint32_t **data, size_t *length)
{
    return FAIL_("Not implemented");
}


void hw_write_long_table_length(
    struct hw_long_table *table, size_t length)
{
    ASSERT_FAIL();
}


void hw_close_long_table(struct hw_long_table *table)
{
    ASSERT_FAIL();
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
