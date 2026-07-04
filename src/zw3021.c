/*
 * HLK-ZW3021 fingerprint sensor driver (minimal evaluation version)
 *
 * On INT rising edge: power on VCC-D, confirm boot, PS_HandShake,
 * PS_AutoIdentify (1:N), log the result, power off VCC-D, and wait for
 * INT to return low before re-arming.
 *
 * Enrollment/delete/clear (PS_AutoEnroll/PS_DeleteChar/PS_Empty) are
 * triggered separately via zw3021_request_*() (called from the
 * BEHAVIOR_LOCALITY_GLOBAL behaviors in behavior_zw3021_*.c) and are
 * queued to the same worker thread, so they never run concurrently with
 * an INT-triggered identify.
 */

#define DT_DRV_COMPAT razilyis_zw3021

#include "zw3021.h"

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#if IS_ENABLED(CONFIG_ZW3021_STORAGE)
#include "zw3021_storage.h"
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/matrix.h>
#endif

LOG_MODULE_REGISTER(zw3021, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) <= 1, "zw3021: only one instance is supported");

/* Protocol constants (see documents/codex_zw3021_driver_spec.md section 1/8) */
#define ZW3021_HDR0 0xEF
#define ZW3021_HDR1 0x01
#define ZW3021_PKT_ID_CMD 0x01
#define ZW3021_PKT_ID_ACK 0x07
#define ZW3021_CMD_HANDSHAKE 0x35
#define ZW3021_CMD_AUTO_IDENTIFY 0x32
#define ZW3021_CMD_AUTO_ENROLL 0x31
#define ZW3021_CMD_DELETE_CHAR 0x0C
#define ZW3021_CMD_EMPTY 0x0D
#define ZW3021_BOOT_BYTE 0x55

/* Not exposed via devicetree: fixed protocol timing, not board wiring. */
#define ZW3021_QUICK_CMD_TIMEOUT_MS 200
#define ZW3021_REARM_POLL_MS 20

#define ZW3021_RX_BUF_LEN 16

struct zw3021_config {
    const struct device *uart_dev;
    struct gpio_dt_spec int_gpio;
    struct gpio_dt_spec power_en_gpio;
    uint32_t power_on_delay_ms;
    uint32_t startup_timeout_ms;
    uint32_t identify_timeout_ms;
    uint32_t enroll_timeout_ms;
    uint8_t score_level;
    uint8_t enroll_times;
};

enum zw3021_request_type {
    ZW3021_REQ_ENROLL,
    ZW3021_REQ_DELETE,
    ZW3021_REQ_CLEAR,
};

struct zw3021_request {
    enum zw3021_request_type type;
    uint16_t id;
};

static const struct uart_config zw3021_uart_cfg = {
    .baudrate = 57600,
    .parity = UART_CFG_PARITY_NONE,
    .stop_bits = UART_CFG_STOP_BITS_2,
    .data_bits = UART_CFG_DATA_BITS_8,
    .flow_ctrl = UART_CFG_FLOW_CTRL_NONE,
};

/* Single-instance driver: file-scope state instead of per-device data. */
static const struct device *zw3021_dev;
static struct gpio_callback zw3021_int_cb;
K_SEM_DEFINE(zw3021_finger_sem, 0, 1);

static struct zw3021_request zw3021_pending_request;
static bool zw3021_request_pending;
K_SEM_DEFINE(zw3021_request_sem, 0, 1);

static void zw3021_uart_send_packet(const struct device *uart_dev, uint8_t packet_id,
                                     const uint8_t *payload, uint16_t payload_len) {
    uint16_t length = payload_len + 2; /* payload + checksum(2) */
    uint16_t sum = packet_id + (length >> 8) + (length & 0xFF);

    uart_poll_out(uart_dev, ZW3021_HDR0);
    uart_poll_out(uart_dev, ZW3021_HDR1);
    uart_poll_out(uart_dev, 0xFF); /* default device address */
    uart_poll_out(uart_dev, 0xFF);
    uart_poll_out(uart_dev, 0xFF);
    uart_poll_out(uart_dev, 0xFF);
    uart_poll_out(uart_dev, packet_id);
    uart_poll_out(uart_dev, length >> 8);
    uart_poll_out(uart_dev, length & 0xFF);

    for (uint16_t i = 0; i < payload_len; i++) {
        uart_poll_out(uart_dev, payload[i]);
        sum += payload[i];
    }

    uart_poll_out(uart_dev, sum >> 8);
    uart_poll_out(uart_dev, sum & 0xFF);
}

