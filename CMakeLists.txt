cmake_minimum_required(VERSION 3.16)

project(FindFRs LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(TBB REQUIRED)

add_executable(findfrs3 findfrs3.cpp)
target_link_libraries(findfrs3 PRIVATE TBB::tbb)
