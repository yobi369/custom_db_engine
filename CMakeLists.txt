cmake_minimum_required(VERSION 3.16)
project(CustomDBEngine VERSION 0.2.0 LANGUAGES CXX)

include(CTest)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

find_package(nlohmann_json 3.10 QUIET)
if(NOT nlohmann_json_FOUND)
  include(FetchContent)
  set(JSON_BuildTests OFF CACHE INTERNAL "")
  FetchContent_Declare(json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)
  FetchContent_MakeAvailable(json)
endif()

add_library(custom_db_engine src/storage_engine.cpp)
add_library(CustomDBEngine::custom_db_engine ALIAS custom_db_engine)
target_compile_features(custom_db_engine PUBLIC cxx_std_17)
target_include_directories(custom_db_engine PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_include_directories(custom_db_engine PRIVATE $<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>)

if(MSVC)
  target_compile_options(custom_db_engine PRIVATE /W4 /permissive-)
else()
  target_compile_options(custom_db_engine PRIVATE -Wall -Wextra -Wpedantic)
endif()

option(CUSTOM_DB_ENGINE_BUILD_EXAMPLES "Build example programs" ON)
if(CUSTOM_DB_ENGINE_BUILD_EXAMPLES)
  add_executable(custom_db_engine_example examples/basic_usage.cpp)
  target_link_libraries(custom_db_engine_example PRIVATE CustomDBEngine::custom_db_engine)
endif()

if(BUILD_TESTING)
  add_executable(storage_engine_test tests/storage_engine_test.cpp)
  target_link_libraries(storage_engine_test PRIVATE CustomDBEngine::custom_db_engine)
  add_test(NAME storage_engine_test COMMAND storage_engine_test)
  set_tests_properties(storage_engine_test PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
endif()

install(TARGETS custom_db_engine
  EXPORT CustomDBEngineTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT CustomDBEngineTargets
  FILE CustomDBEngineTargets.cmake
  NAMESPACE CustomDBEngine::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CustomDBEngine)

configure_package_config_file(
  cmake/CustomDBEngineConfig.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/CustomDBEngineConfig.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/CustomDBEngine")
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/CustomDBEngineConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)
install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/CustomDBEngineConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/CustomDBEngineConfigVersion.cmake"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CustomDBEngine)
