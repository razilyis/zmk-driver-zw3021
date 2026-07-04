/*
 * HLK-ZW3021 fingerprint sensor driver (minimal evaluation version)
 *
 * On INT rising edge: power on VCC-D, confirm boot, PS_HandShake,
 * PS_AutoIdentify (1:N), log the result, power off VCC-D, and wait for
 * INT to return low before re-arming. No behavior/keystroke output.
 */

#define DT_DRV_COMPAT razilyis_zw3021

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

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
#define ZW3021_BOOT_BYTE 0x55

/* Not exposed via devicetree: fixed protocol timing, not board wiring. */
#define ZW3021_HANDSHAKE_RX_TIMEOUT_MS 200
#define ZW3021_REARM_POLL_MS 20

#define ZW3021_RX_BUF_LEN 16

struct zw3021_config {
    const struct device *uart_dev;
    struct gpio_dt_spec int_gpio;
    struct gpio_dt_spec power_en_gpio;
    uint32_t power_on_delay_ms;
    uint32_t startup_timeout_ms;
    uint32_t identify_timeout_ms;
    uint8_t score_level;
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
    uint8_t buf[1 + 5]; /* cmd + largest params used (AutoIdentify: 5 bytes) */

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

    int ret = zw3021_recv_packet(uart_dev, rx, sizeof(rx), &rx_len, ZW3021_HANDSHAKE_RX_TIMEOUT_MS);
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

static void zw3021_handle_finger_event(const struct device *dev) {
    const struct zw3021_config *cfg = dev->config;

    gpio_pin_set_dt(&cfg->power_en_gpio, 1);
    LOG_INF("zw3021: finger detected");
    LOG_INF("zw3021: VCC-D enabled");

    k_sleep(K_MSEC(cfg->power_on_delay_ms));

    zw3021_wait_boot_byte(cfg->uart_dev, cfg->startup_timeout_ms);

    int ret = zw3021_handshake(cfg->uart_dev);
    if (ret != 0) {
        LOG_ERR("zw3021: handshake timeout/error: %d", ret);
        goto power_off;
    }
    LOG_INF("zw3021: PS_HandShake OK");

    LOG_INF("zw3021: identify started");
    uint16_t match_id = 0;
    uint16_t score = 0;
    ret = zw3021_auto_identify(cfg->uart_dev, cfg->score_level, cfg->identify_timeout_ms, &match_id,
                                &score);
    switch (ret) {
    case 0:
        LOG_INF("zw3021: match id=%u score=%u", match_id, score);
        break;
    case -ENOENT:
        LOG_WRN("zw3021: no matching fingerprint");
        break;
    case -ETIMEDOUT:
        LOG_WRN("zw3021: identify timeout");
        break;
    default:
        LOG_ERR("zw3021: identify error: %d", ret);
        break;
    }

power_off:
    gpio_pin_set_dt(&cfg->power_en_gpio, 0);
    LOG_INF("zw3021: VCC-D disabled");

    /* Don't re-arm until the finger is actually lifted, regardless of
     * which path above was taken. */
    while (gpio_pin_get_dt(&cfg->int_gpio) > 0) {
        k_sleep(K_MSEC(ZW3021_REARM_POLL_MS));
    }
}

static void zw3021_thread_fn(void *p1, void *p2, void *p3) {
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    for (;;) {
        k_sem_take(&zw3021_finger_sem, K_FOREVER);

        if (zw3021_dev != NULL) {
            zw3021_handle_finger_event(zw3021_dev);
        }

        /* Discard any extra triggers queued while we were busy, so a
         * finger still resting on the sensor doesn't immediately
         * re-trigger before the INT-low check above has settled. */
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
        .score_level = DT_INST_PROP(inst, score_level),                                             \
    };                                                                                              \
                                                                                                     \
    /* Priority 90: run after the GPIO and UART controller drivers. */    \
    DEVICE_DT_INST_DEFINE(inst, zw3021_init, NULL, NULL, &zw3021_cfg_##inst, POST_KERNEL, 90, NULL);

DT_INST_FOREACH_STATUS_OKAY(ZW3021_INIT)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
