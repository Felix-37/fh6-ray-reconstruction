// User settings of the add-on: atomics shared by the overlay, the hotkeys and the render threads,
// persisted in the global ReShade.ini under [RR_Forza].
//
// Defaults: GI mode "History x4" (7 oct 2026, my recommendation: the most stable image with
// good quality; "Full RR" looks best by day but has more open problems). The "Full RR" sub-options keep
// the values measured best for it (isolated-sample clamp k = 4, disocclusion assist over 4 frames).
// Diffuse hit distance guide on, sharpening 30 %, neutral foliage albedo off. See docs/MEASUREMENTS.md.

#include "common.hpp"

#include <reshade.hpp>

namespace rr::cfg
{
    std::atomic<bool> rr_enabled { true };
    std::atomic<int> preset { 6 };
    std::atomic<int> sky_mode { 1 };
    std::atomic<bool> foliage_neutral { false };   // off since 5 oct 2026: white foliage albedo cost far detail (fences)
    std::atomic<int> foliage_threshold { 95 };
    std::atomic<bool> foliage_specular { true };
    std::atomic<bool> foliage_dilate { false };   // off by default since 4 oct 2026: it also flagged far, thin detail
    std::atomic<bool> foliage_floor_mode { true };
    std::atomic<int> foliage_floor { 20 };
    std::atomic<int> gi_firefly { 0 };   // "RR completo": firefly suppression v2 (0 off .. 3 strong, 4 neighbour range); measured to darken (-0.3 EV)
    std::atomic<int> gi_accum { 1 };
    std::atomic<int> gi_raw_clamp { 1 };   // "RR completo": isolated raw sample clamp, 0 off, 1 k=4, 2 k=2, 3 k=1 (k=4 measured best, 5 oct 2026)
    std::atomic<int> gi_assist { 1 };
    std::atomic<bool> gi_motion_auto { true };
    std::atomic<int> gi_motion_mm { 10 };
    std::atomic<int> gi_motion_mdeg { 50 };
    std::atomic<bool> rr_hit_distance { true };
    std::atomic<int> rr_hit_scale { 10 };
    std::atomic<bool> rr_spec_mv { false };   // experimental, not measured yet (7 oct 2026)
    std::atomic<int> rr_spec_mv_distance { 30 };
    std::atomic<int> rr_exposure_ev { 0 };
    std::atomic<bool> rr_sharpen { true };
    std::atomic<int> rr_sharpness { 30 };   // bench: 30 % brings the fine detail back to ~1.04-1.07 of DLSS-SR
    std::atomic<bool> diag_neutral { false };   // never saved: a diagnostic must not survive a restart
    std::atomic<bool> metal_alpha { false };
    std::atomic<int> gi_temporal_mode { 2 };   // History x4
    std::atomic<bool> reset_history { false };
    std::atomic<int> language { 0 };
    std::atomic<bool> dev_hotkeys { false };   // off for players: F10 alone would write up to 1 GB of capture

    namespace
    {
        constexpr const char *kSection = "RR_Forza";

        void read_bool(const char *key, std::atomic<bool> &value)
        {
            bool v = value.load();
            if (reshade::get_config_value(nullptr, kSection, key, v))
                value = v;
        }

        void read_int(const char *key, std::atomic<int> &value, int lo, int hi)
        {
            int v = value.load();
            if (reshade::get_config_value(nullptr, kSection, key, v) && v >= lo && v <= hi)
                value = v;
        }

        bool valid_preset(int p) { return p == 0 || p == 4 || p == 5 || p == 6; }
    }

    void load()
    {
        read_bool("Enabled", rr_enabled);
        int p = preset.load();
        if (reshade::get_config_value(nullptr, kSection, "Preset", p) && valid_preset(p))
            preset = p;
        {
            // SkyMode replaces SkyNeutral (true = white, false = black).
            bool neutral = true;
            if (reshade::get_config_value(nullptr, kSection, "SkyNeutral", neutral))
                sky_mode = neutral ? 1 : 0;
            read_int("SkyMode", sky_mode, 0, 2);
            sky_mode = 1;   // selector removed (5 oct 2026): black / white / mirror measured identical
        }
        read_bool("FoliageNeutral", foliage_neutral);
        read_int("FoliageThreshold", foliage_threshold, 50, 95);
        read_bool("FoliageSpecularFade", foliage_specular);
        read_bool("FoliageDilateV2", foliage_dilate);
        read_bool("FoliageFloorMode", foliage_floor_mode);
        read_int("FoliageFloor", foliage_floor, 10, 40);

        // Exposure and firefly suppression were measured useless (4 oct 2026): their menu entries are gone
        // and old saved values are ignored.
        rr_exposure_ev = 0;
        gi_accum = 1;
        read_int("GiFireflyV2", gi_firefly, 0, 4);
        read_int("GiRawClamp", gi_raw_clamp, 0, 3);
        read_int("GiDisocclusionAssist", gi_assist, 0, 2);
        read_bool("GiMotionAuto", gi_motion_auto);
        read_int("GiMotionMm", gi_motion_mm, 1, 500);
        read_int("GiMotionMdeg", gi_motion_mdeg, 1, 5000);
        read_bool("RrHitDistance", rr_hit_distance);
        read_int("RrHitScale", rr_hit_scale, 1, 200);
        read_bool("RrSpecularMotion", rr_spec_mv);
        read_int("RrSpecularDistance", rr_spec_mv_distance, 1, 1000);
        read_bool("RrSharpen", rr_sharpen);
        read_int("RrSharpness", rr_sharpness, 0, 100);
        read_bool("MetalFromAlpha", metal_alpha);
        read_int("GiTemporalMode", gi_temporal_mode, 0, 4);
        read_int("Language", language, 0, 1);
        read_bool("DevHotkeys", dev_hotkeys);
    }

