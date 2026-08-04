#!/bin/bash

set -u

FFMPEG=/home/cat/rk3588/install/bin/ffmpeg
INPUT_FILE=${1:?input MJPEG file required}
CHANNELS=${2:?channel count required}
RUN_SECONDS=${3:-7}
LABEL=${4:-mjpeg}
PREFIX=/tmp/mjpeg_bench_${LABEL}_${CHANNELS}

for ((i = 1; i <= CHANNELS; ++i)); do
    progress_file=${PREFIX}_${i}.progress
    log_file=${PREFIX}_${i}.log
    status_file=${PREFIX}_${i}.status

    rm -f "$progress_file" "$log_file" "$status_file"
    (
        timeout --signal=INT --kill-after=2s "${RUN_SECONDS}s" \
            "$FFMPEG" \
            -nostdin -hide_banner -loglevel warning \
            -stats_period 1 \
            -stream_loop -1 \
            -framerate 30 \
            -f mjpeg \
            -c:v mjpeg_rkmpp \
            -i "$INPUT_FILE" \
            -an \
            -fps_mode passthrough \
            -progress "$progress_file" \
            -nostats \
            -f null - \
            > /dev/null 2> "$log_file"
        echo $? > "$status_file"
    ) &
done

wait

min_fps=999999
max_fps=0
sum_fps=0
passed=0
error_lines=0

for ((i = 1; i <= CHANNELS; ++i)); do
    progress_file=${PREFIX}_${i}.progress
    log_file=${PREFIX}_${i}.log
    status_file=${PREFIX}_${i}.status
    fps=$(awk -F= '$1 == "fps" { value = $2 } END { printf "%.2f", value + 0 }' "$progress_file")
    status=$(cat "$status_file")
    errors=$(grep -Eic 'error|failed|mpp_err|cannot|invalid' "$log_file" || true)

    printf 'channel=%d fps=%s status=%s errors=%d\n' "$i" "$fps" "$status" "$errors"

    min_fps=$(awk -v a="$min_fps" -v b="$fps" 'BEGIN { print (b < a) ? b : a }')
    max_fps=$(awk -v a="$max_fps" -v b="$fps" 'BEGIN { print (b > a) ? b : a }')
    sum_fps=$(awk -v a="$sum_fps" -v b="$fps" 'BEGIN { printf "%.2f", a + b }')
    if awk -v value="$fps" 'BEGIN { exit !(value >= 30.0) }'; then
        passed=$((passed + 1))
    fi
    error_lines=$((error_lines + errors))
done

average_fps=$(awk -v sum="$sum_fps" -v count="$CHANNELS" 'BEGIN { printf "%.2f", sum / count }')
printf 'summary label=%s channels=%d passed=%d min_fps=%.2f avg_fps=%s max_fps=%.2f aggregate_fps=%s errors=%d\n' \
    "$LABEL" "$CHANNELS" "$passed" "$min_fps" "$average_fps" "$max_fps" "$sum_fps" "$error_lines"
