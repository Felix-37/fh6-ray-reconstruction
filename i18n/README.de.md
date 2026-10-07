<div align="center">

# Ray Reconstruction für Forza Horizon 6

**DLSS Ray Reconstruction (DLSS-RR) in Forza Horizon 6 auf dem PC, als ReShade-Add-on.**<br>
Ein besserer Denoiser für die Raytracing-Beleuchtung des Spiels, für jede NVIDIA-RTX-Grafikkarte.

[English](../README.md) ·
[Español](README.es.md) ·
[Português (BR)](README.pt-BR.md) ·
[Français](README.fr.md) ·
**Deutsch** ·
[Italiano](README.it.md) ·
[Polski](README.pl.md) ·
[Русский](README.ru.md) ·
[Türkçe](README.tr.md) ·
[日本語](README.ja.md) ·
[한국어](README.ko.md) ·
[简体中文](README.zh-CN.md)

![Status: beta](https://img.shields.io/badge/status-beta-orange)
![Forza Horizon 6](https://img.shields.io/badge/Forza%20Horizon%206-6.440.853.0-0a7cff)
![NVIDIA RTX](https://img.shields.io/badge/GPU-NVIDIA%20RTX-76b900)
![ReShade 6.8+](https://img.shields.io/badge/ReShade-6.8%2B%20add--on-8a2be2)
![License: MIT](https://img.shields.io/badge/license-MIT-green)

</div>

> [!NOTE]
> Diese Übersetzung kann hinter dem [englischen README](../README.md) zurückliegen, das maßgeblich ist.
> Die technische Dokumentation für Modder gibt es nur auf Englisch, in [docs/](../docs/). Das Menü im
> Spiel ist auf Englisch (oder Spanisch); die Optionsnamen unten stehen deshalb auf Englisch.

> [!IMPORTANT]
> Beta. Ich habe es allein entwickelt und nur auf meinem PC getestet. Es funktioniert, aber
> einige Probleme sind noch offen, und dabei komme ich nicht weiter. Wenn du dich mit
> Grafikprogrammierung, DLSS/Streamline oder Shader-Modding auskennst, ist deine Hilfe sehr willkommen:
> siehe Abschnitt „Wo Hilfe gebraucht wird".

## Was es macht

Forza Horizon 6 bietet DLSS Super Resolution, DLAA, Frame Generation und Multi Frame Generation, aber
kein Ray Reconstruction, obwohl der Spielordner das Streamline-RR-Plugin (`sl.dlss_d.dll`) und das
RR-Modell (`nvngx_dlssd.dll`) bereits enthält. Dieses Add-on schaltet es ein:

1. Es fügt DLSS-RR zu den Features hinzu, die das Spiel von Streamline laden lässt.
2. Jedes Mal, wenn das Spiel DLSS Super Resolution auswertet, wertet es stattdessen DLSS Ray
   Reconstruction mit denselben Eingaben aus. Jeder Frame, dem etwas fehlt, fällt auf das normale DLSS
   des Spiels zurück.
3. In jedem Frame baut es aus dem Raytracing-G-Buffer des Spiels die Guide-Buffer, die RR braucht:
   Weltnormalen + Rauheit, diffuse Albedo, spekulare Albedo und diffuse Trefferdistanz.
4. Es kann die eigenen Denoising-Filter des Spiels für die Raytracing-GI verändern: Standardmäßig
   sammeln sie viermal mehr History („History x4", das stabilste Bild), und im Modus „Full RR" werden
   sie entfernt, damit RR die rohe globale Beleuchtung selbst entrauscht.

Die ausführbare Datei und die Dateien des Spiels werden nie verändert. Alles passiert im Speicher,
innerhalb von ReShade.

**Was du bekommst:** sauberere Raytracing-Beleuchtung mit weniger „Brodeln" und Flimmern im indirekten
Licht (Wände bei Nacht, Tunnel, Schatten). Feine Geometrie wie Zäune und Kabel ist oft schärfer.

**Was es kostet:** etwas GPU-Zeit und VRAM (siehe „Leistung").

## Vergleich

Ich habe diese Clips im Spiel auf meinem PC aufgenommen (RTX 4050 Laptop GPU, 1920×1200, stehendes
Auto). Sie zeigen echte Pixel 1:1, aus der Aufnahme ausgeschnitten. Der GI-Modus war **„Full RR“**
(experimentell), nicht der Standard „History x4“.

**Tunnel, Cockpitansicht.** Ich drücke F11 und RR geht aus: Handschuhe und Lenkrad fangen an zu
„kochen“.

<p align="center"><img src="../docs/images/boiling-cockpit-tunnel-f11.gif" width="800" alt="Cockpit in einem Tunnel: RR an, dann schaltet F11 es aus und die Handschuhe fangen an zu kochen"></p>

**Derselbe Tunnel, nebeneinander** (links: aus, rechts: an).

<p align="center"><img src="../docs/images/boiling-cockpit-tunnel.gif" width="800" alt="Lenkrad in einem Tunnel, RR links aus und rechts an"></p>

**Cockpit auf einer Waldstraße.** Zwei Momente derselben Einstellung, 4 s auseinander, in einer
Schleife: aus, dann an. Mit RR hören Handschuhe, A-Säule und Armaturenbrett auf zu kochen, und die
Audi-Ringe am Lenkrad bekommen ihre metallische Spiegelung zurück.

<p align="center"><img src="../docs/images/boiling-cockpit-forest.gif" width="800" alt="Cockpit auf einer Waldstraße, RR aus und dann an"></p>

Im Wald-Clip sinkt das zeitliche Rauschen des rechten Handschuhs von 2,3 auf 0,3–0,5 (an der Aufnahme
gemessen). Mit RR wirkt das Cockpit auch etwas dunkler, und ich weiß noch nicht, welche der beiden
Helligkeiten näher an der richtigen liegt. Außerhalb des Autos, bei Tag, ist der Unterschied viel
kleiner. Details in
[docs/MEASUREMENTS.md](../docs/MEASUREMENTS.md#from-a-gameplay-recording-7-oct-2026) (auf Englisch).

## Voraussetzungen

- Eine **NVIDIA GeForce RTX**-Grafikkarte. Ray Reconstruction läuft auf allen RTX-Generationen
  (20er, 30er, 40er und 50er Serie), ich habe es bisher aber nur auf meiner RTX 4050 Laptop (6 GB) getestet.
- **Forza Horizon 6** für PC, Version **6.440.853.0** (die Version, mit der es entwickelt wurde). Auf
  anderen Versionen sollte RR weiter funktionieren; die GI-Modi setzen unveränderte Shader des Spiels
  voraus, und das Menü sagt dir, wenn das nicht so ist.
- In den Grafikoptionen des Spiels: **DLSS** als Upscaler (beliebiger Qualitätsmodus) und
  **Raytracing-Global-Illumination** an. Frame Generation und Multi Frame Generation können anbleiben.
- **ReShade 6.8.0 oder neuer mit voller Add-on-Unterstützung** (auf [reshade.me](https://reshade.me)
  der Download „with full add-on support").
- Die Streamline-RR-Dateien des Spiels (`sl.dlss_d.dll`, `nvngx_dlssd.dll`), die mit dem Spiel kommen.
  Getestet mit Streamline 2.14.1 und DLSS-RR 310.9.0 / 310.9.1.

## Installation

1. Installiere **ReShade mit voller Add-on-Unterstützung** für `ForzaHorizon6.exe` und wähle
   **DirectX 10/11/12**. Effekt-Shader werden nicht benötigt.
2. Lade `rr-forza-<Version>.zip` von den [Releases](https://github.com/Felix-37/fh6-ray-reconstruction/releases)
   herunter und kopiere `rr-forza.addon64` in den Spielordner, neben `ForzaHorizon6.exe`.
3. Starte das Spiel. Wähle in den Grafikoptionen DLSS und schalte die Raytracing-Global-Illumination ein.
4. Fahre ein paar Sekunden und öffne dann das ReShade-Overlay (standardmäßig Taste `Pos1`/`Home`). Im
   Tab **Ray Reconstruction** muss der Status **„Ray Reconstruction active"** lauten.

**Zum Deinstallieren** `rr-forza.addon64` löschen. Die Einstellungen stehen in `ReShade.ini`, Abschnitt
`[RR_Forza]`, den du ebenfalls löschen kannst.

## Benutzung

- **F11** schaltet zwischen Ray Reconstruction und dem normalen DLSS des Spiels um, zum Vergleichen.
- Der Tab **Ray Reconstruction** des ReShade-Overlays enthält alle Einstellungen. Jede zeigt an, ob sie
  wirklich auf der GPU angekommen ist (`[OK]`, `[...]` oder `[ERROR]`).
  - **General:** Sprache (English / Español), RR an/aus, RR-Preset (F empfohlen), RR-Schärfung.
  - **Guides:** wie die Oberflächen für RR beschrieben werden (Vegetation, Metalness) und eine Diagnose.
  - **Game GI:** was mit den Raytracing-GI-Filtern des Spiels passiert (siehe Tabelle).
  - **Measurement:** ein eingebauter Messstand, der RR mit echten Zahlen gegen normales DLSS vergleicht.
  - **Developer:** Frame-Captures, Export der Spiel-Shader und Entwickler-Hotkeys (F7, F10;
    standardmäßig aus).
  - **Help:** Tasten, Dateien und bekannte Grenzen.

| „Game GI"-Modus | Wirkung |
|---|---|
| Game original | Die RTGI-Filter des Spiels bleiben unberührt, RR säubert ihr Ergebnis. |
| History x2 | Der temporale Filter des Spiels sammelt 2-mal mehr Frames: weniger Brodeln, aber das Licht reagiert langsamer. |
| **History x4** (Standard, empfohlen) | Dasselbe mit 4-mal mehr Frames: das stabilste Bild, mit guter Qualität und ohne die offenen Probleme von Full RR. Das indirekte Licht reagiert etwas langsamer (mögliche Schlieren bei bewegten Schatten). |
| No temporal filter | Die rohe GI jedes Frames geht an RR. Experimentell. |
| Full RR | Die temporalen und räumlichen RTGI-Filter des Spiels werden ersetzt, RR übernimmt das gesamte Denoising. Das beste Bild bei Tag (kein Brodeln im indirekten Licht), aber mit den offenen Problemen unter „Bekannte Probleme". Bewegt sich die Kamera, wechselt es automatisch zu „History x4". Experimentell. |

Die Standardwerte sind meine Empfehlung, gewählt nach den Messungen in
[docs/MEASUREMENTS.md](../docs/MEASUREMENTS.md) und vielen Stunden Fahrt. „Restore defaults" (Tab
Guides) stellt sie wieder her.

## Leistung

Gemessen auf meiner RTX 4050 Laptop, Ausgabe 1920×1200, interne Auflösung 1280×800, Multi Frame
Generation 4x und 138-fps-Limit:

- RR kostet etwa 0,9 ms mehr GPU-Zeit pro angezeigtem Frame als DLSS-SR im selben Qualitätsmodus
  (−6 % fps, wenn die GPU limitiert).
- RR mit DLSS **Balanced** kostete so viel wie das DLSS-SR des Spiels mit **Quality** (gleiche fps,
  gleiche GPU-Last), ohne sichtbaren Verlust. Das ist die Kombination, die ich empfehle.
- Beide Modelle bleiben geladen, damit F11 sauber umschaltet. Das kostet ein paar hundert MB VRAM mehr,
  was auf 6-GB-Karten knapp ist: Dort meldet das Spiel schon ohne RR „out of VRAM"-Warnungen.

## Bekannte Probleme

Noch offen; die bisher gesammelten Belege stehen in
[docs/OPEN-PROBLEMS.md](../docs/OPEN-PROBLEMS.md) (auf Englisch):

1. **Rauschen in Bewegung (Full RR):** Einige Frames lang ist rohes Rauschen zu sehen, wenn du dich
   bewegst oder etwas wieder ins Bild kommt. „GI history x4 while moving" und „Disocclusion assist"
   verringern es, beseitigen es aber nicht.
2. **Spiegelartige Reflexionen** (Glas, Chrom, Glanzlack) bleiben verrauscht, vor allem mit Full RR.
   RR braucht vermutlich spekulare Bewegungsvektoren; die experimentelle Option „Specular motion
   vectors" (Tab Guides, standardmäßig aus) liefert sie mit einer geschätzten Distanz und ist noch nicht
   gemessen.
3. **Glühwürmchen bei Nacht (Full RR):** mit dem „Isolated sample clamp" viel seltener, aber nicht null.
4. **Baumkronen vor hellem Himmel** können mit RR überstrahlt (zu hell) wirken.
5. **Langsames Brodeln in Tunneln** mit den GI-Filtern des Spiels (original oder History x4): RR
   verringert es (−17 bis −22 %), beseitigt es aber nicht; es kommt vielleicht von den RT-Schatten und
   nicht von der GI.

## FAQ

**Funktioniert es auf AMD- oder Intel-GPUs?** Nein. DLSS Ray Reconstruction läuft nur auf NVIDIA-RTX-GPUs.

**Kann ich gebannt werden?** Mir ist kein Fall bekannt, aber ich kann nichts versprechen. Das Add-on
verändert keine Spieldateien: Es hookt die Streamline-Bibliothek von NVIDIA und Direct3D 12 im
Spielprozess, wie es ReShade-Add-ons tun. Online-Nutzung auf eigenes Risiko.

**Funktioniert es mit anderen Mods?** Ich nutze es problemlos zusammen mit dem RenoDX-Add-on
„MFG Unlock". Andere Kombinationen (OptiScaler, ausgetauschte DLSS-DLLs) sind ungetestet.

**Der Tab Game GI meldet, die Filter wurden „nicht gesehen" oder eine Variante wurde „nicht erstellt".**
Fahre zuerst ein paar Sekunden. Bleibt es so, hat deine Spielversion wahrscheinlich andere Shader.
Bitte eröffne ein Issue mit deiner Spielversion und deiner `ReShade.log`.

**Wo ist das Log?** `ReShade.log` im Spielordner. Die Zeilen dieses Add-ons beginnen mit `[RR Forza]`.

## Wo Hilfe gebraucht wird

Dieses Projekt braucht Leute, die sich in einigen dieser Bereiche besser auskennen als ich:

- **Spekulare Bewegungsvektoren und spekulare Trefferdistanz für RR**, um die verrauschten Reflexionen
  zu beheben. Speedlemurs Control-RR-Mod hat das gemacht. Die Reflexionen von Forza sind Inline-Ray-
  Queries plus eine Mip-Pyramide, beschrieben in [docs/FRAME-MAP.md](../docs/FRAME-MAP.md).
- **Disokklusions-Rauschen in Full RR:** wie man RR für Pixel ohne History einen besseren Start gibt.
- **Überstrahlte Baumkronen:** Vom Wind bewegte Vegetation hat keine eigenen Bewegungsvektoren. Würde
  eine Responsiveness- oder „Current Color"-Maske helfen?
- **Tests auf anderen RTX-GPUs**, Auflösungen und Spielversionen sowie **Vergleichs-Screenshots oder
  -Videos** (gleiche Stelle, F11 schaltet RR um). Dieses README hat noch keine Vergleichsbilder.

Fang mit [CONTRIBUTING.md](../CONTRIBUTING.md) an, dann [docs/ARCHITECTURE.md](../docs/ARCHITECTURE.md)
(wie das Add-on funktioniert) und [docs/MEASUREMENTS.md](../docs/MEASUREMENTS.md) (was ausprobiert,
gemessen und verworfen wurde, damit es niemand wiederholt). Zum Kompilieren braucht es nur kostenlose
Werkzeuge: [docs/BUILDING.md](../docs/BUILDING.md).

## Danksagungen

- **speedlemur**, für [Control Ray Reconstruction](https://github.com/speedlemur/renodx/tree/control-rr):
  dieselbe Idee (die DLSS-SR-Auswertung des Spiels mit DLSS-RR beantworten) und die Technik, Shader zur
  Laufzeit zu ersetzen.
- **crosire**, für [ReShade](https://github.com/crosire/reshade) und seine Add-on-API.
- **clshortfuse und die RenoDX-Mitwirkenden**, für [RenoDX](https://github.com/clshortfuse/renodx),
  dessen Shader-Hash-Konvention hier verwendet wird.
- **NVIDIA**, für [Streamline](https://github.com/NVIDIA-RTX/Streamline) und DLSS.
- **Tsuda Kageyu** ([MinHook](https://github.com/TsudaKageyu/minhook)), **Omar Cornut**
  ([Dear ImGui](https://github.com/ocornut/imgui)) und **Microsoft**
  ([DirectX Shader Compiler](https://github.com/microsoft/DirectXShaderCompiler)).

Lizenzen von Drittanbietern: [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md).

## Rechtliches

Dies ist ein inoffizielles Fanprojekt. Es steht in keiner Verbindung zu Microsoft, Xbox Game Studios,
Playground Games oder NVIDIA und wird von ihnen nicht unterstützt. Forza Horizon ist eine Marke von
Microsoft. NVIDIA, RTX und DLSS sind Marken der NVIDIA Corporation.

Das Repository enthält keine Spieldateien. Für die GI-Modi enthält der kompilierte Release veränderte
Versionen zweier Compute-Shader des Spiels, die ohne das Spiel nutzlos sind. Nutzung auf eigenes Risiko.

Der Code steht unter der [MIT-Lizenz](../LICENSE).
