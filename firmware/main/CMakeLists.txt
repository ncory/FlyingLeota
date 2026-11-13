idf_component_register(
    SRCS main.cpp motion.cpp board_io.cpp dock_io.cpp drives_sim.cpp drives_stepdir.cpp drives_odrive.cpp
         load_cells.cpp anemometer.cpp network.cpp screen.cpp nvs_store.cpp console.cpp
    INCLUDE_DIRS .
    REQUIRES leota_core leota_ui
             esp_driver_gpio esp_driver_gptimer esp_driver_i2c esp_driver_rmt esp_driver_twai esp_driver_uart
             esp_eth esp_netif esp_event esp_timer nvs_flash console lwip
             espressif__esp32_p4_function_ev_board)
