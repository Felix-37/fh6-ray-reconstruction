// RR vs DLSS measurement bench ("Measurement" tab).
//
// With the car and the camera still, the bench runs a fixed sequence of variants (DLSS-SR first, as the
// reference, then RR with the chosen options). For each one it applies the settings, lets RR converge for
// kSettleFrames DLSS evaluations, then records kRecordFrames consecutive frames: the DLSS output colour
// (HDR, before the game's tonemap, HUD and post-processing) as luminance, and the DLSS depth. At the end
// of NGX's evaluate marker, on the native command list, a small compute shader (shaders/bench_copy.hlsl)
// reads both game textures in the layouts the game already keeps them in and writes the add-on's own
// textures, which are copied to readback buffers; no barrier ever touches a game resource (copies that
// changed their layouts hung the GPU, 4 oct 2026). Readbacks are mapped once a fence on the present
// queue says the GPU is done, and analysed in a worker thread (metrics.cpp). The user's settings are
// restored as soon as the last frame is recorded.

#include "common.hpp"
#include "metrics.hpp"
#include "bench_copy_cs.h"

#include <reshade.hpp>
#include <MinHook.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <thread>

namespace rr { bool ensure_minhook(); }

namespace
{
    constexpr int kSettleFrames = 90;   // DLSS evaluations to let RR converge after a change
    // After a change of the game's GI filters the GI history itself must reconverge: the variant
    // measured right after the "RR completo" ones still showed their grain with 90 (4 oct 2026).
    constexpr int kSettleFramesGi = 300;
    int g_settle_target = kSettleFrames;

    constexpr int kRecordFrames = 8;
    // The 8 frames are spread over 8 * kRecordStride DLSS evaluations (~1.5 s): the GI "boil" of the
    // game (slow blotches on walls lit indirectly) changes over seconds, and 8 consecutive frames
    // (~0.25 s) did not see it at all (user's screenshot of a night facade, 4 oct 2026).
    constexpr int kRecordStride = 6;
    int g_stride_counter = 0;
    int g_record_eval = 0;   // DLSS evaluations since the record phase began

    struct variant
    {
        std::string name;
        bool rr = false;
        int exposure_ev = 0;   // 0 = no exposure tag (as the game)
        bool diag_neutral = false;
        int gi_mode = -1;      // -1 = keep the user's GI mode
        int metal = -1;        // metalness from the albedo alpha: -1 keep, 0 off, 1 on
        int specular_fade = -1;// fade the specular guide on incoherent normals: -1 keep, 0 off, 1 on
        int firefly = -1;      // "RR completo" firefly level: -1 keep
        int accum = -1;        // "RR completo" pre-accumulation frames: -1 keep
        int sharpen = -1;      // RR output sharpening: -1 keep, 0 off, 1..100 on at that strength
        int sky = -1;          // guide sky mode: -1 keep, 0 black, 1 white, 2 mirror
        bool after_reset = false;
        int raw_clamp = -1;    // "RR completo" isolated raw sample clamp level: -1 keep
        int assist = -1;       // "RR completo" disocclusion assist: -1 keep, 0 off, 1 = 4 frames, 2 = 8 frames
        bool gi_reset = false; // with after_reset: also drop the game's GI history in the same frame (disocclusion bench)
        int hit = -1;          // diffuse hit distance guide for RR: -1 keep, 0 off, 1 on
    };

    int effective_gi(const variant &v, int saved) { return v.gi_mode >= 0 ? v.gi_mode : saved; }

    struct pending
    {
        ID3D12Resource *color = nullptr, *depth = nullptr;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT color_fp {}, depth_fp {};
        UINT color_rows = 0, depth_rows = 0;
        UINT64 color_row = 0, depth_row = 0;
        int variant = 0, frame = 0;
        int frame_number = 0;   // evaluations since recording started (1 = first frame after a reset)
        bool ok_frame = true;   // the evaluation really was the variant's kind (RR or SR)
        void *list = nullptr;   // native command list holding the copies
        int slot = -1;          // readback pool slot of the colour copy
        UINT64 fence_value = 0; // 0 = list not submitted yet
    };

    struct saved_settings
    {
        bool metal = false, specular_fade = true;
        int firefly = 0, accum = 1, sky = 1, raw_clamp = 0, assist = 0;
        bool hit = false;
        bool sharpen = false;
        int sharpness = 30;
        bool rr_enabled = true, diag_neutral = false;
        int exposure_ev = 0, gi_mode = 0;
    };

    enum class phase { idle, settle, record, finishing, analysing, done };

    std::mutex g_mutex;
    phase g_phase = phase::idle;
    std::vector<variant> g_variants;
    int g_current = 0, g_counter = 0, g_recorded = 0;
    saved_settings g_saved;
    std::vector<pending> g_pending;
    std::vector<rr::metrics::frame_set> g_sets;
    std::vector<std::vector<uint8_t>> g_first_color;   // raw R11G11B10 frame 0 per variant, for the DDS dump
    std::filesystem::path g_dir;
    std::string g_summary, g_error;
    rr::metrics::scene g_scene;
    ID3D12Fence *g_fence = nullptr;
    std::atomic<bool> g_active { false };
    std::string g_reset_note;   // what the GI reset of the current disocclusion variant did
    int g_stale_frames = 0;   // recorded frames whose copy never ran (dropped); reported in the summary
    int g_focus = 0;   // 0 general, 1 car reflections, 2 night dots of "RR completo", 3 convergence after a history reset   // settle/record phases: the marker hooks track NGX markers
    std::atomic<int> g_pending_count { 0 }; // lets bench_on_execute skip the lock on every submission
    UINT64 g_fence_value = 0;

    // Last enhanced barrier the game recorded on the DLSS output and depth: the copy round trip starts
    // from exactly that layout/sync/access (as guides.cpp does for the G-buffer). Without one seen, the
    // layouts of the frame captures are used.
    struct observed
    {
        bool seen = false;
        D3D12_BARRIER_LAYOUT layout = D3D12_BARRIER_LAYOUT_UNDEFINED;
        D3D12_BARRIER_SYNC sync = D3D12_BARRIER_SYNC_NONE;
        D3D12_BARRIER_ACCESS access = D3D12_BARRIER_ACCESS_NO_ACCESS;
    };
    observed g_output_seen, g_depth_seen;
    bool g_layouts_logged = false;

    // Latest DLSS tags (set_tag_hook).
    ID3D12Resource *g_output = nullptr, *g_depth = nullptr;
    D3D12_RESOURCE_STATES g_output_state = D3D12_RESOURCE_STATE_COMMON, g_depth_state = D3D12_RESOURCE_STATE_COMMON;

