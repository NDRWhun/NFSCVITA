# Development settings

Settings for development builds: A/B comparisons, native-versus-translated checks, experiments and diagnostics.
They go in `ux0:data/NFSCVITA/nfscvita.cfg`, one `key=value` per line; the first value of a key wins. The log is
`ux0:data/NFSCVITA/nfscvita.log`. The defaults are the intended release behaviour; a release fixes them in code.

## Picture, audio and input

| Key | Default | Effect |
|-----|---------|--------|
| `rear_touch` | `1` | `0` turns the rear touch inputs off |
| `front_touch` | `1` | `0` turns the front touch input off |
| `core3` | `1` | `0` keeps the game off the fourth CPU core even when CapUnlocker is installed |
| `frame_rate` | `30` | `60` shows each frame for one vblank instead of two |
| `flip_wait` | `flip` | the vblanks of `frame_rate` count from the vblank that shows the frame, so each frame shows for at least two (one) vblanks, also the frame after a late one; `call` counts them from the previous frame's wait, as before (the frame after a late one can show for a single vblank) |
| `stretch` | `1` | `0` keeps the aspect ratio of game resolutions other than 960x544 (with borders) instead of filling the screen |
| `motion_blur` | `0` | `1` offers the game's Motion Blur option (a cheaper four-tap blur; turn it on in the game's options) |
| `road_reflection` | `0` | `1` offers the game's Road Reflection option (turn it on in the game's options; it costs a reflection view and four post passes every frame) |
| `envmap_faces` | `1` | faces of the car reflection cube drawn per race frame: one in turn by default; `0` keeps the game's schedule (Low 1, Medium 3, High 6) |
| `envmap_size` | `128` | size of the car reflection cube's faces, 16-256 (a power of two); `256` is the game's size |
| `world_texture_drop` | `2` | top mip levels left out of the city's world textures (track section Y0), 0-3; `0` is full resolution and needs 74 MB at once |
| `section_texture_drop` | `2` | top mip levels left out of the textures of the other streamed track sections, 0-3; `0` is full resolution. The game creates the textures with the same mip level as without it while its own World Detail drop is at least this large (3 levels at the lowest World Detail); the sections need less track streaming memory and less reading |
| `trilinear` | `1` | `0` filters textures within the nearest mip level only (cheaper; mip level changes become visible as lines) |
| `vt_lut` | `nearest` | `blend` reads the colour grading cube as the game does, blending two slices (slower full-screen pass) |
| `mix_rate` | `24000` | sample rate of the audio mixer; `48000` is the game's own rate (twice the mixing work); `22050`, `32000` and `44100` also work |

## Memory and streaming

| Key | Default | Effect |
|-----|---------|--------|
| `car_pool_kb` | `40000` | size of the car pool in KB, 20000-65000; `65000` is the game's size. The game reserves it with the track streaming memory at boot, so a smaller pool leaves that much more guest memory free; cars and parts are evicted sooner when it runs full |
| `section_activate_per_frame` | `0` | `1` activates the track sections whose reads finished one per game frame (the first of the current streaming list), as the game's other build can, instead of all of them in the frame their reads finish; a section may appear a few frames later. Loading screens and the game's waits for the end of loading activate them all at once as before |

## Native code, threading and diagnostics

