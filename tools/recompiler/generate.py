"""Generates the C++ recompilation of Need for Speed Carbon (NFSC.exe) into one directory.

Usage: generate.py --binaries <dir> --out <dir> [--analysis <file.json>] [--fresh] [--jobs N]

<dir> must hold NFSC.exe. Its SHA-256 is checked: the tables below fit only this file. The x86 routines of the sound
banks under <dir>/SOUND are recompiled too (disasm/bankcode.py).

The discovery result is kept in .gencache (disasm/cache.py) and reused while the executable, the discovery
parameters and the sources discovery runs are unchanged; --fresh runs discovery anyway and stores it again. The
code is written by one worker process per core (disasm/parallel.py); the output does not depend on --jobs.

Files in the output directory are replaced only when their content changes; generated files that are no longer
produced are deleted, other files are left alone.
"""

import argparse
import collections
import filecmp
import gc
import hashlib
import json
import os
import re
import shutil
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'disasm'))
import bootstrap  # noqa: E402
bootstrap.ensure_capstone()
import capstone  # noqa: E402
from disasm import analysis, cache, codegen, discover, fpustack, image as pe_image, parallel, writer  # noqa: E402
from disasm import bankcode  # noqa: E402

EXE_NAME = 'NFSC.exe'
EXE_SHA256 = 'baaaaec33873d4ba7e4dbae6a5212049ffdcf9679a35d865d7625b4189952fd3'
MODULE = 'nfsc'
CAPSTONE_MODE = (capstone.CS_ARCH_X86, capstone.CS_MODE_32)
# Discovery cache: .gencache in the repository.
CACHE_DIR = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '.gencache'))

# setjmp: the import thunk of msvcr71 _setjmp3 (jmp [0x9c129c]); its one caller is 0x64db13. Calls to it save a host
# jump buffer keyed by the guest jmp_buf (include/recomp.h); the msvcr71 longjmp shim restores the registers and
# returns there. No guest code implements longjmp, so no jump resumes after setjmp.
SETJMP = 0x87ef70
LONGJMP_JUMPS = {}                      # jump address -> register holding the guest jmp_buf

# Functions that never return although their code has a path to ret. The C runtime is msvcr71.dll: its exits are
# imports (NO_RETURN_IMPORTS).
NO_RETURN = []

# Imports that never return: the C runtime's exits, thread end, C++ throw and the handlers that end the process.
NO_RETURN_IMPORTS = [
    'exit', '_exit', '_amsg_exit', '_endthreadex', 'longjmp', '_CxxThrowException', '?terminate@@YAXXZ',
    '_purecall', '__security_error_handler',
]

# Net x87 pushes of imports: C runtime functions returning a double in st0 (+1), and the _CI functions, which take
# their arguments in st0 (and st1) and leave the result in st0.
IMPORT_FPU_DELTAS = {
    ('msvcr71', 'ceil'): 1, ('msvcr71', 'floor'): 1, ('msvcr71', 'sqrt'): 1, ('msvcr71', 'cos'): 1,
    ('msvcr71', '_CIacos'): 0, ('msvcr71', '_CIasin'): 0, ('msvcr71', '_CIcosh'): 0, ('msvcr71', '_CIsinh'): 0,
    ('msvcr71', '_CItanh'): 0, ('msvcr71', '_CIpow'): -1, ('msvcr71', '_CIfmod'): -1,
}

# Imports that are data, not functions: the import address table slot holds the address of the variable.
DATA_IMPORTS = [('msvcr71', '_iob'), ('msvcr71', '_adjust_fdiv')]