    void apply(const variant &v)
    {
        rr::cfg::rr_enabled = v.rr;
        rr::cfg::rr_exposure_ev = v.exposure_ev;
        rr::cfg::diag_neutral = v.diag_neutral;
        rr::cfg::gi_temporal_mode = v.gi_mode >= 0 ? v.gi_mode : g_saved.gi_mode;
        rr::cfg::metal_alpha = v.metal >= 0 ? v.metal == 1 : g_saved.metal;
        rr::cfg::foliage_specular = v.specular_fade >= 0 ? v.specular_fade == 1 : g_saved.specular_fade;
        rr::cfg::gi_firefly = v.firefly >= 0 ? v.firefly : g_saved.firefly;
        rr::cfg::gi_accum = v.accum >= 0 ? v.accum : g_saved.accum;
        rr::cfg::gi_raw_clamp = v.raw_clamp >= 0 ? v.raw_clamp : g_saved.raw_clamp;
        rr::cfg::gi_assist = v.assist >= 0 ? v.assist : g_saved.assist;
        rr::cfg::rr_hit_distance = v.hit >= 0 ? v.hit == 1 : g_saved.hit;
        rr::cfg::rr_sharpen = v.sharpen >= 0 ? v.sharpen > 0 : g_saved.sharpen;
        rr::cfg::rr_sharpness = v.sharpen > 0 ? v.sharpen : g_saved.sharpness;
        rr::cfg::sky_mode = v.sky >= 0 ? v.sky : g_saved.sky;
        rr::cfg::reset_history = true;
    }

    void restore()
    {
        rr::cfg::rr_enabled = g_saved.rr_enabled;
        rr::cfg::rr_exposure_ev = g_saved.exposure_ev;
        rr::cfg::diag_neutral = g_saved.diag_neutral;
        rr::cfg::gi_temporal_mode = g_saved.gi_mode;
        rr::cfg::metal_alpha = g_saved.metal;
        rr::cfg::foliage_specular = g_saved.specular_fade;
        rr::cfg::gi_firefly = g_saved.firefly;
        rr::cfg::gi_accum = g_saved.accum;
        rr::cfg::gi_raw_clamp = g_saved.raw_clamp;
        rr::cfg::gi_assist = g_saved.assist;
        rr::cfg::rr_hit_distance = g_saved.hit;
        rr::gi_cancel_reset();
        rr::cfg::rr_sharpen = g_saved.sharpen;
        rr::cfg::rr_sharpness = g_saved.sharpness;
        rr::cfg::sky_mode = g_saved.sky;
        rr::cfg::reset_history = true;
    }

