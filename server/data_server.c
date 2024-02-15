/* Socket server for data streaming interface. */

#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <pthread.h>
#include <time.h>

#include "error.h"
#include "hardware.h"
#include "buffered_file.h"
#include "config_server.h"
#include "buffer.h"
#include "prepare.h"
#include "locking.h"
#include "base64.h"
#include "pos_out.h"
#include "ext_out.h"
#include "capture.h"

#include "data_server.h"


/* Central circular data buffer.  The block size is chosen to match the driver
 * block size, and we choose a reasonably large number of 2MB blocks. */
#define DATA_BLOCK_SIZE         (1U << 21)
#define DATA_BLOCK_COUNT        128

/* File buffers.  These are only used for text buffered text communication on
 * the data channel. */
#define IN_BUF_SIZE             4096
#define OUT_BUF_SIZE            16384
/* Proper network buffer.  All data communication uses this buffer size. */
#define NET_BUF_SIZE            65536

/* Should be large enough for the largest single raw sample. */
#define MAX_RAW_SAMPLE_LENGTH   256

/* Length of one base64 line. */
#define BASE64_CONVERT_COUNT    57U


/* Connection and read block polling intervals.  These determine how long it
 * takes for a socket disconnect to be detected. */
#define CONNECTION_POLL_SECS    0
#define CONNECTION_POLL_NSECS   ((unsigned long) (0.2 * NSECS))  // 200 ms

#define READ_BLOCK_POLL_SECS    0
#define READ_BLOCK_POLL_NSECS   ((unsigned long) (0.2 * NSECS))  // 200 ms

/* Allow this many data blocks between the reader and the writer on startup. */
#define BUFFER_READ_MARGIN      (DATA_BLOCK_COUNT / 4)


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Data retrieval from hardware. */

static pthread_mutex_t data_thread_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t data_thread_event;

/* Thread id for shutdown synchronisation.  Alas, we need a separate thread to
 * record whether this id is valid. */
static pthread_t data_thread_id;
static bool data_thread_started = false;


/* The data capture thread drives the two core state machines associated with
 * data capture: hardware and the data capture buffer.
 *
 *  Hardware data capture is either Idle or Busy.  The transition to Busy is in
 *  response to *PCAP.ARM= while the transition back to Idle is notified to the
 *  capture thread.  Thus the data_capture_enabled flag effectively encapsulates
 *  the state of the hardware as far as software is concerned.
 *
 *  The data capture buffer is also either Idle or Busy, and this state is also
 *  directly controlled by the data capture thread, but in the Idle state we
 *  also have to check the number of connected busy clients: if a client is busy
 *  we cannot start a new capture.
 *
 * The data_capture_enabled flag is used to distinguish these two states.  The
 * data_thread_running flag is used to trigger an orderly shutdown of the thread
 * on server shutdown. */
static bool data_capture_enabled = false;
static volatile bool data_thread_running = true;

/* Data capture buffer. */
static struct capture_buffer *data_buffer;
/* Structures used to define data capture in progress.  This are valid while
 * data capture is enabled, invalid otherwise. */
static const struct captured_fields *captured_fields;
static const struct data_capture *data_capture;
/* PCAP ARM timestamp */
static struct timespec pcap_arm_ts;
/* PCAP becomes armed & enabled timestamp */
static struct timespec pcap_start_ts;
static bool pcap_hw_ts_offset_ns_valid;
static int64_t pcap_hw_ts_offset_ns;

/* Data completion code at end of experiment. */
static unsigned int completion_code;
/* Sample count at end of experiment. */
static uint64_t experiment_sample_count;


/* Save start timestamp, either from hardware (if that timestamp is not 0), or
 * from the driver (which was saved in the interrupt handler triggered by the
 * start event) */
static void update_start_timestamp(void)
{
    struct timespec pcap_drv_start_ts, pcap_hw_start_ts;
    hw_get_start_ts(&pcap_drv_start_ts);
    hw_get_hw_start_ts(&pcap_hw_start_ts);
    if (pcap_hw_start_ts.tv_sec == 0 && pcap_hw_start_ts.tv_nsec == 0)
    {
        pcap_start_ts = pcap_drv_start_ts;
        pcap_hw_ts_offset_ns_valid = false;
    }
    else
    {
        int64_t drv_ts_num = (int64_t) pcap_drv_start_ts.tv_sec * NSECS
            + pcap_drv_start_ts.tv_nsec;
        int64_t hw_ts_num = (int64_t) pcap_hw_start_ts.tv_sec * NSECS
            + pcap_hw_start_ts.tv_nsec;
        pcap_start_ts = pcap_hw_start_ts;
        pcap_hw_ts_offset_ns = drv_ts_num - hw_ts_num;
        pcap_hw_ts_offset_ns_valid = true;
    }
}


