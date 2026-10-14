# Hyper90 FPS Booster (`Hyper90FPSBooster.levipack`)

**Hyper90 FPS Booster** is a native C++20 `preload-native` optimization suite for **Minecraft Bedrock Edition on LeviLaunchroid (`arm64-v8a`)**, engineered to boost devices stuck at **60–70 FPS** to an **unbudgable, locked 90 FPS (11.111 ms frame time)**.

The ready-to-import package is included directly in the repository at:
- [`Hyper90FPSBooster.levipack`](./Hyper90FPSBooster.levipack)
- [`release/Hyper90FPSBooster.levipack`](./release/Hyper90FPSBooster.levipack)

---

## 13 Working Mod Menu Optimization Modules

Every module registers individually in **LeviLaunchroid's in-game Mod Menu**, supports real-time toggling and live sliders/controls without restarting the game, and persists its state to `config/config.json`:

| # | Module ID | Display Name | How It Boosts FPS to 90 |
|---|-----------|--------------|-------------------------|
| **1** | `hyper90.frame_pacer` | **1. 90 FPS Unlocker & Frame Pacer** | Hooks `eglSwapInterval` & `eglSwapBuffers` to force `interval = 0` (bypassing Android triple-buffered VSync 45/60Hz step-downs) and paces frames to an exact **11.111 ms (90.0 FPS)** cadence using `clock_nanosleep` + ARM64 sub-350µs `yield` micro-pacing and an **Anti-Stutter Spike Catch-Up Guard**. |
| **2** | `hyper90.engine_options` | **2. Bedrock Engine & RenderDragon Tuner** | Automatically discovers and pre-tunes Minecraft's `options.txt` before `MinecraftActivity` initializes Mojang's C++ engine (and hooks `fopen` on `options.txt`), configuring `gfx_vsync:0`, `gfx_max_framerate:90`, `gfx_msaa:1`, `gfx_texel_aa:0`, `gfx_fancygraphics:0`, `gfx_transparentleaves:0`, `gfx_fancyskies:0`, `gfx_renderclouds:0`, `gfx_smoothlighting:0`, and optimal chunk distance while preserving a `.hyper90.bak` backup. |
| **3** | `hyper90.cpu_scheduler` | **3. Big.LITTLE Prime Core & Thread Booster** | Reads `/sys/devices/system/cpu/cpu*/cpufreq/cpuinfo_max_freq` to identify Snapdragon/Dimensity/Exynos **Prime & Gold performance cores** (`>=75%` max clock), pins the OpenGL RenderThread via `sched_setaffinity`, lowers kernel timer slack to `10 µs` (`PR_SET_TIMERSLACK`), and boosts thread nice priority (`setpriority`). |
| **4** | `hyper90.gl_pipeline` | **4. OpenGL ES Pipeline & State Deduplicator** | Hooks `glHint`, `glEnable`, `glDisable`, `glUseProgram`, `glActiveTexture`, `glBindTexture`, `glDepthMask`, and `glFinish` to force `GL_FASTEST` shader derivative/mipmap hints, block `GL_DITHER` & `GL_MULTISAMPLE_EXT`, eliminate blocking `glFinish()` CPU-GPU pipeline stalls, and deduplicate redundant state/texture/shader binds. |
| **5** | `hyper90.tbdr_tile_saver` | **5. Adreno/Mali TBDR Tile Bandwidth Saver** | Calls `glInvalidateFramebuffer` / `glDiscardFramebufferEXT` on `GL_DEPTH_EXT` and `GL_STENCIL_EXT` attachments of FBO 0 right before `eglSwapBuffers`, preventing mobile TBDR GPUs from writing megabytes of transient depth/stencil tiles back to LPDDR RAM every frame, and coalesces packed `D24S8` `glClear` masks. |
| **6** | `hyper90.texture_lod` | **6. VRAM & Texture Mipmap Optimizer** | Hooks `glTexParameteri` & `glTexParameterf` to clamp `GL_TEXTURE_MAX_ANISOTROPY_EXT` to `1.0f` and rewrite expensive 8-sample trilinear mipmap lookups (`GL_LINEAR_MIPMAP_LINEAR`) into fast 4-sample bilinear/nearest mipmap filtering (`GL_LINEAR_MIPMAP_NEAREST`). |
| **7** | `hyper90.draw_batch_culler` | **7. Zero-Area & Micro-Batch Draw Culler** | Hooks `glDrawElements` & `glDrawArrays` to immediately drop degenerate (`count <= 0`) draw calls and adaptively decimate tiny non-UI micro-batches (`<= 18` indices) when instantaneous FPS dips below `88 FPS`. |
| **8** | `hyper90.entity_particle_guard` | **8. Entity Distance & Particle Budget Limiter** | Enforces a per-frame particle micro-batch budget (`max_particles_per_frame`, automatically scaled in Aggressive/Ultra governor tiers) and hooks `ActorRender::enableInterpolation` in `libminecraftpe.so` to throttle sub-tick entity interpolation under load. |
| **9** | `hyper90.hud_ui_accelerator` | **9. Zero-Alloc HUD & UI Batch Accelerator** | Dirty-checks hotbar slots, player vitals, and UI text strings using a zero-allocation 64-bit FNV-1a hash ring cache, disables UI screen transition animations, and provides an optional fast screen-center overlay crosshair. |
| **10** | `hyper90.memory_arena_tuner` | **10. Bionic Heap & Page-Fault Stutter Shield** | Tunes Android Bionic's Scudo/jemalloc allocator via `mallopt(M_DECAY_TIME, 15000)` to retain freed chunk-mesh pages in user-space cache and `mlock()`s the mod's hot state/telemetry arena in physical RAM to prevent minor page-fault frame spikes. |
| **11** | `hyper90.camera_input_90hz` | **11. 90Hz Sub-Frame Camera & Touch Stabilizer** | Hooks `LocalPlayer::applyTurn` and `Options::getGamma` in `libminecraftpe.so` to apply a critically-damped 90Hz exponential camera filter (with instant **Raw Flick Bypass** for 180° turns), enables Fullbright cave lighting to eliminate torch light-update remesh stutters, and sets `TCP_NODELAY` + `IPTOS_LOWDELAY` on game sockets. |
| **12** | `hyper90.adaptive_governor_hud` | **12. Adaptive 90-Lock Governor & Telemetry HUD** | Runs a 4-tier closed-loop governor (`LOCKED 90+` → `BALANCED 90` → `AGGRESSIVE 90` → `ULTRA 90-LOCK`) monitoring a 120-frame window of average FPS, 1% low FPS, and frame jitter, paired with a draggable in-game **90-FPS Telemetry & Frame Budget HUD** and 2 quick-action buttons (`90-LOCK` and `PURGE`). |
| **13** | `hyper90.frame_blend` | **13. Frame Blend (Experimental 1.5x / 2.0x Frame Gen)** | Inspired by *Mine Frame Gen*. Copies each finished frame into a texture, then presents an extra **blended frame** (`mix(current, previous, strength)`) before the real one via a second `eglSwapBuffers` (2.0x = one per frame, 1.5x = one every two frames). **Off by default.** |

