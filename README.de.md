<p align="center">
  <img src="assets/liu-bunny.png" width="92" height="128" alt="Liu Terminal">
</p>

<h1 align="center">Liu Terminal</h1>

<p align="center">
  <strong>Ein Terminal für jeden KI-Agenten.</strong><br>
  Liu steht zwischen dir und einer Vielzahl von Coding-Agenten und lässt zehn parallele Threads wie einen Ort wirken.<br>
  Geschrieben in <strong>C</strong>, nativ auf macOS, GPU-beschleunigt.
</p>

<p align="center">
  <a href="https://liu.software">Download</a>
  ·
  <a href="https://github.com/calculusteam/liu/releases">Releases</a>
  ·
  <a href="https://liu.software/docs">Dokumentation</a>
  ·
  <a href="https://github.com/calculusteam/liu">GitHub</a>
</p>

<p align="center">
  <strong>Sprachen:</strong>
  <a href="README.md">English</a>
  ·
  <a href="README.tr.md">Türkçe</a>
  ·
  <a href="README.zh.md">简体中文</a>
  ·
  <a href="README.ko.md">한국어</a>
  ·
  <a href="README.es.md">Español</a>
  ·
  <a href="README.de.md">Deutsch</a>
  ·
  <a href="README.it.md">Italiano</a>
  ·
  <a href="README.fr.md">Français</a>
  ·
  <a href="README.ja.md">日本語</a>
  ·
  <a href="README.ar.md">العربية</a>
</p>

---

## Funktionen

### Agenten-Historie

Drücke Cmd+K, um die Befehlspalette zu öffnen, und tippe "agent history", um jede installierte KI-CLI zu durchsuchen: Claude Code, Codex, Copilot, OpenCode, Qwen, Cline, Antigravity. Sitzungen nativ fortsetzen oder Transkripte scrollen, ohne den Agenten zu starten.

### Agenten-Ansicht

Liu verfolgt den Agenten in jedem geteilten Bereich, nicht nur in jedem Tab. Ein Tab benennt sich nach dem Chat des Agenten, trägt ein Logo je Agenten-Bereich und lässt einen Aktivitätsbalken laufen, während Ausgabe entsteht. Ruht der Zeiger darauf, listet ein Panel jeden Bereich: Agent, Chat-Thema und eine Pille mit `working 1m 12s` oder `idle 40s`. Beendet ein Agent einen Arbeitsabschnitt, fällt unter dem Tab eine Karte ein und der Abschlusston erklingt.

### Agenten starten Agenten

Ein Agent in einem Bereich kann über Liu Arbeit an einen anderen abgeben: `liu-agent start codex "port the tests"`. Liu fragt dich zuerst und führt ihn dann als kleinen Job-Tab aus, rechts an den anfragenden Tab angefügt: sein Logo über einem bewegten Dunst, der seinen Zustand zeigt. Ruht der Zeiger darauf, siehst du seine neuesten Zeilen; ein Klick öffnet seinen Chat, in dem du die nächste Runde sendest, ihn anhältst oder seine Sitzung in einem Bereich fortsetzt. Gemeinsam gestartete Agenten arbeiten als Team und erfahren es, wenn zwei dieselbe Datei anfassen.

### Subagenten-Tabs

Startet ein Agent eigene Subagenten (das Agent-Tool von Claude Code, Codex, OpenCode, Copilot CLI, Cursor, Qwen Code, Droid, Crush und mehr), wird jeder zu einem Job-Tab neben dem Tab des Agenten, live aus dessen eigenen Aufzeichnungen gelesen. Fünf Subagenten, fünf Tabs, jeder Dunst ein eigenes Bild. Ruht der Zeiger auf einem, siehst du seine neuesten Zeilen, ein Klick zeigt das ganze Protokoll; am Ende steht jeder auf erledigt oder fehlgeschlagen und verschwindet mit deinem nächsten Prompt.

### Sprach-Prompt

Drücke **⌘⇧M** und sprich. Whisper large-v3-turbo läuft auf diesem Mac (kein Netz, kein Konto, nichts wird hochgeladen) und die Wörter werden in den Bereich getippt, der den Fokus hat: in Claude Code, in einer normalen Shell, in einem REPL. Eine Live-Wellenform zeigt, was das Mikrofon tatsächlich hört; beim Stoppen friert die Spur ein und ein Scan läuft darüber, während das Audio transkribiert wird. Mikrofon wählen, aus allen 100 Sprachen wählen, die Whisper kennt, oder stattdessen eine gehostete Sprach-API ansteuern.

### Skizze

