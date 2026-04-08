# ZenoFrame Math Notes

This document gives the math behind the three main ideas in the project without going line-by-line through the source.

The formulas here are meant to match the implemented behavior closely enough to explain what the code is doing.

## Notation

Let:

- $W$ be frame width
- $H$ be frame height
- $x[n]$ be a 1D row signal
- $z[n]$ be its analytic representation
- $r$ be the number of refreshed rows in a DIR update
- $\rho$ be the target sampling ratio for Phase 3

Frames are currently grayscale float images with:

- $W = 1920$
- $H = 1080$

## Phase 1: Distributed Intra-Refresh

The raw full-frame payload is:

$$
B_{\text{full}} = W \cdot H \cdot b
$$

where $b$ is bytes per pixel.

For a temporal refresh window of $r$ rows:

$$
B_{\text{dir}} = W \cdot r \cdot b
$$

So the payload ratio is:

$$
\frac{B_{\text{dir}}}{B_{\text{full}}} = \frac{r}{H}
$$

For the current 32-row setup:

$$
\frac{32}{1080} \approx 0.0296
$$

which is about:

$$
2.96\%
$$

The receiver keeps a persistent reference frame $R_t$ and updates only the refreshed rows:

$$
R_t(y, x) =
\begin{cases}
F_t(y, x), & y \in \text{refresh window} \\
R_{t-1}(y, x), & \text{otherwise}
\end{cases}
$$

In the no-loss path, this reconstruction is exact.

## Phase 2: Short Hilbert FIR Plus Analytic Interpolation

Phase 2 is used when a temporal refresh span is incomplete.

### 1D Hilbert FIR Approximation

The code computes a short odd-symmetric Hilbert FIR approximation using taps at offsets $k \in \{1, 3, 5\}$:

$$
\hat{x}[n] =
\sum_{k \in \{1,3,5\}}
\frac{2}{\pi k} \left(x[n+k] - x[n-k]\right)
$$

This gives an approximate imaginary component for a row signal $x[n]$.

The analytic signal is then:

$$
z[n] = x[n] + j\hat{x}[n]
$$

### Amplitude And Unit Phasor

For a boundary row, the code computes:

$$
A[n] = |z[n]| = \sqrt{x[n]^2 + \hat{x}[n]^2}
$$

and the normalized complex direction:

$$
u[n] = \frac{z[n]}{|z[n]|}
$$

This separates magnitude from local analytic direction.

### Row Healing

Suppose we have two boundary profiles around a missing span:

- previous boundary: $(A_0[n], u_0[n])$
- next boundary: $(A_1[n], u_1[n])$

For an interpolation parameter $\alpha \in [0, 1]$, the implementation blends:

$$
A_{\alpha}[n] = (1-\alpha)A_0[n] + \alpha A_1[n]
$$

and blends the unit phasors, then renormalizes:

$$
\tilde{u}_{\alpha}[n] = (1-\alpha)u_0[n] + \alpha u_1[n]
$$

$$
u_{\alpha}[n] = \frac{\tilde{u}_{\alpha}[n]}{|\tilde{u}_{\alpha}[n]|}
$$

The healed row value is the real part of the reconstructed analytic signal:

$$
\tilde{x}_{\alpha}[n] = \Re\left(A_{\alpha}[n]u_{\alpha}[n]\right)
$$

The implementation clamps the result to the valid pixel interval:

$$
\tilde{x}_{\alpha}[n] \in [0, 1]
$$

### Why This Is Better Than Stale Hold

Stale hold is effectively:

$$
\tilde{x}[n] = x_{\text{old}}[n]
$$

which does not continue the current boundary structure across the missing span.

The analytic interpolation path at least tries to preserve:

- boundary amplitude
- local row direction/phase behavior
- continuity across the missing rows

## Phase 3: Compressive Sampling

Phase 3 replaces raw frame transport with a tile-based sampled payload.

### Tile Sampling

Let each tile contain $P$ pixels.

For a target sampling ratio $\rho$, the nominal number of samples per tile is:

$$
m \approx \rho P
$$

The current implementation also enforces a minimum sample count per tile, so in practice:

$$
m = \max(m_{\min}, \text{budgeted samples for that tile})
$$

with:

- $\rho = 0.10$
- $m_{\min} = 6$

The sender writes:

1. a payload header
2. one small header per tile
3. sampled local pixel indices and quantized sample values

### Payload Ratio

The reported payload ratio is:

$$
\frac{B_{\text{payload}}}{B_{\text{full}}}
$$

In the current controlled test:

$$
\frac{656654}{8294400} \approx 0.0792
$$

which is:

$$
7.92\%
$$

### Reconstruction Model

The receiver reconstructs each tile using two stages.

#### Fast Affine Plane Path

For smooth tiles, the code first tries an affine model:

$$
\hat{x}(u, v) = a + bu + cv
$$

If the sampled points fit this model with low enough sample error, the affine fit is accepted directly.

This is fast and works well on smooth gradients.

#### Sparse Reconstruction Fallback

For harder tiles, the code falls back to a sparse reconstruction model over a fixed dictionary $D$:

$$
y \approx \Phi D \alpha
$$

where:

- $y$ is the vector of observed samples
- $\Phi$ selects the sampled coordinates
- $D$ is the tile dictionary
- $\alpha$ is a sparse coefficient vector

The implementation uses an OMP-style sparse pursuit with an atom cap:

$$
\|\alpha\|_0 \le K
$$

with the current fallback using:

$$
K = 8
$$

and a dictionary atom budget of:

$$
24
$$

After reconstruction, the tile is clamped to the observed sample range to suppress bad outliers.

## DSP Budget Interpretation

The paced DSP benchmark is not the same thing as the full transport path.

The DSP-only benchmark measures fused image-kernel compute time under a 144 Hz cadence:

$$
T_{\text{dsp}}
$$

The full Phase 3 path includes more than that:

$$
T_{\text{full}} =
T_{\text{encode}} +
T_{\text{build}} +
T_{\text{receiver}} +
T_{\text{ready}} +
T_{\text{validation}}
$$

Using the reported release averages from the 1000-frame in-process CS run:

$$
T_{\text{full}} \approx
4.399 + 1.869 + 2.101 + 0.002 + 0.241 = 8.612 \text{ ms}
$$

So the current strongest honest statement is:

- the DSP path fits the 144 Hz budget at P99 in the paced benchmark
- the full Phase 3 path is accurate and stable, but not yet under the 6.944 ms budget end to end

## What The Math Does Not Yet Cover

These formulas explain the implemented core, but not every engineering detail:

- thread scheduling
- packet fragmentation and queueing behavior
- timeout policy
- exact sample-budget heuristics per tile
- future partial compressed-payload recovery

For those details, use `HOW_IT_WORKS.md` for the short version and the source for the implementation details.
