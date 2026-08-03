foreach(required_var
        PLUGIN_DLL PLUGIN_ICON PLUGIN_CONFIG PACKAGE_INI STAGING_DIR OUTPUT_FILE)
    if(NOT DEFINED ${required_var} OR "${${required_var}}" STREQUAL "")
        message(FATAL_ERROR "Missing required variable: ${required_var}")
    endif()
endforeach()

foreach(required_file
        "${PLUGIN_DLL}" "${PLUGIN_ICON}" "${PLUGIN_CONFIG}" "${PACKAGE_INI}")
    if(NOT EXISTS "${required_file}")
        message(FATAL_ERROR "Package input does not exist: ${required_file}")
    endif()
endforeach()

file(REMOVE_RECURSE "${STAGING_DIR}")
file(MAKE_DIRECTORY "${STAGING_DIR}/plugins")
file(COPY "${PACKAGE_INI}" DESTINATION "${STAGING_DIR}")
file(COPY "${PLUGIN_DLL}" "${PLUGIN_ICON}" "${PLUGIN_CONFIG}"
    DESTINATION "${STAGING_DIR}/plugins")

get_filename_component(output_dir "${OUTPUT_FILE}" DIRECTORY)
file(MAKE_DIRECTORY "${output_dir}")
file(REMOVE "${OUTPUT_FILE}")

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E tar cf "${OUTPUT_FILE}"
        --format=zip package.ini plugins
    WORKING_DIRECTORY "${STAGING_DIR}"
    RESULT_VARIABLE archive_result
    ERROR_VARIABLE archive_error
)
if(NOT archive_result EQUAL 0)
    message(FATAL_ERROR
        "Failed to create TeamSpeak plugin package: ${archive_error}")
endif()
