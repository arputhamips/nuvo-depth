cmake_minimum_required(VERSION 3.20)
project(depth_daemon VERSION 0.1.0 LANGUAGES CXX)

include(CTest)
option(DEPTHD_WITH_OPENCV "Build the OpenCV CPU processing backends" ON)

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

add_library(depthd_rectification STATIC
  src/rectification_backend.cpp
)
add_library(depthd::rectification ALIAS depthd_rectification)

add_library(depthd_obstacle_cloud STATIC
  src/obstacle_cloud.cpp
)
add_library(depthd::obstacle_cloud ALIAS depthd_obstacle_cloud)

add_library(depthd_result_ring STATIC
  src/result_ring.cpp
)
add_library(depthd::result_ring ALIAS depthd_result_ring)

target_include_directories(depthd_contract
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(depthd_contract PUBLIC cxx_std_20)
depthd_enable_warnings(depthd_contract)

target_include_directories(depthd_rectification
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(depthd_rectification PUBLIC cxx_std_20)
depthd_enable_warnings(depthd_rectification)

target_include_directories(depthd_obstacle_cloud
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(depthd_obstacle_cloud PUBLIC cxx_std_20)
depthd_enable_warnings(depthd_obstacle_cloud)

target_include_directories(depthd_result_ring
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(depthd_result_ring PUBLIC cxx_std_20)
target_link_libraries(depthd_result_ring PUBLIC depthd::obstacle_cloud)
depthd_enable_warnings(depthd_result_ring)

add_executable(depthd_obstacle_cloud_benchmark
  tools/obstacle_cloud_benchmark.cpp
)
target_compile_features(depthd_obstacle_cloud_benchmark PRIVATE cxx_std_20)
target_link_libraries(depthd_obstacle_cloud_benchmark PRIVATE
  depthd::obstacle_cloud
)
depthd_enable_warnings(depthd_obstacle_cloud_benchmark)

if(DEPTHD_WITH_OPENCV)
  find_package(OpenCV 4 QUIET COMPONENTS core imgproc imgcodecs calib3d)
  if(OpenCV_FOUND)
    add_library(depthd_opencv_rectification STATIC
      src/opencv_rectification_backend.cpp
    )
    add_library(depthd::opencv_rectification ALIAS depthd_opencv_rectification)
    target_include_directories(depthd_opencv_rectification
      PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    )
    target_compile_features(depthd_opencv_rectification PUBLIC cxx_std_20)
    target_include_directories(depthd_opencv_rectification SYSTEM PRIVATE ${OpenCV_INCLUDE_DIRS})
    target_link_libraries(depthd_opencv_rectification PUBLIC depthd::rectification PRIVATE ${OpenCV_LIBS})
    depthd_enable_warnings(depthd_opencv_rectification)

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

  add_executable(depthd_rectification_backend_test
    tests/rectification_backend_test.cpp
  )
  target_link_libraries(depthd_rectification_backend_test
    PRIVATE depthd::rectification
  )
  depthd_enable_warnings(depthd_rectification_backend_test)
  add_test(NAME depthd_rectification_backend_test
    COMMAND depthd_rectification_backend_test
  )

  add_executable(depthd_obstacle_cloud_test tests/obstacle_cloud_test.cpp)
  target_link_libraries(depthd_obstacle_cloud_test PRIVATE
    depthd::obstacle_cloud
  )
  depthd_enable_warnings(depthd_obstacle_cloud_test)
  add_test(NAME depthd_obstacle_cloud_test COMMAND depthd_obstacle_cloud_test)

  add_executable(depthd_result_ring_test tests/result_ring_test.cpp)
  target_link_libraries(depthd_result_ring_test PRIVATE depthd::result_ring)
  depthd_enable_warnings(depthd_result_ring_test)
  add_test(NAME depthd_result_ring_test COMMAND depthd_result_ring_test)

  if(TARGET depthd_opencv_rectification)
    add_executable(depthd_opencv_rectification_backend_test
      tests/opencv_rectification_backend_test.cpp
    )
    target_link_libraries(depthd_opencv_rectification_backend_test
      PRIVATE depthd::opencv_rectification
    )
    depthd_enable_warnings(depthd_opencv_rectification_backend_test)
    add_test(NAME depthd_opencv_rectification_backend_test
      COMMAND depthd_opencv_rectification_backend_test
    )
  endif()

  if(TARGET depthd_cpu_sgbm)
    add_executable(depthd_production_profile_lock_test
      tests/production_profile_lock_test.cpp
    )
    target_compile_features(depthd_production_profile_lock_test PRIVATE cxx_std_20)
    target_compile_definitions(depthd_production_profile_lock_test PRIVATE
      DEPTHD_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(depthd_production_profile_lock_test PRIVATE
      depthd::cpu_sgbm depthd::obstacle_cloud
    )
    depthd_enable_warnings(depthd_production_profile_lock_test)
    add_test(NAME depthd_production_profile_lock_test
      COMMAND depthd_production_profile_lock_test
    )

    add_executable(depthd_cpu_sgbm_test tests/cpu_sgbm_test.cpp)
    target_compile_features(depthd_cpu_sgbm_test PRIVATE cxx_std_20)
    target_include_directories(depthd_cpu_sgbm_test SYSTEM PRIVATE ${OpenCV_INCLUDE_DIRS})
    target_link_libraries(depthd_cpu_sgbm_test PRIVATE depthd::cpu_sgbm ${OpenCV_LIBS})
    depthd_enable_warnings(depthd_cpu_sgbm_test)
    add_test(NAME depthd_cpu_sgbm_test COMMAND depthd_cpu_sgbm_test)
  endif()
endif()
