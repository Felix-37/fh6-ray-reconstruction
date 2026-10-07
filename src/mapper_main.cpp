// rr-forza: ReShade add-on that brings DLSS Ray Reconstruction to Forza Horizon 6. It answers the
// game's own DLSS Super Resolution evaluation with DLSS-RR, builds the guide buffers RR needs from the
// game's RT G-buffer (guides.cpp) and can replace the game's RTGI denoising filters (replace.cpp).
// It started as a frame mapper (capture.cpp, F10), which is still here as a developer tool.

#include "common.hpp"

// imgui.h before reshade.hpp: register_addon then also fetches ReShade's ImGui function table,
// which the overlay (overlay.cpp) uses.
#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>

#include <cstdio>

extern "C" __declspec(dllexport) const char *NAME = "RR Forza";
extern "C" __declspec(dllexport) const char *DESCRIPTION =
    "DLSS Ray Reconstruction for Forza Horizon 6 (" RR_FORZA_VERSION "). Settings in the \"Ray Reconstruction\" tab; F11 toggles RR. " RR_FORZA_REPO;

namespace rr
{
    void register_capture_events();
    void start_slinit_watcher();
    void stop_slinit_watcher();
    void register_overlay_ui();
    void unregister_overlay_ui();

    void log_info(const std::string &message) { reshade::log::message(reshade::log::level::info, message.c_str()); }
    void log_warn(const std::string &message) { reshade::log::message(reshade::log::level::warning, message.c_str()); }

    std::string hex(uint64_t value)
    {
        char buf[24];
        snprintf(buf, sizeof(buf), "0x%llX", static_cast<unsigned long long>(value));
        return buf;
    }

    std::string format_name(DXGI_FORMAT format)
    {
        switch (format)
        {
        case DXGI_FORMAT_R32G32B32A32_TYPELESS: return "RGBA32_TYPELESS";
        case DXGI_FORMAT_R32G32B32A32_FLOAT: return "RGBA32F";
        case DXGI_FORMAT_R32G32B32A32_UINT: return "RGBA32_UINT";
        case DXGI_FORMAT_R32G32B32_FLOAT: return "RGB32F";
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return "RGBA16_TYPELESS";
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return "RGBA16F";
        case DXGI_FORMAT_R16G16B16A16_UNORM: return "RGBA16_UNORM";
        case DXGI_FORMAT_R16G16B16A16_UINT: return "RGBA16_UINT";
        case DXGI_FORMAT_R16G16B16A16_SNORM: return "RGBA16_SNORM";
        case DXGI_FORMAT_R32G32_TYPELESS: return "RG32_TYPELESS";
        case DXGI_FORMAT_R32G32_FLOAT: return "RG32F";
        case DXGI_FORMAT_R32G32_UINT: return "RG32_UINT";
        case DXGI_FORMAT_R32G8X24_TYPELESS: return "R32G8X24_TYPELESS";
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return "D32S8";
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: return "RGB10A2_TYPELESS";
        case DXGI_FORMAT_R10G10B10A2_UNORM: return "RGB10A2_UNORM";
        case DXGI_FORMAT_R10G10B10A2_UINT: return "RGB10A2_UINT";
        case DXGI_FORMAT_R11G11B10_FLOAT: return "R11G11B10F";
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return "RGBA8_TYPELESS";
        case DXGI_FORMAT_R8G8B8A8_UNORM: return "RGBA8_UNORM";
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "RGBA8_SRGB";
        case DXGI_FORMAT_R8G8B8A8_UINT: return "RGBA8_UINT";
        case DXGI_FORMAT_R8G8B8A8_SNORM: return "RGBA8_SNORM";
        case DXGI_FORMAT_R16G16_TYPELESS: return "RG16_TYPELESS";
        case DXGI_FORMAT_R16G16_FLOAT: return "RG16F";
        case DXGI_FORMAT_R16G16_UNORM: return "RG16_UNORM";
        case DXGI_FORMAT_R16G16_UINT: return "RG16_UINT";
        case DXGI_FORMAT_R16G16_SNORM: return "RG16_SNORM";
        case DXGI_FORMAT_R32_TYPELESS: return "R32_TYPELESS";
        case DXGI_FORMAT_D32_FLOAT: return "D32F";
        case DXGI_FORMAT_R32_FLOAT: return "R32F";
        case DXGI_FORMAT_R32_UINT: return "R32_UINT";
        case DXGI_FORMAT_R24G8_TYPELESS: return "R24G8_TYPELESS";
        case DXGI_FORMAT_D24_UNORM_S8_UINT: return "D24S8";
        case DXGI_FORMAT_R8G8_TYPELESS: return "RG8_TYPELESS";
        case DXGI_FORMAT_R8G8_UNORM: return "RG8_UNORM";
        case DXGI_FORMAT_R8G8_UINT: return "RG8_UINT";
        case DXGI_FORMAT_R8G8_SNORM: return "RG8_SNORM";
        case DXGI_FORMAT_R16_TYPELESS: return "R16_TYPELESS";
        case DXGI_FORMAT_R16_FLOAT: return "R16F";
        case DXGI_FORMAT_D16_UNORM: return "D16";
        case DXGI_FORMAT_R16_UNORM: return "R16_UNORM";
        case DXGI_FORMAT_R16_UINT: return "R16_UINT";
        case DXGI_FORMAT_R8_TYPELESS: return "R8_TYPELESS";
        case DXGI_FORMAT_R8_UNORM: return "R8_UNORM";
        case DXGI_FORMAT_R8_UINT: return "R8_UINT";
        case DXGI_FORMAT_R9G9B9E5_SHAREDEXP: return "RGB9E5";
        case DXGI_FORMAT_B8G8R8A8_UNORM: return "BGRA8_UNORM";
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: return "BGRA8_TYPELESS";
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "BGRA8_SRGB";
        case DXGI_FORMAT_B8G8R8X8_UNORM: return "BGRX8_UNORM";
        default: return "fmt" + std::to_string(int(format));
        }
    }

