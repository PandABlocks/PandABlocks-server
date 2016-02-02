/* Data capture control. */

#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <pthread.h>

#include "error.h"
#include "parse.h"
#include "config_server.h"
#include "data_server.h"
#include "classes.h"
#include "output.h"
#include "locking.h"
#include "hardware.h"

#include "capture.h"



void prepare_data_capture(void)
{
    printf("Write capture masks\n");
    printf("Write capture set\n");
}
