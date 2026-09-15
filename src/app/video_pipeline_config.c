#define LOG_FILE_NAME "video_pipeline_config.c"

#include "app/video_pipeline_config.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VP_MAIN_CONFIG_MAX_BYTES (1024U * 1024U)

typedef struct {
    const char *text;
    size_t length;
    size_t position;
    char *error;
    size_t error_size;
} vp_json_reader_t;

static int vp_json_fail(vp_json_reader_t *reader, const char *format, ...) {
    va_list args;

    if (reader->error != NULL && reader->error_size > 0 &&
        reader->error[0] == '\0') {
        int prefix = snprintf(reader->error,
                              reader->error_size,
                              "JSON byte %zu: ",
                              reader->position);

        if (prefix >= 0 && (size_t)prefix < reader->error_size) {
            va_start(args, format);
            vsnprintf(reader->error + prefix,
                      reader->error_size - (size_t)prefix,
                      format,
                      args);
            va_end(args);
        }
    }
    return -1;
}

static void vp_json_skip_space(vp_json_reader_t *reader) {
    while (reader->position < reader->length &&
           isspace((unsigned char)reader->text[reader->position])) {
        ++reader->position;
    }
}

static int vp_json_consume(vp_json_reader_t *reader, char expected) {
    vp_json_skip_space(reader);
    if (reader->position >= reader->length ||
        reader->text[reader->position] != expected) {
        return vp_json_fail(reader, "expected '%c'", expected);
    }
    ++reader->position;
    return 0;
}

