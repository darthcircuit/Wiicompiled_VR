# WiiCompiled VR on Meta Quest (standalone Android)

This document is the design and build reference for the native Quest build. It
complements `OPENXR.md`, which remains the specification for the presentation
policy, the virtual screen, the first-person camera and frame interpolation:
all of that is shared, unchanged, between the Windows D3D12 product and the
Quest Vulkan product. What differs is everything below the stereo replay: the
graphics binding, the app shell and the platform glue.

## Sources of the design

- **KartPad** (`kartpad-main/`, the `kartpad-android` runtime branch of the
  WiiCompiled fork) proved that the translated game runs on Android arm64 with
  Aurora on Dawn/Vulkan under SDL3's `SDLActivity`. Its lessons carried over:
  the products are shared libraries SDL loads, the activity exports its
  directories through the environment before native code runs, the
  Windows-generated blob assembly needs its section syntax rewritten for ELF,
  and Dawn's android-aarch64 prebuilt package needs one path rewritten.
  KartPad's Android fiber/JNI split (never calling Java-backed SDL APIs from a
  guest fiber stack) is respected here by keeping every OpenXR call on the
  dedicated pacing thread, which is a real `std::thread`.
- **DolphinXR** (`Dolphin-OpenXR-2/`, quest flavour) supplied the Quest-side
  specifics: the loader must be initialised with the *activity* as its
  context or the session never leaves `IDLE`; `XrInstanceCreateInfoAndroidKHR`
  must be chained on instance creation; the manifest needs the Khronos runtime
  broker queries, the `OPENXR_SYSTEM` permission, the `com.oculus.intent.category.VR`
  intent category and the `XR_ACTIVITY_START_MODE_FULL_SPACE_UNMANAGED`
  property; `XR_KHR_android_thread_settings` may reject the renderer-worker
  type on some runtime builds.

## Architecture

### One runtime, two graphics bindings

`runtime/src/vr/openxr_integration.cpp` owns the pacing thread, policy
evaluation, the retained-layer protocol and the head-pose maths. It is written
against the backend-neutral vocabulary in `runtime/include/vr/openxr_backend.h`
(`OpenXRPresentation`, `OpenXRBackendFrame`, `OpenXRBeginStatus`,
`OpenXRSubmissionStatus`) and selects one backend class at compile time:

| Platform | Backend | Binding |
| --- | --- | --- |
| Windows | `OpenXRD3D12Backend` (`openxr_d3d12.cpp`) | Dawn's own D3D12 device and queue are bound to the session; eyes are copied on that queue. |
| Android | `OpenXRVulkanBackend` (`openxr_vulkan.cpp`) | The backend creates its **own** `VkInstance`/`VkDevice` through the OpenXR runtime; Dawn and that device meet on `AHardwareBuffer`s. |

The D3D12 types keep their old names through aliases, so `openxr_d3d12_replay_tests`
and the desktop code did not change.

### Why a second Vulkan device

The pinned Dawn package (`v20260603.191052`) exposes only `VkInstance` from its
Vulkan backend, no `VkDevice`, `VkQueue` or queue family, and it will not enable
the device extensions the OpenXR runtime demands. Binding Dawn's device to the
session is therefore impossible without a patched Dawn. Instead:

1. `xrCreateVulkanInstanceKHR` / `xrCreateVulkanDeviceKHR` (`XR_KHR_vulkan_enable2`,
   with an `XR_KHR_vulkan_enable` fallback that queries the extension lists)
   create a small Vulkan device the runtime is happy with.
2. Per eye, two `AHardwareBuffer`s (R8G8B8A8_UNORM, or RGBA16F when Aurora
   renders float) are allocated and imported on that device
   (`VK_ANDROID_external_memory_android_hardware_buffer`).
3. Aurora imports the same buffers as Dawn shared texture memory
   (`SharedTextureMemoryAHardwareBuffer`) and, inside the frame worker's
   command buffer, copies each replayed eye into the buffer
   (`aurora-main/lib/webgpu/vulkan_interop.cpp`, the twin of
   `d3d12_interop.cpp` and registered through the same stereo sink).
