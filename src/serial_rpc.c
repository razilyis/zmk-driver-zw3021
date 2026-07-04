/*
 * Minimal JSON-line RPC server for managing per-fingerprint-ID output
 * strings (see include/zw3021_storage.h) and triggering enrollment,
 * without ever writing the actual secret strings to git. Shares the same
 * USB CDC-ACM console as logging (the zmk-usb-logging snippet's
 * "snippet_zmk_usb_logging_uart" node) -- so log lines and RPC responses
 * interleave on the same serial stream, matching
 * zmk-module-Fingerprint/src/serial_rpc.c's design. Deliberately not a
 * full JSON parser: request/response fields are flat (no nesting), found
 * by simple substring search, same technique as that reference
 * implementation.
 *
 * Request:  {"cmd":"<name>","req_id":<int>,"finger_id":<int>,"value":"<str>"}
 * Response: {"ok":true,"req_id":<int>,"data":{...}}
 *        or {"ok":false,"req_id":<int>,"message":"..."}
 *
 * Supported cmd values: ping, get_status, get_fingers, get_finger,
 * update_finger, delete_finger, enroll_start, enroll_status.
 */

#include "zw3021.h"
#include "zw3021_storage.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(zw3021_rpc, CONFIG_ZMK_LOG_LEVEL);

#if DT_NODE_EXISTS(DT_NODELABEL(snippet_zmk_usb_logging_uart))

/* Enrollment IDs are only bounded by ZW3021's own template capacity
 * (100 per the datasheet); this is just how far get_fingers scans.
 */
#define ZW3021_RPC_SCAN_MAX_ID 100
/* Worst case: {"cmd":"update_finger","req_id":-2147483648,"finger_id":65535,
 * "value":"<32 chars, fully escaped = 64>"} is ~140 bytes; leave headroom. */
#define ZW3021_RPC_LINE_MAX 176
#define ZW3021_RPC_RESPONSE_MAX 224

static const struct device *rpc_dev;
static char rpc_rx_line[ZW3021_RPC_LINE_MAX];
static uint16_t rpc_rx_pos;
static char rpc_resp_buf[ZW3021_RPC_RESPONSE_MAX];

static void rpc_write(const char *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        uart_poll_out(rpc_dev, data[i]);
    }
}

static void rpc_println(const char *str) {
    rpc_write(str, strlen(str));
    uart_poll_out(rpc_dev, '\n');
}

/* ===== Minimal JSON helpers (flat objects only, no nesting) ===== */

static int json_find_string(const char *json, const char *key, char *out, size_t out_size) {
    char search[32];
    snprintf(search, sizeof(search), "\"%s\":\"", key);

    const char *p = strstr(json, search);
    if (!p) {
        return -1;
    }
    p += strlen(search);

    size_t i = 0;
    while (*p && *p != '"' && i < out_size - 1) {
        if (*p == '\\' && *(p + 1)) {
            p++;
        }
        out[i++] = *p++;
    }
    out[i] = '\0';
    return 0;
}

static int json_find_int(const char *json, const char *key, int def) {
    char search[32];
    snprintf(search, sizeof(search), "\"%s\":", key);

    const char *p = strstr(json, search);
    if (!p) {
        return def;
    }
    p += strlen(search);
    while (*p == ' ') {
        p++;
    }
    if (*p == '-' || (*p >= '0' && *p <= '9')) {
        return atoi(p);
    }
    return def;
}

static void json_escape(const char *in, char *out, size_t out_size) {
    size_t j = 0;
    for (size_t i = 0; in[i] != '\0' && j < out_size - 1; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '\\' || c == '"') {
            if (j + 2 >= out_size) {
                break;
            }
            out[j++] = '\\';
            out[j++] = c;
        } else if (c >= 0x20) {
            out[j++] = c;
        }
    }
    out[j] = '\0';
}

/* ===== Response senders ===== */

