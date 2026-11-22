// Car environment map: the cube faces the game draws each frame. envmap_faces=1 draws one face a frame in turn at
// every update rate; all six are drawn on the first race frame after the cube is created and after frames without it.

#include <lib/winapp.h>
#include <native.h>
#include <platform/config.h>
#include <recomp.h>

namespace nfsc
{

// Translated bodies of the overridden guest functions.
void sub_70de00_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_7218a0_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace
{

const x86::reg32 FrameCount = 0x00AB0A78;      // frames drawn, counted at the end of each frame
const x86::reg32 GameState = 0x00A99BBC;       // 6 in a race: the only state whose frames draw the cube faces
const x86::reg32 RaceState = 6;
const x86::reg32 ViewActive = 0x0C;            // eView: drawn this frame (byte)
const uint32_t Faces = 6;

int s_faces = -1;               // envmap_faces: 1 one face a frame, 0 the game's schedule
bool s_created = true;          // the cube was created since the last race frame
x86::reg32 s_lastFrame = 0;     // frame count of the last race frame

}

namespace native
{

// 0x70de00: creates the car environment cube and its depth surfaces.
void sub_70de00(win32::WinApplication* app, x86::CPU& cpu)
{
    sub_70de00_guest(app, cpu);
    s_created = true;
}

// 0x7218a0 (thiscall: ecx the six face views, two position arguments): sets each face view's matrices and whether
// it is drawn this frame.
void sub_7218a0(win32::WinApplication* app, x86::CPU& cpu)
{
    const x86::reg32 views = cpu.ecx;
    sub_7218a0_guest(app, cpu);
    if (cpu.terminate || rc::ld32(GameState) != RaceState)
        return;
    if (s_faces < 0)
        s_faces = pf::Config::getInt("envmap_faces", 1) == 0 ? 0 : 1;
    const x86::reg32 frame = rc::ld32(FrameCount);
    const bool refresh = s_created || frame != s_lastFrame + 1;
    s_created = false;
    s_lastFrame = frame;
    if (!refresh && !s_faces)
        return;
    for (uint32_t face = 0; face < Faces; ++face)
    {
        const x86::reg32 view = rc::ld32(views + 4 * face);
        if (view)
            rc::st8(view + ViewActive, refresh || face == frame % Faces ? 1 : 0);
    }
}

}
}
