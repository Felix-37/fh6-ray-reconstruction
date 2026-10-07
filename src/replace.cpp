// Live replacement of Forza Horizon 6's RTGI filter shaders (technique from speedlemur's control-rr,
// MIT; first used here for the archived F7/F8 bypasses, see experimentos/).
//
// When the game creates a pipeline whose shader CRC32 is a known target, one twin pipeline per
// variant of that target is created next to it, with the same root signature. A hook on the native
// ID3D12GraphicsCommandList::SetPipelineState swaps the original for the twin that the overlay
// settings select (rr::cfg::gi_temporal_mode, gi_accum, gi_firefly). Mode 0 never swaps anything.
//
// Temporal-filter variants: the game's own DXIL with one instruction changed, reassembled and signed
// offline (tools/dxil_asm.cpp, scripts/patch_rtgi.pl):
//   hist2 / hist4 : history cap x2 / x4                       (modes 1, 2)
//   nofilter      : blend weight 0, current frame only         (mode 3; mode 4 with 1 frame)
//   cap2 / cap4   : history cap limited to 2 / 4 frames        (mode 4 with 2 / 4 frames)
//   nofilter_iso4/2/1 : nofilter plus a clamp of the isolated brightest raw sample of the 3x3 resolve
//                   to k x the second brightest, before the resolve spreads it (mode 4, GiRawClamp 1-3)
// Spatial-filter variants (HLSL with the original swizzle, hash and early-out):
//   pass          : centre sample, no filtering                (mode 4, firefly suppression off)
//   clamp k=3/2/1.25: centre clamped to mean +- k sigma of its neighbours (firefly suppression)
// Modes 3 and 4 hand per-frame noise to the following passes, so they only apply while RR is on.

#include "common.hpp"
#include "rtgi_temporal_hist2.h"
#include "rtgi_temporal_hist4.h"
#include "rtgi_temporal_nofilter.h"
#include "rtgi_temporal_cap2.h"
#include "rtgi_temporal_cap4.h"
#include "rtgi_temporal_nofilter_iso4.h"
#include "rtgi_temporal_nofilter_iso2.h"
#include "rtgi_temporal_nofilter_iso1.h"
#include "rtgi_temporal_orig_reset.h"
#include "rtgi_temporal_nofilter_reset.h"
#include "rtgi_temporal_nofilter_iso4_reset.h"
#include "rtgi_temporal_nofilter_iso2_reset.h"
#include "rtgi_temporal_nofilter_iso1_reset.h"
#include "rtgi_spatial_assist4.h"
#include "rtgi_spatial_assist8.h"
#include "rtgi_temporal_hist4_reset.h"
#include "bypass_rtgi_spatial_cs.h"
#include "rtgi_spatial_clamp_soft.h"
#include "rtgi_spatial_clamp_medium.h"
#include "rtgi_spatial_clamp_strong.h"
#include "rtgi_spatial_clamp_range.h"

#include <reshade.hpp>
#include <MinHook.h>

#include <mutex>

using namespace reshade::api;

namespace rr { bool rr_is_on(); }

