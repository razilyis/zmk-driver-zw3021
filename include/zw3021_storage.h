/*
 * Per-fingerprint-ID output string storage (NVS on a dedicated flash
 * partition, "zw3021_partition"). Never committed to git: entries are only
 * ever written at runtime via the serial RPC interface (src/serial_rpc.c).
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ZW3021_STORAGE_MAX_LEN 32

/* Display name: a non-secret UI label for a slot (not typed out, not
 * subject to the alnum-only constraint output strings have). */
#define ZW3021_STORAGE_NAME_MAX_LEN 48

/* Returns 0 on success, negative errno otherwise. */
int zw3021_storage_init(void);

/* out must be at least ZW3021_STORAGE_MAX_LEN + 1 bytes. Returns 0 on
 * success (out is null-terminated), -ENOENT if nothing is stored for id.
 * Intentionally not exposed over the serial RPC (src/serial_rpc.c) --
 * only zw3021.c reads the raw value back, to type it out on a match.
 * Stored encrypted at rest (see storage.c); this returns the decrypted
 * plaintext.
 */
int zw3021_storage_get(uint16_t id, char *out, size_t out_len);

/* value must be ASCII, at most ZW3021_STORAGE_MAX_LEN - 1 characters. */
int zw3021_storage_set(uint16_t id, const char *value);

/* Also clears the id's "send enter" flag (zw3021_storage_set_enter).
 * Does NOT clear the id's display name (zw3021_storage_set_name) -- the
 * name is a label for the slot, independent of whatever string is
 * currently stored there. */
int zw3021_storage_delete(uint16_t id);

/* Whether to send an Enter keypress after typing id's output string on a
 * match. *out is set to false (disabled) if nothing has been stored yet.
 */
int zw3021_storage_get_enter(uint16_t id, bool *out);
int zw3021_storage_set_enter(uint16_t id, bool enabled);

/* Non-secret per-slot display name (UTF-8 allowed, never typed out).
 * out must be at least ZW3021_STORAGE_NAME_MAX_LEN + 1 bytes. Returns 0
 * on success (out is null-terminated), -ENOENT if nothing is stored for
 * id (out is left untouched by the caller's choice, same convention as
 * zw3021_storage_get). */
int zw3021_storage_get_name(uint16_t id, char *out, size_t out_len);

/* name must be at most ZW3021_STORAGE_NAME_MAX_LEN - 1 bytes (UTF-8). */
int zw3021_storage_set_name(uint16_t id, const char *name);
