#pragma once

#include <windows.h>
#include <d3d12.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// Version shown in the overlay, the log and the add-on description (CMake passes the real value).
#ifndef RR_FORZA_VERSION
#define RR_FORZA_VERSION "0.1.0-beta"
#endif
#define RR_FORZA_REPO "https://github.com/Felix-37/fh6-ray-reconstruction"

namespace rr
{
    // Threads currently inside one of our detours (marker_hooks.cpp). Shutdown waits for zero.
    extern std::atomic<int> g_inflight;
    struct hook_guard
    {
        hook_guard() { g_inflight.fetch_add(1, std::memory_order_acq_rel); }
        ~hook_guard() { g_inflight.fetch_sub(1, std::memory_order_acq_rel); }
    };
    // Set by the final add-on instance once the game has presented enough real frames.
    bool hooks_allowed();

    // Logging through ReShade.log, prefixed by ReShade with the add-on name.
    void log_info(const std::string &message);
    void log_warn(const std::string &message);

    std::string hex(uint64_t value);
    std::string format_name(DXGI_FORMAT format);
    std::string debug_name(ID3D12Object *object);   // empty when the app never called SetName
    std::string describe_resource(ID3D12Resource *resource);

    // Capture window control (capture.cpp).
    bool capture_active();   // true while commands of the captured frame are being recorded
    // Appends one line to the log of a native command list; flushed in submission order.
    void list_event(void *native_list, std::string line);
    // Native command list last used by the calling thread (nullptr if none).
    void *current_list();
    // Appends one line to the frame log directly (events without a command list).
    void frame_event(std::string line);

    // Called from the native barrier hooks after the game's barrier was recorded.
    void on_native_resource_barrier(ID3D12GraphicsCommandList *list, UINT count, const D3D12_RESOURCE_BARRIER *barriers);
    void on_native_barrier(ID3D12GraphicsCommandList7 *list, UINT32 count, const D3D12_BARRIER_GROUP *groups);
    // Record barriers without re-entering our own hooks.
    void raw_resource_barrier(ID3D12GraphicsCommandList *list, UINT count, const D3D12_RESOURCE_BARRIER *barriers);
    void raw_barrier(ID3D12GraphicsCommandList7 *list, UINT32 count, const D3D12_BARRIER_GROUP *groups);

    // Copies an add-on owned texture (current legacy state given) during a capture; key_list is the
    // native list the copy is attributed to for submission tracking.
    void capture_own_texture(ID3D12GraphicsCommandList *list, void *key_list, ID3D12Resource *res, D3D12_RESOURCE_STATES state, const char *label);

    // Phase 3 guide buffers (guides.cpp).
    void guides_set_camera(const float right[3], const float up[3], const float fwd[3], float proj_x, float proj_y);
    void guides_on_barrier(ID3D12GraphicsCommandList7 *list, UINT32 count, const D3D12_BARRIER_GROUP *groups);
    void guides_before_evaluate(void *command_buffer, void *key_list);
    bool guides_outputs(ID3D12Resource **normal_roughness, ID3D12Resource **diffuse, ID3D12Resource **specular, uint32_t &width, uint32_t &height, uint64_t &dispatches, uint64_t &copies);
    ID3D12Resource *guides_hit_distance();                        // diffuse hit distance guide (R16F), nullptr until built
    void guides_on_upscale_pso(ID3D12GraphicsCommandList *list);  // the game set the RTGI upscale pipeline on `list`
    void set_render_extent(uint32_t width, uint32_t height);
    std::string guides_status();
    // Phase 4: Ray Reconstruction (sl_hooks.cpp).
    void guides_metal_toggle();   // F7
    void rr_toggle();             // F11
    std::string rr_status();