namespace
{
    struct variant_info
    {
        int target;   // 0 = temporal filter, 1 = spatial filter
        const char *name, *name_es;
        const void *code;
        size_t size;   // 1 = stub: the build had no game shaders (game-shaders/), the variant does not exist
    };
    const variant_info kVariants[rr::kGiVariants] = {
        { 0, "hist2", "hist2", g_rtgi_temporal_hist2, sizeof(g_rtgi_temporal_hist2) },
        { 0, "hist4", "hist4", g_rtgi_temporal_hist4, sizeof(g_rtgi_temporal_hist4) },
        { 0, "nofilter", "nofilter", g_rtgi_temporal_nofilter, sizeof(g_rtgi_temporal_nofilter) },
        { 0, "cap2", "cap2", g_rtgi_temporal_cap2, sizeof(g_rtgi_temporal_cap2) },
        { 0, "cap4", "cap4", g_rtgi_temporal_cap4, sizeof(g_rtgi_temporal_cap4) },
        { 1, "pass", "pass", g_bypass_rtgi_spatial, sizeof(g_bypass_rtgi_spatial) },
        { 1, "firefly v2 k=4", "destellos v2 k=4", g_rtgi_spatial_clamp_soft, sizeof(g_rtgi_spatial_clamp_soft) },
        { 1, "firefly v2 k=2.5", "destellos v2 k=2.5", g_rtgi_spatial_clamp_medium, sizeof(g_rtgi_spatial_clamp_medium) },
        { 1, "firefly v2 k=1.6", "destellos v2 k=1.6", g_rtgi_spatial_clamp_strong, sizeof(g_rtgi_spatial_clamp_strong) },
        { 1, "neighbour range clamp", "clamp rango", g_rtgi_spatial_clamp_range, sizeof(g_rtgi_spatial_clamp_range) },
        { 0, "nofilter clamp k=4", "nofilter recorte k=4", g_rtgi_temporal_nofilter_iso4, sizeof(g_rtgi_temporal_nofilter_iso4) },
        { 0, "nofilter clamp k=2", "nofilter recorte k=2", g_rtgi_temporal_nofilter_iso2, sizeof(g_rtgi_temporal_nofilter_iso2) },
        { 0, "nofilter clamp k=1", "nofilter recorte k=1", g_rtgi_temporal_nofilter_iso1, sizeof(g_rtgi_temporal_nofilter_iso1) },
        { 0, "original + reset", "original + reinicio", g_rtgi_temporal_orig_reset, sizeof(g_rtgi_temporal_orig_reset) },
        { 0, "nofilter + reset", "nofilter + reinicio", g_rtgi_temporal_nofilter_reset, sizeof(g_rtgi_temporal_nofilter_reset) },
        { 0, "nofilter clamp k=4 + reset", "nofilter recorte k=4 + reinicio", g_rtgi_temporal_nofilter_iso4_reset, sizeof(g_rtgi_temporal_nofilter_iso4_reset) },
        { 0, "nofilter clamp k=2 + reset", "nofilter recorte k=2 + reinicio", g_rtgi_temporal_nofilter_iso2_reset, sizeof(g_rtgi_temporal_nofilter_iso2_reset) },
        { 0, "nofilter clamp k=1 + reset", "nofilter recorte k=1 + reinicio", g_rtgi_temporal_nofilter_iso1_reset, sizeof(g_rtgi_temporal_nofilter_iso1_reset) },
        { 1, "disocclusion assist 4", "desoclusión asistida 4", g_rtgi_spatial_assist4, sizeof(g_rtgi_spatial_assist4) },
        { 1, "disocclusion assist 8", "desoclusión asistida 8", g_rtgi_spatial_assist8, sizeof(g_rtgi_spatial_assist8) },
        { 0, "hist4 + reset", "hist4 + reinicio", g_rtgi_temporal_hist4_reset, sizeof(g_rtgi_temporal_hist4_reset) },
    };
    bool built(int v) { return kVariants[v].size > 1; }
    enum : int { v_hist2, v_hist4, v_nofilter, v_cap2, v_cap4, v_pass, v_clamp_soft, v_clamp_medium, v_clamp_strong, v_clamp_range,
                 v_nofilter_iso4, v_nofilter_iso2, v_nofilter_iso1,
                 v_orig_reset, v_nofilter_reset, v_nofilter_iso4_reset, v_nofilter_iso2_reset, v_nofilter_iso1_reset, v_assist4, v_assist8, v_hist4_reset };

    // One-frame GI history reset for the "Medir desoclusión" bench: the next temporal-filter dispatch
    // uses the _reset twin of whatever variant it would use (0 idle, 1 requested, 2 done, 3 no twin).
    std::atomic<int> g_reset_state { 0 };
    std::atomic<int> g_reset_variant { -1 };

    // RTGI upscale pipelines (CRC 0x14FA42AB), watched for the diffuse hit distance guide (guides.cpp).
    constexpr uint32_t kUpscaleCrc = 0x14FA42AB;
    std::atomic<ID3D12PipelineState *> g_upscale[4] {};

    // "GI historial x4 en movimiento": while the camera moves, "RR completo" hands over to the game's own GI
    // filters with the x4 history (RR alone cannot accumulate the raw GI in motion in dim areas; the user
    // found x4 clearly better than the original filter there); back to raw GI after
    // kHoldFrames still frames. Updated once per frame from slSetConstants.
    constexpr int kHoldFrames = 20;
    std::atomic<bool> g_moving { false };
    std::atomic<int> g_still_frames { 0 };
    std::atomic<float> g_last_move { 0.0f }, g_last_turn { 0.0f };
    std::atomic<uint64_t> g_motion_switches { 0 };

    bool motion_override()
    {
        return rr::cfg::gi_motion_auto.load(std::memory_order_relaxed) && g_moving.load(std::memory_order_relaxed) && !rr::bench_active();
    }