# Guest functions replaced by hand-written native code (native::sub_<address>, src/nfsc/native.cpp); the translated
# body stays as sub_<address>_guest.
NATIVE_OVERRIDES = [
    0x87eac0,   # _ftol2: st0 to a 64-bit integer, truncating
    0x87eca0,   # _chkstk / _alloca_probe: page-probing stack allocation
    0x6b5650,   # SeedRandomNumber: seeds the game's random generator (fixed seed in the race benchmark)
    0x6b7930,   # MainLoop: one game frame (the race benchmark checks the race state after it)
    0x43c670,   # AIVehicleHuman::Update: the player car's AI (the roam benchmark gives it targets)
    0x87f970,   # VP6 movie frame decode (native decoder, src/nfsc/vp6.cpp)
    0x57dff0,   # movie frame YUV to RGB conversion into a locked texture (format 1 native)
    0x725c80,   # texture creation from a texture pack entry (button glyphs replace key textures, src/nfsc/prompts.cpp)
    0x5786b0,   # label hash mapping before string lookups (pad wording for PC wording)
    0x8583a0,   # main menu screen constructor (hides the keyboard-only quit hint)
    0x8d3750,   # memcpy of the movie and audio libraries (movie planes into the renderables)
    0x89f363,   # audio mixer filter stage (src/nfsc/mixer.cpp)
    0x89f4f6,   # audio mixer filter stage with the anti-denormal offset
    0x536c40,   # engine-audio compressed block decode (src/nfsc/audio_decode.cpp)
    0x537310,   # engine-audio sample reader
    0x536bf0,   # engine-audio fractional sample index
    0x8abbb2,   # audio mixer 16-bit fractional interpolation
    0x8b70b0,   # audio mixer 32-bit fractional interpolation
    0x8aa340,   # audio mixer signed 16-bit sample conversion
    0x8ab884,   # audio mixer EA-XA packet decode
    0x8983f4,   # audio mixer scaled integer control product
    0x898423,   # audio mixer scaled integer control pair
    0x8b6d00,   # audio mixer one-pole filter
    0x8aa220,   # audio mixer gain accumulation from the end
    0x8ad305,   # audio mixer gain accumulation
    0x8ad12e,   # audio mixer scale
    0x8b074c,   # audio mixer output clamp
    0x8a74dd,   # audio mixer block render (counted for perf_log)
    0x8a7de6,   # audio mixer DAC thread body (src/nfsc/mixsplit.cpp: the SND11 block service on a worker thread)
    0x8993fa,   # audio System update (waits for the block service handed to the worker)
    0x89f9fd,   # SND11 block service, a stage-1 job of the System update
    0x8a4d78,   # Snd9Service graph stage: copies the block service's mix buffer
    0x70f0b0,   # box transform by a matrix (src/nfsc/hot.cpp)
    0x71b630,   # view frustum test of a box
    0x46fb20,   # bExpandBoundingBox (2D)
    0x46fcc0,   # bBoundingBoxIsInside (2D)
    0x7e9a60,   # 2D bounds of a triangle
    0x605910,   # VU0_MATRIX4_mult
    0x7384c0,   # Irradiance::UpdateCoeffs
    0x46cad0,   # CRC-32 with the length
    0x558360,   # sorted table search (solid and texture index entries)
    0x7e7c30,   # triangle of a packed collision entry
    0x79f9c0,   # distance attenuation of a directed scenery object
    0x699420,   # SplitPermTempChunks
    0x7ec920,   # road spline system reduction
    0x401710,   # vector with w
    0x71a350,   # std::rotate of 8-byte elements
    0x7381a0,   # QueryLightHelper
    0x7f9570,   # road spline tension pass (src/nfsc/sim.cpp)
    0x7e83e0,   # point in a collision triangle
    0x7fc550,   # WWorldPos closest face search
    0x7f8bd0,   # road spline smoothing system
    0x7f9d90,   # road spline smoothing pass
    0x8088e0,   # WRoadNav::UpdateOccludedPosition
    0x7fb180,   # AI best line curvature ahead
    0x7f8930,   # closest road point passed
    0x7e7d40,   # next triangle of a packed collision strip
    0x7e7eb0,   # squared distance from a point to a segment
    0x70de00,   # car environment cube creation (src/nfsc/envmap.cpp)
    0x7218a0,   # car environment cube faces drawn this frame
    0x729320,   # eViewPlatInterface::Render: model submission into the draw list (src/nfsc/drawlist.cpp)
    0x727230,   # eDataRender::FlushList: the draw list drawn through the effects (src/nfsc/flush.cpp)
    0x60ba70,   # FastMem::Alloc (src/nfsc/allocators.cpp: FastMem and slot pools under host locks)
    0x609e80,   # FastMem::Free
    0x60a020,   # FastMem::CreateBlock
    0x60a150,   # FastMem buffer release of an object
    0x60d520,   # spline node list release into FastMem
    0x46f400,   # bMalloc(SlotPool*)
    0x46f420,   # bFree(SlotPool*, void*)
    0x477740,   # SlotPool::Malloc
    0x4777a0,   # SlotPool list allocation
    0x46f470,   # SlotPool::FlushSlotPool
    0x4770c0,   # SlotPool::CleanupExpandedSlotPools
    0x477210,   # SlotPoolManager::CleanupExpandedSlotPools
    0x477540,   # SlotPool::NewSlotPool
    0x4775b0,   # SlotPool::ExpandSlotPool
    0x477860,   # SlotPoolManager::NewSlotPool
    0x4778b0,   # SlotPoolManager::DeleteSlotPool
    0x477b80,   # bNewSlotPool
    0x477c10,   # shared 12-byte slot pool creation
    0x699230,   # Main_DisplayFrame (src/nfsc/display.cpp: on its own thread with display_parallel=1)
    0x476b20,   # bWareMalloc (src/nfsc/census.cpp: allocations tracked with display_census)
    0x476d70,   # bFree(void*)
    0x477230,   # bMalloc(int, int): its callers are the census's allocation sites
    0x461340,   # Attrib::Instance constructor (src/nfsc/gamestate.cpp: collection reference counts under one host lock)
    0x461380,   # Attrib::Instance copy constructor
    0x461b50,   # Attrib::RefSpec copy constructor
    0x465a30,   # Attrib::RefSpec::GetCollection
    0x465a80,   # Attrib::RefSpec::GetCollectionWithDefault
    0x467370,   # Attrib collection reference
    0x4681c0,   # Attrib::Collection::SetParent
    0x4682b0,   # Attrib::Collection parent change
    0x4691d0,   # Attrib collection release
    0x469270,   # Attrib collection change
    0x469610,   # Attrib::Instance lookup with a reference
    0x469870,   # Attrib collection release
    0x469a20,   # Attrib::Instance::Unmodify
    0x469bc0,   # Attrib::Instance::Change(Collection*)
    0x469c40,   # Attrib::Instance::Change(RefSpec&)
    0x469cb0,   # Attrib::Instance::ChangeWithDefault
    0x469d10,   # Attrib::ReleaseCollection
    0x469d50,   # Attrib::RefSpec::Clean
    0x46ad10,   # Attrib::RefSpec::operator=
    0x46ad80,   # Attrib::RefSpec::SetCollection
    0x468880,   # Attrib::Instance::Instance (src/nfsc/gamestate.cpp: game state under one host lock)
    0x4693d0,   # Attrib::Instance::~Instance
    0x46ac80,   # Attrib::Instance::operator=
    0x468cc0,   # Attrib::Database::Delete
    0x46b3c0,   # Attrib::Database::CollectGarbage
    0x46daa0,   # bRandom(int, unsigned int*)
    0x46dae0,   # bRandom(float, unsigned int*)
    0x46db30,   # bRandom(int)
    0x46db70,   # bRandom(float)
    0x471c80,   # bSharedStringPool::Allocate
    0x471ec0,   # bSharedStringPool::Free
    0x6b3b30,   # Joylog::GetData(int, JoylogChannel)
    0x6b3bc0,   # Joylog::GetData(void*, int, JoylogChannel)
    0x6b3b90,   # Joylog::GetSignedData
    0x6b3c50,   # Joylog::AddData(int, int, JoylogChannel)
    0x6b3c80,   # Joylog::AddData(void const*, int, JoylogChannel)
    0x6b3d00,   # Joylog::AddOrGetData(unsigned int, int, JoylogChannel)
    0x6b3db0,   # Joylog::AddOrGetData(char*, JoylogChannel)
    0x6b3ee0,   # Joylog::AddOrGetData(unsigned short*, JoylogChannel)
    0x626f90,   # GManager::AllocIcon
    0x561590,   # EventSys event allocation in a queue
    0x5612c0,   # EventSys flush (src/nfsc/defer.cpp: deferred sim updates run after it with display_defer=1)
    0x6741f0,   # the sim's world update event
    0x671b20,   # the sim's render update event
    0x66c2a0,   # the sim's camera update event
    0x66b8c0,   # the sim's animation update event (deferred with display_overlap)
    0x732840,   # empty wait for the end of rendering (src/nfsc/overlap.cpp: the display overlap's barrier)
    0x6b68d0,   # ServiceResourceLoading (after the display overlap's barrier)
    0x6b7be0,   # UnloadResourceFile
    0x7d9320,   # World_Shutdown
    0x45e690,   # CAnimScene::UpdateTime (after the display overlap's barrier)
    0x45b050,   # CAnimCtrl::UpdateAnimPose
    0x453d80,   # CAnimCtrl::AdvanceAnimTime
    0x45ba20,   # CBasicCharacterAnimEntity::UpdateTimeStep
    0x7da310,   # VehicleRenderConn constructor (after the display overlap's barrier)
    0x7e3b40,   # VehicleRenderConn destructor
    0x7e3be0,   # CarRenderConn destructor
    0x7bf630,   # VehicleRenderConn::OnLoaded
    0x7a7060,   # TrackStreamer::HandleMemoryAllocation (after the display overlap's barrier)
    0x7a81b0,   # TrackStreamer::MakeSpaceInPool (src/nfsc/streaming.cpp: counted)
    0x7e3580,   # CarLoader::MakeSpaceInCarMemoryPool
    0x7e4700,   # CarLoader::MakeSpaceInPool
    0x79e670,   # TrackStreamer::ActivateSection (src/nfsc/streaming.cpp: one per frame with section_activate_per_frame)
    0x7a2a20,   # TrackStreamer::UnactivateSection
    0x6ad8f0,   # LoadChunks (after the display overlap's barrier)
    0x759510,   # AcidEffect::AcidEffect
    0x55bf70,   # eModel::UnInit
    0x79f150,   # scenery hierarchy helper (LoaderScenery, ForceSectionToUnload, FindSceneryHeirarchyByName)
    0x750e30,   # AcidEffect::~AcidEffect
    0x55bb60,   # eStreamPackLoader::UnloadStreamingEntry
    0x45d830,   # CAnimScene::UnBindToGame
    0x699d20,   # EnableBarrierSceneryGroup
    0x6a8ad0,   # RedoTopologyAndSceneryGroups
    0x7a2ed0,   # per-section scenery group refresh
    0x94c850,   # NetCritEnter (src/nfsc/netcrit.cpp: waits with the execution lock released)
    0x94c800,   # NetCritTry
    0x94c8f0,   # NetCritLeave
    0x55cfd0,   # GetTextureInfo (under the game state lock while a display call overlaps)
    0x8127b0,   # WCollisionMgr::GetInstanceListGuts
    0x812a70,   # WCollisionMgr::GetInstanceListGuts
    0x7a0590,   # VisibleSectionManager::FindDrivableSection(int) (under the game state lock, as GetTextureInfo)
    0x7a4680,   # TrackStreamer::DetermineStreamingSections (the same, for its inlined copy of that search)
    0x7a3160,   # VisibleSectionManager::ActivateOverlay (the same, for its inlined copy of that search)
    0x7a3290,   # VisibleSectionManager::FindDrivableSection(bVector3) (its walks of that list under the same lock)
    0x7a03e0,   # VisibleSectionManager::FindClosestBoundary (walks that list, under the same lock)
    0x7a85e0,   # TrackStreamer::ServiceGameState (src/nfsc/streaming.cpp)
    0x7a7e30,   # TrackStreamer::WaitForCurrentLoadingToComplete
    0x7a7e10,   # TrackStreamer::ServiceNonGameState
    0x79e700,   # TrackStreamer section unload (counted for the perf_log pool line)
    0x7a2a90,   # TrackStreamer section jettison
    0x79def0,   # TSMemoryPool::Malloc
    0x725b40,   # deferred Direct3D object releases (at the display overlap's barrier while a call runs)
    0x726090,   # deferred Direct3D object releases of the frame end
    0x5bb510,   # FEManager::Update (display_overlap=4 starts the display call after MainLoop's call)
    0x521fe0,   # EAXSound::Update (src/nfsc/audioupdate.cpp: on the audio thread with audio_parallel)
    0x521860,   # EAXAemsManager::Update (on the game thread when the audio thread reaches it)
    0x8cf74b,   # Csis::System::Lock, the audio System lock (counted for perf_log)
    0x79fa60,   # ScenerySectionHeader::DrawAScenery (src/nfsc/displayports.cpp)
    0x72eba0,   # 3D frame (src/nfsc/framephases.cpp: timed per frame with perf_log)
    0x72e0f0,   # road reflection pass
    0x72e460,   # car environment map face
    0x72e350,   # mirror / PIP views
    0x7275a0,   # motion blur depth pass
    0x72e5b0,   # main view
    0x729770,   # post chain
    0x730cb0,   # front end / HUD layer
    0x7a4ec0,   # emProcessAllEvents
    0x7dcd20,   # World_Service
    0x6adcc0,   # ResourceFile service: decompression and chunk loading of a read file
    0x6b4760,   # CheckQueuedFileCallbacks
    0x557f30,   # eFixUpTables
    0x69ede0,   # LZDecompress
    0x6b5d30,   # StartQueuedFileReading
    0x699320,   # one chunk's load
    0x55c240,   # InternalLoaderSolidHeaderChunks (timed with perf_log)
    0x55eed0,   # InternalLoaderSolidChunks
    0x55dc20,   # NotifySolidLoader
    0x55c830,   # eModel::ConnectSolid
    0x55dd60,   # eSolid::FixTextureTable
    0x7473a0,   # elGetLightMaterial
    0x560400,   # InternalLoadTexturePackHeaderChunks
    0x55f6c0,   # TexturePack::AssignTextureData
    0x55dfc0,   # eTexture::NotifyTexturePackLoading
    0x55ebd0,   # eSolidNotifyTextureLoading
    0x55cd60,   # InitTextureAnimations
    0x55caa0,   # InternalLoadVRAMDataChunks
    0x79afa0,   # GrandSceneryCullInfo::StuffScenery
    0x7d1c40,   # WorldModel::Render
    0x72bc30,   # draw list sort
    0x74e350,   # Render::PreDisplay
    0x7103c0,   # 3D frame set-up call
    0x714db0,   # 3D frame set-up call
    0x7300d0,   # HUD layer 2D call
    0x750c30,   # HUD layer light flare pool
    0x750d40,   # HUD layer light flare pool
    0x6fca30,   # SuspensionSimple::DoWheelForces (src/nfsc/physics.cpp)
    0x5fcdc0,   # FEPackage::UpdateObject with its groups, tracks and interpolation (src/nfsc/feng.cpp)
]

