cmake_minimum_required(VERSION 3.25)

project(radio_receiver C)
set(LEOS_SX126X_PORT "linux")

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(GPIOD REQUIRED IMPORTED_TARGET libgpiod)

find_library(RT_LIB rt)

add_subdirectory("../external/leos-sdk" ${CMAKE_BINARY_DIR}/leos-sdk)
add_subdirectory("../external/sx126x_driver" ${CMAKE_BINARY_DIR}/sx126x_driver)

add_executable(radio_receiver radio_receiver.c)
target_link_libraries(radio_receiver
    leos_sx126x
)

add_executable(
    new_radio_receiver
    new_radio_receiver.c
    sx126x_driver_linux_hal.c
)
target_link_libraries(
    new_radio_receiver
    sx126x_driver
    PkgConfig::GPIOD
    Threads::Threads
)
if(RT_LIB)
    target_link_libraries(new_radio_receiver ${RT_LIB})
endif()
target_include_directories(
    new_radio_receiver
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ../external/sx126x_driver/src
)
