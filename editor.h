// Included after capture/encoding helpers. All coordinates are physical display pixels.
struct DibSurface {
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ previous = nullptr;
    BYTE *pixels = nullptr;
    int w = 0, h = 0;
    void reset() {
        GdiFlush();
        if (dc && previous)
            SelectObject(dc, previous);
        if (bitmap)
            DeleteObject(bitmap);
        if (dc)
            DeleteDC(dc);
        dc = nullptr;
        bitmap = nullptr;
        previous = nullptr;
        pixels = nullptr;
        w = h = 0;
    }
    void create(int width, int height) {
        reset();
        w = width;
        h = height;
        BITMAPINFO b{};
        b.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        b.bmiHeader.biWidth = w;
        b.bmiHeader.biHeight = -h;
        b.bmiHeader.biPlanes = 1;
        b.bmiHeader.biBitCount = 32;
        b.bmiHeader.biCompression = BI_RGB;
        dc = CreateCompatibleDC(nullptr);
        bitmap = CreateDIBSection(dc, &b, DIB_RGB_COLORS, (void **)&pixels, nullptr, 0);
        if (!dc || !bitmap) {
            reset();
            throw std::runtime_error("Editor buffer allocation failed");
        }
        previous = SelectObject(dc, bitmap);
    }
    ~DibSurface() {
        reset();
    }
};
enum class Tool { Select, Rectangle, Ellipse, Arrow, Pen, Text, Mosaic };
struct Mark {
    Tool tool = Tool::Rectangle;
    POINT a{}, b{};
    COLORREF color = RGB(255, 85, 85);
    int width = 3;
    std::vector<POINT> points;
    std::wstring text;
};
enum class Gesture { None, New, Move, Resize, Draw };
static DibSurface brightBuffer, dimBuffer, frameBuffer;
#ifdef HDRSNIP_TEST_BUILD
static bool resourceMode = false;
static HMONITOR testCaptureTarget = nullptr;
static void runResourceTest();
static void editorUnitTests();
static HMONITOR findHdrMonitor() {
    ComPtr<IDXGIFactory1> f;
    check(CreateDXGIFactory1(IID_PPV_ARGS(&f)), "HDR monitor factory");
    for (UINT a = 0;; ++a) {
        ComPtr<IDXGIAdapter1> adapter;
        if (f->EnumAdapters1(a, &adapter) == DXGI_ERROR_NOT_FOUND)
            break;
        for (UINT o = 0;; ++o) {
            ComPtr<IDXGIOutput> out;
            if (adapter->EnumOutputs(o, &out) == DXGI_ERROR_NOT_FOUND)
                break;
            ComPtr<IDXGIOutput6> out6;
            if (FAILED(out.As(&out6)))
                continue;
            DXGI_OUTPUT_DESC1 d{};
            if (SUCCEEDED(out6->GetDesc1(&d)) && d.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020)
                return d.Monitor;
        }
    }
    return nullptr;
}
#else
static bool resourceMode = false;
static constexpr HMONITOR testCaptureTarget = nullptr;
#endif
static std::vector<Mark> marks, redoMarks;
static Mark draft;
static Tool activeTool = Tool::Select;
static Gesture gesture = Gesture::None;
static bool selected = false;
static POINT anchor{};
static RECT beforeDrag{};
static int resizeEdges = 0, colorIndex = 0, strokeWidth = 3;
static HWND textEdit = nullptr;
static HFONT textFont = nullptr;
static POINT textOrigin{};
static constexpr COLORREF palette[] = {RGB(255, 85, 85),  RGB(255, 208, 80),  RGB(80, 220, 190),
                                       RGB(90, 165, 255), RGB(255, 255, 255), RGB(25, 25, 30)};