# Busy-wait loops: the generated code releases the execution lock there so other guest threads can run.
THREAD_SEGMENTS = []

# x87 operands at fixed addresses of the read-only data become constants (analysis.Program.find_constants), except
# the ranges the port writes: the mixer set-up rate (src/nfsc/mixrate.cpp).
CONSTANT_SECTIONS = ['.rdata']
MUTABLE_CONSTANTS = [(0xa03990, 4)]
# Resource data, read through the resource functions only: not scanned for pointers.
RESOURCE_SECTIONS = ['.rsrc']

# Net x87 pushes of functions the depth analysis cannot follow (they leave through a computed jump).
FPU_DELTAS = {}

# Functions that save the caller's flags on entry (pushfd) and restore them before returning (popfd).
FLAGS_PRESERVED = [
    0x8aac87,   # CPUID support test of the audio library
    0x8add90,   # CPUID support test of the audio library
    0x8b6747,   # CPUID support test of the audio library
]

# pushfd -> popfd pairs that save the flags around a call and restore them; the pushed word is read only by the popfd.
FLAGS_RESTORED = {
    0x8bb4c0: 0x8bb4ca,     # audio routine 0x8bb48a around the call of 0x8bb40c
    0x8bb5b7: 0x8bb5c1,     # audio routine 0x8bb57f around the call of 0x8bb40c
}

