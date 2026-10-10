cmake_minimum_required(VERSION 3.20)
if(POLICY CMP0167)
  cmake_policy(SET CMP0167 NEW)
endif()
project(depth_daemon VERSION 0.1.0 LANGUAGES CXX)

include(CTest)
include(GNUInstallDirs)
option(DEPTHD_WITH_OPENCV "Build the OpenCV CPU processing backends" ON)
option(DEPTHD_WITH_CONFIG
  "Build the native yaml-cpp configuration preflight" OFF)
option(DEPTHD_WITH_PI5_CAPTURE
  "Build the native Raspberry Pi 5 libcamera/GPIO capture smoke path" OFF)
option(DEPTHD_WITH_SEMANTICS
  "Build optional research semantic-perception components" OFF)
option(DEPTHD_WITH_HAILORT_SEMANTICS
  "Build the optional research HailoRT semantic inference backend" OFF)
option(DEPTHD_WITH_EGL_GL_DMABUF_RECTIFICATION
  "Build the Pi EGL/OpenGL ES DMABUF rectification backend" OFF)
option(DEPTHD_WITH_GATEWAY
  "Build the native HTTPS gateway" OFF)
option(DEPTHD_WITH_FFMPEG_ENCODER
  "Build the asynchronous FFmpeg image-stream encoder" OFF)
option(DEPTHD_PRODUCTION_BUILD
  "Require every backend needed by the Pi 5 shipping appliance" OFF)
option(DEPTHD_WITH_SYSTEMD "Build systemd readiness/watchdog integration" OFF)

if(DEPTHD_WITH_HAILORT_SEMANTICS AND NOT DEPTHD_WITH_SEMANTICS)
  message(FATAL_ERROR
    "DEPTHD_WITH_HAILORT_SEMANTICS requires DEPTHD_WITH_SEMANTICS=ON")
endif()

if(DEPTHD_PRODUCTION_BUILD)
  if(NOT DEPTHD_WITH_CONFIG OR NOT DEPTHD_WITH_OPENCV OR
     NOT DEPTHD_WITH_PI5_CAPTURE OR
     NOT DEPTHD_WITH_EGL_GL_DMABUF_RECTIFICATION OR
     NOT DEPTHD_WITH_GATEWAY OR NOT DEPTHD_WITH_FFMPEG_ENCODER OR
     NOT DEPTHD_WITH_SYSTEMD)
    message(FATAL_ERROR
      "DEPTHD_PRODUCTION_BUILD requires config, OpenCV, Pi 5 capture, "
      "EGL/GLES DMABUF rectification, FFmpeg encoding, HTTPS gateway, "
      "and systemd support")
  endif()
  if(DEPTHD_WITH_SEMANTICS OR DEPTHD_WITH_HAILORT_SEMANTICS)
    message(FATAL_ERROR
      "Nuvo Depth v1 production builds exclude semantic/Hailo components")
  endif()
endif()

if(DEPTHD_WITH_PI5_CAPTURE AND NOT DEPTHD_WITH_CONFIG)
  message(FATAL_ERROR
    "DEPTHD_WITH_PI5_CAPTURE requires DEPTHD_WITH_CONFIG=ON so the bounded "
    "capture-check executable has one typed configuration authority")
endif()

function(depthd_enable_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive-)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
  endif()
endfunction()

if(DEPTHD_WITH_CONFIG)
  find_package(yaml-cpp CONFIG QUIET)
  if(TARGET yaml-cpp::yaml-cpp)
    set(DEPTHD_YAML_CPP_TARGET yaml-cpp::yaml-cpp)
  elseif(TARGET yaml-cpp)
    set(DEPTHD_YAML_CPP_TARGET yaml-cpp)
  else()
    message(FATAL_ERROR
      "DEPTHD_WITH_CONFIG=ON requires yaml-cpp with a CMake package target. "
      "On Debian/Ubuntu install libyaml-cpp-dev, or configure with "
      "-DDEPTHD_WITH_CONFIG=OFF to preserve the minimal build.")
  endif()

  add_library(depthd_config STATIC
    src/config.cpp
  )
  add_library(depthd::config ALIAS depthd_config)
  target_include_directories(depthd_config
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
      $<INSTALL_INTERFACE:include>
  )
  target_compile_features(depthd_config PUBLIC cxx_std_20)
  target_link_libraries(depthd_config PUBLIC ${DEPTHD_YAML_CPP_TARGET})
  depthd_enable_warnings(depthd_config)

  add_executable(depthd
    apps/depthd_main.cpp
  )
  target_compile_features(depthd PRIVATE cxx_std_20)
  target_link_libraries(depthd PRIVATE depthd::config)
  depthd_enable_warnings(depthd)
endif()

add_library(depthd_contract STATIC
  src/contract.cpp
)
add_library(depthd::contract ALIAS depthd_contract)

add_library(depthd_products STATIC
  src/products.cpp
  src/depth_conversion.cpp
)
add_library(depthd::products ALIAS depthd_products)

