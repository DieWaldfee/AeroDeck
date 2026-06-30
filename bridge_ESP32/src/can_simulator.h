#pragma once

// ══════════════════════════════════════════════════════════════════════════════
// can_simulator.h — CAN-FD Software-Simulator (Ersatz für MCP2518FD)
//
// Aktiv solange -DCAN_HW_ENABLED in platformio.ini NICHT gesetzt ist.
//
// Simuliert:
//   • HID-Buttons 0/1: 4-Schritt-Muster je 1s (4s-Zyklus)
//       1) Btn0 AN / Btn1 AUS  2) Btn0 AN / Btn1 AN
//       3) Btn0 AUS / Btn1 AN  4) Btn0 AUS / Btn1 AUS
//   • HID-Achse 5 (Schieberegler): Dreieckswelle 0→100%→0 (4s-Periode)
//   • HID-Achsen 0–3: langsame Sinus-Bewegung (8/12/6/16s Perioden)
//
// Horizon-Simulation wurde entfernt — das reale MCP2518FD-Gerät übernimmt.
//
// Aktiv wenn:
//   • CAN_HW_ENABLED NICHT gesetzt (reiner Simulator-Modus), ODER
//   • CAN_SIM_INPUTS gesetzt (Hybrid: Hardware + Input-Simulation)
// ══════════════════════════════════════════════════════════════════════════════
#if !defined(CAN_HW_ENABLED) || defined(CAN_SIM_INPUTS)

// HID-Mappings registrieren + Startmeldung loggen.
// Muss vor xTaskCreate(taskCanSimulator) aufgerufen werden.
void canSimulatorInit();

// FreeRTOS-Task: erzeugt periodisch Button-, Achsen- und Heartbeat-Frames
// und schreibt sie in g_canToHidQueue / g_canToUdpQueue / g_canToUsbQueue.
void taskCanSimulator(void* param);

#endif // !CAN_HW_ENABLED || CAN_SIM_INPUTS