# Pointer candidates that are no function pointers: discovery made them entries, but each lies inside another
# function's code and reads flags set before it. They are not registered for indirect calls (those trap).
NOT_FUNCTION_POINTERS = [
    0x437000,   # inside 0x436f50; a dword of the size table at 0xa79690
    0x440053,   # inside 0x440030; a dword of the 16-bit tables at 0xa0db18
    0x446881,   # inside 0x446720; a dword of resource data at 0xbcebf4
    0x49fffe,   # inside 0x49fff0; a dword of resource data at 0xbf65c8
    0x4e5854,   # mid-instruction; the string "TXN" at 0xa17d6c
    0x5aca06,   # inside 0x5ac9f0; an immediate stored into a structure at 0x71ff77 and 0x7247d9
    0x648f4a,   # inside 0x648f40; one of the mid-instruction addresses at 0x9c1338
    0x6683f0,   # inside 0x6683b0; a dword of the records at 0xa68568
    0x668574,   # inside 0x668530; a dword of the records at 0xa686ec
    0x670029,   # mid-instruction; a dword of the 16-bit tables at 0xa0de60
    0x6e6f4d,   # mid-instruction; the string "Mon" at 0xa2260c
]

# SIMD code that exists only behind CPU feature tests; the runtime reports no MMX, SSE or 3DNow!, so these ranges
# trap if reached. A SIMD instruction outside them counts as unsupported. MMX and SSE single precision are
# translated (codegen/mmx.py, codegen/sse.py); the SSE2 integer kernels below are not.
SIMD_GATED = [
    # Movie decoder kernels, stored in the function tables at 0x883000 only when 0x885821 finds SSE2 on a
    # GenuineIntel or AuthenticAMD CPU (the MMX kernels are taken otherwise).
    (0x886f46, 0x888200, 'movie decoder SSE2 kernels (0x883000 / 0x885821)'),
]

