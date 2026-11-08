# Ocarina of Time performance ledger

Goal: Kokiri Forest outdoors at a stable 45-60 FPS at 1x (1920x1080 output) with the owner's
settings, grass, canopy and light haze intact. Read this before proposing an experiment.

## Method

- Host: i7-12700K, RX 7800 XT (RADV, Mesa 26.2.5), Linux/X11. Owner settings
  (`sdl2-config.ini`: Vulkan, 1x, vsync Immediate, FSR, FXAA, GPU accuracy Low, async shaders,
  Boost GPU time).
- Run: `eden-cli` on the owner's save, scripted walk (`EDEN_NX2_KEY_SCRIPT`), outdoors in
  Kokiri Forest from ~30 s. Measure seconds 44-84 with the debug server: `presents` delta over
  `uptime_s` (mean FPS), 1 s FPS samples (median/p95 frame time), `EDEN_DKB_FRAME_TRACE=1`
  per-second frame samples, `EDEN_NX2_GPU_PROFILE=1` zones. Two runs or more per variant,
  same binary where an env switch allows (A/B).
- Noise: about +-0.5 FPS between runs of one binary. One-second FPS samples are quantized;
  use the mean over the window.
- Image check: screenshot at ~88 s, per-channel means ~0.40/0.38/0.31, grass and canopy visible.
- Profilers: `EDEN_NX2_GPU_PROFILE` zones; gdb stack sampling (launch under gdb, SIGINT
  every 250 ms; ptrace_scope=1 forbids attaching); gperftools `libprofiler.so` via
  `LD_PRELOAD` with `CPUPROFILESIGNAL` to bracket a window.

## Where the time goes (HEAD 105d67db7c, 20.9 FPS)

The split GPU thread's back half is the bottleneck: the front waits for it (`split drain`
630 ms/s, mostly macro DMA reads); the back is busy ~860 ms/s.

- `MaxContinuousRange` page walks: 31% of the back (64 MiB table-pointer bindings since
  86069aa9f2 make each walk 8x longer).
- Guest CPU reads of GPU-written compute output: ~1180 trapped reads/s, each with a full
  `Scheduler::Finish` (`OnCPURead` -> `FlushRegion` -> `DownloadMemory`), 18-37% of the back.
- Per draw (~57k draws/s): storage and uniform buffer binding, `SynchronizeBuffer` word scans,
  page-cache marking (`mprotect`), method dispatch (3.7M method calls/s).

## Ledger

| # | Idea | Before -> after (mean FPS, 2 runs) | Verdict | Notes |
|---|---|---|---|---|
| 0 | Earlier: fence-commit coalescing (`EDEN_NX2_GPU_COMMIT_US`) | p50 21.3 ms | abandoned | RADV crash in vkCreateGraphicsPipelines |
| 0 | Earlier: merge perf/oot-fast-draw | slower | abandoned | crashes |
| 0 | Earlier: Selevet bce3c837 acquire queuing | worse | abandoned | |
| 0 | Earlier: async presentation | pathological | abandoned | |
| 0 | Earlier: 0.75x resolution | worse | rejected | also changes the goal's setting |
| 0 | Earlier: AA off / nearest | within noise | rejected | |
| 0 | Earlier: trap-and-flush reads (`EDEN_NX2_PREFLUSH=0`, ef3a246d17) | - | kept | |
| 0 | Earlier: PR #103 inline page writes (86028686ad) | - | kept | |
| 1 | Baseline 105d67db7c | 20.9 / 20.9 (1 s medians) | - | back thread bound |
| 2 | Port zortos 8cce3bc711: cache `MaxContinuousRange` until the page table changes | 20.9/20.9 -> 23.9/25.9 | kept (a4fb6e694d) | read handles 217 -> 31 ms/s |
| 3 | `EDEN_NX2_PREFLUSH=1` (download read ranges at every fence) | ~25 -> 18.4/20.9, stutters | rejected | fence 62 ms/s, method 264 ms/s |
| 4 | zortos 1398d8a1e2 diagnostic `EDEN_NX2_WRITTEN_SSBO_MARK_KIB=64` | traps 1177 -> 968/s | diagnostic only | 8 MiB written spans are a minority of the traps |
| 5 | Trap attribution (temporary tags) | - | diagnostic | ~all traps from compute storage writes; 46 per frame in a few bursts, often consecutive pages of one buffer |
| 6 | 64 MiB table-pointer cap back to 8 MiB (diagnostic, loses grass) | ~25.6 -> 26.9 (1 run) | diagnostic | the 64 MiB pool costs ~1 FPS after #2 |
| 7 | Widen a trap's flush to the GPU-modified run within 256 KiB | 25.97/25.16 -> 26.44/26.34 | superseded by #8 | finishes 1184 -> 835/s |
| 8 | At a flushing read, download the GPU writes in every range the CPU read before (one Finish) | 25.97/25.16 -> 30.75/30.73 | kept (23d33887a8) | finishes 1184 -> 70/s; #7 adds nothing on top (28.38/30.83 vs 28.37/28.88) |

## 2026-11-06: isolated stable-50–60 investigation

- New target supersedes the introductory 45 FPS floor: **mean >=50 FPS and actual guest-present
  interval p95 <=20 ms**, two warm 44–84 s runs, frame generation off. No visual/resolution cuts.
- Worktree `/home/loganp/lichii-perf60`, branch `perf/oot-60`, starts at `a6a0d57900`.
  Owner GUI and PR worktrees are untouched. Harness `oot-dumps/perfscripts/perf60.py` copies
  `sdl2-config.ini` per run and checks process names before launch. Results in `oot-dumps/perf60/`.
- Baseline is rebuilt with owner cache flags (clang 23.1.4, Ninja, RelWithDebInfo, nice 19,
  six jobs). Initial configure needed the owner's additional
  `VulkanHeaders_FORCE_BUNDLED=ON`; missing that flag was a setup failure, not a perf result.
- Measurement-only `EDEN_NX2_PRESENT_TRACE=1` logs each vi2 guest present interval; no framegen
  rate or reciprocal-of-1-second-FPS percentile is substituted for actual interval percentiles.
- Upstream fetched: Eglantine `0ca2032bff`, zortos PR184 `ce0897389a`. Reviewed upstream PR652,
  zortos PR184/179/178/161/166/149 and PR124 diffs. PR184 retains the consumed-read-trap
  `Subtract` and our batched readbacks; unsafe split overlap is deleted. PR179 DMA/SEM
  shortcuts are not correctness-qualified (one unexplained DMA mismatch; SEM unverified).
- Eglantine nx2 is newer than the baseline; merge is required after recording the exact baseline.
  Its native SSBO policy change must not overwrite our broad pointer/vegetation binding contract.
  PR652 has a substantially larger, separate write-bound/completion experiment; not yet measured.
- Submission batching `660e5f0039` + `313463cd27` is already ancestral to `a6a0d57900`;
  do not re-port it. `3835d8f499` idle-core spinning is not ancestral and remains a candidate.
- GPU profiling is enabled by **presence** of `EDEN_NX2_GPU_PROFILE`, even when its value is
  `0`. The new harness leaves it absent for qualification; `=1` is a diagnostic-only run.
  In PR184 profiling also turns off fast Maxwell3D dispatch/CB batching.
- Owner CLI config has `frame_gen=false`, `resolution_setup=3` (1x), `fast_gpu_time=1`
  (Boost), and `use_docked_mode=0`. These are preserved. CLI defaults to a 1280x720 window;
  harness resizes only its own X11 window to **1920x1080** and checks actual geometry.
- Existing capture history: `debug-1080p/normal-1/capture-1793880353-001` has main scene
  targets 864x480; later `debug-perf/capture-1793896929-001` and `debug-speed/...` have
  1440x810 targets (33 passes), with 1920x1080 presentation targets. New capture must verify
  the Max Resolution patch preserves the later native scene size; 1x output is not proof of
  internal scene dimensions. No resolution setting has been reduced.
- Installed Dynamic-fps is IPS32, build `E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348`,
  145 bytes, patches offsets `0x6e6dc4` and `0x3c46300`; the built-in max-resolution branch
  patch is at `main+0x805bd0` and does not overlap. Runtime Loader logs still need checking.
- Independent review `/tmp/opencode/upstream-perf-map.md` identified that **local #23d33887a8
  batching is not the corrected #147**: dirty reads still call `ArmReadTraps` after the core
  disarms them, so deduplication can suppress the next GPU-write rearm. Corrected #147 is now
  the required first standalone correctness commit, before upstream/performance integration.
  The in-progress upstream merge was aborted (only this agent's merge resolutions discarded).
- Initial actual-1080p baseline `base120-1`: 34.552 FPS; median 28.619 ms, p95 31.274 ms,
  p99 33.108 ms, 100% >20 ms (1,408 real guest intervals, 43.30–84.05 s; later runs start
  >=44 s). This is below earlier ~40 FPS; prior harness used CLI's default720p window.
  Screenshot `oot-dumps/perf60/base120-1.png`: grass/canopy/fog/ambient are visible, same
  forest area but current owner save/view differs from the old culling reference NPC framing.
- Runtime Loader **confirms both Dynamic-fps IPS and built-in Max Resolution are applied**
  (`base120-1.out`, 0.425 s and 0.452 s). Baseline focused suite: **305 cases / 8,285,778
  assertions passed**. These are baseline counts, not the new #147 regression tests yet.
- Corrected #147 production blocks were compared byte-for-byte to `9218296008`: the complete
  preflush decision/GetFlushArea/DownloadMemory/DownloadRangesMemory block (5,414 bytes), and
  deletion cleanup (241 bytes), **both exactly match**. Local table-pointer/rendering/debug code
  outside those blocks remains intact. Added the four upstream readback/trap regression cases
  and the real `ReadTrapTracker` helper, not a separate model.
- Completed raw baseline: VSYNC120 **34.55 / 31.75 FPS**; unset **36.40 / 31.96 FPS**.
  The assumed +-0.5 FPS noise does **not** hold in this current setup. Each run is internally
  fairly stable (e.g. unset36.6→36.5 first/last10s, unset31.9→32.2), but differs across boots.
  Screenshots show the same stationary camera and preserved foliage/fog, so no confident small
  A/B claim will be made from these numbers. Hypothesis: hybrid CPU/thread placement or
  shared-core contention; record per-thread last CPU and compare P-core-only `taskset -c0-15`.
- Profile `base120-profile` (diagnostic, 27.944 FPS): GPU front95.6% CPU, GPUBack90.2%,
  VulkanWorker20.7%, device82.5% busy. Outdoors the back spends ~205ms/s method dispatch,
  ~143ms/s bind-stage buffers, ~74.5ms/s handle reads, ~209ms/s other, ~88ms/s waiting.
  Front ~600ms/s split drain (~385ms MME DMA +211ms acquire) and ~179ms writer waits.
  ~970 submissions/s, ~5050 render passes/s,48 Finishes/s waiting~61ms/s.
- **Not a 120/3 pacing cap** in these runs: vi2 requested interval1/hz60, effective60Hz;
  outdoors `sleep_requested_ms=0`, `pace_wall_ms=0`, fence wait~3.16ms/frame while total
  guest intervals~35.7ms. Throughput/front-back dependencies dominate. Changing tick frequency
  alone cannot establish60FPS. Previous approximate31/40 figures are not reproduced here.
- New capture `oot-dumps/perf60/base120-profile/capture-1793923208-001`: **1920x1080 scene
  targets (34 entries)**,960x540 and480x270 subordinate passes. Max-resolution patch is active
  and scene dimensions are larger than older1440x810 /864x480 captures. Current benchmarks
  preserve these1920x1080 dimensions, not merely a1080p screenshot of lower-resolution3D.
- First standalone branch commit: **`0e905ad8ad` corrected #147**,35 cases/240 assertions pass.
  First isolated corrected run `traps120-1`:35.779FPS, median27.624,p9529.476,p9931.065ms,
 100%>20ms. This is not evidence of a speedup versus noisy raw baselines; correctness is kept
  regardless. `traps120-2` was discarded: merging changed the live Python debug-client source
  to conflict markers mid-run, causing the harness request to fail, not an emulator crash.
  Harness now uses a frozen copied client; retry is `traps120-2b`.
- The correctness-run binary was built with #147 only (prior to later source integrations).
  CMake's revision string does not automatically update on commit: it still reports a6a0d57900.
  Source checkpoint is known from build chronology; future final builds will reconfigure to
  refresh it. Frozen raw and corrected binaries: `oot-dumps/perf60-baseline-cli` and
  `oot-dumps/perf60-traps-cli`.
- Further standalone safety commits: **`59a260ccff`** = #149 deferred-submit only
  (`ceed7ca7f6`); **`bd3dd1404d`** = ownership/quiescence port of`df92fc6c95`, preserving
  local inline writes and profiling. No FSR3 change imported. Required Eglantine merge is
  **`528919928d`**, preserving existing eden paths and native/table binding semantics.
- #184 merge is staged, not yet accepted/measured. Explicit A/B controls retained:
  `EDEN_NX2_DEFER_FRAGMENT_BARRIER=0/1`, `EDEN_NX2_ASYNC_NATIVE_PIPELINES=0/1`, existing
  `EDEN_NX2_FLUSH_DRAWS`, `EDEN_NX2_FENCE_FLUSH_US`, `EDEN_NX2_FRAMES_AHEAD`.
  Borrowed production's idle-submission Drain regression (no StorageBindingPolicy change).
  MCP adapter tests passed after conflict resolution. Local frame-dump SIZE_CAP and capture-id
  safeguards retained; automatic merge's duplicate raw field removed. Depth/query files unchanged.
- Corrected #147 unrestricted pair **35.779 /35.602 FPS** (p9529.476/29.514ms,
  p9931.065/31.112ms;100%>20ms). Raw P-core-only pair (`taskset -c0-15`) is
  **37.091 /36.848 FPS**, p9528.109/28.311ms,p9928.827/29.071ms. This restores
  ~+-0.25FPS repeatability in this pair. It is a harness/launcher placement experiment, not a
  code performance change; corrected P-core-only reference pair follows before compiling #184.
  No host governor/clock setting was changed. Affinity is not silently attributed to a code gain.
- Controlled corrected #147 reference: **36.482 /36.464 FPS**, p9528.283/28.346ms,
  p9928.885/29.010ms,100%>20ms. Keep correctness fix despite raw pinned36.85–37.09:
  difference is small and stale CPU reads are not an optimization.
- #184 compilation initially exposed automatically merged duplicate `FrameDumpBuffers`
  declarations/definitions (not a product/runtime failure); removed duplicate upstream copies,
  retaining the identical local implementation and pending-write diagnostics. Rebuild passed.
- Integrated gates: **346 cases/61,864,626 assertions passed**; verify subset38 cases/
 53,578,828 assertions passed; additional async+quiescence verify4 cases/23 assertions passed;
  MCP15 passed. Full suite:739/741 pass,69,481,332/69,481,334 assertions pass. Two NV12
  conversion/color-range failures **also fail in a copied untouched a6a0d57900 owner test binary**
  (2 cases,31/33 assertions), with relevant code unchanged. Logs `oot-tests184*.log` and
  `oot-baseline-nv12.log` under`/tmp/opencode/`. Do not misreport this as a wholly green suite.
- Main integration default pair **42.683 /42.406 FPS** (~+16.6% vs corrected pinned baseline),
  p9524.760/24.958ms,p9925.546/25.798ms,100%>20ms. Visuals in`perf184-default-2.png`:
  foreground grass, distant canopy, fog, depth and sky ambient intact. **Not yet stable50FPS**.
- Barrier A/B (same binary, only deferral disabled): **42.124 /42.513 FPS**, p9525.181/24.820,
  p9925.751/25.581ms. Difference ~0.23FPS is inside observed paired variation; **no proved gain**.
  Screenshot`perf184-no-barrier-2.png` preserves effects. Prefer conservative barrier behavior
  unless further evidence changes this verdict. Remaining async/budget/fence/frames-ahead A/Bs follow.

## Priority switch: Frizti evaluation (owner request)

- Manual perf9 integration is cleanly checkpointed as **`0030b62d10`** on`perf/oot-60`.
  No owner or PR worktree modifications. Do not treat the checkpoint as delivery approval.
- Completed same-binary manual-integration A/Bs (all pinned0-15, VSYNC120, barrieroff):
  async emissionoff42.689/42.650FPS (p9528.135/24.834,p9944.882/25.604ms; first run
  irregular), budget12843.359/41.543 (p9524.354/26.331), fence500us40.110/41.233
  (p9526.431/25.679), frames-ahead032.017/34.205 (p9534.368/32.074).
  All stats JSONs include remaining columns. Async/budget have no reliable mean gain over
  their reference;5ms batching and frame-ahead appear useful, but still not50FPS. Frame-ahead
  permits ~one extra queued frame of input/display latency (~23–24ms here); input latency itself
  was not instrumented. No effect disabled and no native draw/frame skipping added.
- New worktree`/home/loganp/lichii-frizti`, branch`test/frizti-v2-radv`, tag`frizti-v2`
  (`765659af4c`) plus **`f9e78696bf`**, sole committed change: cherry-pick-x of RADV
  depth-float fix`d2da498172`. Same optional measurement-only present trace is uncommitted.
  Building CLI+tests+GUI(`yuzu`) with owner cache flags, clang/Ninja,nice19,6jobs.
- Quick source/build/test diff audit vs`zortos/nx2`: no added external HTTP/curl/exec/process
  launch/network connection found in runtime additions. New output files are emulator log-dir
  sampling profiles, requested profiler paths, and frame/flash diagnostics. New test sockets
  connect only127.0.0.1(debug-server tests); recursive deletion is restricted to a unique
  `temp_directory_path()/tas-input-test-<stamp>-<sequence>` fixture directory. This is a targeted
  diff audit, not a complete security certification. Audit files`/tmp/opencode/frizti-*-audit.diff`.
- Frizti built-in Dynamic FPS records exactly match installed IPS32 bytes (call offset
  module0x6e6cc4 + NSOheader0x100 = IPS0x6e6dc4; routine similarly0x3c46200→0x3c46300).
  Its preflight skips the **whole** built-in patch if any record was changed by a user mod;
  runtime log confirmation is pending. Max Resolution remains a distinct patch.
- Frizti perf9's default-off set: frames_ahead,clean_words,index_rebind,submit_budget,
  fence_flush,fragment_barrier,front_sleep,async_pipelines,bounds; split_overlap forciblyoff.
  However own **queued-acquire payloads**, ranged waits and CB read forwarding are defaulton.
  `EDEN_NX2_ACQUIRE_PRECHECK` defaults0 and`EDEN_NX2_LIST_WAIT_SKIP` defaultsfalse.
- Safety concerns to separate from raw B speed: queued-release ring consumption is not GPU/fence
  completion; ranged/forwarded reads track GPU virtual ranges, not complete physical aliasing.
  Conservative comparison can use`EDEN_NX2_ACQUIRE_QUEUED=0`,
  `EDEN_NX2_MACRO_DMA_DRAIN=1` (bypasses forwarding/ranged early reads),
  `EDEN_NX2_RANGED_WAITS=0`, `EDEN_NX2_FORWARD_READS=0`; do not enableprecheck2/list skip.
  Frizti also lacks df92fc6c95's BackQuiet/OwnsScheduler guard, despite routing many calls toback.
  Exact B (depth fix only) will be labeled experimental; a speed number cannot prove safety.
- Content audit: `src/video_core/macro.cpp` is **identical** inFrizti andoot-1080p, so
  87417bc06c's MME query flush is already present functionally despite divergent ancestry.
  GPU timestamps/query-cache files also have no diff. Additional shader/diagnostic differences
  are catalogued for review; do not blindly replay the local-only commit inventory.

- Run `traps120-1` @ `a6a0d57900`: 35.779 FPS, 44.30–84.97 s; env `{'EDEN_NX2_VSYNC_HZ': '120'}`, CPUSET `unrestricted`. Actual guest intervals n=1455: mean 27.945, median 27.624, p95 29.476, p99 31.065 ms; >20 ms 100.00%. Artifacts: `oot-dumps/perf60/traps120-1.*`. Verdict pending paired comparison/visuals.

- Run `traps120-2b` @ `a6a0d57900`: 35.602 FPS, 44.31–84.99 s; env `{'EDEN_NX2_VSYNC_HZ': '120'}`, CPUSET `unrestricted`. Actual guest intervals n=1448: mean 28.082, median 27.710, p95 29.514, p99 31.112 ms; >20 ms 100.00%. Artifacts: `oot-dumps/perf60/traps120-2b.*`. Verdict pending paired comparison/visuals.

- Run `raw-pcore120-1` @ `a6a0d57900`: 37.091 FPS, 44.27–85.01 s; env `{'EDEN_NX2_VSYNC_HZ': '120'}`, CPUSET `0-15`. Actual guest intervals n=1511: mean 26.956, median 26.820, p95 28.109, p99 28.827 ms; >20 ms 100.00%. Artifacts: `oot-dumps/perf60/raw-pcore120-1.*`. Verdict pending paired comparison/visuals.

- Run `raw-pcore120-2` @ `a6a0d57900`: 36.848 FPS, 44.30–85.04 s; env `{'EDEN_NX2_VSYNC_HZ': '120'}`, CPUSET `0-15`. Actual guest intervals n=1501: mean 27.134, median 27.016, p95 28.311, p99 29.071 ms; >20 ms 100.00%. Artifacts: `oot-dumps/perf60/raw-pcore120-2.*`. Verdict pending paired comparison/visuals.

- Run `traps-pcore120-1` @ `a6a0d57900`: 36.482 FPS, 44.26–84.99 s; env `{'EDEN_NX2_VSYNC_HZ': '120'}`, CPUSET `0-15`. Actual guest intervals n=1486: mean 27.415, median 27.051, p95 28.283, p99 28.885 ms; >20 ms 100.00%. Artifacts: `oot-dumps/perf60/traps-pcore120-1.*`. Verdict pending paired comparison/visuals.

- Run `traps-pcore120-2` @ `a6a0d57900`: 36.464 FPS, 44.30–84.01 s; env `{'EDEN_NX2_VSYNC_HZ': '120'}`, CPUSET `0-15`. Actual guest intervals n=1448: mean 27.419, median 27.028, p95 28.346, p99 29.010 ms; >20 ms 100.00%. Artifacts: `oot-dumps/perf60/traps-pcore120-2.*`. Verdict pending paired comparison/visuals.

- Run `perf184-default-1` @ `528919928d`: 42.683 FPS, 44.31–84.03 s; env `{'EDEN_NX2_VSYNC_HZ': '120'}`, CPUSET `0-15`. Actual guest intervals n=1695: mean 23.427, median 23.318, p95 24.760, p99 25.546 ms; >20 ms 100.00%. Artifacts: `oot-dumps/perf60/perf184-default-1.*`. Verdict pending paired comparison/visuals.

