add_library(retdec-pyc-parser
    pyc_magic.cpp
    py_code_object.cpp
    py_opcodes.cpp
    py_marshal.cpp
    pyc_reader.cpp
)

target_include_directories(retdec-pyc-parser
    PUBLIC
        ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(retdec-pyc-parser
    PUBLIC
        retdec-bc-module
)

set_target_properties(retdec-pyc-parser PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
)
