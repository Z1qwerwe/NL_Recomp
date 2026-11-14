// Nintendo Land Recomp - TV- und GamePad-Bild in einem Fenster.
// Wird vom Grafik-Unterbau (LatteRenderTarget.cpp, per Patch) und vom Frontend benutzt.
#pragma once

struct NLScreens {
    int mode = 0;                 // 0 = eins von beiden (Tab wechselt), 1 = Bild-in-Bild, 2 = nebeneinander
    // Rechner fuer die Bildflaechen: Fenstergroesse (echte Pixel), GamePad gross? -> x, y, w, h
    void (*layout)(int win_w, int win_h, bool drc_big, int* tv, int* drc) = nullptr;
    // Waehrend des Zeichnens: feste Flaeche statt Vollbild, Hintergrund loeschen, Overlay weglassen
    const int* rect = nullptr;
    bool clear = false;
    bool skip_overlay = false;
    // Zuletzt benutzte GamePad-Flaeche im Hauptfenster (fuer Touch per Maus), w = 0: keine
    int drc_rect[4] = {0, 0, 0, 0};
};
extern NLScreens g_nl_screens;
