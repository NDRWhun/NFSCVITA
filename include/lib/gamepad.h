#ifndef LIB_GAMEPAD_H_
#define LIB_GAMEPAD_H_

#include <lib/controls.h>
#include <lib/winapp.h>

namespace win32
{

enum InputType
{
    InputType_Keyboard,
    InputType_Mouse,
    InputType_Gamepad
};

// A DirectInput device.
class Input : public GenericResource
{
public:
    virtual x86::reg32 getButtonCount() const;
    virtual x86::reg32 getAxesCount() const;
    // DIDEVICEINSTANCE / DIDEVCAPS dwDevType, as DirectInput 7 and as DirectInput 8 report it.
    virtual x86::reg32 getDeviceType() const = 0;
    virtual x86::reg32 getDeviceType8() const = 0;
};

// The synthesized keyboard: the keys the pad holds (controls.cpp) and text typed from the on-screen keyboard.
class Keyboard : public Input
{
public:
    virtual x86::reg32 getButtonCount() const;
    virtual x86::reg32 getDeviceType() const;
    virtual x86::reg32 getDeviceType8() const;
    // GetDeviceState: 0x80 at the DirectInput code of each key down.
    static void getState(x86::reg8 (&keys)[256]);
};

// No mouse is attached: it reports no motion and no buttons.
class Mouse : public Input
{
public:
    virtual x86::reg32 getButtonCount() const;
    virtual x86::reg32 getAxesCount() const;
    virtual x86::reg32 getDeviceType() const;
    virtual x86::reg32 getDeviceType8() const;
};

// The Vita controller as DirectInput joystick 0; layout in controls.cpp.
class Gamepad : public Input
{
public:
    // DirectInput axis properties (DIPROP_RANGE, DIPROP_DEADZONE, DIPROP_SATURATION).
    struct AxisProperties
    {
        x86::sreg32 min;
        x86::sreg32 max;
        x86::reg32  deadZone;   // 0..10000 of the distance from the centre
        x86::reg32  saturation;
    };

    explicit Gamepad(x86::reg32 gamepadIndex);
    ~Gamepad();

    virtual x86::reg32 getButtonCount() const;
    virtual x86::reg32 getAxesCount() const;
    virtual x86::reg32 getDeviceType() const;
    virtual x86::reg32 getDeviceType8() const;
    x86::reg32 index() const { return m_index; }

    static x86::reg32 getCount();

    // Guest threads with the execution lock held.
    AxisProperties& axisProperties(controls::Axis axis) { return m_axisProperties[axis]; }

    // Joystick buttons and axes now, axes shaped with this device's properties and scaled to their ranges.
    void getState(x86::reg32* buttons, x86::sreg32 (&axes)[controls::AxisCount], int* pov);

    // Game code run at each getState on the guest thread (execution lock held) with the buttons, hat and shaped axes
    // (-1..1) about to be reported; it may change them. Set once before the game starts.
    typedef void (*PollHook)(x86::reg32* buttons, int* pov, float (&axes)[controls::AxisCount]);
    static void setPollHook(PollHook hook);

    // Input pump: reads the pad, posts the key changes as window messages and runs the on-screen keyboard. Called
    // by the message loop, at each presented frame and at each poll. Any thread.
    static void updateKeys();

    // A text field of the game is edited and holds text (ASCII; any thread; called again while it lasts); its first
    // call opens the on-screen keyboard. Confirmed text replaces the field's as WM_CHAR, then Enter is pressed; a
    // cancel, or Circle with the keyboard closed, presses Escape (or empties the field and presses Enter when it is
    // not cancellable). Cross with the keyboard closed opens it again.
    static void editText(const char* title, const char* text, uint32_t maxLength, const char* extraChars,
                         bool cancellable);
    // The field is no longer edited: a keyboard still open for it closes without typing.
    static void endTextEdit();

private:
    x86::reg32      m_index;
    AxisProperties  m_axisProperties[controls::AxisCount];
};

}

#endif
