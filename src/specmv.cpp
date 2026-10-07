// Specular motion vectors for DLSS-RR (shaders/specmv.hlsl, derived from speedlemur's Control RR).
//
// Inputs: the add-on's copy of the RT G-buffer (gloss), and the game's depth and motion vectors as tagged
// for DLSS (read through SRVs in the state the game declares to Streamline, never transitioned), plus the
// camera matrices from slSetConstants. Output: an RG16F texture at the motion vector resolution, kept in
// NON_PIXEL_SHADER_RESOURCE between frames like the guides, tagged as kBufferTypeSpecularMotionVectors.
// Dispatched from guides_before_evaluate, on the native command list, inside the DLSS evaluation.

#include "common.hpp"
#include "specmv_cs.h"

#include <algorithm>
#include <cstring>
#include <mutex>

namespace
{
    std::mutex g_mutex;
    ID3D12Device *g_device = nullptr;
    ID3D12RootSignature *g_root = nullptr;
    ID3D12PipelineState *g_pso = nullptr;
    ID3D12DescriptorHeap *g_heap = nullptr;
    UINT g_increment = 0;
    constexpr int kSets = 16;   // a descriptor set is rewritten only 16 frames later
    int g_next_set = 0;
    bool g_failed = false;

    ID3D12Resource *g_out = nullptr;
    uint32_t g_out_w = 0, g_out_h = 0;
    std::vector<ID3D12Resource *> g_retired;

    // Latest DLSS tags and constants (render threads).
    ID3D12Resource *g_depth = nullptr, *g_mv = nullptr;
    uint32_t g_depth_state = 0, g_mv_state = 0;
    uint32_t g_mv_w = 0, g_mv_h = 0;
    float g_clip_to_view[16] {}, g_view_to_clip[16] {}, g_clip_to_prev[16] {};
    float g_mv_scale[2] { 1.0f, 1.0f };
    bool g_have_constants = false;

    uint64_t g_dispatches = 0, g_last_tick = 0;
    int g_last_distance = -1;
    std::string g_error, g_skip;

    constexpr uint32_t kReadable = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    // Called with g_mutex held.
    bool ensure(ID3D12GraphicsCommandList *list, uint32_t w, uint32_t h)
    {
        if (g_failed)
            return false;
        if (g_device == nullptr)
        {
            HRESULT hr = list->GetDevice(IID_PPV_ARGS(&g_device));
            if (SUCCEEDED(hr))
                hr = g_device->CreateRootSignature(0, g_specmv_cs, sizeof(g_specmv_cs), IID_PPV_ARGS(&g_root));
            if (SUCCEEDED(hr))
            {
                D3D12_COMPUTE_PIPELINE_STATE_DESC pso {};
                pso.pRootSignature = g_root;
                pso.CS = { g_specmv_cs, sizeof(g_specmv_cs) };
                hr = g_device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&g_pso));
            }
            if (SUCCEEDED(hr))
            {
                D3D12_DESCRIPTOR_HEAP_DESC hd {};
                hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
                hd.NumDescriptors = 4 * kSets;
                hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
                hr = g_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_heap));
            }
            if (FAILED(hr))
            {
                g_failed = true;
                g_error = rr::tr("could not create the specular motion vector shader (", "no se pudo crear el shader de vectores especulares (") + rr::hex(uint32_t(hr)) + ")";
                rr::log_warn("specmv: " + g_error);
                return false;
            }
            g_increment = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }
        if (g_out == nullptr || g_out_w != w || g_out_h != h)
        {
            if (g_out)
                g_retired.push_back(g_out);   // may still be read by frames in flight (a few KB each, rare)
            g_out = nullptr;
            D3D12_HEAP_PROPERTIES heap {};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC d {};
            d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            d.Width = w;
            d.Height = h;
            d.DepthOrArraySize = 1;
            d.MipLevels = 1;
            d.Format = DXGI_FORMAT_R16G16_FLOAT;   // same format as the game's motion vectors
            d.SampleDesc.Count = 1;
            d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            const HRESULT hr = g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                                                                 IID_PPV_ARGS(&g_out));
            if (FAILED(hr))
            {
                g_error = rr::tr("not enough VRAM for the specular motion vectors (", "no hay VRAM para los vectores especulares (") + rr::hex(uint32_t(hr)) + ")";
                return false;
            }
            g_out_w = w;
            g_out_h = h;
            g_error.clear();
            rr::log_info("specmv: output created " + std::to_string(w) + "x" + std::to_string(h));
        }
        return true;
    }

    DXGI_FORMAT depth_srv_format(DXGI_FORMAT f)
    {
        switch (f)
        {
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_R32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        default: return DXGI_FORMAT_UNKNOWN;
        }
    }

    void skip(const std::string &why)
    {
        if (why != g_skip)
        {
            g_skip = why;
            rr::log_info("specmv: skipped (" + why + ")");
        }
    }
}

namespace rr
{
    void specmv_set_tags(ID3D12Resource *depth, uint32_t depth_state, ID3D12Resource *mv, uint32_t mv_state, uint32_t mv_w, uint32_t mv_h)
    {
        std::lock_guard lock(g_mutex);
        if (depth != nullptr)
        {
            g_depth = depth;
            g_depth_state = depth_state;
        }
        if (mv != nullptr)
        {
            g_mv = mv;
            g_mv_state = mv_state;
            g_mv_w = mv_w;
            g_mv_h = mv_h;
        }
    }

