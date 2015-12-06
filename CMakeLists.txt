cmake_minimum_required(VERSION 3.0)
# Open Flow Gate Way
project(ofgw C CXX)

set(CMAKE_BUILD_TYPE Debug)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall")

find_package(Threads REQUIRED)

set(Boost_USE_MULTITHREADED ON)
find_package(Boost REQUIRED COMPONENTS program_options)

set(OFGW_SOURCES
	gateway.cpp
	gateway.hpp
	control.cpp
)

add_executable(ofgw ${OFGW_SOURCES})
target_link_libraries(ofgw ${CMAKE_THREAD_LIBS_INIT} ${Boost_LIBRARIES})