static int vp_json_hex_digit(char value) {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

static int vp_json_parse_hex4(vp_json_reader_t *reader, uint32_t *value) {
    uint32_t result = 0;
    int i;

    if (reader->length - reader->position < 4) {
        return vp_json_fail(reader, "incomplete Unicode escape");
    }
    for (i = 0; i < 4; ++i) {
        int digit = vp_json_hex_digit(reader->text[reader->position++]);

        if (digit < 0) {
            return vp_json_fail(reader, "invalid Unicode escape");
        }
        result = (result << 4) | (uint32_t)digit;
    }
    *value = result;
    return 0;
}

static int vp_json_append_utf8(vp_json_reader_t *reader,
                               uint32_t codepoint,
                               char *output,
                               size_t output_size,
                               size_t *output_length) {
    unsigned char encoded[4];
    size_t encoded_size;
    size_t i;

    if (codepoint <= 0x7fU) {
        encoded[0] = (unsigned char)codepoint;
        encoded_size = 1;
    } else if (codepoint <= 0x7ffU) {
        encoded[0] = (unsigned char)(0xc0U | (codepoint >> 6));
        encoded[1] = (unsigned char)(0x80U | (codepoint & 0x3fU));
        encoded_size = 2;
    } else if (codepoint <= 0xffffU) {
        encoded[0] = (unsigned char)(0xe0U | (codepoint >> 12));
        encoded[1] = (unsigned char)(0x80U | ((codepoint >> 6) & 0x3fU));
        encoded[2] = (unsigned char)(0x80U | (codepoint & 0x3fU));
        encoded_size = 3;
    } else if (codepoint <= 0x10ffffU) {
        encoded[0] = (unsigned char)(0xf0U | (codepoint >> 18));
        encoded[1] = (unsigned char)(0x80U | ((codepoint >> 12) & 0x3fU));
        encoded[2] = (unsigned char)(0x80U | ((codepoint >> 6) & 0x3fU));
        encoded[3] = (unsigned char)(0x80U | (codepoint & 0x3fU));
        encoded_size = 4;
    } else {
        return vp_json_fail(reader, "invalid Unicode code point");
    }
    if (*output_length + encoded_size >= output_size) {
        return vp_json_fail(reader, "string is too long");
    }
    for (i = 0; i < encoded_size; ++i) {
        output[(*output_length)++] = (char)encoded[i];
    }
    return 0;
}

static int vp_json_parse_string(vp_json_reader_t *reader,
                                char *output,
                                size_t output_size) {
    size_t output_length = 0;

    if (output == NULL || output_size == 0) {
        return vp_json_fail(reader, "invalid string destination");
    }
    vp_json_skip_space(reader);
    if (reader->position >= reader->length ||
        reader->text[reader->position] != '"') {
        return vp_json_fail(reader, "expected string");
    }
    ++reader->position;

    while (reader->position < reader->length) {
        unsigned char value = (unsigned char)reader->text[reader->position++];

        if (value == '"') {
            output[output_length] = '\0';
            return 0;
        }
        if (value < 0x20U) {
            return vp_json_fail(reader, "control character in string");
        }
        if (value == '\\') {
            uint32_t codepoint;

            if (reader->position >= reader->length) {
                return vp_json_fail(reader, "incomplete string escape");
            }
            value = (unsigned char)reader->text[reader->position++];
            switch (value) {
                case '"': value = '"'; break;
                case '\\': value = '\\'; break;
                case '/': value = '/'; break;
                case 'b': value = '\b'; break;
                case 'f': value = '\f'; break;
                case 'n': value = '\n'; break;
                case 'r': value = '\r'; break;
                case 't': value = '\t'; break;
                case 'u':
                    if (vp_json_parse_hex4(reader, &codepoint) != 0) {
                        return -1;
                    }
                    if (codepoint >= 0xd800U && codepoint <= 0xdbffU) {
                        uint32_t low;

                        if (reader->length - reader->position < 6 ||
                            reader->text[reader->position] != '\\' ||
                            reader->text[reader->position + 1] != 'u') {
                            return vp_json_fail(reader,
                                                "missing low Unicode surrogate");
                        }
                        reader->position += 2;
                        if (vp_json_parse_hex4(reader, &low) != 0) {
                            return -1;
                        }
                        if (low < 0xdc00U || low > 0xdfffU) {
                            return vp_json_fail(reader,
                                                "invalid low Unicode surrogate");
                        }
                        codepoint = 0x10000U +
                                    ((codepoint - 0xd800U) << 10) +
                                    (low - 0xdc00U);
                    } else if (codepoint >= 0xdc00U && codepoint <= 0xdfffU) {
                        return vp_json_fail(reader,
                                            "unexpected low Unicode surrogate");
                    }
                    if (vp_json_append_utf8(reader,
                                            codepoint,
                                            output,
                                            output_size,
                                            &output_length) != 0) {
                        return -1;
                    }
                    continue;
                default:
                    return vp_json_fail(reader, "invalid string escape");
            }
        }
        if (output_length + 1 >= output_size) {
            return vp_json_fail(reader, "string is too long");
        }
        output[output_length++] = (char)value;
    }
    return vp_json_fail(reader, "unterminated string");
}

static int vp_json_match_literal(vp_json_reader_t *reader,
                                 const char *literal) {
    size_t size = strlen(literal);

    vp_json_skip_space(reader);
    if (reader->length - reader->position < size ||
        memcmp(reader->text + reader->position, literal, size) != 0) {
        return 0;
    }
    reader->position += size;
    return 1;
}

static int vp_json_parse_int(vp_json_reader_t *reader, int *output) {
    const char *start;
    char *end;
    long value;

    vp_json_skip_space(reader);
    if (reader->position >= reader->length) {
        return vp_json_fail(reader, "expected integer");
    }
    start = reader->text + reader->position;
    if (reader->text[reader->position] == '-') {
        ++reader->position;
    }
    if (reader->position >= reader->length ||
        !isdigit((unsigned char)reader->text[reader->position])) {
        return vp_json_fail(reader, "invalid integer");
    }
    if (reader->text[reader->position] == '0') {
        ++reader->position;
        if (reader->position < reader->length &&
            isdigit((unsigned char)reader->text[reader->position])) {
            return vp_json_fail(reader, "integer cannot contain a leading zero");
        }
    } else {
        while (reader->position < reader->length &&
               isdigit((unsigned char)reader->text[reader->position])) {
            ++reader->position;
        }
    }
    if (reader->position < reader->length &&
        (reader->text[reader->position] == '.' ||
         reader->text[reader->position] == 'e' ||
         reader->text[reader->position] == 'E')) {
        return vp_json_fail(reader, "integer field cannot contain a fraction");
    }
    errno = 0;
    value = strtol(start, &end, 10);
    if (end != reader->text + reader->position || errno == ERANGE ||
        value < INT_MIN || value > INT_MAX) {
        return vp_json_fail(reader, "invalid integer");
    }
    *output = (int)value;
    return 0;
}

static int vp_json_parse_bool_or_int(vp_json_reader_t *reader, int *output) {
    if (vp_json_match_literal(reader, "true")) {
        *output = 1;
        return 0;
    }
    if (vp_json_match_literal(reader, "false")) {
        *output = 0;
        return 0;
    }
    if (vp_json_parse_int(reader, output) != 0) {
        return -1;
    }
    if (*output != 0 && *output != 1) {
        return vp_json_fail(reader, "boolean field must be true, false, 0, or 1");
    }
    return 0;
}

static int vp_json_skip_value(vp_json_reader_t *reader);

static int vp_json_skip_object(vp_json_reader_t *reader) {
    char key[64];

    if (vp_json_consume(reader, '{') != 0) {
        return -1;
    }
    vp_json_skip_space(reader);
    if (reader->position < reader->length &&
        reader->text[reader->position] == '}') {
        ++reader->position;
        return 0;
    }
    for (;;) {
        if (vp_json_parse_string(reader, key, sizeof(key)) != 0 ||
            vp_json_consume(reader, ':') != 0 ||
            vp_json_skip_value(reader) != 0) {
            return -1;
        }
        vp_json_skip_space(reader);
        if (reader->position < reader->length &&
            reader->text[reader->position] == '}') {
            ++reader->position;
            return 0;
        }
        if (vp_json_consume(reader, ',') != 0) {
            return -1;
        }
    }
}

static int vp_json_skip_array(vp_json_reader_t *reader) {
    if (vp_json_consume(reader, '[') != 0) {
        return -1;
    }
    vp_json_skip_space(reader);
    if (reader->position < reader->length &&
        reader->text[reader->position] == ']') {
        ++reader->position;
        return 0;
    }
    for (;;) {
        if (vp_json_skip_value(reader) != 0) {
            return -1;
        }
        vp_json_skip_space(reader);
        if (reader->position < reader->length &&
            reader->text[reader->position] == ']') {
            ++reader->position;
            return 0;
        }
        if (vp_json_consume(reader, ',') != 0) {
            return -1;
        }
    }
}

static int vp_json_skip_number(vp_json_reader_t *reader) {
    int digits = 0;

    vp_json_skip_space(reader);
    if (reader->position < reader->length &&
        reader->text[reader->position] == '-') {
        ++reader->position;
    }
    if (reader->position >= reader->length ||
        !isdigit((unsigned char)reader->text[reader->position])) {
        return vp_json_fail(reader, "invalid JSON number");
    }
    if (reader->text[reader->position] == '0') {
        ++reader->position;
        if (reader->position < reader->length &&
            isdigit((unsigned char)reader->text[reader->position])) {
            return vp_json_fail(reader, "JSON number contains a leading zero");
        }
    } else {
        while (reader->position < reader->length &&
               isdigit((unsigned char)reader->text[reader->position])) {
            ++reader->position;
        }
    }
    if (reader->position < reader->length &&
        reader->text[reader->position] == '.') {
        ++reader->position;
        digits = 0;
        while (reader->position < reader->length &&
               isdigit((unsigned char)reader->text[reader->position])) {
            ++reader->position;
            digits = 1;
        }
        if (!digits) {
            return vp_json_fail(reader, "JSON fraction is missing digits");
        }
    }
    if (reader->position < reader->length &&
        (reader->text[reader->position] == 'e' ||
         reader->text[reader->position] == 'E')) {
        ++reader->position;
        if (reader->position < reader->length &&
            (reader->text[reader->position] == '+' ||
             reader->text[reader->position] == '-')) {
            ++reader->position;
        }
        digits = 0;
        while (reader->position < reader->length &&
               isdigit((unsigned char)reader->text[reader->position])) {
            ++reader->position;
            digits = 1;
        }
        if (!digits) {
            return vp_json_fail(reader, "JSON exponent is missing digits");
        }
    }
    return 0;
}

static int vp_json_skip_value(vp_json_reader_t *reader) {
    vp_json_skip_space(reader);
    if (reader->position >= reader->length) {
        return vp_json_fail(reader, "expected JSON value");
    }
    switch (reader->text[reader->position]) {
        case '{': return vp_json_skip_object(reader);
        case '[': return vp_json_skip_array(reader);
        case '"': {
            char ignored[APP_PATH_MAX_LEN];
            return vp_json_parse_string(reader, ignored, sizeof(ignored));
        }
        case 't':
            return vp_json_match_literal(reader, "true")
                       ? 0
                       : vp_json_fail(reader, "invalid JSON value");
        case 'f':
            return vp_json_match_literal(reader, "false")
                       ? 0
                       : vp_json_fail(reader, "invalid JSON value");
        case 'n':
            return vp_json_match_literal(reader, "null")
                       ? 0
                       : vp_json_fail(reader, "invalid JSON value");
        default:
            return vp_json_skip_number(reader);
    }
}

static int vp_json_parse_source_type(vp_json_reader_t *reader,
                                     video_source_type_t *source_type) {
    char value[32];

    if (vp_json_parse_string(reader, value, sizeof(value)) != 0) {
        return -1;
    }
    if (strcmp(value, "usb") == 0) {
        *source_type = VIDEO_SOURCE_USB;
    } else if (strcmp(value, "csi0") == 0) {
        *source_type = VIDEO_SOURCE_CSI0;
    } else if (strcmp(value, "csi1") == 0) {
        *source_type = VIDEO_SOURCE_CSI1;
    } else if (strcmp(value, "hdmi_in") == 0 || strcmp(value, "hdmi") == 0) {
        *source_type = VIDEO_SOURCE_HDMI_IN;
    } else {
        return vp_json_fail(reader,
                            "unsupported source_type '%s'",
                            value);
    }
    return 0;
}

static int vp_json_parse_input(vp_json_reader_t *reader,
                               video_input_config_t *input) {
    enum {
        INPUT_NAME = 1U << 0,
        INPUT_DEVICE = 1U << 1,
        INPUT_FORMAT = 1U << 2,
        INPUT_WIDTH = 1U << 3,
        INPUT_HEIGHT = 1U << 4,
        INPUT_FPS = 1U << 5,
        INPUT_SOURCE_TYPE = 1U << 6,
        INPUT_NATIVE_V4L2 = 1U << 7,
        INPUT_ALL = (1U << 8) - 1U
    };
    unsigned int fields = 0;
    char key[64];

    if (vp_json_consume(reader, '{') != 0) {
        return -1;
    }
    for (;;) {
        vp_json_skip_space(reader);
        if (reader->position < reader->length &&
            reader->text[reader->position] == '}') {
            ++reader->position;
            break;
        }
        if (vp_json_parse_string(reader, key, sizeof(key)) != 0 ||
            vp_json_consume(reader, ':') != 0) {
            return -1;
        }
        if (strcmp(key, "name") == 0) {
            fields |= INPUT_NAME;
            if (vp_json_parse_string(reader, input->name, sizeof(input->name)) != 0) return -1;
        } else if (strcmp(key, "device") == 0) {
            fields |= INPUT_DEVICE;
            if (vp_json_parse_string(reader, input->device, sizeof(input->device)) != 0) return -1;
        } else if (strcmp(key, "input_format") == 0) {
            fields |= INPUT_FORMAT;
            if (vp_json_parse_string(reader, input->input_format, sizeof(input->input_format)) != 0) return -1;
        } else if (strcmp(key, "width") == 0) {
            fields |= INPUT_WIDTH;
            if (vp_json_parse_int(reader, &input->width) != 0) return -1;
        } else if (strcmp(key, "height") == 0) {
            fields |= INPUT_HEIGHT;
            if (vp_json_parse_int(reader, &input->height) != 0) return -1;
        } else if (strcmp(key, "fps") == 0) {
            fields |= INPUT_FPS;
            if (vp_json_parse_int(reader, &input->fps) != 0) return -1;
        } else if (strcmp(key, "source_type") == 0) {
            fields |= INPUT_SOURCE_TYPE;
            if (vp_json_parse_source_type(reader, &input->source_type) != 0) return -1;
        } else if (strcmp(key, "use_native_v4l2") == 0) {
            fields |= INPUT_NATIVE_V4L2;
            if (vp_json_parse_bool_or_int(reader, &input->use_native_v4l2) != 0) return -1;
        } else if (vp_json_skip_value(reader) != 0) {
            return -1;
        }
        vp_json_skip_space(reader);
        if (reader->position < reader->length &&
            reader->text[reader->position] == '}') {
            ++reader->position;
            break;
        }
        if (vp_json_consume(reader, ',') != 0) {
            return -1;
        }
    }
    if (fields != INPUT_ALL) {
        return vp_json_fail(reader, "input object is missing required fields");
    }
    if (input->name[0] == '\0' || input->device[0] == '\0' ||
        input->width < 0 || input->height < 0 || input->fps < 0) {
        return vp_json_fail(reader, "invalid input channel values");
    }
    return 0;
}

static int vp_json_parse_record_output(vp_json_reader_t *reader,
                                       video_encode_params_t *output) {
    enum {
        OUTPUT_WIDTH = 1U << 0,
        OUTPUT_HEIGHT = 1U << 1,
        OUTPUT_FPS = 1U << 2,
        OUTPUT_BITRATE = 1U << 3,
        OUTPUT_ALL = (1U << 4) - 1U
    };
    unsigned int fields = 0;
    char key[64];

    if (vp_json_consume(reader, '{') != 0) {
        return -1;
    }
    for (;;) {
        vp_json_skip_space(reader);
        if (reader->position < reader->length &&
            reader->text[reader->position] == '}') {
            ++reader->position;
            break;
        }
        if (vp_json_parse_string(reader, key, sizeof(key)) != 0 ||
            vp_json_consume(reader, ':') != 0) {
            return -1;
        }
        if (strcmp(key, "width") == 0) {
            fields |= OUTPUT_WIDTH;
            if (vp_json_parse_int(reader, &output->width) != 0) return -1;
        } else if (strcmp(key, "height") == 0) {
            fields |= OUTPUT_HEIGHT;
            if (vp_json_parse_int(reader, &output->height) != 0) return -1;
        } else if (strcmp(key, "fps") == 0) {
            fields |= OUTPUT_FPS;
            if (vp_json_parse_int(reader, &output->fps) != 0) return -1;
        } else if (strcmp(key, "bitrate") == 0) {
            fields |= OUTPUT_BITRATE;
            if (vp_json_parse_int(reader, &output->bitrate) != 0) return -1;
        } else if (vp_json_skip_value(reader) != 0) {
            return -1;
        }
        vp_json_skip_space(reader);
        if (reader->position < reader->length &&
            reader->text[reader->position] == '}') {
            ++reader->position;
            break;
        }
        if (vp_json_consume(reader, ',') != 0) {
            return -1;
        }
    }
    if (fields != OUTPUT_ALL) {
        return vp_json_fail(reader,
                            "record_output object is missing required fields");
    }
    if (output->width < 0 || output->height < 0) {
        return vp_json_fail(reader, "invalid record_output values");
    }
    return 0;
}

static int vp_json_parse_nullable_string(vp_json_reader_t *reader,
                                         char *output,
                                         size_t output_size,
                                         const char **pointer) {
    if (vp_json_match_literal(reader, "null")) {
        output[0] = '\0';
        *pointer = NULL;
        return 0;
    }
    if (vp_json_parse_string(reader, output, output_size) != 0) {
        return -1;
    }
    *pointer = output;
    return 0;
}

static int vp_json_parse_channel(vp_json_reader_t *reader,
                                 vp_main_channels_config_t *config,
                                 int index) {
    enum {
        CHANNEL_INPUT = 1U << 0,
        CHANNEL_RECORD_OUTPUT = 1U << 1,
        CHANNEL_RECORD_DIR = 1U << 2,
        CHANNEL_SNAPSHOT_DIR = 1U << 3,
        CHANNEL_OSD_LABEL = 1U << 4,
        CHANNEL_ENABLED = 1U << 5,
        CHANNEL_ALL = (1U << 5) - 1U
    };
    vp_channel_config_t *channel = &config->channels[index];
    unsigned int fields = 0;
    char key[64];

    channel->enabled = 1;

    if (vp_json_consume(reader, '{') != 0) {
        return -1;
    }
    for (;;) {
        vp_json_skip_space(reader);
        if (reader->position < reader->length &&
            reader->text[reader->position] == '}') {
            ++reader->position;
            break;
        }
        if (vp_json_parse_string(reader, key, sizeof(key)) != 0 ||
            vp_json_consume(reader, ':') != 0) {
            return -1;
        }
        if (strcmp(key, "input") == 0) {
            fields |= CHANNEL_INPUT;
            if (vp_json_parse_input(reader, &channel->input) != 0) return -1;
        } else if (strcmp(key, "record_output") == 0) {
            fields |= CHANNEL_RECORD_OUTPUT;
            if (vp_json_parse_record_output(reader, &channel->record_output) != 0) return -1;
        } else if (strcmp(key, "record_dir") == 0) {
            fields |= CHANNEL_RECORD_DIR;
            if (vp_json_parse_nullable_string(reader,
                                              config->record_dirs[index],
                                              sizeof(config->record_dirs[index]),
                                              &channel->record_dir) != 0) return -1;
        } else if (strcmp(key, "snapshot_dir") == 0) {
            fields |= CHANNEL_SNAPSHOT_DIR;
            if (vp_json_parse_nullable_string(reader,
                                              config->snapshot_dirs[index],
                                              sizeof(config->snapshot_dirs[index]),
                                              &channel->snapshot_dir) != 0) return -1;
        } else if (strcmp(key, "osd_label") == 0) {
            fields |= CHANNEL_OSD_LABEL;
            if (vp_json_parse_nullable_string(reader,
                                              config->osd_labels[index],
                                              sizeof(config->osd_labels[index]),
                                              &channel->osd_label) != 0) return -1;
        } else if (strcmp(key, "enabled") == 0) {
            fields |= CHANNEL_ENABLED;
            if (vp_json_parse_bool_or_int(reader, &channel->enabled) != 0) return -1;
        } else if (vp_json_skip_value(reader) != 0) {
            return -1;
        }
        vp_json_skip_space(reader);
        if (reader->position < reader->length &&
            reader->text[reader->position] == '}') {
            ++reader->position;
            break;
        }
        if (vp_json_consume(reader, ',') != 0) {
            return -1;
        }
    }
    if ((fields & CHANNEL_ALL) != CHANNEL_ALL) {
        return vp_json_fail(reader, "channel object is missing required fields");
    }
    return 0;
}

static int vp_json_parse_channels(vp_json_reader_t *reader,
                                  vp_main_channels_config_t *config) {
    if (vp_json_consume(reader, '[') != 0) {
        return -1;
    }
    vp_json_skip_space(reader);
    if (reader->position < reader->length &&
        reader->text[reader->position] == ']') {
        return vp_json_fail(reader, "channels array cannot be empty");
    }
    for (;;) {
        if (config->channel_count >= VP_MAIN_MAX_CHANNELS) {
            return vp_json_fail(reader,
                                "channels exceeds limit %d",
                                VP_MAIN_MAX_CHANNELS);
        }
        if (vp_json_parse_channel(reader, config, config->channel_count) != 0) {
            return -1;
        }
        if (config->channels[config->channel_count].enabled) {
            ++config->channel_count;
        }
        vp_json_skip_space(reader);
        if (reader->position < reader->length &&
            reader->text[reader->position] == ']') {
            ++reader->position;
            return 0;
        }
        if (vp_json_consume(reader, ',') != 0) {
            return -1;
        }
    }
}

static int vp_json_parse_uint(vp_json_reader_t *reader, uint32_t *output) {
    int value;

    if (vp_json_parse_int(reader, &value) != 0) {
        return -1;
    }
    if (value <= 0) {
        return vp_json_fail(reader, "value must be a positive integer");
    }
    *output = (uint32_t)value;
    return 0;
}

static int vp_json_parse_serial(vp_json_reader_t *reader,
                                vp_main_serial_config_t *serial) {
    char key[64];

    if (vp_json_consume(reader, '{') != 0) {
        return -1;
    }
    for (;;) {
        vp_json_skip_space(reader);
        if (reader->position < reader->length &&
            reader->text[reader->position] == '}') {
            ++reader->position;
            return 0;
        }
        if (vp_json_parse_string(reader, key, sizeof(key)) != 0 ||
            vp_json_consume(reader, ':') != 0) {
            return -1;
        }
        if (strcmp(key, "sbus_device") == 0) {
            if (vp_json_parse_string(reader, serial->sbus_device, sizeof(serial->sbus_device)) != 0) return -1;
        } else if (strcmp(key, "high_speed_camera_device") == 0) {
            if (vp_json_parse_string(reader, serial->high_speed_camera_device, sizeof(serial->high_speed_camera_device)) != 0) return -1;
        } else if (strcmp(key, "high_speed_camera_baudrate") == 0) {
            if (vp_json_parse_uint(reader, &serial->high_speed_camera_baudrate) != 0) return -1;
        } else if (strcmp(key, "thermal_camera_device") == 0) {
            if (vp_json_parse_string(reader, serial->thermal_camera_device, sizeof(serial->thermal_camera_device)) != 0) return -1;
        } else if (strcmp(key, "thermal_camera_baudrate") == 0) {
            if (vp_json_parse_uint(reader, &serial->thermal_camera_baudrate) != 0) return -1;
        } else if (strcmp(key, "laser_device") == 0) {
            if (vp_json_parse_string(reader, serial->laser_device, sizeof(serial->laser_device)) != 0) return -1;
        } else if (strcmp(key, "laser_baudrate") == 0) {
            if (vp_json_parse_uint(reader, &serial->laser_baudrate) != 0) return -1;
        } else if (strcmp(key, "visca_4k_camera_device") == 0) {
            if (vp_json_parse_string(reader, serial->visca_4k_camera_device, sizeof(serial->visca_4k_camera_device)) != 0) return -1;
        } else if (strcmp(key, "visca_4k_camera_baudrate") == 0) {
            if (vp_json_parse_uint(reader, &serial->visca_4k_camera_baudrate) != 0) return -1;
        } else if (strcmp(key, "motor_device") == 0) {
            if (vp_json_parse_string(reader, serial->motor_device, sizeof(serial->motor_device)) != 0) return -1;
        } else if (strcmp(key, "motor_baudrate") == 0) {
            if (vp_json_parse_uint(reader, &serial->motor_baudrate) != 0) return -1;
        } else if (strcmp(key, "motor_slave_addr") == 0) {
            if (vp_json_parse_uint(reader, &serial->motor_slave_addr) != 0) return -1;
        } else if (strcmp(key, "mavlink_config_path") == 0) {
            if (vp_json_parse_string(reader, serial->mavlink_config_path, sizeof(serial->mavlink_config_path)) != 0) return -1;
        } else if (vp_json_skip_value(reader) != 0) {
            return -1;
        }
        vp_json_skip_space(reader);
        if (reader->position < reader->length &&
            reader->text[reader->position] == '}') {
            ++reader->position;
            return 0;
        }
        if (vp_json_consume(reader, ',') != 0) {
            return -1;
        }
    }
}

static int vp_json_parse_root(vp_json_reader_t *reader,
                              vp_main_channels_config_t *config) {
    int channels_found = 0;
    int storage_dir_found = 0;
    int serial_found = 0;
    const char *storage_dir = NULL;
    char key[64];

    if (vp_json_consume(reader, '{') != 0) {
        return -1;
    }
    for (;;) {
        vp_json_skip_space(reader);
        if (reader->position < reader->length &&
            reader->text[reader->position] == '}') {
            ++reader->position;
            break;
        }
        if (vp_json_parse_string(reader, key, sizeof(key)) != 0 ||
            vp_json_consume(reader, ':') != 0) {
            return -1;
        }
        if (strcmp(key, "channels") == 0) {
            if (channels_found) {
                return vp_json_fail(reader, "duplicate channels field");
            }
            channels_found = 1;
            if (vp_json_parse_channels(reader, config) != 0) {
                return -1;
            }
        } else if (strcmp(key, "storage_dir") == 0) {
            if (storage_dir_found) {
                return vp_json_fail(reader, "duplicate storage_dir field");
            }
            storage_dir_found = 1;
            if (vp_json_parse_nullable_string(reader,
                                              config->storage_dir,
                                              sizeof(config->storage_dir),
                                              &storage_dir) != 0) {
                return -1;
            }
        } else if (strcmp(key, "serial") == 0) {
            if (serial_found) {
                return vp_json_fail(reader, "duplicate serial field");
            }
            serial_found = 1;
            if (vp_json_parse_serial(reader, &config->serial) != 0) {
                return -1;
            }
        } else if (vp_json_skip_value(reader) != 0) {
            return -1;
        }
        vp_json_skip_space(reader);
        if (reader->position < reader->length &&
            reader->text[reader->position] == '}') {
            ++reader->position;
            break;
        }
        if (vp_json_consume(reader, ',') != 0) {
            return -1;
        }
    }
    if (!channels_found) {
        return vp_json_fail(reader, "root object is missing channels");
    }
    return 0;
}

static int vp_main_validate_channel_names(vp_json_reader_t *reader,
                                          const vp_main_channels_config_t *config) {
    int i;
    int j;

    for (i = 0; i < config->channel_count; ++i) {
        for (j = i + 1; j < config->channel_count; ++j) {
            if (strcmp(config->channels[i].input.name,
                       config->channels[j].input.name) == 0) {
                return vp_json_fail(reader,
                                    "duplicate channel name '%s'",
                                    config->channels[i].input.name);
            }
        }
    }
    return 0;
}

int vp_main_channels_config_load_json(vp_main_channels_config_t *config,
                                      const char *path,
                                      char *error,
                                      size_t error_size) {
    vp_json_reader_t reader;
    FILE *file = NULL;
    char *text = NULL;
    long file_size;
    size_t bytes_read;
    int result = -1;

    if (error != NULL && error_size > 0) {
        error[0] = '\0';
    }
    if (config == NULL || path == NULL || path[0] == '\0') {
        if (error != NULL && error_size > 0) {
            snprintf(error, error_size, "invalid config path");
        }
        return -1;
    }
    memset(config, 0, sizeof(*config));
    snprintf(config->serial.sbus_device, sizeof(config->serial.sbus_device), "/dev/ttyACM1");
    snprintf(config->serial.high_speed_camera_device, sizeof(config->serial.high_speed_camera_device), "/dev/ttyACM4");
    config->serial.high_speed_camera_baudrate = 115200U;
    snprintf(config->serial.thermal_camera_device, sizeof(config->serial.thermal_camera_device), "/dev/ttyS10");
    config->serial.thermal_camera_baudrate = 115200U;
    snprintf(config->serial.laser_device, sizeof(config->serial.laser_device), "/dev/ttyS9");
    config->serial.laser_baudrate = 115200U;
    snprintf(config->serial.visca_4k_camera_device, sizeof(config->serial.visca_4k_camera_device), "/dev/ttyACM1");
    config->serial.visca_4k_camera_baudrate = 9600U;
    snprintf(config->serial.motor_device, sizeof(config->serial.motor_device), "/dev/ttyS4");
    config->serial.motor_baudrate = 115200U;
    config->serial.motor_slave_addr = 1U;
    snprintf(config->serial.mavlink_config_path, sizeof(config->serial.mavlink_config_path), "/home/cat/rk3588/config/mavlink_control.conf");

    file = fopen(path, "rb");
    if (file == NULL) {
        if (error != NULL && error_size > 0) {
            snprintf(error, error_size, "open %s failed: %s", path, strerror(errno));
        }
        goto cleanup;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (file_size = ftell(file)) < 0 ||
        fseek(file, 0, SEEK_SET) != 0) {
        if (error != NULL && error_size > 0) {
            snprintf(error, error_size, "read %s size failed", path);
        }
        goto cleanup;
    }
    if (file_size == 0 || (unsigned long)file_size > VP_MAIN_CONFIG_MAX_BYTES) {
        if (error != NULL && error_size > 0) {
            snprintf(error,
                     error_size,
                     "%s size must be between 1 and %u bytes",
                     path,
                     VP_MAIN_CONFIG_MAX_BYTES);
        }
        goto cleanup;
    }
    text = (char *)malloc((size_t)file_size + 1U);
    if (text == NULL) {
        if (error != NULL && error_size > 0) {
            snprintf(error, error_size, "out of memory reading %s", path);
        }
        goto cleanup;
    }
    bytes_read = fread(text, 1, (size_t)file_size, file);
    if (bytes_read != (size_t)file_size) {
        if (error != NULL && error_size > 0) {
            snprintf(error, error_size, "short read from %s", path);
        }
        goto cleanup;
    }
    text[bytes_read] = '\0';

    memset(&reader, 0, sizeof(reader));
    reader.text = text;
    reader.length = bytes_read;
    reader.error = error;
    reader.error_size = error_size;
    if (reader.length >= 3 &&
        (unsigned char)reader.text[0] == 0xefU &&
        (unsigned char)reader.text[1] == 0xbbU &&
        (unsigned char)reader.text[2] == 0xbfU) {
        reader.position = 3;
    }
    if (vp_json_parse_root(&reader, config) != 0) {
        goto cleanup;
    }
    if (config->channel_count == 0) {
        vp_json_fail(&reader, "at least one channel must be enabled");
        goto cleanup;
    }
    vp_json_skip_space(&reader);
    if (reader.position != reader.length) {
        vp_json_fail(&reader, "unexpected data after root object");
        goto cleanup;
    }
    if (vp_main_validate_channel_names(&reader, config) != 0) {
        goto cleanup;
    }
    result = 0;

cleanup:
    if (file != NULL) {
        fclose(file);
    }
    free(text);
    if (result != 0) {
        memset(config, 0, sizeof(*config));
    }
    return result;
}
