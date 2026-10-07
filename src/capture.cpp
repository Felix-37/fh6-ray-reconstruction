// Frame capture: records the command stream of one frame (render target bindings, pipelines,
// draw/dispatch counts, PIX markers, Streamline calls) and copies every texture that leaves a
// write state (render target, depth write, UAV) into readback buffers. The data is written to
// <capture_root()>\<time>\ by a background thread so the game only stalls once.

#include "common.hpp"

#include <reshade.hpp>

#include <atomic>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

using namespace reshade::api;

namespace
{
    constexpr uint64_t kMaxBytes = 1024ull << 20;   // readback budget per capture
    constexpr int kMaxCaptures = 400;
    constexpr int kMaxPerResource = 2;
    constexpr uint32_t kMaxDim = 2560;                // skips shadow atlases and other huge targets
    constexpr uint32_t kMinDim = 32;
    constexpr int kDrainPresents = 8;                 // presents to wait before reading back

    enum class state : int { idle, recording, draining };

    struct list_log
    {
        std::vector<std::string> markers;   // open PIX markers on this list
        std::vector<std::string> lines;
        uint32_t draws = 0, dispatches = 0, rays = 0, indirect = 0;
        int depth = 0;
    };

    struct pending_copy
    {
        int index = 0;
        ID3D12Resource *readback = nullptr;
        void *list = nullptr;
        bool submitted = false;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
        UINT rows = 0;
        UINT64 row_size = 0;
        uint32_t width = 0, height = 0;
        std::string label;
    };

    struct captured_image
    {
        int index = 0;
        std::string label;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        uint32_t width = 0, height = 0;
        uint64_t row_size = 0;
        std::vector<uint8_t> data;
    };

    std::mutex g_mutex;
    std::atomic<int> g_state { int(state::idle) };
    std::unordered_map<void *, list_log> g_lists;
    std::vector<std::string> g_frame;
    std::vector<pending_copy> g_copies;
    std::unordered_map<ID3D12Resource *, int> g_per_resource;
    uint64_t g_bytes = 0;
    int g_skipped = 0;
    int g_presents_after = 0;
    int g_record_presents = 0;
    bool g_allow_state_changes = false;   // F10 = false (copies only what needs no barrier), Shift+F10 = true
    constexpr int kRecordPresents = 16;  // presents include DLSS-G generated frames: ~4 real frames at 4x
    std::filesystem::path g_dir;
    bool g_key_down = false;
    bool g_f11_down = false;
    bool g_f7_down = false;
    constexpr int kPresentsBeforeHooks = 120;
    std::atomic<int> g_presents { 0 };
    std::atomic<bool> g_writer_busy { false };
    std::atomic<int> g_ui_request { 0 };   // capture requested from the overlay (rr::request_capture)
    bool g_sl_hooks_done = false;
    std::atomic<bool> g_marker_hooks_done { false };

    // Last native command list touched by this thread. Streamline receives the ReShade proxy
    // pointer, not the native one, so its calls are attributed to the list recorded on the same thread.
    thread_local void *t_last_list = nullptr;

    bool recording() { return g_state.load(std::memory_order_relaxed) == int(state::recording); }

    std::string indent(int depth) { return std::string(size_t(depth) * 2, ' '); }

    void flush_counts(list_log &l)
    {
        if (!(l.draws | l.dispatches | l.rays | l.indirect))
            return;
        std::string s = indent(l.depth) + "  [";
        if (l.draws) s += " draws=" + std::to_string(l.draws);
        if (l.dispatches) s += " dispatch=" + std::to_string(l.dispatches);
        if (l.rays) s += " DispatchRays=" + std::to_string(l.rays);
        if (l.indirect) s += " indirect=" + std::to_string(l.indirect);
        s += " ]";
        l.lines.push_back(std::move(s));
        l.draws = l.dispatches = l.rays = l.indirect = 0;
    }

    void push_line(list_log &l, std::string line)
    {
        flush_counts(l);
        if (line.rfind("END", 0) == 0)
        {
            l.depth = l.depth > 0 ? l.depth - 1 : 0;
            if (!l.markers.empty())
                l.markers.pop_back();
        }
        l.lines.push_back(indent(l.depth) + line);
        if (line.rfind("BEGIN ", 0) == 0)
        {
            l.depth++;
            l.markers.push_back(line.substr(6));
        }
    }

    // Ctrl+Shift+F10: copies limited to this pass (the RT effects, reflections included).
    const char *const kFocusMarker = "RenderRTBufferEffects";
    bool g_focus = false;
    std::unordered_set<uint64_t> g_focus_pipelines;   // pipelines bound inside the focus pass
    bool in_focus(const list_log &l)
    {
        if (!g_focus)
            return true;
        for (const std::string &m : l.markers)
            if (m.find(kFocusMarker) != std::string::npos)
                return true;
        return false;
    }