    int reset_twin_of(int v)
    {
        switch (v)
        {
        case -1: return v_orig_reset;
        case v_nofilter: return v_nofilter_reset;
        case v_nofilter_iso4: return v_nofilter_iso4_reset;
        case v_nofilter_iso2: return v_nofilter_iso2_reset;
        case v_nofilter_iso1: return v_nofilter_iso1_reset;
        case v_hist4: return v_hist4_reset;
        default: return -1;
        }
    }

    struct target_info
    {
        uint32_t crc;
        const char *name;
    };
    const target_info kTargets[2] = { { 0x4DAF8A48, "RTGI_TemporalFilter" }, { 0x209AB6A4, "RTGI_SpatialFilter_Disk" } };

    // Lock-free for the hot path (every SetPipelineState): fixed slots, written rarely.
    struct slot
    {
        std::atomic<ID3D12PipelineState *> original { nullptr };
        std::atomic<ID3D12PipelineState *> twins[rr::kGiVariants] {};
        std::atomic<int> target { -1 };
    };
    constexpr int kSlots = 32;
    slot g_slots[kSlots];
    std::mutex g_write_mutex;
    std::atomic<uint64_t> g_swaps { 0 };
    std::atomic<uint64_t> g_variant_swaps[rr::kGiVariants] {};
    std::atomic<uint64_t> g_variant_tick[rr::kGiVariants] {};
    std::atomic<int> g_found[2] {};   // live game pipelines per target
    std::atomic<int> g_twins_ready { 0 };
    std::atomic<int> g_twin_failures { 0 };
    std::atomic<bool> g_hook_installed { false };
    std::string g_last_failure;   // guarded by g_write_mutex

    using set_pso_fn = void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *, ID3D12PipelineState *);
    set_pso_fn g_set_pso_original = nullptr;

    // Variant each target should use with the current settings, -1 = the game's own shader.
    void desired(int &temporal, int &spatial, bool rr_on)
    {
        temporal = spatial = -1;
        switch (rr::cfg::gi_temporal_mode.load(std::memory_order_relaxed))
        {
        case 1: temporal = v_hist2; break;
        case 2: temporal = v_hist4; break;
        case 3:
            if (rr_on)
                temporal = v_nofilter;
            break;
        case 4:
            if (!rr_on)
                break;
            if (motion_override())
            {
                temporal = v_hist4;   // spatial: the game's own
                break;
            }
            switch (rr::cfg::gi_accum.load(std::memory_order_relaxed))
            {
            case 2: temporal = v_cap2; break;
            case 4: temporal = v_cap4; break;
            default:
                switch (rr::cfg::gi_raw_clamp.load(std::memory_order_relaxed))
                {
                case 1: temporal = v_nofilter_iso4; break;
                case 2: temporal = v_nofilter_iso2; break;
                case 3: temporal = v_nofilter_iso1; break;
                default: temporal = v_nofilter; break;
                }
                break;
            }
            switch (rr::cfg::gi_assist.load(std::memory_order_relaxed))
            {
            case 1: spatial = v_assist4; return;
            case 2: spatial = v_assist8; return;
            default: break;
            }
            switch (rr::cfg::gi_firefly.load(std::memory_order_relaxed))
            {
            case 1: spatial = v_clamp_soft; break;
            case 2: spatial = v_clamp_medium; break;
            case 3: spatial = v_clamp_strong; break;
            case 4: spatial = v_clamp_range; break;
            default: spatial = v_pass; break;
            }
            break;
        default: break;
        }
    }

    void STDMETHODCALLTYPE set_pso_hook(ID3D12GraphicsCommandList *self, ID3D12PipelineState *pso)
    {
        rr::hook_guard guard;
        if (pso != nullptr && rr::cfg::rr_hit_distance.load(std::memory_order_relaxed))
            for (const auto &u : g_upscale)
                if (u.load(std::memory_order_relaxed) == pso)
                    rr::guides_on_upscale_pso(self);
        if (pso != nullptr && (rr::cfg::gi_temporal_mode.load(std::memory_order_relaxed) != 0 || g_reset_state.load(std::memory_order_relaxed) == 1))
        {
            for (slot &s : g_slots)
            {
                if (s.original.load(std::memory_order_acquire) != pso)
                    continue;
                int temporal, spatial;
                desired(temporal, spatial, rr::rr_is_on());
                const bool is_temporal = s.target.load(std::memory_order_relaxed) == 0;
                int v = is_temporal ? temporal : spatial;
                if (is_temporal && g_reset_state.load(std::memory_order_relaxed) == 1)
                {
                    int expected = 1;
                    if (g_reset_state.compare_exchange_strong(expected, 2))
                    {
                        const int r = reset_twin_of(v);
                        if (r >= 0 && s.twins[r].load(std::memory_order_acquire) != nullptr)
                        {
                            v = r;
                            g_reset_variant = r;
                        }
                        else
                            g_reset_state = 3;
                    }
                }
                if (v >= 0)
                    if (ID3D12PipelineState *twin = s.twins[v].load(std::memory_order_acquire))
                    {
                        pso = twin;
                        g_variant_swaps[v].fetch_add(1, std::memory_order_relaxed);
                        g_variant_tick[v].store(GetTickCount64(), std::memory_order_relaxed);
                        if (g_swaps.fetch_add(1, std::memory_order_relaxed) == 0)
                            rr::log_info(std::string("gi: first pipeline swap (") + kVariants[v].name + ")");
                    }
                break;
            }
        }
        g_set_pso_original(self, pso);
    }
}

