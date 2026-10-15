#include <windows.h>
#include <DirectXPackedVector.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <commctrl.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <filesystem>
#include <fstream>
#include <psapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <wincodec.h>
#include <windowsx.h>
#include <wrl/client.h>
#include <thread>
#include <atomic>
#include <dwmapi.h>
#include <array>
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
using namespace DirectX::PackedVector;

static void check(HRESULT hr, const char *what) {
    if (FAILED(hr)) {
        std::ostringstream s;
        s << what << " (0x" << std::hex << (unsigned)hr << ")";
        throw std::runtime_error(s.str());
    }
}
static std::wstring wide(const std::string &s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
struct Image {
    int w = 0, h = 0;
    bool hdr = false;
    RECT desktop{};
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    float sdrWhite = 1.f;
    std::vector<HALF> rgba;
};
static fs::path root, ini, output;
static HWND host = nullptr, overlay = nullptr, settings = nullptr;
static UINT modifiers = MOD_CONTROL | MOD_SHIFT, key = 'S';
static int activeHotkey = 1;
static bool hotkeyRegistered = false;
static Image frozen;
static std::vector<BYTE> preview, darkPreview;
static POINT start{}, finish{};
static bool dragging = false, busy = false;
#ifdef HDRSNIP_TEST_BUILD
static bool integration = false;
#else
static bool integration = false;
#endif
static int integrationResult = 1;
static HFONT font = nullptr;
static UINT taskbarMessage;
static NOTIFYICONDATAW tray{};
static constexpr UINT WM_TRAY = WM_APP + 1;
#ifdef HDRSNIP_TEST_BUILD
static void (*captureMemoryHook)() = nullptr;
#endif
static long long qpcNow() {
    LARGE_INTEGER q{};
    QueryPerformanceCounter(&q);
    return q.QuadPart;
}
static double qpcMs(long long ticks) {
    static double hz = []() {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return (double)f.QuadPart;
    }();
    return ticks * 1000. / hz;
}
struct CaptureTiming {
    long long requested = 0, first = 0, chosen = 0, acquired = 0, readback = 0, finished = 0;
    int images = 0;
};
static CaptureTiming captureTiming;
struct CaptureDevice {
    LUID adapter{};
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
};
static std::vector<CaptureDevice> captureDevices;
static CaptureDevice &deviceFor(IDXGIAdapter1 *adapter) {
    DXGI_ADAPTER_DESC1 desc{};
    check(adapter->GetDesc1(&desc), "Capture adapter identity");
    for (auto &entry : captureDevices)
        if (entry.adapter.HighPart == desc.AdapterLuid.HighPart &&
            entry.adapter.LowPart == desc.AdapterLuid.LowPart &&
            SUCCEEDED(entry.device->GetDeviceRemovedReason()))
            return entry;
    CaptureDevice entry;
    entry.adapter = desc.AdapterLuid;
    check(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                            nullptr, 0, D3D11_SDK_VERSION, &entry.device, nullptr, &entry.context),
          "Create capture device");
    // Keep one device per adapter; capture textures and duplication stay per-shot.
    for (auto &old : captureDevices)
        if (old.adapter.HighPart == entry.adapter.HighPart && old.adapter.LowPart == entry.adapter.LowPart) {
            old = std::move(entry);
            return old;
        }
    captureDevices.push_back(std::move(entry));
    return captureDevices.back();
}
static std::wstring errorMessage(const std::exception &e) {
    return L"截图未完成：\n" + wide(e.what()) +
           L"\n\n可重试一次；若在独占全屏游戏中，请切换为无边框窗口。锁屏、受保护视频或远程会话可能无法捕获"
           L"。";
}
static RECT selection() {
    return {std::min(start.x, finish.x), std::min(start.y, finish.y), std::max(start.x, finish.x),
            std::max(start.y, finish.y)};
}
static void notify(const std::wstring &text, bool error = false) {
    tray.uFlags = NIF_INFO;
    tray.dwInfoFlags = error ? NIIF_ERROR : NIIF_INFO;
    wcscpy_s(tray.szInfoTitle, L"微光 · HDR 截图");
    wcsncpy_s(tray.szInfo, text.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &tray);
}

