// Observes the game's Streamline calls (tags, constants, feature evaluation) through hooks on the
// exports of sl.interposer.dll. The game executable is never touched: it is protected and only the
// NVIDIA module is hooked, the same place MFG Unlock already hooks. MinHook chains on top of any
// existing detour.

#include "common.hpp"

#include <MinHook.h>
#include <sl.h>
#include <sl_helpers.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

namespace rr { bool ensure_minhook(); }

namespace
{
    using set_tag_fn = sl::Result (*)(const sl::FrameToken &, const sl::ViewportHandle &, const sl::ResourceTag *, uint32_t, sl::CommandBuffer *);
    using evaluate_fn = sl::Result (*)(sl::Feature, const sl::FrameToken &, const sl::BaseStructure **, uint32_t, sl::CommandBuffer *);
    using set_constants_fn = sl::Result (*)(const sl::Constants &, const sl::FrameToken &, const sl::ViewportHandle &);

    set_tag_fn g_set_tag = nullptr;
    evaluate_fn g_evaluate = nullptr;
    set_constants_fn g_set_constants = nullptr;
    bool g_constants_logged_this_capture = false;
    std::atomic<uint32_t> g_render_w { 0 }, g_render_h { 0 };   // DLSS input color extent, from the game's tags

    std::string fmt(float v)
    {
        char b[32];
        snprintf(b, sizeof(b), "%.6g", v);
        return b;
    }

    std::string matrix(const sl::float4x4 &m)
    {
        std::string s = "[";
        for (uint32_t r = 0; r < 4; ++r)
        {
            s += (r ? " | " : "");
            s += fmt(m[r].x) + " " + fmt(m[r].y) + " " + fmt(m[r].z) + " " + fmt(m[r].w);
        }
        return s + "]";
    }

    const char *boolean(sl::Boolean b)
    {
        return b == sl::Boolean::eTrue ? "true" : (b == sl::Boolean::eFalse ? "false" : "invalid");
    }

    std::string describe_tag(const sl::ResourceTag &tag)
    {
        std::string s = std::string(sl::getBufferTypeAsStr(tag.type)) + "(" + std::to_string(tag.type) + ")=";
        if (tag.resource == nullptr || tag.resource->native == nullptr)
            s += "null";
        else
            s += rr::describe_resource(static_cast<ID3D12Resource *>(tag.resource->native)) + " state=" + rr::hex(tag.resource->state);
        if (tag.extent.width != 0)
            s += " extent=" + std::to_string(tag.extent.left) + "," + std::to_string(tag.extent.top) + " " +
                 std::to_string(tag.extent.width) + "x" + std::to_string(tag.extent.height);
        s += tag.lifecycle == sl::ResourceLifecycle::eValidUntilPresent ? " [until-present]" :
             tag.lifecycle == sl::ResourceLifecycle::eValidUntilEvaluate ? " [until-evaluate]" : " [only-valid-now]";
        return s;
    }

    void emit(sl::CommandBuffer *cmd, std::string line)
    {
        void *list = rr::current_list();
        if (cmd != nullptr && list != nullptr)
            rr::list_event(list, std::move(line));
        else
            rr::frame_event(std::move(line));
    }

    sl::Result set_tag_hook(const sl::FrameToken &frame, const sl::ViewportHandle &viewport, const sl::ResourceTag *tags, uint32_t count, sl::CommandBuffer *cmd)
    {
        rr::hook_guard guard;
        for (uint32_t i = 0; i < count; ++i)
            if ((tags[i].type == sl::kBufferTypeScalingOutputColor || tags[i].type == sl::kBufferTypeDepth) && tags[i].resource != nullptr &&
                tags[i].resource->native != nullptr)
            {
                auto *res = static_cast<ID3D12Resource *>(tags[i].resource->native);
                if (tags[i].type == sl::kBufferTypeScalingOutputColor)
                    rr::bench_set_tags(res, tags[i].resource->state, nullptr, 0);
                else
                    rr::bench_set_tags(nullptr, 0, res, tags[i].resource->state);
            }
        for (uint32_t i = 0; i < count; ++i)
            if ((tags[i].type == sl::kBufferTypeMotionVectors || tags[i].type == sl::kBufferTypeDepth) && tags[i].resource != nullptr &&
                tags[i].resource->native != nullptr)
            {
                auto *res = static_cast<ID3D12Resource *>(tags[i].resource->native);
                if (tags[i].type == sl::kBufferTypeMotionVectors)
                    rr::specmv_set_tags(nullptr, 0, res, tags[i].resource->state, tags[i].extent.width, tags[i].extent.height);
                else
                    rr::specmv_set_tags(res, tags[i].resource->state, nullptr, 0, 0, 0);
            }
        for (uint32_t i = 0; i < count; ++i)
            if (tags[i].type == sl::kBufferTypeScalingInputColor && tags[i].extent.width != 0)
            {
                rr::set_render_extent(tags[i].extent.width, tags[i].extent.height);
                g_render_w = tags[i].extent.width;
                g_render_h = tags[i].extent.height;
            }
        if (rr::capture_active())
        {
            const uint32_t vp = viewport;
            for (uint32_t i = 0; i < count; ++i)
                emit(cmd, "[SL] slSetTagForFrame vp=" + std::to_string(vp) + " " + describe_tag(tags[i]));
        }
        return g_set_tag(frame, viewport, tags, count, cmd);
    }

