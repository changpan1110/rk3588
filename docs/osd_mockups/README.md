# OSD integration

The production OSD is rendered into a transparent RGBA frame by the CPU and
composited onto the NV12 DRM frame by `overlay_rkrga`. The PNG files in this
directory are visual references; runtime code does not load them per frame.

## Telemetry threads

Use one acquisition thread per independent device. No extra OSD rendering
thread is required. The setters only update the latest values under a mutex;
the existing stream thread snapshots and renders them once per video frame.

```c
/* Gimbal acquisition thread. Values are clamped to the displayed limits. */
vp_control_osd_set_gimbal_angles(control, yaw_deg, pitch_deg, 1);

/* Laser acquisition thread. */
vp_control_osd_set_laser_distance(control,
                                  distance_m,
                                  signal_level,
                                  1);

/* Mark stale or disconnected data invalid. Float arguments are ignored. */
vp_control_osd_set_gimbal_angles(control, 0.0f, 0.0f, 0);
vp_control_osd_set_laser_distance(control, 0.0f, 0, 0);
```

If gimbal and laser data share one serial port, parse both message types in one
acquisition thread and call the corresponding setter. Updates are latest-value
snapshots, not a queue: several packets received between two video frames are
collapsed to the newest values.

## Refresh rate

The visible OSD rate cannot exceed the video frame rate. A 30 fps stream shows
at most 30 OSD updates per second; a 50 or 60 fps stream can show 50 or 60.
Telemetry setters may run faster, but the stream thread only renders the newest
snapshot. The one-line status output now reports the average `osd` stage time so
RGA performance can be measured on the RK3588 target without timing-log spam.
