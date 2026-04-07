cmake_minimum_required(VERSION 3.20)
project(HighSpeedDSP VERSION 1.0 LANGUAGES CXX)

# ============================================================================
# PHASE 1: THE TUNER - Aggressive Optimization Configuration
# ============================================================================
# Goal: Extract every cycle of performance from the CPU
# Target: 144 FPS @ 1080p = 6.9ms per frame for 2,073,600 pixels

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile commands for clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ============================================================================
# OPTIMIZATION FLAGS: The "Turbo Buttons"
# ============================================================================

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Base optimization flags
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
# Keep Debug usable for profiling and iteration instead of effectively O0.
set(CMAKE_CXX_FLAGS_DEBUG "-O1 -g -fno-omit-frame-pointer")

# Architecture-specific optimizations
# -march=native: Use ALL instructions available on THIS CPU (AVX2, FMA, etc.)
# -mtune=native: Optimize instruction scheduling for THIS CPU
set(ARCH_FLAGS "-march=native -mtune=native")

# SIMD/Vectorization flags
# -mavx2: Enable 256-bit vector operations (process 8 floats at once)
# -mfma: Enable Fused Multiply-Add (a*b+c in one instruction)
set(SIMD_FLAGS "-mavx2 -mfma")

# Fast math optimizations
# -ffast-math: Relax IEEE 754 compliance for speed
# -fno-math-errno: Don't set errno for math functions
# -ffinite-math-only: Assume no NaN/Inf values
set(FAST_MATH_FLAGS "-ffast-math -fno-math-errno -ffinite-math-only")

# Loop optimization hints
# -funroll-loops: Unroll loops to reduce branch overhead
# -ftree-vectorize: Auto-vectorize loops (enabled by -O3, but explicit)
set(LOOP_FLAGS "-funroll-loops -ftree-vectorize")

# Inlining aggressiveness
# -finline-functions: Inline functions even if not marked inline
# -finline-limit=1000: Allow larger functions to be inlined
set(INLINE_FLAGS "-finline-functions")

# Link Time Optimization (LTO)
# Allows optimization across compilation units
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)

# Combine all optimization flags
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${ARCH_FLAGS} ${SIMD_FLAGS} ${FAST_MATH_FLAGS} ${LOOP_FLAGS} ${INLINE_FLAGS}")

# ============================================================================
# MEMORY ALIGNMENT FLAGS
# ============================================================================
# Ensure data structures align to cache line boundaries (64 bytes)
add_compile_definitions(CACHE_LINE_SIZE=64)

# ============================================================================
# OPENMP SUPPORT - Multithreading for Parallel Processing
# ============================================================================
# OpenMP allows us to parallelize the outer loop of convolution
# Each thread processes different rows simultaneously

find_package(OpenMP REQUIRED)

if(OpenMP_CXX_FOUND)
    message(STATUS "OpenMP found: ${OpenMP_CXX_VERSION}")
    message(STATUS "OpenMP flags: ${OpenMP_CXX_FLAGS}")
else()
    message(FATAL_ERROR "OpenMP not found. Please install OpenMP support for your compiler.")
endif()

# ============================================================================
# SOURCE FILES
# ============================================================================

add_library(ImageEngine
    src/CompressiveSampling.cpp
    src/ImageEngine.cpp
    src/PhaseConcealment.cpp
)