| Key | Default | Effect |
|-----|---------|--------|
| `tty_log` | `0` | `1` also writes the log to the console (it blocks when no host reads the console) |
| `perf_log` | `0` | `1` logs a performance line every 5 seconds (on by default with `bench=1`). Its `game pools` line ends with the track streaming pool: `track` size/used/peak in KB without the car pool, its largest free block, the car pool, the sections that did not fit (out of memory), and totals since start of the least recently used sections unloaded to make room (with their KB), the sections of the current streaming list thrown out to make room (jettisons), the calls that make room in the pool (make space) and the sections waiting for `section_activate_per_frame` |
| `watchdog` | `1` | `0` turns off the watchdog, which logs the thread states when the game thread produces no frame for `watchdog_seconds` |
| `watchdog_seconds` | `5` | stall time before the watchdog reports, at least 2 |
| `display_parallel` | `1` | `1` runs the game's display call (the rendering part of each frame) on a thread of its own on `display_core`; the game thread waits for it, so the frame keeps its order |
| `display_core` | `2` | CPU core (0-2, 3 with `core3`) of the display thread |
| `display_overlap` | `4` | `0` off (implies `display_parallel=1` when on): `1` runs each race frame's display call beside the game thread's next frame, `2` every frame's; `3` starts each race frame's display call right after its own sim steps and updates, beside the rest of that frame up to its front end update, which waits for it; `4` starts it after the frame's front end update, beside the second event flush, sound, loading service and frame end. With `perf_log` the second timing line gains an `overlap` part (window, barrier, display call, lock and Direct3D waits, and the critical section waited for most with its owner) |
| `display_ports` | `1` | `0` runs the display call's natively ported hot functions (scenery instance culling, ...) as translated code |
| `scenery_native` | `0` | `1` runs the scenery tree walk (TreeCull) and the scenery model submission (StuffScenery) natively |
| `shadow_scenery` | `0` | `1` plans each scenery tree walk and scenery submission natively without running their callees, runs the translated function with its callees' calls recorded and compares calls, registers and the counters and frame memory words it writes; the translated results are kept; the benchmark summary gives `shadow_scenery: <function> N calls compared, D differ, U not planned` and the first differences are logged (slow) |
| `event_query_lag` | `0` | `1` (or `2`): the game's per-frame GPU event query (it waits at the end of each frame until the GPU finished the previous one) counts as done once the GPU finished the frame one (two) before that, so one more frame may be in flight; the frame slots still bound it. Costs that much input latency, nothing visual |
| `display_defer` | `1` | `0` runs the sim's world, render and camera updates after each sim step as the game does, instead of once at the end of the frame |
| `audio_parallel` | `0` | experimental: `1` runs each race frame's audio update (the sound objects, speech and dynamic mix, whose parameter calls wait for the audio system lock the mixer holds) on a thread of its own beside the rest of the frame, until the next frame starts; `2` until the next frame's first event flush (also beside its timers, input and sim event). Core 3 with `core3`, otherwise the idle time of cores 0-2. The sound bank loader inside the update stays on the game thread; sound parameters reach the mixer up to one frame later. With `perf_log` a timing line `perf: audio update` gives the thread's time, the game thread's waits for it, and the game thread's takes of and remaining waits for the audio system lock by part (sim, audio update, other) with the game code that waited most and the takes made while the audio thread ran (also with `audio_parallel=0`) |
| `audio_lock_batch` | `1` | each race frame's audio update (on the game thread, or on its own with `audio_parallel`) takes the audio system lock once and holds it to its end, instead of about 70-85 separate takes that can each wait for the mixer's block service; the mixer waits for the update instead. `0` keeps the game's separate takes. While the update blocks (another lock, a sleep, an object) with no lock take of its own open, the lock is free for the other threads. The `perf: audio update` line gives the batch's wait and hold time and those releases; the mixer's blocks/s and split wait are in the line before |
| `mixer_parallel` | `1` | `0` runs the audio mixer thread under the execution lock, taking turns with the game thread |
| `mixer_split` | `1` | `0` runs the mixer's engine and sound-bank synthesis on the mixer thread itself instead of a worker thread beside it (needs `mixer_parallel=1`) |
| `tick_parallel` | `1` | `0` runs the game's 100 Hz tick thread under the execution lock |
| `mixer_native` | `1` | `0`: the audio mixer's decoders and DSP loops run as translated code |
| `shadow_mixer` | `0` | `1` runs each engine sample read (0x537310: engine block decode and 16-bit conversion) translated first, then natively from the same memory, and compares the registers, the decoder object, the argument slots and the output; the benchmark summary gives `shadow_mixer: N calls compared, D differ` (slow) |
| `bank_native` | `1` | `0`: the sound banks' code runs as translated code |
| `banklib_native` | `1` | `0`: the sound library functions the recompiled bank code calls (control tables, envelopes, oscillators, glides, triggers, counters) run as translated code |
| `shadow_banklib` | `0` | `1` runs each of those library calls translated first, then natively from the same memory, and compares eax and the memory it writes; the benchmark summary gives `shadow_banklib: N calls compared, D differ` and the first differences are logged (slow) |
| `hot_native` | `1` | `0`: the hottest maths functions (culling, bounds, lighting, road navigation) run as translated code |
| `roadnav_native` | `1` | `0`: the road spline smoothing's system solve runs as translated code (with `hot_native=1`) |
| `roadnav2_native` | `1` | `0`: the road spline smoothing pass (its run splitting, smoothing systems and steps) and the navigator's occluded position update run as translated code (with `hot_native=1`) |
| `roadnav3_native` | `1` | `0`: the navigator's road cookies of nearby vehicles (their list, blocking tests and the cut of the road points, `0x8078f0`) run as translated code (with `hot_native=1`) |
| `shadow_roadnav` | `0` | `1` runs each call of the native road navigation functions (spline tension pass, smoothing system, smoothing pass, occluded position update, road cookies, best line curvature, closest road point passed) translated and native on the same memory and compares registers, x87 stack and written memory (the occluded position update calls its guest callees in both runs); the benchmark summary gives `shadow_roadnav: N calls compared, D differ, K in NaN sign or payload only` and the first differences are logged (slow). Shadow compares leave out the x87 status word, which no caller reads after a call |
| `physics_native` | `1` | `0`: the car physics step's native functions (the simple suspension's wheel forces with its tire update) run as translated code |
| `shadow_physics` | `0` | `1` runs each call of the native car physics functions translated and native on the same memory (the suspension, its wheels, the chassis state and the rigid body's force accumulators restored in between) and compares the registers the callers read, the x87 stack and that memory; the benchmark summary gives `shadow_physics: N calls compared, D differ, K in NaN sign or payload only` and the first differences are logged (slow; callees outside that memory run twice, except that a call whose translated run changed a wheel's surface or deleted a surface collection is not repeated: those count as `T translated only`) |
| `wheel_native` | `1` | `0`: the wheel position update (`Wheel::UpdatePosition`, `0x6c73a0`: wheel world position, contact velocity, ground probe, surface) and the world position update (`WWorldPos::Update`, `0x817090`) run as translated code |
| `shadow_wheel` | `0` | `1` runs each call of those two functions translated and native on the same memory (the wheel or position, the ground plane and the argument area restored in between) and compares the registers the callers read, the x87 stack and that memory; the benchmark summary gives `shadow_wheel: N calls compared, D differ, K in NaN sign or payload only, T translated only` (slow; the face search runs twice; a call without a collider (world face search) or a wheel update whose translated run changed the wheel's surface or deleted the surface collection it held is not repeated and counts as translated only) |
| `steering_native` | `1` | `0`: the AI steering step (`AIVehiclePid::OnSteering`, `0x431700`: heading errors, PID error records, gain tables, steering clamp) runs as translated code |
| `shadow_steering` | `0` | `1` runs each AI steering step translated and native on the same memory (the AI, its PID errors with their value rings, its steering controller and the controller's averaging windows restored in between) and compares the registers the callers read, the x87 stack and that memory; the benchmark summary gives `shadow_steering: N calls compared, D differ, K in NaN sign or payload only, T translated only` (slow; the virtual getters run twice; a step that took the base AI steering path or returned before the heading errors is not repeated and counts as translated only) |
| `collision_native` | `1` | `0`: the collision triangle gather (`WCollisionMgr::GetTriList`, `0x813290`: instance spheres, strip triangles, the list append) runs as translated code |
| `shadow_collision` | `0` | `1` runs each triangle gather translated, then native into host memory, and compares the triangles the translated code appended and eax/esp; the benchmark summary gives `shadow_collision: N calls compared, D differ, K in NaN sign or payload only` (slow) |
| `alloc_native` | `1` | `0`: the small-block allocator and slot pools run as translated code |
| `feng_native` | `1` | `0`: the front end and HUD object update (scripts, groups, key tracks) runs as translated code |
| `shadow_feng` | `0` | `1` runs each native front end object update that makes no guest call (no script messages, chained or looping script end, list, group or movie object, mouse offsets or quaternion slerp) translated and native on the same memory and compares registers, x87 state and written memory; the benchmark summary gives `shadow_feng: N calls compared, D differ` and the first differences are logged (slow) |
| `solid_notify_native` | `1` | `0`: the offer of every model still waiting for its solid to each newly loaded solid list (NotifySolidLoader) runs as translated code, with a full eModel::ConnectSolid call per model |
| `native_submit` | `1` | `0`: model submission into the draw list runs as translated code |
| `native_flush` | `1` | `0`: the draw list flush runs as translated code |
| `flush_fields` | `1` | `0`: the native flush reads the effect object's parameter handles and methods for every record instead of once per effect run |
| `flush_prefetch` | `1` | `0`: the native flush does not request the guest data of the next records (record, world matrix, mesh, solid, mesh info, vertex buffer object) ahead of drawing them |
| `flush_headlight` | `1` | `0`: the native flush reads the effect settings' headlight switch at every transform instead of once per effect run |
| `effect_versions` | `1` | `0`: each effect parameter block keeps its change version with its values instead of in one shared array of versions, which CommitChanges reads for every draw |
| `stamp_pairs` | `1` | `0`: the device writes the stamps of a constant upload with one loop per register instead of two registers per step |
| `fused_matrices` | `1` | `0`: the effect layer transposes a matrix parameter into its shader registers at every upload, instead of the matrix setters writing those registers with the parameter |
| `native_material` | `1` | `0`: the light material setter of the native flush runs as translated code |
| `native_parts` | `1` | `0`: the bone and PCA weight setters and the stencil-masked draws of the native flush run as translated code |
| `native_switch` | `1` | `0`: the effect switches of the native flush run as translated code |
| `native_uniforms` | `0` | `1` builds the world effects' finished draws in the native flush, `2` every effect's that allows it (moves work from the render thread to the display call) |
| `native_sync` | `1` | with `native_uniforms`: `0` copies the whole device state and constant registers at the start of every native effect run and compares every state at its end, instead of only the states and registers written since |
| `native_reuse` | `1` | with `native_uniforms`: `0` builds every native draw's state (programs, samplers, blending, alpha test) anew, instead of taking the previous draw's when no state, texture, program or vertex layout changed in between |
| `native_blocks` | `1` | with `native_uniforms`: `0` builds a native draw's new constant blocks in a buffer of their own and copies them into the frame, instead of building them in the frame |
| `vp6_native` | `1` | `0` decodes the movies with the game's own (translated) decoder |
| `vp6_worker` | `1` | `0` keeps the native movie decoder on one thread |
| `vp6_core` | `1` | CPU core of the movie decode thread; `-1` decodes on the game's own movie thread |
| `vp6_gpu` | `1` | `0` converts the movie frames to RGB on the CPU instead of the GPU |
| `gxm_lists` | `1` | `0` records the GPU commands on the render thread alone instead of with helper threads |
| `gxm_core2` | `1` | `0` keeps the GPU command recording helpers off core 2 |
| `gxm_precomputed` | `1` | `0` builds every draw's GPU state when it is drawn instead of using precomputed draws |
| `tiled_targets` | `1` | `0` stores the render targets of one level (the back buffer drawn offscreen, the game's full-screen and post-processing targets) in linear rows instead of 32 x 32 pixel tiles; the tiles are read with 2D locality by the post passes that sample them (the same picture) |
| `vertex_branchless` | `1` | `0` uses the world, road, car and glass vertex programs with the power of the `lit` instruction behind a branch, as before; by default the power is computed for every vertex and selected (the same results; 10-30% fewer vertex cycles, and the programs run in parallel instead of per-instance mode) |
| `draw_sort` | `1` | `0` draws the alpha-tested draws in the game's order; `2` also moves opaque draws ahead of earlier depth-tested draws that write no depth (fewer hidden-surface flushes, but it can change the picture) |
| `alpha_test_elide` | `1` | `0` keeps every alpha test (by default draws whose alpha cannot fail the test skip it) |
| `shadow_vp6` | `0` | `1` runs both movie decoders on every frame and logs any difference (slow) |
| `shadow_memcpy` | `0` | `1` runs the movie and audio libraries' copy function both translated and native and logs any difference (slow; with `mixer_parallel=0` only) |
| `display_census` | `0` | records which guest memory the parts of a race frame write and writes `census.txt` to the data folder: `1` per part of the frame, `2` also per sim update, `3` the display call's reads (census build only), `4` conflicts between the overlapped display call and the game thread (census build only, with `display_overlap`): the first 64 are logged, all go to `census.txt` every 60 calls and at race end with the writing and reading code and malloc allocation sites; `5` likewise conflicts between the audio update on its thread and the game thread (census build only, with `audio_parallel`; reads of one thread against writes of the other, alternating per update). The census build (`tools/build.ps1 -Census`) has a 220 MB guest heap |
| `display_census_interval` | `120` | frames between two sampled race frames with `display_census` |
| `bench` | `0` | `1` starts a benchmark (below) instead of the menus, without a profile: the save is neither read nor written |
| `bench_mode` | `race` | `race`: the race benchmark; `roam`: the free-roam benchmark |
| `bench_race` | `mu.1.1` | race of the race benchmark, by its event ID (`mu.1.1` is a circuit) |
| `bench_laps` | `2` | laps of a circuit, 1-10 (race) |
| `bench_opponents` | `7` | AI opponents, 0-7 (race) |
| `bench_traffic` | `100` | traffic density in percent, 0-100 (roam); `100` is the game's free-roam density, `0` no traffic |
| `bench_cops` | `1` | `0` turns the cops off (roam) |
| `bench_seconds` | `0` race, `60` roam | ends the measurement after this many seconds (race: `0` ends it at the finish; roam: `0` means 60) |
| `bench_exit` | `0` | `1` quits the game after the summary |

Race benchmark: `bench=1` starts a quick race without traffic or cops, in the `viper` driven by the AI, with a fixed
random seed. A `bench: summary` line in the log gives the fps, the average, 99th percentile and longest frame time,
the frames over 33.4 ms and 50 ms from the end of the countdown, and the guest memory peak. Its `gpu` part gives the
GPU time per frame (average, 99th percentile, longest, frames over 33.4 ms) from the display callback: the time from
the later of the frame's last scene submission and the previous frame's completion to its own completion, counted for
the frames the GPU finished more than 1 ms after the previous frame's display wait ended (so the count drops when
the GPU keeps ahead of the display).

Free-roam benchmark: `bench=1` with `bench_mode=roam` starts free roam in the city without the menus and without a
save, in the `viper` driven by the AI, with traffic and cops and a fixed random seed. The AI follows the road and
drives to road nodes connected to it 1.5-2.5 km away (the first one 0.5-1 km, asked for once the car moves or after
15 s) and takes a new one on arrival; a lost
path is requested again up to 3 times, a path search over 5 s is given up for a new target; a car that stays within
15 m for 12 s is put back on the road.
The measurement starts when the car has moved 20 m (or after 60 s in the world) and runs for `bench_seconds`. The
`bench: summary` line has the race benchmark's fields and then the distance driven, the track sections loaded while
measuring, the targets (reached, without a path, path lost), the path requests and the stuck resets.


## Profile-guided optimisation (clang)

1. `tools/build.ps1 -Compiler clang -PgoGenerate` builds the instrumented game into `build-release-pgogen`. Its
   profile runtime is compiler-rt's for the compiler's version (downloaded, or `NFSC_COMPILER_RT`) with the Vita
   platform file of [psp2-tooling](https://github.com/Grarak/psp2-tooling) (`pgo/InstrProfilingPlatformVita.c`,
   downloaded at the commit pinned in `cmake/pgo-runtime.cmake`).
2. Install it and run the race benchmark (`bench=1`). The game writes `ux0:data/NFSCVITA/nfsc.profraw` every 30 s
   and on a clean exit (to `nfsc.profraw.part`, then renamed); fetch it after the race.
3. `llvm-profdata merge -o race.profdata nfsc.profraw` with the `llvm-profdata` of the same LLVM release.
4. `tools/build.ps1 -Compiler clang -PgoUse race.profdata` builds the optimised game into `build-release-clang`.

Record again after a generator change: changed functions lose their profile. Internal functions are keyed by the
source path of the worktree that built the instrumented game; build the optimised game in the same folder, or
rewrite the paths (`llvm-profdata merge --text`, edit, merge back).