### Module 13 notes (Frame Blend)

- It is **frame blending, not motion-vector frame generation**: expect ~1 display frame of extra latency and some ghosting on fast camera turns. Use *Blend Strength* to trade smoothness for ghosting.
- While enabled it **forces EGL swap interval 1 (VSync)** so every queued frame gets its own vsync; Module 1's sleep pacer stays idle. Needs a **90/120 Hz display** (real FPS x multiplier must fit the refresh rate). On a 60 Hz screen it only lowers real FPS.
- Real frames slower than 20 FPS are never blended. The HUD FPS counts every presented frame (generated ones included).
- Needs an OpenGL ES 3.0 context; if setup or a GL call fails, it disables itself and logs `Frame Blend disabled: ...` to logcat (tag `Hyper90`). Toggle it off/on to retry.
- Costs one full-screen copy per frame plus 1-2 full-screen passes on generated frames (bandwidth heavy at high resolutions).

---

## Installation in LeviLaunchroid

1. Download or copy [`Hyper90FPSBooster.levipack`](./Hyper90FPSBooster.levipack) to your Android device.
2. Open **LeviLaunchroid** → **Mods** → **Import `.levipack` / `.so`** and select `Hyper90FPSBooster.levipack`.
3. Enable **Hyper90 FPS Booster** and launch Minecraft.
4. Open the **LeviLaunchroid Mod Menu Overlay** in-game to customize any of the 13 modules, or use the **HUD Editor** tab to drag and snap the live 90-FPS Telemetry Card anywhere on screen.

---

## Building & Running Tests

```bash
./build.sh
```
This compiles and runs the host verification test suite (`tests/test_hyper90.cpp`) verifying all 13 modules, regenerates `config/config.json` and `config/config.schema.json`, compiles the Android `arm64-v8a` shared library (`release/libhyper90_fps_booster.so`), and packages `Hyper90FPSBooster.levipack`.
