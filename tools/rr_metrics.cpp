// rr_metrics: re-runs the RR vs DLSS analysis (src/metrics.cpp) on a bench folder written by the add-on.
// Usage: rr_metrics [--en | --es] <rr-forza-captures\bench-YYYYMMDD-HHMMSS>
// Reads meta.txt and the vN_fK_lum.dds / vN_depth.dds dumps, rewrites report.txt / report.csv /
// report.html and the images in that folder and prints the summary. The report language is the one of
// the original run (meta.txt "lang="; Spanish for runs older than 0.1.0) unless --en / --es is given.

#include "metrics.hpp"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    // DDS written by rr::image::write_dds: 128-byte header + DX10 header, then tightly packed rows.
    bool read_r32_dds(const std::filesystem::path &path, uint32_t &w, uint32_t &h, std::vector<float> &out)
    {
        std::ifstream f(path, std::ios::binary);
        if (!f)
            return false;
        uint32_t header[37] = {};
        f.read(reinterpret_cast<char *>(header), sizeof(header));
        if (!f || header[0] != 0x20534444 || header[32] != 41)   // "DDS ", DXGI_FORMAT_R32_FLOAT
            return false;
        h = header[3];
        w = header[4];
        out.resize(size_t(w) * h);
        f.read(reinterpret_cast<char *>(out.data()), std::streamsize(out.size() * 4));
        return bool(f);
    }
}

int main(int argc, char **argv)
{
    int forced_lang = -1;   // -1 = from meta.txt, 0 = English, 1 = Spanish
    const char *folder = nullptr;
    for (int i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--en") == 0)
            forced_lang = 0;
        else if (strcmp(argv[i], "--es") == 0)
            forced_lang = 1;
        else if (folder == nullptr)
            folder = argv[i];
        else
            folder = nullptr, i = argc;   // two folders: usage error
    }
    if (folder == nullptr)
    {
        fprintf(stderr, "usage: rr_metrics [--en | --es] <bench folder>\n");
        return 2;
    }
    const std::filesystem::path dir = folder;
    std::ifstream meta(dir / "meta.txt");
    if (!meta)
    {
        fprintf(stderr, "no meta.txt in %s\n", folder);
        return 3;
    }
    rr::metrics::scene scene;
    scene.spanish = true;   // runs older than 0.1.0 have no "lang=" line and were written in Spanish
    std::vector<rr::metrics::frame_set> sets;
    std::string line;
    while (std::getline(meta, line))
    {
        if (line.rfind("lang=", 0) == 0)
            scene.spanish = line.substr(5) == "es";
        else if (line.rfind("near=", 0) == 0)
            scene.near_plane = std::stof(line.substr(5));
        else if (line.rfind("near_band=", 0) == 0)
            scene.near_band_m = std::stof(line.substr(10));
        else if (line.rfind("far=", 0) == 0)
            scene.far_plane = std::stof(line.substr(4));
        else if (line.rfind("check=", 0) == 0)
            scene.checks.push_back(line.substr(6));
        else if (line.rfind("description=", 0) == 0)
            scene.description = line.substr(12);
        else if (line.rfind("variant=", 0) == 0)
        {
            // variant=<index>|<name>|<is reference>|<frames>|<frames expected>
            std::vector<std::string> f;
            std::stringstream ss(line.substr(8));
            std::string part;
            while (std::getline(ss, part, '|'))
                f.push_back(part);
            if (f.size() != 5)
                continue;
            rr::metrics::frame_set fs;
            const int index = std::stoi(f[0]);
            fs.name = f[1];
            fs.is_reference = f[2] == "1";
            fs.after_reset = fs.name.find(" after ") != std::string::npos || fs.name.find(" tras ") != std::string::npos;
            fs.frames_expected = std::stoi(f[4]);
            const int frames = std::stoi(f[3]);
            const std::string base = "v" + std::to_string(index);
            for (int k = 0; k < frames; ++k)
            {
                std::vector<float> lum;
                uint32_t w = 0, h = 0;
                if (read_r32_dds(dir / (base + "_f" + std::to_string(k) + "_lum.dds"), w, h, lum))
                {
                    fs.width = w;
                    fs.height = h;
                    fs.luminance.push_back(std::move(lum));
                }
            }
            read_r32_dds(dir / (base + "_depth.dds"), fs.depth_width, fs.depth_height, fs.depth);
            sets.push_back(std::move(fs));
        }
        else if (line.rfind("frames=", 0) == 0)
        {
            // frames=<index>|<frame number of each saved frame, comma separated>
            const size_t bar = line.find('|');
            if (bar == std::string::npos)
                continue;
            const size_t index = size_t(std::stoi(line.substr(7, bar - 7)));
            if (index >= sets.size())
                continue;
            std::stringstream ss(line.substr(bar + 1));
            std::string part;
            while (std::getline(ss, part, ','))
                sets[index].frame_number.push_back(std::stoi(part));
            if (sets[index].frame_number.size() != sets[index].luminance.size())
                sets[index].frame_number.clear();
        }
    }
    if (forced_lang >= 0)
        scene.spanish = forced_lang == 1;
    const std::string summary = rr::metrics::analyze(sets, scene, dir.wstring());
    fwrite(summary.data(), 1, summary.size(), stdout);
    return 0;
}
