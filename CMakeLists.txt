cmake_minimum_required(VERSION 3.20)
project(depth_daemon VERSION 0.1.0 LANGUAGES CXX)

include(CTest)

add_library(depthd_contract STATIC
  src/contract.cpp
)
add_library(depthd::contract ALIAS depthd_contract)

target_include_directories(depthd_contract
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(depthd_contract PUBLIC cxx_std_20)

if(MSVC)
  target_compile_options(depthd_contract PRIVATE /W4 /permissive-)
else()
  target_compile_options(depthd_contract PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
endif()

if(BUILD_TESTING)
  add_executable(depthd_contract_test tests/contract_test.cpp)
  target_link_libraries(depthd_contract_test PRIVATE depthd::contract)
  add_test(NAME depthd_contract_test COMMAND depthd_contract_test)
endif()
