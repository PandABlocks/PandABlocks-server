/* Hardware interface definitions. */

#define BIT_BUS_COUNT   128
#define POS_BUS_COUNT   32

#define CLOCK_FREQUENCY 125000000       // 8ns per tick
#define MAX_CLOCK_VALUE ((1ULL << 48) - 1)


#define UNASSIGNED_REGISTER ((unsigned int) -1)


/* Must be called before any hardware functions.  If an error occurs then
 * program startup should be terminated. */
error__t initialise_hardware(void);

void terminate_hardware(void);


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Hardware setup. */

/* Sets block base address used for special named registers. */
void hw_set_block_base(unsigned int reg);

/* Sets register offset for given named register. */
error__t hw_set_named_register(const char *name, unsigned int reg);

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

/* Write short table data.  For short tables the entire data block is written
 * directly to hardware. */
void hw_write_short_table(
    unsigned int block_base, unsigned int block_number,
    unsigned int reset_reg, unsigned int fill_reg, unsigned int length_reg,
    const uint32_t data[], size_t length);


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Long tables. */

/* For long tables we have a memory mapped area.  After writing into the area we
 * need to release it so that the area can be flushed to RAM and the hardware
 * informed. */

struct hw_long_table;

/* This method is called during startup to prepare the long table structure and
 * open any device resources.  The data area length for each table is returned
 * together with an allocated table structure. */
error__t hw_open_long_table(
    unsigned int block_base, unsigned int count, unsigned int order,
    struct hw_long_table **table, size_t *length);

/* This retrieves the long table data area for the specified block.  The lengt
 * has already been returned by hw_open_long_table(). */
void hw_read_long_table_area(
    struct hw_long_table *table, unsigned int number, uint32_t **data);

/* Updates range of valid data for table. */
void hw_write_long_table_length(
    struct hw_long_table *table, unsigned int number, size_t length);

/* Call this during shutdown to release table and device resources. */
void hw_close_long_table(struct hw_long_table *table);


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Streamed data capture. */

/* The format of streamed data capture is determined by the configured capture
 * masks, set by hw_write_{bit,position}_capture() and internal format rules. */

/* This function should be called repeatedly in a dedicated thread to consume
 * streamed data captured by the hardware.  The number of bytes read into buffer
 * are returned.  If a capture sequence is complete then *data_end is set to
 * false.  Note that zero length results are normal and will be returned at
 * intervals determined by hardware timeout.
 *     This function is not expected to fail, and if there is an IO error in
 * communication with the hardware then the server might as well die. */
size_t hw_read_streamed_data(void *buffer, size_t length, bool *data_end);

/* This function controls the arm/disarm state of data capture.  Data capture is
 * armed by writing true with this function, after which hw_read_streamed_data()
 * should start returning calls with *data_end set to false.  Data streaming is
 * ended either through an internal hardware trigger or by writing false with
 * this function, in which case hw_read_streamed_data() will in due course
 * return with *data_end set to false.
 *     During the interval between calling hw_write_arm(true) and seeing
 * *data_end==true the data capture engine should be treated as locked. */
void hw_write_arm(bool enable);

/* Set bit and position capture masks.  These functions cannot be called while
 * the data capture engine is locked. */
void hw_write_bit_capture(uint32_t capture_mask);

/* Writes capture, frame mode and extended capture masks for the 32 position
 * capture bits.  Note that writing non zero values into reserved fields of the
 * extended_mask has an undefined effect. */
void hw_write_position_capture_masks(
    uint32_t capture_mask, uint32_t framed_mask, uint32_t extended_mask);