    std::string usage_name(resource_usage usage)
    {
        const uint32_t v = uint32_t(usage);
        if (v == 0) return "common";
        if (v & 0x80000000u) return (v == uint32_t(resource_usage::present)) ? "present" : "general";
        std::string s;
        auto add = [&](uint32_t bit, const char *name) { if (v & bit) { if (!s.empty()) s += '|'; s += name; } };
        add(0x4, "RT"); add(0x8, "UAV"); add(0x10, "DSV_W"); add(0x20, "DSV_R");
        add(0x40, "SRV_NPS"); add(0x80, "SRV_PS"); add(0x400, "COPY_DST"); add(0x800, "COPY_SRC");
        add(0x1000, "RESOLVE_DST"); add(0x2000, "RESOLVE_SRC"); add(0x200, "INDIRECT");
        add(0x1, "VB"); add(0x2, "IB"); add(0x8000, "CB"); add(0x400000, "AS");
        return s.empty() ? rr::hex(v) : s;
    }

    // Only the descriptor handle is printed. ReShade's view-to-resource table keeps the resource
    // pointer from view creation time; after the game frees that resource the pointer can belong to
    // another object, and any call through it runs an unrelated method (this corrupted the game).
    std::string describe_view(resource_view view)
    {
        return view.handle == 0 ? std::string("-") : "rtv" + rr::hex(view.handle);
    }

    bool is_planar_depth(DXGI_FORMAT f)
    {
        return f == DXGI_FORMAT_R24G8_TYPELESS || f == DXGI_FORMAT_D24_UNORM_S8_UINT || f == DXGI_FORMAT_R32G8X24_TYPELESS ||
               f == DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    }

