cmake_minimum_required(VERSION 3.22)

idf_component_register(
    SRCS
        "main.c" "gfx.c" "fpga_tx.c" "fpga_rx.c" "fpga_common.c" "pwrmgr.c"
        "music_player.c" "minimp3_impl.c" "ipod_ui.c" "connectivity.c" "reminders.c"
    INCLUDE_DIRS "."
    REQUIRES
        images driver esp_driver_uart esp_driver_spi esp_driver_gpio esp_driver_i2s
        esp_driver_sdmmc nvs_flash fatfs esp_wifi esp_netif esp_http_client mbedtls lwip bt
        common battery button osd menu_mgr settings mutex
        console crc
)
