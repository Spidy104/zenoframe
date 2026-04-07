# Architecture Notes

This document explains how the project is structured, how frames move through the system, and how the three research phases fit into the main sender/receiver path.

The short version:

1. `ImageEngine` owns the DSP kernels.
2. `SenderEngine` packetizes frames.
3. `ReceiverEngine` reassembles raw and temporal frames.
4. `PhaseConcealment` heals incomplete temporal refreshes.
5. `CompressiveSampling` encodes and reconstructs Phase 3 compressed frames.
6. `udp_demo` is the end-to-end harness for full, DIR, and CS modes.
7. `dsp_benchmark` is the DSP-only benchmark harness.

## Goals

The project is a CPU-first C++23 prototype for moving and processing 1080p image frames near a 144 Hz target.

The research story is split into three phases:

| Phase | Goal | Current Status |
| --- | --- | --- |
| Phase 1 | Avoid full-frame I-frame bandwidth spikes with distributed intra-refresh. | Implemented in the main sender/receiver path. |
| Phase 2 | Use a short 1D Hilbert FIR approximation plus analytic-domain interpolation to conceal missing temporal rows. | Implemented and integrated into receiver partial-refresh handling. |
| Phase 3 | Send a compact sample payload instead of a raw full frame. | Implemented with tiled sampling and reconstruction. |

The current system is a strong prototype. It is not yet a hard real-time product, a lossy-network protocol with FEC, or a dataset-backed video codec.

## Fixed Frame Model

Most of the current implementation is intentionally fixed around the shadow frame configuration:

| Property | Value |
| --- | ---: |
| Width | 1920 |
| Height | 1080 |
| Pixels | 2,073,600 |
| Pixel type | 32-bit float |
| Raw frame bytes | 8,294,400 |
| Target frame rate | 144 Hz |
| Target frame budget | 6.944 ms |

This fixed-size model keeps the prototype focused on transport, DSP continuity, and compression behavior rather than general-purpose image container handling.

The important consequence is that several constants, tile choices, and packet budgets are optimized around 1080p. If arbitrary resolutions become a requirement, the tile layout, validation tests, and benchmark assumptions should be revisited together.

## Build And Runtime Assumptions

The project is tuned for the local CPU:

| Setting | Purpose |
| --- | --- |
| `-march=native` | Emit instructions for the build machine. |
| `-mtune=native` | Tune instruction scheduling for the build machine. |
| `-mavx2` | Enable 256-bit SIMD float/vector work. |
| `-mfma` | Enable fused multiply-add instructions. |
| `-ffast-math` | Relax floating-point rules for speed. |
| OpenMP | Parallelize row/tile work. |
| LTO | Allow cross-translation-unit optimization. |

Release builds are the benchmark source of truth. Debug builds are still optimized with `-O1 -g -fno-omit-frame-pointer` so profiling and iteration are not dominated by avoidable `-O0` overhead.

## Main Source Map

| File | Role |
| --- | --- |
| `include/ImageEngine.hpp` | Public image and DSP API. |
| `src/ImageEngine.cpp` | AVX2/OpenMP Gaussian, Sobel, and fused DSP implementation. |
| `include/Protocol.hpp` | Packet header, flags, and protocol helpers. |
| `include/FrameBufferPool.hpp` | Reusable frame slots and ready-frame publication. |
| `include/SPSCQueue.hpp` | Lock-free single-producer/single-consumer queue for ready descriptors. |
| `include/TemporalRefresh.hpp` | DIR scheduler and temporal reference reconstructor. |
| `include/SenderEngine.hpp` | Packetization for full, temporal, DIR, and CS frames. |
| `include/ReceiverEngine.hpp` | Fragment assembly for full/DIR frames plus temporal reconstruction and concealment. |
| `include/PhaseConcealment.hpp` | Phase 2 public concealment data structures and API. |
| `src/PhaseConcealment.cpp` | Phase 2 short Hilbert FIR approximation and analytic-domain continuation implementation. |
| `include/CompressiveSampling.hpp` | Phase 3 payload structs and encode/decode API. |
| `src/CompressiveSampling.cpp` | Phase 3 sampling, payload writing, affine reconstruction, and OMP fallback. |
| `include/CompressiveReceiverEngine.hpp` | Receiver for complete or fragmented Phase 3 compressed payloads. |
| `src/udp_demo.cpp` | End-to-end benchmark/demo driver. |
| `src/main.cpp` | DSP-only benchmark driver. |

