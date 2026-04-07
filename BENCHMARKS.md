# Benchmark Report

Captured from the local release build on 2026-04-07.

This report is intentionally conservative. It separates kernel-level DSP timing, end-to-end demo timing, and burst thermal stress so the project does not overclaim one number as proof for every path.

## Environment And Target

| Item | Value |
| --- | --- |
| Build directory | `build-release` |
| Resolution | 1920x1080 |
| Pixels per frame | 2,073,600 |
| Pixel format | 32-bit float |
| Raw frame bytes | 8,294,400 |
| Target rate | 144 Hz |
| Target budget | 6.944 ms/frame |
| Transport benchmark backend | `--transport=inproc` |

The in-process transport backend still exercises the project sender/receiver logic, but avoids kernel UDP loopback overhead. Use `--transport=udp` when testing socket behavior specifically.

## How To Reproduce

Build release:

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
```

Run the validation suite:

```bash
ctest --test-dir build-release --output-on-failure
```

If sockets are blocked by the environment:

```bash
ctest --test-dir build-release -E 'UDPLoopback' --output-on-failure
```

Run the benchmark commands:

```bash
./build-release/dsp_benchmark
./build-release/test_phase2_concealment
./build-release/test_phase3_compressive
./build-release/udp_demo --transport=inproc --mode=dir --frames=1000 --refresh-rows=32 --timeout-us=2000000 --benchmark-warmup=0 --benchmark-iterations=1 --benchmark-every=0 --progress-every=1000 --log-every=0
./build-release/udp_demo --transport=inproc --mode=cs --frames=1000 --timeout-us=2000000 --benchmark-warmup=0 --benchmark-iterations=1 --benchmark-every=0 --progress-every=1000 --log-every=0
```

## Executive Summary

| Area | Key Result | Interpretation |
| --- | --- | --- |
| DSP paced fused path | Mean `3.185 ms`, P99 `6.284 ms`. | P99 passes the 6.944 ms 144 Hz budget. |
| DSP burst stress | Fused P99 `7.462 ms`, max `16.969 ms`. | Burst mode exposes thermal/scheduling variance. |
| Phase 1 DIR | `245,760` bytes per temporal frame. | 2.96% of full raw frame after the seed frame. |
| Phase 2 concealment | Avg `0.3148 ms`, concealed MAE `0.00031911`. | Fast and much better than stale hold on the test scene. |
| Phase 3 controlled codec | Payload `656,654` bytes, max_abs `0.000301`. | Accurate compact payload at 7.92% of raw bytes. |
| Phase 3 1000-frame path | `1000 / 1000` frames validated. | Stable and accurate, but not yet hard 144 Hz end-to-end. |

## DSP Benchmark

Command:

```bash
./build-release/dsp_benchmark
```

### Single Operations

| Operation | Frames | Mean | Median | P99 | Max | Result |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| Gaussian blur AVX2 | 100 | 1.893 ms | 1.493 ms | 4.428 ms | 4.428 ms | P99 passes 144 Hz |
| Sobel magnitude AVX2 | 100 | 1.511 ms | 1.512 ms | 2.911 ms | 2.911 ms | P99 passes 144 Hz |

These single-op numbers are useful for kernel-level regression checks. They should not be used as the full transport-pipeline frame time.

### Paced 144 Hz Fused Run

The paced run sleeps to the 144 Hz frame cadence and measures fused DSP compute time. This is the best DSP-only number for normal frame-paced claims.

| Metric | Value |
| --- | ---: |
| Frames | 1000 |
| Target budget | 6.944 ms |
| Mean | 3.185 ms |
| Median | 3.252 ms |
| P99 | 6.284 ms |
| Max | 8.772 ms |
| Compute misses | 7 / 1000 (0.70%) |
| Late OS wakeups | 2 / 1000 (0.20%) |
| Skipped frame slots | 7 |
| Effective cadence | 142.9 FPS |

Interpretation: P99 passes the target, but rare max outliers remain. This supports a paced DSP claim, not a hard real-time guarantee.

### Burst Thermal Stress

The burst run intentionally executes continuously without frame pacing. It is a worst-case pressure test for thermal and scheduling variance.

| Pipeline | Frames | Mean | Median | P99 | Max |
| --- | ---: | ---: | ---: | ---: | ---: |
| Fused 5x5 kernel | 1000 | 3.789 ms | 3.884 ms | 7.462 ms | 16.969 ms |
| Separate 3x3 pipeline | 1000 | 3.702 ms | 3.520 ms | 7.094 ms | 20.191 ms |

Thermal analysis:

| Segment | Fused latency |
| --- | ---: |
| First 10% | 2.076 ms |
| Last 10% | 5.839 ms |
| Delta | 181.3% |

Interpretation: the CPU can meet the DSP budget under paced operation at P99, but the burst stress path shows significant thermal and scheduling variance. Any public claim should say "paced P99" rather than "hard real-time under all conditions."

## Phase 1: Distributed Intra-Refresh

Command:

```bash
./build-release/udp_demo --transport=inproc --mode=dir --frames=1000 --refresh-rows=32 --timeout-us=2000000 --benchmark-warmup=0 --benchmark-iterations=1 --benchmark-every=0 --progress-every=1000 --log-every=0
```

### Bandwidth Shape

| Metric | Value |
| --- | ---: |
| Full seed frame payload | 8,294,400 bytes |
| Full seed frame datagrams | 7,078 |
| DIR refresh rows | 32 |
| DIR payload per temporal frame | 245,760 bytes |
| DIR datagrams per temporal frame | 210 |
| DIR payload ratio | 2.96% of full frame |

### Reconstruction Quality

| Metric | Value |
| --- | ---: |
| Validated frames | 1000 / 1000 |
| Final input mean_abs | 0.000000000 |
| Final input max_abs | 0.000000000 |
| Final fused DSP mean_abs | 0.000000000 |
| Final fused DSP max_abs | 0.000000000 |

Interpretation: Phase 1 removes the full-frame bitrate spike after the seed frame and reconstructs exactly in no-loss in-process runs.

## Phase 2: Analytic Concealment

Command:

```bash
./build-release/test_phase2_concealment
```

| Metric | Value |
| --- | ---: |
| Stale-hold MAE | 0.01185084 |
| Concealed MAE | 0.00031911 |
| Healed rows | 24 |
| 32-row concealment best | 0.3022 ms |
| 32-row concealment average | 0.3148 ms |

Interpretation: Phase 2 is sub-millisecond on the tested 32-row concealment pass and materially improves over stale hold.

The test proves the algorithm on a controlled scene. `TemporalDSPContinuity` covers the main receiver continuity path under deterministic row-loss patterns.

## Phase 3: Compressive Sampling

### Controlled Codec Test

Command:

```bash
./build-release/test_phase3_compressive
```

| Metric | Value |
| --- | ---: |
| Payload bytes | 656,654 |
| Sample ratio | 10.00% |
| Payload ratio | 7.92% of raw frame bytes |
| Payload reconstruction MAE | 0.000024 |
| Payload reconstruction max_abs | 0.000301 |
| Encode time | 5.023 ms |
| Reconstruct time | 1.911 ms |
| Sender/receiver round-trip MAE | 0.000024 |
| Sender/receiver round-trip max_abs | 0.000438 |

### 1000-Frame Main Path

Command:

```bash
./build-release/udp_demo --transport=inproc --mode=cs --frames=1000 --timeout-us=2000000 --benchmark-warmup=0 --benchmark-iterations=1 --benchmark-every=0 --progress-every=1000 --log-every=0
```

| Metric | Value |
| --- | ---: |
| Validated frames | 1000 / 1000 |
| Payload bytes per frame | 656,654 |
| Payload ratio | 7.92% of full frame |
| Encode best | 2.444 ms |
| Encode average | 4.399 ms |
| Encode worst | 18.241 ms |
| Average build stage | 1.869 ms |
| Average receiver ingest stage | 2.101 ms |
| Average ready-wait stage | 0.002 ms |
| Average validation stage | 0.241 ms |
| Final input mean_abs | 0.000001541 |
| Final input max_abs | 0.000012338 |
| Final fused DSP mean_abs | 0.000002901 |
| Final fused DSP max_abs | 0.000029855 |

Approximate average stage budget including encode:

```text
4.399 ms encode
+ 1.869 ms build
+ 2.101 ms receiver ingest
+ 0.002 ms ready wait
+ 0.241 ms validation
= 8.612 ms/frame
```

Interpretation: Phase 3 is accurate and stable over 1000 frames, and the payload is below 10% of raw frame bytes. The full CS path is still above the 6.944 ms 144 Hz frame budget on average when encode and receiver ingest are included, and the worst encode outlier still needs work before a hard real-time Phase 3 claim.

## Claim Matrix

| Claim | Supported? | Evidence |
| --- | --- | --- |
| DSP fused kernel fits 144 Hz under paced release P99. | Yes | Paced P99 `6.284 ms` under `6.944 ms`. |
| DIR removes full-frame bitrate spikes after the seed frame. | Yes | DIR temporal frame is `2.96%` of full frame. |
| DIR reconstructs exactly in no-loss runs. | Yes | Final input and fused DSP diff are zero. |
| Phase 2 improves temporal-row loss over stale hold. | Yes | Concealed MAE is much lower than stale MAE. |
| Phase 3 sends below 10% of raw frame bytes. | Yes | Payload ratio is `7.92%`. |
| Phase 3 is accurate on the 1000-frame in-process demo. | Yes | Final input max_abs is `0.000012338`. |
| Phase 3 is hard real-time end-to-end at 144 Hz. | Not yet | Average stage budget is about `8.612 ms/frame`. |
| Phase 3 recovers if 90% of compressed packets are lost. | Not yet | Current compressed payload expects completeness. |

## Benchmark Caveats

- The benchmark numbers are local to this machine because `-march=native` is enabled.
- `--transport=inproc` avoids kernel UDP overhead and should not be presented as real network latency.
- The CS tests use synthetic/smooth frame patterns, not a broad natural-image dataset.
- Burst thermal stress intentionally creates worse variance than normal paced operation.
- OS scheduling, power state, and CPU temperature can change max and P99 numbers run-to-run.

