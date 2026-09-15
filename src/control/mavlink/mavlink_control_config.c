#define LOG_LOCAL_LEVEL LOG_LEVEL_DEBUG
#define LOG_FILE_NAME "mavlink_control_config.c"

#include "control/mavlink/mavlink_transport.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/debug.h"

#define MAVLINK_CONTROL_LINE_MAX 256

static char *mavlink_control_trim(char *value) {
    char *end;

    while (*value == ' ' || *value == '\t' || *value == '\r' || *value == '\n') {
        ++value;
    }
    end = value + strlen(value);
    while (end > value &&
           (end[-1] == ' ' || end[-1] == '\t' ||
            end[-1] == '\r' || end[-1] == '\n')) {
        --end;
    }
    *end = '\0';
    return value;
}

static int mavlink_control_parse_int(
    const char *value,
    int minimum,
    int maximum,
    int *out_value) {
    char *end = NULL;
    long parsed;

    errno = 0;
    parsed = strtol(value, &end, 10);
    if (errno != 0 || end == value || *mavlink_control_trim(end) != '\0' ||
        parsed < minimum || parsed > maximum) {
        return -1;
    }
    *out_value = (int)parsed;
    return 0;
}

static int mavlink_control_copy_string(
    char *destination,
    size_t destination_size,
    const char *value) {
    int written;

    if (destination == NULL || destination_size == 0U || value == NULL) {
        return -1;
    }
    written = snprintf(destination, destination_size, "%s", value);
    return written >= 0 && (size_t)written < destination_size ? 0 : -1;
}

static void mavlink_control_config_defaults(mavlink_control_config_t *config) {
    memset(config, 0, sizeof(*config));
    config->enabled = 1;
    config->debug = 1;
    config->transport = MAVLINK_CONTROL_TRANSPORT_UDP;
    config->system_id = 1U;
    config->component_id = 191U;
    config->udp_port = 14550;
    config->tcp_mode = MAVLINK_CONTROL_TCP_SERVER;
    config->tcp_port = 5760;
    config->serial_baud = 115200;
    (void)mavlink_control_copy_string(
        config->udp_bind_ip,
        sizeof(config->udp_bind_ip),
        "0.0.0.0");
    (void)mavlink_control_copy_string(
        config->tcp_host,
        sizeof(config->tcp_host),
        "0.0.0.0");
    (void)mavlink_control_copy_string(
        config->serial_device,
        sizeof(config->serial_device),
        "/dev/ttyUSB0");
}

static int mavlink_control_config_set(
    mavlink_control_config_t *config,
    const char *key,
    const char *value) {
    int parsed;

    if (strcmp(key, "enabled") == 0) {
        return mavlink_control_parse_int(value, 0, 1, &config->enabled);
    }
    if (strcmp(key, "debug") == 0) {
        return mavlink_control_parse_int(value, 0, 1, &config->debug);
    }
    if (strcmp(key, "transport") == 0) {
        if (strcmp(value, "udp") == 0) {
            config->transport = MAVLINK_CONTROL_TRANSPORT_UDP;
            return 0;
        }
        if (strcmp(value, "tcp") == 0) {
            config->transport = MAVLINK_CONTROL_TRANSPORT_TCP;
            return 0;
        }
        if (strcmp(value, "serial") == 0) {
            config->transport = MAVLINK_CONTROL_TRANSPORT_SERIAL;
            return 0;
        }
        return -1;
    }
    if (strcmp(key, "system_id") == 0) {
        if (mavlink_control_parse_int(value, 1, 255, &parsed) != 0) {
            return -1;
        }
        config->system_id = (uint8_t)parsed;
        return 0;
    }
    if (strcmp(key, "component_id") == 0) {
        if (mavlink_control_parse_int(value, 1, 255, &parsed) != 0) {
            return -1;
        }
        config->component_id = (uint8_t)parsed;
        return 0;
    }
    if (strcmp(key, "udp_bind_ip") == 0) {
        return mavlink_control_copy_string(
            config->udp_bind_ip,
            sizeof(config->udp_bind_ip),
            value);
    }
    if (strcmp(key, "udp_port") == 0) {
        return mavlink_control_parse_int(value, 1, 65535, &config->udp_port);
    }
    if (strcmp(key, "tcp_mode") == 0) {
        if (strcmp(value, "server") == 0) {
            config->tcp_mode = MAVLINK_CONTROL_TCP_SERVER;
            return 0;
        }
        if (strcmp(value, "client") == 0) {
            config->tcp_mode = MAVLINK_CONTROL_TCP_CLIENT;
            return 0;
        }
        return -1;
    }
    if (strcmp(key, "tcp_host") == 0) {
        return mavlink_control_copy_string(
            config->tcp_host,
            sizeof(config->tcp_host),
            value);
    }
    if (strcmp(key, "tcp_port") == 0) {
        return mavlink_control_parse_int(value, 1, 65535, &config->tcp_port);
    }
    if (strcmp(key, "serial_device") == 0) {
        return mavlink_control_copy_string(
            config->serial_device,
            sizeof(config->serial_device),
            value);
    }
    if (strcmp(key, "serial_baud") == 0) {
        return mavlink_control_parse_int(value, 1200, 4000000, &config->serial_baud);
    }

    LOGW("unknown MAVLink control config key ignored: %s", key);
    return 0;
}

int mavlink_control_config_load(
    mavlink_control_config_t *config,
    const char *config_path) {
    FILE *file;
    char line[MAVLINK_CONTROL_LINE_MAX];
    unsigned int line_number = 0U;

    if (config == NULL || config_path == NULL) {
        return -1;
    }
    mavlink_control_config_defaults(config);
    file = fopen(config_path, "r");
    if (file == NULL) {
        if (errno == ENOENT) {
            LOGW("MAVLink config %s not found; using defaults", config_path);
            return 0;
        }
        LOGE("MAVLink config open failed: %s", strerror(errno));
        return -1;
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        char *comment;
        char *separator;
        char *key;
        char *value;

        ++line_number;
        comment = strchr(line, '#');
        if (comment != NULL) {
            *comment = '\0';
        }
        key = mavlink_control_trim(line);
        if (*key == '\0') {
            continue;
        }
        separator = strchr(key, '=');
        if (separator == NULL) {
            LOGE("invalid MAVLink config line %u: missing '='", line_number);
            fclose(file);
            return -1;
        }
        *separator = '\0';
        value = mavlink_control_trim(separator + 1);
        key = mavlink_control_trim(key);
        if (*key == '\0' || *value == '\0' ||
            mavlink_control_config_set(config, key, value) != 0) {
            LOGE("invalid MAVLink config line %u: %s=%s",
                 line_number,
                 key,
                 value);
            fclose(file);
            return -1;
        }
    }

    fclose(file);
    return 0;
}

const char *mavlink_control_transport_name(
    mavlink_control_transport_t transport) {
    switch (transport) {
    case MAVLINK_CONTROL_TRANSPORT_UDP:
        return "udp";
    case MAVLINK_CONTROL_TRANSPORT_TCP:
        return "tcp";
    case MAVLINK_CONTROL_TRANSPORT_SERIAL:
        return "serial";
    default:
        return "unknown";
    }
}
