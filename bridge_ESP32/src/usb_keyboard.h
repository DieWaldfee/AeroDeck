#pragma once
#include <cstdint>

// ══════════════════════════════════════════════════════════════════════════════
// USB-Keyboard-HID — Modifier-Tasten parallel zum HID-Joystick
//
// Keyboard-Reports werden als Report ID 2 über denselben _hid-Kanal wie der
// Joystick (Report ID 1) gesendet. Kein separates USBHIDKeyboard nötig.
// Kein kbBegin() erforderlich — die Keyboard-Collection ist Teil des Joystick-
// Deskriptors und wird mit g_hidJoystick.begin() automatisch registriert.
//
// Verwendung:
//   kbPressModifier(KB_LSHIFT)   → Modifier-Taste drücken (Report ID 2 senden)
//   kbReleaseAll()               → alle Modifier-Tasten loslassen
//   kbModName(KB_LSHIFT)         → "LSHIFT"  (für SHOW MAP / Diagnose)
// ══════════════════════════════════════════════════════════════════════════════

void        kbPressModifier(uint8_t kbMod);   // KB_* Bitmask → Report ID 2 senden
void        kbReleaseAll();                    // leerer Keyboard-Report senden
const char* kbModName(uint8_t kbMod);         // Klartext für SHOW MAP
