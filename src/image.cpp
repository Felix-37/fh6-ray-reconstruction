// Texel decoding, per-channel statistics, raw DDS dumps and 8-bit PNG previews (via WIC).

#include "common.hpp"

#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
    enum class kind { unorm8, snorm8, uint8, unorm16, snorm16, uint16, half, f32, u32, rgb10a2, r11g11b10, rgb9e5, d24s8, bgra8, bgrx8 };

    struct format_info
    {
        int bytes;
        int channels;
        kind k;
        const char *note;
    };

    bool get_info(DXGI_FORMAT f, format_info &o)
    {
        switch (int(f))
        {
        case 1: o = { 16, 4, kind::f32, "typeless read as float" }; return true;
        case 2: o = { 16, 4, kind::f32, "" }; return true;
        case 3: o = { 16, 4, kind::u32, "" }; return true;
        case 6: o = { 12, 3, kind::f32, "" }; return true;
        case 9: o = { 8, 4, kind::half, "typeless read as half" }; return true;
        case 10: o = { 8, 4, kind::half, "" }; return true;
        case 11: o = { 8, 4, kind::unorm16, "" }; return true;
        case 12: o = { 8, 4, kind::uint16, "" }; return true;
        case 13: o = { 8, 4, kind::snorm16, "" }; return true;
        case 15: o = { 8, 2, kind::f32, "typeless read as float" }; return true;
        case 16: o = { 8, 2, kind::f32, "" }; return true;
        case 17: o = { 8, 2, kind::u32, "" }; return true;
        case 19: case 20: case 21: o = { 8, 1, kind::f32, "depth plane read as float" }; return true;
        case 23: o = { 4, 4, kind::rgb10a2, "typeless read as unorm" }; return true;
        case 24: o = { 4, 4, kind::rgb10a2, "" }; return true;
        case 25: o = { 4, 4, kind::rgb10a2, "uint read as unorm" }; return true;
        case 26: o = { 4, 3, kind::r11g11b10, "" }; return true;
        case 27: o = { 4, 4, kind::unorm8, "typeless read as unorm" }; return true;
        case 28: o = { 4, 4, kind::unorm8, "" }; return true;
        case 29: o = { 4, 4, kind::unorm8, "sRGB-encoded values" }; return true;
        case 30: o = { 4, 4, kind::uint8, "" }; return true;
        case 31: o = { 4, 4, kind::snorm8, "" }; return true;
        case 33: o = { 4, 2, kind::half, "typeless read as half" }; return true;
        case 34: o = { 4, 2, kind::half, "" }; return true;
        case 35: o = { 4, 2, kind::unorm16, "" }; return true;
        case 36: o = { 4, 2, kind::uint16, "" }; return true;
        case 37: o = { 4, 2, kind::snorm16, "" }; return true;
        case 39: o = { 4, 1, kind::f32, "typeless read as float" }; return true;
        case 40: case 41: o = { 4, 1, kind::f32, "" }; return true;
        case 42: o = { 4, 1, kind::u32, "" }; return true;
        case 44: case 45: case 46: o = { 4, 2, kind::d24s8, "R = 24-bit depth, G = stencil" }; return true;
        case 48: o = { 2, 2, kind::unorm8, "typeless read as unorm" }; return true;
        case 49: o = { 2, 2, kind::unorm8, "" }; return true;
        case 50: o = { 2, 2, kind::uint8, "" }; return true;
        case 51: o = { 2, 2, kind::snorm8, "" }; return true;
        case 53: o = { 2, 1, kind::half, "typeless read as half" }; return true;
        case 54: o = { 2, 1, kind::half, "" }; return true;
        case 55: case 56: o = { 2, 1, kind::unorm16, "" }; return true;
        case 57: o = { 2, 1, kind::uint16, "" }; return true;
        case 60: o = { 1, 1, kind::unorm8, "typeless read as unorm" }; return true;
        case 61: case 65: o = { 1, 1, kind::unorm8, "" }; return true;
        case 62: o = { 1, 1, kind::uint8, "" }; return true;
        case 63: o = { 1, 1, kind::snorm8, "" }; return true;
        case 67: o = { 4, 3, kind::rgb9e5, "" }; return true;
        case 87: case 90: case 91: o = { 4, 4, kind::bgra8, "" }; return true;
        case 88: case 92: case 93: o = { 4, 3, kind::bgrx8, "" }; return true;
        default: return false;
        }
    }

    float half_to_float(uint16_t h)
    {
        const uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 0x1f, mant = h & 0x3ff;
        float v;
        if (exp == 0) v = std::ldexp(float(mant), -24);
        else if (exp == 31) v = mant ? NAN : INFINITY;
        else v = std::ldexp(float(mant | 0x400), int(exp) - 25);
        return sign ? -v : v;
    }

    float small_float(uint32_t bits, int mant_bits)
    {
        const uint32_t mant = bits & ((1u << mant_bits) - 1), exp = bits >> mant_bits;
        if (exp == 0) return std::ldexp(float(mant), -14 - mant_bits);
        if (exp == 31) return mant ? NAN : INFINITY;
        return std::ldexp(float(mant | (1u << mant_bits)), int(exp) - 15 - mant_bits);
    }

    void decode(const format_info &fi, const uint8_t *p, float out[4])
    {
        out[0] = out[1] = out[2] = 0; out[3] = 1;
        switch (fi.k)
        {
        case kind::unorm8: for (int c = 0; c < fi.channels; ++c) out[c] = p[c] / 255.0f; break;
        case kind::snorm8: for (int c = 0; c < fi.channels; ++c) out[c] = std::max(int8_t(p[c]) / 127.0f, -1.0f); break;
        case kind::uint8: for (int c = 0; c < fi.channels; ++c) out[c] = p[c]; break;
        case kind::unorm16: for (int c = 0; c < fi.channels; ++c) { uint16_t v; memcpy(&v, p + 2 * c, 2); out[c] = v / 65535.0f; } break;
        case kind::snorm16: for (int c = 0; c < fi.channels; ++c) { int16_t v; memcpy(&v, p + 2 * c, 2); out[c] = std::max(v / 32767.0f, -1.0f); } break;
        case kind::uint16: for (int c = 0; c < fi.channels; ++c) { uint16_t v; memcpy(&v, p + 2 * c, 2); out[c] = v; } break;
        case kind::half: for (int c = 0; c < fi.channels; ++c) { uint16_t v; memcpy(&v, p + 2 * c, 2); out[c] = half_to_float(v); } break;
        case kind::f32: for (int c = 0; c < fi.channels; ++c) memcpy(&out[c], p + 4 * c, 4); break;
        case kind::u32: for (int c = 0; c < fi.channels; ++c) { uint32_t v; memcpy(&v, p + 4 * c, 4); out[c] = float(v); } break;
        case kind::rgb10a2:
        {
            uint32_t v; memcpy(&v, p, 4);
            out[0] = (v & 0x3ff) / 1023.0f; out[1] = ((v >> 10) & 0x3ff) / 1023.0f;
            out[2] = ((v >> 20) & 0x3ff) / 1023.0f; out[3] = (v >> 30) / 3.0f;
            break;
        }
        case kind::r11g11b10:
        {
            uint32_t v; memcpy(&v, p, 4);
            out[0] = small_float(v & 0x7ff, 6); out[1] = small_float((v >> 11) & 0x7ff, 6); out[2] = small_float(v >> 22, 5);
            break;
        }
        case kind::rgb9e5:
        {
            uint32_t v; memcpy(&v, p, 4);
            const float scale = std::ldexp(1.0f, int(v >> 27) - 24);
            out[0] = (v & 0x1ff) * scale; out[1] = ((v >> 9) & 0x1ff) * scale; out[2] = ((v >> 18) & 0x1ff) * scale;
            break;
        }
        case kind::d24s8:
        {
            uint32_t v; memcpy(&v, p, 4);
            out[0] = (v & 0xffffff) / 16777215.0f; out[1] = float(v >> 24);
            break;
        }
        case kind::bgra8: out[0] = p[2] / 255.0f; out[1] = p[1] / 255.0f; out[2] = p[0] / 255.0f; out[3] = p[3] / 255.0f; break;
        case kind::bgrx8: out[0] = p[2] / 255.0f; out[1] = p[1] / 255.0f; out[2] = p[0] / 255.0f; break;
        }
    }

    bool is_float_kind(kind k) { return k == kind::half || k == kind::f32 || k == kind::r11g11b10 || k == kind::rgb9e5; }

    bool save_png(const std::wstring &path, uint32_t width, uint32_t height, const std::vector<uint8_t> &pixels, bool gray)
    {
        IWICImagingFactory *factory = nullptr;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
            return false;
        bool ok = false;
        IWICStream *stream = nullptr;
        IWICBitmapEncoder *encoder = nullptr;
        IWICBitmapFrameEncode *frame = nullptr;
        if (SUCCEEDED(factory->CreateStream(&stream)) &&
            SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
            SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
            SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache)) &&
            SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) &&
            SUCCEEDED(frame->Initialize(nullptr)) &&
            SUCCEEDED(frame->SetSize(width, height)))
        {
            WICPixelFormatGUID format = gray ? GUID_WICPixelFormat8bppGray : GUID_WICPixelFormat24bppBGR;
            const UINT stride = width * (gray ? 1 : 3);
            ok = SUCCEEDED(frame->SetPixelFormat(&format)) &&
                 SUCCEEDED(frame->WritePixels(height, stride, UINT(pixels.size()), const_cast<BYTE *>(pixels.data()))) &&
                 SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
        }
        if (frame) frame->Release();
        if (encoder) encoder->Release();
        if (stream) stream->Release();
        factory->Release();
        return ok;
    }

    uint8_t to8(float v) { return uint8_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); }
}

