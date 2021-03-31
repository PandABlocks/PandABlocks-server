/* Extension server for non-FPGA registers. */

/* Initialise connection to extension server. */
error__t initialise_extension_server(unsigned int port);

/* Call during shutdown to terminate connection to server. */
void terminate_extension_server(void);


struct extension_block;
struct extension_address;

/* Create a block extension specification. */
error__t parse_extension_block(
    const char **line, unsigned int count, struct extension_block **block);

void destroy_extension_block(struct extension_block *block);


/* Parses an extension address definition and returns an appropriate structure
 * which can be used for subsequent register read and write operations. */
error__t parse_extension_register(
    const char **line, struct extension_block *block,
    bool write_not_read, struct extension_address **address);

void destroy_extension_address(struct extension_address *address);

/* Writes the given value to the given extension register. */
void extension_write_register(
    const struct extension_address *address,
    unsigned int block_base, unsigned int number, uint32_t value);

/* Returns current value of the given extension register. */
uint32_t extension_read_register(
    const struct extension_address *address,
    unsigned int block_base, unsigned int number);
