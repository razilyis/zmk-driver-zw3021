/*
 * Transport-agnostic JSON RPC command dispatch for managing
 * per-fingerprint-ID output strings (see include/zw3021_storage.h) and
 * triggering enrollment, without ever writing the actual secret strings
 * to git. Shared by src/serial_rpc.c (USB CDC-ACM) and src/ble_rpc.c
 * (standalone BLE GATT service) -- both just frame incoming bytes into
 * complete JSON lines and call zw3021_rpc_process_line(), which sends its
 * response back out via the tx callback the caller provides. Deliberately
 * not a full JSON parser: request/response fields are flat (no nesting),
 * found by simple substring search, matching zmk-module-Fingerprint's
 * serial_rpc.c design.
 *
 * Request:  {"cmd":"<name>","req_id":<int>,"finger_id":<int>,"value":"<str>","enter":<bool>}
 * Response: {"ok":true,"req_id":<int>,"data":{...}}
 *        or {"ok":false,"req_id":<int>,"message":"..."}
 *
 * Supported cmd values: ping, get_status, get_fingers, get_finger,
 * update_finger, delete_finger, set_finger_enter, enroll_start,
 * enroll_status, refresh_enroll_map, get_enrolled.
 *
 * WRITE-ONLY BY DESIGN: get_finger never returns the stored output
 * string, only whether one exists (data.has_value) and the "send enter"
 * flag (data.enter). Neither this USB console nor the BLE GATT service
 * has any authentication beyond BLE's own pairing/encryption (see
 * ble_rpc.c) -- echoing the raw value back would let anyone who can
 * reach either console read out every stored secret without ever
 * triggering a fingerprint match, defeating the point of gating it
 * behind one. update_finger/set_finger_enter can still write blind.
 */

#include "zw3021.h"
#include "zw3021_rpc_commands.h"
#include "zw3021_storage.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(zw3021_rpc_commands, CONFIG_ZMK_LOG_LEVEL);

/* Enrollment IDs are only bounded by ZW3021's own template capacity
 * (100 per the datasheet); this is just how far get_fingers scans.
 */
#define ZW3021_RPC_SCAN_MAX_ID 100
#define ZW3021_RPC_RESPONSE_MAX 224

static char rpc_resp_buf[ZW3021_RPC_RESPONSE_MAX];

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

static bool json_find_bool(const char *json, const char *key, bool def) {
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
    if (strncmp(p, "true", 4) == 0) {
        return true;
    }
    if (strncmp(p, "false", 5) == 0) {
        return false;
    }
    return def;
}

/* ===== Response senders ===== */

static void rpc_send_ok(int req_id, const char *data_json, zw3021_rpc_tx_fn tx) {
    int pos = snprintf(rpc_resp_buf, sizeof(rpc_resp_buf), "{\"ok\":true,\"req_id\":%d,\"data\":%s}\n",
                        req_id, data_json ? data_json : "{}");
    if (pos > 0 && (size_t)pos < sizeof(rpc_resp_buf)) {
        tx(rpc_resp_buf, (size_t)pos);
    }
}

static void rpc_send_error(int req_id, const char *message, zw3021_rpc_tx_fn tx) {
    int pos = snprintf(rpc_resp_buf, sizeof(rpc_resp_buf), "{\"ok\":false,\"req_id\":%d,\"message\":\"%s\"}\n",
                        req_id, message);
    if (pos > 0 && (size_t)pos < sizeof(rpc_resp_buf)) {
        tx(rpc_resp_buf, (size_t)pos);
    }
}

/* ===== Command handlers ===== */

static void cmd_ping(const char *line, int req_id, zw3021_rpc_tx_fn tx) {
    ARG_UNUSED(line);
    rpc_send_ok(req_id, "{\"pong\":true}", tx);
}

static void cmd_get_status(const char *line, int req_id, zw3021_rpc_tx_fn tx) {
    ARG_UNUSED(line);
    char data[48];
    snprintf(data, sizeof(data), "{\"busy\":%s}", zw3021_is_busy() ? "true" : "false");
    rpc_send_ok(req_id, data, tx);
}

static void cmd_get_fingers(const char *line, int req_id, zw3021_rpc_tx_fn tx) {
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
    rpc_send_ok(req_id, data, tx);
}

static void cmd_get_finger(const char *line, int req_id, zw3021_rpc_tx_fn tx) {
    int id = json_find_int(line, "finger_id", -1);
    if (id <= 0) {
        rpc_send_error(req_id, "missing or invalid finger_id", tx);
        return;
    }

    /* Deliberately does not return the stored value itself -- see the
     * WRITE-ONLY BY DESIGN note at the top of this file. */
    char value[ZW3021_STORAGE_MAX_LEN + 1];
    bool has_value = zw3021_storage_get((uint16_t)id, value, sizeof(value)) == 0;
    bool send_enter = false;
    zw3021_storage_get_enter((uint16_t)id, &send_enter);

    char data[ZW3021_RPC_RESPONSE_MAX - 32];
    snprintf(data, sizeof(data), "{\"finger_id\":%d,\"has_value\":%s,\"enter\":%s}", id,
             has_value ? "true" : "false", send_enter ? "true" : "false");
    rpc_send_ok(req_id, data, tx);
}

