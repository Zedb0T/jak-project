# libjakopengoal - Embeddable Jak library (inspired by libsm64)
#
# Builds both a shared library (libjakopengoal.dll/.so) and a test program.

# ---- Shared Library ----
set(LIBJAKOPENGOAL_SOURCES
    src/libjakopengoal.cpp
    src/jak_bridge.cpp
    src/jak_bridge_state.cpp
    src/runtime_launcher.cpp
)

# Thin DLL for Blender — only IPC client, runtime runs in jak_server subprocess
add_library(jakopengoal_thin SHARED src/libjakopengoal_thin.cpp src/jak_ipc_shm.cpp)
target_compile_definitions(jakopengoal_thin PRIVATE JAK_LIB_EXPORT BUILD_LIBJAKOPENGOAL)
target_include_directories(jakopengoal_thin
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
    PRIVATE ${PROJECT_SOURCE_DIR}
)
# Minimal deps — NO runtime, NO common, NO fmt (pure Win32 API only)
if(WIN32)
    # No extra libs needed — uses kernel32 (CreateFileMapping, CreateEvent, etc.)
endif()
set_target_properties(jakopengoal_thin PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)

# jak_server.exe — subprocess that owns the GOAL runtime
if(BUILD_LIBJAKOPENGOAL)
    add_executable(jak_server
        src/jak_server_main.cpp
        src/jak_ipc_shm.cpp
        src/jak_bridge.cpp
        src/jak_bridge_state.cpp
        src/runtime_launcher.cpp
    )
    target_compile_definitions(jak_server PRIVATE BUILD_LIBJAKOPENGOAL RUNTIME_HEADLESS)
    target_include_directories(jak_server PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${PROJECT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/common
        ${PROJECT_SOURCE_DIR}/game
        ${PROJECT_SOURCE_DIR}/third-party
        ${PROJECT_SOURCE_DIR}/third-party/fmt/include
    )
    target_link_libraries(jak_server PRIVATE runtime_headless common fmt)
    if(WIN32)
        target_link_libraries(jak_server PRIVATE mman)
    else()
        target_link_libraries(jak_server PRIVATE pthread dl)
    endif()
    set_target_properties(jak_server PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
endif()

add_library(jakopengoal SHARED ${LIBJAKOPENGOAL_SOURCES})

target_compile_definitions(jakopengoal PRIVATE JAK_LIB_EXPORT BUILD_LIBJAKOPENGOAL)

target_include_directories(jakopengoal
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
    PRIVATE ${PROJECT_SOURCE_DIR}
    PRIVATE ${PROJECT_SOURCE_DIR}/common
    PRIVATE ${PROJECT_SOURCE_DIR}/game
    PRIVATE ${PROJECT_SOURCE_DIR}/third-party
    PRIVATE ${PROJECT_SOURCE_DIR}/third-party/fmt/include
)

# Link against the headless runtime (no graphics/display, safe for DLL loading in Blender)
target_link_libraries(jakopengoal PRIVATE runtime_headless common fmt)
if(WIN32)
    target_link_libraries(jakopengoal PRIVATE mman)
else()
    target_link_libraries(jakopengoal PRIVATE pthread dl)
endif()

# Also build a static lib version for easier integration
add_library(jakopengoal_static STATIC ${LIBJAKOPENGOAL_SOURCES})

target_compile_definitions(jakopengoal_static PUBLIC JAK_LIB_EXPORT BUILD_LIBJAKOPENGOAL)

target_include_directories(jakopengoal_static
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
    PRIVATE ${PROJECT_SOURCE_DIR}
    PRIVATE ${PROJECT_SOURCE_DIR}/common
    PRIVATE ${PROJECT_SOURCE_DIR}/game
    PRIVATE ${PROJECT_SOURCE_DIR}/third-party
    PRIVATE ${PROJECT_SOURCE_DIR}/third-party/fmt/include
)

target_link_libraries(jakopengoal_static PUBLIC runtime common fmt)
if(WIN32)
    target_link_libraries(jakopengoal_static PUBLIC mman)
else()
    target_link_libraries(jakopengoal_static PUBLIC pthread dl)
endif()

# ---- Test Program ----
add_executable(jak_test_app test/main.c)

target_include_directories(jak_test_app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Link against the static version for the test
target_link_libraries(jak_test_app PRIVATE jakopengoal_static)
if(WIN32)
    target_link_libraries(jak_test_app PRIVATE mman)
endif()

# ---- Combined test with libsm64 (optional) ----
option(WITH_LIBSM64 "Build combined test with libsm64" OFF)
if(WITH_LIBSM64)
    add_executable(jak_sm64_test test/main_combined.c)
    target_compile_definitions(jak_sm64_test PRIVATE WITH_LIBSM64)
    target_include_directories(jak_sm64_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LIBSM64_INCLUDE_DIR}
    )
    target_link_libraries(jak_sm64_test PRIVATE jakopengoal_static ${LIBSM64_LIBRARY})
    set_target_properties(jak_sm64_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
else()
    # Build the combined test without libsm64 (Jak only)
    add_executable(jak_sm64_test test/main_combined.c)
    target_include_directories(jak_sm64_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_link_libraries(jak_sm64_test PRIVATE jakopengoal_static)
    set_target_properties(jak_sm64_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
endif()

# ---- Visual combined test (Mario + Jak with SDL3 + OpenGL) ----
# Uses libsm64.lib from third-party/libsm64/src/ (pre-built import lib)
set(LIBSM64_LIB "${PROJECT_SOURCE_DIR}/third-party/libsm64/src/libsm64.lib")
if(EXISTS "${LIBSM64_LIB}")
    add_executable(mario_jak_test test/main_visual.cpp
        "${PROJECT_SOURCE_DIR}/third-party/glad/src/glad.c"
    )
    target_compile_definitions(mario_jak_test PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_include_directories(mario_jak_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/third-party/libsm64/src
        ${PROJECT_SOURCE_DIR}/third-party/glad/include
        ${PROJECT_SOURCE_DIR}/third-party/SDL/include
    )
    target_link_libraries(mario_jak_test PRIVATE
        jakopengoal_static
        ${LIBSM64_LIB}
        SDL3::SDL3
        opengl32
    )
    set_target_properties(mario_jak_test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
    message(STATUS "Building mario_jak_test (found libsm64.lib)")
else()
    message(STATUS "Skipping mario_jak_test (libsm64.lib not found at ${LIBSM64_LIB})")
endif()

# Set output directory
set_target_properties(jakopengoal PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)
set_target_properties(jak_test_app PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# ---- DLL isolation test targets (for debugging DllMain failures) ----
if(WIN32)
    # Test 1: bare DLL (no deps)
    add_library(test_bare SHARED test/test_dll_isolate.cpp)
    set_target_properties(test_bare PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

    # Test 2: DLL + fmt
    add_library(test_fmt SHARED test/test_dll_isolate.cpp)
    target_compile_definitions(test_fmt PRIVATE TEST_FMT)
    target_include_directories(test_fmt PRIVATE ${PROJECT_SOURCE_DIR}/third-party/fmt/include)
    target_link_libraries(test_fmt PRIVATE fmt)
    set_target_properties(test_fmt PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

    # Test 3: DLL + common (pulls in many deps)
    add_library(test_common SHARED test/test_dll_isolate.cpp)
    target_compile_definitions(test_common PRIVATE TEST_COMMON)
    target_include_directories(test_common PRIVATE ${PROJECT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/third-party/fmt/include)
    target_link_libraries(test_common PRIVATE common fmt)
    if(WIN32)
        target_link_libraries(test_common PRIVATE mman)
    endif()
    set_target_properties(test_common PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

    # Test 4: DLL + SDL3
    add_library(test_sdl3 SHARED test/test_dll_isolate.cpp)
    target_compile_definitions(test_sdl3 PRIVATE TEST_SDL3)
    target_include_directories(test_sdl3 PRIVATE ${PROJECT_SOURCE_DIR}/third-party/SDL/include)
    target_link_libraries(test_sdl3 PRIVATE SDL3::SDL3)
    set_target_properties(test_sdl3 PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

    # Test 5: DLL + runtime (the full thing)
    add_library(test_runtime SHARED test/test_dll_isolate.cpp)
    target_compile_definitions(test_runtime PRIVATE TEST_COMMON TEST_SDL3 TEST_IMGUI BUILD_LIBJAKOPENGOAL)
    target_include_directories(test_runtime PRIVATE
        ${PROJECT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/third-party/fmt/include
        ${PROJECT_SOURCE_DIR}/third-party/SDL/include
        ${PROJECT_SOURCE_DIR}/third-party/imgui
    )
    target_link_libraries(test_runtime PRIVATE runtime common fmt)
    if(WIN32)
        target_link_libraries(test_runtime PRIVATE mman)
    endif()
    set_target_properties(test_runtime PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

    # Test 8: minimal bridge reference (just is_runtime_ready)
    add_library(test_bridge2 SHARED test/test_dll_bridge2.cpp)
    target_compile_definitions(test_bridge2 PRIVATE BUILD_LIBJAKOPENGOAL)
    target_include_directories(test_bridge2 PRIVATE ${PROJECT_SOURCE_DIR})
    target_link_libraries(test_bridge2 PRIVATE runtime_headless common fmt)
    if(WIN32)
        target_link_libraries(test_bridge2 PRIVATE mman)
    endif()
    set_target_properties(test_bridge2 PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

    # Test 7: DLL referencing jak_bridge symbols only
    add_library(test_bridge SHARED test/test_dll_bridge.cpp)
    target_compile_definitions(test_bridge PRIVATE BUILD_LIBJAKOPENGOAL)
    target_include_directories(test_bridge PRIVATE
        ${PROJECT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/common
        ${PROJECT_SOURCE_DIR}/game
        ${PROJECT_SOURCE_DIR}/third-party
        ${PROJECT_SOURCE_DIR}/third-party/fmt/include
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_link_libraries(test_bridge PRIVATE runtime_headless common fmt)
    if(WIN32)
        target_link_libraries(test_bridge PRIVATE mman)
    endif()
    set_target_properties(test_bridge PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

    # Test 6a: DLL referencing exec_runtime only
    add_library(test_exec SHARED test/test_dll_exec.cpp)
    target_compile_definitions(test_exec PRIVATE BUILD_LIBJAKOPENGOAL)
    target_include_directories(test_exec PRIVATE ${PROJECT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/game ${PROJECT_SOURCE_DIR}/third-party/fmt/include ${PROJECT_SOURCE_DIR}/third-party/SDL/include)
    target_link_libraries(test_exec PRIVATE runtime common fmt)
    if(WIN32)
        target_link_libraries(test_exec PRIVATE mman)
    endif()
    set_target_properties(test_exec PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

    # Test 6b: DLL referencing MasterExit only
    add_library(test_kboot SHARED test/test_dll_kboot.cpp)
    target_compile_definitions(test_kboot PRIVATE BUILD_LIBJAKOPENGOAL)
    target_include_directories(test_kboot PRIVATE ${PROJECT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/game ${PROJECT_SOURCE_DIR}/third-party/fmt/include)
    target_link_libraries(test_kboot PRIVATE runtime common fmt)
    if(WIN32)
        target_link_libraries(test_kboot PRIVATE mman)
    endif()
    set_target_properties(test_kboot PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

    # Test 6: DLL that references same runtime symbols as jakopengoal
    add_library(test_full SHARED test/test_dll_full.cpp)
    target_compile_definitions(test_full PRIVATE BUILD_LIBJAKOPENGOAL)
    target_include_directories(test_full PRIVATE
        ${PROJECT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/common
        ${PROJECT_SOURCE_DIR}/game
        ${PROJECT_SOURCE_DIR}/third-party
        ${PROJECT_SOURCE_DIR}/third-party/fmt/include
        ${PROJECT_SOURCE_DIR}/third-party/SDL/include
    )
    target_link_libraries(test_full PRIVATE runtime common fmt)
    if(WIN32)
        target_link_libraries(test_full PRIVATE mman)
    endif()
    set_target_properties(test_full PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()
