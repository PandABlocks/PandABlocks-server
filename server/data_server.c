/* Socket server for data streaming interface. */

#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "error.h"
#include "hardware.h"

#include "data_server.h"


void process_data_socket(int scon)
{
    const char *message = "Data connection not yet implemented\n";
    IGNORE(TEST_IO(write(scon, message, strlen(message))));
    close(scon);

    log_message("Connection terminated");
}