4. Ordering across the two devices uses Android sync file descriptors:
   Dawn's `EndAccess` exports a `SharedFenceSyncFD` the OpenXR device waits on
   before its `vkCmdCopyImage` into the acquired swapchain image, and the copy
   signals an exportable semaphore whose sync fd Dawn waits on before it
   writes that buffer again. Image layouts follow Vulkan's rule that a queue
   family ownership release and acquire must repeat the same old/new layout
   pair: Dawn reports its release layout, the OpenXR side acquires with it,
   transitions for the copy, and hands the buffer back in `GENERAL` with a
   transition-free release so Dawn's acquire can mirror it.
5. The copy runs on the queue bound to the session before
   `xrReleaseSwapchainImage`, so the compositor sees ordinary same-queue work.

The cost is one extra GPU copy per eye per frame, a few hundred microseconds
at Quest eye resolutions; the benefit is that stock Dawn is used unchanged
and the OpenXR device outlives Aurora's, which is exactly the failure DolphinXR
hit on Vulkan when a game's device was destroyed under the compositor.

`gpu.cpp` steers Aurora to an RGBA8 surface format under `xrInterop` on Android
(there is no BGRA `AHardwareBuffer` format) and requests the two Dawn features
the bridge needs.

### Controllers

Quest Touch controllers are not HID gamepads, so `openxr_input.cpp` syncs an
OpenXR action set on the pacing thread and feeds a virtual SDL joystick
(`SDL_AttachVirtualJoystick`, type gamepad). Aurora opens it like any pad and
assigns it to player 1; every existing binding, dead zone and overlay setting
applies. Mapping: A/B → South/East, X/Y → West/North, index triggers → trigger
axes, grips → shoulders, thumbsticks → sticks (clicks → stick buttons), left
menu → Start. Bindings are suggested for `oculus/touch_controller` and
`khr/simple_controller`.

### Android platform glue

- `runtime/src/vr/openxr_android.cpp`: `xrInitializeLoaderKHR` with the
  JavaVM and activity SDL already holds, the `XrInstanceCreateInfoAndroidKHR`
  chain (`OpenXRConfig::instance_create_next`), and the optional thread hint.
- `runtime/src/platform/host_platform.cpp` / `runtime_config.h`: the activity
  exports `MKW_ANDROID_DATA_DIR` (external files dir, user reachable) and
  `MKW_ANDROID_RESOURCES_DIR` (unpacked `wii_bootstrap/`, `dsp_coef.bin`,
  `initial_pipeline_cache.db`); the latter stands in for the executable
  directory so the existing adjacent-file lookups work unchanged.
- `main.cpp` includes `SDL_main.h` on Android so `SDLActivity` finds
  `SDL_main` in `libmain.so`, and passes the resources path to Aurora.
- Fibers use the vendored libco AArch64 backend (Bionic is Linux), guest memory
  uses the Linux `mmap` path, the MPRIS media monitor is compiled out.
- **Surface readiness.** Aurora presents only while `g_surfaceReady` is set, and
  on Android that flag starts false. Stock SDL3 exports neither its activity
  mutex (`Android_LockActivityMutex`) nor a readiness hook, so the app's
  `QuestSurface` subclass brackets SDL's `surfaceChanged`/`surfaceDestroyed`
  with `aurora_android_begin/end_surface_mutation` (`aurora/android.h`), and
  Aurora's `SurfaceLock` owns its own recursive mutex. This is KartPad's design.
- **JNI only on the real thread stack.** Guest threads run on libco stacks
  inside the SDL thread, and SDL's Android event pump can reach Java (joystick
  polling, HIDAPI). ART binds JNI transitions to the thread's real stack, so
  Aurora's `pump_events` is a no-op on Android and `UpdateAuroraAndProcessEvents`
  defers a poll made from a guest fiber until control is back on the scheduler
  context (`GuestFiberManager::IsOnSchedulerFiber`). The default guest thread
  shares the scheduler context, so most polls run immediately. Also KartPad's
  finding, from device crashes.
