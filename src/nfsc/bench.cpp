#include "bench.h"
#include "audio_decode.h"
#include "bankcode.h"
#include "collision.h"
#include "feng.h"
#include "guestcall.h"
#include "physics.h"
#include "scenery.h"
#include "sim.h"
#include "vehicle.h"

#include <lib/controls.h>
#include <lib/memmap.h>
#include <lib/window.h>
#include <platform/config.h>
#include <platform/log.h>
#include <platform/perf.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <strings.h>
#include <vector>

namespace nfsc {

// Translated guest functions the roam benchmark calls.
void sub_7fb090(win32::WinApplication* app, x86::CPU& cpu);    // WRoadNav::FindPath
void sub_427bf0(win32::WinApplication* app, x86::CPU& cpu);    // AIVehicle::ResetVehicleToRoadPos
void sub_7fb060(win32::WinApplication* app, x86::CPU& cpu);    // WRoadNav::CancelPathFinding

namespace bench {

namespace
{

// Skip-front-end options (written only by the joylog config pass 0x6BF080, which keeps them outside replays).
// main (0x6B8E70) calls StartSkipFERace (0x7A9B90) when SkipFe is set; QuickGame (0x75E3B0) starts the race whose
// EventID hashes to SkipFeRaceId with SkipFeRaceType as its context; AIVehicleHuman's constructor (0x442163) gives
// the player's car to the AI when SkipFeAiControl is set.
const x86::reg32 SkipFe = 0x00A9E620;
const x86::reg32 SkipFeRaceId = 0x00A63140;         // char*
const x86::reg32 SkipFeAiCars = 0x00A9E630;
const x86::reg32 SkipFeLaps = 0x00A63154;           // looping races only
const x86::reg32 SkipFeRaceType = 0x00A9E648;       // race context, 0: quick race
const x86::reg32 SkipFeAiControl = 0x00A9E656;      // byte
const x86::reg32 SkipFePlayerCar = 0x00A63144;      // char*, the car of the skip-front-end race
// The game's own switch for profiles and saves: off, MemcardManager::Update (0x58DD30) does nothing and
// IsProfileLoaded (0x58E710) answers yes, so a bench neither reads nor writes the owner's save.
const x86::reg32 MemcardEnabled = 0x00A631CC;
// Random generator state seeded by SeedRandomNumber (0x6B5650) through bSetRandomSeed (0x4736A0).
const x86::reg32 RandomSeed = 0x00A4D3D0;
// Game flow manager state (GAMEFLOW_STATE_*).
const x86::reg32 GameFlowState = 0x00A99BBC;
// Race status object and its fields; racers of 0x384 bytes from 0x18.
const x86::reg32 RaceStatus = 0x00A98284;
const x86::reg32 Racers = 0x18;
const x86::reg32 RacerSize = 0x384;
const x86::reg32 MaxRacers = 30;
const x86::reg32 RacerCount = 0x6A08;
const x86::reg32 PlayMode = 0x6A14;                 // 0 roaming, 1 racing
const x86::reg32 MasterTimerRunning = 0x6A2C;       // byte, started at the end of the countdown
const x86::reg32 ActivelyRacing = 0x6A3E;           // byte
// Racer fields: simable handle, character (none for a player), finished flag.
const x86::reg32 RacerSimable = 0x00;
const x86::reg32 RacerCharacter = 0x04;
const x86::reg32 RacerFinished = 0x46;

const char* const DefaultRace = "mu.1.1";
const uint32_t MaxFrames = 20 * 60 * 60;
const x86::reg32 GameFlowInGame = 6;

// Free roam. Without a race ID HandleSkipFEOptions sets no startup race, so GManager::PreBeginGameplay (0x645B60)
// calls GRaceStatus::SetRoaming (0x641310) and World_DEBUGStartLocation (0x7A97B0) starts the car at
// GManager::GetRespawnLocation (0x61E180). In the roaming world the traffic manager's density (0x422CC0) is
// SkipFETrafficDensity / 100 unless SkipFEDisableTraffic, which also keeps the traffic manager from being created
// (0x440F01); SetRoaming sets ICopMgr::mDisableCops (0xA83A50) from SkipFEDisableCops.
const x86::reg32 SkipFeDisableTraffic = 0x00A9E644;     // byte
const x86::reg32 SkipFeTrafficDensity = 0x00A9E640;     // float, percent in the roaming world
const x86::reg32 SkipFeDisableCops = 0x00A63158;        // byte, 1 in the image
// AIVehicleHuman (this of Update): mAiControl, the IVehicleAI interface and its drive-to WRoadNav (GetDriveToNav
// 0x43BB70 returns interface +0x38).
const x86::reg32 HumanAiControl = 0x29C;                // byte
const x86::reg32 HumanVehicleAi = 0x44;
const x86::reg32 HumanDriveToNav = HumanVehicleAi + 0x38;
// WRoadNav: valid flag, navigation type (AStarSearch::Service 0x80B9A0 sets 3 with a path, 2 without), road position
// and direction, path segment count, bOnlyPathfindForward (AStarSearch::Init 0x810B30 starts the search from both ends
// of the nav's segment unless it is set, so a path can begin behind the car, which the AI then leaves at once).
const x86::reg32 NavValid = 0x58;                       // byte
const x86::reg32 NavType = 0x74;
const x86::reg32 NavTypePath = 3;
const x86::reg32 NavPosition = 0x94;                    // UMath::Vector3, y up
const x86::reg32 NavForward = 0xB8;
const x86::reg32 NavPathSegments = 0x304;
const x86::reg32 NavOnlyForward = 0x300;                // byte
// PathFinder::pInstance and its request list (sentinel +0xA4; request: next +0, WRoadNav +0x20), as
// WRoadNav::FindingPath (0x7FB150) walks it.
const x86::reg32 PathFinder = 0x00B77EFC;
const x86::reg32 PathFinderRequests = 0xA4;
const x86::reg32 RequestNav = 0x20;
// Road network (WRoadNetwork::Init 0x813980): valid flag, nodes of 0x20 bytes (UMath::Vector3 position, segment count
// byte +0x10, segment indices u16 from +0x12) and segments of 0x16 bytes (node indices u16 +0 and +2) with their
// counts. Flag bit 0 (+0xA), which WRoadNetwork::GetNextDriveSegment (0x7EB6C0) skips, is set on most of the city's
// segments: a walk that leaves them out reaches only a few nodes around the car.
const x86::reg32 RoadNetworkValid = 0x00B77E38;         // byte
const x86::reg32 RoadNodes = 0x00B77EC8;
const x86::reg32 RoadNodeCount = 0x00B77E8C;
const x86::reg32 RoadNodeSize = 0x20;
const x86::reg32 RoadNodeSegments = 0x10;               // byte
const x86::reg32 RoadNodeSegmentIndex = 0x12;
const x86::reg32 MaxNodeSegments = (RoadNodeSize - RoadNodeSegmentIndex) / 2;
const x86::reg32 RoadSegments = 0x00B77ECC;
const x86::reg32 RoadSegmentCount = 0x00B77E84;
const x86::reg32 RoadSegmentSize = 0x16;
const x86::reg32 NavSegment = 0x8A;                     // s16, the nav's road segment
// TheTrackStreamer: sections of 0x5C bytes (status +0xC: 3 loaded, 4 activated) and their count (loader 0x79E440);
// streaming position entries of 0x90 bytes from +0xD0 (SetStreamingPosition 0x79A0B0), player 0's first: bVector3
// position, set flag +0x48.
const x86::reg32 TrackStreamer = 0x00B70650;
const x86::reg32 StreamSectionCount = 0x4;
const x86::reg32 StreamSectionSize = 0x5C;
const x86::reg32 StreamSectionStatus = 0xC;
const x86::reg32 StreamSectionLoaded = 3;
const x86::reg32 MaxStreamSections = 4096;
const x86::reg32 StreamPosition = 0xD0;
const x86::reg32 StreamPositionSet = StreamPosition + 0x48;     // byte
// Guest scratch block of the roam benchmark: path target, reset position and direction, empty race ID.
const x86::reg32 ScratchTarget = 0x00;
const x86::reg32 ScratchPosition = 0x10;
const x86::reg32 ScratchDirection = 0x20;
const x86::reg32 ScratchRaceId = 0x30;
const x86::reg32 ScratchSize = 0x40;

const int DefaultRoamSeconds = 60;
const float TargetMinDistance = 1500.0f;        // metres from the car to a new target
const float TargetMaxDistance = 2500.0f;
const float FirstTargetMinDistance = 500.0f;    // the first target is nearer: a short first search
const float FirstTargetMaxDistance = 1000.0f;
const uint32_t PathTimeoutUs = 5000000;         // a path request still running after this long is cancelled
const uint32_t FirstPathUs = 15000000;          // the first path waits for the car to move, at most this long
const float TargetReached = 150.0f;             // metres from the target where the next one is chosen
const float PathEndReached = 500.0f;            // a path that ends this close to the target counts as reached
const int TargetTries = 64;
const int MaxPathRetries = 3;                   // requests again of a lost path before the next target
const uint32_t TargetRetryUs = 3000000;         // between path requests
const int MaxTargetLines = 40;
const float MovedToStart = 20.0f;               // metres the car moves before the measurement starts
const uint32_t StartTimeoutUs = 60000000;       // the measurement starts anyway after this long in the world
// Prompts (text messages, dialogs) wait for Cross: while the game is paused (DALPauseStates' count at 0xA8AD18) or
// the roaming car has not moved for PromptAfterUs, Cross is pressed for PromptPressUs every PromptEveryUs.
const x86::reg32 PauseCount = 0x00A8AD18;
const uint32_t PromptAfterUs = 3000000;
const uint32_t PromptEveryUs = 1000000;
const uint32_t PromptPressUs = 150000;
const float StuckRadius = 15.0f;
const uint32_t StuckUs = 12000000;              // in StuckRadius this long: stuck, the car is reset
const float MaxStep = 100.0f;                   // longer moves in one frame are resets, not driven

const char* const s_flowNames[] = {
    "none", "loading front end", "unloading front end", "in front end", "loading region", "loading track",
    "racing", "unloading track", "unloading region", "exit demo disc",
};

enum Phase { Idle, Recording, Stopping, Done };

bool s_enabled = false;
bool s_roam = false;
int s_seconds = 0;
bool s_exit = false;
uint32_t s_startUs = 0;
uint32_t s_raceUs = 0;

// Roam benchmark, game thread: the guest scratch block, the AI's target (UMath::Vector3), counters for the summary.
x86::reg32 s_scratch = 0;
bool s_targetActive = false;
bool s_targetPathed = false;
float s_target[3] = {};
uint32_t s_targetRandom = 0x12345678u;
uint32_t s_lastRequestUs = 0;
bool s_carMoved = false;            // frameRoam: the car has moved MovedToStart in the world
bool s_roamWaiting = false;         // frameRoam: in the world PromptAfterUs and the car has not moved yet
uint32_t s_aiSeenUs = 0;
bool s_forwardSet = false;          // the bench set the nav's bOnlyPathfindForward
uint32_t s_timeouts = 0;
int s_targetLines = 0;
bool s_aiSeen = false;
bool s_resetRequest = false;
bool s_retryTarget = false;
int s_pathRetries = 0;
uint32_t s_targets = 0, s_reached = 0, s_noPath = 0, s_dropped = 0, s_resets = 0, s_requests = 0;
std::vector<uint8_t> s_sectionLoaded;
// Road nodes reachable from the car's segment (pickTarget's search), and the search's marks.
std::vector<uint32_t> s_reachable;
std::vector<uint8_t> s_nodeSeen;
// Summary text after the common fields; written before Stopping, read by the render thread after it.
char s_extra[384] = "";

// Written by the render thread while Recording and Stopping, read by it alone.
std::atomic<int> s_phase{Idle};
std::vector<uint32_t> s_frames;
uint32_t s_frameCount = 0;
uint32_t s_lastPresent = 0;
const char* s_endReason = "";

// Per-frame series: written by one thread each while Recording, read by the render thread at Stopping.
struct SeriesData
{
    std::vector<uint32_t> values;
    std::atomic<uint32_t> count{0};
};
SeriesData s_series[SeriesCount];

struct Stats
{
    uint32_t count;
    double average;
    uint32_t p99;
    uint32_t max;
    uint32_t over;      // above 33.4 ms
};

Stats stats(const uint32_t* values, uint32_t count)
{
    Stats result = { count, 0.0, 0, 0, 0 };
    if (!count)
        return result;
    uint64_t total = 0;
    for (uint32_t i = 0; i < count; ++i)
    {
        total += values[i];
        result.max = std::max(result.max, values[i]);
        result.over += values[i] > 33400;
    }
    result.average = double(total) / count;
    std::vector<uint32_t> sorted(values, values + count);
    const size_t rank = std::min<size_t>(count - 1, (size_t(count) * 99 + 99) / 100 - 1);
    std::nth_element(sorted.begin(), sorted.begin() + rank, sorted.end());
    result.p99 = sorted[rank];
    return result;
}

// The summary's part for the per-frame series.
void seriesText(char* text, size_t size)
{
    const Stats display = stats(s_series[SeriesDisplay].values.data(),
                                s_series[SeriesDisplay].count.load(std::memory_order_acquire));
    const Stats game = stats(s_series[SeriesGame].values.data(),
                             s_series[SeriesGame].count.load(std::memory_order_acquire));
    const Stats steps = stats(s_series[SeriesSimSteps].values.data(),
                              s_series[SeriesSimSteps].count.load(std::memory_order_acquire));
    const Stats gpu = stats(s_series[SeriesGpu].values.data(),
                            s_series[SeriesGpu].count.load(std::memory_order_acquire));
    snprintf(text, size, " | display call avg %.2f ms p99 %.2f max %.2f, over 33.4 ms %u of %u | game thread without "
             "display waits avg %.2f ms p99 %.2f max %.2f, over 33.4 ms %u of %u | sim update events avg %.2f p99 %u "
             "max %u | gpu avg %.2f ms p99 %.2f max %.2f, over 33.4 ms %u of %u",
             display.average / 1000.0, display.p99 / 1000.0, display.max / 1000.0, display.over, display.count,
             game.average / 1000.0, game.p99 / 1000.0, game.max / 1000.0, game.over, game.count, steps.average,
             steps.p99, steps.max, gpu.average / 1000.0, gpu.p99 / 1000.0, gpu.max / 1000.0, gpu.over, gpu.count);
}

// Display callback.
void gpuFrame(uint32_t microseconds)
{
    sample(SeriesGpu, microseconds);
}

x86::reg32 dword(x86::reg32 address)
{
    x86::reg32 value;
    memcpy(&value, win32::g_guestBase + address, 4);
    return value;
}

x86::reg8 byte(x86::reg32 address)
{
    return win32::g_guestBase[address];
}

void setDword(x86::reg32 address, x86::reg32 value)
{
    memcpy(win32::g_guestBase + address, &value, 4);
}

// bSetRandomSeed (0x4736A0) of SeedRandomNumber's start value 0x12345678 with a zero ticker.
x86::reg32 fixedSeed()
{
    const x86::reg32 start = 0x12345678u ^ 0x1D872B41u;
    const x86::reg32 mixed = start ^ (start >> 5);
    return mixed ^ (mixed << 27) ^ start;
}

double seconds()
{
    return double(pf::perf::now() - s_startUs) / 1000000.0;
}

// First racer added as a player (a simable without a character), or 0.
x86::reg32 playerRacer(x86::reg32 status)
{
    const x86::reg32 count = std::min(dword(status + RacerCount), MaxRacers);
    for (x86::reg32 i = 0; i < count; ++i)
    {
        const x86::reg32 racer = status + Racers + i * RacerSize;
        if (dword(racer + RacerSimable) && !dword(racer + RacerCharacter))
            return racer;
    }
    return 0;
}

// Render thread.
void framePresented(uint32_t presentUs)
{
    const int phase = s_phase.load(std::memory_order_acquire);
    if (phase == Recording && s_lastPresent && s_frameCount < s_frames.size())
        s_frames[s_frameCount++] = presentUs - s_lastPresent;
    s_lastPresent = presentUs;
    if (phase != Stopping)
        return;

    const uint32_t frames = s_frameCount;
    uint64_t total = 0;
    uint32_t max = 0, over33 = 0, over50 = 0;
    for (uint32_t i = 0; i < frames; ++i)
    {
        const uint32_t t = s_frames[i];
        total += t;
        max = std::max(max, t);
        over33 += t > 33400;
        over50 += t > 50000;
    }
    uint32_t p99 = 0;
    if (frames)
    {
        std::vector<uint32_t> sorted(s_frames.begin(), s_frames.begin() + frames);
        const size_t rank = std::min<size_t>(frames - 1, (size_t(frames) * 99 + 99) / 100 - 1);
        std::nth_element(sorted.begin(), sorted.begin() + rank, sorted.end());
        p99 = sorted[rank];
    }
    const double elapsed = double(total) / 1000000.0;
    char series[384];
    seriesText(series, sizeof(series));
    pf::logInfo("bench: summary (%s): %u frames in %.1f s, %.2f fps | frame avg %.2f ms p99 %.2f max %.2f"
                " | over 33.4 ms %u (%.1f%%), over 50 ms %u (%.1f%%) | guest memory peak %u MB of %u MB%s%s",
                s_endReason, unsigned(frames), elapsed, elapsed > 0.0 ? double(frames) / elapsed : 0.0,
                frames ? double(total) / 1000.0 / frames : 0.0, double(p99) / 1000.0, double(max) / 1000.0,
                unsigned(over33), frames ? 100.0 * over33 / frames : 0.0,
                unsigned(over50), frames ? 100.0 * over50 / frames : 0.0,
                unsigned(win32::MemMap::peakBytes() >> 20), unsigned(win32::MemMap::totalBytes() >> 20), series,
                s_extra);
    sim::logShadow();
    physics::logShadow();
    collision::logShadow();
    feng::logShadow();
    scenery::logShadow();
    vehicle::logShadow();
    bankcode::logShadow();
    audiodecode::logShadow();
    if (s_frameCount == s_frames.size())
        pf::logInfo("bench: frame buffer full, later frames not counted");
    s_phase.store(Done, std::memory_order_release);
}

void vector(x86::reg32 address, float* value)
{
    memcpy(value, win32::g_guestBase + address, 12);
}

float distance(const float* a, const float* b)
{
    const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return std::sqrt(x * x + y * y + z * z);
}

// Horizontal distance of two UMath::Vector3 (y up).
float flatDistance(const float* a, const float* b)
{
    return std::hypot(a[0] - b[0], a[2] - b[2]);
}

// Player 0's streaming position (bVector3), false before the streamer has one.
bool streamPosition(float* position)
{
    if (!byte(TrackStreamer + StreamPositionSet))
        return false;
    vector(TrackStreamer + StreamPosition, position);
    return true;
}

// Track sections that became loaded or activated since the last call. With start, or when the section count
// changed, it only records them: it returns how many are loaded at the start, 0 after a count change.
uint32_t sectionsLoaded(bool start)
{
    const x86::reg32 sections = dword(TrackStreamer);
    const x86::reg32 count = sections ? std::min(dword(TrackStreamer + StreamSectionCount), MaxStreamSections) : 0;
    const bool record = start || s_sectionLoaded.size() != count;
    if (record)
        s_sectionLoaded.assign(count, 0);
    uint32_t result = 0;
    for (x86::reg32 i = 0; i < count; ++i)
    {
        const uint8_t loaded = dword(sections + i * StreamSectionSize + StreamSectionStatus) >= StreamSectionLoaded;
        result += loaded && (record ? start : !s_sectionLoaded[i]);
        s_sectionLoaded[i] = loaded;
    }
    return result;
}

// WRoadNav::FindingPath (0x7FB150): a path request of the nav is queued or running.
bool findingPath(x86::reg32 nav)
{
    const x86::reg32 finder = dword(PathFinder);
    if (!finder)
        return false;
    const x86::reg32 end = finder + PathFinderRequests;
    int guard = 0;
    for (x86::reg32 request = dword(end); request != end && request && guard < 1024; request = dword(request), ++guard)
        if (dword(request + RequestNav) == nav)
            return true;
    return false;
}

// Target log lines are state changes, but a car that keeps losing its path could fill the log: the first ones only.
bool targetLine()
{
    if (s_targetLines > MaxTargetLines)
        return false;
    if (s_targetLines++ < MaxTargetLines)
        return true;
    pf::logInfo("bench: further target lines left out (the summary counts them)");
    return false;
}

// The road nodes connected to the nav's segment (the whole connected part of the road graph, by a breadth-first walk)
// into s_reachable; empty when the network or the segment is not valid. The walk runs again only when the car's
// segment is outside the last result.
void reachableNodes(x86::reg32 nav)
{
    const x86::reg32 nodes = dword(RoadNodes);
    const x86::reg32 segments = dword(RoadSegments);
    const x86::reg32 nodeCount = dword(RoadNodeCount);
    const x86::reg32 segmentCount = dword(RoadSegmentCount);
    const int32_t start = int16_t(rc::ld16(nav + NavSegment));
    if (!byte(RoadNetworkValid) || !nodes || !segments || start < 0 || x86::reg32(start) >= segmentCount)
    {
        s_reachable.clear();
        return;
    }
    const x86::reg32 startNode = rc::ld16(segments + x86::reg32(start) * RoadSegmentSize);
    if (!s_reachable.empty() && s_nodeSeen.size() == nodeCount && startNode < nodeCount && s_nodeSeen[startNode])
        return;
    s_reachable.clear();
    s_nodeSeen.assign(nodeCount, 0);
    for (x86::reg32 end = 0; end < 2; ++end)
    {
        const x86::reg32 node = rc::ld16(segments + x86::reg32(start) * RoadSegmentSize + end * 2);
        if (node < nodeCount && !s_nodeSeen[node])
        {
            s_nodeSeen[node] = 1;
            s_reachable.push_back(node);
        }
    }
    for (size_t next = 0; next < s_reachable.size(); ++next)
    {
        const x86::reg32 node = nodes + s_reachable[next] * RoadNodeSize;
        const x86::reg32 links = std::min<x86::reg32>(byte(node + RoadNodeSegments), MaxNodeSegments);
        for (x86::reg32 i = 0; i < links; ++i)
        {
            const x86::reg32 segment = rc::ld16(node + RoadNodeSegmentIndex + i * 2);
            if (segment >= segmentCount)
                continue;
            const x86::reg32 record = segments + segment * RoadSegmentSize;
            for (x86::reg32 end = 0; end < 2; ++end)
            {
                const x86::reg32 other = rc::ld16(record + end * 2);
                if (other < nodeCount && !s_nodeSeen[other])
                {
                    s_nodeSeen[other] = 1;
                    s_reachable.push_back(other);
                }
            }
        }
    }
}

// A road node connected to the car's segment between nearest and farthest metres from it (the try nearest to that
// range when none is), by the benchmark's own random sequence: the game's generator stays untouched.
bool pickTarget(x86::reg32 nav, const float* from, float nearest, float farthest, x86::reg32* index)
{
    reachableNodes(nav);
    if (s_reachable.empty())
        return false;
    const x86::reg32 nodes = dword(RoadNodes);
    float best = -1.0f;
    for (int i = 0; i < TargetTries && best != 0.0f; ++i)
    {
        s_targetRandom ^= s_targetRandom << 13;
        s_targetRandom ^= s_targetRandom >> 17;
        s_targetRandom ^= s_targetRandom << 5;
        const x86::reg32 candidate = s_reachable[s_targetRandom % s_reachable.size()];
        float position[3];
        vector(nodes + candidate * RoadNodeSize, position);
        const float away = flatDistance(from, position);
        const float outside = away < nearest ? nearest - away : away > farthest ? away - farthest : 0.0f;
        if (best < 0.0f || outside < best)
        {
            best = outside;
            *index = candidate;
            memcpy(s_target, position, sizeof(s_target));
        }
    }
    return true;
}

// AIVehicle::ResetVehicleToRoadPos (0x427BF0, on IVehicleAI) at the drive-to nav's road position and direction: the
// AI's own reset back onto the road. It sets the nav up again there, which ends a path: the target's path is
// requested again.
void resetCar(win32::WinApplication* app, x86::CPU& cpu, x86::reg32 ai, x86::reg32 nav)
{
    memcpy(win32::g_guestBase + s_scratch + ScratchPosition, win32::g_guestBase + nav + NavPosition, 12);
    memcpy(win32::g_guestBase + s_scratch + ScratchDirection, win32::g_guestBase + nav + NavForward, 12);
    callGuest(app, cpu, &sub_427bf0, ai + HumanVehicleAi,
              { s_scratch + ScratchPosition, s_scratch + ScratchDirection });
    ++s_resets;
    s_retryTarget = s_targetActive && s_pathRetries < MaxPathRetries;
    s_targetActive = false;
}

// Presses Cross on the game's prompts (see PromptAfterUs), so a run never waits for the player.
void answerPrompts()
{
    static uint32_t s_sinceUs = 0;
    static bool s_prompted = false;
    static uint32_t s_answers = 0;
    const bool prompt = int32_t(dword(PauseCount)) > 0 || s_roamWaiting;
    const uint32_t now = pf::perf::now();
    if (!prompt)
    {
        if (s_prompted)
            win32::controls::setHeld(0);
        s_prompted = false;
        return;
    }
    if (!s_prompted)
    {
        s_prompted = true;
        s_sinceUs = now;
    }
    const uint32_t phase = (now - s_sinceUs) % PromptEveryUs;
    const bool press = phase < PromptPressUs;
    if (press && !(s_answers & 1))
        pf::logInfo("bench: %.2f s a prompt is waiting (%s), Cross pressed (%u)", seconds(),
                    s_roamWaiting ? "the car has not moved" : "the game is paused", s_answers / 2 + 1);
    if (press != bool(s_answers & 1))
        ++s_answers;
    win32::controls::setHeld(press ? win32::controls::bit(win32::controls::Cross) : 0);
}

// Race benchmark: race state changes, the measurement from the end of the countdown to the finish.
void frameRace()
{
    static x86::reg32 s_mode = ~0u;
    static int s_timer = -1, s_active = -1, s_finished = -1;

    const x86::reg32 status = dword(RaceStatus);
    const x86::reg32 mode = status ? dword(status + PlayMode) : ~0u;
    const int timer = status ? byte(status + MasterTimerRunning) : -1;
    const int active = status ? byte(status + ActivelyRacing) : -1;
    const x86::reg32 player = status ? playerRacer(status) : 0;
    const int finished = player ? byte(player + RacerFinished) : -1;
    if (mode != s_mode || timer != s_timer || active != s_active || finished != s_finished)
    {
        pf::logInfo("bench: %.2f s race: %s, race timer %s, actively racing %d, player finished %d, %d racers",
                    seconds(), mode == 1 ? "racing (intro and countdown until the timer runs)"
                    : mode == 0 ? "roaming" : "no race status", timer == 1 ? "running" : "stopped", active, finished,
                    status ? int(dword(status + RacerCount)) : 0);
        s_mode = mode;
        s_timer = timer;
        s_active = active;
        s_finished = finished;
    }

    const int phase = s_phase.load(std::memory_order_acquire);
    if (phase == Idle)
    {
        if (mode != 1 || timer != 1)
            return;
        pf::logInfo("bench: %.2f s countdown over, measuring", seconds());
        s_raceUs = pf::perf::now();
        s_phase.store(Recording, std::memory_order_release);
        return;
    }
    if (phase == Recording)
    {
        const char* reason = nullptr;
        if (finished == 1)
            reason = "player finished";
        else if (mode != 1)
            reason = "race left the racing mode";
        else if (s_seconds && pf::perf::now() - s_raceUs >= uint32_t(s_seconds) * 1000000u)
            reason = "time limit";
        if (!reason)
            return;
        s_endReason = reason;
        pf::logInfo("bench: %.2f s measurement ended: %s", seconds(), reason);
        s_phase.store(Stopping, std::memory_order_release);
    }
}

// Roam benchmark: play mode changes; the measurement starts when the car has moved in the roaming world and runs for
// bench_seconds. While measuring: distance, sections loaded, and a reset request when the car is stuck.
void frameRoam(x86::reg32 flow)
{
    static x86::reg32 s_mode = ~0u;
    static bool s_inWorld = false;
    static uint32_t s_worldUs = 0, s_anchorUs = 0;
    static float s_origin[3], s_anchor[3], s_last[3];
    static double s_driven = 0.0;
    static uint32_t s_jumps = 0, s_loaded = 0, s_resident = 0;
    static uint32_t s_base[7];

    const x86::reg32 status = dword(RaceStatus);
    const x86::reg32 mode = status ? dword(status + PlayMode) : ~0u;
    if (mode != s_mode)
    {
        pf::logInfo("bench: %.2f s play mode: %s", seconds(),
                    mode == 0 ? "roaming" : mode == 1 ? "racing" : "no race status");
        s_mode = mode;
    }
    float position[3];
    const bool positioned = streamPosition(position);
    const uint32_t now = pf::perf::now();

    const int phase = s_phase.load(std::memory_order_acquire);
    if (phase == Idle)
    {
        if (flow != GameFlowInGame || mode != 0 || !positioned)
            return;
        if (!s_inWorld)
        {
            s_inWorld = true;
            s_worldUs = now;
            memcpy(s_origin, position, sizeof(s_origin));
            pf::logInfo("bench: %.2f s roaming at (%.0f, %.0f, %.0f), waiting for the car to move", seconds(),
                        position[0], position[1], position[2]);
        }
        if (distance(position, s_origin) >= MovedToStart)
        {
            s_carMoved = true;
            pf::logInfo("bench: %.2f s the car is driving, measuring", seconds());
        }
        else if (now - s_worldUs >= StartTimeoutUs)
            pf::logInfo("bench: %.2f s the car has not moved %.0f m in %u s (AI control %s), measuring anyway",
                        seconds(), MovedToStart, unsigned(StartTimeoutUs / 1000000u),
                        s_aiSeen ? "seen" : "never seen");
        else
        {
            s_roamWaiting = now - s_worldUs >= PromptAfterUs;
            return;
        }
        s_roamWaiting = false;
        s_raceUs = now;
        s_anchorUs = now;
        memcpy(s_anchor, position, sizeof(s_anchor));
        memcpy(s_last, position, sizeof(s_last));
        s_resident = sectionsLoaded(true);
        const uint32_t base[7] = { s_targets, s_reached, s_noPath, s_dropped, s_resets, s_requests, s_timeouts };
        memcpy(s_base, base, sizeof(s_base));
        s_phase.store(Recording, std::memory_order_release);
        return;
    }
    if (phase != Recording)
        return;

    if (positioned)
    {
        const float step = distance(position, s_last);
        if (step > MaxStep)
            ++s_jumps;
        else
            s_driven += step;
        memcpy(s_last, position, sizeof(s_last));
        if (distance(position, s_anchor) > StuckRadius)
        {
            memcpy(s_anchor, position, sizeof(s_anchor));
            s_anchorUs = now;
        }
        else if (now - s_anchorUs >= StuckUs)
        {
            pf::logInfo("bench: %.2f s stuck for %u s at (%.0f, %.0f, %.0f), resetting the car to the road", seconds(),
                        unsigned(StuckUs / 1000000u), position[0], position[1], position[2]);
            s_resetRequest = true;
            s_anchorUs = now;
        }
    }
    s_loaded += sectionsLoaded(false);

    const char* reason = nullptr;
    if (flow != GameFlowInGame)
        reason = "left the game world";
    else if (mode != 0)
        reason = "left the roaming mode";
    else if (now - s_raceUs >= uint32_t(s_seconds) * 1000000u)
        reason = "time limit";
    if (!reason)
        return;
    snprintf(s_extra, sizeof(s_extra), " | roam: driven %.2f km (%u jumps over %.0f m left out), sections loaded %u "
             "(%u resident at the start), targets %u: reached %u, without a path %u, path lost %u, path requests %u "
             "(timed out %u), stuck resets %u", s_driven / 1000.0, s_jumps, MaxStep, s_loaded, s_resident,
             s_targets - s_base[0], s_reached - s_base[1], s_noPath - s_base[2], s_dropped - s_base[3],
             s_requests - s_base[5], s_timeouts - s_base[6], s_resets - s_base[4]);
    s_endReason = reason;
    pf::logInfo("bench: %.2f s measurement ended: %s", seconds(), reason);
    s_phase.store(Stopping, std::memory_order_release);
}

// Roam benchmark settings: skip-front-end start without a race, the player's car under AI control, traffic and cops.
void configureRoam()
{
    const int traffic = std::min(std::max(pf::Config::getInt("bench_traffic", 100), 0), 100);
    const bool cops = pf::Config::getBool("bench_cops", true);
    s_seconds = pf::Config::getInt("bench_seconds", DefaultRoamSeconds);
    if (s_seconds <= 0)
        s_seconds = DefaultRoamSeconds;

    // The scratch block (path target, reset position, empty race ID) lives in guest memory for the whole run.
    win32::MemMap* scratch = new win32::MemMap(ScratchSize);
    s_scratch = scratch->getBlockStart();
    memset(win32::g_guestBase + s_scratch, 0, ScratchSize);
    setDword(SkipFeRaceId, s_scratch + ScratchRaceId);
    setDword(SkipFe, 1);
    win32::g_guestBase[SkipFeAiControl] = 1;
    win32::g_guestBase[SkipFeDisableTraffic] = traffic ? 0 : 1;
    const float density = float(traffic);
    memcpy(win32::g_guestBase + SkipFeTrafficDensity, &density, 4);
    win32::g_guestBase[SkipFeDisableCops] = cops ? 0 : 1;

    const char* car = reinterpret_cast<const char*>(win32::g_guestBase + dword(SkipFePlayerCar));
    pf::logInfo("bench: on, free roam, car %s driven by the AI to road nodes %.0f-%.0f m away, traffic %d%%, "
                "cops %s, seed 0x%08X, no profile, measure %d s from the car's first %.0f m, %s", car,
                TargetMinDistance, TargetMaxDistance, traffic, cops ? "on" : "off", unsigned(fixedSeed()), s_seconds,
                MovedToStart, s_exit ? "quit after the summary" : "keep running after the summary");

    s_frames.assign(std::min<uint32_t>(MaxFrames, uint32_t(s_seconds) * 60 + 600), 0);
    for (SeriesData& series : s_series)
        series.values.assign(s_frames.size(), 0);
    pf::perf::setFrameObserver(framePresented);
    pf::perf::setGpuObserver(gpuFrame);
}

}

void configure()
{
    s_enabled = pf::Config::getBool("bench", false);
    if (!s_enabled)
        return;
    const char* mode = pf::Config::getString("bench_mode", "race");
    s_roam = strcasecmp(mode, "roam") == 0;
    if (!s_roam && strcasecmp(mode, "race") != 0)
        pf::logError("bench: bench_mode=%s is neither race nor roam, race used", mode);
    s_exit = pf::Config::getBool("bench_exit", false);
    s_startUs = pf::perf::now();
    // No profile: career messages and progress of the owner's save stay out of the run, and nothing is saved.
    setDword(MemcardEnabled, 0);
    if (s_roam)
    {
        configureRoam();
        return;
    }
    const char* race = pf::Config::getString("bench_race", DefaultRace);
    const int laps = std::min(std::max(pf::Config::getInt("bench_laps", 2), 1), 10);
    const int opponents = std::min(std::max(pf::Config::getInt("bench_opponents", 7), 0), 7);
    s_seconds = std::max(pf::Config::getInt("bench_seconds", 0), 0);

    // The race ID string lives in guest memory for the whole run.
    const size_t length = strlen(race) + 1;
    win32::MemMap* raceId = new win32::MemMap(x86::reg32(length));
    memcpy(win32::g_guestBase + raceId->getBlockStart(), race, length);
    setDword(SkipFeRaceId, raceId->getBlockStart());
    setDword(SkipFe, 1);
    setDword(SkipFeAiCars, x86::reg32(opponents));
    setDword(SkipFeLaps, x86::reg32(laps));
    setDword(SkipFeRaceType, 0);
    win32::g_guestBase[SkipFeAiControl] = 1;

    const char* car = reinterpret_cast<const char*>(win32::g_guestBase + dword(SkipFePlayerCar));
    pf::logInfo("bench: on, quick race %s, %d laps (circuits), %d opponents, car %s driven by the AI, seed 0x%08X, "
                "no profile, measure %s, %s", race, laps, opponents, car, unsigned(fixedSeed()),
                s_seconds ? "up to the finish or the time limit" : "up to the finish",
                s_exit ? "quit after the summary" : "keep running after the summary");
    if (s_seconds)
        pf::logInfo("bench: time limit %d s", s_seconds);

    s_frames.assign(s_seconds ? std::min<uint32_t>(MaxFrames, uint32_t(s_seconds) * 60 + 600) : MaxFrames, 0);
    for (SeriesData& series : s_series)
        series.values.assign(s_frames.size(), 0);
    pf::perf::setFrameObserver(framePresented);
    pf::perf::setGpuObserver(gpuFrame);
}

void sample(Series series, uint32_t value)
{
    if (!s_enabled || s_phase.load(std::memory_order_acquire) != Recording)
        return;
    SeriesData& data = s_series[series];
    const uint32_t index = data.count.load(std::memory_order_relaxed);
    if (index >= data.values.size())
        return;
    data.values[index] = value;
    data.count.store(index + 1, std::memory_order_release);
}

void seedRandom()
{
    if (s_enabled)
        setDword(RandomSeed, fixedSeed());
}

void frame()
{
    if (!s_enabled)
        return;
    static x86::reg32 s_flow = ~0u;
    static bool s_quit = false;

    const x86::reg32 flow = dword(GameFlowState);
    if (flow != s_flow)
    {
        pf::logInfo("bench: %.2f s game flow %d (%s)", seconds(), int(flow),
                    flow < sizeof(s_flowNames) / sizeof(s_flowNames[0]) ? s_flowNames[flow] : "?");
        s_flow = flow;
    }
    if (s_roam)
        frameRoam(flow);
    else
        frameRace();
    answerPrompts();
    if (s_phase.load(std::memory_order_acquire) == Done && s_exit && !s_quit)
    {
        s_quit = true;
        pf::logInfo("bench: quitting");
        win32::Window::postQuit();
    }
}

void humanUpdate(win32::WinApplication* app, x86::CPU& cpu)
{
    if (!s_roam)
        return;
    static int s_state = -1;
    const x86::reg32 ai = cpu.ecx;
    const x86::reg32 nav = dword(ai + HumanDriveToNav);
    const bool control = byte(ai + HumanAiControl) != 0;
    const bool valid = nav && byte(nav + NavValid);
    const int state = (control ? 1 : 0) | (nav ? 2 : 0) | (valid ? 4 : 0);
    if (state != s_state)
    {
        pf::logInfo("bench: %.2f s AI update: AI control %d, drive-to nav %s, nav valid %d, navigation type %d",
                    seconds(), int(control), nav ? "set" : "none", int(valid), nav ? int(dword(nav + NavType)) : -1);
        s_state = state;
    }
    const uint32_t now = pf::perf::now();
    // A request that searches longer than PathTimeoutUs is cancelled whatever the nav's state: a pending request
    // keeps the AI from driving.
    if (nav && s_targetActive && now - s_lastRequestUs >= PathTimeoutUs && findingPath(nav))
    {
        callGuest(app, cpu, &sub_7fb060, nav, {});
        ++s_timeouts;
        s_targetActive = false;
        s_retryTarget = false;
        s_lastRequestUs = 0;
        if (targetLine())
            pf::logInfo("bench: %.2f s target %u: no path after %u s, request cancelled (still queued: %d)",
                        seconds(), s_targets, unsigned(PathTimeoutUs / 1000000u), int(findingPath(nav)));
    }
    if (!control || !valid)
        return;
    if (!s_aiSeen)
    {
        s_aiSeen = true;
        s_aiSeenUs = now;
        pf::logInfo("bench: %.2f s the AI drives the player car", seconds());
    }
    if (s_resetRequest)
    {
        s_resetRequest = false;
        resetCar(app, cpu, ai, nav);
        pf::logInfo("bench: %.2f s reset done", seconds());
    }
    if (findingPath(nav))
        return;

    // The path request of the current target has ended: with a path the AI follows it up to TargetReached from the
    // target. A path that ends within PathEndReached counts as reached; a path lost earlier (a reset of the nav) is
    // requested again up to MaxPathRetries times; without a path, or after that, a new target follows.
    float position[3];
    vector(nav + NavPosition, position);
    const bool following = dword(nav + NavType) == NavTypePath && int32_t(dword(nav + NavPathSegments)) > 0;
    if (s_targetActive)
    {
        const float away = flatDistance(position, s_target);
        if (following && away >= TargetReached)
        {
            if (!s_targetPathed && targetLine())
                pf::logInfo("bench: %.2f s target %u: path of %d road segments", seconds(), s_targets,
                            int(dword(nav + NavPathSegments)));
            s_targetPathed = true;
            return;
        }
        if (following || away < TargetReached || (s_targetPathed && away < PathEndReached))
        {
            ++s_reached;
            if (targetLine())
                pf::logInfo("bench: %.2f s target %u reached (%.0f m)", seconds(), s_targets, away);
        }
        else if (!s_targetPathed)
        {
            ++s_noPath;
            if (targetLine())
                pf::logInfo("bench: %.2f s target %u: no path", seconds(), s_targets);
        }
        else
        {
            ++s_dropped;
            s_retryTarget = s_pathRetries < MaxPathRetries;
            if (targetLine())
                pf::logInfo("bench: %.2f s target %u: path lost %.0f m before the target%s", seconds(), s_targets,
                            away, s_retryTarget ? ", requesting it again" : "");
        }
        s_targetActive = false;
    }
    // The first request waits until the game's AI has the car moving (at most FirstPathUs): a pending request keeps the
    // AI standing, and the car must drive from the start whatever happens with paths.
    if (!s_carMoved && now - s_aiSeenUs < FirstPathUs)
        return;
    if (s_lastRequestUs && now - s_lastRequestUs < TargetRetryUs)
        return;
    s_lastRequestUs = now;
    const bool retry = s_retryTarget;
    s_retryTarget = false;
    x86::reg32 node = 0;
    const bool first = s_targets == 0;
    if (retry)
        ++s_pathRetries;
    else if (pickTarget(nav, position, first ? FirstTargetMinDistance : TargetMinDistance,
                        first ? FirstTargetMaxDistance : TargetMaxDistance, &node))
        s_pathRetries = 0;
    else
        return;
    memcpy(win32::g_guestBase + s_scratch + ScratchTarget, s_target, sizeof(s_target));
    // Only a request again after a lost path searches forward only; other requests clear the flag the bench set.
    if (retry || s_forwardSet)
        rc::st8(nav + NavOnlyForward, retry ? 1 : 0);
    s_forwardSet = retry;
    const int typeBefore = int(dword(nav + NavType));
    const int segment = int16_t(rc::ld16(nav + NavSegment));
    const int forward = byte(nav + NavOnlyForward);
    const x86::CPU result = callGuest(app, cpu, &sub_7fb090, nav, { s_scratch + ScratchTarget, 0, 0 });
    s_targetActive = (result.eax & 0xFF) != 0;
    s_targetPathed = false;
    ++s_requests;
    if (targetLine())
        pf::logInfo("bench: %.2f s path request %u: taken %d, queued %d, path finder %s, navigation type %d, segment "
                    "%d, forward only %d, car moved %d", seconds(), s_requests, int(s_targetActive),
                    int(findingPath(nav)), dword(PathFinder) ? "present" : "none", typeBefore, segment, forward,
                    int(s_carMoved));
    if (retry)
    {
        if (targetLine())
            pf::logInfo("bench: %.2f s target %u: path request %d again, %.0f m away%s", seconds(), s_targets,
                        s_pathRetries, flatDistance(position, s_target), s_targetActive ? "" : ", refused");
        return;
    }
    ++s_targets;
    if (targetLine())
        pf::logInfo("bench: %.2f s target %u: road node %u of %u connected at (%.0f, %.0f, %.0f), %.0f m away%s",
                    seconds(), s_targets, unsigned(node), unsigned(s_reachable.size()), s_target[0], s_target[1],
                    s_target[2], flatDistance(position, s_target), s_targetActive ? "" : ", path request refused");
}

}}
