add_library(gateway STATIC
    offline_buffer.cpp
    telemetry_publisher.cpp   # MQTT publish loop — Paho C++ async client, QoS 2, nlohmann JSON
    command_handler.cpp       # MQTT subscribe loop — shares telemetry_publisher's mqtt client
)

target_include_directories(gateway
    PUBLIC  ${CMAKE_SOURCE_DIR}/include   # our API headers — propagates to all consumers
    PRIVATE ${SQLITE3_INCLUDE}            # sqlite3.h — only offline_buffer.cpp needs it
    PRIVATE ${PAHO_INCLUDE}              # mqtt/ — telemetry_publisher.cpp + command_handler.cpp
    PRIVATE ${CMAKE_SOURCE_DIR}/include/third_party  # rapidjson — internal to gateway only
)

target_link_libraries(gateway PRIVATE
    can_layer               # command_handler.cpp calls CanWriter (can_layer/can_writer.hpp)
    ${SQLITE3_LIB}          # offline_buffer.cpp only
    ${PAHO_MQTT_C_LIB}      # C core — telemetry_publisher.cpp + command_handler.cpp
    ${PAHO_MQTT_CPP_LIB}    # C++ wrapper — telemetry_publisher.cpp + command_handler.cpp
    OpenSSL::SSL            # TLS for MQTT connection — internal to gateway
    OpenSSL::Crypto         # crypto ops — internal to gateway
    Threads::Threads        # std::thread in publisher/handler loops
)

# Link real DLT on Pi only
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(gateway PRIVATE automotive-dlt)
endif()
