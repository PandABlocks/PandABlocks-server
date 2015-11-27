/* Abstract single register API for class and type support. */

struct register_api;


// This will move into type creation soon enough! */
struct register_methods {
    /* Reads current register value. */
    uint32_t (*read)(void *reg_data, unsigned int number);
    /* Writes to register. */
    void (*write)(void *reg_data, unsigned int number, uint32_t value);
};



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Register access API.  These are all wafer thin wrappers over the methods API
 * above. */

/* Returns current register value. */
uint32_t read_register(struct register_api *reg, unsigned int number);

/* Updates register value. */
void write_register(
    struct register_api *reg, unsigned int number, uint32_t value);


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Various register class creation methods. */

struct register_api *create_register_api(
    const struct register_methods *methods, void *data);

/* Call during shutdown to release resources. */
void destroy_register(struct register_api *reg);


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Class definitions for param, read, write classes. */

extern const struct class_methods param_class_methods;
extern const struct class_methods read_class_methods;
extern const struct class_methods write_class_methods;