# Names of the generated files: <module>.cpp/.h, <module>.<bucket>.cpp, the layout header and the sound bank routines
# (<module>_bankcode.cpp, <module>_bankcode.<n>.cpp).
GENERATED_FILE = re.compile(r'^%s(\.[0-9a-f]+)?\.(cpp|h)$|^%s_layout\.h$|^%s_bankcode(\.[0-9]+)?\.cpp$'
                            % (MODULE, MODULE, MODULE))
# Sound banks with x86 routines (disasm/bankcode.py), below the executable's folder.
SOUND_DIR = 'SOUND'


class GameModule:
    """The executable as the analyses and the writer see it."""
    def __init__(self, image, subroutines):
        self.application_name = MODULE
        self.image = image
        self.entry_point = image.entry_point
        self.exported_symbols = []
        self.subroutines = subroutines


def check_binary(directory):
    path = os.path.join(directory, EXE_NAME)
    if not os.path.isfile(path):
        sys.exit('missing %s' % path)
    # Checked before parsing: other files are refused whatever they contain.
    with open(path, 'rb') as f:
        sha256 = hashlib.sha256(f.read()).hexdigest()
    if sha256 != EXE_SHA256:
        sys.exit('%s: SHA-256 %s, expected %s' % (path, sha256, EXE_SHA256))
    return pe_image.Image(path)


