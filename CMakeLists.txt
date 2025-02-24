cmake_minimum_required(VERSION 3.10)
project(CustomDBEngine)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# Find required packages
find_package(nlohmann_json REQUIRED)

include_directories(${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src)

add_executable(storage_engine_test
    tests/storage_engine_test.cpp
    src/storage_engine.cpp
)

target_include_directories(storage_engine_test 
    PUBLIC 
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(storage_engine_test PRIVATE nlohmann_json::nlohmann_json)

target_link_libraries(storage_engine_test PRIVATE nlohmann_json::nlohmann_json)
