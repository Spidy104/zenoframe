# ZenoFrame

ZenoFrame is a C++23 image transport prototype that processes 1080p frames in about `3.2 ms` on the paced 144 Hz DSP benchmark, with a P99 of `6.284 ms` against a `6.944 ms` frame budget.

Under the hood it is a systems project as much as a DSP one: AVX2/FMA kernels, OpenMP tile/row parallelism, lock-free SPSC ready queues, reusable frame-buffer pools, and a UDP-style sender/receiver path for full frames, temporal refresh frames, and compressed sampled payloads.

The idea started with a simple question: if we are moving 1080p frames at 144 Hz, can we avoid repeatedly blasting full frames over the wire and still keep the image useful when data goes missing?

ZenoFrame explores that with three pieces working together:

1. Distributed intra-refresh, so frames can be refreshed in small rolling row windows instead of full-frame spikes.
2. Analytic concealment using a short 1D Hilbert FIR approximation, so missing temporal rows can be healed instead of simply freezing stale data.
3. Compressive sampling, so the sender can transmit a compact sampled payload and reconstruct the frame on the receiver side.

It has a real sender/receiver path, tests, benchmark docs, and enough caveats in the right places so the numbers do not pretend to be more than they are.

## What Works Today

The main pieces are implemented and wired into the demo path:

| Area | Current state |
| --- | --- |
| Phase 1 DIR | Implemented in the main sender/receiver path. |
| Phase 2 concealment | Implemented in the receiver for incomplete temporal refreshes. |
| Phase 3 compressive sampling | Implemented with tiled sampling and reconstruction. |
| Systems path | Lock-free SPSC ready queues and reusable frame-buffer pools are used in the receiver path. |
| Demos | `full`, `dir`, and `cs` modes run through `udp_demo`. |
| Tests | CTest covers DSP, protocol, transport, temporal refresh, concealment, and compressive sampling. |

The important caveat: Phase 3 sends about 8-10% of the raw frame bytes. That is data reduction. It does not mean the receiver can lose 90% of the compressed UDP packets and still reconstruct everything. Partial compressed-payload recovery is future work.

## Latest Snapshot

These are local release-build numbers from the current benchmark report:

| Area | Result |
| --- | --- |
| DSP paced 144 Hz run | Mean `3.185 ms`, P99 `6.284 ms`, target `6.944 ms`. |
| Phase 1 DIR | `245,760` bytes per 32-row temporal frame, `2.96%` of a raw full frame. |
| Phase 2 concealment | Stale MAE `0.01185084`, concealed MAE `0.00031911`, average `0.3148 ms`. |
| Phase 3 controlled codec | `656,654` bytes, `7.92%` raw payload, MAE `0.000024`, max_abs `0.000301`. |
| Phase 3 1000-frame main path | `1000 / 1000` frames validated, final input max_abs `0.000012338`. |

The benchmark story has nuance. The DSP kernel passes the 144 Hz budget at P99 in a paced release run, but the burst stress test still shows thermal and scheduling variance. Phase 3 is accurate over the 1000-frame in-process demo, but the full CS path is not yet a hard 144 Hz end-to-end guarantee.

If you are evaluating the project, `BENCHMARKS.md` is the best place to start.

## Build

The project is tuned for the build machine. It currently expects AVX2, FMA, OpenMP, and a C++23 compiler.

Debug builds are kept profiling-friendly with `-O1 -g -fno-omit-frame-pointer`:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

