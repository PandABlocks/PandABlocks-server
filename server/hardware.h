/* Hardware interface definitions. */

/* Must be called before any hardware functions.  If an error occurs then
 * program startup should be terminated. */
error__t initialise_hardware(void);

void terminate_hardware(void);

/* Read and write function block configuration register.  Each function block is
 * identified by its function number, the block number within that function, and
 * finally the register within the block. */
void hw_write_config(int function, int block, int reg, uint32_t value);
uint32_t hw_read_config(int function, int block, int reg);
