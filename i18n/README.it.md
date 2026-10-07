<div align="center">

# Ray Reconstruction per Forza Horizon 6

**DLSS Ray Reconstruction (DLSS-RR) in Forza Horizon 6 su PC, come add-on di ReShade.**<br>
Un denoiser migliore per l'illuminazione in ray tracing del gioco, per qualsiasi scheda NVIDIA RTX.

[English](../README.md) ·
[Español](README.es.md) ·
[Português (BR)](README.pt-BR.md) ·
[Français](README.fr.md) ·
[Deutsch](README.de.md) ·
**Italiano** ·
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
> Questa traduzione può essere più vecchia del [README in inglese](../README.md), che fa da riferimento.
> La documentazione tecnica per i modder è solo in inglese, in [docs/](../docs/). Il menu nel gioco è in
> inglese (o spagnolo): i nomi delle opzioni qui sotto sono in inglese, come nel menu.

> [!IMPORTANT]
> Beta. L'ho realizzata da solo e provata solo sul mio PC. Funziona, ma alcuni problemi sono
> ancora aperti e ci sono bloccato. Se conosci la programmazione grafica, DLSS/Streamline o il modding
> degli shader, il tuo aiuto è benvenuto: vedi la sezione «Dove serve aiuto».

## Cosa fa

Forza Horizon 6 offre DLSS Super Resolution, DLAA, Frame Generation e Multi Frame Generation, ma non
Ray Reconstruction, anche se la cartella del gioco contiene già il plugin RR di Streamline
(`sl.dlss_d.dll`) e il modello RR (`nvngx_dlssd.dll`). Questo add-on lo attiva:

1. Aggiunge DLSS-RR alle funzioni che il gioco chiede a Streamline di caricare.
2. Ogni volta che il gioco valuta DLSS Super Resolution, valuta invece DLSS Ray Reconstruction con gli
   stessi input. Qualsiasi frame a cui manca qualcosa torna al DLSS normale del gioco.
3. A ogni frame costruisce, dal G-buffer di ray tracing del gioco, i buffer guida di cui RR ha bisogno:
   normali del mondo + rugosità, albedo diffuso, albedo speculare e distanza di impatto diffusa.
