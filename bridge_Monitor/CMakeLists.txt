cmake_minimum_required(VERSION 3.16)
project(bridge_monitor CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(bridge_monitor
    src/main.cpp
    src/config.cpp
    src/udp_socket.cpp
    src/module_registry.cpp
    src/console_ui.cpp
)

# bridge.ini neben bridge_monitor.exe deployen
add_custom_command(TARGET bridge_monitor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/../bridge_exe/bridge.ini"
        "$<TARGET_FILE_DIR:bridge_monitor>/bridge.ini"
    COMMENT "Copying bridge.ini to output directory")

target_include_directories(bridge_monitor PRIVATE src)

target_link_libraries(bridge_monitor PRIVATE ws2_32)

target_compile_definitions(bridge_monitor PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
)

set_target_properties(bridge_monitor PROPERTIES OUTPUT_NAME "bridge_monitor")

set(THIRD_PARTY_NOTICE_FILE "${CMAKE_SOURCE_DIR}/../THIRD_PARTY_NOTICES.txt")
if(EXISTS "${THIRD_PARTY_NOTICE_FILE}")
    add_custom_command(TARGET bridge_monitor POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${THIRD_PARTY_NOTICE_FILE}"
            "$<TARGET_FILE_DIR:bridge_monitor>/THIRD_PARTY_NOTICES.txt"
        COMMENT "Copying THIRD_PARTY_NOTICES.txt to output directory")
endif()