// DXGI duplication keeps scRGB half floats on HDR desktops; no 8-bit HDR fallback.
static float sdrWhiteForDisplay(const wchar_t *name) {
    for (int attempt = 0; attempt < 3; ++attempt) {
        UINT pc = 0, mc = 0;
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pc, &mc) != ERROR_SUCCESS)
            break;
        std::vector<DISPLAYCONFIG_PATH_INFO> paths(pc);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(mc);
        LONG status =
            QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pc, paths.data(), &mc, modes.data(), nullptr);
        if (status == ERROR_INSUFFICIENT_BUFFER)
            continue;
        if (status != ERROR_SUCCESS)
            break;
        for (UINT i = 0; i < pc; ++i) {
            DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
            source.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, sizeof(source),
                             paths[i].sourceInfo.adapterId, paths[i].sourceInfo.id};
            if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS ||
                _wcsicmp(source.viewGdiDeviceName, name) != 0)
                continue;
            DISPLAYCONFIG_SDR_WHITE_LEVEL white{};
            white.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL, sizeof(white),
                            paths[i].targetInfo.adapterId, paths[i].targetInfo.id};
            if (DisplayConfigGetDeviceInfo(&white.header) == ERROR_SUCCESS && white.SDRWhiteLevel >= 1000 &&
                white.SDRWhiteLevel <= 10000)
                return white.SDRWhiteLevel / 1000.f;
        }
        break;
    }
    return 1.f; // Windows scRGB nominal white is 80 nits when unavailable.
}
static Image capture(HMONITOR monitor) {
    captureTiming = {};
    captureTiming.requested = qpcNow();
    ComPtr<IDXGIFactory1> factory;
    check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "Create DXGI factory");
    for (UINT a = 0;; ++a) {
        ComPtr<IDXGIAdapter1> adapter;
        HRESULT ar = factory->EnumAdapters1(a, &adapter);
        if (ar == DXGI_ERROR_NOT_FOUND)
            break;
        check(ar, "Enumerate adapter");
        for (UINT o = 0;; ++o) {
            ComPtr<IDXGIOutput> out;
            HRESULT r = adapter->EnumOutputs(o, &out);
            if (r == DXGI_ERROR_NOT_FOUND)
                break;
            check(r, "Enumerate display");
            DXGI_OUTPUT_DESC od{};
            check(out->GetDesc(&od), "Display description");
            if (od.Monitor != monitor)
                continue;
            ComPtr<IDXGIOutput6> out6;
            bool hdr = false;
            if (SUCCEEDED(out.As(&out6))) {
                DXGI_OUTPUT_DESC1 d{};
                check(out6->GetDesc1(&d), "HDR display info");
                hdr = d.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
            }
            float displayWhite = hdr ? sdrWhiteForDisplay(od.DeviceName) : 1.f;
            auto &cached = deviceFor(adapter.Get());
            ComPtr<ID3D11Device> device = cached.device;
            ComPtr<ID3D11DeviceContext> context = cached.context;
            struct FlushCapture {
                ID3D11Device *device;
                ID3D11DeviceContext *context;
                ~FlushCapture() {
                    context->ClearState();
                    context->Flush();
                    ComPtr<IDXGIDevice3> idle;
                    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&idle))))
                        idle->Trim();
                }
            } flushCapture{device.Get(), context.Get()};
            ComPtr<IDXGIOutput5> out5;
            check(out.As(&out5), "Windows 10 DXGI 1.5 required");
            DXGI_FORMAT formats[] = {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_B8G8R8A8_UNORM};
            ComPtr<IDXGIOutputDuplication> dupl;
            check(out5->DuplicateOutput1(device.Get(), 0, 2, formats, &dupl), "Start desktop duplication");
            ComPtr<IDXGIResource> resource;
            DXGI_OUTDUPL_FRAME_INFO info{};
            // A newly created duplication can initially deliver pointer-only updates.
            auto deadline = GetTickCount64() + 2500;
            while (true) {
                r = dupl->AcquireNextFrame(150, &info, &resource);
                if (r == DXGI_ERROR_WAIT_TIMEOUT) {
                    if (GetTickCount64() < deadline)
                        continue;
                    check(r, "Timed out waiting for desktop");
                }
                check(r, "Acquire desktop frame");
                if (info.LastPresentTime.QuadPart)
                    break;
                if (GetTickCount64() >= deadline) {
                    dupl->ReleaseFrame();
                    throw std::runtime_error("No desktop image received, only pointer updates");
                }
                resource.Reset();
                check(dupl->ReleaseFrame(), "Release pointer frame");
            }
            struct Release {
                IDXGIOutputDuplication *p;
                ~Release() {
                    if (p)
                        p->ReleaseFrame();
                }
            } release{dupl.Get()};
            captureTiming.first = captureTiming.chosen = info.LastPresentTime.QuadPart;
            captureTiming.acquired = qpcNow();
            captureTiming.images = 1;
            ComPtr<ID3D11Texture2D> source;
            check(resource.As(&source), "Desktop texture");
            D3D11_TEXTURE2D_DESC td{};
            source->GetDesc(&td);
            if (td.Format != DXGI_FORMAT_R16G16B16A16_FLOAT && td.Format != DXGI_FORMAT_B8G8R8A8_UNORM)
                throw std::runtime_error(
                    "Unsupported desktop pixel format; refusing an incorrect HDR conversion");
            if (hdr && td.Format != DXGI_FORMAT_R16G16B16A16_FLOAT)
                throw std::runtime_error(
                    "HDR is enabled but driver returned 8-bit pixels; refusing clipped HDR output");
            D3D11_TEXTURE2D_DESC sd = td;
            sd.Usage = D3D11_USAGE_STAGING;
            sd.BindFlags = 0;
            sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            sd.MiscFlags = 0;
            ComPtr<ID3D11Texture2D> staging;
            check(device->CreateTexture2D(&sd, nullptr, &staging), "Create readback texture");
            context->CopyResource(staging.Get(), source.Get());
            // A new duplication can begin with an older compositor image. Preserve it
            // only as a static-desktop fallback, then prefer a post-request desktop update.
            // Every candidate is copied while owned, before ReleaseFrame invalidates it.
            resource.Reset();
            source.Reset();
            check(dupl->ReleaseFrame(), "Release initial snapshot");
            release.p = nullptr;
            long long refreshDeadline = qpcNow();
            LARGE_INTEGER frequency{};
            QueryPerformanceFrequency(&frequency);
            refreshDeadline += frequency.QuadPart / 20;
            for (int attempts = 0; attempts < 12; ++attempts) {
                bool old = captureTiming.chosen < captureTiming.requested;
                UINT timeout = old && qpcNow() < refreshDeadline ? 5 : 0;
                DXGI_OUTDUPL_FRAME_INFO next{};
                ComPtr<IDXGIResource> candidate;
                HRESULT nextHr = dupl->AcquireNextFrame(timeout, &next, &candidate);
                if (nextHr == DXGI_ERROR_WAIT_TIMEOUT) {
                    if (old && qpcNow() < refreshDeadline)
                        continue;
                    break;
                }
                check(nextHr, "Acquire latest desktop snapshot");
                release.p = dupl.Get();
                if (next.LastPresentTime.QuadPart > captureTiming.chosen) {
                    ComPtr<ID3D11Texture2D> texture;
                    check(candidate.As(&texture), "Latest desktop texture");
                    D3D11_TEXTURE2D_DESC latest{};
                    texture->GetDesc(&latest);
                    if (latest.Width != td.Width || latest.Height != td.Height || latest.Format != td.Format)
                        throw std::runtime_error("Display changed during capture; retry");
                    context->CopyResource(staging.Get(), texture.Get());
                    captureTiming.chosen = next.LastPresentTime.QuadPart;
                    ++captureTiming.images;
                }
                candidate.Reset();
                check(dupl->ReleaseFrame(), "Release latest snapshot");
                release.p = nullptr;
                if (!next.LastPresentTime.QuadPart && timeout == 0)
                    break;
            }
            captureTiming.acquired = qpcNow();
            D3D11_MAPPED_SUBRESOURCE map{};
            check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map), "Read HDR pixels");
            struct Unmap {
                ID3D11DeviceContext *c;
                ID3D11Texture2D *t;
                ~Unmap() {
                    c->Unmap(t, 0);
                }
            } unmap{context.Get(), staging.Get()};
            captureTiming.readback = qpcNow();
