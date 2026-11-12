# FUSE UI — retained-mode GUI / application framework (docs/plans/FUSE_UI_PLAN.md, docs/ui/README.md).
# Builds inside the FUSE tree (Source/FUSE/CMakeLists.txt) or standalone for fast iteration:
#   cmake -S Source/FUSE/UI -B build-ui -G Ninja && cmake --build build-ui && ctest --test-dir build-ui
# fuse_text and fuse_ui_core depend only on the standard library and vendored stb (not fuse_core), so the
# toolkit can be built and tested without the engine.
cmake_minimum_required(VERSION 3.21)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(FUSE_UI LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 23)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    enable_testing()
endif()

get_filename_component(FUSE_UI_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)

option(FUSE_BUILD_UI_WIDGETS "Build fuse_ui_widgets (Tier A widgets)" ON)
option(FUSE_BUILD_UI_TESTS "Build FUSE UI gate tests" ON)
option(FUSE_BUILD_UI_GAME "Build fuse_ui_game (UI7: HUD, world-space UI, gamepad navigation)" ON)
option(FUSE_BUILD_APP "Build fuse_app (application framework; UI9, not yet)" OFF)
option(FUSE_BUILD_UI_DESIGNER "Build fuse_ui_designer (UI8 prototype: library, gate and app)" ON)
option(FUSE_UI_ACCESSIBILITY "Accessibility tree (AccessKit bridge later, D7)" ON)
set(FUSE_UI_TEXT "MINIMAL" CACHE STRING "Text profile: FULL (HarfBuzz + FreeType, fetched), MINIMAL (stb_truetype), COOKED (not yet)")
set_property(CACHE FUSE_UI_TEXT PROPERTY STRINGS FULL MINIMAL COOKED)
set(FUSE_UI_PROFILE "app" CACHE STRING "UI profile: hud, app, editor")
if(FUSE_UI_TEXT STREQUAL "COOKED")
    message(STATUS "FUSE UI: text profile COOKED not implemented yet — using MINIMAL")
endif()

# ---- fuse_text ----
add_library(fuse_text STATIC Text/src/text.cpp)
target_include_directories(fuse_text PUBLIC Text/include)
target_include_directories(fuse_text SYSTEM PRIVATE "${FUSE_UI_REPO_ROOT}/vendor/stb")
target_compile_definitions(fuse_text PRIVATE
    FUSE_TEXT_BUNDLED_FONT="${FUSE_UI_REPO_ROOT}/vendor/fonts/opensans/OpenSans.ttf"
    FUSE_TEXT_FONT_DIR="${FUSE_UI_REPO_ROOT}/vendor/fonts")
target_compile_features(fuse_text PUBLIC cxx_std_23)

