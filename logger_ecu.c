#define _GNU_SOURCE

#include <errno.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define CAN_INTERFACE "vcan0"
#define LOG_FILE "can_log.csv"
#define MAX_FRAME_BYTES 72U

static volatile sig_atomic_t logging_enabled = 1;

static void stop_logger(int signal_number)
{
    (void)signal_number;
    logging_enabled = 0;
}

static int bind_socketcan(const char *interface_name)
{
    struct ifreq interface_request;
    struct sockaddr_can can_address;
    int socket_fd;
    int enable_fd = 1;

    socket_fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (socket_fd < 0) {
        perror("socket(PF_CAN)");
        return -1;
    }

    if (setsockopt(socket_fd, SOL_CAN_RAW, CAN_RAW_FD_FRAMES, &enable_fd, sizeof(enable_fd)) < 0) {
        perror("setsockopt(CAN_RAW_FD_FRAMES)");
        close(socket_fd);
        return -1;
    }

    memset(&interface_request, 0, sizeof(interface_request));
    snprintf(interface_request.ifr_name, IFNAMSIZ, "%s", interface_name);

    if (ioctl(socket_fd, SIOCGIFINDEX, &interface_request) < 0) {
        perror("ioctl(SIOCGIFINDEX)");
        close(socket_fd);
        return -1;
    }

    memset(&can_address, 0, sizeof(can_address));
    can_address.can_family = AF_CAN;
    can_address.can_ifindex = interface_request.ifr_ifindex;

    if (bind(socket_fd, (struct sockaddr *)&can_address, sizeof(can_address)) < 0) {
        perror("bind(AF_CAN)");
        close(socket_fd);
        return -1;
    }

    return socket_fd;
}

static void write_timestamp(char *buffer, size_t buffer_length)
{
    struct timespec current_time;
    struct tm local_time;

    if (clock_gettime(CLOCK_REALTIME, &current_time) != 0) {
        snprintf(buffer, buffer_length, "1970-01-01T00:00:00.000000000Z");
        return;
    }

    if (gmtime_r(&current_time.tv_sec, &local_time) == NULL) {
        snprintf(buffer, buffer_length, "1970-01-01T00:00:00.000000000Z");
        return;
    }

    snprintf(buffer, buffer_length, "%04d-%02d-%02dT%02d:%02d:%02d.%09ldZ",
             local_time.tm_year + 1900,
             local_time.tm_mon + 1,
             local_time.tm_mday,
             local_time.tm_hour,
             local_time.tm_min,
             local_time.tm_sec,
             current_time.tv_nsec);
}

static void format_payload(const unsigned char *payload, unsigned int length,
                           char *buffer, size_t buffer_length)
{
    size_t used = 0U;
    unsigned int index;

    if (buffer_length == 0U) {
        return;
    }

    buffer[0] = '\0';

    for (index = 0U; index < length; ++index) {
        int written;

        if (used >= buffer_length) {
            break;
        }

        written = snprintf(buffer + used, buffer_length - used, "%02X%s",
                           payload[index], (index + 1U < length) ? " " : "");
        if (written < 0) {
            buffer[used] = '\0';
            return;
        }
        used += (size_t)written;
    }
}

int main(void)
{
    int socket_fd;
    FILE *log_stream;
    unsigned char receive_buffer[MAX_FRAME_BYTES];
    unsigned long record_number = 0UL;
    char timestamp[64];
    char payload_text[3U * CANFD_MAX_DLEN + 1U];

    signal(SIGINT, stop_logger);
    signal(SIGTERM, stop_logger);

    log_stream = fopen(LOG_FILE, "a");
    if (log_stream == NULL) {
        perror("fopen(can_log.csv)");
        return EXIT_FAILURE;
    }

    if (ftell(log_stream) == 0L) {
        fprintf(log_stream, "record,timestamp_utc,can_id,frame_type,dlc,payload_hex\n");
        fflush(log_stream);
    }

    socket_fd = bind_socketcan(CAN_INTERFACE);
    if (socket_fd < 0) {
        fclose(log_stream);
        return EXIT_FAILURE;
    }

    printf("Logger ECU listening on %s; writing to %s.\n", CAN_INTERFACE, LOG_FILE);

    while (logging_enabled) {
        ssize_t received_bytes = read(socket_fd, receive_buffer, sizeof(receive_buffer));

        if (received_bytes < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("read(CAN/CAN-FD)");
            break;
        }

        if ((size_t)received_bytes == sizeof(struct can_frame)) {
            const struct can_frame *classic_frame = (const struct can_frame *)receive_buffer;
            canid_t can_identifier = classic_frame->can_id & CAN_EFF_MASK;

            write_timestamp(timestamp, sizeof(timestamp));
            format_payload(classic_frame->data, classic_frame->len, payload_text, sizeof(payload_text));
            record_number++;
            fprintf(log_stream, "%lu,%s,0x%03X,CLASSIC,%u,\"%s\"\n",
                    record_number, timestamp, can_identifier, classic_frame->len, payload_text);
            fflush(log_stream);
        } else if ((size_t)received_bytes == sizeof(struct canfd_frame)) {
            const struct canfd_frame *fd_frame = (const struct canfd_frame *)receive_buffer;
            canid_t can_identifier = fd_frame->can_id & CAN_EFF_MASK;

            write_timestamp(timestamp, sizeof(timestamp));
            format_payload(fd_frame->data, fd_frame->len, payload_text, sizeof(payload_text));
            record_number++;
            fprintf(log_stream, "%lu,%s,0x%03X,CAN_FD,%u,\"%s\"\n",
                    record_number, timestamp, can_identifier, fd_frame->len, payload_text);
            fflush(log_stream);
        } else {
            fprintf(stderr, "Discarded unsupported frame length: %zd bytes\n", received_bytes);
        }
    }

    close(socket_fd);
    fclose(log_stream);
    printf("Logger ECU stopped after %lu records.\n", record_number);
    return EXIT_SUCCESS;
}
