#!/usr/bin/env python3
"""Test-Sender: schickt Fake-Wiimote-Input an die Wii U (Plugin muss laufen).

Nutzung:  python sender.py <WIIU_IP>
Tasten:   a = A, b = B, 1 = Taste 1, 2 = Taste 2, + / - , Steuerkreuz per w (hoch) / s (runter) / x (links) / d (rechts)
          (Konsole: Taste eintippen + Enter, leere Eingabe = nichts gedrueckt)
"""
import socket
import struct
import sys
import threading
import time

PORT = 4405
MAGIC = 0x4E4C4E50  # 'NLNP'

# WPAD_BUTTON_* Werte aus wut (padscore/wpad.h)
BUTTONS = {
    "left": 0x0001, "right": 0x0002, "down": 0x0004, "up": 0x0008,
    "+": 0x0010, "2": 0x0100, "1": 0x0200, "b": 0x0400, "a": 0x0800,
    "-": 0x1000, "home": 0x8000,
}
KEYMAP = {"w": "up", "s": "down", "x": "left", "d": "right"}

hold = 0


def input_loop():
    global hold
    while True:
        line = input("> ").strip().lower()
        mask = 0
        for key in line.split():
            name = KEYMAP.get(key, key)
            mask |= BUTTONS.get(name, 0)
        hold = mask
        # Taste kurz halten, dann loslassen
        time.sleep(0.15)
        hold = 0


def main():
    if len(sys.argv) < 2:
        print("Nutzung: python sender.py <WIIU_IP>")
        sys.exit(1)
    target = (sys.argv[1], PORT)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    threading.Thread(target=input_loop, daemon=True).start()

    frame = 0
    while True:
        pkt = struct.pack(">IIIff", MAGIC, frame, hold, 0.0, 0.0)  # Wii U = Big Endian
        sock.sendto(pkt, target)
        frame += 1
        time.sleep(1 / 60)


if __name__ == "__main__":
    main()
