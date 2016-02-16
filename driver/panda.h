/* Common files and structures for PandA driver. */

/* Initialises register map support. */
int panda_map_init(struct file_operations **fops);

/* Initialises table block support. */
int panda_block_init(struct file_operations **fops);

/* Initialises stream support. */
int panda_stream_init(struct file_operations **fops);
