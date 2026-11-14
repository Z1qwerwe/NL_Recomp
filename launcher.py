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

BG, PANEL, FG, MUTED = "#16161d", "#1f1f29", "#e8e8ef", "#8b8ba0"
ACCENT, ACCENT_HI, OK, WARN, ERR = "#e4002b", "#ff2a4f", "#3ddc84", "#ffcc4d", "#ff6b6b"


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


def run_gui():
    import tkinter as tk
    from tkinter import filedialog, ttk

    settings = pipeline.load_settings()
    msgs = queue.Queue()
    state = {"busy": False, "stop": False, "game": None}

    root = tk.Tk()
    root.title("Nintendo Land Recomp")
    root.geometry("900x640")
    root.minsize(760, 520)
    root.configure(bg=BG)

    style = ttk.Style(root)
    style.theme_use("clam")
    style.configure("TFrame", background=BG)
    style.configure("Panel.TFrame", background=PANEL)
    style.configure("TLabel", background=BG, foreground=FG, font=("Segoe UI", 10))
    style.configure("Panel.TLabel", background=PANEL, foreground=FG)
    style.configure("Muted.TLabel", background=PANEL, foreground=MUTED, font=("Segoe UI", 9))
    style.configure("Title.TLabel", background=BG, foreground=FG, font=("Segoe UI Semibold", 20))
    style.configure("Sub.TLabel", background=BG, foreground=MUTED, font=("Segoe UI", 10))
    style.configure("TButton", background=PANEL, foreground=FG, borderwidth=0,
                    padding=(14, 8), font=("Segoe UI", 10))
    style.map("TButton", background=[("active", "#2c2c3a"), ("disabled", PANEL)],
              foreground=[("disabled", MUTED)])
    style.configure("Accent.TButton", background=ACCENT, foreground="white",
                    font=("Segoe UI Semibold", 11), padding=(22, 10))
    style.map("Accent.TButton", background=[("active", ACCENT_HI), ("disabled", "#5a2230")])
    style.configure("TEntry", fieldbackground="#11111a", foreground=FG, insertcolor=FG,
                    borderwidth=0)
    style.configure("Horizontal.TProgressbar", background=ACCENT, troughcolor=PANEL,
                    borderwidth=0, thickness=6)

    # --- Kopfzeile -------------------------------------------------------
    head = ttk.Frame(root, padding=(20, 16, 20, 4))
    head.pack(fill="x")
    title_var = tk.StringVar(value="Nintendo Land Recomp")
    sub_var = tk.StringVar(value="Waehl deinen entpackten Spielordner aus")
    ttk.Label(head, textvariable=title_var, style="Title.TLabel").pack(anchor="w")
    ttk.Label(head, textvariable=sub_var, style="Sub.TLabel").pack(anchor="w")

    # --- Ordner ----------------------------------------------------------
    paths = ttk.Frame(root, style="Panel.TFrame", padding=14)
    paths.pack(fill="x", padx=20, pady=10)
    path_vars = {}

    def path_row(r, label, key, hint):
        ttk.Label(paths, text=label, style="Panel.TLabel", width=10).grid(row=r * 2, column=0, sticky="w")
        v = tk.StringVar(value=settings.get(key, ""))
        path_vars[key] = v
        ttk.Entry(paths, textvariable=v).grid(row=r * 2, column=1, sticky="ew", padx=8, ipady=4)

        def browse():
            d = filedialog.askdirectory(initialdir=v.get() or os.path.expanduser("~/Documents"))
            if d:
                v.set(os.path.normpath(d))
                on_paths_changed()
        ttk.Button(paths, text="...", command=browse, width=3).grid(row=r * 2, column=2)
        ttk.Label(paths, text=hint, style="Muted.TLabel").grid(row=r * 2 + 1, column=1, sticky="w",
                                                               padx=8, pady=(0, 6))

    path_row(0, "Spiel", "base_dir", "Ordner mit code/, content/ und meta/")
    path_row(1, "Update", "update_dir", "optional, ebenfalls entpackt")
    paths.columnconfigure(1, weight=1)

    # --- Buttons ---------------------------------------------------------
    bar = ttk.Frame(root, padding=(20, 0))
    bar.pack(fill="x")
    buttons = {}

    def add_btn(key, text, cmd, accent=False):
        b = ttk.Button(bar, text=text, command=cmd, style="Accent.TButton" if accent else "TButton")
        b.pack(side="left", padx=(0, 8))
        buttons[key] = b

    progress = ttk.Progressbar(root, mode="determinate", maximum=1.0)
    progress.pack(fill="x", padx=20, pady=(12, 0))

    # --- Log -------------------------------------------------------------
    logbox = tk.Text(root, bg="#0e0e14", fg=FG, insertbackground=FG, relief="flat",
                     font=("Cascadia Mono", 9), padx=10, pady=8, wrap="word")
    logbox.pack(fill="both", expand=True, padx=20, pady=(8, 20))
    for tag, col in (("warn", WARN), ("err", ERR), ("ok", OK), ("cmd", MUTED)):
        logbox.tag_configure(tag, foreground=col)

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

    def do_play():
        worker(lambda: pipeline.step_launch(settings, state["game"], log))

    def do_stop():
        state["stop"] = True
        log("Wird abgebrochen...")

    def open_settings():
        win = tk.Toplevel(root)
        win.title("Einstellungen")
        win.configure(bg=BG)
        win.geometry("640x300")
        frm = ttk.Frame(win, padding=16)
        frm.pack(fill="both", expand=True)
        fields = [
            ("work_dir", "Arbeitsordner", "Hier landen Analyse, Shader-Cache und Build"),
            ("recompiler_cmd", "Recompiler", "leer = eingebauter Recompiler"),
            ("shader_translator", "Shader-Translator", ".exe der Runtime, die GX2 -> SPIR-V macht"),
            ("runtime_dir", "Runtime-Projekt", "leer = mitgelieferte Runtime"),
            ("exe_name", "Exe-Name", "Name der fertigen .exe"),
        ]
        vars_ = {}
        for i, (k, label, hint) in enumerate(fields):
            ttk.Label(frm, text=label).grid(row=i, column=0, sticky="w", pady=4)
            v = tk.StringVar(value=settings.get(k, ""))
            vars_[k] = v
            ttk.Entry(frm, textvariable=v, width=48).grid(row=i, column=1, sticky="ew", padx=8, ipady=3)
            ttk.Label(frm, text=hint, style="Sub.TLabel").grid(row=i, column=2, sticky="w")
        frm.columnconfigure(1, weight=1)

        def save():
            for k, v in vars_.items():
                settings[k] = v.get().strip()
            pipeline.save_settings(settings)
            win.destroy()
            refresh_buttons()
        ttk.Button(frm, text="Speichern", style="Accent.TButton", command=save).grid(
            row=len(fields), column=1, sticky="e", pady=12)

    add_btn("analyze", "Analysieren", do_analyze)
    add_btn("shaders", "Shader vorkompilieren", do_shaders)
    add_btn("build", "Recompilen + Bauen", do_build)
    add_btn("stop", "Stopp", do_stop)
    ttk.Button(bar, text="Einstellungen", command=open_settings).pack(side="right")
    play = ttk.Button(bar, text="SPIELEN", style="Accent.TButton", command=do_play)
    play.pack(side="right", padx=8)
    buttons["play"] = play

    def refresh_buttons():
        g = state["game"]
        ready = bool(g and g.rpx) and not state["busy"]
        for k in ("analyze", "shaders", "build"):
            buttons[k].state(["!disabled"] if ready else ["disabled"])
        buttons["stop"].state(["!disabled"] if state["busy"] else ["disabled"])
        has_exe = bool(g and g.rpx and pipeline.find_exe(settings, g))
        buttons["play"].state(["!disabled"] if has_exe and not state["busy"] else ["disabled"])

    refresh_buttons()
    log("Wassap! Waehl oben deinen entpackten Nintendo-Land-Ordner aus.")
    if settings["base_dir"]:
        on_paths_changed()
    pump()
    root.mainloop()


if __name__ == "__main__":
    if "--cli" in sys.argv:
        run_cli()
    else:
        run_gui()
