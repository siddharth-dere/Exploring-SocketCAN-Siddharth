#define _GNU_SOURCE

#include <errno.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <math.h>
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
#define SPEED_ID 0x100U
#define RPM_ID 0x101U
#define COOLANT_ID 0x102U
#define TEST_ID 0x200U

#define SPEED_PERIOD_MS 100U
#define RPM_PERIOD_MS 100U
#define COOLANT_PERIOD_MS 250U
#define TEST_PERIOD_MS 5000U

static volatile sig_atomic_t g_running = 1;

static void stop_program(int signal_number)
{
    (void)signal_number;
    g_running = 0;
}

static uint16_t encode_u16_le(uint16_t value, unsigned char *payload)
{
    payload[0] = (unsigned char)(value & 0xFFU);
    payload[1] = (unsigned char)((value >> 8U) & 0xFFU);
    return value;
}

static void encode_i16_le(int16_t value, unsigned char *payload)
{
    uint16_t raw_value = (uint16_t)value;
    payload[0] = (unsigned char)(raw_value & 0xFFU);
    payload[1] = (unsigned char)((raw_value >> 8U) & 0xFFU);
}

static int open_can_socket(const char *interface_name)
{
    struct ifreq interface_request;
    struct sockaddr_can can_address;
    int socket_fd;

    socket_fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (socket_fd < 0) {
        perror("socket(PF_CAN)");
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

static int transmit_classic_frame(int socket_fd, canid_t can_id,
                                  const unsigned char *payload, unsigned char payload_length)
{
    struct can_frame frame;
    ssize_t bytes_written;

    if (payload_length > CAN_MAX_DLEN) {
        fprintf(stderr, "Payload length %u exceeds classic CAN capacity\n", payload_length);
        return -1;
    }

    memset(&frame, 0, sizeof(frame));
    frame.can_id = can_id;
    frame.len = payload_length;
    memcpy(frame.data, payload, payload_length);

    bytes_written = write(socket_fd, &frame, sizeof(frame));
    if (bytes_written != (ssize_t)sizeof(frame)) {
        if (bytes_written < 0) {
            perror("write(CAN frame)");
        } else {
            fprintf(stderr, "Partial CAN frame write: %zd bytes\n", bytes_written);
        }
        return -1;
    }

    return 0;
}

static double monotonic_seconds(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        perror("clock_gettime");
        return 0.0;
    }

    return (double)now.tv_sec + ((double)now.tv_nsec / 1000000000.0);
}

static double clamp_double(double value, double lower, double upper)
{
    if (value < lower) {
        return lower;
    }
    if (value > upper) {
        return upper;
    }
    return value;
}

int main(void)
{
    int socket_fd;
    struct timespec sleep_interval;
    double last_speed_time = 0.0;
    double last_rpm_time = 0.0;
    double last_coolant_time = 0.0;
    double last_test_time = 0.0;
    double simulation_time = 0.0;
    unsigned int test_counter = 0U;

    signal(SIGINT, stop_program);
    signal(SIGTERM, stop_program);

    socket_fd = open_can_socket(CAN_INTERFACE);
    if (socket_fd < 0) {
        return EXIT_FAILURE;
    }

    sleep_interval.tv_sec = 0;
    sleep_interval.tv_nsec = 10000000L;

    printf("Vehicle ECU started on %s. Press Ctrl+C to stop.\n", CAN_INTERFACE);

    while (g_running) {
        double current_time = monotonic_seconds();
        double speed_time = fmod(simulation_time, 24.0);
        double road_speed_kmh = 65.0 + 25.0 * sin((2.0 * M_PI * speed_time) / 24.0);
        double engine_load = clamp_double(0.25 + 0.006 * road_speed_kmh + 0.05 * sin(speed_time), 0.20, 0.92);
        double engine_rpm = 850.0 + (road_speed_kmh * 34.0) + (engine_load * 1150.0);
        double coolant_c = 82.0 + (engine_load * 13.0) + (2.5 * sin(speed_time / 3.0));

        road_speed_kmh = clamp_double(road_speed_kmh, 0.0, 180.0);
        engine_rpm = clamp_double(engine_rpm, 700.0, 6500.0);
        coolant_c = clamp_double(coolant_c, -40.0, 140.0);

        if ((current_time - last_speed_time) >= (SPEED_PERIOD_MS / 1000.0)) {
            unsigned char payload[8] = {0};
            uint16_t encoded_speed = (uint16_t)lround(road_speed_kmh * 10.0);

            encode_u16_le(encoded_speed, payload);
            if (transmit_classic_frame(socket_fd, SPEED_ID, payload, 2U) < 0) {
                g_running = 0;
                continue;
            }
            last_speed_time = current_time;
        }

        if ((current_time - last_rpm_time) >= (RPM_PERIOD_MS / 1000.0)) {
            unsigned char payload[8] = {0};
            uint16_t encoded_rpm = (uint16_t)lround(engine_rpm);

            encode_u16_le(encoded_rpm, payload);
            if (transmit_classic_frame(socket_fd, RPM_ID, payload, 2U) < 0) {
                g_running = 0;
                continue;
            }
            last_rpm_time = current_time;
        }

        if ((current_time - last_coolant_time) >= (COOLANT_PERIOD_MS / 1000.0)) {
            unsigned char payload[8] = {0};
            int16_t encoded_coolant = (int16_t)lround(coolant_c * 10.0);

            encode_i16_le(encoded_coolant, payload);
            if (transmit_classic_frame(socket_fd, COOLANT_ID, payload, 2U) < 0) {
                g_running = 0;
                continue;
            }
            last_coolant_time = current_time;
        }

        if ((current_time - last_test_time) >= (TEST_PERIOD_MS / 1000.0)) {
            unsigned char payload[8] = {0};

            payload[0] = 0xA5U;
            payload[1] = (unsigned char)(test_counter & 0xFFU);
            payload[2] = (unsigned char)((test_counter >> 8U) & 0xFFU);
            payload[3] = 0x5AU;
            payload[4] = (unsigned char)(lround(engine_load * 100.0));
            payload[5] = 0xC3U;
            payload[6] = 0x3CU;
            payload[7] = 0x7EU;

            if (transmit_classic_frame(socket_fd, TEST_ID, payload, 8U) < 0) {
                g_running = 0;
                continue;
            }
            test_counter++;
            last_test_time = current_time;
        }

        simulation_time += 0.01;
        if (clock_nanosleep(CLOCK_MONOTONIC, 0, &sleep_interval, NULL) != 0 && errno != EINTR) {
            perror("clock_nanosleep");
            g_running = 0;
        }
    }

    close(socket_fd);
    printf("Vehicle ECU stopped.\n");
    return EXIT_SUCCESS;
}
