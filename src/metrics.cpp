// Image-quality metrics for the RR vs DLSS bench.
//
// Everything works on luminance mapped to a fixed log scale (16 EV, 2^-12 .. 2^4), so the sky does not
// dominate and a ratio means the same in shadows and highlights. With the camera still, the frames of a
// variant differ only by noise, which lets the analysis separate the two things a single screenshot
// mixes up:
//   detail = high-frequency energy of the MEAN of the frames (noise averages out, detail stays)
//   noise  = per-pixel temporal standard deviation across the frames
// Both are reported per distance band, from the DLSS depth (reverse-Z, near/far from slSetConstants).

#include "metrics.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace rr::metrics
{
    namespace
    {
        constexpr int kBands = 5;   // 0 near, 1 mid, 2 far, 3 very far, 4 sky
        const char *const kBandNames[kBands] = { "Near (<15 m)", "Mid (15-60 m)", "Far (60-250 m)", "Very far (>250 m)", "Sky" };
        const char *const kBandNamesEs[kBands] = { "Cerca (<15 m)", "Media (15-60 m)", "Lejos (60-250 m)", "Muy lejos (>250 m)", "Cielo" };
        const char *const kBandKeys[kBands] = { "near", "mid", "far", "very_far", "sky" };

        float small_float(uint32_t bits, int mant_bits)
        {
            const uint32_t mant = bits & ((1u << mant_bits) - 1), exp = bits >> mant_bits;
            if (exp == 0)
                return std::ldexp(float(mant), -14 - mant_bits);
            if (exp == 31)
                return mant ? 0.0f : 65000.0f;   // NaN -> 0, Inf -> clamp
            return std::ldexp(float(mant | (1u << mant_bits)), int(exp) - 15 - mant_bits);
        }

        float tonemap(float l)
        {
            const float v = (std::log2(std::max(l, 1e-6f)) + 12.0f) / 16.0f;
            return std::clamp(v, 0.0f, 1.0f);
        }

        // Separable Gaussian blur with clamped borders.
        std::vector<float> blur(const std::vector<float> &src, uint32_t w, uint32_t h, float sigma)
        {
            const int r = std::max(1, int(std::ceil(sigma * 3.0f)));
            std::vector<float> k(2 * r + 1);
            float sum = 0.0f;
            for (int i = -r; i <= r; ++i)
                sum += k[i + r] = std::exp(-float(i * i) / (2.0f * sigma * sigma));
            for (float &v : k)
                v /= sum;
            std::vector<float> tmp(src.size()), out(src.size());
            for (uint32_t y = 0; y < h; ++y)
                for (uint32_t x = 0; x < w; ++x)
                {
                    float a = 0.0f;
                    for (int i = -r; i <= r; ++i)
                        a += k[i + r] * src[y * w + uint32_t(std::clamp(int(x) + i, 0, int(w) - 1))];
                    tmp[y * w + x] = a;
                }
            for (uint32_t y = 0; y < h; ++y)
                for (uint32_t x = 0; x < w; ++x)
                {
                    float a = 0.0f;
                    for (int i = -r; i <= r; ++i)
                        a += k[i + r] * tmp[uint32_t(std::clamp(int(y) + i, 0, int(h) - 1)) * w + x];
                    out[y * w + x] = a;
                }
            return out;
        }

        struct band_stats
        {
            double fine = 0, medium = 0, noise = 0, level = 0;
            uint64_t count = 0;
        };

        struct set_result
        {
            bool valid = false;
            std::string why_invalid;
            int frames = 0, rejected = 0;   // frames used / frames dropped as damaged
            band_stats bands[kBands];
            double border_level = 0, border_bright_fraction = 0;   // sky contamination
            uint64_t border_count = 0;
            double first_last_diff = 0;   // mean |t(first) - t(last)| over geometry: camera movement check
            std::vector<float> fine_map;  // per-pixel fine detail of the mean image (for the heat map)
            double fireflies_per_frame = 0;   // isolated bright points per megapixel, average over frames
            double fireflies_persistent = 0;  // same, detected in the mean image (points that stay)
            double faint_per_frame = 0;       // faint dots (ratio 1.25) per megapixel, average over frames
            double added_per_frame = 0;       // dots this variant adds over DLSS normal, per megapixel and frame
            double blink1 = 0, blink2 = 0;    // pixel-frames > 1 / > 2 EV above the pixel's temporal median, per megapixel and frame (geometry only)
            std::vector<float> lin_mean;      // linear mean luminance of the used frames
            std::vector<const std::vector<float> *> used;   // frames that passed the damage filter
            std::vector<uint8_t> added_map;   // where added dots were found (any frame)
            std::vector<uint8_t> firefly_map; // 1 where a point was found in any frame (for the map)
        };

        // Isolated bright points (fireflies): a local maximum at least kFireflyRatio times brighter
        // than the mean of the ring of 16 pixels at distance 2 around it. Blobs up to 3x3 still count
        // (RR may spread a point a little); real highlights are larger and fail the ring test.
        constexpr float kFireflyFloor = 1.0f / 256.0f;   // ignore points in near-black areas
        constexpr float kAddedRatio = 1.2f;              // added dot: >= 20 % above its uniform surroundings, relative to DLSS normal

        // ratio 4: strong points; ratio 1.25: faint 2-4 px dots on walls (the night "puntos" seen with
        // "RR completo"), which also catches texture, so only the difference to DLSS normal matters.
        int detect_fireflies(const std::vector<float> &l, uint32_t w, uint32_t h, std::vector<uint8_t> *map, float kFireflyRatio = 4.0f)
        {
            int count = 0;
            for (uint32_t y = 2; y + 2 < h; ++y)
                for (uint32_t x = 2; x + 2 < w; ++x)
                {
                    const float c = l[size_t(y) * w + x];
                    if (c < kFireflyFloor)
                        continue;
                    bool local_max = true;
                    for (int oy = -1; oy <= 1 && local_max; ++oy)
                        for (int ox = -1; ox <= 1; ++ox)
                            if ((ox || oy) && l[size_t(int(y) + oy) * w + uint32_t(int(x) + ox)] > c)
                            {
                                local_max = false;
                                break;
                            }
                    if (!local_max)
                        continue;
                    float ring = 0.0f;
                    for (int o = -2; o <= 2; ++o)
                    {
                        ring += l[size_t(y - 2) * w + uint32_t(int(x) + o)] + l[size_t(y + 2) * w + uint32_t(int(x) + o)];
                        if (o > -2 && o < 2)
                            ring += l[size_t(int(y) + o) * w + x - 2] + l[size_t(int(y) + o) * w + x + 2];
                    }
                    ring /= 16.0f;
                    if (c > kFireflyRatio * ring)
                    {
                        count++;
                        if (map)
                            (*map)[size_t(y) * w + x] = 1;
                    }
                }
            return count;
        }

        set_result analyze_set(const frame_set &fs, const std::vector<uint8_t> &band_of, const std::vector<uint8_t> &border, float sky_level_linear, bool es)
        {
            set_result r;
            const uint32_t w = fs.width, h = fs.height;
            const size_t n = size_t(w) * h;
            // Reject damaged frames (partly black or otherwise unlike the others): compare each frame's
            // mean log luminance and share of black pixels with the median frame.
            std::vector<const std::vector<float> *> frames;
            {
                struct frame_info { double mean; double black; const std::vector<float> *data; };
                std::vector<frame_info> infos;
                for (const std::vector<float> &frame : fs.luminance)
                {
                    double sum = 0, black = 0;
                    for (size_t i = 0; i < n; i += 13)
                    {
                        sum += tonemap(frame[i]);
                        black += frame[i] <= 0.0f;
                    }
                    const double samples = double((n + 12) / 13);
                    infos.push_back({ sum / samples, black / samples, &frame });
                }
                std::vector<double> means;
                for (const frame_info &f : infos)
                    means.push_back(f.mean);
                std::sort(means.begin(), means.end());
                const double median = means.empty() ? 0.0 : means[means.size() / 2];
                double min_black = 1.0;
                for (const frame_info &f : infos)
                    min_black = std::min(min_black, f.black);
                for (const frame_info &f : infos)
                {
                    if (std::fabs(f.mean - median) > 0.02 || f.black > min_black + 0.01)
                        r.rejected++;
                    else
                        frames.push_back(f.data);
                }
            }
            r.frames = int(frames.size());
            if (r.frames < 4)
            {
                r.why_invalid = es ? "solo " + std::to_string(r.frames) + " frames v\xC3\xA1lidos (m\xC3\xADnimo 4)" : "only " + std::to_string(r.frames) + " valid frames (minimum 4)";
                return r;
            }
            std::vector<float> mean(n, 0.0f), sq(n, 0.0f);
            for (const std::vector<float> *frame : frames)
                for (size_t i = 0; i < n; ++i)
                {
                    const float t = tonemap((*frame)[i]);
                    mean[i] += t;
                    sq[i] += t * t;
                }
            const float inv = 1.0f / float(r.frames);
            for (size_t i = 0; i < n; ++i)
            {
                mean[i] *= inv;
                sq[i] = std::sqrt(std::max(sq[i] * inv - mean[i] * mean[i], 0.0f));   // now the temporal std
            }
            const std::vector<float> g1 = blur(mean, w, h, 1.0f);
            const std::vector<float> g25 = blur(g1, w, h, 2.3f);   // sigma ~2.5 overall (sqrt(1 + 2.3^2))
            r.fine_map.resize(n);
            double diff_sum = 0;
            uint64_t diff_count = 0;
            for (size_t i = 0; i < n; ++i)
            {
                const float fine = std::fabs(mean[i] - g1[i]);
                const float medium = std::fabs(g1[i] - g25[i]);
                r.fine_map[i] = fine;
                band_stats &b = r.bands[band_of[i]];
                b.fine += fine;
                b.medium += medium;
                b.noise += sq[i];
                b.level += mean[i];
                b.count++;
                if (band_of[i] != 4)
                {
                    diff_sum += std::fabs(tonemap((*frames.front())[i]) - tonemap((*frames.back())[i]));
                    diff_count++;
                }
                if (border[i])
                {
                    r.border_level += mean[i];
                    const float l_mean = std::exp2(mean[i] * 16.0f - 12.0f);
                    if (sky_level_linear > 0.0f && l_mean >= 0.5f * sky_level_linear)
                        r.border_bright_fraction += 1.0;
                    r.border_count++;
                }
            }
            for (band_stats &b : r.bands)
                if (b.count)
                {
                    b.fine /= double(b.count);
                    b.medium /= double(b.count);
                    b.noise /= double(b.count);
                    b.level /= double(b.count);
                }
            if (r.border_count)
            {
                r.border_level /= double(r.border_count);
                r.border_bright_fraction /= double(r.border_count);
            }
            r.first_last_diff = diff_count ? diff_sum / double(diff_count) : 0.0;
            {
                const double mp = double(n) / 1e6;
                r.firefly_map.assign(n, 0);
                double total = 0;
                for (const std::vector<float> *frame : frames)
                    total += detect_fireflies(*frame, w, h, &r.firefly_map);
                r.fireflies_per_frame = total / double(frames.size()) / mp;
                std::vector<float> &lin_mean = r.lin_mean;
                lin_mean.assign(n, 0.0f);
                for (const std::vector<float> *frame : frames)
                    for (size_t i = 0; i < n; ++i)
                        lin_mean[i] += (*frame)[i];
                for (float &v : lin_mean)
                    v /= float(frames.size());
                r.used = frames;
                r.fireflies_persistent = detect_fireflies(lin_mean, w, h, nullptr) / mp;
                double faint = 0;
                for (const std::vector<float> *frame : frames)
                    faint += detect_fireflies(*frame, w, h, nullptr, 1.25f);
                r.faint_per_frame = faint / double(frames.size()) / mp;
            }
            // Blinking dots (the night "puntos" of RR completo): a pixel that, in some frame, is far above its own
            // temporal median, AND isolated in that frame (at most 16 such pixels in its 9x9 window), so a car
            // or a light passing by (large blobs, seen in the night benches of 5 oct 2026) is not counted.
            // Per megapixel and frame, geometry only, ignoring near-black pixels.
            {
                const size_t nf = frames.size();
                std::vector<uint8_t> mask1(n * nf, 0), mask2(n * nf, 0);
                std::vector<float> vals(nf);
                uint64_t pixels = 0;
                for (size_t i = 0; i < n; ++i)
                {
                    if (band_of[i] == 4)
                        continue;
                    pixels++;
                    for (size_t k = 0; k < nf; ++k)
                        vals[k] = tonemap((*frames[k])[i]);
                    std::vector<float> sorted = vals;
                    std::nth_element(sorted.begin(), sorted.begin() + nf / 2, sorted.end());
                    const float median = sorted[nf / 2];
                    if (median < 1.0f / 16.0f)   // below -11 EV: black
                        continue;
                    for (size_t k = 0; k < nf; ++k)
                    {
                        mask1[k * n + i] = vals[k] > median + 1.0f / 16.0f;
                        mask2[k * n + i] = vals[k] > median + 2.0f / 16.0f;
                    }
                }
                auto isolated = [&](const std::vector<uint8_t> &mask) {
                    uint64_t count = 0;
                    std::vector<uint32_t> sat(size_t(w + 1) * (h + 1));
                    for (size_t k = 0; k < nf; ++k)
                    {
                        const uint8_t *m = mask.data() + k * n;
                        for (uint32_t y = 0; y < h; ++y)
                        {
                            uint32_t row = 0;
                            for (uint32_t x = 0; x < w; ++x)
                            {
                                row += m[size_t(y) * w + x];
                                sat[size_t(y + 1) * (w + 1) + x + 1] = sat[size_t(y) * (w + 1) + x + 1] + row;
                            }
                        }
                        for (uint32_t y = 0; y < h; ++y)
                            for (uint32_t x = 0; x < w; ++x)
                            {
                                if (!m[size_t(y) * w + x])
                                    continue;
                                const uint32_t x0 = x >= 4 ? x - 4 : 0, y0 = y >= 4 ? y - 4 : 0, x1 = std::min(x + 5, w), y1 = std::min(y + 5, h);
                                const uint32_t sum = sat[size_t(y1) * (w + 1) + x1] - sat[size_t(y0) * (w + 1) + x1] - sat[size_t(y1) * (w + 1) + x0] + sat[size_t(y0) * (w + 1) + x0];
                                count += sum <= 16;
                            }
                    }
                    return count;
                };
                const double norm = pixels ? 1e6 / (double(pixels) * double(nf)) : 0.0;
                r.blink1 = double(isolated(mask1)) * norm;
                r.blink2 = double(isolated(mask2)) * norm;
            }
            // Frames right after an RR history reset are meant to change: no motion check for them.
            if (fs.after_reset)
            {
                r.valid = true;
                return r;
            }
            // A still camera gives near-identical first and last frames; more than this means motion.
            if (r.first_last_diff > 0.03)
            {
                r.why_invalid = es ? "la imagen cambi\xC3\xB3 entre el primer y el \xC3\xBAltimo frame (\xC2\xBFse movi\xC3\xB3 la c\xC3\xA1mara?)"
                                   : "the image changed between the first and the last frame (did the camera move?)";
                return r;
            }
            r.valid = true;
            return r;
        }

        std::string fmt(double v, int decimals = 3)
        {
            char b[32];
            snprintf(b, sizeof(b), "%.*f", decimals, v);
            return b;
        }

        // Difference of two mean log-luminance levels (the tonemap scale is 16 EV wide), as "+0.25 EV".
        std::string ev_diff(double v, double ref)
        {
            char b[32];
            snprintf(b, sizeof(b), "%+.2f EV", (v - ref) * 16.0);
            return b;
        }

        std::string ratio(double v, double ref)
        {
            if (ref <= 1e-9)
                return "  -  ";
            return fmt(v / ref, 2);
        }

        bool write_bmp(const std::wstring &path, uint32_t w, uint32_t h, const std::vector<uint8_t> &rgb)
        {
            const uint32_t row = (w * 3 + 3) & ~3u;
            const uint32_t size = 54 + row * h;
            uint8_t header[54] = { 'B', 'M' };
            auto put32 = [&](int at, uint32_t v) { memcpy(header + at, &v, 4); };
            auto put16 = [&](int at, uint16_t v) { memcpy(header + at, &v, 2); };
            put32(2, size);
            put32(10, 54);
            put32(14, 40);
            put32(18, w);
            put32(22, h);
            put16(26, 1);
            put16(28, 24);
            put32(34, row * h);
            FILE *f = _wfopen(path.c_str(), L"wb");
            if (!f)
                return false;
            fwrite(header, 1, 54, f);
            std::vector<uint8_t> line(row, 0);
            for (uint32_t y = 0; y < h; ++y)
            {
                const uint8_t *src = rgb.data() + size_t(h - 1 - y) * w * 3;
                for (uint32_t x = 0; x < w; ++x)
                {
                    line[x * 3 + 0] = src[x * 3 + 2];
                    line[x * 3 + 1] = src[x * 3 + 1];
                    line[x * 3 + 2] = src[x * 3 + 0];
                }
                fwrite(line.data(), 1, row, f);
            }
            fclose(f);
            return true;
        }
    }

    float luminance_r11g11b10(uint32_t v)
    {
        const float r = small_float(v & 0x7ff, 6), g = small_float((v >> 11) & 0x7ff, 6), b = small_float(v >> 22, 5);
        return 0.2126f * r + 0.7152f * g + 0.0722f * b;
    }

    std::string analyze(const std::vector<frame_set> &sets, const scene &s, const std::wstring &out_dir)
    {
        const bool es = s.spanish;
        auto T = [es](const char *en, const char *spanish) { return es ? spanish : en; };
        const frame_set *ref = nullptr;
        for (const frame_set &fs : sets)
            if (fs.is_reference)
                ref = &fs;
        if (ref == nullptr || ref->width == 0 || ref->luminance.empty() || ref->depth.empty())
            return T("Measurement not valid: the DLSS-SR reference or its depth is missing.", "Medici\xC3\xB3n no v\xC3\xA1lida: falta la referencia DLSS normal o su profundidad.");
        const uint32_t w = ref->width, h = ref->height;
        const size_t n = size_t(w) * h;

        // Distance band of every output pixel, from the reference depth (render resolution).
        std::vector<uint8_t> band_of(n), sky(n);
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
            {
                const uint32_t dx = std::min(ref->depth_width - 1, uint32_t(uint64_t(x) * ref->depth_width / w));
                const uint32_t dy = std::min(ref->depth_height - 1, uint32_t(uint64_t(y) * ref->depth_height / h));
                const float d = ref->depth[size_t(dy) * ref->depth_width + dx];
                uint8_t band = 4;
                if (d > 1e-7f)
                {
                    // Reverse-Z: d = n (f - z) / (z (f - n))  ->  z = n f / (d (f - n) + n)
                    const float z = s.near_plane * s.far_plane / (d * (s.far_plane - s.near_plane) + s.near_plane);
                    band = z < s.near_band_m ? 0 : z < 15.0f ? 1 : z < 60.0f ? 1 : z < 250.0f ? 2 : z < 0.95f * s.far_plane ? 3 : 4;
                }
                band_of[size_t(y) * w + x] = band;
                sky[size_t(y) * w + x] = band == 4;
            }
        // Geometry within 3 px of the sky: where sky light could bleed into leaves and thin edges.
        std::vector<uint8_t> border(n, 0);
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
            {
                if (sky[size_t(y) * w + x])
                    continue;
                bool near_sky = false;
                for (int oy = -3; oy <= 3 && !near_sky; ++oy)
                    for (int ox = -3; ox <= 3 && !near_sky; ++ox)
                    {
                        const int xx = int(x) + ox, yy = int(y) + oy;
                        if (xx >= 0 && yy >= 0 && xx < int(w) && yy < int(h) && sky[size_t(yy) * w + xx])
                            near_sky = true;
                    }
                border[size_t(y) * w + x] = near_sky;
            }
        // Median sky luminance of the reference (to call a border pixel "as bright as the sky").
        std::vector<float> sky_values;
        for (size_t i = 0; i < n; i += 7)
            if (sky[i])
                sky_values.push_back(ref->luminance.front()[i]);
        float sky_level = 0.0f;
        if (!sky_values.empty())
        {
            std::nth_element(sky_values.begin(), sky_values.begin() + sky_values.size() / 2, sky_values.end());
            sky_level = sky_values[sky_values.size() / 2];
        }

        std::vector<set_result> results;
        for (const frame_set &fs : sets)
        {
            if (fs.width != w || fs.height != h)
            {
                set_result bad;
                bad.why_invalid = T("resolution differs from the reference", "resoluci\xC3\xB3n distinta a la de la referencia");
                results.push_back(std::move(bad));
                continue;
            }
            results.push_back(analyze_set(fs, band_of, border, sky_level, es));
        }
        size_t ref_index = 0;
        for (size_t i = 0; i < sets.size(); ++i)
            if (sets[i].is_reference)
                ref_index = i;
        // Dots a variant ADDS over DLSS normal: with the camera still, the per-pixel ratio
        // frame / reference mean is ~1 everywhere except where the variant draws something the
        // reference does not. A local maximum of that ratio at least 20 % above the median ratio of
        // its uniform 5x5 surroundings (and visible: luminance over the floor) is one added dot.
        if (!results[ref_index].lin_mean.empty())
        {
            const std::vector<float> &refm = results[ref_index].lin_mean;
            for (size_t v = 0; v < results.size(); ++v)
            {
                set_result &r = results[v];
                if (v == ref_index || r.used.empty())
                    continue;
                r.added_map.assign(n, 0);
                std::vector<float> ratio_map(n);
                double total = 0;
                for (const std::vector<float> *frame : r.used)
                {
                    for (size_t i = 0; i < n; ++i)
                        ratio_map[i] = ((*frame)[i] + 1e-4f) / (refm[i] + 1e-4f);
                    for (uint32_t y = 2; y + 2 < h; ++y)
                        for (uint32_t x = 2; x + 2 < w; ++x)
                        {
                            const size_t i = size_t(y) * w + x;
                            const float c = ratio_map[i];
                            if (c < kAddedRatio || (*frame)[i] < kFireflyFloor)
                                continue;
                            // Dots are blobs of up to ~4 px (RR spreads one GI sample): local maximum
                            // over 5x5, surroundings = the 32 pixels at distance 4.
                            float ring[32];
                            int k = 0;
                            bool local_max = true;
                            for (int oy = -4; oy <= 4 && local_max; ++oy)
                                for (int ox = -4; ox <= 4; ++ox)
                                {
                                    const int ax = std::abs(ox), ay = std::abs(oy);
                                    const int xx = int(x) + ox, yy = int(y) + oy;
                                    if (xx < 0 || yy < 0 || xx >= int(w) || yy >= int(h))
                                    {
                                        local_max = false;   // too close to the border
                                        break;
                                    }
                                    const float q = ratio_map[size_t(yy) * w + size_t(xx)];
                                    if (ax <= 2 && ay <= 2)
                                    {
                                        if ((ox || oy) && q > c)
                                        {
                                            local_max = false;
                                            break;
                                        }
                                    }
                                    else if (ax == 4 || ay == 4)
                                        ring[k++] = q;
                                }
                            if (!local_max || k < 32)
                                continue;
                            // The surroundings must agree with DLSS normal (ratio uniform: a wall, not an
                            // edge that moved), and the dot clearly above them.
                            std::sort(ring, ring + k);
                            const float median = ring[k / 2];
                            if (ring[k - 3] <= 1.15f * ring[2] && c > kAddedRatio * median)
                            {
                                total += 1;
                                r.added_map[i] = 1;
                            }
                        }
                }
                r.added_per_frame = total / double(r.used.size()) / (double(n) / 1e6);
            }
        }
        const set_result &rr0 = results[ref_index];

        // ---- report.txt ----
        std::ostringstream t;
        t << T("RR vs DLSS-SR measurement - rr-forza\n", "Medici\xC3\xB3n RR vs DLSS normal - rr-forza\n") << s.description << "\n";
        t << T("Scale: log2 luminance (16 EV). Detail = high frequency of the mean image (noise-free);\n",
               "Escala: luminancia en log2 (16 EV). Detalle = alta frecuencia de la imagen media (sin ruido);\n");
        t << T("noise = temporal deviation between frames. Ratios against DLSS-SR: 1.00 = same, <1 = less.\n\n",
               "ruido = desviaci\xC3\xB3n temporal entre frames. Ratios contra DLSS normal: 1,00 = igual, <1 = menos.\n\n");
        for (size_t i = 0; i < sets.size(); ++i)
            t << "  [" << i << "] " << sets[i].name << ": " << results[i].frames << "/" << sets[i].frames_expected << " frames"
              << (results[i].rejected ? " (" + std::to_string(results[i].rejected) + T(" dropped as damaged)", " descartados por da\xC3\xB1" "ados)") : std::string()) << ", "
              << (results[i].valid ? std::string(T("valid", "v\xC3\xA1lida")) : T("NOT VALID (", "NO V\xC3\x81LIDA (") + results[i].why_invalid + ")")
              << T(", first-last frame change ", ", cambio 1er-\xC3\xBAltimo frame ") << fmt(results[i].first_last_diff, 4) << "\n";
        t << "\n";
        if (!s.checks.empty())
        {
            t << T("== Verification (what the GPU ran for each variant)\n", "== Verificaci\xC3\xB3n (lo que la GPU ejecut\xC3\xB3 en cada variante)\n");
            for (const std::string &c : s.checks)
                t << "   " << c << "\n";
            t << "\n";
        }
        for (int b = 0; b < kBands; ++b)
        {
            const std::string band_name = b == 0 && s.near_band_m < 15.0f
                ? T("Car (<", "Carro (<") + fmt(s.near_band_m, 0) + " m)" : std::string(es ? kBandNamesEs[b] : kBandNames[b]);
            t << "== " << band_name << " (" << fmt(100.0 * double(rr0.bands[b].count) / double(n), 1) << T(" % of the image)\n", " % de la imagen)\n");
            t << T("   variant                          fine detail   mid detail     noise (abs)  brightness vs ref\n",
                   "   variante                          detalle fino  detalle medio  ruido (abs)  brillo vs ref\n");
            for (size_t i = 0; i < sets.size(); ++i)
            {
                const band_stats &v = results[i].bands[b], &r = rr0.bands[b];
                char line[256];
                snprintf(line, sizeof(line), "   %-32s %8s      %8s      %8s     %8s\n", sets[i].name.c_str(), ratio(v.fine, r.fine).c_str(),
                         ratio(v.medium, r.medium).c_str(), fmt(v.noise, 4).c_str(), ev_diff(v.level, r.level).c_str());
                t << line;
            }
            t << "\n";
        }
        t << T("== Sky contamination (geometry within 3 px of the sky: tree tops, cables, edges; ",
               "== Contaminaci\xC3\xB3n del cielo (geometr\xC3\xAD" "a a <=3 px del cielo: copas, cables, bordes; ")
          << fmt(100.0 * double(rr0.border_count) / double(n), 1) << T(" % of the image)\n", " % de la imagen)\n");
        t << T("   variant                          edge brightness vs ref   % as bright as the sky\n",
               "   variante                          brillo borde vs ref   % tan brillante como el cielo\n");
        for (size_t i = 0; i < sets.size(); ++i)
        {
            char line[256];
            snprintf(line, sizeof(line), "   %-32s %10s           %6s %%\n", sets[i].name.c_str(), ev_diff(results[i].border_level, rr0.border_level).c_str(),
                     fmt(100.0 * results[i].border_bright_fraction, 2).c_str());
            t << line;
        }

        // Disocclusion / convergence: every "<label> after ..." variant against its "<label> converged" partner
        // (Spanish runs: " tras " / " convergido"; same camera, still): per-frame mean |log2 error| on pixels
        // that were stable in the converged frames.
        {
            bool header = false;
            for (size_t i = 0; i < sets.size(); ++i)
            {
                const frame_set &fresh = sets[i];
                size_t cut = fresh.name.find(" after ");
                const char *converged_suffix = " converged";
                if (cut == std::string::npos)
                {
                    cut = fresh.name.find(" tras ");
                    converged_suffix = " convergido";
                }
                if (!fresh.after_reset || cut == std::string::npos || fresh.width != w || fresh.height != h || fresh.luminance.empty())
                    continue;
                const std::string partner = fresh.name.substr(0, cut) + converged_suffix;
                const frame_set *conv = nullptr;
                for (size_t j = 0; j < i; ++j)
                    if (!sets[j].after_reset && sets[j].name == partner && sets[j].width == w && sets[j].height == h && sets[j].luminance.size() >= 2)
                        conv = &sets[j];
                if (conv == nullptr)
                    continue;
                auto lv = [](float l) { return std::log2(std::max(l, 1.0f / 4096.0f)); };
                const size_t nc = conv->luminance.size();
                std::vector<float> mean(n, 0.0f);
                std::vector<uint8_t> use(n, 0);
                size_t used = 0;
                for (size_t p = 0; p < n; ++p)
                {
                    if (sky[p])
                        continue;
                    float lo = 1e9f, hi = -1e9f, sum = 0.0f;
                    for (const std::vector<float> &f : conv->luminance)
                    {
                        const float v = lv(f[p]);
                        lo = std::min(lo, v);
                        hi = std::max(hi, v);
                        sum += v;
                    }
                    if (hi - lo > 0.5f)   // something moved through the converged frames (a car): not a reference
                        continue;
                    mean[p] = sum / float(nc);
                    use[p] = 1;
                    ++used;
                }
                if (used < 1000)
                    continue;
                // Noise floor: the converged frames against their own mean (corrected for the mean including them).
                double floor_sum = 0.0;
                for (const std::vector<float> &f : conv->luminance)
                    for (size_t p = 0; p < n; ++p)
                        if (use[p])
                            floor_sum += std::fabs(lv(f[p]) - mean[p]);
                const double floor_err = floor_sum / double(used) / double(nc) * std::sqrt(double(nc) / double(nc - 1));
                if (!header)
                {
                    header = true;
                    if (es)
                    {
                        t << "\n== Desoclusi\xC3\xB3n (error medio |log2| frente a la imagen convergida, en EV; p\xC3\xADxeles estables sin cielo)\n";
                        t << "   piso = ruido de la propia imagen convergida. Frames 1-8 = los 8 frames justo despu\xC3\xA9s de perder todo el historial.\n";
                        t << "   'limpio en' = primer frame con error <= 1,5 x piso. 'brillo' = sesgo medio del frame 1. 'cambio' = % de p\xC3\xADxeles\n";
                        t << "   que en el frame 8 difieren > 1 EV (si es alto, algo pas\xC3\xB3 por delante: mirar las im\xC3\xA1genes).\n";
                        t << "   x = frame da\xC3\xB1" "ado (copia a medias, casi negro), - = frame no grabado. 'cambio' usa el \xC3\xBAltimo frame v\xC3\xA1lido.\n";
                        t << "   variante                                      piso    f1     f2     f3     f4     f5     f6     f7     f8    limpio en  brillo f1  cambio\n";
                    }
                    else
                    {
                        t << "\n== Disocclusion (mean |log2| error against the converged image, in EV; stable pixels, no sky)\n";
                        t << "   floor = noise of the converged image itself. Frames 1-8 = the 8 frames right after losing all history.\n";
                        t << "   'clean at' = first frame with error <= 1.5 x floor. 'bias' = mean bias of frame 1. 'changed' = % of pixels\n";
                        t << "   that differ by > 1 EV in frame 8 (if high, something passed in front: look at the images).\n";
                        t << "   x = damaged frame (half-written copy, almost black), - = frame not recorded. 'changed' uses the last valid frame.\n";
                        t << "   variant                                       floor   f1     f2     f3     f4     f5     f6     f7     f8    clean at   bias f1    changed\n";
                    }
                }
                char line[512];
                int pos = snprintf(line, sizeof(line), "   %-44s %6s", fresh.name.c_str(), fmt(floor_err, 3).c_str());
                int clean_at = 0;
                double bias1 = 0.0, changed = 0.0;
                // Share of exactly-black pixels: a frame far above the converged ones is a damaged copy.
                auto black_share = [&](const std::vector<float> &f) {
                    size_t black = 0, count = 0;
                    for (size_t p = 0; p < n; p += 13, ++count)
                        black += f[p] <= 0.0f;
                    return count ? double(black) / double(count) : 0.0;
                };
                double conv_black = 1.0;
                for (const std::vector<float> &f : conv->luminance)
                    conv_black = std::min(conv_black, black_share(f));

                for (int slot = 1; slot <= 8; ++slot)
                {
                    // The frame recorded at this position after the reset (frames can be skipped or dropped).
                    int k = -1;
                    for (size_t q = 0; q < fresh.luminance.size(); ++q)
                        if ((fresh.frame_number.empty() ? int(q) + 1 : fresh.frame_number[q]) == slot)
                            k = int(q);
                    if (k < 0)
                    {
                        pos += snprintf(line + pos, sizeof(line) - pos, " %6s", "-");
                        continue;
                    }
                    if (black_share(fresh.luminance[size_t(k)]) > conv_black + 0.01)
                    {
                        pos += snprintf(line + pos, sizeof(line) - pos, " %6s", "x");
                        continue;
                    }
                    double e = 0.0, b = 0.0;
                    size_t big = 0;
                    for (size_t p = 0; p < n; ++p)
                        if (use[p])
                        {
                            const double d = lv(fresh.luminance[size_t(k)][p]) - mean[p];
                            e += std::fabs(d);
                            b += d;
                            big += std::fabs(d) > 1.0;
                        }
                    e /= double(used);
                    if (slot == 1)
                        bias1 = b / double(used);
                    changed = 100.0 * double(big) / double(used);   // last valid frame wins

                    if (clean_at == 0 && e <= 1.5 * floor_err)
                        clean_at = slot;
                    pos += snprintf(line + pos, sizeof(line) - pos, " %6s", fmt(e, 3).c_str());
                }

                char bias[24];
                snprintf(bias, sizeof(bias), "%+.2f", bias1);
                snprintf(line + pos, sizeof(line) - pos, "   %9s  %8s EV  %5s %%\n", clean_at ? ("frame " + std::to_string(clean_at)).c_str() : "> 8",
                         bias, fmt(changed, 1).c_str());
                t << line;
            }
        }

        t << T("\n== Isolated dot flicker (pixels above their temporal median in one frame, in small groups; per megapixel and frame, no sky)\n",
               "\n== Parpadeo de puntos aislados (p\xC3\xADxeles que en un frame superan su mediana temporal, en grupos peque\xC3\xB1os; por megap\xC3\xADxel y frame, sin cielo)\n");
        t << T("   variant                           > 1 EV      > 2 EV\n", "   variante                          > 1 EV      > 2 EV\n");
        for (size_t i = 0; i < sets.size(); ++i)
        {
            char line[256];
            snprintf(line, sizeof(line), "   %-32s %8s    %8s\n", sets[i].name.c_str(), fmt(results[i].blink1, 0).c_str(), fmt(results[i].blink2, 0).c_str());
            t << line;
        }

        // Automatic reading of the far band, the user's question.
        t << T("\n== Fireflies (isolated bright points: local maximum >= 4x the ring at 2 px)\n",
               "\n== Destellos (puntos brillantes aislados: m\xC3\xA1ximo local >= 4x el anillo a 2 px)\n");
        t << T("   variant                           strong/frame (/MP)    persistent (/MP)     faint/frame (/MP, 1.25x threshold)   ADDED vs DLSS-SR (/MP and frame)\n",
               "   variante                          fuertes/frame (/MP)   persistentes (/MP)   tenues/frame (/MP, umbral 1,25x)   A\xC3\x91" "ADIDOS vs DLSS normal (/MP y frame)\n");
        for (size_t i = 0; i < sets.size(); ++i)
        {
            char line[256];
            snprintf(line, sizeof(line), "   %-32s %10s            %10s           %10s                   %10s\n", sets[i].name.c_str(), fmt(results[i].fireflies_per_frame, 1).c_str(),
                     fmt(results[i].fireflies_persistent, 1).c_str(), fmt(results[i].faint_per_frame, 1).c_str(),
                     i == ref_index ? "-" : fmt(results[i].added_per_frame, 1).c_str());
            t << line;
        }
        t << T("\n== Automatic reading (Far band)\n", "\n== Lectura autom\xC3\xA1tica (banda Lejos)\n");
        for (size_t i = 0; i < sets.size(); ++i)
        {
            if (i == ref_index || !results[i].valid || !rr0.valid || rr0.bands[2].count < 1000)
                continue;
            const double fine = results[i].bands[2].fine / std::max(rr0.bands[2].fine, 1e-9);
            const double noise_v = results[i].bands[2].noise, noise_r = rr0.bands[2].noise;
            t << "   " << sets[i].name << ": ";
            if (fine >= 0.95)
                t << T("keeps the far detail (", "conserva el detalle lejano (") << fmt(fine * 100.0, 0) << T(" % of DLSS-SR).\n", " % del de DLSS normal).\n");
            else if (noise_v < noise_r * 0.7)
                t << T("has ", "tiene ") << fmt((1.0 - fine) * 100.0, 0)
                  << T(" % less high frequency, and also less noise: part is cleaning, part may be lost detail.\n",
                       " % menos alta frecuencia, y tambi\xC3\xA9n menos ruido: parte es limpieza, parte puede ser p\xC3\xA9rdida de detalle.\n");
            else
                t << T("LOSES real far detail: ", "PIERDE detalle lejano real: ") << fmt((1.0 - fine) * 100.0, 0)
                  << T(" % less, with similar noise.\n", " % menos, con un ruido parecido.\n");
        }
        if (!rr0.valid)
            t << T("   The reference is not valid: repeat the measurement without moving the camera.\n",
                   "   La referencia no es v\xC3\xA1lida: repetir la medici\xC3\xB3n sin mover la c\xC3\xA1mara.\n");

        const std::string report = t.str();
        {
            std::ofstream f(std::filesystem::path(out_dir + L"\\report.txt"), std::ios::binary);
            f << report;
        }

        // ---- report.csv ----
        {
            std::ofstream f(std::filesystem::path(out_dir + L"\\report.csv"), std::ios::binary);
            f << "variant,valid,frames,band,fine,medium,noise,level,pixels\n";
            for (size_t i = 0; i < sets.size(); ++i)
                for (int b = 0; b < kBands; ++b)
                {
                    const band_stats &v = results[i].bands[b];
                    f << sets[i].name << "," << results[i].valid << "," << results[i].frames << "," << kBandKeys[b] << "," << fmt(v.fine, 6) << ","
                      << fmt(v.medium, 6) << "," << fmt(v.noise, 6) << "," << fmt(v.level, 6) << "," << v.count << "\n";
                }
            for (size_t i = 0; i < sets.size(); ++i)
                f << sets[i].name << "," << results[i].valid << "," << results[i].frames << ",sky_border,,,," << fmt(results[i].border_level, 6) << ","
                  << results[i].border_count << "\n";
        }

        // ---- heatmap_far.bmp: fine-detail ratio of the first other variant, 32 px tiles over the reference ----
        size_t cmp = sets.size();
        for (size_t i = 0; i < sets.size(); ++i)
            if (i != ref_index && results[i].valid && !results[i].fine_map.empty())
            {
                cmp = i;
                break;
            }
        if (cmp < sets.size() && !rr0.fine_map.empty())
        {
            std::vector<uint8_t> rgb(n * 3);
            constexpr uint32_t tile = 32;
            for (uint32_t ty = 0; ty < h; ty += tile)
                for (uint32_t tx = 0; tx < w; tx += tile)
                {
                    double a = 0, r = 0;
                    uint32_t sky_px = 0, px = 0;
                    for (uint32_t y = ty; y < std::min(h, ty + tile); ++y)
                        for (uint32_t x = tx; x < std::min(w, tx + tile); ++x)
                        {
                            const size_t i = size_t(y) * w + x;
                            a += results[cmp].fine_map[i];
                            r += rr0.fine_map[i];
                            sky_px += sky[i];
                            px++;
                        }
                    const bool skip = sky_px * 2 > px || r < 1e-6;
                    const double lr = skip ? 0.0 : std::clamp(std::log2(a / r), -1.0, 1.0);   // -1 = half the detail, +1 = double
                    for (uint32_t y = ty; y < std::min(h, ty + tile); ++y)
                        for (uint32_t x = tx; x < std::min(w, tx + tile); ++x)
                        {
                            const size_t i = size_t(y) * w + x;
                            const float g = tonemap(ref->luminance.front()[i]) * 255.0f;
                            float cr = 128, cg = 128, cb = 128;
                            if (skip)
                                cr = cg = cb = 20;
                            else if (lr < 0)
                                cr = 128 + float(-lr) * 127, cg = cb = 128 - float(-lr) * 110;
                            else
                                cg = 128 + float(lr) * 127, cr = cb = 128 - float(lr) * 110;
                            const bool edge = (x - tx) == 0 || (y - ty) == 0;
                            rgb[i * 3 + 0] = uint8_t(edge ? 0 : std::clamp(0.45f * g + 0.55f * cr, 0.0f, 255.0f));
                            rgb[i * 3 + 1] = uint8_t(edge ? 0 : std::clamp(0.45f * g + 0.55f * cg, 0.0f, 255.0f));
                            rgb[i * 3 + 2] = uint8_t(edge ? 0 : std::clamp(0.45f * g + 0.55f * cb, 0.0f, 255.0f));
                        }
                }
            write_bmp(out_dir + L"\\heatmap_" + std::wstring(sets[cmp].name.begin(), sets[cmp].name.end()) + L".bmp", w, h, rgb);
        }

        // ---- fireflies_v<variant>.bmp: detected points in red over the variant's first frame ----
        for (size_t i = 0; i < sets.size(); ++i)
        {
            if (results[i].firefly_map.empty() || sets[i].luminance.empty())
                continue;
            const std::vector<uint8_t> &marks = results[i].added_map.empty() ? results[i].firefly_map : results[i].added_map;
            std::vector<uint8_t> rgb(n * 3);
            const std::vector<float> &first = sets[i].luminance.front();
            for (size_t k = 0; k < n; ++k)
            {
                const uint8_t g = uint8_t(tonemap(first[k]) * 255.0f);
                rgb[k * 3 + 0] = g;
                rgb[k * 3 + 1] = g;
                rgb[k * 3 + 2] = g;
            }
            for (uint32_t y = 0; y < h; ++y)
                for (uint32_t x = 0; x < w; ++x)
                    if (marks[size_t(y) * w + x])
                        for (int oy = -2; oy <= 2; ++oy)
                            for (int ox = -2; ox <= 2; ++ox)
                            {
                                const int xx = int(x) + ox, yy = int(y) + oy;
                                if ((std::abs(ox) == 2 || std::abs(oy) == 2) && xx >= 0 && yy >= 0 && xx < int(w) && yy < int(h))
                                {
                                    const size_t q = (size_t(yy) * w + size_t(xx)) * 3;
                                    rgb[q + 0] = 255;
                                    rgb[q + 1] = 0;
                                    rgb[q + 2] = 0;
                                }
                            }
            write_bmp(out_dir + L"\\fireflies_v" + std::to_wstring(i) + L".bmp", w, h, rgb);
        }

        // ---- visual comparison: what the numbers cannot show (blocky noise, smearing, dots) ----
        // Per variant: the image (frame 0), its temporal noise (std over the frames x20) and a
        // full-resolution crop of the centre; report.html shows them side by side with the numbers.
        {
            const uint32_t hw = w / 2, hh = h / 2;
            const uint32_t cw = std::min<uint32_t>(640, w), ch = std::min<uint32_t>(400, h);
            const uint32_t cx = (w - cw) / 2, cy = (h - ch) / 2;
            std::ostringstream html;
            html << "<!doctype html><html lang=\"" << (es ? "es" : "en") << "\"><head><meta charset=\"utf-8\"><title>" << T("RR measurement", "Medici\xC3\xB3n RR") << "</title><style>"
                    "body{background:#111;color:#ddd;font:14px system-ui,sans-serif;margin:16px}h1{font-size:20px}"
                    "table{border-collapse:collapse}td{vertical-align:top;padding:6px;border-bottom:1px solid #333}"
                    "img{display:block;max-width:100%;image-rendering:pixelated}.n{white-space:pre;font:12px monospace;color:#aaa}"
                    ".bad{color:#f77}.ok{color:#7d7}</style></head><body><h1>"
                 << T("RR vs DLSS-SR measurement", "Medici\xC3\xB3n RR vs DLSS normal") << "</h1><p>" << s.description << "</p><p>"
                 << T("Columns: image (frame 0, half resolution) \xC2\xB7 temporal noise (black = stable, white = flickers; x20) \xC2\xB7 "
                      "centre crop at full resolution. Click to open at full size.",
                      "Columnas: imagen (frame 0, mitad de resoluci\xC3\xB3n) \xC2\xB7 ruido temporal "
                      "(negro = estable, blanco = parpadea; x20) \xC2\xB7 recorte del centro a resoluci\xC3\xB3n completa. Clic para abrir a tama\xC3\xB1o real.")
                 << "</p><table>";
            for (size_t i = 0; i < sets.size(); ++i)
            {
                const set_result &r = results[i];
                if (r.used.empty())
                    continue;
                const std::vector<float> &f0 = *r.used.front();
                // temporal std, linear-log units, recomputed here over the used frames
                std::vector<float> sd(n, 0.0f);
                {
                    std::vector<float> m(n, 0.0f), q(n, 0.0f);
                    for (const std::vector<float> *f : r.used)
                        for (size_t k = 0; k < n; ++k)
                        {
                            const float t = tonemap((*f)[k]);
                            m[k] += t;
                            q[k] += t * t;
                        }
                    const float inv = 1.0f / float(r.used.size());
                    for (size_t k = 0; k < n; ++k)
                        sd[k] = std::sqrt(std::max(q[k] * inv - (m[k] * inv) * (m[k] * inv), 0.0f));
                }
                std::vector<uint8_t> view(size_t(hw) * hh * 3), noise(size_t(hw) * hh * 3), crop(size_t(cw) * ch * 3);
                for (uint32_t y = 0; y < hh; ++y)
                    for (uint32_t x = 0; x < hw; ++x)
                    {
                        float t = 0.0f, nz = 0.0f;
                        for (uint32_t oy = 0; oy < 2; ++oy)
                            for (uint32_t ox = 0; ox < 2; ++ox)
                            {
                                const size_t k = size_t(2 * y + oy) * w + 2 * x + ox;
                                t += tonemap(f0[k]);
                                nz = std::max(nz, sd[k]);
                            }
                        const uint8_t g = uint8_t(std::clamp(t * 0.25f * 255.0f, 0.0f, 255.0f));
                        const uint8_t v = uint8_t(std::clamp(nz * 20.0f * 255.0f, 0.0f, 255.0f));
                        const size_t o = (size_t(y) * hw + x) * 3;
                        view[o] = view[o + 1] = view[o + 2] = g;
                        noise[o] = v;
                        noise[o + 1] = v;
                        noise[o + 2] = v;
                    }
                for (uint32_t y = 0; y < ch; ++y)
                    for (uint32_t x = 0; x < cw; ++x)
                    {
                        const uint8_t g = uint8_t(tonemap(f0[size_t(cy + y) * w + cx + x]) * 255.0f);
                        const size_t o = (size_t(y) * cw + x) * 3;
                        crop[o] = crop[o + 1] = crop[o + 2] = g;
                    }
                const std::wstring tag = L"v" + std::to_wstring(i);
                write_bmp(out_dir + L"\\" + tag + L"_view.bmp", hw, hh, view);
                write_bmp(out_dir + L"\\" + tag + L"_noise.bmp", hw, hh, noise);
                write_bmp(out_dir + L"\\" + tag + L"_crop.bmp", cw, ch, crop);
                const std::string t8(tag.begin(), tag.end());
                char nums[512];
                snprintf(nums, sizeof(nums),
                         T("fine detail near/mid/far: %s / %s / %s\nnoise near: %s (DLSS-SR %s)\nsky edges: %s, %s %% white\nadded dots: %s /MP",
                           "detalle fino cerca/media/lejos: %s / %s / %s\nruido cerca: %s (DLSS normal %s)\nbordes con cielo: %s, %s %% blancos\npuntos a\xC3\xB1" "adidos: %s /MP"),
                         ratio(r.bands[0].fine, rr0.bands[0].fine).c_str(), ratio(r.bands[1].fine, rr0.bands[1].fine).c_str(), ratio(r.bands[2].fine, rr0.bands[2].fine).c_str(),
                         fmt(r.bands[0].noise, 4).c_str(), fmt(rr0.bands[0].noise, 4).c_str(), ev_diff(r.border_level, rr0.border_level).c_str(),
                         fmt(100.0 * r.border_bright_fraction, 1).c_str(), i == ref_index ? "-" : fmt(r.added_per_frame, 0).c_str());
                html << "<tr><td><b>" << sets[i].name << "</b><br><span class=\"" << (r.valid ? "ok\">" : "bad\">")
                     << (r.valid ? T("valid", "v\xC3\xA1lida") : T("NOT valid", "NO v\xC3\xA1lida")) << "</span><div class=\"n\">" << nums
                     << "</div></td><td><a href=\"" << t8 << "_view.bmp\"><img src=\"" << t8 << "_view.bmp\" width=\"480\"></a></td><td><a href=\"" << t8
                     << "_noise.bmp\"><img src=\"" << t8 << "_noise.bmp\" width=\"480\"></a></td><td><a href=\"" << t8 << "_crop.bmp\"><img src=\"" << t8
                     << "_crop.bmp\" width=\"480\"></a></td></tr>";
            }
            html << "</table></body></html>";
            std::ofstream f(std::filesystem::path(out_dir + L"\\report.html"), std::ios::binary);
            f << html.str();
        }

        // ---- summary for the overlay: far-band detail and sky contamination per variant ----
        std::ostringstream o;
        if (!rr0.valid)
            o << T("REFERENCE NOT VALID: ", "REFERENCIA NO V\xC3\x81LIDA: ") << rr0.why_invalid << "\n";
        for (size_t i = 0; i < sets.size(); ++i)
        {
            if (i == ref_index)
                continue;
            o << sets[i].name << ": ";
            if (!results[i].valid)
            {
                o << T("not valid (", "no v\xC3\xA1lida (") << results[i].why_invalid << ")\n";
                continue;
            }
            // Temporal noise near the camera relative to DLSS-SR: the number that showed the grain of
            // "Full RR + accumulation 2" (x7) while the dot counter looked fine.
            const double noise_ratio = results[i].bands[0].noise / std::max(rr0.bands[0].noise, 1e-6);
            o << (noise_ratio >= 3.0 ? T("NOISE x", "RUIDO x") : T("noise x", "ruido x")) << fmt(noise_ratio, 1)
              << (noise_ratio >= 3.0 ? T(" (visible flicker), ", " (parpadeo visible), ") : ", ");
            o << T("far detail ", "detalle lejos ") << ratio(results[i].bands[2].fine, rr0.bands[2].fine) << T(", very far ", ", muy lejos ")
              << ratio(results[i].bands[3].fine, rr0.bands[3].fine) << T(", sky edges ", ", bordes con cielo ") << ev_diff(results[i].border_level, rr0.border_level)
              << " (" << fmt(100.0 * results[i].border_bright_fraction, 1) << " % vs " << fmt(100.0 * rr0.border_bright_fraction, 1)
              << T(" % as bright as the sky)\n", " % tan brillantes como el cielo)\n");
        }
        return o.str();
    }
}
