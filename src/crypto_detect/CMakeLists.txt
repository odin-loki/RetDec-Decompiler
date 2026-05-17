add_library(retdec-crypto-detect
    aes_detect.cpp
    sha_detect.cpp
    chacha_detect.cpp
    hmac_detect.cpp
    rsa_detect.cpp
    rc4_detect.cpp
    crypto_detector.cpp
)

target_include_directories(retdec-crypto-detect
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(retdec-crypto-detect
    PUBLIC
        retdec-ssa
)

set_target_properties(retdec-crypto-detect PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED YES
)

install(TARGETS retdec-crypto-detect ARCHIVE DESTINATION lib LIBRARY DESTINATION lib)
