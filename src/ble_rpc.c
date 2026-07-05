/*
 * Standalone BLE GATT transport for the JSON RPC command dispatch in
 * src/rpc_commands.c, letting a browser (Web Bluetooth) configure the
 * sensor without a USB cable and without depending on ZMK Studio or the
 * split link to the central half.
 *
 * This is a second, independent BLE connection/advertising set on top of
 * whatever ZMK's own split link (peripheral.c) is doing -- that file is
 * untouched. ZMK's peripheral.c only does directed advertising to the
 * already-known central, which a browser could never discover; this file
 * runs its own undirected, connectable extended-advertising set
 * (CONFIG_BT_EXT_ADV_MAX_ADV_SET must be >= 2) so a browser can find and
 * connect to this board as a second, unrelated peer.
 *
 * Protocol is identical to serial_rpc.c's (see rpc_commands.c): flat JSON
 * lines, brace-balance framed. The GATT service/characteristic UUIDs here
 * are our own, unrelated to ZMK Studio's.
 *
 * Security: both characteristics require an encrypted connection
 * (BT_GATT_PERM_*_ENCRYPT), forcing BLE pairing/bonding before any RPC
 * command works -- consistent with rpc_commands.c's write-only design:
 * the goal is that nobody can read or write a fingerprint's output string
 * without first pairing with the physical device.
 */

#include "zw3021_rpc_commands.h"

#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/ring_buffer.h>

LOG_MODULE_REGISTER(zw3021_ble_rpc, CONFIG_ZMK_LOG_LEVEL);

/* Randomly generated, unrelated to ZMK Studio's UUIDs. */
#define ZW3021_BLE_SERVICE_UUID                                                                  \
    BT_UUID_128_ENCODE(0x3dbb63ed, 0xa039, 0x44dd, 0xa1ed, 0xac9710c5c8c8)
#define ZW3021_BLE_REQUEST_CHRC_UUID                                                              \
    BT_UUID_128_ENCODE(0xe893459f, 0x555a, 0x40e3, 0x9247, 0x08ffa74bc37d)
#define ZW3021_BLE_RESPONSE_CHRC_UUID                                                             \
    BT_UUID_128_ENCODE(0xa11fbda1, 0x5857, 0x4073, 0xa78d, 0x1597c55edb0e)

#define ZW3021_BLE_LINE_MAX 176
#define ZW3021_BLE_TX_RINGBUF_LEN 512

static char rx_line[ZW3021_BLE_LINE_MAX];
static uint16_t rx_pos;
static int rx_brace_depth;
static bool rx_in_string;
static bool rx_escape_next;

static char pending_line[ZW3021_BLE_LINE_MAX];
K_SEM_DEFINE(pending_line_sem, 0, 1);

static struct bt_conn *ble_rpc_conn;
static struct bt_le_ext_adv *ble_rpc_adv;

RING_BUF_DECLARE(tx_ring_buf, ZW3021_BLE_TX_RINGBUF_LEN);

static uint16_t notify_size_for_conn(struct bt_conn *conn) {
    uint16_t size = 20; /* default ATT payload unless negotiated higher */
    struct bt_conn_info info;
    if (conn && bt_conn_get_info(conn, &info) >= 0 && info.le.data_len) {
        size = info.le.data_len->tx_max_len;
    }
    return size;
}

static void tx_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    if (!ble_rpc_conn) {
        ring_buf_reset(&tx_ring_buf);
        return;
    }

    uint16_t chunk_size = notify_size_for_conn(ble_rpc_conn);
    uint8_t chunk[chunk_size];

    while (ring_buf_size_get(&tx_ring_buf) > 0) {
        uint32_t got = ring_buf_get(&tx_ring_buf, chunk, chunk_size);
        if (got == 0) {
            break;
        }

        struct bt_gatt_notify_params params = {0};
        static struct bt_uuid_128 resp_uuid = BT_UUID_INIT_128(ZW3021_BLE_RESPONSE_CHRC_UUID);
        params.uuid = &resp_uuid.uuid;
        params.data = chunk;
        params.len = got;

        int attempts = 5;
        int err;
        do {
            err = bt_gatt_notify_cb(ble_rpc_conn, &params);
            if (err == 0 || err == -ENOTCONN) {
                break;
            }
            k_sleep(K_MSEC(20));
        } while (attempts-- > 0);

        if (err) {
            LOG_WRN("zw3021: BLE RPC notify failed: %d", err);
            break;
        }
    }
}

