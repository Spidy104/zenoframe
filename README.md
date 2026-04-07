# ZenoFrame

ZenoFrame is a C++23 research prototype for a 1080p, 144 Hz image/DSP transport pipeline. It combines an AVX2/OpenMP CPU DSP engine with a UDP-style frame transport and three experimental bandwidth/recovery phases.

The project is trying to answer one practical question:

Can a CPU-side transport pipeline keep useful 1080p frame continuity at high refresh rates while avoiding full-frame bandwidth spikes?

The current answer is:

- Yes for the DSP kernel under paced release benchmarking at P99.
- Yes for Phase 1 distributed intra-refresh in no-loss 1000-frame runs.
- Yes for Phase 2 temporal row concealment in the regression and continuity tests.
- Yes for Phase 3 compact payload reconstruction over 1000 in-process frames.
- Not yet for hard real-time Phase 3 end-to-end guarantees or 90% compressed-packet-loss recovery.

## Project Phases

| Phase | Name | Goal | Status |
| --- | --- | --- | --- |
| Phase 1 | Distributed intra-refresh | Replace repeated full frames with rolling row-window refreshes. | Implemented in the main sender/receiver path. |
| Phase 2 | Analytic concealment | Heal missing temporal refresh rows using Hilbert-style analytic continuation. | Implemented and integrated into receiver partial-refresh handling. |
| Phase 3 | Compressive sampling | Send a compact sampled payload instead of the full raw frame. | Implemented with tiled sampling and reconstruction. |

Important distinction: Phase 3 currently sends about 8-10% of the raw frame bytes. That does not mean the receiver can lose 90% of the transmitted compressed UDP packets. The current compressed payload is expected to arrive complete.

## Current Release Snapshot

These are the current local release-build numbers documented in `BENCHMARKS.md`:

| Area | Result |
| --- | --- |
| DSP paced 144 Hz run | Mean `3.185 ms`, P99 `6.284 ms`, target `6.944 ms`. |
| Phase 1 DIR | `245,760` bytes per 32-row temporal frame, `2.96%` of raw full frame. |
| Phase 2 concealment | Stale MAE `0.01185084`, concealed MAE `0.00031911`, avg `0.3148 ms`. |
| Phase 3 controlled codec | `656,654` bytes, `7.92%` raw payload, MAE `0.000024`, max_abs `0.000301`. |
| Phase 3 1000-frame main path | `1000 / 1000` frames validated, final input max_abs `0.000012338`. |

See `BENCHMARKS.md` for the full interpretation, including the caveats around burst thermal stress and Phase 3 end-to-end timing.

## Requirements

The project is intentionally tuned to the build machine.

| Requirement | Notes |
| --- | --- |
| CMake | 3.20 or newer. |
| C++ compiler | C++23 support required. |
| CPU features | AVX2 and FMA expected by the current build flags. |
| Threading | OpenMP is required. |
| OS/network | Linux/POSIX-style sockets are needed for UDP loopback mode. |

The build uses `-march=native`, `-mtune=native`, `-mavx2`, `-mfma`, `-ffast-math`, OpenMP, and LTO. If you move the binary to a different CPU, rebuild it there.

## Build

Debug is not a pure `-O0` build. It uses `-O1 -g -fno-omit-frame-pointer` so profiling and iteration are still usable.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

