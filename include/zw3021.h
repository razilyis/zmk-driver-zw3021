/*
 * Public request API for the HLK-ZW3021 fingerprint sensor driver.
 * Callers (e.g. behavior drivers) queue a request and return immediately;
 * the driver's worker thread powers the sensor and runs it.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Returns 0 if queued, -EBUSY if a request is already pending/running. */
int zw3021_request_enroll(uint16_t id);
int zw3021_request_delete(uint16_t id);
int zw3021_request_clear(void);

/* Queues a PS_ReadIndexTable(page 0) query, covering fingerprint template
 * IDs 0-255. Returns 0 if queued, -EBUSY if a request is already
 * pending/running. Poll zw3021_is_busy() and then call
 * zw3021_get_enrolled() once it clears. */
int zw3021_request_read_enrolled(void);

/* True while an enroll/delete/clear/read-enrolled request (or an
 * INT-triggered identify) is queued or actively running. */
bool zw3021_is_busy(void);

/* Fills *has_template from the bitmap fetched by the most recent
 * zw3021_request_read_enrolled() (regardless of how long ago). Returns 0
 * on success, -EAGAIN if no bitmap has been read yet since boot, -EINVAL
 * if id is out of range (0-255). */
int zw3021_get_enrolled(uint16_t id, bool *has_template);