    void save()
    {
        reshade::set_config_value(nullptr, kSection, "Enabled", rr_enabled.load());
        reshade::set_config_value(nullptr, kSection, "Preset", preset.load());
        reshade::set_config_value(nullptr, kSection, "SkyMode", sky_mode.load());
        reshade::set_config_value(nullptr, kSection, "FoliageNeutral", foliage_neutral.load());
        reshade::set_config_value(nullptr, kSection, "FoliageThreshold", foliage_threshold.load());
        reshade::set_config_value(nullptr, kSection, "FoliageSpecularFade", foliage_specular.load());
        reshade::set_config_value(nullptr, kSection, "FoliageDilateV2", foliage_dilate.load());
        reshade::set_config_value(nullptr, kSection, "FoliageFloorMode", foliage_floor_mode.load());
        reshade::set_config_value(nullptr, kSection, "FoliageFloor", foliage_floor.load());
        reshade::set_config_value(nullptr, kSection, "GiFireflyV2", gi_firefly.load());
        reshade::set_config_value(nullptr, kSection, "GiRawClamp", gi_raw_clamp.load());
        reshade::set_config_value(nullptr, kSection, "GiDisocclusionAssist", gi_assist.load());
        reshade::set_config_value(nullptr, kSection, "GiMotionAuto", gi_motion_auto.load());
        reshade::set_config_value(nullptr, kSection, "GiMotionMm", gi_motion_mm.load());
        reshade::set_config_value(nullptr, kSection, "GiMotionMdeg", gi_motion_mdeg.load());
        reshade::set_config_value(nullptr, kSection, "RrHitDistance", rr_hit_distance.load());
        reshade::set_config_value(nullptr, kSection, "RrHitScale", rr_hit_scale.load());
        reshade::set_config_value(nullptr, kSection, "RrSpecularMotion", rr_spec_mv.load());
        reshade::set_config_value(nullptr, kSection, "RrSpecularDistance", rr_spec_mv_distance.load());
        reshade::set_config_value(nullptr, kSection, "RrExposureEv", rr_exposure_ev.load());
        reshade::set_config_value(nullptr, kSection, "RrSharpen", rr_sharpen.load());
        reshade::set_config_value(nullptr, kSection, "RrSharpness", rr_sharpness.load());
        reshade::set_config_value(nullptr, kSection, "GiAccum", gi_accum.load());
        reshade::set_config_value(nullptr, kSection, "MetalFromAlpha", metal_alpha.load());
        reshade::set_config_value(nullptr, kSection, "GiTemporalMode", gi_temporal_mode.load());
        reshade::set_config_value(nullptr, kSection, "Language", language.load());
        reshade::set_config_value(nullptr, kSection, "DevHotkeys", dev_hotkeys.load());
    }

    // Language and developer hotkeys are preferences, not image settings: they are kept.
    void restore_defaults()
    {
        rr_enabled = true;
        preset = 6;
        sky_mode = 1;
        foliage_neutral = false;
        foliage_threshold = 95;
        foliage_specular = true;
        foliage_dilate = false;
        foliage_floor_mode = true;
        foliage_floor = 20;
        gi_firefly = 0;
        gi_accum = 1;
        gi_raw_clamp = 1;
        gi_assist = 1;
        gi_motion_auto = true;
        gi_motion_mm = 10;
        gi_motion_mdeg = 50;
        rr_hit_distance = true;
        rr_hit_scale = 10;
        rr_spec_mv = false;
        rr_spec_mv_distance = 30;
        rr_exposure_ev = 0;
        rr_sharpen = true;
        rr_sharpness = 30;
        diag_neutral = false;
        metal_alpha = false;
        gi_temporal_mode = 2;
        reset_history = true;
        save();
    }
}

namespace rr
{
    std::filesystem::path capture_root()
    {
        char buffer[1024] = {};
        size_t size = sizeof(buffer);
        if (reshade::get_config_value(nullptr, "RR_Forza", "CaptureDir", buffer, &size) && buffer[0] != '\0')
            return std::filesystem::path(reinterpret_cast<const char8_t *>(buffer));
        // The folder this add-on was loaded from (the game folder).
        HMODULE self = nullptr;
        wchar_t path[MAX_PATH] = {};
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(&capture_root), &self) &&
            GetModuleFileNameW(self, path, MAX_PATH) != 0)
            return std::filesystem::path(path).parent_path() / L"rr-forza-captures";
        return std::filesystem::path(L"rr-forza-captures");
    }
}
