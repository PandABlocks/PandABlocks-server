/* Hardware simulation support. */

/* Special support for long tables. */

/* Allocates a block of physically mappable memory of the specified size and
 * returns both virtual and physical addresses. */
error__t hw_long_table_allocate(
    unsigned int order, size_t *block_size,
    uint32_t **data, uint32_t *physical_addr, unsigned int *block_id);

/* Releases previously allocated table memory area. */
void hw_long_table_release(unsigned int block_id);

/* Flushes cache for specified area. */
void hw_long_table_flush(
    unsigned int block_id, size_t length,
    unsigned int block_base, unsigned int number);
