cmake_minimum_required(VERSION 3.26)

project(retdec-gui-stub
    LANGUAGES C CXX
    VERSION 0.1.0
    DESCRIPTION "RetDec Qt6 GUI — stub / minimal build"
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)

option(RETDEC_SUPERBUILD_ENABLE_QT6 "Try to build GUI with Qt6 if available" ON)

set(_qt_components Widgets)
if(RETDEC_SUPERBUILD_ENABLE_QT6)
    find_package(Qt6 COMPONENTS ${_qt_components} QUIET)
endif()

if(RETDEC_SUPERBUILD_ENABLE_QT6 AND Qt6Widgets_FOUND)
    # Real Qt6 stub — shows a placeholder window, supports --self-test.
    add_executable(retdec-gui
        retdec-gui-qt.cpp
    )
    set_target_properties(retdec-gui PROPERTIES
        AUTOMOC ON
        AUTOUIC ON
        WIN32_EXECUTABLE TRUE   # suppress console window on Windows
    )
    target_compile_features(retdec-gui PUBLIC cxx_std_23)
    target_link_libraries(retdec-gui PRIVATE Qt6::Widgets)
    target_compile_definitions(retdec-gui PRIVATE RETDEC_GUI_USE_QT=1)
    message(STATUS "retdec-gui: building with Qt6 ${Qt6Widgets_VERSION}")
else()
    # No-Qt fallback — console stub, still supports --self-test for Wine CI.
    add_executable(retdec-gui
        retdec-gui-stub.cpp
    )
    target_compile_features(retdec-gui PUBLIC cxx_std_23)
    message(STATUS "retdec-gui: Qt6 not found — building console stub")
endif()

install(
    TARGETS retdec-gui
    EXPORT  retdec-gui-targets
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