    template <typename T>
    bool hook_address(void *target, const char *name, void *detour, T &original)
    {
        MH_STATUS s = MH_CreateHook(target, detour, reinterpret_cast<void **>(&original));
        if (s == MH_OK)
            s = MH_EnableHook(target);
        if (s != MH_OK)
        {
            rr::log_warn(std::string("hook on ") + name + " failed: " + MH_StatusToString(s));
            return false;
        }
        return true;
    }

    // ---- Phase 4: DLSS Ray Reconstruction in place of DLSS-SR ----

    using dlss_set_options_fn = sl::Result (*)(const sl::ViewportHandle &, const sl::DLSSOptions &);
    using dlssd_set_options_fn = sl::Result (*)(const sl::ViewportHandle &, const sl::DLSSDOptions &);
    using is_loaded_fn = sl::Result (*)(sl::Feature, bool &);
    using get_function_fn = sl::Result (*)(sl::Feature, const char *, void *&);

    dlss_set_options_fn g_dlss_set_options = nullptr;     // trampoline of the hooked slDLSSSetOptions
    dlssd_set_options_fn g_dlssd_set_options = nullptr;   // plain pointer, never hooked

    std::mutex g_rr_mutex;
    struct sr_options
    {
        sl::DLSSMode mode = sl::DLSSMode::eOff;
        uint32_t width = 0, height = 0;
        float pre_exposure = 1.0f, exposure_scale = 1.0f;
        sl::Boolean hdr = sl::Boolean::eTrue, auto_exposure = sl::Boolean::eFalse;
        bool valid = false;
    } g_sr;
    struct camera_basis
    {
        float r[3] {}, u[3] {}, f[3] {}, p[3] {};
        bool valid = false;
    } g_basis;

    // The on/off switch lives in rr::cfg::rr_enabled (settings.cpp).
    int g_rr_state = 0;              // 0 = not checked, 1 = available, -1 = unavailable
    uint64_t g_rr_ok = 0, g_rr_fail = 0, g_sr_fallback = 0;
    uint64_t g_last_dispatches = 0;
    int g_consecutive_fail = 0;
    std::string g_rr_last_error;
    sl::Constants g_last_constants;
    std::atomic<uint64_t> g_hit_tagged { 0 };   // evaluations that carried the diffuse hit distance guide
    std::atomic<uint64_t> g_smv_tagged { 0 };   // evaluations that carried the specular motion vectors
    bool g_have_constants = false;
    bool g_prev_frame_rr = false;
    uint64_t g_last_rr_tick = 0, g_last_copies = 0, g_resets = 0;
    uint32_t g_last_rr_w = 0, g_last_rr_h = 0;
    int g_active_preset = 0;           // preset of the last slDLSSDSetOptions

    // Exposure for RR (kBufferTypeExposure): a 1x1 R32_FLOAT texture holding 2^EV. The game gives DLSS no
    // exposure, which is harmless for DLSS-SR but lets RR filter the raw HDR, where the sky is hundreds of
    // times brighter than a leaf in shadow. Updated from a small upload ring when the value changes.
    ID3D12Resource *g_exposure_tex = nullptr, *g_exposure_upload = nullptr;
    uint8_t *g_exposure_mapped = nullptr;
    float g_exposure_value = -1.0f;    // value currently in the texture
    bool g_exposure_in_common = true;  // fresh texture: still in COMMON
    int g_exposure_slot = 0;
    int g_exposure_applied = 0;
    std::string g_exposure_error;
    std::string g_last_result = "-";   // result of the last RR evaluation
    bool g_last_eval_rr = false;

    using free_fn = sl::Result (*)(sl::Feature, const sl::ViewportHandle &);
    free_fn g_free_resources = nullptr;
    int g_rr_streak = 0, g_sr_streak = 0;
    bool g_sr_freed = false, g_rr_freed = false;
    constexpr int kFreeAfterFrames = 10;   // > frames in flight: the other model is idle on the GPU
    constexpr bool kFreeIdleModels = false;

    uint32_t viewport_of(const sl::BaseStructure **inputs, uint32_t count)
    {
        uint32_t v = 0;
        for (uint32_t i = 0; i < count; ++i)
            if (inputs[i] != nullptr && memcmp(&inputs[i]->structType, &sl::ViewportHandle::s_structType, sizeof(sl::StructType)) == 0)
                v = uint32_t(*static_cast<const sl::ViewportHandle *>(inputs[i]));
        return v;
    }