static constexpr int ELeft = 1, ERight = 2, ETop = 4, EBottom = 8;
static void setSelection(RECT r) {
    start = {r.left, r.top};
    finish = {r.right, r.bottom};
}
static RECT markBounds(const Mark &m) {
    RECT r{std::min(m.a.x, m.b.x), std::min(m.a.y, m.b.y), std::max(m.a.x, m.b.x) + 1,
           std::max(m.a.y, m.b.y) + 1};
    if (m.tool == Tool::Pen && !m.points.empty()) {
        r = {m.points[0].x, m.points[0].y, m.points[0].x + 1, m.points[0].y + 1};
        for (auto p : m.points) {
            r.left = std::min(r.left, p.x);
            r.top = std::min(r.top, p.y);
            r.right = std::max(r.right, p.x + 1);
            r.bottom = std::max(r.bottom, p.y + 1);
        }
    }
    if (m.tool == Tool::Text) {
        r.right = m.a.x + (int)m.text.size() * (20 + m.width * 4) + 32;
        r.bottom = m.a.y + 24 + m.width * 4;
    }
    int pad = m.width * 5 + 8;
    InflateRect(&r, pad, pad);
    return r;
}
static void drawMark(HDC dc, const Mark &m, bool mask = false) {
    COLORREF c = mask ? RGB(255, 255, 255) : m.color;
    HPEN pen = CreatePen(PS_SOLID, m.width, c);
    auto oldPen = SelectObject(dc, pen), oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    RECT r{std::min(m.a.x, m.b.x), std::min(m.a.y, m.b.y), std::max(m.a.x, m.b.x), std::max(m.a.y, m.b.y)};
    if (m.tool == Tool::Rectangle || m.tool == Tool::Mosaic)
        Rectangle(dc, r.left, r.top, r.right, r.bottom);
    if (m.tool == Tool::Ellipse)
        Ellipse(dc, r.left, r.top, r.right, r.bottom);
    if (m.tool == Tool::Arrow) {
        MoveToEx(dc, m.a.x, m.a.y, nullptr);
        LineTo(dc, m.b.x, m.b.y);
        double angle = atan2((double)m.b.y - m.a.y, (double)m.b.x - m.a.x), length = 12. + m.width * 2.;
        for (double offset : {-.52, .52}) {
            MoveToEx(dc, m.b.x, m.b.y, nullptr);
            LineTo(dc, m.b.x - (int)(length * cos(angle + offset)),
                   m.b.y - (int)(length * sin(angle + offset)));
        }
    }
    if (m.tool == Tool::Pen) {
        if (m.points.size() > 1)
            Polyline(dc, m.points.data(), (int)m.points.size());
        else {
            MoveToEx(dc, m.a.x, m.a.y, nullptr);
            LineTo(dc, m.a.x + 1, m.a.y + 1);
        }
    }
    if (m.tool == Tool::Text) {
        HFONT tf = CreateFontW(-(20 + m.width * 4), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Microsoft YaHei UI");
        auto oldFont = SelectObject(dc, tf);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, c);
        TextOutW(dc, m.a.x, m.a.y, m.text.c_str(), (int)m.text.size());
        SelectObject(dc, oldFont);
        DeleteObject(tf);
    }
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}
static Image composeEdited(RECT region) {
    Image result = crop(frozen, region);
    if (marks.empty())
        return result;
    DibSurface mask;
    mask.create(result.w, result.h);
    for (const auto &m : marks) {
        RECT area = markBounds(m);
        OffsetRect(&area, -region.left, -region.top);
        RECT canvas{0, 0, result.w, result.h};
        RECT clipped{};
        if (!IntersectRect(&clipped, &area, &canvas))
            continue;
        if (m.tool == Tool::Mosaic) {
            RECT mr{std::min(m.a.x, m.b.x), std::min(m.a.y, m.b.y), std::max(m.a.x, m.b.x),
                    std::max(m.a.y, m.b.y)};
            OffsetRect(&mr, -region.left, -region.top);
            int block = 8 + m.width * 4;
            for (int by = mr.top; by < mr.bottom; by += block)
                for (int bx = mr.left; bx < mr.right; bx += block) {
                    RECT cell{bx, by, std::min<LONG>(bx + block, mr.right),
                              std::min<LONG>(by + block, mr.bottom)},
                        c{};
                    if (!IntersectRect(&c, &cell, &canvas))
                        continue;
                    double sum[3]{};
                    int n = (c.right - c.left) * (c.bottom - c.top);
                    for (int y = c.top; y < c.bottom; ++y)
                        for (int x = c.left; x < c.right; ++x)
                            for (int k = 0; k < 3; ++k)
                                sum[k] +=
                                    XMConvertHalfToFloat(result.rgba[((size_t)y * result.w + x) * 4 + k]);
                    HALF avg[3];
                    for (int k = 0; k < 3; ++k)
                        avg[k] = XMConvertFloatToHalf((float)(sum[k] / n));
                    for (int y = c.top; y < c.bottom; ++y)
                        for (int x = c.left; x < c.right; ++x)
                            for (int k = 0; k < 3; ++k)
                                result.rgba[((size_t)y * result.w + x) * 4 + k] = avg[k];
                }
        } else {
            GdiFlush();
            memset(mask.pixels, 0, (size_t)result.w * result.h * 4);
            SetViewportOrgEx(mask.dc, -region.left, -region.top, nullptr);
            drawMark(mask.dc, m, true);
            SetViewportOrgEx(mask.dc, 0, 0, nullptr);
            GdiFlush();
            float color[3];
            for (int k = 0; k < 3; ++k) {
                float v = (k == 0   ? GetRValue(m.color)
                           : k == 1 ? GetGValue(m.color)
                                    : GetBValue(m.color)) /
                          255.f;
                color[k] = (v <= .04045f ? v / 12.92f : powf((v + .055f) / 1.055f, 2.4f)) *
                           (result.hdr ? result.sdrWhite : 1.f);
            }
            for (int y = clipped.top; y < clipped.bottom; ++y)
                for (int x = clipped.left; x < clipped.right; ++x) {
                    size_t i = ((size_t)y * result.w + x) * 4;
                    float alpha = mask.pixels[i] / 255.f;
                    if (alpha > 0)
                        for (int k = 0; k < 3; ++k)
                            result.rgba[i + k] = XMConvertFloatToHalf(
                                color[k] * alpha + XMConvertHalfToFloat(result.rgba[i + k]) * (1 - alpha));
                }
        }
    }
    return result;
}
static void rebuildPreview() {
    auto edited = composeEdited({0, 0, frozen.w, frozen.h});
    auto pixels = toneMap(edited);
    GdiFlush();
    memcpy(brightBuffer.pixels, pixels.data(), pixels.size());
}
static void commitMark(Mark mark) {
    marks.push_back(std::move(mark));
    redoMarks.clear();
    rebuildPreview();
}
static void undoMark(bool redo) {
    auto &from = redo ? redoMarks : marks;
    auto &to = redo ? marks : redoMarks;
    if (!from.empty()) {
        to.push_back(std::move(from.back()));
        from.pop_back();
        rebuildPreview();
    }
    if (overlay)
        InvalidateRect(overlay, nullptr, FALSE);
}
static void endText(bool accept) {
    if (!textEdit)
        return;
    HWND edit = textEdit;
    textEdit = nullptr;
    int length = GetWindowTextLengthW(edit);
    std::wstring value(length + 1, L'\0');
    GetWindowTextW(edit, value.data(), length + 1);
    value.resize(length);
    DestroyWindow(edit);
    if (textFont) {
        DeleteObject(textFont);
        textFont = nullptr;
    }
    if (accept && !value.empty()) {
        Mark m;
        m.tool = Tool::Text;
        m.a = m.b = textOrigin;
        m.width = strokeWidth;
        m.color = palette[colorIndex];
        m.text = value;
        commitMark(std::move(m));
    }
    if (overlay) {
        SetFocus(overlay);
        InvalidateRect(overlay, nullptr, FALSE);
    }
}
static LRESULT CALLBACK textProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_KEYDOWN && (wp == VK_RETURN || wp == VK_ESCAPE)) {
        endText(wp == VK_RETURN);
        return 0;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}
