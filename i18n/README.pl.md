<div align="center">

# Ray Reconstruction dla Forza Horizon 6

**DLSS Ray Reconstruction (DLSS-RR) w Forza Horizon 6 na PC, jako dodatek (add-on) do ReShade.**<br>
Lepszy odszumiacz dla oświetlenia ray tracing w grze, dla każdej karty NVIDIA RTX.

[English](../README.md) ·
[Español](README.es.md) ·
[Português (BR)](README.pt-BR.md) ·
[Français](README.fr.md) ·
[Deutsch](README.de.md) ·
[Italiano](README.it.md) ·
**Polski** ·
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
> To tłumaczenie może być starsze niż [README po angielsku](../README.md), które jest wersją wzorcową.
> Dokumentacja techniczna dla modderów jest tylko po angielsku, w [docs/](../docs/). Menu w grze jest po
> angielsku (lub hiszpańsku), dlatego nazwy opcji poniżej podano po angielsku, tak jak w menu.

> [!IMPORTANT]
> Wersja beta. Stworzyłem ją sam i przetestowałem tylko na swoim PC. Działa, ale kilka problemów
> wciąż jest otwartych i utknąłem na nich. Jeśli znasz programowanie grafiki, DLSS/Streamline albo
> modding shaderów, Twoja pomoc jest bardzo mile widziana: zobacz sekcję „Gdzie potrzebna jest pomoc".

## Co robi

Forza Horizon 6 oferuje DLSS Super Resolution, DLAA, Frame Generation i Multi Frame Generation, ale nie
Ray Reconstruction, choć folder gry zawiera już wtyczkę RR Streamline (`sl.dlss_d.dll`) i model RR
(`nvngx_dlssd.dll`). Ten dodatek go włącza:

1. Dodaje DLSS-RR do funkcji, które gra każe Streamline załadować.
2. Za każdym razem, gdy gra uruchamia DLSS Super Resolution, zamiast tego uruchamia DLSS Ray
   Reconstruction z tymi samymi danymi wejściowymi. Każda klatka, w której czegoś brakuje, wraca do
   zwykłego DLSS gry.
3. W każdej klatce buduje z G-bufora ray tracingu gry bufory pomocnicze potrzebne RR: normalne w
   przestrzeni świata + chropowatość, albedo rozproszone, albedo lustrzane i dystans trafienia
   rozproszonego.
