#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
#include <unordered_set>
#include <vector>

namespace {
struct FindContext {
    DWORD processId = 0;
    HWND window = nullptr;
};

BOOL CALLBACK FindTopLevel(HWND window, LPARAM value) {
    auto* context = reinterpret_cast<FindContext*>(value);
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (processId == context->processId && IsWindowVisible(window)) {
        context->window = window;
        return FALSE;
    }
    return TRUE;
}

BOOL CALLBACK FindLargestView(HWND window, LPARAM value) {
    auto* result = reinterpret_cast<std::pair<HWND, LONG>*>(value);
    wchar_t className[128]{};
    GetClassNameW(window, className, 128);
    if (std::wstring(className) != L"TWebFrame.View.1") return TRUE;
    RECT bounds{};
    GetClientRect(window, &bounds);
    const LONG area = (bounds.right - bounds.left) * (bounds.bottom - bounds.top);
    if (area > result->second) *result = {window, area};
    return TRUE;
}

BOOL SaveClientBitmap(HWND window, const std::wstring& path) {
    RECT bounds{};
    if (!GetClientRect(window, &bounds)) return FALSE;
    const int width = bounds.right - bounds.left;
    const int height = bounds.bottom - bounds.top;
    HDC source = GetDC(window);
    HDC memory = CreateCompatibleDC(source);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(source, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    const auto previous = SelectObject(memory, bitmap);
    // Ask the actual view to paint into this DC; screen DCs can be clipped
    // completely when the browser is covered by the IDE or another window.
    DWORD_PTR printResult=0;
    const BOOL copied = SendMessageTimeoutW(window,WM_PRINTCLIENT,
        reinterpret_cast<WPARAM>(memory),PRF_CLIENT|PRF_CHILDREN,SMTO_ABORTIFHUNG,
        5000,&printResult)!=0;
    BITMAPFILEHEADER file{};
    file.bfType = 0x4d42;
    const DWORD imageSize = static_cast<DWORD>(width * height * 4);
    file.bfOffBits = sizeof(file) + sizeof(BITMAPINFOHEADER);
    file.bfSize = file.bfOffBits + imageSize;
    HANDLE output = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD written = 0;
    BOOL saved = copied && output != INVALID_HANDLE_VALUE &&
                 WriteFile(output, &file, sizeof(file), &written, nullptr) &&
                 WriteFile(output, &info.bmiHeader, sizeof(info.bmiHeader), &written, nullptr) &&
                 WriteFile(output, pixels, imageSize, &written, nullptr);
    if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
    SelectObject(memory, previous);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(window, source);
    return saved;
}

void PrintFramePixels(HWND root) {
    for (HWND child = FindWindowExW(root, nullptr, L"TWebFrame.View.1", nullptr);
         child;
         child = FindWindowExW(root, child, L"TWebFrame.View.1", nullptr)) {
        RECT bounds{};
        GetWindowRect(child, &bounds);
        MapWindowPoints(HWND_DESKTOP, root, reinterpret_cast<POINT*>(&bounds), 2);
        const int width = bounds.right - bounds.left;
        const int height = bounds.bottom - bounds.top;
        if (!IsWindowVisible(child) || width <= 20 || height <= 20) continue;
        HDC dc = GetDC(child);
        std::unordered_set<COLORREF> colors;
        size_t valid = 0, nonNeutral = 0;
        for (int y = 4; dc && y < height - 4; y += 7)
            for (int x = 4; x < width - 4; x += 7) {
                const auto color = GetPixel(dc, x, y);
                if (color == CLR_INVALID) continue;
                ++valid;
                colors.insert(color);
                const auto r = GetRValue(color), g = GetGValue(color), b = GetBValue(color);
                if (std::max({r, g, b}) - std::min({r, g, b}) > 8 ||
                    r < 235 || g < 235 || b < 235)
                    ++nonNeutral;
            }
        if (dc) ReleaseDC(child, dc);
        std::wcout << L"FRAME " << bounds.left << L':' << bounds.top << L':' << width
                   << L':' << height << L" valid=" << valid << L" unique="
                   << colors.size() << L" nonNeutral=" << nonNeutral << L'\n';
    }
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) return 2;
    SetProcessDPIAware();
    FindContext context{static_cast<DWORD>(std::wcstoul(argv[1], nullptr, 10)), nullptr};
    for (int attempt = 0; attempt < 100 && !context.window; ++attempt) {
        EnumWindows(FindTopLevel, reinterpret_cast<LPARAM>(&context));
        if (!context.window) Sleep(100);
    }
    if (!context.window) return 3;
    std::pair<HWND, LONG> largest{nullptr, 0};
    EnumChildWindows(context.window, FindLargestView, reinterpret_cast<LPARAM>(&largest));
    if (!largest.first) return 4;
    if(argc<4||std::wstring(argv[3])!=L"--no-scroll"){
        SetForegroundWindow(context.window);
        SetFocus(largest.first);
        for (int index = 0; index < 8; ++index)
            PostMessageW(largest.first, WM_MOUSEWHEEL, MAKEWPARAM(0, -WHEEL_DELTA),
                         MAKELPARAM(400, 400));
    }
    Sleep(1500);
    RedrawWindow(context.window,nullptr,nullptr,RDW_INVALIDATE|RDW_UPDATENOW|RDW_ALLCHILDREN);
    PrintFramePixels(largest.first);
    return SaveClientBitmap(largest.first, argv[2]) ? 0 : 5;
}
