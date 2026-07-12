add_library(gateway STATIC
    offline_buffer.cpp
    telemetry_publisher.cpp   # MQTT publish loop — Paho C++ async client, QoS 2, nlohmann JSON
    command_handler.cpp       # MQTT subscribe loop — shares telemetry_publisher's mqtt client
    ota_agent.cpp             # OTA flow: download (curl) → SHA256 → backup → flash → rollback
    anomaly_detector.cpp      # INT8 ONNX inference on CAN signals — publishes alerts via MQTT
)

target_include_directories(gateway
    PUBLIC  ${CMAKE_SOURCE_DIR}/include   # our API headers — propagates to all consumers
    PRIVATE ${SQLITE3_INCLUDE}            # sqlite3.h — only offline_buffer.cpp needs it
    PRIVATE ${PAHO_INCLUDE}              # mqtt/ — telemetry_publisher.cpp + command_handler.cpp
    PRIVATE ${CMAKE_SOURCE_DIR}/include/third_party  # rapidjson — internal to gateway only
    PRIVATE ${ONNXRUNTIME_INCLUDE}                  # onnxruntime_cxx_api.h — anomaly_detector.cpp
    PRIVATE ${DLT_INCLUDE}                          # dlt/dlt.h — real DLT on Linux only, empty var on Mac
)

target_link_libraries(gateway PRIVATE
    can_layer               # command_handler.cpp calls CanWriter (can_layer/can_writer.hpp)
    ${SQLITE3_LIB}          # offline_buffer.cpp only
    ${PAHO_MQTT_C_LIB}      # C core — telemetry_publisher.cpp + command_handler.cpp
    ${PAHO_MQTT_CPP_LIB}    # C++ wrapper — telemetry_publisher.cpp + command_handler.cpp
    CURL::libcurl           # HTTPS firmware download — ota_agent.cpp only
    ${ONNXRUNTIME_LIB}      # ONNX Runtime — anomaly_detector.cpp
    OpenSSL::SSL            # TLS for MQTT + curl peer verification
    OpenSSL::Crypto         # SHA256 hash verification — ota_agent.cpp
    Threads::Threads        # std::thread in publisher/handler/ota loops
)

# Link real DLT on Pi only, and only if it was actually found (root CMakeLists.txt
# sets DLT_LIB via find_library against the real libdlt.so — see note there about
# why find_package(automotive-dlt) was abandoned).
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND DLT_LIB)
    target_link_libraries(gateway PRIVATE ${DLT_LIB})
endif()
