cmake_minimum_required(VERSION 3.22.1)
project(cpe_relay_android LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(BEDROCK_PROTOCOL_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(BEDROCK_PROTOCOL_CPP_BUILD_BOT OFF CACHE BOOL "" FORCE)
set(BEDROCK_PROTOCOL_CPP_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BEDROCK_PROTOCOL_CPP_INSTALL OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)

get_filename_component(
    BEDROCK_PROTOCOL_ROOT
    "${CMAKE_CURRENT_LIST_DIR}/../../../../../.."
    ABSOLUTE
)
add_subdirectory(
    "${BEDROCK_PROTOCOL_ROOT}"
    "${CMAKE_CURRENT_BINARY_DIR}/bedrock-protocol"
)

find_library(ANDROID_LOG_LIBRARY log REQUIRED)

add_library(
    bedrock_relay_android
    SHARED
    native_bridge.cpp
)

target_link_libraries(
    bedrock_relay_android
    PRIVATE
        BedrockProtocol::bedrock_protocol
        ${ANDROID_LOG_LIBRARY}
)

target_compile_options(
    bedrock_relay_android
    PRIVATE
        -Wall
        -Wextra
        -Wno-deprecated-declarations
)

# Android 15+ devices may use a 16 KiB page size. NDK r27 applies compatible
# segment alignment to this JNI library; the flag keeps it explicit for local
# command-line builds as well.
target_link_options(
    bedrock_relay_android
    PRIVATE "-Wl,-z,max-page-size=16384"
)