    // Called with g_rr_mutex held, on the thread that evaluates DLSS.
    void free_idle_model(bool rr_used, uint32_t viewport_value)
    {
        // Disabled (3 oct 2026): after slFreeResources Streamline did not rebuild the model correctly
        // when switching back with F11 (DLSS-SR black screen, DLSS-RR drawn only in a render-size
        // corner). Both models stay allocated, as in the version that switched cleanly.
        if (!kFreeIdleModels)
            return;
        if (g_free_resources == nullptr)
            return;
        const sl::ViewportHandle viewport(viewport_value);
        if (rr_used)
        {
            g_sr_streak = 0;
            g_rr_freed = false;
            if (!g_sr_freed && ++g_rr_streak >= kFreeAfterFrames)
            {
                const sl::Result r = g_free_resources(sl::kFeatureDLSS, viewport);
                g_sr_freed = true;
                rr::log_info(std::string("DLSS-SR resources freed while RR is active: ") + sl::getResultAsStr(r));
            }
        }
        else if (g_rr_ok > 0)
        {
            g_rr_streak = 0;
            g_sr_freed = false;
            if (!g_rr_freed && ++g_sr_streak >= kFreeAfterFrames)
            {
                const sl::Result r = g_free_resources(sl::kFeatureDLSS_RR, viewport);
                g_rr_freed = true;
                rr::log_info(std::string("DLSS-RR resources freed while DLSS-SR is active: ") + sl::getResultAsStr(r));
            }
        }
    }

    sl::Result dlss_set_options_hook(const sl::ViewportHandle &viewport, const sl::DLSSOptions &o)
    {
        rr::hook_guard guard;
        {
            std::lock_guard lock(g_rr_mutex);
            const bool first = !g_sr.valid || g_sr.mode != o.mode || g_sr.width != o.outputWidth || g_sr.height != o.outputHeight;
            g_sr.mode = o.mode;
            g_sr.width = o.outputWidth;
            g_sr.height = o.outputHeight;
            g_sr.pre_exposure = o.preExposure;
            g_sr.exposure_scale = o.exposureScale;
            g_sr.hdr = o.colorBuffersHDR;
            g_sr.auto_exposure = o.useAutoExposure;
            g_sr.valid = true;
            if (first)
                rr::log_info("slDLSSSetOptions (new or changed): mode=" + std::to_string(int(o.mode)) + " output=" + std::to_string(o.outputWidth) + "x" +
                             std::to_string(o.outputHeight) + " preExposure=" + fmt(o.preExposure) + " exposureScale=" + fmt(o.exposureScale) +
                             " hdr=" + boolean(o.colorBuffersHDR) + " autoExposure=" + boolean(o.useAutoExposure));
        }
        return g_dlss_set_options(viewport, o);
    }

    // Row-vector matrices (v * M), the convention of the game's cameraViewToClip.
    void build_matrices(const camera_basis &b, sl::float4x4 &world_to_view, sl::float4x4 &view_to_world)
    {
        auto dot = [](const float *a, const float *c) { return a[0] * c[0] + a[1] * c[1] + a[2] * c[2]; };
        world_to_view.setRow(0, sl::float4(b.r[0], b.u[0], b.f[0], 0.0f));
        world_to_view.setRow(1, sl::float4(b.r[1], b.u[1], b.f[1], 0.0f));
        world_to_view.setRow(2, sl::float4(b.r[2], b.u[2], b.f[2], 0.0f));
        world_to_view.setRow(3, sl::float4(-dot(b.p, b.r), -dot(b.p, b.u), -dot(b.p, b.f), 1.0f));
        view_to_world.setRow(0, sl::float4(b.r[0], b.r[1], b.r[2], 0.0f));
        view_to_world.setRow(1, sl::float4(b.u[0], b.u[1], b.u[2], 0.0f));
        view_to_world.setRow(2, sl::float4(b.f[0], b.f[1], b.f[2], 0.0f));
        view_to_world.setRow(3, sl::float4(b.p[0], b.p[1], b.p[2], 1.0f));
    }