static K_WORK_DEFINE(tx_work, tx_work_handler);

static void ble_rpc_transmit(const char *data, size_t len) {
    if (!ble_rpc_conn) {
        return;
    }
    ring_buf_put(&tx_ring_buf, (const uint8_t *)data, len);
    k_work_submit(&tx_work);
}

static void feed_byte(uint8_t b) {
    if (rx_pos == 0 && (b == '\n' || b == '\r' || b == ' ')) {
        return;
    }

    if (rx_pos >= sizeof(rx_line) - 1) {
        rx_pos = 0;
        rx_brace_depth = 0;
        rx_in_string = false;
        rx_escape_next = false;
        zw3021_rpc_send_framing_error("line too long", ble_rpc_transmit);
        return;
    }

    rx_line[rx_pos++] = (char)b;

    if (rx_escape_next) {
        rx_escape_next = false;
    } else if (rx_in_string && b == '\\') {
        rx_escape_next = true;
    } else if (b == '"') {
        rx_in_string = !rx_in_string;
    } else if (!rx_in_string && b == '{') {
        rx_brace_depth++;
    } else if (!rx_in_string && b == '}' && rx_brace_depth > 0) {
        rx_brace_depth--;
        if (rx_brace_depth == 0) {
            rx_line[rx_pos] = '\0';
            /* Hand off to the processing thread rather than blocking the
             * BT RX context (matches ZMK Studio's own rpc.c pattern). */
            strncpy(pending_line, rx_line, sizeof(pending_line) - 1);
            pending_line[sizeof(pending_line) - 1] = '\0';
            rx_pos = 0;
            k_sem_give(&pending_line_sem);
        }
    }
}

static ssize_t on_request_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                 const void *buf, uint16_t len, uint16_t offset, uint8_t flags) {
    ARG_UNUSED(conn);
    ARG_UNUSED(attr);
    ARG_UNUSED(offset);
    ARG_UNUSED(flags);

    const uint8_t *bytes = buf;
    for (uint16_t i = 0; i < len; i++) {
        feed_byte(bytes[i]);
    }
    return len;
}

static void on_response_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    ARG_UNUSED(attr);
    LOG_DBG("zw3021: BLE RPC notifications %s", value == BT_GATT_CCC_NOTIFY ? "enabled" : "disabled");
}

BT_GATT_SERVICE_DEFINE(zw3021_rpc_svc, BT_GATT_PRIMARY_SERVICE(BT_UUID_DECLARE_128(ZW3021_BLE_SERVICE_UUID)),
                        BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(ZW3021_BLE_REQUEST_CHRC_UUID),
                                                BT_GATT_CHRC_WRITE, BT_GATT_PERM_WRITE_ENCRYPT, NULL,
                                                on_request_write, NULL),
                        BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(ZW3021_BLE_RESPONSE_CHRC_UUID),
                                                BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_NONE, NULL, NULL,
                                                NULL),
                        BT_GATT_CCC(on_response_ccc_changed,
                                    BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT));

static void rpc_processing_thread(void *p1, void *p2, void *p3) {
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    for (;;) {
        k_sem_take(&pending_line_sem, K_FOREVER);
        zw3021_rpc_process_line(pending_line, ble_rpc_transmit);
    }
}

K_THREAD_DEFINE(zw3021_ble_rpc_tid, 2048, rpc_processing_thread, NULL, NULL, NULL, 10, 0, 0);

static int start_advertising(void);