static void zw3021_send_command(const struct device *uart_dev, uint8_t cmd, const uint8_t *params,
                                 uint16_t params_len) {
    uint8_t buf[1 + 5]; /* cmd + largest params used (AutoIdentify/AutoEnroll: 5 bytes) */

    __ASSERT_NO_MSG(params_len <= sizeof(buf) - 1);

    buf[0] = cmd;
    if (params_len > 0) {
        memcpy(&buf[1], params, params_len);
    }
    zw3021_uart_send_packet(uart_dev, ZW3021_PKT_ID_CMD, buf, 1 + params_len);
}

/* Waits for a well-formed 0xEF01 packet and validates its checksum.
 * On success, out_buf holds the payload with the trailing checksum
 * bytes stripped (out_len = payload length - 2), independent of packet
 * ID. Returns -ETIMEDOUT, -EBADMSG, or 0.
 */
static int zw3021_recv_packet(const struct device *uart_dev, uint8_t *out_buf, uint16_t out_buf_len,
                               uint16_t *out_len, uint32_t timeout_ms) {
    int64_t deadline = k_uptime_get() + timeout_ms;
    uint8_t byte;
    uint8_t hdr[9]; /* header(2) + address(4) + packet_id(1) + length(2) */
    uint16_t hdr_idx = 0;
    uint16_t payload_len = 0;
    uint16_t payload_idx = 0;
    bool have_hdr0 = false;

    while (k_uptime_get() < deadline) {
        if (uart_poll_in(uart_dev, &byte) != 0) {
            k_sleep(K_MSEC(1));
            continue;
        }

        if (!have_hdr0) {
            if (byte == ZW3021_HDR0) {
                have_hdr0 = true;
            }
            continue;
        }

        if (hdr_idx == 0) {
            if (byte != ZW3021_HDR1) {
                have_hdr0 = (byte == ZW3021_HDR0);
                continue;
            }
            hdr[0] = ZW3021_HDR0;
            hdr[1] = ZW3021_HDR1;
            hdr_idx = 2;
            continue;
        }

        if (hdr_idx < sizeof(hdr)) {
            hdr[hdr_idx++] = byte;
            if (hdr_idx == sizeof(hdr)) {
                payload_len = (hdr[7] << 8) | hdr[8];
                if (payload_len < 2 || payload_len > out_buf_len) {
                    LOG_ERR("zw3021: invalid packet length %u", payload_len);
                    have_hdr0 = false;
                    hdr_idx = 0;
                    continue;
                }
                payload_idx = 0;
            }
            continue;
        }

        out_buf[payload_idx++] = byte;
        if (payload_idx != payload_len) {
            continue;
        }

        uint16_t sum = hdr[6];
        sum += hdr[7];
        sum += hdr[8];
        for (uint16_t i = 0; i < payload_len - 2; i++) {
            sum += out_buf[i];
        }
        uint16_t recv_checksum = (out_buf[payload_len - 2] << 8) | out_buf[payload_len - 1];
        if (sum != recv_checksum) {
            LOG_ERR("zw3021: invalid checksum");
            return -EBADMSG;
        }

        *out_len = payload_len - 2;
        return 0;
    }

    return -ETIMEDOUT;
}

static void zw3021_wait_boot_byte(const struct device *uart_dev, uint32_t timeout_ms) {
    int64_t deadline = k_uptime_get() + timeout_ms;
    uint8_t byte;

    while (k_uptime_get() < deadline) {
        if (uart_poll_in(uart_dev, &byte) == 0) {
            if (byte == ZW3021_BOOT_BYTE) {
                LOG_INF("zw3021: boot handshake byte received");
                return;
            }
            continue;
        }
        k_sleep(K_MSEC(1));
    }

    LOG_WRN("zw3021: startup timeout, trying handshake");
}

