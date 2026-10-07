<div align="center">

# Ray Reconstruction pour Forza Horizon 6

**DLSS Ray Reconstruction (DLSS-RR) dans Forza Horizon 6 sur PC, sous forme d'add-on ReShade.**<br>
Un meilleur débruiteur pour l'éclairage en ray tracing du jeu, pour toutes les cartes NVIDIA RTX.

[English](../README.md) ·
[Español](README.es.md) ·
[Português (BR)](README.pt-BR.md) ·
**Français** ·
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
> Cette traduction peut être en retard sur le [README anglais](../README.md), qui fait référence.
> La documentation technique pour les moddeurs n'existe qu'en anglais, dans [docs/](../docs/). Le menu
> en jeu est en anglais (ou en espagnol) : les noms d'options ci-dessous sont donnés en anglais, comme
> dans le menu.

> [!IMPORTANT]
> Version bêta. Je l'ai développée seul et testée uniquement sur mon PC. Elle fonctionne, mais
> certains problèmes restent ouverts et je suis bloqué dessus. Si vous connaissez la programmation
> graphique, DLSS/Streamline ou le modding de shaders, votre aide est la bienvenue : voir la section
> « Où l'aide est nécessaire ».

## Ce que fait l'add-on

Forza Horizon 6 propose DLSS Super Resolution, DLAA, Frame Generation et Multi Frame Generation, mais
pas Ray Reconstruction, alors que le dossier du jeu contient déjà le plugin RR de Streamline
(`sl.dlss_d.dll`) et le modèle RR (`nvngx_dlssd.dll`). Cet add-on l'active :

1. Il ajoute DLSS-RR aux fonctionnalités que le jeu demande à Streamline de charger.
2. Chaque fois que le jeu évalue DLSS Super Resolution, il évalue DLSS Ray Reconstruction à la place,
   avec les mêmes entrées. Toute image à laquelle il manque quelque chose revient au DLSS normal du jeu.
3. À chaque image, il construit les buffers de guidage dont RR a besoin à partir du G-buffer de ray
   tracing du jeu : normales monde + rugosité, albédo diffus, albédo spéculaire et distance d'impact
   diffuse.
4. Il peut modifier les filtres de débruitage du GI en ray tracing du jeu : par défaut, ils accumulent
   quatre fois plus d'historique (« History x4 », l'image la plus stable), et en mode « Full RR » ils
   sont retirés pour que RR débruite lui-même l'illumination globale brute.

L'exécutable et les fichiers du jeu ne sont jamais modifiés. Tout se passe en mémoire, dans ReShade.

**Ce que vous gagnez :** un éclairage en ray tracing plus propre, avec moins de « bouillonnement » et
de scintillement dans la lumière indirecte (murs la nuit, tunnels, ombres). La géométrie fine, comme
les grillages et les câbles, est souvent plus nette.

**Ce que ça coûte :** un peu de temps GPU et de VRAM (voir « Performances »).

## Comparaison

J'ai enregistré ces clips dans le jeu, sur mon PC (RTX 4050 Laptop GPU, 1920×1200, voiture à
l'arrêt). Ils montrent de vrais pixels à 1:1, recadrés depuis l'enregistrement. Le mode de GI était
**« Full RR »** (expérimental), pas le mode par défaut « History x4 ».

**Tunnel, vue cockpit.** J'appuie sur F11 et RR se désactive : les gants et le volant se mettent à
bouillonner.

<p align="center"><img src="../docs/images/boiling-cockpit-tunnel-f11.gif" width="800" alt="Cockpit dans un tunnel : RR activé, puis F11 le désactive et les gants se mettent à bouillonner"></p>

**Le même tunnel, côte à côte** (à gauche : désactivé, à droite : activé).

<p align="center"><img src="../docs/images/boiling-cockpit-tunnel.gif" width="800" alt="Volant dans un tunnel, RR désactivé à gauche et activé à droite"></p>

