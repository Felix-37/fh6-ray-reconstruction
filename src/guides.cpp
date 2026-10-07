// Phase 3: builds the DLSS Ray Reconstruction guide buffers every frame.
//
// 1. When the game ends its RT G-buffer pass (an enhanced barrier moves the R32_UINT packed
//    normal/gloss target and, a few barriers later, the RGBA8_SRGB albedo target from
//    RENDER_TARGET to SHADER_RESOURCE), both are copied into textures owned by the add-on, using a
//    layout round trip built from the game's own barrier (exact layout, subresource 0 only).
// 2. Right before the game's slEvaluateFeature(kFeatureDLSS), a compute shader turns the copies
//    into normal+roughness, diffuse albedo and specular albedo. The game disables Streamline's
//    command-list state tracking, so it already re-binds its state after DLSS evaluation, and DLSS
//    itself overwrites the same compute state (heaps, root signature, pipeline) the shader uses.
//
// Add-on textures are moved explicitly from and back to COMMON inside every command list that uses
// them, so no state is carried between the game's command lists (ALLOW_SIMULTANEOUS_ACCESS also
// allows COMMON -> UNORDERED_ACCESS for textures).

#include "common.hpp"
#include "guides_cs.h"

#include <algorithm>
#include <atomic>
#include <initializer_list>
#include <vector>
#include <mutex>

namespace
{
    struct camera_data
    {
        float right[3] {}, up[3] {}, fwd[3] {};
        float proj_x = 0, proj_y = 0;
        bool valid = false;
    };

    std::mutex g_mutex;
    camera_data g_camera;

    ID3D12Device *g_device = nullptr;
    ID3D12RootSignature *g_root_signature = nullptr;
    ID3D12PipelineState *g_pipeline = nullptr;
    ID3D12DescriptorHeap *g_heap = nullptr;
    UINT g_increment = 0;
    UINT g_heap_half = 0;   // descriptor set in use (0 or 1); flips on every resize
    struct retired { ID3D12Resource *res; uint64_t frame; };
    std::vector<retired> g_retired;   // old guide textures, released once the GPU is surely done
    ID3D12Resource *g_albedo = nullptr, *g_packed = nullptr;               // copies of the game's targets
    ID3D12Resource *g_normal_roughness = nullptr, *g_diffuse = nullptr, *g_specular = nullptr;
    ID3D12Resource *g_gi = nullptr, *g_hit = nullptr;   // copy of the game's final GI; diffuse hit distance guide
    uint64_t g_gi_frames = 0;                            // GI copies recorded
    ID3D12Resource *g_gi_source = nullptr;               // the game texture last copied (stable across frames)
    uint64_t g_gi_source_frames = 0;                     // consecutive copies from that same texture
    uint64_t g_hit_built = 0;                            // guide dispatches that wrote the hit distance
    uint32_t g_width = 0, g_height = 0;
    bool g_failed = false;
    uint64_t g_copied_frames = 0, g_built_frames = 0, g_used_copy = 0;
    uint32_t g_applied_flags = 0, g_applied_metal = 0;
    int g_applied_threshold = 0;
    uint64_t g_last_dispatch_tick = 0;
    // Metalness source and the other guide options live in rr::cfg (settings.cpp).

    struct candidate
    {
        ID3D12GraphicsCommandList *list = nullptr;
        D3D12_TEXTURE_BARRIER barrier {};
        int age = 1000;
    };
    thread_local candidate t_packed;

    // RTGI upscale (CRC 0x14FA42AB): after its pipeline is set, its two RGBA16F inputs get UAV->SRV barriers,
    // then its output (the final GI: rgb + normalised hit distance in w) does. The third one is copied.
    struct upscale_track
    {
        ID3D12GraphicsCommandList *list = nullptr;
        int seen = 0;
        int age = 0;
    };
    thread_local upscale_track t_upscale;
    std::atomic<int> g_upscale_logged { 0 };

    // The first frames' candidates go to the log, to check the choice against a frame capture (F10).
    void log_upscale_barrier(int index, ID3D12Resource *res)
    {
        if (g_upscale_logged.fetch_add(1) < 9)
            rr::log_info("guides: RGBA16F UAV->SRV #" + std::to_string(index) + " after the RTGI upscale: " + rr::hex(uint64_t(res)) + (index == 3 ? " (copied)" : ""));
    }
    constexpr int kSets = 7;   // descriptors per set: 3 SRV (albedo, packed, GI) + 4 UAV (normal, diffuse, specular, hit)
    std::atomic<uint32_t> g_render_w { 0 }, g_render_h { 0 };   // DLSS input color extent (Streamline tag)