Release builds are the ones to use for benchmark claims:

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
```

The CMake configuration uses `-march=native`, so rebuild on the machine where you want to run the benchmarks.

## Run Tests

Run the full release test suite:

```bash
ctest --test-dir build-release --output-on-failure
```

If you are in a sandbox that blocks sockets, skip the UDP loopback test:

```bash
ctest --test-dir build-release -E 'UDPLoopback' --output-on-failure
```

There is also a convenience target:

```bash
cmake --build build-release --target check
```

For the test map and troubleshooting notes, see `TESTING.md`.

## Try The Demo

The main demo is `udp_demo`. It has three modes:

| Mode | What it does |
| --- | --- |
| `--mode=full` | Sends full raw frames. Useful as the baseline. |
| `--mode=dir` | Sends a full seed frame, then rolling row-window refreshes. |
| `--mode=cs` | Sends the Phase 3 compressed sampled payload. |

It also has two transport backends:

| Backend | What it does |
| --- | --- |
| `--transport=inproc` | Injects packets/payloads in process. Best for measuring codec and receiver behavior. |
| `--transport=udp` | Uses real loopback UDP sockets. Best for socket/packetization checks. |

Full-frame smoke test:

```bash
./build-release/udp_demo --transport=inproc --mode=full --frames=3 --log-every=0
```

Phase 1 distributed intra-refresh:

```bash
./build-release/udp_demo --transport=inproc --mode=dir --frames=1000 --refresh-rows=32 --timeout-us=2000000 --benchmark-warmup=0 --benchmark-iterations=1 --benchmark-every=0 --progress-every=1000 --log-every=0
```

Phase 3 compressive sampling:

```bash
./build-release/udp_demo --transport=inproc --mode=cs --frames=1000 --timeout-us=2000000 --benchmark-warmup=0 --benchmark-iterations=1 --benchmark-every=0 --progress-every=1000 --log-every=0
```

`--transport=inproc` is the benchmark-friendly path. It still exercises the sender/receiver code, but it avoids kernel UDP overhead so the codec and receiver costs are easier to see.

## Run The DSP Benchmark

```bash
./build-release/dsp_benchmark
```

This prints three views of the DSP path:

- single-operation Gaussian and Sobel timings
- a paced 144 Hz fused DSP run
- a burst thermal stress run

Use the paced section for normal frame-budget discussion. Use the burst stress section as a warning about thermal and scheduler behavior.

## Repo Guide

| Path | What to look for |
| --- | --- |
| `src/udp_demo.cpp` | End-to-end full/DIR/CS demo flow. |
| `src/CompressiveSampling.cpp` | Phase 3 sampling and reconstruction. |
| `src/PhaseConcealment.cpp` | Phase 2 analytic row healing. |
| `src/ImageEngine.cpp` | AVX2/OpenMP image kernels. |
| `include/SenderEngine.hpp` | Sender-side packetization. |
| `include/ReceiverEngine.hpp` | Main full/DIR receiver path. |
| `include/CompressiveReceiverEngine.hpp` | Phase 3 compressed receiver path. |
| `include/FrameBufferPool.hpp` | Reusable frame slots and ready-frame publication. |
| `include/SPSCQueue.hpp` | Lock-free SPSC queue used for ready descriptors. |
| `include/Protocol.hpp` | Packet header and flags. |
| `tests/` | Regression, protocol, transport, temporal, Phase 2, and Phase 3 coverage. |

## Docs

| File | Why it exists |
| --- | --- |
| `ARCHITECTURE.md` | A deeper walkthrough of the data flow and phase design. |
| `BENCHMARKS.md` | Current benchmark numbers and how to interpret them. |
| `TESTING.md` | Test commands, test map, and troubleshooting notes. |
| `ROADMAP.md` | What is done, what is next, and what should not be claimed yet. |
| `results.md` | Compatibility pointer for older references to benchmark results. |

## What I Would Not Claim Yet

This part matters. ZenoFrame is promising, but it is not magic:

- It is not trying to be a production video codec yet.
- It does not guarantee hard real-time behavior under arbitrary OS load or thermal conditions.
- It does not yet recover when only 10% of compressed UDP packets arrive.
- It has not been validated across a broad natural-image/video dataset.
- It is not a portable binary distribution, because the build is intentionally native-tuned.
- It is not a production network protocol with congestion control, retransmission, or FEC.

## License

ZenoFrame is MIT licensed. See `LICENSE`.
