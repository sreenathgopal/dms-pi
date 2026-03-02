cmake_minimum_required(VERSION 3.16)
project(dms VERSION 3.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Release optimizations — ARM flags only on ARM targets
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm|aarch64|ARM")
        add_compile_options(-O2 -march=armv8-a+crc -mtune=cortex-a53 -flto)
        add_link_options(-flto -s)
    else()
        add_compile_options(-O2)
    endif()
endif()

# --- System dependencies ---
find_package(PkgConfig REQUIRED)
find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs videoio objdetect dnn)
pkg_check_modules(GPIOD libgpiod)      # optional on non-Pi
pkg_check_modules(MHD REQUIRED libmicrohttpd)

# --- Header-only deps (FetchContent) ---
include(cmake/FetchDeps.cmake)

# --- TFLite (built from source, installed to /usr/local) ---
find_library(TFLITE_LIB tensorflow-lite PATHS /usr/local/lib)
if(NOT TFLITE_LIB)
    message(WARNING "TFLite not found — building without TFLite support")
    set(TFLITE_LIB "")
    add_compile_definitions(DMS_NO_TFLITE)
endif()

# TFLite headers
if(TFLITE_LIB)
    set(TFLITE_INCLUDE_DIRS
        /usr/local/include
        /usr/local/include/flatbuffers
    )
endif()

# --- DMS executable ---
add_executable(dms
    src/main.cpp
    src/config.cpp
    src/face_detector.cpp
    src/landmark_tracker.cpp
    src/classifier.cpp
    src/state_machine.cpp
    src/alert_service.cpp
    src/management_service.cpp
)

target_include_directories(dms PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${OpenCV_INCLUDE_DIRS}
    ${MHD_INCLUDE_DIRS}
    ${TFLITE_INCLUDE_DIRS}
)

target_link_libraries(dms PRIVATE
    ${OpenCV_LIBS}
    ${MHD_LIBRARIES}
    nlohmann_json::nlohmann_json
)

# Linux link libraries
target_link_libraries(dms PRIVATE pthread dl)

if(TFLITE_LIB)
    target_link_libraries(dms PRIVATE ${TFLITE_LIB})
endif()

if(GPIOD_FOUND)
    target_include_directories(dms PRIVATE ${GPIOD_INCLUDE_DIRS})
    target_link_libraries(dms PRIVATE ${GPIOD_LIBRARIES})
else()
    target_compile_definitions(dms PRIVATE DMS_NO_GPIO)
endif()

# Install
install(TARGETS dms DESTINATION bin)
