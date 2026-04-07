# Testing Guide

This document explains how to validate the project and how to interpret the test targets.

## Quick Validation

Build release:

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
```

Run all CTest tests:

```bash
ctest --test-dir build-release --output-on-failure
```

If the environment blocks sockets, skip the UDP loopback test:

```bash
ctest --test-dir build-release -E 'UDPLoopback' --output-on-failure
```

Run the project convenience target:

```bash
cmake --build build-release --target check
```

## Test Categories

| Category | Tests |
| --- | --- |
| DSP correctness | `ImageBasicOperations`, `ConvolutionAlgorithms`, `DSPRegression` |
| DSP performance/regression | `PerformanceBenchmarks`, `ThreadScaling` |
| Temporal refresh | `TemporalRefresh`, `TemporalReceiver`, `TemporalBitrate`, `TemporalDSPContinuity` |
| Transport/protocol | `ProtocolContract`, `SPSCQueue`, `SenderEngine`, `ReceiverEngine`, `TransportStress`, `TransportRandomized`, `UDPLoopback` |
| Phase 2 | `Phase2Concealment`, `TemporalDSPContinuity` |
| Phase 3 | `Phase3Compressive` |

## Full Test Map

| Test | What it covers |
| --- | --- |
| `ImageBasicOperations` | Image allocation, pixel access, fill behavior, and basic invariants. |
| `ConvolutionAlgorithms` | Gaussian blur, Sobel edge detection, and convolution correctness. |
| `PerformanceBenchmarks` | Baseline multi-resolution performance checks. |
| `ThreadScaling` | OpenMP/thread behavior and scaling sanity checks. |
| `DSPRegression` | DSP equivalence and regression checks for fused and separate paths. |
| `TemporalRefresh` | Distributed intra-refresh scheduling and row-window behavior. |
| `TemporalReceiver` | Temporal receiver reconstruction and invalid metadata handling. |
| `TemporalBitrate` | DIR bitrate comparison against full-frame refresh. |
| `TemporalDSPContinuity` | Main receiver continuity when temporal refresh rows are missing. |
| `ProtocolContract` | Packet layout, flags, and binary protocol expectations. |
| `SPSCQueue` | Single-producer/single-consumer queue lifecycle. |
| `SenderEngine` | Sender-side integration without real sockets. |
| `TransportStress` | Multi-frame transport stress conditions. |
| `TransportRandomized` | Seeded randomized transport properties. |
| `ReceiverEngine` | Main receiver assembly and ready-frame behavior. |
| `Phase2Concealment` | Analytic concealment quality and timing. |
| `Phase3Compressive` | CS payload encode/decode and sender/receiver round trip. |
| `UDPLoopback` | Real UDP loopback transport. |

## Manual Validation Targets

Some executables are intentionally built but not treated as routine automated pass/fail tests.

| Target | Purpose |
| --- | --- |
| `dsp_benchmark` | Manual benchmark suite with paced and burst timing. |
| `udp_demo` | End-to-end full/DIR/CS transport demo. |
| `test_phase6` | Legacy/manual fused pipeline benchmark helper. |
| `test_avx2` | Manual AVX2 benchmark helper. |

Build them with:

```bash
cmake --build build-release --target manual_validation
```

## Recommended Smoke Runs

Run the DSP benchmark:

```bash
./build-release/dsp_benchmark
```

Run full-frame transport:

```bash
./build-release/udp_demo --transport=inproc --mode=full --frames=3 --log-every=0
```

Run Phase 1 DIR:

```bash
./build-release/udp_demo --transport=inproc --mode=dir --frames=1000 --refresh-rows=32 --timeout-us=2000000 --benchmark-warmup=0 --benchmark-iterations=1 --benchmark-every=0 --progress-every=1000 --log-every=0
```

Run Phase 3 CS:

```bash
./build-release/udp_demo --transport=inproc --mode=cs --frames=1000 --timeout-us=2000000 --benchmark-warmup=0 --benchmark-iterations=1 --benchmark-every=0 --progress-every=1000 --log-every=0
```

Run real UDP loopback:

```bash
./build-release/udp_demo --transport=udp --mode=full --frames=3 --log-every=0
```

## Troubleshooting

If `UDPLoopback` fails in a sandbox:

- Run `ctest --test-dir build-release -E 'UDPLoopback' --output-on-failure`.
- Run the loopback test again in an environment with socket access.
- Prefer `--transport=inproc` for codec and receiver benchmarking inside restricted environments.

If benchmark numbers vary:

- Prefer Release builds for reported numbers.
- Use the paced 144 Hz benchmark for normal DSP claims.
- Treat the burst stress benchmark as a thermal and scheduling stress test.
- Close heavy background processes before comparing runs.
- Remember that `-march=native` makes the numbers machine-specific.

If Phase 3 timing looks worse than expected:

- Check `cs_encode_ms` in the `udp_demo` summary.
- Check receiver ingest stage time.
- Compare against `test_phase3_compressive` to separate codec cost from demo/transport cost.
- Avoid running debug and release benchmarks concurrently.

