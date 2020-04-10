cmake_minimum_required(VERSION 3.0)

project(web-crawler)

set(CMAKE_CXX_STANDARD 17)

set(CMAKE_BUILD_TYPE Release)

find_package(Boost REQUIRED COMPONENTS system)

find_package(Threads REQUIRED)

set(Boost_USE_MULTITHREADED ON)

include_directories(${Boost_INCLUDE_DIRS})

add_executable(web-crawler main.cpp thread_safe_queue.hpp)

target_link_libraries(web-crawler ${Boost_LIBRARIES} Threads::Threads)