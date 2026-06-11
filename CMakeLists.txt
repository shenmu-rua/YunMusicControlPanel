cmake_minimum_required(VERSION 3.10)
project(YunMusicPlugin C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Output DLL directly to project root for easy testing
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# Source files
set(SOURCES
    src/plugin.c
    src/config.c
    src/api_bot.c
    src/api_netease.c
    src/lrc_parser.c
    src/ui_win32.c
    src/cJSON.c
    resources/yunmusic.rc
)

# Create DLL (shared library)
add_library(YunMusicPlugin SHARED ${SOURCES})

# Include directories
# SDK headers use: #include "teamspeak/public_definitions.h" and #include "ts3_functions.h"
# So the include path must be include/teamspeak/ to resolve both patterns
target_include_directories(YunMusicPlugin PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include/teamspeak
    ${CMAKE_CURRENT_SOURCE_DIR}/include/cjson
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/resources
)

# MSVC-specific settings
if(MSVC)
    target_compile_definitions(YunMusicPlugin PRIVATE _CRT_SECURE_NO_WARNINGS)
    # Force UTF-8 source encoding and C11 mode
    target_compile_options(YunMusicPlugin PRIVATE /utf-8 /std:c11)
endif()

# Link Windows libraries
target_link_libraries(YunMusicPlugin PRIVATE
    winhttp
    comctl32
    user32
    gdi32
    kernel32
    ole32
)

# Define UNICODE for Win32 API
target_compile_definitions(YunMusicPlugin PRIVATE UNICODE _UNICODE)

# Compiler warnings
if(MINGW)
    target_compile_options(YunMusicPlugin PRIVATE -Wall -Wno-unused-variable -Wno-unused-function)
endif()

# Install (for manual deployment)
install(TARGETS YunMusicPlugin RUNTIME DESTINATION ".")
install(FILES yunmusic.ini DESTINATION ".")