    // SEH guards: resources reached through ReShade view tracking can be stale (descriptor reused
    // after the resource was freed), so every COM call on a pointer we did not get from a barrier
    // is protected. Plain functions: clang does not allow __try next to C++ destructors.
    static bool safe_get_desc(ID3D12Resource *resource, D3D12_RESOURCE_DESC *out)
    {
        __try
        {
            *out = resource->GetDesc();
            return out->Dimension >= D3D12_RESOURCE_DIMENSION_BUFFER && out->Dimension <= D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static bool safe_get_name(ID3D12Object *object, const GUID &guid, UINT *size, void *buffer)
    {
        __try
        {
            return SUCCEEDED(object->GetPrivateData(guid, size, buffer));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    std::string debug_name(ID3D12Object *object)
    {
        // WKPDID_D3DDebugObjectNameW
        static const GUID name_w = { 0x4cca5fd8, 0x921f, 0x42c8, { 0x85, 0x66, 0x70, 0xca, 0xf2, 0xa9, 0xb4, 0xb6 } };
        wchar_t buffer[128] = {};
        UINT size = sizeof(buffer) - sizeof(wchar_t);
        if (object == nullptr || !safe_get_name(object, name_w, &size, buffer) || size == 0)
            return {};
        char out[256] = {};
        WideCharToMultiByte(CP_UTF8, 0, buffer, -1, out, sizeof(out) - 1, nullptr, nullptr);
        return out;
    }

    std::string describe_resource(ID3D12Resource *resource)
    {
        D3D12_RESOURCE_DESC desc {};
        if (resource == nullptr || !safe_get_desc(resource, &desc))
            return hex(uint64_t(resource)) + ":<invalid>";
        std::string s = hex(uint64_t(resource)) + ":" + format_name(desc.Format) + ":" +
                        std::to_string(desc.Width) + "x" + std::to_string(desc.Height);
        if (desc.DepthOrArraySize > 1) s += "x" + std::to_string(desc.DepthOrArraySize);
        if (desc.MipLevels > 1) s += ":mips" + std::to_string(desc.MipLevels);
        const std::string name = debug_name(resource);
        if (!name.empty()) s += ":\"" + name + "\"";
        return s;
    }
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        if (!reshade::register_addon(module))
            return FALSE;
        rr::cfg::load();
        rr::register_capture_events();
        rr::register_shader_events();
        rr::register_overlay_ui();
        rr::start_slinit_watcher();
        rr::log_info("rr-forza " RR_FORZA_VERSION " loaded; settings in the \"Ray Reconstruction\" overlay tab (ReShade.ini [RR_Forza])");
        break;
    case DLL_PROCESS_DETACH:
        if (reserved == nullptr)
            rr::stop_slinit_watcher();
        if (reserved == nullptr)   // FreeLibrary at runtime: the detours live in this module
            rr::shutdown_hooks();
        rr::unregister_overlay_ui();
        reshade::unregister_addon(module);
        break;
    }
    return TRUE;
}
