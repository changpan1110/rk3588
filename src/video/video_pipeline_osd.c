#include "video_pipeline_internal.h"

#include <stdint.h>
#include <string.h>

#include <ft2build.h>
#include FT_FREETYPE_H

static void vp_osd_rgba_rect(AVFrame *f,
                             int x,
                             int y,
                             int width,
                             int height,
                             uint8_t red,
                             uint8_t green,
                             uint8_t blue,
                             uint8_t alpha) {
    int x0;
    int y0;
    int x1;
    int y1;
    int px;
    int py;

    if (f == NULL || f->format != AV_PIX_FMT_RGBA || f->data[0] == NULL ||
        width <= 0 || height <= 0) {
        return;
    }
    x0 = x < 0 ? 0 : x;
    y0 = y < 0 ? 0 : y;
    x1 = x + width > f->width ? f->width : x + width;
    y1 = y + height > f->height ? f->height : y + height;
    for (py = y0; py < y1; ++py) {
        uint8_t *row = f->data[0] + py * f->linesize[0];

        for (px = x0; px < x1; ++px) {
            uint8_t *pixel = row + px * 4;

            pixel[0] = red;
            pixel[1] = green;
            pixel[2] = blue;
            pixel[3] = alpha;
        }
    }
}

static uint32_t vp_osd_utf8_next(const char **cursor) {
    const unsigned char *s = (const unsigned char *)*cursor;
    uint32_t codepoint;

    if (s[0] < 0x80) {
        *cursor += 1;
        return s[0];
    }
    if ((s[0] & 0xe0) == 0xc0 && s[1] != '\0' &&
        (s[1] & 0xc0) == 0x80) {
        codepoint = ((uint32_t)(s[0] & 0x1f) << 6) |
                    (uint32_t)(s[1] & 0x3f);
        if (codepoint >= 0x80) {
            *cursor += 2;
            return codepoint;
        }
    } else if ((s[0] & 0xf0) == 0xe0 && s[1] != '\0' && s[2] != '\0' &&
               (s[1] & 0xc0) == 0x80 &&
               (s[2] & 0xc0) == 0x80) {
        codepoint = ((uint32_t)(s[0] & 0x0f) << 12) |
                    ((uint32_t)(s[1] & 0x3f) << 6) |
                    (uint32_t)(s[2] & 0x3f);
        if (codepoint >= 0x800 && !(codepoint >= 0xd800 && codepoint <= 0xdfff)) {
            *cursor += 3;
            return codepoint;
        }
    } else if ((s[0] & 0xf8) == 0xf0 && s[1] != '\0' && s[2] != '\0' &&
               s[3] != '\0' &&
               (s[1] & 0xc0) == 0x80 &&
               (s[2] & 0xc0) == 0x80 &&
               (s[3] & 0xc0) == 0x80) {
        codepoint = ((uint32_t)(s[0] & 0x07) << 18) |
                    ((uint32_t)(s[1] & 0x3f) << 12) |
                    ((uint32_t)(s[2] & 0x3f) << 6) |
                    (uint32_t)(s[3] & 0x3f);
        if (codepoint >= 0x10000 && codepoint <= 0x10ffff) {
            *cursor += 4;
            return codepoint;
        }
    }
    *cursor += 1;
    return 0xfffd;
}

static void vp_osd_rgba_blend(uint8_t *pixel,
                              uint8_t red,
                              uint8_t green,
                              uint8_t blue,
                              uint8_t alpha) {
    unsigned int dst_alpha;
    unsigned int inverse;
    unsigned int out_alpha;
    unsigned int channel;
    const uint8_t color[3] = {red, green, blue};
    int i;

    if (alpha == 0) {
        return;
    }
    dst_alpha = pixel[3];
    inverse = 255U - alpha;
    out_alpha = alpha + (dst_alpha * inverse + 127U) / 255U;
    if (out_alpha == 0) {
        return;
    }
    for (i = 0; i < 3; ++i) {
        channel = (unsigned int)color[i] * alpha +
                  (unsigned int)pixel[i] * dst_alpha * inverse / 255U;
        pixel[i] = (uint8_t)(channel / out_alpha);
    }
    pixel[3] = (uint8_t)out_alpha;
}

