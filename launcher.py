#!/usr/bin/env python3
"""Nintendo Land Recomp Launcher.

Start:  python launcher.py           (GUI)
        python launcher.py --cli     (alle Schritte ohne GUI, nutzt settings.json)
"""
import os
import queue
import sys
import threading
import traceback

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nlrecomp import pipeline  # noqa: E402

# Wii-U-Look: helles Glas, weisse Kacheln mit Glanz, Cyan als Akzent
BG, BG2, CARD, EDGE = "#eef2f6", "#dde4eb", "#ffffff", "#d3dae2"
FG, MUTED = "#3e424a", "#8c929c"
ACCENT, ACCENT_HI, ACCENT_DK = "#00aee8", "#33c2f0", "#0090c8"
OK, WARN, ERR = "#1f9d55", "#c88a00", "#d93a3a"
RING = ["#e63c3c", "#fa9628", "#fad732", "#50be5a", "#3c96e6", "#965ad2"]

def run_cli():
    s = pipeline.load_settings()
    stop = lambda: False  # noqa: E731
    g = pipeline.step_detect(s, print)
    pipeline.step_analyze(s, g, print)
    pipeline.step_shaders(s, g, print, lambda f: None, stop)
    for step in (pipeline.step_recompile, pipeline.step_build):
        try:
            step(s, g, print, stop)
        except RuntimeError as e:
            print(f"Uebersprungen: {e}")


def _round_rect(c, x0, y0, x1, y1, r, **kw):
    """Abgerundetes Rechteck auf einem Canvas."""
    r = max(0, min(r, (x1 - x0) / 2, (y1 - y0) / 2))
    pts = [x0 + r, y0, x1 - r, y0, x1, y0, x1, y0 + r, x1, y1 - r, x1, y1, x1 - r, y1,
           x0 + r, y1, x0, y1, x0, y1 - r, x0, y0 + r, x0, y0]
    return c.create_polygon(pts, smooth=True, **kw)


