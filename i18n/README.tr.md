<div align="center">

# Forza Horizon 6 için Ray Reconstruction

**PC'de Forza Horizon 6 için DLSS Ray Reconstruction (DLSS-RR), bir ReShade eklentisi (add-on) olarak.**<br>
Oyunun ışın izlemeli aydınlatması için daha iyi bir gürültü giderici, tüm NVIDIA RTX ekran kartları için.

[English](../README.md) ·
[Español](README.es.md) ·
[Português (BR)](README.pt-BR.md) ·
[Français](README.fr.md) ·
[Deutsch](README.de.md) ·
[Italiano](README.it.md) ·
[Polski](README.pl.md) ·
[Русский](README.ru.md) ·
**Türkçe** ·
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
> Bu çeviri, esas alınan [İngilizce README](../README.md) dosyasının gerisinde kalabilir. Modcular için
> teknik belgeler yalnızca İngilizcedir ve [docs/](../docs/) klasöründedir. Oyun içi menü İngilizce (veya
> İspanyolca) olduğundan aşağıdaki seçenek adları menüdeki gibi İngilizce verilmiştir.

> [!IMPORTANT]
> Beta sürümü; tek başıma yaptım ve yalnızca kendi bilgisayarımda test ettim. Çalışıyor, ancak
> bazı sorunlar hâlâ açık ve bunlarda takıldım. Grafik programlama, DLSS/Streamline ya da shader
> modlama biliyorsanız yardımınız çok değerli: "Nerede yardım gerekiyor" bölümüne bakın.

## Ne yapar

Forza Horizon 6; DLSS Super Resolution, DLAA, Frame Generation ve Multi Frame Generation sunuyor, ancak
oyun klasöründe Streamline RR eklentisi (`sl.dlss_d.dll`) ve RR modeli (`nvngx_dlssd.dll`) zaten
bulunmasına rağmen Ray Reconstruction sunmuyor. Bu eklenti onu etkinleştirir:

1. Oyunun Streamline'dan yüklemesini istediği özelliklere DLSS-RR'yi ekler.
2. Oyun DLSS Super Resolution'ı her çalıştırdığında, aynı girdilerle onun yerine DLSS Ray
   Reconstruction'ı çalıştırır. Bir şeyi eksik olan her kare, oyunun normal DLSS'ine geri döner.
3. Her karede, oyunun ışın izleme G-buffer'ından RR'nin ihtiyaç duyduğu kılavuz tamponlarını oluşturur:
   dünya normalleri + pürüzlülük, difüz albedo, spekülar albedo ve difüz isabet mesafesi.
4. Oyunun kendi ışın izlemeli GI gürültü giderme filtrelerini değiştirebilir: varsayılan olarak dört
   kat daha fazla geçmiş biriktirirler ("History x4", en kararlı görüntü); "Full RR" modunda ise
   kaldırılırlar ve ham küresel aydınlatmayı doğrudan RR temizler.

Oyunun çalıştırılabilir dosyası ve dosyaları hiçbir zaman değiştirilmez. Her şey bellekte, ReShade
içinde olur.

**Ne kazanırsınız:** dolaylı ışıkta daha az "kaynama" ve titreşimle daha temiz ışın izlemeli aydınlatma
(gece duvarlar, tüneller, gölgeler). Çitler ve kablolar gibi ince geometri çoğu zaman daha keskin görünür.

**Bedeli:** biraz GPU süresi ve VRAM ("Performans" bölümüne bakın).

## Karşılaştırma

Bu klipleri oyunda, kendi bilgisayarımda kaydettim (RTX 4050 Laptop GPU, 1920×1200, araç duruyor).
Kayıttan kırpılmış gerçek pikselleri 1:1 gösteriyorlar. GI modu varsayılan "History x4" değil,
**"Full RR"** (deneysel) idi.

**Tünel, kokpit görünümü.** F11'e basıyorum ve RR kapanıyor: eldivenler ve direksiyon "kaynamaya"
başlıyor.

<p align="center"><img src="../docs/images/boiling-cockpit-tunnel-f11.gif" width="800" alt="Tünelde kokpit: RR açık, sonra F11 onu kapatıyor ve eldivenler kaynamaya başlıyor"></p>

**Aynı tünel, yan yana** (solda: kapalı, sağda: açık).

<p align="center"><img src="../docs/images/boiling-cockpit-tunnel.gif" width="800" alt="Tünelde direksiyon, solda RR kapalı ve sağda açık"></p>

