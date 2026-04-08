# Results

This file is kept as a compatibility entry point because earlier versions of the project used `results.md` as the main benchmark report.

The current report is now split across:

| File | Purpose |
| --- | --- |
| `BENCHMARKS.md` | Current measured numbers and benchmark interpretation. |
| `README.md` | Project overview and common commands. |
| `HOW_IT_WORKS.md` | Short conceptual walkthrough of the project. |
| `MATH.md` | The formulas behind the implemented phases. |
| `TESTING.md` | Validation commands, CTest map, and troubleshooting. |
| `ROADMAP.md` | Remaining work and non-goals. |

## Current Benchmark Commands

```bash
./build-release/dsp_benchmark
./build-release/test_phase2_concealment
./build-release/test_phase3_compressive
./build-release/udp_demo --transport=inproc --mode=dir --frames=1000 --refresh-rows=32 --timeout-us=2000000 --benchmark-warmup=0 --benchmark-iterations=1 --benchmark-every=0 --progress-every=1000 --log-every=0
./build-release/udp_demo --transport=inproc --mode=cs --frames=1000 --timeout-us=2000000 --benchmark-warmup=0 --benchmark-iterations=1 --benchmark-every=0 --progress-every=1000 --log-every=0
```

## Current Short Summary

| Area | Current result |
| --- | --- |
| DSP paced 144 Hz | P99 `6.284 ms` against a `6.944 ms` target. |
| Phase 1 DIR | `2.96%` of full raw-frame payload per temporal refresh frame. |
| Phase 2 concealment | Avg `0.3148 ms`, concealed MAE `0.00031911`. |
| Phase 3 controlled codec | `7.92%` raw payload, MAE `0.000024`, max_abs `0.000301`. |
| Phase 3 1000-frame path | `1000 / 1000` frames validated, final input max_abs `0.000012338`. |

For the full details and caveats, read `BENCHMARKS.md`.
