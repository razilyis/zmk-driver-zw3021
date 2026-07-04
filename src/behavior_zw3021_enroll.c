/*
 * BEHAVIOR_LOCALITY_GLOBAL behavior: on a split build this is invoked on
 * every side, including the BLE central, where CONFIG_ZW3021 is unset and
 * this is a no-op. Only the side with the physical sensor actually
 * enrolls (and logs there, via the zw3021 log module in zw3021.c).
 */

#define DT_DRV_COMPAT razilyis_zw3021_enroll

#include <zephyr/device.h>

#include <drivers/behavior.h>

#if IS_ENABLED(CONFIG_ZW3021)
#include "zw3021.h"
#endif

static int behavior_zw3021_enroll_pressed(struct zmk_behavior_binding *binding,
                                           struct zmk_behavior_binding_event event) {
    ARG_UNUSED(event);

#if IS_ENABLED(CONFIG_ZW3021)
    zw3021_request_enroll((uint16_t)binding->param1);
#else
    ARG_UNUSED(binding);
#endif

    return 0;
}

static int behavior_zw3021_enroll_released(struct zmk_behavior_binding *binding,
                                            struct zmk_behavior_binding_event event) {
    ARG_UNUSED(binding);
    ARG_UNUSED(event);
    return 0;
}

static const struct behavior_driver_api behavior_zw3021_enroll_driver_api = {
    .locality = BEHAVIOR_LOCALITY_GLOBAL,
    .binding_pressed = behavior_zw3021_enroll_pressed,
    .binding_released = behavior_zw3021_enroll_released,
};

static int behavior_zw3021_enroll_init(const struct device *dev) {
    ARG_UNUSED(dev);
    return 0;
}

DEVICE_DT_INST_DEFINE(0, behavior_zw3021_enroll_init, NULL, NULL, NULL, POST_KERNEL,
                       CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &behavior_zw3021_enroll_driver_api);
