/* Abstract single register API for class and type support. */

struct register_api;


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Register access API.  These are all wafer thin wrappers over the methods API
 * above. */

/* Returns current register value. */
uint32_t read_register(struct register_api *reg, unsigned int number);

/* Updates register value. */
void write_register(
    struct register_api *reg, unsigned int number, uint32_t value);

/* Returns change set associated with register. */
void register_change_set(
    struct register_api *reg, const uint64_t report_index[], bool changes[]);


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Various register class creation methods. */

/* Creates a simple parameter register: reading returns last read value, writing
 * writes through to hardware and caches value, and we have parameter change
 * support. */
struct register_api *create_param_register(unsigned int count);

/* Creates direct read-only and write only registers. */
struct register_api *create_read_register(unsigned int count);
struct register_api *create_write_register(unsigned int count);

/* Create register interfaces to bit_out and pos_out classes.  These two
 * implementations need the class data. */
struct register_api *create_bit_out_register(void *class_data);
struct register_api *create_pos_out_register(void *class_data);


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Register administration. */

/* Alas we don't get the block base address until after we've been created. */
error__t validate_register(struct register_api *reg, unsigned int block_base);

/* Call during shutdown to release resources. */
void destroy_register(struct register_api *reg);

/* Used to parse register definition line for register. */
error__t register_parse_register(const char **line, struct register_api *reg);
