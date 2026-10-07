<div align="center">

# Forza Horizon 6 的 Ray Reconstruction

**以 ReShade 插件（add-on）形式，在 PC 版《极限竞速：地平线 6》中启用 DLSS Ray Reconstruction（DLSS-RR）。**<br>
为游戏的光线追踪照明提供更好的降噪器，适用于所有 NVIDIA RTX 显卡。

[English](../README.md) ·
[Español](README.es.md) ·
[Português (BR)](README.pt-BR.md) ·
[Français](README.fr.md) ·
[Deutsch](README.de.md) ·
[Italiano](README.it.md) ·
[Polski](README.pl.md) ·
[Русский](README.ru.md) ·
[Türkçe](README.tr.md) ·
[日本語](README.ja.md) ·
[한국어](README.ko.md) ·
**简体中文**

![Status: beta](https://img.shields.io/badge/status-beta-orange)
![Forza Horizon 6](https://img.shields.io/badge/Forza%20Horizon%206-6.440.853.0-0a7cff)
![NVIDIA RTX](https://img.shields.io/badge/GPU-NVIDIA%20RTX-76b900)
![ReShade 6.8+](https://img.shields.io/badge/ReShade-6.8%2B%20add--on-8a2be2)
![License: MIT](https://img.shields.io/badge/license-MIT-green)

</div>

> [!NOTE]
> 本译文可能落后于作为基准的[英文 README](../README.md)。面向模组作者的技术文档只有英文版，位于
> [docs/](../docs/)。游戏内菜单为英文（或西班牙文），因此下文的选项名称与菜单一致，保留英文。

> [!IMPORTANT]
> 测试版（beta），由我一个人开发，只在我自己的电脑上测试过。它能正常工作，但仍有一些未解决的问题，我卡在
> 了这里。如果你了解图形编程、DLSS/Streamline 或着色器修改，非常欢迎你的帮助：请看“需要帮助的地方”。

## 它做什么

《极限竞速：地平线 6》支持 DLSS Super Resolution、DLAA、Frame Generation 和 Multi Frame Generation，
但不支持 Ray Reconstruction，尽管游戏目录里已经有 Streamline 的 RR 插件（`sl.dlss_d.dll`）和 RR 模型
（`nvngx_dlssd.dll`）。这个插件会启用它：

1. 把 DLSS-RR 加入游戏要求 Streamline 加载的功能列表。
2. 每当游戏执行 DLSS Super Resolution 时，改为用相同的输入执行 DLSS Ray Reconstruction。任何缺少
   数据的帧都会退回到游戏原本的 DLSS。
3. 每一帧都从游戏的光线追踪 G-buffer 构建 RR 所需的引导缓冲：世界空间法线 + 粗糙度、漫反射反照率、
   镜面反照率，以及漫反射命中距离。
4. 可以修改游戏自带的光线追踪 GI 降噪滤波器：默认让它们累积 4 倍的历史（“History x4”，最稳定的
   画面）；在“Full RR”模式下则移除这些滤波器，让 RR 直接对原始全局光照降噪。

游戏的可执行文件和文件都不会被修改。一切都在内存中、在 ReShade 内部完成。

**你会得到：** 更干净的光线追踪照明，间接光的“沸腾”和闪烁更少（夜晚的墙面、隧道、阴影）。
栅栏、电线等细小几何体通常也更清晰。

**代价：** 少量 GPU 时间和显存（见“性能”）。

## 对比

这些片段是我在自己的 PC 上于游戏内录制的（RTX 4050 Laptop GPU，1920×1200，车辆静止）。
它们以 1:1 显示从录像中裁剪出的真实像素。GI 模式是 **“Full RR”**（实验性），而不是默认的“History x4”。

**隧道，驾驶舱视角。** 我按下 F11，RR 关闭：手套和方向盘开始“沸腾”。

<p align="center"><img src="../docs/images/boiling-cockpit-tunnel-f11.gif" width="800" alt="隧道中的驾驶舱：RR 开启，然后 F11 将其关闭，手套开始沸腾"></p>

**同一条隧道，左右对比**（左：关闭，右：开启）。

<p align="center"><img src="../docs/images/boiling-cockpit-tunnel.gif" width="800" alt="隧道中的方向盘，左边 RR 关闭，右边 RR 开启"></p>

**森林道路上的驾驶舱。** 同一镜头相隔 4 秒的两个时刻，循环播放：先关闭，再开启。开启 RR 后，
手套、A 柱和仪表台不再沸腾，方向盘上的 Audi 四环也恢复了金属反光。

<p align="center"><img src="../docs/images/boiling-cockpit-forest.gif" width="800" alt="森林道路上的驾驶舱，RR 先关闭再开启"></p>

在森林片段中，右手套的时间噪声从 2.3 降到 0.3–0.5（在录像上测量）。开启 RR 后驾驶舱看起来也稍暗一些，
我还不知道两者中哪一个更接近正确的亮度。在车外、白天，差别要小得多。详情见
[docs/MEASUREMENTS.md](../docs/MEASUREMENTS.md#from-a-gameplay-recording-7-oct-2026)（英文）。

## 需求

- 一张 **NVIDIA GeForce RTX** 显卡。Ray Reconstruction 可在所有 RTX 世代（20、30、40、50 系列）上
  运行，但目前我只在自己的 RTX 4050 Laptop（6 GB）上测试过。
- PC 版 **Forza Horizon 6**，版本 **6.440.853.0**（开发时使用的版本）。在其他版本上 RR 本身应该仍能
  工作；GI 模式要求游戏着色器没有变化，若有变化，菜单会提示。
- 游戏画面设置中：放大器选择 **DLSS**（任意质量模式），并开启**光线追踪全局光照**。Frame Generation
  和 Multi Frame Generation 可以保持开启。
- **带完整插件支持的 ReShade 6.8.0 或更新版本**（[reshade.me](https://reshade.me) 上的
  “with full add-on support”下载）。
- 游戏自带的 Streamline RR 文件（`sl.dlss_d.dll`、`nvngx_dlssd.dll`）。已在 Streamline 2.14.1 和
  DLSS-RR 310.9.0 / 310.9.1 上测试。

## 安装

1. 为 `ForzaHorizon6.exe` 安装**带完整插件支持的 ReShade**，并选择 **DirectX 10/11/12**。
   不需要任何效果着色器。
2. 从 [Releases](https://github.com/Felix-37/fh6-ray-reconstruction/releases) 下载
   `rr-forza-<版本>.zip`，把 `rr-forza.addon64` 复制到游戏目录中，与 `ForzaHorizon6.exe` 放在一起。
3. 启动游戏。在画面设置中选择 DLSS 并开启光线追踪全局光照。
4. 开几秒车，然后打开 ReShade 叠加界面（默认 `Home` 键）。在 **Ray Reconstruction** 标签页中，
   状态应显示 **“Ray Reconstruction active”**。

**卸载时**删除 `rr-forza.addon64` 即可。设置保存在 `ReShade.ini` 的 `[RR_Forza]` 段中，也可以一并删除。

## 使用

- 按 **F11** 可在 Ray Reconstruction 和游戏原本的 DLSS 之间切换，方便对比。
- ReShade 叠加界面的 **Ray Reconstruction** 标签页包含所有设置。每项设置都会显示它是否真的已应用到
  GPU（`[OK]`、`[...]` 或 `[ERROR]`）。
  - **General：** 语言（English / Español）、RR 开关、RR 预设（推荐 F）、RR 锐化。
  - **Guides：** 如何向 RR 描述表面（植被处理、金属度），以及一个诊断选项。
  - **Game GI：** 如何处理游戏的光线追踪 GI 滤波器（见下表）。
  - **Measurement：** 内置测试台，用真实数据比较 RR 与普通 DLSS。
  - **Developer：** 帧捕获、导出游戏着色器、开发者快捷键（F7、F10，默认关闭）。
  - **Help：** 按键、文件和已知限制。

| “Game GI”模式 | 作用 |
|---|---|
| Game original | 保留游戏的 RTGI 滤波器，由 RR 清理其结果。 |
| History x2 | 游戏的时域滤波器累积 2 倍的帧：沸腾更少，但光照响应更慢。 |
| **History x4**（默认，推荐） | 同上，累积 4 倍的帧：最稳定的画面，画质良好，也没有 Full RR 的未解决问题。间接光的响应稍慢（移动的阴影可能出现拖影）。 |
| No temporal filter | 每一帧的原始 GI 直接交给 RR。实验性。 |
| Full RR | 替换游戏的时域和空域 RTGI 滤波器，降噪全部由 RR 完成。白天画面最好（间接光完全没有沸腾），但存在“已知问题”中列出的未解决问题。镜头移动时会自动切换到“History x4”。实验性。 |

默认值是我的推荐设置，依据 [docs/MEASUREMENTS.md](../docs/MEASUREMENTS.md) 中的测量结果和长时间的
驾驶选定。“Restore defaults”（Guides 标签页）可恢复默认值。

## 性能

在我的 RTX 4050 Laptop 上测量，输出 1920×1200，内部分辨率 1280×800，Multi Frame Generation 4x，
帧率上限 138 fps：

- 与相同质量模式下的 DLSS-SR 相比，RR 每个显示帧多消耗约 0.9 ms 的 GPU 时间（GPU 为瓶颈时帧率 −6 %）。
- DLSS **Balanced** 模式下的 RR，开销与游戏 **Quality** 模式的 DLSS-SR 相当（相同帧率、相同 GPU
  负载），且没有可见的画质损失。这是我推荐的组合。
- 为了让 F11 切换干净利落，两个模型都保持加载，因此会多占用几百 MB 显存，在 6 GB 显卡上比较紧张：
  在这类显卡上，即使不用 RR，游戏也已经会报出“out of VRAM”警告。

## 已知问题

仍未解决；目前收集到的证据见 [docs/OPEN-PROBLEMS.md](../docs/OPEN-PROBLEMS.md)（英文）：

1. **运动中的噪点（Full RR）：** 移动时，或物体重新进入画面时，会有几帧看到原始噪点。
   “GI history x4 while moving”和“Disocclusion assist”可以减轻，但无法消除。
2. **镜面般的反射**（玻璃、镀铬、亮面车漆）仍有噪点，在 Full RR 下尤其明显。RR 可能需要镜面运动
   矢量；实验性选项“Specular motion vectors”（Guides 标签页，默认关闭）会用估算的距离提供它们，
   但尚未测量。
3. **夜晚的萤火虫噪点（Full RR）：** 开启“Isolated sample clamp”后少了很多，但不是零。
4. **明亮天空前的树冠**在 RR 下可能显得过曝。
5. 使用游戏的 GI 滤波器（原始或 History x4）时**隧道中缓慢的沸腾**：RR 能减少 17–22 %，但无法
   消除；它可能来自 RT 阴影而不是 GI。

## 常见问题

**AMD 或 Intel 显卡能用吗？** 不能。DLSS Ray Reconstruction 只能在 NVIDIA RTX 显卡上运行。

**会被封号吗？** 我没听说过这样的案例，但无法做任何保证。本插件不修改游戏文件：它像其他 ReShade
插件一样，在游戏进程内挂钩 NVIDIA 的 Streamline 库和 Direct3D 12。在线使用风险自负。

**能和其他模组一起用吗？** 我一直与 RenoDX 的“MFG Unlock”插件一起使用，没有问题。其他组合（OptiScaler、
替换 DLSS DLL 等）未经测试。

**Game GI 标签页显示滤波器“未发现”或某个变体“未创建”。** 先开几秒车。如果仍然如此，你的游戏版本很可能
使用了不同的着色器。请附上游戏版本和你的 `ReShade.log` 提交 issue。

**日志在哪里？** 游戏目录中的 `ReShade.log`。本插件的日志行以 `[RR Forza]` 开头。

## 需要帮助的地方

这个项目需要在以下方面比我更懂的人：

- 用于修复噪点反射的 **RR 镜面运动矢量和镜面命中距离**。speedlemur 的 Control RR 模组实现过。
  Forza 的反射由内联光线查询加 mip 金字塔构成，详见 [docs/FRAME-MAP.md](../docs/FRAME-MAP.md)。
- **Full RR 中的去遮挡噪点：** 如何为没有历史的像素给 RR 一个更好的起点。
- **过曝的树冠：** 随风摆动的植被没有自己的运动矢量。响应度遮罩或“当前颜色”遮罩会有帮助吗？
- 在**其他 RTX 显卡**、分辨率和游戏版本上的**测试**，以及**对比截图或视频**（同一地点，按 F11 切换
  RR）。本 README 目前还没有对比图。

请先阅读 [CONTRIBUTING.md](../CONTRIBUTING.md)，然后是 [docs/ARCHITECTURE.md](../docs/ARCHITECTURE.md)
（插件如何工作）和 [docs/MEASUREMENTS.md](../docs/MEASUREMENTS.md)（尝试过、测量过并放弃的方案，
避免重复）。编译只需要免费工具：[docs/BUILDING.md](../docs/BUILDING.md)。

## 致谢

- **speedlemur**：[Control Ray Reconstruction](https://github.com/speedlemur/renodx/tree/control-rr)，
  同样的思路（用 DLSS-RR 响应游戏的 DLSS-SR 调用），以及运行时替换着色器的技术。
- **crosire**：[ReShade](https://github.com/crosire/reshade) 及其插件 API。
- **clshortfuse 和 RenoDX 的贡献者们**：[RenoDX](https://github.com/clshortfuse/renodx)，本项目沿用了
  其着色器哈希约定。
- **NVIDIA**：[Streamline](https://github.com/NVIDIA-RTX/Streamline) 和 DLSS。
- **Tsuda Kageyu**（[MinHook](https://github.com/TsudaKageyu/minhook)）、**Omar Cornut**
  （[Dear ImGui](https://github.com/ocornut/imgui)）和 **Microsoft**
  （[DirectX Shader Compiler](https://github.com/microsoft/DirectXShaderCompiler)）。

第三方许可证：[THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md)。

## 法律声明

这是一个非官方的粉丝项目，与 Microsoft、Xbox Game Studios、Playground Games 或 NVIDIA 无关，也未获得
它们的认可。Forza Horizon 是 Microsoft 的商标。NVIDIA、RTX 和 DLSS 是 NVIDIA Corporation 的商标。

本仓库不包含任何游戏文件。为提供 GI 模式，发布版本中包含游戏的两个计算着色器的修改版本，离开游戏
毫无用处。使用风险自负。

代码以 [MIT 许可证](../LICENSE)发布。
