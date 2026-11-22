#include <lib/controls.h>
#include <platform/config.h>
#include <psp2/ctrl.h>
#include <psp2/touch.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <iterator>

namespace win32 { namespace controls
{

// The Vita pad: a gamepad with the EMS USB2 adapter's product GUID (VID 0B43, PID 0003).
const char* const JoystickName       = "Vita Gamepad";
const x86::reg32  JoystickDeviceType = 0x0204;      // DIDEVTYPE_JOYSTICK, DIDEVTYPEJOYSTICK_GAMEPAD
const GUID        JoystickProduct    = { 0x00030B43, 0x0000, 0x0000, { 0, 0, 'P', 'I', 'D', 'V', 'I', 'D' } };

// ---- Layout ----

// Joystick buttons by DirectInput index (Triangle, Circle, Cross, Square first). Select is a tap (button 8): see the
// Mapper.
struct ButtonMapping
{
    Input input;
    int   button;
};

static const ButtonMapping s_buttons[] =
{
    { Triangle,  0 },
    { Circle,    1 },
    { Cross,     2 },
    { Square,    3 },
    { RearLeft,  4 },
    { RearRight, 5 },
    { L,         6 },
    { R,         7 },
    { Start,     9 },
    { FrontTop,  10 },
    { DpadUp,    12 },
    { DpadRight, 13 },
    { DpadDown,  14 },
    { DpadLeft,  15 },
};

// Button that a short press of Select (no chord) reports.
static const int s_selectTapButton = 8;
static const int s_buttonCount = 16;

// Object names by button index and by axis slot (absent axes have none).
static const char* const s_buttonNames[s_buttonCount] =
{
    "Triangle", "Circle", "Cross", "Square", "Rear touch left", "Rear touch right", "L", "R",
    "Select", "Start", "Front touch top", "Button 12", "D-pad up", "D-pad right", "D-pad down", "D-pad left",
};
static const char* const s_axisNames[AxisCount] =
{
    "Left stick X", "Left stick Y", "", "Right stick X", "", "Right stick Y", "", "",
};

// Keys held while an input is held: the d-pad moves the cursor of a text field (WM_KEYDOWN Left / Right).
struct KeyMapping
{
    Input    input;
    uint32_t keys;
};

static constexpr uint32_t keys(Key key) { return uint32_t(1) << key; }

static const KeyMapping s_keys[] =
{
    { DpadLeft,  keys(KeyLeft)   },
    { DpadRight, keys(KeyRight)  },
};

// Select + input. An input pressed while Select is held uses this table until it is released.
struct Chord
{
    Input    input;
    uint32_t keys;
    int      button;    // -1 for none
    bool     ime;       // opens the on-screen keyboard
};

static const Chord s_chords[] =
{
    { Triangle,  0, -1, true  },    // text entry
};

// How long a Select tap holds its button, enough for one game frame at 10 fps.
static const uint64_t s_tapUs = 100000;

// Left and right stick: dead zone and saturation as fractions of full deflection; the response between them is
// linear.
static const float s_stickDeadZone   = 0.08f;
static const float s_stickSaturation = 0.94f;

// Front touch strip height and the panel sizes (front 1920x1088, rear 1920x890 starting at y 108).
static const int s_frontStripHeight = 136;
static const int s_panelCentreX     = 960;

// ---- Keys ----

// Order of enum Key.
static const KeyCode s_keyCodes[] =
{
    { 0x26, 0x48, true  },  // up
    { 0x28, 0x50, true  },  // down
    { 0x25, 0x4B, true  },  // left
    { 0x27, 0x4D, true  },  // right
    { 0x1B, 0x01, false },  // escape
    { 0x0D, 0x1C, false },  // enter
    { 0x10, 0x2A, false },  // left shift
    { 0x2E, 0x53, true  },  // delete
    { 0x70, 0x3B, false },  // F1
    { 0x71, 0x3C, false },  // F2
    { 0x72, 0x3D, false },  // F3
};
static_assert(std::size(s_keyCodes) == KeyCount, "s_keyCodes must list every Key");
static_assert(KeyCount <= 32, "keys are a 32-bit set");
static_assert(InputCount <= 32, "inputs are a 32-bit set");

// Set-1 scancodes of A..Z and 0..9.
static const x86::reg8 s_letterScancodes[26] =
{
    0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
    0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C,
};
static const x86::reg8 s_digitScancodes[10] = { 0x0B, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A };

const KeyCode& keyCode(Key key)
{
    return s_keyCodes[key];
}

const KeyCode& shiftKey()
{
    return s_keyCodes[KeyLeftShift];
}

bool characterKey(char c, KeyCode* key, bool* shift)
{
    *shift = c >= 'A' && c <= 'Z';
    if (c >= 'a' && c <= 'z')
        *key = KeyCode{ x86::reg32('A' + (c - 'a')), s_letterScancodes[c - 'a'], false };
    else if (*shift)
        *key = KeyCode{ x86::reg32(c), s_letterScancodes[c - 'A'], false };
    else if (c >= '0' && c <= '9')
        *key = KeyCode{ x86::reg32(c), s_digitScancodes[c - '0'], false };
    else if (c == ' ')
        *key = KeyCode{ 0x20, 0x39, false };
    else
        return false;
    return true;
}

// ---- Joystick ----

uint32_t buttonMask(Input input)
{
    uint32_t mask = 0;
    for (const ButtonMapping& mapping : s_buttons)
    {
        if (mapping.input == input)
            mask |= uint32_t(1) << mapping.button;
    }
    return mask;
}

int selectTapButton()
{
    return s_selectTapButton;
}

// Four axes: X, Y (left stick), Rx and Rz (right stick).
bool axisPresent(Axis axis)
{
    return axis == AxisX || axis == AxisY || axis == AxisRx || axis == AxisRz;
}

x86::reg32 presentAxisCount()
{
    return 4;
}

x86::reg32 joystickButtonCount()
{
    return s_buttonCount;
}

const char* buttonName(x86::reg32 button)
{
    return button < x86::reg32(s_buttonCount) ? s_buttonNames[button] : "";
}

const char* axisName(Axis axis)
{
    return s_axisNames[axis];
}

// ---- Pad ----

struct PadButton
{
    uint32_t mask;
    Input    input;
};

static const PadButton s_padButtons[] =
{
    { SCE_CTRL_CROSS,    Cross     },
    { SCE_CTRL_CIRCLE,   Circle    },
    { SCE_CTRL_SQUARE,   Square    },
    { SCE_CTRL_TRIANGLE, Triangle  },
    { SCE_CTRL_LTRIGGER, L         },
    { SCE_CTRL_RTRIGGER, R         },
    { SCE_CTRL_SELECT,   Select    },
    { SCE_CTRL_START,    Start     },
    { SCE_CTRL_UP,       DpadUp    },
    { SCE_CTRL_DOWN,     DpadDown  },
    { SCE_CTRL_LEFT,     DpadLeft  },
    { SCE_CTRL_RIGHT,    DpadRight },
};

static bool s_touchReady = false;
static bool s_rearTouch = true;
static bool s_frontTouch = true;

// Settings rear_touch / front_touch (both on by default) turn the touch inputs off.
static void initTouch()
{
    s_touchReady = true;
    s_rearTouch = pf::Config::getBool("rear_touch", true);
    s_frontTouch = pf::Config::getBool("front_touch", true);
    if (s_frontTouch)
        sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    if (s_rearTouch)
        sceTouchSetSamplingState(SCE_TOUCH_PORT_BACK, SCE_TOUCH_SAMPLING_STATE_START);
}

static float stickValue(unsigned char raw)
{
    return (float(raw) - 127.5f) / 127.5f;
}

static std::atomic<uint32_t> s_held{0};

void setHeld(uint32_t inputs)
{
    s_held.store(inputs, std::memory_order_relaxed);
}

PadSample readPad()
{
    if (!s_touchReady)
        initTouch();
    SceCtrlData pad;
    memset(&pad, 0, sizeof(pad));
    sceCtrlPeekBufferPositive(0, &pad, 1);

    PadSample sample;
    sample.inputs = s_held.load(std::memory_order_relaxed);
    for (const PadButton& button : s_padButtons)
    {
        if (pad.buttons & button.mask)
            sample.inputs |= bit(button.input);
    }
    sample.sticks[0] = stickValue(pad.lx);
    sample.sticks[1] = stickValue(pad.ly);
    sample.sticks[2] = stickValue(pad.rx);
    sample.sticks[3] = stickValue(pad.ry);

    SceTouchData touch;
    if (s_rearTouch && sceTouchPeek(SCE_TOUCH_PORT_BACK, &touch, 1) >= 0)
    {
        for (SceUInt32 i = 0; i < touch.reportNum && i < SCE_TOUCH_MAX_REPORT; ++i)
            sample.inputs |= bit(touch.report[i].x < s_panelCentreX ? RearLeft : RearRight);
    }
    if (s_frontTouch && sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1) >= 0)
    {
        for (SceUInt32 i = 0; i < touch.reportNum && i < SCE_TOUCH_MAX_REPORT; ++i)
        {
            if (touch.report[i].y < s_frontStripHeight)
                sample.inputs |= bit(FrontTop);
        }
    }
    return sample;
}

// ---- Mapper ----

static const Chord* findChord(Input input)
{
    for (const Chord& chord : s_chords)
    {
        if (chord.input == input)
            return &chord;
    }
    return nullptr;
}

Mapper::Mapper()
    :   m_previous(0)
    ,   m_chorded(0)
    ,   m_selectUsed(false)
    ,   m_selectPulseEndUs(0)
{
}

State Mapper::update(const PadSample& pad, uint64_t nowUs)
{
    State state;
    memset(&state, 0, sizeof(state));
    const uint32_t inputs = pad.inputs;
    const uint32_t pressed = inputs & ~m_previous;
    const uint32_t released = m_previous & ~inputs;
    const bool select = (inputs & bit(Select)) != 0;

    if (pressed & bit(Select))
        m_selectUsed = false;
    m_chorded &= inputs;
    for (int input = 0; input < InputCount; ++input)
    {
        if (!(pressed & bit(Input(input))) || input == Select || !select)
            continue;
        const Chord* chord = findChord(Input(input));
        if (!chord)
            continue;
        m_chorded |= bit(Input(input));
        m_selectUsed = true;
        if (chord->ime)
            state.openIme = true;
    }
    if ((released & bit(Select)) && !m_selectUsed)
        m_selectPulseEndUs = nowUs + s_tapUs;
    m_previous = inputs;

    const uint32_t normal = inputs & ~m_chorded;
    for (const ButtonMapping& mapping : s_buttons)
    {
        if (normal & bit(mapping.input))
            state.buttons |= uint32_t(1) << mapping.button;
    }
    for (const KeyMapping& mapping : s_keys)
    {
        if (normal & bit(mapping.input))
            state.keys |= mapping.keys;
    }
    for (const Chord& chord : s_chords)
    {
        if (!(m_chorded & bit(chord.input)))
            continue;
        state.keys |= chord.keys;
        if (chord.button >= 0)
            state.buttons |= uint32_t(1) << chord.button;
    }
    if (nowUs < m_selectPulseEndUs)
        state.buttons |= uint32_t(1) << s_selectTapButton;

    state.axes[AxisX] = pad.sticks[0];
    state.axes[AxisY] = pad.sticks[1];
    state.axes[AxisRx] = pad.sticks[2];
    state.axes[AxisRz] = pad.sticks[3];

    // Hat 0 follows the d-pad: hundredths of a degree clockwise from up, -1 when centred.
    const bool up = normal & bit(DpadUp), down = normal & bit(DpadDown);
    const bool left = normal & bit(DpadLeft), right = normal & bit(DpadRight);
    const int dy = (down ? 1 : 0) - (up ? 1 : 0);
    const int dx = (right ? 1 : 0) - (left ? 1 : 0);
    static const int s_angles[3][3] = { { 31500, 0, 4500 }, { 27000, -1, 9000 }, { 22500, 18000, 13500 } };
    state.pov = s_angles[dy + 1][dx + 1];
    return state;
}

// ---- Stick shaping ----

void shapeStick(float x, float y, uint32_t deadZone, uint32_t saturation, float* outX, float* outY)
{
    const float dead = std::fmax(s_stickDeadZone, float(deadZone) / 10000.0f);
    const float full = std::fmin(s_stickSaturation, float(saturation) / 10000.0f);
    const float magnitude = std::sqrt(x * x + y * y);
    if (magnitude <= dead)
    {
        *outX = 0.0f;
        *outY = 0.0f;
        return;
    }
    const float t = full > dead ? std::fmin(1.0f, (magnitude - dead) / (full - dead)) : 1.0f;
    const float scale = t / magnitude;
    *outX = std::fmax(-1.0f, std::fmin(1.0f, x * scale));
    *outY = std::fmax(-1.0f, std::fmin(1.0f, y * scale));
}

}}
