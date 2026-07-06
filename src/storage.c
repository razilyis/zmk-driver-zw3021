#include "zw3021_storage.h"

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/fs/nvs.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>
#include <zephyr/storage/flash_map.h>

#include <tinycrypt/aes.h>
#include <tinycrypt/ctr_mode.h>
#include <tinycrypt/sha256.h>

LOG_MODULE_REGISTER(zw3021_storage, CONFIG_ZMK_LOG_LEVEL);

/* The "send enter" flag and per-ID display name live in the same NVS
 * instance as the output strings, but in disjoint key ranges so none of
 * them can ever collide with a real finger id (which stays well under
 * these in practice).
 */
#define ZW3021_STORAGE_ENTER_KEY_OFFSET 0x4000
#define ZW3021_STORAGE_NAME_KEY_OFFSET 0x8000

/* Separate NVS instance on zw3021_partition (independent of ZMK's own
 * settings/BLE-bonding NVS on storage_partition). See
 * boards/shields/mona2/mona2_l.overlay in zmk-config-moNa2-v2 for the
 * partition definition this depends on.
 */
static struct nvs_fs zw3021_nvs;
static bool zw3021_storage_ready;

/* Output strings are encrypted at rest (AES-128-CTR) so that a raw flash
 * dump (e.g. of a lost/discarded keyboard) doesn't reveal them in
 * plaintext. This does NOT protect against an attacker with live SWD
 * debug access to the running chip -- the key derivation below is public
 * source, so anyone who can read hwinfo_get_device_id() from the chip can
 * reproduce the key. It only raises the bar against casual flash
 * extraction. The nRF52840 has no secure element (no TrustZone/CryptoCell)
 * and its hardware AES ECB peripheral is claimed by the BLE controller
 * (see zephyr's drivers/crypto/Kconfig.nrf_ecb: "depends on !BT_CTLR"),
 * so this uses tinycrypt's software AES-128 instead -- already linked
 * into most ZMK BLE builds via LE Secure Connections pairing, but
 * selected explicitly here (see Kconfig) so it's not left to chance.
 *
 * The key itself is never stored in flash; it's re-derived every boot
 * from the chip's own unique hardware ID, so it never appears anywhere
 * on the wire or in the RPC/NVS data path.
 */
#define ZW3021_STORAGE_NONCE_LEN TC_AES_BLOCK_SIZE /* tc_ctr_mode's counter is always one AES block */
#define ZW3021_STORAGE_KEY_SALT "zw3021-storage-v1"

static struct tc_aes_key_sched_struct zw3021_key_sched;
static bool zw3021_key_ready;

static void zw3021_storage_ensure_key(void) {
    if (zw3021_key_ready) {
        return;
    }

    uint8_t chip_id[8] = {0};
    hwinfo_get_device_id(chip_id, sizeof(chip_id));

    struct tc_sha256_state_struct sha;
    uint8_t digest[32];
    tc_sha256_init(&sha);
    tc_sha256_update(&sha, (const uint8_t *)ZW3021_STORAGE_KEY_SALT,
                      sizeof(ZW3021_STORAGE_KEY_SALT) - 1);
    tc_sha256_update(&sha, chip_id, sizeof(chip_id));
    tc_sha256_final(digest, &sha);

    tc_aes128_set_encrypt_key(&zw3021_key_sched, digest);
    zw3021_key_ready = true;
}

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

    /* Stored entry is [nonce (ZW3021_STORAGE_NONCE_LEN) || ciphertext]. */
    uint8_t blob[ZW3021_STORAGE_NONCE_LEN + ZW3021_STORAGE_MAX_LEN];
    ssize_t ret = nvs_read(&zw3021_nvs, id, blob, sizeof(blob));
    if (ret <= (ssize_t)ZW3021_STORAGE_NONCE_LEN) {
        return -ENOENT;
    }

    /* nvs_read() can report more bytes than requested (meaning the stored
     * entry is larger than the buffer); it never writes past sizeof(blob)
     * regardless, so clamp before indexing. */
    size_t total = (size_t)ret < sizeof(blob) ? (size_t)ret : sizeof(blob);
    size_t ciphertext_len = total - ZW3021_STORAGE_NONCE_LEN;
    if (ciphertext_len >= out_len) {
        ciphertext_len = out_len - 1;
    }

    zw3021_storage_ensure_key();

    uint8_t ctr[ZW3021_STORAGE_NONCE_LEN];
    memcpy(ctr, blob, sizeof(ctr));
    tc_ctr_mode((uint8_t *)out, ciphertext_len, blob + ZW3021_STORAGE_NONCE_LEN, ciphertext_len, ctr,
                &zw3021_key_sched);

    out[ciphertext_len] = '\0';
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

    zw3021_storage_ensure_key();

    uint8_t blob[ZW3021_STORAGE_NONCE_LEN + ZW3021_STORAGE_MAX_LEN];
    sys_csrand_get(blob, ZW3021_STORAGE_NONCE_LEN); /* fresh nonce every write */

    uint8_t ctr[ZW3021_STORAGE_NONCE_LEN];
    memcpy(ctr, blob, sizeof(ctr));
    tc_ctr_mode(blob + ZW3021_STORAGE_NONCE_LEN, len, (const uint8_t *)value, len, ctr,
                &zw3021_key_sched);

    ssize_t ret = nvs_write(&zw3021_nvs, id, blob, ZW3021_STORAGE_NONCE_LEN + len);
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

    nvs_delete(&zw3021_nvs, id + ZW3021_STORAGE_ENTER_KEY_OFFSET);
    return nvs_delete(&zw3021_nvs, id);
}

int zw3021_storage_get_enter(uint16_t id, bool *out) {
    *out = false;
    if (!zw3021_storage_ready) {
        return -ENODEV;
    }

    uint8_t value = 0;
    ssize_t ret = nvs_read(&zw3021_nvs, id + ZW3021_STORAGE_ENTER_KEY_OFFSET, &value, sizeof(value));
    if (ret > 0) {
        *out = value != 0;
    }
    return 0;
}

int zw3021_storage_set_enter(uint16_t id, bool enabled) {
    if (!zw3021_storage_ready) {
        return -ENODEV;
    }

    uint8_t value = enabled ? 1 : 0;
    ssize_t ret = nvs_write(&zw3021_nvs, id + ZW3021_STORAGE_ENTER_KEY_OFFSET, &value, sizeof(value));
    return ret < 0 ? (int)ret : 0;
}

/* The display name is not a secret (just a UI label for the slot), so
 * unlike the output string it's stored in plaintext and freely readable. */
int zw3021_storage_get_name(uint16_t id, char *out, size_t out_len) {
    if (!zw3021_storage_ready) {
        return -ENODEV;
    }

    ssize_t ret = nvs_read(&zw3021_nvs, id + ZW3021_STORAGE_NAME_KEY_OFFSET, out, out_len - 1);
    if (ret <= 0) {
        return -ENOENT;
    }

    size_t written = (size_t)ret < out_len - 1 ? (size_t)ret : out_len - 1;
    out[written] = '\0';
    return 0;
}

int zw3021_storage_set_name(uint16_t id, const char *name) {
    if (!zw3021_storage_ready) {
        return -ENODEV;
    }

    size_t len = strlen(name);
    if (len >= ZW3021_STORAGE_NAME_MAX_LEN) {
        return -EINVAL;
    }

    ssize_t ret = nvs_write(&zw3021_nvs, id + ZW3021_STORAGE_NAME_KEY_OFFSET, name, len);
    return ret < 0 ? (int)ret : 0;
}
