"""Shader-Vorkompilierung.

Schritt 1 (laeuft schon): Alle GX2-Shader aus den Spieldateien holen, auch aus
verschachtelten .szs/.sarc/.pack-Archiven, Duplikate per Hash rauswerfen und in
einen Cache-Ordner schreiben.

Schritt 2 (braucht den Shader-Translator der Runtime): Jeden Shader nach SPIR-V
uebersetzen, damit die Runtime beim Spielen nichts mehr kompilieren muss.
Solange es den Translator noch nicht gibt, bleiben die Shader als "pending" im
Manifest stehen.
"""
import hashlib
import json
import os
import subprocess

from .archives import walk

# Dateien, in denen keine Shader stecken (Audio, Sprachausgabe)
SKIP_EXT = {".bfstm", ".bfsar", ".bfwav", ".txt"}
SKIP_DIRS = {".svn", "Voice", "Sound"}
ARCHIVE_EXT = {".szs", ".sarc", ".pack", ".gsh", ".sharcfb", ".sharc"}

# Magics der Shader-Formate
SHADER_MAGICS = {
    b"Gfx2": "gsh",       # GX2-Shader (.gsh) / Texturen (.gtx) -> Endung pruefen
    b"BAHS": "sharcfb",   # AGL-Shader-Archiv, kompiliert (enthaelt GX2-Programme)
    b"AAHS": "sharc",     # AGL-Shader-Archiv, Quelltext
}


def _iter_source_files(roots):
    """Liefert (relativer Pfad, voller Pfad). Update-Dateien ueberschreiben Base."""
    seen = set()
    for root in roots:
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
            for fn in filenames:
                ext = os.path.splitext(fn)[1].lower()
                if ext in SKIP_EXT or ext not in ARCHIVE_EXT:
                    continue
                full = os.path.join(dirpath, fn)
                rel = os.path.relpath(full, root).replace("\\", "/")
                if rel in seen:
                    continue
                seen.add(rel)
                yield rel, full


def extract_shaders(roots, cache_dir, log=print, progress=lambda f: None, stop=lambda: False):
    """Sammelt alle Shader in cache_dir/raw/. Gibt das Manifest zurueck."""
    raw_dir = os.path.join(cache_dir, "raw")
    os.makedirs(raw_dir, exist_ok=True)
    manifest_path = os.path.join(cache_dir, "manifest.json")
    manifest = {"files": {}, "shaders": {}}
    if os.path.isfile(manifest_path):
        manifest = json.load(open(manifest_path))

    sources = list(_iter_source_files(roots))
    for i, (rel, full) in enumerate(sources):
        if stop():
            log("Abgebrochen.")
            break
        progress(i / max(1, len(sources)))
        st = os.stat(full)
        stamp = f"{st.st_size}:{int(st.st_mtime)}"
        if manifest["files"].get(rel) == stamp:
            continue  # schon gescannt, Datei unveraendert
        found = 0
        try:
            for inner, data in walk(open(full, "rb").read(), rel):
                kind = SHADER_MAGICS.get(data[:4])
                if not kind or inner.lower().endswith(".gtx"):
                    continue
                h = hashlib.sha1(data).hexdigest()
                entry = manifest["shaders"].setdefault(h, {
                    "kind": kind, "size": len(data), "sources": [], "status": "pending"})
                if inner not in entry["sources"]:
                    entry["sources"].append(inner)
                out = os.path.join(raw_dir, f"{h}.{kind}")
                if not os.path.exists(out):
                    with open(out, "wb") as f:
                        f.write(data)
                found += 1
        except Exception as e:  # kaputtes oder unbekanntes Archiv
            log(f"  ! {rel}: {e}")
        if found:
            log(f"  {rel}: {found} Shader")
        manifest["files"][rel] = stamp

    progress(1.0)
    json.dump(manifest, open(manifest_path, "w"), indent=1)
    uniq = len(manifest["shaders"])
    log(f"Shader-Cache: {uniq} einzigartige Shader-Dateien in {raw_dir}")
    return manifest


def translate_shaders(cache_dir, translator, log=print, progress=lambda f: None, stop=lambda: False):
    """Ruft den Shader-Translator fuer jeden noch nicht uebersetzten Shader auf.

    Erwartete Schnittstelle des Translators (kommt mit der Runtime):
        translator <eingabe.gsh|.sharcfb> <ausgabe-ordner>
    und schreibt dort .spv-Dateien (eine pro Shader-Programm/Variante).
    """
    manifest_path = os.path.join(cache_dir, "manifest.json")
    manifest = json.load(open(manifest_path))
    if not translator or not os.path.isfile(translator):
        pending = sum(1 for s in manifest["shaders"].values() if s["status"] != "done")
        log(f"Kein Shader-Translator eingestellt. {pending} Shader bleiben 'pending' "
            "und werden uebersetzt, sobald die Runtime ihren Translator hat.")
        return manifest

    spv_dir = os.path.join(cache_dir, "spirv")
    os.makedirs(spv_dir, exist_ok=True)
    todo = [(h, s) for h, s in manifest["shaders"].items() if s["status"] != "done"]
    for i, (h, s) in enumerate(todo):
        if stop():
            break
        progress(i / max(1, len(todo)))
        src = os.path.join(cache_dir, "raw", f"{h}.{s['kind']}")
        out = os.path.join(spv_dir, h)
        os.makedirs(out, exist_ok=True)
        r = subprocess.run([translator, src, out], capture_output=True, text=True)
        s["status"] = "done" if r.returncode == 0 else "error"
        if r.returncode != 0:
            log(f"  ! {h[:12]} ({s['sources'][0]}): {r.stderr.strip()[:200]}")
    progress(1.0)
    json.dump(manifest, open(manifest_path, "w"), indent=1)
    done = sum(1 for s in manifest["shaders"].values() if s["status"] == "done")
    log(f"Shader uebersetzt: {done}/{len(manifest['shaders'])}")
    return manifest