static void vp_osd_draw_glyph_bitmap(AVFrame *rgba_frame,
                                     const FT_GlyphSlot glyph,
                                     int pen_x,
                                     int baseline,
                                     int outline) {
    int row;
    int col;

    for (row = 0; row < (int)glyph->bitmap.rows; ++row) {
        int y = baseline - glyph->bitmap_top + row;
        const uint8_t *bitmap_row;

        if (y < 0 || y >= rgba_frame->height) {
            continue;
        }
        bitmap_row = glyph->bitmap.pitch >= 0
                         ? glyph->bitmap.buffer + row * glyph->bitmap.pitch
                         : glyph->bitmap.buffer +
                               (glyph->bitmap.rows - 1 - row) *
                                   (-glyph->bitmap.pitch);
        for (col = 0; col < (int)glyph->bitmap.width; ++col) {
            int x = pen_x + glyph->bitmap_left + col;
            uint8_t coverage = bitmap_row[col];

            if (coverage == 0) {
                continue;
            }
            if (outline) {
                int offset_y;
                int offset_x;

                for (offset_y = -2; offset_y <= 2; ++offset_y) {
                    for (offset_x = -2; offset_x <= 2; ++offset_x) {
                        uint8_t *pixel;
                        int draw_x = x + offset_x;
                        int draw_y = y + offset_y;

                        if (offset_x * offset_x + offset_y * offset_y > 4 ||
                            draw_x < 0 || draw_x >= rgba_frame->width ||
                            draw_y < 0 || draw_y >= rgba_frame->height) {
                            continue;
                        }
                        pixel = rgba_frame->data[0] +
                                draw_y * rgba_frame->linesize[0] + draw_x * 4;
                        vp_osd_rgba_blend(pixel,
                                          0,
                                          0,
                                          0,
                                          (uint8_t)((unsigned int)coverage * 190U /
                                                    255U));
                    }
                }
            } else if (x >= 0 && x < rgba_frame->width) {
                uint8_t *pixel = rgba_frame->data[0] +
                                 y * rgba_frame->linesize[0] + x * 4;

                vp_osd_rgba_blend(pixel,
                                  255,
                                  255,
                                  255,
                                  (uint8_t)((unsigned int)coverage * 235U / 255U));
            }
        }
    }
}

app_status_t vp_osd_render_source_label(const char *text,
                                        const char *font_path,
                                        int font_size,
                                        AVFrame *rgba_frame) {
    FT_Library library = NULL;
    FT_Face face = NULL;
    const char *cursor;
    int baseline;
    int pen_x = 16;
    int ret;

    if (text == NULL || text[0] == '\0' || font_path == NULL ||
        font_path[0] == '\0' || font_size <= 0 || rgba_frame == NULL ||
        rgba_frame->format != AV_PIX_FMT_RGBA || rgba_frame->data[0] == NULL) {
        return APP_ERR_PARAM;
    }
    if (av_frame_make_writable(rgba_frame) < 0) {
        return APP_ERR_FFMPEG;
    }
    memset(rgba_frame->data[0],
           0,
           (size_t)rgba_frame->linesize[0] * (size_t)rgba_frame->height);

    ret = FT_Init_FreeType(&library);
    if (ret != 0) {
        return APP_ERR_IO;
    }
    ret = FT_New_Face(library, font_path, 0, &face);
    if (ret != 0) {
        FT_Done_FreeType(library);
        return APP_ERR_IO;
    }
    FT_Select_Charmap(face, FT_ENCODING_UNICODE);
    ret = FT_Set_Pixel_Sizes(face, 0, (FT_UInt)font_size);
    if (ret != 0) {
        FT_Done_Face(face);
        FT_Done_FreeType(library);
        return APP_ERR_PARAM;
    }

    baseline = (rgba_frame->height -
                (int)((face->size->metrics.ascender -
                       face->size->metrics.descender) >> 6)) /
                   2 +
               (int)(face->size->metrics.ascender >> 6);
    cursor = text;
    while (*cursor != '\0' && pen_x < rgba_frame->width - 8) {
        uint32_t codepoint = vp_osd_utf8_next(&cursor);
        FT_GlyphSlot glyph;

        if (FT_Load_Char(face, codepoint, FT_LOAD_RENDER) != 0) {
            continue;
        }
        glyph = face->glyph;
        vp_osd_draw_glyph_bitmap(rgba_frame, glyph, pen_x, baseline, 1);
        vp_osd_draw_glyph_bitmap(rgba_frame, glyph, pen_x, baseline, 0);
        pen_x += (int)(glyph->advance.x >> 6);
    }

    FT_Done_Face(face);
    FT_Done_FreeType(library);
    return APP_OK;
}