/* Right after a disconnect, bt_le_ext_adv_start() has been observed to
 * fail transiently (-ENOMEM on real hardware) -- the Bluetooth stack
 * needs a moment to reclaim the just-closed connection's resources
 * before it can start advertising again. Retry with capped exponential
 * backoff rather than giving up (which would require a power cycle to
 * reconnect at all). */
#define BLE_RPC_ADV_RETRY_INITIAL_DELAY_MS 500
#define BLE_RPC_ADV_RETRY_MAX_DELAY_MS 5000

static uint32_t adv_retry_delay_ms = BLE_RPC_ADV_RETRY_INITIAL_DELAY_MS;

static void start_adv_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    int err = start_advertising();
    if (err) {
        LOG_WRN("zw3021: BLE RPC advertising start failed (%d), retrying in %u ms", err,
                adv_retry_delay_ms);
        struct k_work_delayable *dwork = k_work_delayable_from_work(work);
        k_work_schedule(dwork, K_MSEC(adv_retry_delay_ms));
        adv_retry_delay_ms = MIN(adv_retry_delay_ms * 2, BLE_RPC_ADV_RETRY_MAX_DELAY_MS);
    } else {
        adv_retry_delay_ms = BLE_RPC_ADV_RETRY_INITIAL_DELAY_MS;
    }
}

static K_WORK_DELAYABLE_DEFINE(start_adv_work, start_adv_work_handler);

static void schedule_advertising_start(uint32_t delay_ms) {
    k_work_schedule(&start_adv_work, K_MSEC(delay_ms));
}

/* Default BLE connection parameters service a connection event every
 * 30-50ms with zero peripheral latency -- i.e. the radio needs
 * high-priority interrupt time roughly 20-30 times per second for the
 * entire lifetime of the connection, not just while data is flowing.
 * Confirmed on real hardware: this alone (no notifications/polling
 * needed) was enough to make the ZW3021's PS_HandShake response never
 * arrive, even with its own receive timeout raised to a full second.
 * This is just a background config console with no latency
 * requirements, so trade connection responsiveness for a much longer
 * interval + peripheral latency, freeing up the CPU for the sensor's
 * UART timing. Supervision timeout must exceed
 * (1 + latency) * interval_max * 2 (spec-mandated slack included below).
 */
#define BLE_RPC_CONN_INTERVAL_MIN 400 /* 500ms (units of 1.25ms) */
#define BLE_RPC_CONN_INTERVAL_MAX 800 /* 1000ms */
#define BLE_RPC_CONN_LATENCY 4
#define BLE_RPC_CONN_TIMEOUT 1500 /* 15000ms (units of 10ms) */

static void on_ext_adv_connected(struct bt_le_ext_adv *adv, struct bt_le_ext_adv_connected_info *info) {
    ARG_UNUSED(adv);
    ble_rpc_conn = info->conn;
    LOG_INF("zw3021: BLE RPC client connected");

    int err = bt_conn_le_param_update(
        ble_rpc_conn, BT_LE_CONN_PARAM(BLE_RPC_CONN_INTERVAL_MIN, BLE_RPC_CONN_INTERVAL_MAX,
                                        BLE_RPC_CONN_LATENCY, BLE_RPC_CONN_TIMEOUT));
    if (err) {
        LOG_WRN("zw3021: BLE RPC connection param update request failed: %d", err);
    }
}

static struct bt_le_ext_adv_cb ext_adv_cb = {
    .connected = on_ext_adv_connected,
};

static void on_disconnected(struct bt_conn *conn, uint8_t reason) {
    ARG_UNUSED(reason);
    if (conn != ble_rpc_conn) {
        return;
    }
    LOG_INF("zw3021: BLE RPC client disconnected");
    ble_rpc_conn = NULL;
    ring_buf_reset(&tx_ring_buf);
    rx_pos = 0;
    rx_brace_depth = 0;
    rx_in_string = false;
    rx_escape_next = false;
    schedule_advertising_start(BLE_RPC_ADV_RETRY_INITIAL_DELAY_MS);
}

