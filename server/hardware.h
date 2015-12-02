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


/* Hardware setup. */

/* Sets block base address used for special named registers. */
void hw_set_block_base(unsigned int reg);

/* Sets register offset for given named register. */
error__t hw_set_named_register(const char *name, unsigned int reg);

/* Checks that all register offsets have been set. */
error__t hw_validate(void);



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

/* Set bit and position capture masks. */
void hw_write_bit_capture(uint32_t capture_mask);
void hw_write_position_capture(uint32_t capture_mask);

/* Write short table data.  For short tables the entire data block is written
 * directly to hardware. */
void hw_write_short_table(
    unsigned int block_base, unsigned int block_number,
    unsigned int reset_reg, unsigned int fill_reg,
    const uint32_t data[], size_t length);


/* Long table management.  For long tables we have a memory mapped area.  After
 * writing into the area we need to release it so that the area can be flushed
 * to RAM and the hardware informed. */

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