#ifdef HDRSNIP_TEST_BUILD
            if (captureMemoryHook)
                captureMemoryHook();
#endif
            Image img;
            img.desktop = od.DesktopCoordinates;
            img.w = img.desktop.right - img.desktop.left;
            img.h = img.desktop.bottom - img.desktop.top;
            img.hdr = hdr;
            img.format = td.Format;
            img.sdrWhite = displayWhite;
            img.rgba.resize((size_t)img.w * img.h * 4);
            if (td.Format == DXGI_FORMAT_R16G16B16A16_FLOAT &&
                (od.Rotation == DXGI_MODE_ROTATION_IDENTITY ||
                 od.Rotation == DXGI_MODE_ROTATION_UNSPECIFIED) &&
                td.Width == (UINT)img.w && td.Height == (UINT)img.h) {
                for (int y = 0; y < img.h; ++y)
                    memcpy(img.rgba.data() + (size_t)y * img.w * 4,
                           (const BYTE *)map.pData + (size_t)y * map.RowPitch, (size_t)img.w * 8);
                for (size_t i = 3; i < img.rgba.size(); i += 4)
                    img.rgba[i] = XMConvertFloatToHalf(1.f);
                captureTiming.finished = qpcNow();
                return img;
            }
            for (int y = 0; y < img.h; ++y)
                for (int x = 0; x < img.w; ++x) {
                    int sx = x, sy = y;
                    switch (od.Rotation) {
                    case DXGI_MODE_ROTATION_ROTATE90:
                        sx = y;
                        sy = (int)td.Height - 1 - x;
                        break;
                    case DXGI_MODE_ROTATION_ROTATE180:
                        sx = (int)td.Width - 1 - x;
                        sy = (int)td.Height - 1 - y;
                        break;
                    case DXGI_MODE_ROTATION_ROTATE270:
                        sx = (int)td.Width - 1 - y;
                        sy = x;
                        break;
                    default:
                        break;
                    }
                    if (sx < 0 || sy < 0 || sx >= (int)td.Width || sy >= (int)td.Height)
                        throw std::runtime_error("Display rotation dimensions changed; please retry");
                    auto dst = &img.rgba[((size_t)y * img.w + x) * 4];
                    const BYTE *row = (const BYTE *)map.pData + (size_t)sy * map.RowPitch;
                    if (td.Format == DXGI_FORMAT_R16G16B16A16_FLOAT)
                        memcpy(dst, row + (size_t)sx * 8, 8);
                    else {
                        const BYTE *p = row + (size_t)sx * 4;
                        for (int c = 0; c < 3; ++c) {
                            float v = p[2 - c] / 255.f;
                            dst[c] = XMConvertFloatToHalf(v <= .04045f ? v / 12.92f
                                                                       : powf((v + .055f) / 1.055f, 2.4f));
                        }
                    }
                    dst[3] = XMConvertFloatToHalf(1.f);
                }
            captureTiming.finished = qpcNow();
            return img;
        }
    }
    throw std::runtime_error("Display not found");
}
static Image crop(const Image &src, RECT r) {
    r.left = std::clamp<LONG>(r.left, 0, src.w);
    r.right = std::clamp<LONG>(r.right, 0, src.w);
    r.top = std::clamp<LONG>(r.top, 0, src.h);
    r.bottom = std::clamp<LONG>(r.bottom, 0, src.h);
    if (r.right <= r.left || r.bottom <= r.top)
        throw std::runtime_error("Empty selection");
    Image dst;
    dst.w = r.right - r.left;
    dst.h = r.bottom - r.top;
    dst.hdr = src.hdr;
    dst.format = src.format;
    dst.sdrWhite = src.sdrWhite;
    dst.rgba.resize((size_t)dst.w * dst.h * 4);
    for (int y = 0; y < dst.h; ++y)
        memcpy(&dst.rgba[(size_t)y * dst.w * 4], &src.rgba[((size_t)(y + r.top) * src.w + r.left) * 4],
               (size_t)dst.w * 8);
    return dst;
}
static float clean(float v) {
    return std::isfinite(v) ? std::max(v, 0.f) : 0.f;
}
#ifdef HDRSNIP_TEST_BUILD
static std::vector<BYTE> toneMapReference(const Image &img) {
    std::vector<BYTE> p((size_t)img.w * img.h * 4);
    for (size_t i = 0; i < img.rgba.size(); i += 4) {
        float rgb[3];
        for (int c = 0; c < 3; ++c) {
            float v = XMConvertHalfToFloat(img.rgba[i + c]);
            rgb[c] = std::isfinite(v) ? v : 0.f;
        }
        float scale = 1.f;
        if (img.hdr) {
            // Undo Windows SDR-white boosting before compressing highlights.
            float white = std::isfinite(img.sdrWhite) && img.sdrWhite > 0 ? img.sdrWhite : 1.f;
            for (float &v : rgb)
                v /= white;
            // Bring negative wide-gamut components into gamut along the neutral axis.
            float l = clean(.2126f * rgb[0] + .7152f * rgb[1] + .0722f * rgb[2]);
            float minimum = std::min({rgb[0], rgb[1], rgb[2]});
            if (minimum < 0) {
                float saturation = l > 0 ? l / (l - minimum) : 0;
                for (float &v : rgb)
                    v = l + (v - l) * saturation;
            }
            // A bounded max-RGB shoulder uses one scale for all channels: no hue-changing
            // per-channel clipping. Preserve linear midtones below the 0.6 knee.
            float peak = std::max({rgb[0], rgb[1], rgb[2], 0.f});
            if (peak > .6f) {
                float mapped = .6f + .4f * (-std::expm1(-(peak - .6f) / .4f));
                scale = mapped / peak;
            }
        }
        for (int c = 0; c < 3; ++c) {
            float v = std::clamp(rgb[c] * scale, 0.f, 1.f);
            v = v <= .0031308f ? 12.92f * v : 1.055f * powf(v, 1 / 2.4f) - .055f;
            p[i + 2 - c] = (BYTE)std::lround(v * 255);
        }
        p[i + 3] = 255;
    }
    return p;
}
#endif
struct PreviewTables {
    std::array<float, 65536> half{};
    std::array<BYTE, 65536> srgb{};
    std::array<float, 32769> shoulder{};
    PreviewTables() {
        for (int i = 0; i < 65536; ++i) {
            float h = XMConvertHalfToFloat((HALF)i);
            half[i] = std::isfinite(h) ? h : 0.f;
            float v = i / 65535.f;
            v = v <= .0031308f ? 12.92f * v : 1.055f * powf(v, 1 / 2.4f) - .055f;
            srgb[i] = (BYTE)std::lround(v * 255);
        }
        for (int i = 0; i <= 32768; ++i) {
            float peak = i / 4096.f;
            shoulder[i] = peak <= .6f ? peak : .6f + .4f * (-std::expm1(-(peak - .6f) / .4f));
        }
    }
};
static const PreviewTables &previewTables() {
    static PreviewTables tables;
    return tables;
}
static std::vector<BYTE> toneMap(const Image &img) {
    const auto &table = previewTables();
    size_t count = (size_t)img.w * img.h;
    std::vector<BYTE> out(count * 4);
    float reciprocal = img.hdr && std::isfinite(img.sdrWhite) && img.sdrWhite > 0 ? 1.f / img.sdrWhite : 1.f;
    auto work = [&](size_t first, size_t last) {
        for (size_t pixel = first; pixel < last; ++pixel) {
            size_t i = pixel * 4;
            float r = table.half[img.rgba[i]], g = table.half[img.rgba[i + 1]],
                  b = table.half[img.rgba[i + 2]];
            float scale = 1.f;
            if (img.hdr) {
                r *= reciprocal;
                g *= reciprocal;
                b *= reciprocal;
                float minimum = std::min({r, g, b});
                if (minimum < 0) {
                    float l = clean(.2126f * r + .7152f * g + .0722f * b);
                    float saturation = l > 0 ? l / (l - minimum) : 0;
                    r = l + (r - l) * saturation;
                    g = l + (g - l) * saturation;
                    b = l + (b - l) * saturation;
                }
                float peak = std::max({r, g, b, 0.f});
                if (peak > .6f) {
                    float mapped = peak >= 8 ? 1.f : table.shoulder[(size_t)(peak * 4096.f + .5f)];
                    scale = mapped / peak;
                }
            }
            auto encode = [&](float v) {
                return table.srgb[(size_t)(std::clamp(v * scale, 0.f, 1.f) * 65535.f + .5f)];
            };
            out[i] = encode(b);
            out[i + 1] = encode(g);
            out[i + 2] = encode(r);
            out[i + 3] = 255;
        }
    };
    unsigned threads =
        count >= 1024 * 1024 ? std::min(4u, std::max(1u, std::thread::hardware_concurrency())) : 1u;
    if (threads == 1) {
        work(0, count);
        return out;
    }
    std::atomic<size_t> next{0};
    auto queue = [&]() {
        while (true) {
            size_t begin = next.fetch_add(16384, std::memory_order_relaxed);
            if (begin >= count)
                break;
            work(begin, std::min(begin + 16384, count));
        }
    };
    std::vector<std::thread> workers;
    workers.reserve(threads - 1);
    for (unsigned t = 1; t < threads; ++t) {
        try {
            workers.emplace_back(queue);
        } catch (const std::system_error &) {
            break;
        }
    }
    queue();
    for (auto &worker : workers)
        worker.join();
    return out;
}
static ComPtr<IWICImagingFactory> wic() {
    ComPtr<IWICImagingFactory> f;
    check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)),
          "WIC factory");
    return f;
}
static void encode(const fs::path &path, const Image &img, bool hdr) {
    auto f = wic();
    ComPtr<IWICStream> stream;
    check(f->CreateStream(&stream), "Image stream");
    check(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE), "Open image file");
    ComPtr<IWICBitmapEncoder> encoder;
    check(f->CreateEncoder(hdr ? GUID_ContainerFormatWmp : GUID_ContainerFormatPng, nullptr, &encoder),
          "Image encoder");
    check(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "Initialize encoder");
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    check(encoder->CreateNewFrame(&frame, &props), "Image frame");
    if (hdr) {
        PROPBAG2 option{};
        option.pstrName = const_cast<wchar_t *>(L"ImageQuality");
        VARIANT v{};
        v.vt = VT_R4;
        v.fltVal = 1.f;
        check(props->Write(1, &option, &v), "Lossless JPEG XR quality");
    }
    check(frame->Initialize(props.Get()), "Initialize frame");
    check(frame->SetSize(img.w, img.h), "Image dimensions");
    check(frame->SetResolution(96, 96), "Image DPI");
    WICPixelFormatGUID format = hdr ? GUID_WICPixelFormat64bppRGBAHalf : GUID_WICPixelFormat32bppBGRA;
    auto wanted = format;
    check(frame->SetPixelFormat(&format), "Image pixel format");
    if (format != wanted)
        throw std::runtime_error("Encoder rejected requested pixel precision");
    if (!hdr) {
        ComPtr<IWICMetadataQueryWriter> metadata;
        check(frame->GetMetadataQueryWriter(&metadata), "PNG metadata");
        PROPVARIANT intent{};
        intent.vt = VT_UI1;
        intent.bVal = 0;
        check(metadata->SetMetadataByName(L"/sRGB/RenderingIntent", &intent), "PNG sRGB tag");
    }
    auto png = hdr ? std::vector<BYTE>{} : toneMap(img);
    UINT stride = img.w * (hdr ? 8 : 4);
    BYTE *data = hdr ? (BYTE *)img.rgba.data() : png.data();
    check(frame->WritePixels(img.h, stride, stride * img.h, data), "Write image pixels");
    check(frame->Commit(), "Commit frame");
    check(encoder->Commit(), "Commit image");
}
static bool copyClipboard(const Image &img) {
    auto bytes = toneMap(img);
    size_t n = sizeof(BITMAPINFOHEADER) + bytes.size();
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, n);
    if (!mem)
        return false;
    void *p = GlobalLock(mem);
    if (!p) {
        GlobalFree(mem);
        return false;
    }
    BITMAPINFOHEADER header{};
    header.biSize = sizeof(header);
    header.biWidth = img.w;
    header.biHeight = -img.h;
    header.biPlanes = 1;
    header.biBitCount = 32;
    header.biCompression = BI_RGB;
    memcpy(p, &header, sizeof(header));
    memcpy((BYTE *)p + sizeof(header), bytes.data(), bytes.size());
    GlobalUnlock(mem);
    // Offer explicit sRGB DIBV5 as well as the legacy DIB for older paste targets.
    HGLOBAL v5mem = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPV5HEADER) + bytes.size());
    if (v5mem) {
        void *dst = GlobalLock(v5mem);
        if (dst) {
            BITMAPV5HEADER v5{};
            v5.bV5Size = sizeof(v5);
            v5.bV5Width = img.w;
            v5.bV5Height = -img.h;
            v5.bV5Planes = 1;
            v5.bV5BitCount = 32;
            v5.bV5Compression = BI_BITFIELDS;
            v5.bV5RedMask = 0x00ff0000;
            v5.bV5GreenMask = 0x0000ff00;
            v5.bV5BlueMask = 0x000000ff;
            v5.bV5AlphaMask = 0xff000000;
            v5.bV5CSType = LCS_sRGB;
            v5.bV5Intent = LCS_GM_IMAGES;
            memcpy(dst, &v5, sizeof(v5));
            memcpy((BYTE *)dst + sizeof(v5), bytes.data(), bytes.size());
            GlobalUnlock(v5mem);
        } else {
            GlobalFree(v5mem);
            v5mem = nullptr;
        }
    }
    if (OpenClipboard(host)) {
        EmptyClipboard();
        bool ok = SetClipboardData(CF_DIB, mem) != nullptr;
        if (!ok)
            GlobalFree(mem);
        if (v5mem && !SetClipboardData(CF_DIBV5, v5mem))
            GlobalFree(v5mem);
        CloseClipboard();
        return ok;
    }
    GlobalFree(mem);
    if (v5mem)
        GlobalFree(v5mem);
    return false;
}
static std::wstring timestamp() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t b[80];
    swprintf_s(b, L"%04u%02u%02u_%02u%02u%02u_%03u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
               t.wMilliseconds);
    return b;
}
static void save(const Image &img) {
    fs::create_directories(output);
    auto base = output / (L"Snip_" + timestamp());
    fs::path primary = base;
    primary += img.hdr ? L".jxr" : L".png";
    fs::path temporary = primary;
    temporary += L".tmp";
    try {
        encode(temporary, img, img.hdr);
        fs::rename(temporary, primary);
    } catch (...) {
        fs::remove(temporary);
        throw;
    }
    bool companionFailed = false;
    if (img.hdr) {
        fs::path png = base;
        png += L".png";
        fs::path tempPng = png;
        tempPng += L".tmp";
        try {
            encode(tempPng, img, false);
            fs::rename(tempPng, png);
        } catch (...) {
            fs::remove(tempPng);
            companionFailed = true;
        }
    }
    bool copied = integration ? false : copyClipboard(img);
    notify((img.hdr ? L"已保存 HDR 原图（JXR）" : L"当前屏幕为 SDR，已保存 PNG") +
           std::wstring(companionFailed ? L"；PNG 副本保存失败。"
                        : copied        ? L"；普通预览已复制。"
                                        : L"；剪贴板未更新。") +
           L"\n" + primary.filename().wstring());
}

