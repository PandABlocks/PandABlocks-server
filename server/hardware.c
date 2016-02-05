/* Hardware interface for PandA. */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <pthread.h>

#include "panda_device.h"

#include "error.h"
#include "locking.h"

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
        ((block_base & 0x1f) << 10) |   // 5 bits for block identifier
        ((block_number & 0xf) << 6) |   // 4 bits for block number
        (reg & 0x3f);                   // 6 bits for register within block
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

/* All of these register names also appear in the *REG fields of the register
 * configuration file.  For simplicity, because we need to refer to the
 * registers directly by number in this file, all we do is check that the named
 * registers in *REG have precisely the offsets we expect here. */
#define FPGA_VERSION            0
#define FPGA_BUILD              1
#define SLOW_VERSION            2
#define BIT_READ_RST            3
#define BIT_READ_VALUE          4
#define POS_READ_RST            5
#define POS_READ_VALUE          6
#define POS_READ_CHANGES        7
#define PCAP_START_WRITE        8
#define PCAP_WRITE              9
#define PCAP_FRAMING_MASK       10
#define PCAP_FRAMING_ENABLE     11
#define PCAP_FRAMING_MODE       12
#define PCAP_ARM                13
#define PCAP_DISARM             14
#define SLOW_REGISTER_STATUS    15

#define NAMED_REGISTER(name)    [name] = #name
static const char *named_registers[] = {
    NAMED_REGISTER(FPGA_VERSION),
    NAMED_REGISTER(FPGA_BUILD),
    NAMED_REGISTER(SLOW_VERSION),
    NAMED_REGISTER(BIT_READ_RST),
    NAMED_REGISTER(BIT_READ_VALUE),
    NAMED_REGISTER(POS_READ_RST),
    NAMED_REGISTER(POS_READ_VALUE),
    NAMED_REGISTER(POS_READ_CHANGES),
    NAMED_REGISTER(PCAP_START_WRITE),
    NAMED_REGISTER(PCAP_WRITE),
    NAMED_REGISTER(PCAP_FRAMING_MASK),
    NAMED_REGISTER(PCAP_FRAMING_ENABLE),
    NAMED_REGISTER(PCAP_FRAMING_MODE),
    NAMED_REGISTER(PCAP_ARM),
    NAMED_REGISTER(PCAP_DISARM),
    NAMED_REGISTER(SLOW_REGISTER_STATUS),
};
static bool named_register_seen[ARRAY_SIZE(named_registers)] = { };

static unsigned int reg_block_base = UNASSIGNED_REGISTER;


void hw_set_block_base(unsigned int reg)
{
    reg_block_base = reg;
}


error__t hw_set_named_register(const char *name, unsigned int reg)
{
    for (unsigned int i = 0; i < ARRAY_SIZE(named_registers); i ++)
        if (strcmp(named_registers[i], name) == 0)
            return
                TEST_OK_(i == reg,
                    "Register *REG.%s expected with offset %u", name, i)  ?:
                TEST_OK_(!named_register_seen[i], "Register recorded twice")  ?:
                DO(named_register_seen[i] = true);
    return FAIL_("Invalid register name");
}

error__t hw_validate(void)
{
    for (unsigned int i = 0; i < ARRAY_SIZE(named_registers); i ++)
        if (!named_register_seen[i])
            return FAIL_("Register %s not in *REG list", named_registers[i]);
    return ERROR_OK;
}


static inline void write_named_register(unsigned int offset, uint32_t value)
{
    hw_write_register(reg_block_base, 0, offset, value);
}