    // Live state for the overlay (overlay.cpp). Copies taken under each module's own lock.
    struct rr_snapshot
    {
        int state = 0;                  // 0 = not checked yet, 1 = available, -1 = unavailable
        bool enabled = false;
        bool evaluating = false;        // the last DLSS evaluation used RR
        uint64_t ok = 0, fail = 0, fallback = 0, resets = 0;
        std::string last_error, last_skip, last_result;
        int mode = 0;                   // sl::DLSSMode the game asked for
        uint32_t out_w = 0, out_h = 0, render_w = 0, render_h = 0;
        float pre_exposure = 1.0f;
        bool hdr = true, auto_exposure = false;
        int active_preset = 0;          // preset sent with the last slDLSSDSetOptions
        uint64_t last_rr_tick = 0;      // GetTickCount64 of the last RR evaluation, 0 = never
        float camera_near = 0.0f, camera_far = 0.0f;   // from the game's slSetConstants
        int exposure_ev_applied = 0;    // exposure EV tagged with the last RR evaluation, 0 = none
        std::string exposure_error;     // why the exposure tag could not be used, empty = fine
        uint64_t hit_tagged = 0;        // RR evaluations that carried the diffuse hit distance guide
        uint64_t smv_tagged = 0;        // RR evaluations that carried the specular motion vectors
    };
    rr_snapshot rr_get_snapshot();

    struct guides_snapshot
    {
        uint32_t width = 0, height = 0;
        uint64_t copies = 0, dispatches = 0;
        bool pipeline_ready = false, camera_valid = false;
        // Values the last guide dispatch really used (to confirm a menu change reached the GPU).
        uint32_t applied_flags = 0, applied_metal = 0;
        int applied_threshold = 0;
        uint64_t last_dispatch_tick = 0;   // GetTickCount64 of the last dispatch, 0 = never
        uint64_t gi_copies = 0, gi_source = 0, gi_source_frames = 0, hit_dispatches = 0;   // diffuse hit distance guide
    };
    guides_snapshot guides_get_snapshot();
    ID3D12Device *guides_native_device();   // nullptr until the guides ran once

    // RTGI filter replacement (replace.cpp). Variants: 0 hist2, 1 hist4, 2 nofilter, 3 cap2, 4 cap4
    // (temporal filter); 5 pass, 6 clamp k=3, 7 clamp k=2, 8 clamp k=1.25, 9 clamp to neighbour range (spatial filter);
    // 10-12 nofilter + isolated sample clamp; 13-17 one-frame history reset twins; 18-19 disocclusion assist; 20 hist4 + reset (motion hand-over).
    constexpr int kGiVariants = 21;
    struct gi_snapshot
    {
        bool temporal_found = false, spatial_found = false, hook_installed = false;
        int twins = 0, failures = 0;
        uint64_t swaps = 0;
        int want_temporal = -1, want_spatial = -1;   // variants the current settings select, -1 = original
        int variant_twins[kGiVariants] = {};         // live game pipelines with a twin of each variant
        uint64_t variant_swaps[kGiVariants] = {};
        uint64_t variant_tick[kGiVariants] = {};     // GetTickCount64 of the last swap to each variant
        std::string last_failure;
    };
    gi_snapshot gi_get_snapshot();
    const char *gi_variant_name(int variant);
    // False when the build had no game shaders (game-shaders/ empty): only the HLSL spatial variants exist.
    bool gi_variants_built();
    // "Medir desoclusión": the next temporal-filter dispatch drops every pixel's GI history (one frame).
    void gi_request_reset();
    void gi_cancel_reset();
    std::string gi_reset_status();   // "" while pending
    // "GI original en movimiento": camera motion per frame (from slSetConstants), and what it decided.
    void gi_motion_update(float move_m, float turn_deg, uint32_t frame);
    struct gi_motion_snapshot
    {
        bool moving = false;
        float move_m = 0.0f, turn_deg = 0.0f;   // last frame
        uint64_t switches = 0;                  // static -> moving transitions seen
        int still_frames = 0;
    };
    gi_motion_snapshot gi_motion_get();

    // Capture requests from the overlay: 1 = F10, 2 = Shift+F10, 3 = Ctrl+Shift+F10.
    void request_capture(int mode);

    // Optional sharpening of the RR output (sharpen.cpp).
    struct sharpen_snapshot
    {
        uint64_t last_tick = 0;   // GetTickCount64 of the last pass, 0 = never
        int last_strength = -1;
        std::string error;
    };
    void sharpen_after_rr();   // right after an RR evaluation, on the native list NGX used
    sharpen_snapshot sharpen_get_snapshot();

