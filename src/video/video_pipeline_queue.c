#include "video_pipeline_internal.h"

void vp_queue_push_locked(vp_frame_queue_t *q, const AVFrame *frame) {
    int slot;

    if (q->count == VP_QUEUE_SIZE) {
        slot = q->head;
        if (q->frames[slot] != NULL) {
            av_frame_unref(q->frames[slot]);
        }
        q->head = (q->head + 1) % VP_QUEUE_SIZE;
        q->count--;
        q->dropped++;
    }

    slot = (q->head + q->count) % VP_QUEUE_SIZE;
    if (q->frames[slot] == NULL) {
        q->frames[slot] = av_frame_alloc();
        if (q->frames[slot] == NULL) {
            return;
        }
    } else {
        av_frame_unref(q->frames[slot]);
    }

    if (av_frame_ref(q->frames[slot], frame) == 0) {
        q->count++;
        if (q->count > q->max_count) {
            q->max_count = q->count;
        }
    }
}

int vp_queue_pop_locked(vp_frame_queue_t *q, AVFrame *dst) {
    if (q->count == 0) {
        return 0;
    }
    av_frame_unref(dst);
    if (av_frame_ref(dst, q->frames[q->head]) < 0) {
        return 0;
    }
    av_frame_unref(q->frames[q->head]);
    q->head = (q->head + 1) % VP_QUEUE_SIZE;
    q->count--;
    return 1;
}

void vp_queue_clear_locked(vp_frame_queue_t *q) {
    int i;

    for (i = 0; i < VP_QUEUE_SIZE; ++i) {
        if (q->frames[i] != NULL) {
            av_frame_unref(q->frames[i]);
        }
    }
    q->head = 0;
    q->count = 0;
}

void vp_queue_push_latest_locked(vp_frame_queue_t *q, const AVFrame *frame) {
    int stale_count;

    if (q == NULL || frame == NULL) {
        return;
    }

    stale_count = q->count;
    if (stale_count > 0) {
        vp_queue_clear_locked(q);
        q->dropped += stale_count;
    }
    vp_queue_push_locked(q, frame);
}

void vp_queue_free(vp_frame_queue_t *q) {
    int i;

    for (i = 0; i < VP_QUEUE_SIZE; ++i) {
        if (q->frames[i] != NULL) {
            av_frame_free(&q->frames[i]);
        }
    }
}