static inline uint32_t read_named_register(unsigned int offset)
{
    return hw_read_register(reg_block_base, 0, offset);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Slow register access has the extra problem that it can fail. */


static pthread_mutex_t slow_mutex = PTHREAD_MUTEX_INITIALIZER;

static unsigned int busy_wait_timeout = 1000;


/* This waits for the given named register to go to zero.  This should happen
 * within a few microseconds. */
static void wait_for_slow_ready(void)
{
    for (unsigned int i = 0; i < busy_wait_timeout; i ++)
        if (read_named_register(SLOW_REGISTER_STATUS) == 0)
            return;

    /* Damn.  Looks like we're stuck.  Log this but return anyway.  We're
     * probably going to storm the log with errors. */
    log_error("SLOW_REGISTER_STATUS stuck at non-zero value");
}


/* To write a slow register we need to check that the write buffer is empty
 * before initiating the write. */
void hw_write_slow_register(
    unsigned int block_base, unsigned int block_number, unsigned int reg,
    uint32_t value)
{
    LOCK(slow_mutex);
    /* Wait for any preceding write to complete. */
    wait_for_slow_ready();
    /* Initiate write. */
    hw_write_register(block_base, block_number, reg, value);
    UNLOCK(slow_mutex);
}


/* Reading a slow register is a bit more involved: we read the status (as a
 * sanity check), write the target register to initiate the read, then wait for
 * status to clear before finally returning the result. */
uint32_t hw_read_slow_register(
    unsigned int block_base, unsigned int block_number, unsigned int reg)
{
    LOCK(slow_mutex);
    /* Wait for any preceding write to complete. */
    wait_for_slow_ready();
    /* Initiate the read. */
    hw_write_register(block_base, block_number, reg, 0);
    /* Wait for read to complete. */
    wait_for_slow_ready();
    /* Retrieve the result. */
    uint32_t result = hw_read_register(block_base, block_number, reg);
    UNLOCK(slow_mutex);
    return result;
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


/******************************************************************************/
/* Data capture. */

#ifndef SIM_HARDWARE

static int stream = -1;


size_t hw_read_streamed_data(void *buffer, size_t length, bool *data_end)
{
    ssize_t count = read(stream, buffer, length);
    if (count == EAGAIN)
    {
        /* Read timed out at hardware level (this is normal). */
        *data_end = false;
        return 0;
    }
    else if (count == 0)
    {
        /* Nothing more from this capture stream.  This particular device will
         * allow us to pick up again once data capture is restarted. */
        *data_end = true;
        return 0;
    }
    else if (ERROR_REPORT(TEST_OK(count), "Error reading /dev/panda.stream"))
    {
        /* Well, that was unexpected.  Presume there's no more data. */
        *data_end = true;
        return 0;
    }
    else
    {
        /* All in order, we have data. */
        *data_end = false;
        return (size_t) count;
    }
}

#endif


void hw_write_arm(bool enable)
{
    if (enable)
        write_named_register(PCAP_ARM, 0);
    else
        write_named_register(PCAP_DISARM, 0);
}


void hw_write_framing_mask(uint32_t framing_mask, uint32_t framing_mode)
{
    write_named_register(PCAP_FRAMING_MASK, framing_mask);
    write_named_register(PCAP_FRAMING_MODE, framing_mode);
}


void hw_write_framing_enable(bool enable)
{
    write_named_register(PCAP_FRAMING_ENABLE, enable);
}


void hw_write_capture_set(const unsigned int capture[], size_t count)
{
    write_named_register(PCAP_START_WRITE, 0);
    for (size_t i = 0; i < count; i ++)
        write_named_register(PCAP_WRITE, capture[i]);
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

static int map = -1;

error__t initialise_hardware(void)
{
    return
        TEST_IO_(map = open("/dev/panda.map", O_RDWR | O_SYNC),
            "Unable to open PandA device /dev/panda.map")  ?:
        TEST_IO(ioctl(map, PANDA_MAP_SIZE, &register_map_size))  ?:
        TEST_IO(register_map = mmap(
            0, register_map_size,
            PROT_READ | PROT_WRITE, MAP_SHARED, map, 0))  ?:

        TEST_IO_(stream = open("/dev/panda.stream", O_RDONLY),
            "Unable to open PandA device /dev/panda.stream");
}


void terminate_hardware(void)
{
    ERROR_REPORT(
        IF(register_map,
            TEST_IO(munmap(
                CAST_FROM_TO(volatile uint32_t *, void *, register_map),
                register_map_size)))  ?:
        IF(map >= 0, TEST_IO(close(map)))  ?:
        IF(stream >= 0, TEST_IO(close(stream))),
        "Calling terminate_hardware");
}

#endif
