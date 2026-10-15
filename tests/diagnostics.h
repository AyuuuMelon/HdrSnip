static void require(bool ok, const char *s) {
    if (!ok)
        throw std::runtime_error(s);
}
static void verifyHdr(const fs::path &path, const Image &image) {
    auto factory = wic();
    ComPtr<IWICBitmapDecoder> decoder;
    check(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                             WICDecodeMetadataCacheOnLoad, &decoder),
          "Read saved HDR");
    ComPtr<IWICBitmapFrameDecode> frame;
    check(decoder->GetFrame(0, &frame), "Saved HDR frame");
    UINT w, h;
    check(frame->GetSize(&w, &h), "Saved HDR dimensions");
    require(w == (UINT)image.w && h == (UINT)image.h, "Saved HDR dimensions differ");
    WICPixelFormatGUID pf;
    check(frame->GetPixelFormat(&pf), "Saved HDR format");
    require(pf == GUID_WICPixelFormat64bppRGBAHalf, "Saved HDR precision differs");
    std::vector<HALF> pixels(image.rgba.size());
    check(frame->CopyPixels(nullptr, w * 8, (UINT)pixels.size() * 2, (BYTE *)pixels.data()),
          "Saved HDR pixels");
    if (pixels != image.rgba) {
        std::ofstream diagnostics(fs::path(path.wstring() + L".diff.txt"));
        size_t count = 0, numerical = 0;
        for (size_t i = 0; i < pixels.size(); ++i)
            if (pixels[i] != image.rgba[i]) {
                float before = XMConvertHalfToFloat(image.rgba[i]), after = XMConvertHalfToFloat(pixels[i]);
                if (before != after)
                    ++numerical;
                if (count < 20)
                    diagnostics << i << " source_half=" << image.rgba[i] << " decoded_half=" << pixels[i]
                                << " source=" << before << " decoded=" << after << '\n';
                ++count;
            }
        diagnostics << "bit_differences=" << count << " numeric_differences=" << numerical << '\n';
        if (numerical)
            throw std::runtime_error("Saved HDR pixel values differ; see .diff.txt");
    }
}
static int selfTest(const fs::path &dir, bool live, HMONITOR forced = nullptr) {
    fs::create_directories(dir);
    std::ofstream report(dir / L"test-results.txt");
    try {
        Image img;
        img.w = 32;
        img.h = 24;
        img.hdr = true;
        img.rgba.resize(32 * 24 * 4);
        for (int y = 0; y < img.h; ++y)
            for (int x = 0; x < img.w; ++x) {
                size_t i = ((size_t)y * img.w + x) * 4;
                img.rgba[i] = XMConvertFloatToHalf(x * .5f);
                img.rgba[i + 1] = XMConvertFloatToHalf(y * .25f);
                img.rgba[i + 2] = XMConvertFloatToHalf(-.125f);
                img.rgba[i + 3] = XMConvertFloatToHalf(1.f);
            }
        auto cut = crop(img, {3, 4, 19, 16});
        require(cut.w == 16 && cut.h == 12, "crop dimensions");
        require(cut.rgba[0] == img.rgba[(4 * 32 + 3) * 4], "crop origin");
        auto jxr = dir / L"hdr-roundtrip.jxr";
        encode(jxr, cut, true);
        auto f = wic();
        ComPtr<IWICBitmapDecoder> dec;
        check(f->CreateDecoderFromFilename(jxr.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad,
                                           &dec),
              "Decode JXR");
        ComPtr<IWICBitmapFrameDecode> frame;
        check(dec->GetFrame(0, &frame), "Decode frame");
        WICPixelFormatGUID pf;
        check(frame->GetPixelFormat(&pf), "Read decoded format");
        require(pf == GUID_WICPixelFormat64bppRGBAHalf, "HDR precision lost");
        std::vector<HALF> data(cut.rgba.size());
        check(frame->CopyPixels(nullptr, cut.w * 8, (UINT)data.size() * 2, (BYTE *)data.data()),
              "Read decoded HDR");
        require(data == cut.rgba, "Lossless HDR pixels changed");
        report << "PASS lossless JXR half-float roundtrip (negative + >1 highlights), crop origin and "
                  "dimensions\n";
        encode(dir / L"preview.png", cut, false);
        auto p = toneMap(cut);
        require(p[3] == 255, "PNG alpha");
        report << "PASS PNG encoding and SDR tone map\n";
        Image patches;
        patches.w = 5;
        patches.h = 1;
        patches.hdr = true;
        patches.sdrWhite = 2.5f;
        const float values[] = {.18f, .18f, .18f, 1, 1, 1, 1, 1, 4, 4, 4, 1, 2, 1, .5f, 1, -.2f, .8f, .4f, 1};
        for (size_t i = 0; i < 20; ++i)
            patches.rgba.push_back(XMConvertFloatToHalf(values[i] * (i % 4 == 3 ? 1.f : 2.5f)));
        auto mapped = toneMap(patches);
        require(abs((int)mapped[0] - 118) <= 1, "SDR-white normalization changed midgray");
        require(mapped[4] > 230 && mapped[4] < 250, "Diffuse white is blown or too dark");
        require(mapped[8] > mapped[4], "Highlight detail lost");
        auto linear = [](BYTE b) {
            float v = b / 255.f;
            return v <= .04045f ? v / 12.92f : powf((v + .055f) / 1.055f, 2.4f);
        };
        float rr = linear(mapped[14]), gg = linear(mapped[13]), bb = linear(mapped[12]);
        require(fabsf(rr / gg - 2) < .04f && fabsf(gg / bb - 2) < .04f, "Saturated highlight hue shifted");
        require(mapped[14] < 255, "Saturated highlight channel clipped");
        Image normalized = patches;
        normalized.sdrWhite = 1;
        for (size_t i = 0; i < 20; ++i)
            normalized.rgba[i] = XMConvertFloatToHalf(values[i]);
        auto equal = toneMap(normalized);
        for (size_t i = 0; i < mapped.size(); ++i)
            require(abs((int)mapped[i] - equal[i]) <= 1, "White-level invariance failed");
        normalized.hdr = false;
        auto sdr = toneMap(normalized);
        require(abs((int)sdr[0] - 118) <= 1 && sdr[4] == 255, "SDR path changed");
        report << "PASS white-level normalization, midgray, highlight separation, saturated hue ratios, "
                  "negative gamut, SDR bypass\n";
        ComPtr<IWICBitmapDecoder> pngDecoder;
        check(f->CreateDecoderFromFilename((dir / L"preview.png").c_str(), nullptr, GENERIC_READ,
                                           WICDecodeMetadataCacheOnLoad, &pngDecoder),
              "Read PNG metadata");
        ComPtr<IWICBitmapFrameDecode> pngFrame;
        check(pngDecoder->GetFrame(0, &pngFrame), "PNG frame");
        ComPtr<IWICMetadataQueryReader> pngMetadata;
        check(pngFrame->GetMetadataQueryReader(&pngMetadata), "PNG metadata reader");
        PROPVARIANT tag{};
        check(pngMetadata->GetMetadataByName(L"/sRGB/RenderingIntent", &tag), "sRGB tag missing");
        require(tag.vt == VT_UI1 && tag.bVal == 0, "Invalid PNG sRGB tag");
        PropVariantClear(&tag);
        report << "PASS explicit PNG sRGB metadata\n";
        Image tiny;
        tiny.w = 16;
        tiny.h = 16;
        tiny.hdr = true;
        tiny.rgba.resize(16 * 16 * 4);
        HALF patterns[] = {0, 0x8000, 1, 0x8001, 2, 0x8002, 0x3bff, 0x3c00, 0x7bff, 0xfbff};
        for (size_t i = 0; i < tiny.rgba.size(); i += 4) {
            for (int c = 0; c < 3; ++c)
                tiny.rgba[i + c] = patterns[(i / 4 + c) % 10];
            tiny.rgba[i + 3] = XMConvertFloatToHalf(1);
        }
        encode(dir / L"half-edge-cases.jxr", tiny, true);
        verifyHdr(dir / L"half-edge-cases.jxr", tiny);
        report << "PASS HDR half edge cases roundtrip\n";
        if (live) {
            POINT cursor;
            GetCursorPos(&cursor);
            auto screen = capture(forced ? forced : MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST));
            report << "LIVE display " << screen.w << "x" << screen.h << " HDR=" << screen.hdr
                   << " format=" << screen.format << " SDR_white_multiplier=" << screen.sdrWhite << "\n";
            float max = 0;
            size_t peak = 0;
            for (size_t i = 0; i < screen.rgba.size(); ++i)
                if (i % 4 != 3 && XMConvertHalfToFloat(screen.rgba[i]) > max) {
                    max = XMConvertHalfToFloat(screen.rgba[i]);
                    peak = i / 4;
                }
            LONG x = std::clamp<LONG>((LONG)(peak % screen.w) - 64, 0, std::max(0, screen.w - 128)),
                 y = std::clamp<LONG>((LONG)(peak / screen.w) - 48, 0, std::max(0, screen.h - 96));
            auto sample =
                crop(screen, {x, y, std::min<LONG>(x + 128, screen.w), std::min<LONG>(y + 96, screen.h)});
            auto path = dir / (screen.hdr ? L"live.jxr" : L"live.png");
            encode(path, sample, screen.hdr);
            if (screen.hdr)
                verifyHdr(path, sample);
            report << "PASS live capture, crop, encode" << (screen.hdr ? ", exact HDR file roundtrip" : "")
                   << "; max linear RGB=" << max << "\n";
        }
        Image colors;
        colors.w = 256;
        colors.h = 256;
        colors.hdr = true;
        colors.sdrWhite = 6;
        colors.rgba.resize(256 * 256 * 4);
        for (size_t i = 0; i < colors.rgba.size(); i += 4) {
            unsigned seed = (unsigned)(i / 4);
            colors.rgba[i] = (HALF)seed;
            colors.rgba[i + 1] = (HALF)(seed * 257u + 171u);
            colors.rgba[i + 2] = (HALF)(seed * 131u + 993u);
            colors.rgba[i + 3] = XMConvertFloatToHalf(1);
        }
        auto reference = toneMapReference(colors);
        auto fast = toneMap(colors);
        for (size_t i = 0; i < fast.size(); ++i)
            require(abs((int)reference[i] - fast[i]) <= 1,
                    "Fast tone map changed color by more than one byte level");
        Image large = colors;
        large.w = 1024;
        large.h = 1024;
        large.rgba.resize(1024 * 1024 * 4);
        for (size_t i = 0; i < large.rgba.size(); ++i)
            large.rgba[i] = colors.rgba[i % colors.rgba.size()];
        auto parallel = toneMap(large);
        for (size_t i = 0; i < parallel.size(); ++i)
            require(abs((int)parallel[i] - reference[i % reference.size()]) <= 1,
                    "Parallel tone-map output differs");
        report << "PASS fast/parallel tone map matches reference within 1/255 over half-float patterns, "
                  "including negative/nonfinite values\n";
        editorUnitTests();
        report << "PASS synthetic redaction changes actual HDR pixels, preserves outside pixels and exact "
                  "crop\n";
        return 0;
    } catch (const std::exception &e) {
        report << "FAIL " << e.what() << "\n";
        return 1;
    }
}
#include "editor_tests.h"
#include "latency_tests.h"