/* Diagnostic only: bt_conn_le_param_update() above is just a request --
 * the central can ignore or partially honor it. Log what was actually
 * negotiated so real hardware testing can confirm whether the longer
 * interval requested in on_ext_adv_connected() actually took effect. */
static void on_le_param_updated(struct bt_conn *conn, uint16_t interval, uint16_t latency,
                                 uint16_t timeout) {
    if (conn != ble_rpc_conn) {
        return;
    }
    LOG_INF("zw3021: BLE RPC conn params updated: interval=%u (%u ms) latency=%u timeout=%u (%u ms)",
            interval, interval * 5 / 4, latency, timeout, timeout * 10);
}

BT_CONN_CB_DEFINE(zw3021_ble_rpc_conn_cb) = {
    .disconnected = on_disconnected,
    .le_param_updated = on_le_param_updated,
};

/* No device-name field here: FLAGS (3 bytes) + a 128-bit service UUID
 * (18 bytes) already use most of the legacy 31-byte advertising payload
 * budget, and Web Bluetooth clients match by service UUID (see
 * docs/index.html), not by name. */
static const struct bt_data ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    BT_DATA_BYTES(BT_DATA_UUID128_ALL, ZW3021_BLE_SERVICE_UUID),
};

static int start_advertising(void) {
    if (!ble_rpc_adv) {
        /* Plain BT_LE_ADV_CONN (no BT_LE_ADV_OPT_USE_NAME): the explicit
         * ad[] below already uses most of the legacy 31-byte advertising
         * payload budget, and clients match by service UUID, not name. */
        int err = bt_le_ext_adv_create(BT_LE_ADV_CONN, &ext_adv_cb, &ble_rpc_adv);
        if (err) {
            LOG_ERR("zw3021: failed to create BLE RPC advertising set: %d", err);
            return err;
        }
    }

    int err = bt_le_ext_adv_set_data(ble_rpc_adv, ad, ARRAY_SIZE(ad), NULL, 0);
    if (err) {
        LOG_ERR("zw3021: failed to set BLE RPC advertising data: %d", err);
        return err;
    }

    err = bt_le_ext_adv_start(ble_rpc_adv, NULL);
    if (err) {
        LOG_ERR("zw3021: failed to start BLE RPC advertising: %d", err);
        return err;
    }

    LOG_INF("zw3021: BLE RPC advertising started");
    return 0;
}

/* bt_le_ext_adv_create() fails with -EAGAIN if called before the BT
 * identity address is ready, which only happens once settings_load() has
 * run (main.c calls it after all SYS_INIT hooks, including this one, have
 * already executed) -- confirmed on real hardware: "No ID address. App
 * must call settings_load()" logged immediately before this failure.
 * ZMK's own zmk_peripheral_ble_init() (src/split/bluetooth/peripheral.c)
 * hits the same ordering problem and solves it the same way: register a
 * settings handler whose h_commit callback -- called once, after
 * settings_load() finishes loading everything -- does the actual
 * BT setup instead of doing it directly at SYS_INIT time.
 */
#if IS_ENABLED(CONFIG_SETTINGS)

static int ble_rpc_settings_set(const char *name, size_t len, settings_read_cb read_cb,
                                 void *cb_arg) {
    ARG_UNUSED(name);
    ARG_UNUSED(len);
    ARG_UNUSED(read_cb);
    ARG_UNUSED(cb_arg);
    return 0;
}

static int ble_rpc_settings_commit(void) {
    schedule_advertising_start(0);
    return 0;
}

static struct settings_handler zw3021_ble_rpc_settings_handler = {
    .name = "zw3021_ble_rpc",
    .h_set = ble_rpc_settings_set,
    .h_commit = ble_rpc_settings_commit,
};
#endif

static int zw3021_ble_rpc_init(void) {
#if IS_ENABLED(CONFIG_SETTINGS)
    return settings_register(&zw3021_ble_rpc_settings_handler);
#else
    schedule_advertising_start(0);
    return 0;
#endif
}

SYS_INIT(zw3021_ble_rpc_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