- Run `perf184-default-2` @ `528919928d`: 42.406 FPS, 44.29–84.03 s; env `{'EDEN_NX2_VSYNC_HZ': '120'}`, CPUSET `0-15`. Actual guest intervals n=1685: mean 23.581, median 23.423, p95 24.958, p99 25.798 ms; >20 ms 100.00%. Artifacts: `oot-dumps/perf60/perf184-default-2.*`. Verdict pending paired comparison/visuals.

- Run `perf184-no-barrier-1` @ `528919928d`: 42.124 FPS, 44.28–84.02 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'EDEN_NX2_DEFER_FRAGMENT_BARRIER': '0'}`, CPUSET `0-15`. Actual guest intervals n=1674: mean 23.735, median 23.520, p95 25.181, p99 25.751 ms; >20 ms 100.00%. Artifacts: `oot-dumps/perf60/perf184-no-barrier-1.*`. Verdict pending paired comparison/visuals.

- Run `perf184-no-barrier-2` @ `528919928d`: 42.513 FPS, 44.27–85.03 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'EDEN_NX2_DEFER_FRAGMENT_BARRIER': '0'}`, CPUSET `0-15`. Actual guest intervals n=1733: mean 23.517, median 23.344, p95 24.820, p99 25.581 ms; >20 ms 100.00%. Artifacts: `oot-dumps/perf60/perf184-no-barrier-2.*`. Verdict pending paired comparison/visuals.

- Run `perf184-no-async-1` @ `528919928d`: 42.689 FPS, 44.25–84.03 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'EDEN_NX2_DEFER_FRAGMENT_BARRIER': '0', 'EDEN_NX2_ASYNC_NATIVE_PIPELINES': '0'}`, CPUSET `0-15`. Actual guest intervals n=1698: mean 23.426, median 23.675, p95 28.135, p99 44.882 ms; >20 ms 77.68%. Artifacts: `oot-dumps/perf60/perf184-no-async-1.*`. Verdict pending paired comparison/visuals.

- Run `perf184-no-async-2` @ `528919928d`: 42.650 FPS, 44.25–85.02 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'EDEN_NX2_DEFER_FRAGMENT_BARRIER': '0', 'EDEN_NX2_ASYNC_NATIVE_PIPELINES': '0'}`, CPUSET `0-15`. Actual guest intervals n=1739: mean 23.438, median 23.271, p95 24.834, p99 25.604 ms; >20 ms 100.00%. Artifacts: `oot-dumps/perf60/perf184-no-async-2.*`. Verdict pending paired comparison/visuals.

- Run `perf184-budget128-1` @ `528919928d`: 43.359 FPS, 44.30–84.03 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'EDEN_NX2_DEFER_FRAGMENT_BARRIER': '0', 'EDEN_NX2_FLUSH_DRAWS': '128'}`, CPUSET `0-15`. Actual guest intervals n=1723: mean 23.057, median 22.974, p95 24.354, p99 25.156 ms; >20 ms 100.00%. Artifacts: `oot-dumps/perf60/perf184-budget128-1.*`. Verdict pending paired comparison/visuals.

- Run `perf184-budget128-2` @ `528919928d`: 41.543 FPS, 44.31–84.08 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'EDEN_NX2_DEFER_FRAGMENT_BARRIER': '0', 'EDEN_NX2_FLUSH_DRAWS': '128'}`, CPUSET `0-15`. Actual guest intervals n=1652: mean 24.080, median 23.773, p95 26.331, p99 30.463 ms; >20 ms 100.00%. Artifacts: `oot-dumps/perf60/perf184-budget128-2.*`. Verdict pending paired comparison/visuals.

- Run `perf184-fence500-1` @ `528919928d`: 40.110 FPS, 44.25–84.04 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'EDEN_NX2_DEFER_FRAGMENT_BARRIER': '0', 'EDEN_NX2_FENCE_FLUSH_US': '500'}`, CPUSET `0-15`. Actual guest intervals n=1596: mean 24.925, median 24.794, p95 26.431, p99 28.054 ms; >20 ms 100.00%. Artifacts: `oot-dumps/perf60/perf184-fence500-1.*`. Verdict pending paired comparison/visuals.

- Run `perf184-fence500-2` @ `528919928d`: 41.233 FPS, 44.27–84.02 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'EDEN_NX2_DEFER_FRAGMENT_BARRIER': '0', 'EDEN_NX2_FENCE_FLUSH_US': '500'}`, CPUSET `0-15`. Actual guest intervals n=1639: mean 24.263, median 24.119, p95 25.679, p99 26.899 ms; >20 ms 100.00%. Artifacts: `oot-dumps/perf60/perf184-fence500-2.*`. Verdict pending paired comparison/visuals.

- Run `perf184-frames0-1` @ `528919928d`: 32.017 FPS, 44.26–84.99 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'EDEN_NX2_DEFER_FRAGMENT_BARRIER': '0', 'EDEN_NX2_FRAMES_AHEAD': '0'}`, CPUSET `0-15`. Actual guest intervals n=1304: mean 31.221, median 30.961, p95 34.368, p99 35.738 ms; >20 ms 100.00%. Artifacts: `oot-dumps/perf60/perf184-frames0-1.*`. Verdict pending paired comparison/visuals.

- Run `perf184-frames0-2` @ `528919928d`: 34.205 FPS, 44.26–84.99 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'EDEN_NX2_DEFER_FRAGMENT_BARRIER': '0', 'EDEN_NX2_FRAMES_AHEAD': '0'}`, CPUSET `0-15`. Actual guest intervals n=1393: mean 29.227, median 29.064, p95 32.074, p99 34.197 ms; >20 ms 100.00%. Artifacts: `oot-dumps/perf60/perf184-frames0-2.*`. Verdict pending paired comparison/visuals.

