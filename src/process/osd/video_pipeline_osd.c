#include "video_pipeline_internal.h"

#include <math.h>
#include <stdio.h>
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
                                     int outline_radius,
                                     uint8_t red,
                                     uint8_t green,
                                     uint8_t blue,
                                     uint8_t alpha) {
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
            if (outline_radius > 0) {
                int offset_y;
                int offset_x;

                for (offset_y = -outline_radius;
                     offset_y <= outline_radius;
                     ++offset_y) {
                    for (offset_x = -outline_radius;
                         offset_x <= outline_radius;
                         ++offset_x) {
                        uint8_t *pixel;
                        int draw_x = x + offset_x;
                        int draw_y = y + offset_y;

                        if (offset_x * offset_x + offset_y * offset_y >
                                outline_radius * outline_radius ||
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
                                          (uint8_t)((unsigned int)coverage * alpha /
                                                    255U));
                    }
                }
            } else if (x >= 0 && x < rgba_frame->width) {
                uint8_t *pixel = rgba_frame->data[0] +
                                 y * rgba_frame->linesize[0] + x * 4;

                vp_osd_rgba_blend(pixel,
                                  red,
                                  green,
                                  blue,
                                  (uint8_t)((unsigned int)coverage * alpha /
                                            255U));
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
        vp_osd_draw_glyph_bitmap(rgba_frame,
                                 glyph,
                                 pen_x,
                                 baseline,
                                 2,
                                 0,
                                 0,
                                 0,
                                 190);
        vp_osd_draw_glyph_bitmap(rgba_frame,
                                 glyph,
                                 pen_x,
                                 baseline,
                                 0,
                                 255,
                                 255,
                                 255,
                                 235);
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
    const int right_margin = 82;
    const int start_y = 22;
    const int positions[6] = {0, 28, 68, 96, 136, 164};
    const int timer_width = 164 + digit_width;
    int start_x;
    int values[6];
    int i;

    start_x = f->width - right_margin - timer_width;
    if (start_x < 0) {
        start_x = 0;
    }

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

#define VP_OSD_BASE_WIDTH 1920
#define VP_OSD_BASE_HEIGHT 1080
#define VP_OSD_LAYOUT_FONT_PATH \
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"
#define VP_OSD_FONT_PIXELS_PER_SCALE 8
#define VP_OSD_SCALE_ALPHA 128
#define VP_OSD_CROSSHAIR_ALPHA 145
#define VP_OSD_TEXT_ALPHA 175
#define VP_OSD_POINTER_ALPHA 175
#define VP_OSD_OUTLINE_ALPHA 115

static pthread_mutex_t vp_osd_layout_font_lock = PTHREAD_MUTEX_INITIALIZER;
static FT_Library vp_osd_layout_font_library;
static FT_Face vp_osd_layout_font_face;
static int vp_osd_layout_font_attempted;
static int vp_osd_layout_font_available;

static const uint8_t vp_osd_font_digits[10][7] = {
    {0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e},
    {0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e},
    {0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f},
    {0x1e, 0x01, 0x01, 0x0e, 0x01, 0x01, 0x1e},
    {0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02},
    {0x1f, 0x10, 0x10, 0x1e, 0x01, 0x01, 0x1e},
    {0x0e, 0x10, 0x10, 0x1e, 0x11, 0x11, 0x0e},
    {0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
    {0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e},
    {0x0e, 0x11, 0x11, 0x0f, 0x01, 0x01, 0x0e}
};

static int vp_osd_scale_x(const AVFrame *f, int value) {
    return (value * f->width + VP_OSD_BASE_WIDTH / 2) / VP_OSD_BASE_WIDTH;
}

static int vp_osd_scale_y(const AVFrame *f, int value) {
    return (value * f->height + VP_OSD_BASE_HEIGHT / 2) / VP_OSD_BASE_HEIGHT;
}

static int vp_osd_scale_size(const AVFrame *f, int value) {
    int x = vp_osd_scale_x(f, value);
    int y = vp_osd_scale_y(f, value);
    int scaled = x < y ? x : y;

    return scaled > 0 ? scaled : 1;
}

static float vp_osd_clamp_float(float value, float minimum, float maximum) {
    if (value < minimum) {
        return minimum;
    }
    if (value > maximum) {
        return maximum;
    }
    return value;
}

static const uint8_t *vp_osd_font_glyph(char c) {
    static const uint8_t blank[7] = {0, 0, 0, 0, 0, 0, 0};
    static const uint8_t plus[7] = {0, 0x04, 0x04, 0x1f, 0x04, 0x04, 0};
    static const uint8_t minus[7] = {0, 0, 0, 0x1f, 0, 0, 0};
    static const uint8_t dot[7] = {0, 0, 0, 0, 0, 0x06, 0x06};
    static const uint8_t degree[7] = {0x0c, 0x12, 0x12, 0x0c, 0, 0, 0};
    static const uint8_t glyph_a[7] = {0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11};
    static const uint8_t glyph_d[7] = {0x1e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1e};
    static const uint8_t glyph_e[7] = {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f};
    static const uint8_t glyph_k[7] = {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11};
    static const uint8_t glyph_l[7] = {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f};
    static const uint8_t glyph_m[7] = {0, 0, 0x1a, 0x15, 0x15, 0x11, 0x11};
    static const uint8_t glyph_o[7] = {0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e};
    static const uint8_t glyph_r[7] = {0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11};
    static const uint8_t glyph_s[7] = {0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e};

    if (c >= '0' && c <= '9') {
        return vp_osd_font_digits[c - '0'];
    }
    switch (c) {
        case '+': return plus;
        case '-': return minus;
        case '.': return dot;
        case '@': return degree;
        case 'A': return glyph_a;
        case 'D': return glyph_d;
        case 'E': return glyph_e;
        case 'K': return glyph_k;
        case 'L': return glyph_l;
        case 'M':
        case 'm': return glyph_m;
        case 'O': return glyph_o;
        case 'R': return glyph_r;
        case 'S': return glyph_s;
        default: return blank;
    }
}

static int vp_osd_bitmap_text_width(const char *text, int scale) {
    size_t length = text != NULL ? strlen(text) : 0;

    return length > 0 ? (int)(length * (size_t)(6 * scale) - (size_t)scale) : 0;
}

static void vp_osd_draw_bitmap_text(AVFrame *f,
                                    const char *text,
                                    int x,
                                    int y,
                                    int scale,
                                    uint8_t red,
                                    uint8_t green,
                                    uint8_t blue,
                                    uint8_t alpha) {
    const char *cursor;
    int pen_x = x;

    if (f == NULL || text == NULL || scale <= 0) {
        return;
    }
    for (cursor = text; *cursor != '\0'; ++cursor) {
        const uint8_t *glyph = vp_osd_font_glyph(*cursor);
        int row;

        for (row = 0; row < 7; ++row) {
            int col;

            for (col = 0; col < 5; ++col) {
                if ((glyph[row] & (1U << (4 - col))) != 0) {
                    int px = pen_x + col * scale;
                    int py = y + row * scale;

                    vp_osd_rgba_rect(f,
                                     px - 1,
                                     py - 1,
                                     scale + 2,
                                     scale + 2,
                                     0,
                                     0,
                                     0,
                                     180);
                    vp_osd_rgba_rect(f,
                                     px,
                                     py,
                                     scale,
                                     scale,
                                     red,
                                     green,
                                     blue,
                                     alpha);
                }
            }
        }
        pen_x += 6 * scale;
    }
}

static int vp_osd_layout_font_ready_locked(void) {
    if (vp_osd_layout_font_attempted) {
        return vp_osd_layout_font_available;
    }
    vp_osd_layout_font_attempted = 1;
    if (FT_Init_FreeType(&vp_osd_layout_font_library) != 0) {
        return 0;
    }
    if (FT_New_Face(vp_osd_layout_font_library,
                    VP_OSD_LAYOUT_FONT_PATH,
                    0,
                    &vp_osd_layout_font_face) != 0) {
        FT_Done_FreeType(vp_osd_layout_font_library);
        vp_osd_layout_font_library = NULL;
        return 0;
    }
    FT_Select_Charmap(vp_osd_layout_font_face, FT_ENCODING_UNICODE);
    vp_osd_layout_font_available = 1;
    return 1;
}

static uint32_t vp_osd_layout_codepoint(uint32_t codepoint) {
    return codepoint == '@' ? 0x00b0U : codepoint;
}

static int vp_osd_freetype_text_width(const char *text, int scale) {
    const char *cursor;
    int width = 0;
    int font_size = scale * VP_OSD_FONT_PIXELS_PER_SCALE;

    if (text == NULL || scale <= 0) {
        return -1;
    }
    pthread_mutex_lock(&vp_osd_layout_font_lock);
    if (!vp_osd_layout_font_ready_locked() ||
        FT_Set_Pixel_Sizes(vp_osd_layout_font_face,
                           0,
                           (FT_UInt)font_size) != 0) {
        pthread_mutex_unlock(&vp_osd_layout_font_lock);
        return -1;
    }
    cursor = text;
    while (*cursor != '\0') {
        uint32_t codepoint = vp_osd_layout_codepoint(
            vp_osd_utf8_next(&cursor));

        if (FT_Load_Char(vp_osd_layout_font_face,
                         codepoint,
                         FT_LOAD_DEFAULT) == 0) {
            width += (int)(vp_osd_layout_font_face->glyph->advance.x >> 6);
        }
    }
    pthread_mutex_unlock(&vp_osd_layout_font_lock);
    return width;
}

static int vp_osd_draw_freetype_text(AVFrame *f,
                                     const char *text,
                                     int x,
                                     int y,
                                     int scale,
                                     uint8_t red,
                                     uint8_t green,
                                     uint8_t blue,
                                     uint8_t alpha) {
    const char *cursor;
    int pen_x = x;
    int baseline;
    int font_size = scale * VP_OSD_FONT_PIXELS_PER_SCALE;

    if (f == NULL || text == NULL || scale <= 0) {
        return 0;
    }
    pthread_mutex_lock(&vp_osd_layout_font_lock);
    if (!vp_osd_layout_font_ready_locked() ||
        FT_Set_Pixel_Sizes(vp_osd_layout_font_face,
                           0,
                           (FT_UInt)font_size) != 0) {
        pthread_mutex_unlock(&vp_osd_layout_font_lock);
        return 0;
    }
    baseline = y + (int)(vp_osd_layout_font_face->size->metrics.ascender >> 6);
    cursor = text;
    while (*cursor != '\0') {
        uint32_t codepoint = vp_osd_layout_codepoint(
            vp_osd_utf8_next(&cursor));
        FT_GlyphSlot glyph;

        if (FT_Load_Char(vp_osd_layout_font_face,
                         codepoint,
                         FT_LOAD_RENDER) != 0) {
            continue;
        }
        glyph = vp_osd_layout_font_face->glyph;
        vp_osd_draw_glyph_bitmap(f,
                                 glyph,
                                 pen_x,
                                 baseline,
                                 scale >= 3 ? 2 : 1,
                                 0,
                                 0,
                                 0,
                                 VP_OSD_OUTLINE_ALPHA);
        vp_osd_draw_glyph_bitmap(f,
                                 glyph,
                                 pen_x,
                                 baseline,
                                 0,
                                 red,
                                 green,
                                 blue,
                                 alpha);
        pen_x += (int)(glyph->advance.x >> 6);
    }
    pthread_mutex_unlock(&vp_osd_layout_font_lock);
    return 1;
}

static int vp_osd_text_width(const char *text, int scale) {
    int width = vp_osd_freetype_text_width(text, scale);

    return width >= 0 ? width : vp_osd_bitmap_text_width(text, scale);
}

static void vp_osd_draw_text(AVFrame *f,
                             const char *text,
                             int x,
                             int y,
                             int scale,
                             uint8_t red,
                             uint8_t green,
                             uint8_t blue,
                             uint8_t alpha) {
    if (vp_osd_draw_freetype_text(f,
                                  text,
                                  x,
                                  y,
                                  scale,
                                  red,
                                  green,
                                  blue,
                                  alpha)) {
        return;
    }
    vp_osd_draw_bitmap_text(f,
                            text,
                            x,
                            y,
                            scale,
                            red,
                            green,
                            blue,
                            alpha);
}

static void vp_osd_draw_text_centered(AVFrame *f,
                                      const char *text,
                                      int center_x,
                                      int y,
                                      int scale,
                                      uint8_t red,
                                      uint8_t green,
                                      uint8_t blue,
                                      uint8_t alpha) {
    vp_osd_draw_text(f,
                     text,
                     center_x - vp_osd_text_width(text, scale) / 2,
                     y,
                     scale,
                     red,
                     green,
                     blue,
                     alpha);
}

static void vp_osd_draw_hline(AVFrame *f,
                              int x,
                              int y,
                              int width,
                              int thickness,
                              uint8_t red,
                              uint8_t green,
                              uint8_t blue,
                              uint8_t alpha) {
    if (width <= 0 || thickness <= 0) {
        return;
    }
    vp_osd_rgba_rect(f,
                     x - 2,
                     y - 2,
                     width + 4,
                     thickness + 4,
                     0,
                     0,
                     0,
                     (uint8_t)((unsigned int)alpha * 2U / 3U));
    vp_osd_rgba_rect(f, x, y, width, thickness, red, green, blue, alpha);
}

static void vp_osd_draw_vline(AVFrame *f,
                              int x,
                              int y,
                              int height,
                              int thickness,
                              uint8_t red,
                              uint8_t green,
                              uint8_t blue,
                              uint8_t alpha) {
    if (height <= 0 || thickness <= 0) {
        return;
    }
    vp_osd_rgba_rect(f,
                     x - 2,
                     y - 2,
                     thickness + 4,
                     height + 4,
                     0,
                     0,
                     0,
                     (uint8_t)((unsigned int)alpha * 2U / 3U));
    vp_osd_rgba_rect(f, x, y, thickness, height, red, green, blue, alpha);
}

static void vp_osd_draw_down_triangle(AVFrame *f,
                                      int center_x,
                                      int top_y,
                                      int half_width,
                                      int height) {
    int row;

    for (row = 0; row < height + 4; ++row) {
        int half = ((half_width + 2) * (height + 3 - row)) / (height + 3);
        vp_osd_rgba_rect(f, center_x - half, top_y - 2 + row, half * 2 + 1, 1,
                         0, 0, 0, VP_OSD_OUTLINE_ALPHA);
    }
    for (row = 0; row < height; ++row) {
        int half = (half_width * (height - row)) / height;
        vp_osd_rgba_rect(f, center_x - half, top_y + row, half * 2 + 1, 1,
                         255, 255, 255, VP_OSD_POINTER_ALPHA);
    }
}

static void vp_osd_draw_right_triangle(AVFrame *f,
                                       int left_x,
                                       int center_y,
                                       int width,
                                       int half_height) {
    int column;

    for (column = 0; column < width + 4; ++column) {
        int half = ((half_height + 2) * (width + 3 - column)) / (width + 3);
        vp_osd_rgba_rect(f, left_x - 2 + column, center_y - half, 1,
                         half * 2 + 1, 0, 0, 0, VP_OSD_OUTLINE_ALPHA);
    }
    for (column = 0; column < width; ++column) {
        int half = (half_height * (width - column)) / width;
        vp_osd_rgba_rect(f, left_x + column, center_y - half, 1,
                         half * 2 + 1, 255, 255, 255, VP_OSD_POINTER_ALPHA);
    }
}

static void vp_osd_draw_ring(AVFrame *f, int center_x, int center_y, int radius) {
    int x;
    int y;

    for (y = -radius - 3; y <= radius + 3; ++y) {
        for (x = -radius - 3; x <= radius + 3; ++x) {
            int distance_squared = x * x + y * y;

            if (distance_squared >= (radius - 3) * (radius - 3) &&
                distance_squared <= (radius + 3) * (radius + 3)) {
                vp_osd_rgba_rect(f, center_x + x, center_y + y, 1, 1,
                                 0, 0, 0, VP_OSD_OUTLINE_ALPHA);
            }
        }
    }
    for (y = -radius - 1; y <= radius + 1; ++y) {
        for (x = -radius - 1; x <= radius + 1; ++x) {
            int distance_squared = x * x + y * y;

            if (distance_squared >= (radius - 1) * (radius - 1) &&
                distance_squared <= (radius + 1) * (radius + 1)) {
                vp_osd_rgba_rect(f, center_x + x, center_y + y, 1, 1,
                                 255, 255, 255, VP_OSD_CROSSHAIR_ALPHA);
            }
        }
    }
}

static void vp_osd_draw_yaw_scale(const vp_osd_params_t *params, AVFrame *f) {
    const int left = vp_osd_scale_x(f, 510);
    const int right = vp_osd_scale_x(f, 1410);
    const int line_y = vp_osd_scale_y(f, 142);
    const int line_thickness = vp_osd_scale_size(f, 3);
    const int label_scale = vp_osd_scale_size(f, 3);
    const int value_scale = vp_osd_scale_size(f, 5);
    char text[32];
    int angle;

    vp_osd_draw_hline(f, left, line_y, right - left + 1, line_thickness,
                      255, 255, 255, VP_OSD_SCALE_ALPHA);
    for (angle = -140; angle <= 140; angle += 10) {
        int x = left + (angle + 140) * (right - left) / 280;
        int primary = angle == -140 || angle == -70 || angle == 0 ||
                      angle == 70 || angle == 140;
        int tick_base = angle == 0 ? 34 : (primary ? 26 :
                        ((angle % 20) == 0 ? 16 : 10));
        int tick_height = vp_osd_scale_size(f, tick_base);

        vp_osd_draw_vline(f, x, line_y, tick_height, line_thickness,
                          255, 255, 255, VP_OSD_SCALE_ALPHA);
        if (primary) {
            if (angle > 0) {
                snprintf(text, sizeof(text), "+%d@", angle);
            } else {
                snprintf(text, sizeof(text), "%d@", angle);
            }
            vp_osd_draw_text_centered(f,
                                      text,
                                      x,
                                      vp_osd_scale_y(f, 184),
                                      label_scale,
                                      255,
                                      255,
                                      255,
                                      VP_OSD_TEXT_ALPHA);
        }
    }

    if (params->yaw_valid) {
        float yaw = vp_osd_clamp_float(params->yaw_deg,
                                       VP_OSD_YAW_MIN_DEG,
                                       VP_OSD_YAW_MAX_DEG);
        int pointer_x = left + (int)((yaw + 140.0f) * (float)(right - left) /
                                     280.0f + 0.5f);

        vp_osd_draw_down_triangle(f,
                                  pointer_x,
                                  vp_osd_scale_y(f, 118),
                                  vp_osd_scale_size(f, 10),
                                  vp_osd_scale_size(f, 19));
        snprintf(text, sizeof(text), "%+06.1f@", yaw);
    } else {
        snprintf(text, sizeof(text), "---.-@");
    }
    vp_osd_draw_text_centered(f,
                              text,
                              vp_osd_scale_x(f, 960),
                              vp_osd_scale_y(f, 70),
                              value_scale,
                              255,
                              255,
                              255,
                              VP_OSD_TEXT_ALPHA);
}

static void vp_osd_draw_pitch_scale(const vp_osd_params_t *params, AVFrame *f) {
    static const struct {
        int y;
        const char *label;
        int major;
    } ticks[] = {
        {270, "+30@", 1}, {338, NULL, 0}, {405, "+15@", 1},
        {472, NULL, 0}, {540, "0@", 1}, {585, NULL, 0},
        {630, "-30@", 1}, {675, NULL, 0}, {720, "-60@", 1},
        {765, NULL, 0}, {810, "-90@", 1}
    };
    const int pitch_x = vp_osd_scale_x(f, 1710);
    const int top = vp_osd_scale_y(f, 270);
    const int center = vp_osd_scale_y(f, 540);
    const int bottom = vp_osd_scale_y(f, 810);
    const int line_thickness = vp_osd_scale_size(f, 3);
    const int label_scale = vp_osd_scale_size(f, 3);
    const int value_scale = vp_osd_scale_size(f, 5);
    size_t i;
    char text[32];

    vp_osd_draw_vline(f, pitch_x, top, bottom - top + 1, line_thickness,
                      255, 255, 255, VP_OSD_SCALE_ALPHA);
    for (i = 0; i < sizeof(ticks) / sizeof(ticks[0]); ++i) {
        int y = vp_osd_scale_y(f, ticks[i].y);
        int tick_width = vp_osd_scale_size(f, ticks[i].major ? 28 : 15);

        vp_osd_draw_hline(f,
                          pitch_x - tick_width,
                          y,
                          tick_width,
                          line_thickness,
                          255,
                          255,
                          255,
                          VP_OSD_SCALE_ALPHA);
        if (ticks[i].label != NULL) {
            int text_width = vp_osd_text_width(ticks[i].label, label_scale);

            vp_osd_draw_text(f,
                             ticks[i].label,
                             pitch_x - tick_width -
                                 vp_osd_scale_size(f, 14) - text_width,
                             y - label_scale * 3,
                             label_scale,
                             255,
                             255,
                             255,
                             VP_OSD_TEXT_ALPHA);
        }
    }

    if (params->pitch_valid) {
        float pitch = vp_osd_clamp_float(params->pitch_deg,
                                         VP_OSD_PITCH_MIN_DEG,
                                         VP_OSD_PITCH_MAX_DEG);
        int pointer_y;

        if (pitch >= 0.0f) {
            pointer_y = center -
                        (int)(pitch * (float)(center - top) / 30.0f + 0.5f);
        } else {
            pointer_y = center +
                        (int)((-pitch) * (float)(bottom - center) / 90.0f + 0.5f);
        }
        vp_osd_draw_right_triangle(f,
                                   pitch_x - vp_osd_scale_size(f, 30),
                                   pointer_y,
                                   vp_osd_scale_size(f, 25),
                                   vp_osd_scale_size(f, 10));
        snprintf(text, sizeof(text), "%+06.1f@", pitch);
    } else {
        snprintf(text, sizeof(text), "---.-@");
    }
    vp_osd_draw_text_centered(f,
                              text,
                              pitch_x,
                              vp_osd_scale_y(f, 842),
                              value_scale,
                              255,
                              255,
                              255,
                              VP_OSD_TEXT_ALPHA);
}

static void vp_osd_draw_crosshair_and_laser(const vp_osd_params_t *params,
                                             AVFrame *f) {
    const int arm = vp_osd_scale_size(f, 72);
    const int gap = vp_osd_scale_size(f, 18);
    const int thickness = vp_osd_scale_size(f, 3);
    const int text_scale = vp_osd_scale_size(f, 4);
    int center_x = params->cross_x >= 0 ? params->cross_x : f->width / 2;
    int center_y = params->cross_y >= 0 ? params->cross_y : f->height / 2;
    int laser_y;
    int label_width;
    int value_width;
    int start_x;
    char value[32];

    if (center_x < 0) {
        center_x = 0;
    } else if (center_x >= f->width) {
        center_x = f->width - 1;
    }
    if (center_y < 0) {
        center_y = 0;
    } else if (center_y >= f->height) {
        center_y = f->height - 1;
    }

    vp_osd_draw_hline(f, center_x - arm, center_y, arm - gap, thickness,
                      255, 255, 255, VP_OSD_CROSSHAIR_ALPHA);
    vp_osd_draw_hline(f, center_x + gap, center_y, arm - gap, thickness,
                      255, 255, 255, VP_OSD_CROSSHAIR_ALPHA);
    vp_osd_draw_vline(f, center_x, center_y - arm, arm - gap, thickness,
                      255, 255, 255, VP_OSD_CROSSHAIR_ALPHA);
    vp_osd_draw_vline(f, center_x, center_y + gap, arm - gap, thickness,
                      255, 255, 255, VP_OSD_CROSSHAIR_ALPHA);
    vp_osd_draw_ring(f, center_x, center_y, vp_osd_scale_size(f, 8));

    if (params->laser_valid) {
        float distance = vp_osd_clamp_float(params->distance_m, 0.0f, 9999.9f);

        snprintf(value, sizeof(value), "%.1f m", distance);
    } else {
        snprintf(value, sizeof(value), "---.- m");
    }
    label_width = vp_osd_text_width("LASER", text_scale);
    value_width = vp_osd_text_width(value, text_scale);
    start_x = center_x - (label_width + value_width +
                          vp_osd_scale_size(f, 24)) / 2;
    laser_y = center_y + vp_osd_scale_size(f, 120);
    if (params->laser_continuous) {
        int64_t now_us = app_get_time_us();
        int red_phase = (now_us / 500000LL) % 2 == 0;
        vp_osd_draw_status_dot(f,
                               start_x - vp_osd_scale_size(f, 18),
                               laser_y + text_scale * 3,
                               vp_osd_scale_size(f, 5),
                               red_phase ? 255 : 32,
                               red_phase ? 32 : 255,
                               32,
                               1);
    } else if (params->laser_measuring) {
        int64_t now_us = app_get_time_us();
        int red_phase = (now_us / 150000LL) % 2 == 0;
        vp_osd_draw_status_dot(f,
                               start_x - vp_osd_scale_size(f, 18),
                               laser_y + text_scale * 3,
                               vp_osd_scale_size(f, 5),
                               red_phase ? 255 : 32,
                               red_phase ? 32 : 255,
                               32,
                               1);
    } else if (params->laser_error) {
        vp_osd_draw_status_dot(f,
                               start_x - vp_osd_scale_size(f, 18),
                               laser_y + text_scale * 3,
                               vp_osd_scale_size(f, 5),
                               255,
                               200,
                               0,
                               1);
    } else {
        vp_osd_draw_status_dot(f,
                               start_x - vp_osd_scale_size(f, 18),
                               laser_y + text_scale * 3,
                               vp_osd_scale_size(f, 5),
                               255,
                               32,
                               32,
                               params->laser_valid);
    }
    vp_osd_draw_text(f,
                     "LASER",
                     start_x,
                     laser_y,
                     text_scale,
                     255,
                     255,
                     255,
                     VP_OSD_TEXT_ALPHA);
    vp_osd_draw_text(f,
                     value,
                     start_x + label_width + vp_osd_scale_size(f, 24),
                     laser_y,
                     text_scale,
                     255,
                     32,
                     32,
                     245);
}

static void vp_osd_draw_layout(const vp_osd_params_t *params, AVFrame *f) {
    vp_osd_draw_yaw_scale(params, f);
    vp_osd_draw_pitch_scale(params, f);
    vp_osd_draw_crosshair_and_laser(params, f);
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
    int gap = 18;
    int arm = 72;
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
        for (x = cx - arm; x <= cx + arm; ++x) {
            if (x < 0 || x >= f->width ||
                (x > cx - gap && x < cx + gap)) {
                continue;
            }
            y_plane[y * f->linesize[0] + x] = 255;
        }
    }

    for (x = cx - thick; x <= cx + thick; ++x) {
        if (x < 0 || x >= f->width) {
            continue;
        }
        for (y = cy - arm; y <= cy + arm; ++y) {
            if (y < 0 || y >= f->height ||
                (y > cy - gap && y < cy + gap)) {
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

    vp_osd_draw_layout(params, rgba_frame);
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
    uint32_t next_seq;

    if (p == NULL || params == NULL) {
        return APP_ERR_PARAM;
    }
    pthread_mutex_lock(&p->osd_lock);
    next_seq = p->osd.seq + 1U;
    p->osd = *params;
    p->osd.seq = next_seq;
    pthread_mutex_unlock(&p->osd_lock);
    return APP_OK;
}

app_status_t vp_osd_get(vp_ctx_t *p, vp_osd_params_t *out_params) {
    if (p == NULL || out_params == NULL) {
        return APP_ERR_PARAM;
    }
    vp_osd_get_snapshot(p, out_params);
    return APP_OK;
}

app_status_t vp_osd_set_gimbal_angles(vp_ctx_t *p,
                                      float yaw_deg,
                                      float pitch_deg,
                                      int valid) {
    if (p == NULL || (valid && (!isfinite(yaw_deg) || !isfinite(pitch_deg)))) {
        return APP_ERR_PARAM;
    }
    pthread_mutex_lock(&p->osd_lock);
    if (valid) {
        p->osd.yaw_deg = vp_osd_clamp_float(yaw_deg,
                                            VP_OSD_YAW_MIN_DEG,
                                            VP_OSD_YAW_MAX_DEG);
        p->osd.pitch_deg = vp_osd_clamp_float(pitch_deg,
                                              VP_OSD_PITCH_MIN_DEG,
                                              VP_OSD_PITCH_MAX_DEG);
    }
    p->osd.yaw_valid = valid != 0;
    p->osd.pitch_valid = valid != 0;
    p->osd.seq++;
    pthread_mutex_unlock(&p->osd_lock);
    return APP_OK;
}

app_status_t vp_osd_set_laser_distance(vp_ctx_t *p,
                                       float distance_m,
                                       int signal_level,
                                       int valid) {
    if (p == NULL ||
        (valid && (!isfinite(distance_m) || distance_m < 0.0f))) {
        return APP_ERR_PARAM;
    }
    pthread_mutex_lock(&p->osd_lock);
    if (valid) {
        p->osd.distance_m = distance_m;
        p->osd.signal_level = signal_level;
    }
    p->osd.laser_valid = valid != 0;
    p->osd.seq++;
    pthread_mutex_unlock(&p->osd_lock);
    return APP_OK;
}

app_status_t vp_osd_set_laser_continuous(vp_ctx_t *p, int active) {
    if (p == NULL) {
        return APP_ERR_PARAM;
    }
    pthread_mutex_lock(&p->osd_lock);
    p->osd.laser_continuous = active != 0;
    p->osd.seq++;
    pthread_mutex_unlock(&p->osd_lock);
    return APP_OK;
}

app_status_t vp_osd_set_laser_measuring(vp_ctx_t *p, int measuring) {
    if (p == NULL) {
        return APP_ERR_PARAM;
    }
    pthread_mutex_lock(&p->osd_lock);
    p->osd.laser_measuring = measuring != 0;
    p->osd.seq++;
    pthread_mutex_unlock(&p->osd_lock);
    return APP_OK;
}

app_status_t vp_osd_set_laser_error(vp_ctx_t *p, int error) {
    if (p == NULL) {
        return APP_ERR_PARAM;
    }
    pthread_mutex_lock(&p->osd_lock);
    p->osd.laser_error = error != 0;
    p->osd.seq++;
    pthread_mutex_unlock(&p->osd_lock);
    return APP_OK;
}
