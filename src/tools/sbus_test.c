#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "serial/sbus.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#include <unistd.h>
#endif

static volatile sig_atomic_t g_stop_requested = 0;

static void handle_stop_signal(int signo) {
    (void)signo;
    g_stop_requested = 1;
}

static void sleep_one_millisecond(void) {
#ifdef _WIN32
    Sleep(1);
#else
    const struct timespec delay = {0, 1000000L};
    nanosleep(&delay, NULL);
#endif
}

static void build_test_frame(const uint16_t channels[SBUS_CHANNEL_COUNT],
                             uint8_t flags,
                             uint8_t frame[SBUS_FRAME_SIZE]) {
    size_t channel;

    memset(frame, 0, SBUS_FRAME_SIZE);
    frame[0] = 0x0f;
    for (channel = 0; channel < SBUS_CHANNEL_COUNT; ++channel) {
        const size_t bit_offset = channel * 11U;
        size_t bit;

        for (bit = 0; bit < 11U; ++bit) {
            if ((channels[channel] & (1U << bit)) != 0U) {
                const size_t payload_bit = bit_offset + bit;
                frame[1U + payload_bit / 8U] |=
                    (uint8_t)(1U << (payload_bit % 8U));
            }
        }
    }
    frame[23] = flags;
    frame[24] = 0x00;
}

static int expect_int(const char *name, int actual, int expected) {
    if (actual == expected) {
        return 0;
    }

    fprintf(stderr, "FAIL: %s actual=%d expected=%d\n", name, actual, expected);
    return 1;
}

