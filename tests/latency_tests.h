// A real, changing desktop marker on a separate UI thread. No screen images are retained.
static std::atomic<HWND> clockWindow{nullptr};
static std::atomic<unsigned> clockSequence{0};
static LRESULT CALLBACK clockProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_TIMER || msg == WM_APP + 1) {
        unsigned n = ++clockSequence;
        InvalidateRect(hwnd, nullptr, FALSE);
        UpdateWindow(hwnd);
        DwmFlush();
        return n;
    }
    if (msg == WM_ERASEBKGND)
        return 1;
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT r{0, 0, 560, 125};
        HBRUSH background = CreateSolidBrush(RGB(20, 35, 40));
        FillRect(dc, &r, background);
        DeleteObject(background);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(220, 255, 240));
        std::wstring text = L"HDR 截图帧时序测试 · " + std::to_wstring(clockSequence.load());
        TextOutW(dc, 20, 22, text.c_str(), (int)text.size());
        unsigned n = clockSequence.load();
        for (int i = 0; i < 32; ++i) {
            RECT cell{20 + i * 16, 68, 32 + i * 16, 92};
            HBRUSH b = CreateSolidBrush((n & (1u << i)) ? RGB(255, 255, 255) : RGB(0, 0, 0));
            FillRect(dc, &cell, b);
            DeleteObject(b);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    if (msg == WM_DESTROY) {
        KillTimer(hwnd, 1);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
static int frameTest(const fs::path &dir) {
    fs::create_directories(dir);
    std::ofstream csv(dir / L"frame-timing.csv"), report(dir / L"frame-results.txt");
    csv << "sample,required_marker,captured_marker,first_frame_vs_request_ms,chosen_frame_vs_request_ms,"
           "acquire_ms,readback_ms,capture_total_ms,preview_ms,chosen_frame_age_at_ready_ms,images\n";
    std::thread clockThread;
    try {
        HMONITOR monitor = findHdrMonitor();
        require(monitor != nullptr, "HDR monitor required");
        MONITORINFO mi{sizeof(mi)};
        require(GetMonitorInfoW(monitor, &mi) != FALSE, "Clock monitor info");
        auto warmStart = qpcNow();
        warmCaptureDevices();
        double warmMs = qpcMs(qpcNow() - warmStart);
        clockSequence = 0;
        clockWindow = nullptr;
        clockThread = std::thread([=]() {
            WNDCLASSW wc{};
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = L"HdrSnipFrameClock";
            wc.lpfnWndProc = clockProc;
            RegisterClassW(&wc);
            HWND hwnd =
                CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName,
                                L"HdrSnip frame test", WS_POPUP, mi.rcMonitor.left + 30,
                                mi.rcMonitor.top + 30, 560, 125, nullptr, nullptr, wc.hInstance, nullptr);
            clockWindow = hwnd;
            if (!hwnd)
                return;
            SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            SetTimer(hwnd, 1, 8, nullptr);
            MSG m{};
            while (GetMessageW(&m, nullptr, 0, 0) > 0) {
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
            clockWindow = nullptr;
        });
        for (int i = 0; i < 200 && !clockWindow.load(); ++i)
            Sleep(5);
        require(clockWindow.load() != nullptr, "Clock window creation failed");
        double total = 0, previewTotal = 0;
        int stale = 0;
        for (int sample = 0; sample < 10; ++sample) {
            unsigned required = (unsigned)SendMessageW(clockWindow.load(), WM_APP + 1, 0, 0);
            auto image = capture(monitor);
            unsigned actual = 0;
            for (int i = 0; i < 32; ++i) {
                int x = 30 + 26 + i * 16, y = 30 + 80;
                float v = XMConvertHalfToFloat(image.rgba[((size_t)y * image.w + x) * 4]);
                if (v > image.sdrWhite * .5f)
                    actual |= 1u << i;
            }
            auto before = qpcNow();
            auto png = toneMap(image);
            auto ready = qpcNow();
            double captureMs = qpcMs(before - captureTiming.requested), previewMs = qpcMs(ready - before);
            if (actual < required)
                ++stale;
            total += captureMs;
            previewTotal += previewMs;
            csv << sample << ',' << required << ',' << actual << ','
                << qpcMs(captureTiming.first - captureTiming.requested) << ','
                << qpcMs(captureTiming.chosen - captureTiming.requested) << ','
                << qpcMs(captureTiming.acquired - captureTiming.requested) << ','
                << qpcMs(captureTiming.readback - captureTiming.acquired) << ',' << captureMs << ','
                << previewMs << ',' << qpcMs(ready - captureTiming.chosen) << ',' << captureTiming.images
                << '\n';
            csv.flush();
            Sleep(20);
        }
        report << "One-time device/table warmup=" << warmMs
               << " ms (same preparation as silent background startup).\n";
        report << "Real HDR desktop marker, 10 captures; captured_marker < required_marker means a provably "
                  "pre-request image.\nStale marker captures="
               << stale << "/10\nMean capture=" << total / 10
               << " ms; mean SDR preview conversion=" << previewTotal / 10 << " ms\n";
        PostMessageW(clockWindow.load(), WM_CLOSE, 0, 0);
        clockThread.join();
        return stale ? 1 : 0;
    } catch (const std::exception &e) {
        report << "FAIL " << e.what() << '\n';
        HWND hwnd = clockWindow.load();
        if (hwnd)
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        if (clockThread.joinable())
            clockThread.join();
        return 1;
    }
}
