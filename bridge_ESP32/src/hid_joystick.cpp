#include "hid_joystick.h"
#include <cstring>

JoystickHID g_hidJoystick;

// ── HID Report Descriptor ─────────────────────────────────────────────────────
// Report ID 1 — Joystick: 8 Achsen (int16) + 128 Buttons  → 32 Byte Nutzdaten
// Report ID 2 — Keyboard: 1 Byte Modifier + 1 Byte Reserved + 6 Keycodes → 8 Byte
//
// Report IDs sind zwingend wenn zwei Collections im selben HID-Interface leben.
// Windows-Seite: bei Deskriptor-Änderung einmalig HID-Gerät in Gerätemanager
// deinstallieren und ESP32 neu einstecken.
static const uint8_t HID_JOYSTICK_DESC[] = {
    // ── Report ID 1: Joystick ─────────────────────────────────────────────────
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x04,        // Usage (Joystick)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x01,        //   Report ID (1)

    // 8 Achsen (int16, −32767..+32767)
    0x05, 0x01,        //   Usage Page (Generic Desktop)
    0x09, 0x30,        //   Usage (X)
    0x09, 0x31,        //   Usage (Y)
    0x09, 0x32,        //   Usage (Z)
    0x09, 0x33,        //   Usage (Rx)
    0x09, 0x34,        //   Usage (Ry)
    0x09, 0x35,        //   Usage (Rz)
    0x09, 0x36,        //   Usage (Slider)
    0x09, 0x37,        //   Usage (Dial)
    0x16, 0x01, 0x80,  //   Logical Minimum (−32767)
    0x26, 0xFF, 0x7F,  //   Logical Maximum (+32767)
    0x75, 0x10,        //   Report Size (16)
    0x95, 0x08,        //   Report Count (8)
    0x81, 0x02,        //   Input (Data, Variable, Absolute)

    // 128 Buttons (1 Bit je Button)
    0x05, 0x09,        //   Usage Page (Button)
    0x19, 0x01,        //   Usage Minimum (1)
    0x29, 0x80,        //   Usage Maximum (128)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x80,        //   Report Count (128)
    0x81, 0x02,        //   Input (Data, Variable, Absolute)
    0xC0,              // End Collection (Joystick)

    // ── Report ID 2: Keyboard (Standard-Boot-Format) ──────────────────────────
    // Modifier-Byte Bits (HID Usage 0xE0..0xE7, Reihenfolge):
    //   Bit 0 = Left Control  (KB_LCTRL  = 0x01) ← direkt kompatibel mit KB_*-Konstanten
    //   Bit 1 = Left Shift    (KB_LSHIFT = 0x02)
    //   Bit 2 = Left Alt      (KB_LALT   = 0x04)
    //   Bit 3 = Left GUI      (nicht in KB_* genutzt)
    //   Bit 4 = Right Control (KB_RCTRL  = 0x10)
    //   Bit 5 = Right Shift   (KB_RSHIFT = 0x20)
    //   Bit 6 = Right Alt     (nicht in KB_* genutzt)
    //   Bit 7 = Right GUI     (nicht in KB_* genutzt)
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x06,        // Usage (Keyboard/Keypad)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x02,        //   Report ID (2)

    // Modifier-Bits (8 × 1 Bit)
    0x05, 0x07,        //   Usage Page (Keyboard/Keypad)
    0x19, 0xE0,        //   Usage Minimum (Left Control)
    0x29, 0xE7,        //   Usage Maximum (Right GUI)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x08,        //   Report Count (8)
    0x81, 0x02,        //   Input (Data, Variable, Absolute)

    // Reserved (1 Byte)
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x08,        //   Report Size (8)
    0x81, 0x03,        //   Input (Constant, Variable, Absolute)

    // 6 Keycodes (je 1 Byte)
    0x95, 0x06,        //   Report Count (6)
    0x75, 0x08,        //   Report Size (8)
    0x15, 0x00,        //   Logical Minimum (0)
    0x26, 0xFF, 0x00,  //   Logical Maximum (255)
    0x05, 0x07,        //   Usage Page (Keyboard/Keypad)
    0x19, 0x00,        //   Usage Minimum (0)
    0x29, 0xFF,        //   Usage Maximum (255)
    0x81, 0x00,        //   Input (Data, Array, Absolute)
    0xC0               // End Collection (Keyboard)
};

// Interner Puffer für den 32-Byte-HID-Report
struct JoystickReport {
    int16_t axes[8];     // 16 Bytes
    uint8_t buttons[16]; // 16 Bytes
};
static_assert(sizeof(JoystickReport) == 32, "HID Report muss 32 Bytes sein");

// ── _onGetDescriptor ─────────────────────────────────────────────────────────
uint16_t JoystickHID::_onGetDescriptor(uint8_t* buffer) {
    memcpy(buffer, HID_JOYSTICK_DESC, sizeof(HID_JOYSTICK_DESC));
    return sizeof(HID_JOYSTICK_DESC);
}

// ── begin ─────────────────────────────────────────────────────────────────────
// Muss VOR USB.begin() aufgerufen werden, damit das Interface registriert ist
// bevor der TinyUSB-Stack startet.
bool JoystickHID::begin() {
    _hid.addDevice(this, sizeof(HID_JOYSTICK_DESC));
    _hid.begin();
    return true;
}

// ── send ─────────────────────────────────────────────────────────────────────
// Report ID 1 — Joystick (32 Byte Nutzdaten; ID-Byte vom Stack vorangestellt).
// Gibt false zurück wenn kein USB-Host verbunden oder Timeout (100 ms).
bool JoystickHID::send(const JoystickState& state) {
    JoystickReport rep;
    memcpy(rep.axes,    state.axes,    sizeof(rep.axes));
    memcpy(rep.buttons, state.buttons, sizeof(rep.buttons));
    return _hid.SendReport(1, &rep, sizeof(rep));
}

// ── sendKeyboard ──────────────────────────────────────────────────────────────
// Report ID 2 — Keyboard (8 Byte: Modifier + Reserved + 6 Keycodes).
// KB_*-Bitmask wird direkt als HID-Modifier-Byte verwendet (Bit-Positionen identisch).
// Gibt false zurück wenn kein USB-Host verbunden oder Timeout (100 ms).
bool JoystickHID::sendKeyboard(uint8_t modifiers) {
    uint8_t rep[8] = {};
    rep[0] = modifiers;   // Modifier-Byte (KB_LCTRL=bit0, KB_LSHIFT=bit1, ...)
    // rep[1] = reserved = 0
    // rep[2..7] = keycodes = 0 (nur Modifier, keine regulären Tasten)
    return _hid.SendReport(2, rep, sizeof(rep));
}