add_library(depthd_rectification STATIC
  src/rectification_backend.cpp
  src/egl_gl_dmabuf_rectification_backend.cpp
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

add_library(depthd_capture STATIC
  src/core/pairer.cpp
  platform/pi5/camera_identity.cpp
  platform/pi5/pio_pwm_trigger.cpp
  platform/pi5/capture_libcamera.cpp
)
add_library(depthd::capture ALIAS depthd_capture)
target_include_directories(depthd_capture
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(depthd_capture PUBLIC cxx_std_20)
target_link_libraries(depthd_capture PUBLIC depthd::products Threads::Threads)
depthd_enable_warnings(depthd_capture)

add_library(depthd_runtime STATIC
  src/core/runtime.cpp
  src/core/native_capture_runner.cpp
  platform/linux/capture_mapping.cpp
)
add_library(depthd::runtime ALIAS depthd_runtime)
target_include_directories(depthd_runtime
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(depthd_runtime PUBLIC cxx_std_20)
target_link_libraries(depthd_runtime PUBLIC
  depthd::capture depthd::rectification depthd::products Threads::Threads
)
depthd_enable_warnings(depthd_runtime)

if(DEPTHD_WITH_CONFIG)
  add_library(depthd_exposure_control STATIC src/exposure_control.cpp)
  add_library(depthd::exposure_control ALIAS depthd_exposure_control)
  target_include_directories(depthd_exposure_control
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
      $<INSTALL_INTERFACE:include>
  )
  target_compile_features(depthd_exposure_control PUBLIC cxx_std_20)
  target_link_libraries(depthd_exposure_control PUBLIC
    depthd::runtime ${DEPTHD_YAML_CPP_TARGET}
  )
  depthd_enable_warnings(depthd_exposure_control)
  if(TARGET depthd)
    target_link_libraries(depthd PRIVATE depthd::exposure_control)
  endif()
endif()

if(DEPTHD_WITH_FFMPEG_ENCODER)
  if(NOT DEPTHD_WITH_CONFIG)
    message(FATAL_ERROR
      "DEPTHD_WITH_FFMPEG_ENCODER requires DEPTHD_WITH_CONFIG=ON")
  endif()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(DEPTHD_FFMPEG REQUIRED IMPORTED_TARGET
    libavcodec libavutil libswscale)
  add_library(depthd_image_encoder STATIC src/image_encoder.cpp)
  add_library(depthd::image_encoder ALIAS depthd_image_encoder)
  target_include_directories(depthd_image_encoder
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
      $<INSTALL_INTERFACE:include>
  )
  target_compile_features(depthd_image_encoder PUBLIC cxx_std_20)
  target_link_libraries(depthd_image_encoder PUBLIC
    depthd::runtime ${DEPTHD_YAML_CPP_TARGET}
    PRIVATE PkgConfig::DEPTHD_FFMPEG Threads::Threads
  )
  depthd_enable_warnings(depthd_image_encoder)
  if(TARGET depthd)
    target_link_libraries(depthd PRIVATE depthd::image_encoder)
    target_compile_definitions(depthd PRIVATE DEPTHD_WITH_IMAGE_ENCODER=1)
  endif()
endif()

add_library(depthd_ipc_v2 STATIC
  src/ipc_v2.cpp
  platform/linux/local_ipc.cpp
)
add_library(depthd::ipc_v2 ALIAS depthd_ipc_v2)
target_include_directories(depthd_ipc_v2
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(depthd_ipc_v2 PUBLIC cxx_std_20)
target_link_libraries(depthd_ipc_v2 PUBLIC depthd::runtime)
depthd_enable_warnings(depthd_ipc_v2)

add_library(depthd_wire_protocol STATIC
  src/wire_protocol.cpp
  src/frame_wire.cpp
)
add_library(depthd::wire_protocol ALIAS depthd_wire_protocol)
target_include_directories(depthd_wire_protocol
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(depthd_wire_protocol PUBLIC cxx_std_20)
target_link_libraries(depthd_wire_protocol PUBLIC depthd::ipc_v2)
depthd_enable_warnings(depthd_wire_protocol)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_library(depthd_local_frame_server STATIC
    platform/linux/local_frame_server.cpp
  )
  add_library(depthd::local_frame_server ALIAS depthd_local_frame_server)
  target_include_directories(depthd_local_frame_server
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
      $<INSTALL_INTERFACE:include>
  )
  target_compile_features(depthd_local_frame_server PUBLIC cxx_std_20)
  target_link_libraries(depthd_local_frame_server PUBLIC
    depthd::ipc_v2 depthd::wire_protocol Threads::Threads
  )
  depthd_enable_warnings(depthd_local_frame_server)
  if(TARGET depthd)
    target_link_libraries(depthd PRIVATE depthd::local_frame_server)
    target_compile_definitions(depthd PRIVATE DEPTHD_WITH_LOCAL_FRAME_SERVER=1)
  endif()
endif()

add_library(nuvo_depth_sdk SHARED src/sdk_c.cpp src/wire_protocol.cpp)
add_library(nuvo::depth ALIAS nuvo_depth_sdk)
target_include_directories(nuvo_depth_sdk
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(nuvo_depth_sdk PUBLIC cxx_std_20)
target_compile_definitions(nuvo_depth_sdk PRIVATE NUVO_DEPTH_BUILD=1)
set_target_properties(nuvo_depth_sdk PROPERTIES
  OUTPUT_NAME nuvo-depth VERSION ${PROJECT_VERSION} SOVERSION 1
  CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON
)
depthd_enable_warnings(nuvo_depth_sdk)

configure_file(cmake/nuvo-depth.pc.in nuvo-depth.pc @ONLY)

add_library(depthd_gateway STATIC
  src/gateway.cpp
)
add_library(depthd::gateway ALIAS depthd_gateway)
target_include_directories(depthd_gateway
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(depthd_gateway PUBLIC cxx_std_20)
target_link_libraries(depthd_gateway PUBLIC
  depthd::ipc_v2 depthd::wire_protocol
)
depthd_enable_warnings(depthd_gateway)

if(DEPTHD_WITH_GATEWAY)
  find_package(Boost 1.74 REQUIRED COMPONENTS system)
  find_package(OpenSSL 3.0 REQUIRED)
  add_executable(nuvo-gateway apps/nuvo_gateway_main.cpp)
  target_compile_features(nuvo-gateway PRIVATE cxx_std_20)
  target_link_libraries(nuvo-gateway PRIVATE
    Boost::system OpenSSL::SSL OpenSSL::Crypto Threads::Threads
    depthd::local_frame_server
    depthd::gateway
  )
  if(TARGET depthd_image_encoder)
    target_link_libraries(nuvo-gateway PRIVATE depthd::image_encoder)
    target_compile_definitions(nuvo-gateway PRIVATE
      DEPTHD_WITH_IMAGE_ENCODER=1)
  endif()
  depthd_enable_warnings(nuvo-gateway)
endif()

if(DEPTHD_WITH_PI5_CAPTURE)
  if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR
      "DEPTHD_WITH_PI5_CAPTURE requires Linux, libcamera, and the GPIO "
      "character-device API")
  endif()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(DEPTHD_LIBCAMERA REQUIRED IMPORTED_TARGET libcamera)
  target_compile_definitions(depthd_capture PRIVATE
    DEPTHD_WITH_PI5_CAPTURE=1
  )
  target_link_libraries(depthd_capture PRIVATE PkgConfig::DEPTHD_LIBCAMERA)
endif()

if(TARGET depthd)
  target_link_libraries(depthd PRIVATE depthd::capture)
endif()

if(DEPTHD_WITH_SYSTEMD)
  if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT TARGET depthd)
    message(FATAL_ERROR "DEPTHD_WITH_SYSTEMD requires a Linux depthd target")
  endif()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(DEPTHD_SYSTEMD REQUIRED IMPORTED_TARGET libsystemd)
  target_link_libraries(depthd PRIVATE PkgConfig::DEPTHD_SYSTEMD)
  target_compile_definitions(depthd PRIVATE DEPTHD_WITH_SYSTEMD=1)
endif()

if(DEPTHD_WITH_SEMANTICS)
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
endif()

add_library(depthd_native_output_core STATIC
  src/native_output.cpp
)
add_library(depthd::native_output_core ALIAS depthd_native_output_core)

add_library(depthd_native_output SHARED
  src/native_output_c.cpp
)

if(DEPTHD_WITH_SEMANTICS)
  add_library(depthd_semantic_output SHARED
    src/semantic_output_c.cpp
  )

  add_library(depthd_native_semantic SHARED
    src/native_semantic_runtime.cpp
  )
endif()

target_include_directories(depthd_contract
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(depthd_contract PUBLIC cxx_std_20)
depthd_enable_warnings(depthd_contract)

target_include_directories(depthd_products
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(depthd_products PUBLIC cxx_std_20)
depthd_enable_warnings(depthd_products)

target_include_directories(depthd_rectification
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(depthd_rectification PUBLIC cxx_std_20)
set_target_properties(depthd_rectification PROPERTIES
  POSITION_INDEPENDENT_CODE ON
)
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

if(DEPTHD_WITH_SEMANTICS)
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
endif()

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

if(DEPTHD_WITH_SEMANTICS)
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
endif()

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
  find_package(OpenCV 4.6 QUIET COMPONENTS core imgproc imgcodecs calib3d)
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
    set_target_properties(depthd_opencv_rectification PROPERTIES
      POSITION_INDEPENDENT_CODE ON
    )
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
    target_link_libraries(depthd_cpu_sgbm PUBLIC
      depthd::contract depthd::products
      PRIVATE ${OpenCV_LIBS}
    )
    depthd_enable_warnings(depthd_cpu_sgbm)

    if(DEPTHD_WITH_CONFIG)
      add_library(depthd_calibration_store STATIC src/calibration_store.cpp)
      add_library(depthd::calibration_store ALIAS depthd_calibration_store)
      target_include_directories(depthd_calibration_store
        PUBLIC
          $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
          $<INSTALL_INTERFACE:include>
      )
      target_include_directories(depthd_calibration_store SYSTEM PRIVATE
        ${OpenCV_INCLUDE_DIRS}
      )
      target_compile_features(depthd_calibration_store PUBLIC cxx_std_20)
      target_link_libraries(depthd_calibration_store PUBLIC
        depthd::config depthd::products depthd::rectification
        PRIVATE ${OpenCV_LIBS}
      )
      depthd_enable_warnings(depthd_calibration_store)

      add_library(depthd_runtime_factory STATIC
        src/core/runtime_factory.cpp
      )
      add_library(depthd::runtime_factory ALIAS depthd_runtime_factory)
      target_include_directories(depthd_runtime_factory
        PUBLIC
          $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
          $<INSTALL_INTERFACE:include>
      )
      target_compile_features(depthd_runtime_factory PUBLIC cxx_std_20)
      target_link_libraries(depthd_runtime_factory PUBLIC
        depthd::runtime depthd::config depthd::calibration_store
        depthd::opencv_rectification depthd::cpu_sgbm
      )
      depthd_enable_warnings(depthd_runtime_factory)

      target_link_libraries(depthd PRIVATE
        depthd::runtime_factory depthd::ipc_v2 depthd::wire_protocol
        ${OpenCV_LIBS}
      )
      target_include_directories(depthd SYSTEM PRIVATE ${OpenCV_INCLUDE_DIRS})
      target_compile_definitions(depthd PRIVATE DEPTHD_WITH_NATIVE_PIPELINE=1)
    endif()

    add_executable(depthd_sgbm_benchmark tools/sgbm_benchmark.cpp)
    target_compile_features(depthd_sgbm_benchmark PRIVATE cxx_std_20)
    target_include_directories(depthd_sgbm_benchmark SYSTEM PRIVATE ${OpenCV_INCLUDE_DIRS})
    target_link_libraries(depthd_sgbm_benchmark PRIVATE depthd::cpu_sgbm ${OpenCV_LIBS})
    depthd_enable_warnings(depthd_sgbm_benchmark)

    if(DEPTHD_WITH_CONFIG)
      add_executable(depthd_sintel_benchmark tools/sintel_stereo_benchmark.cpp)
      target_compile_features(depthd_sintel_benchmark PRIVATE cxx_std_20)
      target_include_directories(depthd_sintel_benchmark SYSTEM PRIVATE ${OpenCV_INCLUDE_DIRS})
      target_link_libraries(depthd_sintel_benchmark PRIVATE
        depthd::cpu_sgbm depthd::config ${OpenCV_LIBS})
      depthd_enable_warnings(depthd_sintel_benchmark)
      if(BUILD_TESTING)
        add_test(NAME depthd_sintel_benchmark_self_test
          COMMAND depthd_sintel_benchmark --self-test)
      endif()
    endif()
  else()
    message(STATUS "OpenCV 4.6 or newer not found; skipping the CPU SGBM backend")
  endif()
endif()

if(TARGET depthd_image_encoder)
  add_executable(depthd_encoder_benchmark tools/encoder_benchmark.cpp)
  target_compile_features(depthd_encoder_benchmark PRIVATE cxx_std_20)
  target_link_libraries(depthd_encoder_benchmark PRIVATE
    depthd::image_encoder)
  depthd_enable_warnings(depthd_encoder_benchmark)
endif()

if(DEPTHD_WITH_EGL_GL_DMABUF_RECTIFICATION)
  if(NOT UNIX)
    message(FATAL_ERROR
      "DEPTHD_WITH_EGL_GL_DMABUF_RECTIFICATION requires Linux DMABUF support")
  endif()
  if(NOT TARGET depthd_opencv_rectification)
    message(FATAL_ERROR
      "DEPTHD_WITH_EGL_GL_DMABUF_RECTIFICATION requires the cpu_opencv fallback")
  endif()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(DEPTHD_EGL REQUIRED IMPORTED_TARGET egl)
  pkg_check_modules(DEPTHD_GLESV2 REQUIRED IMPORTED_TARGET glesv2)

  add_library(depthd_egl_gl_dmabuf_rectification STATIC
    src/egl_gl_dmabuf_platform.cpp
  )
  add_library(depthd::egl_gl_dmabuf_rectification ALIAS
    depthd_egl_gl_dmabuf_rectification)
  target_include_directories(depthd_egl_gl_dmabuf_rectification
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
      $<INSTALL_INTERFACE:include>
  )
  target_compile_features(depthd_egl_gl_dmabuf_rectification PUBLIC cxx_std_20)
  target_link_libraries(depthd_egl_gl_dmabuf_rectification
    PUBLIC depthd::rectification
    PRIVATE
      depthd::opencv_rectification
      PkgConfig::DEPTHD_EGL
      PkgConfig::DEPTHD_GLESV2
  )
  set_target_properties(depthd_egl_gl_dmabuf_rectification PROPERTIES
    POSITION_INDEPENDENT_CODE ON
  )
  depthd_enable_warnings(depthd_egl_gl_dmabuf_rectification)

  if(TARGET depthd_runtime_factory)
    target_link_libraries(depthd_runtime_factory PRIVATE
      depthd::egl_gl_dmabuf_rectification
    )
    target_compile_definitions(depthd_runtime_factory PRIVATE
      DEPTHD_RUNTIME_WITH_EGL_GL=1
    )
  endif()

  add_library(depthd_egl_gl_dmabuf_rectification_c SHARED
    src/egl_gl_dmabuf_rectification_c.cpp
  )
  target_include_directories(depthd_egl_gl_dmabuf_rectification_c
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
      $<INSTALL_INTERFACE:include>
  )
  target_compile_features(depthd_egl_gl_dmabuf_rectification_c PUBLIC cxx_std_20)
  target_link_libraries(depthd_egl_gl_dmabuf_rectification_c PRIVATE
    depthd::egl_gl_dmabuf_rectification
  )
  set_target_properties(depthd_egl_gl_dmabuf_rectification_c PROPERTIES
    OUTPUT_NAME depthd_egl_gl_dmabuf_rectification
  )
  depthd_enable_warnings(depthd_egl_gl_dmabuf_rectification_c)
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

install(TARGETS depthd_native_output
  LIBRARY DESTINATION "${DEPTHD_LIBEXEC_DIR}"
  RUNTIME DESTINATION "${DEPTHD_LIBEXEC_DIR}"
)
if(TARGET depthd)
  # Keep the historical libexec location for rollback tooling while installing
  # the native appliance entry point in the normal executable path.
  install(TARGETS depthd
    RUNTIME DESTINATION "${DEPTHD_LIBEXEC_DIR}"
  )
  install(TARGETS depthd RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()
if(TARGET nuvo-gateway)
  install(TARGETS nuvo-gateway
    RUNTIME DESTINATION "lib/nuvo-depth"
  )
endif()
install(TARGETS nuvo_depth_sdk
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
install(FILES include/nuvo/depth.h include/nuvo/stream_decoder.hpp
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/nuvo"
)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/nuvo-depth.pc"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig"
)
install(DIRECTORY sdk/python/nuvo_depth
  DESTINATION "lib/python3/dist-packages"
  FILES_MATCHING PATTERN "*.py"
)
install(PROGRAMS tools/nuvoctl.py
  DESTINATION "${CMAKE_INSTALL_BINDIR}" RENAME nuvoctl
)
install(PROGRAMS tools/nuvo_record.py
  DESTINATION "${CMAKE_INSTALL_BINDIR}" RENAME nuvo-record
)
install(PROGRAMS tools/nuvo_replay.py
  DESTINATION "${CMAKE_INSTALL_BINDIR}" RENAME nuvo-replay
)
install(PROGRAMS tools/nuvo_system_agent.py
  DESTINATION "lib/nuvo-depth" RENAME nuvo-system-agent
)
install(PROGRAMS tools/calibration/nuvo_calibration.py
  DESTINATION "lib/nuvo-depth" RENAME nuvo-calibration
)
install(DIRECTORY web/
  DESTINATION "${CMAKE_INSTALL_DATADIR}/nuvo-depth/web"
)
install(DIRECTORY presets/
  DESTINATION "${CMAKE_INSTALL_DATADIR}/nuvo-depth/presets"
)
install(FILES config/calibration/pi5-profile-matrix.example.json
  DESTINATION "${CMAKE_INSTALL_DATADIR}/nuvo-depth/calibration"
)
install(FILES config/nuvo_pi5_depth_100mm.yaml config/appliance.yaml
  config/encoding.json config/stereo.json config/exposure.json
  config/gateway-qualification.example.json
  DESTINATION "${CMAKE_INSTALL_DATADIR}/nuvo-depth/examples"
)
install(FILES config/pi/depthd-imx296-trigger.conf
  DESTINATION "${CMAKE_INSTALL_DATADIR}/nuvo-depth/platform/pi5"
)
install(DIRECTORY qualification/
  DESTINATION "${CMAKE_INSTALL_DATADIR}/nuvo-depth/qualification"
)
install(PROGRAMS
  tools/qualification/run_qualification.py
  tools/qualification/analyze_depth_accuracy.py
  DESTINATION "lib/nuvo-depth"
)
install(PROGRAMS
  tools/release/generate_spdx.py
  tools/release/stage_apt_repository.py
  DESTINATION "lib/nuvo-depth"
)
install(FILES
  deployment/systemd/nuvo-depth.target
  deployment/systemd/nuvo-trigger-prepare.service
  deployment/systemd/nuvo-depthd-native.service
  deployment/systemd/nuvo-gateway.service
  deployment/systemd/nuvo-calibration@.service
  deployment/systemd/nuvo-usb-gadget.service
  deployment/systemd/nuvo-system-agent.socket
  deployment/systemd/nuvo-system-agent@.service
  deployment/systemd/nuvo-transport-rollback.service
  deployment/systemd/nuvo-transport-rollback.timer
  DESTINATION "lib/systemd/system"
)
install(PROGRAMS tools/pi/nuvo_trigger_prepare.py
  DESTINATION "lib/nuvo-depth" RENAME nuvo-trigger-prepare
)
install(FILES deployment/systemd/nuvo-depth.sysusers.conf
  DESTINATION "lib/sysusers.d" RENAME nuvo-depth.conf
)
install(FILES deployment/systemd/nuvo-depth.tmpfiles.conf
  DESTINATION "lib/tmpfiles.d" RENAME nuvo-depth.conf
)
install(FILES deployment/avahi/nuvo-depth.service
  DESTINATION "${CMAKE_INSTALL_DATADIR}/avahi/services"
)
install(FILES
  docs/user-guide.md
  docs/operations-troubleshooting.md
  docs/pi5-release-readiness-20260914.md
  docs/pi5-image-stream-encoding.md
  docs/stereo-auto-exposure.md
  docs/stereo-exposure-evaluation-20260916.md
  docs/pi5-fullres-stereo-flicker-trace.md
  docs/production-implementation-status.md
  docs/native-runtime-ipc-gateway.md
  docs/pi5-depth-100mm-profile.md
  docs/performance-profiles.md
  docs/pi5-hil-qualification-20260909.md
  docs/hardware-qualification-handoff-prompt.md
  docs/nuvo-depth-production-oracle-prompt.md
  deployment/README.md
  DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/nuvo-depth"
)
install(PROGRAMS
  tools/pi/profile_async_zero_copy_pipeline.py
  tools/pi/async_stereo_capture.py
  tools/pi/capture_stream_profile.py
  tools/pi/capture_rectified_pair.py
  tools/pi/profile_cpu_pipeline.py
  tools/pi/stereo_auto_exposure.py
  tools/pi/validate_exposure_live.py
  tools/pi/depthd_native_output.py
  tools/pi/depthd_egl_gl_rectification.py
  DESTINATION "${DEPTHD_LIBEXEC_DIR}"
)
if(TARGET depthd_egl_gl_dmabuf_rectification_c)
  install(TARGETS depthd_egl_gl_dmabuf_rectification_c
    LIBRARY DESTINATION "${DEPTHD_LIBEXEC_DIR}"
  )
endif()
install(PROGRAMS deployment/depthd_assets.py
  DESTINATION "${CMAKE_INSTALL_SBINDIR}"
  RENAME depthd-assets
)
install(FILES
  config/nuvo_pi5_depth_100mm.yaml
  DESTINATION "${CMAKE_INSTALL_DATADIR}/nuvo-depthd/runtime-seed/config"
)
install(FILES
  config/calibration/nuvo_depth_100mm/front_left.yaml
  config/calibration/nuvo_depth_100mm/front_right.yaml
  DESTINATION
    "${CMAKE_INSTALL_DATADIR}/nuvo-depthd/runtime-seed/config/calibration/nuvo_depth_100mm"
)
install(FILES deployment/assets/nuvo-pi5-depth-100mm-assets.json
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
    add_test(NAME depthd_trigger_prepare_test
      COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/trigger_prepare_test.py
    )
    add_test(NAME depthd_assets_test
      COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/depthd_assets_test.py
    )
    add_test(NAME depthd_pipeline_deadline_accounting_test
      COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/pipeline_deadline_accounting_test.py
    )
    add_test(NAME depthd_python_sdk_test
      COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/python_sdk_test.py
    )
    add_test(NAME depthd_frame_wire_sdk_roundtrip_test
      COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/frame_wire_sdk_roundtrip_test.py
        $<TARGET_FILE:depthd_frame_wire_test>
    )
    add_test(NAME depthd_calibration_activation_test
      COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/calibration_activation_test.py
    )
    add_test(NAME depthd_calibration_session_test
      COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/calibration_session_test.py
    )
    add_test(NAME depthd_calibration_charuco_synthetic_test
      COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/calibration_charuco_synthetic_test.py
    )
    add_test(NAME depthd_nuvoctl_test
      COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/nuvoctl_test.py
    )
    add_test(NAME depthd_system_agent_test
      COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/system_agent_test.py
    )
    add_test(NAME depthd_web_ui_static_test
      COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/web_ui_static_test.py
    )
    if(DEPTHD_WITH_CONFIG)
      add_test(NAME depthd_performance_profiles_test
        COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/tests/performance_profiles_test.py
          $<TARGET_FILE:depthd>
      )
    endif()
    add_test(NAME depthd_qualification_tools_test
      COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/qualification_tools_test.py
    )
    if(TARGET nuvo-gateway)
      add_test(NAME depthd_gateway_integration_test
        COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/tests/gateway_integration_test.py
          $<TARGET_FILE:nuvo-gateway> ${CMAKE_CURRENT_SOURCE_DIR}/web
      )
    endif()
  endif()

  if(DEPTHD_WITH_CONFIG)
    add_executable(depthd_exposure_control_test
      tests/exposure_control_test.cpp
    )
    target_compile_features(depthd_exposure_control_test PRIVATE cxx_std_20)
    target_compile_definitions(depthd_exposure_control_test PRIVATE
      DEPTHD_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(depthd_exposure_control_test PRIVATE
      depthd::exposure_control
    )
    depthd_enable_warnings(depthd_exposure_control_test)
    add_test(NAME depthd_exposure_control_test
      COMMAND depthd_exposure_control_test
    )

    add_executable(depthd_config_test tests/config_test.cpp)
    target_compile_features(depthd_config_test PRIVATE cxx_std_20)
    target_compile_definitions(depthd_config_test PRIVATE
      DEPTHD_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(depthd_config_test PRIVATE depthd::config)
    depthd_enable_warnings(depthd_config_test)
    add_test(NAME depthd_config_test COMMAND depthd_config_test)

    add_executable(depthd_architecture_coherence_test
      tests/architecture_coherence_test.cpp
    )
    target_compile_features(depthd_architecture_coherence_test PRIVATE cxx_std_20)
    target_compile_definitions(depthd_architecture_coherence_test PRIVATE
      DEPTHD_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(depthd_architecture_coherence_test PRIVATE
      depthd::config depthd::capture depthd::products depthd::ipc_v2
      depthd::gateway depthd::wire_protocol
    )
    depthd_enable_warnings(depthd_architecture_coherence_test)
    add_test(NAME depthd_architecture_coherence_test
      COMMAND depthd_architecture_coherence_test
    )
  endif()

  add_executable(depthd_contract_test tests/contract_test.cpp)
  target_link_libraries(depthd_contract_test PRIVATE depthd::contract)
  add_test(NAME depthd_contract_test COMMAND depthd_contract_test)

  add_executable(depthd_products_test tests/products_test.cpp)
  target_link_libraries(depthd_products_test PRIVATE depthd::products)
  depthd_enable_warnings(depthd_products_test)
  add_test(NAME depthd_products_test COMMAND depthd_products_test)

  add_executable(depthd_camera_identity_test tests/camera_identity_test.cpp)
  target_link_libraries(depthd_camera_identity_test PRIVATE depthd::capture)
  depthd_enable_warnings(depthd_camera_identity_test)
  add_test(NAME depthd_camera_identity_test COMMAND depthd_camera_identity_test)

  add_executable(depthd_capture_lease_test tests/capture_lease_test.cpp)
  target_link_libraries(depthd_capture_lease_test PRIVATE depthd::capture)
  depthd_enable_warnings(depthd_capture_lease_test)
  add_test(NAME depthd_capture_lease_test COMMAND depthd_capture_lease_test)
  add_executable(depthd_pio_pwm_trigger_test tests/pio_pwm_trigger_test.cpp)
  target_link_libraries(depthd_pio_pwm_trigger_test PRIVATE depthd::capture)
  depthd_enable_warnings(depthd_pio_pwm_trigger_test)
  add_test(NAME depthd_pio_pwm_trigger_test COMMAND depthd_pio_pwm_trigger_test)

  add_executable(depthd_pairer_test tests/pairer_test.cpp)
  target_link_libraries(depthd_pairer_test PRIVATE depthd::capture)
  depthd_enable_warnings(depthd_pairer_test)
  add_test(NAME depthd_pairer_test COMMAND depthd_pairer_test)

  add_executable(depthd_runtime_test tests/runtime_test.cpp)
  target_link_libraries(depthd_runtime_test PRIVATE depthd::runtime)
  depthd_enable_warnings(depthd_runtime_test)
  add_test(NAME depthd_runtime_test COMMAND depthd_runtime_test)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(depthd_capture_mapping_cache_test tests/capture_mapping_cache_test.cpp)
    target_link_libraries(depthd_capture_mapping_cache_test PRIVATE depthd::runtime)
    depthd_enable_warnings(depthd_capture_mapping_cache_test)
    add_test(NAME depthd_capture_mapping_cache_test COMMAND depthd_capture_mapping_cache_test)
  endif()

  add_executable(depthd_native_capture_runner_test
    tests/native_capture_runner_test.cpp
  )
  target_link_libraries(depthd_native_capture_runner_test PRIVATE depthd::runtime)
  depthd_enable_warnings(depthd_native_capture_runner_test)
  add_test(NAME depthd_native_capture_runner_test
    COMMAND depthd_native_capture_runner_test
  )

  if(TARGET depthd_image_encoder)
    target_link_libraries(depthd_native_capture_runner_test PRIVATE
      depthd::image_encoder)
    target_compile_definitions(depthd_native_capture_runner_test PRIVATE
      DEPTHD_WITH_IMAGE_ENCODER=1)
    add_executable(depthd_image_encoder_test tests/image_encoder_test.cpp)
    target_link_libraries(depthd_image_encoder_test PRIVATE
      depthd::image_encoder PkgConfig::DEPTHD_FFMPEG)
    depthd_enable_warnings(depthd_image_encoder_test)
    add_test(NAME depthd_image_encoder_test COMMAND depthd_image_encoder_test)
  endif()

  add_executable(depthd_ipc_v2_test tests/ipc_v2_test.cpp)
  target_link_libraries(depthd_ipc_v2_test PRIVATE depthd::ipc_v2)
  depthd_enable_warnings(depthd_ipc_v2_test)
  add_test(NAME depthd_ipc_v2_test COMMAND depthd_ipc_v2_test)

  add_executable(depthd_wire_protocol_test tests/wire_protocol_test.cpp)
  target_link_libraries(depthd_wire_protocol_test PRIVATE
    depthd::wire_protocol
  )
  depthd_enable_warnings(depthd_wire_protocol_test)
  add_test(NAME depthd_wire_protocol_test COMMAND depthd_wire_protocol_test)

  add_executable(depthd_frame_wire_test tests/frame_wire_test.cpp)
  target_link_libraries(depthd_frame_wire_test PRIVATE depthd::wire_protocol)
  depthd_enable_warnings(depthd_frame_wire_test)
  add_test(NAME depthd_frame_wire_test COMMAND depthd_frame_wire_test)

  add_executable(depthd_sdk_c_test tests/sdk_c_test.cpp)
  target_link_libraries(depthd_sdk_c_test PRIVATE
    nuvo::depth depthd::wire_protocol
  )
  depthd_enable_warnings(depthd_sdk_c_test)
  add_test(NAME depthd_sdk_c_test COMMAND depthd_sdk_c_test)

  add_executable(depthd_gateway_test tests/gateway_test.cpp)
  target_link_libraries(depthd_gateway_test PRIVATE depthd::gateway)
  depthd_enable_warnings(depthd_gateway_test)
  add_test(NAME depthd_gateway_test COMMAND depthd_gateway_test)

  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(depthd_local_ipc_linux_test
      tests/local_ipc_linux_test.cpp
    )
    target_link_libraries(depthd_local_ipc_linux_test PRIVATE
      depthd::ipc_v2 Threads::Threads
    )
    depthd_enable_warnings(depthd_local_ipc_linux_test)
    add_test(NAME depthd_local_ipc_linux_test
      COMMAND depthd_local_ipc_linux_test
    )

    add_executable(depthd_local_frame_server_test
      tests/local_frame_server_test.cpp
    )
    target_link_libraries(depthd_local_frame_server_test PRIVATE
      depthd::local_frame_server Threads::Threads
    )
    depthd_enable_warnings(depthd_local_frame_server_test)
    add_test(NAME depthd_local_frame_server_test
      COMMAND depthd_local_frame_server_test
    )
  endif()

  add_executable(depthd_depth_conversion_test tests/depth_conversion_test.cpp)
  target_link_libraries(depthd_depth_conversion_test PRIVATE depthd::products)
  depthd_enable_warnings(depthd_depth_conversion_test)
  add_test(NAME depthd_depth_conversion_test COMMAND depthd_depth_conversion_test)

  add_executable(depthd_disparity_validity_test
    tests/disparity_validity_test.cpp
  )
  target_link_libraries(depthd_disparity_validity_test PRIVATE depthd::products)
  depthd_enable_warnings(depthd_disparity_validity_test)
  add_test(NAME depthd_disparity_validity_test
    COMMAND depthd_disparity_validity_test
  )

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

  add_executable(depthd_egl_gl_dmabuf_rectification_backend_test
    tests/egl_gl_dmabuf_rectification_backend_test.cpp
  )
  target_link_libraries(depthd_egl_gl_dmabuf_rectification_backend_test
    PRIVATE depthd::rectification
  )
  depthd_enable_warnings(depthd_egl_gl_dmabuf_rectification_backend_test)
  add_test(NAME depthd_egl_gl_dmabuf_rectification_backend_test
    COMMAND depthd_egl_gl_dmabuf_rectification_backend_test
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

  if(DEPTHD_WITH_SEMANTICS)
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
  endif()

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
        add_test(NAME depthd_dmabuf_capture_adapter_test
          COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/dmabuf_capture_adapter_test.py
        )
        if(TARGET depthd_egl_gl_dmabuf_rectification_c)
          add_test(NAME depthd_egl_gl_ctypes_fallback_test
            COMMAND ${Python3_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/egl_gl_ctypes_fallback_test.py
              --library $<TARGET_FILE:depthd_egl_gl_dmabuf_rectification_c>
          )
          set_tests_properties(depthd_egl_gl_ctypes_fallback_test PROPERTIES
            ENVIRONMENT
              "PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}/tools/pi"
          )
        endif()
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
        RESULT_VARIABLE DEPTHD_PYTHON_OPENCV_RESULT
        OUTPUT_QUIET
        ERROR_QUIET
      )
      if(DEPTHD_PYTHON_OPENCV_RESULT EQUAL 0)
        add_test(NAME depthd_egl_gl_map_adapter_test
          COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/egl_gl_map_adapter_test.py
        )
      endif()
      if(DEPTHD_WITH_SEMANTICS AND DEPTHD_PYTHON_OPENCV_RESULT EQUAL 0)
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
    target_include_directories(depthd_opencv_rectification_backend_test SYSTEM PRIVATE ${OpenCV_INCLUDE_DIRS})
    target_link_libraries(depthd_opencv_rectification_backend_test
      PRIVATE depthd::opencv_rectification
    )
    depthd_enable_warnings(depthd_opencv_rectification_backend_test)
    add_test(NAME depthd_opencv_rectification_backend_test
      COMMAND depthd_opencv_rectification_backend_test
    )
  endif()

  if(TARGET depthd_cpu_sgbm)
    if(TARGET depthd_calibration_store)
      add_executable(depthd_calibration_store_test
        tests/calibration_store_test.cpp
      )
      target_compile_features(depthd_calibration_store_test PRIVATE cxx_std_20)
      target_compile_definitions(depthd_calibration_store_test PRIVATE
        DEPTHD_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
      )
      target_link_libraries(depthd_calibration_store_test PRIVATE
        depthd::calibration_store
      )
      depthd_enable_warnings(depthd_calibration_store_test)
      add_test(NAME depthd_calibration_store_test
        COMMAND depthd_calibration_store_test
      )

      add_executable(depthd_runtime_factory_test
        tests/runtime_factory_test.cpp
      )
      target_compile_features(depthd_runtime_factory_test PRIVATE cxx_std_20)
      target_compile_definitions(depthd_runtime_factory_test PRIVATE
        DEPTHD_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
      )
      target_link_libraries(depthd_runtime_factory_test PRIVATE
        depthd::runtime_factory
      )
      depthd_enable_warnings(depthd_runtime_factory_test)
      add_test(NAME depthd_runtime_factory_test
        COMMAND depthd_runtime_factory_test
      )
    endif()
    add_executable(depthd_runtime_opencv_integration_test
      tests/runtime_opencv_integration_test.cpp
    )
    target_include_directories(depthd_runtime_opencv_integration_test SYSTEM
      PRIVATE ${OpenCV_INCLUDE_DIRS}
    )
    target_link_libraries(depthd_runtime_opencv_integration_test PRIVATE
      depthd::runtime depthd::opencv_rectification depthd::cpu_sgbm
      ${OpenCV_LIBS}
    )
    depthd_enable_warnings(depthd_runtime_opencv_integration_test)
    add_test(NAME depthd_runtime_opencv_integration_test
      COMMAND depthd_runtime_opencv_integration_test
    )

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
    target_link_libraries(depthd_cpu_sgbm_test PRIVATE
      depthd::cpu_sgbm depthd::opencv_rectification ${OpenCV_LIBS})
    depthd_enable_warnings(depthd_cpu_sgbm_test)
    add_test(NAME depthd_cpu_sgbm_test COMMAND depthd_cpu_sgbm_test)
  endif()
endif()

if(BUILD_TESTING)
  # Release hardware builds must exercise the same assertions as host tests.
  # CMake's Release flags otherwise define NDEBUG and silently remove them.
  get_property(DEPTHD_TEST_CANDIDATES DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
  foreach(DEPTHD_TEST_TARGET IN LISTS DEPTHD_TEST_CANDIDATES)
    if(DEPTHD_TEST_TARGET MATCHES "_test$")
      if(MSVC)
        target_compile_options(${DEPTHD_TEST_TARGET} PRIVATE /UNDEBUG)
      else()
        target_compile_options(${DEPTHD_TEST_TARGET} PRIVATE -UNDEBUG)
      endif()
    endif()
  endforeach()
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
  "python3, python3-numpy, python3-opencv (>= 4.6), python3-yaml, python3-picamera2, libegl1, libgles2"
)
if(DEPTHD_PACKAGE_REVISION AND
   NOT DEPTHD_WITH_EGL_GL_DMABUF_RECTIFICATION)
  message(FATAL_ERROR
    "Production packages require DEPTHD_WITH_EGL_GL_DMABUF_RECTIFICATION=ON")
endif()
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
  "Native 100 mm stereo depth camera daemon for Raspberry Pi 5"
)
include(CPack)