namespace rr::image
{
    analysis analyze(DXGI_FORMAT format, uint32_t width, uint32_t height, const uint8_t *data, uint64_t row_size)
    {
        analysis a;
        format_info fi;
        if (!get_info(format, fi))
            return a;
        a.supported = true;
        a.channels = fi.channels;
        a.decode_note = fi.note;
        double sum[4] = {}, len_raw = 0, len_unorm = 0;
        uint64_t finite[4] = {}, len_count = 0;
        double mn[4], mx[4];
        for (int c = 0; c < 4; ++c) { mn[c] = INFINITY; mx[c] = -INFINITY; }
        for (uint32_t y = 0; y < height; ++y)
        {
            const uint8_t *row = data + size_t(y) * row_size;
            for (uint32_t x = 0; x < width; ++x)
            {
                float t[4];
                decode(fi, row + size_t(x) * fi.bytes, t);
                bool all_finite = true;
                for (int c = 0; c < fi.channels; ++c)
                {
                    if (!std::isfinite(t[c])) { a.stats[c].non_finite++; all_finite = false; continue; }
                    sum[c] += t[c]; finite[c]++;
                    mn[c] = std::min(mn[c], double(t[c])); mx[c] = std::max(mx[c], double(t[c]));
                }
                if (fi.channels >= 3 && all_finite)
                {
                    len_raw += std::sqrt(double(t[0]) * t[0] + double(t[1]) * t[1] + double(t[2]) * t[2]);
                    const double u0 = t[0] * 2.0 - 1, u1 = t[1] * 2.0 - 1, u2 = t[2] * 2.0 - 1;
                    len_unorm += std::sqrt(u0 * u0 + u1 * u1 + u2 * u2);
                    len_count++;
                }
            }
        }
        for (int c = 0; c < fi.channels; ++c)
        {
            a.stats[c].min = finite[c] ? mn[c] : 0;
            a.stats[c].max = finite[c] ? mx[c] : 0;
            a.stats[c].mean = finite[c] ? sum[c] / double(finite[c]) : 0;
        }
        if (len_count)
        {
            a.mean_length_raw = len_raw / double(len_count);
            a.mean_length_unorm = len_unorm / double(len_count);
        }
        return a;
    }

