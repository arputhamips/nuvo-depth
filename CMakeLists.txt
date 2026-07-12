cmake_minimum_required(VERSION 3.20)
project(depth_daemon VERSION 0.1.0 LANGUAGES CXX)

include(CTest)
option(DEPTHD_WITH_OPENCV "Build the OpenCV CPU SGBM backend" ON)

function(depthd_enable_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive-)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
  endif()
endfunction()

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
depthd_enable_warnings(depthd_contract)

if(DEPTHD_WITH_OPENCV)
  find_package(OpenCV 4 QUIET COMPONENTS core imgproc imgcodecs calib3d)
  if(OpenCV_FOUND)
    add_library(depthd_cpu_sgbm STATIC src/cpu_sgbm_backend.cpp)
    add_library(depthd::cpu_sgbm ALIAS depthd_cpu_sgbm)
    target_include_directories(depthd_cpu_sgbm
      PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    )
    target_compile_features(depthd_cpu_sgbm PUBLIC cxx_std_20)
    target_include_directories(depthd_cpu_sgbm SYSTEM PRIVATE ${OpenCV_INCLUDE_DIRS})
    target_link_libraries(depthd_cpu_sgbm PUBLIC depthd::contract PRIVATE ${OpenCV_LIBS})
    depthd_enable_warnings(depthd_cpu_sgbm)

    add_executable(depthd_sgbm_benchmark tools/sgbm_benchmark.cpp)
    target_compile_features(depthd_sgbm_benchmark PRIVATE cxx_std_20)
    target_include_directories(depthd_sgbm_benchmark SYSTEM PRIVATE ${OpenCV_INCLUDE_DIRS})
    target_link_libraries(depthd_sgbm_benchmark PRIVATE depthd::cpu_sgbm ${OpenCV_LIBS})
    depthd_enable_warnings(depthd_sgbm_benchmark)
  else()
    message(STATUS "OpenCV not found; skipping the CPU SGBM backend")
  endif()
endif()

if(BUILD_TESTING)
  add_executable(depthd_contract_test tests/contract_test.cpp)
  target_link_libraries(depthd_contract_test PRIVATE depthd::contract)
  add_test(NAME depthd_contract_test COMMAND depthd_contract_test)

  if(TARGET depthd_cpu_sgbm)
    add_executable(depthd_cpu_sgbm_test tests/cpu_sgbm_test.cpp)
    target_compile_features(depthd_cpu_sgbm_test PRIVATE cxx_std_20)
    target_include_directories(depthd_cpu_sgbm_test SYSTEM PRIVATE ${OpenCV_INCLUDE_DIRS})
    target_link_libraries(depthd_cpu_sgbm_test PRIVATE depthd::cpu_sgbm ${OpenCV_LIBS})
    depthd_enable_warnings(depthd_cpu_sgbm_test)
    add_test(NAME depthd_cpu_sgbm_test COMMAND depthd_cpu_sgbm_test)
  endif()
endif()
