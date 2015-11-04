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


static uint32_t *register_map;
static uint32_t register_map_size;


static unsigned int make_offset(
    unsigned int block_base, unsigned int block_number, unsigned int reg)
{
    return
        ((block_base & 0x1f) << 8) |
        ((block_number & 0xf) << 4) |
        (reg & 0xf);
}

void hw_write_config(
    unsigned int block_base, unsigned int block_number, unsigned int reg,
    uint32_t value)
{
    printf("hw_write_config %u:%u:%u <= %u\n",
        block_base, block_number, reg, value);
    register_map[make_offset(block_base, block_number, reg)] = value;
}

uint32_t hw_read_config(
    unsigned int block_base, unsigned int block_number, unsigned int reg)
{
    uint32_t result = register_map[make_offset(block_base, block_number, reg)];
    printf("hw_read_config %u:%u:%u => %u\n",
        block_base, block_number, reg, result);
    return result;
}


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
        TEST_IO(munmap(register_map, register_map_size))  ?:
        TEST_IO(close(map)),
        "Calling terminate_hardware");
}
