/* This file is part of Power Supply Controller project
 * Copyright (C) 2008  Isa Uzun, Diamond Light Source Ltd.
 *
 * Contact:
 *      Dr. Isa Uzun,
 *      Diamond Light Source Ltd,
 *      Diamond House,
 *      Chilton,
 *      Didcot,
 *      Oxfordshire,
 *      OX11 0DE
 *      isa.uzun@diamond.ac.uk
 */

/* FPGALoad - Utility for donwloading PSC Xilinx Spartan3E firmware prom
 * file using SBC. Firmware must be stored in binary format.
 *
 * Following Colibri PXA270 GPIOs are used to interface to FPGA for programming.
 *    GPIO11 -> CCLK  ( output, initialise 0 )
 *    GPIO12 -> D0    ( output, initialise 0 )
 *    GPIO14 -> PROGB ( output, initialise 1 )
 *    GPIO16 -> DONE  ( input  )
 *    GPIO19 -> INIT  ( input  )
 *
 *
 *
 */

/*                    _         ______________________________
 PROG_B (output) XXXXX |_______|
                                   ___________________________
 INIT_B (input ) XXXXXXXXX________|
                                           _        _
 CCLK   (output) XXXXX____________________| |______| |________

 DO     (output) XXXXX_____________________X________X_________
 */



#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <ctype.h>
#include <termios.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <time.h>

#define ASSERT(e) if(!(e)) assert_fail(__FILE__, __LINE__)

static void assert_fail(const char *filename, int linenumber)
{
    fprintf(stderr, "Error at line %d, file %s (%d) [%s]\n",
        linenumber, filename, errno, strerror(errno));
    exit(1);
}


/* The GPIOs we are using.  These are all indexes into the gpio_info table
 * below, configure that table for the correct GPIO numbers. */
#define GPIO_CCLK   0
#define GPIO_D0     1
#define GPIO_PROGB  2
#define GPIO_DONE   3
#define GPIO_INIT   4
#define GPIO_M0     5


/* GPIO access table: a table of open file handles for the five GPIOs we are
 * controlling. */

/* This offset is added to each GPIO number to map from hardware pin number to
 * kernel identifier. */
#define GPIO_OFFSET     906

struct gpio_info {
    int gpio;
    int file;
};
static struct gpio_info gpio_info[] = {
    [GPIO_CCLK]  = { .gpio = GPIO_OFFSET + 9, },
    [GPIO_D0]    = { .gpio = GPIO_OFFSET + 10, },
    [GPIO_PROGB] = { .gpio = GPIO_OFFSET + 13, },
    [GPIO_DONE]  = { .gpio = GPIO_OFFSET + 11, },
    [GPIO_INIT]  = { .gpio = GPIO_OFFSET + 12, },
    [GPIO_M0]    = { .gpio = GPIO_OFFSET + 0, },
};


static bool read_gpio(int gpio)
{
    char buf[16];
    ASSERT(read(gpio_info[gpio].file, buf, sizeof(buf)) > 0);
printf("read_gpio %d %d => %s\n", gpio, gpio_info[gpio].file, buf);
    return buf[0] == '1';
}

static void write_gpio(int gpio, bool value)
{
//     char buf_out[3] = { value, '\n', 0 };
    char buf_out[3] = " ";
    buf_out[0] = value ? '1' : '0';
printf("writing %s to %d %d\n", buf_out, gpio, gpio_info[gpio].file);
    ASSERT(write(gpio_info[gpio].file, buf_out, 1) == 1);
}




/* Writes given formatted string to file. */
static void write_to_file(const char *file_name, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    char string[256];
    int len = vsnprintf(string, sizeof(string), format, args);
    va_end(args);

    int file = open(file_name, O_WRONLY);
    ASSERT(file != -1);
    ASSERT(write(file, string, (size_t) len) == len);
    close(file);
}


/* Configures given GPIO for input or output and opens it for access. */
static void configure_gpio(int gpio, bool output)
{
    struct gpio_info *info = &gpio_info[gpio];
    write_to_file("/sys/class/gpio/export", "%d", info->gpio);

    const char *direction = output ? "out" : "in";
//     int flags = output ? O_WRONLY : O_RDONLY;
    int flags = O_RDWR;

    char filename[64];
    snprintf(filename, sizeof(filename),
        "/sys/class/gpio/gpio%d/direction", info->gpio);
    write_to_file(filename, direction);

    /* Finally open the newly opened gpio. */
    snprintf(filename, sizeof(filename),
        "/sys/class/gpio/gpio%d/value", info->gpio);
    info->file = open(filename, flags);
    ASSERT(info->file != -1);
}