**Cockpit sur une route en forêt.** Deux moments du même plan, à 4 s d'intervalle, en boucle :
désactivé, puis activé. Avec RR, les gants, le montant A et le tableau de bord ne bouillonnent plus, et
les anneaux Audi du volant retrouvent leur reflet métallique.

<p align="center"><img src="../docs/images/boiling-cockpit-forest.gif" width="800" alt="Cockpit sur une route en forêt, RR désactivé puis activé"></p>

Dans le clip de la forêt, le bruit temporel du gant droit passe de 2,3 à 0,3–0,5 (mesuré sur
l'enregistrement). Avec RR, le cockpit paraît aussi un peu plus sombre, et je ne sais pas encore
lequel des deux est le plus proche de la bonne luminosité. Hors de la voiture, de jour, la différence
est bien plus faible. Détails dans
[docs/MEASUREMENTS.md](../docs/MEASUREMENTS.md#from-a-gameplay-recording-7-oct-2026) (en anglais).

## Configuration requise

- Une carte **NVIDIA GeForce RTX**. Ray Reconstruction fonctionne sur toutes les générations RTX (séries
  20, 30, 40 et 50), mais pour l'instant je ne l'ai testé que sur ma RTX 4050 Laptop (6 Go).
- **Forza Horizon 6** sur PC, version **6.440.853.0** (la version utilisée pour le développement). Sur
  d'autres versions, RR devrait fonctionner ; les modes GI exigent que les shaders du jeu n'aient pas
  changé, et le menu vous prévient si ce n'est pas le cas.
- Dans les options vidéo du jeu : **DLSS** comme upscaler (n'importe quel mode de qualité) et
  **illumination globale en ray tracing** activée. Frame Generation et Multi Frame Generation peuvent
  rester activés.
- **ReShade 6.8.0 ou plus récent avec la prise en charge complète des add-ons** (sur
  [reshade.me](https://reshade.me), le téléchargement « with full add-on support »).
- Les fichiers RR de Streamline fournis avec le jeu (`sl.dlss_d.dll`, `nvngx_dlssd.dll`). Testé avec
  Streamline 2.14.1 et DLSS-RR 310.9.0 / 310.9.1.

## Installation

1. Installez **ReShade avec la prise en charge complète des add-ons** pour `ForzaHorizon6.exe` et
   choisissez **DirectX 10/11/12**. Aucun shader d'effet n'est nécessaire.
2. Téléchargez `rr-forza-<version>.zip` depuis les [Releases](https://github.com/Felix-37/fh6-ray-reconstruction/releases)
   et copiez `rr-forza.addon64` dans le dossier du jeu, à côté de `ForzaHorizon6.exe`.
3. Lancez le jeu. Dans les options vidéo, choisissez DLSS et activez l'illumination globale en ray
   tracing.
4. Roulez quelques secondes, puis ouvrez l'overlay ReShade (touche `Début`/`Home` par défaut). Dans
   l'onglet **Ray Reconstruction**, l'état doit indiquer **« Ray Reconstruction active »**.

**Pour désinstaller,** supprimez `rr-forza.addon64`. Ses réglages se trouvent dans `ReShade.ini`,
section `[RR_Forza]`, que vous pouvez aussi supprimer.

## Utilisation

- **F11** bascule entre Ray Reconstruction et le DLSS normal du jeu, pour comparer.
- L'onglet **Ray Reconstruction** de l'overlay ReShade contient tous les réglages. Chaque réglage
  indique s'il est vraiment arrivé jusqu'au GPU (`[OK]`, `[...]` ou `[ERROR]`).
  - **General :** langue (English / Español), RR activé ou non, preset RR (F recommandé), netteté RR.
  - **Guides :** la façon dont les surfaces sont décrites à RR (feuillage, metalness) et un diagnostic.
  - **Game GI :** ce qui arrive aux filtres GI en ray tracing du jeu (voir le tableau).
  - **Measurement :** un banc de mesure intégré qui compare RR au DLSS normal avec de vrais chiffres.
  - **Developer :** captures d'image, export des shaders du jeu et raccourcis de développement (F7,
    F10 ; désactivés par défaut).
  - **Help :** touches, fichiers et limites connues.

| Mode « Game GI » | Effet |
|---|---|
| Game original | Les filtres RTGI du jeu restent intacts et RR nettoie leur résultat. |
| History x2 | Le filtre temporel du jeu accumule 2 fois plus d'images : moins de bouillonnement, mais une lumière plus lente à réagir. |
| **History x4** (par défaut, recommandé) | La même chose avec 4 fois plus d'images : l'image la plus stable, de bonne qualité et sans les problèmes ouverts de Full RR. La lumière indirecte réagit un peu plus lentement (traînées possibles sur les ombres en mouvement). |
| No temporal filter | Le GI brut de chaque image va à RR. Expérimental. |
| Full RR | Les filtres RTGI temporel et spatial du jeu sont remplacés et RR fait tout le débruitage. La meilleure image de jour (aucun bouillonnement de la lumière indirecte), mais avec les problèmes ouverts listés dans « Problèmes connus ». Quand la caméra bouge, il passe automatiquement à « History x4 ». Expérimental. |

Les valeurs par défaut sont ma recommandation, choisies après les mesures de
[docs/MEASUREMENTS.md](../docs/MEASUREMENTS.md) et de nombreuses heures de conduite. « Restore defaults »
(onglet Guides) les rétablit.

## Performances

Mesuré sur ma RTX 4050 Laptop, sortie 1920×1200, rendu interne 1280×800, Multi Frame Generation 4x et
limite à 138 i/s :

- RR coûte environ 0,9 ms de GPU de plus par image affichée que DLSS-SR dans le même mode de qualité
  (−6 % d'i/s quand le GPU est la limite).
- RR en DLSS **Balanced** a coûté autant que le DLSS-SR du jeu en **Quality** (mêmes i/s, même charge
  GPU), sans perte visible. C'est la combinaison que je recommande.
- Les deux modèles restent chargés pour que F11 bascule proprement. Cela consomme quelques centaines
  de Mo de VRAM en plus, ce qui est juste sur les cartes de 6 Go : le jeu y signale déjà des
  avertissements « out of VRAM » sans RR.

## Problèmes connus

Toujours ouverts ; les éléments réunis jusqu'ici sont dans
[docs/OPEN-PROBLEMS.md](../docs/OPEN-PROBLEMS.md) (en anglais) :

1. **Bruit en mouvement (Full RR) :** pendant quelques images, du bruit brut apparaît quand vous vous
   déplacez ou quand quelque chose revient à l'écran. « GI history x4 while moving » et « Disocclusion
   assist » le réduisent sans le supprimer.
2. Les **reflets de type miroir** (vitres, chromes, peinture brillante) restent bruités, surtout en
   Full RR. RR a probablement besoin de vecteurs de mouvement spéculaires ; l'option expérimentale
   « Specular motion vectors » (onglet Guides, désactivée par défaut) les fournit avec une distance
   estimée, et n'est pas encore mesurée.
3. **Lucioles la nuit (Full RR) :** bien plus rares avec l'« Isolated sample clamp », mais pas à zéro.
4. Les **cimes des arbres sur un ciel clair** peuvent paraître brûlées (trop claires) avec RR.
5. **Bouillonnement lent dans les tunnels** avec les filtres GI du jeu (d'origine ou History x4) : RR
   le réduit (−17 à −22 %), sans le supprimer ; il vient peut-être des ombres RT et non du GI.

## FAQ

**Ça marche sur les GPU AMD ou Intel ?** Non. DLSS Ray Reconstruction ne fonctionne que sur les GPU
NVIDIA RTX.

**Puis-je être banni ?** Je ne connais aucun cas, mais je ne peux rien promettre. L'add-on ne touche pas
aux fichiers du jeu : il intercepte la bibliothèque Streamline de NVIDIA et Direct3D 12 dans le
processus du jeu, comme le font les add-ons ReShade. L'utilisation en ligne se fait à vos risques.

**Compatible avec d'autres mods ?** Je l'utilise avec l'add-on « MFG Unlock » de RenoDX sans
problème. Les autres combinaisons (OptiScaler, remplacement des DLL DLSS) n'ont pas été testées.

**L'onglet Game GI dit que les filtres n'ont « pas été vus » ou qu'une variante n'a « pas été créée ».**
Roulez quelques secondes d'abord. Si rien ne change, votre version du jeu a probablement d'autres
shaders. Ouvrez une issue avec votre version du jeu et votre `ReShade.log`.

**Où est le journal ?** `ReShade.log`, dans le dossier du jeu. Les lignes de cet add-on commencent par
`[RR Forza]`.

## Où l'aide est nécessaire

Ce projet a besoin de personnes qui en savent plus que moi sur certains de ces sujets :

- **Vecteurs de mouvement spéculaires et distance d'impact spéculaire pour RR**, pour corriger les
  reflets bruités. Le mod Control RR de speedlemur l'a fait. Les reflets de Forza reposent sur des
  requêtes de rayons inline et une pyramide de mips, décrites dans [docs/FRAME-MAP.md](../docs/FRAME-MAP.md).
- **Bruit de désocclusion en Full RR :** comment donner à RR un meilleur point de départ pour les
  pixels sans historique.
- **Cimes d'arbres brûlées :** le feuillage animé par le vent n'a pas ses propres vecteurs de mouvement.
  Un masque de réactivité ou de « couleur courante » aiderait-il ?
- **Des tests sur d'autres GPU RTX**, résolutions et versions du jeu, et des **captures ou vidéos côte à
  côte** (même endroit, F11 bascule RR). Ce README n'a pas encore d'images de comparaison.

Commencez par [CONTRIBUTING.md](../CONTRIBUTING.md), puis [docs/ARCHITECTURE.md](../docs/ARCHITECTURE.md)
(fonctionnement de l'add-on) et [docs/MEASUREMENTS.md](../docs/MEASUREMENTS.md) (ce qui a été essayé,
mesuré et écarté, pour que personne ne le refasse). La compilation n'exige que des outils gratuits :
[docs/BUILDING.md](../docs/BUILDING.md).

## Crédits

- **speedlemur**, pour [Control Ray Reconstruction](https://github.com/speedlemur/renodx/tree/control-rr) :
  la même idée (répondre à l'évaluation DLSS-SR du jeu par DLSS-RR) et la technique de remplacement de
  shaders à l'exécution.
- **crosire**, pour [ReShade](https://github.com/crosire/reshade) et son API d'add-ons.
- **clshortfuse et les contributeurs de RenoDX**, pour [RenoDX](https://github.com/clshortfuse/renodx),
  dont la convention de hachage des shaders est utilisée ici.
- **NVIDIA**, pour [Streamline](https://github.com/NVIDIA-RTX/Streamline) et DLSS.
- **Tsuda Kageyu** ([MinHook](https://github.com/TsudaKageyu/minhook)), **Omar Cornut**
  ([Dear ImGui](https://github.com/ocornut/imgui)) et **Microsoft**
  ([DirectX Shader Compiler](https://github.com/microsoft/DirectXShaderCompiler)).

Licences tierces : [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md).

## Mentions légales

Ceci est un projet de fan non officiel. Il n'est ni affilié ni approuvé par Microsoft, Xbox Game
Studios, Playground Games ou NVIDIA. Forza Horizon est une marque de Microsoft. NVIDIA, RTX et DLSS sont
des marques de NVIDIA Corporation.

Le dépôt ne contient aucun fichier du jeu. Pour proposer les modes GI, la version compilée contient des
versions modifiées de deux compute shaders du jeu, inutiles sans le jeu. Utilisation à vos risques.

Le code est publié sous [licence MIT](../LICENSE).