- Time conversion for frame interpolation uses `XR_KHR_convert_timespec_time`
  (CLOCK_MONOTONIC, the clock behind `steady_clock` on Bionic).

### Build system

- `runtime/CMakeLists.txt` recognises `CMAKE_SYSTEM_NAME=Android` on arm64 as
  `MKW_PLATFORM_ANDROID`: OpenXR on by default, Dawn from the pinned
  android-aarch64 package (digest pinned in `AuroraDawnProvider.cmake`, which
  also rewrites the package's absolute `liblog.so` path and looks the package
  up with `NO_CMAKE_FIND_ROOT_PATH` so the NDK sysroot rule does not hide it),
  SDL3 built shared (or `-DAURORA_SDL3_PROVIDER=system` for the AAR prefab),
  tests off, products built as `libmain.so` / `libmain_retro_rewind.so`,
  `-mcpu=cortex-a77` (Quest 2's XR2 Gen 1; Quest 3/Pro are supersets).
- `runtime/cmake/PublicProducts.cmake` gains `MKW_GENERATED_DIR` so a build
  configured from a checkout can name the translator output tree, and on
  Android rewrites the PE/COFF `.section .rdata,"dr"` of a Windows-generated
  blob `.S` into ELF `.rodata` plus a GNU-stack note. The translator itself
  also learned `--target-os windows|macos|linux|android` for
  `generate-data-init` and `translate-mod`, for pipelines that generate on
  another host.
- `android/`: the Gradle project. `app/src/main/cpp/CMakeLists.txt` adds the
  repository's `runtime/` as a subdirectory with those Android choices;
  flavours `base` and `retroRewind` pick the product target and library name.

## Building

Prerequisites on the Windows host (all already present on the machine this
was developed on): JDK 17, Android SDK with platform 34+, NDK `29.0.14206865`,
SDK CMake `3.22.1`, `adb`; a translated graph for your own disc (the installer's
`BuildWorkspace/generated`, produced by the normal Windows pipeline).

```powershell
powershell -ExecutionPolicy Bypass -File android/Prepare-QuestDependencies.ps1        # SDL3 3.4.4 AAR into android/app/libs
powershell -ExecutionPolicy Bypass -File android/Build-Quest.ps1 -Install             # base game, debug-signed
powershell -ExecutionPolicy Bypass -File android/Build-Quest.ps1 -Flavor retroRewind  # Retro Rewind (needs translate-mod output)
adb push DATA /sdcard/Android/data/org.wiicompiled.quest/files/WiiCompiledOpenXRVR/DATA
```

`Config.toml`, saves and per-run logs live next to `DATA` under
`WiiCompiledOpenXRVR`; the activity writes a first `Config.toml` with
`[vr] enabled = true` and `paths.dvd_root` set. Logs: `adb logcat -s SDL WiiCompiledQuest`
plus the `Logs/<product>_<stamp>_pid<pid>/console.log` folder the runtime writes.

A CMake-only cross-compile of the native runtime (no game) is the quick
compile check and needs no Gradle:

```powershell
cmake -S runtime -B .scratch/android-audit-build -G Ninja `
  -DCMAKE_TOOLCHAIN_FILE=$env:LOCALAPPDATA/Android/Sdk/ndk/29.0.14206865/build/cmake/android.toolchain.cmake `
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DANDROID_STL=c++_shared `
  -DCMAKE_BUILD_TYPE=Release -DMKW_BUILD_PRODUCTS=OFF
cmake --build .scratch/android-audit-build --target mkw_android_native_compile aurora_core aurora_gx
```

## Validation status

What has been verified on the development machine (September 2026):

- Translator: `dotnet test` passes with the new `--target-os` tests (17/17 in
  the touched suites).
- Windows: `mkw_openxr_replay_tests` and `mkw_vr_policy_tests` pass; the
  `mkw_runtime_common` and `aurora_core` targets compile with the refactored
  integration; the workspace product rebuild links `WiiCompiled.exe`.
- Android: the cross-compile audit passes. `mkw_android_native_compile`,
  `aurora_core` and `aurora_gx` all build for `aarch64-none-linux-android29`
  with NDK 29.0.14206865, which covers the whole native runtime including
  `openxr_vulkan.cpp`, `openxr_android.cpp`, `openxr_input.cpp` and the Aurora
  AHardwareBuffer bridge. Three Bionic portability fixes came out of it: the
  `std::min` call in `hle/audio/audio.cpp` needed an explicit type (`int64_t`
  is `long` on LP64 Android while the clock rep is `long long`),
  `guest_flat_memory.cpp` needs a `__NR_memfd_create` shim below API 30, and
  Crypto++'s `cpu.cpp` needs the NDK's `cpu-features` source compiled in.
- **Device, 2026-09-16: running on a Quest 3** (HorizonOS 14, API 34). The
  runtime negotiates `XR_KHR_vulkan_enable2` (Vulkan 1.0 to 1.2), creates
  1680x1760 `R8G8B8A8_SRGB` (VkFormat 43) swapchains, attaches the controller
  actions, and the session reaches `FOCUSED`. The game boots through the title
  movies into the attract race, the policy switches to `immersive-race` with
  all 189 perspective draws replayed per eye, the first projection layer is
  submitted, and the menus return to the virtual screen. No WebGPU or OpenXR
  errors over a 90 second session.

Bring-up fixes that only a device could reveal:

| Symptom | Cause | Fix |
| --- | --- | --- |
| Activity crashed with `EACCES` on `Config.toml` | `adb shell mkdir` had created the app's data directory, so the shell user owned it | Let the app create its own directory; `Run-Quest.ps1` launches once before placing DATA |
| DATA unreadable by the game | adb-placed files stay owned by the shell user and directories are `2770` | `chmod -R a+rX DATA` as the owning shell user; `run-as` cannot reach shared storage (SELinux) |
| "Cannot persist NAND setting.txt" | FUSE storage has no hard links; `link()` fails with `EACCES`, not `EPERM` | Android falls back to exists-check plus `rename` in `nand_settings.h` |
| `SharedFence ... signaled value (0) was not 1` | A sync fd is binary; Dawn expects value 1 | `vulkan_interop.cpp` passes 1 |
| Link error on `Android_LockActivityMutex` | SDL's activity mutex is not exported | Aurora-owned mutex plus the `QuestSurface` bracket (above) |
| Crypto++ `cpu-features.h` not found | The NDK ships cpu-features as source | Compiled into `mkw_cryptopp` on Android |
| Exploded racers and menu characters; smeared movie panels in the menus; then, once those were fixed, damaged eyes and slightly misplaced detail on characters | The Adreno 740 driver reads the wrong bytes when the shader multiplies an index by a stride that is not a multiple of 4. That covers the vertex fetch (`ubuf.vtx_start + vidx * stride + offset`) and indexed array reads (`array_start + index * stride`, e.g. 6-byte S16 normals). GX packs both byte-tight, so skinned models (a 1-byte `PNMTXIDX` first, stride 7) broke everywhere | Android pads every uploaded vertex and every indexed-array element to a 4-byte stride (`padded_upload_stride` in `lib/gx/gx.cpp`). Offsets inside a vertex or element are unchanged, and desktop is unchanged. **Fixed, headset-verified 2026-09-16** at character select and a Grand Prix start |

How the explosion was isolated, so the next Adreno rendering bug starts further
ahead:

- The CPU side was identical to Windows: a per-draw audit of palette indices
  and matrices matched byte for byte. Menus reach the headset as the mono
  desktop image, so stereo replay was not involved either.
- Shader-side rewrites did **not** help and were removed: constant-index palette
  matrix selection, shift-free sign extension, replacing `extractBits`, and
  byte helpers rewritten with constant shifts or integer division. The last two
  made menus worse, which is what pointed away from any one helper.
- Moving every attribute to a 4-byte boundary on the CPU fixed the explosion.
  Padding only the stride, with offsets still packed, fixed it just as well,
  which narrows the fault to the `vidx * stride` term. Mario's eyes stayed
  wrong under both, until indexed arrays got the same element padding. That
  combined padding is the shipped fix. It costs one copy per vertex and per
  array element. Peak uploads at a 12-racer race start were about 570 KB of
  the 3 MB vertex buffer and 710 KB of the 8 MB storage buffer.
- KartPad's Android reports of corrupted drivers on Adreno 750 match this
  symptom. That is plausible but not tested.

Diagnostics that stay in the build, all read once at launch from system
properties. Set them with `adb shell setprop <name> <value>` before starting
the app:

| Property | Effect |
| --- | --- |
| `debug.wiicompiled.vtxpad 0` | Turns the stride padding off, to re-check a driver update |
| `debug.wiicompiled.validation 1` | Keeps WebGPU validation and robustness on in release builds |
| `debug.wiicompiled.inject <n>:<button>` | Presses `a`, `b`, `x`, `y`, `start`, `up`, `down`, `left` or `right` for 12 XR frames each time `<n>` changes |

The injector makes headset tests possible with nobody wearing the headset.
Keep the display awake, drive the menus, then take a compositor screenshot:

```powershell
adb shell am broadcast -a com.oculus.vrpowermanager.prox_close
adb shell setprop debug.wiicompiled.inject 1:a    # title -> licence; bump the number per press
adb shell am startservice -n com.oculus.metacam/.capture.CaptureService -a TAKE_SCREENSHOT
adb pull /sdcard/Oculus/Screenshots/<newest>.jpg
```

From a cold start, five `a` presses about 5 s apart, starting once the title
screen is up, reach Grand Prix character select. A value left over from an
earlier run is ignored on the first read. Presses only land while the XR
session is `FOCUSED`.

Open measurement: the attract race advanced 603 game frames in about 14 s,
roughly 43 FPS against the game's 60, with an optimized build (`-O3`,
translated code `-O2`, `-mcpu=cortex-a77`). Profiling on the XR2 Gen 2 is the
first performance task.

Verified on device since: the menus on the virtual screen, controller input
(the user has driven races), and an immersive Grand Prix start with all 12
racers rendering correctly. Not yet verified: stereo comfort and scale,
lifecycle (Quest menu, guardian, sleep), and a full race to the finish.
When diagnosing a new device, the session log should show, in order: the loader log line
(`OpenXR Android loader initialized`), the requirements line (which binding
extension was negotiated), `OpenXR Vulkan swapchains ready`, the session
state reaching `FOCUSED`, `presentation=virtual-screen` for the menus, and
`first immersive packet consumed` on race entry. A black headset with a working
Android mirror points at the AHardwareBuffer copy (check for `vkImportSemaphoreFdKHR`
or `EndAccess` errors); a black mirror too points at Aurora itself.

## Known gaps and next steps

- **Device bring-up.** Run on a Quest 3, capture logcat, fix what the runtime
  rejects. Likely first candidates: the exact `XR_KHR_vulkan_enable2` device
  extension negotiation, Dawn's begin/end layout reporting for AHardwareBuffer
  imports, and swapchain format choice (`R8G8B8A8_SRGB` is expected).
- **Performance.** The desktop product targets x86-64-v3; nothing has been
  profiled on the XR2. Expect shader compilation stalls on first run (Aurora's
  pipeline cache is bundled) and start with `render_scale` below 1.0 if the
  compositor reports missed frames. `XR_FB_foveation` is not used yet.
- **Lifecycle.** Backgrounding (the Quest menu, guardian) pauses the session
  through the ordinary `STOPPING`/`READY` events; SDL's Android surface loss is
  handled by Aurora's existing Android paths. Neither has been exercised.
- **Input.** D-pad (trick inputs) is not bound; remap in `Config.toml` or bind
  the thumbstick directions in a follow-up. Haptics are wired but nothing calls
  them yet.
- **Retro Rewind on device** needs the mod's translation and its extracted
  content pushed next to `DATA`, exactly like the desktop product.
- **Release signing and store packaging** are out of scope; `Build-Quest.ps1`
  produces debug-signed APKs for sideloading.
