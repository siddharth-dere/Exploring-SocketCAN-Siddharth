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
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define CAN_INTERFACE "vcan0"
#define CAN_ID_SPEED 0x100U
#define CAN_ID_RPM 0x101U
#define CAN_ID_COOLANT 0x102U
#define SIGNAL_COUNT 3U
#define ECU_TIMEOUT_SECONDS 2.0

struct telemetry_state {
    double speed_kmh;
    double rpm;
    double coolant_c;
    double last_update[ SIGNAL_COUNT ];
    unsigned long message_count;
};

static volatile sig_atomic_t keep_running = 1;

static void request_shutdown(int signal_number)
{
    (void)signal_number;
    keep_running = 0;
}

static int create_filtered_socket(const char *interface_name)
{
    struct ifreq interface_request;
    struct sockaddr_can can_address;
    struct can_filter filters[3];
    int socket_fd;

    socket_fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (socket_fd < 0) {
        perror("socket(PF_CAN)");
        return -1;
    }

    filters[0].can_id = CAN_ID_SPEED;
    filters[0].can_mask = CAN_SFF_MASK;
    filters[1].can_id = CAN_ID_RPM;
    filters[1].can_mask = CAN_SFF_MASK;
    filters[2].can_id = CAN_ID_COOLANT;
    filters[2].can_mask = CAN_SFF_MASK;

    if (setsockopt(socket_fd, SOL_CAN_RAW, CAN_RAW_FILTER, filters, sizeof(filters)) < 0) {
        perror("setsockopt(CAN_RAW_FILTER)");
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

static uint16_t load_le_u16(const unsigned char *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8U);
}

static void render_dashboard(const struct telemetry_state *state, double now)
{
    int speed_online = ((now - state->last_update[0]) <= ECU_TIMEOUT_SECONDS);
    int rpm_online = ((now - state->last_update[1]) <= ECU_TIMEOUT_SECONDS);
    int coolant_online = ((now - state->last_update[2]) <= ECU_TIMEOUT_SECONDS);

    printf("\033[2J\033[H");
    printf("==============================================\n");
    printf("        SOCKETCAN VEHICLE DASHBOARD           \n");
    printf("==============================================\n");
    printf("Bus Interface : %s\n", CAN_INTERFACE);
    printf("Frames Seen   : %lu\n", state->message_count);
    printf("----------------------------------------------\n");
    printf("Speed         : %7.1f km/h   [%s]\n", state->speed_kmh, speed_online ? "ONLINE " : "TIMEOUT");
    printf("Engine RPM    : %7.0f rpm    [%s]\n", state->rpm, rpm_online ? "ONLINE " : "TIMEOUT");
    printf("Coolant Temp  : %7.1f C      [%s]\n", state->coolant_c, coolant_online ? "ONLINE " : "TIMEOUT");
    printf("----------------------------------------------\n");
    printf("Diagnostic    : %s\n", (speed_online && rpm_online && coolant_online) ? "ALL SIGNALS HEALTHY" : "ECU / SIGNAL LOSS DETECTED");
    printf("Press Ctrl+C to stop.\n");
    fflush(stdout);
}

int main(void)
{
    struct telemetry_state state;
    int socket_fd;
    struct timespec clock_value;
    double current_time;

    memset(&state, 0, sizeof(state));
    state.last_update[0] = 0.0;
    state.last_update[1] = 0.0;
    state.last_update[2] = 0.0;

    signal(SIGINT, request_shutdown);
    signal(SIGTERM, request_shutdown);

    socket_fd = create_filtered_socket(CAN_INTERFACE);
    if (socket_fd < 0) {
        return EXIT_FAILURE;
    }

    printf("Dashboard ECU listening on %s.\n", CAN_INTERFACE);

    while (keep_running) {
        fd_set read_set;
        struct timeval wait_time;
        struct can_frame received_frame;
        ssize_t bytes_read;

        FD_ZERO(&read_set);
        FD_SET(socket_fd, &read_set);
        wait_time.tv_sec = 0;
        wait_time.tv_usec = 200000;

        if (select(socket_fd + 1, &read_set, NULL, NULL, &wait_time) < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("select");
            break;
        }

        if (FD_ISSET(socket_fd, &read_set)) {
            memset(&received_frame, 0, sizeof(received_frame));
            bytes_read = read(socket_fd, &received_frame, sizeof(received_frame));
            if (bytes_read < 0) {
                if (errno == EINTR || errno == EAGAIN) {
                    continue;
                }
                perror("read(CAN frame)");
                break;
            }

            if (bytes_read == (ssize_t)sizeof(struct can_frame)) {
                const struct can_frame *received = &received_frame;

                if (clock_gettime(CLOCK_MONOTONIC, &clock_value) != 0) {
                    perror("clock_gettime");
                    break;
                }
                current_time = (double)clock_value.tv_sec + (double)clock_value.tv_nsec / 1000000000.0;
                state.message_count++;

                switch (received->can_id & CAN_SFF_MASK) {
                    case CAN_ID_SPEED:
                        if (received->len >= 2U) {
                            state.speed_kmh = load_le_u16(received->data) / 10.0;
                            state.last_update[0] = current_time;
                        }
                        break;
                    case CAN_ID_RPM:
                        if (received->len >= 2U) {
                            state.rpm = (double)load_le_u16(received->data);
                            state.last_update[1] = current_time;
                        }
                        break;
                    case CAN_ID_COOLANT:
                        if (received->len >= 2U) {
                            int16_t raw_temp = (int16_t)load_le_u16(received->data);
                            state.coolant_c = raw_temp / 10.0;
                            state.last_update[2] = current_time;
                        }
                        break;
                    default:
                        break;
                }
            } else {
                fprintf(stderr, "Unexpected CAN frame size: %zd bytes\n", bytes_read);
            }
        }

        if (clock_gettime(CLOCK_MONOTONIC, &clock_value) != 0) {
            perror("clock_gettime");
            break;
        }
        current_time = (double)clock_value.tv_sec + (double)clock_value.tv_nsec / 1000000000.0;
        render_dashboard(&state, current_time);
    }

    close(socket_fd);
    printf("Dashboard ECU stopped.\n");
    return EXIT_SUCCESS;
}
