"""Erkennt Wii-U-Spielordner (Base + Update) und liest die Metadaten."""
from __future__ import annotations
import os
import re
from dataclasses import dataclass, field


@dataclass
class GameDir:
    path: str
    kind: str              # "decrypted", "encrypted" oder "unknown"
    title_id: str = ""
    version: int = 0
    name: str = ""
    rpx: str = ""          # Pfad zur Haupt-.rpx (falls entschluesselt)

    @property
    def is_update(self):
        return self.title_id.lower().startswith("0005000e")


@dataclass
class Game:
    base: GameDir
    update: GameDir | None = None
    warnings: list = field(default_factory=list)

    @property
    def rpx(self):
        """Die Update-.rpx gewinnt, wenn ein entschluesseltes Update da ist."""
        if self.update and self.update.kind == "decrypted" and self.update.rpx:
            return self.update.rpx
        return self.base.rpx

    def content_roots(self):
        """Update zuerst: Dateien im Update ueberschreiben die aus dem Base-Spiel."""
        roots = []
        if self.update and self.update.kind == "decrypted":
            roots.append(os.path.join(self.update.path, "content"))
        roots.append(os.path.join(self.base.path, "content"))
        return [r for r in roots if os.path.isdir(r)]


def _xml_value(text, tag):
    m = re.search(rf"<{tag}[^>]*>([^<]*)</{tag}>", text)
    return m.group(1).strip() if m else ""


def inspect_dir(path) -> GameDir:
    if os.path.isfile(os.path.join(path, "title.tmd")):
        g = GameDir(path, "encrypted")
        m = re.search(r"\[([0-9a-fA-F]{16})\]", os.path.basename(path))
        if m:
            g.title_id = m.group(1).lower()
        g.name = re.sub(r"\s*\[.*$", "", os.path.basename(path))
        return g

    code = os.path.join(path, "code")
    if not os.path.isdir(code):
        return GameDir(path, "unknown")

    g = GameDir(path, "decrypted")
    meta = os.path.join(path, "meta", "meta.xml")
    app = os.path.join(code, "app.xml")
    cos = os.path.join(code, "cos.xml")
    for f in (meta, app):
        if os.path.isfile(f):
            t = open(f, encoding="utf-8", errors="replace").read()
            g.title_id = g.title_id or _xml_value(t, "title_id").lower()
            v = _xml_value(t, "title_version")
            if v:
                g.version = int(v, 16) if f == app else int(v)
            g.name = g.name or _xml_value(t, "longname_de") or _xml_value(t, "longname_en")
    # Haupt-RPX steht in cos.xml -> argstr
    rpx_name = ""
    if os.path.isfile(cos):
        rpx_name = _xml_value(open(cos, encoding="utf-8", errors="replace").read(), "argstr")
    if not rpx_name:
        rpxs = [f for f in os.listdir(code) if f.lower().endswith(".rpx")]
        rpx_name = rpxs[0] if rpxs else ""
    if rpx_name and os.path.isfile(os.path.join(code, rpx_name)):
        g.rpx = os.path.join(code, rpx_name)
    return g


def find_game(base_path, update_path=None) -> Game:
    base = inspect_dir(base_path)
    update = inspect_dir(update_path) if update_path else None
    game = Game(base, update)

    if base.kind == "encrypted":
        game.warnings.append("Das Base-Spiel ist verschluesselt (title.tmd/.app). "
                             "Dump es mit dumpling von deiner Wii U, dann ist es entpackt.")
    elif base.kind == "unknown":
        game.warnings.append("Im Base-Ordner gibt's keinen code/-Ordner. Falscher Ordner?")
    if update:
        if update.kind == "encrypted":
            game.warnings.append("Das Update ist verschluesselt und wird ignoriert. "
                                 "Dump es auch mit dumpling.")
        elif update.kind == "decrypted" and base.title_id and update.title_id \
                and base.title_id[-8:] != update.title_id[-8:]:
            game.warnings.append(f"Update ({update.title_id}) passt nicht zum Spiel ({base.title_id}).")
    return game