    bool write_dds(const std::wstring &path, DXGI_FORMAT format, uint32_t width, uint32_t height, const uint8_t *data, uint64_t row_size)
    {
        uint32_t header[32 + 5] = {};
        header[0] = 0x20534444;           // "DDS "
        header[1] = 124;                  // dwSize
        header[2] = 0x1 | 0x2 | 0x4 | 0x8 | 0x1000;   // CAPS | HEIGHT | WIDTH | PITCH | PIXELFORMAT
        header[3] = height;
        header[4] = width;
        header[5] = uint32_t(row_size);
        header[7] = 1;                    // mip count
        header[19] = 32;                  // ddspf.dwSize
        header[20] = 0x4;                 // DDPF_FOURCC
        header[21] = 0x30315844;          // "DX10"
        header[27] = 0x1000;              // DDSCAPS_TEXTURE
        header[32] = uint32_t(format);    // DDS_HEADER_DXT10
        header[33] = 3;                   // D3D10_RESOURCE_DIMENSION_TEXTURE2D
        header[35] = 1;                   // array size
        FILE *f = _wfopen(path.c_str(), L"wb");
        if (!f)
            return false;
        fwrite(header, sizeof(header), 1, f);
        fwrite(data, 1, size_t(row_size) * height, f);
        fclose(f);
        return true;
    }