target_include_directories(ImageEngine PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Link OpenMP to the library
target_link_libraries(ImageEngine PUBLIC OpenMP::OpenMP_CXX)

# Phase 8: Elite Fused Pipeline Validator (main executable)
add_executable(dsp_benchmark src/main.cpp)
target_link_libraries(dsp_benchmark PRIVATE ImageEngine)

# UDP transport demo
add_executable(udp_demo src/udp_demo.cpp)
target_link_libraries(udp_demo PRIVATE ImageEngine)

# ============================================================================
# TESTING FRAMEWORK
# ============================================================================

# Enable CTest
enable_testing()

# Test 1: Basic Image Operations
add_executable(test_image_basic
    tests/test_image_basic.cpp
)
target_link_libraries(test_image_basic PRIVATE ImageEngine)
add_test(NAME ImageBasicOperations COMMAND test_image_basic)

# Test 2: Convolution Algorithms
add_executable(test_convolution
    tests/test_convolution.cpp
)
target_link_libraries(test_convolution PRIVATE ImageEngine)
add_test(NAME ConvolutionAlgorithms COMMAND test_convolution)

# Test 3: Performance Benchmarks
add_executable(test_performance
    tests/test_performance.cpp
)
target_link_libraries(test_performance PRIVATE ImageEngine)
add_test(NAME PerformanceBenchmarks COMMAND test_performance)

# Test 4: Thread Scaling & SIMD Verification
add_executable(test_threading
    tests/test_threading.cpp
)
target_link_libraries(test_threading PRIVATE ImageEngine)
add_test(NAME ThreadScaling COMMAND test_threading)

# Test 5: Phase 6 Optimizations (Separable & Fused)
add_executable(test_phase6
    tests/test_phase6.cpp
)
target_link_libraries(test_phase6 PRIVATE ImageEngine)
# Not added to CTest - run manually for benchmarking

# Test 6: Phase 7 AVX2 Benchmarks
add_executable(test_avx2
    tests/test_avx2.cpp
)
target_link_libraries(test_avx2 PRIVATE ImageEngine)

# Test 7: DSP regression and equivalence coverage
add_executable(test_dsp_regression
    tests/test_dsp_regression.cpp
)
target_link_libraries(test_dsp_regression PRIVATE ImageEngine)
add_test(NAME DSPRegression COMMAND test_dsp_regression)

# Test 8: Distributed intra-refresh schedule and reconstruction
add_executable(test_temporal_refresh
    tests/test_temporal_refresh.cpp
)
target_link_libraries(test_temporal_refresh PRIVATE ImageEngine)
add_test(NAME TemporalRefresh COMMAND test_temporal_refresh)

# Test 9: Temporal refresh transport receiver round-trip
add_executable(test_temporal_receiver
    tests/test_temporal_receiver.cpp
)
target_link_libraries(test_temporal_receiver PRIVATE ImageEngine)
add_test(NAME TemporalReceiver COMMAND test_temporal_receiver)

# Test 10: Temporal refresh bitrate spike comparison
add_executable(test_temporal_bitrate
    tests/test_temporal_bitrate.cpp
)
target_link_libraries(test_temporal_bitrate PRIVATE ImageEngine)
add_test(NAME TemporalBitrate COMMAND test_temporal_bitrate)

# Test 11: Temporal stream DSP continuity
add_executable(test_temporal_dsp_continuity
    tests/test_temporal_dsp_continuity.cpp
)
target_link_libraries(test_temporal_dsp_continuity PRIVATE ImageEngine)
add_test(NAME TemporalDSPContinuity COMMAND test_temporal_dsp_continuity)

# Test 12: Protocol binary contract
add_executable(test_protocol
    tests/test_protocol.cpp
)
target_link_libraries(test_protocol PRIVATE ImageEngine)
add_test(NAME ProtocolContract COMMAND test_protocol)

# Test 13: Lock-free queue and zero-copy slot lifecycle
add_executable(test_spsc_queue
    tests/test_spsc_queue.cpp
)
target_link_libraries(test_spsc_queue PRIVATE ImageEngine)
add_test(NAME SPSCQueue COMMAND test_spsc_queue)

# Test 14: Sender/receiver integration without sockets
add_executable(test_sender_engine
    tests/test_sender_engine.cpp
)
target_link_libraries(test_sender_engine PRIVATE ImageEngine)
add_test(NAME SenderEngine COMMAND test_sender_engine)

# Test 15: Multi-frame transport stress scenarios
add_executable(test_transport_stress
    tests/test_transport_stress.cpp
)
target_link_libraries(test_transport_stress PRIVATE ImageEngine)
add_test(NAME TransportStress COMMAND test_transport_stress)

# Test 16: Seeded randomized transport properties
add_executable(test_transport_randomized
    tests/test_transport_randomized.cpp
)
target_link_libraries(test_transport_randomized PRIVATE ImageEngine)
add_test(NAME TransportRandomized COMMAND test_transport_randomized)

# Test 17: Receiver assembly pipeline
add_executable(test_receiver_engine
    tests/test_receiver_engine.cpp
)
target_link_libraries(test_receiver_engine PRIVATE ImageEngine)
add_test(NAME ReceiverEngine COMMAND test_receiver_engine)

add_executable(test_phase2_concealment
    tests/test_phase2_concealment.cpp
)
target_link_libraries(test_phase2_concealment PRIVATE ImageEngine)
add_test(NAME Phase2Concealment COMMAND test_phase2_concealment)

add_executable(test_phase3_compressive
    tests/test_phase3_compressive.cpp
)
target_link_libraries(test_phase3_compressive PRIVATE ImageEngine)
add_test(NAME Phase3Compressive COMMAND test_phase3_compressive)

# Test 18: UDP loopback transport
add_executable(test_udp_loopback
    tests/test_udp_loopback.cpp
)
target_link_libraries(test_udp_loopback PRIVATE ImageEngine)
add_test(NAME UDPLoopback COMMAND test_udp_loopback)

set_tests_properties(
    ImageBasicOperations
    ConvolutionAlgorithms
    PerformanceBenchmarks
    ThreadScaling
    DSPRegression
    PROPERTIES LABELS "automated;dsp"
)

set_tests_properties(
    TemporalRefresh
    TemporalReceiver
    TemporalBitrate
    TemporalDSPContinuity
    PROPERTIES LABELS "automated;temporal;transport"
)

set_tests_properties(
    ProtocolContract
    SPSCQueue
    SenderEngine
    TransportStress
    TransportRandomized
    ReceiverEngine
    Phase2Concealment
    Phase3Compressive
    PROPERTIES LABELS "automated;transport"
)

set_tests_properties(
    UDPLoopback
    PROPERTIES LABELS "automated;transport;network"
)

add_custom_target(manual_validation
    DEPENDS dsp_benchmark udp_demo test_phase6 test_avx2
)

add_custom_target(check
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
    DEPENDS
        test_image_basic
        test_convolution
        test_performance
        test_threading
        test_dsp_regression
        test_temporal_refresh
        test_temporal_receiver
        test_temporal_bitrate
        test_temporal_dsp_continuity
        test_protocol
        test_spsc_queue
        test_sender_engine
        test_transport_stress
        test_transport_randomized
        test_receiver_engine
        test_phase2_concealment
        test_phase3_compressive
        test_udp_loopback
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    USES_TERMINAL
)

# ============================================================================
# DIAGNOSTIC OUTPUT
# ============================================================================

message(STATUS "==========================================================")
message(STATUS "High-Speed DSP Build Configuration")
message(STATUS "==========================================================")
message(STATUS "Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ Standard: C++${CMAKE_CXX_STANDARD}")
message(STATUS "Compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "Optimization Flags: ${CMAKE_CXX_FLAGS}")
message(STATUS "LTO Enabled: ${CMAKE_INTERPROCEDURAL_OPTIMIZATION}")
message(STATUS "Automated validation target: check")
message(STATUS "Manual validation target: manual_validation")
message(STATUS "==========================================================")