    // Specular motion vectors for RR (specmv.cpp).
    struct specmv_snapshot
    {
        uint64_t dispatches = 0, last_tick = 0;   // last_tick: GetTickCount64 of the last dispatch, 0 = never
        int last_distance = -1;                   // metres used by the last dispatch
        std::string error, skip;                  // skip: why the last frame was not computed, empty = fine
    };
    void specmv_set_tags(ID3D12Resource *depth, uint32_t depth_state, ID3D12Resource *mv, uint32_t mv_state, uint32_t mv_w, uint32_t mv_h);
    void specmv_set_constants(const float clip_to_view[16], const float view_to_clip[16], const float clip_to_prev[16], float mv_scale_x, float mv_scale_y);
    void specmv_dispatch(ID3D12GraphicsCommandList *list, ID3D12Resource *packed, uint32_t packed_w, uint32_t packed_h);
    ID3D12Resource *specmv_output(uint32_t &w, uint32_t &h);
    specmv_snapshot specmv_get_snapshot();
    ID3D12Resource *dlss_output_resource();   // last tagged ScalingOutputColor (bench.cpp)

    // RR vs DLSS measurement bench (bench.cpp, "Medición" tab).
    struct bench_snapshot
    {
        bool running = false;
        std::string state, error, summary, folder;
        int variants = 0, current = 0;
        float progress = 0.0f;
    };
    extern std::atomic<bool> bench_include_neutral;      // optional variant: RR with neutral guides
    extern std::atomic<bool> bench_include_gi_original;  // optional variant: RR with the game's GI filters
    extern std::atomic<bool> bench_include_exposure;     // optional variants: RR at -2 / -4 EV
    extern std::atomic<bool> bench_include_gi_modes;     // optional variants: RR with GI original / history x2 / x4
    extern std::atomic<bool> bench_include_sharpen;      // optional variants: RR with sharpening 30 / 60 / 90 %
    extern std::atomic<bool> bench_include_fireflies;    // optional variants: "RR completo" with firefly levels / pre-accumulation
    void bench_start(int focus);   // 0 general, 1 car reflections, 2 night dots of "RR completo", 3 convergence
    void bench_abort();
    bool bench_running();
    bench_snapshot bench_get_snapshot();
    void bench_set_tags(ID3D12Resource *output, uint32_t output_state, ID3D12Resource *depth, uint32_t depth_state);
    bool bench_active();   // cheap check for the marker hooks
    void bench_on_barrier(UINT32 count, const D3D12_BARRIER_GROUP *groups);   // layouts of the DLSS output / depth
    void bench_after_evaluate(bool rr_used);   // after slEvaluateFeature returned (all of Streamline's work recorded)
    ID3D12GraphicsCommandList *last_ngx_list();  // native list of the last NGX upscaler marker on this thread
    void bench_on_present(ID3D12CommandQueue *queue);
    void bench_on_execute(ID3D12CommandQueue *queue, void *native_list);   // a command list was submitted

    // On-screen confirmation of a hotkey (overlay.cpp), shown even with the ReShade overlay closed.
    enum class notice { rr_toggle, metal_toggle, capture };
    void notify(notice what);
    std::string capture_state_text();
}

