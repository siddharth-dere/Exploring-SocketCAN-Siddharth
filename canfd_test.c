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
#define CANFD_TEST_ID 0x300U
#define CANFD_PAYLOAD_SIZE 64U
#define TRANSMIT_INTERVAL_MS 1000U

static volatile sig_atomic_t sender_active = 1;

static void stop_sender(int signal_number)
{
    (void)signal_number;
    sender_active = 0;
}

static int setup_fd_socket(const char *interface_name)
{
    struct ifreq interface_request;
    struct sockaddr_can can_address;
    int socket_fd;
    int fd_frames_enabled = 1;

    socket_fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (socket_fd < 0) {
        perror("socket(PF_CAN)");
        return -1;
    }

    if (setsockopt(socket_fd, SOL_CAN_RAW, CAN_RAW_FD_FRAMES,
                   &fd_frames_enabled, sizeof(fd_frames_enabled)) < 0) {
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

static unsigned char crc8_like_mix(unsigned int seed)
{
    unsigned int value = seed ^ 0xA7U;
    unsigned int bit;

    for (bit = 0U; bit < 8U; ++bit) {
        value = ((value << 1U) ^ ((value & 0x80U) ? 0x1DU : 0U)) & 0xFFU;
    }

    return (unsigned char)value;
}

int main(void)
{
    int socket_fd;
    unsigned int sequence = 0U;
    struct timespec pause_time;

    signal(SIGINT, stop_sender);
    signal(SIGTERM, stop_sender);

    socket_fd = setup_fd_socket(CAN_INTERFACE);
    if (socket_fd < 0) {
        return EXIT_FAILURE;
    }

    pause_time.tv_sec = 0;
    pause_time.tv_nsec = TRANSMIT_INTERVAL_MS * 1000000L;

    printf("CAN FD test transmitter active on %s using ID 0x%03X.\n",
           CAN_INTERFACE, CANFD_TEST_ID);

    while (sender_active) {
        struct canfd_frame frame;
        ssize_t written_bytes;
        unsigned int index;

        memset(&frame, 0, sizeof(frame));
        frame.can_id = CANFD_TEST_ID;
        frame.len = CANFD_PAYLOAD_SIZE;
        frame.flags = 0U;

        for (index = 0U; index < CANFD_PAYLOAD_SIZE; ++index) {
            frame.data[index] = (unsigned char)((index + sequence) & 0xFFU);
        }

        frame.data[0] = 0xC0U;
        frame.data[1] = 0xDEU;
        frame.data[2] = (unsigned char)(sequence & 0xFFU);
        frame.data[3] = (unsigned char)((sequence >> 8U) & 0xFFU);
        frame.data[63] = crc8_like_mix(sequence);

        written_bytes = write(socket_fd, &frame, sizeof(frame));
        if (written_bytes != (ssize_t)sizeof(frame)) {
            if (written_bytes < 0) {
                perror("write(CAN-FD frame)");
            } else {
                fprintf(stderr, "Partial CAN-FD write: %zd bytes\n", written_bytes);
            }
            break;
        }

        printf("CAN FD frame %u sent: ID=0x%03X DLC=%u payload=64 bytes\n",
               sequence, CANFD_TEST_ID, frame.len);
        fflush(stdout);
        sequence++;

        if (clock_nanosleep(CLOCK_MONOTONIC, 0, &pause_time, NULL) != 0 && errno != EINTR) {
            perror("clock_nanosleep");
            break;
        }
    }

    close(socket_fd);
    printf("CAN FD test transmitter stopped.\n");
    return EXIT_SUCCESS;
}
