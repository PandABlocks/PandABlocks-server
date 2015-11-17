/* Interface to database of record definitions. */

/* This function loads the configuration databases into memory. */
error__t load_config_databases(
    const char *config_db, const char *register_db,
    const char *description_db);

void terminate_databases(void);
