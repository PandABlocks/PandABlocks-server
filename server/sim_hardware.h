/* Hardware simulation support. */

/* Special support for long tables. */

/* Allocates a block of physically mappable memory of the specified size. */
error__t hw_long_table_allocate(
    unsigned int block_base, unsigned int number,
    unsigned int base_reg, unsigned int length_reg,
    unsigned int order,
    size_t *block_size, uint32_t **data, int *block_id);

/* Releases previously allocated table memory area. */
void hw_long_table_release(int block_id);

/* Performs write for long table. */
void hw_long_table_write(
    int block_id, const void *data, size_t length, size_t offset);