Release is the source of truth for benchmark claims.

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
```

## Test

Run the full release suite:

```bash
ctest --test-dir build-release --output-on-failure
```

If your environment blocks sockets, skip the UDP loopback test:

```bash
ctest --test-dir build-release -E 'UDPLoopback' --output-on-failure
```

Run the convenience target:

```bash
cmake --build build-release --target check
```

See `TESTING.md` for the full validation guide and test map.

## Run The Main Demo

`udp_demo` is the main end-to-end harness. It supports three modes:

| Mode | Meaning |
| --- | --- |
| `--mode=full` | Full raw-frame transport baseline. |
| `--mode=dir` | Phase 1 distributed intra-refresh. |
| `--mode=cs` | Phase 3 compressive sampling. |

It also supports two transport backends:

| Backend | Meaning |
| --- | --- |
| `--transport=inproc` | Fast benchmark path with in-process packet injection. |
| `--transport=udp` | Real loopback UDP sockets. |

Full raw-frame smoke test:

```bash
./build-release/udp_demo --transport=inproc --mode=full --frames=3 --log-every=0
```

Phase 1 DIR 1000-frame run:

```bash
./build-release/udp_demo --transport=inproc --mode=dir --frames=1000 --refresh-rows=32 --timeout-us=2000000 --benchmark-warmup=0 --benchmark-iterations=1 --benchmark-every=0 --progress-every=1000 --log-every=0
```

Phase 3 CS 1000-frame run:

```bash
./build-release/udp_demo --transport=inproc --mode=cs --frames=1000 --timeout-us=2000000 --benchmark-warmup=0 --benchmark-iterations=1 --benchmark-every=0 --progress-every=1000 --log-every=0
```

The in-process transport is the preferred benchmark path. It still exercises the sender/receiver logic, but removes kernel socket overhead so codec and receiver costs are easier to see.

## Run The DSP Benchmark

```bash
./build-release/dsp_benchmark
```

The benchmark reports:

- single-operation Gaussian and Sobel timings
- a paced 144 Hz fused DSP run
- a burst thermal stress run

Use the paced 144 Hz section for normal frame-budget claims. Use the burst stress section as a thermal/variance warning.

## Documentation Map

| File | Purpose |
| --- | --- |
| `README.md` | Project overview, build/run commands, and status. |
| `ARCHITECTURE.md` | Detailed system architecture and data flow. |
| `BENCHMARKS.md` | Current benchmark report and interpretation. |
| `TESTING.md` | Validation commands, test map, and troubleshooting. |
| `ROADMAP.md` | Remaining work and prioritized next steps. |
| `results.md` | Compatibility pointer to the current benchmark report. |
| `LICENSE` | MIT license for the project. |

## Repository Map

| Path | Purpose |
| --- | --- |
| `include/ImageEngine.hpp`, `src/ImageEngine.cpp` | AVX2/OpenMP image operations and fused DSP kernels. |
| `include/TemporalRefresh.hpp` | DIR refresh schedule and row-window metadata helpers. |
| `include/SenderEngine.hpp` | Sender-side packetization for full, DIR, and CS frames. |
| `include/ReceiverEngine.hpp` | Receiver-side assembly, temporal reconstruction, and Phase 2 concealment integration. |
| `include/PhaseConcealment.hpp`, `src/PhaseConcealment.cpp` | Phase 2 analytic/Hilbert concealment module. |
| `include/CompressiveSampling.hpp`, `src/CompressiveSampling.cpp` | Phase 3 sampling, encoding, and reconstruction. |
| `include/CompressiveReceiverEngine.hpp` | Phase 3 compressed payload receiver. |
| `include/Protocol.hpp` | Packet header and protocol contract. |
| `src/udp_demo.cpp` | End-to-end demo and transport benchmark harness. |
| `src/main.cpp` | DSP benchmark suite. |
| `tests/` | Automated and benchmark-style tests. |

## What Is Done

- Phase 1 DIR is implemented in the main path.
- Phase 2 concealment is implemented in the main receiver.
- Phase 3 compressed payload encode/decode is implemented in the sender/receiver/demo path.
- The main demo supports full, DIR, and CS modes.
- The benchmark suite reports paced and burst behavior separately.
- The CTest suite covers the core DSP, protocol, transport, temporal, concealment, and CS paths.

## What Is Not Claimed Yet

- Hard real-time guarantees under all OS scheduling and thermal conditions.
- Recovery when only 10% of compressed UDP packets arrive.
- Dataset-wide image/video quality validation on natural content.
- Portable performance across CPUs without rebuilding.
- A production network protocol with congestion control, retransmission, or forward-error correction.

## License

This project is licensed under the MIT License. See `LICENSE`.

## Suggested Reading Order

1. Read this README for the project shape.
2. Read `BENCHMARKS.md` to understand the current numbers.
3. Read `ARCHITECTURE.md` to understand how the code paths fit together.
4. Read `TESTING.md` before changing behavior.
5. Read `ROADMAP.md` before making claims about what is finished.