#include "editor.h"
static std::wstring hotkeyName() {
    std::wstring s;
    if (modifiers & MOD_CONTROL)
        s += L"Ctrl + ";
    if (modifiers & MOD_ALT)
        s += L"Alt + ";
    if (modifiers & MOD_SHIFT)
        s += L"Shift + ";
    if (modifiers & MOD_WIN)
        s += L"Win + ";
    wchar_t name[80]{};
    GetKeyNameTextW((LONG)(MapVirtualKeyW(key, MAPVK_VK_TO_VSC) << 16), name, 80);
    return s + name;
}
static void addTray() {
    tray = {};
    tray.cbSize = sizeof(tray);
    tray.hWnd = host;
    tray.uID = 1;
    tray.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    tray.uCallbackMessage = WM_TRAY;
    tray.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
    auto tip = L"微光 HDR 截图 · " + hotkeyName();
    wcsncpy_s(tray.szTip, tip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_ADD, &tray);
}
static HWND control(HWND parent, const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int w,
                    int h, int id) {
    auto c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, parent,
                             (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
    return c;
}
static bool writeConfig(UINT mods, UINT vk, const fs::path &dir) {
    auto temp = ini;
    temp += L".tmp";
    DeleteFileW(temp.c_str());
    bool ok =
        WritePrivateProfileStringW(L"General", L"Modifiers", std::to_wstring(mods).c_str(), temp.c_str()) &&
        WritePrivateProfileStringW(L"General", L"Key", std::to_wstring(vk).c_str(), temp.c_str()) &&
        WritePrivateProfileStringW(L"General", L"Output", dir.c_str(), temp.c_str());
    if (ok)
        ok = MoveFileExW(temp.c_str(), ini.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!ok)
        DeleteFileW(temp.c_str());
    return ok;
}
static LRESULT CALLBACK settingsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        control(hwnd, L"STATIC", L"微光  /  HDR 截图", 0, 24, 20, 450, 28, 0);
        control(hwnd, L"STATIC", L"全局快捷键（在下方按下组合键）", 0, 24, 64, 450, 24, 0);
        auto hk = control(hwnd, HOTKEY_CLASSW, L"", WS_BORDER | WS_TABSTOP, 24, 94, 300, 30, 101);
        BYTE flags = 0;
        if (modifiers & MOD_CONTROL)
            flags |= HOTKEYF_CONTROL;
        if (modifiers & MOD_SHIFT)
            flags |= HOTKEYF_SHIFT;
        if (modifiers & MOD_ALT)
            flags |= HOTKEYF_ALT;
        SendMessageW(hk, HKM_SETHOTKEY, MAKEWORD(key, flags), 0);
        auto win =
            control(hwnd, L"BUTTON", L"加上 Win 键", BS_AUTOCHECKBOX | WS_TABSTOP, 340, 94, 160, 30, 102);
        SendMessageW(win, BM_SETCHECK, (modifiers & MOD_WIN) ? BST_CHECKED : BST_UNCHECKED, 0);
        control(hwnd, L"STATIC", L"保存位置", 0, 24, 142, 460, 24, 0);
        control(hwnd, L"EDIT", output.c_str(), WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 24, 172, 382, 30,
                103);
        control(hwnd, L"BUTTON", L"浏览…", WS_TABSTOP, 418, 170, 78, 34, 104);
        control(hwnd, L"STATIC",
                L"HDR 屏幕：无损 JXR 原图 + PNG 预览\nSDR "
                L"屏幕：PNG；普通预览自动复制到剪贴板。\n截取快捷键触发时，鼠标所在屏幕；暂不跨屏框选。",
                0, 24, 224, 480, 78, 0);
        control(hwnd, L"BUTTON", L"保存设置", BS_DEFPUSHBUTTON | WS_TABSTOP, 24, 322, 140, 38, 105);
        control(hwnd, L"BUTTON", L"打开截图文件夹", WS_TABSTOP, 178, 322, 160, 38, 106);
        control(hwnd, L"STATIC", L"关闭窗口后继续驻留托盘，右键托盘可退出。", 0, 24, 384, 480, 24, 0);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == 104) {
            ComPtr<IFileDialog> dlg;
            if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                           IID_PPV_ARGS(&dlg)))) {
                DWORD opts;
                dlg->GetOptions(&opts);
                dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
                if (SUCCEEDED(dlg->Show(hwnd))) {
                    ComPtr<IShellItem> item;
                    if (SUCCEEDED(dlg->GetResult(&item))) {
                        PWSTR p = nullptr;
                        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                            SetDlgItemTextW(hwnd, 103, p);
                            CoTaskMemFree(p);
                        }
                    }
                }
            }
            return 0;
        }
        if (LOWORD(wp) == 106) {
            try {
                fs::create_directories(output);
                ShellExecuteW(hwnd, L"open", output.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            } catch (const std::exception &e) {
                MessageBoxW(hwnd, wide(e.what()).c_str(), L"无法打开目录", MB_ICONERROR);
            }
            return 0;
        }
        if (LOWORD(wp) == 105) {
            WORD hk = (WORD)SendDlgItemMessageW(hwnd, 101, HKM_GETHOTKEY, 0, 0);
            UINT vk = LOBYTE(hk), mods = 0;
            BYTE f = HIBYTE(hk);
            if (f & HOTKEYF_CONTROL)
                mods |= MOD_CONTROL;
            if (f & HOTKEYF_SHIFT)
                mods |= MOD_SHIFT;
            if (f & HOTKEYF_ALT)
                mods |= MOD_ALT;
            if (SendDlgItemMessageW(hwnd, 102, BM_GETCHECK, 0, 0) == BST_CHECKED)
                mods |= MOD_WIN;
            if (!vk || !mods) {
                MessageBoxW(hwnd, L"请选择带 Ctrl、Alt、Shift 或 Win 的组合键。", L"快捷键",
                            MB_ICONINFORMATION);
                return 0;
            }
            wchar_t buffer[32768];
            GetDlgItemTextW(hwnd, 103, buffer, 32768);
            fs::path dir(buffer);
            try {
                if (!dir.is_absolute())
                    throw std::runtime_error("Please choose an absolute save folder");
                fs::create_directories(dir);
            } catch (const std::exception &e) {
                MessageBoxW(hwnd, wide(e.what()).c_str(), L"保存位置不可用", MB_ICONERROR);
                return 0;
            }
            bool changed = mods != modifiers || vk != key || !hotkeyRegistered;
            int candidate = 3 - activeHotkey;
            if (changed && !RegisterHotKey(host, candidate, mods | MOD_NOREPEAT, vk)) {
                MessageBoxW(hwnd, L"该快捷键已被占用，请换一个组合。原快捷键仍然有效。", L"快捷键冲突",
                            MB_ICONWARNING);
                return 0;
            }
            if (!writeConfig(mods, vk, dir)) {
                if (changed)
                    UnregisterHotKey(host, candidate);
                MessageBoxW(hwnd, L"设置文件写入失败，原设置保留。", L"无法保存", MB_ICONERROR);
                return 0;
            }
            if (changed) {
                UnregisterHotKey(host, activeHotkey);
                activeHotkey = candidate;
                hotkeyRegistered = true;
            }
            modifiers = mods;
            key = vk;
            output = dir;
            Shell_NotifyIconW(NIM_DELETE, &tray);
            addTray();
            ShowWindow(hwnd, SW_HIDE);
            notify(L"设置已保存 · " + hotkeyName());
            return 0;
        }
        break;
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
static void showSettings() {
    if (!settings)
        settings =
            CreateWindowExW(WS_EX_APPWINDOW, L"HdrSnipSettings", L"微光 · HDR 截图设置",
                            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT,
                            CW_USEDEFAULT, 550, 470, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    ShowWindow(settings, SW_SHOWNORMAL);
    SetWindowPos(settings, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(settings);
}
static LRESULT CALLBACK hostProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == taskbarMessage) {
        addTray();
        return 0;
    }
    switch (msg) {
    case WM_APP + 31:
        try {
            warmCaptureDevices();
        } catch (const std::exception &e) {
            std::ofstream log(root / L"startup-error.log", std::ios::app);
            log << "Capture warmup: " << e.what() << '\n';
        }
        return 0;
#ifdef HDRSNIP_TEST_BUILD
    case WM_APP + 30:
        runResourceTest();
        return 0;
#endif
    case WM_APP + 3:
        showSettings();
        return 0;
    case WM_HOTKEY:
        beginCapture();
        return 0;
    case WM_TRAY:
        if (lp == WM_LBUTTONDBLCLK) {
            showSettings();
            return 0;
        }
        if (lp == WM_RBUTTONUP) {
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, 1, (L"框选截图    " + hotkeyName()).c_str());
            AppendMenuW(menu, MF_STRING, 2, L"设置…");
            AppendMenuW(menu, MF_STRING, 3, L"打开截图文件夹");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, 4, L"退出");
            POINT p;
            GetCursorPos(&p);
            SetForegroundWindow(hwnd);
            int item = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, hwnd, nullptr);
            DestroyMenu(menu);
            PostMessageW(hwnd, WM_NULL, 0, 0);
            if (item == 1)
                PostMessageW(hwnd, WM_HOTKEY, 1, 0);
            if (item == 2)
                showSettings();
            if (item == 3) {
                try {
                    fs::create_directories(output);
                    ShellExecuteW(hwnd, L"open", output.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                } catch (const std::exception &e) {
                    notify(wide(e.what()), true);
                }
            }
            if (item == 4)
                DestroyWindow(hwnd);
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        closeOverlay();
        UnregisterHotKey(hwnd, activeHotkey);
        Shell_NotifyIconW(NIM_DELETE, &tray);
        PostQuitMessage(integration ? integrationResult : 0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

#ifdef HDRSNIP_TEST_BUILD
#include "tests/diagnostics.h"
#endif
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(co))
        return 1;
    struct CoCleanup {
        ~CoCleanup() {
            CoUninitialize();
        }
    } cleanup;
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> args;
    for (int i = 1; i < argc; ++i)
        args.emplace_back(argv[i]);
    LocalFree(argv);
    bool background = std::find(args.begin(), args.end(), L"--background") != args.end();
    wchar_t executable[32768];
    GetModuleFileNameW(nullptr, executable, 32768);
    root = fs::path(executable).parent_path();
    ini = root / L"settings.ini";
#ifdef HDRSNIP_TEST_BUILD
    int testExit = runDiagnostics(args);
    if (testExit >= 0)
        return testExit;
    resourceMode = !args.empty() && args[0] == L"--resource-test";
    bool testEditor = resourceMode || (!args.empty() && args[0] == L"--integration-test");
#else
    constexpr bool testEditor = false;
#endif
    HANDLE mutex = CreateMutexW(nullptr, TRUE,
                                testEditor ? L"Local\\HdrSnip.Integration.2" : L"Local\\HdrSnip.Singleton.1");
    if (!mutex)
        return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        auto existing = FindWindowW(L"HdrSnipHost", nullptr);
        if (existing && !background)
            PostMessageW(existing, WM_APP + 3, 0, 0);
        CloseHandle(mutex);
        return 0;
    }
    try {
        PWSTR pictures = nullptr;
        check(SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &pictures), "Pictures folder");
        output = fs::path(pictures) / L"HdrSnip";
        CoTaskMemFree(pictures);
        wchar_t folder[32768];
        GetPrivateProfileStringW(L"General", L"Output", output.c_str(), folder, 32768, ini.c_str());
        output = folder;
        modifiers = GetPrivateProfileIntW(L"General", L"Modifiers", MOD_CONTROL | MOD_SHIFT, ini.c_str());
        key = GetPrivateProfileIntW(L"General", L"Key", 'S', ini.c_str());
#ifdef HDRSNIP_TEST_BUILD
        integration = testEditor;
        if (integration) {
            if (args.size() > 1)
                output = args[1];
            fs::create_directories(output);
            testCaptureTarget = findHdrMonitor();
        }
#endif
        INITCOMMONCONTROLSEX ic{sizeof(ic), ICC_WIN95_CLASSES};
        InitCommonControlsEx(&ic);
        font = CreateFontW(-18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0,
                           CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
        WNDCLASSW wc{};
        wc.style = CS_DBLCLKS;
        wc.hInstance = instance;
        wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
        wc.hCursor = LoadCursorW(nullptr, IDC_CROSS);
        wc.lpfnWndProc = overlayProc;
        wc.lpszClassName = L"HdrSnipOverlay";
        RegisterClassW(&wc);
        wc.style = 0;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpfnWndProc = settingsProc;
        wc.lpszClassName = L"HdrSnipSettings";
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        RegisterClassW(&wc);
        wc.lpfnWndProc = hostProc;
        wc.lpszClassName = L"HdrSnipHost";
        RegisterClassW(&wc);
        taskbarMessage = RegisterWindowMessageW(L"TaskbarCreated");
        host = CreateWindowW(L"HdrSnipHost", L"HdrSnip", 0, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
        if (!host)
            throw std::runtime_error("Host window failed");
        addTray();
        bool registered = RegisterHotKey(host, 1, modifiers | MOD_NOREPEAT, key) != 0;
        hotkeyRegistered = registered;
        if (resourceMode)
            PostMessageW(host, WM_APP + 30, 0, 0);
        else if (integration)
            PostMessageW(host, WM_HOTKEY, 1, 0);
        else if (!background &&
                 (!fs::exists(ini) || !registered || (!args.empty() && args[0] == L"--settings"))) {
            showSettings();
            if (!registered)
                MessageBoxW(settings, L"当前快捷键已被其他程序占用，请换一个组合。", L"快捷键冲突",
                            MB_ICONWARNING);
        }
        if (!integration)
            PostMessageW(host, WM_APP + 31, 0, 0);
        if (background && !registered) {
            auto tip = L"微光 HDR 截图 · 快捷键被占用，双击设置";
            tray.uFlags = NIF_TIP;
            wcsncpy_s(tray.szTip, tip, _TRUNCATE);
            Shell_NotifyIconW(NIM_MODIFY, &tray);
        }
        MSG msg{};
        int result;
        while ((result = GetMessageW(&msg, nullptr, 0, 0)) > 0) {
            if (!settings || !IsWindowVisible(settings) || !IsDialogMessageW(settings, &msg)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        if (font)
            DeleteObject(font);
        CloseHandle(mutex);
        return result == -1 ? 1 : (int)msg.wParam;
    } catch (const std::exception &e) {
        if (!background)
            MessageBoxW(nullptr, errorMessage(e).c_str(), L"微光 HDR 截图", MB_ICONERROR);
        else {
            std::ofstream log(root / L"startup-error.log", std::ios::app);
            log << e.what() << "\n";
        }
        CloseHandle(mutex);
        return 1;
    }
}
