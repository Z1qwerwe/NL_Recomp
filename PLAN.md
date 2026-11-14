# Plan fuer v0.2 (Stand 15.11.2026)

Logo: `assets/` (logo.png, logo_512.png, icon_256.png, nl_icon.ico)

## 1. Bugs (zuerst)

### Stream ruckelt, Ton hackt, Bild friert ein
Hauptursache: Steam-Networking sendet standardmaessig max. **256 KB/s (~2 Mbit/s)**, wir schicken ~7,5 Mbit/s.
Der Sendepuffer (512 KB) laeuft voll, unzuverlaessige Pakete fliegen weg -> Luecken, Keyframe-Schleife, Ton-Aussetzer.
- `SteamNetworkingUtils SetGlobalConfigValueInt32`: SendRateMin/Max auf ~4 MB/s, SendBufferSize 4 MB
- Video in eigene Stuecke <= 1100 Byte mit Rahmen-Nummer + Teil-Index; Bild nur dekodieren, wenn komplett
- Keyframe alle 1 s (GOP) + Intra-Refresh; nach Paketverlust Keyframe anfordern (Host-Limit 250 ms bleibt)
- Bitrate automatisch an Verbindung anpassen (Ping/Qualitaet aus GetSessionConnectionInfo)
- Ton: Jitter-Puffer (~60 ms) im Client, gleichmaessig abspielen statt "verwerfen wenn > 8"; spaeter Opus statt PCM (1,5 Mbit/s -> 0,1)

### Wiimote beim Gast nur mit Klicken, nicht mit Mausbewegung
Client schickt keinen Zeiger. Neu: Protokoll v2 mit Zeiger-X/Y (Maus ueber dem Bild, sonst rechter Stick/Gyro),
Host-Wiimote nimmt den Zeiger direkt.

### "Stream passiert mehrmals"
Noch klaeren (siehe Fragen). Verdacht: HELLO wird jede Sekunde wiederholt / mehrere Bild-Abgriffe pro Frame. Mit Logs pruefen.

## 2. Schnelle Features
- **Logo** ueberall: Launcher-Kopf + Fenster-Icon, NintendoLand.exe / NLOnline.exe Icon, In-Game-Menue, NLOnline-Startbild, README
- **"Nintendo Land" -> "NL"** in Launcher und NLOnline (Titel, Texte, Fenstername)
- **Englisch**: alle Texte (Launcher, Menue, NLOnline) ueber eine Texttabelle, Sprache waehlbar (Deutsch/English, Standard = Windows-Sprache)
- **Live-Chat**: T oder Enter oeffnet Eingabe, Nachrichten zuverlaessig ueber Steam, Host verteilt an alle, Anzeige oben links 8 s

## 3. Groessere Features
- **Echte Wiimotes per Bluetooth**: Cemu kann echte Wiimotes (InputAPI::Wiimote, Windows-Bluetooth-Kopplung oder DolphinBar).
  Beim Host: automatisch als Spieler 2-5 einhaengen (mit Cemus fertiger Belegung). Beim Gast (NLOnline): Wiimote-Lesen per HID
  einbauen und Tasten/Zeiger/Bewegung mitschicken - zweiter Schritt.
- **Miis**: Recherche, ob/wie man Miis im Launcher erstellt/importiert (Mii-Datenbank der Konsole im Spielstand-Ordner,
  Formate .cfsd/.ffsd/QR). Ziel: jeder Online-Spieler bringt seinen Mii mit.

## 4. Schwer / Forschung
- **Spieler in der Plaza sehen und bewegen**: braucht Eingriffe ins Spiel (Mii-Figuren in der Plaza steuern). Erst nach "echtem Online" sinnvoll.
- **Echtes Online (jeder hat das Spiel, mehrere Clients)**: einziger realistischer Weg = Lockstep-Netplay wie Dolphin
  (Determinismus-Arbeit von vorher: Zaehler identisch, offen sind kleine Zyklen-Abweichung + Stack Overflow im Testmodus).
  Viel Arbeit, Ergebnis unsicher. Empfehlung: erst Stream gut machen, dann Lockstep als Experiment.

## Reihenfolge morgen
1. Stream-Fixes (Senderate, Pakete, Ton, Zeiger) -> zusammen testen
2. Logo, "NL", Englisch, Chat
3. Echte Wiimotes beim Host
4. Mii-Recherche, dann entscheiden
