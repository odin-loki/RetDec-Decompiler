add_library(retdec-jvm-reconstruct
    stack_sim.cpp
    slot_coalesce.cpp
    local_rebuild.cpp
    pattern_lift.cpp
    jvm_reconstruct.cpp
)

target_include_directories(retdec-jvm-reconstruct
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
)

target_link_libraries(retdec-jvm-reconstruct
    PUBLIC retdec-bc-module
)

set_target_properties(retdec-jvm-reconstruct PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

install(TARGETS retdec-jvm-reconstruct
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/include/retdec/jvm_reconstruct
    DESTINATION include/retdec
)