static void cmd_set_finger_enter(const char *line, int req_id, zw3021_rpc_tx_fn tx) {
    int id = json_find_int(line, "finger_id", -1);
    if (id <= 0) {
        rpc_send_error(req_id, "missing or invalid finger_id", tx);
        return;
    }
    bool enable = json_find_bool(line, "enter", false);

    int ret = zw3021_storage_set_enter((uint16_t)id, enable);
    if (ret != 0) {
        rpc_send_error(req_id, "storage write failed", tx);
        return;
    }
    rpc_send_ok(req_id, NULL, tx);
}

static void cmd_update_finger(const char *line, int req_id, zw3021_rpc_tx_fn tx) {
    int id = json_find_int(line, "finger_id", -1);
    char value[ZW3021_STORAGE_MAX_LEN + 1];

    if (id <= 0) {
        rpc_send_error(req_id, "missing or invalid finger_id", tx);
        return;
    }
    if (json_find_string(line, "value", value, sizeof(value)) != 0) {
        rpc_send_error(req_id, "missing value", tx);
        return;
    }

    int ret = zw3021_storage_set((uint16_t)id, value);
    if (ret != 0) {
        rpc_send_error(req_id, "storage write failed", tx);
        return;
    }
    rpc_send_ok(req_id, NULL, tx);
}

static void cmd_delete_finger(const char *line, int req_id, zw3021_rpc_tx_fn tx) {
    int id = json_find_int(line, "finger_id", -1);
    if (id <= 0) {
        rpc_send_error(req_id, "missing or invalid finger_id", tx);
        return;
    }

    /* Also clears id's "send enter" flag (zw3021_storage_delete). */
    zw3021_storage_delete((uint16_t)id);
    rpc_send_ok(req_id, NULL, tx);
}

static void cmd_refresh_enroll_map(const char *line, int req_id, zw3021_rpc_tx_fn tx) {
    ARG_UNUSED(line);
    int ret = zw3021_request_read_enrolled();
    if (ret != 0) {
        rpc_send_error(req_id, "busy", tx);
        return;
    }
    rpc_send_ok(req_id, NULL, tx);
}

static void cmd_get_enrolled(const char *line, int req_id, zw3021_rpc_tx_fn tx) {
    int id = json_find_int(line, "finger_id", -1);
    if (id <= 0) {
        rpc_send_error(req_id, "missing or invalid finger_id", tx);
        return;
    }

    bool has_template = false;
    /* -EAGAIN (no refresh_enroll_map has completed yet since boot) still
     * reports valid:false rather than an RPC error -- this is a normal,
     * expected state right after connecting. */
    bool valid = zw3021_get_enrolled((uint16_t)id, &has_template) == 0;

    char data[ZW3021_RPC_RESPONSE_MAX - 32];
    snprintf(data, sizeof(data), "{\"finger_id\":%d,\"valid\":%s,\"has_template\":%s}", id,
             valid ? "true" : "false", (valid && has_template) ? "true" : "false");
    rpc_send_ok(req_id, data, tx);
}

static void cmd_enroll_start(const char *line, int req_id, zw3021_rpc_tx_fn tx) {
    int id = json_find_int(line, "finger_id", -1);
    if (id <= 0) {
        rpc_send_error(req_id, "missing or invalid finger_id", tx);
        return;
    }

    int ret = zw3021_request_enroll((uint16_t)id);
    if (ret != 0) {
        rpc_send_error(req_id, "busy", tx);
        return;
    }
    rpc_send_ok(req_id, NULL, tx);
}

static void cmd_enroll_status(const char *line, int req_id, zw3021_rpc_tx_fn tx) {
    ARG_UNUSED(line);
    char data[48];
    snprintf(data, sizeof(data), "{\"busy\":%s}", zw3021_is_busy() ? "true" : "false");
    rpc_send_ok(req_id, data, tx);
}

struct rpc_command {
    const char *name;
    void (*handler)(const char *line, int req_id, zw3021_rpc_tx_fn tx);
};

static const struct rpc_command rpc_commands[] = {
    {"ping", cmd_ping},
    {"get_status", cmd_get_status},
    {"get_fingers", cmd_get_fingers},
    {"get_finger", cmd_get_finger},
    {"update_finger", cmd_update_finger},
    {"delete_finger", cmd_delete_finger},
    {"set_finger_enter", cmd_set_finger_enter},
    {"enroll_start", cmd_enroll_start},
    {"enroll_status", cmd_enroll_status},
    {"refresh_enroll_map", cmd_refresh_enroll_map},
    {"get_enrolled", cmd_get_enrolled},
};

void zw3021_rpc_send_framing_error(const char *message, zw3021_rpc_tx_fn tx) {
    rpc_send_error(-1, message, tx);
}

void zw3021_rpc_process_line(const char *line, zw3021_rpc_tx_fn tx) {
    char cmd[24];
    if (json_find_string(line, "cmd", cmd, sizeof(cmd)) != 0) {
        rpc_send_error(-1, "missing cmd field", tx);
        return;
    }

    int req_id = json_find_int(line, "req_id", -1);

    for (size_t i = 0; i < ARRAY_SIZE(rpc_commands); i++) {
        if (strcmp(cmd, rpc_commands[i].name) == 0) {
            rpc_commands[i].handler(line, req_id, tx);
            return;
        }
    }

    rpc_send_error(req_id, "unknown command", tx);
}
