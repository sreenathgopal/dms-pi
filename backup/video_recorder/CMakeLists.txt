cmake_minimum_required(VERSION 3.10)
project(video_recorder)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find OpenCV
find_package(OpenCV REQUIRED)

# Find ZeroMQ via vcpkg's cmake config
find_package(ZeroMQ CONFIG QUIET)
find_package(cppzmq CONFIG QUIET)

if(ZeroMQ_FOUND AND cppzmq_FOUND)
    message(STATUS "ZeroMQ found via vcpkg config")
    add_executable(video_recorder video_recorder.cpp)
    target_link_libraries(video_recorder
        ${OpenCV_LIBS}
        cppzmq)
elseif(ZeroMQ_FOUND)
    message(STATUS "ZeroMQ found, cppzmq header-only")
    add_executable(video_recorder video_recorder.cpp)
    target_link_libraries(video_recorder
        ${OpenCV_LIBS}
        libzmq)
else()
    # Manual find for Linux/Pi
    find_path(ZMQ_HPP zmq.hpp PATHS /usr/include /usr/local/include)
    find_library(ZMQ_LIB zmq PATHS /usr/lib /usr/local/lib)

    if(ZMQ_HPP AND ZMQ_LIB)
        message(STATUS "ZeroMQ found manually: ${ZMQ_LIB}")
        add_executable(video_recorder video_recorder.cpp)
        target_include_directories(video_recorder PRIVATE ${ZMQ_HPP})
        target_link_libraries(video_recorder ${OpenCV_LIBS} ${ZMQ_LIB})
    else()
        message(FATAL_ERROR "ZeroMQ not found!\n"
            "  Linux/Pi: sudo apt install libzmq3-dev libcppzmq-dev\n"
            "  Windows:  vcpkg install zeromq:x64-windows cppzmq:x64-windows")
    endif()
endif()

message(STATUS "OpenCV: ${OpenCV_VERSION}")