    ID3D12Resource *readback(ID3D12Device *dev, UINT64 size)
    {
        D3D12_HEAP_PROPERTIES heap {};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC buf {};
        buf.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buf.Width = size;
        buf.Height = 1;
        buf.DepthOrArraySize = 1;
        buf.MipLevels = 1;
        buf.SampleDesc.Count = 1;
        buf.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ID3D12Resource *r = nullptr;
        if (FAILED(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buf, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&r))))
            return nullptr;
        return r;
    }

    // Own GPU objects for the readback (created on first use, kept for the session).
    ID3D12Device *g_device = nullptr;
    ID3D12RootSignature *g_root = nullptr;
    ID3D12PipelineState *g_pso = nullptr;
    ID3D12DescriptorHeap *g_heap = nullptr;
    UINT g_increment = 0;
    constexpr int kDescriptorSets = 16;   // a set is rewritten only 16 frames later, long after the GPU used it
    int g_next_set = 0;
    ID3D12Resource *g_lum_tex = nullptr, *g_depth_tex = nullptr;
    uint32_t g_tex_w = 0, g_tex_h = 0, g_tex_dw = 0, g_tex_dh = 0;
    std::vector<ID3D12Resource *> g_retired_tex;   // textures of an earlier size, kept alive (rare)

    ID3D12Resource *own_texture(uint32_t w, uint32_t h)
    {
        D3D12_HEAP_PROPERTIES heap {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d {};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = w;
        d.Height = h;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.Format = DXGI_FORMAT_R32_FLOAT;
        d.SampleDesc.Count = 1;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ID3D12Resource *r = nullptr;
        if (FAILED(g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&r))))
            return nullptr;
        return r;
    }

    // Called with g_mutex held. Returns an error text, empty on success.
    // Readback pool, created with everything else when the bench starts: no GPU or readback resource is
    // ever created while the game records a frame (resources created mid-frame, with the VRAM at its
    // limit, are the main suspect of the intermittent DEVICE_HUNG seen in some runs).
    ID3D12Resource *g_rb_color[kRecordFrames] = {}, *g_rb_depth = nullptr;
    bool g_rb_busy[kRecordFrames] = {}, g_rb_depth_busy = false;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT g_color_fp {}, g_depth_fp {};
    UINT g_color_rows = 0, g_depth_rows = 0;
    UINT64 g_color_row = 0, g_depth_row = 0;
    uint32_t g_pool_w = 0, g_pool_h = 0, g_pool_dw = 0, g_pool_dh = 0;

    std::string ensure_gpu(ID3D12Device *dev, uint32_t w, uint32_t h, uint32_t dw, uint32_t dh)
    {
        if (g_device == nullptr)
        {
            g_device = dev;
            g_device->AddRef();
            if (FAILED(g_device->CreateRootSignature(0, g_bench_copy_cs, sizeof(g_bench_copy_cs), IID_PPV_ARGS(&g_root))))
                return rr::tr("CreateRootSignature failed", "CreateRootSignature fall\xC3\xB3");
            D3D12_COMPUTE_PIPELINE_STATE_DESC pso {};
            pso.pRootSignature = g_root;
            pso.CS = { g_bench_copy_cs, sizeof(g_bench_copy_cs) };
            if (FAILED(g_device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&g_pso))))
                return rr::tr("CreateComputePipelineState failed", "CreateComputePipelineState fall\xC3\xB3");
            D3D12_DESCRIPTOR_HEAP_DESC hd {};
            hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            hd.NumDescriptors = 4 * kDescriptorSets;
            hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            if (FAILED(g_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_heap))))
                return rr::tr("CreateDescriptorHeap failed", "CreateDescriptorHeap fall\xC3\xB3");
            g_increment = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }
        // The fence must exist before the first list with copies is submitted (bench_on_execute).
        if (g_fence == nullptr && FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence))))
            return rr::tr("CreateFence failed", "CreateFence falló");
        if (g_lum_tex == nullptr || g_tex_w != w || g_tex_h != h || g_tex_dw != dw || g_tex_dh != dh)
        {
            if (g_lum_tex) g_retired_tex.push_back(g_lum_tex);
            if (g_depth_tex) g_retired_tex.push_back(g_depth_tex);
            g_lum_tex = own_texture(w, h);
            g_depth_tex = own_texture(dw, dh);
            g_tex_w = w, g_tex_h = h, g_tex_dw = dw, g_tex_dh = dh;
            if (g_lum_tex == nullptr || g_depth_tex == nullptr)
                return rr::tr("could not create the add-on's own textures (VRAM)", "no se pudieron crear las texturas propias (VRAM)");
        }
        if (g_rb_color[0] == nullptr || g_pool_w != w || g_pool_h != h || g_pool_dw != dw || g_pool_dh != dh)
        {
            for (bool busy : g_rb_busy)
                if (busy)
                    return rr::tr("the previous measurement is still being read back", "la medici\xC3\xB3n anterior a\xC3\xBAn no termin\xC3\xB3 de leerse");
            if (g_rb_depth_busy)
                return rr::tr("the previous measurement is still being read back", "la medici\xC3\xB3n anterior a\xC3\xBAn no termin\xC3\xB3 de leerse");
            for (ID3D12Resource *&r : g_rb_color)
                if (r) { r->Release(); r = nullptr; }
            if (g_rb_depth) { g_rb_depth->Release(); g_rb_depth = nullptr; }
            const D3D12_RESOURCE_DESC ld = g_lum_tex->GetDesc(), ddesc = g_depth_tex->GetDesc();
            UINT64 ltotal = 0, dtotal = 0;
            g_device->GetCopyableFootprints(&ld, 0, 1, 0, &g_color_fp, &g_color_rows, &g_color_row, &ltotal);
            g_device->GetCopyableFootprints(&ddesc, 0, 1, 0, &g_depth_fp, &g_depth_rows, &g_depth_row, &dtotal);
            for (ID3D12Resource *&r : g_rb_color)
                if ((r = readback(g_device, ltotal)) == nullptr)
                    return rr::tr("could not create the readback buffers (memory)", "no se pudieron crear los buffers de lectura (memoria)");
            if ((g_rb_depth = readback(g_device, dtotal)) == nullptr)
                return rr::tr("could not create the readback buffers (memory)", "no se pudieron crear los buffers de lectura (memoria)");
            g_pool_w = w, g_pool_h = h, g_pool_dw = dw, g_pool_dh = dh;
        }
        return {};
    }

    // Records, on the native list, the copy of the game's output (as luminance) and depth into the
    // add-on's textures, then their copy into the readback buffers of `p`.
    void record_readback(ID3D12GraphicsCommandList *list, pending &p, uint32_t w, uint32_t h, uint32_t dw, uint32_t dh)
    {
        // Descriptors for this frame: u0 game output, u1 luminance, u2 depth copy, t0 game depth.
        const int set = g_next_set;
        g_next_set = (g_next_set + 1) % kDescriptorSets;
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_heap->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += SIZE_T(set) * 4 * g_increment;
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = g_heap->GetGPUDescriptorHandleForHeapStart();
        gpu.ptr += UINT64(set) * 4 * g_increment;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav {};
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uav.Format = g_output->GetDesc().Format;
        g_device->CreateUnorderedAccessView(g_output, nullptr, &uav, cpu);
        cpu.ptr += g_increment;
        uav.Format = DXGI_FORMAT_R32_FLOAT;
        g_device->CreateUnorderedAccessView(g_lum_tex, nullptr, &uav, cpu);
        cpu.ptr += g_increment;
        g_device->CreateUnorderedAccessView(g_depth_tex, nullptr, &uav, cpu);
        cpu.ptr += g_increment;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv {};
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;   // depth plane of R32G8X24
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        srv.Texture2D.PlaneSlice = 0;
        g_device->CreateShaderResourceView(g_depth, &srv, cpu);

        // NGX's writes to the output must be finished before the shader reads it. A global barrier
        // orders execution and memory without touching any resource's layout.
        ID3D12GraphicsCommandList7 *list7 = nullptr;
        if (SUCCEEDED(list->QueryInterface(IID_PPV_ARGS(&list7))))
        {
            D3D12_GLOBAL_BARRIER gb {};
            gb.SyncBefore = D3D12_BARRIER_SYNC_ALL;
            gb.SyncAfter = D3D12_BARRIER_SYNC_COMPUTE_SHADING;
            // Only accesses valid on every queue type: DLSS may run on a compute command list, where a
            // RENDER_TARGET access in a barrier is invalid (the bench with it hung the GPU, 4 oct 2026).
            gb.AccessBefore = D3D12_BARRIER_ACCESS_UNORDERED_ACCESS | D3D12_BARRIER_ACCESS_COPY_DEST;
            gb.AccessAfter = D3D12_BARRIER_ACCESS_UNORDERED_ACCESS | D3D12_BARRIER_ACCESS_SHADER_RESOURCE;
            D3D12_BARRIER_GROUP g {};
            g.Type = D3D12_BARRIER_TYPE_GLOBAL;
            g.NumBarriers = 1;
            g.pGlobalBarriers = &gb;
            rr::raw_barrier(list7, 1, &g);
            list7->Release();
        }

        // NGX already changed the descriptor heaps, root signature and pipeline during the evaluation
        // and the game re-binds its own afterwards (it runs Streamline without state tracking).
        list->SetDescriptorHeaps(1, &g_heap);
        list->SetComputeRootSignature(g_root);
        list->SetPipelineState(g_pso);
        list->SetComputeRootDescriptorTable(0, gpu);
        const uint32_t constants[5] = { w, h, dw, dh, p.depth != nullptr ? 1u : 0u };
        list->SetComputeRoot32BitConstants(1, 5, constants, 0);
        list->Dispatch((w + 7) / 8, (h + 7) / 8, 1);

        // Own textures only from here: legacy barriers are fine on resources the add-on owns.
        D3D12_RESOURCE_BARRIER b[2] = {};
        for (int i = 0; i < 2; ++i)
        {
            b[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b[i].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            b[i].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        }
        b[0].Transition.pResource = g_lum_tex;
        b[1].Transition.pResource = g_depth_tex;
        list->ResourceBarrier(2, b);
        D3D12_TEXTURE_COPY_LOCATION d {}, src {};
        d.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        d.pResource = p.color;
        d.PlacedFootprint = p.color_fp;
        src.pResource = g_lum_tex;
        list->CopyTextureRegion(&d, 0, 0, 0, &src, nullptr);
        if (p.depth)
        {
            d.pResource = p.depth;
            d.PlacedFootprint = p.depth_fp;
            src.pResource = g_depth_tex;
            list->CopyTextureRegion(&d, 0, 0, 0, &src, nullptr);
        }
        for (int i = 0; i < 2; ++i)
            std::swap(b[i].Transition.StateBefore, b[i].Transition.StateAfter);
        list->ResourceBarrier(2, b);
    }

    // Maps a finished readback and returns its rows packed (no pitch padding).
    bool read_rows(ID3D12Resource *r, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT &fp, UINT rows, UINT64 row, std::vector<uint8_t> &out)
    {
        void *mapped = nullptr;
        const D3D12_RANGE range { 0, SIZE_T(fp.Offset + uint64_t(fp.Footprint.RowPitch) * (rows - 1) + row) };
        if (FAILED(r->Map(0, &range, &mapped)))
            return false;
        out.resize(size_t(row) * rows);
        const uint8_t *src = static_cast<const uint8_t *>(mapped) + fp.Offset;
        for (UINT y = 0; y < rows; ++y)
            memcpy(out.data() + size_t(y) * row, src + size_t(y) * fp.Footprint.RowPitch, size_t(row));
        // Zero the buffer after reading it: if a later copy into this slot never reaches the GPU (the list
        // it was recorded on gets reset without being executed), the frame reads as all zeros and is
        // dropped instead of silently repeating an older frame. Convergence bench of 5 oct 2026: frames
        // identical to the previous variant appeared among the frames after a reset.
        uint8_t *dst = static_cast<uint8_t *>(mapped) + fp.Offset;
        for (UINT y = 0; y < rows; ++y)
            memset(dst + size_t(y) * fp.Footprint.RowPitch, 0, size_t(row));
        r->Unmap(0, &range);
        return true;
    }

    // Stops the sequence and restores the user's settings. Copies already recorded on the GPU stay in
    // g_pending: bench_on_present releases them once the fence says the GPU is done with them (their
    // data is dropped because g_sets is empty). Called with g_mutex held.
    void abort_locked(const std::string &why)
    {
        if (g_phase != phase::settle && g_phase != phase::record && g_phase != phase::finishing)
            return;
        restore();
        g_sets.clear();
        g_first_color.clear();
        g_phase = phase::idle;
        g_active = false;
        g_error = why;
        rr::log_info("bench: aborted");
    }

    void analysis_thread(std::vector<rr::metrics::frame_set> sets, std::vector<std::vector<uint8_t>> first_color, std::filesystem::path dir, rr::metrics::scene scene)
    {
        // Raw dumps for offline re-analysis (tools/rr_metrics.exe) and visual inspection.
        std::string meta = "near=" + std::to_string(scene.near_plane) + "\nfar=" + std::to_string(scene.far_plane) + "\nnear_band=" + std::to_string(scene.near_band_m) + "\ndescription=" + scene.description +
                           "\nlang=" + (scene.spanish ? "es" : "en") + "\n";
        for (const std::string &c : scene.checks)
            meta += "check=" + c + "\n";
        for (size_t i = 0; i < sets.size(); ++i)
        {
            const rr::metrics::frame_set &fs = sets[i];
            meta += "variant=" + std::to_string(i) + "|" + fs.name + "|" + (fs.is_reference ? "1" : "0") + "|" + std::to_string(fs.luminance.size()) + "|" +
                    std::to_string(fs.frames_expected) + "\n";
            if (!fs.frame_number.empty())
            {
                meta += "frames=" + std::to_string(i) + "|";
                for (size_t k = 0; k < fs.frame_number.size(); ++k)
                    meta += (k ? "," : "") + std::to_string(fs.frame_number[k]);
                meta += "\n";
            }
            const std::wstring base = (dir / (L"v" + std::to_wstring(i))).wstring();
            for (size_t k = 0; k < fs.luminance.size(); ++k)
                rr::image::write_dds(base + L"_f" + std::to_wstring(k) + L"_lum.dds", DXGI_FORMAT_R32_FLOAT, fs.width, fs.height,
                                     reinterpret_cast<const uint8_t *>(fs.luminance[k].data()), uint64_t(fs.width) * 4);
            if (!fs.depth.empty())
                rr::image::write_dds(base + L"_depth.dds", DXGI_FORMAT_R32_FLOAT, fs.depth_width, fs.depth_height,
                                     reinterpret_cast<const uint8_t *>(fs.depth.data()), uint64_t(fs.depth_width) * 4);
            if (i < first_color.size() && !first_color[i].empty())
                rr::image::write_dds(base + L"_f0_color.dds", DXGI_FORMAT_R11G11B10_FLOAT, fs.width, fs.height, first_color[i].data(), uint64_t(fs.width) * 4);
        }
        {
            FILE *f = _wfopen((dir / L"meta.txt").wstring().c_str(), L"wb");
            if (f)
            {
                fwrite(meta.data(), 1, meta.size(), f);
                fclose(f);
            }
        }
        const std::string summary = rr::metrics::analyze(sets, scene, dir.wstring());
        rr::log_info("bench: analysis written to " + dir.string());
        std::lock_guard lock(g_mutex);
        g_summary = summary;
        g_phase = phase::done;
    }
    // The fence for a list with copies must be signalled AFTER that list is on the queue. ReShade's
    // execute_command_list event fires BEFORE ID3D12CommandQueue::ExecuteCommandLists: a signal from it
    // could complete before the copy ran, and the CPU then read a half-written (mostly black) frame
    // (disocclusion bench, 5 oct 2026). The native ExecuteCommandLists is hooked and signals right after.
    using execute_fn = void(STDMETHODCALLTYPE *)(ID3D12CommandQueue *, UINT, ID3D12CommandList *const *);
    execute_fn g_execute_original = nullptr;
    std::atomic<int> g_execute_hook { 0 };   // 0 not tried, 1 installed, 2 failed (signal from the event, as before)

    void signal_after_locked(ID3D12CommandQueue *queue, UINT count, ID3D12CommandList *const *lists)
    {
        if (g_pending.empty() || g_fence == nullptr)
            return;
        bool ours = false;
        for (UINT k = 0; k < count; ++k)
            for (const pending &p : g_pending)
                ours |= p.list == lists[k] && p.fence_value == 0;
        if (!ours)
            return;
        queue->Signal(g_fence, ++g_fence_value);
        for (UINT k = 0; k < count; ++k)
            for (pending &p : g_pending)
                if (p.list == lists[k] && p.fence_value == 0)
                    p.fence_value = g_fence_value;
    }

    void STDMETHODCALLTYPE execute_hook(ID3D12CommandQueue *self, UINT count, ID3D12CommandList *const *lists)
    {
        rr::hook_guard guard;
        g_execute_original(self, count, lists);
        if (g_pending_count.load(std::memory_order_relaxed) == 0)
            return;
        std::lock_guard lock(g_mutex);
        signal_after_locked(self, count, lists);
    }

    void ensure_execute_hook(ID3D12CommandQueue *queue)
    {
        if (g_execute_hook.load(std::memory_order_relaxed) != 0 || !rr::hooks_allowed())
            return;
        int expected = 0;
        if (!g_execute_hook.compare_exchange_strong(expected, 2))
            return;
        constexpr size_t kExecuteCommandLists = 10;   // ID3D12CommandQueue vtable slot
        void *target = (*reinterpret_cast<void ***>(queue))[kExecuteCommandLists];
        MH_STATUS s = rr::ensure_minhook() ? MH_CreateHook(target, reinterpret_cast<void *>(execute_hook), reinterpret_cast<void **>(&g_execute_original)) : MH_ERROR_NOT_INITIALIZED;
        if (s == MH_OK)
            s = MH_EnableHook(target);
        if (s != MH_OK)
        {
            rr::log_warn(std::string("bench: ExecuteCommandLists hook failed (") + MH_StatusToString(s) + "), the fence is signalled from the ReShade event");
            return;
        }
        g_execute_hook = 1;
        rr::log_info("bench: ExecuteCommandLists hook installed (fence signalled after each submission with copies)");
    }
}

