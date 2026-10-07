// Image-quality metrics for the RR vs DLSS bench (bench.cpp) and the offline tool
// (tools/rr_metrics.cpp). Independent of D3D12 and ReShade.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rr::metrics
{
    // One bench variant: several consecutive frames of the DLSS output with the camera still.
    struct frame_set
    {
        std::string name;
        bool is_reference = false;            // DLSS-SR: the baseline every ratio is computed against
        uint32_t width = 0, height = 0;       // output resolution
        std::vector<std::vector<float>> luminance;   // linear luminance per frame, width * height
        std::vector<float> depth;             // reverse-Z device depth, depth_width * depth_height
        uint32_t depth_width = 0, depth_height = 0;
        int frames_expected = 0;              // frames recorded; luminance.size() may be smaller (rejected frames)
        bool after_reset = false;             // frames 1..N right after an RR history reset (convergence bench): no motion check
        std::vector<int> frame_number;        // per luminance frame: evaluations since recording began (1 = first after a reset); empty = 1, 2, 3...
    };

    struct scene
    {
        float near_plane = 0.1f, far_plane = 50000.0f;
        std::vector<std::string> checks;      // per-variant verification of what the GPU really ran (GI shader swaps)
        float near_band_m = 15.0f;            // upper limit of the "near" band; 8 m in the car-reflection bench (only the car)
        std::string description;              // DLSS mode, settings, date (copied into the report)
        bool spanish = false;                 // report language (the overlay's at the time of the run)
    };

    // Linear luminance from one R11G11B10_FLOAT texel.
    float luminance_r11g11b10(uint32_t texel);

    // Runs every metric, writes report.txt, report.csv and heatmap_far.bmp into `out_dir`, and
    // returns a short summary for the overlay.
    std::string analyze(const std::vector<frame_set> &sets, const scene &s, const std::wstring &out_dir);
}
