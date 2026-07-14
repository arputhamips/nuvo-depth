cmake_minimum_required(VERSION 3.20)
project(depth_daemon VERSION 0.1.0 LANGUAGES CXX)

include(CTest)
include(GNUInstallDirs)
option(DEPTHD_WITH_OPENCV "Build the OpenCV CPU processing backends" ON)
option(DEPTHD_WITH_HAILORT_SEMANTICS
  "Build the native HailoRT semantic inference backend" OFF)

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

find_package(Threads REQUIRED)

add_library(depthd_semantic_core STATIC
  src/semantic_types.cpp
  src/semantic_fusion.cpp
  src/semantic_perception.cpp
)
add_library(depthd::semantic_core ALIAS depthd_semantic_core)

add_library(depthd_semantic_ring STATIC
  src/semantic_ring.cpp
)
add_library(depthd::semantic_ring ALIAS depthd_semantic_ring)

add_library(depthd_native_output_core STATIC
  src/native_output.cpp
)
add_library(depthd::native_output_core ALIAS depthd_native_output_core)

add_library(depthd_native_output SHARED
  src/native_output_c.cpp
)

add_library(depthd_semantic_output SHARED
  src/semantic_output_c.cpp
)

add_library(depthd_native_semantic SHARED
  src/native_semantic_runtime.cpp
)

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
set_target_properties(depthd_obstacle_cloud PROPERTIES
  POSITION_INDEPENDENT_CODE ON
)
depthd_enable_warnings(depthd_obstacle_cloud)