/* Returns 0 on confirm=OK, -ETIMEDOUT, -EBADMSG, or -EIO. */
static int zw3021_handshake(const struct device *uart_dev) {
    uint8_t rx[ZW3021_RX_BUF_LEN];
    uint16_t rx_len;

    zw3021_send_command(uart_dev, ZW3021_CMD_HANDSHAKE, NULL, 0);

    int ret = zw3021_recv_packet(uart_dev, rx, sizeof(rx), &rx_len, ZW3021_QUICK_CMD_TIMEOUT_MS);
    if (ret != 0) {
        return ret;
    }
    if (rx_len < 1) {
        return -EBADMSG;
    }

    return (rx[0] == 0x00) ? 0 : -EIO;
}

/* Returns 0 on match (out_id/out_score valid), -ENOENT (no match / empty
 * DB / empty template), -ETIMEDOUT, -EBADMSG, or -EIO.
 */
static int zw3021_auto_identify(const struct device *uart_dev, uint8_t score_level,
                                 uint32_t timeout_ms, uint16_t *out_id, uint16_t *out_score) {
    /* AutoIdentify params = 0x0004 (bit2=1): the sensor returns only the
     * final result in a single response packet instead of one packet per
     * matching stage (see spec section 1.1). ID = 0xFFFF requests 1:N
     * search across the whole database.
     */
    uint8_t params[5] = {
        score_level, 0xFF, 0xFF, 0x00, 0x04,
    };
    uint8_t rx[ZW3021_RX_BUF_LEN];
    uint16_t rx_len;

    zw3021_send_command(uart_dev, ZW3021_CMD_AUTO_IDENTIFY, params, sizeof(params));

    int ret = zw3021_recv_packet(uart_dev, rx, sizeof(rx), &rx_len, timeout_ms);
    if (ret != 0) {
        return ret;
    }
    if (rx_len < 2) {
        return -EBADMSG;
    }

    uint8_t confirm = rx[0];
    switch (confirm) {
    case 0x00:
        if (rx_len < 6) {
            return -EBADMSG;
        }
        *out_id = (rx[2] << 8) | rx[3];
        *out_score = (rx[4] << 8) | rx[5];
        return 0;
    case 0x09: /* no matching fingerprint */
        LOG_WRN("zw3021: no matching fingerprint");
        return -ENOENT;
    case 0x24: /* fingerprint database empty */
        LOG_WRN("zw3021: fingerprint database empty");
        return -ENOENT;
    case 0x23: /* fingerprint template empty */
        LOG_WRN("zw3021: fingerprint template empty");
        return -ENOENT;
    case 0x26: /* timeout */
        return -ETIMEDOUT;
    default:
        LOG_ERR("zw3021: auto identify error, confirm=0x%02x", confirm);
        return -EIO;
    }
}

/* Returns 0 on a fully stored enrollment, -ETIMEDOUT, -EBADMSG, or -EIO. */
static int zw3021_auto_enroll(const struct device *uart_dev, uint16_t id, uint8_t enroll_times,
                               uint32_t timeout_ms) {
    /* params = 0x000C:
     *  bit2=1: don't require per-step status (may not be honored for every
     *          intermediate capture step -- the receive loop below reads
     *          until a terminal packet regardless of how many arrive).
     *  bit3=1: allow overwriting an existing template at this ID, so the
     *          same test ID can be re-enrolled without deleting first.
     *  bit4=0: allow the same finger to be enrolled again under another ID
     *          (convenient when testing with one finger / multiple IDs).
     *  bit5=0: require lifting the finger between captures (safer default).
     */
    uint8_t params[5] = {
        (uint8_t)(id >> 8), (uint8_t)id, enroll_times, 0x00, 0x0C,
    };
    uint8_t rx[ZW3021_RX_BUF_LEN];
    uint16_t rx_len;
    int64_t deadline = k_uptime_get() + timeout_ms;

    zw3021_send_command(uart_dev, ZW3021_CMD_AUTO_ENROLL, params, sizeof(params));

    for (;;) {
        int64_t remaining = deadline - k_uptime_get();
        if (remaining <= 0) {
            return -ETIMEDOUT;
        }

        int ret = zw3021_recv_packet(uart_dev, rx, sizeof(rx), &rx_len, (uint32_t)remaining);
        if (ret != 0) {
            return ret;
        }
        if (rx_len < 2) {
            return -EBADMSG;
        }

        uint8_t confirm = rx[0];
        uint8_t stage = rx[1];

        /* Per the protocol manual, a feature-generation failure at the
         * capture stage (07 at stage 02) doesn't end the enrollment: the
         * sensor goes back to waiting for another successful capture of
         * the same attempt. Every other non-zero confirm code ends the
         * flow (matches the manual's explicit "结束流程" for each case).
         */
        if (confirm == 0x07 && stage == 0x02) {
            LOG_WRN("zw3021: enroll: feature generation failed, retrying capture");
            continue;
        }

        if (confirm != 0x00) {
            switch (confirm) {
            case 0x0B:
                LOG_WRN("zw3021: enroll: invalid id");
                break;
            case 0x1F:
                LOG_WRN("zw3021: enroll: fingerprint database full");
                break;
            case 0x22:
                LOG_WRN("zw3021: enroll: id already has a template");
                break;
            case 0x25:
                LOG_WRN("zw3021: enroll: invalid enroll-times");
                break;
            case 0x26:
                return -ETIMEDOUT;
            case 0x27:
                LOG_WRN("zw3021: enroll: fingerprint already registered under another id");
                break;
            case 0x0A:
                LOG_WRN("zw3021: enroll: template merge failed");
                break;
            default:
                LOG_ERR("zw3021: enroll error, confirm=0x%02x stage=0x%02x", confirm, stage);
                break;
            }
            return -EIO;
        }

        if (stage == 0x06) {
            return 0; /* store result: confirm=0x00 at this stage means success */
        }

        LOG_DBG("zw3021: enroll: stage=0x%02x", stage);
    }
}

