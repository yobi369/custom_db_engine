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
option(CUSTOM_DB_ENGINE_BUILD_BENCHMARKS "Build storage performance benchmark" ON)
option(CUSTOM_DB_ENGINE_BUILD_SERVER "Build the standalone HTTP database server" OFF)

if(CUSTOM_DB_ENGINE_BUILD_SERVER)
  find_package(OpenSSL 3.0 REQUIRED COMPONENTS Crypto)
  include(FetchContent)
  set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
  set(HTTPLIB_USE_ZLIB_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
  set(HTTPLIB_USE_BROTLI_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG v0.18.3
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(httplib)
  add_executable(custom_db_server server/main.cpp server/auth_crypto.cpp server/auth_store.cpp server/audit_log.cpp)
  target_compile_features(custom_db_server PRIVATE cxx_std_17)
  target_link_libraries(custom_db_server PRIVATE CustomDBEngine::custom_db_engine httplib::httplib nlohmann_json::nlohmann_json OpenSSL::Crypto)
  if(BUILD_TESTING)
    add_executable(auth_crypto_test tests/auth_crypto_test.cpp server/auth_crypto.cpp)
    target_compile_features(auth_crypto_test PRIVATE cxx_std_17)
    target_link_libraries(auth_crypto_test PRIVATE OpenSSL::Crypto)
    add_test(NAME auth_crypto_test COMMAND auth_crypto_test)

    add_executable(auth_store_test tests/auth_store_test.cpp server/auth_store.cpp server/auth_crypto.cpp)
    target_compile_features(auth_store_test PRIVATE cxx_std_17)
    target_compile_definitions(auth_store_test PRIVATE CUSTOMDB_AUTH_STORE_TEST_FAULTS)
    target_link_libraries(auth_store_test PRIVATE nlohmann_json::nlohmann_json OpenSSL::Crypto)
    add_test(NAME auth_store_test COMMAND auth_store_test)

    add_executable(audit_log_test tests/audit_log_test.cpp server/audit_log.cpp)
    target_compile_features(audit_log_test PRIVATE cxx_std_17)
    target_link_libraries(audit_log_test PRIVATE nlohmann_json::nlohmann_json)
    add_test(NAME audit_log_test COMMAND audit_log_test)

    add_executable(auth_http_fixture tests/auth_http_fixture.cpp server/auth_store.cpp server/auth_crypto.cpp)
    target_compile_features(auth_http_fixture PRIVATE cxx_std_17)
    target_link_libraries(auth_http_fixture PRIVATE nlohmann_json::nlohmann_json OpenSSL::Crypto)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME auth_http_integration
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/auth_http_integration_test.py
              $<TARGET_FILE:custom_db_server> $<TARGET_FILE:auth_http_fixture>)
    set_tests_properties(auth_http_integration PROPERTIES TIMEOUT 45)
  endif()
  if(BUILD_TESTING)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME custom_db_server_integration
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/server_integration_test.py $<TARGET_FILE:custom_db_server>)
    set_tests_properties(custom_db_server_integration PROPERTIES TIMEOUT 30)
    add_test(NAME openapi_contract_validation
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/openapi_validation_test.py)
  endif()
endif()

if(CUSTOM_DB_ENGINE_BUILD_BENCHMARKS)
  add_executable(custom_db_engine_benchmark benchmarks/storage_engine_benchmark.cpp)
  target_link_libraries(custom_db_engine_benchmark PRIVATE CustomDBEngine::custom_db_engine)
  if(BUILD_TESTING)
    add_test(NAME storage_benchmark_smoke COMMAND custom_db_engine_benchmark 25)
    set_tests_properties(storage_benchmark_smoke PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
    add_test(NAME storage_benchmark_medium COMMAND custom_db_engine_benchmark 1000)
    set_tests_properties(storage_benchmark_medium PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
  endif()
endif()
if(CUSTOM_DB_ENGINE_BUILD_EXAMPLES)
  add_executable(custom_db_engine_example examples/basic_usage.cpp)
  target_link_libraries(custom_db_engine_example PRIVATE CustomDBEngine::custom_db_engine)
endif()

if(BUILD_TESTING)
  find_package(Threads REQUIRED)
  add_executable(storage_engine_test tests/storage_engine_test.cpp)
  target_link_libraries(storage_engine_test PRIVATE CustomDBEngine::custom_db_engine Threads::Threads)
  add_test(NAME storage_engine_test COMMAND storage_engine_test)
  set_tests_properties(storage_engine_test PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

  add_executable(storage_temp_recovery_test tests/storage_temp_recovery_test.cpp)
  target_link_libraries(storage_temp_recovery_test PRIVATE CustomDBEngine::custom_db_engine)
  add_test(NAME storage_temp_recovery_test COMMAND storage_temp_recovery_test)
  set_tests_properties(storage_temp_recovery_test PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

  # Build a separate engine copy with fault injection compiled in only for tests.
  add_library(custom_db_engine_fault_test STATIC src/storage_engine.cpp)
  target_compile_features(custom_db_engine_fault_test PUBLIC cxx_std_17)
  target_include_directories(custom_db_engine_fault_test PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
  target_include_directories(custom_db_engine_fault_test PRIVATE
    $<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>)
  target_compile_definitions(custom_db_engine_fault_test PRIVATE CUSTOM_DB_ENGINE_TEST_FAULTS)
  add_executable(storage_checkpoint_fault_test tests/storage_checkpoint_fault_test.cpp)
  target_link_libraries(storage_checkpoint_fault_test PRIVATE custom_db_engine_fault_test)
  add_test(NAME storage_checkpoint_fault_test COMMAND storage_checkpoint_fault_test)
  set_tests_properties(storage_checkpoint_fault_test PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

  add_executable(storage_snapshot_publish_fault_test tests/storage_snapshot_publish_fault_test.cpp)
  target_link_libraries(storage_snapshot_publish_fault_test PRIVATE custom_db_engine_fault_test)
  add_test(NAME storage_snapshot_publish_fault_test COMMAND storage_snapshot_publish_fault_test)
  set_tests_properties(storage_snapshot_publish_fault_test PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

  add_executable(storage_wal_partial_append_fault_test tests/storage_wal_partial_append_fault_test.cpp)
  target_link_libraries(storage_wal_partial_append_fault_test PRIVATE custom_db_engine_fault_test)
  add_test(NAME storage_wal_partial_append_fault_test COMMAND storage_wal_partial_append_fault_test)
  set_tests_properties(storage_wal_partial_append_fault_test PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

  add_executable(property_storage_test tests/property_storage_test.cpp)
  target_link_libraries(property_storage_test PRIVATE CustomDBEngine::custom_db_engine)
  add_test(NAME property_storage_test COMMAND property_storage_test)
  set_tests_properties(property_storage_test PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
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
