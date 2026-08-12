cmake_minimum_required(VERSION 3.10)
project(YunMusicPlugin VERSION 1.2.15 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Output the DLL under the selected build configuration directory.
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# Source files
set(SOURCES
    src/plugin.c
    src/config.c
    src/auth_store.c
    src/cookie_jar.c
    src/daily_policy.c
    src/api_bot.c
    src/bot_queue_parser.c
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

target_compile_definitions(YunMusicPlugin PRIVATE
    YUNMUSIC_VERSION="${PROJECT_VERSION}"
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
    crypt32
)

# Define UNICODE for Win32 API
target_compile_definitions(YunMusicPlugin PRIVATE UNICODE _UNICODE)

# Keep runtime assets beside the DLL for local testing.
add_custom_command(TARGET YunMusicPlugin POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/YunMusicPlugin.png"
        "$<TARGET_FILE_DIR:YunMusicPlugin>/YunMusicPlugin.png"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/yunmusic.ini.example"
        "$<TARGET_FILE_DIR:YunMusicPlugin>/yunmusic.ini.example"
)

# Build a TeamSpeak Package Installer archive on every normal build. Keep the
# version in the filename so test packages cannot be confused with older DLLs.
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/package.ini.in"
    "${CMAKE_CURRENT_BINARY_DIR}/package.ini"
    @ONLY
)
set(TS3_PACKAGE_FILENAME
     "YunMusicPlugin-v${PROJECT_VERSION}-win64.ts3_plugin")
add_custom_command(TARGET YunMusicPlugin POST_BUILD
    COMMAND ${CMAKE_COMMAND}
        "-DPLUGIN_DLL=$<TARGET_FILE:YunMusicPlugin>"
        "-DPLUGIN_ICON=${CMAKE_CURRENT_SOURCE_DIR}/resources/YunMusicPlugin.png"
        "-DPLUGIN_CONFIG=${CMAKE_CURRENT_SOURCE_DIR}/yunmusic.ini.example"
        "-DPACKAGE_INI=${CMAKE_CURRENT_BINARY_DIR}/package.ini"
        "-DSTAGING_DIR=${CMAKE_CURRENT_BINARY_DIR}/ts3_package/$<CONFIG>"
        "-DOUTPUT_FILE=$<TARGET_FILE_DIR:YunMusicPlugin>/${TS3_PACKAGE_FILENAME}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/package_ts3_plugin.cmake"
    COMMENT "Creating ${TS3_PACKAGE_FILENAME}"
    VERBATIM
)

# Compiler warnings
if(MINGW)
    target_compile_options(YunMusicPlugin PRIVATE -Wall -Wno-unused-variable -Wno-unused-function)
endif()

# Pure-C parser tests do not require TeamSpeak or Windows APIs.
include(CTest)
if(BUILD_TESTING)
    add_executable(lrc_parser_tests
        tests/lrc_parser_tests.c
        src/lrc_parser.c
    )
    target_include_directories(lrc_parser_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    if(MSVC)
        target_compile_options(lrc_parser_tests PRIVATE /utf-8 /std:c11)
    endif()
    add_test(NAME lrc_parser_tests COMMAND lrc_parser_tests)

    add_executable(daily_policy_tests
        tests/daily_policy_tests.c
        src/daily_policy.c
    )
    target_include_directories(daily_policy_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    if(MSVC)
        target_compile_options(daily_policy_tests PRIVATE /utf-8 /std:c11)
    endif()
    add_test(NAME daily_policy_tests COMMAND daily_policy_tests)

    add_executable(cookie_jar_tests
        tests/cookie_jar_tests.c
        src/cookie_jar.c
    )
    target_include_directories(cookie_jar_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    if(MSVC)
        target_compile_definitions(cookie_jar_tests PRIVATE
            _CRT_SECURE_NO_WARNINGS)
        target_compile_options(cookie_jar_tests PRIVATE /utf-8 /std:c11)
    endif()
    add_test(NAME cookie_jar_tests COMMAND cookie_jar_tests)

    add_executable(bot_queue_parser_tests
        tests/bot_queue_parser_tests.c
        src/bot_queue_parser.c
    )
    target_include_directories(bot_queue_parser_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    if(MSVC)
        target_compile_definitions(bot_queue_parser_tests PRIVATE
            _CRT_SECURE_NO_WARNINGS)
        target_compile_options(bot_queue_parser_tests PRIVATE /utf-8 /std:c11)
    endif()
    add_test(NAME bot_queue_parser_tests COMMAND bot_queue_parser_tests)

    if(WIN32)
        add_executable(auth_store_tests
            tests/auth_store_tests.c
            src/auth_store.c
        )
        target_include_directories(auth_store_tests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
        )
        target_link_libraries(auth_store_tests PRIVATE crypt32)
        if(MSVC)
            target_compile_definitions(auth_store_tests PRIVATE
                _CRT_SECURE_NO_WARNINGS)
            target_compile_options(auth_store_tests PRIVATE /utf-8 /std:c11)
        endif()
        add_test(NAME auth_store_tests COMMAND auth_store_tests)

        add_executable(api_netease_security_tests
            tests/api_netease_security_tests.c
            src/api_netease.c
            src/cookie_jar.c
            src/cJSON.c
        )
        target_include_directories(api_netease_security_tests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/include/cjson
        )
        target_link_libraries(api_netease_security_tests PRIVATE winhttp)
        if(MSVC)
            target_compile_definitions(api_netease_security_tests PRIVATE
                _CRT_SECURE_NO_WARNINGS)
            target_compile_options(api_netease_security_tests PRIVATE
                /utf-8 /std:c11)
        endif()
        add_test(NAME api_netease_security_tests
            COMMAND api_netease_security_tests)

        add_executable(api_bot_unavailable_tests
            tests/api_bot_unavailable_tests.c
            src/api_bot.c
            src/bot_queue_parser.c
            src/cJSON.c
        )
        target_include_directories(api_bot_unavailable_tests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/include/cjson
        )
        target_link_libraries(api_bot_unavailable_tests PRIVATE winhttp)
        if(MSVC)
            target_compile_definitions(api_bot_unavailable_tests PRIVATE
                _CRT_SECURE_NO_WARNINGS)
            target_compile_options(api_bot_unavailable_tests PRIVATE
                /utf-8 /std:c11)
        endif()
        add_test(NAME api_bot_unavailable_tests
            COMMAND api_bot_unavailable_tests)
    endif()
endif()

# Install (for manual deployment)
install(TARGETS YunMusicPlugin RUNTIME DESTINATION ".")
install(FILES
    resources/YunMusicPlugin.png
    DESTINATION "."
)
install(FILES
    yunmusic.ini.example
    DESTINATION "."
)