/* Returns 0 on success, -ETIMEDOUT, -EBADMSG, or -EIO. */
static int zw3021_delete_char(const struct device *uart_dev, uint16_t page_id, uint16_t n) {
    uint8_t params[4] = {
        (uint8_t)(page_id >> 8), (uint8_t)page_id, (uint8_t)(n >> 8), (uint8_t)n,
    };
    uint8_t rx[ZW3021_RX_BUF_LEN];
    uint16_t rx_len;

    zw3021_send_command(uart_dev, ZW3021_CMD_DELETE_CHAR, params, sizeof(params));

    int ret = zw3021_recv_packet(uart_dev, rx, sizeof(rx), &rx_len, ZW3021_QUICK_CMD_TIMEOUT_MS);
    if (ret != 0) {
        return ret;
    }
    if (rx_len < 1) {
        return -EBADMSG;
    }
    if (rx[0] == 0x00) {
        return 0;
    }

    LOG_WRN("zw3021: delete failed, confirm=0x%02x", rx[0]);
    return -EIO;
}

/* Returns 0 on success, -ETIMEDOUT, -EBADMSG, or -EIO. */
static int zw3021_empty_database(const struct device *uart_dev) {
    uint8_t rx[ZW3021_RX_BUF_LEN];
    uint16_t rx_len;

    zw3021_send_command(uart_dev, ZW3021_CMD_EMPTY, NULL, 0);

    int ret = zw3021_recv_packet(uart_dev, rx, sizeof(rx), &rx_len, ZW3021_QUICK_CMD_TIMEOUT_MS);
    if (ret != 0) {
        return ret;
    }
    if (rx_len < 1) {
        return -EBADMSG;
    }
    if (rx[0] == 0x00) {
        return 0;
    }

    LOG_WRN("zw3021: clear database failed, confirm=0x%02x", rx[0]);
    return -EIO;
}

/* Shared power-on/boot-confirm/handshake preamble used by both the
 * INT-triggered identify flow and the on-demand enroll/delete/clear
 * requests. Returns 0 if the sensor is ready for commands.
 */
static int zw3021_power_on_and_handshake(const struct zw3021_config *cfg) {
    gpio_pin_set_dt(&cfg->power_en_gpio, 1);
    LOG_INF("zw3021: VCC-D enabled");

    k_sleep(K_MSEC(cfg->power_on_delay_ms));

    zw3021_wait_boot_byte(cfg->uart_dev, cfg->startup_timeout_ms);

    int ret = zw3021_handshake(cfg->uart_dev);
    if (ret != 0) {
        LOG_ERR("zw3021: handshake timeout/error: %d", ret);
        return ret;
    }
    LOG_INF("zw3021: PS_HandShake OK");
    return 0;
}

