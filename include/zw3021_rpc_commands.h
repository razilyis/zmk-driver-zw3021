/*
 * Transport-agnostic JSON RPC command dispatch, shared by the USB CDC-ACM
 * console (src/serial_rpc.c) and the standalone BLE GATT RPC service
 * (src/ble_rpc.c). See src/rpc_commands.c for the protocol itself.
 */

#pragma once

#include <stddef.h>

/* Called by the dispatcher to send exactly one response line (including
 * its trailing '\n') back over whichever transport received the request.
 * Threaded explicitly through the call chain rather than a global, since
 * the USB and BLE transports each run on their own thread and requests
 * could otherwise be processed concurrently.
 */
typedef void (*zw3021_rpc_tx_fn)(const char *data, size_t len);

/* Parses one complete JSON RPC request line and synchronously sends
 * exactly one response via tx.
 */
void zw3021_rpc_process_line(const char *line, zw3021_rpc_tx_fn tx);

/* Sends a {"ok":false,"req_id":-1,"message":"..."} response for transport-
 * level framing errors (e.g. a line that never fit the receive buffer)
 * that never made it to zw3021_rpc_process_line.
 */
void zw3021_rpc_send_framing_error(const char *message, zw3021_rpc_tx_fn tx);