/* Performs a complete experiment capture: start data buffer, process the data
 * stream until hardware is complete, stop data buffer. */
static void capture_experiment(void)
{
    start_write(data_buffer);
    size_t sample_length = get_raw_sample_length(data_capture);
    log_message("Starting capture: %zu bytes/sample", sample_length);

    uint64_t total_bytes = 0;
    experiment_sample_count = 0;
    completion_code = 0;

    bool at_eof = false;
    bool ts_captured = false;
    while (data_thread_running  &&  !at_eof)
    {
        void *block = get_write_block(data_buffer);
        size_t count;
        do
            count = hw_read_streamed_data(block, DATA_BLOCK_SIZE, &at_eof);
        while (data_thread_running  &&  count == 0  &&  !at_eof);

        /* Do our best to capture a timestamp before releasing the first block
         * of the experiment.  If the experiment is empty this return an empty
         * timestamp, that's how it goes. */
        if (!ts_captured)
        {
            update_start_timestamp();
            ts_captured = true;
        }

        /* Unconditionally release the write block here.  Either we have data to
         * send (exited loop above with count==0), or we're at the end of the
         * experiment, in which case an empty block is harmless. */
        release_write_block(data_buffer, count);

        total_bytes += count;
        experiment_sample_count = total_bytes / sample_length;
    }

    completion_code = hw_read_streamed_completion();

    end_write(data_buffer);
    log_message("Captured %"PRIu64" samples: %s",
        experiment_sample_count, hw_decode_completion(completion_code));
}


/* Data thread: the responsive half of the data capture state machine.  Captures
 * hardware data to internal buffer in response to triggered experiments. */
static void *data_thread(void *context)
{
    while (data_thread_running)
    {
        /* Wait for data capture to start. */
        WITH_MUTEX(data_thread_mutex)
            while (data_thread_running  &&  !data_capture_enabled)
                WAIT(data_thread_mutex, data_thread_event);

        if (data_thread_running)
            capture_experiment();

        WITH_MUTEX(data_thread_mutex)
            data_capture_enabled = false;
    }
    return NULL;
}


