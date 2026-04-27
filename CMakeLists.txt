cmake_minimum_required(VERSION 3.20.0)

zephyr_include_directories(include)

if(CONFIG_ZMK_BLE_ADV)
    target_sources(app PRIVATE src/ble_adv.c)
endif()