namespace rr
{
    std::atomic<bool> bench_include_neutral { false };
    std::atomic<bool> bench_include_gi_original { false };
    std::atomic<bool> bench_include_exposure { false };
    std::atomic<bool> bench_include_fireflies { false };
    std::atomic<bool> bench_include_sharpen { false };
    std::atomic<bool> bench_include_gi_modes { false };

    ID3D12Resource *dlss_output_resource()
    {
        std::lock_guard lock(g_mutex);
        return g_output;
    }

    void bench_set_tags(ID3D12Resource *output, uint32_t output_state, ID3D12Resource *depth, uint32_t depth_state)
    {
        std::lock_guard lock(g_mutex);
        if (output != nullptr)
        {
            g_output = output;
            g_output_state = D3D12_RESOURCE_STATES(output_state);
        }
        if (depth != nullptr)
        {
            g_depth = depth;
            g_depth_state = D3D12_RESOURCE_STATES(depth_state);
        }
    }

    bool bench_running()
    {
        std::lock_guard lock(g_mutex);
        return g_phase == phase::settle || g_phase == phase::record || g_phase == phase::finishing || g_phase == phase::analysing;
    }

    void bench_start(int focus)
    {
        const rr_snapshot s = rr_get_snapshot();
        std::lock_guard lock(g_mutex);
        if (g_phase == phase::settle || g_phase == phase::record || g_phase == phase::finishing || g_phase == phase::analysing)
            return;
        g_error.clear();
        g_summary.clear();
        if (s.state != 1)
        {
            g_error = tr("RR is not available: nothing to measure.", "RR no est\xC3\xA1 disponible: no se puede medir.");
            return;
        }
        if (g_output == nullptr || g_depth == nullptr)
        {
            g_error = tr("The DLSS textures have not been seen yet: get into the game and try again.",
                         "Todav\xC3\xAD" "a no se vieron las texturas de DLSS: entra en partida y vuelve a intentarlo.");
            return;
        }
        {
            // Every GPU and readback resource the bench needs is created now, before recording, on the
            // NATIVE device. g_output->GetDevice() gave a device whose descriptor heap crashed the native
            // SetDescriptorHeaps (access violation in D3D12Core, 4 oct 2026: ReShade's proxy device).
            ID3D12Device *dev = guides_native_device();
            if (dev == nullptr)
            {
                g_error = tr("No native device yet (the RR guides have not run): turn RR on and try again.",
                             "Todav\xC3\xAD" "a no hay dispositivo nativo (las gu\xC3\xAD" "as de RR no han corrido): activa RR y vuelve a intentarlo.");
                return;
            }
            dev->AddRef();
            const D3D12_RESOURCE_DESC cd = g_output->GetDesc(), dd = g_depth->GetDesc();
            const std::string err = ensure_gpu(dev, uint32_t(cd.Width), cd.Height, uint32_t(dd.Width), dd.Height);
            dev->Release();
            if (!err.empty())
            {
                g_error = tr("Cannot measure: ", "No se puede medir: ") + err;
                return;
            }
        }
        g_saved = { cfg::rr_enabled.load(), cfg::diag_neutral.load(), cfg::rr_exposure_ev.load(), cfg::gi_temporal_mode.load() };
        g_variants.clear();
        g_saved.metal = cfg::metal_alpha.load();
        g_saved.specular_fade = cfg::foliage_specular.load();
        g_saved.firefly = cfg::gi_firefly.load();
        g_saved.accum = cfg::gi_accum.load();
        g_saved.raw_clamp = cfg::gi_raw_clamp.load();
        g_saved.assist = cfg::gi_assist.load();
        g_saved.hit = cfg::rr_hit_distance.load();
        g_saved.sharpen = cfg::rr_sharpen.load();
        g_saved.sharpness = cfg::rr_sharpness.load();
        g_saved.sky = cfg::sky_mode.load();
        g_variants.push_back({ tr("DLSS-SR (game)", "DLSS normal"), false, 0, false, -1 });
        const int ev = g_saved.exposure_ev;
        g_variants.push_back({ tr("RR (your settings)", "RR (tus ajustes)"), true, ev, false, -1 });
        g_focus = focus;
        if (focus == 1)
        {
            // Car reflections: the guide settings that decide how RR treats a glossy body, and the GI mode
            // (the user saw raw noise in the reflections of a metallic red car with "Full RR").
            variant m { g_saved.metal ? tr("RR without metalness", "RR sin metalness") : tr("RR with metalness", "RR con metalness"), true, ev, false, -1 };
            m.metal = g_saved.metal ? 0 : 1;
            g_variants.push_back(m);
            variant sp { g_saved.specular_fade ? tr("RR without specular fade", "RR sin atenuar especular") : tr("RR with specular fade", "RR con especular atenuado"), true, ev, false, -1 };
            sp.specular_fade = g_saved.specular_fade ? 0 : 1;
            g_variants.push_back(sp);
            if (g_saved.gi_mode == 4)
                g_variants.push_back({ tr("RR with original GI", "RR con GI original"), true, ev, false, 0 });
            else
            {
                variant c { tr("Full RR", "RR completo"), true, ev, false, 4 };
                g_variants.push_back(c);
                variant cm { g_saved.metal ? tr("Full RR without metalness", "RR completo sin metalness") : tr("Full RR with metalness", "RR completo con metalness"), true, ev, false, 4 };
                cm.metal = g_saved.metal ? 0 : 1;
                g_variants.push_back(cm);
            }
        }
        else if (focus == 2)
        {
            // Night dots of "RR completo". The absolute radiance cap (5 oct 2026) did nothing measurable (dim
            // scenes never reach it) and the spatial v2 acts after the 3x3 resolve has spread the dot. The
            // isolated raw sample clamp acts before the resolve: measured alone (spatial off) at each level,
            // then the medium level with v2 media on top.
            static const char *const names_en[] = { "Full RR, no clamp", "Full RR, isolated sample k=4", "Full RR, isolated sample k=2", "Full RR, isolated sample k=1" };
            static const char *const names_es[] = { "RR completo, sin recorte", "RR completo, muestra aislada k=4", "RR completo, muestra aislada k=2", "RR completo, muestra aislada k=1" };
            for (int level = 0; level < 4; ++level)
            {
                variant v { tr(names_en[level], names_es[level]), true, ev, false, 4 };
                v.firefly = 0;
                v.accum = 1;
                v.raw_clamp = level;
                g_variants.push_back(v);
            }
            variant both { tr("Full RR, isolated sample k=2 + v2 medium", "RR completo, muestra aislada k=2 + recorte v2 media"), true, ev, false, 4 };
            both.firefly = 2;
            both.accum = 1;
            both.raw_clamp = 2;
            g_variants.push_back(both);
        }
        else if (focus == 3)
        {
            // Disocclusion (what driving shows: pixels that just came into view, or came back after leaving the
            // screen). Each mode is measured converged, then frames 1..8 right after dropping BOTH histories in
            // the same frame: RR's (reset) and the game's GI history (one temporal-filter dispatch with the
            // previous count read as 0). The whole screen then behaves like a disoccluded region, with a still
            // camera so the converged frames are the ground truth. Worst case: in motion only borders and
            // revealed areas lose their history, and RR can borrow from neighbours that kept theirs.
            // The report pairs "<label> after ..." / "<label> tras ..." with "<label> converged" / "<label> convergido"
            // by name (metrics.cpp), so both suffixes must stay as they are.
            struct mode_variant { const char *label_en, *label_es; int gi; int assist; int hit; };
            const mode_variant modes[] = { { "RR original GI", "RR GI original", 0, 0, 0 }, { "Full RR", "RR completo", 4, 0, 0 },
                                           { "Full RR + assist 8", "RR completo + asistida 8", 4, 2, 0 },
                                           { "Full RR + assist 8 + hit distance", "RR completo + asistida 8 + distancia", 4, 2, 1 } };
            for (const mode_variant &m : modes)
            {
                const std::string label = tr(m.label_en, m.label_es);
                variant converged { label + tr(" converged", " convergido"), true, ev, false, m.gi };
                converged.assist = m.assist;
                converged.hit = m.hit;
                g_variants.push_back(converged);
                variant fresh { label + tr(" after disocclusion (frames 1-8)", " tras desoclusi\xC3\xB3n (frames 1-8)"), true, ev, false, m.gi };
                fresh.assist = m.assist;
                fresh.hit = m.hit;
                fresh.after_reset = true;
                fresh.gi_reset = true;
                g_variants.push_back(fresh);
            }
        }
        else
        {
            if (bench_include_gi_modes)
            {
                // The game's GI filter: original vs longer history, against the slow "boil" on walls.
                static const char *const gi_names_en[] = { "RR with original GI", "RR with GI history x2", "RR with GI history x4" };
                static const char *const gi_names_es[] = { "RR con GI original", "RR con GI historial x2", "RR con GI historial x4" };
                for (int mode = 0; mode < 3; ++mode)
                    if (mode != g_saved.gi_mode)
                        g_variants.push_back({ tr(gi_names_en[mode], gi_names_es[mode]), true, ev, false, mode });
            }
            {
                // Diffuse hit distance guide for RR: the opposite of the user's setting, same GI mode.
                variant h { g_saved.hit ? tr("RR without hit distance", "RR sin distancia de impacto") : tr("RR with hit distance", "RR con distancia de impacto"), true, ev, false, -1 };
                h.hit = g_saved.hit ? 0 : 1;
                g_variants.push_back(h);
            }
            if (bench_include_sharpen)
                for (int strength : { 30, 60, 90 })
                {
                    variant v { tr("RR with sharpening ", "RR con afinado ") + std::to_string(strength) + " %", true, ev, false, -1 };
                    v.sharpen = strength;
                    g_variants.push_back(v);
                }
            if (bench_include_neutral)
                g_variants.push_back({ tr("RR neutral guides", "RR gu\xC3\xAD" "as neutras"), true, ev, true, -1 });
            if (bench_include_gi_original && g_saved.gi_mode != 0)
                g_variants.push_back({ tr("RR with original GI", "RR con GI original"), true, ev, false, 0 });
        }

        // Control: the user's RR measured again at the end (before any GI-changing variant). If it differs from the first RR
        // variant, something drifted during the run (camera, weather, a car) and differences of that
        // size between variants mean nothing (night bench of 4 oct 2026: first RR variant x2 noise).
        g_variants.push_back({ tr("RR (your settings) - control at the end", "RR (tus ajustes) - control al final"), true, ev, false, -1 });
        // Variants that change the GI filters go last (stable sort keeps the rest in order).
        std::stable_sort(g_variants.begin() + 1, g_variants.end(), [&](const variant &a, const variant &b) {
            return (effective_gi(a, g_saved.gi_mode) != g_saved.gi_mode) < (effective_gi(b, g_saved.gi_mode) != g_saved.gi_mode);
        });
        g_sets.assign(g_variants.size(), {});
        g_first_color.assign(g_variants.size(), {});
        for (size_t i = 0; i < g_variants.size(); ++i)
        {
            g_sets[i].name = g_variants[i].name;
            g_sets[i].is_reference = !g_variants[i].rr;
            g_sets[i].frames_expected = kRecordFrames;
            g_sets[i].after_reset = g_variants[i].after_reset;
        }
        char stamp[32];
        const std::time_t now = std::time(nullptr);
        std::tm tm {};
        localtime_s(&tm, &now);
        std::strftime(stamp, sizeof(stamp), "bench-%Y%m%d-%H%M%S", &tm);
        g_dir = capture_root() / stamp;
        std::error_code ec;
        std::filesystem::create_directories(g_dir, ec);
        g_scene.near_plane = s.camera_near > 0 ? s.camera_near : 0.1f;
        g_scene.far_plane = s.camera_far > 0 ? s.camera_far : 50000.0f;
        g_scene.near_band_m = g_focus == 1 ? 8.0f : 15.0f;
        g_scene.checks.clear();
        g_scene.spanish = lang_es();
        g_stale_frames = 0;
        const char *focus_tag = focus == 1 ? tr("[car reflections] ", "[reflejos del carro] ") : focus == 2 ? tr("[night fireflies] ", "[destellos de noche] ") :
                                focus == 3 ? tr("[disocclusion] ", "[desoclusi\xC3\xB3n] ") : "";
        g_scene.description = std::string(focus_tag) + "DLSS " + std::to_string(s.render_w) + "x" + std::to_string(s.render_h) + " -> " + std::to_string(s.out_w) + "x" +
                              std::to_string(s.out_h) + tr(", RR preset ", ", preset RR ") + std::to_string(cfg::preset.load()) + tr(", GI mode ", ", modo GI ") + std::to_string(g_saved.gi_mode) +
                              ", " + std::to_string(kRecordFrames) + tr(" frames per variant (one every ", " frames por variante (uno cada ") + std::to_string(kRecordStride) +
                              tr(") after ", ") tras ") + std::to_string(kSettleFrames) + tr(" settle frames (", " de estabilizaci\xC3\xB3n (") + std::to_string(kSettleFramesGi) +
                              tr(" after switching between DLSS-SR and RR or the GI)", " tras cambiar entre DLSS normal y RR o el GI)");
        g_current = 0;
        g_counter = 0;
        g_recorded = 0;
        g_settle_target = kSettleFrames;
        apply(g_variants[0]);
        g_output_seen = {};
        g_depth_seen = {};
        g_layouts_logged = false;
        g_phase = phase::settle;
        g_active = true;
        log_info("bench: started (" + std::to_string(g_variants.size()) + " variants) -> " + g_dir.string());
    }

