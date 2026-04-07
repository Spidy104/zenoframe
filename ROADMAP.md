# Roadmap

The core prototype is implemented. This file is the honest boundary between what the project already proves and what would make it stronger as a research artifact or production candidate.

## Done

| Area | Status |
| --- | --- |
| Phase 1 distributed intra-refresh | Implemented in the main sender/receiver path. |
| Phase 2 analytic concealment | Implemented and integrated into receiver timeout/partial-refresh handling. |
| Phase 3 compressive sampling | Implemented with tiled sampling, compact payloads, and receiver reconstruction. |
| Main demos | `full`, `dir`, and `cs` modes run through `udp_demo`. |
| DSP benchmark reporting | `dsp_benchmark` reports single-op, paced 144 Hz, and burst stress numbers. |
| Documentation | README, architecture notes, benchmark report, testing guide, and roadmap are present. |

## Highest Priority Next Work

1. Phase 3 packet-loss tolerance.

Current state: Phase 3 sends roughly 8-10% of the raw frame bytes, but still expects the compressed payload to arrive complete.

What to add:

- per-tile missing-data tracking
- partial compressed payload reconstruction
- missing-tile concealment using nearby reconstructed tiles
- optional integration with Phase 2-style analytic concealment
- tests where deterministic fragments or tiles are dropped

Definition of done:

- A CS frame can be published with missing compressed fragments.
- The ready-frame descriptor reports partial/missing-tile state clearly.
- Quality remains bounded against stale or dropped-frame baselines.
- Tests cover 1%, 5%, 10%, and bursty compressed-fragment loss.

2. Phase 3 end-to-end timing.

Current state: Phase 3 reconstruction quality is strong, but the 1000-frame main path averages about `8.612 ms/frame` when encode and receiver ingest are included.

What to improve:

- reduce encode worst-case outliers
- reduce receiver ingest overhead
- reduce frame-build overhead in the demo path
- add clearer per-stage histograms or P95/P99 reporting
- consider a persistent encode workspace if profiling shows repeated allocation or setup cost

Definition of done:

- 1000-frame CS in-process run reports average under `6.944 ms/frame`.
- P99 stage budget is reported, not just average.
- Worst encode spikes are either removed or clearly explained.

3. Natural-image quality evaluation.

Current state: most tests use synthetic frames and deterministic gradients. That is useful for regressions, but not enough for broad visual-quality claims.

What to add:

- a small local dataset harness
- support for loading deterministic raw/PGM/portable test images
- metrics for MAE, max_abs, and DSP-continuity error
- per-image summary tables
- optional artifacts for worst-case tiles

Definition of done:

- The benchmark report includes results across multiple image classes.
- The docs clearly separate synthetic quality from dataset quality.

## Medium Priority Work

4. CI.

Add a CI workflow for the automated CTest suite.

Recommended shape:

- build Debug
- build Release
- run `ctest --output-on-failure`
- either run `UDPLoopback` separately or mark it clearly when sockets are unavailable
- upload benchmark logs only for manual/performance jobs, not every PR

5. Portable build profile.

The current build is intentionally tuned with `-march=native`, AVX2, FMA, OpenMP, and LTO. That is good for the prototype but not for distributing binaries.

What to add:

- a CMake option for native vs portable CPU flags
- runtime feature checks or documented build presets
- a scalar or lower-SIMD fallback if portability becomes a real goal

6. Better benchmark ergonomics.

The benchmark numbers are useful, but the output is still mostly terminal text.

What to add:

- optional CSV or JSON output
- stable summary blocks for automated comparison
- P50/P95/P99 for `udp_demo` stage timing
- separate warm and cold run profiles

## Lower Priority Ideas

7. Forward-error correction or retransmission.

This would move the transport closer to a real network protocol, but it is a different research direction than the current codec/DSP work.

8. GPU offload.

The current project is CPU-first. A GPU path could improve throughput, but it would change the architecture and benchmark story substantially.

9. Arbitrary resolution support.

The current prototype is built around 1920x1080. Supporting arbitrary resolutions would require retuning tile sizes, packet counts, buffer sizes, and tests.

## Non-Goals For The Current Prototype

- Guaranteeing hard real-time behavior under arbitrary OS scheduling and thermal conditions.
- Claiming recovery when only 10% of compressed UDP packets arrive.
- Supporting arbitrary resolutions without revisiting tile sizes, packet layout, and memory budgets.
- Providing a production network protocol with congestion control, retransmission, or forward-error correction.
- Claiming natural-video quality without dataset-backed testing.

## Suggested Next Milestone

The best next milestone is Phase 3 packet-loss tolerance.

Reason:

- It directly addresses the most important limitation in the current Phase 3 story.
- It builds on the already-implemented Phase 2 concealment work.
- It creates a much stronger research claim than another small micro-optimization pass.

Suggested milestone title:

`Phase 3.5: Partial CS Payload Recovery`

Suggested deliverables:

- compressed-fragment drop tests
- missing-tile metadata in the receiver
- partial CS reconstruction path
- concealment fallback for missing tiles
- benchmark section comparing complete payload, partial payload, and stale/drop behavior

