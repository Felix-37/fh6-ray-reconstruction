<div align="center">

# Forza Horizon 6 用 Ray Reconstruction

**PC 版 Forza Horizon 6 で DLSS Ray Reconstruction（DLSS-RR）を使えるようにする ReShade アドオン。**<br>
ゲームのレイトレーシング照明のための、より優れたデノイザー。すべての NVIDIA RTX グラフィックスカードに対応。

[English](../README.md) ·
[Español](README.es.md) ·
[Português (BR)](README.pt-BR.md) ·
[Français](README.fr.md) ·
[Deutsch](README.de.md) ·
[Italiano](README.it.md) ·
[Polski](README.pl.md) ·
[Русский](README.ru.md) ·
[Türkçe](README.tr.md) ·
**日本語** ·
[한국어](README.ko.md) ·
[简体中文](README.zh-CN.md)

![Status: beta](https://img.shields.io/badge/status-beta-orange)
![Forza Horizon 6](https://img.shields.io/badge/Forza%20Horizon%206-6.440.853.0-0a7cff)
![NVIDIA RTX](https://img.shields.io/badge/GPU-NVIDIA%20RTX-76b900)
![ReShade 6.8+](https://img.shields.io/badge/ReShade-6.8%2B%20add--on-8a2be2)
![License: MIT](https://img.shields.io/badge/license-MIT-green)

</div>

> [!NOTE]
> この翻訳は基準となる[英語版 README](../README.md) より古い場合があります。モッダー向けの技術資料は
> 英語のみで、[docs/](../docs/) にあります。ゲーム内メニューは英語（またはスペイン語）なので、以下の
> 項目名はメニューと同じ英語表記にしています。

> [!IMPORTANT]
> ベータ版です。私が 1 人で作り、自分の PC でしかテストしていません。動作はしますが、未解決の問題が残って
> おり、行き詰まっています。グラフィックスプログラミング、DLSS/Streamline、シェーダー改造に詳しい方の
> 協力を心から歓迎します。「協力が必要なところ」を参照してください。

## 何をするのか

Forza Horizon 6 は DLSS Super Resolution、DLAA、Frame Generation、Multi Frame Generation に対応して
いますが、Ray Reconstruction には対応していません。それでもゲームのフォルダーには Streamline の RR
プラグイン（`sl.dlss_d.dll`）と RR モデル（`nvngx_dlssd.dll`）がすでに入っています。このアドオンは
それを有効にします。

1. ゲームが Streamline に読み込ませる機能の一覧に DLSS-RR を追加します。
2. ゲームが DLSS Super Resolution を実行するたびに、同じ入力で代わりに DLSS Ray Reconstruction を
   実行します。何かが欠けているフレームは、ゲーム本来の DLSS に戻ります。
3. 毎フレーム、ゲームのレイトレーシング用 G バッファーから RR に必要なガイドバッファーを作ります。
   ワールド法線 + ラフネス、ディフューズアルベド、スペキュラーアルベド、ディフューズのヒット距離です。
4. ゲーム自身のレイトレーシング GI デノイズフィルターを変更できます。既定では 4 倍の履歴を蓄積させ
   （「History x4」、最も安定した画像）、「Full RR」モードではフィルターを外して、生のグローバル
   イルミネーションを RR 自身にデノイズさせます。

ゲームの実行ファイルやファイルは一切変更しません。すべては ReShade の中、メモリ上で行われます。

**得られるもの:** 間接光の「沸き立ち」やちらつきが少ない、きれいなレイトレーシング照明（夜の壁、
トンネル、影など）。フェンスや電線のような細いジオメトリも多くの場合よりシャープになります。

**代償:** GPU 時間と VRAM を少し使います（「パフォーマンス」を参照）。

## 比較

これらのクリップは私の PC でゲーム内で録画したものです（RTX 4050 Laptop GPU、1920×1200、車は停止）。
録画から切り出した実際のピクセルを等倍（1:1）で表示しています。GI モードはデフォルトの「History x4」ではなく、
**「Full RR」**（実験的）でした。

**トンネル、コックピット視点。** F11 を押すと RR がオフになり、グローブとステアリングが「沸騰」し始めます。

<p align="center"><img src="../docs/images/boiling-cockpit-tunnel-f11.gif" width="800" alt="トンネル内のコックピット：RR オン、その後 F11 でオフにするとグローブが沸騰し始める"></p>

**同じトンネル、左右比較**（左：オフ、右：オン）。

<p align="center"><img src="../docs/images/boiling-cockpit-tunnel.gif" width="800" alt="トンネル内のステアリング、左は RR オフ、右は RR オン"></p>

**森の道でのコックピット。** 同じショットの 4 秒離れた 2 つの瞬間をループしています：オフ、次にオン。
RR を使うと、グローブ、A ピラー、ダッシュボードの沸騰が止まり、ステアリングの Audi のリングに金属的な反射が戻ります。

<p align="center"><img src="../docs/images/boiling-cockpit-forest.gif" width="800" alt="森の道でのコックピット、RR オフ、次にオン"></p>

森のクリップでは、右グローブの時間的ノイズが 2.3 から 0.3–0.5 に下がります（録画上で測定）。RR ではコックピットが
少し暗くも見えますが、どちらが正しい明るさに近いのかはまだ分かりません。車の外、昼間では、差はずっと小さくなります。
詳細は [docs/MEASUREMENTS.md](../docs/MEASUREMENTS.md#from-a-gameplay-recording-7-oct-2026)（英語）にあります。

## 動作要件

- **NVIDIA GeForce RTX** グラフィックスカード。Ray Reconstruction はすべての RTX 世代（20、30、40、
  50 シリーズ）で動作しますが、私がテストしたのは今のところ自分の RTX 4050 Laptop（6 GB）だけです。
- PC 版 **Forza Horizon 6**、バージョン **6.440.853.0**（開発に使ったバージョン）。他のバージョンでも
  RR 自体は動くはずです。GI モードはゲームのシェーダーが変わっていないことが前提で、変わっている場合は
  メニューに表示されます。
- ゲームのグラフィック設定で、アップスケーラーに **DLSS**（品質モードは任意）を選び、**レイトレーシング
  によるグローバルイルミネーション**を有効にします。Frame Generation と Multi Frame Generation は有効の
  ままで構いません。
- **フルアドオン対応の ReShade 6.8.0 以降**（[reshade.me](https://reshade.me) の
  「with full add-on support」版）。
- ゲームに同梱されている Streamline の RR ファイル（`sl.dlss_d.dll`、`nvngx_dlssd.dll`）。
  Streamline 2.14.1 と DLSS-RR 310.9.0 / 310.9.1 でテスト済みです。

## インストール

1. `ForzaHorizon6.exe` に **フルアドオン対応の ReShade** をインストールし、**DirectX 10/11/12** を
   選びます。エフェクトシェーダーは不要です。
2. [Releases](https://github.com/Felix-37/fh6-ray-reconstruction/releases) から
   `rr-forza-<バージョン>.zip` をダウンロードし、`rr-forza.addon64` をゲームフォルダー
   （`ForzaHorizon6.exe` と同じ場所）にコピーします。
3. ゲームを起動し、グラフィック設定で DLSS を選んでレイトレーシング GI を有効にします。
4. 数秒走ってから ReShade のオーバーレイを開きます（既定は `Home` キー）。**Ray Reconstruction** タブの
   状態が **「Ray Reconstruction active」** になっていれば成功です。

**アンインストール**は `rr-forza.addon64` を削除するだけです。設定は `ReShade.ini` の `[RR_Forza]`
セクションにあり、そのセクションも削除できます。

## 使い方

- **F11** で Ray Reconstruction とゲーム本来の DLSS を切り替えて比較できます。
- ReShade オーバーレイの **Ray Reconstruction** タブにすべての設定があります。各設定は、実際に GPU まで
  届いたかどうかを表示します（`[OK]`、`[...]`、`[ERROR]`）。
  - **General:** 言語（English / Español）、RR のオン／オフ、RR プリセット（F 推奨）、RR のシャープ化。
  - **Guides:** 表面を RR にどう伝えるか（植生の扱い、メタルネス）と診断機能。
  - **Game GI:** ゲームのレイトレーシング GI フィルターをどう扱うか（下の表を参照）。
  - **Measurement:** RR と通常の DLSS を実際の数値で比較する内蔵ベンチ。
  - **Developer:** フレームキャプチャ、ゲームシェーダーの書き出し、開発者用ホットキー（F7、F10。既定はオフ）。
  - **Help:** キー、ファイル、既知の制限。

| 「Game GI」モード | 内容 |
|---|---|
| Game original | ゲームの RTGI フィルターはそのままで、その結果を RR がきれいにします。 |
| History x2 | ゲームの時間フィルターが 2 倍のフレームを蓄積します。沸き立ちは減りますが、光の反応が遅くなります。 |
| **History x4**（既定・推奨） | 同じく 4 倍のフレームを蓄積します。最も安定した画像で、画質も良く、Full RR の未解決の問題がありません。間接光の反応は少し遅くなります（動く影に尾を引くことがあります）。 |
| No temporal filter | 各フレームの生の GI を RR に渡します。実験的。 |
| Full RR | ゲームの時間・空間 RTGI フィルターを置き換え、デノイズをすべて RR が行います。昼間は最も良い画像（間接光の沸き立ちがまったくない）ですが、「既知の問題」にある未解決の問題があります。カメラが動いている間は自動的に「History x4」に切り替わります。実験的。 |

既定値は私のおすすめで、[docs/MEASUREMENTS.md](../docs/MEASUREMENTS.md) の計測結果と長時間の
走行をもとに選んでいます。「Restore defaults」（Guides タブ）で元に戻せます。

## パフォーマンス

私の RTX 4050 Laptop、出力 1920×1200、内部解像度 1280×800、Multi Frame Generation 4x、138 fps 上限で計測:

- RR は同じ品質モードの DLSS-SR より、表示フレームあたり約 0.9 ms 多く GPU 時間を使います（GPU が
  ボトルネックのとき fps は −6 %）。
- DLSS **Balanced** の RR は、ゲームの **Quality** の DLSS-SR とほぼ同じ負荷でした（同じ fps、同じ GPU
  負荷）。見た目の劣化もありませんでした。私のおすすめの組み合わせです。
- F11 で問題なく切り替えられるよう、両方のモデルを読み込んだままにしています。そのため VRAM を
  数百 MB 余分に使い、6 GB のカードでは余裕がありません。そうしたカードでは RR なしでもゲームが
  「out of VRAM」の警告を出します。

## 既知の問題

未解決です。これまでに集めたデータは [docs/OPEN-PROBLEMS.md](../docs/OPEN-PROBLEMS.md)（英語）にあります。

1. **動いているときのノイズ（Full RR）:** 移動したときや、何かが再び画面に入ったとき、数フレームの間
   生のノイズが見えます。「GI history x4 while moving」と「Disocclusion assist」で減りますが、
   消えはしません。
2. **鏡のような反射**（ガラス、クローム、光沢塗装）は、特に Full RR でノイズが残ります。RR には
   おそらくスペキュラーのモーションベクトルが必要です。実験的なオプション「Specular motion vectors」
   （Guides タブ、既定はオフ）が推定距離でそれを渡しますが、まだ計測していません。
3. **夜のファイアフライ（Full RR）:**「Isolated sample clamp」でかなり減りますが、ゼロではありません。
4. **明るい空を背景にした木の梢**が、RR では白飛びして見えることがあります。
5. ゲームの GI フィルター（オリジナルまたは History x4）での **トンネル内のゆっくりした沸き立ち**:
   RR で 17〜22 % 減りますが、なくなりはしません。GI ではなく RT シャドウが原因かもしれません。

## よくある質問

**AMD や Intel の GPU で動きますか？** いいえ。DLSS Ray Reconstruction は NVIDIA RTX GPU でしか動き
ません。

**BAN されることはありますか？** そうした例は知りませんが、何も保証できません。このアドオンはゲームの
ファイルには触れず、ReShade のアドオンと同じように、ゲームのプロセス内で NVIDIA の Streamline
ライブラリと Direct3D 12 をフックします。オンラインでの使用は自己責任です。

**他の MOD と一緒に使えますか？** 私は RenoDX の「MFG Unlock」アドオンと問題なく併用しています。
それ以外の組み合わせ（OptiScaler、DLSS DLL の差し替えなど）は未テストです。

**Game GI タブに、フィルターが「見つからない」、またはバリアントが「作成されていない」と表示されます。**
まず数秒走ってください。それでも変わらない場合、お使いのゲームバージョンはシェーダーが異なる可能性が
高いです。ゲームのバージョンと `ReShade.log` を添えて issue を作成してください。

**ログはどこですか？** ゲームフォルダーの `ReShade.log` です。このアドオンの行は `[RR Forza]` で始まります。

## 協力が必要なところ

このプロジェクトには、次の分野で私より詳しい方の力が必要です。

- ノイズの残る反射を直すための、**RR 用スペキュラーモーションベクトルとスペキュラーヒット距離**。
  speedlemur の Control RR MOD はこれを実現しています。Forza の反射はインラインのレイクエリと
  ミップピラミッドでできており、[docs/FRAME-MAP.md](../docs/FRAME-MAP.md) に説明があります。
- **Full RR のディスオクルージョンノイズ:** 履歴のないピクセルに対して、RR により良い出発点を与える方法。
- **白飛びする木の梢:** 風で揺れる植生には専用のモーションベクトルがありません。レスポンシブネスや
  「現在色」のマスクは効果があるでしょうか？
- **他の RTX GPU**、解像度、ゲームバージョンでの**テスト**と、**比較用のスクリーンショットや動画**
  （同じ場所で F11 により RR を切り替えたもの）。この README にはまだ比較画像がありません。

まず [CONTRIBUTING.md](../CONTRIBUTING.md)、次に [docs/ARCHITECTURE.md](../docs/ARCHITECTURE.md)
（アドオンの仕組み）と [docs/MEASUREMENTS.md](../docs/MEASUREMENTS.md)（試して、計測して、見送った
こと。同じことを繰り返さないために）を読んでください。ビルドに必要なのは無料のツールだけです:
[docs/BUILDING.md](../docs/BUILDING.md)。

## クレジット

- **speedlemur** — [Control Ray Reconstruction](https://github.com/speedlemur/renodx/tree/control-rr)。
  同じ発想（ゲームの DLSS-SR 呼び出しに DLSS-RR で応える）と、実行時にシェーダーを置き換える手法。
- **crosire** — [ReShade](https://github.com/crosire/reshade) とそのアドオン API。
- **clshortfuse と RenoDX の貢献者の皆さん** — [RenoDX](https://github.com/clshortfuse/renodx)。
  シェーダーのハッシュ規則をここで使っています。
- **NVIDIA** — [Streamline](https://github.com/NVIDIA-RTX/Streamline) と DLSS。
- **Tsuda Kageyu**（[MinHook](https://github.com/TsudaKageyu/minhook)）、**Omar Cornut**
  （[Dear ImGui](https://github.com/ocornut/imgui)）、**Microsoft**
  （[DirectX Shader Compiler](https://github.com/microsoft/DirectXShaderCompiler)）。

サードパーティのライセンス: [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md)。

## 法的事項

これは非公式のファンプロジェクトです。Microsoft、Xbox Game Studios、Playground Games、NVIDIA とは
関係がなく、承認も受けていません。Forza Horizon は Microsoft の商標です。NVIDIA、RTX、DLSS は NVIDIA
Corporation の商標です。

このリポジトリにはゲームのファイルは含まれていません。GI モードを提供するため、配布ビルドにはゲームの
コンピュートシェーダー 2 つを改変したものが含まれますが、ゲームなしでは役に立ちません。自己責任で
ご利用ください。

コードは [MIT ライセンス](../LICENSE)で公開しています。
