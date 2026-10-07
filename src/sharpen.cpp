// Optional adaptive sharpening of the DLSS Ray Reconstruction output (General tab, "RR sharpening").
// Runs right after slEvaluateFeature(kFeatureDLSS_RR) returned, on the native command list NGX used:
// pass 0 copies the output into the add-on's own R11G11B10F texture, pass 1 sharpens from that copy back
// into the output (shaders/rr_sharpen.hlsl). The output is only accessed through a UAV in the layout
// the game keeps it in during DLSS (UNORDERED_ACCESS); global barriers order the passes. Never runs for
// DLSS-SR frames.

#include "common.hpp"
#include "rr_sharpen_cs.h"

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
    ID3D12Resource *g_copy = nullptr;
    uint32_t g_w = 0, g_h = 0;
    std::vector<ID3D12Resource *> g_retired;
    bool g_failed = false;
    std::string g_error;
    uint64_t g_last_tick = 0;
    int g_last_strength = -1;

    void global_barrier(ID3D12GraphicsCommandList *list, D3D12_BARRIER_SYNC before, D3D12_BARRIER_SYNC after)
    {
        ID3D12GraphicsCommandList7 *list7 = nullptr;
        if (FAILED(list->QueryInterface(IID_PPV_ARGS(&list7))))
            return;
        D3D12_GLOBAL_BARRIER gb {};
        gb.SyncBefore = before;
        gb.SyncAfter = after;
        gb.AccessBefore = D3D12_BARRIER_ACCESS_UNORDERED_ACCESS | D3D12_BARRIER_ACCESS_COPY_DEST;
        gb.AccessAfter = D3D12_BARRIER_ACCESS_UNORDERED_ACCESS | D3D12_BARRIER_ACCESS_SHADER_RESOURCE;
        D3D12_BARRIER_GROUP g {};
        g.Type = D3D12_BARRIER_TYPE_GLOBAL;
        g.NumBarriers = 1;
        g.pGlobalBarriers = &gb;
        rr::raw_barrier(list7, 1, &g);
        list7->Release();
    }

    // Called with g_mutex held.
    bool ensure(ID3D12GraphicsCommandList *list, uint32_t w, uint32_t h)
    {
        if (g_failed)
            return false;
        if (g_device == nullptr)
        {
            HRESULT hr = list->GetDevice(IID_PPV_ARGS(&g_device));
            if (SUCCEEDED(hr))
                hr = g_device->CreateRootSignature(0, g_rr_sharpen_cs, sizeof(g_rr_sharpen_cs), IID_PPV_ARGS(&g_root));
            if (SUCCEEDED(hr))
            {
                D3D12_COMPUTE_PIPELINE_STATE_DESC pso {};
                pso.pRootSignature = g_root;
                pso.CS = { g_rr_sharpen_cs, sizeof(g_rr_sharpen_cs) };
                hr = g_device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&g_pso));
            }
            if (SUCCEEDED(hr))
            {
                D3D12_DESCRIPTOR_HEAP_DESC hd {};
                hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
                hd.NumDescriptors = 2 * kSets;
                hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
                hr = g_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_heap));
            }
            if (FAILED(hr))
            {
                g_failed = true;
                g_error = rr::tr("could not create the sharpening shader (", "no se pudo crear el shader de afinado (") + rr::hex(uint32_t(hr)) + ")";
                rr::log_warn("sharpen: " + g_error);
                return false;
            }
            g_increment = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }
        if (g_copy == nullptr || g_w != w || g_h != h)
        {
            if (g_copy)
                g_retired.push_back(g_copy);   // may still be in use by frames in flight
            g_copy = nullptr;
            D3D12_HEAP_PROPERTIES heap {};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC d {};
            d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            d.Width = w;
            d.Height = h;
            d.DepthOrArraySize = 1;
            d.MipLevels = 1;
            d.Format = DXGI_FORMAT_R11G11B10_FLOAT;   // same format as the output: half the VRAM of RGBA16F
            d.SampleDesc.Count = 1;
            d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            const HRESULT hr = g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&g_copy));
            if (FAILED(hr))
            {
                g_error = rr::tr("not enough VRAM for the sharpening texture (", "no hay VRAM para la textura de afinado (") + rr::hex(uint32_t(hr)) + ")";
                return false;
            }
            g_w = w;
            g_h = h;
            g_error.clear();
        }
        return true;
    }
}

namespace rr
{
    void sharpen_after_rr()
    {
        if (!cfg::rr_sharpen.load(std::memory_order_relaxed))
            return;
        ID3D12GraphicsCommandList *list = last_ngx_list();
        ID3D12Resource *output = dlss_output_resource();
        if (list == nullptr || output == nullptr)
            return;
        const D3D12_RESOURCE_DESC od = output->GetDesc();
        if (od.Format != DXGI_FORMAT_R11G11B10_FLOAT || (od.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0)
            return;
        const uint32_t w = uint32_t(od.Width), h = od.Height;

        std::lock_guard lock(g_mutex);
        if (!ensure(list, w, h))
            return;
        const int set = g_next_set;
        g_next_set = (g_next_set + 1) % kSets;
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_heap->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += SIZE_T(set) * 2 * g_increment;
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = g_heap->GetGPUDescriptorHandleForHeapStart();
        gpu.ptr += UINT64(set) * 2 * g_increment;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav {};
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uav.Format = DXGI_FORMAT_R11G11B10_FLOAT;
        g_device->CreateUnorderedAccessView(output, nullptr, &uav, cpu);
        cpu.ptr += g_increment;
        uav.Format = DXGI_FORMAT_R11G11B10_FLOAT;
        g_device->CreateUnorderedAccessView(g_copy, nullptr, &uav, cpu);

        const int strength = std::clamp(cfg::rr_sharpness.load(), 0, 100);
        float sharpness = float(strength) / 100.0f;
        uint32_t constants[4] = { w, h, 0, 0 };
        memcpy(&constants[2], &sharpness, 4);

        global_barrier(list, D3D12_BARRIER_SYNC_ALL, D3D12_BARRIER_SYNC_COMPUTE_SHADING);   // RR's writes done
        list->SetDescriptorHeaps(1, &g_heap);
        list->SetComputeRootSignature(g_root);
        list->SetPipelineState(g_pso);
        list->SetComputeRootDescriptorTable(0, gpu);
        list->SetComputeRoot32BitConstants(1, 4, constants, 0);
        list->Dispatch((w + 7) / 8, (h + 7) / 8, 1);                                       // copy
        global_barrier(list, D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_SYNC_COMPUTE_SHADING);
        constants[3] = 1;
        list->SetComputeRoot32BitConstants(1, 4, constants, 0);
        list->Dispatch((w + 7) / 8, (h + 7) / 8, 1);                                       // sharpen
        global_barrier(list, D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_SYNC_ALL);   // visible to the game

        if (g_last_tick == 0)
            log_info("sharpen: first RR sharpening pass recorded (" + std::to_string(strength) + " %)");
        g_last_tick = GetTickCount64();
        g_last_strength = strength;
    }

    sharpen_snapshot sharpen_get_snapshot()
    {
        sharpen_snapshot s;
        std::lock_guard lock(g_mutex);
        s.last_tick = g_last_tick;
        s.last_strength = g_last_strength;
        s.error = g_error;
        return s;
    }
}
