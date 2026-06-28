add_library(analytics STATIC
    anomaly_detector.cpp
)

target_include_directories(analytics PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
)

# ONNX Runtime C++ (sudo apt install libonnxruntime-dev  OR  install from GitHub releases)
find_library(ORT_LIB onnxruntime REQUIRED)
find_path(ORT_INCLUDE onnxruntime_cxx_api.h PATH_SUFFIXES onnxruntime REQUIRED)

target_include_directories(analytics PUBLIC ${ORT_INCLUDE})
target_link_libraries(analytics PRIVATE ${ORT_LIB})
