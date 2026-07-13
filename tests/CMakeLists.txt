# Regression tests. Not part of the default `all` target output that ships to
# the Pi -- built on demand via `cmake --build . --target test_command_handler_shutdown`
# or run via `ctest`.

add_executable(test_command_handler_shutdown test_command_handler_shutdown.cpp)

target_include_directories(test_command_handler_shutdown PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${PAHO_INCLUDE}
)

target_link_libraries(test_command_handler_shutdown PRIVATE
    gateway
    can_layer
    ${PAHO_MQTT_C_LIB}
    ${PAHO_MQTT_CPP_LIB}
    ${SQLITE3_LIB}
    ${ONNXRUNTIME_LIB}
    CURL::libcurl
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND DLT_LIB)
    target_link_libraries(test_command_handler_shutdown PRIVATE ${DLT_LIB})
endif()

add_test(
    NAME command_handler_shutdown
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/run_command_handler_shutdown_test.sh
            $<TARGET_FILE:test_command_handler_shutdown>
)