static int run_parser_tests(void) {
    const uint16_t expected[SBUS_CHANNEL_COUNT] = {
        172, 992, 1811, 400, 600, 1100, 1700, 800,
        250, 500, 750, 1000, 1250, 1500, 1750, 2047
    };
    uint8_t frame_bytes[SBUS_FRAME_SIZE];
    uint8_t noisy_stream[3 + SBUS_FRAME_SIZE * 2];
    sbus_frame_t frame;
    sbus_controls_t controls;
    sbus_parser_t parser;
    size_t parsed_count;
    size_t channel;
    int failures = 0;

    build_test_frame(expected, 0x05, frame_bytes);
    failures += expect_int("parse status", sbus_parse_frame(frame_bytes, &frame), SBUS_OK);
    for (channel = 0; channel < SBUS_CHANNEL_COUNT; ++channel) {
        char name[32];
        snprintf(name, sizeof(name), "channel %zu", channel + 1U);
        failures += expect_int(name, frame.channels[channel], expected[channel]);
    }
    failures += expect_int("digital channel 17", frame.digital_channel_17, 1);
    failures += expect_int("digital channel 18", frame.digital_channel_18, 0);
    failures += expect_int("frame lost", frame.frame_lost, 1);
    failures += expect_int("failsafe", frame.failsafe, 0);

    sbus_channels_decode(&frame, &controls);
    failures += expect_int("joystick 1", controls.joystick[0], -1000);
    failures += expect_int("joystick 2", controls.joystick[1], 0);
    failures += expect_int("knob 1", controls.knob[0], 1000);
    failures += expect_int("knob 2", controls.knob[1], -722);
    failures += expect_int("button 5", controls.button[0], 0);
    failures += expect_int("button 6", controls.button[1], 1);
    failures += expect_int("button 7", controls.button[2], 1);
    failures += expect_int("button 8", controls.button[3], 0);

    sbus_parser_init(&parser);
    noisy_stream[0] = 0xaa;
    noisy_stream[1] = 0x55;
    noisy_stream[2] = 0x00;
    memcpy(noisy_stream + 3, frame_bytes, SBUS_FRAME_SIZE);
    memcpy(noisy_stream + 3 + SBUS_FRAME_SIZE, frame_bytes, SBUS_FRAME_SIZE);
    parsed_count = sbus_parser_feed(&parser, noisy_stream, 11, &frame);
    failures += expect_int("fragment 1 frame count", (int)parsed_count, 0);
    parsed_count = sbus_parser_feed(&parser,
                                    noisy_stream + 11,
                                    sizeof(noisy_stream) - 11,
                                    &frame);
    failures += expect_int("fragment 2 frame count", (int)parsed_count, 2);

#ifndef _WIN32
    {
        int pipe_fd[2];
        sbus_data_t data;
        sbus_frame_t sent_frame;
        uint8_t sent_bytes[SBUS_FRAME_SIZE];
        ssize_t sent_size;
        sbus_status_t send_status;

        if (pipe(pipe_fd) != 0) {
            fprintf(stderr, "FAIL: create SBUS send test pipe\n");
            failures++;
        } else {
            memset(&sent_frame, 0, sizeof(sent_frame));
            memcpy(sent_frame.channels, expected, sizeof(expected));
            sent_frame.digital_channel_17 = 1;
            sent_frame.frame_lost = 1;
            data.fd = pipe_fd[1];
            sbus_parser_init(&data.parser);

            send_status = sbus_data_send(&data, &sent_frame);
            failures += expect_int("data send status", send_status, SBUS_OK);
            sent_size = -1;
            if (send_status == SBUS_OK) {
                sent_size = read(pipe_fd[0], sent_bytes, sizeof(sent_bytes));
                failures += expect_int("data send size",
                                       (int)sent_size,
                                       SBUS_FRAME_SIZE);
            }
            if (send_status == SBUS_OK && sent_size == SBUS_FRAME_SIZE) {
                failures += expect_int("data send frame parse",
                                       sbus_parse_frame(sent_bytes, &frame),
                                       SBUS_OK);
                for (channel = 0; channel < SBUS_CHANNEL_COUNT; ++channel) {
                    failures += expect_int("data send channel",
                                           frame.channels[channel],
                                           expected[channel]);
                }
                failures += expect_int("data send channel 17",
                                       frame.digital_channel_17,
                                       1);
                failures += expect_int("data send frame lost",
                                       frame.frame_lost,
                                       1);
            }

            sbus_data_deinit(&data);
            close(pipe_fd[0]);
        }
    }
#endif

    frame_bytes[24] = 0x7f;
    failures += expect_int("invalid footer",
                           sbus_parse_frame(frame_bytes, &frame),
                           SBUS_ERR_FRAME);

    if (failures == 0) {
        printf("PASS: SBUS parser decoded all 16 channels.\n");
        printf("PASS: CH1-2 joystick, CH3-4 knob, CH5-8 button mapping is correct.\n");
    }
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

static void print_live_controls(const sbus_controls_t *controls) {
    size_t channel;

    printf("\rRAW");
    for (channel = 0; channel < SBUS_CONTROL_CHANNEL_COUNT; ++channel) {
        printf(" CH%zu=%4u", channel + 1U, controls->raw[channel]);
    }
    printf(" | JOY=%d,%d KNOB=%d,%d BTN=%u,%u,%u,%u LOST=%u FAILSAFE=%u",
           controls->joystick[0],
           controls->joystick[1],
           controls->knob[0],
           controls->knob[1],
           controls->button[0],
           controls->button[1],
           controls->button[2],
           controls->button[3],
           controls->frame_lost,
           controls->failsafe);
    fflush(stdout);
}

static int run_live_test(const char *device) {
    sbus_data_t data;
    sbus_frame_t frame;
    sbus_controls_t controls;
    sbus_status_t status;

    status = sbus_data_init(&data, device);
    if (status != SBUS_OK) {
        fprintf(stderr, "Cannot open %s: %s\n", device, sbus_status_string(status));
        return EXIT_FAILURE;
    }

    signal(SIGINT, handle_stop_signal);
    signal(SIGTERM, handle_stop_signal);
    printf("Reading %s at 100000 baud, 8E2. Press Ctrl+C to stop.\n", device);
    while (!g_stop_requested) {
        status = sbus_data_receive(&data, &frame);
        if (status == SBUS_OK) {
            sbus_channels_decode(&frame, &controls);
            print_live_controls(&controls);
        } else if (status != SBUS_NO_FRAME) {
            fprintf(stderr, "\nRead failed: %s\n", sbus_status_string(status));
            sbus_data_deinit(&data);
            return EXIT_FAILURE;
        }
        sleep_one_millisecond();
    }

    printf("\n");
    sbus_data_deinit(&data);
    return EXIT_SUCCESS;
}

int main(int argc, char **argv) {
    if (argc == 1) {
        return run_parser_tests();
    }
    if (argc == 2) {
        return run_live_test(argv[1]);
    }

    fprintf(stderr, "Usage: %s [serial-device]\n", argv[0]);
    return EXIT_FAILURE;
}