namespace rr
{
    // From shader_dump.cpp's init_pipeline handler, for every compute pipeline.
    void replacement_on_init(device *dev, pipeline_layout layout, uint32_t crc, pipeline pso)
    {
        if (crc == kUpscaleCrc)
        {
            for (auto &u : g_upscale)
            {
                ID3D12PipelineState *expected = nullptr;
                if (u.compare_exchange_strong(expected, reinterpret_cast<ID3D12PipelineState *>(pso.handle)))
                {
                    log_info("gi: RTGI upscale pipeline seen (diffuse hit distance source)");
                    break;
                }
            }
            return;
        }
        int index = -1;
        for (int i = 0; i < 2; ++i)
            if (kTargets[i].crc == crc)
                index = i;
        if (index < 0)
            return;
        auto *native_device = reinterpret_cast<ID3D12Device *>(dev->get_native());

        ID3D12PipelineState *twins[kGiVariants] = {};
        int created = 0;
        for (int v = 0; v < kGiVariants; ++v)
        {
            if (kVariants[v].target != index || !built(v))
                continue;
            // The reassembled temporal variants carry the game's own root signature (RTS0); the HLSL
            // spatial variants do not, so the game's root signature is passed when ReShade has it.
            D3D12_COMPUTE_PIPELINE_STATE_DESC desc {};
            desc.pRootSignature = reinterpret_cast<ID3D12RootSignature *>(layout.handle);
            desc.CS = { kVariants[v].code, kVariants[v].size };
            const HRESULT hr = native_device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&twins[v]));
            if (FAILED(hr))
            {
                twins[v] = nullptr;
                g_twin_failures++;
                std::lock_guard lock(g_write_mutex);
                g_last_failure = std::string("CreateComputePipelineState ") + hex(uint32_t(hr)) + " (" + kVariants[v].name + ")";
                log_warn(std::string("gi: twin pipeline ") + kVariants[v].name + " for " + kTargets[index].name + " failed: " + hex(uint32_t(hr)) +
                         (layout.handle == 0 ? " (no root signature from ReShade)" : ""));
                continue;
            }
            created++;
        }

        std::lock_guard lock(g_write_mutex);
        g_found[index]++;
        for (slot &s : g_slots)
        {
            if (s.original.load() == nullptr)
            {
                s.target.store(index, std::memory_order_relaxed);
                for (int v = 0; v < kGiVariants; ++v)
                    s.twins[v].store(twins[v], std::memory_order_release);
                s.original.store(reinterpret_cast<ID3D12PipelineState *>(pso.handle), std::memory_order_release);
                g_twins_ready += created;
                log_info(std::string("gi: twin pipeline ready for ") + kTargets[index].name + " (" + std::to_string(created) + " variant(s))");
                return;
            }
        }
        for (ID3D12PipelineState *p : twins)
            if (p != nullptr)
                p->Release();
        log_warn("gi: no free slot");
    }

    // The game destroyed a pipeline: forget it so a reused address is never swapped. The twins are
    // leaked on purpose (a command list in flight may still reference them; a few KB each).
    void replacement_on_destroy(pipeline pso)
    {
        for (auto &u : g_upscale)
        {
            ID3D12PipelineState *expected = reinterpret_cast<ID3D12PipelineState *>(pso.handle);
            u.compare_exchange_strong(expected, nullptr);
        }
        std::lock_guard lock(g_write_mutex);
        for (slot &s : g_slots)
            if (s.original.load() == reinterpret_cast<ID3D12PipelineState *>(pso.handle))
            {
                s.original.store(nullptr, std::memory_order_release);
                const int index = s.target.load();
                if (index >= 0)
                    g_found[index]--;
            }
    }

    bool install_replacement_hook(ID3D12GraphicsCommandList *any_list)
    {
        constexpr size_t kSetPipelineState = 25;   // ID3D12GraphicsCommandList vtable slot (offsetof-verified)
        void **vtable = *reinterpret_cast<void ***>(any_list);
        void *target = vtable[kSetPipelineState];
        MH_STATUS s = MH_CreateHook(target, reinterpret_cast<void *>(set_pso_hook), reinterpret_cast<void **>(&g_set_pso_original));
        if (s == MH_OK)
            s = MH_EnableHook(target);
        if (s != MH_OK)
        {
            log_warn(std::string("gi: SetPipelineState hook failed: ") + MH_StatusToString(s));
            return false;
        }
        g_hook_installed = true;
        return true;
    }

    void gi_motion_update(float move_m, float turn_deg, uint32_t)
    {
        g_last_move = move_m;
        g_last_turn = turn_deg;
        const bool now_moving = move_m * 1000.0f > float(cfg::gi_motion_mm.load()) || turn_deg * 1000.0f > float(cfg::gi_motion_mdeg.load());
        if (now_moving)
        {
            g_still_frames = 0;
            if (!g_moving.exchange(true))
            {
                g_motion_switches++;
                // Hand-over: the game's filter would otherwise trust a history that holds raw GI (written by
                // "RR completo") for many frames. Dropping it for one frame makes it treat the screen as just
                // revealed (wide spatial filter), as measured with "Medir desoclusión".
                if (motion_override() && cfg::gi_temporal_mode.load() == 4 && rr_is_on())
                    gi_request_reset();
            }
        }
        else if (g_moving.load() && g_still_frames.fetch_add(1) + 1 >= kHoldFrames)
            g_moving = false;
    }

    gi_motion_snapshot gi_motion_get()
    {
        gi_motion_snapshot s;
        s.moving = g_moving.load();
        s.move_m = g_last_move.load();
        s.turn_deg = g_last_turn.load();
        s.switches = g_motion_switches.load();
        s.still_frames = g_still_frames.load();
        return s;
    }

    void gi_request_reset()
    {
        g_reset_variant = -1;
        g_reset_state = 1;
    }

    // "" while the request is pending; otherwise what the GPU got for the reset frame.
    std::string gi_reset_status()
    {
        switch (g_reset_state.load())
        {
        case 2: return std::string(tr("GI reset (", "GI reiniciado (")) + gi_variant_name(g_reset_variant.load()) + ")";
        case 3: return tr("GI NOT reset (no reset variant for this mode)", "GI SIN reiniciar (no hay variante de reinicio para este modo)");
        case 1: return "";
        default: return tr("no GI reset requested", "GI sin petición de reinicio");
        }
    }

    bool gi_variants_built() { return built(v_nofilter); }

    void gi_cancel_reset()
    {
        int expected = 1;
        g_reset_state.compare_exchange_strong(expected, 0);
    }

    const char *gi_variant_name(int v) { return v >= 0 && v < kGiVariants ? tr(kVariants[v].name, kVariants[v].name_es) : "original"; }

    gi_snapshot gi_get_snapshot()
    {
        gi_snapshot g;
        g.temporal_found = g_found[0].load() > 0;
        g.spatial_found = g_found[1].load() > 0;
        g.twins = g_twins_ready.load();
        g.failures = g_twin_failures.load();
        g.swaps = g_swaps.load();
        g.hook_installed = g_hook_installed.load();
        desired(g.want_temporal, g.want_spatial, rr_is_on());
        for (int v = 0; v < kGiVariants; ++v)
        {
            g.variant_swaps[v] = g_variant_swaps[v].load();
            g.variant_tick[v] = g_variant_tick[v].load();
        }
        std::lock_guard lock(g_write_mutex);
        g.last_failure = g_last_failure;
        for (slot &s : g_slots)
            if (s.original.load() != nullptr)
                for (int v = 0; v < kGiVariants; ++v)
                    g.variant_twins[v] += s.twins[v].load() != nullptr;
        return g;
    }
}
