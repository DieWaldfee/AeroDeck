cmake_minimum_required(VERSION 3.16)
project(bridge CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# SimConnect SDK path — set via: cmake -DSIMCONNECT_SDK_PATH="C:\MSFS SDK\SimConnect SDK"
set(SIMCONNECT_SDK_PATH "" CACHE PATH "Path to SimConnect SDK root (contains include/ and lib/)")

if(NOT SIMCONNECT_SDK_PATH)
    message(FATAL_ERROR
        "SIMCONNECT_SDK_PATH is not set.\n"
        "Run cmake with: -DSIMCONNECT_SDK_PATH=\"C:\\MSFS 2024 SDK\\SimConnect SDK\"")
endif()

set(SIMCONNECT_INCLUDE "${SIMCONNECT_SDK_PATH}/include")
set(SIMCONNECT_LIB_DIR "${SIMCONNECT_SDK_PATH}/lib/EX1")

# Support multiple SDK layouts (EX1, direct lib, or static).
if(NOT EXISTS "${SIMCONNECT_LIB_DIR}/SimConnect.lib")
    if(EXISTS "${SIMCONNECT_SDK_PATH}/lib/SimConnect.lib")
        set(SIMCONNECT_LIB_DIR "${SIMCONNECT_SDK_PATH}/lib")
    elseif(EXISTS "${SIMCONNECT_SDK_PATH}/lib/static/SimConnect.lib")
        set(SIMCONNECT_LIB_DIR "${SIMCONNECT_SDK_PATH}/lib/static")
    endif()
endif()

if(NOT EXISTS "${SIMCONNECT_LIB_DIR}/SimConnect.lib")
    message(FATAL_ERROR
        "Could not find SimConnect.lib under '${SIMCONNECT_SDK_PATH}'.\n"
        "Checked: lib/EX1, lib/, and lib/static")
endif()

set(SIMCONNECT_DLL "")
if(EXISTS "${SIMCONNECT_LIB_DIR}/SimConnect.dll")
    set(SIMCONNECT_DLL "${SIMCONNECT_LIB_DIR}/SimConnect.dll")
elseif(EXISTS "${SIMCONNECT_SDK_PATH}/lib/SimConnect.dll")
    set(SIMCONNECT_DLL "${SIMCONNECT_SDK_PATH}/lib/SimConnect.dll")
elseif(EXISTS "${SIMCONNECT_SDK_PATH}/SimConnect.dll")
    set(SIMCONNECT_DLL "${SIMCONNECT_SDK_PATH}/SimConnect.dll")
endif()

add_executable(bridge
    src/main.cpp
    src/config.cpp
    src/simconnect_client.cpp
    src/udp_sender.cpp
    src/serial_transport.cpp
    src/can_frame.cpp
    src/debug_console.cpp
    src/file_logger.cpp
    src/error_logger.cpp
    src/monitor_forwarder.cpp
    src/configure_mode.cpp
)

# bridge.ini neben bridge.exe deployen
add_custom_command(TARGET bridge POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/bridge.ini"
        "$<TARGET_FILE_DIR:bridge>/bridge.ini"
    COMMENT "Copying bridge.ini to output directory")

target_include_directories(bridge PRIVATE
    src
    "${SIMCONNECT_INCLUDE}"
)

target_link_directories(bridge PRIVATE "${SIMCONNECT_LIB_DIR}")

target_link_libraries(bridge PRIVATE
    SimConnect
    ws2_32
)

target_compile_definitions(bridge PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
)

set_target_properties(bridge PROPERTIES OUTPUT_NAME "bridge")

if(SIMCONNECT_DLL)
    add_custom_command(TARGET bridge POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${SIMCONNECT_DLL}"
            "$<TARGET_FILE_DIR:bridge>/SimConnect.dll"
        COMMENT "Copying SimConnect.dll next to bridge executable")
else()
    message(WARNING
        "SimConnect.dll was not found. bridge.exe may fail to start unless SimConnect.dll is available in PATH or executable directory.")
endif()

set(THIRD_PARTY_NOTICE_FILE "${CMAKE_SOURCE_DIR}/../THIRD_PARTY_NOTICES.txt")
if(EXISTS "${THIRD_PARTY_NOTICE_FILE}")
    add_custom_command(TARGET bridge POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${THIRD_PARTY_NOTICE_FILE}"
            "$<TARGET_FILE_DIR:bridge>/THIRD_PARTY_NOTICES.txt"
        COMMENT "Copying THIRD_PARTY_NOTICES.txt to output directory")
endif()
