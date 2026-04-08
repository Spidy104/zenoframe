# How ZenoFrame Works

This is the short version of the project for someone who wants the idea without reading a long architecture note.

## The Core Problem

If you try to move 1080p frames at 144 Hz with a naive "send the whole frame every time" approach, you run into two problems quickly:

1. The payload is large and bursty.
2. If part of the frame update is missing, the receiver either stalls or shows stale data.

ZenoFrame explores three ways to make that better.

## Phase 1: Distributed Intra-Refresh

Instead of sending a full frame every time, ZenoFrame sends:

1. One full seed frame.
2. A small rolling row window on each later frame.

For the current 32-row setting at 1080p:

- full frame: 8,294,400 bytes
- temporal refresh payload: 245,760 bytes
- ratio: 2.96% of a full frame

On the receiver side, those refreshed rows are written into a persistent reference frame. In the no-loss path, that reconstruction is exact.

What this buys you:

- much flatter bandwidth after the seed frame
- a simpler transport problem than repeated full-frame bursts

## Phase 2: Analytic Concealment

Phase 2 only matters when temporal refresh data is incomplete.

Instead of saying "some rows are missing, just keep the old ones," the receiver tries to infer missing row spans from nearby valid rows.

The current implementation:

1. Looks at the valid rows around the missing span.
2. Computes a short 1D Hilbert FIR approximation for those boundary rows.
3. Treats each row as an analytic signal with real and imaginary components.
4. Interpolates in the analytic domain to fill in the missing rows.

So the idea is not simple linear blur in image space. It is phase-aware continuation of the local row structure.

What this buys you:

- better continuity than stale hold in the regression scene
- sub-millisecond concealment cost on the tested 32-row pass

## Phase 3: Compressive Sampling

Phase 3 asks a different question:

Can we avoid sending all raw frame bytes at all?

The current answer is:

1. Split the image into tiles.
2. Sample only part of each tile.
3. Write those samples into a compact payload.
4. Reconstruct the tile on the receiver side.

In the current implementation:

- target sample ratio: 10%
- actual payload ratio: about 7.92% of raw frame bytes
- tiles use a fast affine-plane path first
- harder tiles fall back to an OMP-style sparse reconstruction path

What this buys you:

- a compact payload that still reconstructs very accurately on the current synthetic/smooth tests

What it does not yet buy you:

- reliable recovery when most compressed UDP fragments are missing

That is the next major gap.

## The Main Data Flow

For a frame entering the system:

1. The sender chooses a transport mode:
   - full
   - DIR
   - CS
2. The sender packetizes the data.
3. The receiver reassembles the packets or payload.
4. The receiver publishes a ready frame.
5. The demo/tests compare the ready frame against the expected result.

The same high-level machinery is reused across modes:

- reusable frame buffers
- lock-free SPSC ready queues
- a shared protocol/header layer
- the same validation and benchmark harnesses

## Why The Repo Has Several Docs

Each markdown file has a different job:

| File | Best use |
| --- | --- |
| `README.md` | Quick project pitch and commands |
| `HOW_IT_WORKS.md` | Short conceptual walkthrough |
| `MATH.md` | The formulas behind the phases |
| `BENCHMARKS.md` | Current measured numbers |
| `TESTING.md` | Validation commands and test map |
| `ROADMAP.md` | Remaining work and non-goals |

## Current Honest Bottom Line

The strongest current claims are:

- the paced DSP path fits the 144 Hz budget at P99 in the release benchmark
- Phase 1 gives a very large payload reduction after the seed frame with exact no-loss reconstruction
- Phase 2 is meaningfully better than stale hold in the regression test
- Phase 3 sends a much smaller payload and reconstructs accurately over the current 1000-frame in-process demo

The most important current non-claim is:

- Phase 3 is not yet a "works even if 90% of compressed UDP packets are lost" system
