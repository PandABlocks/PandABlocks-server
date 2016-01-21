/* Hardware simulation support. */

/* Special support for long tables. */

/* Allocates a block of physically mappable memory of the specified size and
 * returns both virtual and physical addresses. */
error__t hw_long_table_allocate(
    unsigned int order,
    uint32_t **data, uint32_t *physical_addr, void **table_id);

/* Releases previously allocated table memory area. */
void hw_long_table_release(void *table_id);

/* Flushes cache for specified area. */
void hw_long_table_flush(
    void *table_id, size_t length,
    unsigned int block_base, unsigned int number);