    // Creates a readback buffer and records barrier_in, a copy of subresource 0, and barrier_out on
    // the game's command list. Called with g_mutex held, right after the game's own barrier.
    template <typename In, typename Out>
    void record_copy(ID3D12GraphicsCommandList *list, ID3D12Resource *res, const D3D12_RESOURCE_DESC &desc, const std::string &label,
                     std::string &line, In barrier_in, Out barrier_out)
    {
        if (desc.Width < kMinDim || desc.Height < kMinDim || desc.Width > kMaxDim || desc.Height > kMaxDim)
        {
            line += "  (no copy: size)";
            return;
        }
        int &count = g_per_resource[res];
        if (count >= kMaxPerResource)
            return;

        ID3D12Device *dev = nullptr;
        if (FAILED(res->GetDevice(IID_PPV_ARGS(&dev))))
            return;
        pending_copy copy;
        UINT64 total = 0;
        dev->GetCopyableFootprints(&desc, 0, 1, 0, &copy.footprint, &copy.rows, &copy.row_size, &total);
        if (total == 0 || total == UINT64_MAX || int(g_copies.size()) >= kMaxCaptures || g_bytes + total > kMaxBytes)
        {
            g_skipped++;
            line += "  (no copy: budget)";
            dev->Release();
            return;
        }
        D3D12_HEAP_PROPERTIES heap {};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC buf {};
        buf.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buf.Width = total;
        buf.Height = 1;
        buf.DepthOrArraySize = 1;
        buf.MipLevels = 1;
        buf.SampleDesc.Count = 1;
        buf.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        const HRESULT hr = dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buf, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&copy.readback));
        dev->Release();
        if (FAILED(hr))
        {
            line += "  (no copy: CreateCommittedResource " + rr::hex(uint32_t(hr)) + ")";
            return;
        }

        barrier_in();
        D3D12_TEXTURE_COPY_LOCATION dst {};
        dst.pResource = copy.readback;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = copy.footprint;
        D3D12_TEXTURE_COPY_LOCATION src {};
        src.pResource = res;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        barrier_out();

        count++;
        copy.index = int(g_copies.size());
        copy.list = list;
        copy.width = uint32_t(desc.Width);
        copy.height = desc.Height;
        copy.label = label;
        g_bytes += total;
        line += "  => #" + std::to_string(copy.index);
        g_copies.push_back(std::move(copy));
    }

    bool texture_ok(const D3D12_RESOURCE_DESC &desc)
    {
        return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.SampleDesc.Count == 1 && desc.MipLevels == 1 && desc.DepthOrArraySize == 1;
    }

    std::string state_name(UINT s)
    {
        return usage_name(resource_usage(s));
    }

    std::string access_name(D3D12_BARRIER_ACCESS a)
    {
        if (a == D3D12_BARRIER_ACCESS_NO_ACCESS) return "NO_ACCESS";
        if (a == D3D12_BARRIER_ACCESS_COMMON) return "COMMON";
        std::string s;
        auto add = [&](UINT bit, const char *n) { if (UINT(a) & bit) { if (!s.empty()) s += '|'; s += n; } };
        add(D3D12_BARRIER_ACCESS_RENDER_TARGET, "RT"); add(D3D12_BARRIER_ACCESS_UNORDERED_ACCESS, "UAV");
        add(D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE, "DSV_W"); add(D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ, "DSV_R");
        add(D3D12_BARRIER_ACCESS_SHADER_RESOURCE, "SRV"); add(D3D12_BARRIER_ACCESS_COPY_DEST, "COPY_DST");
        add(D3D12_BARRIER_ACCESS_COPY_SOURCE, "COPY_SRC"); add(D3D12_BARRIER_ACCESS_RESOLVE_DEST, "RESOLVE_DST");
        add(D3D12_BARRIER_ACCESS_RESOLVE_SOURCE, "RESOLVE_SRC");
        return s.empty() ? rr::hex(UINT(a)) : s;
    }

    // Read-only layouts that can go to COPY_SOURCE and back on this queue type.
    bool read_layout(D3D12_BARRIER_LAYOUT l, D3D12_COMMAND_LIST_TYPE type)
    {
        switch (l)
        {
        case D3D12_BARRIER_LAYOUT_SHADER_RESOURCE:
        case D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_READ:
            return true;
        case D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE:
            return type == D3D12_COMMAND_LIST_TYPE_DIRECT;
        case D3D12_BARRIER_LAYOUT_COMPUTE_QUEUE_SHADER_RESOURCE:
            return type == D3D12_COMMAND_LIST_TYPE_COMPUTE;
        default:
            return false;
        }
    }

    // Layouts that already allow copy reads, so a capture never changes the texture layout.
    bool copy_readable_layout(D3D12_BARRIER_LAYOUT l, D3D12_COMMAND_LIST_TYPE type)
    {
        switch (l)
        {
        case D3D12_BARRIER_LAYOUT_COMMON:
        case D3D12_BARRIER_LAYOUT_GENERIC_READ:
        case D3D12_BARRIER_LAYOUT_COPY_SOURCE:
            return true;
        case D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_COMMON:
        case D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_GENERIC_READ:
            return type == D3D12_COMMAND_LIST_TYPE_DIRECT;
        case D3D12_BARRIER_LAYOUT_COMPUTE_QUEUE_COMMON:
        case D3D12_BARRIER_LAYOUT_COMPUTE_QUEUE_GENERIC_READ:
            return type == D3D12_COMMAND_LIST_TYPE_COMPUTE;
        default:
            return false;
        }
    }

    void writer_thread(std::filesystem::path dir, std::vector<std::string> frame, std::vector<captured_image> images, int skipped, int unsubmitted)
    {
        const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        std::string table;
        for (const captured_image &img : images)
        {
            char base[64];
            snprintf(base, sizeof(base), "%04d", img.index);
            const std::wstring path = (dir / base).wstring();
            const rr::image::analysis info = rr::image::analyze(img.format, img.width, img.height, img.data.data(), img.row_size);
            rr::image::write_dds(path + L".dds", img.format, img.width, img.height, img.data.data(), img.row_size);
            if (info.supported)
                rr::image::write_previews(path, img.format, img.width, img.height, img.data.data(), img.row_size, info);

            table += "#" + std::to_string(img.index) + "  " + img.label + "\n";
            if (!info.supported)
            {
                table += "    (format not decoded)\n";
                continue;
            }
            if (!info.decode_note.empty())
                table += "    decode: " + info.decode_note + "\n";
            static const char ch[4] = { 'R', 'G', 'B', 'A' };
            for (int c = 0; c < info.channels; ++c)
            {
                char buf[160];
                snprintf(buf, sizeof(buf), "    %c min=%.5g max=%.5g mean=%.5g%s\n", ch[c], info.stats[c].min, info.stats[c].max, info.stats[c].mean,
                         info.stats[c].non_finite ? (" non-finite=" + std::to_string(info.stats[c].non_finite)).c_str() : "");
                table += buf;
            }
            if (info.channels >= 3)
            {
                char buf[128];
                snprintf(buf, sizeof(buf), "    |xyz| raw=%.4f  |xyz*2-1|=%.4f\n", info.mean_length_raw, info.mean_length_unorm);
                table += buf;
            }
        }

        std::string text = "rr-mapper frame map\n";
        text += std::string("mode: ") + (g_focus ? "Ctrl+Shift+F10 (state changes, only inside RenderRTBufferEffects)" : g_allow_state_changes ? "Shift+F10 (state changes allowed)" : "F10 (no state changes)") + "\n";
        text += "captures: " + std::to_string(images.size()) + ", skipped (budget): " + std::to_string(skipped) +
                ", recorded but never submitted: " + std::to_string(unsubmitted) + "\n\n== COMMAND STREAM (submission order) ==\n";
        for (const std::string &l : frame)
            text += l + "\n";
        text += "\n== CAPTURED TEXTURES ==\n" + table;

        if (FILE *f = _wfopen((dir / L"frame-map.txt").c_str(), L"wb"))
        {
            fwrite(text.data(), 1, text.size(), f);
            fclose(f);
        }
        if (SUCCEEDED(co))
            CoUninitialize();
        rr::log_info("capture written to " + dir.string() + " (" + std::to_string(images.size()) + " textures)");
        g_writer_busy = false;
    }

    // Reads back every submitted copy and hands the data to the writer thread.
    void finish_capture(command_queue *queue)
    {
        queue->wait_idle();

        std::vector<captured_image> images;
        int unsubmitted = 0;
        for (pending_copy &c : g_copies)
        {
            if (!c.submitted)
            {
                unsubmitted++;
                c.readback->Release();
                continue;
            }
            captured_image img;
            img.index = c.index;
            img.label = c.label;
            img.format = c.footprint.Footprint.Format;
            img.width = c.footprint.Footprint.Width;
            img.height = c.footprint.Footprint.Height;
            img.row_size = c.row_size;
            void *mapped = nullptr;
            // The last row has no pitch padding: the buffer ends at Offset + RowPitch * (rows - 1) + row_size.
            // (RowPitch * rows overran it whenever the width was not a multiple of 256 bytes: E_INVALIDARG.)
            const D3D12_RANGE range { 0, SIZE_T(c.footprint.Offset + uint64_t(c.footprint.Footprint.RowPitch) * (c.rows - 1) + c.row_size) };
            const HRESULT map_hr = c.readback->Map(0, &range, &mapped);
            if (SUCCEEDED(map_hr))
            {
                img.data.resize(size_t(c.row_size) * c.rows);
                const uint8_t *src = static_cast<const uint8_t *>(mapped) + c.footprint.Offset;
                for (UINT y = 0; y < c.rows; ++y)
                    memcpy(img.data.data() + size_t(y) * c.row_size, src + size_t(y) * c.footprint.Footprint.RowPitch, size_t(c.row_size));
                const D3D12_RANGE none { 0, 0 };
                c.readback->Unmap(0, &none);
                images.push_back(std::move(img));
            }
            else
            {
                g_frame.push_back("(readback #" + std::to_string(c.index) + " could not be mapped: " + rr::hex(uint32_t(map_hr)) + ")");
            }
            c.readback->Release();
        }

        // Lists recorded inside the window but never executed are still worth seeing.
        for (auto &[list, log] : g_lists)
        {
            flush_counts(log);
            if (log.lines.empty())
                continue;
            g_frame.push_back("== (list " + rr::hex(uint64_t(list)) + " recorded, not executed inside the window)");
            for (std::string &l : log.lines)
                g_frame.push_back(std::move(l));
        }

        g_writer_busy = true;
        if (!g_focus_pipelines.empty())
        {
            const int n = rr::dump_shaders(g_dir, std::vector<uint64_t>(g_focus_pipelines.begin(), g_focus_pipelines.end()));
            g_frame.push_back("(shaders dumped: " + std::to_string(n) + " of " + std::to_string(g_focus_pipelines.size()) + " pipelines in focus)");
            g_focus_pipelines.clear();
        }
        std::thread(writer_thread, g_dir, std::move(g_frame), std::move(images), g_skipped, unsubmitted).detach();

        g_frame.clear();
        g_copies.clear();
        g_lists.clear();
        g_per_resource.clear();
        g_bytes = 0;
        g_skipped = 0;
    }

    void begin_capture()
    {
        time_t now = time(nullptr);
        tm local {};
        localtime_s(&local, &now);
        wchar_t name[32];
        wcsftime(name, 32, L"%Y%m%d-%H%M%S", &local);
        g_dir = rr::capture_root() / name;
        std::error_code ec;
        std::filesystem::create_directories(g_dir, ec);
        if (ec)
        {
            rr::log_warn("cannot create " + g_dir.string() + ": " + ec.message());
            return;
        }
        g_lists.clear();
        g_frame.clear();
        g_copies.clear();
        g_per_resource.clear();
        g_bytes = 0;
        g_record_presents = 0;
        g_skipped = 0;
        g_state = int(state::recording);
        rr::log_info("capture started -> " + g_dir.string() + (g_allow_state_changes ? " (Shift+F10: state changes allowed)" : " (F10: no state changes)"));
    }

    // ---- ReShade events ----

    // Command lists created before the add-on loaded never raise init_command_list, so the
    // marker hooks are installed from the first command list seen in any event.
    void ensure_marker_hooks(command_list *cmd)
    {
        if (!rr::hooks_allowed())
            return;
        if (g_marker_hooks_done.load(std::memory_order_relaxed))
            return;
        auto *native = reinterpret_cast<ID3D12GraphicsCommandList *>(cmd->get_native());
        if (native == nullptr || native->GetType() == D3D12_COMMAND_LIST_TYPE_COPY)
            return;
        if (!g_marker_hooks_done.exchange(true))
            rr::install_marker_hooks(native);
    }

    void count(command_list *cmd, int kind)
    {
        ensure_marker_hooks(cmd);
        t_last_list = reinterpret_cast<void *>(cmd->get_native());
        if (!recording())
            return;
        std::lock_guard lock(g_mutex);
        if (!recording())
            return;
        list_log &l = g_lists[reinterpret_cast<void *>(cmd->get_native())];
        switch (kind)
        {
        case 0: l.draws++; break;
        case 1: l.dispatches++; break;
        case 2: l.rays++; break;
        default: l.indirect++; break;
        }
    }

    bool on_draw(command_list *cmd, uint32_t, uint32_t, uint32_t, uint32_t) { count(cmd, 0); return false; }
    bool on_draw_indexed(command_list *cmd, uint32_t, uint32_t, uint32_t, int32_t, uint32_t) { count(cmd, 0); return false; }
    bool on_dispatch(command_list *cmd, uint32_t, uint32_t, uint32_t) { count(cmd, 1); return false; }
    bool on_dispatch_rays(command_list *cmd, resource, uint64_t, uint64_t, resource, uint64_t, uint64_t, uint64_t, resource, uint64_t, uint64_t, uint64_t,
                          resource, uint64_t, uint64_t, uint64_t, uint32_t, uint32_t, uint32_t) { count(cmd, 2); return false; }
    bool on_indirect(command_list *cmd, indirect_command, resource, uint64_t, uint32_t, uint32_t) { count(cmd, 3); return false; }

    void on_bind_render_targets(command_list *cmd, uint32_t count_rtv, const resource_view *rtvs, resource_view dsv)
    {
        t_last_list = reinterpret_cast<void *>(cmd->get_native());
        if (!recording())
            return;
        std::string line = "OMSetRenderTargets";
        for (uint32_t i = 0; i < count_rtv; ++i)
            line += "  RT" + std::to_string(i) + "=" + describe_view(rtvs[i]);
        line += "  DSV=" + describe_view(dsv);
        rr::list_event(reinterpret_cast<void *>(cmd->get_native()), std::move(line));
    }

    bool on_begin_render_pass(command_list *cmd, uint32_t count_rt, const render_pass_render_target_desc *rts, const render_pass_depth_stencil_desc *ds, render_pass_flags)
    {
        if (!recording())
            return false;
        std::string line = "BeginRenderPass";
        for (uint32_t i = 0; i < count_rt; ++i)
            line += "  RT" + std::to_string(i) + "=" + describe_view(rts[i].view);
        line += "  DSV=" + (ds ? describe_view(ds->view) : std::string("-"));
        rr::list_event(reinterpret_cast<void *>(cmd->get_native()), std::move(line));
        return false;
    }

    void on_bind_pipeline(command_list *cmd, pipeline_stage stages, pipeline pso)
    {
        t_last_list = reinterpret_cast<void *>(cmd->get_native());
        if (!recording() || pso.handle == 0)
            return;
        auto *object = reinterpret_cast<ID3D12Object *>(pso.handle);
        std::string name = rr::debug_name(object);
        const std::string shader = rr::shader_label(pso.handle, nullptr);
        std::string line = "PSO " + rr::hex(pso.handle) + (shader.empty() ? "" : " [" + shader + "]") + (name.empty() ? "" : " \"" + name + "\"");
        std::lock_guard lock(g_mutex);
        if (!recording())
            return;
        list_log &l = g_lists[reinterpret_cast<void *>(cmd->get_native())];
        if (g_focus && in_focus(l))
            g_focus_pipelines.insert(pso.handle);
        push_line(l, std::move(line));
    }

    void on_execute(command_queue *queue, command_list *cmd)
    {
        rr::bench_on_execute(reinterpret_cast<ID3D12CommandQueue *>(queue->get_native()), reinterpret_cast<void *>(cmd->get_native()));
        const int s = g_state.load();
        if (s == int(state::idle))
            return;
        std::lock_guard lock(g_mutex);
        void *native = reinterpret_cast<void *>(cmd->get_native());
        for (pending_copy &c : g_copies)
            if (c.list == native)
                c.submitted = true;
        auto it = g_lists.find(native);
        if (it == g_lists.end())
            return;
        flush_counts(it->second);
        g_frame.push_back("== ExecuteCommandLists queue=" + rr::hex(queue->get_native()) + " list=" + rr::hex(uint64_t(native)));
        for (std::string &line : it->second.lines)
            g_frame.push_back(std::move(line));
        g_lists.erase(it);
    }

    void on_present(command_queue *queue, swapchain *, const rect *, const rect *, uint32_t, const rect *)
    {
        // ReShade loads a temporary instance of the add-ons at start and unloads it ~20 s later;
        // that one never reaches 120 presents, so it never installs hot hooks.
        if (g_presents < kPresentsBeforeHooks)
            g_presents++;
        if (!g_sl_hooks_done && rr::hooks_allowed())
            g_sl_hooks_done = rr::install_streamline_hooks();
        rr::bench_on_present(reinterpret_cast<ID3D12CommandQueue *>(queue->get_native()));

        // F7 and F10 are developer hotkeys (off by default: F10 alone writes up to 1 GB); the overlay
        // buttons work either way. F11 (RR on/off) is always active.
        const bool dev_keys = rr::cfg::dev_hotkeys.load(std::memory_order_relaxed);
        const bool f7 = dev_keys && (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
        if (f7 && !g_f7_down)
        {
            rr::guides_metal_toggle();
            rr::notify(rr::notice::metal_toggle);
        }
        g_f7_down = f7;
        const bool f11 = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
        if (f11 && !g_f11_down)
        {
            rr::rr_toggle();
            rr::notify(rr::notice::rr_toggle);
        }
        g_f11_down = f11;
        const bool key = dev_keys && (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
        const int ui_request = g_ui_request.exchange(0);
        const bool pressed = (key && !g_key_down) || ui_request != 0;
        g_key_down = key;

        // Read before taking g_mutex: guides code holds its own mutex while calling into the capture
        // (lock order guides -> capture), so the capture must never wait on the guides mutex.
        const std::string guide_status = pressed ? rr::guides_status() + "\n" + rr::rr_status() : std::string();

        std::lock_guard lock(g_mutex);
        switch (state(g_state.load()))
        {
        case state::idle:
            if (pressed)
            {
                if (g_writer_busy)
                    rr::log_warn("previous capture is still being written; F10 ignored");
                else
                {
                    if (ui_request != 0)
                    {
                        g_allow_state_changes = ui_request >= 2;
                        g_focus = ui_request == 3;
                    }
                    else
                    {
                        g_allow_state_changes = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
                        g_focus = g_allow_state_changes && (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
                    }
                    begin_capture();
                    if (ui_request == 0)
                        rr::notify(rr::notice::capture);
                    g_frame.push_back(guide_status);
                }
            }
            break;
        case state::recording:
            g_frame.push_back("== Present");
            if (++g_record_presents >= kRecordPresents)
            {
                g_state = int(state::draining);
                g_presents_after = 0;
            }
            break;
        case state::draining:
            if (++g_presents_after >= kDrainPresents)
            {
                finish_capture(queue);
                g_state = int(state::idle);
            }
            break;
        }
    }
}

namespace rr
{
    bool hooks_allowed() { return g_presents.load() >= kPresentsBeforeHooks; }
}

namespace rr
{
    bool capture_active() { return recording(); }

    void *current_list() { return t_last_list; }

    void list_event(void *native_list, std::string line)
    {
        if (!recording())
            return;
        std::lock_guard lock(g_mutex);
        if (!recording())
            return;
        push_line(g_lists[native_list], std::move(line));
    }

    void frame_event(std::string line)
    {
        if (!recording())
            return;
        std::lock_guard lock(g_mutex);
        g_frame.push_back(std::move(line));
    }

    void on_native_resource_barrier(ID3D12GraphicsCommandList *list, UINT count_barriers, const D3D12_RESOURCE_BARRIER *barriers)
    {
        t_last_list = list;
        if (!recording())
            return;
        constexpr UINT write_states = D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_UNORDERED_ACCESS | D3D12_RESOURCE_STATE_DEPTH_WRITE;
        std::lock_guard lock(g_mutex);
        if (!recording())
            return;
        list_log &l = g_lists[list];
        const D3D12_COMMAND_LIST_TYPE type = list->GetType();
        for (UINT i = 0; i < count_barriers; ++i)
        {
            const D3D12_RESOURCE_BARRIER &b = barriers[i];
            if (b.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION || b.Transition.pResource == nullptr)
                continue;
            const UINT from = b.Transition.StateBefore, to = b.Transition.StateAfter;
            if (!(from & write_states) || (to & write_states & from) == (from & write_states))
                continue;   // only transitions that leave a write state
            ID3D12Resource *res = b.Transition.pResource;
            const D3D12_RESOURCE_DESC desc = res->GetDesc();
            if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D)
                continue;
            std::string line = "ResourceBarrier " + rr::describe_resource(res) + " " + state_name(from) + "->" + state_name(to);
            if (b.Flags & D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY) line += " [BEGIN_ONLY]";
            if (b.Flags & D3D12_RESOURCE_BARRIER_FLAG_END_ONLY) line += " [END_ONLY]";
            if (b.Transition.Subresource != D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) line += " sub=" + std::to_string(b.Transition.Subresource);

            if (b.Flags & D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY)
                line += "  (no copy: split barrier still in flight)";
            else if (!texture_ok(desc) || (b.Transition.Subresource != D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES && b.Transition.Subresource != 0))
                line += "  (no copy: subresources)";
            else if (type == D3D12_COMMAND_LIST_TYPE_COPY)
                line += "  (no copy: copy queue)";
            else if (!in_focus(l))
                line += "  (no copy: outside focus)";
            else
            {
                const bool transition = !(to & D3D12_RESOURCE_STATE_COPY_SOURCE);
                if (transition && !g_allow_state_changes)
                {
                    line += "  (no copy: needs a state change; use Shift+F10)";
                    push_line(l, std::move(line));
                    continue;
                }
                D3D12_RESOURCE_BARRIER in {}, out {};
                in.Type = out.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                in.Transition.pResource = out.Transition.pResource = res;
                in.Transition.Subresource = out.Transition.Subresource = 0;
                in.Transition.StateBefore = D3D12_RESOURCE_STATES(to);
                in.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                out.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
                out.Transition.StateAfter = D3D12_RESOURCE_STATES(to);
                const std::string label = rr::describe_resource(res) + " " + state_name(from) + "->" + state_name(to) +
                                          (is_planar_depth(desc.Format) ? " (depth plane)" : "");
                record_copy(list, res, desc, label, line,
                    [&] { if (transition) rr::raw_resource_barrier(list, 1, &in); },
                    [&] { if (transition) rr::raw_resource_barrier(list, 1, &out); });
            }
            push_line(l, std::move(line));
        }
    }

    void on_native_barrier(ID3D12GraphicsCommandList7 *list7, UINT32 count_groups, const D3D12_BARRIER_GROUP *groups)
    {
        auto *list = static_cast<ID3D12GraphicsCommandList *>(list7);
        t_last_list = list;
        if (!recording())
            return;
        constexpr UINT write_access = D3D12_BARRIER_ACCESS_RENDER_TARGET | D3D12_BARRIER_ACCESS_UNORDERED_ACCESS | D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE;
        std::lock_guard lock(g_mutex);
        if (!recording())
            return;
        list_log &l = g_lists[list];
        const D3D12_COMMAND_LIST_TYPE type = list->GetType();
        for (UINT32 g = 0; g < count_groups; ++g)
        {
            if (groups[g].Type != D3D12_BARRIER_TYPE_TEXTURE)
                continue;
            for (UINT32 i = 0; i < groups[g].NumBarriers; ++i)
            {
                const D3D12_TEXTURE_BARRIER &tb = groups[g].pTextureBarriers[i];
                if (tb.pResource == nullptr || tb.AccessBefore == D3D12_BARRIER_ACCESS_NO_ACCESS)
                    continue;
                const UINT from = tb.AccessBefore, to = (tb.AccessAfter == D3D12_BARRIER_ACCESS_NO_ACCESS) ? 0u : UINT(tb.AccessAfter);
                if (!(from & write_access) || (to & write_access & from) == (from & write_access))
                    continue;
                ID3D12Resource *res = tb.pResource;
                const D3D12_RESOURCE_DESC desc = res->GetDesc();
                std::string line = "Barrier " + rr::describe_resource(res) + " access " + access_name(tb.AccessBefore) + "->" + access_name(tb.AccessAfter) +
                                   " layout " + std::to_string(int(tb.LayoutBefore)) + "->" + std::to_string(int(tb.LayoutAfter)) +
                                   " sync " + rr::hex(UINT(tb.SyncBefore)) + "->" + rr::hex(UINT(tb.SyncAfter));
                const D3D12_BARRIER_SUBRESOURCE_RANGE &r = tb.Subresources;
                const bool covers_zero = r.IndexOrFirstMipLevel == 0xffffffffu ||
                                         (r.NumMipLevels == 0 ? r.IndexOrFirstMipLevel == 0 : (r.IndexOrFirstMipLevel == 0 && r.FirstArraySlice == 0 && r.FirstPlane == 0));
                const bool single_slice = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.SampleDesc.Count == 1 && desc.DepthOrArraySize == 1;
                if (!single_slice || !covers_zero)
                    line += "  (no copy: subresources)";
                else if (!in_focus(l))
                    line += "  (no copy: outside focus)";
                else if (!g_allow_state_changes)
                    line += "  (no copy: needs a barrier; use Shift+F10)";
                else if (tb.LayoutAfter == D3D12_BARRIER_LAYOUT_UNDEFINED || !(copy_readable_layout(tb.LayoutAfter, type) || read_layout(tb.LayoutAfter, type)))
                    line += "  (no copy: layout " + std::to_string(int(tb.LayoutAfter)) + ")";
                else
                {
                    // Round trip from the layout the game just set (exact, from its own barrier) to
                    // COPY_SOURCE and back, only on subresource 0.
                    const bool no_access = tb.AccessAfter == D3D12_BARRIER_ACCESS_NO_ACCESS;
                    D3D12_TEXTURE_BARRIER in {}, out {};
                    in.pResource = out.pResource = res;
                    const bool keep = copy_readable_layout(tb.LayoutAfter, type);
                    in.LayoutBefore = out.LayoutAfter = tb.LayoutAfter;
                    in.LayoutAfter = out.LayoutBefore = keep ? tb.LayoutAfter : D3D12_BARRIER_LAYOUT_COPY_SOURCE;
                    in.Subresources = out.Subresources = { 0, 1, 0, 1, 0, 1 };
                    in.SyncBefore = no_access ? D3D12_BARRIER_SYNC_NONE : tb.SyncAfter;
                    in.AccessBefore = no_access ? D3D12_BARRIER_ACCESS_NO_ACCESS : tb.AccessAfter;
                    in.SyncAfter = D3D12_BARRIER_SYNC_COPY;
                    in.AccessAfter = D3D12_BARRIER_ACCESS_COPY_SOURCE;
                    out.SyncBefore = D3D12_BARRIER_SYNC_COPY;
                    out.AccessBefore = D3D12_BARRIER_ACCESS_COPY_SOURCE;
                    out.SyncAfter = no_access ? D3D12_BARRIER_SYNC_NONE : tb.SyncAfter;
                    out.AccessAfter = tb.AccessAfter;
                    D3D12_BARRIER_GROUP gin {}, gout {};
                    gin.Type = gout.Type = D3D12_BARRIER_TYPE_TEXTURE;
                    gin.NumBarriers = gout.NumBarriers = 1;
                    gin.pTextureBarriers = &in;
                    gout.pTextureBarriers = &out;
                    const std::string label = rr::describe_resource(res) + " access " + access_name(tb.AccessBefore) + "->" + access_name(tb.AccessAfter) +
                                              " layout " + std::to_string(int(tb.LayoutAfter));
                    record_copy(list, res, desc, label, line,
                        [&] { rr::raw_barrier(list7, 1, &gin); },
                        [&] { rr::raw_barrier(list7, 1, &gout); });
                }
                push_line(l, std::move(line));
            }
        }
    }

    void capture_own_texture(ID3D12GraphicsCommandList *list, void *key_list, ID3D12Resource *res, D3D12_RESOURCE_STATES state, const char *label)
    {
        if (!recording())
            return;
        std::lock_guard lock(g_mutex);
        if (!recording())
            return;
        list_log &l = g_lists[key_list];
        const D3D12_RESOURCE_DESC desc = res->GetDesc();
        std::string line = std::string("OWN ") + label + " " + rr::describe_resource(res);
        D3D12_RESOURCE_BARRIER in {}, out {};
        in.Type = out.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        in.Transition.pResource = out.Transition.pResource = res;
        in.Transition.Subresource = out.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        in.Transition.StateBefore = out.Transition.StateAfter = state;
        in.Transition.StateAfter = out.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        record_copy(list, res, desc, std::string(label) + " " + rr::describe_resource(res), line,
            [&] { rr::raw_resource_barrier(list, 1, &in); },
            [&] { rr::raw_resource_barrier(list, 1, &out); });
        push_line(l, std::move(line));
    }

    void request_capture(int mode)
    {
        if (mode >= 1 && mode <= 3)
            g_ui_request = mode;
    }

    std::string capture_state_text()
    {
        std::lock_guard lock(g_mutex);
        const std::string dir = g_dir.empty() ? std::string(rr::tr("(none this session)", "(ninguna en esta sesión)")) : g_dir.string();
        switch (state(g_state.load()))
        {
        case state::recording:
            return rr::tr("recording frames... -> ", "grabando frames... -> ") + dir;
        case state::draining:
            return rr::tr("copying textures from the GPU... -> ", "copiando texturas de la GPU... -> ") + dir;
        default:
            return (g_writer_busy ? rr::tr("writing files -> ", "escribiendo archivos -> ") : rr::tr("ready. Last: ", "lista. Última: ")) + dir;
        }
    }

    void register_capture_events()
    {
        reshade::register_event<reshade::addon_event::draw>(on_draw);
        reshade::register_event<reshade::addon_event::draw_indexed>(on_draw_indexed);
        reshade::register_event<reshade::addon_event::dispatch>(on_dispatch);
        reshade::register_event<reshade::addon_event::dispatch_rays>(on_dispatch_rays);
        reshade::register_event<reshade::addon_event::draw_or_dispatch_indirect>(on_indirect);
        reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(on_bind_render_targets);
        reshade::register_event<reshade::addon_event::begin_render_pass>(on_begin_render_pass);
        reshade::register_event<reshade::addon_event::bind_pipeline>(on_bind_pipeline);
        reshade::register_event<reshade::addon_event::execute_command_list>(on_execute);
        reshade::register_event<reshade::addon_event::present>(on_present);
    }
}
