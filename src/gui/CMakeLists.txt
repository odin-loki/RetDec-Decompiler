# retdec-gui on FuseUI (third_party/fuseui) and Forge (third_party/forge).
#
#   retdec-gui-model  src/gui/model  -- no UI: artifacts, launching the decompiler,
#                                       projects, settings, export, graph layout,
#                                       filtering. Pure C++23 on Forge; unit-tested
#                                       without a window.
#   retdec-gui-ui     src/gui/ui     -- FuseUI views (main window, panels, dialogs).
#                                       Testable headless: a UiContext needs no
#                                       window, and CpuRasterizer renders frames.
#   retdec-gui        src/gui/app    -- the executable (native window, or headless
#                                       with RETDEC_GUI_HEADLESS=1).
#
# Directories are globbed so a new model/view file needs no edit here.

get_filename_component(RETDEC_GUI_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

# Resources that ship next to the binary (share/retdec/gui) and are found at run
# time, the way decompiler-config.json is.
set(RETDEC_GUI_RESOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/resources")

# ── Model ────────────────────────────────────────────────────────────────────
file(GLOB RETDEC_GUI_MODEL_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/model/*.cpp")
add_library(retdec-gui-model STATIC ${RETDEC_GUI_MODEL_SOURCES})
target_include_directories(retdec-gui-model PUBLIC ${RETDEC_GUI_ROOT}/include)
target_link_libraries(retdec-gui-model PUBLIC retdec::forge Threads::Threads)
target_compile_features(retdec-gui-model PUBLIC cxx_std_23)
target_compile_definitions(retdec-gui-model PRIVATE
	RETDEC_GUI_SOURCE_RESOURCE_DIR="${RETDEC_GUI_RESOURCE_DIR}")
if(TARGET retdec::neural)
	target_link_libraries(retdec-gui-model PUBLIC retdec::neural)
	target_compile_definitions(retdec-gui-model PUBLIC RETDEC_GUI_HAS_NEURAL)
endif()

# ── Views ────────────────────────────────────────────────────────────────────
file(GLOB RETDEC_GUI_UI_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/ui/*.cpp")
add_library(retdec-gui-ui STATIC ${RETDEC_GUI_UI_SOURCES})
target_link_libraries(retdec-gui-ui PUBLIC retdec-gui-model fuse_ui_widgets fuse_ui_doc fuse_app)
target_compile_features(retdec-gui-ui PUBLIC cxx_std_23)

# ── Executable ───────────────────────────────────────────────────────────────
add_executable(retdec-gui WIN32 MACOSX_BUNDLE app/main.cpp)
target_link_libraries(retdec-gui PRIVATE retdec-gui-ui fuse_app_native)
set_target_properties(retdec-gui PROPERTIES
	OUTPUT_NAME "retdec-gui"
	MACOSX_BUNDLE_BUNDLE_NAME "RetDec"
	MACOSX_BUNDLE_GUI_IDENTIFIER "com.imortek.retdec"
	MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}")
if(MSVC)
	# WIN32 subsystem, but main() is the entry point on every platform.
	target_link_options(retdec-gui PRIVATE /ENTRY:mainCRTStartup)
endif()

# Stage resources next to the build-tree binary as they are installed, so a
# build-tree run finds them in ../share/retdec/gui.
add_custom_command(TARGET retdec-gui POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:retdec-gui>/../share/retdec/gui"
	COMMAND ${CMAKE_COMMAND} -E copy_directory "${RETDEC_GUI_RESOURCE_DIR}"
		"$<TARGET_FILE_DIR:retdec-gui>/../share/retdec/gui"
	COMMENT "Staging retdec-gui resources")

install(TARGETS retdec-gui
	BUNDLE DESTINATION .
	RUNTIME DESTINATION bin)
install(DIRECTORY "${RETDEC_GUI_RESOURCE_DIR}/" DESTINATION share/retdec/gui)
