zephyr_library()
zephyr_library_include_directories(include)
# Needed for <drivers/behavior.h> / zmk/behavior.h used by the behavior_*
# sources below; ZMK's own app/include isn't on the module include path
# by default (see zmk-module-Fingerprint's CMakeLists.txt for the same fix).
zephyr_library_include_directories(${APPLICATION_SOURCE_DIR}/include)

zephyr_library_sources_ifdef(CONFIG_ZW3021 src/zw3021.c)
zephyr_library_sources_ifdef(CONFIG_ZW3021_ENROLL_BEHAVIOR src/behavior_zw3021_enroll.c)
zephyr_library_sources_ifdef(CONFIG_ZW3021_DELETE_BEHAVIOR src/behavior_zw3021_delete.c)
zephyr_library_sources_ifdef(CONFIG_ZW3021_CLEAR_BEHAVIOR src/behavior_zw3021_clear.c)
zephyr_library_sources_ifdef(CONFIG_ZW3021_STORAGE src/storage.c)
zephyr_library_sources_ifdef(CONFIG_ZW3021_SERIAL_RPC src/serial_rpc.c)