// User settings, shown in the overlay and saved to ReShade.ini, section [RR_Forza] (settings.cpp).
// Atomics: written by the overlay / hotkeys on the present thread, read on the render threads.
namespace rr::cfg
{
    extern std::atomic<bool> rr_enabled;         // F11
    extern std::atomic<int> preset;              // sl::DLSSDPreset value (6 = F)
    extern std::atomic<int> sky_mode;            // guides: pixels without geometry 0 black, 1 white diffuse, 2 specular mirror
    extern std::atomic<bool> foliage_neutral;    // guides v3: white diffuse on incoherent normals
    extern std::atomic<int> foliage_threshold;   // percent, mean neighbour dot below which a pixel counts as foliage
    extern std::atomic<bool> foliage_specular;   // guides v2: fade the specular guide on incoherent normals
    extern std::atomic<bool> foliage_dilate;     // widen the foliage mask over a 5x5 window (flat leaf cards)
    extern std::atomic<bool> foliage_floor_mode; // foliage: true = albedo floor (keeps detail), false = white
    extern std::atomic<int> foliage_floor;       // albedo floor in percent (10..40)
    extern std::atomic<int> gi_firefly;          // "RR completo": 0 off, 1 soft (k=3), 2 medium (k=2), 3 strong (k=1.25)
    extern std::atomic<int> gi_raw_clamp;        // "RR completo": clamp of the isolated brightest raw GI sample in the 3x3 resolve, 0 off, 1 k=4, 2 k=2, 3 k=1
    extern std::atomic<bool> gi_motion_auto;     // "RR completo": switch to the game's GI while the camera moves
    extern std::atomic<int> gi_motion_mm;        // camera displacement per frame that counts as moving, in mm
    extern std::atomic<int> gi_motion_mdeg;      // camera turn per frame that counts as moving, in thousandths of a degree
    extern std::atomic<bool> rr_hit_distance;    // tag the diffuse hit distance guide for RR (kBufferTypeDiffuseHitDistance)
    extern std::atomic<int> rr_hit_scale;        // metres for the game's normalised hit distance 1.0 (hit = scale * w^2)
    extern std::atomic<bool> rr_spec_mv;         // specular motion vectors for RR (kBufferTypeSpecularMotionVectors)
    extern std::atomic<int> rr_spec_mv_distance; // estimated reflection distance in metres (for gloss 1)
    extern std::atomic<int> gi_assist;           // "RR completo": the game's spatial filter on young pixels only, 0 off, 1 = fades out over 4 frames, 2 = over 8
    extern std::atomic<int> gi_accum;            // "RR completo": frames of pre-accumulation, 1 / 2 / 4
    extern std::atomic<bool> rr_sharpen;         // adaptive sharpening of the RR output
    extern std::atomic<int> rr_sharpness;        // 0..100
    extern std::atomic<int> rr_exposure_ev;      // exposure handed to RR as 2^EV (kBufferTypeExposure); 0 = none, as the game
    extern std::atomic<bool> diag_neutral;       // diagnostic, not saved: neutral guides over the whole image
    extern std::atomic<bool> metal_alpha;        // F7: metalness from the albedo alpha (experimental)
    extern std::atomic<int> gi_temporal_mode;    // RTGI filter: 0 original, 1 history x2, 2 x4, 3 no temporal, 4 no temporal + no spatial
    extern std::atomic<bool> reset_history;      // one-shot request, consumed by the next RR evaluation
    extern std::atomic<int> language;            // overlay and bench report: 0 English, 1 Spanish
    extern std::atomic<bool> dev_hotkeys;        // F7 (metalness) and F10 (captures); F11 always works
    void load();
    void save();
    void restore_defaults();
}

namespace rr
{
    // Interface language: English by default, Spanish selectable in the overlay.
    inline bool lang_es() { return cfg::language.load(std::memory_order_relaxed) == 1; }
    inline const char *tr(const char *en, const char *es) { return lang_es() ? es : en; }

    // Folder for frame captures, bench runs and exported shaders: [RR_Forza] CaptureDir in ReShade.ini,
    // otherwise "rr-forza-captures" next to the add-on (the game folder).
    std::filesystem::path capture_root();
    // Writes the game shaders the build patches (RTGI filters, see game-shaders/README.md) into
    // capture_root()/game-shaders. Returns a status line for the overlay.
    std::string export_game_shaders();
    // Shader identification (shader_dump.cpp).
    void register_shader_events();
    std::string shader_label(uint64_t pipeline_handle, uint32_t *crc_out);
    int dump_shaders(const std::filesystem::path &dir, const std::vector<uint64_t> &pipelines);

    // Hook installers.
    bool install_marker_hooks(ID3D12GraphicsCommandList *any_list);
    bool install_streamline_hooks();
    void shutdown_hooks();
}

namespace rr::image
{
    struct channel_stats
    {
        double min = 0, max = 0, mean = 0;
        uint64_t non_finite = 0;
    };

    struct analysis
    {
        int channels = 0;
        std::string decode_note;           // how a typeless format was interpreted
        channel_stats stats[4];
        double mean_length_raw = 0;        // mean |xyz| as stored
        double mean_length_unorm = 0;      // mean |xyz * 2 - 1| (unorm-encoded vectors)
        bool supported = false;
    };

    // Rows are tightly packed (row_size bytes each, no pitch padding).
    analysis analyze(DXGI_FORMAT format, uint32_t width, uint32_t height, const uint8_t *data, uint64_t row_size);

    bool write_dds(const std::wstring &path, DXGI_FORMAT format, uint32_t width, uint32_t height,
                   const uint8_t *data, uint64_t row_size);
    // Writes an 8-bit preview (and a separate alpha preview when alpha varies). Needs COM on the calling thread.
    bool write_previews(const std::wstring &base_path, DXGI_FORMAT format, uint32_t width, uint32_t height,
                        const uint8_t *data, uint64_t row_size, const analysis &info);
}
