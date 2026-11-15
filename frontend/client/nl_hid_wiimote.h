// NL Recomp - echte Wiimotes fuer NLOnline (Windows-HID, ohne Zusatz-Treiber).
// Koppeln wie bei Dolphin/Cemu: Windows-Bluetooth -> Geraet hinzufuegen -> 1+2 druecken
// (oder DolphinBar im Modus 4). Liest Knoepfe, Beschleunigung, Kamera (Zeiger), Nunchuk und MotionPlus.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct NLHidWiimoteState {
    bool connected = false;
    std::string name;                 // "Wiimote 1"
    int battery = -1;                 // 0..100
    bool nunchuk = false, motion_plus = false;
    // Knoepfe als nlnet::WiiButton-Bits (Wiimote + Nunchuk C/Z)
    uint16_t buttons = 0;
    float nx = 0, ny = 0;             // Nunchuk-Stick -1..1, y nach oben positiv
    // Beschleunigung in g, Rohachsen der Wiimote (wiibrew): x seitlich, y vorne, z oben (flach: 0,0,1)
    float acc[3] = {0, 0, 1};
    // MotionPlus in Grad/s: pitch + = Spitze hoch, yaw + = nach links drehen, roll + = rechts kippen
    float pitch_rate = 0, yaw_rate = 0, roll_rate = 0;
    bool gyro_valid = false;
    // Kamera: Zeiger 0..1 auf dem Bild, gueltig wenn die Sensorleiste gesehen wird
    bool ir_seen = false;
    int ir_dots = 0;                  // wie viele Lichtpunkte die Kamera sieht (2 = perfekt)
    float px = 0.5f, py = 0.5f;
    uint32_t sample_us = 0;           // Zeitstempel der letzten Daten
};

void nl_hidwii_start();                               // Suche im Hintergrund (alle 2 s)
void nl_hidwii_stop();
std::vector<NLHidWiimoteState> nl_hidwii_list();      // alle verbundenen (Kopie)
bool nl_hidwii_get(int index, NLHidWiimoteState& out);
void nl_hidwii_rumble(int index, bool on);
void nl_hidwii_set_mpls_signs(int pitch, int yaw, int roll);   // +1/-1, falls eine Achse falsch herum ist
