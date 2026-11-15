// NL Recomp - Eingaben von NLOnline: eigene Belegung (Tastatur, Maus, Controller), echte Wiimotes,
// Zeiger und Bewegung. Daraus wird das fertige Wiimote-Paket fuer den Host.
#pragma once
#include <ostream>
#include <string>
#include <windows.h>

#include "../net/nl_proto.h"

struct SDL_Gamepad;
struct ImFont;

struct NLInFrame {
    bool playing = false;          // im Spiel (nicht Startseite)
    bool blocked = false;          // Pause-Menue/Chat offen: nichts ans Spiel
    bool mouse_on_tv = false;      // Maus liegt auf dem TV-Bild
    float mouse_u = 0, mouse_v = 0;
    bool focused = false;          // Fenster ist vorne
};

void nlin_init();
void nlin_shutdown();
void nlin_set_gamepad(SDL_Gamepad* pad, const std::string& name);
// Einmal pro Bild: liest alles und baut das Paket (ohne seq)
nlnet::Input nlin_poll(const NLInFrame& f);
// Zeiger, wie er gerade beim Gast ist (fuer die eigene Zeiger-Anzeige)
bool nlin_pointer(float& u, float& v, bool& from_wiimote);
bool nlin_sideways();
void nlin_set_sideways(bool on);

// Einstellungen in NLOnline.ini
bool nlin_load_kv(const std::string& k, const std::string& v);
void nlin_save(std::ostream& out);

// Steuerungs-Seite (ImGui, im aktuellen Fenster). Gibt true zurueck, wenn Einstellungen geaendert wurden.
bool nlin_ui(ImFont* big);
bool nlin_capturing();   // gerade wird eine Taste neu belegt (Esc bricht ab)