4. Può modificare i filtri di denoise della GI in ray tracing del gioco: di default accumulano quattro
   volte più storico («History x4», l'immagine più stabile), e nella modalità «Full RR» vengono rimossi
   perché sia RR stesso a pulire l'illuminazione globale grezza.

L'eseguibile e i file del gioco non vengono mai modificati. Tutto avviene in memoria, dentro ReShade.

**Cosa ottieni:** un'illuminazione in ray tracing più pulita, con meno «ribollio» e sfarfallio nella
luce indiretta (muri di notte, gallerie, ombre). La geometria sottile, come recinzioni e cavi, spesso
risulta più nitida.

**Cosa costa:** un po' di tempo di GPU e di VRAM (vedi «Prestazioni»).

## Confronto

Ho registrato queste clip nel gioco, sul mio PC (RTX 4050 Laptop GPU, 1920×1200, auto ferma). Mostrano
pixel reali 1:1, ritagliati dalla registrazione. La modalità GI era **"Full RR"** (sperimentale), non
quella predefinita "History x4".

**Galleria, visuale abitacolo.** Premo F11 e RR si spegne: i guanti e il volante iniziano a "ribollire".

<p align="center"><img src="../docs/images/boiling-cockpit-tunnel-f11.gif" width="800" alt="Abitacolo in una galleria: RR acceso, poi F11 lo spegne e i guanti iniziano a ribollire"></p>

**La stessa galleria, affiancata** (a sinistra: spento, a destra: acceso).

<p align="center"><img src="../docs/images/boiling-cockpit-tunnel.gif" width="800" alt="Volante in una galleria, RR spento a sinistra e acceso a destra"></p>

**Abitacolo su una strada nel bosco.** Due momenti della stessa inquadratura, a 4 s di distanza, in
loop: spento, poi acceso. Con RR i guanti, il montante A e il cruscotto smettono di ribollire, e gli
anelli Audi sul volante ritrovano il riflesso metallico.

<p align="center"><img src="../docs/images/boiling-cockpit-forest.gif" width="800" alt="Abitacolo su una strada nel bosco, RR spento e poi acceso"></p>

Nella clip del bosco, il rumore temporale del guanto destro scende da 2,3 a 0,3–0,5 (misurato sulla
registrazione). Con RR l'abitacolo sembra anche un po' più scuro, e non so ancora quale dei due sia più
vicino alla luminosità corretta. Fuori dall'auto, di giorno, la differenza è molto più piccola.
Dettagli in [docs/MEASUREMENTS.md](../docs/MEASUREMENTS.md#from-a-gameplay-recording-7-oct-2026) (in inglese).

## Requisiti

- Una scheda **NVIDIA GeForce RTX**. Ray Reconstruction funziona su tutte le generazioni RTX (serie 20,
  30, 40 e 50), ma finora l'ho provato solo sulla mia RTX 4050 Laptop (6 GB).
- **Forza Horizon 6** per PC, versione **6.440.853.0** (la versione su cui è stato sviluppato). Su altre
  versioni RR dovrebbe funzionare; le modalità GI richiedono che gli shader del gioco non siano cambiati,
  e il menu te lo segnala.
- Nelle opzioni video del gioco: **DLSS** come upscaler (qualsiasi modalità di qualità) e
  **illuminazione globale in ray tracing** attiva. Frame Generation e Multi Frame Generation possono
  restare attivi.
- **ReShade 6.8.0 o più recente con supporto completo agli add-on** (su [reshade.me](https://reshade.me),
  il download «with full add-on support»).
- I file RR di Streamline del gioco (`sl.dlss_d.dll`, `nvngx_dlssd.dll`), inclusi nel gioco. Provato
  con Streamline 2.14.1 e DLSS-RR 310.9.0 / 310.9.1.

## Installazione

1. Installa **ReShade con supporto completo agli add-on** per `ForzaHorizon6.exe` e scegli
   **DirectX 10/11/12**. Non serve nessuno shader di effetti.
2. Scarica `rr-forza-<versione>.zip` dalle [Releases](https://github.com/Felix-37/fh6-ray-reconstruction/releases)
   e copia `rr-forza.addon64` nella cartella del gioco, accanto a `ForzaHorizon6.exe`.
3. Avvia il gioco. Nelle opzioni video scegli DLSS e attiva l'illuminazione globale in ray tracing.
4. Guida per qualche secondo, poi apri l'overlay di ReShade (tasto `Home` di default). Nella scheda
   **Ray Reconstruction** lo stato deve dire **«Ray Reconstruction active»**.

**Per disinstallare,** elimina `rr-forza.addon64`. Le impostazioni sono in `ReShade.ini`, sezione
`[RR_Forza]`, che puoi eliminare anch'essa.

## Uso

- **F11** passa da Ray Reconstruction al DLSS normale del gioco e viceversa, per confrontare.
- La scheda **Ray Reconstruction** dell'overlay di ReShade contiene tutte le impostazioni. Ognuna mostra
  se è davvero arrivata alla GPU (`[OK]`, `[...]` o `[ERROR]`).
  - **General:** lingua (English / Español), RR sì/no, preset di RR (consigliato F), nitidezza di RR.
  - **Guides:** come le superfici vengono descritte a RR (vegetazione, metalness) e una diagnostica.
  - **Game GI:** cosa succede ai filtri della GI in ray tracing del gioco (vedi la tabella).
  - **Measurement:** un banco di misura integrato che confronta RR con il DLSS normale con numeri reali.
  - **Developer:** cattura di frame, esportazione degli shader del gioco e tasti di sviluppo (F7, F10;
    disattivati di default).
  - **Help:** tasti, file e limiti noti.

| Modalità «Game GI» | Cosa fa |
|---|---|
| Game original | I filtri RTGI del gioco restano intatti e RR pulisce il loro risultato. |
| History x2 | Il filtro temporale del gioco accumula 2 volte più frame: meno ribollio, ma la luce reagisce più lentamente. |
| **History x4** (predefinita, consigliata) | Lo stesso con 4 volte più frame: l'immagine più stabile, di buona qualità e senza i problemi aperti di Full RR. La luce indiretta reagisce un po' più lentamente (possibili scie sulle ombre in movimento). |
| No temporal filter | La GI grezza di ogni frame va a RR. Sperimentale. |
| Full RR | I filtri RTGI temporale e spaziale del gioco vengono sostituiti e RR fa tutto il denoise. L'immagine migliore di giorno (nessun ribollio della luce indiretta), ma con i problemi aperti elencati in «Problemi noti». Quando la telecamera si muove, passa da sola a «History x4». Sperimentale. |

I valori predefiniti sono la mia raccomandazione, scelti dopo le misure di
[docs/MEASUREMENTS.md](../docs/MEASUREMENTS.md) e molte ore di guida. «Restore defaults» (scheda Guides)
li ripristina.

## Prestazioni

Misurato sulla mia RTX 4050 Laptop, uscita 1920×1200, risoluzione interna 1280×800, Multi Frame
Generation 4x e limite a 138 fps:

- RR costa circa 0,9 ms di GPU in più per frame mostrato rispetto a DLSS-SR nella stessa modalità di
  qualità (−6 % di fps quando il limite è la GPU).
- RR con DLSS **Balanced** è costato quanto il DLSS-SR del gioco in **Quality** (stessi fps, stesso
  carico della GPU), senza perdite visibili. È la combinazione che consiglio.
- Entrambi i modelli restano caricati perché F11 commuti senza problemi. Questo usa qualche centinaio di
  MB di VRAM in più, il che è al limite sulle schede da 6 GB: lì il gioco segnala già avvisi
  «out of VRAM» senza RR.

## Problemi noti

Ancora aperti; le prove raccolte finora sono in
[docs/OPEN-PROBLEMS.md](../docs/OPEN-PROBLEMS.md) (in inglese):

1. **Rumore in movimento (Full RR):** per qualche frame si vede rumore grezzo quando ti muovi o quando
   qualcosa rientra nell'inquadratura. «GI history x4 while moving» e «Disocclusion assist» lo riducono,
   ma non lo eliminano.
2. I **riflessi a specchio** (vetro, cromature, vernice lucida) restano rumorosi, soprattutto con Full
   RR. Probabilmente RR ha bisogno di vettori di movimento speculari; l'opzione sperimentale «Specular
   motion vectors» (scheda Guides, disattivata di default) li fornisce con una distanza stimata, e non è
   ancora misurata.
3. **Lucciole di notte (Full RR):** molto più rare con l'«Isolated sample clamp», ma non a zero.
4. Le **chiome degli alberi contro un cielo luminoso** possono apparire bruciate (troppo chiare) con RR.
5. **Ribollio lento nelle gallerie** con i filtri GI del gioco (originale o History x4): RR lo riduce
   (−17/−22 %), ma non lo elimina; forse viene dalle ombre RT e non dalla GI.

## Domande frequenti

**Funziona su GPU AMD o Intel?** No. DLSS Ray Reconstruction funziona solo su GPU NVIDIA RTX.

**Posso essere bannato?** Non conosco nessun caso, ma non posso promettere nulla. L'add-on non tocca i
file del gioco: aggancia la libreria Streamline di NVIDIA e Direct3D 12 dentro il processo del gioco,
come fanno gli add-on di ReShade. L'uso online è a tuo rischio.

**Funziona con altre mod?** Lo uso insieme all'add-on «MFG Unlock» di RenoDX senza problemi. Le
altre combinazioni (OptiScaler, sostituzione delle DLL di DLSS) non sono state provate.

**La scheda Game GI dice che i filtri «non sono stati visti» o che una variante «non è stata creata».**
Guida prima qualche secondo. Se resta così, probabilmente la tua versione del gioco ha shader diversi.
Apri una issue con la versione del gioco e il tuo `ReShade.log`.

**Dov'è il log?** `ReShade.log`, nella cartella del gioco. Le righe di questo add-on iniziano con
`[RR Forza]`.

## Dove serve aiuto

Questo progetto ha bisogno di persone che ne sappiano più di me su alcuni di questi temi:

- **Vettori di movimento speculari e distanza di impatto speculare per RR**, per sistemare i riflessi
  rumorosi. La mod Control RR di speedlemur l'ha fatto. I riflessi di Forza sono ray query inline più una
  piramide di mip, descritti in [docs/FRAME-MAP.md](../docs/FRAME-MAP.md).
- **Rumore di disocclusione in Full RR:** come dare a RR un punto di partenza migliore per i pixel senza
  storico.
- **Chiome bruciate:** la vegetazione mossa dal vento non ha vettori di movimento propri. Una maschera
  di reattività o di «colore corrente» aiuterebbe?
- **Test su altre GPU RTX**, risoluzioni e versioni del gioco, e **screenshot o video affiancati**
  (stesso punto, F11 attiva/disattiva RR). Questo README non ha ancora immagini di confronto.

Inizia da [CONTRIBUTING.md](../CONTRIBUTING.md), poi [docs/ARCHITECTURE.md](../docs/ARCHITECTURE.md)
(come funziona l'add-on) e [docs/MEASUREMENTS.md](../docs/MEASUREMENTS.md) (cosa è stato provato,
misurato e scartato, perché nessuno lo ripeta). Per compilarlo bastano strumenti gratuiti:
[docs/BUILDING.md](../docs/BUILDING.md).

## Riconoscimenti

- **speedlemur**, per [Control Ray Reconstruction](https://github.com/speedlemur/renodx/tree/control-rr):
  la stessa idea (rispondere alla valutazione DLSS-SR del gioco con DLSS-RR) e la tecnica di sostituire
  gli shader in fase di esecuzione.
- **crosire**, per [ReShade](https://github.com/crosire/reshade) e la sua API per add-on.
- **clshortfuse e i contributori di RenoDX**, per [RenoDX](https://github.com/clshortfuse/renodx), la cui
  convenzione di hash degli shader è usata qui.
- **NVIDIA**, per [Streamline](https://github.com/NVIDIA-RTX/Streamline) e DLSS.
- **Tsuda Kageyu** ([MinHook](https://github.com/TsudaKageyu/minhook)), **Omar Cornut**
  ([Dear ImGui](https://github.com/ocornut/imgui)) e **Microsoft**
  ([DirectX Shader Compiler](https://github.com/microsoft/DirectXShaderCompiler)).

Licenze di terze parti: [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md).

## Note legali

Questo è un progetto non ufficiale di fan. Non è affiliato né approvato da Microsoft, Xbox Game Studios,
Playground Games o NVIDIA. Forza Horizon è un marchio di Microsoft. NVIDIA, RTX e DLSS sono marchi di
NVIDIA Corporation.

Il repository non contiene file del gioco. Per offrire le modalità GI, la versione compilata contiene
versioni modificate di due compute shader del gioco, inutili senza il gioco. Usalo a tuo rischio.

Il codice è distribuito con [licenza MIT](../LICENSE).
