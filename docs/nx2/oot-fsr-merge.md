# OoT FSR integration: upstream merge (2026-11-07)

Base: `fsr/oot` at `8a6cff1987`. Merged `zortos/nx2` at `92f0212408` (43 new commits),
including #606 (`65e979adb3`), #616 (`f96cded333`), #587 (`95df0bc79d`).
No owner binary, config or cache is replaced by this work.

## Conflicts and decisions

| File | Decision |
| --- | --- |
| `src/common/thread.cpp` | Keep local LinuxProcessors Intel hybrid PMU P-core discovery: cpu_capacity alone misclassifies the owner's 12700K. #530's other compatible changes merge normally. |
| `src/common/thread.h` | Keep local description of the EDEN_NX2_PIN_THREADS override and six-performance-core guard. |
| `src/core/hle/service/nx2_gsv.h` | Retain local guest-interval publication and host-refresh genlock, alongside upstream FPS-limit documentation. Preserves pacer refresh fix `8a6cff1987`. |
| `src/core/hle/service/nx2_present_pacer.h` | Take upstream's expanded documentation; implementation unchanged. |
| `src/qt_common/config/shared_translation.cpp` | Retain override documentation for local pinning behavior; retain new upstream settings. |
| `src/tests/CMakeLists.txt` | Union: keep local runtime-gather tests and add upstream virtual-range-cache tests. |
| `src/video_core/gpu_thread.cpp` | Keep GpuSplit thread marking and add upstream Linux timer-slack reduction; both apply. |
| `src/video_core/renderer_vulkan/vk_graphics_pipeline.cpp` | Keep instrumented aligned texture-handle evaluation, which already implements #245; retain new mesh/compute pipeline logic. |
| `src/video_core/renderer_vulkan/vk_rasterizer.cpp` | Add upstream OoT DLSS path; keep only one existing TracePendingCpuWrites implementation. |
| `src/video_core/renderer_vulkan/vk_rasterizer.h` | Add OoT declarations; keep only one TracePendingCpuWrites declaration. |

## Clean merges requiring semantic review

- #432 and #351 were already cherry-picked (`7b3f82c0a1`, `739093ccd2`). Git coalesces
  their identical implementation changes; do not apply them a second time.
- Undo #432's cache-version bump: **VulkanPipelineCacheVersion stays 53**, preserving
  the owner's 19,428-pipeline pack. No serialization layout change is needed for that fix.
- #530 and #463 were already merged into this branch; shared ancestry/identical changes
  are retained without duplicate implementations.
- Upstream Eglantine was fetched too. It is a divergent source fork, not a linear
  ancestor update (392 local / 175 remote unique commits at the base); this task merges
  the requested zortos integration branch rather than importing unrelated Eglantine work.

## Follow-up review

PR #644's diff is retained privately at
`/home/loganp/oot-dumps/fsr/research/zortos-644.diff`. Its capture uses recognized guest
effects resources, not NVIDIA APIs, and its motion-reprojected current/history mask is
useful for FSR reactive/transparency inputs. Backend integration and verification are
recorded with the FSR change, independently of this merge.

As merged, #606's entire camera/motion/replacement path is behind
`EGLANTINE_STREAMLINE`: Linux does **not** compile it and it does **not** fall back to
FSR. Fire Emblem's existing separate FSR path assumes HDR/reversed infinite depth;
OoT requires LDR/non-reversed finite depth and pixel-space motion.
