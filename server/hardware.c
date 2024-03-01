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

static int map_file = -1;

static volatile uint32_t *register_map;
static size_t register_map_size;


struct register_fields {
    unsigned int reg : BLOCK_REGISTER_BITS;
    unsigned int number : BLOCK_INSTANCE_BITS;
    unsigned int type : BLOCK_TYPE_BITS;
    unsigned int _fill :
        32 - BLOCK_REGISTER_BITS - BLOCK_INSTANCE_BITS - BLOCK_TYPE_BITS;
};

/* This has to be bigger or same size than the linux kernel structure with the
 * same name.
 * We are using this to convert to a stardard timespec in a way that we are
 * compatible with 32-bit and 64-bit architectures, however, we will not
 * need it when we update to a newer glibc (which contains __timespec64) */
struct timespec64 {
    int64_t tv_sec;     /* Seconds */
    uint32_t tv_nsec;   /* Nanoseconds */
    uint32_t : 32;      /* padding */
};


static unsigned int make_offset(
    unsigned int block_base, unsigned int block_number, unsigned int reg)
{
    struct register_fields offset = {
        .reg = reg & (BLOCK_REGISTER_COUNT-1),
        .number = block_number & (BLOCK_INSTANCE_COUNT-1),
        .type = block_base & (BLOCK_TYPE_COUNT-1),
    };
    return CAST_FROM_TO(typeof(offset), unsigned int, offset);
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
    return register_map[make_offset(block_base, block_number, reg)];
}

#endif



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Named register support. */

/* Named registers. */

/* Structure for validing registers during startup. */
struct named_register {
    const char *name;
    unsigned int range;
    bool seen;
};

/* Similar structure for named constants. */
struct named_constant {
    const char *name;
    unsigned int value;
    bool allow_default;
    bool seen;
};

/* This #include statement pulls in definitions of hardware control registers
 * defined in the *REG section of the register configuration.  During startup we
 * check that we are loading the same configuration by cross-checking the
 * register definitions.  This header is generated by the script
 * named_registers.py and contains #define statements for each hardware register
 * in the *REG block, together with struct named_register section for validation
 * during startup. */
#include "named_registers.h"


error__t hw_set_block_base(unsigned int reg)
{
    return TEST_OK_(reg == REG_BLOCK_BASE, "*REG block base mismatch");
}


error__t hw_set_named_register_range(
    const char *name, unsigned int start, unsigned int end)
{
    struct named_register *reg = &named_registers[start];
    return
        TEST_OK_(start < ARRAY_SIZE(named_registers),
            "Register out of range")  ?:
        TEST_OK_(reg->name  &&  strcmp(reg->name, name) == 0,
            "Wrong offset value for this register")  ?:
        TEST_OK_(!reg->seen, "Register already assigned")  ?:
        TEST_OK_(end == start + reg->range - 1, "Invalid range of values")  ?:
        DO(reg->seen = true);
}


error__t hw_set_named_register(const char *name, unsigned int reg)
{
    return hw_set_named_register_range(name, reg, reg);
}


error__t hw_set_named_constant(const char *name, unsigned int value)
{
    for (unsigned int i = 0; i < ARRAY_SIZE(named_constants); i ++)
    {
        struct named_constant *constant = &named_constants[i];
        if (strcmp(constant->name, name) == 0)
            return
                TEST_OK_(!constant->seen, "Repeated constant %s", name)  ?:
                DO(constant->seen = true)  ?:
                TEST_OK_(constant->value == value,
                    "Unexpected value for constant %s: %u != %u",
                    name, value, constant->value);
    }
    return FAIL_("Unknown constant %s=%u in registers file", name, value);
}


error__t hw_validate(void)
{
    for (unsigned int i = 0; i < ARRAY_SIZE(named_registers); i ++)
    {
        struct named_register *reg = &named_registers[i];
        if (reg->name  &&  !reg->seen)
            return FAIL_("Register %s not in *REG list", reg->name);
    }
    for (unsigned int i = 0; i < ARRAY_SIZE(named_constants); i ++)
    {
        struct named_constant *constant = &named_constants[i];
        if (!constant->seen  &&  !constant->allow_default)
            return FAIL_("Constant %s not seen in registers file",
                constant->name);
    }
    return ERROR_OK;
}