- Run `fz-b0-warm` @ `f9e78696bf`: 44.823 FPS, 45.04–84.80 s; env `{'EDEN_NX2_VSYNC_HZ': '120'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1782: mean 22.304, median 22.255, p95 24.498, p99 26.195 ms; >20 ms 95.74%. Artifacts: `oot-dumps/perf60/fz-b0-warm.*`. Verdict pending paired comparison/visuals.

- Run `fz-b0-1` @ `f9e78696bf`: 44.672 FPS, 44.27–84.05 s; env `{'EDEN_NX2_VSYNC_HZ': '120'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1777: mean 22.390, median 22.371, p95 23.879, p99 24.821 ms; >20 ms 98.99%. Artifacts: `oot-dumps/perf60/fz-b0-1.*`. Verdict pending paired comparison/visuals.

- Run `fz-b0-2` @ `f9e78696bf`: 44.115 FPS, 44.15–85.00 s; env `{'EDEN_NX2_VSYNC_HZ': '120'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1802: mean 22.667, median 22.654, p95 24.149, p99 25.110 ms; >20 ms 99.28%. Artifacts: `oot-dumps/perf60/fz-b0-2.*`. Verdict pending paired comparison/visuals.

- Run `fz-b1a-1` @ `f9e78696bf`: 43.799 FPS, 44.25–84.02 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'EDEN_NX2_PERF_ON': 'frames_ahead,fence_flush,submit_budget'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1742: mean 22.832, median 22.751, p95 24.395, p99 25.634 ms; >20 ms 99.89%. Artifacts: `oot-dumps/perf60/fz-b1a-1.*`. Verdict pending paired comparison/visuals.

- Run `fz-b1a-2` @ `f9e78696bf`: 43.778 FPS, 44.26–84.05 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'EDEN_NX2_PERF_ON': 'frames_ahead,fence_flush,submit_budget'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1742: mean 22.850, median 22.726, p95 24.733, p99 25.858 ms; >20 ms 99.89%. Artifacts: `oot-dumps/perf60/fz-b1a-2.*`. Verdict pending paired comparison/visuals.

- Run `fz-b1b-1` @ `f9e78696bf`: 44.993 FPS, 44.26–84.02 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'EDEN_NX2_PERF_ON': 'frames_ahead,fence_flush,submit_budget,clean_words,index_rebind,front_sleep,async_pipelines,fragment_barrier'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1789: mean 22.227, median 22.143, p95 23.534, p99 24.735 ms; >20 ms 99.72%. Artifacts: `oot-dumps/perf60/fz-b1b-1.*`. Verdict pending paired comparison/visuals.

- Run `fz-b1b-2` @ `f9e78696bf`: 44.537 FPS, 44.26–84.07 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'EDEN_NX2_PERF_ON': 'frames_ahead,fence_flush,submit_budget,clean_words,index_rebind,front_sleep,async_pipelines,fragment_barrier'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1773: mean 22.456, median 22.350, p95 23.963, p99 25.067 ms; >20 ms 100.00%. Artifacts: `oot-dumps/perf60/fz-b1b-2.*`. Verdict pending paired comparison/visuals.

- Run `fz-b2-1` @ `f9e78696bf`: 41.139 FPS, 44.25–84.01 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'EDEN_NX2_ACQUIRE_QUEUED': '0', 'EDEN_NX2_MACRO_DMA_DRAIN': '1', 'EDEN_NX2_RANGED_WAITS': '0', 'EDEN_NX2_FORWARD_READS': '0'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1636: mean 24.310, median 24.159, p95 25.720, p99 26.903 ms; >20 ms 100.00%. Artifacts: `oot-dumps/perf60/fz-b2-1.*`. Verdict pending paired comparison/visuals.

- Run `fz-b2-2` @ `f9e78696bf`: 41.956 FPS, 44.27–84.03 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'EDEN_NX2_ACQUIRE_QUEUED': '0', 'EDEN_NX2_MACRO_DMA_DRAIN': '1', 'EDEN_NX2_RANGED_WAITS': '0', 'EDEN_NX2_FORWARD_READS': '0'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1668: mean 23.840, median 23.822, p95 25.614, p99 26.575 ms; >20 ms 100.00%. Artifacts: `oot-dumps/perf60/fz-b2-2.*`. Verdict pending paired comparison/visuals.

- Run `fz-b3-1` @ `f9e78696bf`: 42.500 FPS, 44.28–84.02 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'EDEN_NX2_ACQUIRE_QUEUED': '0', 'EDEN_NX2_MACRO_DMA_DRAIN': '1', 'EDEN_NX2_RANGED_WAITS': '0', 'EDEN_NX2_FORWARD_READS': '0', 'EDEN_NX2_PERF_ON': 'frames_ahead,fence_flush,submit_budget,clean_words,index_rebind,front_sleep,async_pipelines,fragment_barrier'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1689: mean 23.526, median 23.428, p95 24.894, p99 26.490 ms; >20 ms 100.00%. Artifacts: `oot-dumps/perf60/fz-b3-1.*`. Verdict pending paired comparison/visuals.

- Run `fz-b3-2` @ `f9e78696bf`: 42.107 FPS, 44.21–85.01 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'EDEN_NX2_ACQUIRE_QUEUED': '0', 'EDEN_NX2_MACRO_DMA_DRAIN': '1', 'EDEN_NX2_RANGED_WAITS': '0', 'EDEN_NX2_FORWARD_READS': '0', 'EDEN_NX2_PERF_ON': 'frames_ahead,fence_flush,submit_budget,clean_words,index_rebind,front_sleep,async_pipelines,fragment_barrier'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1718: mean 23.754, median 23.631, p95 25.353, p99 26.520 ms; >20 ms 100.00%. Artifacts: `oot-dumps/perf60/fz-b3-2.*`. Verdict pending paired comparison/visuals.

## Frizti verdict (takeover agent, 2026-11-06)

Build: `/home/loganp/lichii-frizti` `test/frizti-v2-radv` @ `f9e78696bf` (tag `frizti-v2` + RADV depth-compare
fix `d2da498172`) plus two uncommitted local edits: measurement-only `EDEN_NX2_PRESENT_TRACE`, and a Linux
build fix (`vk_flash_watch.h` included Xlib before `nvdata.h`; Frizti v2 does **not compile on Linux** without it).
Focused tests: 306 cases / 8,282,088 assertions pass.

Harness: identical to the perf/oot-60 runs: `perf60.py`, copied owner config, 1920x1080 window, VSYNC_HZ=120,
`taskset -c 0-15`, Kokiri static view, guest present intervals 44–84 s. First run (`fz-b0-warm`) warmed
Frizti's own pipeline cache files and is excluded from the verdict.

| Config | FPS (run 1 / 2) | median ms | p95 ms | p99 ms | >20 ms |
|---|---|---|---|---|---|
| perf/oot-60 `0030b62d10` default (reference) | 42.68 / 42.41 | 23.32 / 23.42 | 24.76 / 24.96 | 25.55 / 25.80 | 100 / 100 % |
| **B0** Frizti default | **44.67 / 44.12** | 22.37 / 22.65 | 23.88 / 24.15 | 24.82 / 25.11 | 99.0 / 99.3 % |
| B1a + `PERF_ON=frames_ahead,fence_flush,submit_budget` | 43.80 / 43.78 | 22.75 / 22.73 | 24.39 / 24.73 | 25.63 / 25.86 | 99.9 / 99.9 % |
| B1b + perf/oot-60's full perf9 set (adds clean_words, index_rebind, front_sleep, async_pipelines, fragment_barrier) | 44.99 / 44.54 | 22.14 / 22.35 | 23.53 / 23.96 | 24.73 / 25.07 | 99.7 / 100 % |
| B2 = B0 + conservative env (Frizti's own sync experiments off) | 41.14 / 41.96 | 24.16 / 23.82 | 25.72 / 25.61 | 26.90 / 26.58 | 100 / 100 % |
| B3 = B1b + conservative env | 42.50 / 42.11 | 23.43 / 23.63 | 24.89 / 25.35 | 26.49 / 26.52 | 100 / 100 % |

Conservative env = `EDEN_NX2_ACQUIRE_QUEUED=0 EDEN_NX2_MACRO_DMA_DRAIN=1 EDEN_NX2_RANGED_WAITS=0 EDEN_NX2_FORWARD_READS=0`.

## Reading

- Frizti default is **~+4% (≈ +1.9 FPS)** over perf/oot-60. **Neither is close to the stable-50 goal**
  (p95 ~24 ms vs 20 ms target; ~99% of frames >20 ms).
- Frizti's gain comes from **its own default-on synchronization experiments** (queued-acquire payloads,
  ranged waits, CB read forwarding): B0 → B2 loses ~2.8 FPS. With those off, Frizti + the perf9 set
  (B3, 42.3) equals perf/oot-60 (42.5). Those experiments are the ones with open safety questions (ring
  consumption ≠ GPU completion; ranged/forwarded reads track GPU VA, not physical aliasing). That
  speed is not yet proven to be correct.
- Enabling perf9 frames-ahead / fence batching / 4096 budget on Frizti gives nothing measurable
  (B1a −0.6, B1b +0.4 FPS, both inside run-to-run spread).
- No crash, device loss or hang in 11 runs. Screenshots (`oot-dumps/perf60/fz-*.png`) all match the
  perf184 reference: grass, canopy, fog, haze and exposure are intact, with per-channel means 0.37–0.38/0.36–0.37/0.30–0.31.
  No "brown/black slab" (the artifact Frizti attributes to frames-ahead) was seen with frames-ahead on
  (B1a/B1b/B3), in this static scene only.
- Loader confirms the user's Dynamic-fps IPS is used ("Skipping Dynamic FPS by Dark (built-in): a user mod
  already patched main+0x6e6cc4") and Max Resolution (built-in) applied.

## Where the time goes (both trees, DKB frame trace, ~44 FPS)

- Split GPU **front ~19.3 ms CPU/frame (88% busy), back ~18.4 ms/frame (82%)**. These are the cap.
- Real GPU execution (Vulkan timestamps) ~9.1–9.7 ms/frame. The RX 7800 XT is **not** the limit;
  sysfs `gpu_busy_percent` ~88% is misleading.
- Guest CPU cores ~12 ms host CPU per frame each (~55%). Not the limit.
- Not a pacing cap: `sleep_requested_ms=0`, pace 0, effective 60 Hz.

Artifacts: `/home/loganp/oot-dumps/perf60/fz-*.{out,json,stats.json,png}`.

- Run `p60-gprof-1` @ `528919928d`: 41.049 FPS, 44.25–85.03 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'LD_PRELOAD': '/usr/lib/libprofiler.so', 'CPUPROFILE': '/home/loganp/oot-dumps/perf60/p60-gprof-1.prof', 'CPUPROFILESIGNAL': '41', 'CPUPROFILE_FREQUENCY': '500'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1674: mean 24.352, median 24.220, p95 25.948, p99 27.094 ms; >20 ms 100.00%. Artifacts: `oot-dumps/perf60/p60-gprof-1.*`. Verdict pending paired comparison/visuals.

- Run `p60-zprof-1` @ `528919928d`: 36.591 FPS, 44.27–84.01 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'EDEN_NX2_GPU_PROFILE': '1'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1454: mean 27.341, median 27.006, p95 29.887, p99 34.236 ms; >20 ms 100.00%. Artifacts: `oot-dumps/perf60/p60-zprof-1.*`. Verdict pending paired comparison/visuals.

- Run `p60-cmddiag-1` @ `528919928d`: 39.844 FPS, 44.27–84.04 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'EDEN_NX2_CMDPAGE_DIAG': '1'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1585: mean 25.092, median 24.948, p95 26.622, p99 28.063 ms; >20 ms 100.00%. Artifacts: `oot-dumps/perf60/p60-cmddiag-1.*`. Verdict pending paired comparison/visuals.

- Run `fz-zprof-1` @ `f9e78696bf`: 40.353 FPS, 44.25–84.02 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'EDEN_NX2_GPU_PROFILE': '1'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1605: mean 24.784, median 24.704, p95 26.068, p99 26.986 ms; >20 ms 100.00%. Artifacts: `oot-dumps/perf60/fz-zprof-1.*`. Verdict pending paired comparison/visuals.

- Run `p60-sched-1` @ `528919928d`: 40.201 FPS, 44.27–84.02 s; env `{'EDEN_NX2_VSYNC_HZ': '120'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1598: mean 24.877, median 24.773, p95 26.563, p99 28.098 ms; >20 ms 100.00%. Artifacts: `oot-dumps/perf60/p60-sched-1.*`. Verdict pending paired comparison/visuals.

- Run `fz-dmadrain-1` @ `f9e78696bf`: 41.445 FPS, 44.25–84.04 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'EDEN_NX2_MACRO_DMA_DRAIN': '1'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1649: mean 24.126, median 24.044, p95 25.802, p99 26.867 ms; >20 ms 100.00%. Artifacts: `oot-dumps/perf60/fz-dmadrain-1.*`. Verdict pending paired comparison/visuals.

- Run `fz-noacq-1` @ `f9e78696bf`: 43.836 FPS, 44.26–84.04 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'EDEN_NX2_ACQUIRE_QUEUED': '0'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1744: mean 22.813, median 22.771, p95 24.329, p99 25.485 ms; >20 ms 98.97%. Artifacts: `oot-dumps/perf60/fz-noacq-1.*`. Verdict pending paired comparison/visuals.

- Run `fz-dmadrain-2` @ `f9e78696bf`: 40.485 FPS, 44.26–84.03 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'EDEN_NX2_MACRO_DMA_DRAIN': '1'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1610: mean 24.696, median 24.500, p95 26.317, p99 27.165 ms; >20 ms 100.00%. Artifacts: `oot-dumps/perf60/fz-dmadrain-2.*`. Verdict pending paired comparison/visuals.

- Run `fz-noacq-2` @ `f9e78696bf`: 44.988 FPS, 44.26–84.02 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'EDEN_NX2_ACQUIRE_QUEUED': '0'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1789: mean 22.227, median 22.192, p95 23.909, p99 24.862 ms; >20 ms 94.75%. Artifacts: `oot-dumps/perf60/fz-noacq-2.*`. Verdict pending paired comparison/visuals.

- Run `fz-diag-1` @ `f9e78696bf`: 41.454 FPS, 44.26–84.02 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'EDEN_NX2_CMDPAGE_DIAG': '1'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1648: mean 24.125, median 24.188, p95 25.783, p99 26.550 ms; >20 ms 100.00%. Artifacts: `oot-dumps/perf60/fz-diag-1.*`. Verdict pending paired comparison/visuals.

- Run `p60-pin-iso-1` @ `528919928d`: 38.285 FPS, 44.28–84.13 s; env `{'EDEN_NX2_VSYNC_HZ': '120'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1526: mean 26.115, median 25.843, p95 28.167, p99 31.088 ms; >20 ms 100.00%. Artifacts: `oot-dumps/perf60/p60-pin-iso-1.*`. Verdict pending paired comparison/visuals.

- Run `p60-pin-iso-2` @ `528919928d`: 40.845 FPS, 44.29–84.20 s; env `{'EDEN_NX2_VSYNC_HZ': '120'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1630: mean 24.481, median 24.346, p95 26.091, p99 27.698 ms; >20 ms 100.00%. Artifacts: `oot-dumps/perf60/p60-pin-iso-2.*`. Verdict pending paired comparison/visuals.

- Run `p60-ref-1` @ `528919928d`: 39.747 FPS, 44.21–85.05 s; env `{'EDEN_NX2_VSYNC_HZ': '120'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1623: mean 25.154, median 25.018, p95 26.873, p99 28.069 ms; >20 ms 100.00%. Artifacts: `oot-dumps/perf60/p60-ref-1.*`. Verdict pending paired comparison/visuals.

- Run `p60-ref-2` @ `528919928d`: 40.297 FPS, 44.22–84.03 s; env `{'EDEN_NX2_VSYNC_HZ': '120'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1604: mean 24.819, median 24.711, p95 26.320, p99 27.207 ms; >20 ms 100.00%. Artifacts: `oot-dumps/perf60/p60-ref-2.*`. Verdict pending paired comparison/visuals.

- Run `fz-b0-3` @ `f9e78696bf`: 43.880 FPS, 44.25–84.04 s; env `{'EDEN_NX2_VSYNC_HZ': '120'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1746: mean 22.788, median 22.826, p95 24.517, p99 25.479 ms; >20 ms 99.31%. Artifacts: `oot-dumps/perf60/fz-b0-3.*`. Verdict pending paired comparison/visuals.

- Run `p60-ref-3` @ `528919928d`: 39.607 FPS, 44.26–84.02 s; env `{'EDEN_NX2_VSYNC_HZ': '120'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1575: mean 25.253, median 25.067, p95 26.908, p99 28.534 ms; >20 ms 100.00%. Artifacts: `oot-dumps/perf60/p60-ref-3.*`. Verdict pending paired comparison/visuals.

- Run `fz-b0-4` @ `f9e78696bf`: 42.877 FPS, 44.26–84.05 s; env `{'EDEN_NX2_VSYNC_HZ': '120'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1706: mean 23.323, median 23.167, p95 25.264, p99 26.880 ms; >20 ms 99.77%. Artifacts: `oot-dumps/perf60/fz-b0-4.*`. Verdict pending paired comparison/visuals.

- Run `p60-ref-4` @ `528919928d`: 38.627 FPS, 44.25–84.07 s; env `{'EDEN_NX2_VSYNC_HZ': '120'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1538: mean 25.886, median 25.417, p95 28.878, p99 31.631 ms; >20 ms 100.00%. Artifacts: `oot-dumps/perf60/p60-ref-4.*`. Verdict pending paired comparison/visuals.

- Run `fz-b3-3` @ `f9e78696bf`: 39.919 FPS, 44.22–84.05 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'EDEN_NX2_ACQUIRE_QUEUED': '0', 'EDEN_NX2_MACRO_DMA_DRAIN': '1', 'EDEN_NX2_RANGED_WAITS': '0', 'EDEN_NX2_FORWARD_READS': '0', 'EDEN_NX2_PERF_ON': 'frames_ahead,fence_flush,submit_budget,clean_words,index_rebind,front_sleep,async_pipelines,fragment_barrier'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1590: mean 25.061, median 24.810, p95 27.115, p99 29.337 ms; >20 ms 100.00%. Artifacts: `oot-dumps/perf60/fz-b3-3.*`. Verdict pending paired comparison/visuals.

- Run `fz-b2-3` @ `f9e78696bf`: 38.369 FPS, 44.24–84.07 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'EDEN_NX2_ACQUIRE_QUEUED': '0', 'EDEN_NX2_MACRO_DMA_DRAIN': '1', 'EDEN_NX2_RANGED_WAITS': '0', 'EDEN_NX2_FORWARD_READS': '0'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1528: mean 26.060, median 25.763, p95 28.281, p99 30.605 ms; >20 ms 100.00%. Artifacts: `oot-dumps/perf60/fz-b2-3.*`. Verdict pending paired comparison/visuals.

- Run `p60-ref-5` @ `528919928d`: 41.194 FPS, 44.20–84.98 s; env `{'EDEN_NX2_VSYNC_HZ': '120'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1680: mean 24.281, median 24.045, p95 26.549, p99 28.628 ms; >20 ms 100.00%. Artifacts: `oot-dumps/perf60/p60-ref-5.*`. Verdict pending paired comparison/visuals.

- Run `fz-b3-4` @ `f9e78696bf`: 43.424 FPS, 44.26–84.01 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'EDEN_NX2_ACQUIRE_QUEUED': '0', 'EDEN_NX2_MACRO_DMA_DRAIN': '1', 'EDEN_NX2_RANGED_WAITS': '0', 'EDEN_NX2_FORWARD_READS': '0', 'EDEN_NX2_PERF_ON': 'frames_ahead,fence_flush,submit_budget,clean_words,index_rebind,front_sleep,async_pipelines,fragment_barrier'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1726: mean 23.034, median 22.816, p95 24.999, p99 27.560 ms; >20 ms 100.00%. Artifacts: `oot-dumps/perf60/fz-b3-4.*`. Verdict pending paired comparison/visuals.

- Run `fz-b2-4` @ `f9e78696bf`: 43.823 FPS, 44.27–85.02 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'EDEN_NX2_ACQUIRE_QUEUED': '0', 'EDEN_NX2_MACRO_DMA_DRAIN': '1', 'EDEN_NX2_RANGED_WAITS': '0', 'EDEN_NX2_FORWARD_READS': '0'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1786: mean 22.822, median 22.722, p95 24.636, p99 25.695 ms; >20 ms 98.10%. Artifacts: `oot-dumps/perf60/fz-b2-4.*`. Verdict pending paired comparison/visuals.

- Run `p60-ref-6` @ `528919928d`: 41.037 FPS, 44.26–85.03 s; env `{'EDEN_NX2_VSYNC_HZ': '120'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1673: mean 24.369, median 24.128, p95 26.648, p99 28.421 ms; >20 ms 100.00%. Artifacts: `oot-dumps/perf60/p60-ref-6.*`. Verdict pending paired comparison/visuals.

## Takeover agent: bottleneck and Frizti attribution (2026-11-06)

- **Environment noise**: the desktop app (electron, ~50% of a CPU, one process at nice -8) runs
  during benchmarks; single runs of one binary now spread 38.4–43.8 FPS. Same-session interleaved
  means only. perf/oot-60 `0030b62d10` measured 42.5 earlier but **40.1 mean (6 runs)** this
  session. Frozen copy: `oot-dumps/perf60-0030b62d10-cli`.
- Same-session interleaved: Frizti default 43.88/42.88 vs perf/oot-60 39.61/38.63; Frizti
  conservative+perf9 (B3) 39.92/43.42, conservative (B2) 38.37/43.82 vs perf/oot-60 41.19/41.04.
  Frizti's safe pieces are inside the noise; the gain is its MME DMA ranged waits.
- Attribution on Frizti: `EDEN_NX2_MACRO_DMA_DRAIN=1` alone 41.45/40.48 (gain gone);
  `EDEN_NX2_ACQUIRE_QUEUED=0` alone 43.84/44.99 (queued acquires worth nothing).
- **Frizti verify mode fails**: `EDEN_NX2_CMDPAGE_DIAG=1` (each ranged-wait MME read repeated after
  a full drain): 7 reads of ~3.5M differ (2 words), all at query report addresses. Frizti also skips
  forwarded reads under diag, so forwarding was never verified.
- Why: query report results are written by `GPUFencingThread` (or a later fence release) when the
  host GPU completes, outside the split's command order. A full drain only adds latency; it does
  not order them. Mismatches there are timing, present in both modes.
- **Bottleneck (perf/oot-60, GPU_PROFILE, gdb stacks, schedstat)**: real GPU time ~9.7 ms/frame
  (not GPU bound; sysfs busy% misleads). Guest cores ~55%. Run-queue wait ~6 ms/s (no CPU
  contention). Back: ~13% idle waiting for the front, ~8% blocked in read-trap `Scheduler::Finish`
  (65/s, 81 ms/s), rest work (method 219, bind stage buffers 137, other 148, update buffers 49,
  draw 64, compute 43 ms/s). Front: 715 ms/s waiting on the back: macro DMA read drains
  39,945/s (all macro 0xe2c, 6 words), acquires 193 ms/s, list writers 189 ms/s.
- perf/oot-60 diag: 44k macro DMA reads/s, 0 on GPU-written pages, ~79/s changed by the drain.
- Rejected: placement pinning (back/front/VK worker alone on physical cores, rest 6-15):
  38.29/40.85 vs ~40 reference, no gain.

- Run `rg-diag-1` @ `528919928d`: 41.823 FPS, 44.27–84.01 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'EDEN_NX2_CMDPAGE_DIAG': '1'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1662: mean 23.919, median 23.808, p95 25.349, p99 26.397 ms; >20 ms 100.00%. Artifacts: `oot-dumps/perf60/rg-diag-1.*`. Verdict pending paired comparison/visuals.

- Run `rg-def-1` @ `528919928d`: 44.924 FPS, 44.26–84.02 s; env `{'EDEN_NX2_VSYNC_HZ': '120'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1786: mean 22.257, median 21.982, p95 24.349, p99 27.074 ms; >20 ms 99.78%. Artifacts: `oot-dumps/perf60/rg-def-1.*`. Verdict pending paired comparison/visuals.

- Run `rg-drain-1` @ `528919928d`: 45.133 FPS, 44.29–84.01 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'EDEN_NX2_MACRO_DMA_DRAIN': '1'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1793: mean 22.163, median 21.837, p95 24.286, p99 26.239 ms; >20 ms 98.10%. Artifacts: `oot-dumps/perf60/rg-drain-1.*`. Verdict pending paired comparison/visuals.

- Run `p60-ref-7` @ `528919928d`: 40.171 FPS, 44.19–85.01 s; env `{'EDEN_NX2_VSYNC_HZ': '120'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1640: mean 24.893, median 24.673, p95 27.252, p99 30.697 ms; >20 ms 100.00%. Artifacts: `oot-dumps/perf60/p60-ref-7.*`. Verdict pending paired comparison/visuals.

- Run `rg-def-2` @ `528919928d`: 42.078 FPS, 44.25–84.11 s; env `{'EDEN_NX2_VSYNC_HZ': '120'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1677: mean 23.770, median 23.484, p95 25.911, p99 28.409 ms; >20 ms 100.00%. Artifacts: `oot-dumps/perf60/rg-def-2.*`. Verdict pending paired comparison/visuals.

- Run `rg-drain-2` @ `528919928d`: 40.306 FPS, 44.26–84.05 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'EDEN_NX2_MACRO_DMA_DRAIN': '1'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1604: mean 24.801, median 24.680, p95 26.346, p99 27.354 ms; >20 ms 100.00%. Artifacts: `oot-dumps/perf60/rg-drain-2.*`. Verdict pending paired comparison/visuals.

- Run `p60-ref-8` @ `528919928d`: 39.425 FPS, 44.22–84.01 s; env `{'EDEN_NX2_VSYNC_HZ': '120'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1569: mean 25.360, median 25.087, p95 27.798, p99 30.172 ms; >20 ms 100.00%. Artifacts: `oot-dumps/perf60/p60-ref-8.*`. Verdict pending paired comparison/visuals.

- Run `ab-drain-1` @ `528919928d`: 41.517 FPS, 44.24–84.01 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'EDEN_NX2_AB': 'macro_drain', 'EDEN_NX2_AB_PERIOD_S': '4'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1651: mean 24.084, median 23.885, p95 26.418, p99 28.324 ms; >20 ms 100.00%. Artifacts: `oot-dumps/perf60/ab-drain-1.*`. Verdict pending paired comparison/visuals.

- Run `ab-drain-2` @ `528919928d`: 40.493 FPS, 44.23–84.07 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'EDEN_NX2_AB': 'macro_drain', 'EDEN_NX2_AB_PERIOD_S': '4'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1613: mean 24.702, median 24.519, p95 27.470, p99 29.795 ms; >20 ms 100.00%. Artifacts: `oot-dumps/perf60/ab-drain-2.*`. Verdict pending paired comparison/visuals.

- Run `rg2-diag-1` @ `528919928d`: 47.071 FPS, 44.25–85.04 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'EDEN_NX2_CMDPAGE_DIAG': '1'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1920: mean 21.242, median 21.224, p95 22.524, p99 23.288 ms; >20 ms 95.94%. Artifacts: `oot-dumps/perf60/rg2-diag-1.*`. Verdict pending paired comparison/visuals.

- Run `ab-drain-3` @ `528919928d`: 41.049 FPS, 44.28–84.08 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'EDEN_NX2_AB': 'macro_drain', 'EDEN_NX2_AB_PERIOD_S': '4'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1634: mean 24.361, median 24.140, p95 27.291, p99 30.680 ms; >20 ms 100.00%. Artifacts: `oot-dumps/perf60/ab-drain-3.*`. Verdict pending paired comparison/visuals.

- Run `ab-drain-4` @ `528919928d`: 43.560 FPS, 44.24–85.02 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'EDEN_NX2_AB': 'macro_drain', 'EDEN_NX2_AB_PERIOD_S': '4'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1776: mean 22.956, median 22.737, p95 25.858, p99 28.184 ms; >20 ms 99.32%. Artifacts: `oot-dumps/perf60/ab-drain-4.*`. Verdict pending paired comparison/visuals.

| # | Idea | Before -> after | Verdict | Notes |
|---|---|---|---|---|
| 9 | Ranged MME DMA read waits (port Frizti 25b1a3a6b0, e9a3d2b403, 22aad455dc onto perf/oot-60, plus deferred guest-memory writers marked and a verify mode that separates async report writes) | within-run A/B vs `EDEN_NX2_MACRO_DMA_DRAIN=1`, 4 runs: 39.65 -> 42.34 and 40.67 -> 44.03 FPS; p95 27.42 -> 25.90, 27.36 -> 25.14 ms | **kept** (`perf/oot-60-ranged` 084c126c31, ad202483af, 83c70d6d39, b67bf6a4a0) | verify: 2.70M reads, 0 differences, 0 on GPU-written pages, 9 changed by fence-thread report writes (not ordered by a full drain either) |
| 10 | Frizti forwarding of queued CB uploads (1c9a048e9d) | 0 forwarded reads in this scene (also 0 in Frizti) | dropped | inert |

- **Method change**: within-process A/B. Measurement-only `EDEN_NX2_AB=<switch> EDEN_NX2_AB_PERIOD_S=4`
  flips the switch every 4 s, and the present trace tags each interval with its phase
  (`perfscripts/measure-ab.patch`, `perfscripts/ab60.py`). Both arms see the same host noise;
  cross-run comparisons in this session are unreliable (+-3 FPS).

- Run `rg-zprof-1` @ `528919928d`: 42.726 FPS, 44.30–84.01 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'EDEN_NX2_GPU_PROFILE': '1'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1697: mean 23.414, median 23.111, p95 25.360, p99 25.998 ms; >20 ms 100.00%. Artifacts: `oot-dumps/perf60/rg-zprof-1.*`. Verdict pending paired comparison/visuals.

## STOPPED on owner request (takeover agent, 2026-11-06 ~02:50): exact state

- **Best correct candidate**: `/home/loganp/lichii-perf60` branch `perf/oot-60-ranged` @ `b67bf6a4a0`
  (= perf/oot-60 `0030b62d10` + 084c126c31, ad202483af, 83c70d6d39, b67bf6a4a0). Clean tree.
  Measurement-only code (present trace + `EDEN_NX2_AB` hook + `src/common/nx2_ab.h`) is in
  `git stash@{0}` "MEASUREMENT ONLY (do not ship)" and in `oot-dumps/perfscripts/measure-ab.patch`.
- **The binary in `lichii-perf60/build/bin` is NOT b67bf6a4a0**: its last build was a temporary
  instrumentation build (forwarding + TEMP counters, `/tmp/opencode/temp-forward-instr.patch`).
  Rebuild b67bf6a4a0 before delivery. The frozen pre-change reference is `oot-dumps/perf60-0030b62d10-cli`.
- Frizti worktree `test/frizti-v2-radv` @ `433a1746bb` (WIP Linux build fix committed); its
  `nx2_gsv.h` present trace is uncommitted (measurement only).
- Finding at stop: Frizti's ranged list holds at most 256 pending ranges. In OoT ~8,100 ranged
  writes/s and ~8,000/s **fold** into the unranged mark (queue always full), so most CB uploads still
  make reads wait (front: 21k macro-read waits/s, 15k "cbuf upload"). **Next step**: replace the
  deque with a direct-mapped table keyed by 16-byte bucket (newest position per bucket; evicting a
  still-pending entry folds it into the unranged mark), and re-verify with CMDPAGE_DIAG (must stay
  0 unexplained). Forwarding of queued CB data was 0/s (the waits are folds, not data misses) and was dropped.
- Display caveat: the owner's remote session (sunshine) changes the X screen (1920x1080, then
  2560x1440). The harness now has `FULLSCREEN=1`, valid only when the screen is exactly 1920x1080;
  the windowed 1920x1080 resize needs a larger screen. Runs `fwd-why-*` are diagnostic only.
- Remaining levers (evidence above): read-trap `Scheduler::Finish` on the back (65-76/s, 81-102
  ms/s); acquire (6.2k/s, 236 ms/s) and list-writer (7.9k/s, 206 ms/s) front waits; back
  bind/method costs. Not started.

- Run `wt-diag-1` @ `72a8e1b1b4`: 42.855 FPS, 44.24–84.96 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'EDEN_NX2_CMDPAGE_DIAG': '1'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1745: mean 23.343, median 23.208, p95 24.705, p99 25.423 ms; >20 ms 100.00%. Artifacts: `oot-dumps/perf60/wt-diag-1.*`. Verdict pending paired comparison/visuals.

## Writer-table continuation (2026-11-06, GPT-6.1 Sol)

- Worktree `/home/loganp/lichii-perf60`, branch `perf/oot-60-writer-table`; owner GUI worktree/build untouched.
  Candidate checkpoint **72a8e1b1b4**, performance acceptance pending (do not deliver yet).
  Fixed 16,384-entry / 16-byte bucket table (~384 KiB), front-only insertion/query/retirement,
  acquired back executed position. Pending collision evictions fold to a separate global mark;
  writes >256 buckets and reads >1024 buckets use bounded conservative fallbacks. New tests cover
  exact boundaries, multi-bucket writes, eviction/retirement, repeated addresses, ring/sequence wrap,
  randomized exact-range oracle, and actual back execution vs publication.
- First focused gate: **323 cases / 8,487,716 assertions passed**. Additional native-macro/DMA gate:
  **11 cases / 209,070 assertions passed** (overlapping writer tests). Two more assertions groups
  subsequently added; final gate will supersede these counts.
- `wt-diag-1`: **2,061,932 table target checks; zero missed exact pending writers; zero oracle
  overflow. 2,061,932 repeated DMA reads; zero unexplained data differences, zero GPU-written-page
  reads, eight async-report changes** separated by the existing diagnostic. Screenshot retains
  grass, canopy, fog, haze, and exposure. This diagnostic-only run is not a performance claim.
- Updated copied-run harness to use the current session XAUTHORITY rather than the stale prior
  session file. Host hardware unchanged; desktop is now Xwayland, screen **3440x1440**. Each run
  checks `xrandr`, then verifies its own window is **1920x1080**, windowed (not fullscreen).
- Measurement-only patch (not committed): `oot-dumps/perfscripts/writer-table-measure.patch`.
  `EDEN_NX2_AB=writer_table`: **A = exact b67bf6a4a0 legacy list behavior, B = writer table**.
  Both trackers receive every writer and retire at each read; only the actual wait target flips.
  Per-phase writes/folds/read-wait counters logged without enabling the heavier GPU profiler.
- Refetched Eglantine: `upstream/nx2` remains ancestral at `0ca2032bff`. Newly visible unmerged
  `oot-optimization-fixes` has **dc5938b8bb (Frizti 99b3b49595)**, narrowing list-writer marks to
  methods that may write memory; read its full diff and native plain-method dispatch. It does not
  implement a range table. Candidate for Task 2 if the measured table remains below target.
  PR652/670 diffs re-reviewed, cached at `/tmp/opencode/upstream-{652-writer,670}-review.diff`.

- Run `wt-ab-1` @ `72a8e1b1b4`: 45.793 FPS, 44.33–84.03 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'EDEN_NX2_AB': 'writer_table', 'EDEN_NX2_AB_PERIOD_S': '4'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1818: mean 21.847, median 21.617, p95 23.571, p99 24.193 ms; >20 ms 98.68%. Artifacts: `oot-dumps/perf60/wt-ab-1.*`. Verdict pending paired comparison/visuals.

- Run `wt-ab-2` @ `72a8e1b1b4`: 46.406 FPS, 44.32–84.03 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'EDEN_NX2_AB': 'writer_table', 'EDEN_NX2_AB_PERIOD_S': '4'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1843: mean 21.550, median 21.412, p95 22.919, p99 23.484 ms; >20 ms 97.56%. Artifacts: `oot-dumps/perf60/wt-ab-2.*`. Verdict pending paired comparison/visuals.

- Run `wt-ab-3` @ `72a8e1b1b4`: 46.500 FPS, 44.33–84.05 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'EDEN_NX2_AB': 'writer_table', 'EDEN_NX2_AB_PERIOD_S': '4'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1847: mean 21.500, median 21.272, p95 23.113, p99 23.834 ms; >20 ms 98.32%. Artifacts: `oot-dumps/perf60/wt-ab-3.*`. Verdict pending paired comparison/visuals.

- Run `wt-ab-4` @ `72a8e1b1b4`: 46.704 FPS, 44.32–84.04 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'EDEN_NX2_AB': 'writer_table', 'EDEN_NX2_AB_PERIOD_S': '4'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1855: mean 21.415, median 21.272, p95 22.671, p99 23.263 ms; >20 ms 98.54%. Artifacts: `oot-dumps/perf60/wt-ab-4.*`. Verdict pending paired comparison/visuals.

- Run `wt-profile-1` @ `72a8e1b1b4`: 41.929 FPS, 44.32–84.01 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'EDEN_NX2_GPU_PROFILE': '1'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1664: mean 23.845, median 23.623, p95 25.620, p99 26.285 ms; >20 ms 100.00%. Artifacts: `oot-dumps/perf60/wt-profile-1.*`. Verdict pending paired comparison/visuals.

### Initial writer-table measurement (not final acceptance)

| Run | A legacy FPS | B table FPS | A/B median ms | A/B p95 ms | A/B p99 ms |
|---|---:|---:|---|---|---|
| wt-ab-1 | 45.36 | 46.02 | 21.74 / 21.57 | 23.73 / 23.54 | 24.69 / 24.10 |
| wt-ab-2 | 46.23 | 46.63 | 21.40 / 21.40 | 22.96 / 22.84 | 23.48 / 23.41 |
| wt-ab-3 | 46.59 | 46.44 | 21.27 / 21.26 | 22.99 / 23.16 | 23.83 / 23.83 |
| wt-ab-4 | 46.50 | 46.87 | 21.29 / 21.26 | 22.71 / 22.65 | 23.26 / 23.28 |
| pooled | **46.17** | **46.49** | **21.41 / 21.37** | **23.24 / 23.15** | **24.00 / 23.79** |

- Only +0.7% pooled, one of four pairs negative: not enough to accept a new structure for FPS
  alone yet. Pending-writer decisions (not actual waits: the back may catch up between selection
  and WaitFor's acquired load): **4279 / 1182 per second**; folds **343823 / 157 per second**.
  Fold counts here are per `RangeLastWrite` call, ~1.1 million calls/s, not the prior agent's
  coalesced-range counter. Fold fractions **31.41% / 0.0142%**. Never compare the raw fold count
  directly against that earlier ~8k/s count without reconciling counter definitions.
- Instrumentation audit found two confounders to fix before the next pool: per-upload AB clock
  reads at ~1M/s, and scanning the legacy list in *both* arms. These conservatively hide part of
  the lookup/CPU improvement. New counter version will measure **actual WaitFor returns**.
- Production table insertion also unnecessarily acquired `consumed` on each upload; a front-owned
  previously acquired `consumed_cache` is safe (staleness can only retain/fold extra dependencies).
  Removing that cache-line traffic before final qualification, keeping exact retirement ordering.
- `wt-profile-1`: table's **actual macro waits ~1100/s, 113–117 ms/s**; cbuf-only waits ~40/s;
  list-writer waits **7.6–7.8k/s, 223–237 ms/s**, acquire **6.0–6.2k/s, 263–271 ms/s**.
  Back method **253–257 ms/s**, bind-stage **132–134 ms/s**, other **142–147 ms/s**.
  Narrower upstream list-writer classification remains a justified next experiment.
- Current owner save has advanced (four hearts/slingshot), and the stationary camera differs from
  older ab-drain/rg-def screenshots. New A/B arms share the same current save/view. All viewed
  screenshots preserve grass/canopy/fog/haze/exposure, but these are **not** pixel-aligned comparisons
  to the old save. No resolution/effect changes were made. No owner files/configs/save were edited.

### Revised table checkpoint and active batch

- Candidate amended to **04ed31dce8**: insertion now uses the front's previously acquired executed
  position; all ten writer-table/execution tests included. Final focused union (including actual
  `[native_macro]` and `[dma_pusher]` tags) **326 cases / 8,498,786 assertions passed**.
  Production commit still awaits a meaningful final performance win before delivery approval.
- Measurement-only **writer-table-measure-v2.patch** removes per-upload clock reads, scans the
  legacy range list only in baseline arm A (but retires it at every read in both arms), and counts
  actual WaitFor returns rather than pending-at-selection decisions. Never commit this patch.
- Active serial shell batch (survived user interruption): build CLI, then `wt2-ab-1..4`,
  `wt2-diag-1`, `wt2-plain-1..2` via `writer-batch.py`. Nice19/six jobs; each run checks owner
  process names and screen geometry. Do not launch another emulator or rebuild the measurement
  binary during this batch. Wait for the registered background completion notification.
- Source status during batch: production at 04ed31dce8; **only measurement instrumentation is
  uncommitted**. Existing stash@{0} untouched. Reverse `writer-table-measure-v2.patch`, remove the
  two intent-to-add headers from the index, and rebuild before any eventual delivery.

- Run `wt2-ab-1` @ `72a8e1b1b4`: 46.021 FPS, 44.32–84.02 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'EDEN_NX2_AB': 'writer_table', 'EDEN_NX2_AB_PERIOD_S': '4'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1827: mean 21.725, median 21.529, p95 23.281, p99 23.968 ms; >20 ms 99.51%. Artifacts: `oot-dumps/perf60/wt2-ab-1.*`. Verdict pending paired comparison/visuals.

- Run `wt2-ab-2` @ `72a8e1b1b4`: 46.470 FPS, 44.32–84.05 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'EDEN_NX2_AB': 'writer_table', 'EDEN_NX2_AB_PERIOD_S': '4'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1846: mean 21.517, median 21.286, p95 23.156, p99 23.835 ms; >20 ms 98.21%. Artifacts: `oot-dumps/perf60/wt2-ab-2.*`. Verdict pending paired comparison/visuals.

- Run `wt2-ab-3` @ `72a8e1b1b4`: 46.621 FPS, 44.32–84.02 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'EDEN_NX2_AB': 'writer_table', 'EDEN_NX2_AB_PERIOD_S': '4'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1851: mean 21.459, median 21.297, p95 22.778, p99 23.385 ms; >20 ms 98.65%. Artifacts: `oot-dumps/perf60/wt2-ab-3.*`. Verdict pending paired comparison/visuals.

- Run `wt2-ab-4` @ `72a8e1b1b4`: 45.950 FPS, 44.32–84.01 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'EDEN_NX2_AB': 'writer_table', 'EDEN_NX2_AB_PERIOD_S': '4'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1824: mean 21.765, median 21.526, p95 23.626, p99 24.205 ms; >20 ms 98.36%. Artifacts: `oot-dumps/perf60/wt2-ab-4.*`. Verdict pending paired comparison/visuals.

- Run `wt2-diag-1` @ `72a8e1b1b4`: 42.286 FPS, 44.32–84.01 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'EDEN_NX2_CMDPAGE_DIAG': '1'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1678: mean 23.642, median 23.435, p95 25.206, p99 25.895 ms; >20 ms 100.00%. Artifacts: `oot-dumps/perf60/wt2-diag-1.*`. Verdict pending paired comparison/visuals.

- Run `wt2-plain-1` @ `72a8e1b1b4`: 46.586 FPS, 44.32–85.04 s; env `{'EDEN_NX2_VSYNC_HZ': '120'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1897: mean 21.463, median 21.325, p95 22.875, p99 23.411 ms; >20 ms 97.58%. Artifacts: `oot-dumps/perf60/wt2-plain-1.*`. Verdict pending paired comparison/visuals.

- Run `wt2-plain-2` @ `72a8e1b1b4`: 45.992 FPS, 44.31–84.04 s; env `{'EDEN_NX2_VSYNC_HZ': '120'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1827: mean 21.741, median 21.492, p95 23.304, p99 23.841 ms; >20 ms 99.45%. Artifacts: `oot-dumps/perf60/wt2-plain-2.*`. Verdict pending paired comparison/visuals.

### Writer table final verdict: REJECTED (04ed31dce8)

| Revised run | A legacy FPS | B table FPS | A/B median ms | A/B p95 ms | A/B p99 ms |
|---|---:|---:|---|---|---|
| wt2-ab-1 | 46.16 | 45.90 | 21.55 / 21.51 | 23.27 / 23.24 | 23.86 / 24.06 |
| wt2-ab-2 | 46.59 | 46.37 | 21.29 / 21.28 | 23.17 / 23.06 | 23.84 / 23.79 |
| wt2-ab-3 | 46.72 | 46.49 | 21.29 / 21.31 | 22.78 / 22.78 | 23.34 / 23.43 |
| wt2-ab-4 | 46.11 | 45.78 | 21.52 / 21.53 | 23.66 / 23.60 | 24.22 / 24.11 |
| pooled | **46.40** | **46.13** | **21.40 / 21.41** | **23.25 / 23.25** | **23.96 / 24.02** |

- **No reproducible performance gain** across both pools: revised table loses ~0.6%, all four
  pairs negative. Do not ship complexity that does not improve the target. Retained only on
  experimental branch `perf/oot-60-writer-table` @ **04ed31dce8**, not in the next candidate.
- Actual `WaitFor` returns/s by A/B class: **8775.65 / 1211.02** (macro-read site). Fold
  events/s: **391631.12 / 173.22**, fold fraction **35.5658% / 0.0158%** of ~1.10M insert
  calls/s. This dramatically reduces dependency counts, not the limiting end-to-end throughput.
- Revised diag **2,053,752 reads/checks**, **0 unexplained data differences**, **0 missed pending
  writers**, **0 oracle overflow**, **0 GPU-written-page reads**, **2 async-report changes**.
  Plain table runs **46.586 / 45.992 FPS**, p95 **22.875 / 23.304 ms**: neither qualifies.
- Viewed `wt2-plain-2.png`, `wt2-ab-3.png`, `wt-ab-1.png`, `wt-diag-1.png` against prior
  ab-drain/rg-def references. Current save/camera differs, but grass/canopy/fog/haze/exposure intact.
- Version-string caveat: wt2 binary was built from **04ed31dce8 + v2 instrumentation**, but CMake
  retained 72a8e1b1b4 in the revision string. Source/build chronology and recorded v2 patch identify
  it; do not attribute the revised runs to the old source based on the stale displayed hash.
- Instrumentation reversed; intent-to-add headers removed. Switched the clean worktree to new
  branch **perf/oot-60-method-writers**, based on **b67bf6a4a0** (no writer table). Proceeding to
  Task 2, narrower actual-write list markers from Eglantine **dc5938b8bb / Frizti 99b3b49595**.
  Also reviewed zortos **b624390a7b**: batches draw-written list-page downloads, not the same
  classification change. Current profile's list GPU wait is only ~43 ms/s; do classification first.

- Run `mw-diag-1` @ `5f0ff815d4`: 44.094 FPS, 44.32–84.01 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'EDEN_NX2_CMDPAGE_DIAG': '1'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1750: mean 22.679, median 22.537, p95 23.823, p99 24.603 ms; >20 ms 100.00%. Artifacts: `oot-dumps/perf60/mw-diag-1.*`. Verdict pending paired comparison/visuals.

- Run `mw-ab-1` @ `5f0ff815d4`: 47.722 FPS, 44.32–84.03 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'EDEN_NX2_AB': 'method_writers', 'EDEN_NX2_AB_PERIOD_S': '4'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1895: mean 20.955, median 20.867, p95 22.086, p99 22.666 ms; >20 ms 88.18%. Artifacts: `oot-dumps/perf60/mw-ab-1.*`. Verdict pending paired comparison/visuals.

- Run `mw-ab-2` @ `5f0ff815d4`: 47.498 FPS, 44.32–84.00 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'EDEN_NX2_AB': 'method_writers', 'EDEN_NX2_AB_PERIOD_S': '4'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1885: mean 21.057, median 20.927, p95 22.139, p99 22.930 ms; >20 ms 89.76%. Artifacts: `oot-dumps/perf60/mw-ab-2.*`. Verdict pending paired comparison/visuals.

- Run `mw-ab-3` @ `5f0ff815d4`: 47.951 FPS, 44.32–84.02 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'EDEN_NX2_AB': 'method_writers', 'EDEN_NX2_AB_PERIOD_S': '4'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1904: mean 20.854, median 20.758, p95 22.057, p99 22.609 ms; >20 ms 83.51%. Artifacts: `oot-dumps/perf60/mw-ab-3.*`. Verdict pending paired comparison/visuals.

- Run `mw-ab-4` @ `5f0ff815d4`: 47.451 FPS, 44.32–84.05 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'EDEN_NX2_AB': 'method_writers', 'EDEN_NX2_AB_PERIOD_S': '4'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1885: mean 21.077, median 20.993, p95 22.202, p99 22.852 ms; >20 ms 92.04%. Artifacts: `oot-dumps/perf60/mw-ab-4.*`. Verdict pending paired comparison/visuals.

- Run `mw-plain-1` @ `5f0ff815d4`: 48.098 FPS, 44.32–84.01 s; env `{'EDEN_NX2_VSYNC_HZ': '120'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1909: mean 20.783, median 20.736, p95 21.867, p99 22.451 ms; >20 ms 82.50%. Artifacts: `oot-dumps/perf60/mw-plain-1.*`. Verdict pending paired comparison/visuals.

- Run `mw-plain-2` @ `5f0ff815d4`: 47.621 FPS, 44.32–84.03 s; env `{'EDEN_NX2_VSYNC_HZ': '120'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1891: mean 20.998, median 20.851, p95 22.154, p99 23.117 ms; >20 ms 88.31%. Artifacts: `oot-dumps/perf60/mw-plain-2.*`. Verdict pending paired comparison/visuals.

- Run `mw-profile-1` @ `5f0ff815d4`: 44.184 FPS, 44.32–84.02 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'EDEN_NX2_GPU_PROFILE': '1'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1754: mean 22.632, median 22.426, p95 24.088, p99 24.684 ms; >20 ms 100.00%. Artifacts: `oot-dumps/perf60/mw-profile-1.*`. Verdict pending paired comparison/visuals.

- Run `back-gprof-1` @ `b67bf6a4a0`: 47.352 FPS, 44.31–84.03 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'LD_PRELOAD': '/usr/lib/libprofiler.so', 'CPUPROFILE': '/home/loganp/oot-dumps/perf60/back-gprof-1.prof', 'CPUPROFILESIGNAL': '41', 'CPUPROFILE_FREQUENCY': '500'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1881: mean 21.113, median 20.979, p95 22.356, p99 23.286 ms; >20 ms 88.46%. Artifacts: `oot-dumps/perf60/back-gprof-1.*`. Verdict pending paired comparison/visuals.

### Task 2: narrower list-writer classification REJECTED (5f0ff815d4)

Ported upstream dc5938b8bb / Frizti 99b3b49595, plus native/legacy-mode classification tests.
Clean CLI/tests build: **323 cases / 8,297,134 assertions passed**. No LIST_WAIT_SKIP/LIST_FAST,
queued-acquire payload, or completion shortcut enabled.

| Run | A old FPS | B narrowed FPS | A/B median ms | A/B p95 ms | A/B p99 ms |
|---|---:|---:|---|---|---|
| mw-ab-1 | 47.55 | 47.80 | 20.91 / 20.85 | 22.21 / 22.05 | 22.82 / 22.69 |
| mw-ab-2 | 47.59 | 47.41 | 20.92 / 20.94 | 22.08 / 22.19 | 22.83 / 22.93 |
| mw-ab-3 | 48.10 | 47.81 | 20.81 / 20.72 | 21.99 / 22.10 | 22.55 / 22.69 |
| mw-ab-4 | 47.58 | 47.31 | 21.00 / 20.99 | 22.19 / 22.25 | 22.67 / 22.97 |
| pooled | **47.71** | **47.58** | **20.91 / 20.88** | **22.13 / 22.14** | **22.75 / 22.87** |

- Actual WaitFor/Drain returns per second, A/B: list **8859.63 / 8444.83**, macro
  **8604.67 / 8399.94**, acquire **6980.27 / 7002.11**. No reproducible FPS gain; do not keep.
  Plain narrowed runs **48.098 / 47.621 FPS**, p95 **21.867 / 22.154 ms**: not the goal.
- Diag **2,133,647 DMA reads; 0 unexplained differences; 0 GPU-written-page reads; 4 async-report
  changes; 0 missed draw writes on entry or elsewhere**. 22 list words changed by the correct
  existing flush path (not missed writes). `mw-plain-2.png` preserves foliage/fog/haze/exposure.
- Candidate remains only on rejected branch `perf/oot-60-method-writers` @ **5f0ff815d4**.
  Measurement-only patch `perfscripts/method-writers-measure.patch` reversed and intent headers
  removed. Next branch `perf/oot-60-back-costs` starts again at b67bf6a4a0, no rejected changes.
- New **non-GPU_PROFILE** CPU sample (`back-gprof-1.prof.0`, 50–80 s, 500 Hz; owned child only)
  points to back draw configuration **33.6%**, graphics uniform binding **13.8%**, mprotect **3.9%**,
  UpdatePagesCachedBatch **4.4%**. Native-bank MarkRegionAsCpuModified itself **1.4%** of back;
  its downstream page protect/reprotect adds cost. Front still mostly waits. Samples are useful
  for attribution, not exact CPU utilization (gperftools sampling/root classification bias).
- Reviewed full Frizti **26ef19bd9d / dbcbe547b3** diff: native bank refresh uses the existing
  UploadMemory path directly instead of manufacturing CPU-dirty notifications. It preserves the
  same CPU-only guard, mapped prefix, and refresh cadence. This addresses sampled protection churn
  without skipping required refreshes; next isolated experiment. No safe evidence-backed acquire
  shortcut found, so do not equate queued release payloads or ring consumption with completion.

- Run `nb-diag-1` @ `4fa0d36744`: 46.025 FPS, 44.31–84.00 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'EDEN_NX2_CMDPAGE_DIAG': '1'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1827: mean 21.724, median 21.674, p95 22.787, p99 23.383 ms; >20 ms 98.85%. Artifacts: `oot-dumps/perf60/nb-diag-1.*`. Verdict pending paired comparison/visuals.

- Run `nb-ab-1` @ `4fa0d36744`: 48.487 FPS, 44.32–84.02 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'EDEN_NX2_AB': 'native_bank', 'EDEN_NX2_AB_PERIOD_S': '4'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1925: mean 20.625, median 20.554, p95 21.860, p99 22.397 ms; >20 ms 74.75%. Artifacts: `oot-dumps/perf60/nb-ab-1.*`. Verdict pending paired comparison/visuals.

- Run `nb-ab-2` @ `4fa0d36744`: 48.853 FPS, 44.32–84.03 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'EDEN_NX2_AB': 'native_bank', 'EDEN_NX2_AB_PERIOD_S': '4'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1940: mean 20.472, median 20.368, p95 21.749, p99 22.311 ms; >20 ms 69.28%. Artifacts: `oot-dumps/perf60/nb-ab-2.*`. Verdict pending paired comparison/visuals.

- Run `nb-ab-3` @ `4fa0d36744`: 48.982 FPS, 44.32–84.01 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'EDEN_NX2_AB': 'native_bank', 'EDEN_NX2_AB_PERIOD_S': '4'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1944: mean 20.414, median 20.331, p95 21.703, p99 22.220 ms; >20 ms 65.74%. Artifacts: `oot-dumps/perf60/nb-ab-3.*`. Verdict pending paired comparison/visuals.

- Run `nb-ab-4` @ `4fa0d36744`: 48.442 FPS, 44.32–84.02 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'EDEN_NX2_AB': 'native_bank', 'EDEN_NX2_AB_PERIOD_S': '4'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1923: mean 20.643, median 20.540, p95 21.986, p99 22.594 ms; >20 ms 75.25%. Artifacts: `oot-dumps/perf60/nb-ab-4.*`. Verdict pending paired comparison/visuals.

- Run `nb-plain-1` @ `4fa0d36744`: 49.346 FPS, 44.33–84.02 s; env `{'EDEN_NX2_VSYNC_HZ': '120'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1959: mean 20.260, median 20.167, p95 21.474, p99 22.405 ms; >20 ms 58.30%. Artifacts: `oot-dumps/perf60/nb-plain-1.*`. Verdict pending paired comparison/visuals.

- Run `nb-plain-2` @ `4fa0d36744`: 49.226 FPS, 44.33–84.05 s; env `{'EDEN_NX2_VSYNC_HZ': '120'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1955: mean 20.315, median 20.193, p95 21.623, p99 22.366 ms; >20 ms 60.20%. Artifacts: `oot-dumps/perf60/nb-plain-2.*`. Verdict pending paired comparison/visuals.

### Task 2 deliverable: native-bank direct refresh KEPT (5ce57b84aa)

Ported Frizti 26ef19bd9d / dbcbe547b3. No refresh skipping, lower resolution, GPU-completion
shortcut or settings change. Existing CPU-only guard, mapped-prefix bound, refresh cadence and
ordered UploadMemory/CopyBuffer path retained; only artificial CPU-dirty/protection churn removed.

| Run | A dirty-mark FPS | B direct FPS | A/B median ms | A/B p95 ms | A/B p99 ms |
|---|---:|---:|---|---|---|
| nb-ab-1 | 47.91 | 49.03 | 20.85 / 20.22 | 22.06 / 21.49 | 22.50 / 22.06 |
| nb-ab-2 | 48.12 | 49.59 | 20.69 / 20.15 | 21.92 / 21.26 | 22.54 / 22.06 |
| nb-ab-3 | 48.48 | 49.52 | 20.62 / 20.07 | 21.88 / 21.29 | 22.28 / 22.22 |
| nb-ab-4 | 47.86 | 49.05 | 20.87 / 20.19 | 22.15 / 21.66 | 22.66 / 22.46 |
| pooled | **48.09** | **49.30** | **20.77 / 20.16** | **22.01 / 21.43** | **22.52 / 22.16** |

- Four of four positive; **+2.5% FPS**, p95 reduced ~0.58 ms. Plain runs **49.346 / 49.226 FPS**,
  p95 **21.474 / 21.623 ms**, p99 **22.405 / 22.366 ms**. **Stable-50 goal NOT met.**
- Diag **2,255,657 DMA reads, 0 unexplained data differences, 0 GPU-written-page reads, 8 async
  report changes, 0 missed draw writes on entry/elsewhere**. This validates the existing command/
  macro coherence checks; it is not a full proof of every uniform binding. Ordering argument:
  use the same regular upload function that SynchronizeBuffer would invoke, for the identical
  CPU-owned mapped prefix; dirty genuine CPU ranges still synchronize normally afterwards.
- Viewed `nb-plain-2.png` and `nb-ab-1.png`: grass/canopy/fog/haze/exposure and sky lighting intact,
  matching current save/view references. No rejected shader/effect/resolution workaround added.
- Final kept commit **5ce57b84aa** on **perf/oot-60-back-costs**, direct child of **b67bf6a4a0**.
  Only production diff is **one file, 9 insertions / 2 deletions** in
  `src/video_core/buffer_cache/buffer_cache.h`. Neither rejected table nor rejected classification
  is included. Measurement-only `native-bank-measure.patch` reversed; intent header removed.
- Clean production reconfigure/build CLI + tests completed **after** stripping all measurement
  code, nice19/six jobs. Focused gate **322 cases / 8,297,100 assertions passed** again;
  `/tmp/opencode/native-bank-final-{configure,build,tests}.log`. Worktree clean, revision refreshed.
  `build/bin/eden-cli` is now the clean 5ce57b84aa executable, **not** an instrumentation build.
- **Ready for orchestrator review and owner-build integration as an incremental improvement**, not
  as stable-50 qualification. Owner GUI worktree/build and config files untouched; no pushes/PRs.
  No emulator or benchmark remains running. Existing stash@{0} preserved.
- If continuing toward stable50, re-sample after this kept change. Evidence suggests back draw/
  uniform/storage binding costs, not another unsafe front acquire shortcut. Unmeasured prior art
  already read: 1725659895 clean-word skip, c8415d0e80 storage binding cache, abb9ae8426 bitmap-word
  marking, d713f4462a layout-interval cache. Check ancestry/equivalent code before porting.

- Run `next-profile-1` @ `5ce57b84aa`: 43.077 FPS, 44.29–84.01 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'EDEN_NX2_GPU_PROFILE': '1'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1711: mean 23.209, median 22.867, p95 25.370, p99 27.476 ms; >20 ms 100.00%. Artifacts: `oot-dumps/perf60/next-profile-1.*`. Verdict pending paired comparison/visuals.

- Run `next-plain-1` @ `5ce57b84aa`: 47.626 FPS, 44.28–85.04 s; env `{'EDEN_NX2_VSYNC_HZ': '120'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1941: mean 21.006, median 20.866, p95 22.461, p99 23.701 ms; >20 ms 87.22%. Artifacts: `oot-dumps/perf60/next-plain-1.*`. Verdict pending paired comparison/visuals.

- Run `next-gprof-1` @ `5ce57b84aa`: 47.936 FPS, 44.28–85.02 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'LD_PRELOAD': '/usr/lib/libprofiler.so', 'CPUPROFILE': '/home/loganp/oot-dumps/perf60/next-gprof-1.prof', 'CPUPROFILESIGNAL': '41', 'CPUPROFILE_FREQUENCY': '500'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1953: mean 20.861, median 20.722, p95 22.180, p99 23.022 ms; >20 ms 80.54%. Artifacts: `oot-dumps/perf60/next-gprof-1.*`. Verdict pending paired comparison/visuals.

## Post-delivery profile and build-88 integration (2026-11-06)

- Owner approved/delivered 5ce57b84aa in GUI; this agent never touched that worktree/build.
- Re-profile on 5ce: back's top three **active** exclusive zones: method **5.01 ms/frame**, other
  **3.26 ms/frame**, bind-stage buffers **2.55 ms/frame**, calculated from the 43.077 FPS
  diagnostic run's per-second totals. Front writer waits **8.02 ms/frame**, acquire **6.09 ms/frame**
  (blocked time, not additional back CPU work), active macro processing **4.09 ms/frame**.
- Sampling without GPU_PROFILE (`next-gprof-1.prof.0`, report `oot-dumps/next-gprof-1.txt`): back
  graphics configure **32.5%**, uniform binding **12.2%**, compute native processing **10.6%**.
  mprotect **1.5%**, down from ~3.9% before native-bank change. Sampling percentages are attribution,
  not calibrated ms/frame; prefer zone totals above for timing.
- Present distribution: 5/25/50/75/95/99th percentiles **19.63/20.37/20.88/21.42/22.46/23.70 ms**.
  237 intervals <20; 825 at20–21; 630 at21–22; 172 at22–23; 35 at23–25; only four >25 ms
  (including isolated112.5 and66.1 ms outliers). **Mostly uniform cost above20 ms**, not a periodic
  spike train; removing rare stutters alone cannot meet p95. Session base FPS now47.6–47.9, showing
  why cross-session absolute49.3 cannot be silently assumed reproducible.
- Owner redirected next step to merge build-88. New branch **perf/oot-60-b88**, merge checkpoint
  **02abea340c**, parents5ce57b84aa and zortos/nx2 **55532f2d81**. Preserved #147 consumed trap
  subtraction, #149 deferred-submit fix, scheduler ownership/BackQuiet, RADV depth lowering,
  64MiB table-pointer pools, ranged macro waits and CMDPAGE_DIAG. Reconciled direct bank upload.
  Adopted #194 host-read visibility, storage binding cache, list-page batches, prebuild/cachev53,
  FSR3 fixes, and built-in Dynamic FPS. Desktop4096/5ms defaults provisional until A/B vs128/0.5ms.
- Resolved overlapping frame-dump duplicates while retaining SIZE_CAP/capture-id safeguards.
  Initial clean compile passes; focused349 cases/8,308,533 assertions and new-port39/11,612
  assertions passed (overlapping gates). Correct actual buffer-port tag is `[buffer_ports]`.
- Fetched/reviewed current Eglantine, Frizti and zortos branches/PRs. Diffs persisted under
  `oot-dumps/upstream-pr{194,206,215,231,243,245}.diff`; merge conflict record and resolution patch
  also persisted under oot-dumps. `/tmp` is not relied on for new artifacts.
- Open-pr review: #245 aligns CPU handle evaluation to the same host storage base used by shaders
  (clearly relevant correctness fix, pending separate port); #206 uses returned CAS value for retry
  (standard robust algorithm, no current hang evidence); #215 really waits pending query reports,
  whereas existing87417bc06c merely asks for a guest-host sync and skips front-owned caches. #215
  adds a full back roundtrip on *every* front DMA read; evaluate separately, not hidden in this merge.

- Run `b88-old-warm` @ `5ce57b84aa`: 13.146 FPS, 44.07–84.84 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'XDG_CACHE_HOME': '/home/loganp/oot-dumps/cache-b88-base'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=536: mean 76.128, median 80.151, p95 96.081, p99 109.957 ms; >20 ms 100.00%. Artifacts: `oot-dumps/perf60/b88-old-warm.*`. Verdict pending paired comparison/visuals.

- Run `b88-new-warm` @ `02abea340c`: 15.851 FPS, 44.03–84.85 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'XDG_CACHE_HOME': '/home/loganp/oot-dumps/cache-b88-merge'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=647: mean 63.207, median 63.755, p95 75.619, p99 86.806 ms; >20 ms 100.00%. Artifacts: `oot-dumps/perf60/b88-new-warm.*`. Verdict pending paired comparison/visuals.

- Run `b88-submit-ab-1` @ `02abea340c`: 11.647 FPS, 44.15–84.93 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'XDG_CACHE_HOME': '/home/loganp/oot-dumps/cache-b88-merge', 'EDEN_NX2_AB': 'submission', 'EDEN_NX2_AB_PERIOD_S': '4'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=475: mean 85.671, median 84.149, p95 103.247, p99 117.098 ms; >20 ms 100.00%. Artifacts: `oot-dumps/perf60/b88-submit-ab-1.*`. Verdict pending paired comparison/visuals.

- Run `b88-submit-ab-2` @ `02abea340c`: 27.812 FPS, 44.05–84.97 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'XDG_CACHE_HOME': '/home/loganp/oot-dumps/cache-b88-merge', 'EDEN_NX2_AB': 'submission', 'EDEN_NX2_AB_PERIOD_S': '4'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1138: mean 35.949, median 24.431, p95 70.810, p99 77.719 ms; >20 ms 99.82%. Artifacts: `oot-dumps/perf60/b88-submit-ab-2.*`. Verdict pending paired comparison/visuals.

- Run `b88-submit-ab-3` @ `02abea340c`: 44.371 FPS, 44.24–84.04 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'XDG_CACHE_HOME': '/home/loganp/oot-dumps/cache-b88-merge', 'EDEN_NX2_AB': 'submission', 'EDEN_NX2_AB_PERIOD_S': '4'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1766: mean 22.549, median 22.388, p95 24.548, p99 26.070 ms; >20 ms 99.09%. Artifacts: `oot-dumps/perf60/b88-submit-ab-3.*`. Verdict pending paired comparison/visuals.

- Run `b88-submit-ab-4` @ `02abea340c`: 44.818 FPS, 44.26–84.02 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'XDG_CACHE_HOME': '/home/loganp/oot-dumps/cache-b88-merge', 'EDEN_NX2_AB': 'submission', 'EDEN_NX2_AB_PERIOD_S': '4'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1782: mean 22.302, median 22.135, p95 24.231, p99 25.721 ms; >20 ms 98.09%. Artifacts: `oot-dumps/perf60/b88-submit-ab-4.*`. Verdict pending paired comparison/visuals.

- Run `b88-diag-1` @ `02abea340c`: 38.906 FPS, 44.25–84.07 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'XDG_CACHE_HOME': '/home/loganp/oot-dumps/cache-b88-merge', 'EDEN_NX2_CMDPAGE_DIAG': '1', 'EDEN_NX2_SPLIT_CHECK': '1'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1549: mean 25.707, median 25.379, p95 28.476, p99 31.950 ms; >20 ms 100.00%. Artifacts: `oot-dumps/perf60/b88-diag-1.*`. Verdict pending paired comparison/visuals.

- Run `b88-submit-ab-5` @ `02abea340c`: 43.701 FPS, 44.25–84.04 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'XDG_CACHE_HOME': '/home/loganp/oot-dumps/cache-b88-merge', 'EDEN_NX2_AB': 'submission', 'EDEN_NX2_AB_PERIOD_S': '4'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1739: mean 22.891, median 22.709, p95 25.001, p99 26.329 ms; >20 ms 99.65%. Artifacts: `oot-dumps/perf60/b88-submit-ab-5.*`. Verdict pending paired comparison/visuals.

- Run `b88-submit-ab-6` @ `02abea340c`: 42.520 FPS, 44.21–84.03 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'XDG_CACHE_HOME': '/home/loganp/oot-dumps/cache-b88-merge', 'EDEN_NX2_AB': 'submission', 'EDEN_NX2_AB_PERIOD_S': '4'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1693: mean 23.509, median 23.303, p95 25.610, p99 28.000 ms; >20 ms 99.88%. Artifacts: `oot-dumps/perf60/b88-submit-ab-6.*`. Verdict pending paired comparison/visuals.

- Run `b88-old-1` @ `5ce57b84aa`: 42.514 FPS, 44.27–84.07 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'XDG_CACHE_HOME': '/home/loganp/oot-dumps/cache-b88-base'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1692: mean 23.519, median 23.335, p95 25.598, p99 28.276 ms; >20 ms 99.88%. Artifacts: `oot-dumps/perf60/b88-old-1.*`. Verdict pending paired comparison/visuals.

- Run `b88-new-1` @ `02abea340c`: 42.940 FPS, 44.24–84.13 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'XDG_CACHE_HOME': '/home/loganp/oot-dumps/cache-b88-merge', 'EDEN_NX2_FLUSH_DRAWS': '128', 'EDEN_NX2_FENCE_FLUSH_US': '500'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1713: mean 23.282, median 23.078, p95 26.067, p99 28.969 ms; >20 ms 99.01%. Artifacts: `oot-dumps/perf60/b88-new-1.*`. Verdict pending paired comparison/visuals.

- Run `b88-new-2` @ `02abea340c`: 42.810 FPS, 44.20–84.09 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'XDG_CACHE_HOME': '/home/loganp/oot-dumps/cache-b88-merge', 'EDEN_NX2_FLUSH_DRAWS': '128', 'EDEN_NX2_FENCE_FLUSH_US': '500'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1708: mean 23.359, median 23.124, p95 25.672, p99 28.282 ms; >20 ms 99.59%. Artifacts: `oot-dumps/perf60/b88-new-2.*`. Verdict pending paired comparison/visuals.

- Run `b88-old-2` @ `5ce57b84aa`: 40.855 FPS, 44.23–84.08 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'XDG_CACHE_HOME': '/home/loganp/oot-dumps/cache-b88-base'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1628: mean 24.483, median 24.216, p95 26.667, p99 29.789 ms; >20 ms 100.00%. Artifacts: `oot-dumps/perf60/b88-old-2.*`. Verdict pending paired comparison/visuals.

- Run `b88-old-3` @ `5ce57b84aa`: 41.681 FPS, 44.23–84.08 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'XDG_CACHE_HOME': '/home/loganp/oot-dumps/cache-b88-base'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1661: mean 23.988, median 23.863, p95 26.085, p99 27.428 ms; >20 ms 100.00%. Artifacts: `oot-dumps/perf60/b88-old-3.*`. Verdict pending paired comparison/visuals.

- Run `b88-new-3` @ `02abea340c`: 43.590 FPS, 44.23–84.06 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'XDG_CACHE_HOME': '/home/loganp/oot-dumps/cache-b88-merge', 'EDEN_NX2_FLUSH_DRAWS': '128', 'EDEN_NX2_FENCE_FLUSH_US': '500'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1736: mean 22.942, median 22.829, p95 24.926, p99 27.001 ms; >20 ms 99.42%. Artifacts: `oot-dumps/perf60/b88-new-3.*`. Verdict pending paired comparison/visuals.

- Run `b88-new-4` @ `02abea340c`: 43.321 FPS, 44.24–84.08 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'XDG_CACHE_HOME': '/home/loganp/oot-dumps/cache-b88-merge', 'EDEN_NX2_FLUSH_DRAWS': '128', 'EDEN_NX2_FENCE_FLUSH_US': '500'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1726: mean 23.090, median 22.894, p95 25.072, p99 27.540 ms; >20 ms 100.00%. Artifacts: `oot-dumps/perf60/b88-new-4.*`. Verdict pending paired comparison/visuals.

- Run `b88-old-4` @ `5ce57b84aa`: 40.168 FPS, 44.24–84.15 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'XDG_CACHE_HOME': '/home/loganp/oot-dumps/cache-b88-base'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1603: mean 24.905, median 24.591, p95 27.779, p99 31.740 ms; >20 ms 100.00%. Artifacts: `oot-dumps/perf60/b88-old-4.*`. Verdict pending paired comparison/visuals.

- Run `b88-final-diag-1` @ `ff0de3d0a1`: 40.837 FPS, 44.20–84.02 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'XDG_CACHE_HOME': '/home/loganp/oot-dumps/cache-b88-merge', 'EDEN_NX2_CMDPAGE_DIAG': '1', 'EDEN_NX2_SPLIT_CHECK': '1'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1626: mean 24.475, median 24.231, p95 26.282, p99 29.388 ms; >20 ms 100.00%. Artifacts: `oot-dumps/perf60/b88-final-diag-1.*`. Verdict pending paired comparison/visuals.

- Run `b88-old-5` @ `5ce57b84aa`: 42.538 FPS, 44.23–84.06 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'XDG_CACHE_HOME': '/home/loganp/oot-dumps/cache-b88-base'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1694: mean 23.497, median 23.292, p95 25.360, p99 27.161 ms; >20 ms 99.94%. Artifacts: `oot-dumps/perf60/b88-old-5.*`. Verdict pending paired comparison/visuals.

- Run `b88-final-plain-1` @ `ff0de3d0a1`: 44.918 FPS, 44.24–84.09 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'XDG_CACHE_HOME': '/home/loganp/oot-dumps/cache-b88-merge'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1790: mean 22.265, median 22.113, p95 24.080, p99 25.669 ms; >20 ms 98.16%. Artifacts: `oot-dumps/perf60/b88-final-plain-1.*`. Verdict pending paired comparison/visuals.

- Run `b88-final-plain-2` @ `ff0de3d0a1`: 44.521 FPS, 44.24–84.07 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'XDG_CACHE_HOME': '/home/loganp/oot-dumps/cache-b88-merge'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1773: mean 22.454, median 22.288, p95 24.360, p99 25.735 ms; >20 ms 98.93%. Artifacts: `oot-dumps/perf60/b88-final-plain-2.*`. Verdict pending paired comparison/visuals.

- Run `b88-old-6` @ `5ce57b84aa`: 42.597 FPS, 44.23–84.05 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'XDG_CACHE_HOME': '/home/loganp/oot-dumps/cache-b88-base'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1696: mean 23.473, median 23.327, p95 25.316, p99 26.714 ms; >20 ms 100.00%. Artifacts: `oot-dumps/perf60/b88-old-6.*`. Verdict pending paired comparison/visuals.

### Build-88 delivered candidate checkpoint: ff0de3d0a1 (ready for review, not stable50)

**Commits** on `perf/oot-60-b88`:
- **02abea340c**: merge zortos build-88, preserving all local coherence/rendering contracts.
- **dbe2d23b4d**: select upstream128 draws /500us after four warm within-run RADV A/Bs. Both
 4096/5000 and128/500 remain selectable using EDEN_NX2_FLUSH_DRAWS/EDEN_NX2_FENCE_FLUSH_US.
- **ff0de3d0a1**: port reviewed zortos PR245 /1735209618, align storage-sourced CPU texture handle
 evaluation to the same host-aligned buffer base used by the shader. Existing code supported this
 only under HANDLE_ALIGN opt-in; now correct by default in Vulkan and OpenGL. Retain zero-handle
 diagnostics; add upstream direct/indirect alignment regression test.

**Measurement isolation**: shader+Mesa caches copied into `oot-dumps/cache-b88-base` and
 `cache-b88-merge`; XDG_CACHE_HOME points only there. Owner real shader caches/config/GUI untouched.
 Frozen trace binaries `oot-dumps/bin-5ce57b84aa-trace/eden-cli` and `bin-b88-trace/eden-cli`.
 New cache v53 files confirmed (`vulkan.fork-v53.bin`, `vulkan_used.fork-v53.bin`, driver pipelines).
 Loader confirms built-in Dark Dynamic FPS **skipped because the user IPS patched main+0x6e6cc4**;
 installed user mod retains precedence. Framegen off, handheld1x, output window verified1920x1080;
 Xwayland screen3440x1440 checked before every run.

**Excluded startup/nonstationary data**: old/new warm runs and submission-ab1/2. Both binaries were
 initially GPU-saturated at13–16FPS (front/back only20–30% CPU), then shifted mid-session to40+FPS.
 Outdoor runtime pipeline builds ceased after warming. No causal claim about the host was proved.
 The old5ce cold screenshot had transient white grass too; this baseline cold/startup artifact is
 **not claimed fixed**. Warm final screenshots are intact. Do not pool these transients with the
 later warm A/Bs, or compare this session's absolute FPS to the previous49.3FPS session.

| Warm submission A/B | A4096/5000 FPS | B128/500 FPS | A/B median ms | A/B p95 ms | A/B p99 ms |
|---|---:|---:|---|---|---|
| ab3 | 43.45 | 45.24 | 22.86 /22.02 | 24.88 /23.83 | 26.11 /25.33 |
| ab4 | 43.86 | 45.81 | 22.61 /21.69 | 24.70 /23.68 | 25.72 /25.48 |
| ab5 | 42.89 | 44.41 | 23.12 /22.33 | 25.24 /24.75 | 26.74 /26.58 |
| ab6 | 41.59 | 43.53 | 23.78 /22.86 | 26.09 /25.02 | 29.04 /26.31 |
| pooled | **42.94** | **44.75** | **23.08 /22.20** | **25.33 /24.45** | **27.19 /26.13** |

Four of four favor upstream defaults (+4.2%). This supersedes the earlier different-base RADV
 fence500us rejection; the context changed, and within-run evidence controls the new decision.

**Merged vs5ce**, separate warm caches, interleaved old1/new1/new2/old2/old3/new3/new4/old4:

| Pair | 5ce FPS | merged128/500 FPS | 5ce/merged p95 ms |
|---|---:|---:|---|
| 1 | 42.514 | 42.940 | 25.598 /26.067 |
| 2 | 40.855 | 42.810 | 26.667 /25.672 |
| 3 | 41.681 | 43.590 | 26.085 /24.926 |
| 4 | 40.168 | 43.321 | 27.779 /25.072 |
| pooled intervals | **41.285** | **43.128** | **26.711 /25.470** |

Pooled median **23.961 ->22.958ms**, p99 **29.617 ->27.883ms**, +4.5% FPS. Not an equivalent
 within-process switch for the whole merge, hence interleaved comparison rather than attribution
 to one subfeature. Earlier standalone narrow-list no-gain result remains valid; it is included
 here as part of required upstream merge, not advertised as an isolated performance win.

**Final PR245/default validation**, old5/final1/final2/old6: base **42.538/42.597FPS** vs final
 **44.918/44.521FPS**; final p95 **24.080/24.360ms**, p99 **25.669/25.735ms**. Pooled base/final
 **42.567/44.725FPS**, median **23.309/22.185ms**, p95 **25.360/24.192ms**, p99 **27.015/25.788ms**.
 No submission env overrides in final runs; production defaults exercised. **Goal NOT met.**

**Correctness/visual gates**:
- Initial merged diag1:1,885,363 DMA reads,0 unexplained differences,3 async-report changes,
 0 ownership warnings. Final diag: **1,983,732 DMA reads;0 unexplained differences;0 GPU-written
 page reads;2 async-report changes;0 missed draw writes on entry/elsewhere**, split checker on.
- Viewed final2 and old6 side-by-side: grass/canopy/fog/haze/exposure/sky lighting intact in warm
 scene. No effects skipped or internal resolution changed. Screenshots under perf60/b88-*.png.
- All required #194 new tests included. Final **clean production** reconfigure/build CLI+tests,
 after reversing all trace/AB patches: **371 cases /8,308,822 assertions passed**. Logs persist as
 `oot-dumps/b88-delivery-{configure,build,tests}.log`. Worktree clean atff0de3d0a1; `build/bin/eden-cli`
 is the clean production binary. Measurement patches retained only under perfscripts, not committed.
- Reviewed PR206 fully: robust CAS feedback retry, but no CAS hang/root-cause evidence in this
 scene and shader-cache retranslation not separately verified; **not ported yet**. PR215 goes beyond
 87417bc06c: waits query callbacks even below High accuracy and incurs a back roundtrip on every
 front DMA read. No warm-scene missed-query evidence established; **deferred for targeted trace**,
 not silently dropped or conflated with the existing sync request. PR210/223 skipped as instructed.

**Ready for owner integration as an incremental build-88 correctness/performance deliverable**,
 not stable50 qualification. Review all three commits; owner GUI/source/config/cache remain untouched
 by this agent. No emulator/batch running. Existing measurement stash preserved. Next stable50 work
 should profile this merged/default base, with copies of v53 caches, before new synchronization work.

- Run `host-plain-1` @ `ff0de3d0a1`: 45.775 FPS, 44.26–84.89 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'XDG_CACHE_HOME': '/home/loganp/oot-dumps/cache-b88-merge'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1860: mean 21.849, median 21.656, p95 23.417, p99 24.689 ms; >20 ms 95.81%. Artifacts: `oot-dumps/perf60/host-plain-1.*`. Verdict pending paired comparison/visuals.

- Run `host-profile-1` @ `ff0de3d0a1`: 42.063 FPS, 44.24–84.84 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'EDEN_NX2_GPU_PROFILE': '1', 'XDG_CACHE_HOME': '/home/loganp/oot-dumps/cache-b88-merge'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1708: mean 23.782, median 23.446, p95 26.009, p99 27.428 ms; >20 ms 100.00%. Artifacts: `oot-dumps/perf60/host-profile-1.*`. Verdict pending paired comparison/visuals.

- Run `host-query-1` @ `ff0de3d0a1`: 42.325 FPS, 44.25–84.86 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'EDEN_NX2_QUERY_DMA_TRACE': '1', 'XDG_CACHE_HOME': '/home/loganp/oot-dumps/cache-b88-merge'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1719: mean 23.631, median 23.562, p95 25.315, p99 26.339 ms; >20 ms 99.77%. Artifacts: `oot-dumps/perf60/host-query-1.*`. Verdict pending paired comparison/visuals.

- Run `ts-base-1` @ `ff0de3d0a1`: 45.739 FPS, 44.25–84.87 s; env `{'EDEN_NX2_VSYNC_HZ': '120', 'XDG_CACHE_HOME': '/home/loganp/oot-dumps/cache-b88-merge'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{}`. Actual guest intervals n=1858: mean 21.856, median 21.720, p95 23.614, p99 25.434 ms; >20 ms 96.23%. Artifacts: `oot-dumps/perf60/ts-base-1.*`. Verdict pending paired comparison/visuals.

## Takeover (Opus 5.5, 2026-11-06 ~11:20-11:50): STOPPED on owner request

- **No source changes.** `/home/loganp/lichii-perf60` branch `perf/oot-60-host-profile` @ `ff0de3d0a1`,
  clean. No candidate commit beyond ff0de3d0a1. No build was started. No emulator/batch running.
- **Host state** (from harness telemetry `host-{plain,profile,query}-1.host*.json`, not analyzed before):
  intel_pstate governor `powersave` with EPP `performance` on all 20 policies, turbo on; P-cores held
  ~4.55-4.69 GHz averages, E-cores ~3.6 GHz; package <=56 C, thermal throttle counts 0; CPU pressure
  ~0. Xwayland screen 3440x1440 (bench window verified 1920x1080). Background CPU during runs:
  opencode-desktop electron ~72%, Discord ~26-30%, kwin_wayland ~10%, helium browser ~3%, all of them
  also render on the 7800 XT (renderD129). RX 7800 XT: sclk ~2.6 GHz, `power_dpm_force_performance_level`
  auto, `gpu_busy_percent` ~90% but the emulator's own timestamps show GPU submission sum ~11 ms per
  frame across a ~25 ms span, so the GPU is not the limit (busy% counts queue-active time).
  Nothing is throttling. The background apps (~1 core in total) are the only thing to suggest closing for benchmarking.
- **New tooling** (measurement only, under perfscripts): `tsample.c`/`tsample` = per-thread user-space
  cycle sampler (perf_event_open + LBR call stacks, no root; Yama limits only ATTACH), `tsample-run.py`
  (plain perf60 run, samples only its own child 50-80 s), `tagg.py` (inline-expanded self/inclusive),
  `ttree.py` (callee breakdown).
- Run `ts-base-1` (bin-b88-trace = ff0de3d0a1 + present trace, plain, cache-b88-merge): **45.74 FPS**,
  n=1858, artifacts `perf60/ts-base-1.{tsample,back.txt,front.txt,vkw.txt}`. Samples at 2 kHz over 30 s:
  GPU 50.3k (~84% on-CPU), GPUBack 48.6k (~81%), VulkanWorker 29.2k.
  - **Front**: 54% of on-CPU samples are spin-waits on the back. Acquire drain 22.5%
    (ProcessSemaphoreTriggerMethod 20.8%, CanResumeAcquire 1.7%), list-writer WaitFor in DmaPusher::Step 16.6%,
    FrontDmaRead WaitFor 15.1%, plus **9.0% SpinRecursiveMutex::LockContended in FrontDmaRead
    flush_if_dirty -> IsMemoryDirty -> MustFlushRegion (buffer_cache.mutex held by the back through
    draw setup)**. Real front work is ~37% of samples.
  - **Back**: ~10% BackLoop idle spin (SplitWait atomic load), so ~70% real work. Draw 51%
    (GraphicsPipeline::Configure 36%: BindHostStageBuffers 15.3% [uniform 13.2%: BindSmallUniformReusing
    5.1%, UploadMemory 4.9%], UpdateGraphicsBuffers 5.1%, FillImageViews 3.3%, BindHostGeometryBuffers 2.7%),
    KeplerCompute 8.6%, FlushCaching 7.5% (InnerInvalidation 6.5%: buffer WriteMemory 2.7%, lock 1.1%,
    query 1.0%, texture 0.7%, shader 0.5%), OnCacheInvalidation 3.6%, CounterReport 3.9%, I2M 3.8%.
  - Reading: both threads idle a lot. This is a front/back ping-pong latency chain, not one saturated
    thread. Fits the rejected writer table result (fewer waits, no FPS gain).
- **Unverified candidates (not implemented)**:
  1. zortos PR #250 page filter (`oot-dumps/upstream-pr250.diff`) would skip about 2.5% of back samples.
     Gap found in our tree and upstream: `ShaderCache::InvalidatePagesInRegion` also erases
     `rejected_native` entries, which the filter doesn't mark. Mark `addr` when a rejection is
     inserted (shader_cache.cpp:313). Skipping `InvalidateRegions` also delays
     `RemovePendingShaders` for entries marked by OnCacheInvalidation. Keep that behaviour, e.g. with a
     pending flag.
  2. Lock-free superset bitmap for `gpu_modified_ranges`, so the front's flush_if_dirty skips
     buffer_cache.mutex (front 9% contended). Needs an ordering proof plus CMDPAGE_DIAG.
  3. Drain() also waits for BackQuiet (idle hook -> Scheduler::FlushDeferred -> possible submit) on
     every acquire drain (~4.6k/s). Check whether that submit is on the critical path. Not measured.
- White grass: not investigated this session.

- Run `host60-calibrate` @ `ff0de3d0a1`: 46.464 FPS, 75.77–115.37 s; env `{'XDG_CACHE_HOME': '/home/loganp/oot-dumps/cache-perf60-current'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{'fps_limit': '0', 'use_docked_mode': '0'}`. Actual guest intervals n=1840: mean 21.518, median 21.606, p95 23.839, p99 25.305 ms; >20 ms 77.61%. Hitches >50/>100 ms: 2/1. Runtime-pipeline report lines: 1 (details in stats JSON). Artifacts: `oot-dumps/perf60/host60-calibrate.*`. Verdict pending paired comparison/visuals.

- Run `host60-base-1` @ `ff0de3d0a1`: 46.490 FPS, 74.60–114.24 s; env `{'XDG_CACHE_HOME': '/home/loganp/oot-dumps/cache-perf60-current'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{'fps_limit': '0', 'use_docked_mode': '0'}`. Actual guest intervals n=1843: mean 21.503, median 21.644, p95 23.618, p99 27.379 ms; >20 ms 80.36%. Hitches >50/>100 ms: 1/0. Runtime-pipeline report lines: 1 (details in stats JSON). Artifacts: `oot-dumps/perf60/host60-base-1.*`. Verdict pending paired comparison/visuals.

## 2026-11-06: fresh 9d940ae916 investigation (GPT-6.1 Sol)

- Fast-forwarded isolated `perf/oot-60-host-profile` worktree to exact owner commit **9d940ae916**. Frozen baseline executable `oot-dumps/bin-9d940ae916-trace/eden-cli` is built from that source plus only `present-trace.patch`; its revision string is stale ff0de3d0a1 (CMake does not refresh it automatically). Baseline build completed with nice19/six jobs.
- Fetched both remotes; Eglantine gained seven commits, net source change empty against our tree (StarFox merge/revert and already-present TLD4 guard). Required upstream merge is **df6ac4be40**, no source delta. Survey `research/pr-survey-perf.md` read, relevant diffs read. Source-level checks supersede its ancestry-only claims: #146/#147/#175/#176/#194 and command-list copies are already implemented/equivalent; #670's old hash cache is superseded by mapping-run snapshots in current `MaxContinuousRange`.
- Harness `perfscripts/perf60.py`: frame-counted A presses f240..f984 instead of wall time, server/first-present readiness with 180s deadline, 44s settle after first present and 40s gameplay window, visible debug-client errors, Debug/Frontend log info, FPS limit0 and handheld copied-config overrides, >50/>100ms hitch counts and full runtime pipeline report lines in stats JSON. `serial-perf60.py` owns shared bench.lock (finally removes), waits boundedly for compilers and launches serial queues. Original configs/caches never edited; full owner cache copied to `cache-perf60-current`.
- Calibration **host60-calibrate**: loaded **19,495** pipelines, first present log31.34s, detected uptime30.72s; measured75.77–115.37s,46.464FPS,p9523.839ms,hitches2/1,7 runtime pipelines(35.3ms total). Input timing is robust to pipeline preload. **Scene mismatch:** owner save now loads Hyrule market square, not older wt-ab-4 Kokiri Forest. Calibration screenshot is correct/stable gameplay but not the old forest benchmark; new runs cannot claim forest parity until a reproducible forest route/save is available. No owner save was changed.
- Pending candidates, NOT accepted yet: corrected #250 page filter (cover rejected shader addresses, preserve pending shader removals, conservative arithmetic overflow); #179/#231 semaphore precheck + bounded poll drains (uses our existing all-memory-writer tracking rather than importing their whole tracking implementation); #179 SEM_EXECUTE defer; #179 LIST settled-written-page heuristic. Latest user instruction allows risky defaults with per-feature off switches. No accepted shortcut or FPS claim yet.

- Run `host60-base-2` @ `ff0de3d0a1`: 44.431 FPS, 75.73–115.35 s; env `{'XDG_CACHE_HOME': '/home/loganp/oot-dumps/cache-perf60-current'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{'fps_limit': '0', 'use_docked_mode': '0'}`. Actual guest intervals n=1760: mean 22.508, median 22.484, p95 25.040, p99 28.249 ms; >20 ms 90.85%. Hitches >50/>100 ms: 1/0. Runtime-pipeline report lines: 1 (details in stats JSON). Artifacts: `oot-dumps/perf60/host60-base-2.*`. Verdict pending paired comparison/visuals.

- Run `host60-base-3` @ `ff0de3d0a1`: 46.306 FPS, 75.63–115.22 s; env `{'XDG_CACHE_HOME': '/home/loganp/oot-dumps/cache-perf60-current'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{'fps_limit': '0', 'use_docked_mode': '0'}`. Actual guest intervals n=1833: mean 21.591, median 21.715, p95 23.974, p99 26.179 ms; >20 ms 79.71%. Hitches >50/>100 ms: 1/0. Runtime-pipeline report lines: 1 (details in stats JSON). Artifacts: `oot-dumps/perf60/host60-base-3.*`. Verdict pending paired comparison/visuals.

- Run `host60-base-sample` @ `ff0de3d0a1`: 45.527 FPS, 75.69–115.25 s; env `{'XDG_CACHE_HOME': '/home/loganp/oot-dumps/cache-perf60-current'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{'fps_limit': '0', 'use_docked_mode': '0'}`. Actual guest intervals n=1801: mean 21.959, median 22.035, p95 24.154, p99 28.352 ms; >20 ms 84.62%. Hitches >50/>100 ms: 2/1. Runtime pipelines built over run: 5 (1 report lines; details in stats JSON). Artifacts: `oot-dumps/perf60/host60-base-sample.*`. Verdict pending paired comparison/visuals.

- Fresh warm **market-square** baseline (not old forest): host60-base-1/2/3 =46.490/44.431/46.306FPS, mean45.742, spread44.431–46.490(~4.5%). Pooled5436 intervals,p9524.489ms,hitches>50/>100=3/0. Runtime pipelines7/5/5 per whole run; repeated late variants remain despite19,495 preloads. Actual GPU submission sum8.813/9.107/8.924ms/frame; front17.52/18.18/17.60 and back18.16/18.92/18.06CPUms/frame. Host GPUbusy87% is not proof of GPU saturation.
- Added optional `SAMPLE=1` to perf60: samples its own child only, for30s inside the recalibrated gameplay window (old tsample-run.py50–80s misses much of this window). Profile host60-base-sample45.527FPS,2/1hitches,5pipeline builds;51,777front/52,419back/31,711worker cycle samples,0lost. Shared bench lock correctly waited60s for another agent's already-running compiler before launch. This profile run is diagnostic, not a fourth qualification baseline.

- Run `host60-ab-base-1` @ `ff0de3d0a1`: 46.446 FPS, 75.67–115.28 s; env `{'XDG_CACHE_HOME': '/home/loganp/oot-dumps/cache-perf60-current'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{'fps_limit': '0', 'use_docked_mode': '0'}`. Actual guest intervals n=1840: mean 21.523, median 21.714, p95 23.708, p99 25.703 ms; >20 ms 80.27%. Hitches >50/>100 ms: 1/0. Runtime pipelines built over run: 5 (1 report lines; details in stats JSON). Artifacts: `oot-dumps/perf60/host60-ab-base-1.*`. Verdict pending paired comparison/visuals.

- Run `host60-ab-all-1` @ `df6ac4be40`: 47.390 FPS, 75.69–115.28 s; env `{'XDG_CACHE_HOME': '/home/loganp/oot-dumps/cache-perf60-current'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{'fps_limit': '0', 'use_docked_mode': '0'}`. Actual guest intervals n=1876: mean 21.099, median 21.219, p95 23.198, p99 24.865 ms; >20 ms 76.01%. Hitches >50/>100 ms: 2/1. Runtime pipelines built over run: 6 (1 report lines; details in stats JSON). Artifacts: `oot-dumps/perf60/host60-ab-all-1.*`. Verdict pending paired comparison/visuals.

- Run `host60-ab-base-2` @ `ff0de3d0a1`: 46.182 FPS, 75.66–115.31 s; env `{'XDG_CACHE_HOME': '/home/loganp/oot-dumps/cache-perf60-current'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{'fps_limit': '0', 'use_docked_mode': '0'}`. Actual guest intervals n=1831: mean 21.653, median 21.794, p95 24.072, p99 25.235 ms; >20 ms 80.61%. Hitches >50/>100 ms: 1/0. Runtime pipelines built over run: 6 (1 report lines; details in stats JSON). Artifacts: `oot-dumps/perf60/host60-ab-base-2.*`. Verdict pending paired comparison/visuals.

- Run `host60-ab-all-2` @ `df6ac4be40`: 46.902 FPS, 56.33–95.94 s; env `{'XDG_CACHE_HOME': '/home/loganp/oot-dumps/cache-perf60-current'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{'fps_limit': '0', 'use_docked_mode': '0'}`. Actual guest intervals n=1858: mean 21.328, median 21.361, p95 23.820, p99 25.338 ms; >20 ms 77.93%. Hitches >50/>100 ms: 1/0. Runtime pipeline builds reported: 5 (1 report lines; details in stats JSON, trailing partial windows may be unreported). Artifacts: `oot-dumps/perf60/host60-ab-all-2.*`. Verdict pending paired comparison/visuals.

- Run `host60-ab-safe-1` @ `df6ac4be40`: 47.393 FPS, 55.31–94.92 s; env `{'XDG_CACHE_HOME': '/home/loganp/oot-dumps/cache-perf60-current', 'EDEN_NX2_NO_ACQUIRE_PRECHECK': '1', 'EDEN_NX2_NO_SEM_EXECUTE_DEFER': '1', 'EDEN_NX2_NO_LIST_FAST': '1'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{'fps_limit': '0', 'use_docked_mode': '0'}`. Actual guest intervals n=1877: mean 21.110, median 21.239, p95 23.122, p99 24.878 ms; >20 ms 75.76%. Hitches >50/>100 ms: 2/1. Runtime pipeline builds reported: 5 (1 report lines; details in stats JSON, trailing partial windows may be unreported). Artifacts: `oot-dumps/perf60/host60-ab-safe-1.*`. Verdict pending paired comparison/visuals.

- Run `host60-ab-safe-2` @ `df6ac4be40`: 47.199 FPS, 55.31–94.93 s; env `{'XDG_CACHE_HOME': '/home/loganp/oot-dumps/cache-perf60-current', 'EDEN_NX2_NO_ACQUIRE_PRECHECK': '1', 'EDEN_NX2_NO_SEM_EXECUTE_DEFER': '1', 'EDEN_NX2_NO_LIST_FAST': '1'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{'fps_limit': '0', 'use_docked_mode': '0'}`. Actual guest intervals n=1870: mean 21.190, median 21.320, p95 23.406, p99 25.012 ms; >20 ms 76.10%. Hitches >50/>100 ms: 2/1. Runtime pipeline builds reported: 6 (1 report lines; details in stats JSON, trailing partial windows may be unreported). Artifacts: `oot-dumps/perf60/host60-ab-safe-2.*`. Verdict pending paired comparison/visuals.

### Probe 1: #250 plus old await prototypes; cache-aging confounder

- Required tests passed **360 cases /9,218,445 assertions**. Frozen prototype `oot-dumps/bin-host60-probe1/eden-cli` has corrected #250 plus three default-on prototypes with off switches: `EDEN_NX2_NO_ACQUIRE_PRECHECK=1`, `EDEN_NX2_NO_SEM_EXECUTE_DEFER=1`, `EDEN_NX2_NO_LIST_FAST=1`. These are **NOT accepted/shipped**. Sources and new headers/tests are archived next to the binary.
- Interleaved base1/all1/base2/all2:46.446/47.390/46.182/46.902FPS. P95ms23.708/23.198/24.072/23.820; hitches1/0,2/1,1/0,1/0; runtime pipeline reports5/6/6/5. Safe #250-only pair47.393/47.199FPS,p9523.122/23.406,hitches2/1 each,reports5/6. No10% gain, and no demonstrated incremental benefit from these three shortcuts over the safe-only result. They were removed from current source; frozen prototype retained only as rejected research, not committed. This does not assert that each independently harms performance.
- Risks of retired prototypes: acquire precheck can observe old CPU semaphore values before asynchronous host query completion (bounded16poll/1ms drains retained); SEM_EXECUTE deferral changes when pending fence/readback work becomes visible; LIST heuristic assumes the written-page set has settled after2s and can miss a first queued GPU write to a new command-list page, corrupting later commands/draws. Screenshot host60-ab-all-1 preserves current square geometry/vegetation/haze/shadows/exposure; quantitative static-region differences are within baseline-vs-baseline variation (`research/host60-probe1-visuals.json`). Animated NPCs/wind prevent byte-identical PNGs.
- **Important invalidation of part of this batch:** preloaded pipeline counts were19,498 through base2, but only4,961 in all2 and4,602 in safe1 (64 extra background prebuilds). Root: `SaveUsedPipelines()` increments all unused hint ages each call, including15-second saves; sixteen intended sessions become only a few short runs. Default aging eventually removed unused old content from the copied hint list. Do not claim same-cache qualification from the second pair/safe pair.
- Harness now forces existing **EDEN_NX2_PRELOAD_ALL_PIPELINES=1** for every side and records effective EDEN env in stats JSON, so pipeline resident/preload state stays comparable. The owner cache is untouched; only the isolated copy ages. New qualification is required. Also corrected runtime metric naming: summed ten-second report lines are **reported builds**, not necessarily all builds (a trailing partial window can be unreported).
- Source correction to the initial timing hypothesis: `Nx2KeyClockSeconds` starts on first SDL iteration after preload, not process start. The old A presses were not necessarily lost. Fixed44–84s process-time sampling was still wrong because it overlaps menus/loading after the longer preload; first-present-relative settling fixes that. Current market save differs from old Kokiri save independently of this timing issue.
- Probe2 (pending): keep old three prototypes disabled; test CPU-only DMA dirty-cache precheck (retains relevant writer wait) and more aggressive CPU-only DMA wait elision independently. This adapts #179's CPU-owned read premise to the already-present ranged-wait implementation. Escapes `EDEN_NX2_NO_DMA_DIRTY_PRECHECK=1` and `EDEN_NX2_NO_DMA_WAIT_ELISION=1`. Elision risk: an as-yet unexecuted first GPU write or a queued inline/report write to a previously clean page can make macro parameters stale; no complete future-writer proof is claimed. Dirty precheck relies on complete GPU-written-page accounting for relevant writers. Full-drain mode overrides wait elision; CMDPAGE_DIAG repeats reads with the original cache query/flush after full drain.
- Probe2 required gate **361 cases /9,218,637 assertions passed**, including a real back-thread test that memory-writer waits publish both GPU-written page bits and data. No shortcut has been accepted or committed.

- Run `host60-p2-base-warm` @ `ff0de3d0a1`: 46.621 FPS, 75.66–115.24 s; env `{'XDG_CACHE_HOME': '/home/loganp/oot-dumps/cache-perf60-current'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{'fps_limit': '0', 'use_docked_mode': '0'}`. Actual guest intervals n=1845: mean 21.457, median 21.573, p95 23.807, p99 25.392 ms; >20 ms 78.81%. Hitches >50/>100 ms: 1/0. Runtime pipeline builds reported: 5 (1 report lines; details in stats JSON, trailing partial windows may be unreported). Artifacts: `oot-dumps/perf60/host60-p2-base-warm.*`. Verdict pending paired comparison/visuals.

- Run `host60-p2-base-1` @ `ff0de3d0a1`: 46.995 FPS, 74.56–114.14 s; env `{'XDG_CACHE_HOME': '/home/loganp/oot-dumps/cache-perf60-current'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{'fps_limit': '0', 'use_docked_mode': '0'}`. Actual guest intervals n=1860: mean 21.275, median 21.466, p95 23.279, p99 25.764 ms; >20 ms 77.37%. Hitches >50/>100 ms: 1/0. Runtime pipeline builds reported: 6 (1 report lines; details in stats JSON, trailing partial windows may be unreported). Artifacts: `oot-dumps/perf60/host60-p2-base-1.*`. Verdict pending paired comparison/visuals.

- Run `host60-p2-dma-1` @ `df6ac4be40`: 47.163 FPS, 74.55–114.16 s; env `{'XDG_CACHE_HOME': '/home/loganp/oot-dumps/cache-perf60-current', 'EDEN_NX2_NO_ACQUIRE_PRECHECK': '1', 'EDEN_NX2_NO_SEM_EXECUTE_DEFER': '1', 'EDEN_NX2_NO_LIST_FAST': '1'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{'fps_limit': '0', 'use_docked_mode': '0'}`. Actual guest intervals n=1868: mean 21.198, median 21.319, p95 23.314, p99 24.470 ms; >20 ms 76.02%. Hitches >50/>100 ms: 2/1. Runtime pipeline builds reported: 6 (1 report lines; details in stats JSON, trailing partial windows may be unreported). Artifacts: `oot-dumps/perf60/host60-p2-dma-1.*`. Verdict pending paired comparison/visuals.

- **CORRECTION to the preceding aging-root-cause note:** no per-15-second cumulative aging bug exists. `SaveUsedPipelines` constructs each output from the immutable startup `used_pipelines` map; repeated saves recompute age+1, they do not assign the new ages back into that map. Thus unused hints age once per session as intended. Repeated benchmark *sessions* expired ~14,500 old hints. A read-only check of owner/current hint files shows a 14,544-entry age8 cohort in the owner file and an aged-out cohort in our isolated copy. The observed preload confounder is real; the claimed code defect is withdrawn. **Do not implement an aging fix.** Full-preload env control remains valid.
- Other surveyed candidates now checked by full diff/source, not ancestry: Eglantine #374 direct one-page descriptor reads are already present verbatim in `texture_cache/descriptor_table.h`; #562's clean-upload bookkeeping is already superseded by per-word `clean_words`/`ForEachDirtyUploadRange`, and its index-bind epoch cache is already present. #670 is exactly64→4096 hash entries, superseded by mapping-run snapshots. No port warranted for those.

- Run `host60-p2-base-2` @ `ff0de3d0a1`: 46.275 FPS, 74.61–114.22 s; env `{'XDG_CACHE_HOME': '/home/loganp/oot-dumps/cache-perf60-current'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{'fps_limit': '0', 'use_docked_mode': '0'}`. Actual guest intervals n=1833: mean 21.611, median 21.763, p95 24.079, p99 25.308 ms; >20 ms 78.89%. Hitches >50/>100 ms: 1/0. Runtime pipeline builds reported: 5 (1 report lines; details in stats JSON, trailing partial windows may be unreported). Artifacts: `oot-dumps/perf60/host60-p2-base-2.*`. Verdict pending paired comparison/visuals.

- Run `host60-p2-dma-2` @ `df6ac4be40`: 47.328 FPS, 74.57–114.15 s; env `{'XDG_CACHE_HOME': '/home/loganp/oot-dumps/cache-perf60-current', 'EDEN_NX2_NO_ACQUIRE_PRECHECK': '1', 'EDEN_NX2_NO_SEM_EXECUTE_DEFER': '1', 'EDEN_NX2_NO_LIST_FAST': '1'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{'fps_limit': '0', 'use_docked_mode': '0'}`. Actual guest intervals n=1873: mean 21.126, median 21.280, p95 23.477, p99 24.462 ms; >20 ms 74.27%. Hitches >50/>100 ms: 1/0. Runtime pipeline builds reported: 5 (1 report lines; details in stats JSON, trailing partial windows may be unreported). Artifacts: `oot-dumps/perf60/host60-p2-dma-2.*`. Verdict pending paired comparison/visuals.

- Run `host60-p2-safe-1` @ `df6ac4be40`: 47.157 FPS, 74.66–114.27 s; env `{'XDG_CACHE_HOME': '/home/loganp/oot-dumps/cache-perf60-current', 'EDEN_NX2_NO_ACQUIRE_PRECHECK': '1', 'EDEN_NX2_NO_SEM_EXECUTE_DEFER': '1', 'EDEN_NX2_NO_LIST_FAST': '1', 'EDEN_NX2_NO_DMA_WAIT_ELISION': '1', 'EDEN_NX2_NO_DMA_DIRTY_PRECHECK': '1'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{'fps_limit': '0', 'use_docked_mode': '0'}`. Actual guest intervals n=1868: mean 21.199, median 21.328, p95 23.456, p99 24.840 ms; >20 ms 76.45%. Hitches >50/>100 ms: 1/0. Runtime pipeline builds reported: 6 (1 report lines; details in stats JSON, trailing partial windows may be unreported). Artifacts: `oot-dumps/perf60/host60-p2-safe-1.*`. Verdict pending paired comparison/visuals.

- Run `host60-p2-dirty-1` @ `df6ac4be40`: 47.090 FPS, 74.58–114.23 s; env `{'XDG_CACHE_HOME': '/home/loganp/oot-dumps/cache-perf60-current', 'EDEN_NX2_NO_ACQUIRE_PRECHECK': '1', 'EDEN_NX2_NO_SEM_EXECUTE_DEFER': '1', 'EDEN_NX2_NO_LIST_FAST': '1', 'EDEN_NX2_NO_DMA_WAIT_ELISION': '1'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{'fps_limit': '0', 'use_docked_mode': '0'}`. Actual guest intervals n=1867: mean 21.234, median 21.351, p95 23.709, p99 26.648 ms; >20 ms 76.22%. Hitches >50/>100 ms: 1/0. Runtime pipeline builds reported: 5 (1 report lines; details in stats JSON, trailing partial windows may be unreported). Artifacts: `oot-dumps/perf60/host60-p2-dirty-1.*`. Verdict pending paired comparison/visuals.

- Run `host60-p2-safe-2` @ `df6ac4be40`: 46.785 FPS, 75.68–115.29 s; env `{'XDG_CACHE_HOME': '/home/loganp/oot-dumps/cache-perf60-current', 'EDEN_NX2_NO_ACQUIRE_PRECHECK': '1', 'EDEN_NX2_NO_SEM_EXECUTE_DEFER': '1', 'EDEN_NX2_NO_LIST_FAST': '1', 'EDEN_NX2_NO_DMA_WAIT_ELISION': '1', 'EDEN_NX2_NO_DMA_DIRTY_PRECHECK': '1'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{'fps_limit': '0', 'use_docked_mode': '0'}`. Actual guest intervals n=1853: mean 21.372, median 21.568, p95 23.289, p99 26.047 ms; >20 ms 78.63%. Hitches >50/>100 ms: 1/0. Runtime pipeline builds reported: 5 (1 report lines; details in stats JSON, trailing partial windows may be unreported). Artifacts: `oot-dumps/perf60/host60-p2-safe-2.*`. Verdict pending paired comparison/visuals.

- Run `host60-p2-dirty-2` @ `df6ac4be40`: 46.365 FPS, 75.78–115.40 s; env `{'XDG_CACHE_HOME': '/home/loganp/oot-dumps/cache-perf60-current', 'EDEN_NX2_NO_ACQUIRE_PRECHECK': '1', 'EDEN_NX2_NO_SEM_EXECUTE_DEFER': '1', 'EDEN_NX2_NO_LIST_FAST': '1', 'EDEN_NX2_NO_DMA_WAIT_ELISION': '1'}`, CPUSET `0-15`, output 1920x1080, copied-config overrides `{'fps_limit': '0', 'use_docked_mode': '0'}`. Actual guest intervals n=1837: mean 21.566, median 21.747, p95 23.680, p99 25.283 ms; >20 ms 79.97%. Hitches >50/>100 ms: 2/1. Runtime pipeline builds reported: 6 (1 report lines; details in stats JSON, trailing partial windows may be unreported). Artifacts: `oot-dumps/perf60/host60-p2-dirty-2.*`. Verdict pending paired comparison/visuals.

### Probe 2: DMA shortcuts REJECTED; full-preload controls

- Full preload forced for all runs, avoiding hint-age confounding. Fresh baseline warm/1/2 **46.621/46.995/46.275FPS**, mean46.631, spread0.720FPS. Interleaved base1/DMA1/base2/DMA2 =46.995/47.163/46.275/47.328FPS; means46.635/47.245(+1.31%), pooled base p9523.787ms. Both DMA shortcuts were active in B, old three await prototypes disabled. This is not10%.
- Safe-only (#250) vs dirty-precheck-only, interleaved: **47.157/47.090/46.785/46.365FPS**, means46.971/46.728. Dirty-precheck loses both pairs. Combined DMA47.245 is only0.274FPS above safe46.971, below observed across-boot noise. **Both DMA shortcuts removed from source**, including their helper/test experiment; no shortcut accepted or committed. The strongest wait-elision experiment did not turn fewer waits into higher end-to-end throughput.
- Run p95ms: base1/2 23.279/24.079; DMA1/2 23.314/23.477; safe1/2 23.456/23.289; dirty1/2 23.709/23.680. Hitches>50/>100: base1/2 1/0 each; DMA1 2/1,DMA2 1/0; safe1/2 1/0 each; dirty1 1/0,dirty2 2/1. Runtime pipeline build reports respectively6/5,6/5,6/5,5/6. No stutter reduction established. Frozen `bin-host60-probe2` retains these *rejected* experiments only for reproducibility.
- Final narrow safe candidate: reuse existing f17682097a mapping snapshots for **large** `GetSubmappedRangeImpl` queries (small queries keep the page walk). Current baseline profile attributes1.79% of back samples to public GPU subrange queries and1.30% to device subranges from FlushCaching. Snapshot entries gain their already-known device base; query clipping/holes/device discontinuities must match the page walk. Full #374/#562/#670 diffs checked; these are already present or superseded. Both repositories' PR search for “submapped” found no candidate; issue searches also checked.
- New mapping tests compare2048 randomized queries per big-page size with a page-wise oracle, plus known sparse/discontinuous/partial-unmap answers. Measurement-only240-present phase switches for page-filter and mapping-run paths (4 frames at each phase edge excluded in analysis) will isolate small gains without cross-boot noise. All measurement code will be removed before any accepted commit. No accepted code fix yet.

## 2026-11-06 19:55: probe 3 stopped; PR #224 merged into the owner build

- The perf agent stopped on usage limits (openai, then opencode funds, then anthropic key, then the
  claude-agent 5-hour limit). Probe 3 produced **no accepted fix**: `page_filter.h` (#250) and the
  large-query mapping-snapshot reuse are uncommitted in `lichii-perf60` together with measurement-only
  code (`nx2_host60_ab.h`, `EDEN_NX2_PRESENT_TRACE`), and neither was benchmarked.
- Probe 3 build/tests (`host60-probe3-build.log`, `host60-probe3-tests.log`, 19:06): build exit 0;
  tests 362 cases / **361 passed, 1 failed**, 2 failed assertions. The failure is the new oracle case
  "Submapped mapping snapshots match a page-wise oracle after remapping" at
  `memory_manager_runs.cpp:150` (snapshot returned 2 runs where the page walk returns 3), so the
  mapping-snapshot change is not correct and ships nothing. The halted work stays out of the owner build.
- Probe 2 verdicts stand unchanged: DMA/await shortcuts +1.31% mean FPS over 2 interleaved pairs
  (46.635 -> 47.245 FPS, p95 unchanged/slightly worse, hitches unchanged) is below cross-boot noise and
  was rejected and removed from source; #250-only was 46.971 (also noise). No wait-elision code is kept.
- Every accepted perf finding is already in the owner build: `5ce57b84aa` (native-bank direct refresh,
  +2.5% FPS in 4/4 interleaved A/Bs) is an ancestor of `ff0de3d0a1`, `9d940ae916` and the current HEAD.
  Nothing from the perf worktree needed merging.
- Merged **zortos293/Lichii #224** (upstream squash `a7a281757d`, opt-in `EDEN_NX2_ASYNC_DISCARD`) into
  `oot-1080p` as **`cc9e7153e7`**, `git cherry-pick -x`; 4 files, +54/-4. Native draws may now use the
  legacy async-shader discard heuristic instead of stalling the GPU thread on a pipeline still building,
  the path behind the 60 -> ~20 FPS drops the PR reports for this game. Default stays **off** (a skipped
  native draw can be missing for a frame or more, so this is a visual trade, not a default-on shortcut);
  enabling it is `EDEN_NX2_ASYNC_DISCARD=1`, which logs the switch and a skipped-lookup count every 256.
- Build: `cmake --build build --target lichii eden-cli tests --parallel 12`, exit 0. Focused gate
  `tests "[async_draw]"`: 15 cases / 243 assertions passed, including the new opt-in policy case.
  GUI `build/bin/lichii` reports `cc9e7153e7-oot-1080p`; rollback `build/bin/lichii-before-pr224-20261106`
  is the previous `c932007ba9` binary (overlay commit, unchanged).

## walk-bench subagent — 2026-11-07

- Building `/home/loganp/oot-dumps/perfscripts/walk_bench.py` with copied configuration,
  cache and NAND, atomic shared lock, deterministic stick frame counts, JSON/text output and
  interleaved A/B runs. No owner configuration/cache/save writes.
- Initial capability check on requested `/home/loganp/lichii-1080p/build/bin/eden-cli`:
  boot log reports `oot-1080p-cc9e7153e7-oot-1080p`, not the brief's newer source HEAD.
  Debug request `frame_times` returns `unknown tool frame_times`; binary lacks the
  `NX2 present sample` format string used by perf60.py's interval parser. Required median,
  percentiles and hitch measurements cannot truthfully be obtained from this binary.
- Existing server serialises `input_sequence` and `screenshot`; no scheduled screenshot API.
  Next-present boundary screenshots can be bracketed by present counts, not called frame-exact.
  Ten-second pipeline log aggregates likewise cannot yield exact per-window build counts.
  HARNESS_READY will not be created until these acceptance gaps are resolved.
- Calibration evidence: `/home/loganp/oot-dumps/walkbench/camera-calibration.before.png`
  shows Link in gameplay in Kokiri Forest. `right60.png` shows the opposite view with
  Link's face and the ladder/treehouse behind him. `left60.png` is short of returning;
  `left75.png` (60 + 15 frames) is near the initial view. A separate `right75.png`
  again shows the opposite view. Right-stick control works without Z-targeting, but
  exact repeatable 180° rotation is not proven. Trial turn length is 75 frames.
- First cold-cache probe needed about 239 s before runtime draws and hit the original
  240 s readiness timeout. The next probe used its isolated warmed cache and reached
  gameplay. Startup timeout is now 900 s. At 1200 presents the captured image was still
  the loading screen, so the trial gameplay gate moved to 2000 presents, followed by
  300 neutral settle frames. No GUI or owner process was stopped.
- Focused proof: `python3 -m unittest -v test_walk_bench.py` passes all three tests
  (window filtering, hitch thresholds, missing-vs-zero metrics, config default override).
- Three-run batch could not launch: integration Ninja PID 19684 was actively compiling
  `eden-cli lichii tests` in `/home/loganp/lichii-amd/build`. The harness correctly waited
  without launching an emulator. Cancelled only my own waiting wrapper PID 23276 with
  SIGINT and released the lock rather than claiming a three-run validation. No run table
  or spread exists. Detailed capability/calibration evidence is in `walkbench/capabilities.json`
  and `walkbench/calibration.json`; screenshots in `walkbench/calibration-contact.png`.
- Orchestrator authorised completing measurement support: created `/home/loganp/lichii-base`
  branch `bench/base-d04e6f396e` from owner d04e6f396e; cherry-picked upstream #182
  1d7fe9b7d8 as d015d086c7 and #301 c87f09a84c as 79cc856e97. Only conflict was the
  AGENTS tool list; resolved by preserving existing methods and adding frame_times.
  Fresh configure reused owner CMake options through the merge agent's copier.
- Baseline build queued behind existence of integration CLI and the shared lock/active
  compiler gate, `nice -n 19 ... --target eden-cli --parallel 6`; freeze target is
  `/home/loganp/oot-dumps/bin-base-d04e6f396e/eden-cli`. No owner-tree build or mutation.
- #182's public frame_times handler blocks the input server and exposes only 120 rounded
  samples. Added `present_ring.py` to read the same 4096-entry nanosecond ring read-only
  from the harness's own Popen child via Linux /proc/PID/mem and its unstripped ELF symbol.
  This preserves every interval and needs no integration source changes. Segments retain
  raw intervals, monotonic/script timestamps, and individual #301 trace BEGIN/END lines
  with per-segment byte ranges; neutral screenshot gaps are excluded. Four unit tests pass.
- Three-repeat calibration queued after baseline freeze: tests both right-stick turns and
  the approved deterministic alternative (turn Link with left-stick down, then ZL camera
  reset). Five boundary screenshots remain between released-stick input segments, as
  expressly accepted by the orchestrator; capture bounds remain honestly recorded.
- Baseline CLI built successfully (1657 Ninja steps) and frozen. The first integration
  calibration exposed KDE's asynchronous top-level resize: immediate geometry verification
  observed 1280x720 before the requested 1920x1080 configure event. Restored perf60's proven
  one-second post-resize settle. This failure produced no benchmark measurement.
- `calibration-amd-2` successfully captured all three trials of each camera alternative,
  at integration build cdb70d6c7480b4bf897003083defc5ee9b9bd749. Contact sheet:
  `/home/loganp/oot-dumps/walkbench/calibration-amd-contact.png`. Right-stick turns repeat
  substantially better; Link-turn/ZL trials drift and are rejected. 75-frame stick holds
  appear short of 180°, so `calibration-amd-3` repeats 90-frame holds three times.
- Five focused Python tests now pass, including an actual read-only timestamp-array read
  from the test's own child process. Frozen baseline exposes #182's ring symbol with expected
  0x8000-byte size. Both walks now end with 30 fixed neutral frames before screenshots, so
  camera/character easing finishes on a fixed input schedule instead of variable RPC gaps.
- `calibration-amd-3` (build 4fbfe07c16545effdac2bc1bee2a07c83e372805) confirms 90-frame
  right-stick turns in three repeats: Link faces the camera in opposite views and the
  initial scene returns after the reverse hold. Contact: `walkbench/calibration-90-contact.png`.
  Selected 90 active +30 neutral frames per turn, and 120 active +30 neutral per walk,
  total 540 scripted intervals. No ZL fallback is retained in the benchmark path.
- Initial end-to-end `validated-v1-amd1` acquired exactly 540 raw intervals, 52.122 FPS,
  median/p95/p99 16.923/25.695/35.119 ms, 8/1/0 hitches >33/50/100 ms, boot count19502,
  zero scripted-window pipeline builds. However full-stick walking reached the ladder,
  so this trajectory is rejected, not the accepted scenario. Cancelled only my own
  waiting/booting validation wrapper and its emulator before the baseline measurement.
- Final walking input is half-strength [0,0.5] for 120 frames: walking, not running, so
  the scenario stays clear of the ladder. `validated-v2` restarts all four measurements
  on this same sequence and a frozen integration executable, with identical cache copies.
- Final `validated-v2` completed three AMD runs plus one baseline, exactly 540 raw
  intervals each, identical input SHA-256 5484526965b6107976e02621342b1eec75aa33f8500d03e3576db6b660e82b53.
  AMD @4fbfe07c1654: FPS53.471/53.804/54.065, median17.219/17.102/17.222 ms,
  p95 22.253/22.054/21.750 ms, p99 34.246/36.697/36.535 ms. Window builds0/0/0;
  boot pipelines19502 each. FPS range0.594 (1.105% of mean), population CV0.452%.
  Settle first/last-half median drift<=0.431%, no settle hitches>50ms, supporting300frames.
- Base @79cc856e97 (d04e6f396e + measurement-only #182/#301): FPS54.489,
  median16.962/p95 24.470/p99 41.286 ms, 126 window builds, boot19540.
  **Not a controlled visual-parity comparison:** first turn takes2.361s with100builds
  vs AMD2.058–2.064s and camera/path drift is visible. Remaining segments build2/13/11.
  Identical input frames do not guarantee guest-time-independent camera/world state under
  stalls; A/B outputs explicitly require boundary PNG review before drawing conclusions.
- Final visual proof `walkbench/validated-v2-contact.png` shows matching landmarks/path
  across all three AMD runs (wind/Navi/particles differ), and the baseline drift. Detailed
  results/caveats/table in `/home/loganp/oot-dumps/walkbench/VALIDATION.md`. Harness,
  README and usage marker `/home/loganp/oot-dumps/perfscripts/HARNESS_READY` published.
  All benchmark children stopped/locks released; current shaderpack lock left untouched.
- Final focused gate: six Python tests pass in0.015s, including A1/B1/A2/B2 ordering,
  variant/common option propagation and reading actual owned-child timestamp memory.

## shaderpack agent — 2026-11-07

- Pacman log `/var/log/pacman.log:8162,8230`: Mesa upgraded 26.2.5 -> 26.2.6 at 2026-11-06 23:33:31 UTC, RADV at 23:33:38; BEFORE both reported owner sessions. Both owner logs report RADV 26.2.6. The Frizti port did not change graphics key layout/Refresh.
- Pack has 19,428 entries (17,719 graphics, all driver3/0x06802005, +1,709 compute); evidence cache is 31,298,996 bytes, 952 entries (844 graphics all driver3/0x06802006 +108 compute), parses exactly to EOF. Of the 844 graphics, ZERO effective-key hits against pack, but 840/844 match exactly when only driver bytes are ignored (843/844 shader-set hits); all 108 compute keys are already in pack. This is direct driver-update miss evidence, separate from the cache disappearing.
- Session2 boot1 15,870 + 3,558 skipped = 19,428: count reduction is boot selection, NOT pack entry loss at that point. Used-list filtering/aging predates port. Runtime bursts precede the first eviction; old log at t492.524 later evicts 17,844 and keeps 1,913. The Frizti 8,000 threshold only gates starting eviction, not a lower bound on retained count.
- Source audit: transferable writes append, only I/O/header errors delete its exact file (with error logs); used-list truncates only used file. CopyCompatible does not overwrite existing destination or remove sources. AdoptLegacy is not used in Vulkan loader. Neither reaches `.before-shaderpack` files or vulkan.bin. Owner second GUI boot creates a fresh renderer/cache and has no cache-read filesystem errors. Current cache inode birth23:59:52 means recreation, not an in-place shrink. Exact deleting actor is not attributable from supplied logs; external selective deletion remains possible. Archive.zip original birth16:10 remains, against whole-folder deletion.
- Read upstream Eglantine #708/#723 and zortos #313/#408/#467 diffs. #708 / `0ba4606e27` solves native preload/eviction: build all Switch2 at boot and keep them. Preparing port plus existing loader-restamp.patch with backward-compatible recorded used hash matching.
- Owner cache untouched. Isolated reproduction tree `/home/loganp/oot-dumps/shaderpack/repro-20261107/` prepared; initial benchmark attempt correctly refused because harness agent held bench lock / eden-cli.

## merge-zortos subagent — 2026-11-07

- Fetched zortos first: nx2 advanced from requested `1467051856` to `1d86dd61e0` (#497 microphone follow-up); merged the fetched branch into `/home/loganp/lichii-amd`, `amd/oot`, from `d04e6f396e` (59 upstream commits). Initial merge commit `780a502ca0`, final verification/amend pending.
- Resolved all 17 textual conflict files and silent duplicate pinning/instance-loop auto-merges. Upstream Qt pinning control defaults ON; `EDEN_NX2_PIN_THREADS=0/1` overrides off/on. Shared upstream performance-core planner/Windows CPU sets plus our Linux sysfs/allowed-affinity path; Linux Intel hybrid PMU identifies P-cores (host cpu_capacity is 1024 for both P/E). Expected host front2-3/back4-5/guest0-1,6-15, not E-only guests. One implementation remains.
- Cache v53/record/key layout unchanged; #192 already agrees with our guarded/task-mesh serialization. #178 keeps one upstream multi-method instance-loop tracker. #214/#425/#438/#439/#429 retained; #433 overlaps our broader sparse/host-limit-safe runtime gathers, retained with adapted upstream tests. #215's stronger pending-query wait also applied to our DMA snapshot flush path.
- Fresh build of eden-cli/lichii/tests queued at nice19/parallel6 behind the harness bench lock; no game boot, owner build, owner INI/cache writes or push. Full conflict/decision report: `/home/loganp/oot-dumps/research/merge-zortos-1467051856.md`; configure/build logs in the same directory. Results pending.

### merge-zortos completion — 2026-11-07

- Final merge **`c631e038966bd4a69957e9d308a10c1055f93041`**, parents `d04e6f396e` + fetched `1d86dd61e0`. All 17 conflict decisions and silent overlaps are documented in `/home/loganp/oot-dumps/research/merge-zortos-1467051856.md`.
- Cold clang23.1.4/Ninja RelWithDebInfo build **PASS**, eden-cli/lichii/tests plus all auxiliary test targets, nice19/parallel6. Full CTest **5/5 PASS**, 19.91s: main859 cases/69,775,229 assertions; SDL4/57; updater8/99; config10/474; Qt compatibility27 passed. Known video-render/uniform-vote failures are now fixed upstream, none remain. Python CPU-only tools62/62 pass; optional Python Vulkan tests unavailable (missing vulkan package).
- CTest found a silent production merge regression: our restart-time direct device_memory.emplace bypassed upstream #330's validation and reached kernel slabs with a null guest backing pointer. GDB confirmed the stack; unified allocation validation in AllocateDeviceMemory fixes both initialization paths and upstream failure/retry tests pass. Also adapted the remaining upstream TLD4.B test to live offset copies and corrected stale config/debug-method expectations; no config behavior changed.
- Proof caveat: three concurrent shaderpack edits (pipeline_cache_files.cpp, vk_graphics_pipeline.h, vk_pipeline_cache.cpp) were preserved, deliberately excluded from the merge commit, and included in the verified integration working tree. No game boot, owner configuration/cache writes or push. Logs: `merge-build-verified.log`, `merge-ctest-verified.log`; metadata-only final SHA restamp/recheck in `merge-build-stamp.log` and `merge-ctest-final.log`.

- Final SHA restamp/rebuild **exit0**, `GetSCMRev=c631e038966bd4a69957e9d308a10c1055f93041`; repeat CTest **5/5 PASS**, 16.39s, main859 cases/69,853,254 assertions (randomized count differs between runs). During this pass the shaderpack agent committed driver restamping as `dfb49dd9a6` on top of the merge; its preload/eviction work remains unstaged in vk_pipeline_cache.cpp. Left both untouched. Report updated with the final state and verification qualification.

- Shaderpack fixes committed on amd/oot after final merge: `dfb49dd9a6` normalizes compatible graphics keys to current driver at load, stores driver-neutral used hashes, accepts recorded/current legacy hashes, and adds two cache regression tests; `4fbfe07c16` ports Eglantine #708 /0ba4606e27 (preload/retain all Switch2 pipelines). No version/layout change, no async-discard change, no disk pack rewrite.
- Build PASS: fresh reconfigure and eden-cli/tests parallel6, `oot-dumps/research/shaderpack-build.log`. Focused cache regressions PASS:94 assertions in11 cases (`tests '[driver_restamp],[pipeline_cache_files]'`). Full working-tree CTest previously PASS5/5 with the exact source, main859 cases/69,775,229 assertions (merge agent's verified logs). Runtime proof pending.
- Additional timeline: Fish history2089-2090 records reinstall via `python3 install_shader_pack.py` at epoch1794009018=23:50:18 UTC. Installer chooses majority stamp in the used-list intersection before its newest-key fallback; large old-driver pack can dominate and make it stamp26.2.5 again after an update. Current owner cache has only26.2.6 keys, so restoration will have an unambiguous current stamp. Loader fix avoids future dependence on installer stamp inference.
- Isolated two-boot proof running with PRELOAD_ALL_PIPELINES=0, copied config/data/save/cache, sentinel legacy and before-shaderpack files, cache prefix SHA/inode/size checks, and graceful X11 WM_DELETE_WINDOW shutdown (not SIGTERM). Separate pre-port CLI old-stamp/current-stamp comparison queued. Owner cache still untouched.

- Pre-port controlled A/B completed with the existing CLI `cc9e7153e7` (not the newer measurement baseline): same fresh19,428-entry pack, copied save/config, current host RADV26.2.6, same frame-script to3,600 presents. OLD5 stamp:649 runtime builds reported and649 new serialized graphics entries. CURRENT6 stamp:0 builds in coarse10-second reports, but2 new graphics entries serialized (do NOT call this literally zero builds). Both bootcounts19,428; both exit0 gracefully; both preserve original911,739,333-byte prefix SHA, same inode, and legacy/backup sentinels. Thus driver mismatch alone reproduces the600-build burst before Frizti. Files:`shaderpack/validation/preport-{old,current}-stamp/{results.json,boot1.out,boot1.png}`.
- Fixed 2-boot attempt1 never launched: shared harness validation held bench.lock beyond its20-minute acquisition timeout. Retrying as fixed-two-boots-v2 with event-driven lock acquisition. This is scheduling, not an emulator/cache failure. Isolated source used list now deliberately EMPTY, PRELOAD_ALL_PIPELINES=0, so first fixed boot must still load all19,428 (old policy would load4,709). Owner cache untouched.

- Fixed two-boot proof PASS on4fbfe07c16: first boot19,428 entries, second19,430 (first session added2 entries); final cache parses19,432 (4 new:2 compute+2 graphics). Both exit0 via WM_DELETE_WINDOW. Cache sizes911,739,333 ->911,816,858 ->911,966,199; same inode and original911,739,333-byte SHA prefix preserved both times; legacy and before-shaderpack sentinels unchanged. The old-stamp pack was NOT rewritten on disk. Loader restamps17,719 graphics each boot, rejects0 for feature mismatches, skips0 at boot. First used list EMPTY and PRELOAD_ALL_PIPELINES=0: demonstrates upstream#708 policy, not env forcing. First present245.26s cold /28.20s warm.10 driver BEGINs after first present per boot, no600-build bursts; coarse10s reports contain0 lines, not proof of literally zero builds. Screenshots show Link, grass, canopy, fog and objects. Files:`shaderpack/validation/fixed-two-boots-v2/{results.json,boot1.out,boot2.out,boot1.png,boot2.png}`.
- HARNESS_READY received; running official walk_bench.py cache-fixed-walk then cache-d04-walk from the same pristine old-stamp fixture. Added only the fixed test's local current-driver host pipeline blob to the ISOLATED source fixture to avoid repeating a4-minute cold preload; transferable pack stays exact19,428 old-stamp entries. This is build-count/correctness proof, not an FPS comparison claim. Owner cache still untouched pending this last check.

## flash-bloom subagent — 2026-11-07

- Task: goal item 5 (flashing lights / huge bloom in some OoT zones, more since the Frizti v3 port).
- Detector: `/home/loganp/oot-dumps/flash/scripts/flash_hunt.py NAME ROUTE [KEY=VAL]` reuses walk_bench
  isolation/lock/boot, captures every present (`EDEN_NX2_FRAME_CAPTURE=/dev/shm/..`,
  `EDEN_NX2_FRAME_CAPTURE_EVERY=1`, existing nx2_gsv capture, no emulator change), scores each frame
  (mean luma vs median of the previous 8; near-white min(RGB)>245 pixel spikes), keeps flagged frames
  plus predecessor as PNG in `/home/loganp/oot-dumps/flash/runs/NAME/`. Owner GUI visual settings that
  differ from the CLI config are copied in by default (notably `game_dlss_network=true`,
  `anti_aliasing=2`, `scaling_filter=15`); `OWNERVIS=0` turns that off.
- Upstream notes read: Eglantine skill doc (visual-bug-investigation.md): OoT white wash = culled
  vegetation (storage buffers behind loaded pointers), one-frame material flashes = stale native
  uniform bank (`EDEN_NX2_BANK_REUPLOAD`), LOD vegetation flashes = rejected compute programs.

## amd-ab subagent (Frizti v3 features on AMD) — 2026-11-07

- Harness findings before any A/B: walk_bench.py runs the CLI with `MESA_VK_WSI_PRESENT_MODE=immediate` and the copied `sdl2-config.ini` has `use_vsync=0` (Immediate) — the owner's GUI runs Mailbox (qt-config `use_vsync=1`, async_presentation=true). So the pacer / present-mode questions cannot be answered with the stock harness; a copy `perfscripts/walk_bench_vs.py` adds `VSYNC=<0-3>` (sets use_vsync, drops the Mesa override) and `ASYNC_PRESENT`/`VIDEODRIVER`.
- Host pacer root cause #1: `QueryRefreshPeriodNs()` (src/video_core/renderer_vulkan/vk_host_pacer.cpp) returns nullopt everywhere except `_WIN32`, so `refresh_known` is never true on Linux, `HostPacer::pacing` stays false, `g_host_refresh_ns` stays 0 (no genlock either). Harness logs: "Host pacing off: ... at 60.000 Hz" (the unset default). Present wait is supported ("Host pacing: present wait supported") so only the refresh period is missing.
- Mean-vs-median question (validated-v2): AMD walk2 segment sits at a steady 20.8-21.2 ms (146/150 intervals in 19.5-23 ms), the baseline's walk2 averages 18.7 ms with only 35/150 there. The baseline's camera/path drifted into a lighter view (see VALIDATION.md), so its higher mean FPS is a scene difference, not an emulation speedup; AMD has the better tail (hitches 6-8 vs 16, p95 22 vs 24.5). Segments turn1/walk1/turn2 are equal within noise (AMD 17.1/17.9/18.2, base-excluding-stall 17.96/17.1).
- DMA snapshot log on AMD (DMA default 9): ~70% reads skipped, all misses "written since" (same as owner log).

- DMA snapshot A/B (stock harness: Immediate, frozen build/bin/eden-cli 4fbfe07c16, pinning default-on, 3 interleaved pairs, `walkbench/amdab-dma-{A,B}{1,2,3}`): default(9) FPS 53.35 (52.24-54.31), median 17.18, p95 22.27, p99 35.9, ±1ms 42.4%, sd 4.83, hitches>33 8.0/>50 1.3; EDEN_NX2_DMA_SNAPSHOT=0: FPS 52.82 (52.42-53.22), median 17.54, p95 23.17, p99 34.8, ±1ms 32.3%, sd 3.90, hitches 6.7/0.7. +1.0% FPS (inside the 2% run noise), p95 -0.9 ms. Final log lines: ~900k reads skipped / ~400k drained (69% hit), 0 differed, 393 new page + ~408k "written since".
- Pinning facts (probe runs `walkbench/probe-pin-{amd,base}.meta.json`, /proc/PID/task/*): amd/oot with `pin_gpu_threads=true` written explicitly in the config: log `GPU thread pinning: front CPUs 2,3, back CPUs 4,5, guest CPUs 0,1,6-15`; GPU=2-3, GPUBack=4-5 (+ DeviceMemory:Co thread 4-5), CPUCore_0-5 / TimeWorker / am:EventObserve / nx2:ColdHint = 0-1,6-15; everything else (VulkanWorker, VulkanPresent, VulkanPresentWa, VkPipelineBuild x19, audio, SDL, WSI swapchain threads) 0-19 (E-cores included). Baseline d04e6f396e (env-driven, default on): same front/back, but guest cores/TimeWorker/ColdHint = 0-1,6-19 (E-cores 16-19 allowed).
- harness fix in the copy `perfscripts/walk_bench_vs.py`: `[Renderer] pin_gpu_threads\default=false` + `pin_gpu_threads=true` is written explicitly (a key without `\default=false` is ignored by the config reader), `PIN_SETTING=0` flips it, metadata records `pin_gpu_threads_config` and `thread_affinities`; `Common:Info` added to the log filter.

- **Harness/owner mismatch found (important):** the owner's desktop entry runs `/home/loganp/bin/lichii-oot-test.sh`, which exports `EDEN_NX2_VSYNC_HZ=120` (the guest vsync event, nx2_gsv.h StartVsyncTimer; default 60) and `EDEN_NX2_OOT_VISUAL_WORKAROUND=0`. walk_bench.py strips every inherited `EDEN_NX2_*` and only sets the workaround, so the walk harness (and validated-v2 numbers) run the guest at a 60 Hz vsync tick while the owner plays at 120. A frame that misses a tick idles until the next one (16.7 ms vs 8.3 ms quantum) which is exactly the 16.7/20.8/25 ms clustering. All amd-ab feature A/Bs after the first (DMA at 60 Hz, kept as `amdab-dma-*`) therefore run with `EDEN_NX2_VSYNC_HZ=120`, `VSYNC=1` (Mailbox, Mesa override removed) and pinning explicit-on. The first pin A/B (60 Hz, Immediate) was discarded after one pair member (`amd-ab/discarded60/`).
- Owner changed the approach (12:46 UTC): no more scripted wandering. The one scripted probe run
  (`flash/runs/amd-k1`, amd/oot 4fbfe07c16, 1589 captured presents) found no flash; its 9 flags
  were a gradual brightening while the camera tilted to the sky (false positives of my simple
  detector), which supports the owner's point.
- Work tree: `/home/loganp/lichii-flash`, branch `flash/oot` from amd/oot 4fbfe07c16 (the tuning
  agent has uncommitted edits in lichii-amd). Commits so far: f70fbb13ae port of Frizti cafd28d7dc
  + a9ca4c53ea (flash watch); cfb752a04b port of zortos #351 (b72195422b, table-pointer pools bound
  past 64 MiB: OoT's fog light probes lie up to ~130 MiB past the pool pointer, the cells past
  64 MiB gave the fog an overbright constant = white haze over far field/sky); 6fee965c5b port of
  zortos #432 (2e69168aae, NMin/NMax/NClamp: RADV returns NaN from FMin/FMax/FClamp, bloom spreads
  it over the screen, issue #325). Both landed in zortos nx2 today, after our merge point
  1d86dd61e0. #432 ported WITHOUT its cache version bump 53->54 (transferable cache stores guest
  envs, SPIR-V is re-emitted at load; a bump renames the cache file and drops the owner's pack).
- Upstream issues matching the owner's report: zortos #483 (blinding bloom streak, Kakariko, Frizti
  v3), #386 (overexposure lower part of Saria's house), #505/#507/#508 (white sky, build 95,
  AMD), #325 (AMD NaN haze), #392/#419 (AMD bloom/flicker). Frizti test/no-sky-fold 36e8c6b7d4 was
  a throwaway test; the IMul32 0/1 folding stayed (Duck912 954985d351, in our tree): it binds the
  sky-ambient SH buffer the materials and fog read.

## 2026-11-07 13:30: amd/oot handed to the owner (pacer build 8a6cff1987)

Owner asked to stop the tuning/flash agents and ship a build to play. amd/oot now:
c631e03896 (zortos/nx2 through 1d86dd61e0) + dfb49dd9a6 (driver re-stamp at load) +
4fbfe07c16 (retain prebuilt pipelines) + 7329839b95 (#530 Linux GPU-thread pinning; on by
default, EDEN_NX2_PIN_THREADS overrides; P-core list from
/sys/bus/event_source/devices/cpu_core/cpus because cpu_capacity reads 1024 on P- and
E-cores alike on the i7-12700K) + 0a921a899e (#463 skip stale uploads,
EDEN_NX2_TC_SKIP_STALE=0) + 739093ccd2 (#351 table-pointer pools past 64 MiB) +
7b3f82c0a1 (#432 NaN-defined min/max/clamp, no cache version bump: guest shaders are
cached, SPIR-V is not) + 8a6cff1987 (host pacer gets the display refresh on Linux).

8a6cff1987 is UNVERIFIED at runtime by design: the tuning agent's measurement found
QueryRefreshPeriodNs() returns nothing outside Windows, so Frizti's host pacer and the
guest frame-clock genlock never engaged on Linux ("Host pacing off ... at 60.000 Hz").
The frontends now publish QScreen::refreshRate / SDL_GetCurrentDisplayMode to
Nx2Perf::g_display_refresh_hz; EDEN_NX2_HOST_REFRESH_HZ overrides it, EDEN_NX2_HOST_PACE=0
disables pacing. Pacer gates: present mode Fifo/Mailbox/FifoRelaxed, speed limit on and not
Turbo, frame gen off, async presentation on (default) for the present-wait thread.

Not integrated: flash/oot f70fbb13ae (flash watch; compiled, never run) and the flash-watch
additions in a stash; tuning agent's DMA-snapshot diagnostics in another stash.
Owner build: /home/loganp/lichii-1080p/build/bin/lichii = 8a6cff1987-amd/oot, rollback
lichii-before-amdmerge-20261107 = d04e6f396e-oot-1080p. Shader cache stays v53
(19,428-entry pack loads, 0 builds in the walk window on 4fbfe07c16).
Measured before the last three commits (walk_bench, 540 frames, 3 runs): amd 53.5-54.1 fps,
p95 21.8-22.3 ms, hitches >33/>50 ms 6-8/0-1, 0 window builds; baseline d04e6f396e 54.5 fps,
p95 24.5, 16/4, 110-126 window builds. Pinning on vs off: +4.2% fps (50.86 vs 48.79).

## FSR subagent — 2026-11-07

- Worktree `/home/loganp/lichii-fsr`, branch `fsr/oot`, base `8a6cff1987`; owner GUI tree,
  configuration and shader cache untouched. FSR 3 only; FSR 4 explicitly out of scope.
- Merged fetched zortos/nx2 `92f0212408` (43 commits) as `42bb52f18d`, preserving pipeline
  cache v53, Linux PMU P-core discovery and display-refresh host-pacer fix. Conflict report:
  `/home/loganp/lichii-fsr/docs/nx2/oot-fsr-merge.md` (all ten files and duplicate ports).
- Ported #644 effects capture/bias (`82494c4820`, `74a2d20c90`, `4352c06bbf`) as
  `f978071da1`; excluded unrelated NVIDIA preset/model-memory changes. Capture is generic
  scene-before/after effects plus motion history, useful as FSR reactive/composition masks.
- Confirmed #606 is wholly EGLANTINE_STREAMLINE-gated: not compiled on Linux and no FSR
  fallback. Current implementation work shares its camera/Link-motion/HUD contract with FSR,
  adding LDR, standard finite depth, unit-exposure variants rather than reusing Fire Emblem's
  HDR/reversed-infinite-depth constants. Off-safe Qt setting and existing fsr/native override.
- Fresh clang/Ninja build configured from amd/oot options. Initial build hit the tool's
  two-minute timeout (not a compiler failure); resumed with no timeout, nice19/six jobs.
  Evidence directory `/home/loganp/oot-dumps/fsr/`; runtime and performance not yet claimed.
- SDK 2.0.0 comparison: all changed fsr3upscaler GPU headers are copyright/path-only;
  actual negative-RCAS-output fix is in shared `fsr1/ffx_fsr1.h` (three small limiter changes).
  Private diffs retained under `oot-dumps/fsr/research/sdk-2.0.0/`; port after main path works.

## FSR subagent (continued) — 2026-11-08

- Smoke run fsr-smoke-01 was invalid (slot 1 walks into an entrance -> loading screens) AND the FSR
  path never engaged: with the built-in Max Resolution add-on (on in the owner's GUI and CLI
  configs) handheld renders 1920x1080 and runs output pass `c033c3ddc9f0897b` at 1:1; with it off,
  the 4/3 pass hashes `ef542df87cacfe9e` here, not #606's `e40f5efb5480e692`. Both have #606's
  E5B9G9R9 -> B10G11R11 contract; added (9fbab8fd54, 3c91ba6002). c033 is FSR-only (native AA).
- Harness: private wrapper `oot-dumps/fsr/fsr_bench.py` (SAVE_SLOT=2 default: Down at f520 before
  the 3rd A, verified at file select; OCARINA_FSR written with \default=false; MAX_RES=0;
  FRAME_GEN=1; VISUAL=1). Slot 2 spawns in the Kokiri Forest clearing (probe-e.png).
- Contract: fsr-s2-fsr-01 (MaxRes on) 3032 contiguous FSR evaluations 1920x1080->1920x1080,
  frames 1545-4576, 1 load-time fallback; fsr-mr0b-fsr (MaxRes off) 2954 contiguous evaluations
  1440x810->1920x1080, frames 1633-4586, 1 load-time fallback. Effects mask on all but the first.
- A/B fsrab1 (bin 9fbab8fd54, slot 2, MaxRes on, both 1920x1080 scene): native 49.69 FPS mean,
  FSR native AA 52.11 (+4.9%, pairs +2.5/+13.8/-1.1%): within run noise, no cost measured, no gain claimed.
  Table: oot-dumps/fsr/fsrab1.table.md.
- FSR3 frame gen (frame_gen_type=Fsr3, x2) starts with FSR upscaling on: host pacer 1034-1065
  host presents per window at 9.7 ms vs 500 at 17-20 ms without; FSR evaluations continue.
- Boot time: per-run empty Mesa cache made RADV rebuild 19,498 pipelines: preload ends at
  209-219 s, BOOT_FRAME 3500 reached at 290-301 s. Added walk_bench `MESA_CACHE=shared`
  (opt-in, /home/loganp/oot-dumps/bench-mesa-cache, seeded from ~/.cache/mesa_shader_cache):
  preload ends at 26.5-34 s, BOOT_FRAME 3500 at 102-106 s.
- SDK 2.0.0 RCAS lower limiter ported (cc9a423ba9); runs on bin cc9a423ba9/3c91ba6002 used it.

## 2026-11-08: FSR 3.1 for OoT reviewed, zortos merged, installed (cd4f727816)

fsr/oot = amd/oot 8a6cff1987 + zortos/nx2 92f0212408 (42bb52f18d: #606 OoT DLSS inputs, #616,
#587) + #644 effects mask port (f978071da1) + OoT FSR 3.1 (84baa07097, 5f6f9655f9, 9fbab8fd54,
83b443a07f, 3c91ba6002) + FSR 3.1.5 RCAS limiter (cc9a423ba9) + zortos/nx2 ce9be9d14a
(cd4f727816: #657, #659, #664; took #664's AmpereAnchor timestamp code unchanged). Cache v53.
#606 was Streamline-only (not compiled on Linux) and its pass hashes did not match RADV: added
c033c3ddc9f0897b (1:1 pass, Max Resolution on) and ef542df87cacfe9e (810p->1080p pass).
Enable: Graphics "Use AMD FSR 3.1 upscaling in Ocarina of Time" (default off) or
EDEN_NX2_DLSS_REPLACEMENT=fsr|native. Zero jitter (game doesn't jitter).
fsrab1 (5f6f9655f9, 3 pairs, 1920x1080->1920x1080): native 49.69 fps, FSR 52.11; pairs
+2.5/+13.8/-1.1%: no measurable cost. Merged build cd4f727816, one run each (slot 2, 1080->1080):
native 54.81 fps p95 21.19 hitches 8/0/0; FSR 54.45 fps p95 21.25 hitches 7/0/0; 0 window
builds; FSR evaluates from frame 1356 to the end, one native fallback during load.
Grids: oot-dumps/fsr/fsrvis-grid.jpg, mr0-grid.jpg, merge-cd4f-grid.jpg.
Harness: Mesa disk cache now shared by default (MESA_CACHE=isolated restores per-run): pipeline
preload 209-219 s -> 26.5-34 s; gameplay ~54 s after launch instead of ~235 s.
Owner GUI: build/bin/lichii = cd4f727816-fsr/oot; rollback lichii-before-fsr-20261108 = 8a6cff1987.
Not pushed: fsr/oot, amd/oot, oot-1080p, flash/oot (gh token lacks `workflow` scope; oot-perf pushed).

## 2026-11-08: three upstream PRs opened from prepared branches

- zortos293/Lichii #693 "common: use Intel's P-core list for GPU thread pinning on Linux hybrid
  CPUs" (base nx2 bf4c42e223, head 658d7cf7ac, branch pr/linux-pcore-list). Follow-up to merged
  #530; cpu_capacity is 1024 for P- and E-cores on the i7-12700K. Build ok, ctest 5/5.
- Frizti/Fritchii #13 "vulkan: give the host frame pacer the display refresh on Linux" (repo is
  Frizti/Lichii renamed Fritchii; base oot-gather-fix 72e5ad6c22, head 219c681e4f, branch
  pr/host-pacer-linux-refresh). Non-Windows path only, so it does not collide with Frizti #10.
  Build ok; 4 pre-existing Catch2 failures in src/tests/video_core reproduce on the base too.
- zortos293/Lichii #695 "Ocarina of Time: AMD FSR 3.1 in place of the game's anti-aliasing
  network" (base nx2 bf4c42e223, head 46cf26e5d3, branch pr/oot-fsr; 7 commits including the
  #644 effects-mask port and the FSR 3.1.5 RCAS limiter). Build ok, ctest 5/5. Body names the
  overlap with open #678 and that the #644 commit can be dropped if #644 lands first.
- Branches for everything else the earlier survey found (shader-cache re-stamp, overlay stats,
  RCAS PR to Eglantine, local nx2) were prepared and dropped per the owner's scope change.
- Pushing works with `git -c credential.helper= -c 'credential.helper=!gh auth git-credential'`;
  the gh token now has `workflow` scope, and fsr/oot, amd/oot, oot-1080p, flash/oot, oot-perf are
  on origin (the earlier "Not pushed" note is resolved).