/* Shared power-off + re-arm postamble; always runs regardless of how the
 * preceding command sequence turned out.
 */
static void zw3021_power_off_and_rearm(const struct zw3021_config *cfg) {
    gpio_pin_set_dt(&cfg->power_en_gpio, 0);
    LOG_INF("zw3021: VCC-D disabled");

    while (gpio_pin_get_dt(&cfg->int_gpio) > 0) {
        k_sleep(K_MSEC(ZW3021_REARM_POLL_MS));
    }
}

#if IS_ENABLED(CONFIG_ZW3021_STORAGE)
/* Virtual output keyboard: 36 positions reserved at the end of the keymap
 * (row 4 in the moNa2 matrix transform, unreachable by real kscan hardware
 * -- see boards/shields/mona2/mona2.dtsi in zmk-config-moNa2-v2), bound on
 * default_layer to '0'-'9' then 'a'-'z' in that order. Typing a stored
 * string is done by walking it and raising a press+release
 * position-changed event per character, which ZMK's own
 * split_peripheral_listener automatically forwards to the BLE central
 * (zmk/app/src/split/peripheral.c) -- no protocol changes needed.
 *
 * Case/symbols are not supported in this phase: only lowercase letters and
 * digits round-trip correctly (see zw3021_char_to_offset()).
 */
#define ZW3021_OUTPUT_KEYBOARD_LEN 36
#define ZW3021_OUTPUT_BASE_POSITION (ZMK_KEYMAP_LEN - ZW3021_OUTPUT_KEYBOARD_LEN)
#define ZW3021_KEYPRESS_GAP_MS 5

static int zw3021_char_to_offset(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'z') {
        return 10 + (c - 'a');
    }
    if (c >= 'A' && c <= 'Z') {
        return 10 + (c - 'A'); /* folded to the same lowercase-key position */
    }
    return -1;
}

static void zw3021_emit_char(char c) {
    int offset = zw3021_char_to_offset(c);
    if (offset < 0) {
        LOG_WRN("zw3021: output: unsupported character 0x%02x, skipping", (uint8_t)c);
        return;
    }

    uint32_t position = ZW3021_OUTPUT_BASE_POSITION + offset;

    raise_zmk_position_state_changed((struct zmk_position_state_changed){
        .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
        .state = true,
        .position = position,
        .timestamp = k_uptime_get(),
    });
    k_sleep(K_MSEC(ZW3021_KEYPRESS_GAP_MS));

    raise_zmk_position_state_changed((struct zmk_position_state_changed){
        .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
        .state = false,
        .position = position,
        .timestamp = k_uptime_get(),
    });
    k_sleep(K_MSEC(ZW3021_KEYPRESS_GAP_MS));
}

static void zw3021_emit_string(const char *str) {
    for (const char *p = str; *p != '\0'; p++) {
        zw3021_emit_char(*p);
    }
}

static void zw3021_emit_match_output(uint16_t match_id) {
    char value[ZW3021_STORAGE_MAX_LEN + 1];

    int ret = zw3021_storage_get(match_id, value, sizeof(value));
    if (ret != 0) {
        LOG_DBG("zw3021: no stored output string for id=%u", match_id);
        return;
    }

    zw3021_emit_string(value);
}
#endif /* IS_ENABLED(CONFIG_ZW3021_STORAGE) */

static void zw3021_handle_finger_event(const struct device *dev) {
    const struct zw3021_config *cfg = dev->config;

    LOG_INF("zw3021: finger detected");

    if (zw3021_power_on_and_handshake(cfg) == 0) {
        LOG_INF("zw3021: identify started");
        uint16_t match_id = 0;
        uint16_t score = 0;
        int ret = zw3021_auto_identify(cfg->uart_dev, cfg->score_level, cfg->identify_timeout_ms,
                                        &match_id, &score);
        switch (ret) {
        case 0:
            LOG_INF("zw3021: match id=%u score=%u", match_id, score);
#if IS_ENABLED(CONFIG_ZW3021_STORAGE)
            zw3021_emit_match_output(match_id);
#endif
            break;
        case -ENOENT:
            /* zw3021_auto_identify() already logged the specific reason. */
            break;
        case -ETIMEDOUT:
            LOG_WRN("zw3021: identify timeout");
            break;
        default:
            LOG_ERR("zw3021: identify error: %d", ret);
            break;
        }
    }

    zw3021_power_off_and_rearm(cfg);
}

