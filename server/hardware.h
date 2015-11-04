/* Hardware interface definitions. */

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
uint32_t hw_read_config(
    unsigned int block_base, unsigned int block_number, unsigned int reg);