**Orman yolunda kokpit.** Aynı çekimin 4 s arayla iki anı, döngü halinde: önce kapalı, sonra açık.
RR ile eldivenler, A direği ve gösterge paneli kaynamayı bırakıyor ve direksiyondaki Audi halkaları
metalik yansımalarını geri kazanıyor.

<p align="center"><img src="../docs/images/boiling-cockpit-forest.gif" width="800" alt="Orman yolunda kokpit, RR kapalı ve sonra açık"></p>

Orman klibinde sağ eldivenin zamansal gürültüsü 2,3'ten 0,3–0,5'e düşüyor (kayıt üzerinde ölçtüm). RR
ile kokpit biraz daha karanlık da görünüyor ve ikisinden hangisinin doğru parlaklığa daha yakın
olduğunu henüz bilmiyorum. Aracın dışında, gündüz, fark çok daha küçük. Ayrıntılar
[docs/MEASUREMENTS.md](../docs/MEASUREMENTS.md#from-a-gameplay-recording-7-oct-2026) içinde (İngilizce).

## Gereksinimler

- Bir **NVIDIA GeForce RTX** ekran kartı. Ray Reconstruction tüm RTX nesillerinde (20, 30, 40 ve 50
  serisi) çalışır, ancak şimdiye kadar yalnızca kendi RTX 4050 Laptop'umda (6 GB) test ettim.
- PC için **Forza Horizon 6**, sürüm **6.440.853.0** (geliştirmenin yapıldığı sürüm). Diğer sürümlerde
  RR'nin çalışması beklenir; GI modları oyunun shader'larının değişmemiş olmasını gerektirir ve menü
  değiştiklerinde bunu bildirir.
- Oyunun görüntü ayarlarında: ölçekleyici olarak **DLSS** (herhangi bir kalite modu) ve açık **ışın
  izlemeli küresel aydınlatma**. Frame Generation ve Multi Frame Generation açık kalabilir.
- **Tam eklenti desteğine sahip ReShade 6.8.0 veya daha yenisi** ([reshade.me](https://reshade.me)
  adresindeki "with full add-on support" indirmesi).
- Oyunla birlikte gelen Streamline RR dosyaları (`sl.dlss_d.dll`, `nvngx_dlssd.dll`). Streamline 2.14.1
  ve DLSS-RR 310.9.0 / 310.9.1 ile test edildi.

## Kurulum

1. `ForzaHorizon6.exe` için **tam eklenti desteğine sahip ReShade** kurun ve **DirectX 10/11/12**
   seçin. Efekt shader'larına gerek yoktur.
2. [Releases](https://github.com/Felix-37/fh6-ray-reconstruction/releases) sayfasından
   `rr-forza-<sürüm>.zip` dosyasını indirin ve `rr-forza.addon64` dosyasını oyun klasörüne,
   `ForzaHorizon6.exe` dosyasının yanına kopyalayın.
3. Oyunu başlatın. Görüntü ayarlarında DLSS'i seçin ve ışın izlemeli küresel aydınlatmayı açın.
4. Birkaç saniye sürün, ardından ReShade arayüzünü açın (varsayılan olarak `Home` tuşu).
   **Ray Reconstruction** sekmesinde durum **"Ray Reconstruction active"** olmalıdır.

**Kaldırmak için** `rr-forza.addon64` dosyasını silin. Ayarları `ReShade.ini` içindeki `[RR_Forza]`
bölümündedir; o bölümü de silebilirsiniz.

## Kullanım

- **F11**, karşılaştırmak için Ray Reconstruction ile oyunun normal DLSS'i arasında geçiş yapar.
- ReShade arayüzündeki **Ray Reconstruction** sekmesinde tüm ayarlar bulunur. Her ayar GPU'ya gerçekten
  ulaşıp ulaşmadığını gösterir (`[OK]`, `[...]` veya `[ERROR]`).
  - **General:** dil (English / Español), RR açık/kapalı, RR ön ayarı (F önerilir), RR keskinleştirme.
  - **Guides:** yüzeylerin RR'ye nasıl tanımlandığı (bitki örtüsü, metalness) ve bir tanılama.
  - **Game GI:** oyunun ışın izlemeli GI filtrelerine ne olduğu (tabloya bakın).
  - **Measurement:** RR'yi normal DLSS ile gerçek sayılarla karşılaştıran yerleşik bir ölçüm aracı.
  - **Developer:** kare yakalama, oyun shader'larını dışa aktarma ve geliştirici kısayolları (F7, F10;
    varsayılan olarak kapalı).
  - **Help:** tuşlar, dosyalar ve bilinen sınırlar.

| "Game GI" modu | Ne yapar |
|---|---|
| Game original | Oyunun RTGI filtrelerine dokunulmaz, RR onların sonucunu temizler. |
| History x2 | Oyunun zamansal filtresi 2 kat daha fazla kare biriktirir: daha az kaynama, ama ışık daha yavaş tepki verir. |
| **History x4** (varsayılan, önerilen) | Aynısı, 4 kat daha fazla kareyle: en kararlı görüntü, iyi kalite ve Full RR'nin açık sorunları yok. Dolaylı ışık biraz daha yavaş tepki verir (hareketli gölgelerde iz kalabilir). |
| No temporal filter | Her karenin ham GI'ı RR'ye gider. Deneysel. |
| Full RR | Oyunun zamansal ve uzamsal RTGI filtreleri değiştirilir, tüm gürültü gidermeyi RR yapar. Gündüz en iyi görüntü (dolaylı ışıkta hiç kaynama yok), ama "Bilinen sorunlar" bölümündeki açık sorunlarla. Kamera hareket ederken kendiliğinden "History x4" moduna geçer. Deneysel. |

Varsayılan değerler benim önerimdir; [docs/MEASUREMENTS.md](../docs/MEASUREMENTS.md) içindeki
ölçümlerden ve uzun saatler süren sürüşlerden sonra seçildi. "Restore defaults" (Guides sekmesi) onları
geri getirir.

## Performans

Kendi RTX 4050 Laptop'umda ölçtüm; çıkış 1920×1200, dahili çözünürlük 1280×800, Multi Frame Generation
4x ve 138 fps sınırı:

- RR, aynı kalite modundaki DLSS-SR'ye göre gösterilen kare başına yaklaşık 0,9 ms daha fazla GPU süresi
  harcar (darboğaz GPU olduğunda −%6 fps).
- DLSS **Balanced** modundaki RR, oyunun **Quality** modundaki DLSS-SR'si kadar maliyetliydi (aynı fps,
  aynı GPU yükü) ve görünür bir kayıp yoktu. Önerdiğim kombinasyon budur.
- F11'in sorunsuz geçiş yapması için iki model de yüklü kalır. Bu, birkaç yüz MB fazladan VRAM kullanır
  ve 6 GB'lık kartlarda sınırdadır: orada oyun RR olmadan bile "out of VRAM" uyarıları verir.

## Bilinen sorunlar

Hâlâ açık; şimdiye kadar toplanan kanıtlar [docs/OPEN-PROBLEMS.md](../docs/OPEN-PROBLEMS.md)
dosyasındadır (İngilizce):

1. **Hareket halinde gürültü (Full RR):** hareket ettiğinizde veya bir şey yeniden ekrana girdiğinde
   birkaç kare boyunca ham gürültü görünür. "GI history x4 while moving" ve "Disocclusion assist" bunu
   azaltır ama yok etmez.
2. **Ayna benzeri yansımalar** (cam, krom, parlak boya) özellikle Full RR'de gürültülü kalır. RR'nin
   muhtemelen spekülar hareket vektörlerine ihtiyacı var; deneysel "Specular motion vectors" seçeneği
   (Guides sekmesi, varsayılan olarak kapalı) bunları tahmini bir mesafeyle sağlar ve henüz ölçülmedi.
3. **Gece ateş böcekleri (Full RR):** "Isolated sample clamp" ile çok daha seyrek, ama sıfır değil.
4. **Parlak gökyüzüne karşı ağaç tepeleri** RR ile yanık (fazla parlak) görünebilir.
5. Oyunun GI filtreleriyle (orijinal veya History x4) **tünellerde yavaş kaynama**: RR bunu azaltır
   (%17 ile %22 arası), ama yok etmez; GI'dan değil RT gölgelerinden geliyor olabilir.

## SSS

**AMD veya Intel GPU'larda çalışır mı?** Hayır. DLSS Ray Reconstruction yalnızca NVIDIA RTX GPU'larda çalışır.

**Ban yiyebilir miyim?** Bildiğim bir vaka yok, ama hiçbir şey vaat edemem. Eklenti oyunun dosyalarına
dokunmaz: ReShade eklentilerinin yaptığı gibi, oyun sürecinin içinde NVIDIA'nın Streamline kütüphanesine
ve Direct3D 12'ye bağlanır. Çevrimiçi kullanım kendi sorumluluğunuzdadır.

**Diğer modlarla çalışır mı?** RenoDX'in "MFG Unlock" eklentisiyle birlikte sorunsuz kullanıyorum. Diğer
kombinasyonlar (OptiScaler, DLSS DLL değişimi) test edilmedi.

**Game GI sekmesi filtrelerin "görülmediğini" veya bir varyantın "oluşturulmadığını" söylüyor.** Önce
birkaç saniye sürün. Değişmezse oyun sürümünüzde muhtemelen farklı shader'lar vardır. Oyun sürümünüzle
ve `ReShade.log` dosyanızla bir issue açın.

**Günlük nerede?** Oyun klasöründeki `ReShade.log`. Bu eklentinin satırları `[RR Forza]` ile başlar.

## Nerede yardım gerekiyor

Bu projenin, bu konuların bazılarında benden daha fazlasını bilen insanlara ihtiyacı var:

- Gürültülü yansımaları düzeltmek için **RR'ye spekülar hareket vektörleri ve spekülar isabet mesafesi**.
  speedlemur'un Control RR modu bunu yaptı. Forza'nın yansımaları, satır içi ışın sorguları ve bir mip
  piramidinden oluşur; [docs/FRAME-MAP.md](../docs/FRAME-MAP.md) içinde anlatılmıştır.
- **Full RR'de disoklüzyon gürültüsü:** geçmişi olmayan pikseller için RR'ye nasıl daha iyi bir başlangıç
  verilir.
- **Yanık ağaç tepeleri:** rüzgârla hareket eden bitki örtüsünün kendi hareket vektörleri yok. Bir
  tepkisellik veya "mevcut renk" maskesi yardımcı olur mu?
- **Diğer RTX GPU'larda**, çözünürlüklerde ve oyun sürümlerinde **testler** ile **yan yana ekran görüntüleri
  veya videolar** (aynı yer, F11 RR'yi açıp kapatır). Bu README'de henüz karşılaştırma görüntüsü yok.

[CONTRIBUTING.md](../CONTRIBUTING.md) ile başlayın, ardından [docs/ARCHITECTURE.md](../docs/ARCHITECTURE.md)
(eklentinin nasıl çalıştığı) ve [docs/MEASUREMENTS.md](../docs/MEASUREMENTS.md) (kimse tekrarlamasın diye
denenen, ölçülen ve bırakılanlar). Derlemek için yalnızca ücretsiz araçlar gerekir:
[docs/BUILDING.md](../docs/BUILDING.md).

## Teşekkürler

- **speedlemur**: [Control Ray Reconstruction](https://github.com/speedlemur/renodx/tree/control-rr)
  için; aynı fikir (oyunun DLSS-SR çağrısına DLSS-RR ile cevap vermek) ve çalışma anında shader değiştirme
  tekniği.
- **crosire**: [ReShade](https://github.com/crosire/reshade) ve eklenti API'si için.
- **clshortfuse ve RenoDX katkıcıları**: shader karma (hash) kuralı burada kullanılan
  [RenoDX](https://github.com/clshortfuse/renodx) için.
- **NVIDIA**: [Streamline](https://github.com/NVIDIA-RTX/Streamline) ve DLSS için.
- **Tsuda Kageyu** ([MinHook](https://github.com/TsudaKageyu/minhook)), **Omar Cornut**
  ([Dear ImGui](https://github.com/ocornut/imgui)) ve **Microsoft**
  ([DirectX Shader Compiler](https://github.com/microsoft/DirectXShaderCompiler)).

Üçüncü taraf lisansları: [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md).

## Yasal uyarı

Bu, resmî olmayan bir hayran projesidir. Microsoft, Xbox Game Studios, Playground Games veya NVIDIA ile
bağlantılı değildir ve onlar tarafından onaylanmamıştır. Forza Horizon, Microsoft'un ticari markasıdır.
NVIDIA, RTX ve DLSS, NVIDIA Corporation'ın ticari markalarıdır.

Depo, oyuna ait hiçbir dosya içermez. GI modlarını sunabilmek için derlenmiş sürüm, oyunun iki compute
shader'ının değiştirilmiş hâllerini içerir; bunlar oyun olmadan işe yaramaz. Kullanım riski size aittir.

Kod, [MIT lisansı](../LICENSE) ile yayımlanmıştır.