target_include_directories(depthd_result_ring
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(depthd_result_ring PUBLIC cxx_std_20)
target_link_libraries(depthd_result_ring PUBLIC depthd::obstacle_cloud)
set_target_properties(depthd_result_ring PROPERTIES
  POSITION_INDEPENDENT_CODE ON
)
depthd_enable_warnings(depthd_result_ring)

target_include_directories(depthd_semantic_core
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(depthd_semantic_core PUBLIC cxx_std_20)
target_link_libraries(depthd_semantic_core PUBLIC
  depthd::obstacle_cloud Threads::Threads
)
set_target_properties(depthd_semantic_core PROPERTIES
  POSITION_INDEPENDENT_CODE ON
)
depthd_enable_warnings(depthd_semantic_core)

target_include_directories(depthd_semantic_ring
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(depthd_semantic_ring PUBLIC cxx_std_20)
target_link_libraries(depthd_semantic_ring PUBLIC depthd::semantic_core)
set_target_properties(depthd_semantic_ring PROPERTIES
  POSITION_INDEPENDENT_CODE ON
)
depthd_enable_warnings(depthd_semantic_ring)

target_include_directories(depthd_native_output_core
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(depthd_native_output_core PUBLIC cxx_std_20)
target_link_libraries(depthd_native_output_core PUBLIC
  depthd::obstacle_cloud depthd::result_ring
)
set_target_properties(depthd_native_output_core PROPERTIES
  POSITION_INDEPENDENT_CODE ON
)
depthd_enable_warnings(depthd_native_output_core)

target_include_directories(depthd_native_output
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(depthd_native_output PUBLIC cxx_std_20)
target_compile_definitions(depthd_native_output PRIVATE
  DEPTHD_NATIVE_OUTPUT_BUILD
)
target_link_libraries(depthd_native_output PRIVATE depthd::native_output_core)
depthd_enable_warnings(depthd_native_output)

target_include_directories(depthd_semantic_output
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(depthd_semantic_output PUBLIC cxx_std_20)
target_compile_definitions(depthd_semantic_output PRIVATE
  DEPTHD_SEMANTIC_OUTPUT_BUILD
)
target_link_libraries(depthd_semantic_output PRIVATE
  depthd::semantic_core depthd::semantic_ring depthd::native_output_core
)
depthd_enable_warnings(depthd_semantic_output)

target_include_directories(depthd_native_semantic
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(depthd_native_semantic PUBLIC cxx_std_20)
target_compile_definitions(depthd_native_semantic PRIVATE
  DEPTHD_NATIVE_SEMANTIC_BUILD
)
target_link_libraries(depthd_native_semantic PRIVATE
  depthd::semantic_core depthd::semantic_ring depthd::native_output_core
)
depthd_enable_warnings(depthd_native_semantic)

add_executable(depthd_obstacle_cloud_benchmark
  tools/obstacle_cloud_benchmark.cpp
)
target_compile_features(depthd_obstacle_cloud_benchmark PRIVATE cxx_std_20)
target_link_libraries(depthd_obstacle_cloud_benchmark PRIVATE
  depthd::obstacle_cloud
)
depthd_enable_warnings(depthd_obstacle_cloud_benchmark)

add_executable(depthd_result_ring_inspect
  tools/result_ring_inspect.cpp
)
target_compile_features(depthd_result_ring_inspect PRIVATE cxx_std_20)
target_link_libraries(depthd_result_ring_inspect PRIVATE depthd::result_ring)
depthd_enable_warnings(depthd_result_ring_inspect)

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

if(DEPTHD_WITH_HAILORT_SEMANTICS)
  if(NOT OpenCV_FOUND)
    message(FATAL_ERROR
      "DEPTHD_WITH_HAILORT_SEMANTICS requires OpenCV core/imgproc/imgcodecs")
  endif()
  find_path(DEPTHD_HAILORT_INCLUDE_DIR hailo/hailort.hpp)
  find_library(DEPTHD_HAILORT_LIBRARY NAMES hailort
    PATHS /lib /usr/lib /usr/lib/aarch64-linux-gnu)
  if(NOT DEPTHD_HAILORT_INCLUDE_DIR OR NOT DEPTHD_HAILORT_LIBRARY)
    message(FATAL_ERROR
      "HailoRT headers/library not found; install the matching HailoRT dev package")
  endif()
  target_sources(depthd_native_semantic PRIVATE
    src/hailort_semantic_backend.cpp
  )
  target_compile_definitions(depthd_native_semantic PRIVATE
    DEPTHD_WITH_HAILORT_SEMANTICS=1
  )
  target_include_directories(depthd_native_semantic SYSTEM PRIVATE
    ${OpenCV_INCLUDE_DIRS} ${DEPTHD_HAILORT_INCLUDE_DIR}
  )
  target_link_libraries(depthd_native_semantic PRIVATE
    ${OpenCV_LIBS} ${DEPTHD_HAILORT_LIBRARY}
  )
  message(STATUS
    "Native HailoRT semantics enabled: ${DEPTHD_HAILORT_LIBRARY}")
endif()

# This is a private runtime directory, not an ABI-dependent public library
# location. Keep it stable across arm64 and build hosts so the unit never needs
# a CMake multiarch triplet.
set(DEPTHD_LIBEXEC_DIR "lib/nuvo-depthd")

install(TARGETS depthd_native_output depthd_native_semantic
  LIBRARY DESTINATION "${DEPTHD_LIBEXEC_DIR}"
  RUNTIME DESTINATION "${DEPTHD_LIBEXEC_DIR}"
)
install(PROGRAMS
  tools/pi/profile_async_zero_copy_pipeline.py
  tools/pi/async_stereo_capture.py
  tools/pi/capture_stream_profile.py
  tools/pi/capture_rectified_pair.py
  tools/pi/profile_cpu_pipeline.py
  tools/pi/stereo_auto_exposure.py
  tools/pi/depthd_native_output.py
  tools/pi/depthd_native_semantic.py
  DESTINATION "${DEPTHD_LIBEXEC_DIR}"
)
install(PROGRAMS deployment/depthd_assets.py
  DESTINATION "${CMAKE_INSTALL_SBINDIR}"
  RENAME depthd-assets
)
install(FILES
  config/nuvo_pi5_hailo8l.yaml
  DESTINATION "${CMAKE_INSTALL_DATADIR}/nuvo-depthd/runtime-seed/config"
)
install(FILES
  config/calibration/nuvo_reference/front_left.yaml
  config/calibration/nuvo_reference/front_right.yaml
  DESTINATION
    "${CMAKE_INSTALL_DATADIR}/nuvo-depthd/runtime-seed/config/calibration/nuvo_reference"
)
install(FILES deployment/assets/nuvo-pi5-hailo8l-assets.json
  DESTINATION "${CMAKE_INSTALL_DATADIR}/nuvo-depthd/runtime-seed"
)
install(FILES deployment/systemd/nuvo-depthd.service
  DESTINATION "lib/systemd/system"
)
install(FILES deployment/systemd/nuvo-depthd.tmpfiles.conf
  DESTINATION "lib/tmpfiles.d"
)
install(FILES deployment/README.md
  DESTINATION "share/doc/nuvo-depthd"
  RENAME deployment.md
)

if(BUILD_TESTING)
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(NAME depthd_assets_test
      COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/depthd_assets_test.py
    )
  endif()

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

  add_executable(depthd_semantic_fusion_test tests/semantic_fusion_test.cpp)
  target_link_libraries(depthd_semantic_fusion_test PRIVATE
    depthd::semantic_core
  )
  depthd_enable_warnings(depthd_semantic_fusion_test)
  add_test(NAME depthd_semantic_fusion_test
    COMMAND depthd_semantic_fusion_test
  )

  add_executable(depthd_semantic_perception_test
    tests/semantic_perception_test.cpp
  )
  target_link_libraries(depthd_semantic_perception_test PRIVATE
    depthd::semantic_core
  )
  depthd_enable_warnings(depthd_semantic_perception_test)
  add_test(NAME depthd_semantic_perception_test
    COMMAND depthd_semantic_perception_test
  )

  add_executable(depthd_semantic_ring_test tests/semantic_ring_test.cpp)
  target_link_libraries(depthd_semantic_ring_test PRIVATE
    depthd::semantic_ring
  )
  depthd_enable_warnings(depthd_semantic_ring_test)
  add_test(NAME depthd_semantic_ring_test COMMAND depthd_semantic_ring_test)

  add_executable(depthd_semantic_output_test tests/semantic_output_test.cpp)
  target_link_libraries(depthd_semantic_output_test PRIVATE
    depthd_semantic_output depthd::semantic_ring
  )
  depthd_enable_warnings(depthd_semantic_output_test)
  add_test(NAME depthd_semantic_output_test COMMAND depthd_semantic_output_test)

  add_executable(depthd_native_output_test tests/native_output_test.cpp)
  target_link_libraries(depthd_native_output_test PRIVATE
    depthd::native_output_core depthd_native_output
  )
  depthd_enable_warnings(depthd_native_output_test)
  add_test(NAME depthd_native_output_test COMMAND depthd_native_output_test)

  if(UNIX)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
      execute_process(
        COMMAND ${Python3_EXECUTABLE} -c "import numpy"
        RESULT_VARIABLE DEPTHD_PYTHON_NUMPY_RESULT
        OUTPUT_QUIET
        ERROR_QUIET
      )
      if(DEPTHD_PYTHON_NUMPY_RESULT EQUAL 0)
        add_test(NAME depthd_native_output_python_test
          COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/native_output_python_test.py
            --library $<TARGET_FILE:depthd_native_output>
        )
        set_tests_properties(depthd_native_output_python_test PROPERTIES
          ENVIRONMENT
            "PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}/tools/pi"
        )
      endif()
      execute_process(
        COMMAND ${Python3_EXECUTABLE} -c "import numpy, cv2"
        RESULT_VARIABLE DEPTHD_PYTHON_SEMANTIC_RESULT
        OUTPUT_QUIET
        ERROR_QUIET
      )
      if(DEPTHD_PYTHON_SEMANTIC_RESULT EQUAL 0)
        add_test(NAME depthd_semantic_worker_test
          COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/semantic_worker_test.py
        )
        set_tests_properties(depthd_semantic_worker_test PROPERTIES
          ENVIRONMENT
            "PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}/tools/pi"
        )
      endif()
    endif()
  endif()

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
      depthd::cpu_sgbm depthd::obstacle_cloud depthd::native_output_core
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

set(CPACK_GENERATOR "DEB")
set(CPACK_PACKAGE_NAME "nuvo-depthd")
set(CPACK_PACKAGE_CONTACT "Meridian Robotics")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Meridian Robotics")
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(DEPTHD_PACKAGE_REVISION "" CACHE STRING
  "Clean Git revision embedded in the Debian package version")
if(DEPTHD_PACKAGE_REVISION)
  string(LENGTH "${DEPTHD_PACKAGE_REVISION}" DEPTHD_PACKAGE_REVISION_LENGTH)
  if(NOT DEPTHD_PACKAGE_REVISION MATCHES "^[0-9a-f]+$" OR
     DEPTHD_PACKAGE_REVISION_LENGTH LESS 7 OR
     DEPTHD_PACKAGE_REVISION_LENGTH GREATER 40)
    message(FATAL_ERROR "DEPTHD_PACKAGE_REVISION must be a lowercase Git SHA")
  endif()
  set(CPACK_PACKAGE_VERSION
    "${PROJECT_VERSION}+git${DEPTHD_PACKAGE_REVISION}")
endif()
set(CPACK_DEBIAN_PACKAGE_DEPENDS
  "python3, python3-numpy, python3-opencv, python3-yaml, python3-picamera2, hailort (>= 4.23.0), hailort-pcie-driver"
)
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
  "Native stereo depth and Hailo semantic perception daemon for Nuvo"
)
include(CPack)