# FULL profile (decision D2): HarfBuzz + FreeType through FetchContent (static, no optional deps), SheenBidi (UAX #9)
# and libunibreak (UAX #14) fetched and compiled from their C sources.
if(FUSE_UI_TEXT STREQUAL "FULL")
    include(FetchContent)
    set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
    set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
    set(HB_HAVE_FREETYPE OFF CACHE BOOL "" FORCE)
    set(HB_BUILD_SUBSET OFF CACHE BOOL "" FORCE)
    set(HB_BUILD_UTILS OFF CACHE BOOL "" FORCE)
    set(HB_HAVE_GLIB OFF CACHE BOOL "" FORCE)
    set(HB_HAVE_ICU OFF CACHE BOOL "" FORCE)
    set(SKIP_INSTALL_ALL ON CACHE BOOL "" FORCE)
    FetchContent_Declare(fuse_freetype GIT_REPOSITORY https://gitlab.freedesktop.org/freetype/freetype.git
        GIT_TAG VER-2-13-3 GIT_SHALLOW TRUE EXCLUDE_FROM_ALL)
    FetchContent_Declare(fuse_harfbuzz GIT_REPOSITORY https://github.com/harfbuzz/harfbuzz.git
        GIT_TAG 10.1.0 GIT_SHALLOW TRUE EXCLUDE_FROM_ALL)
    set(_fuse_bsl ${BUILD_SHARED_LIBS})
    set(BUILD_SHARED_LIBS OFF)
    FetchContent_MakeAvailable(fuse_freetype fuse_harfbuzz)
    set(BUILD_SHARED_LIBS ${_fuse_bsl})
    FetchContent_Declare(fuse_sheenbidi GIT_REPOSITORY https://github.com/Tehreer/SheenBidi.git
        GIT_TAG v2.7 GIT_SHALLOW TRUE SOURCE_SUBDIR fuse_no_cmake)
    FetchContent_Declare(fuse_unibreak GIT_REPOSITORY https://github.com/adah1972/libunibreak.git
        GIT_TAG libunibreak_6_1 GIT_SHALLOW TRUE SOURCE_SUBDIR fuse_no_cmake)
    # SOURCE_SUBDIR names a directory that does not exist: download only, the C sources are compiled below.
    FetchContent_MakeAvailable(fuse_sheenbidi fuse_unibreak)
    enable_language(C)
    add_library(fuse_sheenbidi STATIC "${fuse_sheenbidi_SOURCE_DIR}/Source/SheenBidi.c")
    target_compile_definitions(fuse_sheenbidi PRIVATE SB_CONFIG_UNITY)
    target_include_directories(fuse_sheenbidi SYSTEM PUBLIC "${fuse_sheenbidi_SOURCE_DIR}/Headers")
    target_include_directories(fuse_sheenbidi PRIVATE "${fuse_sheenbidi_SOURCE_DIR}/Source")
    set(_ub "${fuse_unibreak_SOURCE_DIR}/src")
    add_library(fuse_unibreak STATIC ${_ub}/linebreak.c ${_ub}/linebreakdata.c ${_ub}/linebreakdef.c
        ${_ub}/unibreakbase.c ${_ub}/unibreakdef.c ${_ub}/eastasianwidthdef.c ${_ub}/emojidef.c)
    target_include_directories(fuse_unibreak SYSTEM PUBLIC "${_ub}")
    set_target_properties(fuse_sheenbidi fuse_unibreak PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_sources(fuse_text PRIVATE Text/src/text_full.cpp)
    target_compile_definitions(fuse_text PRIVATE FUSE_TEXT_FULL=1)
    target_link_libraries(fuse_text PRIVATE harfbuzz freetype fuse_sheenbidi fuse_unibreak)
endif()

# ---- fuse_ui_core ----
add_library(fuse_ui_core STATIC Core/src/style.cpp Core/src/draw_list.cpp Core/src/ui.cpp Core/src/immediate.cpp)
target_include_directories(fuse_ui_core PUBLIC Core/include)
target_link_libraries(fuse_ui_core PUBLIC fuse_text)
if(FUSE_UI_ACCESSIBILITY)
    target_compile_definitions(fuse_ui_core PUBLIC FUSE_UI_ACCESSIBILITY=1)
endif()

if(FUSE_BUILD_UI_WIDGETS)
    add_library(fuse_ui_widgets STATIC Widgets/src/widgets.cpp Widgets/src/widgets_tier_b.cpp Widgets/src/widgets_tier_b2.cpp Widgets/src/widgets_tier_cd.cpp Widgets/src/widgets_tier_d2.cpp Widgets/src/platform_feel.cpp Widgets/src/theme.cpp Widgets/src/inspector.cpp)
    target_include_directories(fuse_ui_widgets PUBLIC Widgets/include)
    target_link_libraries(fuse_ui_widgets PUBLIC fuse_ui_core)
endif()

# ---- fuse_ui_doc: .fuseui JSON documents, binding model and expression bytecode ----
# fuse_ui_sql: SqliteModel, an ItemModel over vendor/sqlite (lazy windowed row cache)
option(FUSE_BUILD_UI_SQL "Build fuse_ui_sql (SQLite item model over vendor/sqlite)" ON)
if(FUSE_BUILD_UI_SQL AND FUSE_BUILD_UI_WIDGETS AND EXISTS "${FUSE_UI_REPO_ROOT}/vendor/sqlite/sqlite3.c")
    enable_language(C)
    add_library(fuse_ui_sqlite3 STATIC "${FUSE_UI_REPO_ROOT}/vendor/sqlite/sqlite3.c")
    set_target_properties(fuse_ui_sqlite3 PROPERTIES LINKER_LANGUAGE C)
    target_include_directories(fuse_ui_sqlite3 PUBLIC "${FUSE_UI_REPO_ROOT}/vendor/sqlite")
    target_compile_definitions(fuse_ui_sqlite3 PRIVATE SQLITE_THREADSAFE=0 SQLITE_OMIT_LOAD_EXTENSION=1 SQLITE_DQS=0)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(fuse_ui_sqlite3 PRIVATE -w)
    endif()
    add_library(fuse_ui_sql STATIC Sql/src/sqlite_model.cpp)
    target_include_directories(fuse_ui_sql PUBLIC Sql/include)
    target_link_libraries(fuse_ui_sql PUBLIC fuse_ui_widgets PRIVATE fuse_ui_sqlite3)
    set(FUSE_UI_HAVE_SQL ON)
endif()

# fuse_ui_a11y: accessibility bridge (snapshot / diff / actions); AccessKit backend when FUSE_UI_ACCESSKIT_DIR
# points at an accesskit-c build (include/accesskit.h + lib/libaccesskit.a; needs a Rust toolchain to produce).
if(FUSE_UI_ACCESSIBILITY)
    set(FUSE_UI_ACCESSKIT_DIR "" CACHE PATH "accesskit-c prefix (include/, lib/) for the AccessKit a11y backend; empty = none")
    add_library(fuse_ui_a11y STATIC A11y/src/a11y.cpp A11y/src/a11y_accesskit.cpp)
    target_include_directories(fuse_ui_a11y PUBLIC A11y/include)
    target_link_libraries(fuse_ui_a11y PUBLIC fuse_ui_core)
    find_library(FUSE_UI_ACCESSKIT_LIB NAMES accesskit PATHS "${FUSE_UI_ACCESSKIT_DIR}/lib" NO_DEFAULT_PATH)
    if(FUSE_UI_ACCESSKIT_DIR AND FUSE_UI_ACCESSKIT_LIB AND EXISTS "${FUSE_UI_ACCESSKIT_DIR}/include/accesskit.h")
        target_include_directories(fuse_ui_a11y PRIVATE "${FUSE_UI_ACCESSKIT_DIR}/include")
        target_compile_definitions(fuse_ui_a11y PUBLIC FUSE_UI_HAVE_ACCESSKIT=1)
        find_package(Threads REQUIRED)
        target_link_libraries(fuse_ui_a11y PRIVATE "${FUSE_UI_ACCESSKIT_LIB}" Threads::Threads ${CMAKE_DL_LIBS} m)
        message(STATUS "FUSE UI: AccessKit backend from ${FUSE_UI_ACCESSKIT_DIR}")
    else()
        target_compile_definitions(fuse_ui_a11y PUBLIC FUSE_UI_HAVE_ACCESSKIT=0)
    endif()
endif()

option(FUSE_BUILD_UI_DOC "Build fuse_ui_doc (.fuseui documents + bindings)" ON)
if(FUSE_BUILD_UI_DOC AND FUSE_BUILD_UI_WIDGETS)
    add_library(fuse_ui_doc STATIC Doc/src/json.cpp Doc/src/document.cpp Doc/src/cook.cpp)
    target_include_directories(fuse_ui_doc PUBLIC Doc/include)
    target_link_libraries(fuse_ui_doc PUBLIC fuse_ui_widgets)
    # UI8 cooker: .fuseui / .fusestyle -> cooked blobs + generated widget-registration list
    add_executable(fuse_ui_cook Doc/tools/fuse_ui_cook.cpp)
    target_link_libraries(fuse_ui_cook PRIVATE fuse_ui_doc)
endif()

# ---- fuse_ui_script: Lua bindings (vendored Lua 5.2, the fuse_script backend) ----
# fuse_ui_video: Theora decode for the video widget (vendored libtheora 1.2.0 + libogg 1.3.6, BSD-3-Clause)
option(FUSE_BUILD_UI_THEORA "Build fuse_ui_video (Theora .ogv decode for the video widget)" ON)
if(FUSE_BUILD_UI_THEORA AND FUSE_BUILD_UI_WIDGETS)
    add_subdirectory(Video)
endif()

# fuse_ui_designer (UI8 prototype): DesignDocument model + Designer UI over fuse_ui_doc
if(FUSE_BUILD_UI_DESIGNER AND FUSE_BUILD_UI_WIDGETS AND FUSE_BUILD_UI_DOC)
    add_library(fuse_ui_designer_lib STATIC Designer/src/designer.cpp)
    target_include_directories(fuse_ui_designer_lib PUBLIC Designer/include)
    target_link_libraries(fuse_ui_designer_lib PUBLIC fuse_ui_doc)
endif()

option(FUSE_BUILD_UI_SCRIPT "Build fuse_ui_script (Lua bindings)" ON)
if(FUSE_BUILD_UI_SCRIPT AND TARGET fuse_ui_doc)
    if(NOT TARGET fuse_lua AND EXISTS "${FUSE_UI_REPO_ROOT}/vendor/lua/CMakeLists.txt")
        add_subdirectory("${FUSE_UI_REPO_ROOT}/vendor/lua" "${CMAKE_CURRENT_BINARY_DIR}/vendor_lua")
    endif()
    if(TARGET fuse_lua)
        add_library(fuse_ui_script STATIC Script/src/lua_ui.cpp)
        target_include_directories(fuse_ui_script PUBLIC Script/include)
        target_link_libraries(fuse_ui_script PUBLIC fuse_ui_doc fuse_lua)
    endif()
endif()

# ---- fuse_ui_render (CPU packing, always) + fuse_ui_vk (GPU backend "ui.draw_list", needs a Vulkan loader) ----
add_library(fuse_ui_render STATIC Render/src/gpu_draw_data.cpp)
target_include_directories(fuse_ui_render PUBLIC Render/include)
target_link_libraries(fuse_ui_render PUBLIC fuse_ui_core)

option(FUSE_BUILD_UI_VK "Build fuse_ui_vk (Vulkan compute backend for the draw list)" ON)
if(FUSE_BUILD_UI_VK)
    if(NOT TARGET Vulkan::Vulkan)
        find_package(Vulkan QUIET)
    endif()
    if(NOT TARGET Vulkan::Vulkan)
        # Cross / conda toolchains often hide the host loader from FindVulkan; look for it directly.
        find_library(FUSE_UI_VULKAN_LOADER NAMES vulkan vulkan-1 libvulkan.so.1
                     HINTS ENV VULKAN_SDK PATHS /usr/lib/x86_64-linux-gnu /usr/lib64 /usr/lib PATH_SUFFIXES lib Lib)
        find_path(FUSE_UI_VULKAN_INCLUDE vulkan/vulkan.h HINTS ENV VULKAN_SDK PATHS /usr/include PATH_SUFFIXES include Include)
        if(FUSE_UI_VULKAN_LOADER AND FUSE_UI_VULKAN_INCLUDE)
            add_library(fuse_ui_vulkan_loader INTERFACE)
            target_link_libraries(fuse_ui_vulkan_loader INTERFACE "${FUSE_UI_VULKAN_LOADER}")
            # Cross / conda toolchains list the host /usr/include as implicit (CMake drops it) without searching it:
            # expose only the Vulkan headers through a private symlink dir.
            set(_vkinc "${CMAKE_CURRENT_BINARY_DIR}/ui_vk_headers")
            file(MAKE_DIRECTORY "${_vkinc}")
            foreach(_d vulkan vk_video)
                if(EXISTS "${FUSE_UI_VULKAN_INCLUDE}/${_d}" AND NOT EXISTS "${_vkinc}/${_d}")
                    file(CREATE_LINK "${FUSE_UI_VULKAN_INCLUDE}/${_d}" "${_vkinc}/${_d}" SYMBOLIC)
                endif()
            endforeach()
            target_include_directories(fuse_ui_vulkan_loader SYSTEM INTERFACE "${_vkinc}")
            set(_fuse_ui_vk_link fuse_ui_vulkan_loader)
        endif()
    else()
        set(_fuse_ui_vk_link Vulkan::Vulkan)
    endif()
    find_program(FUSE_UI_GLSLANG NAMES glslangValidator HINTS ENV VULKAN_SDK PATH_SUFFIXES bin Bin)
    if(_fuse_ui_vk_link)
        set(_gen "${CMAKE_CURRENT_BINARY_DIR}/ui_vk_gen")
        file(MAKE_DIRECTORY "${_gen}")
        set(_src "${CMAKE_CURRENT_SOURCE_DIR}/Render/src/shaders/ui_draw_list.comp")
        set(_fixture "${CMAKE_CURRENT_SOURCE_DIR}/Render/src/shaders/fixtures/ui_draw_list.comp.spv")
        if(FUSE_UI_GLSLANG)
            set(_spv "${_gen}/ui_draw_list.comp.spv")
            add_custom_command(OUTPUT "${_spv}"
                COMMAND "${FUSE_UI_GLSLANG}" --target-env vulkan1.1 "${_src}" -o "${_spv}"
                DEPENDS "${_src}" COMMENT "glslangValidator ui_draw_list.comp" VERBATIM)
        elseif(EXISTS "${_fixture}")
            set(_spv "${_fixture}")
        endif()
        add_library(fuse_ui_vk STATIC Render/src/ui_vk.cpp)
        target_link_libraries(fuse_ui_vk PUBLIC fuse_ui_render PRIVATE ${_fuse_ui_vk_link})
        if(_spv)
            add_custom_command(OUTPUT "${_gen}/ui_draw_list_spv.h"
                COMMAND "${CMAKE_COMMAND}" -DSPV=${_spv} -DOUT=${_gen}/ui_draw_list_spv.h -DNAME=kUiDrawListSpv
                        -P "${CMAKE_CURRENT_SOURCE_DIR}/Render/cmake/embed_spv.cmake"
                DEPENDS "${_spv}" "${CMAKE_CURRENT_SOURCE_DIR}/Render/cmake/embed_spv.cmake" VERBATIM)
            target_sources(fuse_ui_vk PRIVATE "${_gen}/ui_draw_list_spv.h")
            target_include_directories(fuse_ui_vk PRIVATE "${_gen}")
        endif()
        set(FUSE_UI_VK_LINK ${_fuse_ui_vk_link} CACHE INTERNAL "")
        # In-tree with the Vulkan RHI: the render-graph pass wrapper (UiLayerGpu, next to hybrid::SpriteLayerGpu).
        if(TARGET fuse_rhi AND FUSE_BUILD_VULKAN)
            target_sources(fuse_ui_vk PRIVATE Render/src/ui_layer_gpu.cpp)
            target_link_libraries(fuse_ui_vk PUBLIC fuse_rhi)
            target_compile_definitions(fuse_ui_vk PUBLIC FUSE_HAS_VULKAN_RHI=1)
            # The runtime UI stage (UiFrameSource -> frame::FrameUiSource) when the frame composer is in the tree.
            if(TARGET fuse_frame)
                target_link_libraries(fuse_ui_vk PUBLIC fuse_frame)
                target_compile_definitions(fuse_ui_vk PUBLIC FUSE_UI_HAS_FRAME=1)
                # E02 gate: the SceneRenderer UI stage fed by UiFrameSource (Renderer/cmake/scene_renderer.cmake).
                if(TARGET fuse_scene_renderer_vk)
                    target_link_libraries(fuse_scene_renderer_vk PRIVATE fuse_ui_vk)
                    target_compile_definitions(fuse_scene_renderer_vk PRIVATE FUSE_SR_TEST_HAS_FUSE_UI=1)
                    set(_sr_lock "${CMAKE_BINARY_DIR}/Source/FUSE/Renderer/tests/run_vulkan_icd_locked.sh")
                    if(EXISTS "${_sr_lock}" OR FUSE_VULKAN_BACKEND)
                        add_test(NAME fuse_scene_renderer_vk_fuse_ui_t0
                                 COMMAND "${_sr_lock}" "$<TARGET_FILE:fuse_scene_renderer_vk>" --mode fuse_ui --tier t0)
                    else()
                        add_test(NAME fuse_scene_renderer_vk_fuse_ui_t0
                                 COMMAND fuse_scene_renderer_vk --mode fuse_ui --tier t0)
                    endif()
                    # UI7 HUD-less invariant: the UI stage never changes the HUD-less image / TAAU history.
                    if(EXISTS "${_sr_lock}" OR FUSE_VULKAN_BACKEND)
                        add_test(NAME fuse_ui_hudless_history_invariant COMMAND "${_sr_lock}"
                                 "$<TARGET_FILE:fuse_scene_renderer_vk>" --mode hudless_invariant --tier t0)
                    else()
                        add_test(NAME fuse_ui_hudless_history_invariant
                                 COMMAND fuse_scene_renderer_vk --mode hudless_invariant --tier t0)
                    endif()
                    set_tests_properties(fuse_scene_renderer_vk_fuse_ui_t0 fuse_ui_hudless_history_invariant PROPERTIES
                        ENVIRONMENT "VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json" RUN_SERIAL TRUE
                        SKIP_RETURN_CODE 77 TIMEOUT 1800 LABELS "gate;vulkan;renderer;scene_renderer;ui")
                endif()
            endif()
        endif()
    else()
        message(STATUS "FUSE UI: no Vulkan loader found, fuse_ui_vk skipped (CPU raster only)")
    endif()
endif()

# ---- fuse_app: application shell (UI5) ----
option(FUSE_BUILD_UI_APP "Build fuse_app (Application / AppWindow, on-demand loop) and its samples" ON)
if(FUSE_BUILD_UI_APP AND TARGET fuse_ui_doc)
    add_subdirectory(App)
endif()
if(TARGET fuse_ui_designer_lib AND TARGET fuse_app_native)
    add_executable(fuse_ui_designer Designer/app/designer_main.cpp)
    target_link_libraries(fuse_ui_designer PRIVATE fuse_ui_designer_lib fuse_app_native)
endif()

# ---- fuse_ui_game: game UI (UI7) — HUD widgets, world markers / panels, gamepad nav, input arbitration ----
if(FUSE_BUILD_UI_GAME AND FUSE_BUILD_UI_WIDGETS)
    add_library(fuse_ui_game STATIC Game/src/game_ui.cpp Game/src/game_extras.cpp)
    target_include_directories(fuse_ui_game PUBLIC Game/include)
    target_link_libraries(fuse_ui_game PUBLIC fuse_ui_widgets)
    # engine bridges (in tree only): UiSurface as an ECS component, theme sounds through fuse_audio
    if(TARGET fuse_ecs)
        add_library(fuse_ui_game_ecs STATIC Game/src/ecs_surface.cpp)
        target_link_libraries(fuse_ui_game_ecs PUBLIC fuse_ui_game fuse_ecs)
    endif()
    if(TARGET fuse_audio)
        add_library(fuse_ui_game_audio STATIC Game/src/ui_audio.cpp)
        target_link_libraries(fuse_ui_game_audio PUBLIC fuse_ui_game fuse_audio)
    endif()
    if(TARGET fuse_app)
        foreach(s hud_demo world_panel_demo)
            add_executable(${s} Game/samples/${s}.cpp)
            target_link_libraries(${s} PRIVATE fuse_ui_game fuse_app)
        endforeach()
    endif()
endif()

if(FUSE_BUILD_UI_TESTS AND FUSE_BUILD_UI_WIDGETS)
    add_subdirectory(tests)
endif()
