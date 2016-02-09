/* Support for bit_out class. */

/* Update cached bit values from hardware. */
void do_bit_out_refresh(uint64_t change_index);


// !!!! Note: this will move from BIT system command to an attribute
/* BITSn.BITS? implementation, reports bit names in specific capture block. */
void report_capture_bits(struct connection_result *result, unsigned int group);

/* Used to set the associate bit group name. */
void set_bit_group_name(unsigned int group, const char *name);


error__t initialise_bit_out(void);
void terminate_bit_out(void);


/* Type and class definitions. */
extern const struct type_methods bit_mux_type_methods;
extern const struct class_methods bit_out_class_methods;