static void zw3021_handle_request_event(const struct device *dev, const struct zw3021_request *req) {
    const struct zw3021_config *cfg = dev->config;

    if (zw3021_power_on_and_handshake(cfg) == 0) {
        switch (req->type) {
        case ZW3021_REQ_ENROLL: {
            LOG_INF("zw3021: enroll started, id=%u times=%u", req->id, cfg->enroll_times);
            int ret =
                zw3021_auto_enroll(cfg->uart_dev, req->id, cfg->enroll_times, cfg->enroll_timeout_ms);
            if (ret == 0) {
                LOG_INF("zw3021: enroll stored id=%u", req->id);
            } else {
                LOG_WRN("zw3021: enroll failed: %d", ret);
            }
            break;
        }
        case ZW3021_REQ_DELETE: {
            int ret = zw3021_delete_char(cfg->uart_dev, req->id, 1);
            if (ret == 0) {
                LOG_INF("zw3021: deleted id=%u", req->id);
            } else {
                LOG_WRN("zw3021: delete failed: %d", ret);
            }
            break;
        }
        case ZW3021_REQ_CLEAR: {
            int ret = zw3021_empty_database(cfg->uart_dev);
            if (ret == 0) {
                LOG_INF("zw3021: database cleared");
            } else {
                LOG_WRN("zw3021: clear failed: %d", ret);
            }
            break;
        }
        }
    }

    zw3021_power_off_and_rearm(cfg);
}

static int zw3021_queue_request(enum zw3021_request_type type, uint16_t id) {
    if (zw3021_request_pending) {
        LOG_WRN("zw3021: busy, dropping request (type=%d)", type);
        return -EBUSY;
    }

    zw3021_pending_request.type = type;
    zw3021_pending_request.id = id;
    zw3021_request_pending = true;
    k_sem_give(&zw3021_request_sem);
    return 0;
}

int zw3021_request_enroll(uint16_t id) { return zw3021_queue_request(ZW3021_REQ_ENROLL, id); }

int zw3021_request_delete(uint16_t id) { return zw3021_queue_request(ZW3021_REQ_DELETE, id); }

int zw3021_request_clear(void) { return zw3021_queue_request(ZW3021_REQ_CLEAR, 0); }

bool zw3021_is_busy(void) { return zw3021_request_pending; }

static void zw3021_thread_fn(void *p1, void *p2, void *p3) {
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    struct k_poll_event events[2] = {
        K_POLL_EVENT_STATIC_INITIALIZER(K_POLL_TYPE_SEM_AVAILABLE, K_POLL_MODE_NOTIFY_ONLY,
                                         &zw3021_finger_sem, 0),
        K_POLL_EVENT_STATIC_INITIALIZER(K_POLL_TYPE_SEM_AVAILABLE, K_POLL_MODE_NOTIFY_ONLY,
                                         &zw3021_request_sem, 0),
    };

    for (;;) {
        k_poll(events, ARRAY_SIZE(events), K_FOREVER);

        if (events[0].state == K_POLL_STATE_SEM_AVAILABLE) {
            k_sem_take(&zw3021_finger_sem, K_NO_WAIT);
            if (zw3021_dev != NULL) {
                zw3021_handle_finger_event(zw3021_dev);
            }
        }

        if (events[1].state == K_POLL_STATE_SEM_AVAILABLE) {
            k_sem_take(&zw3021_request_sem, K_NO_WAIT);
            struct zw3021_request req = zw3021_pending_request;
            if (zw3021_dev != NULL) {
                zw3021_handle_request_event(zw3021_dev, &req);
            }
            zw3021_request_pending = false;
        }

        events[0].state = K_POLL_STATE_NOT_READY;
        events[1].state = K_POLL_STATE_NOT_READY;

        /* Discard any extra finger triggers queued while we were busy
         * (e.g. the finger lifts/places during an enroll's own capture
         * cycles), so they don't immediately kick off a stray identify.
         */
        while (k_sem_take(&zw3021_finger_sem, K_NO_WAIT) == 0) {
            /* drain */
        }
    }
}