4. Może zmienić własne filtry odszumiania GI ray tracingu gry: domyślnie kumulują cztery razy więcej
   historii („History x4", najstabilniejszy obraz), a w trybie „Full RR" są usuwane, aby RR sam
   odszumiał surowe oświetlenie globalne.

Plik wykonywalny i pliki gry nigdy nie są modyfikowane. Wszystko dzieje się w pamięci, wewnątrz ReShade.

**Co zyskujesz:** czystsze oświetlenie ray tracing, z mniejszym „gotowaniem się" i migotaniem światła
pośredniego (ściany nocą, tunele, cienie). Cienka geometria, jak ogrodzenia i kable, często jest ostrzejsza.

**Ile to kosztuje:** trochę czasu GPU i VRAM (zobacz „Wydajność").

## Porównanie

Nagrałem te klipy w grze na moim PC (RTX 4050 Laptop GPU, 1920×1200, samochód stoi). Pokazują
prawdziwe piksele 1:1, wycięte z nagrania. Tryb GI to **„Full RR”** (eksperymentalny), a nie domyślny
„History x4”.

**Tunel, widok z kokpitu.** Naciskam F11 i RR się wyłącza: rękawiczki i kierownica zaczynają „gotować
się”.

<p align="center"><img src="../docs/images/boiling-cockpit-tunnel-f11.gif" width="800" alt="Kokpit w tunelu: RR włączony, potem F11 go wyłącza i rękawiczki zaczynają się gotować"></p>

**Ten sam tunel, obok siebie** (po lewej: wyłączony, po prawej: włączony).

<p align="center"><img src="../docs/images/boiling-cockpit-tunnel.gif" width="800" alt="Kierownica w tunelu, RR wyłączony po lewej i włączony po prawej"></p>

**Kokpit na leśnej drodze.** Dwa momenty tego samego ujęcia, w odstępie 4 s, w pętli: wyłączony,
potem włączony. Z RR rękawiczki, słupek A i deska rozdzielcza przestają się gotować, a pierścienie Audi
na kierownicy odzyskują metaliczne odbicie.

<p align="center"><img src="../docs/images/boiling-cockpit-forest.gif" width="800" alt="Kokpit na leśnej drodze, RR wyłączony, a potem włączony"></p>

W klipie z lasu szum czasowy prawej rękawiczki spada z 2,3 do 0,3–0,5 (zmierzone na nagraniu). Z RR
kokpit wygląda też trochę ciemniej i jeszcze nie wiem, która z dwóch jasności jest bliższa poprawnej.
Poza samochodem, w dzień, różnica jest dużo mniejsza. Szczegóły w
[docs/MEASUREMENTS.md](../docs/MEASUREMENTS.md#from-a-gameplay-recording-7-oct-2026) (po angielsku).

## Wymagania

- Karta **NVIDIA GeForce RTX**. Ray Reconstruction działa na wszystkich generacjach RTX (serie 20, 30,
  40 i 50), ale na razie przetestowałem go tylko na swoim RTX 4050 Laptop (6 GB).
- **Forza Horizon 6** na PC, wersja **6.440.853.0** (wersja, na której powstawał dodatek). W innych
  wersjach RR powinien nadal działać; tryby GI wymagają niezmienionych shaderów gry, a menu informuje,
  gdy się zmieniły.
- W ustawieniach grafiki gry: **DLSS** jako upscaler (dowolny tryb jakości) i włączone **oświetlenie
  globalne ray tracing**. Frame Generation i Multi Frame Generation mogą pozostać włączone.
- **ReShade 6.8.0 lub nowszy z pełną obsługą dodatków** (na [reshade.me](https://reshade.me) pobieranie
  „with full add-on support").
- Pliki RR Streamline dostarczane z grą (`sl.dlss_d.dll`, `nvngx_dlssd.dll`). Testowane ze Streamline
  2.14.1 i DLSS-RR 310.9.0 / 310.9.1.

## Instalacja

1. Zainstaluj **ReShade z pełną obsługą dodatków** dla `ForzaHorizon6.exe` i wybierz
   **DirectX 10/11/12**. Shadery efektów nie są potrzebne.
2. Pobierz `rr-forza-<wersja>.zip` z [Releases](https://github.com/Felix-37/fh6-ray-reconstruction/releases)
   i skopiuj `rr-forza.addon64` do folderu gry, obok `ForzaHorizon6.exe`.
3. Uruchom grę. W ustawieniach grafiki wybierz DLSS i włącz oświetlenie globalne ray tracing.
4. Pojeździj kilka sekund, a potem otwórz nakładkę ReShade (domyślnie klawisz `Home`). W karcie
   **Ray Reconstruction** status musi brzmieć **„Ray Reconstruction active"**.

**Aby odinstalować,** usuń `rr-forza.addon64`. Jego ustawienia są w `ReShade.ini`, w sekcji
`[RR_Forza]`, którą też możesz usunąć.

## Użycie

- **F11** przełącza między Ray Reconstruction a zwykłym DLSS gry, aby porównać.
- Karta **Ray Reconstruction** w nakładce ReShade zawiera wszystkie ustawienia. Każde pokazuje, czy
  naprawdę dotarło do GPU (`[OK]`, `[...]` lub `[ERROR]`).
  - **General:** język (English / Español), RR wł./wył., preset RR (zalecany F), wyostrzanie RR.
  - **Guides:** jak powierzchnie są opisywane dla RR (roślinność, metalness) oraz diagnostyka.
  - **Game GI:** co dzieje się z filtrami GI ray tracingu gry (zobacz tabelę).
  - **Measurement:** wbudowane stanowisko pomiarowe, które porównuje RR ze zwykłym DLSS na prawdziwych
    liczbach.
  - **Developer:** przechwytywanie klatek, eksport shaderów gry i skróty deweloperskie (F7, F10;
    domyślnie wyłączone).
  - **Help:** klawisze, pliki i znane ograniczenia.

| Tryb „Game GI" | Działanie |
|---|---|
| Game original | Filtry RTGI gry pozostają nietknięte, a RR czyści ich wynik. |
| History x2 | Filtr czasowy gry kumuluje 2 razy więcej klatek: mniej „gotowania", ale światło reaguje wolniej. |
| **History x4** (domyślny, zalecany) | To samo z 4 razy większą liczbą klatek: najstabilniejszy obraz, dobrej jakości i bez otwartych problemów Full RR. Światło pośrednie reaguje nieco wolniej (możliwe smugi przy ruchomych cieniach). |
| No temporal filter | Surowe GI z każdej klatki trafia do RR. Eksperymentalny. |
| Full RR | Czasowy i przestrzenny filtr RTGI gry zostają zastąpione, a całe odszumianie wykonuje RR. Najlepszy obraz w dzień (brak „gotowania" światła pośredniego), ale z otwartymi problemami z sekcji „Znane problemy". Gdy kamera się porusza, automatycznie przełącza się na „History x4". Eksperymentalny. |

Wartości domyślne to moje zalecenie, wybrane po pomiarach z
[docs/MEASUREMENTS.md](../docs/MEASUREMENTS.md) i wielu godzinach jazdy. „Restore defaults" (karta
Guides) je przywraca.

## Wydajność

Zmierzone na moim RTX 4050 Laptop, wyjście 1920×1200, rozdzielczość wewnętrzna 1280×800, Multi Frame
Generation 4x i limit 138 fps:

- RR kosztuje ok. 0,9 ms czasu GPU więcej na wyświetloną klatkę niż DLSS-SR w tym samym trybie jakości
  (−6 % fps, gdy ograniczeniem jest GPU).
- RR w DLSS **Balanced** kosztował tyle samo co DLSS-SR gry w **Quality** (te same fps, to samo
  obciążenie GPU), bez widocznej straty. To kombinacja, którą polecam.
- Oba modele pozostają załadowane, aby F11 przełączał bez problemów. Zajmuje to kilkaset MB VRAM więcej,
  co jest na granicy na kartach 6 GB: tam gra już bez RR zgłasza ostrzeżenia „out of VRAM".

## Znane problemy

Wciąż otwarte; zebrane dotąd dowody są w [docs/OPEN-PROBLEMS.md](../docs/OPEN-PROBLEMS.md) (po angielsku):

1. **Szum w ruchu (Full RR):** przez kilka klatek widać surowy szum, gdy się poruszasz lub gdy coś wraca
   na ekran. „GI history x4 while moving" i „Disocclusion assist" go zmniejszają, ale nie usuwają.
2. **Odbicia lustrzane** (szkło, chrom, błyszczący lakier) pozostają zaszumione, zwłaszcza w Full RR.
   RR prawdopodobnie potrzebuje lustrzanych wektorów ruchu; eksperymentalna opcja „Specular motion
   vectors" (karta Guides, domyślnie wyłączona) dostarcza je z szacowanym dystansem i nie została
   jeszcze zmierzona.
3. **Świetliki nocą (Full RR):** dużo rzadsze dzięki „Isolated sample clamp", ale nie zero.
4. **Korony drzew na tle jasnego nieba** mogą z RR wyglądać na przepalone (za jasne).
5. **Powolne „gotowanie" w tunelach** z filtrami GI gry (oryginalnymi lub History x4): RR je zmniejsza
   (od −17 do −22 %), ale nie usuwa; może pochodzić z cieni RT, a nie z GI.

## FAQ

**Czy działa na kartach AMD lub Intel?** Nie. DLSS Ray Reconstruction działa tylko na kartach NVIDIA RTX.

**Czy mogę dostać bana?** Nie znam żadnego przypadku, ale niczego nie mogę obiecać. Dodatek nie zmienia
plików gry: podpina się do biblioteki Streamline NVIDIA i Direct3D 12 w procesie gry, tak jak robią to
dodatki ReShade. Gra online na własne ryzyko.

**Czy działa z innymi modami?** Używam go razem z dodatkiem „MFG Unlock" od RenoDX bez problemów. Inne
kombinacje (OptiScaler, podmiana bibliotek DLL DLSS) nie były testowane.

**Karta Game GI mówi, że filtrów „nie widziano" albo że wariantu „nie utworzono".** Najpierw pojeździj
kilka sekund. Jeśli nic się nie zmieni, Twoja wersja gry ma prawdopodobnie inne shadery. Otwórz issue z
wersją gry i swoim `ReShade.log`.

**Gdzie jest log?** `ReShade.log` w folderze gry. Linie tego dodatku zaczynają się od `[RR Forza]`.

## Gdzie potrzebna jest pomoc

Ten projekt potrzebuje osób, które wiedzą więcej ode mnie o niektórych z tych tematów:

- **Lustrzane wektory ruchu i lustrzany dystans trafienia dla RR**, aby naprawić zaszumione odbicia.
  Zrobił to mod Control RR autorstwa speedlemur. Odbicia w Forza to zapytania o promienie inline plus
  piramida mipmap, opisane w [docs/FRAME-MAP.md](../docs/FRAME-MAP.md).
- **Szum dezokluzji w Full RR:** jak dać RR lepszy start dla pikseli bez historii.
- **Przepalone korony drzew:** roślinność poruszana wiatrem nie ma własnych wektorów ruchu. Czy pomogłaby
  maska responsywności lub „bieżącego koloru"?
- **Testy na innych kartach RTX**, rozdzielczościach i wersjach gry oraz **zrzuty ekranu lub filmy
  porównawcze** (to samo miejsce, F11 przełącza RR). To README nie ma jeszcze obrazów porównawczych.

Zacznij od [CONTRIBUTING.md](../CONTRIBUTING.md), potem [docs/ARCHITECTURE.md](../docs/ARCHITECTURE.md)
(jak działa dodatek) i [docs/MEASUREMENTS.md](../docs/MEASUREMENTS.md) (co wypróbowano, zmierzono i
odrzucono, żeby nikt tego nie powtarzał). Do kompilacji wystarczą darmowe narzędzia:
[docs/BUILDING.md](../docs/BUILDING.md).

## Podziękowania

- **speedlemur**, za [Control Ray Reconstruction](https://github.com/speedlemur/renodx/tree/control-rr):
  ten sam pomysł (odpowiadanie na wywołanie DLSS-SR gry przez DLSS-RR) i technikę podmiany shaderów w
  czasie działania.
- **crosire**, za [ReShade](https://github.com/crosire/reshade) i jego API dodatków.
- **clshortfuse i współtwórcom RenoDX**, za [RenoDX](https://github.com/clshortfuse/renodx), którego
  konwencja haszowania shaderów jest tu używana.
- **NVIDIA**, za [Streamline](https://github.com/NVIDIA-RTX/Streamline) i DLSS.
- **Tsuda Kageyu** ([MinHook](https://github.com/TsudaKageyu/minhook)), **Omarowi Cornutowi**
  ([Dear ImGui](https://github.com/ocornut/imgui)) i **Microsoft**
  ([DirectX Shader Compiler](https://github.com/microsoft/DirectXShaderCompiler)).

Licencje stron trzecich: [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md).

## Informacje prawne

To nieoficjalny projekt fanowski. Nie jest powiązany z Microsoft, Xbox Game Studios, Playground Games
ani NVIDIA i nie jest przez nie wspierany. Forza Horizon jest znakiem towarowym Microsoft. NVIDIA, RTX
i DLSS są znakami towarowymi NVIDIA Corporation.

Repozytorium nie zawiera plików gry. Aby udostępnić tryby GI, skompilowane wydanie zawiera zmodyfikowane
wersje dwóch compute shaderów gry, bezużyteczne bez gry. Używasz na własne ryzyko.

Kod jest udostępniony na [licencji MIT](../LICENSE).