/* Releases GPIO configuration. */
static void unconfigure_gpio(int gpio)
{
    struct gpio_info *info = &gpio_info[gpio];
    close(info->file);
    write_to_file("/sys/class/gpio/unexport", "%d", info->gpio);
}


/*
 Following GPIOs are used to interface to FPGA for programming.
    GPIO11 -> CCLK  ( output, initialise 0 )
    GPIO12 -> D0    ( output, initialise 0 )
    GPIO14 -> PROGB ( output, initialise 1 )
    GPIO16 -> DONE  ( input  )
    GPIO19 -> INIT  ( input  )
*/
static void GpioInit(void)
{
    configure_gpio(GPIO_M0,    true);
    write_gpio(GPIO_M0, 1);

    configure_gpio(GPIO_CCLK,  true);
    configure_gpio(GPIO_D0,    true);
    configure_gpio(GPIO_PROGB, true);
    configure_gpio(GPIO_DONE, false);
    configure_gpio(GPIO_INIT, false);

    // Initialise PROG_B output to 1, other outputs to 0.
    write_gpio(GPIO_PROGB, 1);
    write_gpio(GPIO_D0, 0);
    write_gpio(GPIO_CCLK, 0);
}

static void GpioClose(void)
{
    unconfigure_gpio(GPIO_CCLK);
    unconfigure_gpio(GPIO_D0);
    unconfigure_gpio(GPIO_PROGB);
    unconfigure_gpio(GPIO_DONE);
    unconfigure_gpio(GPIO_INIT);
    unconfigure_gpio(GPIO_M0);
}


// Generate a cclk signal
static void cclk(void)
{
    write_gpio(GPIO_CCLK, 1);
    write_gpio(GPIO_CCLK, 0);
}

// Set D0 output and strobe Configuration Clock (CCLK)
static bool WriteDataOut(bool  bit)
{
    if (!read_gpio(GPIO_DONE)  &&  !read_gpio(GPIO_INIT))
    {
        printf("DONE and INIT_B both low during programming, Config error!\n");
        return false;
    }
    else
    {
        write_gpio(GPIO_D0, bit);
        cclk();
        return true;
    }
}

// This function takes a 8-bit configuration byte, and
// serializes it, MSB first, LSB Last
static bool ShiftDataOut(unsigned char data)
{
    bool ok = true;
    for (int bit = 0; ok  &&  bit < 8; bit ++)
    {
        ok = WriteDataOut((data >> bit) & 1);
        if (!ok)
            printf("Error writing bit %d\n", bit);
    }
    return ok;
}

/* Performs the actual FPGA programming work.  Returns false on failure. */
static bool program_FPGA(void)
{
    /* STEP-1: De-assert PROG_B */
    usleep(1000);
    write_gpio(GPIO_PROGB, 0);
    usleep(1000);

    /* STEP-2: Wait for INIT to go LOW */
    if (read_gpio(GPIO_INIT))
    {
        fprintf(stderr, "INIT_B signal is not LOW.\n");
        return false;
    }

    /* STEP-3: Assert PROG_B */
    write_gpio(GPIO_PROGB, 1);

    /* STEP-4: Wait for INIT to go HIGH */
    int init_count = 0;
    while (!read_gpio(GPIO_INIT))
    {
        if (init_count++ > 10000)
        {
            fprintf(stderr, "INIT_B signal is not HIGH\n");
            return false;
        }
    }
    usleep(1000);

    /* STEP-5: Read firmware binary file */
    printf("Programming FPGA...\n");
    int len;
    unsigned char StreamData[4096];
    int block = 0;
    while (
        len = read(STDIN_FILENO, StreamData, sizeof(StreamData)),
        len > 0)
    {
        printf(".");  fflush(stdout);
        block += 1;
        for (int j=0; j < len; j++)
        {
            if (!ShiftDataOut(StreamData[j]))
            {
                printf("Error at offset %d, block %d\n", j, block);
                return false;
            }
        }
    }
    printf("\n");

    /* STEP-6: Wait for DONE to be asserted */
    printf("Waiting for DONE to go HIGH...\n");
    int done_count = 0;
    while (!read_gpio(GPIO_DONE))
    {
        if (done_count++ > 1000)
        {
            fprintf(stderr, "DONE signal is not HIGH\n");
            return false;
        }
        cclk();
    }

    /* STEP-7 : Apply 8 additioanl CCLKs after DONE asserted to ensure
     * completion of FPGA start-up sequence.  */
    printf("Programming complete...\n");
    return true;
}

int main(int argc, char **argv)
{
    printf("Initialising GPIOs...\n");
    GpioInit();

    bool ok = program_FPGA();

    GpioClose();

    return ok ? 0 : 1;
}