## Packet And Frame Lifecycle

The high-level lifecycle is:

1. A source frame is generated or provided as contiguous float pixels.
2. `SenderEngine` turns the frame or compressed payload into protocol fragments.
3. Fragments are delivered by either UDP loopback or in-process injection.
4. A receiver assembles fragments into a complete frame or payload.
5. The receiver publishes a `ReadyFrame` descriptor through a queue.
6. The demo or test acquires the ready frame, validates it, then releases the slot.

The receiver-side storage is designed around reusable slots. The goal is to avoid repeatedly allocating and freeing full 1080p buffers on the hot path.

## Protocol Layer

`Protocol.hpp` defines the packet header and the flags used by the sender and receivers.

The important protocol concepts are:

| Concept | Purpose |
| --- | --- |
| Sequence number | Identifies the frame or payload sequence. |
| Frame ID | Tracks a logical frame identifier. |
| Fragment index/count | Allows receivers to assemble a payload from many packets. |
| Payload byte count | Allows the final fragment to be smaller than the normal fragment size. |
| Temporal refresh flag | Marks Phase 1/DIR temporal row-window packets. |
| Compressive sampling flag | Marks Phase 3 compressed payload packets. |
| Refresh row metadata | Tells the receiver which row window a temporal payload updates. |

Full and temporal packets are handled by `ReceiverEngine`. Compressive sampling packets are handled by `CompressiveReceiverEngine`.

## Buffering And Ready Frames

`FrameBufferPool` owns the reusable storage for reconstructed or assembled frames.

The receiver path uses two ideas:

1. Assembly slots track in-progress packet fragments.
2. Ready slots hold completed images that downstream code can inspect.

When a frame is complete:

1. The receiver obtains a ready slot.
2. The final reconstructed pixels are written or published into that slot.
3. A descriptor is pushed to the ready queue.
4. The consumer calls `tryAcquireReadyFrame`.
5. The consumer later calls `releaseReadyFrame`.

This keeps frame ownership explicit and avoids accidental lifetime problems when benchmarking or validating large images.

## DSP Engine

`ImageEngine` provides the image processing operations used by the benchmark and validation paths:

| Function family | Purpose |
| --- | --- |
| `gaussianBlur` / `gaussianBlurInto` | Blur an input image. |
| `sobelMagnitude` / `sobelMagnitudeInto` | Compute edge magnitude. |
| `blurAndSobelFused` / `blurAndSobelFusedInto` | Run the fused blur plus Sobel-style pipeline. |

The `Into` APIs reuse caller-owned output buffers. The benchmark uses these APIs so the reported timings are closer to the actual fixed-frame compute path and not dominated by allocation.

`dsp_benchmark` reports three useful views:

| Benchmark section | Meaning |
| --- | --- |
| Single operation benchmarks | Measures Gaussian and Sobel separately. |
| Paced 144 Hz run | Sleeps to a 6.944 ms cadence and measures fused compute time. |
| Burst thermal stress | Runs continuously to expose thermal and scheduling variance. |

The paced run is the best number for normal DSP frame-budget claims. The burst stress run is deliberately harsher and should be interpreted as a thermal/variance warning.

## Full Frame Mode

Full mode is the baseline.

Flow:

1. `SenderEngine` packetizes the entire raw 8,294,400 byte frame.
2. `ReceiverEngine` assembles all fragments.
3. The completed raw frame is published as a ready frame.
4. Validation compares the received pixels with the expected source frame.

