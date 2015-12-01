/* Socket server for data streaming interface. */

#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

#include "error.h"
#include "hardware.h"

#include "data_server.h"


error__t process_data_socket(int scon)
{
    const char *message = "Data connection not yet implemented\n";
    return TEST_IO(write(scon, message, strlen(message)));
}
