#include "input.h"

#include <lib/controls.h>
#include <lib/gamepad.h>
#include <lib/memmap.h>
#include <platform/log.h>
#include <cctype>
#include <cmath>
#include <cstring>
#include <iterator>
#include <string>

namespace nfsc { namespace input {

namespace
{

using win32::controls::Input;

// Controller config (0x679BF0): 72 scalars {name, entries}; entries {type, joystick, index}, keyboard 0-1, joystick j
// 2+2j and 3+2j. GameDevice::PollDevice (0x692240) reads each scalar as its largest entry value (0x67A340).
const x86::reg32 ConfigTable = 0x00A98700;
const x86::reg32 ConfigEntrySize = 12;
const x86::reg32 JoystickCount = 0x00A98948;    // joysticks the device manager found (0x679BA0)

// Entry types (0x67A340): a button, or an axis lX/lY/lZ or lRx/lRy/lRz (index 0-2) read below or above its centre.
enum EntryType : x86::reg32
{
    Unbound = 0, Button = 2, AxisBelow = 4, AxisAbove = 5, RotationBelow = 6, RotationAbove = 7
};

// Device scalars bound to the pad, by index in the controller config.
enum Scalar
{
    Gas, Brake, SteerLeft, SteerRight, Handbrake, Speedbreaker, Nitrous, ShiftDown, ShiftUp, Clutch,
    ShiftFirst, ShiftSecond, ShiftThird, ShiftFourth, ShiftFifth, ShiftSixth, ShiftReverse, ShiftNeutral,
    ResetCar, CrewAbility, EngageEvent, WorldMap, Messages, FeButton2, CameraChange, LookBack, CameraPullBack,
    Screenshot, Pause, FeLeft, FeRight, FeUp, FeDown, FeRightStickLeft, FeRightStickRight, FeRightStickUp,
    FeRightStickDown, FeAccept, FeCancel, FeStart, FeLeftTrigger, FeRightTrigger, FeButton0, FeButton1, FeButton3,
    FeButton4, FeButton5, SkipNis, NextSong,
    BoundScalars
};

// An entry before resolution: a pad input (button), the Select tap, or an axis index.
const int SelectTap = -1;
struct Entry
{
    EntryType type;
    int       source;
};

struct Binding
{
    Entry primary;
    Entry alternate;
};

const Entry None = { Unbound, 0 };
constexpr Entry pad(Input input) { return Entry{ Button, int(input) }; }

// Axis and rotation indices of the sticks: left X/Y are lX/lY, right X/Y are lRx/lRz.
const int StickX = 0, StickY = 1, RightX = 0, RightY = 2;

// The layout, by scalar. Driving: R gas, L brake, Cross nitrous, Square handbrake, Triangle camera, Circle look back.
const Binding s_layout[] =
{
    { pad(Input::R),         None },                            // Gas
    { pad(Input::L),         None },                            // Brake
    { { AxisBelow, StickX }, None },                            // SteerLeft
    { { AxisAbove, StickX }, None },                            // SteerRight
    { pad(Input::Square),    None },                            // Handbrake
    { pad(Input::DpadDown),  pad(Input::RearRight) },           // Speedbreaker
    { pad(Input::Cross),     None },                            // Nitrous
    { { RotationAbove, RightY }, None },                        // ShiftDown
    { { RotationBelow, RightY }, None },                        // ShiftUp
    { None, None },                                             // Clutch
    { None, None }, { None, None }, { None, None },             // ShiftFirst .. ShiftThird
    { None, None }, { None, None }, { None, None },             // ShiftFourth .. ShiftSixth
    { None, None }, { None, None },                             // ShiftReverse, ShiftNeutral
    { { Button, SelectTap }, None },                            // ResetCar
    { pad(Input::RearLeft),  None },                            // CrewAbility
    { pad(Input::DpadUp),    None },                            // EngageEvent
    { pad(Input::DpadLeft),  None },                            // WorldMap
    { pad(Input::DpadRight), None },                            // Messages
    { { Button, SelectTap }, None },                            // FeButton2
    { pad(Input::Triangle),  None },                            // CameraChange
    { pad(Input::Circle),    None },                            // LookBack
    { None, None },                                             // CameraPullBack
    { None, None },                                             // Screenshot
    { pad(Input::Start),     None },                            // Pause
    { pad(Input::DpadLeft),  { AxisBelow, StickX } },           // FeLeft
    { pad(Input::DpadRight), { AxisAbove, StickX } },           // FeRight
    { pad(Input::DpadUp),    { AxisBelow, StickY } },           // FeUp
    { pad(Input::DpadDown),  { AxisAbove, StickY } },           // FeDown
    { { RotationBelow, RightX }, None },                        // FeRightStickLeft
    { { RotationAbove, RightX }, None },                        // FeRightStickRight
    { { RotationBelow, RightY }, None },                        // FeRightStickUp
    { { RotationAbove, RightY }, None },                        // FeRightStickDown
    { pad(Input::Cross),     None },                            // FeAccept
    { pad(Input::Circle),    None },                            // FeCancel
    { pad(Input::Start),     None },                            // FeStart
    { pad(Input::L),         None },                            // FeLeftTrigger
    { pad(Input::R),         None },                            // FeRightTrigger
    { pad(Input::RearRight), None },                            // FeButton0
    { pad(Input::RearLeft),  None },                            // FeButton1
    { None, None },                                             // FeButton3
    { pad(Input::Square),    None },                            // FeButton4
    { pad(Input::Triangle),  None },                            // FeButton5
    { pad(Input::Cross),     None },                            // SkipNis
    { pad(Input::FrontTop),  None },                            // NextSong
};
static_assert(std::size(s_layout) == BoundScalars, "s_layout must list every bound scalar");

// Resolved joystick 0 entries {type, joystick, index}, primary then alternate, by scalar.
x86::reg32 s_entries[BoundScalars][6];

// ActionQueues (vector 0xA99248): enabled [+0x26C] > 0 (0x678960), port [+0x268], name [+0x284] from SetConfig
// (0x696AC0). The player's driving queue is named "InputPlayer" (0x9C3278, 0x428823).
const x86::reg32 QueueData = 0x00A9924C;
const x86::reg32 QueueCount = 0x00A99254;
const x86::reg32 QueueEnabled = 0x26C;
const x86::reg32 QueuePort = 0x268;
const x86::reg32 QueueName = 0x284;
const x86::reg32 PlayerQueueName = 0x009C3278;
const x86::reg32 MaxQueues = 64;

// FEKeyboardInput (0xA604B8): UTF-16 text +4, text box [+0x27C] while capturing, frames since [+0x280]; WM_CHAR
// (0x7121FE) reaches NotifyChar (0x5C5260) after 0x1D frames. Text box (0x5BAD50): mode +4, length limit +0x10.
const x86::reg32 KeyboardInput = 0x00A604B8;
const x86::reg32 InputText = KeyboardInput + 4;
const x86::reg32 InputTextBox = KeyboardInput + 0x27C;
const x86::reg32 InputFrames = KeyboardInput + 0x280;
const x86::reg32 InputFramesReady = 0x1D;
const x86::reg32 TextCapacity = 0x9C;
const x86::reg32 TextBoxMode = 0x04;
const x86::reg32 TextBoxMaxLength = 0x10;
// Symbols the general modes 0 and 1 take besides letters and digits (0x570340): ints from 0xA60740 to 0xA607D0.
const x86::reg32 SymbolTable = 0x00A60740;
const x86::reg32 SymbolTableEnd = 0x00A607D0;

// A stick counts as released below this deflection.
const float StickRest = 0.25f;

x86::reg32 dword(x86::reg32 address)
{
    x86::reg32 value;
    memcpy(&value, win32::g_guestBase + address, 4);
    return value;
}

x86::reg32 resolveIndex(const Entry& entry)
{
    if (entry.type != Button)
        return x86::reg32(entry.source);
    if (entry.source == SelectTap)
        return x86::reg32(win32::controls::selectTapButton());
    return x86::reg32(__builtin_ctz(win32::controls::buttonMask(Input(entry.source))));
}

void resolveLayout()
{
    for (int scalar = 0; scalar < BoundScalars; ++scalar)
    {
        const Entry entries[2] = { s_layout[scalar].primary, s_layout[scalar].alternate };
        for (int i = 0; i < 2; ++i)
        {
            s_entries[scalar][3 * i + 0] = entries[i].type;
            s_entries[scalar][3 * i + 1] = 0;
            s_entries[scalar][3 * i + 2] = entries[i].type == Unbound ? 0 : resolveIndex(entries[i]);
        }
    }
}

// Writes the layout into joystick 0's entries; other scalars and the keyboard entries keep the game's bindings.
void bindPad()
{
    static bool s_logged = false;
    const x86::reg32 joysticks = dword(JoystickCount);
    if (!s_logged)
    {
        pf::logInfo("input: %u joystick(s) in the controller config", unsigned(joysticks));
        s_logged = true;
    }
    if (!joysticks)
        return;
    for (int scalar = 0; scalar < BoundScalars; ++scalar)
    {
        const x86::reg32 entries = dword(ConfigTable + 8 * x86::reg32(scalar) + 4);
        if (entries)
            memcpy(win32::g_guestBase + entries + 2 * ConfigEntrySize, s_entries[scalar], 2 * ConfigEntrySize);
    }
}

// The player's driving queue is enabled.
bool driving()
{
    const x86::reg32 data = dword(QueueData);
    const x86::reg32 count = dword(QueueCount);
    if (!data || count > MaxQueues)
        return false;
    for (x86::reg32 i = 0; i < count; ++i)
    {
        const x86::reg32 queue = dword(data + 4 * i);
        if (queue && x86::sreg32(dword(queue + QueueEnabled)) > 0 && x86::sreg32(dword(queue + QueuePort)) >= 0
            && dword(queue + QueueName) == PlayerQueueName)
            return true;
    }
    return false;
}

// Inputs held when the controls change between the front end and driving stay hidden until released.
void hideHeld(x86::reg32* buttons, int* pov, float (&axes)[win32::controls::AxisCount])
{
    using namespace win32::controls;
    static int s_driving = -1;
    static x86::reg32 s_buttons = 0;
    static bool s_hat = false;
    static bool s_sticks[2] = {};
    const float left = std::hypot(axes[AxisX], axes[AxisY]);
    const float right = std::hypot(axes[AxisRx], axes[AxisRz]);
    const int now = driving() ? 1 : 0;
    if (now != s_driving)
    {
        if (s_driving >= 0)
        {
            s_buttons = *buttons;
            s_hat = *pov >= 0;
            s_sticks[0] = left >= StickRest;
            s_sticks[1] = right >= StickRest;
        }
        s_driving = now;
        pf::logInfo("input: %s controls", now ? "driving" : "front-end");
    }
    s_buttons &= *buttons;
    s_hat = s_hat && *pov >= 0;
    s_sticks[0] = s_sticks[0] && left >= StickRest;
    s_sticks[1] = s_sticks[1] && right >= StickRest;
    *buttons &= ~s_buttons;
    if (s_hat)
        *pov = -1;
    if (s_sticks[0])
        axes[AxisX] = axes[AxisY] = 0.0f;
    if (s_sticks[1])
        axes[AxisRx] = axes[AxisRz] = 0.0f;
}

// Characters a text box takes besides letters and digits, by its mode.
std::string extraCharacters(x86::reg32 mode)
{
    if (mode == 4)
        return "@._";
    if (mode == 2)
        return " ,@.";
    std::string extra;
    if (mode == 0 || mode == 1)
    {
        for (x86::reg32 entry = SymbolTable; entry < SymbolTableEnd; entry += 4)
        {
            const x86::reg32 c = dword(entry);
            if (c >= 0x20 && c < 0x7F && !std::isalnum(int(c)))
                extra += char(c);
        }
    }
    return extra;
}

// Hands the captured field to the on-screen keyboard once it takes characters; MenuScreen::BaseNotify (0x59A100)
// accepts it on PAD_ACCEPT (Enter) and cancels it on PAD_BACK (Escape).
void pollTextField()
{
    static bool s_editing = false;
    const x86::reg32 box = dword(InputTextBox);
    if (!box || dword(InputFrames) <= InputFramesReady)
    {
        if (s_editing)
            win32::Gamepad::endTextEdit();
        s_editing = false;
        return;
    }
    char text[TextCapacity + 1];
    x86::reg32 length = 0;
    for (x86::reg32 i = 0; i < TextCapacity; ++i)
    {
        x86::reg16 c;
        memcpy(&c, win32::g_guestBase + InputText + 2 * i, 2);
        if (!c)
            break;
        if (c >= 0x20 && c < 0x7F)
            text[length++] = char(c);
    }
    text[length] = 0;
    const x86::reg32 limit = dword(box + TextBoxMaxLength);
    const x86::reg32 maxLength = limit < 1 ? 1 : limit > TextCapacity - 1 ? TextCapacity - 1 : limit;
    win32::Gamepad::editText("Text", text, maxLength, extraCharacters(dword(box + TextBoxMode)).c_str(), true);
    s_editing = true;
}

// Runs in each pad read: GetDeviceState from the joystick poll 0x8929BF, before PollDevice reads the config.
void padPollHook(x86::reg32* buttons, int* pov, float (&axes)[win32::controls::AxisCount])
{
    pollTextField();
    bindPad();
    hideHeld(buttons, pov, axes);
}

}

void install()
{
    resolveLayout();
    win32::Gamepad::setPollHook(padPollHook);
}

}}
