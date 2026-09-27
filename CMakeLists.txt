cmake_minimum_required(VERSION 3.30)
project(FlashNextVelocityEngine LANGUAGES C CXX HIP)

set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_HIP_STANDARD 20)
set(CMAKE_HIP_STANDARD_REQUIRED ON)
set(CMAKE_HIP_ARCHITECTURES gfx1151 CACHE STRING "Strix Halo target")

if(NOT WIN32)
  message(FATAL_ERROR "FlashNextVelocity Engine builds on Windows only")
endif()
if(NOT CMAKE_HIP_ARCHITECTURES STREQUAL "gfx1151")
  message(FATAL_ERROR "This build is intentionally pinned to gfx1151")
endif()

set(GUFO_ROOT "" CACHE PATH "Pinned Gufo Windows checkout supplied by scripts/build.ps1")
if(NOT EXISTS "${GUFO_ROOT}/src/models/qwen38_flash_next/engine.cpp")
  message(FATAL_ERROR "Pinned Gufo source is missing. Run BUILD.bat.")
endif()

# Use the pinned Windows port's POSIX bridge while retaining FlashNextVelocity's
# own native runtime and model-specific Windows file handling.
include_directories(BEFORE ${GUFO_ROOT}/compat/win32/include)
set(CMAKE_HIP_FLAGS "${CMAKE_HIP_FLAGS} -I${GUFO_ROOT}/compat/win32/hip_include")
add_compile_definitions(_CRT_DECLARE_NONSTDC_NAMES=0 _CRT_SECURE_NO_WARNINGS _USE_MATH_DEFINES)
add_link_options(LINKER:/STACK:8388608)
add_library(gufo_posix_compat STATIC
  ${GUFO_ROOT}/compat/win32/posix.cpp
  ${GUFO_ROOT}/compat/win32/spawn.cpp)
target_link_libraries(gufo_posix_compat PUBLIC ws2_32 psapi)

find_package(Threads REQUIRED)
find_package(ICU REQUIRED COMPONENTS uc i18n)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(PNG REQUIRED)
find_package(JPEG REQUIRED)
find_package(CURL REQUIRED)
find_package(hip CONFIG REQUIRED)
find_package(hipblas CONFIG REQUIRED)
find_package(hipblaslt CONFIG REQUIRED)
find_package(rocblas CONFIG REQUIRED)
find_path(HIPCUB_INCLUDE_DIR hipcub/hipcub.hpp REQUIRED)
find_path(ROCPRIM_INCLUDE_DIR rocprim/rocprim.hpp REQUIRED)
find_path(ROCWMMA_INCLUDE_DIR rocwmma/rocwmma.hpp REQUIRED)

add_library(fnv_core STATIC
  ${GUFO_ROOT}/src/core/gguf_reader.cpp
  ${GUFO_ROOT}/src/core/sampling.cpp
  ${GUFO_ROOT}/src/core/crypto/sha256.cpp
  ${GUFO_ROOT}/src/core/quant/ggml_dequant.cpp
  ${GUFO_ROOT}/src/core/image.cpp
  ${GUFO_ROOT}/src/core/hip/weight_upload.cpp
  ${GUFO_ROOT}/src/models/qwen/tokenizer.cpp
  ${GUFO_ROOT}/src/models/qwen/chat_template.cpp
  ${GUFO_ROOT}/src/models/qwen/vision/prompt.cpp
  ${GUFO_ROOT}/src/models/qwen/vision/encoder.hip
  ${GUFO_ROOT}/src/models/qwen/vision/device_input.hip
  ${GUFO_ROOT}/src/models/qwen/hip/kernels/sample.hip
  ${GUFO_ROOT}/src/models/qwen/hip/kernels/hipblaslt_gemm.hip
  src/native/win_file.cpp
  src/native/win_plan_database.cpp
)

set_source_files_properties(
  ${GUFO_ROOT}/src/models/qwen/vision/encoder.hip
  ${GUFO_ROOT}/src/models/qwen/vision/device_input.hip
  ${GUFO_ROOT}/src/models/qwen/hip/kernels/sample.hip
  ${GUFO_ROOT}/src/models/qwen/hip/kernels/hipblaslt_gemm.hip
  PROPERTIES LANGUAGE HIP)