static void vp_osd_draw_digit(AVFrame *f,
                              int digit,
                              int x,
                              int y,
                              int width,
                              int height,
                              int thickness,
                              uint8_t red,
                              uint8_t green,
                              uint8_t blue,
                              uint8_t alpha) {
    static const uint8_t segments[10] = {
        0x3f, 0x06, 0x5b, 0x4f, 0x66,
        0x6d, 0x7d, 0x07, 0x7f, 0x6f
    };
    uint8_t mask;
    int vertical_height = height / 2 - thickness;
    if (digit < 0 || digit > 9) {
        return;
    }
    mask = segments[digit];
    if (mask & 0x01) {
        vp_osd_rgba_rect(f, x + thickness, y, width - thickness * 2,
                         thickness, red, green, blue, alpha);
    }
    if (mask & 0x02) {
        vp_osd_rgba_rect(f, x + width - thickness, y + thickness,
                         thickness, vertical_height, red, green, blue, alpha);
    }
    if (mask & 0x04) {
        vp_osd_rgba_rect(f, x + width - thickness, y + height / 2,
                         thickness, vertical_height, red, green, blue, alpha);
    }
    if (mask & 0x08) {
        vp_osd_rgba_rect(f, x + thickness, y + height - thickness,
                         width - thickness * 2, thickness,
                         red, green, blue, alpha);
    }
    if (mask & 0x10) {
        vp_osd_rgba_rect(f, x, y + height / 2,
                         thickness, vertical_height, red, green, blue, alpha);
    }
    if (mask & 0x20) {
        vp_osd_rgba_rect(f, x, y + thickness,
                         thickness, vertical_height, red, green, blue, alpha);
    }
    if (mask & 0x40) {
        vp_osd_rgba_rect(f, x + thickness, y + height / 2 - thickness / 2,
                         width - thickness * 2, thickness,
                         red, green, blue, alpha);
    }
}

static void vp_osd_draw_colon(AVFrame *f,
                              int x,
                              int y,
                              uint8_t red,
                              uint8_t green,
                              uint8_t blue,
                              uint8_t alpha) {
    vp_osd_rgba_rect(f, x, y + 11, 5, 5, red, green, blue, alpha);
    vp_osd_rgba_rect(f, x, y + 31, 5, 5, red, green, blue, alpha);
}

static void vp_osd_draw_record_time(AVFrame *f, uint64_t seconds) {
    const int digit_width = 23;
    const int digit_height = 46;
    const int thickness = 5;
    const int start_x = 82;
    const int start_y = 22;
    const int positions[6] = {0, 28, 68, 96, 136, 164};
    int values[6];
    int i;

    if (seconds > 359999ULL) {
        seconds = 359999ULL;
    }
    values[0] = (int)(seconds / 36000ULL);
    values[1] = (int)((seconds / 3600ULL) % 10ULL);
    values[2] = (int)((seconds / 600ULL) % 6ULL);
    values[3] = (int)((seconds / 60ULL) % 10ULL);
    values[4] = (int)((seconds / 10ULL) % 6ULL);
    values[5] = (int)(seconds % 10ULL);

    for (i = 0; i < 6; ++i) {
        vp_osd_draw_digit(f,
                          values[i],
                          start_x + positions[i] + 2,
                          start_y + 2,
                          digit_width,
                          digit_height,
                          thickness,
                          0,
                          0,
                          0,
                          110);
        vp_osd_draw_digit(f,
                          values[i],
                          start_x + positions[i],
                          start_y,
                          digit_width,
                          digit_height,
                          thickness,
                          255,
                          255,
                          255,
                          220);
    }
    vp_osd_draw_colon(f, start_x + 57, start_y + 2, 0, 0, 0, 110);
    vp_osd_draw_colon(f, start_x + 125, start_y + 2, 0, 0, 0, 110);
    vp_osd_draw_colon(f, start_x + 55, start_y, 255, 255, 255, 220);
    vp_osd_draw_colon(f, start_x + 123, start_y, 255, 255, 255, 220);
}

static void vp_osd_draw_status_dot(AVFrame *f,
                                   int center_x,
                                   int center_y,
                                   int radius,
                                   uint8_t red,
                                   uint8_t green,
                                   uint8_t blue,
                                   int visible) {
    int x;
    int y;

    if (!visible) {
        return;
    }
    for (y = center_y - radius - 2; y <= center_y + radius + 2; ++y) {
        for (x = center_x - radius - 2; x <= center_x + radius + 2; ++x) {
            int dx = x - center_x;
            int dy = y - center_y;
            int distance_squared = dx * dx + dy * dy;

            if (distance_squared <= (radius + 2) * (radius + 2)) {
                if (distance_squared <= radius * radius) {
                    vp_osd_rgba_rect(f, x, y, 1, 1, red, green, blue, 235);
                } else {
                    vp_osd_rgba_rect(f, x, y, 1, 1, 0, 0, 0, 150);
                }
            }
        }
    }
}

