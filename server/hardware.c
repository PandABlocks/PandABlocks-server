/* Hardware interface for PandA. */

#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
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
    unsigned int function, unsigned int block, unsigned int reg)
{
    return ((function & 0x1f) << 8) | ((block & 0xf) << 4) | (reg & 0xf);
}

void hw_write_config(
    unsigned int function, unsigned int block, unsigned int reg,
    uint32_t value)
{
    register_map[make_offset(function, block, reg)] = value;
}

uint32_t hw_read_config(
    unsigned int function, unsigned int block, unsigned int reg)
{
    return register_map[make_offset(function, block, reg)];
}


error__t initialise_hardware(void)
{
#ifdef __arm__
    int map;
    return
        TEST_IO_(map = open("/dev/panda.map", O_RDWR | O_SYNC),
            "Unable to open PandA device")  ?:
        TEST_IO(ioctl(map, PANDA_MAP_SIZE, &register_map_size))  ?:
        TEST_IO(register_map = mmap(
            0, register_map_size, PROT_READ | PROT_WRITE, MAP_SHARED,
            map, 0));
#else
    /* Compiling simulation version. */
    register_map_size = 0x00040000;
    register_map = malloc(register_map_size);
    memset(register_map, 0x55, register_map_size);
    return ERROR_OK;
#endif
}


void terminate_hardware(void)
{
#ifdef __arm__
    ERROR_REPORT(
        TEST_IO(munmap(register_map, register_map_size))  ?:
        TEST_IO(close(map)), "Calling terminate_hardware");
#else
    if (register_map)
        free(register_map);
#endif
}
