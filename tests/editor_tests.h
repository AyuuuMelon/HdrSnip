static void editorUnitTests() {
    frozen = {};
    frozen.w = 96;
    frozen.h = 64;
    frozen.hdr = true;
    frozen.sdrWhite = 6;
    frozen.rgba.resize(96 * 64 * 4);
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 96; ++x) {
            auto i = ((size_t)y * 96 + x) * 4;
            frozen.rgba[i] = XMConvertFloatToHalf(x * .125f);
            frozen.rgba[i + 1] = XMConvertFloatToHalf(y * .125f);
            frozen.rgba[i + 2] = XMConvertFloatToHalf(-.125f);
            frozen.rgba[i + 3] = XMConvertFloatToHalf(1);
        }
    Mark mosaic;
    mosaic.tool = Tool::Mosaic;
    mosaic.a = {16, 16};
    mosaic.b = {56, 48};
    mosaic.width = 3;
    marks.push_back(mosaic);
    auto result = composeEdited({0, 0, 96, 64});
    require(frozen.rgba[(16 * 96 + 16) * 4] != frozen.rgba[(16 * 96 + 17) * 4],
            "Synthetic source has no fine detail");
    require(result.rgba[(16 * 96 + 16) * 4] == result.rgba[(16 * 96 + 17) * 4],
            "HDR mosaic left fine detail intact");
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 96; ++x)
            if (x < 16 || x >= 56 || y < 16 || y >= 48)
                for (int k = 0; k < 4; ++k)
                    require(result.rgba[((size_t)y * 96 + x) * 4 + k] ==
                                frozen.rgba[((size_t)y * 96 + x) * 4 + k],
                            "Redaction changed unrelated HDR pixel");
    auto piece = composeEdited({16, 16, 56, 48});
    require(piece.w == 40 && piece.h == 32 && piece.sdrWhite == 6, "Annotated crop metadata changed");
    closeOverlay();
}
static Image surfaceImage(DibSurface &surface) {
    GdiFlush();
    Image img;
    img.w = surface.w;
    img.h = surface.h;
    img.rgba.resize((size_t)img.w * img.h * 4);
    for (size_t i = 0; i < img.rgba.size(); i += 4) {
        for (int k = 0; k < 3; ++k) {
            float v = surface.pixels[i + 2 - k] / 255.f;
            img.rgba[i + k] =
                XMConvertFloatToHalf(v <= .04045f ? v / 12.92f : powf((v + .055f) / 1.055f, 2.4f));
        }
        img.rgba[i + 3] = XMConvertFloatToHalf(1);
    }
    return img;
}
static void testDrag(POINT a, POINT b) {
    SendMessageW(overlay, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(a.x, a.y));
    SendMessageW(overlay, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(b.x, b.y));
    SendMessageW(overlay, WM_LBUTTONUP, 0, MAKELPARAM(b.x, b.y));
}
static void runEditorIntegration() {
    std::ofstream report(output / L"editor-results.txt");
    try {
        require(frozen.hdr, "Integration target is not HDR");
        UpdateWindow(overlay);
        require(IsWindowVisible(overlay) != FALSE,
                "Editor window did not become visible after silent process startup");
        testDrag({80, 120}, {600, 450});
        require(selected && overlay && marks.empty(), "Selection closed before confirmation");
        RECT r = selection();
        require(r.left == 80 && r.right == 600 && r.bottom == 450, "Initial selection mismatch");
        report << "PASS mouse selection remains editable, no automatic save\n";
        testDrag({600, 450}, {650, 480});
        r = selection();
        require(r.right == 650 && r.bottom == 480, "Corner resize failed");
        testDrag({300, 250}, {320, 265});
        r = selection();
        require(r.left == 100 && r.top == 135 && r.right == 670 && r.bottom == 495, "Selection move failed");
        for (int edges :
             {ELeft, ERight, ETop, EBottom, ELeft | ETop, ERight | ETop, ELeft | EBottom, ERight | EBottom}) {
            setSelection({100, 135, 670, 495});
            POINT p{385, 315};
            if (edges & ELeft)
                p.x = 100;
            if (edges & ERight)
                p.x = 670;
            if (edges & ETop)
                p.y = 135;
            if (edges & EBottom)
                p.y = 495;
            POINT q{p.x + (edges & (ELeft | ERight) ? 5 : 0), p.y + (edges & (ETop | EBottom) ? 5 : 0)};
            testDrag(p, q);
            auto t = selection();
            require(!(edges & ELeft) || t.left == 105, "Left resize");
            require(!(edges & ERight) || t.right == 675, "Right resize");
            require(!(edges & ETop) || t.top == 140, "Top resize");
            require(!(edges & EBottom) || t.bottom == 500, "Bottom resize");
        }
        setSelection({100, 135, 670, 495});
        report << "PASS selection move and all eight resize handles\n";
        POINT stable{350, 300};
        size_t pi = ((size_t)stable.y * frozen.w + stable.x) * 4;
        for (int i = 0; i < 80; ++i) {
            beforeDrag = selection();
            anchor = {670, 495};
            resizeEdges = ERight | EBottom;
            gesture = Gesture::Resize;
            pointerMove({670 + i % 20, 495 + i % 15});
            RedrawWindow(overlay, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
            GdiFlush();
            require(memcmp(frameBuffer.pixels + pi, brightBuffer.pixels + pi, 3) == 0,
                    "Selected interior changed during resize paint");
            gesture = Gesture::None;
        }
        setSelection({100, 135, 670, 495});
        report << "PASS 80 resize frames preserve selected interior; presentation uses one completed-frame "
                  "blit\n";
        activeTool = Tool::Rectangle;
        testDrag({130, 180}, {260, 245});
        activeTool = Tool::Ellipse;
        testDrag({300, 175}, {430, 245});
        activeTool = Tool::Arrow;
        testDrag({150, 340}, {290, 275});
        activeTool = Tool::Pen;
        testDrag({330, 290}, {470, 335});
        require(marks.size() == 4, "Drawing tool commits failed");
        activeTool = Tool::Text;
        pointerDown(overlay, {130, 390});
        require(textEdit != nullptr, "Inline text field missing");
        SetWindowTextW(textEdit, L"HDR 标注测试");
        SendMessageW(textEdit, WM_KEYDOWN, VK_RETURN, 0);
        require(marks.size() == 5 && !textEdit, "Text commit failed");
        activeTool = Tool::Mosaic;
        testDrag({480, 180}, {620, 350});
        require(marks.size() == 6, "Mosaic commit failed");
        auto withMosaic = composeEdited(selection());
        undoMark(false);
        require(marks.size() == 5 && redoMarks.size() == 1, "Undo failed");
        undoMark(true);
        require(marks.size() == 6 && redoMarks.empty(), "Redo failed");
        auto edited = composeEdited(selection());
        require(edited.rgba == withMosaic.rgba, "Redo did not restore pixels");
        auto original = crop(frozen, selection());
        require(edited.rgba != original.rgba, "Annotations did not alter exported pixels");
        int rx = 500 - selection().left, ry = 200 - selection().top;
        size_t first = ((size_t)ry * edited.w + rx) * 4;
        size_t neighbor = ((size_t)ry * edited.w + rx + 1) * 4;
        require(edited.rgba[first] == edited.rgba[neighbor], "Mosaic did not redact exported HDR pixels");
        encode(output / L"edited.jxr", edited, true);
        verifyHdr(output / L"edited.jxr", edited);
        encode(output / L"edited.png", edited, false);
        report << "PASS rectangle, ellipse, arrow, pen, inline Chinese text, mosaic, undo/redo, annotated "
                  "HDR JXR exact roundtrip\n";
        activeTool = Tool::Select;
        RedrawWindow(overlay, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
        paintEditor(frameBuffer.dc, {0, 0, frozen.w, frozen.h});
        auto visual = surfaceImage(frameBuffer);
        encode(output / L"editor-preview.png",
               crop(visual, {70, 95, std::min(1250, frozen.w), std::min(625, frozen.h)}), false);
        finishEditor(true);
        require(!overlay && !busy && frozen.rgba.empty() && !brightBuffer.dc && !frameBuffer.dc &&
                    preview.capacity() == 0 && marks.capacity() == 0,
                "Editor buffers retained after save");
        report
            << "PASS explicit save closes editor and frees CPU/GDI buffers; test did not modify clipboard\n";
        report.flush();
    } catch (const std::exception &e) {
        report << "FAIL " << e.what() << "\n";
        integrationResult = 1;
        closeOverlay();
        PostMessageW(host, WM_CLOSE, 0, 0);
    }
}
struct ResourceSample {
    unsigned long long local = 0, shared = 0, privateBytes = 0;
    DWORD gdi = 0, user = 0, handles = 0;
    unsigned adapters = 0;
};
static ResourceSample resources() {
    ResourceSample s;
    PROCESS_MEMORY_COUNTERS_EX pm{};
    pm.cb = sizeof(pm);
    if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pm, sizeof(pm)))
        s.privateBytes = pm.PrivateUsage;
    s.gdi = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    s.user = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
    GetProcessHandleCount(GetCurrentProcess(), &s.handles);
    ComPtr<IDXGIFactory1> f;
    check(CreateDXGIFactory1(IID_PPV_ARGS(&f)), "VRAM sample factory");
    for (UINT a = 0;; ++a) {
        ComPtr<IDXGIAdapter1> adapter;
        if (f->EnumAdapters1(a, &adapter) == DXGI_ERROR_NOT_FOUND)
            break;
        ComPtr<IDXGIAdapter3> a3;
        if (FAILED(adapter.As(&a3)))
            continue;
        DXGI_QUERY_VIDEO_MEMORY_INFO local{}, shared{};
        if (SUCCEEDED(a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local)) &&
            SUCCEEDED(a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &shared))) {
            s.local += local.CurrentUsage;
            s.shared += shared.CurrentUsage;
            ++s.adapters;
        }
    }
    return s;
}
static unsigned long long peakCaptureLocal = 0, peakCaptureShared = 0;
static void measureCapture() {
    auto s = resources();
    peakCaptureLocal = std::max(peakCaptureLocal, s.local);
    peakCaptureShared = std::max(peakCaptureShared, s.shared);
}
static void runResourceTest() {
    std::ofstream csv(output / L"resource-samples.csv"), report(output / L"resource-results.txt");
    csv << "cycle,phase,local_bytes,shared_bytes,private_bytes,gdi,user,handles,adapters\n";
    auto record = [&](int n, const char *phase) {
        auto s = resources();
        csv << n << ',' << phase << ',' << s.local << ',' << s.shared << ',' << s.privateBytes << ',' << s.gdi
            << ',' << s.user << ',' << s.handles << ',' << s.adapters << '\n';
        csv.flush();
        return s;
    };
    try {
        require(testCaptureTarget != nullptr, "No HDR monitor available for resource test");
        record(0, "baseline");
        ResourceSample warm{}, last{};
        captureMemoryHook = measureCapture;
        for (int i = 1; i <= 30; ++i) {
            beginCapture();
            require(overlay && frozen.hdr, "HDR capture failed during resource test");
            setSelection({50, 80, 370, 280});
            selected = true;
            if (i % 3 == 0) {
                Mark m;
                m.tool = Tool::Mosaic;
                m.a = {100, 110};
                m.b = {210, 170};
                commitMark(m);
                auto cut = composeEdited(selection());
                encode(output / L"resource-save.jxr", cut, true);
                encode(output / L"resource-save.png", cut, false);
            }
            paintEditor(frameBuffer.dc, {0, 0, frozen.w, frozen.h});
            GdiFlush();
            record(i, "editor-open");
            closeOverlay();
            require(!busy && !overlay && !brightBuffer.dc && !dimBuffer.dc && !frameBuffer.dc &&
                        frozen.rgba.capacity() == 0 && preview.capacity() == 0 && marks.capacity() == 0,
                    "Resources retained after close");
            Sleep(150);
            last = record(i, "closed");
            if (i == 5)
                warm = last;
        }
        captureMemoryHook = nullptr;
        Sleep(1000);
        last = record(31, "settled");
        require(last.adapters > 0, "GPU memory counters unavailable");
        require(peakCaptureLocal + peakCaptureShared > 0,
                "GPU counters did not observe live capture allocations");
        report << "HDR rounds=30 (20 cancel, 10 annotate+encode), real desktop capture, hidden editor window "
                  "lifecycle\n"
               << "GPU query adapters=" << last.adapters << "\n"
               << "Peak while D3D capture resources alive: local=" << peakCaptureLocal
               << ", shared=" << peakCaptureShared << " bytes\n"
               << "After warmup close: local=" << warm.local << ", shared=" << warm.shared
               << ", private=" << warm.privateBytes << ", GDI=" << warm.gdi << ", USER=" << warm.user
               << ", handles=" << warm.handles << "\n"
               << "Final settled close: local=" << last.local << ", shared=" << last.shared
               << ", private=" << last.privateBytes << ", GDI=" << last.gdi << ", USER=" << last.user
               << ", handles=" << last.handles << "\n";
        require(last.local <= warm.local + 4 * 1024 * 1024 && last.shared <= warm.shared + 4 * 1024 * 1024,
                "GPU memory grew beyond warmup allowance");
        require(last.gdi <= warm.gdi + 2 && last.user <= warm.user + 2 && last.handles <= warm.handles + 4,
                "GDI/USER/kernel handles grew after repeated screenshots");
        require(last.privateBytes <= warm.privateBytes + 32 * 1024 * 1024,
                "Private memory grew beyond warmup allowance");
        report << "PASS no accumulating process GPU allocations or GDI/USER/kernel handles in this sample; "
                  "CPU editor buffers freed every cycle.\nThis sample excludes DWM and driver-owned "
                  "allocations; visible-window and long-session testing remain separate.\n";
        integrationResult = 0;
    } catch (const std::exception &e) {
        captureMemoryHook = nullptr;
        report << "FAIL " << e.what() << "\n";
        integrationResult = 1;
        closeOverlay();
    }
    PostMessageW(host, WM_CLOSE, 0, 0);
}
