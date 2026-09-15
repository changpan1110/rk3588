// #ifndef SBUS_RC8_COMMAND_H
// #define SBUS_RC8_COMMAND_H

// #include "control/sbus/sbus_rc8.h"
// #include "process/video/video_pipeline.h"
// #include "control/video_pipeline/video_pipeline_control.h"

// typedef struct sbus_rc8_command sbus_rc8_command_t;

// /*
//  * Initializes one RC command thread.
//  * CH5 cycles cycle_channels[0..2], CH6 toggles recording,
//  * and CH7 snapshots the currently selected video channel.
//  */
// app_status_t sbus_rc8_command_init(
//     sbus_rc8_command_t **out_command,
//     sbus_rc8_t *sbus_rc8,
//     vp_ctx_t *pipeline,
//     vp_control_t *control,
//     const char *const cycle_channels[3]);

// void sbus_rc8_command_deinit(sbus_rc8_command_t *command);

// #endif