    bool covers_subresource_zero(const D3D12_BARRIER_SUBRESOURCE_RANGE &r)
    {
        return r.IndexOrFirstMipLevel == 0xffffffffu ||
               (r.NumMipLevels == 0 ? r.IndexOrFirstMipLevel == 0 : (r.IndexOrFirstMipLevel == 0 && r.FirstArraySlice == 0 && r.FirstPlane == 0));
    }

    // Inputs (copies): simultaneous access, COMMON at the start and end of every command list.
    // Outputs (uav = true): no simultaneous access (so no decay), persistent explicit state
    // NON_PIXEL_SHADER_RESOURCE, which is what NGX reads: Streamline is tagged with that state and
    // never has to transition them in the middle of its evaluation.
    ID3D12Resource *create_texture(DXGI_FORMAT format, bool uav)
    {
        D3D12_HEAP_PROPERTIES heap {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = g_width;
        desc.Height = g_height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
        const D3D12_RESOURCE_STATES initial = uav ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_COMMON;
        ID3D12Resource *res = nullptr;
        const HRESULT hr = g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, initial, nullptr, IID_PPV_ARGS(&res));
        if (FAILED(hr))
        {
            rr::log_warn("guides: CreateCommittedResource failed " + rr::hex(uint32_t(hr)));
            return nullptr;
        }
        return res;
    }

