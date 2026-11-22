// Button prompts: key textures get Vita button glyphs (assets/prompts/) when created, PC input wording is looked
// up as the game's pad wording, and the main menu's keyboard-only quit hint is hidden.

#include "display.h"
#include "framephases.h"
#include "worldtex.h"

#include <lib/guest.h>
#include <lib/winapp.h>
#include <native.h>
#include <platform/log.h>
#include <platform/perf.h>
#include <recomp.h>

#include <cstddef>
#include <cstring>

extern "C" {
#define NFSC_GLYPH(name) extern const unsigned char prompt_##name[]; extern const size_t prompt_##name##_size;
NFSC_GLYPH(cross_argb)
NFSC_GLYPH(circle_argb)
NFSC_GLYPH(square_argb)
NFSC_GLYPH(triangle_argb)
NFSC_GLYPH(l_argb)
NFSC_GLYPH(r_argb)
NFSC_GLYPH(start_argb)
NFSC_GLYPH(select_argb)
NFSC_GLYPH(rear_left_argb)
NFSC_GLYPH(rear_right_argb)
NFSC_GLYPH(l_r_argb)
NFSC_GLYPH(l_dxt5)
NFSC_GLYPH(r_dxt5)
NFSC_GLYPH(dpad_left_right_dxt5)
NFSC_GLYPH(dpad_up_down_dxt5)
NFSC_GLYPH(rstick_dxt5)
#undef NFSC_GLYPH
}

