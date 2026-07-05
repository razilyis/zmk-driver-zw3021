/*
 * USB CDC-ACM transport for the JSON RPC command dispatch in
 * src/rpc_commands.c. Shares the same USB CDC-ACM console as logging (the
 * zmk-usb-logging snippet's "snippet_zmk_usb_logging_uart" node) -- so log
 * lines and RPC responses interleave on the same serial stream.
 *
 * Framing: a request is dispatched as soon as its top-level {...} braces
 * balance back to zero, rather than waiting for a newline. Several serial
 * terminals turned out not to send one reliably; this makes framing
 * independent of that. Braces inside a quoted string don't count.
 */

#include "zw3021_rpc_commands.h"

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(zw3021_rpc, CONFIG_ZMK_LOG_LEVEL);

#if DT_NODE_EXISTS(DT_NODELABEL(snippet_zmk_usb_logging_uart))

/* Worst case: {"cmd":"update_finger","req_id":-2147483648,"finger_id":65535,
 * "value":"<32 chars, fully escaped = 64>"} is ~140 bytes; leave headroom. */
#define ZW3021_RPC_LINE_MAX 176

static const struct device *rpc_dev;
static char rpc_rx_line[ZW3021_RPC_LINE_MAX];
static uint16_t rpc_rx_pos;

static int rpc_brace_depth;
static bool rpc_in_string;
static bool rpc_escape_next;

static void rpc_transmit(const char *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        uart_poll_out(rpc_dev, data[i]);
    }
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

            /* Ignore whitespace/newlines between messages; a terminal that
             * does send line endings still works fine, it's just no longer
             * required. */
            if (rpc_rx_pos == 0 && (b == '\n' || b == '\r' || b == ' ')) {
                continue;
            }

            if (rpc_rx_pos >= sizeof(rpc_rx_line) - 1) {
                rpc_rx_pos = 0;
                rpc_brace_depth = 0;
                rpc_in_string = false;
                rpc_escape_next = false;
                zw3021_rpc_send_framing_error("line too long", rpc_transmit);
                continue;
            }

            rpc_rx_line[rpc_rx_pos++] = (char)b;

            if (rpc_escape_next) {
                rpc_escape_next = false;
            } else if (rpc_in_string && b == '\\') {
                rpc_escape_next = true;
            } else if (b == '"') {
                rpc_in_string = !rpc_in_string;
            } else if (!rpc_in_string && b == '{') {
                rpc_brace_depth++;
            } else if (!rpc_in_string && b == '}' && rpc_brace_depth > 0) {
                rpc_brace_depth--;
                if (rpc_brace_depth == 0) {
                    rpc_rx_line[rpc_rx_pos] = '\0';
                    zw3021_rpc_process_line(rpc_rx_line, rpc_transmit);
                    rpc_rx_pos = 0;
                }
            }
        }

        int64_t now = k_uptime_get();
        if (got_byte) {
            last_rx_time = now;
        } else if (rpc_rx_pos > 0 && (now - last_rx_time) > 2000) {
            /* A message that never finished (dropped bytes, or braces that
             * never balanced) would otherwise sit here forever, silently
             * corrupting the next real message that arrives. */
            LOG_WRN("zw3021: RPC: discarding stale partial message");
            rpc_rx_pos = 0;
            rpc_brace_depth = 0;
            rpc_in_string = false;
            rpc_escape_next = false;
        }

        k_sleep(K_MSEC(20));
    }
}

K_THREAD_DEFINE(zw3021_rpc_tid, 2048, rpc_thread, NULL, NULL, NULL, 10, 0, 0);

#else

#warning "CONFIG_ZW3021_SERIAL_RPC is enabled but the zmk-usb-logging snippet isn't applied to this build; serial RPC will not be available."

#endif /* DT_NODE_EXISTS(DT_NODELABEL(snippet_zmk_usb_logging_uart)) */