    bool write_previews(const std::wstring &base_path, DXGI_FORMAT format, uint32_t width, uint32_t height,
                        const uint8_t *data, uint64_t row_size, const analysis &info)
    {
        format_info fi;
        if (!get_info(format, fi))
            return false;

        // Per-channel mapping to 0..1: scale * v + offset.
        float scale[4], offset[4];
        const bool floating = is_float_kind(fi.k);
        double rgb_min = INFINITY, rgb_max = -INFINITY;
        for (int c = 0; c < std::min(fi.channels, 3); ++c)
        {
            rgb_min = std::min(rgb_min, info.stats[c].min);
            rgb_max = std::max(rgb_max, info.stats[c].max);
        }
        bool reinhard = false;
        for (int c = 0; c < 4; ++c)
        {
            scale[c] = 1; offset[c] = 0;
            const double lo = info.stats[c].min, hi = info.stats[c].max;
            const bool normalize = fi.channels <= 2 || c == 3 || fi.k == kind::uint8 || fi.k == kind::uint16 || fi.k == kind::u32 || fi.k == kind::d24s8;
            if (normalize || (floating && fi.channels <= 2))
            {
                if (hi - lo > 1e-9) { scale[c] = float(1.0 / (hi - lo)); offset[c] = float(-lo / (hi - lo)); }
                else { scale[c] = 0; offset[c] = 0.5f; }
            }
            else if (fi.k == kind::snorm8 || fi.k == kind::snorm16)
            {
                scale[c] = 0.5f; offset[c] = 0.5f;
            }
            else if (floating)
            {
                if (rgb_min >= 0 && rgb_max <= 1.0001) { /* raw */ }
                else if (rgb_min >= 0) reinhard = true;
                else { scale[c] = 0.5f; offset[c] = 0.5f; }
            }
        }

        const uint32_t step = std::max(1u, (width + 959) / 960);
        const uint32_t w = width / step, h = height / step;
        std::vector<uint8_t> rgb(size_t(w) * h * 3), alpha;
        const bool want_alpha = fi.channels == 4 && info.stats[3].max - info.stats[3].min > 1e-6;
        if (want_alpha)
            alpha.resize(size_t(w) * h);
        for (uint32_t y = 0; y < h; ++y)
        {
            const uint8_t *row = data + size_t(y) * step * row_size;
            for (uint32_t x = 0; x < w; ++x)
            {
                float t[4];
                decode(fi, row + size_t(x) * step * fi.bytes, t);
                float m[4];
                for (int c = 0; c < 4; ++c)
                {
                    const float v = std::isfinite(t[c]) ? t[c] : 0.0f;
                    m[c] = (reinhard && c < 3) ? v / (1.0f + v) : v * scale[c] + offset[c];
                }
                if (fi.channels == 1) m[1] = m[2] = m[0];
                if (fi.channels == 2) m[2] = 0;
                uint8_t *px = &rgb[(size_t(y) * w + x) * 3];
                px[0] = to8(m[2]); px[1] = to8(m[1]); px[2] = to8(m[0]);   // BGR
                if (want_alpha)
                    alpha[size_t(y) * w + x] = to8(m[3]);
            }
        }
        bool ok = save_png(base_path + L".png", w, h, rgb, false);
        if (want_alpha)
            ok &= save_png(base_path + L"_a.png", w, h, alpha, true);
        return ok;
    }
}