def run_gui():
    import tkinter as tk
    from tkinter import filedialog, ttk

    settings = pipeline.load_settings()
    msgs = queue.Queue()
    state = {"busy": False, "stop": False, "game": None}

    root = tk.Tk()
    root.title("Nintendo Land Recomp")
    root.geometry("980x700")
    root.minsize(820, 600)
    root.configure(bg=BG)

    # ttk nur noch fuer die Nebenfenster (Erweitert), im hellen Stil
    style = ttk.Style(root)
    style.theme_use("clam")
    style.configure("TFrame", background=BG)
    style.configure("TLabel", background=BG, foreground=FG, font=("Segoe UI", 10))
    style.configure("Section.TLabel", background=BG, foreground=FG, font=("Segoe UI Semibold", 12))
    style.configure("Sub.TLabel", background=BG, foreground=MUTED, font=("Segoe UI", 9))
    style.configure("TButton", background=CARD, foreground=FG, bordercolor=EDGE, padding=(14, 8), font=("Segoe UI", 10))
    style.map("TButton", background=[("active", "#f0faff")])
    style.configure("Accent.TButton", background=ACCENT, foreground="white", font=("Segoe UI Semibold", 10), padding=(18, 8))
    style.map("Accent.TButton", background=[("active", ACCENT_HI)])
    style.configure("TEntry", fieldbackground="#f3f6f9", foreground=FG, bordercolor=EDGE)

    class GlassButton(tk.Canvas):
        """Glaenzende Kachel wie im Wii-U-Menue (normal weiss, akzent = Cyan)."""

        def __init__(self, parent, text, command, accent=False, w=170, h=54, font=("Segoe UI Semibold", 11),
                     icon=None):
            super().__init__(parent, width=w + 8, height=h + 8, bg=parent["bg"], highlightthickness=0, bd=0,
                             cursor="hand2")
            self.text, self.command, self.accent, self.w, self.h, self.font, self.icon = text, command, accent, w, h, font, icon
            self.enabled, self.hover, self.down = True, False, False
            self.bind("<Enter>", lambda e: self._set(hover=True))
            self.bind("<Leave>", lambda e: self._set(hover=False, down=False))
            self.bind("<ButtonPress-1>", lambda e: self._set(down=True))
            self.bind("<ButtonRelease-1>", self._release)
            self._draw()

        def _set(self, **kw):
            for k, v in kw.items():
                setattr(self, k, v)
            self._draw()

        def _release(self, e):
            fire = self.down and self.hover and self.enabled
            self._set(down=False)
            if fire and self.command:
                self.command()

        def state(self, st):
            self.enabled = "disabled" not in st
            self.configure(cursor="hand2" if self.enabled else "arrow")
            self._draw()

        def _draw(self):
            self.delete("all")
            o = 2 if self.down else 0
            x0, y0, x1, y1 = 4, 3 + o, 4 + self.w, 3 + self.h + o
            r = min(22, self.h / 2)
            if not self.down:
                _round_rect(self, x0 + 1, y0 + 3, x1 + 1, y1 + 4, r, fill="#cfd7e0", outline="")
            if not self.enabled:
                fill, gloss, edge, fg = "#f3f5f7", "#f8f9fa", "#dfe3e8", "#b8bec7"
            elif self.accent:
                fill, gloss, edge, fg = (ACCENT_HI if self.hover else ACCENT), "#5fd0f5", ACCENT_DK, "white"
            else:
                fill, gloss, edge, fg = ("#f0faff" if self.hover else CARD), "#ffffff", (ACCENT if self.hover else EDGE), FG
            _round_rect(self, x0, y0, x1, y1, r, fill=fill, outline=edge, width=1.5)
            _round_rect(self, x0 + 3, y0 + 3, x1 - 3, y0 + (y1 - y0) * 0.48, r - 3, fill=gloss, outline="")
            label = (self.icon + "  " + self.text) if self.icon else self.text
            self.create_text((x0 + x1) / 2, (y0 + y1) / 2 + 1, text=label, fill=fg, font=self.font)

    class Card(tk.Canvas):
        """Weisse Glas-Karte mit Schatten; Inhalt liegt in self.inner."""

        def __init__(self, parent, pad=16, height=None):
            super().__init__(parent, bg=parent["bg"], highlightthickness=0, bd=0, height=height or 10)
            self.pad = pad
            self.inner = tk.Frame(self, bg=CARD)
            self.win = self.create_window(pad, pad, window=self.inner, anchor="nw")
            self.bind("<Configure>", self._draw)
            self.inner.bind("<Configure>", self._fit)
            self.auto_height = height is None

        def _fit(self, e):
            if self.auto_height:
                self.configure(height=e.height + 2 * self.pad + 6)

        def _draw(self, e=None):
            self.delete("bg")
            w, h = self.winfo_width(), self.winfo_height()
            _round_rect(self, 3, 6, w - 3, h - 1, 22, fill="#d6dde5", outline="", tags="bg")
            _round_rect(self, 2, 2, w - 2, h - 5, 22, fill=CARD, outline=EDGE, width=1.5, tags="bg")
            _round_rect(self, 5, 5, w - 5, 46, 18, fill="#ffffff", outline="", tags="bg")
            self.tag_lower("bg")
            self.itemconfigure(self.win, width=w - 2 * self.pad, height=None if self.auto_height else h - 2 * self.pad - 6)

    # --- Kopfzeile: rundes Logo (wie das Mii-Bild oben links) + Titel -------
    head = tk.Frame(root, bg=BG)
    head.pack(fill="x", padx=24, pady=(20, 6))
    logo = tk.Canvas(head, width=64, height=64, bg=BG, highlightthickness=0)
    logo.pack(side="left")
    _round_rect(logo, 2, 2, 62, 62, 18, fill=CARD, outline=EDGE, width=1.5)
    for i, colr in enumerate(RING):
        logo.create_arc(10, 10, 54, 54, start=90 - i * 60, extent=-60, fill=colr, outline=colr)
    logo.create_oval(21, 21, 43, 43, fill="white", outline="white")
    logo.create_text(32, 32, text="NL", fill=FG, font=("Segoe UI Black", 9))
    titles = tk.Frame(head, bg=BG)
    titles.pack(side="left", padx=14)
    title_var = tk.StringVar(value="Nintendo Land Recomp")
    sub_var = tk.StringVar(value="Waehl deinen entpackten Spielordner aus")
    tk.Label(titles, textvariable=title_var, bg=BG, fg=FG, font=("Segoe UI Semibold", 22)).pack(anchor="w")
    tk.Label(titles, textvariable=sub_var, bg=BG, fg=MUTED, font=("Segoe UI", 10)).pack(anchor="w")

    # --- Ordner ----------------------------------------------------------
    pcard = Card(root)
    pcard.pack(fill="x", padx=20, pady=8)
    paths = pcard.inner
    path_vars = {}

    def path_row(r, label, key, hint):
        tk.Label(paths, text=label, bg=CARD, fg=FG, font=("Segoe UI Semibold", 10), width=8,
                 anchor="w").grid(row=r * 2, column=0, sticky="w")
        v = tk.StringVar(value=settings.get(key, ""))
        path_vars[key] = v
        tk.Entry(paths, textvariable=v, bg="#f3f6f9", fg=FG, insertbackground=FG, relief="flat",
                 highlightthickness=1, highlightbackground=EDGE, highlightcolor=ACCENT,
                 font=("Segoe UI", 10)).grid(row=r * 2, column=1, sticky="ew", padx=8, ipady=6)

        def browse():
            d = filedialog.askdirectory(initialdir=v.get() or os.path.expanduser("~/Documents"))
            if d:
                v.set(os.path.normpath(d))
                on_paths_changed()
        GlassButton(paths, "...", browse, w=44, h=34, font=("Segoe UI Semibold", 10)).grid(row=r * 2, column=2)
        tk.Label(paths, text=hint, bg=CARD, fg=MUTED, font=("Segoe UI", 9)).grid(
            row=r * 2 + 1, column=1, sticky="w", padx=8, pady=(0, 6))

    path_row(0, "Spiel", "base_dir", "Ordner mit code/, content/ und meta/")
    path_row(1, "Update", "update_dir", "optional, ebenfalls entpackt")
    paths.columnconfigure(1, weight=1)

    # --- Kacheln -----------------------------------------------------------
    bar = tk.Frame(root, bg=BG)
    bar.pack(fill="x", padx=20, pady=(10, 0))
    buttons = {}

    # --- Fortschritt: schmaler Cyan-Balken ---------------------------------
    class Progress(tk.Canvas):
        def __init__(self, parent):
            super().__init__(parent, height=12, bg=BG, highlightthickness=0)
            self.value = 0.0
            self.bind("<Configure>", lambda e: self._draw())

        def __setitem__(self, k, v):
            self.value = max(0.0, min(1.0, float(v)))
            self._draw()

        def _draw(self):
            self.delete("all")
            w = self.winfo_width()
            _round_rect(self, 0, 2, w, 10, 4, fill=BG2, outline="")
            if self.value > 0:
                _round_rect(self, 0, 2, max(10, w * self.value), 10, 4, fill=ACCENT, outline="")

    progress = Progress(root)
    progress.pack(fill="x", padx=26, pady=(14, 0))

    # --- Log ---------------------------------------------------------------
    lcard = Card(root, pad=14, height=200)
    lcard.pack(fill="both", expand=True, padx=20, pady=(10, 20))
    logbox = tk.Text(lcard.inner, bg=CARD, fg=FG, insertbackground=FG, relief="flat", bd=0,
                     font=("Cascadia Mono", 9), padx=6, pady=4, wrap="word", highlightthickness=0)
    logbox.pack(fill="both", expand=True)
    for tag, colr in (("warn", WARN), ("err", ERR), ("ok", OK), ("cmd", MUTED)):
        logbox.tag_configure(tag, foreground=colr)

    def log(msg):
        msgs.put(("log", str(msg)))

    def set_progress(f):
        msgs.put(("progress", f))

    def pump():
        try:
            while True:
                kind, val = msgs.get_nowait()
                if kind == "log":
                    tag = ("warn" if val.startswith("WARNUNG") or "pending" in val else
                           "err" if val.startswith(("FEHLER", "  !")) else
                           "ok" if val.startswith("Fertig") else
                           "cmd" if val.startswith(">") else "")
                    logbox.insert("end", val + "\n", tag)
                    logbox.see("end")
                elif kind == "progress":
                    progress["value"] = val
                elif kind == "done":
                    state["busy"] = False
                    refresh_buttons()
        except queue.Empty:
            pass
        root.after(50, pump)

    def save_paths():
        for k, v in path_vars.items():
            settings[k] = v.get().strip()
        pipeline.save_settings(settings)

    def on_paths_changed():
        save_paths()
        if not settings["base_dir"]:
            return
        try:
            g = pipeline.step_detect(settings, log)
            state["game"] = g
            title_var.set(g.base.name or "Unbekanntes Spiel")
            ver = g.update.version if g.update and g.update.kind == "decrypted" else g.base.version
            sub_var.set(f"{g.base.title_id}  |  v{ver}  |  "
                        + ("bereit" if g.rpx else "nicht entpackt"))
        except Exception as e:
            log(f"FEHLER: {e}")
        refresh_buttons()

    def worker(fn):
        if state["busy"]:
            return
        save_paths()
        state.update(busy=True, stop=False)
        refresh_buttons()
        progress["value"] = 0

        def run():
            try:
                fn()
                log("Fertig.")
            except Exception as e:
                log(f"FEHLER: {e}")
                if not isinstance(e, RuntimeError):
                    log(traceback.format_exc())
            msgs.put(("done", None))
        threading.Thread(target=run, daemon=True).start()

    stop_fn = lambda: state["stop"]  # noqa: E731

    def do_analyze():
        worker(lambda: pipeline.step_analyze(settings, state["game"], log))

    def do_shaders():
        worker(lambda: pipeline.step_shaders(settings, state["game"], log, set_progress, stop_fn))

    def do_build():
        def all_steps():
            g = state["game"]
            pipeline.step_analyze(settings, g, log)
            pipeline.step_shaders(settings, g, log, set_progress, stop_fn)
            pipeline.step_recompile(settings, g, log, stop_fn)
            pipeline.step_build(settings, g, log, stop_fn)
        worker(all_steps)

    def do_cemu():
        worker(lambda: pipeline.step_cemu_build(settings, state["game"], log, stop_fn))

    def do_play():
        worker(lambda: pipeline.step_launch(settings, state["game"], log))

    def do_stop():
        state["stop"] = True
        log("Wird abgebrochen...")

    def open_advanced():
        win = tk.Toplevel(root)
        win.title("Erweitert (Entwickler)")
        win.configure(bg=BG)
        win.geometry("760x340")
        frm = ttk.Frame(win, padding=16)
        frm.pack(fill="both", expand=True)
        fields = [
            ("work_dir", "Arbeitsordner", "Analyse, Shader-Cache und generierter Code"),
            ("cemu_dir", "Engine-Ordner", "leer = C:\\NLR"),
            ("recompiler_cmd", "Recompiler", "leer = eingebauter Recompiler"),
            ("runtime_dir", "Runtime-Projekt", "leer = mitgelieferte Runtime"),
        ]
        vars_ = {}
        for i, (k, label, hint) in enumerate(fields):
            ttk.Label(frm, text=label).grid(row=i, column=0, sticky="w", pady=4)
            v = tk.StringVar(value=settings.get(k, ""))
            vars_[k] = v
            ttk.Entry(frm, textvariable=v, width=44).grid(row=i, column=1, sticky="ew", padx=8, ipady=3)
            ttk.Label(frm, text=hint, style="Sub.TLabel").grid(row=i, column=2, sticky="w")
        frm.columnconfigure(1, weight=1)
        row = ttk.Frame(frm)
        row.grid(row=len(fields), column=0, columnspan=3, sticky="w", pady=(12, 0))
        ttk.Button(row, text="Nur analysieren", command=lambda: (win.destroy(), do_analyze())).pack(side="left")
        ttk.Button(row, text="Eigene Test-Runtime bauen", command=lambda: (win.destroy(), do_build())).pack(
            side="left", padx=8)

        def save():
            for k, v in vars_.items():
                settings[k] = v.get().strip()
            pipeline.save_settings(settings)
            win.destroy()
            refresh_buttons()
        ttk.Button(frm, text="Speichern", style="Accent.TButton", command=save).grid(
            row=len(fields) + 1, column=1, sticky="e", pady=12)

    def open_settings():
        """Spiel-Einstellungen: Grafik, Ton, Mikro, Steuerung."""
        win = tk.Toplevel(root)
        win.title("Einstellungen")
        win.configure(bg=BG)
        win.geometry("560x430")
        frm = ttk.Frame(win, padding=16)
        frm.pack(fill="both", expand=True)

        ttk.Label(frm, text="Grafik", style="Section.TLabel").grid(row=0, column=0, sticky="w")
        v_full = tk.BooleanVar(value=bool(settings.get("fullscreen")))
        v_vsync = tk.BooleanVar(value=bool(settings.get("vsync", True)))
        v_pad = tk.BooleanVar(value=bool(settings.get("gamepad_window")))
        v_mic = tk.BooleanVar(value=bool(settings.get("mic", True)))
        v_res = tk.StringVar(value=f"{settings.get('window_w', 1280)}x{settings.get('window_h', 720)}")
        v_vol = tk.IntVar(value=int(settings.get("volume", 60)))
        ttk.Checkbutton(frm, text="Vollbild", variable=v_full).grid(row=1, column=0, sticky="w")
        ttk.Checkbutton(frm, text="VSync (kein Bildreissen)", variable=v_vsync).grid(row=2, column=0, sticky="w")
        ttk.Checkbutton(frm, text="GamePad-Bild in eigenem Fenster", variable=v_pad).grid(row=3, column=0, sticky="w")
        ttk.Label(frm, text="Fenstergroesse").grid(row=4, column=0, sticky="w", pady=(6, 0))
        ttk.Combobox(frm, textvariable=v_res, width=12,
                     values=["1280x720", "1600x900", "1920x1080", "2560x1440"]).grid(row=4, column=1, sticky="w")
        ttk.Label(frm, text="Ton", style="Section.TLabel").grid(row=5, column=0, sticky="w", pady=(12, 0))
        ttk.Label(frm, text="Lautstaerke").grid(row=6, column=0, sticky="w")
        ttk.Scale(frm, from_=0, to=100, variable=v_vol, orient="horizontal").grid(row=6, column=1, sticky="ew")
        ttk.Checkbutton(frm, text="Mikrofon benutzen (GamePad-Mikro)", variable=v_mic).grid(
            row=7, column=0, columnspan=2, sticky="w")

        ttk.Label(frm, text="Steuerung", style="Section.TLabel").grid(row=8, column=0, sticky="w", pady=(12, 0))
        ttk.Label(frm, text="Im Spiel F1 druecken: Tasten und Controller-Knoepfe live umbelegen.\n"
                            "Controller werden automatisch erkannt (1. = GamePad, weitere = Wiimotes).",
                  style="Sub.TLabel", justify="left").grid(row=9, column=0, columnspan=2, sticky="w")
        frm.columnconfigure(1, weight=1)

        def save():
            settings["fullscreen"] = v_full.get()
            settings["vsync"] = v_vsync.get()
            settings["gamepad_window"] = v_pad.get()
            settings["mic"] = v_mic.get()
            settings["volume"] = int(v_vol.get())
            try:
                w, h = v_res.get().lower().split("x")
                settings["window_w"], settings["window_h"] = int(w), int(h)
            except ValueError:
                pass
            pipeline.save_settings(settings)
            log("Einstellungen gespeichert.", )
            win.destroy()
        bottom = ttk.Frame(frm)
        bottom.grid(row=10, column=0, columnspan=3, sticky="ew", pady=(16, 0))
        ttk.Button(bottom, text="Speichern", style="Accent.TButton", command=save).pack(side="right")

    def tile(key, text, cmd, icon, accent=False, w=170):
        t = GlassButton(bar, text, cmd, accent=accent, w=w, h=58, icon=icon)
        buttons[key] = t
        return t

    tile("cemu", "Spiel bauen", do_cemu, "\u2692").pack(side="left", padx=(0, 6))
    tile("shaders", "Shader vorbereiten", do_shaders, "\u2728", w=200).pack(side="left", padx=6)
    tile("stop", "Stopp", do_stop, "\u25A0", w=120).pack(side="left", padx=6)
    tile("adv", "Erweitert", open_advanced, "\u2699", w=140).pack(side="right", padx=(6, 0))
    play = GlassButton(bar, "SPIELEN", do_play, accent=True, w=210, h=58, icon="\u25B6",
                       font=("Segoe UI Black", 13))
    play.pack(side="right", padx=6)
    buttons["play"] = play

    def refresh_buttons():
        g = state["game"]
        ready = bool(g and g.rpx) and not state["busy"]
        for k in ("shaders", "cemu"):
            buttons[k].state(["!disabled"] if ready else ["disabled"])
        buttons["stop"].state(["!disabled"] if state["busy"] else ["disabled"])
        has_exe = bool(g and g.rpx and (pipeline.find_game_exe(settings) or pipeline.find_exe(settings, g)))
        buttons["play"].state(["!disabled"] if has_exe and not state["busy"] else ["disabled"])

    refresh_buttons()
    log("Wassap! Waehl oben deinen entpackten Nintendo-Land-Ordner aus.")
    log("Grafik, Ton und Steuerung stellst du im Spiel ein: Esc druecken.")
    if settings["base_dir"]:
        on_paths_changed()
    pump()
    root.mainloop()


if __name__ == "__main__":
    if "--cli" in sys.argv:
        run_cli()
    else:
        run_gui()