static inline void write_named_register(unsigned int offset, uint32_t value)
{
    hw_write_register(REG_BLOCK_BASE, 0, offset, value);
}


static inline uint32_t read_named_register(unsigned int offset)
{
    return hw_read_register(REG_BLOCK_BASE, 0, offset);
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


void hw_read_versions(
    uint32_t *fpga_version, uint32_t *fpga_build, uint32_t *user_version)
{
    *fpga_version = read_named_register(FPGA_VERSION);
    *fpga_build   = read_named_register(FPGA_BUILD);
    *user_version = read_named_register(USER_VERSION);
}


void hw_write_mac_address(unsigned int offset, uint64_t mac_address)
{
    ASSERT_OK(offset < MAC_ADDRESS_COUNT);
    write_named_register(MAC_ADDRESS_BASE + 2*offset,
        (uint32_t) (mac_address & 0xffffff));
    write_named_register(MAC_ADDRESS_BASE + 2*offset + 1,
        (uint32_t) ((mac_address >> 24) & 0xffffff));
}


uint32_t hw_read_fpga_capabilities(void)
{
    return read_named_register(FPGA_CAPABILITIES);
}


uint32_t hw_read_nominal_clock(void)
{
    uint32_t frequency = read_named_register(NOMINAL_CLOCK);
    if (frequency > 0)
        return frequency;
    else
        return NOMINAL_CLOCK_FREQUENCY;
}


/******************************************************************************/
/* Data capture. */

#ifndef SIM_HARDWARE

static int stream = -1;


size_t hw_read_streamed_data(void *buffer, size_t length, bool *data_end)
{
    ssize_t count = read(stream, buffer, length);
    if (count < 0  &&  errno == EAGAIN)
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
    else if (error_report(TEST_IO(count)))
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


void hw_write_arm_streamed_data(void)
{
    error_report(TEST_IO(ioctl(stream, PANDA_DMA_ARM)));
}


unsigned int hw_read_streamed_completion(void)
{
    uint32_t completion = 0;
    error_report(TEST_IO(ioctl(stream, PANDA_COMPLETION, &completion)));
    return completion;
}


bool hw_get_start_ts(struct timespec *ts)
{
    struct timespec64 compat_ts = {};
    if (ioctl(stream, PANDA_GET_START_TS, &compat_ts) == -1)
    {
        // EAGAIN indicates the timestamp hasn't been captured yet
        if (errno != EAGAIN)
            error_report(TEST_IO(-1));
        return false;
    }
    else
    {
        ts->tv_sec = (time_t) compat_ts.tv_sec;
        ts->tv_nsec = (typeof(ts->tv_nsec)) compat_ts.tv_nsec;
        return true;
    }
}


bool hw_get_hw_start_ts(struct timespec *ts)
{
    ts->tv_sec = (time_t) read_named_register(PCAP_TS_SEC);
    ts->tv_nsec = (typeof(ts->tv_nsec)) ((uint64_t)
        read_named_register(PCAP_TS_TICKS) * NSECS / hw_read_nominal_clock());
    ts->tv_sec += ts->tv_nsec / NSECS;
    ts->tv_nsec = ts->tv_nsec % NSECS;
    return true;
}

#endif


const char *hw_decode_completion(unsigned int completion)
{
    /* Test bits in the completion code until a bit is found.  The order here
     * determines the priority of report. */
    if (completion == 0)
        return "Ok";
    else if (completion & PANDA_COMPLETION_DMA)
        return "DMA data error";
    else if (completion & PANDA_COMPLETION_OVERRUN)
        return "Driver data overrun";
    else if (completion & PANDA_COMPLETION_FRAMING)
        return "Framing error";
    else if (completion & PANDA_COMPLETION_DISARM)
        return "Disarmed";
    else
        return "Unexpected completion error";
}


void hw_write_arm(bool enable)
{
    if (enable)
        write_named_register(PCAP_ARM, 0);
    else
        write_named_register(PCAP_DISARM, 0);
}


void hw_write_capture_set(const unsigned int capture[], size_t count)
{
    ASSERT_OK(count < MAX_PCAP_WRITE_COUNT);

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
    int *block_ids;             // Id for table
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
    unsigned int block_base, unsigned int number,
    unsigned int base_reg, unsigned int length_reg,
    unsigned int order, size_t *block_size,
    uint32_t **data, int *block_id)
{
    struct panda_block block = {
        .order = order,
        .block_base   =
            sizeof(uint32_t) * make_offset(block_base, number, base_reg),
        .block_length =
            sizeof(uint32_t) * make_offset(block_base, number, length_reg),
    };
    return
        TEST_IO_(*block_id = open("/dev/panda.block", O_RDWR | O_SYNC),
            "Unable to open PandA device /dev/panda.block")  ?:
        TEST_IO(*block_size = (uint32_t) ioctl(
            *block_id, PANDA_BLOCK_CREATE, &block))  ?:
        TEST_IO(*data = mmap(
            0, *block_size, PROT_READ, MAP_SHARED, *block_id, 0));
}


static void hw_long_table_release(int block_id)
{
    close(block_id);
}


static void hw_long_table_write(
    int block_id, const void *data, size_t length, size_t offset)
{
    ASSERT_OK_IO(lseek(block_id, (off_t) offset, SEEK_SET) == (off_t) offset);
    ASSERT_OK_IO(write(block_id, data, length) == (ssize_t) length);
}
#endif


static error__t create_long_table(
    struct hw_table *table, unsigned int order, size_t *block_length,
    unsigned int base_reg, unsigned int length_reg)
{
    table->long_table = (struct long_table) {
        .block_ids = malloc(table->count * sizeof(int)),
    };
    memset(table->long_table.block_ids, 0, table->count * sizeof(unsigned int));

    size_t block_size = 0;
    error__t error = ERROR_OK;
    for (unsigned int i = 0; !error  &&  i < table->count; i ++)
        error = hw_long_table_allocate(
            table->block_base, i, base_reg, length_reg,
            order, &block_size,
            &table->data[i], &table->long_table.block_ids[i]);
    *block_length = block_size / sizeof(uint32_t);
    return error;
}


static void destroy_long_table(struct hw_table *table)
{
    for (unsigned int i = 0; i < table->count; i ++)
        hw_long_table_release(table->long_table.block_ids[i]);
    free(table->long_table.block_ids);
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
    /* The order is log2 number of pages, so the corresponding length in words
     * uses 1024 words per page (we're happy to hard-wire the page size of 4096
     * here). */
    *table = create_hw_table(block_base, block_count, LONG_TABLE);
    return create_long_table(*table, order, length, base_reg, length_reg);
}


const uint32_t *hw_read_table_data(struct hw_table *table, unsigned int number)
{
    return table->data[number];
}


void hw_write_table(
    struct hw_table *table, unsigned int number,
    size_t offset, const uint32_t data[], size_t length)
{
    switch (table->table_type)
    {
        case SHORT_TABLE:
            memcpy(table->data[number] + offset,
                data, length * sizeof(uint32_t));
            write_short_table(table, number, offset + length);
            break;
        case LONG_TABLE:
            hw_long_table_write(
                table->long_table.block_ids[number], data,
                length * sizeof(uint32_t), offset * sizeof(uint32_t));
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


bool sim_hardware(void)
{
#ifdef SIM_HARDWARE
    return true;
#else
    return false;
#endif
}


#ifndef SIM_HARDWARE

error__t initialise_hardware(void)
{
    return
        TEST_IO_(map_file = open("/dev/panda.map", O_RDWR | O_SYNC),
            "Unable to open PandA device /dev/panda.map")  ?:
        TEST_IO(register_map_size =
            (size_t) ioctl(map_file, PANDA_MAP_SIZE))  ?:
        TEST_IO(register_map = mmap(
            0, register_map_size,
            PROT_READ | PROT_WRITE, MAP_SHARED, map_file, 0))  ?:

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
        IF(map_file >= 0, TEST_IO(close(map_file)))  ?:
        IF(stream >= 0, TEST_IO(close(stream))),
        "Calling terminate_hardware");
}

#endif
