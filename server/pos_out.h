/* pos_out field class. */

struct pos_out;

error__t initialise_pos_out(void);
void terminate_pos_out(void);

/* Returns scaling information for given pos_out field. */
size_t get_pos_out_info(
    struct pos_out *pos_out, unsigned int number,
    double *scale, double *offset, char units[], size_t length);

extern const struct class_methods pos_out_class_methods;
