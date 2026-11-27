idf_build_get_property(use_registry IDF_COMPONENT_MANAGER)
if(NOT use_registry)
    set(local_usb esp_tinyusb)
endif()
idf_component_register(SRCS "../../main/frame_central.c" "decoder.c" "bridge.c" "usb_pad.c" "pro3.c" "pairing.c" "setup.c"
                            "ble_host.c" "ble_hid.c" "ble_report.c" "motion_input.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "../app/index.html" "../app/style.css" "../app/ui.js" "../app/touch.js" "../app/backup.js"
    PRIV_REQUIRES ${local_usb} bt nvs_flash esp_timer esp_driver_gpio esp_wifi esp_http_server esp_netif esp_event)
target_compile_definitions(${COMPONENT_LIB} PRIVATE FRAME_BRIDGE=1)
