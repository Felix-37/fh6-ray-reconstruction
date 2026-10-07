<div align="center">

# Ray Reconstruction para Forza Horizon 6

**DLSS Ray Reconstruction (DLSS-RR) en Forza Horizon 6 para PC, como add-on de ReShade.**<br>
Un mejor denoiser para la iluminación por ray tracing del juego, para cualquier tarjeta NVIDIA RTX.

[English](../README.md) ·
**Español** ·
[Português (BR)](README.pt-BR.md) ·
[Français](README.fr.md) ·
[Deutsch](README.de.md) ·
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
> Esta traducción puede ir por detrás del [README en inglés](../README.md), que es la referencia.
> La documentación técnica para modders está solo en inglés, en [docs/](../docs/).

> [!IMPORTANT]
> Beta. La hice yo solo y solo la he probado en mi PC. Funciona, pero hay problemas abiertos en
> los que estoy atascado. Si sabes de programación gráfica, de DLSS/Streamline o de modding de
> shaders, tu ayuda es muy bienvenida: mira la sección «Dónde se necesita ayuda».

## Qué hace

Forza Horizon 6 trae DLSS Super Resolution, DLAA, Frame Generation y Multi Frame Generation, pero no
Ray Reconstruction, aunque la carpeta del juego ya incluye el plugin RR de Streamline (`sl.dlss_d.dll`)
y el modelo de RR (`nvngx_dlssd.dll`). Este add-on lo activa:

1. Añade DLSS-RR a las funciones que el juego le pide cargar a Streamline.
2. Cada vez que el juego evalúa DLSS Super Resolution, evalúa en su lugar DLSS Ray Reconstruction, con
   las mismas entradas. Cualquier frame al que le falte algo vuelve al DLSS normal del juego.
3. En cada frame construye, a partir del G-buffer de ray tracing del juego, los buffers guía que RR
   necesita: normales del mundo + rugosidad, albedo difuso, albedo especular y distancia de impacto
   difusa.
4. Puede cambiar los filtros de denoise del GI por ray tracing del propio juego: por defecto acumulan
   cuatro veces más historial («Historial x4», la imagen más estable), y en el modo «RR completo» se
   quitan para que RR limpie directamente la iluminación global cruda.

El ejecutable y los archivos del juego nunca se modifican. Todo ocurre en memoria, dentro de ReShade.

**Lo que ganas:** iluminación por ray tracing más limpia, con menos «hervor» y parpadeo en la luz
indirecta (paredes de noche, túneles, sombras). La geometría fina, como vallas y cables, suele verse
más nítida.

**Lo que cuesta:** algo de tiempo de GPU y de VRAM (ver «Rendimiento»).

## Comparación

Grabé estos clips en el juego, en mi PC (RTX 4050 Laptop GPU, 1920×1200, coche quieto). Muestran
píxeles reales a 1:1, recortados de la grabación. El modo de GI era **"RR completo"** (experimental),
no el modo por defecto "Historial x4".

**Túnel, vista de cabina.** Pulso F11 y RR se apaga: los guantes y el volante empiezan a hervir.

<p align="center"><img src="../docs/images/boiling-cockpit-tunnel-f11.gif" width="800" alt="Cabina en un túnel: RR encendido, luego F11 lo apaga y los guantes empiezan a hervir"></p>

**El mismo túnel, lado a lado** (izquierda: apagado, derecha: encendido).

<p align="center"><img src="../docs/images/boiling-cockpit-tunnel.gif" width="800" alt="Volante en un túnel, RR apagado a la izquierda y encendido a la derecha"></p>

**Cabina en una carretera de bosque.** Dos momentos de la misma toma, separados por 4 s, en bucle:
apagado y luego encendido. Con RR, los guantes, el pilar A y el tablero dejan de hervir, y los aros de
Audi del volante recuperan su reflejo metálico.

<p align="center"><img src="../docs/images/boiling-cockpit-forest.gif" width="800" alt="Cabina en una carretera de bosque, RR apagado y luego encendido"></p>