set_source_files_properties(
  ${GUFO_ROOT}/src/models/qwen/vision/encoder.hip
  ${GUFO_ROOT}/src/models/qwen/vision/device_input.hip
  ${GUFO_ROOT}/src/models/qwen/hip/kernels/sample.hip
  ${GUFO_ROOT}/src/models/qwen/hip/kernels/hipblaslt_gemm.hip
  PROPERTIES COMPILE_OPTIONS "-O3;-ffast-math;-fno-finite-math-only;-Wno-unused-command-line-argument")

target_include_directories(fnv_core PUBLIC
  ${GUFO_ROOT}
  ${CMAKE_CURRENT_SOURCE_DIR}/src/native)
target_include_directories(fnv_core SYSTEM PRIVATE
  ${HIPCUB_INCLUDE_DIR}
  ${ROCPRIM_INCLUDE_DIR}
  ${ROCWMMA_INCLUDE_DIR})
target_compile_definitions(fnv_core PUBLIC ENGINE_ENABLE_HIP=1 NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(fnv_core PRIVATE
  $<$<COMPILE_LANGUAGE:CXX>:-O2>
  $<$<COMPILE_LANGUAGE:HIP>:-O3;-ffast-math;-fno-finite-math-only;-Wno-unused-command-line-argument>)
target_link_libraries(fnv_core PUBLIC
  Threads::Threads ICU::uc ICU::i18n OpenSSL::Crypto PNG::PNG JPEG::JPEG
  CURL::libcurl hip::host roc::hipblas roc::hipblaslt roc::rocblas
  gufo_posix_compat ws2_32 bcrypt)

# Gufo's qwen38_flash_next CMakeLists links a target named gufo_core.
# Expose our Windows core implementation under that expected target name so
# CMake resolves the dependency as a target instead of emitting -lgufo_core.lib.
add_library(gufo_core ALIAS fnv_core)

set(ENGINE_ENABLE_HIP ON CACHE BOOL "" FORCE)
add_subdirectory(${GUFO_ROOT}/src/models/qwen38_flash_next ${CMAKE_BINARY_DIR}/gufo-qwen38)
target_include_directories(gufo_qwen38_flash_next PUBLIC ${GUFO_ROOT} ${CMAKE_CURRENT_SOURCE_DIR}/src/native)
target_compile_definitions(gufo_qwen38_flash_next PUBLIC ENGINE_ENABLE_HIP=1 NOMINMAX WIN32_LEAN_AND_MEAN)
if(TARGET gufo_qwen38_flash_next_mmq)
  target_include_directories(gufo_qwen38_flash_next_mmq PUBLIC ${GUFO_ROOT})
  target_compile_definitions(gufo_qwen38_flash_next_mmq PUBLIC ENGINE_ENABLE_HIP=1 NOMINMAX WIN32_LEAN_AND_MEAN)
endif()


add_executable(FlashNextVelocityUnitTests tests/native_unit_tests.cpp)
target_include_directories(FlashNextVelocityUnitTests PRIVATE
  ${GUFO_ROOT}
  ${CMAKE_CURRENT_SOURCE_DIR}/src/native)
target_compile_definitions(FlashNextVelocityUnitTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(FlashNextVelocityUnitTests PRIVATE -O2)
set_target_properties(FlashNextVelocityUnitTests PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

add_executable(FlashNextVelocityEngine src/native/main.cpp)
target_include_directories(FlashNextVelocityEngine PRIVATE ${GUFO_ROOT} ${CMAKE_CURRENT_SOURCE_DIR}/src/native)
target_compile_definitions(FlashNextVelocityEngine PRIVATE ENGINE_ENABLE_HIP=1 NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(FlashNextVelocityEngine PRIVATE -O2)
target_link_libraries(FlashNextVelocityEngine PRIVATE gufo_qwen38_flash_next fnv_core ws2_32 bcrypt)
set_target_properties(FlashNextVelocityEngine PROPERTIES
  OUTPUT_NAME "FlashNextVelocity.Engine"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