K_THREAD_DEFINE(zw3021_tid, 1536, zw3021_thread_fn, NULL, NULL, NULL, 10, 0, 0);

static void zw3021_int_handler(const struct device *port, struct gpio_callback *cb,
                                uint32_t pins) {
    ARG_UNUSED(port);
    ARG_UNUSED(cb);
    ARG_UNUSED(pins);

    k_sem_give(&zw3021_finger_sem);
}

static int zw3021_init(const struct device *dev) {
    const struct zw3021_config *cfg = dev->config;
    int ret;

    if (!device_is_ready(cfg->uart_dev)) {
        LOG_ERR("zw3021: UART device not ready");
        return -ENODEV;
    }

    ret = uart_configure(cfg->uart_dev, &zw3021_uart_cfg);
    if (ret != 0) {
        LOG_ERR("zw3021: failed to configure UART (57600/8N2): %d", ret);
        return ret;
    }

    if (!gpio_is_ready_dt(&cfg->power_en_gpio)) {
        LOG_ERR("zw3021: power-en-gpios not ready");
        return -ENODEV;
    }
    ret = gpio_pin_configure_dt(&cfg->power_en_gpio, GPIO_OUTPUT_INACTIVE);
    if (ret != 0) {
        LOG_ERR("zw3021: failed to configure power-en-gpios: %d", ret);
        return ret;
    }

    if (!gpio_is_ready_dt(&cfg->int_gpio)) {
        LOG_ERR("zw3021: int-gpios not ready");
        return -ENODEV;
    }
    ret = gpio_pin_configure_dt(&cfg->int_gpio, GPIO_INPUT);
    if (ret != 0) {
        LOG_ERR("zw3021: failed to configure int-gpios: %d", ret);
        return ret;
    }

    gpio_init_callback(&zw3021_int_cb, zw3021_int_handler, BIT(cfg->int_gpio.pin));
    ret = gpio_add_callback_dt(&cfg->int_gpio, &zw3021_int_cb);
    if (ret != 0) {
        LOG_ERR("zw3021: failed to add int-gpios callback: %d", ret);
        return ret;
    }

    ret = gpio_pin_interrupt_configure_dt(&cfg->int_gpio, GPIO_INT_EDGE_TO_ACTIVE);
    if (ret != 0) {
        LOG_ERR("zw3021: failed to configure int-gpios interrupt: %d", ret);
        return ret;
    }

    zw3021_dev = dev;

#if IS_ENABLED(CONFIG_ZW3021_STORAGE)
    ret = zw3021_storage_init();
    if (ret != 0) {
        /* Non-fatal: identify still works and just won't type anything. */
        LOG_ERR("zw3021: storage init failed, match output disabled: %d", ret);
    }
#endif

    LOG_INF("zw3021: initialized");
    return 0;
}

#define ZW3021_INIT(inst)                                                                         \
    static const struct zw3021_config zw3021_cfg_##inst = {                                        \
        .uart_dev = DEVICE_DT_GET(DT_INST_BUS(inst)),                                              \
        .int_gpio = GPIO_DT_SPEC_INST_GET(inst, int_gpios),                                        \
        .power_en_gpio = GPIO_DT_SPEC_INST_GET(inst, power_en_gpios),                              \
        .power_on_delay_ms = DT_INST_PROP(inst, power_on_delay_ms),                                 \
        .startup_timeout_ms = DT_INST_PROP(inst, startup_timeout_ms),                               \
        .identify_timeout_ms = DT_INST_PROP(inst, identify_timeout_ms),                             \
        .enroll_timeout_ms = DT_INST_PROP(inst, enroll_timeout_ms),                                 \
        .score_level = DT_INST_PROP(inst, score_level),                                             \
        .enroll_times = DT_INST_PROP(inst, enroll_times),                                           \
    };                                                                                              \
                                                                                                     \
    /* Priority 90: run after the GPIO and UART controller drivers. */    \
    DEVICE_DT_INST_DEFINE(inst, zw3021_init, NULL, NULL, &zw3021_cfg_##inst, POST_KERNEL, 90, NULL);

DT_INST_FOREACH_STATUS_OKAY(ZW3021_INIT)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
