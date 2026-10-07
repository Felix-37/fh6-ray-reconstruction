// Shader identification for phase 5: remembers the compute and pixel shader bytecode of every
// pipeline the game creates (ReShade init_pipeline event), keyed by CRC32 of the bytecode - the same
// hash RenoDX uses for shader replacement (renodx::utils::hash::ComputeCRC32). Captures print the
// hash of every bound pipeline and dump the bytecode of those used inside the focus pass, to be
// disassembled offline (dxc -dumpbin). Read-only: nothing is replaced here.

#include "common.hpp"

#include <reshade.hpp>

#include <filesystem>
#include <mutex>
#include <unordered_map>

using namespace reshade::api;

namespace rr
{
    void replacement_on_init(reshade::api::device *dev, reshade::api::pipeline_layout layout, uint32_t crc, reshade::api::pipeline pso);
    void replacement_on_destroy(reshade::api::pipeline pso);
}

namespace
{
    struct shader_info
    {
        uint32_t crc = 0;
        bool compute = false;
    };

    // Thousands of pipelines share the same shaders: bytecode is stored once per CRC.
    constexpr size_t kMaxStoredBytes = 512ull << 20;

    std::mutex g_mutex;
    std::unordered_map<uint64_t, shader_info> g_pipelines;
    std::unordered_map<uint32_t, std::vector<uint8_t>> g_code;
    size_t g_stored_bytes = 0;

    uint32_t crc32(const uint8_t *data, size_t size)
    {
        static uint32_t table[256];
        static bool ready = false;
        if (!ready)
        {
            for (uint32_t i = 0; i < 256; ++i)
            {
                uint32_t c = i;
                for (int k = 0; k < 8; ++k)
                    c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                table[i] = c;
            }
            ready = true;
        }
        uint32_t crc = 0xFFFFFFFFu;
        for (size_t i = 0; i < size; ++i)
            crc = (crc >> 8) ^ table[(crc ^ data[i]) & 0xFF];
        return ~crc;
    }

    void on_init_pipeline(device *dev, pipeline_layout layout, uint32_t count, const pipeline_subobject *subobjects, pipeline pso)
    {
        for (uint32_t i = 0; i < count; ++i)
        {
            const pipeline_subobject &so = subobjects[i];
            const bool cs = so.type == pipeline_subobject_type::compute_shader;
            const bool ps = so.type == pipeline_subobject_type::pixel_shader;
            if ((!cs && !ps) || so.data == nullptr || so.count == 0)
                continue;
            const auto *desc = static_cast<const shader_desc *>(so.data);
            if (desc->code == nullptr || desc->code_size == 0)
                continue;
            shader_info info;
            info.compute = cs;
            info.crc = crc32(static_cast<const uint8_t *>(desc->code), desc->code_size);
            if (cs)
                rr::replacement_on_init(dev, layout, info.crc, pso);   // outside g_mutex: creates pipelines
            std::lock_guard lock(g_mutex);
            if (g_code.find(info.crc) == g_code.end() && g_stored_bytes + desc->code_size <= kMaxStoredBytes)
            {
                g_code[info.crc].assign(static_cast<const uint8_t *>(desc->code), static_cast<const uint8_t *>(desc->code) + desc->code_size);
                g_stored_bytes += desc->code_size;
            }
            // A graphics pipeline has one pixel shader; a compute pipeline one compute shader.
            g_pipelines[pso.handle] = std::move(info);
            return;
        }
    }

    void on_destroy_pipeline(device *, pipeline pso)
    {
        rr::replacement_on_destroy(pso);
        std::lock_guard lock(g_mutex);
        auto it = g_pipelines.find(pso.handle);
        if (it == g_pipelines.end())
            return;
        g_pipelines.erase(it);   // bytecode stays: other pipelines may share it
    }
}

