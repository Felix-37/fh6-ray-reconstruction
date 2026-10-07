// Phase 5: live replacement of the game's reflection blur shaders (technique from speedlemur's
// control-rr, MIT). When the game creates a pipeline whose shader CRC32 is in kTargets, a twin
// pipeline with the pass-through shader and the same root signature is created next to it. A hook
// on the native ID3D12GraphicsCommandList::SetPipelineState swaps the original for the twin while
// the bypass is on (F8) and Ray Reconstruction is active, so DLSS-RR receives the unblurred
// reflections. Nothing on disk changes; turning F8 off, or RR off, restores the game's blur at once.

#include "common.hpp"
#include "bypass_filtermip_cs.h"
#include "bypass_rtgi_spatial_cs.h"

#include <reshade.hpp>
#include <MinHook.h>

#include <mutex>

using namespace reshade::api;

namespace rr { bool ensure_minhook(); bool rr_is_on(); }

namespace
{
    struct target
    {
        uint32_t crc;
        int group;   // 0 = reflections (F8), 1 = RTGI (F7)
        const char *name;
        const void *code;
        size_t size;
    };
    const target kTargets[] = {
        { 0x531F30AA, 0, "SSR_FullScreen_FilterMip_Horizontal", g_bypass_filtermip, sizeof(g_bypass_filtermip) },
        { 0x62D725EE, 0, "SSR_FullScreen_FilterMip_Vertical", g_bypass_filtermip, sizeof(g_bypass_filtermip) },
        { 0x209AB6A4, 1, "RTGI_SpatialFilter_Disk", g_bypass_rtgi_spatial, sizeof(g_bypass_rtgi_spatial) },
    };

    // Lock-free for the hot path (every SetPipelineState): fixed slots, written rarely.
    struct slot
    {
        std::atomic<ID3D12PipelineState *> original { nullptr };
        std::atomic<ID3D12PipelineState *> replacement { nullptr };
        std::atomic<int> group { 0 };
    };
    constexpr int kSlots = 32;
    slot g_slots[kSlots];
    std::mutex g_write_mutex;
    std::atomic<bool> g_bypass[2] { false, false };   // [0] reflections F8, [1] RTGI F7
    std::atomic<uint64_t> g_swaps { 0 };

    using set_pso_fn = void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *, ID3D12PipelineState *);
    set_pso_fn g_set_pso_original = nullptr;
    void *g_set_pso_target = nullptr;

    void STDMETHODCALLTYPE set_pso_hook(ID3D12GraphicsCommandList *self, ID3D12PipelineState *pso)
    {
        rr::hook_guard guard;
        if (pso != nullptr && (g_bypass[0].load(std::memory_order_relaxed) || g_bypass[1].load(std::memory_order_relaxed)) && rr::rr_is_on())
        {
            for (slot &s : g_slots)
            {
                if (s.original.load(std::memory_order_acquire) == pso)
                {
                    if (!g_bypass[s.group.load(std::memory_order_relaxed)].load(std::memory_order_relaxed))
                        break;
                    if (ID3D12PipelineState *r = s.replacement.load(std::memory_order_acquire))
                    {
                        pso = r;
                        if (g_swaps.fetch_add(1, std::memory_order_relaxed) == 0)
                            rr::log_info("bypass: first pipeline swap");
                    }
                    break;
                }
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
        const target *t = nullptr;
        for (const target &k : kTargets)
            if (k.crc == crc)
                t = &k;
        if (t == nullptr || layout.handle == 0)
            return;
        auto *native_device = reinterpret_cast<ID3D12Device *>(dev->get_native());
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc {};
        desc.pRootSignature = reinterpret_cast<ID3D12RootSignature *>(layout.handle);
        desc.CS = { t->code, t->size };
        ID3D12PipelineState *twin = nullptr;
        const HRESULT hr = native_device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&twin));
        if (FAILED(hr))
        {
            log_warn(std::string("bypass: twin pipeline for ") + t->name + " failed: " + hex(uint32_t(hr)));
            return;
        }
        std::lock_guard lock(g_write_mutex);
        for (slot &s : g_slots)
        {
            if (s.original.load() == nullptr)
            {
                s.group.store(t->group, std::memory_order_relaxed);
                s.replacement.store(twin, std::memory_order_release);
                s.original.store(reinterpret_cast<ID3D12PipelineState *>(pso.handle), std::memory_order_release);
                log_info(std::string("bypass: twin pipeline ready for ") + t->name);
                return;
            }
        }
        twin->Release();
        log_warn("bypass: no free slot");
    }

    // The game destroyed a pipeline: forget it so a reused address is never swapped. The twin is
    // leaked on purpose (a command list in flight may still reference it; a few KB each).
    void replacement_on_destroy(pipeline pso)
    {
        std::lock_guard lock(g_write_mutex);
        for (slot &s : g_slots)
            if (s.original.load() == reinterpret_cast<ID3D12PipelineState *>(pso.handle))
                s.original.store(nullptr, std::memory_order_release);
    }

    bool install_replacement_hook(ID3D12GraphicsCommandList *any_list)
    {
        constexpr size_t kSetPipelineState = 25;   // ID3D12GraphicsCommandList vtable slot (offsetof-verified)
        void **vtable = *reinterpret_cast<void ***>(any_list);
        g_set_pso_target = vtable[kSetPipelineState];
        MH_STATUS s = MH_CreateHook(g_set_pso_target, reinterpret_cast<void *>(set_pso_hook), reinterpret_cast<void **>(&g_set_pso_original));
        if (s == MH_OK)
            s = MH_EnableHook(g_set_pso_target);
        if (s != MH_OK)
        {
            log_warn(std::string("bypass: SetPipelineState hook failed: ") + MH_StatusToString(s));
            return false;
        }
        return true;
    }

    void bypass_toggle(int group)
    {
        const bool on = !g_bypass[group].load();
        g_bypass[group] = on;
        int ready = 0;
        for (slot &s : g_slots)
            ready += s.original.load() != nullptr && s.group.load() == group;
        log_info(std::string(group == 0 ? "F8: reflection blur bypass " : "F7: RTGI spatial filter bypass ") + (on ? "ON (only while RR is active)" : "OFF") + ", " + std::to_string(ready) + " twin pipeline(s) ready");
    }

    std::string bypass_status()
    {
        int ready = 0;
        for (slot &s : g_slots)
            ready += s.original.load() != nullptr;
        return std::string("bypass: reflections=") + (g_bypass[0] ? "on" : "off") + " rtgi=" + (g_bypass[1] ? "on" : "off") + " twins=" + std::to_string(ready) + " swaps=" + std::to_string(g_swaps.load());
    }
}
