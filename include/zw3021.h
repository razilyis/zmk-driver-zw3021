/*
 * Public request API for the HLK-ZW3021 fingerprint sensor driver.
 * Callers (e.g. behavior drivers) queue a request and return immediately;
 * the driver's worker thread powers the sensor and runs it.
 */

#pragma once

#include <stdint.h>

/* Returns 0 if queued, -EBUSY if a request is already pending/running. */
int zw3021_request_enroll(uint16_t id);
int zw3021_request_delete(uint16_t id);
int zw3021_request_clear(void);
