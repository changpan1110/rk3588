#ifndef PROCESS_OSD_H
#define PROCESS_OSD_H

#include "common/common.h"
#include "serial/uart_laser.h"

app_status_t process_osd_apply_laser(video_frame_t *frame, const laser_data_t *laser);

#endif
