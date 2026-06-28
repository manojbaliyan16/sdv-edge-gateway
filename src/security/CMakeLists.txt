add_library(security STATIC
    tls_config.cpp
    token_auth.cpp
    firmware_verifier.cpp
)

target_include_directories(security PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(security PRIVATE
    OpenSSL::SSL
    OpenSSL::Crypto
)