def simd_range(address):
    return next((r for r in SIMD_GATED if r[0] <= address < r[1]), None)


def write_analysis(path, program, module, discovery):
    functions = []
    for sub in module.subroutines:
        a = sub.get_entry_point()
        info = program.stack.get(a)
        functions.append({'entry': a, 'origin': discovery.origin[a], 'ret_live': program.ret_live[a],
                          'live_in': program.live_in[a], 'preserved': info.preserved if info else 0,
                          'native': a in program.native_overrides, 'instructions': len(sub.instructions),
                          'no_return': a in discovery.no_return, 'address_taken': a in program.address_taken})
    sections = [{'name': s.name, 'address': s.address, 'length': s.virtual_size} for s in module.image.sections]
    tables = [{'address': t.address, 'targets': t.targets, 'bounded': t.bounded} for t in discovery.tables.values()]
    rejected = [{'address': a, 'origin': o, 'reason': r} for a, (o, r) in sorted(discovery.rejected.items())]
    sources = {'code_refs': {a: sorted(s) for a, s in discovery.code_refs.items()},
               'data_pointers': {a: sorted(s) for a, s in discovery.data_pointers.items()},
               'string_pointers': {a: sorted(s) for a, s in discovery.string_pointers.items()}}
    traps = [{'address': a, 'mnemonic': m, 'class': k} for a, m, k in sorted(set(codegen.simd_sites))]
    code_ranges = []
    for a in sorted(discovery.code):
        end = a + discovery.instruction(a).size
        if code_ranges and a <= code_ranges[-1][1]:
            code_ranges[-1][1] = max(code_ranges[-1][1], end)
        else:
            code_ranges.append([a, end])
    mnemonics = collections.Counter(discovery.instruction(a).mnemonic for a in sorted(discovery.code))
    with open(path, 'w') as f:
        json.dump({'functions': functions, 'sections': sections, 'tables': tables, 'gaps': discovery.gaps(),
                   'rejected': rejected, 'sources': sources, 'simd_traps': traps,
                   'unsupported': sorted(set(codegen.unhandled_sites)), 'rounds': discovery.rounds,
                   'index_tables': discovery.index_tables, 'code_ranges': code_ranges,
                   'mnemonics': dict(mnemonics), 'no_return': sorted(discovery.no_return)}, f)


def sync_output(stage, out_dir):
    """Copies the files generated in stage whose content changed (unchanged files keep their timestamps, so the build
    recompiles just the changed ones) and deletes generated files of earlier runs; other files are left alone."""
    names = set(os.listdir(stage))
    changed = removed = 0
    for name in sorted(names):
        assert GENERATED_FILE.match(name), name
        target = os.path.join(out_dir, name)
        if not os.path.isfile(target) or not filecmp.cmp(os.path.join(stage, name), target, shallow=False):
            shutil.copyfile(os.path.join(stage, name), target)
            changed += 1
    for name in os.listdir(out_dir):
        if name not in names and GENERATED_FILE.match(name) and os.path.isfile(os.path.join(out_dir, name)):
            os.remove(os.path.join(out_dir, name))
            removed += 1
    print('generated %d files into %s (%d changed, %d removed)' % (len(names), out_dir, changed, removed))


