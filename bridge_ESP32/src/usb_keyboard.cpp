#include "usb_keyboard.h"
#include "config_manager.h"   // KB_* Konstanten
#include "hid_joystick.h"     // g_hidJoystick.sendKeyboard()

// Keyboard-Report über Report ID 2 im gemeinsamen _hid-Kanal senden.
// KB_*-Bitmask entspricht direkt dem HID-Modifier-Byte (Bit-Positionen identisch).

void kbPressModifier(uint8_t kbMod) {
    if (!kbMod) return;
    g_hidJoystick.sendKeyboard(kbMod);
}

void kbReleaseAll() {
    g_hidJoystick.sendKeyboard(0);
}

const char* kbModName(uint8_t kbMod) {
    switch (kbMod) {
        case KB_NONE:   return "-";
        case KB_LCTRL:  return "LCTRL";
        case KB_LSHIFT: return "LSHIFT";
        case KB_LALT:   return "LALT";
        case KB_RCTRL:  return "RCTRL";
        case KB_RSHIFT: return "RSHIFT";
        default:        return "MULTI";
    }
}