Full mode is useful because it proves the basic protocol, packet assembly, ready queue, and DSP validation path before the experimental phases are added.

For 1080p float frames, a full frame is currently split into 7,078 datagrams in the demo settings.

## Phase 1: Distributed Intra-Refresh

Phase 1 replaces repeated full-frame updates with a seed frame plus small row-window refreshes.

The problem it targets:

- Sending a full frame every time creates large periodic bitrate spikes.
- At 1080p float data, a raw full frame is 8,294,400 bytes.
- At high frame rates, that can be too bursty for a simple UDP transport.

The DIR flow:

1. Send a full seed frame.
2. Keep a reference frame on the receiver.
3. For each temporal frame, send only `N` refreshed rows.
4. The receiver writes those rows into the reference frame.
5. The updated reference frame is published as the reconstructed output.

With `--refresh-rows=32`, the current release demo reports:

| Metric | Value |
| --- | ---: |
| Full seed frame | 8,294,400 bytes |
| Full seed datagrams | 7,078 |
| DIR temporal payload | 245,760 bytes |
| DIR temporal datagrams | 210 |
| DIR temporal payload ratio | 2.96% |

The temporal row window may wrap around the bottom of the frame. That is intentional and is covered by the temporal receiver logic and tests.

The main code pieces are:

| Component | Role |
| --- | --- |
| `DistributedIntraRefreshScheduler` | Chooses the refresh span for a frame sequence. |
| `TemporalRefreshReconstructor` | Maintains and updates the receiver reference frame. |
| `SenderEngine::sendDistributedIntraRefreshFrame` | Sends a frame as a DIR refresh packet stream. |
| `ReceiverEngine::onTemporalPacket` | Receives temporal fragments and reconstructs the reference. |

## Phase 2: Analytic Concealment

Phase 2 handles the case where a temporal refresh window is incomplete.

The baseline behavior would be to keep stale rows or drop the partial update. Phase 2 adds an optional force-commit path that applies the rows that did arrive and heals the missing row spans.

The algorithm:

1. Identify missing row spans inside the temporal refresh window.
2. Find valid boundary rows above and below the missing span.
3. Compute a 1D Hilbert FIR approximation for boundary rows using odd taps at offsets `1`, `3`, and `5` with coefficients `2 / (pi * k)`.
4. Treat each row as an analytic signal with real and imaginary components.
5. Interpolate in the analytic/complex domain.
6. Blend multiple neighboring rows to improve stability.
7. Write healed rows back into the temporal reference frame.

The current implementation avoids per-call scratch allocation by keeping a `ConcealmentWorkspace` owned by the receiver.

The main code pieces are:

| Component | Role |
| --- | --- |
| `ConcealmentSpan` | Describes a missing row range. |
| `ConcealmentWorkspace` | Reusable scratch buffers for masks and Hilbert rows. |
| `concealMissingRowsWithAnalyticContinuation` | Performs the row healing pass. |
| `ReceiverEngine::applyAvailableTemporalRows` | Applies received rows and records missing spans. |
| `ReceiverEngine::pollExpiredFrames` | Decides whether to drop or force-commit partial temporal frames. |

The current policy split is:

| Policy | Behavior |
| --- | --- |
| `DropPartial` | Drop incomplete temporal frames to protect the reference. |
| `ForceCommitPartial` | Apply available rows, heal missing rows, then publish the reconstructed reference. |

Measured release result from `test_phase2_concealment`:

| Metric | Value |
| --- | ---: |
| Stale-hold MAE | 0.01185084 |
| Concealed MAE | 0.00031911 |
| 32-row concealment average | 0.3148 ms |

Interpretation: Phase 2 is fast enough for the frame budget and materially better than stale hold in the regression scene.

## Phase 3: Compressive Sampling

Phase 3 sends a compact sampled representation instead of a raw frame.