static int runDiagnostics(const std::vector<std::wstring> &args) {
    if (args.size() > 1 && args[0] == L"--frame-test")
        return frameTest(args[1]);
    if (args.size() > 1 && args[0] == L"--test-hdr-display") {
        try {
            ComPtr<IDXGIFactory1> factory;
            check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "Display factory");
            for (UINT a = 0;; ++a) {
                ComPtr<IDXGIAdapter1> adapter;
                if (factory->EnumAdapters1(a, &adapter) == DXGI_ERROR_NOT_FOUND)
                    break;
                for (UINT o = 0;; ++o) {
                    ComPtr<IDXGIOutput> out;
                    if (adapter->EnumOutputs(o, &out) == DXGI_ERROR_NOT_FOUND)
                        break;
                    ComPtr<IDXGIOutput6> out6;
                    if (FAILED(out.As(&out6)))
                        continue;
                    DXGI_OUTPUT_DESC1 d{};
                    if (SUCCEEDED(out6->GetDesc1(&d)) &&
                        d.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020)
                        return selfTest(args[1], true, d.Monitor);
                }
            }
            return 2;
        } catch (...) {
            return 1;
        }
    }
    if (!args.empty() && (args[0] == L"--self-test" || args[0] == L"--capture-test"))
        return selfTest(args.size() > 1 ? fs::path(args[1]) : root / L"tests", args[0] == L"--capture-test");
    return -1;
}
