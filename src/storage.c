#include "zw3021_storage.h"

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/fs/nvs.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>

LOG_MODULE_REGISTER(zw3021_storage, CONFIG_ZMK_LOG_LEVEL);

/* Separate NVS instance on zw3021_partition (independent of ZMK's own
 * settings/BLE-bonding NVS on storage_partition). See
 * boards/shields/mona2/mona2_l.overlay in zmk-config-moNa2-v2 for the
 * partition definition this depends on.
 */
static struct nvs_fs zw3021_nvs;
static bool zw3021_storage_ready;

int zw3021_storage_init(void) {
    zw3021_nvs.flash_device = FIXED_PARTITION_DEVICE(zw3021_partition);
    if (!device_is_ready(zw3021_nvs.flash_device)) {
        LOG_ERR("zw3021: storage flash device not ready");
        return -ENODEV;
    }

    zw3021_nvs.offset = FIXED_PARTITION_OFFSET(zw3021_partition);
    zw3021_nvs.sector_size = 4096; /* nRF52840 flash page size */
    zw3021_nvs.sector_count = 8;   /* matches the 32KB partition size */

    int ret = nvs_mount(&zw3021_nvs);
    if (ret != 0) {
        LOG_ERR("zw3021: storage NVS mount failed: %d", ret);
        return ret;
    }

    zw3021_storage_ready = true;
    LOG_INF("zw3021: storage ready");
    return 0;
}

int zw3021_storage_get(uint16_t id, char *out, size_t out_len) {
    if (!zw3021_storage_ready) {
        return -ENODEV;
    }

    ssize_t ret = nvs_read(&zw3021_nvs, id, out, out_len - 1);
    if (ret <= 0) {
        return -ENOENT;
    }

    /* nvs_read() can report more bytes than requested (meaning the stored
     * entry is larger than the buffer); it never writes past out_len - 1
     * regardless, so clamp before indexing to null-terminate. */
    size_t written = (size_t)ret < out_len - 1 ? (size_t)ret : out_len - 1;
    out[written] = '\0';
    return 0;
}

int zw3021_storage_set(uint16_t id, const char *value) {
    if (!zw3021_storage_ready) {
        return -ENODEV;
    }

    size_t len = strlen(value);
    if (len >= ZW3021_STORAGE_MAX_LEN) {
        return -EINVAL;
    }

    ssize_t ret = nvs_write(&zw3021_nvs, id, value, len);
    if (ret < 0) {
        LOG_ERR("zw3021: storage write failed for id %u: %zd", id, ret);
        return (int)ret;
    }

    return 0;
}

int zw3021_storage_delete(uint16_t id) {
    if (!zw3021_storage_ready) {
        return -ENODEV;
    }

    return nvs_delete(&zw3021_nvs, id);
}
