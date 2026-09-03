idf_component_register(
    SRCS
        "main.cpp"
        "controller_inputs.cpp"
        "radio_controller.cpp"
        "controller_display.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_adc esp_driver_gpio esp_driver_spi esp_timer nvs_flash
)

target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_20)