    void bench_abort()
    {
        std::lock_guard lock(g_mutex);
        abort_locked(tr("Measurement cancelled; settings restored.", "Medici\xC3\xB3n cancelada; ajustes restaurados."));
    }

    void bench_on_barrier(UINT32 count, const D3D12_BARRIER_GROUP *groups)
    {
        std::lock_guard lock(g_mutex);
        for (UINT32 i = 0; i < count; ++i)
        {
            if (groups[i].Type != D3D12_BARRIER_TYPE_TEXTURE)
                continue;
            for (UINT32 k = 0; k < groups[i].NumBarriers; ++k)
            {
                const D3D12_TEXTURE_BARRIER &tb = groups[i].pTextureBarriers[k];
                observed *o = tb.pResource == g_output ? &g_output_seen : tb.pResource == g_depth ? &g_depth_seen : nullptr;
                if (o != nullptr)
                    *o = { true, tb.LayoutAfter, tb.SyncAfter, tb.AccessAfter };
            }
        }
    }

    bool bench_active()
    {
        return g_active.load(std::memory_order_relaxed);
    }

    // Right after slEvaluateFeature returned: everything DLSS records for this frame (NGX's work and
    // Streamline's own passes after it) is already on the list. At the END of NGX's marker the DLSS-RR
    // output was not complete yet (frames read half black, 4 oct 2026). The list is the native one NGX
    // used, remembered by the marker hook on this thread (the game may pass ReShade's proxy).
    void bench_after_evaluate(bool rr_used)
    {
        ID3D12GraphicsCommandList *list = rr::last_ngx_list();
        std::lock_guard lock(g_mutex);
        if (g_phase == phase::settle)
        {
            if (++g_counter >= g_settle_target)
            {
                g_phase = phase::record;
                g_recorded = 0;
                g_stride_counter = 0;
                g_record_eval = 0;
                // Convergence variant: the next evaluation starts from an empty RR history (what a pixel
                // that just came into view gets while driving), and the 8 frames after it are recorded.
                if (g_variants[g_current].after_reset)
                    rr::cfg::reset_history = true;
                if (g_variants[g_current].gi_reset)
                    rr::gi_request_reset();
                g_reset_note.clear();
            }
            return;
        }
        if (g_phase != phase::record || list == nullptr || g_output == nullptr || g_depth == nullptr)
            return;
        ++g_record_eval;
        if (g_stride_counter++ % (g_variants[g_current].after_reset ? 1 : kRecordStride) != 0)
            return;

        const D3D12_RESOURCE_DESC cd = g_output->GetDesc(), dd = g_depth->GetDesc();
        const uint32_t w = uint32_t(cd.Width), h = cd.Height, dw = uint32_t(dd.Width), dh = dd.Height;
        if (!g_layouts_logged)
        {
            g_layouts_logged = true;
            const D3D12_COMMAND_LIST_TYPE type = list->GetType();
            rr::log_info(std::string("bench: DLSS command list type ") +
                         (type == D3D12_COMMAND_LIST_TYPE_DIRECT ? "DIRECT" : type == D3D12_COMMAND_LIST_TYPE_COMPUTE ? "COMPUTE" : std::to_string(int(type))));
            rr::log_info("bench: output " + rr::format_name(cd.Format) + " flags " + rr::hex(uint32_t(cd.Flags)) + " state " + rr::hex(uint32_t(g_output_state)) +
                         ", depth " + rr::format_name(dd.Format) + " state " + rr::hex(uint32_t(g_depth_state)) + "; last game layouts seen: output " +
                         (g_output_seen.seen ? std::to_string(int(g_output_seen.layout)) : std::string("none")) + ", depth " +
                         (g_depth_seen.seen ? std::to_string(int(g_depth_seen.layout)) : std::string("none")));
        }
        // The shader reads the output through a UAV and the depth through an SRV, in the layouts the game
        // declares to Streamline for this evaluation. Anything else: stop without touching anything.
        if (g_output_state != D3D12_RESOURCE_STATE_UNORDERED_ACCESS || (g_depth_state & (D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)) == 0 ||
            (cd.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0 || cd.Format != DXGI_FORMAT_R11G11B10_FLOAT || dd.Format != DXGI_FORMAT_R32G8X24_TYPELESS)
        {
            abort_locked(tr("unexpected DLSS format or state (output ", "formato o estado de DLSS inesperado (salida ") + rr::format_name(cd.Format) + " " +
                         rr::hex(uint32_t(g_output_state)) + tr(", depth ", ", profundidad ") + rr::format_name(dd.Format) + " " + rr::hex(uint32_t(g_depth_state)) +
                         tr("): nothing is copied", "): no se copia nada"));
            return;
        }
        if (g_device == nullptr || g_lum_tex == nullptr || g_rb_color[0] == nullptr || w != g_pool_w || h != g_pool_h || dw != g_pool_dw || dh != g_pool_dh)
        {
            abort_locked(tr("the resolution changed during the measurement", "la resoluci\xC3\xB3n cambi\xC3\xB3 durante la medici\xC3\xB3n"));
            return;
        }

        // Disocclusion bench: the GI reset must have reached the GPU in the same frame as frame 1.
        if (g_recorded == 0 && g_variants[g_current].gi_reset)
        {
            g_reset_note = rr::gi_reset_status();
            if (g_reset_note.empty())
            {
                rr::gi_cancel_reset();
                g_reset_note = tr("GI reset LATE (after frame 1): this variant is not valid", "GI reiniciado TARDE (después del frame 1): esta variante no vale");
            }
        }
        // Pool slot of this frame; if the previous variant's copy in it was not read yet, skip a frame (the
        // frame number keeps the real position, so the disocclusion table does not shift).
        const int slot = g_recorded;
        if (g_rb_busy[slot] || (slot == 0 && g_rb_depth_busy))
            return;
        pending p;
        p.variant = g_current;
        p.frame = g_recorded;
        p.frame_number = g_record_eval;
        p.slot = slot;
        p.ok_frame = rr_used == g_variants[g_current].rr;
        p.color = g_rb_color[slot];
        p.color_fp = g_color_fp;
        p.color_rows = g_color_rows;
        p.color_row = g_color_row;
        p.depth = slot == 0 ? g_rb_depth : nullptr;   // the scene is still: one depth per variant
        p.depth_fp = g_depth_fp;
        p.depth_rows = g_depth_rows;
        p.depth_row = g_depth_row;
        g_rb_busy[slot] = true;
        if (p.depth)
            g_rb_depth_busy = true;
        record_readback(list, p, w, h, dw, dh);
        p.list = list;
        g_pending.push_back(p);
        g_pending_count = int(g_pending.size());

        if (++g_recorded >= kRecordFrames)
        {
            // What the GPU really ran for this variant: the GI shader variants the settings select and
            // when each was last swapped in (a twin used within the last 2 s counts as applied).
            {
                const gi_snapshot gs = gi_get_snapshot();
                const uint64_t now = GetTickCount64();
                auto part = [&](const char *what, int want) {
                    if (want < 0)
                        return std::string(what) + " original";
                    const uint64_t tick = gs.variant_tick[want];
                    const bool live = tick != 0 && now - tick < 2000;
                    return std::string(what) + " " + gi_variant_name(want) +
                           (live ? tr(" (applied, ", " (aplicado, ") + std::to_string(gs.variant_swaps[want]) + tr(" uses)", " usos)") : std::string(tr(" (NOT APPLIED)", " (NO APLICADO)")));
                };
                std::string check = g_variants[g_current].name + ": " + (g_variants[g_current].rr ? "RR" : tr("DLSS-SR", "DLSS normal")) + ", GI " +
                                    part("temporal", gs.want_temporal) + ", " + part(tr("spatial", "espacial"), gs.want_spatial);
                if (g_variants[g_current].after_reset)
                    check += tr(", RR history reset right before frame 1", ", historial de RR reiniciado justo antes del frame 1");
                if (g_variants[g_current].gi_reset)
                    check += ", " + g_reset_note;
                rr::log_info("bench check: " + check);
                g_scene.checks.push_back(check);
            }
            if (++g_current < int(g_variants.size()))
            {
                // Switching between DLSS-SR and RR, or changing the GI filters, needs longer to settle: the
                // first RR variant after DLSS-SR still flickered (noise x3) with 90 frames.
                const auto assist_of = [](const variant &v) { return (v.assist >= 0 ? v.assist : g_saved.assist) * 2 + (v.hit >= 0 ? v.hit : int(g_saved.hit)); };
                const bool gi_changed = effective_gi(g_variants[g_current], g_saved.gi_mode) != effective_gi(g_variants[g_current - 1], g_saved.gi_mode) ||
                                        assist_of(g_variants[g_current]) != assist_of(g_variants[g_current - 1]);
                const bool rr_changed = g_variants[g_current].rr != g_variants[g_current - 1].rr;
                g_settle_target = gi_changed || rr_changed ? kSettleFramesGi : kSettleFrames;
                apply(g_variants[g_current]);
                g_phase = phase::settle;
                g_counter = 0;
            }
            else
            {
                restore();   // the user gets their settings back right away; reading back continues
                g_phase = phase::finishing;
                g_active = false;
            }
        }
    }

    void bench_on_present(ID3D12CommandQueue *queue)
    {
        std::vector<pending> ready;
        {
            std::lock_guard lock(g_mutex);
            if (g_pending.empty() && g_phase != phase::finishing)
                return;
            if (g_fence == nullptr && queue != nullptr)
            {
                ID3D12Device *dev = nullptr;
                if (SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&dev))))
                {
                    dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence));
                    dev->Release();
                }
            }
            if (g_fence == nullptr)
                return;
            // A readback is mapped (and later released) only after the fence signalled right behind the
            // submission of ITS command list completed. The present thread is not the render thread: a
            // signal at present time could complete before the list was even submitted, and the GPU then
            // wrote into a released buffer (DEVICE_HUNG, 4 oct 2026).
            const UINT64 done = g_fence->GetCompletedValue();
            for (size_t i = 0; i < g_pending.size();)
            {
                if (g_pending[i].fence_value != 0 && g_pending[i].fence_value <= done)
                {
                    ready.push_back(g_pending[i]);
                    g_pending.erase(g_pending.begin() + long(i));
                    g_pending_count = int(g_pending.size());
                }
                else
                    ++i;
            }
        }

        // Read back outside the lock (a few ms per frame).
        for (pending &p : ready)
        {
            std::vector<uint8_t> color, depth;
            const bool ok_color = read_rows(p.color, p.color_fp, p.color_rows, p.color_row, color);
            const bool ok_depth = p.depth != nullptr && read_rows(p.depth, p.depth_fp, p.depth_rows, p.depth_row, depth);
            std::lock_guard lock(g_mutex);
            if (p.slot >= 0)
                g_rb_busy[p.slot] = false;
            if (p.depth)
                g_rb_depth_busy = false;
            if (p.variant >= int(g_sets.size()))
                continue;
            rr::metrics::frame_set &fs = g_sets[p.variant];
            const uint32_t w = p.color_fp.Footprint.Width, h = p.color_fp.Footprint.Height;
            const bool stale = ok_color && std::all_of(color.begin(), color.end(), [](uint8_t b) { return b == 0; });
            if (stale)
            {
                g_stale_frames++;
                rr::log_info("bench: frame " + std::to_string(p.frame) + " of variant " + std::to_string(p.variant) + " never reached the GPU (empty readback), dropped");
            }
            if (ok_color && !stale && p.ok_frame && p.color_fp.Footprint.Format == DXGI_FORMAT_R32_FLOAT)
            {
                fs.width = w;
                fs.height = h;
                std::vector<float> lum(size_t(w) * h);
                for (uint32_t y = 0; y < h; ++y)
                    memcpy(lum.data() + size_t(y) * w, color.data() + size_t(y) * p.color_row, size_t(w) * 4);
                fs.luminance.push_back(std::move(lum));
                fs.frame_number.push_back(p.frame_number);
            }
            if (ok_depth)
            {
                // Depth copied by the shader into an R32_FLOAT texture.
                const uint32_t dw = p.depth_fp.Footprint.Width, dh = p.depth_fp.Footprint.Height;
                fs.depth_width = dw;
                fs.depth_height = dh;
                fs.depth.resize(size_t(dw) * dh);
                for (uint32_t y = 0; y < dh; ++y)
                    memcpy(fs.depth.data() + size_t(y) * dw, depth.data() + size_t(y) * p.depth_row, size_t(dw) * 4);
            }
        }

        std::lock_guard lock(g_mutex);
        if (g_phase == phase::finishing && g_pending.empty())
        {
            g_phase = phase::analysing;
            g_scene.checks.push_back(tr("frames dropped because their copy never reached the GPU: ", "frames descartados porque su copia nunca llegó a la GPU: ") + std::to_string(g_stale_frames));
            std::thread(analysis_thread, std::move(g_sets), std::move(g_first_color), g_dir, g_scene).detach();
            g_sets.clear();
            g_first_color.clear();
        }
    }

    // ReShade's execute_command_list event (BEFORE the submission): installs the native hook that signals
    // after it; only if that hook failed does it signal here, as before.
    void bench_on_execute(ID3D12CommandQueue *queue, void *native_list)
    {
        if (queue == nullptr || native_list == nullptr)
            return;
        ensure_execute_hook(queue);
        if (g_execute_hook.load(std::memory_order_relaxed) == 1 || g_pending_count.load(std::memory_order_relaxed) == 0)
            return;
        std::lock_guard lock(g_mutex);
        ID3D12CommandList *const list = reinterpret_cast<ID3D12CommandList *>(native_list);
        signal_after_locked(queue, 1, &list);
    }

    bench_snapshot bench_get_snapshot()
    {
        bench_snapshot b;
        std::lock_guard lock(g_mutex);
        b.error = g_error;
        b.summary = g_summary;
        b.folder = g_dir.empty() ? std::string() : g_dir.string();
        b.variants = int(g_variants.size());
        b.current = g_current;
        switch (g_phase)
        {
        case phase::idle: b.state = tr("ready", "lista"); break;
        case phase::settle:
            b.running = true;
            b.state = tr("settling \"", "estabilizando \"") + g_variants[g_current].name + "\" (" + std::to_string(g_counter) + "/" + std::to_string(g_settle_target) + ")";
            break;
        case phase::record:
            b.running = true;
            b.state = tr("recording \"", "grabando \"") + g_variants[g_current].name + "\" (" + std::to_string(g_recorded) + "/" + std::to_string(kRecordFrames) + ")";
            break;
        case phase::finishing:
            b.running = true;
            b.state = tr("reading the last frames back from the GPU", "leyendo los \xC3\xBAltimos frames de la GPU");
            break;
        case phase::analysing:
            b.running = true;
            b.state = tr("analysing and saving (a few seconds)", "analizando y guardando (unos segundos)");
            break;
        case phase::done: b.state = tr("done", "terminada"); break;
        }
        b.progress = g_variants.empty() ? 0.0f :
                     std::min(1.0f, (float(g_current) + (g_phase == phase::record ? 0.5f + 0.5f * float(g_recorded) / kRecordFrames :
                                                         g_phase == phase::settle ? 0.5f * float(g_counter) / float(g_settle_target) : 1.0f)) / float(g_variants.size()));
        return b;
    }
}