/* Forces the data capture thread to exit in an orderly way. */
static void stop_data_thread(void)
{
    WITH_MUTEX(data_thread_mutex)
    {
        data_thread_running = false;
        SIGNAL(data_thread_event);
    }
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* User interface and control. */


static error__t start_data_capture(void)
{
    captured_fields = prepare_captured_fields();
    error__t error = prepare_data_capture(captured_fields, &data_capture);
    if (!error)
    {
        hw_write_arm_streamed_data();
        hw_write_arm(true);
        data_capture_enabled = true;
    }
    SIGNAL(data_thread_event);
    return error;
}


error__t arm_capture(void)
{
    unsigned int readers, active;

    return
        TEST_OK_(check_pcap_valid(),
            "PCAP not supported with this configuration")  ?:
        ERROR_WITH_MUTEX(data_thread_mutex,
            TEST_OK_(!data_capture_enabled,
                "Data capture already in progress")  ?:
            /* If data capture is not enabled then we can safely expect the
             * buffer status to be idle. */
            TEST_OK(!read_buffer_status(data_buffer, &readers, &active))  ?:
            TEST_OK_(active == 0, "Data clients still taking data")  ?:
            /* Get PCAP ARM timestamp and store as a global */
            TEST_IO(clock_gettime(CLOCK_REALTIME, &pcap_arm_ts))  ?:
            start_data_capture());
}


error__t disarm_capture(void)
{
    hw_write_arm(false);
    return ERROR_OK;
}


error__t get_capture_status(struct connection_result *result)
{
    unsigned int readers, active;
    bool status = read_buffer_status(data_buffer, &readers, &active);
    return format_one_result(
        result, "%s %u %u", status ? "Busy" : "Idle", readers, active);
}


error__t get_capture_count(struct connection_result *result)
{
    return format_one_result(result, "%"PRIu64, experiment_sample_count);
}


error__t get_capture_completion(struct connection_result *result)
{
    return ERROR_WITH_MUTEX(data_thread_mutex,
        format_one_result(result, "%s", data_capture_enabled ? "Busy" :
            hw_decode_completion(completion_code)));
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Data delivery to client. */


struct data_connection {
    int scon;
    struct buffered_file *file;
    struct reader_state *reader;
    struct data_options options;
};


/* Every data request must start with a newline terminated format request. */
static bool process_data_request(struct data_connection *connection)
{
    char line[MAX_LINE_LENGTH];
    if (read_line(connection->file, line, sizeof(line), false))
    {
        error__t error = parse_data_options(line, &connection->options);
        if (!error  &&  connection->options.omit_status)
            return true;
        else
        {
            int length = error ?
                snprintf(line, sizeof(line), "ERR %s\n", error_format(error))
            :
                snprintf(line, sizeof(line), "OK\n");
            error_discard(error);
            write_string(connection->file, line, (size_t) length);
            return flush_out_buf(connection->file)  &&  !error;
        }
    }
    else
        return false;
}


/* Checks the connection by attempting a peek on the socket.  Also silently
 * consume any extra data the client sends us at this point. */
static bool check_connection(struct data_connection *connection)
{
    char buffer[4096];
    ssize_t rx = recv(connection->scon, buffer, sizeof(buffer), MSG_DONTWAIT);
    return (rx == -1  &&  errno == EAGAIN)  ||  rx > 0;
}


/* Block until capture begins or the socket is closed. */
static bool wait_for_capture(
    struct data_connection *connection,
    uint64_t *lost_samples, size_t *skip_bytes)
{
    /* Block here waiting for data capture to begin or for the client to
     * disconnect.  Alas, detecting disconnection is a bit of a pain: we either
     * have to poll or use file handles (eventfd(2)) for wakeup, and using a
     * file handle is a pain because we'd need one per client.  So we poll. */
    const struct timespec timeout = {
        .tv_sec  = CONNECTION_POLL_SECS,
        .tv_nsec = CONNECTION_POLL_NSECS, };
    bool opened = false;
    uint64_t lost_bytes;
    while (check_connection(connection)  &&  !opened)
        opened = open_reader(
            connection->reader, BUFFER_READ_MARGIN, &timeout, &lost_bytes);

    if (opened)
    {
        /* Convert lost bytes into lost samples and byte count we need to skip
         * to get back into line. */
        size_t sample_size = get_raw_sample_length(data_capture);
        *lost_samples = (lost_bytes + sample_size - 1) / sample_size;
        uint64_t extra_bytes = lost_bytes % sample_size;
        *skip_bytes = extra_bytes > 0 ? sample_size - (size_t) extra_bytes : 0;
    }
    return opened;
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Data processing and transmission. */


/* State needed for data stream processing. */
struct data_capture_state {
    /* Underlying connection with socket connection and selected options. */
    struct data_connection *connection;
    /* Computed raw and binary converted single sample sizes, needed for buffer
     * processing and preparation. */
    size_t raw_sample_length;
    size_t binary_sample_length;

    /* Number of bytes currently in sample buffer. */
    size_t sample_buffer_count;
    /* Number of bytes currently in output buffer. */
    size_t output_buffer_count;

    /* Buffer for storing a single raw sample. */
    char sample_buffer[MAX_RAW_SAMPLE_LENGTH];
    /* Binary processed data. */
    char output_buffer[NET_BUF_SIZE];
};


/* Process as many samples as will fit into the output buffer. */
static unsigned int process_samples(
    struct data_capture_state *state, const void **buffer, size_t *length)
{
    /* Work out how many samples are available and how many will fit into the
     * output buffer. */
    size_t free_space =
        sizeof(state->output_buffer) - state->output_buffer_count;
    unsigned int sample_count = (unsigned int) MIN(
        *length / state->raw_sample_length,
        free_space / state->binary_sample_length);

    convert_raw_data_to_binary(
        data_capture, &state->connection->options, sample_count,
        *buffer, state->output_buffer + state->output_buffer_count);
    state->output_buffer_count += sample_count * state->binary_sample_length;

    size_t consumed = sample_count * state->raw_sample_length;
    *buffer += consumed;
    *length -= consumed;
    return sample_count;
}


/* Update and handle the single sample buffer.  This is needed to cope with
 * alignment errors on the data stream. */
static unsigned int process_single_sample(
    struct data_capture_state *state, const void **buffer, size_t *length)
{
    if (state->sample_buffer_count > 0)
    {
        size_t to_copy = MIN(
            *length, state->raw_sample_length - state->sample_buffer_count);
        memcpy(
            state->sample_buffer + state->sample_buffer_count,
            *buffer, to_copy);
        *buffer += to_copy;
        *length -= to_copy;
        state->sample_buffer_count += to_copy;
        if (state->sample_buffer_count >= state->raw_sample_length)
        {
            const void *sample_buffer = state->sample_buffer;
            return process_samples(
                state, &sample_buffer, &state->sample_buffer_count);
        }
    }
    return 0;       // Buffer not processed
}


/* Adds any residual data onto the single sample buffer. */
static void update_single_sample_buffer(
    struct data_capture_state *state, const void *buffer, size_t length)
{
    memcpy(state->sample_buffer + state->sample_buffer_count, buffer, length);
    state->sample_buffer_count += length;
}


/* Reset the output buffer, and if appropriate add the appropriate binary
 * prefix. */
static void prepare_output_buffer(struct data_capture_state *state)
{
    if (state->connection->options.data_format == DATA_FORMAT_FRAMED)
        /* Allow room for framing header. */
        state->output_buffer_count = 8;
    else
        state->output_buffer_count = 0;
}


/* Transmits data block in base 64. */
static bool write_block_base64(
    struct buffered_file *file, const void *data, size_t data_length)
{
    /* Simply work through the data a line at a time. */
    bool ok = true;
    while (ok  &&  data_length > 0)
    {
        /* Space for encoded buffer, leading space, trailing newline, null. */
        char line[128];
        COMPILE_ASSERT(
            sizeof(line) > BASE64_ENCODE_LENGTH(BASE64_CONVERT_COUNT) + 2);
        size_t to_encode = MIN(data_length, BASE64_CONVERT_COUNT);
        line[0] = ' ';
        size_t encoded = base64_encode(data, to_encode, line + 1);
        line[encoded + 1] = '\n';
        ok = write_string(file, line, encoded + 2);

        data += to_encode;
        data_length -= to_encode;
    }
    return ok;
}


static void set_frame_header(void *buffer, size_t frame_length)
{
    ASSERT_OK(frame_length <= UINT32_MAX);

    /* Update the first four bytes of the buffer with a header followed by a
     * frame byte count. */
    memcpy(buffer, "BIN ", 4);
    CAST_FROM_TO(void *, uint32_t *, buffer)[1] = (uint32_t) frame_length;
}


/* Send the output buffer in the appropriate format. */
static bool send_output_buffer(
    struct data_capture_state *state, unsigned int samples)
{
    if (state->connection->options.data_format == DATA_FORMAT_FRAMED)
        set_frame_header(state->output_buffer, state->output_buffer_count);

    switch (state->connection->options.data_format)
    {
        case DATA_FORMAT_ASCII:
            return send_binary_as_ascii(
                data_capture, &state->connection->options,
                state->connection->file, samples, state->output_buffer);
        case DATA_FORMAT_BASE64:
            return write_block_base64(
                state->connection->file,
                state->output_buffer, state->output_buffer_count);
        default:
            return write_block(
                state->connection->file,
                state->output_buffer, state->output_buffer_count);
    }
}


/* Returns false if unable to send data, returns true otherwise.  *data_ok is
 * set to false and data processing is abandoned if buffer overrun is seen. */
static bool process_capture_block(
    struct data_capture_state *state, const void *buffer, size_t length,
    uint64_t *sent_samples, bool *data_ok)
{
    /* Ensure output buffer is ready for a fresh transmission. */
    prepare_output_buffer(state);

    /* Ensure the buffer is aligned to a multiple of samples. */
    unsigned int samples = process_single_sample(state, &buffer, &length);

    /* Now loop until we've consumed the input buffer. */
    do {
        /* Process as much of the input buffer as will fit into the output
         * buffer, and if there's anything to send, transmit it. */
        samples += process_samples(state, &buffer, &length);
        if (samples > 0)
        {
            /* Check for buffer overrun while preparing this block.  On failure
             * just bail out early, but don't report communication error. */
            *data_ok = check_read_block(state->connection->reader);
            if (!*data_ok)
                return true;

            /* Transmit the output buffer, if we can.  If this fails, just
             * bail out. */
            if (!send_output_buffer(state, samples))
                return false;

            /* In case there's more to come, reinitialise the output. */
            prepare_output_buffer(state);
            *sent_samples += samples;
            samples = 0;
        }
    } while (length >= state->raw_sample_length);

    /* Add any residue to the single sample buffer. */
    update_single_sample_buffer(state, buffer, length);
    return true;
}


/* If in RAW and FRAMED mode, we avoid extra memcpys by taking the input
 * buffer and writing blocks directly from it. We always send a integer
 * number of samples to the user, copying the residual to the output_buffer
 * to hold onto it until the next buffer. The contents of output_buffer
 * is an 8 byte header (which is filled in just before it is sent) plus
 * any residual from the last buffer.
 * Returns false if unable to send data, returns true otherwise.  *data_ok is
 * set to false and data processing is abandoned if buffer overrun is seen. */
static bool passthrough_capture_block(
    struct data_capture_state *state, const void *buffer, size_t length,
    uint64_t *sent_samples, bool *data_ok)
{
    /* The number of samples of residual + buffer we will send */
    size_t samples =
        (state->output_buffer_count + length) / state->raw_sample_length;
    /* The bytes of the buffer to send this time */
    size_t buffer_to_send =
        samples * state->raw_sample_length - state->output_buffer_count;
    /* The length of the residual including header */
    size_t residual = 8 + state->output_buffer_count;

    /* Put the frame length at the start of the residual output buffer */
    set_frame_header(state->output_buffer, residual + buffer_to_send);

    /* Send frame header and residual output buffer, then the current buffer */
    bool sent_ok =
        write_block(state->connection->file, state->output_buffer, residual) &&
        write_block(state->connection->file, buffer, buffer_to_send);
    /* Transmit the output buffer, if we can.  If this fails, just bail out. */
    if (!sent_ok)
        return false;

    /* Reset the buffer count to have space for "BIN <frame_length>" again
     * and add any residue to the output buffer */
    buffer += buffer_to_send;
    length -= buffer_to_send;
    memcpy(state->output_buffer + 8, buffer, length);
    state->output_buffer_count = length;

    *sent_samples += samples;
    /* Check here if the reader has just stamped on our data. If it did, then
     * we're too late to save the bad data we just sent, but signal that we
     * overrun so the client can discard it */
    *data_ok = check_read_block(state->connection->reader);
    return true;
}


/* Sends the data stream until end of stream or there's a problem with the
 * client connection.  Any client connection problem is stored in the
 * connection, so is not returned. */
static void send_data_stream(
    struct data_connection *connection, size_t skip_bytes,
    uint64_t *sent_samples)
{
    struct data_capture_state state = {
        .connection = connection,
        .raw_sample_length = get_raw_sample_length(data_capture),
        .binary_sample_length = get_binary_sample_length(
            data_capture, &connection->options),
    };

    const struct timespec timeout = {
        .tv_sec  = READ_BLOCK_POLL_SECS,
        .tv_nsec = READ_BLOCK_POLL_NSECS, };

    /* Read and process buffers. */
    bool ok = true;
    bool data_ok = true;
    bool passthrough =
        state.connection->options.data_format == DATA_FORMAT_FRAMED &&
        state.connection->options.data_process == DATA_PROCESS_RAW;
    while (ok  &&  data_ok)
    {
        size_t in_length;
        const void *buffer =
            get_read_block(connection->reader, &timeout, &in_length);
        if (buffer == NULL  ||  !check_connection(connection))
            break;

        if (unlikely(skip_bytes > 0))
        {
            size_t skipped = MIN(skip_bytes, in_length);
            skip_bytes -= skipped;
            in_length -= skipped;
            buffer += skipped;
        }

        if (in_length > 0)
        {
            if (passthrough)
                ok = passthrough_capture_block(
                    &state, buffer, in_length, sent_samples, &data_ok);
            else
                ok = process_capture_block(
                    &state, buffer, in_length, sent_samples, &data_ok);
        }

        /* We flush the output buffer at this stage in case we're running slowly
         * so that the client will see progress. */
        if (ok)
            ok = flush_out_buf(connection->file);
    }
}


static bool send_data_completion(
    struct data_connection *connection,
    uint64_t sent_samples, uint64_t lost_samples,
    enum reader_status status, unsigned int completion)
{
    /* This list of completion strings must match the definition of the
     * reader_status enumeration in buffer.h. */
    static const char *completions[] = {
        [READER_STATUS_CLOSED]   = "Early disconnect",
        [READER_STATUS_OVERRUN]  = "Data overrun",
    };
    const char *message = status ?
        completions[status] : hw_decode_completion(completion);

    if (!connection->options.omit_status)
        write_formatted_string(connection->file,
            "END %"PRIu64" %s\n", sent_samples, message);
    log_message("Sent %"PRIu64" (+%"PRIu64") %s",
        sent_samples, lost_samples, message);
    return flush_out_buf(connection->file);
}


/* This is the top level handler for a single data client connection.  The
 * connection must open with a format request, after which we will send data
 * capture results while the socket is connected. */
error__t process_data_socket(int scon)
{
    struct data_connection connection = {
        .scon = scon,
        .file = create_buffered_file(scon, IN_BUF_SIZE, OUT_BUF_SIZE),
    };

    if (process_data_request(&connection))
    {
        connection.reader = create_reader(data_buffer);
        uint64_t lost_samples;
        size_t skip_bytes;
        bool ok = true;
        while (ok  &&
            wait_for_capture(&connection, &lost_samples, &skip_bytes))
        {
            if (!connection.options.omit_header)
                ok = send_data_header(
                    captured_fields, data_capture,
                    &connection.options, connection.file, lost_samples,
                    &pcap_arm_ts, &pcap_start_ts, pcap_hw_ts_offset_ns_valid,
                    pcap_hw_ts_offset_ns);

            uint64_t sent_samples = 0;
            if (ok)
                send_data_stream(&connection, skip_bytes, &sent_samples);

            /* Ensure we always close the reader, even if sending the stream
             * failed.  Note that we pick up the completion code before closing
             * the reader, as in principle it can change immediately after the
             * last reader has closed. */
            unsigned int completion = completion_code;
            enum reader_status status = close_reader(connection.reader);
            ok = send_data_completion(
                &connection, sent_samples, lost_samples, status, completion);

            if (connection.options.one_shot)
                break;
        }
        destroy_reader(connection.reader);
    }

    return destroy_buffered_file(connection.file);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Initialisation and shutdown. */


error__t initialise_data_server(void)
{
    pwait_initialise(&data_thread_event);
    log_message("Allocate %dx %d blocks", DATA_BLOCK_COUNT, DATA_BLOCK_SIZE);
    data_buffer = create_buffer(DATA_BLOCK_SIZE, DATA_BLOCK_COUNT);
    return ERROR_OK;
}


error__t start_data_server(void)
{
    /* We want to run the data thread as a high priority real time thread so
     * that at least the data doesn't back up in the hardware.  Even if things
     * get starved further downstream, it'll be better to at least bring the
     * data into userspace.
     *
     * A good explanation of the process is here:
     *
     *  http://stackoverflow.com/questions/3649281/
     *      how-to-increase-thread-priority-in-pthreads
     *
     * Basically we have to call pthread_setschedparam to configure the thread
     * for real time scheduling, then we can set the priority.  The scheduling
     * options are described in sched_setscheduler(2).  Of course we can only do
     * this on the real target system. */
    struct sched_param sched_param = { .sched_priority = 20 };
    /* Pin this thread to CPU0, along with all the interrupts */
    cpu_set_t cpu_set;
    CPU_ZERO(&cpu_set);
    CPU_SET(0, &cpu_set);
    return
        TEST_PTHREAD(pthread_create(
            &data_thread_id, NULL, data_thread, NULL))  ?:
        DO(data_thread_started = true)  ?:
        IF(!sim_hardware(),
            TEST_PTHREAD(pthread_setschedparam(
                data_thread_id, SCHED_RR, &sched_param))  ?:
            TEST_PTHREAD(pthread_setaffinity_np(
                data_thread_id, sizeof(cpu_set_t), &cpu_set)));
}


void terminate_data_server_early(void)
{
    if (data_thread_started)
    {
        stop_data_thread();
        error_report(TEST_PTHREAD(pthread_join(data_thread_id, NULL)));
        shutdown_buffer(data_buffer);
    }
}


void terminate_data_server(void)
{
    /* Can't do this safely until all our clients have gone. */
    if (data_buffer)
        destroy_buffer(data_buffer);
}
