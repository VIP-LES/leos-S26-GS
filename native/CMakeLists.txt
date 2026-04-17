cmake_minimum_required(VERSION 3.15)


project(radio_receiver)
set(LEOS_SX126X_PORT "linux")


add_subdirectory("../external/leos-sdk" ${CMAKE_BINARY_DIR}/leos-sdk)


add_executable(radio_receiver radio_receiver.c)
target_link_libraries(radio_receiver 
    leos_sx126x
)