Drücke **⌘K** und wähle *Sketch*: Über dem Terminal öffnet sich eine Zeichenfläche; Stift, Linie, Pfeil, Rechteck und Radierer, 14 Farben und ein Regler für die Strichstärke. **P** **L** **A** **R** **E** wechseln das Werkzeug, **⌘Z** macht rückgängig. **Esc** oder **Return** macht aus der Zeichnung ein PNG und legt es in die Zwischenablage, bereit zum Einfügen in einen Agenten, der Bilder liest; zeige ein Layout, statt es zu beschreiben. **⌥+Esc** schließt ohne Kopieren.

### KI-Themen-Erstellung

Beschreiben Sie ein Aussehen, ein lokaler Agent gibt JSON aus, Liu wendet es live an. Gespeichert in `~/.config/Liu/themes/`. Der Agent bekommt Lius eigenen Theme-Leitfaden und füllt alle 25 Farbfelder. Angeboten werden nur Agenten aus deinem PATH; das Ergebnis erscheint neben den 19 integrierten Themes unter Einstellungen → Appearance.

### Markdown-Leser & -Editor

Der Datei-Browser öffnet READMEs, Designs und Agenten-Notizen: gestylte Markdown-Vorschau oder Raw-Bearbeitung im Fenster. Bilder, Code und Links im Lesemodus. Codeblöcke zeigen ihre Sprache und einen Kopieren-Button, und `![[note]]` bettet eine andere Notiz direkt ein.

### Code-Editor

Drücke **⌘K** und wähle *Code Editor*: Der Tab wird zu einer kleinen IDE; Explorer-Leiste links, die Datei rechts, ein Dateityp-Symbol auf jeder Zeile und jedem Tab. Ein tabellengesteuerter Highlighter deckt über 50 Sprachen ab, und das Script in einer Vue-, Svelte- oder Astro-Komponente wird vom selben Code gelext wie eine .js-Datei. **⌘F** sucht, **⌘⇧F** ersetzt, **⌘S** speichert; die Statusleiste zeigt Zeile/Spalte, Sprache, Zeilenende und Kodierung.

### Graph-Ansicht

Ein Wissensgraph jeder Markdown-Datei in einem Ordner: Knoten sind Dateien, Kanten sind `[[Wikilinks]]` und Einbettungen. Ein kräftegesteuertes Layout ordnet sie an; beim Überfahren einer Notiz leuchten deren Nachbarn auf, ziehen zum Anheften, klicken zum Öffnen.

### Notizen-Vault

Jede Notiz lebt in einem Vault. **⌘K** → Notiz erstellen lässt einen lokalen Agenten Markdown direkt hineinschreiben, `[[Wikilinks]]` verbinden sie und speisen den Graphen, und Notiz öffnen / Notizen durchsuchen springt überallhin. Terminal-Agenten erben `$LIU_VAULT` und schreiben ebenfalls dorthin: **⌘⇧A** gibt jede Notiz als `@source` zurück. Eine Wissensbasis, geteilt von Ihnen und Ihren Agenten.

### Integriertes Benachrichtigungssystem

`liu-notify` meldet sich nur, wenn ein Agent Sie wirklich braucht, und nur dann: per Sprachhinweis, Desktop-Banner oder einem eigenen Sound. Der Dienst läuft in Liu: Beim Start erhalten Ihre Agenten die Hooks, beim Beenden von Liu werden sie entfernt, Agenten, die Sie anderswo ausführen, sind also nicht betroffen.

### Token-Einsparung

Drücken Sie zweimal **Ctrl+⌘** im Prompt: das lokale Metal-beschleunigte LLM (oder der gewählte Agent) übersetzt Nicht-Englisch ins Englische, *bevor* die Anfrage Ihr Gerät verlässt. Das Kontextfenster schrumpft, API-Kosten sinken drastisch.

### Eingebauter SSH

Vollständiger SSH-Client hinter einer Session: Live-Datei-Upload/-Download, Remote-Dateibrowser, Sitzungsmenü, Port-Weiterleitung (-L / -D), Passwort-/Schlüssel-/Agent-Auth, Known-Hosts TOFU, Mosh-Reconnect und Verlauf.

### Befehlspalette

Ein einziges **⌘K** durchsucht jeden Befehl, jede Einstellung, jeden Host, jedes Snippet, jedes Theme und jeden Agent-History-Eintrag: keine Menüsuche, keine Muskelerinnerung nötig. Sie wechselt auch ihre Form: `Open Note` ist ein Notiz-Schnellwechsler, `Search Notes` durchsucht deinen Vault im Volltext und `Go to Heading` springt durch die Gliederung des geöffneten Dokuments.

### Splits + Tabs

Bis zu 32 Tabs, jeder mit bis zu 8 Panes in einem H/V-Split-Baum. Pane-Drag-and-Drop. **⌘⇧I** sendet Eingaben gleichzeitig an alle aktiven Panes. Zieh einen Tab heraus, um ein Fenster zu erzeugen, oder auf die Tab-Leiste eines anderen Fensters, um ihm beizutreten. Nach einem Neustart öffnet Liu deine Tabs wieder.

