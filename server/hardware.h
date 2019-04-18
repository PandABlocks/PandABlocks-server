/* Hardware interface definitions. */

#define BIT_BUS_COUNT       128
#define POS_BUS_COUNT       32
#define EXT_BUS_COUNT       16


#define CLOCK_FREQUENCY 125000000       // 8ns per tick
#define MAX_CLOCK_VALUE ((1ULL << 48) - 1)


#define UNASSIGNED_REGISTER ((unsigned int) -1)
/* The following fields determine the structure of the block register addressing
 * scheme.  We have a fixed number of block types, each block has a possible
 * number of instances, each instance has a number of registers. */
#define BLOCK_TYPE_BITS         5       // 32 possible block types
#define BLOCK_INSTANCE_BITS     4       // up to 16 instances per block
#define BLOCK_REGISTER_BITS     6       // 64 registers per block
#define BLOCK_TYPE_COUNT        (1U << BLOCK_TYPE_BITS)
#define BLOCK_INSTANCE_COUNT    (1U << BLOCK_INSTANCE_BITS)
#define BLOCK_REGISTER_COUNT    (1U << BLOCK_REGISTER_BITS)


/* Special codings for reserved bit bus and position bus indices. */
#define BIT_BUS_ZERO        BIT_BUS_COUNT
#define BIT_BUS_ONE         (BIT_BUS_COUNT + 1)
#define POS_BUS_ZERO        POS_BUS_COUNT


/* Must be called before any hardware functions.  If an error occurs then
 * program startup should be terminated. */
error__t initialise_hardware(void);

void terminate_hardware(void);

/* Checks whether we're in simulation mode. */
bool sim_hardware(void);


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Hardware setup. */

/* Sets block base address used for special named registers. */
error__t hw_set_block_base(unsigned int reg);

/* Sets register offset for given named register. */
error__t hw_set_named_register(const char *name, unsigned int reg);

/* Sets register range for given named register. */
error__t hw_set_named_register_range(
    const char *name, unsigned int start, unsigned int end);

/* Checks that all register offsets have been set. */
error__t hw_validate(void);


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Configuration and simple hardware control. */

/* Read and write function block configuration registers.  Each function block
 * is identified by its function number or "block base", the block number within
 * that function, and finally the register within the block. */
void hw_write_register(
    unsigned int block_base, unsigned int block_number, unsigned int reg,
    uint32_t value);
uint32_t hw_read_register(
    unsigned int block_base, unsigned int block_number, unsigned int reg);

/* Read bit values and changes. */
void hw_read_bits(bool bits[BIT_BUS_COUNT], bool changes[BIT_BUS_COUNT]);

/* Read position values and changes. */
void hw_read_positions(
    uint32_t positions[POS_BUS_COUNT], bool changes[POS_BUS_COUNT]);

/* Reads the three version registers. */
void hw_read_versions(
    uint32_t *fpga_version, uint32_t *fpga_build, uint32_t *user_version);

/* Writes to one of the dedicated MAC address registers. */
#define MAC_ADDRESS_COUNT   4   // Offset must be smaller than this
void hw_write_mac_address(unsigned int offset, uint64_t mac_address);


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Table API. */

/* The API for short and long tables is pretty similar: open the table, then
 * write blocks. */

struct hw_table;    // Common interface to long and short tables


/* Creates short tables with given control registers.  All the tables for the
 * selected block are opened together with this call. */
error__t hw_open_short_table(
    unsigned int block_base, unsigned int block_number,
    unsigned int reset_reg, unsigned int fill_reg, unsigned int length_reg,
    size_t max_length, struct hw_table **table);

/* Creates long table.  The size is specified as a power of 2 and the actual
 * maximum length in words is returned. */
error__t hw_open_long_table(
    unsigned int block_base, unsigned int block_count, unsigned int order,
    unsigned int base_reg, unsigned int length_reg,
    struct hw_table **table, size_t *length);

/* When called during initialisation returns data area of block for readback. */
const uint32_t *hw_read_table_data(struct hw_table *table, unsigned int number);


/* Writes given block of data to table. */
void hw_write_table(
    struct hw_table *table, unsigned int number,
    size_t offset, const uint32_t data[], size_t length);

/* Releases table resources during server shutdown. */
void hw_close_table(struct hw_table *table);


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Streamed data capture. */

/* The format of streamed data capture is determined by the configured capture
 * masks, set by hw_write_{bit,position}_capture() and internal format rules. */

/* This function must be called before starting to read captured data. */
void hw_write_arm_streamed_data(void);

/* This function should be called repeatedly in a dedicated thread to consume
 * streamed data captured by the hardware.  The number of bytes read into buffer
 * are returned.  If a capture sequence is complete then *data_end is set to
 * false.  Note that zero length results are normal and will be returned at
 * intervals determined by hardware timeout.
 *     This function is not expected to fail, and if there is an IO error in
 * communication with the hardware then the server might as well die. */
size_t hw_read_streamed_data(void *buffer, size_t length, bool *data_end);

/* This returns the completion code after hw_read_streamed_data has returned
 * data_end. */
unsigned int hw_read_streamed_completion(void);
/* Converts the completion code into a printable string. */
const char *hw_decode_completion(unsigned int completion);

/* This function controls the arm/disarm state of data capture.  Data capture is
 * armed by writing true with this function, after which hw_read_streamed_data()
 * should start returning calls with *data_end set to false.  Data streaming is
 * ended either through an internal hardware trigger or by writing false with
 * this function, in which case hw_read_streamed_data() will in due course
 * return with *data_end set to false.
 *     During the interval between calling hw_write_arm(true) and seeing
 * *data_end==true the data capture engine should be treated as locked. */
void hw_write_arm(bool enable);

/* Writes the capture delay register. */
void hw_write_data_delay(unsigned int capture_index, unsigned int delay);


/* Macros for formatting position and extension bus entries into capture field
 * values.  The bit layout for each type is as follows:
 *
 *                  32            9 8        4   2    0
 *                  +------------+-+----------+-+------+
 *  Position bus    |        0   |0|  pos-ix  |0| mode |
 *                  +------------+-+----------+-+------+
 *
 *                                9   7      4
 *                  +------------+-+-+--------+--------+
 *  Extension bus   |        0   |1|0| ext-ix |    0   |
 *                  +------------+-+-+--------+--------+
 */
#define CAPTURE_POS_BUS(pos_ix, mode) \
    ((((pos_ix) & 0x1F) << 4) | ((mode) & 0x7))
#define CAPTURE_EXT_BUS(ext_ix) \
    ((1 << 9) | (((ext_ix) & 0xF) << 4))

/* Definitions of position capture fields. */
#define POS_FIELD_VALUE         0
#define POS_FIELD_DIFF          1
#define POS_FIELD_SUM_LOW       2
#define POS_FIELD_SUM_HIGH      3
#define POS_FIELD_MIN           4
#define POS_FIELD_MAX           5

/* Writes list of capture bus fields to capture. */
void hw_write_capture_set(const unsigned int capture[], size_t count);