def report(discovery, module, program):
    """Statistics and every place the generated code traps. Returns the number of problems: unsupported
    instructions in reachable code and SIMD outside the gated ranges."""
    image = module.image
    origins = collections.Counter(discovery.origin[s.get_entry_point()] for s in module.subroutines)
    instructions = sum(len(s.instructions) for s in module.subroutines)
    code_bytes = sum(discovery.instruction(a).size for a in discovery.code)
    text = sum(s.virtual_size for s in image.code_sections)
    print('functions: %d (%s); %d no-return, %d address-taken; %d discovery rounds'
          % (len(module.subroutines), ', '.join('%s %d' % kv for kv in sorted(origins.items())),
             len(discovery.no_return), len(program.address_taken), discovery.rounds))
    print('instructions: %d emitted, %d distinct (%d of %d code-section bytes)'
          % (instructions, len(discovery.code), code_bytes, text))
    print('jump tables: %d (%d bounded by their switch), byte index tables: %d'
          % (len(discovery.tables), sum(t.bounded for t in discovery.tables.values()), len(discovery.index_tables)))
    reasons = collections.Counter(discovery.rejected.values())
    print('pointer candidates rejected: %d (%s); text dwords skipped: %d'
          % (len(discovery.rejected), ', '.join('%s/%s %d' % (o, r, n) for (o, r), n in sorted(reasons.items())),
             len(discovery.string_pointers)))
    gaps = discovery.gaps()
    print('code-section bytes outside functions and tables (not padding): %d in %d ranges'
          % (sum(b - a for a, b in gaps), len(gaps)))
    mnemonics = collections.Counter(discovery.instruction(a).mnemonic for a in discovery.code)
    print('mnemonics: %d distinct' % len(mnemonics))
    sites = fpustack.audit(program)
    calls = dict(((k, d), n) for (s, k, d), n in sites.items() if s == 'call')
    tails = dict(((k, d), n) for (s, k, d), n in sites.items() if s == 'tail')
    print('indirect calls: %d; import table %d (host, no check); float result inferred %d, none inferred %d '
          '(checked: %s); open %d and no static x87 depth %d (stack in cpu.fpu, no check)'
          % (sum(calls.values()), calls.get(('import', 0), 0), calls.get(('inferred', 1), 0),
             calls.get(('inferred', 0), 0), 'yes' if program.indirect_fpu_checked else 'no',
             sum(n for (k, _), n in calls.items() if k == 'open'),
             sum(n for (k, _), n in calls.items() if k == 'unknown')))
    print('indirect tail jumps into guest code: %d; float result inferred %d, none inferred %d; open %d and no static '
          'x87 depth %d' % (sum(tails.values()), tails.get(('inferred', 1), 0), tails.get(('inferred', 0), 0),
                            sum(n for (k, _), n in tails.items() if k == 'open'),
                            sum(n for (k, _), n in tails.items() if k == 'unknown')))
    traps = collections.defaultdict(set)
    for fn in program.functions.values():
        for i in fn.reachable:
            ins = fn.instructions[i]
            if fn.effects[i].boundary == 'trap':
                traps[getattr(ins, 'reason', None) or ins.mnemonic].add(ins.address)
    simd = set(a for a, _, _ in codegen.simd_sites)
    for name, sites in sorted(traps.items()):
        if sites & simd:
            continue
        sites = sorted(sites)
        print('trap: %-28s %4d site(s): %s' % (name, len(sites), ' '.join('0x%x' % a for a in sites[:12])))
    problems = 0
    by_range = collections.Counter()
    for a, m, kind in sorted(set(codegen.simd_sites)):
        r = simd_range(a)
        if r is None:
            print('UNGATED SIMD instruction: 0x%x %s (%s)' % (a, m, kind))
            problems += 1
        else:
            by_range[(r[2], kind)] += 1
    for (where, kind), n in sorted(by_range.items()):
        print('gated SIMD trap: %-6s %5d in %s' % (kind, n, where))
    if codegen.unhandled_sites:
        unhandled = collections.Counter(m for _, m in set(codegen.unhandled_sites))
        print('UNSUPPORTED instructions (trap at run time): %s' % ', '.join('%s x%d' % kv for kv in unhandled.items()))
        problems += len(set(codegen.unhandled_sites))
    return problems


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--binaries', required=True, help='directory with %s' % EXE_NAME)
    parser.add_argument('--out', required=True, help='output directory for the generated sources')
    parser.add_argument('--analysis', help='also write the analysis summary (JSON) for test tools')
    parser.add_argument('--memory-x87', action='store_true',
                        help='keep the x87 stack in cpu.fpu instead of C++ locals (for comparisons)')
    parser.add_argument('--memory-registers', action='store_true',
                        help='keep x86 registers in cpu instead of C++ locals (for comparisons)')
    parser.add_argument('--fpu-check', action='store_true',
                        help='assert the x87 depth after every call whose depth is known (finds leaking callees)')
    parser.add_argument('--partial-reads', action='store_true',
                        help='count partial register writes as reads of the whole register (for comparisons)')
    parser.add_argument('--load-constants', action='store_true',
                        help='load x87 operands of read-only data from memory, not as constants (for comparisons)')
    parser.add_argument('--alias-cpu', action='store_true',
                        help='no restrict on the cpu parameter of translated functions (for comparisons)')
    parser.add_argument('--preempt-call', action='store_true',
                        help='loop preemption points call the yield directly, not through the stub (for comparisons)')
    parser.add_argument('--cache', default=CACHE_DIR,
                        help='directory of the discovery cache (default: .gencache in the repository)')
    parser.add_argument('--fresh', action='store_true',
                        help='run discovery even when the cache has its result, and store it again')
    parser.add_argument('--jobs', type=int, default=os.cpu_count() or 1,
                        help='worker processes that write the code (default: one per core)')
    args = parser.parse_args()
    started = time.time()
    image = check_binary(args.binaries)
    print('%s: SHA-256 ok, %d sections, %d relocations, %d imports'
          % (EXE_NAME, len(image.sections), len(image.relocations), len(image.imports)))
    os.makedirs(args.out, exist_ok=True)

    cs = capstone.Cs(*CAPSTONE_MODE)
    cs.detail = True
    names = set((i.dll, i.name) for i in image.imports)
    for table in (IMPORT_FPU_DELTAS, DATA_IMPORTS):
        unknown = sorted(set(table) - names)
        if unknown:
            sys.exit('not imported by %s: %s' % (EXE_NAME, ', '.join('%s!%s' % k for k in unknown)))
    import_fpu_deltas = dict((i.iat_address, IMPORT_FPU_DELTAS[(i.dll, i.name)]) for i in image.imports
                             if (i.dll, i.name) in IMPORT_FPU_DELTAS)
    lines = []

    def log(line):
        print(line)
        lines.append(line)

    discovery = discover.Discovery(image, cs, log=log, no_return=NO_RETURN, no_return_imports=NO_RETURN_IMPORTS)
    entries = NATIVE_OVERRIDES + ([SETJMP] if SETJMP is not None else [])
    key = cache.key(image, (CAPSTONE_MODE, NO_RETURN, NO_RETURN_IMPORTS, entries))
    cached = None if args.fresh else cache.load(args.cache, key)
    if cached:
        # The discovery messages as a fresh run prints them.
        for line in cached['log']:
            print(line)
        cache.restore(discovery, cached['discovery'])
        print('discovery result from the cache (key %s)' % key[:16])
    else:
        discovery.discover(extra_entries=entries)
        try:
            cache.save(args.cache, key, {'log': lines, 'discovery': cache.discovery_state(discovery)})
        except OSError as e:
            print('discovery cache not written: %s' % e)
    print('discovery took %.0f s' % (time.time() - started))
    subroutines = [discovery.subroutine(e) for e in sorted(discovery.entries)]
    # The decoded program stays alive to the end: the garbage collector no longer scans it.
    gc.freeze()
    module = GameModule(image, subroutines)
    code_tables = {}
    for t in discovery.tables.values():
        code_tables[t.start] = code_tables[t.address] = t.end
    for a, count in discovery.index_tables.items():
        code_tables[a] = None if count is None else a + count
    program = analysis.Program([module], THREAD_SEGMENTS, [], {}, FLAGS_PRESERVED, (), NATIVE_OVERRIDES,
                               local_registers=not args.memory_registers, local_x87=not args.memory_x87,
                               setjmp=SETJMP, longjmp_jumps=LONGJMP_JUMPS, fpu_check=args.fpu_check,
                               fpu_deltas=FPU_DELTAS, preempt_loops=True, address_taken=discovery.address_taken,
                               import_fpu_deltas=import_fpu_deltas, flags_restored=FLAGS_RESTORED,
                               not_function_pointers=NOT_FUNCTION_POINTERS, partial_reads=args.partial_reads,
                               constant_sections=() if args.load_constants else CONSTANT_SECTIONS,
                               mutable_constants=MUTABLE_CONSTANTS, resource_sections=RESOURCE_SECTIONS,
                               code_tables=code_tables, restrict_cpu=not args.alias_cpu,
                               preempt_stub=not args.preempt_call)
    # With workers, they make the per-function decisions of the functions they write.
    program.analyze(decide=args.jobs <= 1)
    # Every direct call and tail call must reach a generated function.
    missing = sorted(set(x.target for fn in program.functions.values() for i in fn.reachable for x in fn.exits[i]
                         if x.kind in ('call', 'tail') and x.target not in program.functions))
    if missing:
        sys.exit('calls to addresses that are no function: %s' % ' '.join('0x%x' % a for a in missing[:20]))
    unknown = sorted(set(NOT_FUNCTION_POINTERS + FLAGS_PRESERVED) - set(program.functions))
    if unknown:
        sys.exit('listed entries that are no function: %s' % ' '.join('0x%x' % a for a in unknown))
    with tempfile.TemporaryDirectory(prefix='nfscgen-') as stage:
        out = writer.Writer(module, program, stage, data_imports=DATA_IMPORTS)
        bucket_writer = None
        if args.jobs > 1:
            bucket_writer = parallel.write_buckets(out, discovery, program, THREAD_SEGMENTS, [], args.jobs)
        out.write(bucket_writer)
        print(bankcode.write(stage, MODULE, os.path.join(args.binaries, SOUND_DIR), image, program.functions))
        sync_output(stage, args.out)
    if args.analysis:
        write_analysis(args.analysis, program, module, discovery)
    problems = report(discovery, module, program)
    print('generation took %.0f s' % (time.time() - started))
    if problems:
        sys.exit('%d unsupported or ungated instruction(s) in reachable code' % problems)


if __name__ == '__main__':
    main()
