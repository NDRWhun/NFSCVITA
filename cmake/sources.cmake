# Hand-written runtime sources. The generated game code is globbed separately.

# Direct3D frame model and texture layouts (platform neutral).
set(NFSC_RENDER_SOURCES
  src/platform/render.cpp
  src/platform/texture.cpp
)
# Direct3D 9 and D3DX layer.
set(NFSC_D3D9_SOURCES
  src/lib/winapi/d3d9.cpp
  src/lib/winapi/d3d9_device.cpp
  src/lib/winapi/d3d9_native.cpp
  src/lib/winapi/d3d9_record.cpp
  src/lib/winapi/d3d9_resources.cpp
  src/lib/winapi/d3d9_state.cpp
  src/lib/winapi/d3dx9_30.cpp
  src/lib/winapi/d3dx9_effect.cpp
)

set(NFSC_PLATFORM_SOURCES
  ${NFSC_RENDER_SOURCES}
  src/platform/config.cpp
  src/platform/cores.cpp
  src/platform/filesystem.cpp
  src/platform/jobs.cpp
  src/platform/ime.cpp
  src/platform/log.cpp
  src/platform/memfuncs.cpp
  src/platform/perf.cpp
  src/platform/time.cpp
  src/platform/sha256.cpp
  src/platform/sync.cpp
  src/platform/watchdog.cpp
  src/platform/msgqueue.cpp
  src/platform/mmx_selftest.cpp
  src/platform/fpu_selftest.cpp
  src/platform/gxm/gxm_device.cpp
  src/platform/gxm/gxm_render.cpp
)

set(NFSC_LIB_SOURCES
  src/lib/audio.cpp
  src/lib/controls.cpp
  src/lib/cpu.cpp
  src/lib/event.cpp
  src/lib/file.cpp
  src/lib/fpucontrol.cpp
  src/lib/gamepad.cpp
  src/lib/gamethreads.cpp
  src/lib/heap.cpp
  src/lib/jump.cpp
  src/lib/library.cpp
  src/lib/mutex.cpp
  src/lib/registry.cpp
  src/lib/shimgate.cpp
  src/lib/timer.cpp
  src/lib/winapp.cpp
  src/lib/window.cpp
)

set(NFSC_WINAPI_SOURCES
  ${NFSC_D3D9_SOURCES}
  src/lib/winapi/advapi32.cpp
  src/lib/winapi/com.cpp
  src/lib/winapi/dinput8.cpp
  src/lib/winapi/dsound.cpp
  src/lib/winapi/imports.cpp
  src/lib/winapi/kernel32.cpp
  src/lib/winapi/msvcp71.cpp
  src/lib/winapi/msvcr71_format.cpp
  src/lib/winapi/msvcr71_io.cpp
  src/lib/winapi/netapi32.cpp
  src/lib/winapi/ole32.cpp
  src/lib/winapi/rpcrt4.cpp
  src/lib/winapi/shfolder.cpp
  src/lib/winapi/support.cpp
  src/lib/winapi/user32.cpp
  src/lib/winapi/winmm.cpp
  src/lib/winapi/ws2_32.cpp
  src/lib/winapi/dinput/idirectinput8.cpp
  src/lib/winapi/dinput/idirectinputdevice.cpp
  src/lib/winapi/dsound/idirectsound.cpp
  src/lib/winapi/dsound/idirectsound3dlistener.cpp
  src/lib/winapi/dsound/idirectsoundbuffer.cpp
)

# Port code that works with guest addresses of NFSC.exe.
set(NFSC_GAME_PORT_SOURCES
  src/nfsc/allocators.cpp
  src/nfsc/audio_decode.cpp
  src/nfsc/audioupdate.cpp
  src/nfsc/bankcode.cpp
  src/nfsc/banklib.cpp
  src/nfsc/bench.cpp
  src/nfsc/collision.cpp
  src/nfsc/defer.cpp
  src/nfsc/display.cpp
  src/nfsc/displayports.cpp
  src/nfsc/drawlist.cpp
  src/nfsc/envmap.cpp
  src/nfsc/feng.cpp
  src/nfsc/gamestate.cpp
  src/nfsc/flush.cpp
  src/nfsc/framephases.cpp
  src/nfsc/hot.cpp
  src/nfsc/input.cpp
  src/nfsc/mixer.cpp
  src/nfsc/mixrate.cpp
  src/nfsc/mixsplit.cpp
  src/nfsc/native.cpp
  src/nfsc/netcrit.cpp
  src/nfsc/overlap.cpp
  src/nfsc/physics.cpp
  src/nfsc/pools.cpp
  src/nfsc/prompts.cpp
  src/nfsc/scenery.cpp
  src/nfsc/shadow.cpp
  src/nfsc/sim.cpp
  src/nfsc/solids.cpp
  src/nfsc/streaming.cpp
  src/nfsc/vehicle.cpp
  src/nfsc/vp6.cpp
  src/nfsc/vp6_native.cpp
  src/nfsc/worldtex.cpp
)

# Runtime sources that include headers generated from the game binary (nfsc_layout.h).
set(NFSC_GENERATED_HEADER_SOURCES
  src/lib/memmap.cpp
  src/lib/thread.cpp
  src/lib/winapi/msvcr71.cpp
  src/nfsc/census.cpp
)

# Runtime sources that build without the generated code (the CI runtime check).
set(NFSC_STANDALONE_SOURCES
  ${NFSC_PLATFORM_SOURCES}
  ${NFSC_LIB_SOURCES}
  ${NFSC_WINAPI_SOURCES}
  ${NFSC_GAME_PORT_SOURCES}
)

set(NFSC_RUNTIME_SOURCES
  ${NFSC_STANDALONE_SOURCES}
  ${NFSC_GENERATED_HEADER_SOURCES}
)
