# libjakopengoal - Embeddable Jak library (inspired by libsm64)
#
# Builds both a shared library (libjakopengoal.dll/.so) and a test program.

# ---- Shared Library ----
set(LIBJAKOPENGOAL_SOURCES
    src/libjakopengoal.cpp
)

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

# Link against the existing jak-project runtime (static lib)
# This gives us the full GOAL runtime, kernel, graphics, etc.
target_link_libraries(jakopengoal PRIVATE runtime common fmt)
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