The current implementation is designed for smooth/synthetic frame content and fast CPU reconstruction. It is a compressive data-reduction path, not a packet-loss recovery path.

The encode flow:

1. Divide the frame into fixed-size tiles.
2. Choose samples per tile using an exact-budget, activity-aware sampler.
3. Quantize sampled float values.
4. Write all tile headers and samples into one contiguous payload buffer.
5. Packetize that compressed payload through `SenderEngine`.

The decode flow:

1. Parse the compressed payload header.
2. Parse each tile payload.
3. Try the fast affine-plane reconstruction path for smooth tiles.
4. Fall back to the tiled OMP-style reconstruction path for harder tiles.
5. Clamp reconstructed tiles to the observed sample range.
6. Publish the reconstructed full frame through `CompressiveReceiverEngine`.

Current Phase 3 constants:

| Setting | Value |
| --- | ---: |
| Tile width | 24 |
| Tile height | 10 |
| Target sample ratio | 10.00% |
| Minimum samples per tile | 6 |
| Dictionary atoms | 24 |
| OMP atoms | 8 |

The main code pieces are:

| Component | Role |
| --- | --- |
| `CompressiveSamplingConfig` | Controls sample ratio, atom counts, and sample floor. |
| `CompressivePayloadHeader` | Describes the whole compressed frame payload. |
| `CompressiveTileHeader` | Describes one compressed tile. |
| `CompressiveTileSample` | Stores a local sample index and quantized value. |
| `encodeCompressiveFramePayload` | Builds the compressed payload. |
| `reconstructCompressiveFramePayload` | Reconstructs a full frame from a compressed payload. |
| `CompressiveReceiverEngine::onPayload` | Fast in-process ingest path for a complete compressed payload. |

Current release controlled-test numbers:

| Metric | Value |
| --- | ---: |
| Payload bytes | 656,654 |
| Payload ratio | 7.92% of raw frame bytes |
| Payload reconstruction MAE | 0.000024 |
| Payload reconstruction max_abs | 0.000301 |
| Encode time | 5.023 ms |
| Reconstruct time | 1.911 ms |

Current release 1000-frame in-process demo numbers:

| Metric | Value |
| --- | ---: |
| Validated frames | 1000 / 1000 |
| Payload bytes per frame | 656,654 |
| Final input mean_abs | 0.000001541 |
| Final input max_abs | 0.000012338 |
| Final fused DSP mean_abs | 0.000002901 |
| Final fused DSP max_abs | 0.000029855 |

Important boundary: Phase 3 currently expects the compressed payload to arrive complete. If most compressed fragments are missing, reconstruction is not yet guaranteed. That future work would need missing-tile metadata, partial payload reconstruction, and concealment across missing tile regions.

## Transport Backends

`udp_demo` supports two transport backends:

| Backend | What it does | When to use |
| --- | --- | --- |
| `--transport=inproc` | Injects payloads directly into the receiver path without kernel UDP. | Benchmarking codec and receiver behavior. |
| `--transport=udp` | Sends packets through loopback UDP sockets. | Testing socket behavior and real packetization. |

The in-process path is not a fake algorithm path. It still exercises the sender/receiver logic, but it removes kernel socket overhead so codec and reconstruction work are easier to measure.

## Demo Modes

`udp_demo` supports three content modes:

| Mode | Meaning | Receiver |
| --- | --- | --- |
| `--mode=full` | Send raw full frames. | `ReceiverEngine` |
| `--mode=dir` | Seed once, then send temporal row refreshes. | `ReceiverEngine` |
| `--mode=cs` | Send compressed sample payloads. | `CompressiveReceiverEngine` |

Common benchmark options:

| Option | Meaning |
| --- | --- |
| `--frames=N` | Number of frames to run. |
| `--timeout-us=N` | Receiver timeout. |
| `--refresh-rows=N` | DIR rows per temporal refresh frame. |
| `--benchmark-warmup=N` | Warmup iterations for detailed DSP validation. |
| `--benchmark-iterations=N` | Iterations for detailed DSP validation. |
| `--benchmark-every=N` | How often to run detailed validation. `0` means only the first frame. |
| `--progress-every=N` | How often to print progress frames. |
| `--log-every=N` | Packet/frame log cadence. |

## Test Coverage

The CTest suite covers the main subsystems:

| Test | Area |
| --- | --- |
| `ImageBasicOperations` | Basic image construction and pixel operations. |
| `ConvolutionAlgorithms` | Convolution correctness. |
| `PerformanceBenchmarks` | Baseline performance regression checks. |
| `ThreadScaling` | OpenMP/thread behavior. |
| `DSPRegression` | DSP equivalence and regression coverage. |
| `TemporalRefresh` | DIR schedule behavior. |
| `TemporalReceiver` | Temporal receiver reconstruction. |
| `TemporalBitrate` | DIR bitrate-spike comparison. |
| `TemporalDSPContinuity` | Phase 2 continuity under temporal loss. |
| `ProtocolContract` | Packet and flag layout. |
| `SPSCQueue` | Ready queue behavior. |
| `SenderEngine` | Sender integration without sockets. |
| `TransportStress` | Multi-frame stress scenarios. |
| `TransportRandomized` | Seeded randomized transport properties. |
| `ReceiverEngine` | Main receiver assembly behavior. |
| `Phase2Concealment` | Analytic concealment quality and timing. |
| `Phase3Compressive` | CS encode/decode and sender/receiver round trip. |
| `UDPLoopback` | Real UDP loopback behavior. |

`test_phase6` and `test_avx2` are built as manual validation helpers rather than routine CTest checks.

## Performance Interpretation

Use the right tool for the claim:

| Claim | Use |
| --- | --- |
| DSP kernel fits a 144 Hz frame budget | `dsp_benchmark`, paced 144 Hz section. |
| CPU thermal behavior under continuous pressure | `dsp_benchmark`, burst thermal stress section. |
| DIR bitrate reduction and exact reconstruction | `udp_demo --mode=dir`. |
| Phase 2 concealment quality and latency | `test_phase2_concealment` and `TemporalDSPContinuity`. |
| Phase 3 codec payload ratio and reconstruction quality | `test_phase3_compressive`. |
| Phase 3 main path over many frames | `udp_demo --mode=cs --frames=1000`. |

Do not mix these into one overbroad claim.

Correct interpretation:

- The DSP-only fused path currently passes 144 Hz at P99 under paced release benchmarking.
- DIR currently reconstructs exactly in no-loss 1000-frame in-process runs.
- Phase 2 currently improves temporal-loss quality over stale hold in regression tests.
- Phase 3 currently sends about 7.92% of raw frame bytes and stays accurate over the 1000-frame in-process demo.

Incorrect interpretation:

- The whole project is hard real-time under every OS and thermal condition.
- Phase 3 can lose 90% of its compressed UDP packets and still reconstruct reliably.
- Synthetic-frame quality automatically proves natural-video quality.
- `--transport=inproc` is the same as real network behavior.

## Current Non-Goals

The current prototype intentionally does not yet solve:

- hard real-time scheduling across arbitrary host load
- network congestion control
- retransmission or forward-error correction
- partial compressed-payload recovery
- arbitrary resolution support
- GPU offload
- dataset-wide image/video quality evaluation

These are good next steps, but they are outside the implemented core as of the current benchmark report.

## Suggested Reading Order

If you are new to the repo, read in this order:

1. `README.md` for the project overview and commands.
2. `BENCHMARKS.md` for current measured numbers.
3. This file for the architecture and phase-by-phase behavior.
4. `ROADMAP.md` for the remaining work and non-goals.
5. `src/udp_demo.cpp` if you want the end-to-end control flow.
6. `src/CompressiveSampling.cpp` if you want the Phase 3 codec details.
