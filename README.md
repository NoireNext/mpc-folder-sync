# MPC Folder Sync

Instrument-Plugin (Plugin-Programm, erscheint auf dem Hauptbildschirm unter „Plugins“) für Akai-MPC-Standalone-Geräte von Noirescripts. Es kopiert per Knopfdruck einen Ordner der MPC mit **rsync über SSH** auf einen Server oder ein NAS. Es erzeugt keinen Klang. **rsync und ein SSH-Client sind im Plugin enthalten** (statisch gebaut für arm32 und arm64), die MPC muss nichts mitbringen.

## Bedienung auf der MPC

Eine Seite **SYNC**:

| Element | Funktion |
|---|---|
| Target | Ziel 1–8 aus `foldersync.conf` (Q-Link 13 oder Antippen) |
| Start Sync | Taster: einmal drücken startet den Sync, leuchtet als „RUNNING“ solange er läuft |
| Status | nur Anzeige: Ready, Running, Done, Failed, No rsync, No ssh, No config, Bad target, Cannot run |
| Dry Run | An = Probelauf (`rsync -n`), es wird nichts kopiert |
| Name / Source / Destination | nur Text: gewähltes Ziel, Quellordner auf der MPC, Ziel als `user@host:ordner` |
| Progress | nur Text: Fortschritt des Syncs in Prozent (Gesamtfortschritt von rsync), nach dem Ende 100% |
| Job 1–8 | Liste aller Ziele aus der Konfiguration untereinander, das gewählte mit `>` markiert |

Ein geladenes Projekt startet nie von selbst einen Sync.

## Konfiguration

Plugins haben auf der MPC keine Texteingabe. Adresse, Ordner und Schlüssel stehen in:

`Noirescripts - VST - MPC Folder Sync/Content/foldersync.conf`

```
[Backup NAS]
host   = 192.168.1.20
user   = admin
port   = 22
source = /media/MPC-Projekte/
dest   = /volume1/backup/mpc/
key    = id_ed25519
delete = no
# rsync_path = /usr/syno/bin/rsync   (nur falls rsync auf dem Ziel nicht im Standard-PATH liegt)
exclude = *.tmp                       # beliebig oft; Muster, die nicht kopiert werden
exclude = .Trashes/
```

**Excludes:** pro Zeile ein Muster (rsync-Syntax: `*.tmp`, `Ordner/`, `Samples/Cache/`). Mit `delete = yes` werden ausgeschlossene Dateien auf dem Ziel nicht gelöscht. Der Prozentwert ist der Gesamtfortschritt; rsync zählt dazu vorher alle Dateien, bei sehr großen Ordnern dauert der Start daher etwas länger.

Die Datei enthält alle Felder erklärt. Den richtigen `source`-Pfad zeigt `Content/foldersync.log` nach dem ersten Start („Eingehängte Laufwerke“). Bitte die **IP-Adresse** eintragen (`.local`-Namen werden nicht aufgelöst).

## Anmeldung per Passwort (einfachster Weg)

In `foldersync.conf` statt `key` die Zeile `password = ...` eintragen (Rest der Zeile, Leerzeichen und Sonderzeichen erlaubt). Das Passwort geht nur per Umgebungsvariable an den SSH-Client, nicht in Befehlszeile oder Log. Ist zusätzlich `key` gesetzt, gilt der Schlüssel. Das Ziel muss Passwort-Anmeldung per SSH erlauben (bei Synology/QNAP üblich, bei Servern mit `PasswordAuthentication no` nicht). Das Passwort liegt im Klartext auf dem Laufwerk: ein eigenes Konto nur für den Sync verwenden.

## Anmeldung per SSH-Schlüssel (sicherer, einmalig am Rechner einrichten)

1. `ssh-keygen -t ed25519 -f id_ed25519 -N ""` (ohne Passwort).
2. Inhalt von `id_ed25519.pub` auf dem Ziel in `~/.ssh/authorized_keys` eintragen.
3. Die Datei `id_ed25519` (privat!) in den `Content`-Ordner des Plugins legen. Sie wird beim Sync automatisch für den mitgelieferten SSH-Client (Dropbear) umgewandelt.
4. Beim ersten Verbinden wird der Host-Schlüssel des Ziels gemerkt (`Content/home/.ssh/known_hosts`); ändert er sich später, wird die Verbindung abgelehnt.

Der private Schlüssel liegt im Klartext auf dem Laufwerk: nur für ein eigenes Konto verwenden, das möglichst nur den Zielordner schreiben darf. Auf dem Ziel müssen ein SSH-Server und **rsync** laufen.

## Was passiert beim Sync

`rsync -rtv --partial --modify-window=2 --timeout=30 --stats` (plus `--delete` bei `delete = yes`). Bewusst `-rt` statt `-a`, weil FAT/exFAT keine Rechte kennen. `source` mit `/` am Ende kopiert den Inhalt, ohne `/` den Ordner selbst. Die komplette Ausgabe steht in `Content/foldersync.log`.

Für die Dauer des Syncs werden rsync, SSH-Client und Schlüssel als Kopien nach `/tmp` gelegt (Laufwerke sind oft „noexec“, FAT meldet alle Dateien als offen) und danach gelöscht. Der Sync läuft in einem eigenen Thread, nie im Audio-Thread.

## Grenzen / nicht auf Hardware getestet

- Getestet: Build arm32/arm64/native, Validierung, Host-Simulator, Sync gegen einen lokalen SSH-Server mit den gebauten Programmen (Dry Run, Kopieren, Löschen, OpenSSH-Schlüssel, Fehlerfälle, Abbruch, Aufräumen). Die ARM-Programme selbst konnten hier nicht ausgeführt werden.
- Auf der MPC zu prüfen: ob `/tmp` ausführbar ist (sonst Status „Cannot run“), ob Status und Texte sofort neu gezeichnet werden und wie der Taster aussieht.
- Textanzeigen sind auf etwa 31 Zeichen begrenzt; bei langen Pfaden wird der Anfang durch `...` ersetzt.
- Plugins laufen ungesichert im MPC-Prozess: Projekt vor dem ersten Test speichern.
- Version 1.1 und neuer haben eine neue Plugin-ID (`N2fs`). Die alte Effekt-Version (1.0) vorher aus dem Ordner `VST2 Plugins` entfernen.

## Lizenz

Plugin-Code: MIT, siehe `LICENSE`. Nicht mit Akai Professional oder inMusic verbunden. „MPC“ und „Akai Professional“ sind Marken von inMusic Brands, Inc., „VST“ ist eine Marke von Steinberg Media Technologies GmbH.

Plugin-Code MIT. Mitgeliefert: rsync 3.4.1 (GPLv3+), Dropbear (MIT-Stil); Details und Quelltext-Hinweis in `Content/bin/THIRD_PARTY.txt`, Bauanleitung in `BUILDING-BINARIES.md`.

## Build

Das Repo enthält nur das Plugin. Zum Bauen wird das [MPC Plugin Builder SDK](https://github.com/inmusic-software/mpc-plugin-builder-sdk) (MIT, inMusic) gebraucht:
dieses Repo nach `<SDK>/plugins/MPC Folder Sync/` kopieren oder klonen.

```
python3 skin-src/make_skin.py      # erzeugt skins/ (Oberfläche)
python3 tools/build.py "plugins/MPC Folder Sync"
python3 tools/package.py "plugins/MPC Folder Sync"
```