    void specmv_set_constants(const float clip_to_view[16], const float view_to_clip[16], const float clip_to_prev[16], float mv_scale_x, float mv_scale_y)
    {
        std::lock_guard lock(g_mutex);
        memcpy(g_clip_to_view, clip_to_view, 64);
        memcpy(g_view_to_clip, view_to_clip, 64);
        memcpy(g_clip_to_prev, clip_to_prev, 64);
        g_mv_scale[0] = mv_scale_x;
        g_mv_scale[1] = mv_scale_y;
        g_have_constants = true;
    }

    // `packed` is the add-on's copy of the RT G-buffer, already in NON_PIXEL_SHADER_RESOURCE on `list`.
    void specmv_dispatch(ID3D12GraphicsCommandList *list, ID3D12Resource *packed, uint32_t packed_w, uint32_t packed_h)
    {
        if (!cfg::rr_spec_mv.load(std::memory_order_relaxed))
            return;
        std::lock_guard lock(g_mutex);
        if (!g_have_constants || g_depth == nullptr || g_mv == nullptr)
            return skip("no DLSS depth, motion vectors or constants yet");
        if ((g_depth_state & kReadable) == 0 || (g_mv_state & kReadable) == 0)
            return skip("depth or motion vectors not in a shader-readable state (" + hex(g_depth_state) + ", " + hex(g_mv_state) + ")");
        const D3D12_RESOURCE_DESC dd = g_depth->GetDesc(), md = g_mv->GetDesc();
        const uint32_t w = g_mv_w != 0 ? g_mv_w : uint32_t(md.Width), h = g_mv_h != 0 ? g_mv_h : md.Height;
        const DXGI_FORMAT depth_format = depth_srv_format(dd.Format);
        if (depth_format == DXGI_FORMAT_UNKNOWN || md.Format != DXGI_FORMAT_R16G16_FLOAT)
            return skip("unexpected depth or motion vector format (" + format_name(dd.Format) + ", " + format_name(md.Format) + ")");
        if (w != packed_w || h != packed_h || uint64_t(w) > dd.Width || h > dd.Height)
            return skip("motion vector, depth and G-buffer sizes differ");
        if (!ensure(list, w, h))
            return;

        const int set = g_next_set;
        g_next_set = (g_next_set + 1) % kSets;
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_heap->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += SIZE_T(set) * 4 * g_increment;
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = g_heap->GetGPUDescriptorHandleForHeapStart();
        gpu.ptr += UINT64(set) * 4 * g_increment;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv {};
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        srv.Format = DXGI_FORMAT_R32_UINT;
        g_device->CreateShaderResourceView(packed, &srv, cpu);
        cpu.ptr += g_increment;
        srv.Format = depth_format;
        g_device->CreateShaderResourceView(g_depth, &srv, cpu);
        cpu.ptr += g_increment;
        srv.Format = DXGI_FORMAT_R16G16_FLOAT;
        g_device->CreateShaderResourceView(g_mv, &srv, cpu);
        cpu.ptr += g_increment;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav {};
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uav.Format = DXGI_FORMAT_R16G16_FLOAT;
        g_device->CreateUnorderedAccessView(g_out, nullptr, &uav, cpu);

        uint32_t constants[54] = {};
        memcpy(&constants[0], g_clip_to_view, 64);
        memcpy(&constants[16], g_view_to_clip, 64);
        memcpy(&constants[32], g_clip_to_prev, 64);
        constants[48] = w;
        constants[49] = h;
        const int distance = std::clamp(cfg::rr_spec_mv_distance.load(), 1, 1000);
        const float distance_f = float(distance);
        memcpy(&constants[50], &distance_f, 4);
        memcpy(&constants[52], &g_mv_scale[0], 4);
        memcpy(&constants[53], &g_mv_scale[1], 4);

        D3D12_RESOURCE_BARRIER b {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = g_out;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        raw_resource_barrier(list, 1, &b);
        list->SetDescriptorHeaps(1, &g_heap);
        list->SetComputeRootSignature(g_root);
        list->SetPipelineState(g_pso);
        list->SetComputeRoot32BitConstants(0, 54, constants, 0);
        list->SetComputeRootDescriptorTable(1, gpu);
        list->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        raw_resource_barrier(list, 1, &b);

        if (g_dispatches++ == 0)
            log_info("specmv: first dispatch (" + std::to_string(w) + "x" + std::to_string(h) + ", mvecScale " + std::to_string(g_mv_scale[0]) + "," +
                     std::to_string(g_mv_scale[1]) + ", " + std::to_string(distance) + " m)");
        g_last_tick = GetTickCount64();
        g_last_distance = distance;
        g_skip.clear();
    }

    // The texture to tag, or nullptr when it was never written (the tag is then left out).
    ID3D12Resource *specmv_output(uint32_t &w, uint32_t &h)
    {
        std::lock_guard lock(g_mutex);
        if (g_out == nullptr || g_dispatches == 0)
            return nullptr;
        w = g_out_w;
        h = g_out_h;
        return g_out;
    }

    specmv_snapshot specmv_get_snapshot()
    {
        specmv_snapshot s;
        std::lock_guard lock(g_mutex);
        s.dispatches = g_dispatches;
        s.last_tick = g_last_tick;
        s.last_distance = g_last_distance;
        s.error = g_error;
        s.skip = g_skip;
        return s;
    }
}
