cmake_minimum_required(VERSION 3.22)
project(hyper90_fps_booster VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(HYPER90_BUILD_TESTS "Build host verification test suite" OFF)

set(HYPER90_CORE_SOURCES
    src/Config.cpp
    src/Telemetry.cpp
    src/EngineOptionsTuner.cpp
    src/SystemOptimizers.cpp
    src/NativeHooks.cpp
    src/FrameBlend.cpp
    src/Modules.cpp
)

if(ANDROID)
    include(FetchContent)

    FetchContent_Declare(
        preloader
        GIT_REPOSITORY https://github.com/LiteLDev/preloader-android.git
        GIT_TAG 0.2.2
    )
    FetchContent_MakeAvailable(preloader)

    add_library(hyper90_fps_booster SHARED
        ${HYPER90_CORE_SOURCES}
        src/Hyper90Mod.cpp
    )

    target_include_directories(hyper90_fps_booster PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    target_compile_options(hyper90_fps_booster PRIVATE
        -O3
        -fvisibility=hidden
        -fvisibility-inlines-hidden
        -ffunction-sections
        -fdata-sections
        -Wall
        -Wextra
    )

    find_library(ANDROID_LIB android)
    find_library(LOG_LIB log)
    find_library(EGL_LIB EGL)
    find_library(GLESV2_LIB GLESv2)
    find_library(GLESV3_LIB GLESv3)

    target_link_libraries(hyper90_fps_booster PRIVATE
        preloader::preloader
        ${ANDROID_LIB}
        ${LOG_LIB}
        ${EGL_LIB}
        ${GLESV2_LIB}
        ${GLESV3_LIB}
    )

    target_link_options(hyper90_fps_booster PRIVATE
        "-Wl,--gc-sections"
        "-Wl,-s"
        "-Wl,-z,max-page-size=16384"
    )
endif()

if(HYPER90_BUILD_TESTS AND NOT ANDROID)
    enable_testing()
    add_executable(test_hyper90
        tests/test_hyper90.cpp
        ${HYPER90_CORE_SOURCES}
    )
    target_include_directories(test_hyper90 PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${PRELOADER_INCLUDE_DIR}
        ${JSON_INCLUDE_DIR}
        ${FMT_INCLUDE_DIR}
    )
    target_compile_definitions(test_hyper90 PRIVATE FMT_HEADER_ONLY)
    add_test(NAME Hyper90ModuleVerification COMMAND test_hyper90)
endif()