    // Writes `value` into the exposure texture if it changed (copy recorded on `list`). Called with
    // g_rr_mutex held. Returns false when the texture cannot be used (the frame then goes untagged).
    bool update_exposure(ID3D12GraphicsCommandList *list, ID3D12Resource *any_resource, float value)
    {
        if (g_exposure_tex == nullptr)
        {
            ID3D12Device *dev = nullptr;
            if (FAILED(any_resource->GetDevice(IID_PPV_ARGS(&dev))))
                return false;
            D3D12_HEAP_PROPERTIES heap {};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC tex {};
            tex.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            tex.Width = 1;
            tex.Height = 1;
            tex.DepthOrArraySize = 1;
            tex.MipLevels = 1;
            tex.Format = DXGI_FORMAT_R32_FLOAT;
            tex.SampleDesc.Count = 1;
            HRESULT hr = dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &tex, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&g_exposure_tex));
            if (SUCCEEDED(hr))
            {
                heap.Type = D3D12_HEAP_TYPE_UPLOAD;
                D3D12_RESOURCE_DESC buf {};
                buf.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                buf.Width = 3 * D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
                buf.Height = 1;
                buf.DepthOrArraySize = 1;
                buf.MipLevels = 1;
                buf.SampleDesc.Count = 1;
                buf.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                hr = dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buf, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&g_exposure_upload));
                if (SUCCEEDED(hr))
                    hr = g_exposure_upload->Map(0, nullptr, reinterpret_cast<void **>(&g_exposure_mapped));
            }
            dev->Release();
            if (FAILED(hr))
            {
                g_exposure_error = rr::tr("could not create the exposure texture (", "no se pudo crear la textura de exposici\xC3\xB3n (") + rr::hex(uint32_t(hr)) + ")";
                if (g_exposure_tex) { g_exposure_tex->Release(); g_exposure_tex = nullptr; }
                if (g_exposure_upload) { g_exposure_upload->Release(); g_exposure_upload = nullptr; }
                return false;
            }
            g_exposure_error.clear();
            rr::log_info("RR: exposure texture created");
        }
        if (value == g_exposure_value)
            return true;
        // A ring of 3 slots: a slot is rewritten only 3 changes later, long after the GPU read it.
        g_exposure_slot = (g_exposure_slot + 1) % 3;
        const UINT64 offset = UINT64(g_exposure_slot) * D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
        memcpy(g_exposure_mapped + offset, &value, sizeof(value));
        D3D12_RESOURCE_BARRIER b {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = g_exposure_tex;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = g_exposure_in_common ? D3D12_RESOURCE_STATE_COMMON : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        list->ResourceBarrier(1, &b);
        D3D12_TEXTURE_COPY_LOCATION dst {}, src {};
        dst.pResource = g_exposure_tex;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.pResource = g_exposure_upload;
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Offset = offset;
        src.PlacedFootprint.Footprint = { DXGI_FORMAT_R32_FLOAT, 1, 1, 1, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT };
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        list->ResourceBarrier(1, &b);
        g_exposure_in_common = false;
        g_exposure_value = value;
        return true;
    }

    // Expected fallback (not a failure): this frame uses DLSS-SR. Logged once per distinct reason.
    std::string g_last_skip;
    void rr_skip(const std::string &why)
    {
        g_sr_fallback++;
        g_prev_frame_rr = false;
        if (why != g_last_skip)
        {
            g_last_skip = why;
            rr::log_info("RR skipped this frame (DLSS-SR used): " + why);
        }
    }

    void rr_fail(const std::string &why)
    {
        g_rr_fail++;
        if (++g_consecutive_fail >= 3 && g_rr_state == 1)
        {
            g_rr_state = -1;
            rr::log_warn("Ray Reconstruction disabled after 3 consecutive failures: " + why);
        }
        if (why != g_rr_last_error)
        {
            g_rr_last_error = why;
            rr::log_warn("Ray Reconstruction frame fell back to DLSS-SR: " + why);
        }
    }

    // Evaluates DLSS-RR instead of DLSS-SR. Returns false (nothing recorded) when the frame must
    // fall back to the game's own DLSS-SR evaluation.
    bool try_ray_reconstruction(const sl::FrameToken &frame, const sl::BaseStructure **inputs, uint32_t count, sl::CommandBuffer *cmd, sl::Result &result)
    {
        std::lock_guard lock(g_rr_mutex);
        if (!rr::cfg::rr_enabled || g_rr_state != 1 || g_dlssd_set_options == nullptr)
            return false;
        if (!g_sr.valid || !g_basis.valid)
            return rr_skip("no DLSS options or camera yet"), false;

        ID3D12Resource *nr = nullptr, *diffuse = nullptr, *specular = nullptr;
        uint32_t w = 0, h = 0;
        uint64_t dispatches = 0, copies = 0;
        if (!rr::guides_outputs(&nr, &diffuse, &specular, w, h, dispatches, copies))
            return rr_skip("guide buffers not built yet"), false;
        // Frames without a fresh G-buffer copy (menus, loading, Alt+Tab) or whose guides do not match
        // the DLSS input size use DLSS-SR. Not counted as failures.
        if (copies == g_last_copies)
            return rr_skip("no G-buffer copy this frame"), false;
        const uint32_t ew = g_render_w.load(), eh = g_render_h.load();
        if (ew != 0 && (w != ew || h != eh))
            return rr_skip("guide size differs from the DLSS input size"), false;
        // If the previous frame was RR, the guide shader must have run inside that evaluation
        // (NGX marker matched). Otherwise RR would read stale guides: disable it for the session.
        if (g_prev_frame_rr && dispatches == g_last_dispatches)
        {
            g_rr_state = -1;
            rr::log_warn("Ray Reconstruction disabled: the guide shader did not run inside the RR evaluation (NGX marker name not matched)");
            return false;
        }

        uint32_t viewport_value = 0;
        for (uint32_t i = 0; i < count; ++i)
            if (inputs[i] != nullptr && memcmp(&inputs[i]->structType, &sl::ViewportHandle::s_structType, sizeof(sl::StructType)) == 0)
                viewport_value = uint32_t(*static_cast<const sl::ViewportHandle *>(inputs[i]));
        const sl::ViewportHandle viewport(viewport_value);

        sl::DLSSDOptions o;
        o.mode = g_sr.mode;
        o.outputWidth = g_sr.width;
        o.outputHeight = g_sr.height;
        o.preExposure = g_sr.pre_exposure;
        o.exposureScale = g_sr.exposure_scale;
        o.colorBuffersHDR = g_sr.hdr;
        o.normalRoughnessMode = sl::DLSSDNormalRoughnessMode::ePacked;
        // Same preset for every quality mode. Default F (DLSS 4.5 second-generation transformer), chosen
        // by the user; the overlay can switch to D, E or the driver default.
        const int preset = rr::cfg::preset.load();
        const bool preset_changed = preset != g_active_preset;
        o.dlaaPreset = o.qualityPreset = o.balancedPreset = o.performancePreset = o.ultraPerformancePreset = o.ultraQualityPreset = sl::DLSSDPreset(preset);
        build_matrices(g_basis, o.worldToCameraView, o.cameraViewToWorld);
        sl::Result r = g_dlssd_set_options(viewport, o);
        if (r != sl::Result::eOk)
            return rr_fail(std::string("slDLSSDSetOptions -> ") + sl::getResultAsStr(r)), false;
        if (preset_changed)
        {
            if (g_active_preset != 0 || g_rr_ok > 0)
                rr::log_info("RR preset changed to " + std::to_string(preset) + " (6 = F, 5 = E, 4 = D, 0 = driver default)");
            g_active_preset = preset;
        }

        // Exposure: make sure the texture holds 2^EV before tagging it (recorded on the game's list).
        const int ev = rr::cfg::rr_exposure_ev.load();
        bool tag_exposure = false;
        if (ev != 0)
            tag_exposure = update_exposure(reinterpret_cast<ID3D12GraphicsCommandList *>(cmd), nr, std::exp2(float(ev)));

        // The guides stay in NON_PIXEL_SHADER_RESOURCE between frames (guides.cpp).
        static sl::Resource resources[6];
        resources[0] = sl::Resource(sl::ResourceType::eTex2d, nr, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        resources[1] = sl::Resource(sl::ResourceType::eTex2d, diffuse, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        resources[2] = sl::Resource(sl::ResourceType::eTex2d, specular, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        const sl::Extent extent { 0, 0, w, h };
        resources[3] = sl::Resource(sl::ResourceType::eTex2d, g_exposure_tex, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        static const sl::Extent exposure_extent { 0, 0, 1, 1 };
        ID3D12Resource *const hit = rr::cfg::rr_hit_distance.load() ? rr::guides_hit_distance() : nullptr;
        if (hit != nullptr)
            resources[4] = sl::Resource(sl::ResourceType::eTex2d, hit, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        sl::ResourceTag tags[7] = {
            sl::ResourceTag(&resources[0], sl::kBufferTypeNormalRoughness, sl::ResourceLifecycle::eValidUntilPresent, &extent),
            sl::ResourceTag(&resources[0], sl::kBufferTypeNormals, sl::ResourceLifecycle::eValidUntilPresent, &extent),
            sl::ResourceTag(&resources[1], sl::kBufferTypeAlbedo, sl::ResourceLifecycle::eValidUntilPresent, &extent),
            sl::ResourceTag(&resources[2], sl::kBufferTypeSpecularAlbedo, sl::ResourceLifecycle::eValidUntilPresent, &extent),
            sl::ResourceTag(&resources[3], sl::kBufferTypeExposure, sl::ResourceLifecycle::eValidUntilPresent, &exposure_extent),
        };
        uint32_t tag_count = tag_exposure ? 5 : 4;
        if (hit != nullptr)
            tags[tag_count++] = sl::ResourceTag(&resources[4], sl::kBufferTypeDiffuseHitDistance, sl::ResourceLifecycle::eValidUntilPresent, &extent);
        // Specular motion vectors (specmv.cpp): written by the guide pass inside this evaluation, at the motion
        // vector resolution. Left out until the pass ran once, and whenever the option is off.
        uint32_t smv_w = 0, smv_h = 0;
        ID3D12Resource *const smv = rr::cfg::rr_spec_mv.load() ? rr::specmv_output(smv_w, smv_h) : nullptr;
        static sl::Extent smv_extent {};
        if (smv != nullptr)
        {
            smv_extent = { 0, 0, smv_w, smv_h };
            resources[5] = sl::Resource(sl::ResourceType::eTex2d, smv, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            tags[tag_count++] = sl::ResourceTag(&resources[5], sl::kBufferTypeSpecularMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &smv_extent);
            g_smv_tagged++;
        }
        r = g_set_tag(frame, viewport, tags, tag_count, cmd);
        if (hit != nullptr)
            g_hit_tagged++;
        if (r != sl::Result::eOk)
            return rr_fail(std::string("slSetTagForFrame (guides) -> ") + sl::getResultAsStr(r)), false;

        g_last_dispatches = dispatches;
        g_last_copies = copies;
        const uint64_t now = GetTickCount64();
        const bool requested = rr::cfg::reset_history.exchange(false);
        const bool reset = requested || preset_changed || !g_prev_frame_rr || now - g_last_rr_tick > 250 || w != g_last_rr_w || h != g_last_rr_h;
        if (reset && g_have_constants)
        {
            sl::Constants c = g_last_constants;
            c.reset = sl::Boolean::eTrue;
            g_set_constants(c, frame, viewport);
            if (g_resets++ < 20)
                rr::log_info(requested ? "RR history reset (requested from the overlay)" : "RR history reset (after a switch, a gap, a resize or a preset change)");
        }
        r = g_evaluate(sl::kFeatureDLSS_RR, frame, inputs, count, cmd);
        g_last_rr_tick = now;
        g_last_rr_w = w;
        g_last_rr_h = h;
        if (r != sl::Result::eOk && r != sl::Result::eWarnOutOfVRAM)
            return rr_fail(std::string("slEvaluateFeature(kFeatureDLSS_RR) -> ") + sl::getResultAsStr(r)), false;

        g_consecutive_fail = 0;
        g_last_result = sl::getResultAsStr(r);
        g_exposure_applied = tag_exposure ? ev : 0;
        if (g_rr_ok++ == 0)
            rr::log_info(std::string("Ray Reconstruction active (first evaluation: ") + sl::getResultAsStr(r) + ", preset " + std::to_string(preset) + ", mode " + std::to_string(int(o.mode)) +
                         ", " + std::to_string(o.outputWidth) + "x" + std::to_string(o.outputHeight) + ")");
        g_prev_frame_rr = true;
        g_last_eval_rr = true;
        free_idle_model(true, viewport_value);
        result = r;
        return true;
    }

    // Checks once whether Streamline loaded the DLSS-RR plugin and hooks slDLSSSetOptions.
    void setup_ray_reconstruction(HMODULE module)
    {
        auto get_function = reinterpret_cast<get_function_fn>(GetProcAddress(module, "slGetFeatureFunction"));
        auto is_loaded = reinterpret_cast<is_loaded_fn>(GetProcAddress(module, "slIsFeatureLoaded"));
        g_free_resources = reinterpret_cast<free_fn>(GetProcAddress(module, "slFreeResources"));
        if (get_function == nullptr || is_loaded == nullptr)
        {
            rr::log_warn("RR: sl.interposer lacks slGetFeatureFunction/slIsFeatureLoaded");
            g_rr_state = -1;
            return;
        }
        void *fn = nullptr;
        if (get_function(sl::kFeatureDLSS, "slDLSSSetOptions", fn) == sl::Result::eOk && fn != nullptr)
            hook_address(fn, "slDLSSSetOptions", reinterpret_cast<void *>(dlss_set_options_hook), g_dlss_set_options);
        bool loaded = false;
        const sl::Result r = is_loaded(sl::kFeatureDLSS_RR, loaded);
        rr::log_info(std::string("RR: slIsFeatureLoaded(kFeatureDLSS_RR) -> ") + sl::getResultAsStr(r) + ", loaded=" + (loaded ? "yes" : "no") +
                     ", sl.dlss_d.dll in process: " + (GetModuleHandleW(L"sl.dlss_d.dll") ? "yes" : "no"));
        fn = nullptr;
        if (loaded && get_function(sl::kFeatureDLSS_RR, "slDLSSDSetOptions", fn) == sl::Result::eOk && fn != nullptr)
        {
            g_dlssd_set_options = reinterpret_cast<dlssd_set_options_fn>(fn);
            g_rr_state = 1;
            rr::log_info(std::string("RR: available; F11 or the overlay toggle DLSS-RR / DLSS-SR (starts ") + (rr::cfg::rr_enabled ? "ON)" : "OFF)"));
        }
        else
        {
            g_rr_state = -1;
            rr::log_warn("RR: DLSS-RR plugin not loaded by the game's slInit; the next step is to add it there");
        }
    }

    sl::Result evaluate_hook(sl::Feature feature, const sl::FrameToken &frame, const sl::BaseStructure **inputs, uint32_t count, sl::CommandBuffer *cmd)
    {
        rr::hook_guard guard;
        if (feature == sl::kFeatureDLSS)
        {
            sl::Result rr_result = sl::Result::eOk;
            if (try_ray_reconstruction(frame, inputs, count, cmd, rr_result))
            {
                rr::sharpen_after_rr();          // before the bench copy, so the bench measures what is shown
                rr::bench_after_evaluate(true);
                if (rr::capture_active())
                    emit(cmd, std::string("[SL] slEvaluateFeature kFeatureDLSS replaced by kFeatureDLSS_RR -> ") + sl::getResultAsStr(rr_result));
                return rr_result;
            }
            if (rr::cfg::rr_enabled && g_rr_state == 1)
                g_sr_fallback++;
        }
        const sl::Result result = g_evaluate(feature, frame, inputs, count, cmd);
        if (feature == sl::kFeatureDLSS && (result == sl::Result::eOk || result == sl::Result::eWarnOutOfVRAM))
        {
            rr::bench_after_evaluate(false);
            std::lock_guard lock(g_rr_mutex);
            g_prev_frame_rr = false;
            g_last_eval_rr = false;
            free_idle_model(false, viewport_of(inputs, count));
        }
        if (rr::capture_active())
        {
            std::string line = std::string("[SL] slEvaluateFeature ") + sl::getFeatureAsStr(feature) + " -> " + sl::getResultAsStr(result);
            for (uint32_t i = 0; i < count; ++i)
            {
                const sl::BaseStructure *in = inputs[i];
                if (in == nullptr)
                    continue;
                if (memcmp(&in->structType, &sl::ViewportHandle::s_structType, sizeof(sl::StructType)) == 0)
                    line += " vp=" + std::to_string(uint32_t(*static_cast<const sl::ViewportHandle *>(in)));
                else if (memcmp(&in->structType, &sl::ResourceTag::s_structType, sizeof(sl::StructType)) == 0)
                    line += "\n      input " + describe_tag(*static_cast<const sl::ResourceTag *>(in));
                else
                    line += " input(other struct)";
            }
            emit(cmd, std::move(line));
        }
        return result;
    }

    sl::Result set_constants_hook(const sl::Constants &c, const sl::FrameToken &frame, const sl::ViewportHandle &viewport)
    {
        rr::hook_guard guard;
        {
            const float r[3] = { c.cameraRight.x, c.cameraRight.y, c.cameraRight.z };
            const float u[3] = { c.cameraUp.x, c.cameraUp.y, c.cameraUp.z };
            const float f[3] = { c.cameraFwd.x, c.cameraFwd.y, c.cameraFwd.z };
            rr::guides_set_camera(r, u, f, c.cameraViewToClip[0].x, c.cameraViewToClip[1].y);
            rr::specmv_set_constants(&c.clipToCameraView[0].x, &c.cameraViewToClip[0].x, &c.clipToPrevClip[0].x, c.mvecScale.x, c.mvecScale.y);
            std::lock_guard lock(g_rr_mutex);
            for (int i = 0; i < 3; ++i)
            {
                g_basis.r[i] = r[i];
                g_basis.u[i] = u[i];
                g_basis.f[i] = f[i];
            }
            g_basis.p[0] = c.cameraPos.x;
            g_basis.p[1] = c.cameraPos.y;
            g_basis.p[2] = c.cameraPos.z;
            g_basis.valid = true;
            g_last_constants = c;
            g_have_constants = true;
        }
        {
            // Camera motion for "GI original en movimiento": displacement and turn since the previous frame
            // (one update per frame token; the game may set constants more than once per frame).
            static float prev_p[3] {}, prev_f[3] {};
            static uint32_t prev_frame = 0;
            static bool have_prev = false;
            const uint32_t token = uint32_t(frame);
            if (!have_prev || token != prev_frame)
            {
                const float p[3] = { c.cameraPos.x, c.cameraPos.y, c.cameraPos.z }, f[3] = { c.cameraFwd.x, c.cameraFwd.y, c.cameraFwd.z };
                if (have_prev)
                {
                    float move = 0.0f, dot = 0.0f, na = 0.0f, nb = 0.0f;
                    for (int i = 0; i < 3; ++i)
                    {
                        move += (p[i] - prev_p[i]) * (p[i] - prev_p[i]);
                        dot += f[i] * prev_f[i];
                        na += f[i] * f[i];
                        nb += prev_f[i] * prev_f[i];
                    }
                    const float cosine = na > 0 && nb > 0 ? std::min(1.0f, std::max(-1.0f, dot / std::sqrt(na * nb))) : 1.0f;
                    rr::gi_motion_update(std::sqrt(move), std::acos(cosine) * 57.29578f, token);
                }
                for (int i = 0; i < 3; ++i)
                {
                    prev_p[i] = p[i];
                    prev_f[i] = f[i];
                }
                prev_frame = token;
                have_prev = true;
            }
        }
        if (rr::capture_active() && !g_constants_logged_this_capture)
        {
            g_constants_logged_this_capture = true;
            std::string s = "[SL] slSetConstants vp=" + std::to_string(uint32_t(viewport));
            s += "\n      cameraViewToClip " + matrix(c.cameraViewToClip);
            s += "\n      clipToCameraView " + matrix(c.clipToCameraView);
            s += "\n      clipToPrevClip " + matrix(c.clipToPrevClip);
            s += "\n      jitter=" + fmt(c.jitterOffset.x) + "," + fmt(c.jitterOffset.y) + " mvecScale=" + fmt(c.mvecScale.x) + "," + fmt(c.mvecScale.y);
            s += "\n      cameraPos=" + fmt(c.cameraPos.x) + "," + fmt(c.cameraPos.y) + "," + fmt(c.cameraPos.z) +
                 " up=" + fmt(c.cameraUp.x) + "," + fmt(c.cameraUp.y) + "," + fmt(c.cameraUp.z) +
                 " right=" + fmt(c.cameraRight.x) + "," + fmt(c.cameraRight.y) + "," + fmt(c.cameraRight.z) +
                 " fwd=" + fmt(c.cameraFwd.x) + "," + fmt(c.cameraFwd.y) + "," + fmt(c.cameraFwd.z);
            s += "\n      near=" + fmt(c.cameraNear) + " far=" + fmt(c.cameraFar) + " fov=" + fmt(c.cameraFOV) + " aspect=" + fmt(c.cameraAspectRatio);
            s += std::string("\n      depthInverted=") + boolean(c.depthInverted) + " cameraMotionIncluded=" + boolean(c.cameraMotionIncluded) +
                 " motionVectors3D=" + boolean(c.motionVectors3D) + " reset=" + boolean(c.reset) + " mvecDilated=" + boolean(c.motionVectorsDilated) +
                 " mvecJittered=" + boolean(c.motionVectorsJittered);
            rr::frame_event(std::move(s));
        }
        else if (!rr::capture_active())
        {
            g_constants_logged_this_capture = false;
        }
        return g_set_constants(c, frame, viewport);
    }

    template <typename T>
    bool hook(HMODULE module, const char *name, void *detour, T &original)
    {
        void *target = reinterpret_cast<void *>(GetProcAddress(module, name));
        if (target == nullptr)
        {
            rr::log_warn(std::string("sl.interposer has no export ") + name);
            return false;
        }
        MH_STATUS s = MH_CreateHook(target, detour, reinterpret_cast<void **>(&original));
        if (s == MH_OK)
            s = MH_EnableHook(target);
        if (s != MH_OK)
        {
            rr::log_warn(std::string("hook on ") + name + " failed: " + MH_StatusToString(s));
            return false;
        }
        return true;
    }
}

namespace rr
{
    bool rr_is_on() { return rr::cfg::rr_enabled.load(std::memory_order_relaxed) && g_rr_state == 1; }
}

namespace rr
{
    void rr_toggle()
    {
        const bool on = !rr::cfg::rr_enabled.load();
        rr::cfg::rr_enabled = on;
        cfg::save();
        log_info(std::string("F11: Ray Reconstruction ") + (on ? "ON" : "OFF (DLSS-SR)"));
    }

    rr_snapshot rr_get_snapshot()
    {
        rr_snapshot s;
        s.hit_tagged = g_hit_tagged.load();
        s.smv_tagged = g_smv_tagged.load();
        std::lock_guard lock(g_rr_mutex);
        s.state = g_rr_state;
        s.enabled = rr::cfg::rr_enabled.load();
        s.evaluating = g_last_eval_rr;
        s.ok = g_rr_ok;
        s.fail = g_rr_fail;
        s.fallback = g_sr_fallback;
        s.resets = g_resets;
        s.last_error = g_rr_last_error;
        s.last_skip = g_last_skip;
        s.last_result = g_last_result;
        s.mode = int(g_sr.mode);
        s.out_w = g_sr.width;
        s.out_h = g_sr.height;
        s.render_w = g_render_w.load();
        s.render_h = g_render_h.load();
        s.pre_exposure = g_sr.pre_exposure;
        s.hdr = g_sr.hdr == sl::Boolean::eTrue;
        s.auto_exposure = g_sr.auto_exposure == sl::Boolean::eTrue;
        s.active_preset = g_active_preset;
        s.last_rr_tick = g_last_eval_rr ? g_last_rr_tick : 0;
        if (g_have_constants)
        {
            s.camera_near = g_last_constants.cameraNear;
            s.camera_far = g_last_constants.cameraFar;
        }
        s.exposure_ev_applied = g_exposure_applied;
        s.exposure_error = g_exposure_error;
        return s;
    }

    std::string rr_status()
    {
        std::lock_guard lock(g_rr_mutex);
        return std::string("rr: state=") + std::to_string(g_rr_state) + " enabled=" + (rr::cfg::rr_enabled ? "1" : "0") + " ok=" + std::to_string(g_rr_ok) +
               " fail=" + std::to_string(g_rr_fail) + " sr_fallback=" + std::to_string(g_sr_fallback) + (g_rr_last_error.empty() ? "" : " last_error=" + g_rr_last_error);
    }
}

namespace rr
{
    // Returns false only while sl.interposer is not loaded yet (retry on the next present).
    bool install_streamline_hooks()
    {
        HMODULE module = GetModuleHandleW(L"sl.interposer.dll");
        if (module == nullptr)
            return false;
        if (!ensure_minhook())
            return true;
        int ok = 0;
        ok += hook(module, "slSetTagForFrame", reinterpret_cast<void *>(set_tag_hook), g_set_tag);
        ok += hook(module, "slEvaluateFeature", reinterpret_cast<void *>(evaluate_hook), g_evaluate);
        ok += hook(module, "slSetConstants", reinterpret_cast<void *>(set_constants_hook), g_set_constants);
        log_info("Streamline observer hooks installed: " + std::to_string(ok) + "/3");
        setup_ray_reconstruction(module);
        return true;
    }
}
