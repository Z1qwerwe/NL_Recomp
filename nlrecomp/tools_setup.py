"""Bau-Werkzeuge pruefen und bei Bedarf per winget installieren (Git, Visual Studio 2022 Build Tools mit C++ und CMake)."""
import glob
import os
import shutil
import subprocess

VSWHERE = os.path.join(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"),
                       "Microsoft Visual Studio", "Installer", "vswhere.exe")

GIT_CMD = ["winget", "install", "--id", "Git.Git", "-e", "--silent",
           "--accept-package-agreements", "--accept-source-agreements"]
VS_CMD = ["winget", "install", "--id", "Microsoft.VisualStudio.2022.BuildTools", "-e",
          "--accept-package-agreements", "--accept-source-agreements", "--override",
          "--passive --wait --norestart "
          "--add Microsoft.VisualStudio.Workload.VCTools "
          "--add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 "
          "--add Microsoft.VisualStudio.Component.VC.CMake.Project "
          "--add Microsoft.VisualStudio.Component.Windows11SDK.22621 "
          "--includeRecommended"]


def has_msvc():
    """Visual Studio 2022 (oder Build Tools) mit C++-Compiler vorhanden?"""
    if not os.path.isfile(VSWHERE):
        return False
    try:
        out = subprocess.run([VSWHERE, "-latest", "-products", "*", "-version", "[17.0,18.0)",
                              "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                              "-property", "installationPath"],
                             capture_output=True, text=True, timeout=30).stdout.strip()
    except Exception:
        return False
    return bool(out)


def missing_tools():
    """Liste der fehlenden Werkzeuge: [(schluessel, anzeigename, groesse)]"""
    from . import cemu, pipeline
    missing = []
    if not cemu.find_git():
        missing.append(("git", "Git", "ca. 300 MB"))
    if not has_msvc() or not pipeline._find_cmake():
        missing.append(("vs", "Visual Studio 2022 Build Tools (C++ und CMake)", "ca. 6-8 GB"))
    return missing


def winget_available():
    return shutil.which("winget") is not None


def install(missing, log, stop=lambda: False):
    """Fehlende Werkzeuge installieren. Windows fragt dabei einmal nach Administratorrechten."""
    for key, name, size in missing:
        if stop():
            return
        cmd = GIT_CMD if key == "git" else VS_CMD
        log(f"Installiere {name} ({size}) - das kann eine Weile dauern ...")
        log("> " + " ".join(cmd[:4]))
        p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             text=True, encoding="utf-8", errors="replace")
        for line in p.stdout:
            line = line.strip()
            # winget malt Fortschrittsbalken mit Steuerzeichen - nur lesbare Zeilen zeigen
            if line and not set(line) <= set("-\\|/ ") and "█" not in line and "▒" not in line:
                log("  " + line)
        code = p.wait()
        if code != 0:
            log(f"WARNUNG: winget meldet Code {code} fuer {name} - pruefe, ob es trotzdem da ist ...")
    # Neu installiertes Git ist im laufenden Launcher noch nicht im PATH
    for p in (r"C:\Program Files\Git\cmd", r"C:\Program Files (x86)\Git\cmd"):
        if os.path.isfile(os.path.join(p, "git.exe")) and p not in os.environ.get("PATH", ""):
            os.environ["PATH"] = p + os.pathsep + os.environ.get("PATH", "")
    still = missing_tools()
    if still:
        names = ", ".join(n for _, n, _ in still)
        raise RuntimeError(f"Immer noch nicht gefunden: {names}. Bitte einmal den PC neu starten und es nochmal versuchen.")
    log("Fertig: alle Bau-Werkzeuge sind da.")
