#pragma once
#include <USBHID.h>
#include "bridge_types.h"

// ══════════════════════════════════════════════════════════════════════════════
// USB HID Joystick — nativer USB-Port (GPIO19/20), kein CDC auf diesem Port
//
// HID Report Descriptor: 8 Achsen (int16) + 128 Buttons (1 Bit je)
// Report-Größe: 8×2 + 128/8 = 32 Bytes, kein Report-ID-Byte
//
// Achsen-Skala: −32767 (min) … 0 (Mitte) … +32767 (max)
// Buttons:      Button 1 = buttons[0] Bit 0
//
// Initialisierung in main.cpp:
//   g_hidJoystick.begin();   ← vor USB.begin()!
//   USB.begin();
// ══════════════════════════════════════════════════════════════════════════════

class JoystickHID : public USBHIDDevice {
public:
    bool begin();
    bool send(const JoystickState& state);
    bool sendKeyboard(uint8_t modifiers);   // Report ID 2: Modifier-Byte → Keyboard-HID
    bool ready()       { return _hid.ready(); }
    uint16_t _onGetDescriptor(uint8_t* buffer) override;

private:
    USBHID _hid;
};

extern JoystickHID g_hidJoystick;
