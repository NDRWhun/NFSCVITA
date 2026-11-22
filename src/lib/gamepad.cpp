#include <lib/gamepad.h>
#include <lib/window.h>
#include <platform/ime.h>
#include <platform/log.h>
#include <platform/sync.h>
#include <platform/time.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>

namespace win32
{

namespace
{

// Typed text goes out one key message per this interval.
const uint64_t TypingIntervalUs = 20000;
// A typed key change waits for this many reads of the keyboard device after the previous one, or this long.
const uint32_t KeyReads = 2;
const uint64_t KeyReadTimeoutUs = 1000000;

// A key message, or a WM_CHAR when character is set.
struct TypedKey
{
    controls::KeyCode   key;
    bool                down;
    char                character;
};

// The text field the game edits (editText).
struct Field
{
    bool        active;
    bool        shown;          // the keyboard was opened for it
    char16_t    title[pf::ime::MaxTitleLength + 1];
    char16_t    text[pf::ime::MaxTextLength + 1];
    uint32_t    length;         // characters in text
    uint32_t    maxLength;
    char        extraChars[64]; // accepted besides letters and digits
    bool        cancellable;    // Escape cancels it
};

// Backspace, as a character, deletes before the cursor.
const char CharBackspace = 0x08;
// End moves the line editor's cursor behind the text.
const controls::KeyCode KeyEnd = { 0x23, 0x4F, true };

// The pump runs on the message thread, at frames and in polls; s_mutex guards everything below.
pf::LwMutex             s_mutex("input");
controls::Mapper        s_mapper;
controls::State         s_state;
uint32_t                s_keysDown;         // controls::Key bits posted down
bool                    s_imeOpen;          // on-screen keyboard requested or shown for s_field
bool                    s_releaseWait;      // after the keyboard and its typing, until the pad is released
uint32_t                s_padInputs;        // inputs of the last pump
Field                   s_field;
std::deque<TypedKey>    s_typing;           // key messages of confirmed text not posted yet
uint64_t                s_typedUs;          // when the last one was posted
uint32_t                s_keyboardReads;    // Keyboard::getState calls
uint32_t                s_typedReads;       // s_keyboardReads when the last one was posted
bool                    s_typedDown[256];   // typed keys down, by DirectInput code
Gamepad::PollHook       s_pollHook;         // not guarded: set before the game starts

void copyText(char16_t* target, const char* source, uint32_t maxLength)
{
    uint32_t length = 0;
    for (; source[length] && length < maxLength; ++length)
        target[length] = char16_t(uint8_t(source[length]));
    target[length] = 0;
}

void postKey(const controls::KeyCode& key, bool down)
{
    Window::pushKey(key.vk, key.scancode, key.extended, down);
}

// Posts the changes from s_keysDown to held: releases in reverse key order, then presses in key order.
void postKeys(uint32_t held)
{
    const uint32_t released = s_keysDown & ~held;
    const uint32_t pressed = held & ~s_keysDown;
    for (int key = controls::KeyCount - 1; key >= 0; --key)
    {
        if (released & (uint32_t(1) << key))
            postKey(controls::keyCode(controls::Key(key)), false);
    }
    for (int key = 0; key < controls::KeyCount; ++key)
    {
        if (pressed & (uint32_t(1) << key))
            postKey(controls::keyCode(controls::Key(key)), true);
    }
    s_keysDown = held;
}

void queueKey(const controls::KeyCode& key)
{
    s_typing.push_back(TypedKey{ key, true, 0 });
    s_typing.push_back(TypedKey{ key, false, 0 });
}

void queueChar(char character)
{
    s_typing.push_back(TypedKey{ controls::KeyCode{ 0, 0, false }, false, character });
}

// Empties s_field: cursor to the end, then one Backspace per character.
void queueClear()
{
    queueKey(KeyEnd);
    for (uint32_t i = 0; i < s_field.length; ++i)
        queueChar(CharBackspace);
}

// Replaces the field's text with the confirmed text, one WM_CHAR per character the field takes (at most its
// maxLength), and presses Enter, which commits the field.
void queueText(const char* text)
{
    queueClear();
    uint32_t typed = 0;
    for (const char* c = text; *c && typed < s_field.maxLength; ++c)
    {
        const bool alphanumeric = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9');
        const bool printable = x86::reg8(*c) >= 0x20 && x86::reg8(*c) < 0x7f;
        if (!printable || !(alphanumeric || strchr(s_field.extraChars, *c)))
        {
            pf::logInfo("input: '%c' cannot be typed", *c);
            continue;
        }
        queueChar(*c);
        ++typed;
    }
    queueKey(controls::keyCode(controls::KeyEnter));
}

// Cancels a field with Escape; a field that refuses that is emptied and committed instead.
void queueCancel()
{
    if (s_field.cancellable)
    {
        queueKey(controls::keyCode(controls::KeyEscape));
        return;
    }
    queueClear();
    queueKey(controls::keyCode(controls::KeyEnter));
}

void openIme()
{
    s_imeOpen = pf::ime::requestOpen(s_field.title, s_field.text, s_field.maxLength);
}

void updateIme()
{
    char text[pf::ime::MaxTextLength + 1];
    const pf::ime::Result result = pf::ime::takeResult(text, sizeof(text));
    if (result == pf::ime::ResultPending)
        return;
    s_imeOpen = false;
    if (!s_field.active)
        return;     // the field closed meanwhile
    if (result == pf::ime::ResultConfirmed)
        queueText(text);
    else if (result == pf::ime::ResultCancelled)
        queueCancel();
}

// A key change waits until the keyboard device has been read KeyReads times since the previous message.
void typeNext(uint64_t now)
{
    if (s_typing.empty() || now - s_typedUs < TypingIntervalUs)
        return;
    if (!s_typing.front().character && s_keyboardReads - s_typedReads < KeyReads && now - s_typedUs < KeyReadTimeoutUs)
        return;
    const TypedKey typed = s_typing.front();
    s_typing.pop_front();
    if (typed.character)
    {
        Window::pushChar(x86::reg8(typed.character));
    }
    else
    {
        s_typedDown[typed.key.dik()] = typed.down;
        postKey(typed.key, typed.down);
    }
    s_typedUs = now;
    s_typedReads = s_keyboardReads;
}

// Under s_mutex. While the on-screen keyboard is up or its text is being typed, and until every pad input is
// released after that (a button that closed the keyboard does not reach the game), the pad holds no keys or buttons.
// While a field is edited with the keyboard closed, Cross (or the Select chord) opens it and Circle cancels the
// field; neither reaches the game then. The pad holds keys only while a field is edited.
void pump()
{
    const uint64_t now = pf::ticksUs();
    const controls::PadSample pad = controls::readPad();
    const uint32_t pressed = pad.inputs & ~s_padInputs;
    s_padInputs = pad.inputs;
    controls::State state = s_mapper.update(pad, now);
    if (s_imeOpen || !s_typing.empty())
        s_releaseWait = true;
    else if (!pad.inputs)
        s_releaseWait = false;
    const bool busy = s_imeOpen || !s_typing.empty() || s_releaseWait;
    if (!busy && s_field.active && !s_field.shown)
    {
        openIme();
        s_field.shown = s_imeOpen;
    }
    else if (!busy && s_field.active && (pressed & controls::bit(controls::Circle)))
    {
        queueCancel();
    }
    else if (!busy && s_field.active && (state.openIme || (pressed & controls::bit(controls::Cross))))
    {
        openIme();
    }
    if (s_field.active)
        state.buttons &= ~(controls::buttonMask(controls::Circle) | controls::buttonMask(controls::Cross));
    else
        state.keys = 0;
    if (busy)
    {
        state.buttons = 0;
        state.keys = 0;
        state.pov = -1;
        memset(state.axes, 0, sizeof(state.axes));
    }
    postKeys(state.keys);
    if (s_imeOpen)
        updateIme();
    typeNext(now);
    s_state = state;
}

}

x86::reg32 Input::getButtonCount() const
{
    return 0;
}

x86::reg32 Input::getAxesCount() const
{
    return 0;
}

// DIDEVTYPE_KEYBOARD, DIDEVTYPEKEYBOARD_PCENH.
x86::reg32 Keyboard::getDeviceType() const
{
    return 0x0403;
}

// DI8DEVTYPE_KEYBOARD, DI8DEVTYPEKEYBOARD_PCENH.
x86::reg32 Keyboard::getDeviceType8() const
{
    return 0x0413;
}

x86::reg32 Keyboard::getButtonCount() const
{
    return 128;
}

void Keyboard::getState(x86::reg8 (&keys)[256])
{
    pf::LwLockGuard lock(s_mutex);
    ++s_keyboardReads;
    pump();
    memset(keys, 0, sizeof(keys));
    for (int key = 0; key < controls::KeyCount; ++key)
    {
        if (s_keysDown & (uint32_t(1) << key))
            keys[controls::keyCode(controls::Key(key)).dik()] = 0x80;
    }
    for (int code = 0; code < 256; ++code)
    {
        if (s_typedDown[code])
            keys[code] = 0x80;
    }
}

// DIDEVTYPE_MOUSE, DIDEVTYPEMOUSE_TRADITIONAL.
x86::reg32 Mouse::getDeviceType() const
{
    return 0x0202;
}

// DI8DEVTYPE_MOUSE, DI8DEVTYPEMOUSE_TRADITIONAL.
x86::reg32 Mouse::getDeviceType8() const
{
    return 0x0212;
}

x86::reg32 Mouse::getButtonCount() const
{
    return 3;
}

x86::reg32 Mouse::getAxesCount() const
{
    return 3;
}

Gamepad::Gamepad(x86::reg32 gamepadIndex)
    :   m_index(gamepadIndex)
{
    // DirectInput defaults: range 0..65535, no dead zone, saturation at full deflection.
    for (AxisProperties& axis : m_axisProperties)
        axis = { 0, 65535, 0, 10000 };
}

Gamepad::~Gamepad()
{
}

x86::reg32 Gamepad::getCount()
{
    return 1;
}

x86::reg32 Gamepad::getButtonCount() const
{
    return controls::joystickButtonCount();
}

x86::reg32 Gamepad::getAxesCount() const
{
    return controls::presentAxisCount();
}

x86::reg32 Gamepad::getDeviceType() const
{
    return controls::JoystickDeviceType;
}

// DI8DEVTYPE_GAMEPAD, DI8DEVTYPEGAMEPAD_STANDARD.
x86::reg32 Gamepad::getDeviceType8() const
{
    return 0x0215;
}

// Each stick is shaped as a whole, the left one with the properties of lX, the right one with those of Rx; absent
// axes read centred.
void Gamepad::getState(x86::reg32* buttons, x86::sreg32 (&axes)[controls::AxisCount], int* pov)
{
    controls::State state;
    {
        pf::LwLockGuard lock(s_mutex);
        pump();
        state = s_state;
    }
    float shaped[controls::AxisCount] = {};
    const AxisProperties& left = m_axisProperties[controls::AxisX];
    controls::shapeStick(state.axes[controls::AxisX], state.axes[controls::AxisY], left.deadZone, left.saturation,
                         &shaped[controls::AxisX], &shaped[controls::AxisY]);
    const AxisProperties& right = m_axisProperties[controls::AxisRx];
    controls::shapeStick(state.axes[controls::AxisRx], state.axes[controls::AxisRz], right.deadZone, right.saturation,
                         &shaped[controls::AxisRx], &shaped[controls::AxisRz]);
    *pov = state.pov;
    *buttons = state.buttons;
    if (s_pollHook)
        s_pollHook(buttons, pov, shaped);
    for (int axis = 0; axis < controls::AxisCount; ++axis)
    {
        const AxisProperties& properties = m_axisProperties[axis];
        const float position = (shaped[axis] + 1.0f) * 0.5f;
        const float range = float(properties.max) - float(properties.min);
        axes[axis] = x86::sreg32(std::lround(float(properties.min) + position * range));
    }
}

void Gamepad::setPollHook(PollHook hook)
{
    s_pollHook = hook;
}

void Gamepad::updateKeys()
{
    pf::LwLockGuard lock(s_mutex);
    pump();
}

void Gamepad::editText(const char* title, const char* text, uint32_t maxLength, const char* extraChars,
                       bool cancellable)
{
    pf::LwLockGuard lock(s_mutex);
    if (!s_field.active)
    {
        s_field.active = true;
        s_field.shown = false;
        pf::logInfo("input: text field \"%s\" \"%s\" (%u characters, also \"%s\"%s)", title, text, unsigned(maxLength),
                    extraChars, cancellable ? "" : ", no cancel");
    }
    copyText(s_field.title, title, pf::ime::MaxTitleLength);
    copyText(s_field.text, text, pf::ime::MaxTextLength);
    s_field.length = uint32_t(strnlen(text, pf::ime::MaxTextLength));
    s_field.maxLength = maxLength;
    snprintf(s_field.extraChars, sizeof(s_field.extraChars), "%s", extraChars);
    s_field.cancellable = cancellable;
}

void Gamepad::endTextEdit()
{
    pf::LwLockGuard lock(s_mutex);
    if (!s_field.active)
        return;
    s_field.active = false;
    if (s_imeOpen)
        pf::ime::requestClose();
    pf::logInfo("input: text field closed");
}

}
