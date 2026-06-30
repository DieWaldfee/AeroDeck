#pragma once
#include <string>

// Prüft bridge.ini auf Konsistenz, Vollständigkeit und häufige Fehler.
// Schreibt das Ergebnis nach inicheck.log im aktuellen Verzeichnis.
// Rückgabe: 0 = OK / nur Warnungen, 1 = mindestens ein FEHLER.
int runIniCheck(const std::string& iniPath);