static void beginText(POINT p) {
    endText(true);
    textOrigin = p;
    auto r = selection();
    int width = std::min<LONG>(std::max<LONG>(120, r.right - p.x), 420);
    width = std::min<LONG>(width, frozen.w - p.x);
    textFont = CreateFontW(-(20 + strokeWidth * 4), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Microsoft YaHei UI");
    textEdit =
        CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, p.x, p.y, width,
                        28 + strokeWidth * 4, overlay, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(textEdit, WM_SETFONT, (WPARAM)textFont, TRUE);
    SendMessageW(textEdit, EM_SETLIMITTEXT, 512, 0);
    SetWindowSubclass(textEdit, textProc, 1, 0);
    SetFocus(textEdit);
}
static void closeOverlay() {
    if (textEdit) {
        DestroyWindow(textEdit);
        textEdit = nullptr;
    }
    if (textFont) {
        DeleteObject(textFont);
        textFont = nullptr;
    }
    HWND old = overlay;
    overlay = nullptr;
    if (old)
        DestroyWindow(old);
    brightBuffer.reset();
    dimBuffer.reset();
    frameBuffer.reset();
    frozen = {};
    std::vector<BYTE>().swap(preview);
    std::vector<BYTE>().swap(darkPreview);
    std::vector<Mark>().swap(marks);
    std::vector<Mark>().swap(redoMarks);
    draft = {};
    selected = false;
    gesture = Gesture::None;
    dragging = false;
    busy = false;
}
static POINT clampPoint(LPARAM lp) {
    return {std::clamp(GET_X_LPARAM(lp), 0, frozen.w), std::clamp(GET_Y_LPARAM(lp), 0, frozen.h)};
}
static int edgeAt(POINT p) {
    if (!selected)
        return 0;
    auto r = selection();
    if (p.x < r.left - 8 || p.x > r.right + 8 || p.y < r.top - 8 || p.y > r.bottom + 8)
        return 0;
    int e = 0;
    if (abs(p.x - r.left) <= 7)
        e |= ELeft;
    else if (abs(p.x - r.right) <= 7)
        e |= ERight;
    if (abs(p.y - r.top) <= 7)
        e |= ETop;
    else if (abs(p.y - r.bottom) <= 7)
        e |= EBottom;
    return e;
}
struct EditorButton {
    int id;
    std::wstring label;
    RECT rect;
};
static std::vector<EditorButton> toolbar() {
    if (!selected || gesture == Gesture::New)
        return {};
    std::vector<EditorButton> buttons;
    const wchar_t *names[] = {L"选区 V", L"方框 R",   L"椭圆 O",     L"箭头 A",      L"画笔 P",
                              L"文字 T", L"马赛克 M", L"颜色",       L"粗细",        L"撤销",
                              L"重做",   L"重选",     L"复制 Enter", L"保存 Ctrl+S", L"取消"};
    int widths[] = {66, 66, 66, 66, 66, 66, 88, 56, 56, 56, 56, 56, 100, 114, 56};
    int total = 16;
    for (int w : widths)
        total += w + 4;
    int available = std::min(total, frozen.w - 24), rows = (total + available - 1) / available,
        height = rows * 42 + 12;
    auto r = selection();
    int x = std::clamp<LONG>(r.right - available, 12, std::max(12, frozen.w - available - 12));
    int y = r.bottom + 12;
    if (y + height > frozen.h - 8)
        y = r.top - height - 12;
    if (y < 8)
        y = std::max(8, frozen.h - height - 8);
    int current = x + 6, rowY = y + 6;
    for (int i = 0; i < 15; ++i) {
        if (current + widths[i] > x + available - 4) {
            current = x + 6;
            rowY += 42;
        }
        std::wstring label = names[i];
        if (i == 8)
            label = std::to_wstring(strokeWidth) + L" px";
        buttons.push_back({i, label, {current, rowY, current + widths[i], rowY + 36}});
        current += widths[i] + 4;
    }
    return buttons;
}
static int buttonAt(POINT p) {
    for (const auto &b : toolbar())
        if (PtInRect(&b.rect, p))
            return b.id;
    return -1;
}
static void paintEditor(HDC target, RECT dirty) {
    // Only this final BitBlt touches the window; dark/bright/marks are composed offscreen.
    BitBlt(frameBuffer.dc, 0, 0, frozen.w, frozen.h, dimBuffer.dc, 0, 0, SRCCOPY);
    auto r = selection();
    if (selected || gesture == Gesture::New) {
        BitBlt(frameBuffer.dc, r.left, r.top, r.right - r.left, r.bottom - r.top, brightBuffer.dc, r.left,
               r.top, SRCCOPY);
        SaveDC(frameBuffer.dc);
        IntersectClipRect(frameBuffer.dc, r.left, r.top, r.right, r.bottom);
        if (gesture == Gesture::Draw)
            drawMark(frameBuffer.dc, draft);
        RestoreDC(frameBuffer.dc, -1);
        HPEN pen = CreatePen(PS_SOLID, 2, RGB(100, 230, 205));
        auto old = SelectObject(frameBuffer.dc, pen),
             brush = SelectObject(frameBuffer.dc, GetStockObject(NULL_BRUSH));
        Rectangle(frameBuffer.dc, r.left, r.top, r.right, r.bottom);
        SelectObject(frameBuffer.dc, brush);
        SelectObject(frameBuffer.dc, old);
        DeleteObject(pen);
        if (selected) {
            HBRUSH dot = CreateSolidBrush(RGB(170, 255, 230));
            for (int y : {(int)r.top, (int)((r.top + r.bottom) / 2), (int)r.bottom})
                for (int x : {(int)r.left, (int)((r.left + r.right) / 2), (int)r.right}) {
                    if (x == (r.left + r.right) / 2 && y == (r.top + r.bottom) / 2)
                        continue;
                    RECT d{x - 3, y - 3, x + 4, y + 4};
                    FillRect(frameBuffer.dc, &d, dot);
                }
            DeleteObject(dot);
        }
    }
    SelectObject(frameBuffer.dc, font);
    SetBkMode(frameBuffer.dc, TRANSPARENT);
    SetTextColor(frameBuffer.dc, RGB(240, 250, 248));
    std::wstring hint = selected
                            ? std::to_wstring(r.right - r.left) + L" × " + std::to_wstring(r.bottom - r.top) +
                                  L"  · 拖动边缘调整 · 选区工具内拖动可移动 · Enter 复制 / Ctrl+S 保存"
                            : L"拖动框选 · Esc / 右键取消  |  " +
                                  std::wstring(frozen.hdr ? L"HDR 原图 + SDR 预览" : L"SDR PNG");
    RECT label{20, 16, frozen.w - 20, 48};
    HBRUSH labelBg = CreateSolidBrush(RGB(25, 35, 42));
    FillRect(frameBuffer.dc, &label, labelBg);
    DeleteObject(labelBg);
    DrawTextW(frameBuffer.dc, hint.c_str(), -1, &label, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    for (const auto &b : toolbar()) {
        bool chosen = b.id < 7 && b.id == (int)activeTool;
        COLORREF color = chosen ? RGB(34, 105, 96) : (b.id == 12 ? RGB(29, 106, 94) : RGB(35, 44, 53));
        HBRUSH bg = CreateSolidBrush(color);
        FillRect(frameBuffer.dc, &b.rect, bg);
        DeleteObject(bg);
        SetTextColor(frameBuffer.dc, RGB(235, 245, 245));
        if (b.id == 7) {
            RECT swatch = b.rect;
            InflateRect(&swatch, -8, -8);
            HBRUSH sw = CreateSolidBrush(palette[colorIndex]);
            FillRect(frameBuffer.dc, &swatch, sw);
            DeleteObject(sw);
        } else
            DrawTextW(frameBuffer.dc, b.label.c_str(), -1, const_cast<RECT *>(&b.rect),
                      DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    }
    BitBlt(target, dirty.left, dirty.top, dirty.right - dirty.left, dirty.bottom - dirty.top, frameBuffer.dc,
           dirty.left, dirty.top, SRCCOPY);
}
static void finishEditor(bool saveFiles) {
    if (!selected)
        return;
    endText(true);
    try {
        auto image = composeEdited(selection());
        if (saveFiles)
            save(image);
        else if (!integration && !copyClipboard(image))
            throw std::runtime_error("Clipboard busy; selection retained, please retry");
        integrationResult = 0;
        closeOverlay();
        if (integration)
            PostMessageW(host, WM_CLOSE, 0, 0);
    } catch (const std::exception &e) {
        notify(errorMessage(e), true);
    }
}
static void performButton(int id) {
    endText(true);
    if (id < 7) {
        activeTool = (Tool)id;
    } else if (id == 7) {
        colorIndex = (colorIndex + 1) % 6;
    } else if (id == 8) {
        strokeWidth = strokeWidth == 3 ? 6 : strokeWidth == 6 ? 10 : 3;
    } else if (id == 9)
        undoMark(false);
    else if (id == 10)
        undoMark(true);
    else if (id == 11) {
        marks.clear();
        redoMarks.clear();
        selected = false;
        activeTool = Tool::Select;
        rebuildPreview();
    } else if (id == 12) {
        finishEditor(false);
        return;
    } else if (id == 13) {
        finishEditor(true);
        return;
    } else if (id == 14) {
        closeOverlay();
        return;
    }
    if (overlay)
        InvalidateRect(overlay, nullptr, FALSE);
}
static void pointerDown(HWND hwnd, POINT p) {
    endText(true);
    int button = buttonAt(p);
    if (button >= 0) {
        performButton(button);
        return;
    }
    anchor = p;
    beforeDrag = selection();
    resizeEdges = edgeAt(p);
    if (resizeEdges)
        gesture = Gesture::Resize;
    else if (selected && PtInRect(&beforeDrag, p)) {
        if (activeTool == Tool::Select)
            gesture = Gesture::Move;
        else if (activeTool == Tool::Text) {
            beginText(p);
            return;
        } else {
            gesture = Gesture::Draw;
            draft = {};
            draft.tool = activeTool;
            draft.a = draft.b = p;
            draft.color = palette[colorIndex];
            draft.width = strokeWidth;
            draft.points.push_back(p);
        }
    } else {
        gesture = Gesture::New;
        selected = false;
        start = finish = p;
    }
    SetCapture(hwnd);
    InvalidateRect(hwnd, nullptr, FALSE);
}
static void pointerMove(POINT p) {
    if (gesture == Gesture::None)
        return;
    if (gesture == Gesture::New)
        finish = p;
    else if (gesture == Gesture::Move) {
        LONG dx = std::clamp<LONG>(p.x - anchor.x, -beforeDrag.left, frozen.w - beforeDrag.right),
             dy = std::clamp<LONG>(p.y - anchor.y, -beforeDrag.top, frozen.h - beforeDrag.bottom);
        RECT r = beforeDrag;
        OffsetRect(&r, dx, dy);
        setSelection(r);
    } else if (gesture == Gesture::Resize) {
        auto r = beforeDrag;
        if (resizeEdges & ELeft)
            r.left = std::min(p.x, r.right - 2);
        if (resizeEdges & ERight)
            r.right = std::max(p.x, r.left + 2);
        if (resizeEdges & ETop)
            r.top = std::min(p.y, r.bottom - 2);
        if (resizeEdges & EBottom)
            r.bottom = std::max(p.y, r.top + 2);
        setSelection(r);
    } else if (gesture == Gesture::Draw) {
        auto r = selection();
        p.x = std::clamp<LONG>(p.x, r.left, r.right - 1);
        p.y = std::clamp<LONG>(p.y, r.top, r.bottom - 1);
        draft.b = p;
        if (draft.tool == Tool::Pen)
            draft.points.push_back(p);
    }
    InvalidateRect(overlay, nullptr, FALSE);
}
static void pointerUp(POINT p) {
    if (gesture == Gesture::None)
        return;
    pointerMove(p);
    auto ended = gesture;
    gesture = Gesture::None;
    ReleaseCapture();
    if (ended == Gesture::New) {
        auto r = selection();
        selected = r.right - r.left >= 2 && r.bottom - r.top >= 2;
    } else if (ended == Gesture::Draw) {
        if (draft.tool == Tool::Pen || abs(draft.a.x - draft.b.x) + abs(draft.a.y - draft.b.y) > 2)
            commitMark(draft);
        draft = {};
    }
    InvalidateRect(overlay, nullptr, FALSE);
}
#ifdef HDRSNIP_TEST_BUILD
static void runEditorIntegration();
#endif
static LRESULT CALLBACK overlayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    try {
        switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            paintEditor(dc, ps.rcPaint);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_LBUTTONDOWN:
            pointerDown(hwnd, clampPoint(lp));
            return 0;
        case WM_MOUSEMOVE:
            pointerMove(clampPoint(lp));
            return 0;
        case WM_LBUTTONUP:
            pointerUp(clampPoint(lp));
            return 0;
        case WM_LBUTTONDBLCLK:
            if (selected && activeTool == Tool::Select && buttonAt(clampPoint(lp)) < 0)
                finishEditor(false);
            return 0;
        case WM_RBUTTONDOWN:
            closeOverlay();
            return 0;
        case WM_SETCURSOR: {
            POINT p;
            GetCursorPos(&p);
            ScreenToClient(hwnd, &p);
            RECT currentSelection = selection();
            int edge = edgeAt(p);
            LPCWSTR cursor = IDC_CROSS;
            if (buttonAt(p) >= 0)
                cursor = IDC_HAND;
            else if (edge == (ELeft | ETop) || edge == (ERight | EBottom))
                cursor = IDC_SIZENWSE;
            else if (edge == (ELeft | EBottom) || edge == (ERight | ETop))
                cursor = IDC_SIZENESW;
            else if (edge & (ELeft | ERight))
                cursor = IDC_SIZEWE;
            else if (edge)
                cursor = IDC_SIZENS;
            else if (selected && PtInRect(&currentSelection, p) && activeTool == Tool::Select)
                cursor = IDC_SIZEALL;
            SetCursor(LoadCursorW(nullptr, cursor));
            return TRUE;
        }
        case WM_KEYDOWN: {
            bool ctrl = GetKeyState(VK_CONTROL) < 0;
            if (wp == VK_ESCAPE) {
                closeOverlay();
                return 0;
            }
            if (ctrl && wp == 'Z') {
                undoMark(GetKeyState(VK_SHIFT) < 0);
                return 0;
            }
            if (ctrl && wp == 'Y') {
                undoMark(true);
                return 0;
            }
            if ((ctrl && wp == 'C') || wp == VK_RETURN) {
                finishEditor(false);
                return 0;
            }
            if (ctrl && wp == 'S') {
                finishEditor(true);
                return 0;
            }
            if (ctrl && wp == 'A') {
                setSelection({0, 0, frozen.w, frozen.h});
                selected = true;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (selected && wp >= VK_LEFT && wp <= VK_DOWN) {
                int step = GetKeyState(VK_SHIFT) < 0 ? 10 : 1;
                auto r = selection();
                LONG dx = wp == VK_LEFT    ? -step
                          : wp == VK_RIGHT ? step
                                           : 0,
                     dy = wp == VK_UP     ? -step
                          : wp == VK_DOWN ? step
                                          : 0;
                dx = std::clamp<LONG>(dx, -r.left, frozen.w - r.right);
                dy = std::clamp<LONG>(dy, -r.top, frozen.h - r.bottom);
                OffsetRect(&r, dx, dy);
                setSelection(r);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            const wchar_t *keys = L"VROAPTM";
            const wchar_t *pos = wcschr(keys, (wchar_t)wp);
            if (!ctrl && pos && wp)
                performButton((int)(pos - keys));
            return 0;
        }
        case WM_CAPTURECHANGED:
            if (gesture != Gesture::None) {
                if (gesture == Gesture::New)
                    selected = false;
                else
                    setSelection(beforeDrag);
                gesture = Gesture::None;
                draft = {};
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE)
                PostMessageW(hwnd, WM_APP + 2, 0, 0);
            return 0;
        case WM_APP + 2: {
            HWND foreground = GetForegroundWindow();
            if (foreground != hwnd && !IsChild(hwnd, foreground) && !integration)
                closeOverlay();
            return 0;
        }
#ifdef HDRSNIP_TEST_BUILD
        case WM_APP + 20:
            runEditorIntegration();
            return 0;
#endif
        }
    } catch (const std::exception &e) {
        notify(errorMessage(e), true);
        if (gesture != Gesture::None) {
            gesture = Gesture::None;
            ReleaseCapture();
        }
#ifdef HDRSNIP_TEST_BUILD
        if (integration) {
            std::ofstream report(output / L"editor-results.txt", std::ios::app);
            report << "FAIL " << e.what() << "\n";
            closeOverlay();
            PostMessageW(host, WM_CLOSE, 0, 0);
        }
#endif
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
static void beginCapture() {
    if (busy)
        return;
    busy = true;
    try {
        POINT cursor;
        GetCursorPos(&cursor);
        frozen = capture(testCaptureTarget ? testCaptureTarget
                                           : MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST));
        preview = toneMap(frozen);
        darkPreview = preview;
        for (size_t i = 0; i < darkPreview.size(); i += 4)
            for (int k = 0; k < 3; ++k)
                darkPreview[i + k] = (BYTE)(darkPreview[i + k] * .43f);
        brightBuffer.create(frozen.w, frozen.h);
        dimBuffer.create(frozen.w, frozen.h);
        frameBuffer.create(frozen.w, frozen.h);
        memcpy(brightBuffer.pixels, preview.data(), preview.size());
        memcpy(dimBuffer.pixels, darkPreview.data(), darkPreview.size());
        start = finish = {};
        selected = false;
        gesture = Gesture::None;
        activeTool = Tool::Select;
        auto r = frozen.desktop;
        overlay = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"HdrSnipOverlay", L"微光 · 截图编辑",
                                  WS_POPUP | WS_CLIPCHILDREN, r.left, r.top, frozen.w, frozen.h, nullptr,
                                  nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!overlay)
            throw std::runtime_error("Cannot create screenshot editor");
        if (!resourceMode) {
            ShowWindow(overlay, SW_SHOW);
            SetWindowPos(overlay, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
            SetForegroundWindow(overlay);
            SetFocus(overlay);
        }
        if (integration && !resourceMode)
            PostMessageW(overlay, WM_APP + 20, 0, 0);
    } catch (const std::exception &e) {
        closeOverlay();
        notify(errorMessage(e), true);
        if (integration)
            PostMessageW(host, WM_CLOSE, 0, 0);
    }
}
static void warmCaptureDevices() {
    previewTables();
    ComPtr<IDXGIFactory1> factory;
    check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "Prepare capture adapters");
    for (UINT a = 0;; ++a) {
        ComPtr<IDXGIAdapter1> adapter;
        HRESULT ar = factory->EnumAdapters1(a, &adapter);
        if (ar == DXGI_ERROR_NOT_FOUND)
            break;
        check(ar, "Prepare adapter");
        bool hdr = false;
        for (UINT o = 0;; ++o) {
            ComPtr<IDXGIOutput> display;
            HRESULT hr = adapter->EnumOutputs(o, &display);
            if (hr == DXGI_ERROR_NOT_FOUND)
                break;
            check(hr, "Prepare display");
            ComPtr<IDXGIOutput6> output6;
            if (SUCCEEDED(display.As(&output6))) {
                DXGI_OUTPUT_DESC1 d{};
                if (SUCCEEDED(output6->GetDesc1(&d)) && d.AttachedToDesktop &&
                    d.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) {
                    hdr = true;
                    break;
                }
            }
        }
        if (hdr) {
            auto &cache = deviceFor(adapter.Get());
            cache.context->ClearState();
            cache.context->Flush();
            ComPtr<IDXGIDevice3> idle;
            if (SUCCEEDED(cache.device.As(&idle)))
                idle->Trim();
        }
    }
}
