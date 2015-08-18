/* Common files and structures for PandA driver. */

/* Initialises register map support. */
int panda_map_init(struct file_operations **fops, const char **name);

/* Dummy initialisers until we've worked out what we need. */
int panda_dummy1_init(struct file_operations **fops, const char **name);
int panda_dummy2_init(struct file_operations **fops, const char **name);