### Weiter, wo du aufgehört hast

Liu speichert deine Tabs, während es läuft, sodass nach Beenden, Absturz oder Neustart dieselben Fenster, Tabs, Teilungen und Ordner zurückkommen, jeder Bereich mit seinem früheren Bildschirm über einer gedämpften `Restored session`-Zeile. Ein Agent, der lief, wird von der Shell auf seiner eigenen Sitzung neu gestartet (Claude Code, Codex, OpenCode und jeder Agent mit Fortsetzen-Option), ohne dass etwas hineingetippt wird.

### Datei-Browser

Sidebar lokal + SFTP-Browser mit Ein-Klick-Öffnen. NSFilePromise zieht und überträgt Dateien im Hintergrund zum Finder. 32 MiB LRU-Bild-Cache. Er öffnet sich im Arbeitsverzeichnis des fokussierten Bereichs. Ziehst du eine Finder-Datei auf einen Bereich, wird ihr Pfad in den Prompt geschrieben oder per SFTP hochgeladen, wenn der Bereich eine SSH-Sitzung ist.

### Tab-Schlafmodus

Tabs, die N Minuten inaktiv sind, werden automatisch ausgesetzt: CPU und RAM sinken nahezu auf null. Klicken Sie, um sofort genau dort fortzufahren, wo Sie aufgehört haben, mit intaktem Scrollback. Nur ein Tab, der am Prompt wartet, schläft ein. Ein Build, ein Dev-Server oder ein Agent mitten in einem Zug hält ihn wach; im Schlaf wird der Scrollback auf die Festplatte gesichert.

### Tab-Gruppen

Bündeln Sie zusammengehörige Terminals in benannte, farbcodierte Gruppen direkt in der Tab-Leiste, im Chrome-Stil. Erstellen Sie eine Gruppe über das Rechtsklickmenü eines Tabs oder die Befehlspalette, und ziehen Sie Tabs hinein oder heraus. Klappen Sie eine Gruppe zu, um ihre Tabs auszublenden und in den Schlafmodus zu versetzen, wodurch deren PTY/SSH-Verbindungen freigegeben werden. Umfärben aus einer 8-Farben-Palette, inline umbenennen, Gruppierung aufheben oder eine ganze Gruppe auf einmal schließen. Benannte, farbige Gruppen überstehen Neustarts: bis zu 8 gleichzeitig.

### Site-Manager

Registrieren Sie lokale Projektordner als Sites, starten und stoppen Sie deren Dev-Server in einem Panel und beobachten Sie Live-Prozesslogs: Local-URL, HMR und Status auf einen Blick. Browser öffnen, neu starten oder Site entfernen, ohne das Terminal zu verlassen.

### Liu Browser

Aktiviere den Element-Picker im integrierten Browser von Liu und klicke auf ein beliebiges Element einer Live-Seite: einen Button, eine Karte, eine verrutschte Zeile. Liu erfasst dessen CSS-Selektor, Tag, Text, Größe und das vollständige HTML und fügt es als Kontext-Chip inline in den Prompt deines Agenten ein. Keine Screenshots, kein Copy-Paste: Zeige auf den exakten Knoten, und Claude, Codex oder jede CLI sieht genau das, was du siehst. Ideal für "korrigiere das", "gestalte das um" oder "warum ist das nicht zentriert?".

---

## Unterstützte Plattformen

| Plattform | Architektur | Format |
| :--- | :--- | :--- |
| **macOS** | Apple Silicon (arm64) | `.dmg` |
| **macOS** | Intel (x86_64) | `.dmg` |

macOS ist das primäre und derzeit einzige unterstützte Ziel.

## Unser Ziel

- [ ] **Linux-Unterstützung**, ARM64- und x86_64-Builds (`.tar.gz`), apt (PPA), dnf (COPR) und AUR. Linux ist geplant, aber noch nicht unterstützt.

---

## Aus dem Quellcode bauen

**Abhängigkeiten**

- macOS, `brew install libssh2`
- SQLite3, stb_truetype, stb_image und cJSON liegen unter `third_party/`

```bash
git clone https://github.com/calculusteam/liu.git && cd liu

./build.sh              # interaktives Build-TUI (macOS)
./build.sh --yes        # nicht-interaktiver empfohlener Build
./build.sh --package    # Build + Release-Artefakt

# Oder CMake direkt:
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(sysctl -n hw.ncpu)
./build/Liu
```

Ein Build erzeugt auch `liu-notify` (Benachrichtigungs-Daemon) und `liu-history` (Agent-History-CLI).

---

<p align="center">
  <sub>
    Apache-2.0
  </sub>
</p>