    // Called with g_mutex held.
    bool ensure_resources(ID3D12GraphicsCommandList *list, uint32_t width, uint32_t height)
    {
        if (g_failed)
            return false;
        if (g_device == nullptr)
        {
            if (FAILED(list->GetDevice(IID_PPV_ARGS(&g_device))))
                return g_failed = true, false;
            if (FAILED(g_device->CreateRootSignature(0, g_guides_cs, sizeof(g_guides_cs), IID_PPV_ARGS(&g_root_signature))))
            {
                rr::log_warn("guides: CreateRootSignature failed");
                return g_failed = true, false;
            }
            D3D12_COMPUTE_PIPELINE_STATE_DESC pso {};
            pso.pRootSignature = g_root_signature;
            pso.CS = { g_guides_cs, sizeof(g_guides_cs) };
            if (FAILED(g_device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&g_pipeline))))
            {
                rr::log_warn("guides: CreateComputePipelineState failed");
                return g_failed = true, false;
            }
            D3D12_DESCRIPTOR_HEAP_DESC hd {};
            hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            hd.NumDescriptors = 2 * kSets;   // two sets, so a resize never rewrites descriptors in flight
            hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            if (FAILED(g_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_heap))))
            {
                rr::log_warn("guides: CreateDescriptorHeap failed");
                return g_failed = true, false;
            }
            g_increment = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }
        if (g_width == width && g_height == height && g_albedo != nullptr)
            return true;
        if (g_albedo != nullptr)
        {
            // The GPU may still use the old textures and descriptors for a few frames.
            for (ID3D12Resource *r : { g_albedo, g_packed, g_normal_roughness, g_diffuse, g_specular, g_gi, g_hit })
                g_retired.push_back({ r, g_copied_frames });
            g_gi_frames = 0;
            g_gi_source = nullptr;
            g_gi_source_frames = 0;
            g_hit_built = 0;
            g_heap_half ^= 1;
            rr::log_info("guides: render size changed to " + std::to_string(width) + "x" + std::to_string(height) + "; old textures retired");
        }

        g_width = width;
        g_height = height;
        g_albedo = create_texture(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, false);
        g_packed = create_texture(DXGI_FORMAT_R32_UINT, false);
        g_normal_roughness = create_texture(DXGI_FORMAT_R16G16B16A16_FLOAT, true);
        g_diffuse = create_texture(DXGI_FORMAT_R8G8B8A8_UNORM, true);
        g_specular = create_texture(DXGI_FORMAT_R8G8B8A8_UNORM, true);
        g_gi = create_texture(DXGI_FORMAT_R16G16B16A16_FLOAT, false);
        g_hit = create_texture(DXGI_FORMAT_R16_FLOAT, true);
        if (!g_albedo || !g_packed || !g_normal_roughness || !g_diffuse || !g_specular || !g_gi || !g_hit)
            return g_failed = true, false;

        D3D12_CPU_DESCRIPTOR_HANDLE h = g_heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += SIZE_T(g_heap_half) * kSets * g_increment;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv {};
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        g_device->CreateShaderResourceView(g_albedo, &srv, h);
        h.ptr += g_increment;
        srv.Format = DXGI_FORMAT_R32_UINT;
        g_device->CreateShaderResourceView(g_packed, &srv, h);
        h.ptr += g_increment;
        srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        g_device->CreateShaderResourceView(g_gi, &srv, h);
        h.ptr += g_increment;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav {};
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        g_device->CreateUnorderedAccessView(g_normal_roughness, nullptr, &uav, h);
        h.ptr += g_increment;
        uav.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        g_device->CreateUnorderedAccessView(g_diffuse, nullptr, &uav, h);
        h.ptr += g_increment;
        g_device->CreateUnorderedAccessView(g_specular, nullptr, &uav, h);
        h.ptr += g_increment;
        uav.Format = DXGI_FORMAT_R16_FLOAT;
        g_device->CreateUnorderedAccessView(g_hit, nullptr, &uav, h);
        rr::log_info("guides: resources created for " + std::to_string(width) + "x" + std::to_string(height));
        return true;
    }

    // Explicit legacy transitions on add-on textures only (never on game resources). Every command
    // list starts and ends with them in COMMON, whatever ExecuteCommandLists batching the game uses.
    void transition(ID3D12GraphicsCommandList *list, std::initializer_list<ID3D12Resource *> resources, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
        D3D12_RESOURCE_BARRIER b[8] {};
        UINT n = 0;
        for (ID3D12Resource *r : resources)
        {
            b[n].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b[n].Transition.pResource = r;
            b[n].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b[n].Transition.StateBefore = before;
            b[n].Transition.StateAfter = after;
            n++;
        }
        rr::raw_resource_barrier(list, n, b);
    }

    // Round trip of one game texture from the layout its barrier just set to COPY_SOURCE and back.
    void make_round_trip(const D3D12_TEXTURE_BARRIER &tb, D3D12_TEXTURE_BARRIER &in, D3D12_TEXTURE_BARRIER &out)
    {
        const bool no_access = tb.AccessAfter == D3D12_BARRIER_ACCESS_NO_ACCESS;
        in = {};
        out = {};
        in.pResource = out.pResource = tb.pResource;
        in.Subresources = out.Subresources = { 0, 1, 0, 1, 0, 1 };
        in.LayoutBefore = out.LayoutAfter = tb.LayoutAfter;
        in.LayoutAfter = out.LayoutBefore = D3D12_BARRIER_LAYOUT_COPY_SOURCE;
        in.SyncBefore = no_access ? D3D12_BARRIER_SYNC_NONE : tb.SyncAfter;
        in.AccessBefore = no_access ? D3D12_BARRIER_ACCESS_NO_ACCESS : tb.AccessAfter;
        in.SyncAfter = out.SyncBefore = D3D12_BARRIER_SYNC_COPY;
        in.AccessAfter = out.AccessBefore = D3D12_BARRIER_ACCESS_COPY_SOURCE;
        out.SyncAfter = no_access ? D3D12_BARRIER_SYNC_NONE : tb.SyncAfter;
        out.AccessAfter = tb.AccessAfter;
    }

    void copy_gbuffer(ID3D12GraphicsCommandList7 *list7, const D3D12_TEXTURE_BARRIER &packed_tb, const D3D12_TEXTURE_BARRIER &albedo_tb, uint32_t width, uint32_t height)
    {
        auto *list = static_cast<ID3D12GraphicsCommandList *>(list7);
        std::lock_guard lock(g_mutex);
        for (size_t i = 0; i < g_retired.size();)
        {
            if (g_retired[i].frame + 120 < g_copied_frames)
            {
                g_retired[i].res->Release();
                g_retired[i] = g_retired.back();
                g_retired.pop_back();
            }
            else
                ++i;
        }
        if (!ensure_resources(list, width, height))
            return;
        D3D12_TEXTURE_BARRIER in[2], out[2];
        make_round_trip(packed_tb, in[0], out[0]);
        make_round_trip(albedo_tb, in[1], out[1]);
        D3D12_BARRIER_GROUP gin {}, gout {};
        gin.Type = gout.Type = D3D12_BARRIER_TYPE_TEXTURE;
        gin.NumBarriers = gout.NumBarriers = 2;
        gin.pTextureBarriers = in;
        gout.pTextureBarriers = out;
        rr::raw_barrier(list7, 1, &gin);
        transition(list, { g_packed, g_albedo }, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);

        D3D12_TEXTURE_COPY_LOCATION dst {}, src {};
        dst.Type = src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource = g_packed;
        src.pResource = packed_tb.pResource;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        dst.pResource = g_albedo;
        src.pResource = albedo_tb.pResource;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

        transition(list, { g_packed, g_albedo }, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        rr::raw_barrier(list7, 1, &gout);
        if (g_copied_frames++ == 0)
            rr::log_info("guides: first G-buffer copy recorded");
    }

    // The game's final GI right after the RTGI upscale wrote it (same layout round trip as the G-buffer).
    void copy_gi(ID3D12GraphicsCommandList7 *list7, const D3D12_TEXTURE_BARRIER &tb)
    {
        auto *list = static_cast<ID3D12GraphicsCommandList *>(list7);
        std::lock_guard lock(g_mutex);
        if (g_gi == nullptr)
            return;
        D3D12_TEXTURE_BARRIER in, out;
        make_round_trip(tb, in, out);
        D3D12_BARRIER_GROUP gin {}, gout {};
        gin.Type = gout.Type = D3D12_BARRIER_TYPE_TEXTURE;
        gin.NumBarriers = gout.NumBarriers = 1;
        gin.pTextureBarriers = &in;
        gout.pTextureBarriers = &out;
        rr::raw_barrier(list7, 1, &gin);
        transition(list, { g_gi }, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION dst {}, src {};
        dst.Type = src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource = g_gi;
        src.pResource = tb.pResource;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        transition(list, { g_gi }, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        rr::raw_barrier(list7, 1, &gout);
        g_gi_source_frames = tb.pResource == g_gi_source ? g_gi_source_frames + 1 : 1;
        g_gi_source = tb.pResource;
        if (g_gi_frames++ == 0)
            rr::log_info("guides: first final-GI copy recorded (" + rr::hex(uint64_t(tb.pResource)) + ")");
    }
}

namespace rr
{
    void guides_set_camera(const float right[3], const float up[3], const float fwd[3], float proj_x, float proj_y)
    {
        std::lock_guard lock(g_mutex);
        for (int i = 0; i < 3; ++i)
        {
            g_camera.right[i] = right[i];
            g_camera.up[i] = up[i];
            g_camera.fwd[i] = fwd[i];
        }
        g_camera.proj_x = proj_x;
        g_camera.proj_y = proj_y;
        g_camera.valid = proj_x != 0 && proj_y != 0;
    }

    // Every enhanced barrier the game records passes through here (cheap filter first).
    void guides_on_barrier(ID3D12GraphicsCommandList7 *list7, UINT32 count_groups, const D3D12_BARRIER_GROUP *groups)
    {
        auto *list = static_cast<ID3D12GraphicsCommandList *>(list7);
        for (UINT32 g = 0; g < count_groups; ++g)
        {
            if (groups[g].Type != D3D12_BARRIER_TYPE_TEXTURE)
                continue;
            for (UINT32 i = 0; i < groups[g].NumBarriers; ++i)
            {
                const D3D12_TEXTURE_BARRIER &tb = groups[g].pTextureBarriers[i];
                if (t_packed.list == list)
                    t_packed.age++;
                if (t_upscale.list == list && tb.pResource != nullptr && tb.LayoutBefore == D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS &&
                    tb.LayoutAfter == D3D12_BARRIER_LAYOUT_SHADER_RESOURCE && covers_subresource_zero(tb.Subresources))
                {
                    const D3D12_RESOURCE_DESC desc = tb.pResource->GetDesc();
                    const uint32_t ew = g_render_w.load(), eh = g_render_h.load();
                    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT && desc.DepthOrArraySize == 1 &&
                        desc.MipLevels == 1 && desc.SampleDesc.Count == 1 && desc.Width == ew && desc.Height == eh &&
                        (log_upscale_barrier(++t_upscale.seen, tb.pResource), t_upscale.seen == 3))
                    {
                        if (cfg::rr_hit_distance.load() && list->GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT)
                            copy_gi(list7, tb);
                        t_upscale.list = nullptr;
                    }
                }
                if (t_upscale.list == list && ++t_upscale.age > 64)
                    t_upscale.list = nullptr;   // the pattern did not show up: give up for this frame
                if (tb.pResource == nullptr || tb.LayoutBefore != D3D12_BARRIER_LAYOUT_RENDER_TARGET ||
                    tb.LayoutAfter != D3D12_BARRIER_LAYOUT_SHADER_RESOURCE || !covers_subresource_zero(tb.Subresources))
                    continue;
                const D3D12_RESOURCE_DESC desc = tb.pResource->GetDesc();
                if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.MipLevels != 1 || desc.DepthOrArraySize != 1 || desc.SampleDesc.Count != 1)
                    continue;
                if (desc.Format == DXGI_FORMAT_R32_UINT)
                {
                    t_packed.list = list;
                    t_packed.barrier = tb;
                    t_packed.age = 0;
                }
                else if (desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB && t_packed.list == list && t_packed.age <= 6)
                {
                    const D3D12_RESOURCE_DESC pd = t_packed.barrier.pResource->GetDesc();
                    const uint32_t ew = g_render_w.load(), eh = g_render_h.load();
                    const bool matches_dlss = ew == 0 || (desc.Width == ew && desc.Height == eh);
                    if (pd.Width == desc.Width && pd.Height == desc.Height && matches_dlss && list->GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT)
                        copy_gbuffer(list7, t_packed.barrier, tb, uint32_t(desc.Width), desc.Height);
                    t_packed.list = nullptr;
                }
            }
        }
    }

    // From replace.cpp's SetPipelineState hook when the game sets the RTGI upscale pipeline.
    void guides_on_upscale_pso(ID3D12GraphicsCommandList *list)
    {
        t_upscale.list = list;
        t_upscale.seen = 0;
        t_upscale.age = 0;
    }

    void guides_metal_toggle()
    {
        const bool on = !cfg::metal_alpha.load();
        cfg::metal_alpha = on;
        cfg::save();
        rr::log_info(std::string("F7: guide metalness ") + (on ? "from albedo alpha (experimental)" : "off (dielectric everywhere)"));
    }

    // Records the guide shader on the command list the game evaluates DLSS on. `key_list` is the
    // native list used to attribute captures (the game may pass a proxy pointer to Streamline).
    void guides_before_evaluate(void *command_buffer, void *key_list)
    {
        std::lock_guard lock(g_mutex);
        if (g_pipeline == nullptr || g_albedo == nullptr || !g_camera.valid || g_used_copy == g_copied_frames)
            return;
        g_used_copy = g_copied_frames;
        auto *list = static_cast<ID3D12GraphicsCommandList *>(command_buffer);

        uint32_t constants[16] = {};
        auto put = [&](int at, float v) { memcpy(&constants[at], &v, 4); };
        for (int i = 0; i < 3; ++i)
        {
            put(i, g_camera.right[i]);
            put(4 + i, g_camera.up[i]);
            put(8 + i, g_camera.fwd[i]);
        }
        put(3, g_camera.proj_x);
        put(7, g_camera.proj_y);
        constants[12] = g_width;
        constants[13] = g_height;
        const bool hit = cfg::rr_hit_distance.load() && g_gi_frames > 0;
        constants[14] = (cfg::metal_alpha.load() ? 1u : 0u) | (uint32_t(std::clamp(cfg::rr_hit_scale.load(), 1, 200)) * 100u << 8);
        put(11, cfg::foliage_threshold.load() * 0.01f);
        constants[15] = (cfg::sky_mode.load() == 1 ? 1u : 0u) | (cfg::sky_mode.load() == 2 ? 64u : 0u) | (cfg::foliage_neutral.load() ? 2u : 0u) | (cfg::foliage_specular.load() ? 4u : 0u) |
                       (cfg::foliage_dilate.load() ? 8u : 0u) | (cfg::diag_neutral.load() ? 16u : 0u) |
                       (cfg::foliage_floor_mode.load() ? 32u : 0u) | (hit ? 128u : 0u) | (uint32_t(cfg::foliage_floor.load()) << 16);

        transition(list, { g_albedo, g_packed, g_gi }, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        transition(list, { g_normal_roughness, g_diffuse, g_specular, g_hit }, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->SetDescriptorHeaps(1, &g_heap);
        list->SetComputeRootSignature(g_root_signature);
        list->SetPipelineState(g_pipeline);
        list->SetComputeRoot32BitConstants(0, 16, constants, 0);
        D3D12_GPU_DESCRIPTOR_HANDLE table = g_heap->GetGPUDescriptorHandleForHeapStart();
        table.ptr += UINT64(g_heap_half) * kSets * g_increment;
        list->SetComputeRootDescriptorTable(1, table);
        list->Dispatch((g_width + 7) / 8, (g_height + 7) / 8, 1);
        // Specular motion vectors read the same G-buffer copy (gloss), still in NON_PIXEL_SHADER_RESOURCE here.
        specmv_dispatch(list, g_packed, g_width, g_height);
        g_applied_flags = constants[15];
        if (hit)
            g_hit_built++;
        g_applied_metal = constants[14] & 255u;
        g_applied_threshold = cfg::foliage_threshold.load();
        g_last_dispatch_tick = GetTickCount64();
        if (g_built_frames++ == 0)
            rr::log_info("guides: first guide dispatch recorded");

        if (capture_active())
        {
            capture_own_texture(list, key_list, g_normal_roughness, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, "GUIDE normal+roughness (world)");
            capture_own_texture(list, key_list, g_diffuse, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, "GUIDE diffuse albedo");
            capture_own_texture(list, key_list, g_specular, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, "GUIDE specular albedo");
            capture_own_texture(list, key_list, g_albedo, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "GUIDE input copy: albedo");
            capture_own_texture(list, key_list, g_packed, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "GUIDE input copy: packed normal+gloss");
        }
        if (capture_active() && hit)
            capture_own_texture(list, key_list, g_hit, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, "GUIDE diffuse hit distance");
        transition(list, { g_albedo, g_packed, g_gi }, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        transition(list, { g_normal_roughness, g_diffuse, g_specular, g_hit }, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    // Guide outputs for Ray Reconstruction; false until the first copy and dispatch exist.
    void set_render_extent(uint32_t width, uint32_t height)
    {
        g_render_w = width;
        g_render_h = height;
    }

    // Native device (taken from the native command list NGX records on). A device obtained any other
    // way can be ReShade's proxy, whose descriptor heaps crash a native SetDescriptorHeaps.
    ID3D12Device *guides_native_device()
    {
        std::lock_guard lock(g_mutex);
        return g_device;
    }

    guides_snapshot guides_get_snapshot()
    {
        guides_snapshot s;
        std::lock_guard lock(g_mutex);
        s.width = g_width;
        s.height = g_height;
        s.copies = g_copied_frames;
        s.dispatches = g_built_frames;
        s.pipeline_ready = g_pipeline != nullptr;
        s.camera_valid = g_camera.valid;
        s.applied_flags = g_applied_flags;
        s.applied_metal = g_applied_metal;
        s.applied_threshold = g_applied_threshold;
        s.last_dispatch_tick = g_last_dispatch_tick;
        s.gi_copies = g_gi_frames;
        s.gi_source = uint64_t(g_gi_source);
        s.gi_source_frames = g_gi_source_frames;
        s.hit_dispatches = g_hit_built;
        return s;
    }

    bool guides_outputs(ID3D12Resource **normal_roughness, ID3D12Resource **diffuse, ID3D12Resource **specular, uint32_t &width, uint32_t &height, uint64_t &dispatches, uint64_t &copies)
    {
        std::lock_guard lock(g_mutex);
        if (g_normal_roughness == nullptr || g_built_frames == 0)
            return false;
        *normal_roughness = g_normal_roughness;
        *diffuse = g_diffuse;
        *specular = g_specular;
        width = g_width;
        height = g_height;
        dispatches = g_built_frames;
        copies = g_copied_frames;
        return true;
    }

    // Diffuse hit distance guide: only once the final-GI copy has run (it keeps the last copy if a frame misses it).
    ID3D12Resource *guides_hit_distance()
    {
        std::lock_guard lock(g_mutex);
        return g_hit != nullptr && g_hit_built > 0 && g_gi_frames > 0 ? g_hit : nullptr;
    }

    std::string guides_status()
    {
        std::lock_guard lock(g_mutex);
        return "guides: copies=" + std::to_string(g_copied_frames) + " dispatches=" + std::to_string(g_built_frames) +
               (g_failed ? " (FAILED)" : "") + " size=" + std::to_string(g_width) + "x" + std::to_string(g_height);
    }
}