int vp_osd_needed(vp_ctx_t *p) {
    int needed;

    pthread_mutex_lock(&p->osd_lock);
    needed = p->osd.show_crosshair;
    pthread_mutex_unlock(&p->osd_lock);
    return needed;
}

void vp_osd_draw(vp_ctx_t *p, AVFrame *f) {
    vp_osd_params_t osd;
    uint8_t *y_plane;
    int cx;
    int cy;
    int gap = 16;
    int thick = 2;
    int x;
    int y;

    if (f->format != AV_PIX_FMT_NV12 || f->data[0] == NULL) {
        return;
    }

    pthread_mutex_lock(&p->osd_lock);
    osd = p->osd;
    pthread_mutex_unlock(&p->osd_lock);
    if (!osd.show_crosshair) {
        return;
    }

    cx = osd.cross_x >= 0 ? osd.cross_x : f->width / 2;
    cy = osd.cross_y >= 0 ? osd.cross_y : f->height / 2;
    y_plane = f->data[0];

    for (y = cy - thick; y <= cy + thick; ++y) {
        if (y < 0 || y >= f->height) {
            continue;
        }
        for (x = 0; x < f->width; ++x) {
            if (x > cx - gap && x < cx + gap) {
                continue;
            }
            y_plane[y * f->linesize[0] + x] = 255;
        }
    }

    for (x = cx - thick; x <= cx + thick; ++x) {
        if (x < 0 || x >= f->width) {
            continue;
        }
        for (y = 0; y < f->height; ++y) {
            if (y > cy - gap && y < cy + gap) {
                continue;
            }
            y_plane[y * f->linesize[0] + x] = 255;
        }
    }
}

void vp_osd_get_snapshot(vp_ctx_t *p, vp_osd_params_t *out) {
    if (p == NULL || out == NULL) {
        return;
    }
    pthread_mutex_lock(&p->osd_lock);
    *out = p->osd;
    pthread_mutex_unlock(&p->osd_lock);
}

app_status_t vp_osd_render_rgba(const vp_osd_params_t *params,
                                uint64_t recording_seconds,
                                int recording,
                                int blink_on,
                                int snapshot_flash_on,
                                AVFrame *rgba_frame) {
    int ret;

    if (params == NULL || rgba_frame == NULL ||
        rgba_frame->format != AV_PIX_FMT_RGBA ||
        rgba_frame->data[0] == NULL) {
        return APP_ERR_PARAM;
    }
    ret = av_frame_make_writable(rgba_frame);
    if (ret < 0) {
        return APP_ERR_FFMPEG;
    }
    memset(rgba_frame->data[0],
           0,
           (size_t)rgba_frame->linesize[0] * (size_t)rgba_frame->height);
    if (!params->show_crosshair) {
        return APP_OK;
    }

    if (recording) {
        vp_osd_draw_record_time(rgba_frame, recording_seconds);
        vp_osd_draw_status_dot(rgba_frame,
                               rgba_frame->width - 26,
                               27,
                               10,
                               255,
                               32,
                               32,
                               blink_on);
    }
    vp_osd_draw_status_dot(rgba_frame,
                           rgba_frame->width - 26,
                           70,
                           10,
                           32,
                           230,
                           96,
                           snapshot_flash_on);
    return APP_OK;
}

app_status_t vp_osd_notify_snapshot(vp_ctx_t *p) {
    if (p == NULL) {
        return APP_ERR_PARAM;
    }
    pthread_mutex_lock(&p->osd_lock);
    p->osd.snapshot_flash_until_us = app_get_time_us() + 800000LL;
    pthread_mutex_unlock(&p->osd_lock);
    return APP_OK;
}

app_status_t vp_osd_update(vp_ctx_t *p, const vp_osd_params_t *params) {
    if (p == NULL || params == NULL) {
        return APP_ERR_PARAM;
    }
    pthread_mutex_lock(&p->osd_lock);
    p->osd = *params;
    p->osd.seq++;
    pthread_mutex_unlock(&p->osd_lock);
    return APP_OK;
}