namespace nfsc
{

// Translated bodies of the overridden guest functions.
void sub_725c80_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_5786b0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_8583a0_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace
{

// TextureInfo: name hash +0x0C, image size +0x1C, width/height +0x28/+0x2A (int16), mip levels +0x32 (int8),
// image data +0x50, name +0x59; [+0] is the platform info with the D3DFORMAT at +0x0C.
const x86::reg32 TexNameHash = 0x0C;
const x86::reg32 TexImageSize = 0x1C;
const x86::reg32 TexWidth = 0x28;
const x86::reg32 TexHeight = 0x2A;
const x86::reg32 TexMipLevels = 0x32;
const x86::reg32 TexImageData = 0x50;
const x86::reg32 TexName = 0x59;
const x86::reg32 PlatFormat = 0x0C;

const x86::reg32 FormatArgb = 21;            // D3DFMT_A8R8G8B8
const x86::reg32 FormatDxt5 = 0x35545844;    // 'DXT5'

struct Glyph
{
    const unsigned char* data;
    const size_t*        size;
};

#define GLYPH(name) Glyph{ prompt_##name, &prompt_##name##_size }

// A key texture of the pack GLOBALTEXTURESPC (GLOBAL/GLOBALB.BUN) and the glyph of the Vita button that does the
// same in this port's layout (the key's front-end action in the game's default keyboard bindings, 0xA61C88).
struct Substitution
{
    const char* texture;
    int         width, height, mips;
    x86::reg32  format;
    Glyph       glyph;
};

const Substitution s_textures[] =
{
    { "PC_KEY_ENTER",      32, 32, 1, FormatArgb, GLYPH(cross_argb) },       // accept
    { "PC_KEY_ESC",        32, 32, 1, FormatArgb, GLYPH(circle_argb) },      // back
    { "PC_KEY_ESC_FRENCH", 32, 32, 1, FormatArgb, GLYPH(circle_argb) },
    { "PC_KEY_SPC",        32, 32, 1, FormatArgb, GLYPH(start_argb) },       // start
    { "PC_KEY_SPC_SPANISH", 32, 32, 1, FormatArgb, GLYPH(start_argb) },
    { "PC_KEY_SPC_ITALIAN", 32, 32, 1, FormatArgb, GLYPH(start_argb) },
    { "PC_KEY_SPC_DANISH", 32, 32, 1, FormatArgb, GLYPH(start_argb) },
    { "PC_KEY_SPC_FRENCH", 32, 32, 1, FormatArgb, GLYPH(start_argb) },
    { "PC_KEY_SPC_GERMAN", 32, 32, 1, FormatArgb, GLYPH(start_argb) },
    { "PC_KEY_SPC_CZECH",  32, 32, 1, FormatArgb, GLYPH(start_argb) },
    { "PC_KEY_SPC_DUTCH",  32, 32, 1, FormatArgb, GLYPH(start_argb) },
    { "PC_KEY_1",          32, 32, 1, FormatArgb, GLYPH(triangle_argb) },    // screen option 5
    { "PC_KEY_2",          32, 32, 1, FormatArgb, GLYPH(square_argb) },      // screen option 4
    { "PC_KEY_3",          32, 32, 1, FormatArgb, GLYPH(rear_left_argb) },   // screen option 1
    { "PC_KEY_M",          32, 32, 1, FormatArgb, GLYPH(rear_right_argb) },  // screen option 0
    { "PC_KEY_M_FRENCH",   32, 32, 1, FormatArgb, GLYPH(rear_right_argb) },
    { "PC_KEY_T",          32, 32, 1, FormatArgb, GLYPH(select_argb) },      // screen option 2
    { "PC_KEY_9",          32, 32, 1, FormatArgb, GLYPH(l_argb) },           // left trigger
    { "PC_KEY_0",          32, 32, 1, FormatArgb, GLYPH(r_argb) },           // right trigger
    { "PC_GREATERTHAN_LESSTHAN_KEYS", 64, 32, 1, FormatArgb, GLYPH(l_r_argb) },
    { "L1",                32, 32, 3, FormatDxt5, GLYPH(l_dxt5) },           // trigger page and toggle arrows
    { "R1",                32, 32, 3, FormatDxt5, GLYPH(r_dxt5) },
    { "PC_ARROW_KEYS",     64, 32, 3, FormatDxt5, GLYPH(dpad_left_right_dxt5) },
    { "PC_ARROW_KEYS_UP_DOWN", 64, 32, 3, FormatDxt5, GLYPH(dpad_up_down_dxt5) },
    { "PC_CLICKDRAG_ICON", 32, 32, 1, FormatDxt5, GLYPH(rstick_dxt5) },      // rotate the camera
};

#undef GLYPH

// Label lookups (0x5786b0 maps a label hash before every localized string lookup): PC wording -> the game's pad
// wording, present in every language file.
struct Redirect
{
    x86::reg32 from, to;
};

const Redirect s_labels[] =
{
    { 0x8E7ACDFE, 0x703A92CC },    // title screen: MENU_SPLASH_START_PROMPT_PC -> MENU_SPLASH_START_PROMPT
};

// FeMainMenu: package name [this+0x0C], quit hint group (keyboard Q, 0x711fba); 0x5a0250 (cdecl: package name,
// object hash) finds an object; FEObject flags +0x1C: bit 0 hidden, 0x2400000 changed (as 0x570460 sets them).
const x86::reg32 ScreenPackage = 0x0C;
const x86::reg32 MainMenuQuitHint = 0x70674A7C;
const x86::reg32 FindObject = 0x005A0250;
const x86::reg32 ObjectFlags = 0x1C;
const x86::reg32 ObjectHidden = 0x2400001;

// The game's name hash (0x471050): h = h * 33 + c from 0xFFFFFFFF.
x86::reg32 nameHash(const char* name)
{
    x86::reg32 h = 0xFFFFFFFF;
    for (; *name; ++name)
        h = h * 33 + x86::reg8(*name);
    return h;
}

const Substitution* findTexture(x86::reg32 info)
{
    const x86::reg32 hash = rc::ld32(info + TexNameHash);
    const char* name = reinterpret_cast<const char*>(native::host(info + TexName));
    for (const Substitution& s : s_textures)
        if (nameHash(s.texture) == hash && strncmp(name, s.texture, 35) == 0)
            return &s;
    return nullptr;
}

// Writes the glyph over the texture's image data when the texture is the expected one.
void substitute(x86::reg32 info)
{
    const Substitution* s = findTexture(info);
    if (!s)
        return;
    const x86::reg32 data = rc::ld32(info + TexImageData);
    const x86::reg32 format = rc::ld32(rc::ld32(info) + PlatFormat);
    const int width = int16_t(rc::ld16(info + TexWidth));
    const int height = int16_t(rc::ld16(info + TexHeight));
    const int mips = int8_t(rc::ld8(info + TexMipLevels));
    const x86::reg32 size = rc::ld32(info + TexImageSize);
    if (!data || format != s->format || width != s->width || height != s->height || mips != s->mips ||
        size != *s->glyph.size)
    {
        pf::logError("prompts: %s is %dx%d, %d mip(s), format 0x%x, %u bytes; not replaced", s->texture, width,
                     height, mips, unsigned(format), unsigned(size));
        return;
    }
    memcpy(native::host(data), s->glyph.data, size);
    pf::logInfo("prompts: %s replaced by its button glyph", s->texture);
}

x86::reg32 redirectLabel(x86::reg32 hash)
{
    for (const Redirect& r : s_labels)
        if (r.from == hash)
            return r.to;
    return hash;
}

// Hides an object of a package, on a copy of the caller's state.
void hideObject(win32::WinApplication* app, x86::CPU& cpu, x86::reg32 package, x86::reg32 object)
{
    x86::CPU copy = cpu;
    copy.esp -= 8;
    rc::st32(copy.esp, package);
    rc::st32(copy.esp + 4, object);
    native::callGuest(app, copy, FindObject);
    if (copy.terminate)
        cpu.terminate = true;
    else if (copy.eax)
        rc::st32(copy.eax + ObjectFlags, rc::ld32(copy.eax + ObjectFlags) | ObjectHidden);
}

}

namespace native
{

// 0x725c80: creates the Direct3D texture of a TextureInfo (ecx) and copies its image data into it.
void sub_725c80(win32::WinApplication* app, x86::CPU& cpu)
{
    substitute(cpu.ecx);
    const worldtex::CreationSkip skip(cpu.ecx);
    if (!pf::perf::enabled() || display::onDisplayThread(cpu))
    {
        sub_725c80_guest(app, cpu);
        return;
    }
    const uint32_t start = pf::perf::now();
    sub_725c80_guest(app, cpu);
    phases::textureCreation(pf::perf::now() - start);
}

// 0x5786b0: maps a label hash (cdecl argument) to the hash the string tables are searched for.
void sub_5786b0(win32::WinApplication* app, x86::CPU& cpu)
{
    sub_5786b0_guest(app, cpu);
    cpu.eax = redirectLabel(cpu.eax);
}

// 0x8583a0: FeMainMenu constructor (thiscall, returns this).
void sub_8583a0(win32::WinApplication* app, x86::CPU& cpu)
{
    sub_8583a0_guest(app, cpu);
    if (cpu.terminate || !cpu.eax)
        return;
    const x86::reg32 package = rc::ld32(cpu.eax + ScreenPackage);
    if (package)
        hideObject(app, cpu, package, MainMenuQuitHint);
}

}
}
