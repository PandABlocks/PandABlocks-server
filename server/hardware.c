/* Hardware interface for PandA. */

#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "panda_device.h"

#include "error.h"
#include "hardware.h"

#define CONTROL_AREA_SIZE

static uint32_t *register_map;
static uint32_t register_map_size;


static int make_offset(int function, int block, int reg)
{
    return ((function & 0x1f) << 8) | ((block & 0xf) << 4) | (reg & 0xf);
}

void hw_write_config(int function, int block, int reg, uint32_t value)
{
    register_map[make_offset(function, block, reg)] = value;
}

uint32_t hw_read_config(int function, int block, int reg)
{
    return register_map[make_offset(function, block, reg)];
}


bool initialise_hardware(void)
{
#ifdef __arm__
    int map;
    return
        TEST_IO_(map = open("/dev/panda.map", O_RDWR | O_SYNC),
            "Unable to open PandA device")  &&
        TEST_IO(ioctl(map, PANDA_MAP_SIZE, &register_map_size))  &&
        TEST_IO(register_map = mmap(
            0, register_map_size, PROT_READ | PROT_WRITE, MAP_SHARED,
            map, 0));
#else
    /* Compiling simulation version. */
    register_map_size = 0x00040000;
    register_map = malloc(register_map_size);
    return true;
#endif
}
