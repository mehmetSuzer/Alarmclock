cmake_minimum_required(VERSION 3.19)

include(pico_sdk_import.cmake)

project(pico-alarmclock)

pico_sdk_init()

add_executable(alarmclock
    alarmclock.cpp
    OLED.cpp
)

target_link_libraries(alarmclock
    pico_stdlib
    hardware_rtc
    hardware_i2c
    pico_multicore
)

pico_add_extra_outputs(alarmclock)