En el clip del bosque, el ruido temporal del guante derecho baja de 2,3 a 0,3–0,5 (medido sobre la
grabación). Con RR la cabina también se ve un poco más oscura, y todavía no sé cuál de las dos está más
cerca del brillo correcto. Fuera del coche, de día, la diferencia es mucho menor. Detalles en
[docs/MEASUREMENTS.md](../docs/MEASUREMENTS.md#from-a-gameplay-recording-7-oct-2026) (en inglés).

## Requisitos

- Una tarjeta **NVIDIA GeForce RTX**. Ray Reconstruction funciona en todas las generaciones RTX (series
  20, 30, 40 y 50), pero de momento solo la he probado en mi RTX 4050 Laptop (6 GB).
- **Forza Horizon 6** para PC, versión **6.440.853.0** (la versión con la que se desarrolló). En otras
  versiones RR debería seguir funcionando; los modos de GI necesitan que los shaders del juego no
  cambien, y el menú avisa cuando no es así.
- En las opciones de vídeo del juego: **DLSS** como escalador (cualquier modo de calidad) e
  **iluminación global por ray tracing** activada. Frame Generation y Multi Frame Generation pueden
  seguir activados.
- **ReShade 6.8.0 o posterior con soporte completo de add-ons** (en [reshade.me](https://reshade.me),
  la descarga «with full add-on support»).
- Los archivos de RR de Streamline del propio juego (`sl.dlss_d.dll`, `nvngx_dlssd.dll`), que vienen
  con el juego. Probado con Streamline 2.14.1 y DLSS-RR 310.9.0 / 310.9.1.

## Instalación

1. Instala **ReShade con soporte completo de add-ons** para `ForzaHorizon6.exe` y elige
   **DirectX 10/11/12**. No hace falta ningún shader de efectos.
2. Descarga `rr-forza-<versión>.zip` desde [Releases](https://github.com/Felix-37/fh6-ray-reconstruction/releases)
   y copia `rr-forza.addon64` en la carpeta del juego, junto a `ForzaHorizon6.exe`.
3. Abre el juego. En las opciones de vídeo, elige DLSS y activa la iluminación global por ray tracing.
4. Conduce unos segundos y abre el overlay de ReShade (tecla `Inicio` por defecto). En la pestaña
   **Ray Reconstruction**, el estado debe decir **«Ray Reconstruction activo»**. El idioma del menú se
   cambia en la pestaña **General** (Language / Idioma).

**Para desinstalar,** borra `rr-forza.addon64`. Sus ajustes están en `ReShade.ini`, sección
`[RR_Forza]`, y también puedes borrar esa sección.

## Uso

- **F11** cambia entre Ray Reconstruction y el DLSS normal del juego, para comparar.
- La pestaña **Ray Reconstruction** del overlay de ReShade tiene todos los ajustes. Cada ajuste indica
  si de verdad llegó a la GPU (`[OK]`, `[...]` o `[ERROR]`).
  - **General:** idioma (English / Español), RR sí/no, preset de RR (se recomienda F), afinado de RR.
  - **Guías:** cómo se describen las superficies a RR (tratamiento del follaje, metalness) y un
    diagnóstico.
  - **GI del juego:** qué pasa con los filtros del GI por ray tracing del juego (ver la tabla).
  - **Medición:** un banco de pruebas integrado que compara RR con el DLSS normal con números reales.
  - **Desarrollo:** capturas de frame, exportación de shaders del juego y teclas de desarrollo (F7,
    F10; apagadas por defecto).
  - **Ayuda:** teclas, archivos y límites conocidos.

| Modo de GI del juego | Qué hace |
|---|---|
| Original del juego | Los filtros RTGI del juego no se tocan y RR limpia su resultado. |
| Historial x2 | El filtro temporal del juego acumula 2 veces más frames: menos hervor, pero la luz reacciona más lento. |
| **Historial x4** (por defecto, recomendado) | Lo mismo con 4 veces más frames: la imagen más estable, con buena calidad y sin los problemas abiertos de RR completo. La luz indirecta reacciona algo más lento (posible estela en sombras que se mueven). |
| Sin filtro temporal | El GI crudo de cada frame va a RR. Experimental. |
| RR completo | Se sustituyen los filtros RTGI temporal y espacial del juego y RR hace todo el denoise. La mejor imagen de día (nada de hervor en la luz indirecta), pero con los problemas abiertos de «Problemas conocidos». Mientras la cámara se mueve, pasa solo a «Historial x4». Experimental. |

Los valores por defecto son mi recomendación, elegidos tras las mediciones de
[docs/MEASUREMENTS.md](../docs/MEASUREMENTS.md) y muchas horas conduciendo. «Restaurar valores por
defecto» (pestaña Guías) los recupera.

## Rendimiento

Medido en mi RTX 4050 Laptop, con salida a 1920×1200, resolución interna de 1280×800, Multi Frame
Generation 4x y límite de 138 fps:

- RR cuesta unos 0,9 ms de GPU más por frame mostrado que DLSS-SR en el mismo modo de calidad (−6 % de
  fps cuando la GPU es el límite).
- RR en DLSS **Equilibrado** costó lo mismo que el DLSS-SR del juego en **Calidad** (mismos fps, misma
  carga de GPU), sin pérdida visible. Es la combinación que recomiendo.
- Los dos modelos quedan cargados para que F11 cambie sin fallos. Eso usa unos cientos de MB más de
  VRAM, lo que va justo en tarjetas de 6 GB: ahí el juego ya avisa de «VRAM llena» sin RR.

## Problemas conocidos

Siguen abiertos; las pruebas reunidas hasta ahora están en
[docs/OPEN-PROBLEMS.md](../docs/OPEN-PROBLEMS.md) (en inglés):

1. **Ruido en movimiento (RR completo):** durante unos frames se ve ruido crudo al moverte o cuando algo
   vuelve a entrar en pantalla. «GI historial x4 en movimiento» y «Desoclusión asistida» lo reducen,
   pero no lo quitan.
2. Los **reflejos tipo espejo** (vidrio, cromados, pintura brillante) siguen con ruido, sobre todo con
   RR completo. Probablemente RR necesita vectores de movimiento especulares; la opción experimental
   «Vectores de movimiento especulares» (pestaña Guías, apagada por defecto) se los da con una
   distancia estimada, y aún no está medida.
3. **Destellos de noche (RR completo):** mucho menos frecuentes con el «Recorte de muestra aislada»,
   pero no desaparecen del todo.
4. Las **copas de los árboles contra un cielo brillante** pueden verse quemadas (demasiado claras) con RR.
5. **Hervor lento en túneles** con los filtros GI del juego (original o Historial x4): RR lo reduce
   (−17 a −22 %), pero no lo elimina; puede venir de las sombras RT y no del GI.

## Preguntas frecuentes

**¿Funciona en GPU AMD o Intel?** No. DLSS Ray Reconstruction solo funciona en GPU NVIDIA RTX.

**¿Me pueden banear?** No conozco ningún caso, pero no puedo prometer nada. El add-on no toca los
archivos del juego: engancha la biblioteca Streamline de NVIDIA y Direct3D 12 dentro del proceso del
juego, como hacen los add-ons de ReShade. Usarlo online es bajo tu propio riesgo.

**¿Funciona con otros mods?** Lo uso junto al add-on «MFG Unlock» de RenoDX sin problemas. Otras
combinaciones (OptiScaler, cambio de DLL de DLSS) no están probadas.

**La pestaña GI del juego dice que los filtros «no se vieron» o que una variante «no se creó».**
Conduce unos segundos primero. Si sigue igual, seguramente tu versión del juego tiene otros shaders.
Abre un issue con tu versión del juego y tu `ReShade.log`.

**¿Dónde está el log?** `ReShade.log`, en la carpeta del juego. Las líneas de este add-on empiezan con
`[RR Forza]`.

## Dónde se necesita ayuda

Este proyecto necesita a gente que sepa más que yo de algunos de estos temas:

- **Vectores de movimiento especulares y distancia de impacto especular para RR**, para arreglar los
  reflejos con ruido. El mod Control RR de speedlemur lo hizo. Los reflejos de Forza son consultas de
  rayos en línea más una pirámide de mips, descritos en [docs/FRAME-MAP.md](../docs/FRAME-MAP.md).
- **Ruido de desoclusión en RR completo:** cómo darle a RR un mejor punto de partida en los píxeles que
  no tienen historial.
- **Copas de árboles quemadas:** el follaje que mueve el viento no tiene vectores de movimiento
  propios. ¿Ayudaría una máscara de respuesta o de «color actual»?
- **Pruebas en otras GPU RTX**, resoluciones y versiones del juego, y **capturas o vídeos lado a lado**
  (mismo sitio, F11 alterna RR). Este README aún no tiene imágenes de comparación.

Empieza por [CONTRIBUTING.md](../CONTRIBUTING.md), luego [docs/ARCHITECTURE.md](../docs/ARCHITECTURE.md)
(cómo funciona el add-on) y [docs/MEASUREMENTS.md](../docs/MEASUREMENTS.md) (qué se probó, midió y
descartó, para que nadie lo repita). Para compilarlo solo hacen falta herramientas gratuitas:
[docs/BUILDING.md](../docs/BUILDING.md).

## Créditos

- **speedlemur**, por [Control Ray Reconstruction](https://github.com/speedlemur/renodx/tree/control-rr):
  la misma idea (responder a la evaluación DLSS-SR del juego con DLSS-RR) y la técnica de sustituir
  shaders en tiempo de ejecución.
- **crosire**, por [ReShade](https://github.com/crosire/reshade) y su API de add-ons.
- **clshortfuse y los colaboradores de RenoDX**, por [RenoDX](https://github.com/clshortfuse/renodx),
  cuya convención de hash de shaders se usa aquí.
- **NVIDIA**, por [Streamline](https://github.com/NVIDIA-RTX/Streamline) y DLSS.
- **Tsuda Kageyu** ([MinHook](https://github.com/TsudaKageyu/minhook)), **Omar Cornut**
  ([Dear ImGui](https://github.com/ocornut/imgui)) y **Microsoft**
  ([DirectX Shader Compiler](https://github.com/microsoft/DirectXShaderCompiler)).

Licencias de terceros: [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md).

## Aviso legal

Este es un proyecto no oficial hecho por fans. No está afiliado ni respaldado por Microsoft, Xbox Game
Studios, Playground Games ni NVIDIA. Forza Horizon es una marca de Microsoft. NVIDIA, RTX y DLSS son
marcas de NVIDIA Corporation.

El repositorio no contiene archivos del juego. Para ofrecer los modos de GI, la versión compilada
contiene versiones modificadas de dos compute shaders del juego, que no sirven sin el juego. Úsalo
bajo tu propio riesgo.

El código se publica bajo la [licencia MIT](../LICENSE).