static void rpc_send_ok(int req_id, const char *data_json) {
    int pos = snprintf(rpc_resp_buf, sizeof(rpc_resp_buf), "{\"ok\":true,\"req_id\":%d,\"data\":%s}",
                        req_id, data_json ? data_json : "{}");
    if (pos > 0 && (size_t)pos < sizeof(rpc_resp_buf)) {
        rpc_println(rpc_resp_buf);
    }
}

static void rpc_send_error(int req_id, const char *message) {
    int pos = snprintf(rpc_resp_buf, sizeof(rpc_resp_buf), "{\"ok\":false,\"req_id\":%d,\"message\":\"%s\"}",
                        req_id, message);
    if (pos > 0 && (size_t)pos < sizeof(rpc_resp_buf)) {
        rpc_println(rpc_resp_buf);
    }
}

/* ===== Command handlers ===== */

static void cmd_ping(const char *line, int req_id) {
    ARG_UNUSED(line);
    rpc_send_ok(req_id, "{\"pong\":true}");
}

static void cmd_get_status(const char *line, int req_id) {
    ARG_UNUSED(line);
    char data[48];
    snprintf(data, sizeof(data), "{\"busy\":%s}", zw3021_is_busy() ? "true" : "false");
    rpc_send_ok(req_id, data);
}

static void cmd_get_fingers(const char *line, int req_id) {
    ARG_UNUSED(line);
    char data[ZW3021_RPC_RESPONSE_MAX - 32];
    int pos = snprintf(data, sizeof(data), "{\"ids\":[");
    bool first = true;
    char scratch[ZW3021_STORAGE_MAX_LEN + 1];

    for (int id = 1; id <= ZW3021_RPC_SCAN_MAX_ID && (size_t)pos < sizeof(data) - 8; id++) {
        if (zw3021_storage_get((uint16_t)id, scratch, sizeof(scratch)) == 0) {
            pos += snprintf(data + pos, sizeof(data) - pos, "%s%d", first ? "" : ",", id);
            first = false;
        }
    }
    snprintf(data + pos, sizeof(data) - pos, "]}");
    rpc_send_ok(req_id, data);
}

static void cmd_get_finger(const char *line, int req_id) {
    int id = json_find_int(line, "finger_id", -1);
    if (id <= 0) {
        rpc_send_error(req_id, "missing or invalid finger_id");
        return;
    }

    char value[ZW3021_STORAGE_MAX_LEN + 1];
    if (zw3021_storage_get((uint16_t)id, value, sizeof(value)) != 0) {
        rpc_send_error(req_id, "not found");
        return;
    }

    char escaped[ZW3021_STORAGE_MAX_LEN * 2 + 1];
    json_escape(value, escaped, sizeof(escaped));

    char data[ZW3021_RPC_RESPONSE_MAX - 32];
    snprintf(data, sizeof(data), "{\"finger_id\":%d,\"value\":\"%s\"}", id, escaped);
    rpc_send_ok(req_id, data);
}

static void cmd_update_finger(const char *line, int req_id) {
    int id = json_find_int(line, "finger_id", -1);
    char value[ZW3021_STORAGE_MAX_LEN + 1];

    if (id <= 0) {
        rpc_send_error(req_id, "missing or invalid finger_id");
        return;
    }
    if (json_find_string(line, "value", value, sizeof(value)) != 0) {
        rpc_send_error(req_id, "missing value");
        return;
    }

    int ret = zw3021_storage_set((uint16_t)id, value);
    if (ret != 0) {
        rpc_send_error(req_id, "storage write failed");
        return;
    }
    rpc_send_ok(req_id, NULL);
}

static void cmd_delete_finger(const char *line, int req_id) {
    int id = json_find_int(line, "finger_id", -1);
    if (id <= 0) {
        rpc_send_error(req_id, "missing or invalid finger_id");
        return;
    }

    zw3021_storage_delete((uint16_t)id);
    rpc_send_ok(req_id, NULL);
}