namespace rr
{
    void register_shader_events()
    {
        reshade::register_event<reshade::addon_event::init_pipeline>(on_init_pipeline);
        reshade::register_event<reshade::addon_event::destroy_pipeline>(on_destroy_pipeline);
    }

    // "cs 0x1234ABCD" / "ps 0x..." or empty when the pipeline was created before the add-on loaded.
    std::string shader_label(uint64_t pipeline_handle, uint32_t *crc_out)
    {
        std::lock_guard lock(g_mutex);
        auto it = g_pipelines.find(pipeline_handle);
        if (it == g_pipelines.end())
            return {};
        if (crc_out)
            *crc_out = it->second.crc;
        char b[32];
        snprintf(b, sizeof(b), "%s 0x%08X", it->second.compute ? "cs" : "ps", it->second.crc);
        return b;
    }

    // The game shaders the build patches (CMakeLists.txt, scripts/patch_rtgi*.pl). They are not in the
    // repository: every contributor exports them from their own copy of the game, from the bytecode the
    // game handed to CreateComputePipelineState (kept by on_init_pipeline above).
    std::string export_game_shaders()
    {
        static const struct { uint32_t crc; const char *name; } kWanted[] = {
            { 0x4DAF8A48, "RTGI_TemporalFilter" }, { 0x209AB6A4, "RTGI_SpatialFilter_Disk" } };
        const std::filesystem::path dir = capture_root() / L"game-shaders";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec)
            return std::string(tr("cannot create ", "no se puede crear ")) + dir.string() + ": " + ec.message();
        int written = 0;
        std::string missing;
        for (const auto &w : kWanted)
        {
            std::vector<uint8_t> code;
            {
                std::lock_guard lock(g_mutex);
                auto it = g_code.find(w.crc);
                if (it != g_code.end())
                    code = it->second;
            }
            if (code.empty())
            {
                missing += std::string(missing.empty() ? "" : ", ") + w.name;
                continue;
            }
            char name[48];
            snprintf(name, sizeof(name), "shader_0x%08X.cs.cso", w.crc);
            if (FILE *f = _wfopen((dir / name).c_str(), L"wb"))
            {
                const bool ok = fwrite(code.data(), 1, code.size(), f) == code.size();
                fclose(f);
                written += ok;
            }
        }
        log_info("export_game_shaders: " + std::to_string(written) + "/2 written to " + dir.string() + (missing.empty() ? "" : ", not seen: " + missing));
        if (!missing.empty())
            return std::to_string(written) + "/2 " + tr("exported; the game has not created yet: ", "exportados; el juego a\xC3\xBAn no cre\xC3\xB3: ") + missing +
                   tr(" (drive for a few seconds with RT GI on and try again)", " (conduce unos segundos con el GI por RT activo y reintenta)");
        return std::to_string(written) + "/2 " + tr("exported to ", "exportados a ") + dir.string();
    }

    // Writes the bytecode of the given pipelines to dir/shader_<crc>.<cs|ps>.cso; returns files written.
    int dump_shaders(const std::filesystem::path &dir, const std::vector<uint64_t> &pipelines)
    {
        struct item { shader_info info; std::vector<uint8_t> code; };
        std::vector<item> copies;
        {
            std::lock_guard lock(g_mutex);
            for (uint64_t p : pipelines)
            {
                auto it = g_pipelines.find(p);
                if (it == g_pipelines.end())
                    continue;
                auto code = g_code.find(it->second.crc);
                if (code != g_code.end())
                    copies.push_back({ it->second, code->second });
            }
        }
        int written = 0;
        for (const item &s : copies)
        {
            char name[48];
            snprintf(name, sizeof(name), "shader_0x%08X.%s.cso", s.info.crc, s.info.compute ? "cs" : "ps");
            if (FILE *f = _wfopen((dir / name).c_str(), L"wb"))
            {
                fwrite(s.code.data(), 1, s.code.size(), f);
                fclose(f);
                written++;
            }
        }
        return written;
    }
}
