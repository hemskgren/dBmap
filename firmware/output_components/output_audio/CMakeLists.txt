idf_component_register(
    SRCS
        "audio_amp.c"
        "audio_media.c"
        "audio_player.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES
        driver
        esp_driver_gpio
        freertos
        esp_http_client
        esp-tls
        mbedtls
        vfs
        node_common
        joltwallet__littlefs
        chmorgan__esp-libhelix-mp3
)
