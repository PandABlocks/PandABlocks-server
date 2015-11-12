/* Hardware interface definitions. */

#define BIT_BUS_COUNT   128
#define POS_BUS_COUNT   32


/* Must be called before any hardware functions.  If an error occurs then
 * program startup should be terminated. */
error__t initialise_hardware(void);

void terminate_hardware(void);

/* Read and write function block configuration registers.  Each function block
 * is identified by its function number or "block base", the block number within
 * that function, and finally the register within the block. */
void hw_write_config(
    unsigned int block_base, unsigned int block_number, unsigned int reg,
    uint32_t value);
uint32_t hw_read_data(
    unsigned int block_base, unsigned int block_number, unsigned int reg);

/* Write table data. */
void hw_write_table_data(
    unsigned int block_base, unsigned int block_number, unsigned int reg,
    bool start, const uint32_t data[], size_t length);

/* Read bit values and changes. */
void hw_read_bits(bool bits[BIT_BUS_COUNT], bool changes[BIT_BUS_COUNT]);

/* Read position values and changes. */
void hw_read_positions(
    uint32_t positions[POS_BUS_COUNT], bool changes[POS_BUS_COUNT]);
