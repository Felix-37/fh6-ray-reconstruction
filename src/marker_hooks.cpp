// Hooks on the native ID3D12GraphicsCommandList implementation (the functions its vtable points
// to inside the D3D12 runtime):
// - BeginEvent/EndEvent/SetMarker: PIX marker names, used to name the render passes.
// - ResourceBarrier and Barrier (enhanced barriers): exact barrier data. ReShade's barrier event
//   drops split-barrier flags, subresource indices and enhanced-barrier layouts; capturing on that
//   incomplete data corrupted the game's textures (3 oct 2026), so captures now work from here.

#include "common.hpp"

#include <MinHook.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace
{
    using begin_event_fn = void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *, UINT, const void *, UINT);
    using end_event_fn = void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *);
    using resource_barrier_fn = void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList *, UINT, const D3D12_RESOURCE_BARRIER *);
    using barrier_fn = void(STDMETHODCALLTYPE *)(ID3D12GraphicsCommandList7 *, UINT32, const D3D12_BARRIER_GROUP *);

    // Vtable slots verified against the llvm-mingw d3d12.h (offsetof on the C interface).
    constexpr size_t kResourceBarrier = 26, kSetMarker = 56, kBeginEvent = 57, kEndEvent = 58, kBarrier = 80;

    begin_event_fn g_begin_original = nullptr;
    begin_event_fn g_marker_original = nullptr;
    end_event_fn g_end_original = nullptr;
    resource_barrier_fn g_resource_barrier_original = nullptr;
    barrier_fn g_barrier_original = nullptr;
    std::vector<void *> g_targets;
    int g_raw_dumps = 0;

    int score(const std::string &s)
    {
        int good = 0;
        for (unsigned char c : s)
            good += (isalnum(c) || c == '_' || c == ' ' || c == '/' || c == ':' || c == '-' || c == '.') ? 1 : -3;
        return good;
    }

    std::string from_wide(const wchar_t *text, size_t max_chars)
    {
        std::string out;
        for (size_t i = 0; i < max_chars && text[i] != 0; ++i)
            out += (text[i] >= 0x20 && text[i] < 0x7f) ? char(text[i]) : '\x01';
        return out;
    }

    std::string from_ansi(const char *text, size_t max_chars)
    {
        std::string out;
        for (size_t i = 0; i < max_chars && text[i] != 0; ++i)
            out += (text[i] >= 0x20 && text[i] < 0x7f) ? text[i] : '\x01';
        return out;
    }

    // Longest printable run, narrow or UTF-16, as a fallback for unknown encodings.
    std::string scan_printable(const uint8_t *data, size_t size)
    {
        std::string best, cur;
        for (size_t i = 0; i < size; ++i)
        {
            if (data[i] >= 0x20 && data[i] <= 0x7e) cur += char(data[i]);
            else { if (cur.size() > best.size()) best = cur; cur.clear(); }
        }
        if (cur.size() > best.size()) best = cur;
        std::string wbest;
        cur.clear();
        for (size_t i = 0; i + 1 < size; i += 2)
        {
            if (data[i + 1] == 0 && data[i] >= 0x20 && data[i] <= 0x7e) cur += char(data[i]);
            else { if (cur.size() > wbest.size()) wbest = cur; cur.clear(); }
        }
        if (cur.size() > wbest.size()) wbest = cur;
        return wbest.size() > best.size() ? wbest : best;
    }

    std::string raw_hex(const void *data, UINT size)
    {
        std::string s;
        const auto *p = static_cast<const uint8_t *>(data);
        char b[4];
        for (UINT i = 0; i < size && i < 64; ++i) { snprintf(b, sizeof(b), "%02x", p[i]); s += b; if (i % 8 == 7) s += ' '; }
        return s;
    }

    // Metadata 0 = ANSI string, 1 = UTF-16 string, 2 = PIX3 blob. Every candidate decoding is
    // scored and the most text-like one wins; the first blobs are also dumped raw to refine this.
    std::string decode(UINT metadata, const void *data, UINT size)
    {
        if (data == nullptr || size == 0)
            return {};
        __try
        {
            std::string best = scan_printable(static_cast<const uint8_t *>(data), size);
            auto consider = [&best](std::string s) { if (!s.empty() && score(s) > score(best)) best = std::move(s); };
            consider(from_ansi(static_cast<const char *>(data), size));
            consider(from_wide(static_cast<const wchar_t *>(data), size / 2));
            const auto *words = static_cast<const uint64_t *>(data);
            for (size_t skip = 1; skip <= 4 && skip * 8 < size; ++skip)
            {
                consider(from_ansi(reinterpret_cast<const char *>(words + skip), size - skip * 8));
                consider(from_wide(reinterpret_cast<const wchar_t *>(words + skip), (size - skip * 8) / 2));
            }
            if (g_raw_dumps < 4)
            {
                g_raw_dumps++;
                best += "   {meta=" + std::to_string(metadata) + " size=" + std::to_string(size) + " raw=" + raw_hex(data, size) + "}";
            }
            return best;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return "<unreadable>";
        }
    }

    // Exact, allocation-free match of a marker name for metadata 0 (ANSI), 1 (UTF-16) and 2 (PIX3,
    // text from byte 24, UTF-16 or ANSI). Runs on every BeginEvent, so no decoding here.
    bool marker_is(UINT metadata, const void *data, UINT size, const char *name)
    {
        if (data == nullptr)
            return false;
        const size_t n = strlen(name);
        const auto *b = static_cast<const uint8_t *>(data);
        auto ansi_at = [&](size_t off) { if (off + n + 1 > size) return false; for (size_t i = 0; i < n; ++i) if (b[off + i] != uint8_t(name[i])) return false; return b[off + n] == 0; };
        auto wide_at = [&](size_t off) { if (off + (n + 1) * 2 > size) return false; for (size_t i = 0; i < n; ++i) if (b[off + 2 * i] != uint8_t(name[i]) || b[off + 2 * i + 1] != 0) return false; return b[off + 2 * n] == 0 && b[off + 2 * n + 1] == 0; };
        __try
        {
            // NGX declares metadata 1 (UTF-16) but sends ANSI ("nv.ngx.dlss.Evaluate", 21 bytes), so the
            // declared encoding is not trusted: both encodings are tried at both offsets.
            (void)metadata;
            return ansi_at(0) || wide_at(0) || wide_at(24) || ansi_at(24);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // DLSS-SR and DLSS-RR evaluations (never DLSS-G). The RR name is not known yet, so the likely
    // spellings are accepted and every distinct "nv.ngx." marker is logged once to learn it.
    bool ngx_upscaler_marker(UINT metadata, const void *data, UINT size)
    {
        static const char *const names[] = { "nv.ngx.dlss.Evaluate", "nv.ngx.dlssd.Evaluate", "nv.ngx.dlss_d.Evaluate",
                                             "nv.ngx.dlssrr.Evaluate", "nv.ngx.dlss_rr.Evaluate" };
        for (const char *n : names)
            if (marker_is(metadata, data, size, n))
                return true;
        if (data != nullptr && size >= 7 && size < 64 && memcmp(data, "nv.ngx.", 7) == 0)
        {
            static std::mutex m;
            static std::vector<std::string> seen;
            std::string name(static_cast<const char *>(data), strnlen(static_cast<const char *>(data), size));
            std::lock_guard lock(m);
            if (seen.size() < 32 && std::find(seen.begin(), seen.end(), name) == seen.end())
            {
                seen.push_back(name);
                rr::log_info("NGX marker seen: " + name);
            }
        }
        return false;
    }

    // Marker nesting per native list, kept only while the bench runs: the bench copies the DLSS
    // output at the END of NGX's evaluate marker, on the native list, with enhanced barriers that
    // match the layouts the game uses (output UNORDERED_ACCESS, depth SHADER_RESOURCE).
    struct marker_depth
    {
        int depth = 0, ngx_depth = 0;
        bool rr = false;
    };
    std::mutex g_depth_mutex;
    std::unordered_map<ID3D12GraphicsCommandList *, marker_depth> g_depths;

    thread_local ID3D12GraphicsCommandList *t_ngx_list = nullptr;

    void STDMETHODCALLTYPE begin_event_hook(ID3D12GraphicsCommandList *self, UINT metadata, const void *data, UINT size)
    {
        rr::hook_guard guard;
        // NGX opens this marker on the native list right before recording DLSS: the guide shader
        // goes there, on the native list, so ReShade's command-list proxy never sees our objects.
        const bool upscaler = ngx_upscaler_marker(metadata, data, size);
        if (upscaler)
        {
            t_ngx_list = self;
            rr::guides_before_evaluate(self, self);
        }
        if (rr::bench_active())
        {
            std::lock_guard lock(g_depth_mutex);
            marker_depth &d = g_depths[self];
            d.depth++;
            if (upscaler)
            {
                d.ngx_depth = d.depth;
                d.rr = !marker_is(metadata, data, size, "nv.ngx.dlss.Evaluate");
            }
        }
        if (rr::capture_active())
            rr::list_event(self, "BEGIN " + decode(metadata, data, size));
        g_begin_original(self, metadata, data, size);
    }

    void STDMETHODCALLTYPE set_marker_hook(ID3D12GraphicsCommandList *self, UINT metadata, const void *data, UINT size)
    {
        rr::hook_guard guard;
        if (rr::capture_active())
            rr::list_event(self, "MARKER " + decode(metadata, data, size));
        g_marker_original(self, metadata, data, size);
    }

    void STDMETHODCALLTYPE end_event_hook(ID3D12GraphicsCommandList *self)
    {
        rr::hook_guard guard;
        if (rr::capture_active())
            rr::list_event(self, "END");
        g_end_original(self);
        if (rr::bench_active())
        {
            bool ngx_end = false, rr_used = false;
            {
                std::lock_guard lock(g_depth_mutex);
                auto it = g_depths.find(self);
                if (it != g_depths.end() && it->second.depth > 0)
                {
                    marker_depth &d = it->second;
                    if (d.ngx_depth != 0 && d.ngx_depth == d.depth)
                    {
                        ngx_end = true;
                        rr_used = d.rr;
                        d.ngx_depth = 0;
                    }
                    d.depth--;
                }
            }
            (void)ngx_end;
            (void)rr_used;
        }
        else if (!g_depths.empty())
        {
            std::lock_guard lock(g_depth_mutex);
            g_depths.clear();
        }
    }

    void STDMETHODCALLTYPE resource_barrier_hook(ID3D12GraphicsCommandList *self, UINT count, const D3D12_RESOURCE_BARRIER *barriers)
    {
        rr::hook_guard guard;
        g_resource_barrier_original(self, count, barriers);
        if (rr::capture_active())
            rr::on_native_resource_barrier(self, count, barriers);
    }

    void STDMETHODCALLTYPE barrier_hook(ID3D12GraphicsCommandList7 *self, UINT32 count, const D3D12_BARRIER_GROUP *groups)
    {
        rr::hook_guard guard;
        g_barrier_original(self, count, groups);
        rr::guides_on_barrier(self, count, groups);
        if (rr::bench_active())
            rr::bench_on_barrier(count, groups);
        if (rr::capture_active())
            rr::on_native_barrier(self, count, groups);
    }

    bool g_minhook_ready = false;

    bool create(void *target, void *detour, void **original, const char *name)
    {
        const MH_STATUS s = MH_CreateHook(target, detour, original);
        if (s != MH_OK)
        {
            rr::log_warn(std::string("hook ") + name + " failed: " + MH_StatusToString(s));
            return false;
        }
        g_targets.push_back(target);
        return true;
    }
}

namespace rr
{
    std::atomic<int> g_inflight { 0 };
}

namespace rr
{
    bool ensure_minhook()
    {
        if (g_minhook_ready)
            return true;
        const MH_STATUS status = MH_Initialize();
        g_minhook_ready = (status == MH_OK || status == MH_ERROR_ALREADY_INITIALIZED);
        if (!g_minhook_ready)
            log_warn(std::string("MH_Initialize failed: ") + MH_StatusToString(status));
        return g_minhook_ready;
    }

    // The originals bypass our own hooks (and nothing else: ReShade sits above, in its proxy).
    void raw_resource_barrier(ID3D12GraphicsCommandList *list, UINT count, const D3D12_RESOURCE_BARRIER *barriers)
    {
        g_resource_barrier_original(list, count, barriers);
    }

    void raw_barrier(ID3D12GraphicsCommandList7 *list, UINT32 count, const D3D12_BARRIER_GROUP *groups)
    {
        g_barrier_original(list, count, groups);
    }

    bool install_replacement_hook(ID3D12GraphicsCommandList *any_list);   // replace.cpp

    bool install_marker_hooks(ID3D12GraphicsCommandList *any_list)
    {
        if (!ensure_minhook())
            return false;
        void **vtable = *reinterpret_cast<void ***>(any_list);
        bool ok = true;
        ok &= create(vtable[kBeginEvent], reinterpret_cast<void *>(begin_event_hook), reinterpret_cast<void **>(&g_begin_original), "BeginEvent");
        ok &= create(vtable[kEndEvent], reinterpret_cast<void *>(end_event_hook), reinterpret_cast<void **>(&g_end_original), "EndEvent");
        ok &= create(vtable[kSetMarker], reinterpret_cast<void *>(set_marker_hook), reinterpret_cast<void **>(&g_marker_original), "SetMarker");
        ok &= create(vtable[kResourceBarrier], reinterpret_cast<void *>(resource_barrier_hook), reinterpret_cast<void **>(&g_resource_barrier_original), "ResourceBarrier");

        // Enhanced barriers only exist on ID3D12GraphicsCommandList7; the native object exposes all
        // interface versions through one linear vtable, so slot 80 is valid when the QI succeeds.
        ID3D12GraphicsCommandList7 *list7 = nullptr;
        bool enhanced = false;
        if (SUCCEEDED(any_list->QueryInterface(IID_PPV_ARGS(&list7))))
        {
            void **vtable7 = *reinterpret_cast<void ***>(list7);
            enhanced = create(vtable7[kBarrier], reinterpret_cast<void *>(barrier_hook), reinterpret_cast<void **>(&g_barrier_original), "Barrier");
            list7->Release();
        }
        if (!ok)
        {
            for (void *t : g_targets) MH_RemoveHook(t);
            g_targets.clear();
            return false;
        }
        for (void *t : g_targets)
            MH_EnableHook(t);
        install_replacement_hook(any_list);
        log_info(std::string("native command list hooks installed (markers, ResourceBarrier") + (enhanced ? ", Barrier" : "; no ID3D12GraphicsCommandList7") + ")");
        return true;
    }

    void shutdown_hooks()
    {
        if (!g_minhook_ready)
            return;
        MH_DisableHook(MH_ALL_HOOKS);
        // A thread may still be inside a detour (these functions run thousands of times per
        // second): wait until none is, so no code of this module runs after it is unmapped.
        for (int i = 0; i < 2000 && g_inflight.load() != 0; ++i)
            Sleep(1);
        if (g_inflight.load() != 0)
            log_warn("shutdown: threads still inside detours after 2 s");
        MH_Uninitialize();
        g_minhook_ready = false;
    }
}

namespace rr
{
    ID3D12GraphicsCommandList *last_ngx_list() { return t_ngx_list; }
}