static void cmd_enroll_start(const char *line, int req_id) {
    int id = json_find_int(line, "finger_id", -1);
    if (id <= 0) {
        rpc_send_error(req_id, "missing or invalid finger_id");
        return;
    }

    int ret = zw3021_request_enroll((uint16_t)id);
    if (ret != 0) {
        rpc_send_error(req_id, "busy");
        return;
    }
    rpc_send_ok(req_id, NULL);
}

static void cmd_enroll_status(const char *line, int req_id) {
    ARG_UNUSED(line);
    char data[48];
    snprintf(data, sizeof(data), "{\"busy\":%s}", zw3021_is_busy() ? "true" : "false");
    rpc_send_ok(req_id, data);
}

struct rpc_command {
    const char *name;
    void (*handler)(const char *line, int req_id);
};

static const struct rpc_command rpc_commands[] = {
    {"ping", cmd_ping},
    {"get_status", cmd_get_status},
    {"get_fingers", cmd_get_fingers},
    {"get_finger", cmd_get_finger},
    {"update_finger", cmd_update_finger},
    {"delete_finger", cmd_delete_finger},
    {"enroll_start", cmd_enroll_start},
    {"enroll_status", cmd_enroll_status},
};

static void rpc_process_line(const char *line) {
    char cmd[24];
    if (json_find_string(line, "cmd", cmd, sizeof(cmd)) != 0) {
        rpc_send_error(-1, "missing cmd field");
        return;
    }

    int req_id = json_find_int(line, "req_id", -1);

    for (size_t i = 0; i < ARRAY_SIZE(rpc_commands); i++) {
        if (strcmp(cmd, rpc_commands[i].name) == 0) {
            rpc_commands[i].handler(line, req_id);
            return;
        }
    }

    rpc_send_error(req_id, "unknown command");
}

static void rpc_thread(void *p1, void *p2, void *p3) {
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    rpc_dev = DEVICE_DT_GET(DT_NODELABEL(snippet_zmk_usb_logging_uart));
    if (!device_is_ready(rpc_dev)) {
        LOG_ERR("zw3021: RPC console device not ready");
        return;
    }

    LOG_INF("zw3021: serial RPC ready");

    int64_t last_rx_time = k_uptime_get();

    for (;;) {
        uint8_t b;
        bool got_byte = false;
        while (uart_poll_in(rpc_dev, &b) == 0) {
            got_byte = true;
            if (b == '\n' || b == '\r') {
                if (rpc_rx_pos > 0) {
                    rpc_rx_line[rpc_rx_pos] = '\0';
                    rpc_process_line(rpc_rx_line);
                    rpc_rx_pos = 0;
                }
            } else if (rpc_rx_pos < sizeof(rpc_rx_line) - 1) {
                rpc_rx_line[rpc_rx_pos++] = b;
            } else {
                rpc_rx_pos = 0;
                rpc_send_error(-1, "line too long");
            }
        }

        int64_t now = k_uptime_get();
        if (got_byte) {
            last_rx_time = now;
        } else if (rpc_rx_pos > 0 && (now - last_rx_time) > 2000) {
            /* A message sent without a line ending (or cut off mid-send)
             * would otherwise sit here forever, silently corrupting the
             * next real message that arrives. */
            LOG_WRN("zw3021: RPC: discarding stale partial line (no newline received)");
            rpc_rx_pos = 0;
        }

        k_sleep(K_MSEC(20));
    }
}

K_THREAD_DEFINE(zw3021_rpc_tid, 2048, rpc_thread, NULL, NULL, NULL, 10, 0, 0);

#else

#warning "CONFIG_ZW3021_SERIAL_RPC is enabled but the zmk-usb-logging snippet isn't applied to this build; serial RPC will not be available."

#endif /* DT_NODE_EXISTS(DT_NODELABEL(snippet_zmk_usb_logging_uart)) */
