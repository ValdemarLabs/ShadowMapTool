#include "gui/App.hpp"
#include "gui/Resources.h"

#include "archive/MapArchive.hpp"
#include "assets/AssetProvider.hpp"
#include "formats/Png.hpp"
#include "shadow/ShadowGenerator.hpp"
#include "util/FileIO.hpp"

#include <d2d1helper.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cwctype>
#include <exception>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string_view>
#include <vector>

namespace w3shadow::gui {
namespace {

using Microsoft::WRL::ComPtr;

constexpr wchar_t WindowClassName[] = L"W3ShadowModernWindow";
constexpr wchar_t WindowTitle[] = L"ShadowMap Tool";
constexpr wchar_t SettingsRegistryKey[] = L"Software\\ShadowMapTool";

D2D1_COLOR_F rgb(const std::uint32_t value, const float alpha = 1.0F)
{
    return D2D1::ColorF(
        static_cast<float>((value >> 16U) & 0xFFU) / 255.0F,
        static_cast<float>((value >> 8U) & 0xFFU) / 255.0F,
        static_cast<float>(value & 0xFFU) / 255.0F,
        alpha);
}

bool contains(const D2D1_RECT_F& rectangle, const float x, const float y)
{
    return x >= rectangle.left && x <= rectangle.right &&
           y >= rectangle.top && y <= rectangle.bottom;
}

bool createIconBitmap(
    ID2D1RenderTarget* target,
    const HICON icon,
    const UINT size,
    ComPtr<ID2D1Bitmap>& output)
{
    BITMAPINFO information{};
    information.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    information.bmiHeader.biWidth = static_cast<LONG>(size);
    information.bmiHeader.biHeight = -static_cast<LONG>(size);
    information.bmiHeader.biPlanes = 1;
    information.bmiHeader.biBitCount = 32;
    information.bmiHeader.biCompression = BI_RGB;

    void* pixels = nullptr;
    const HBITMAP bitmap =
        CreateDIBSection(nullptr, &information, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (bitmap == nullptr || pixels == nullptr) return false;
    const HDC context = CreateCompatibleDC(nullptr);
    if (context == nullptr) {
        DeleteObject(bitmap);
        return false;
    }

    const HGDIOBJ previous = SelectObject(context, bitmap);
    std::memset(pixels, 0, static_cast<std::size_t>(size) * size * 4U);
    const BOOL drawn = DrawIconEx(context, 0, 0, icon, static_cast<int>(size),
                                  static_cast<int>(size), 0, nullptr, DI_NORMAL);
    SelectObject(context, previous);
    DeleteDC(context);
    if (!drawn) {
        DeleteObject(bitmap);
        return false;
    }

    const auto properties = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    const HRESULT result = target->CreateBitmap(
        D2D1::SizeU(size, size), pixels, size * 4U, properties, &output);
    DeleteObject(bitmap);
    return SUCCEEDED(result);
}

std::wstring widen(const std::string_view value)
{
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                         static_cast<int>(value.size()), nullptr, 0);
    const UINT codePage = size > 0 ? CP_UTF8 : CP_ACP;
    const DWORD flags = size > 0 ? MB_ERR_INVALID_CHARS : 0;
    const int fallbackSize = MultiByteToWideChar(codePage, flags, value.data(),
                                                 static_cast<int>(value.size()), nullptr, 0);
    if (fallbackSize <= 0) return L"Unknown error";
    std::wstring result(static_cast<std::size_t>(fallbackSize), L'\0');
    MultiByteToWideChar(codePage, flags, value.data(), static_cast<int>(value.size()),
                        result.data(), fallbackSize);
    return result;
}

std::string narrow(const std::wstring_view value)
{
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                         static_cast<int>(value.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring formatBytes(const std::uint64_t bytes)
{
    std::wostringstream stream;
    if (bytes >= 1024U * 1024U) {
        stream.setf(std::ios::fixed);
        stream.precision(1);
        stream << static_cast<double>(bytes) / (1024.0 * 1024.0) << L" MiB";
    } else {
        stream << (bytes + 1023U) / 1024U << L" KiB";
    }
    return stream.str();
}

std::wstring patternLabel(const Pattern pattern)
{
    switch (pattern) {
    case Pattern::Black: return L"All shadow";
    case Pattern::White: return L"All lit";
    case Pattern::Checker: return L"Checker";
    case Pattern::XGradient: return L"X gradient";
    case Pattern::YGradient: return L"Y gradient";
    case Pattern::Quadrants: return L"Quadrants";
    }
    return L"Pattern";
}

std::wstring patternFilePart(const Pattern pattern)
{
    switch (pattern) {
    case Pattern::Black: return L"black";
    case Pattern::White: return L"white";
    case Pattern::Checker: return L"checker";
    case Pattern::XGradient: return L"x-gradient";
    case Pattern::YGradient: return L"y-gradient";
    case Pattern::Quadrants: return L"quadrants";
    }
    return L"pattern";
}

std::optional<std::filesystem::path> openMapDialog(HWND owner)
{
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog)))) {
        throw std::runtime_error("Unable to create the Windows open dialog");
    }

    const COMDLG_FILTERSPEC filters[] = {
        {L"Warcraft III maps", L"*.w3x;*.w3m"},
        {L"All files", L"*.*"}};
    dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
    dialog->SetTitle(L"Choose a Warcraft III map");
    FILEOPENDIALOGOPTIONS options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);
    const HRESULT shown = dialog->Show(owner);
    if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return std::nullopt;
    if (FAILED(shown)) throw std::runtime_error("The Windows open dialog failed");

    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(&item))) throw std::runtime_error("No map was selected");
    PWSTR path = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
        throw std::runtime_error("Unable to read the selected map path");
    }
    const std::filesystem::path result(path);
    CoTaskMemFree(path);
    return result;
}

std::optional<std::filesystem::path> chooseFolderDialog(
    HWND owner, const std::wstring_view title)
{
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog)))) {
        throw std::runtime_error("Unable to create the Windows folder dialog");
    }
    dialog->SetTitle(std::wstring(title).c_str());
    FILEOPENDIALOGOPTIONS options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_PATHMUSTEXIST);
    const HRESULT shown = dialog->Show(owner);
    if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return std::nullopt;
    if (FAILED(shown)) throw std::runtime_error("The Windows folder dialog failed");

    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(&item))) throw std::runtime_error("No folder was selected");
    PWSTR path = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
        throw std::runtime_error("Unable to read the selected folder path");
    }
    const std::filesystem::path result(path);
    CoTaskMemFree(path);
    return result;
}

std::optional<std::filesystem::path> openLibraryDialog(HWND owner)
{
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog)))) {
        throw std::runtime_error("Unable to create the Windows open dialog");
    }
    const COMDLG_FILTERSPEC filters[] = {
        {L"CascLib library", L"CascLib.dll"},
        {L"Dynamic-link libraries", L"*.dll"}};
    dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
    dialog->SetTitle(L"Choose CascLib.dll");
    FILEOPENDIALOGOPTIONS options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);
    const HRESULT shown = dialog->Show(owner);
    if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return std::nullopt;
    if (FAILED(shown)) throw std::runtime_error("The Windows open dialog failed");

    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(&item))) throw std::runtime_error("No library was selected");
    PWSTR path = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
        throw std::runtime_error("Unable to read the selected library path");
    }
    const std::filesystem::path result(path);
    CoTaskMemFree(path);
    return result;
}

std::optional<std::filesystem::path> readRegistryPath(
    const HKEY root, const wchar_t* key, const wchar_t* name)
{
    DWORD byteCount = 0;
    if (RegGetValueW(root, key, name, RRF_RT_REG_SZ, nullptr, nullptr, &byteCount) != ERROR_SUCCESS ||
        byteCount < sizeof(wchar_t)) {
        return std::nullopt;
    }
    std::wstring value(byteCount / sizeof(wchar_t), L'\0');
    if (RegGetValueW(root, key, name, RRF_RT_REG_SZ, nullptr, value.data(), &byteCount) !=
        ERROR_SUCCESS) {
        return std::nullopt;
    }
    while (!value.empty() && value.back() == L'\0') value.pop_back();
    return value.empty() ? std::nullopt
                         : std::optional<std::filesystem::path>(std::filesystem::path(value));
}

std::optional<std::filesystem::path> executableDirectory()
{
    std::vector<wchar_t> path(32768U);
    const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0U || length >= path.size()) return std::nullopt;
    return std::filesystem::path(path.data(), path.data() + length).parent_path();
}

bool isWarcraftDirectory(const std::filesystem::path& path)
{
    return isCascWarcraftDirectory(path) || isLegacyWarcraftDirectory(path);
}

bool isCascLibrary(const std::filesystem::path& path)
{
    return !path.empty() && std::filesystem::is_regular_file(path);
}

bool hasUsableAssetSettings(
    const std::optional<std::filesystem::path>& warcraftDirectory,
    const std::optional<std::filesystem::path>& cascLibrary)
{
    if (!warcraftDirectory) return false;
    if (isLegacyWarcraftDirectory(*warcraftDirectory)) return true;
    return isCascWarcraftDirectory(*warcraftDirectory) && cascLibrary &&
           isCascLibrary(*cascLibrary);
}

std::optional<std::filesystem::path> saveFileDialog(
    HWND owner,
    const std::wstring_view title,
    const std::filesystem::path& suggested,
    const wchar_t* filterName,
    const wchar_t* filterPattern,
    const wchar_t* extension)
{
    ComPtr<IFileSaveDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog)))) {
        throw std::runtime_error("Unable to create the Windows save dialog");
    }

    const COMDLG_FILTERSPEC filter[] = {{filterName, filterPattern}};
    dialog->SetFileTypes(1, filter);
    dialog->SetDefaultExtension(extension);
    dialog->SetFileName(suggested.filename().c_str());
    dialog->SetTitle(std::wstring(title).c_str());
    FILEOPENDIALOGOPTIONS options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PATHMUSTEXIST | FOS_OVERWRITEPROMPT | FOS_STRICTFILETYPES);

    const auto parent = suggested.parent_path();
    if (!parent.empty()) {
        ComPtr<IShellItem> folder;
        if (SUCCEEDED(SHCreateItemFromParsingName(parent.c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
            dialog->SetFolder(folder.Get());
        }
    }

    const HRESULT shown = dialog->Show(owner);
    if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return std::nullopt;
    if (FAILED(shown)) throw std::runtime_error("The Windows save dialog failed");

    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(&item))) throw std::runtime_error("No output path was selected");
    PWSTR path = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
        throw std::runtime_error("Unable to read the selected output path");
    }
    const std::filesystem::path result(path);
    CoTaskMemFree(path);
    return result;
}

bool isMapExtension(std::filesystem::path path)
{
    auto extension = path.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](const wchar_t character) { return static_cast<wchar_t>(std::towlower(character)); });
    return extension == L".w3x" || extension == L".w3m";
}

} // namespace

int App::run(
    HINSTANCE instance,
    const int showCommand,
    std::optional<std::filesystem::path> startupMap,
    const bool smokeTest)
{
    initializeSessionLog();
    logEvent(StatusKind::Neutral, L"ShadowMap Tool 1.3 session started.");
    if (!initializeFactories()) {
        logEvent(StatusKind::Error, L"Application startup failed: graphics factories could not be initialized.");
        return 1;
    }
    if (!createWindow(instance, smokeTest ? SW_HIDE : showCommand)) {
        logEvent(StatusKind::Error, L"Application startup failed: the main window could not be created.");
        return 1;
    }
    if (!smokeTest) loadAssetSettings(true);
    if (startupMap) loadMap(*startupMap);
    if (smokeTest) {
        showHelp_ = true;
        updateEditControls();
        if (!createDeviceResources()) {
            logEvent(StatusKind::Error, L"GUI smoke test failed: device resources could not be created.");
            return 2;
        }
        paint();
        showHelp_ = false;
        showAbout_ = true;
        aboutSection_ = 0;
        updateEditControls();
        paint();
        showAbout_ = false;
        showAdvanced_ = true;
        focused_ = Target::AdvancedGaussianRadius;
        updateEditControls();
        paint();
        SetTimer(window_, 1U, 100U, nullptr);
    }

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    logEvent(StatusKind::Neutral, L"Session ended.");
    return static_cast<int>(message.wParam);
}

bool App::initializeFactories()
{
    if (FAILED(D2D1CreateFactory(
            D2D1_FACTORY_TYPE_SINGLE_THREADED,
            __uuidof(ID2D1Factory),
            nullptr,
            reinterpret_cast<void**>(d2dFactory_.GetAddressOf())))) {
        MessageBoxW(nullptr, L"Unable to initialize Direct2D.", WindowTitle, MB_OK | MB_ICONERROR);
        return false;
    }
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(writeFactory_.GetAddressOf())))) {
        MessageBoxW(nullptr, L"Unable to initialize DirectWrite.", WindowTitle, MB_OK | MB_ICONERROR);
        return false;
    }
    return true;
}

bool App::createWindow(HINSTANCE instance, const int showCommand)
{
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = windowProcedure;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = static_cast<HICON>(LoadImageW(
        instance, MAKEINTRESOURCEW(IDI_W3SHADOW), IMAGE_ICON,
        GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON),
        LR_DEFAULTCOLOR | LR_SHARED));
    if (windowClass.hIcon == nullptr) windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    windowClass.hIconSm = static_cast<HICON>(LoadImageW(
        instance, MAKEINTRESOURCEW(IDI_W3SHADOW), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
        LR_DEFAULTCOLOR | LR_SHARED));
    if (windowClass.hIconSm == nullptr) windowClass.hIconSm = windowClass.hIcon;
    windowClass.hbrBackground = nullptr;
    windowClass.lpszClassName = WindowClassName;
    if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"Unable to register the application window.", WindowTitle,
                    MB_OK | MB_ICONERROR);
        return false;
    }

    const UINT systemDpi = GetDpiForSystem();
    const float scale = static_cast<float>(systemDpi) / 96.0F;
    window_ = CreateWindowExW(
        WS_EX_APPWINDOW | WS_EX_ACCEPTFILES,
        WindowClassName,
        WindowTitle,
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        static_cast<int>(1180.0F * scale),
        static_cast<int>(760.0F * scale),
        nullptr,
        nullptr,
        instance,
        this);
    if (window_ == nullptr) {
        MessageBoxW(nullptr, L"Unable to create the application window.", WindowTitle,
                    MB_OK | MB_ICONERROR);
        return false;
    }

    dpi_ = GetDpiForWindow(window_);
    editBrush_ = CreateSolidBrush(RGB(24, 33, 54));
    constexpr std::array<const wchar_t*, 3> defaults{L"1", L"1", L"-1"};
    for (std::size_t index = 0; index < lightEdits_.size(); ++index) {
        lightEdits_[index] = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", defaults[index],
            WS_CHILD | ES_CENTER | ES_AUTOHSCROLL | WS_TABSTOP,
            0, 0, 48, 28, window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(1001U + index)),
            instance, nullptr);
        if (lightEdits_[index] == nullptr) {
            MessageBoxW(window_, L"Unable to create the light-vector controls.", WindowTitle,
                        MB_OK | MB_ICONERROR);
            return false;
        }
        SendMessageW(lightEdits_[index], WM_SETFONT,
                     reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
    }
    BOOL darkMode = TRUE;
    constexpr DWORD UseImmersiveDarkMode = 20;
    DwmSetWindowAttribute(window_, UseImmersiveDarkMode, &darkMode, sizeof(darkMode));
    DragAcceptFiles(window_, TRUE);
    updateEditControls();
    ShowWindow(window_, showCommand);
    UpdateWindow(window_);
    return true;
}

LRESULT CALLBACK App::windowProcedure(
    const HWND window,
    const UINT message,
    const WPARAM wParam,
    const LPARAM lParam)
{
    App* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        app = static_cast<App*>(create->lpCreateParams);
        app->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    return app != nullptr ? app->handleMessage(message, wParam, lParam)
                          : DefWindowProcW(window, message, wParam, lParam);
}

LRESULT App::handleMessage(const UINT message, const WPARAM wParam, const LPARAM lParam)
{
    switch (message) {
    case WM_PAINT: {
        PAINTSTRUCT paintStructure{};
        BeginPaint(window_, &paintStructure);
        paint();
        EndPaint(window_, &paintStructure);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_COMMAND:
        if (HIWORD(wParam) == EN_CHANGE && LOWORD(wParam) >= 1001U &&
            LOWORD(wParam) <= 1003U && previewKind_ == PreviewKind::Calculated) {
            calculationDirty_ = true;
            InvalidateRect(window_, nullptr, FALSE);
        }
        break;
    case WM_CTLCOLOREDIT: {
        const HDC context = reinterpret_cast<HDC>(wParam);
        SetTextColor(context, RGB(244, 247, 255));
        SetBkColor(context, RGB(24, 33, 54));
        return reinterpret_cast<LRESULT>(editBrush_);
    }
    case WM_SIZE:
        resize(LOWORD(lParam), HIWORD(lParam));
        return 0;
    case WM_DPICHANGED: {
        updateDpi(HIWORD(wParam));
        const auto* suggested = reinterpret_cast<RECT*>(lParam);
        SetWindowPos(window_, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left, suggested->bottom - suggested->top,
                     SWP_NOACTIVATE | SWP_NOZORDER);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* information = reinterpret_cast<MINMAXINFO*>(lParam);
        const float scale = static_cast<float>(dpi_) / 96.0F;
        information->ptMinTrackSize.x = static_cast<LONG>(1020.0F * scale);
        information->ptMinTrackSize.y = static_cast<LONG>(680.0F * scale);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        const auto point = mousePoint(lParam);
        const auto target = hitTest(point.x, point.y);
        if (showAdvanced_ && isSlider(target)) {
            focused_ = target;
            draggedSlider_ = target;
            SetCapture(window_);
            updateSlider(target, point.x);
            return 0;
        }
        break;
    }
    case WM_MOUSEMOVE: {
        if (!trackingMouse_) {
            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window_, 0};
            TrackMouseEvent(&tracking);
            trackingMouse_ = true;
        }
        const auto point = mousePoint(lParam);
        if (draggedSlider_ != Target::None) {
            updateSlider(draggedSlider_, point.x);
            return 0;
        }
        const auto target = hitTest(point.x, point.y);
        if (target != hovered_) {
            hovered_ = target;
            InvalidateRect(window_, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        trackingMouse_ = false;
        hovered_ = Target::None;
        InvalidateRect(window_, nullptr, FALSE);
        return 0;
    case WM_LBUTTONUP: {
        const auto point = mousePoint(lParam);
        if (draggedSlider_ != Target::None) {
            updateSlider(draggedSlider_, point.x);
            draggedSlider_ = Target::None;
            ReleaseCapture();
            return 0;
        }
        const auto target = hitTest(point.x, point.y);
        if (target != Target::None) {
            focused_ = target;
            activate(target);
        }
        return 0;
    }
    case WM_CAPTURECHANGED:
        draggedSlider_ = Target::None;
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE && showAdvanced_) {
            showAdvanced_ = false;
            focused_ = Target::Advanced;
            updateEditControls();
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        }
        if (wParam == VK_ESCAPE && showAssets_) {
            showAssets_ = false;
            focused_ = Target::Assets;
            updateEditControls();
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        }
        if (wParam == VK_ESCAPE && showHelp_) {
            showHelp_ = false;
            focused_ = Target::Help;
            updateEditControls();
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        }
        if (wParam == VK_ESCAPE && showAbout_) {
            showAbout_ = false;
            focused_ = Target::About;
            updateEditControls();
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        }
        if (wParam == VK_F1) {
            showAdvanced_ = false;
            showAssets_ = false;
            showAbout_ = false;
            showHelp_ = true;
            focused_ = Target::HelpClose;
            updateEditControls();
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        }
        if (wParam == VK_TAB) {
            moveFocus((GetKeyState(VK_SHIFT) & 0x8000) != 0);
            return 0;
        }
        if (showAdvanced_ && isSlider(focused_) &&
            (wParam == VK_LEFT || wParam == VK_RIGHT)) {
            stepSlider(focused_, wParam == VK_LEFT ? -1 : 1);
            return 0;
        }
        if (wParam == VK_RETURN || wParam == VK_SPACE) {
            activate(focused_);
            return 0;
        }
        if (wParam == 'O' && (GetKeyState(VK_CONTROL) & 0x8000) != 0) {
            chooseMap();
            return 0;
        }
        if (wParam == 'S' && (GetKeyState(VK_CONTROL) & 0x8000) != 0) {
            calculateShadows();
            return 0;
        }
        break;
    case WM_DROPFILES: {
        const HDROP drop = reinterpret_cast<HDROP>(wParam);
        const UINT length = DragQueryFileW(drop, 0, nullptr, 0);
        std::vector<wchar_t> path(static_cast<std::size_t>(length) + 1U);
        if (length != 0 && DragQueryFileW(drop, 0, path.data(), static_cast<UINT>(path.size())) != 0) {
            const std::filesystem::path dropped(path.data());
            if (isMapExtension(dropped)) loadMap(dropped);
            else setStatus(L"Drop a .w3x or .w3m map file.", StatusKind::Warning);
        }
        DragFinish(drop);
        return 0;
    }
    case WM_TIMER:
        if (wParam == 1U) {
            KillTimer(window_, 1U);
            DestroyWindow(window_);
            return 0;
        }
        break;
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            POINT pixel{};
            GetCursorPos(&pixel);
            ScreenToClient(window_, &pixel);
            const float scale = static_cast<float>(dpi_) / 96.0F;
            if (hitTest(pixel.x / scale, pixel.y / scale) != Target::None) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
        }
        break;
    case WM_DESTROY:
        if (editBrush_ != nullptr) {
            DeleteObject(editBrush_);
            editBrush_ = nullptr;
        }
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window_, message, wParam, lParam);
}

bool App::createDeviceResources()
{
    if (renderTarget_) return true;
    RECT client{};
    GetClientRect(window_, &client);
    const auto pixelSize = D2D1::SizeU(
        static_cast<UINT32>(std::max<LONG>(1, client.right - client.left)),
        static_cast<UINT32>(std::max<LONG>(1, client.bottom - client.top)));
    if (FAILED(d2dFactory_->CreateHwndRenderTarget(
            D2D1::RenderTargetProperties(),
            D2D1::HwndRenderTargetProperties(window_, pixelSize),
            &renderTarget_))) {
        return false;
    }
    renderTarget_->SetDpi(static_cast<float>(dpi_), static_cast<float>(dpi_));

    const auto makeBrush = [&](const std::uint32_t color, ComPtr<ID2D1SolidColorBrush>& brush) {
        return renderTarget_->CreateSolidColorBrush(rgb(color), &brush);
    };
    if (FAILED(makeBrush(0x0B1020, backgroundBrush_)) ||
        FAILED(makeBrush(0x11182A, panelBrush_)) ||
        FAILED(makeBrush(0x182136, surfaceBrush_)) ||
        FAILED(makeBrush(0x2A3653, borderBrush_)) ||
        FAILED(makeBrush(0xF4F7FF, textBrush_)) ||
        FAILED(makeBrush(0x97A3BA, mutedBrush_)) ||
        FAILED(makeBrush(0x7C5CFC, accentBrush_)) ||
        FAILED(makeBrush(0x49D19A, successBrush_)) ||
        FAILED(makeBrush(0xFFCB6B, warningBrush_)) ||
        FAILED(makeBrush(0xFF6B7A, errorBrush_))) {
        discardDeviceResources();
        return false;
    }

    const auto makeFormat = [&](const float size, const DWRITE_FONT_WEIGHT weight,
                                ComPtr<IDWriteTextFormat>& format) {
        return writeFactory_->CreateTextFormat(
            L"Segoe UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, size, L"en-US", &format);
    };
    if (FAILED(makeFormat(26.0F, DWRITE_FONT_WEIGHT_SEMI_BOLD, titleFormat_)) ||
        FAILED(makeFormat(16.0F, DWRITE_FONT_WEIGHT_SEMI_BOLD, headingFormat_)) ||
        FAILED(makeFormat(14.0F, DWRITE_FONT_WEIGHT_NORMAL, bodyFormat_)) ||
        FAILED(makeFormat(12.0F, DWRITE_FONT_WEIGHT_NORMAL, smallFormat_)) ||
        FAILED(makeFormat(13.0F, DWRITE_FONT_WEIGHT_SEMI_BOLD, buttonFormat_)) ||
        FAILED(makeFormat(14.0F, DWRITE_FONT_WEIGHT_NORMAL, helpBodyFormat_))) {
        discardDeviceResources();
        return false;
    }
    buttonFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    buttonFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    helpBodyFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    for (auto* format : {bodyFormat_.Get(), smallFormat_.Get()}) {
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }

    const auto appIcon = static_cast<HICON>(LoadImageW(
        GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_W3SHADOW), IMAGE_ICON,
        64, 64, LR_DEFAULTCOLOR | LR_SHARED));
    if (appIcon != nullptr) createIconBitmap(renderTarget_.Get(), appIcon, 64U, appIconBitmap_);

    if (!shadowMap_) rebuildPreview();
    else refreshPreviewBitmap();
    return true;
}

void App::discardDeviceResources()
{
    previewBitmap_.Reset();
    backgroundBrush_.Reset();
    panelBrush_.Reset();
    surfaceBrush_.Reset();
    borderBrush_.Reset();
    textBrush_.Reset();
    mutedBrush_.Reset();
    accentBrush_.Reset();
    successBrush_.Reset();
    warningBrush_.Reset();
    errorBrush_.Reset();
    appIconBitmap_.Reset();
    titleFormat_.Reset();
    headingFormat_.Reset();
    bodyFormat_.Reset();
    smallFormat_.Reset();
    buttonFormat_.Reset();
    helpBodyFormat_.Reset();
    renderTarget_.Reset();
}

void App::resize(const UINT width, const UINT height)
{
    if (renderTarget_) renderTarget_->Resize(D2D1::SizeU(width, height));
    updateEditControls();
    InvalidateRect(window_, nullptr, FALSE);
}

void App::updateDpi(const UINT dpi)
{
    dpi_ = dpi;
    if (renderTarget_) renderTarget_->SetDpi(static_cast<float>(dpi), static_cast<float>(dpi));
    updateEditControls();
    InvalidateRect(window_, nullptr, FALSE);
}

App::Layout App::calculateLayout() const
{
    const auto size = renderTarget_ ? renderTarget_->GetSize() : D2D1::SizeF(1180.0F, 760.0F);
    constexpr float margin = 28.0F;
    constexpr float leftWidth = 350.0F;
    constexpr float gap = 22.0F;
    const float contentTop = 98.0F;
    const float contentBottom = size.height - 62.0F;
    const float rightLeft = margin + leftWidth + gap;

    Layout layout;
    layout.mapCard = D2D1::RectF(margin, contentTop, margin + leftWidth, contentTop + 116.0F);
    layout.browse = D2D1::RectF(layout.mapCard.right - 98.0F, layout.mapCard.top + 60.0F,
                                layout.mapCard.right - 16.0F, layout.mapCard.top + 98.0F);

    layout.patternCard = D2D1::RectF(margin, contentTop + 130.0F,
                                     margin + leftWidth, contentTop + 452.0F);
    const float optionLeft = layout.patternCard.left + 16.0F;
    const float optionWidth = (leftWidth - 42.0F) / 2.0F;
    for (std::size_t index = 0; index < layout.patterns.size(); ++index) {
        const float column = static_cast<float>(index % 2U);
        const float row = static_cast<float>(index / 2U);
        layout.patterns[index] = D2D1::RectF(
            optionLeft + column * (optionWidth + 10.0F),
            layout.patternCard.top + 54.0F + row * 49.0F,
            optionLeft + column * (optionWidth + 10.0F) + optionWidth,
            layout.patternCard.top + 94.0F + row * 49.0F);
    }
    for (std::size_t index = 0; index < layout.calculationOptions.size(); ++index) {
        const float column = static_cast<float>(index % 2U);
        const float row = static_cast<float>(index / 2U);
        layout.calculationOptions[index] = D2D1::RectF(
            optionLeft + column * (optionWidth + 10.0F),
            layout.patternCard.top + 50.0F + row * 46.0F,
            optionLeft + column * (optionWidth + 10.0F) + optionWidth,
            layout.patternCard.top + 87.0F + row * 46.0F);
    }
    const float qualityWidth = (leftWidth - 52.0F) / 3.0F;
    for (std::size_t index = 0; index < layout.qualityOptions.size(); ++index) {
        layout.qualityOptions[index] = D2D1::RectF(
            optionLeft + static_cast<float>(index) * (qualityWidth + 10.0F),
            layout.patternCard.top + 202.0F,
            optionLeft + static_cast<float>(index) * (qualityWidth + 10.0F) + qualityWidth,
            layout.patternCard.top + 239.0F);
    }
    const float editWidth = 62.0F;
    for (std::size_t index = 0; index < layout.lightEdits.size(); ++index) {
        layout.lightEdits[index] = D2D1::RectF(
            optionLeft + static_cast<float>(index) * (editWidth + 22.0F),
            layout.patternCard.top + 276.0F,
            optionLeft + static_cast<float>(index) * (editWidth + 22.0F) + editWidth,
            layout.patternCard.top + 310.0F);
    }

    layout.advanced = D2D1::RectF(
        layout.patternCard.right - 142.0F, layout.patternCard.top + 10.0F,
        layout.patternCard.right - 16.0F, layout.patternCard.top + 40.0F);

    layout.outputCard = D2D1::RectF(margin, contentTop + 466.0F,
                                    margin + leftWidth, contentTop + 562.0F);
    const float modeWidth = (leftWidth - 42.0F) / 2.0F;
    layout.copyMode = D2D1::RectF(layout.outputCard.left + 16.0F, layout.outputCard.top + 44.0F,
                                  layout.outputCard.left + 16.0F + modeWidth,
                                  layout.outputCard.top + 82.0F);
    layout.inPlaceMode = D2D1::RectF(layout.copyMode.right + 10.0F, layout.copyMode.top,
                                     layout.outputCard.right - 16.0F, layout.copyMode.bottom);

    layout.previewCard = D2D1::RectF(rightLeft, contentTop, size.width - margin, contentBottom);
    layout.previewCanvas = D2D1::RectF(layout.previewCard.left + 18.0F,
                                       layout.previewCard.top + 66.0F,
                                       layout.previewCard.right - 18.0F,
                                       layout.previewCard.bottom - 78.0F);
    const float actionsTop = layout.previewCard.bottom - 58.0F;
    layout.exportShd = D2D1::RectF(layout.previewCard.left + 18.0F, actionsTop,
                                   layout.previewCard.left + 126.0F, actionsTop + 40.0F);
    layout.exportPng = D2D1::RectF(layout.exportShd.right + 10.0F, actionsTop,
                                   layout.exportShd.right + 118.0F, actionsTop + 40.0F);
    layout.buildMap = D2D1::RectF(layout.previewCard.right - 330.0F, actionsTop,
                                  layout.previewCard.right - 168.0F, actionsTop + 40.0F);
    layout.saveMap = D2D1::RectF(layout.previewCard.right - 158.0F, actionsTop,
                                 layout.previewCard.right - 18.0F, actionsTop + 40.0F);
    layout.testMode = D2D1::RectF(size.width - 484.0F, 29.0F, size.width - 372.0F, 61.0F);
    layout.assets = D2D1::RectF(size.width - 362.0F, 29.0F, size.width - 286.0F, 61.0F);
    layout.logs = D2D1::RectF(size.width - 276.0F, 29.0F, size.width - 210.0F, 61.0F);
    layout.about = D2D1::RectF(size.width - 200.0F, 29.0F, size.width - 112.0F, 61.0F);
    layout.help = D2D1::RectF(size.width - 102.0F, 29.0F, size.width - 28.0F, 61.0F);
    const float helpWidth = std::min(760.0F, size.width - 80.0F);
    const float helpHeight = std::min(570.0F, size.height - 80.0F);
    const float helpLeft = (size.width - helpWidth) / 2.0F;
    const float helpTop = (size.height - helpHeight) / 2.0F;
    layout.helpClose = D2D1::RectF(helpLeft + helpWidth - 120.0F, helpTop + helpHeight - 58.0F,
                                   helpLeft + helpWidth - 24.0F, helpTop + helpHeight - 20.0F);
    const float assetsWidth = std::min(760.0F, size.width - 80.0F);
    const float assetsHeight = std::min(410.0F, size.height - 80.0F);
    const float assetsLeft = (size.width - assetsWidth) / 2.0F;
    const float assetsTop = (size.height - assetsHeight) / 2.0F;
    layout.assetsWarcraftBrowse = D2D1::RectF(
        assetsLeft + assetsWidth - 132.0F, assetsTop + 116.0F,
        assetsLeft + assetsWidth - 24.0F, assetsTop + 154.0F);
    layout.assetsCascBrowse = D2D1::RectF(
        assetsLeft + assetsWidth - 132.0F, assetsTop + 215.0F,
        assetsLeft + assetsWidth - 24.0F, assetsTop + 253.0F);
    layout.assetsAutoDetect = D2D1::RectF(
        assetsLeft + 24.0F, assetsTop + assetsHeight - 60.0F,
        assetsLeft + 152.0F, assetsTop + assetsHeight - 22.0F);
    layout.assetsClose = D2D1::RectF(
        assetsLeft + assetsWidth - 120.0F, assetsTop + assetsHeight - 60.0F,
        assetsLeft + assetsWidth - 24.0F, assetsTop + assetsHeight - 22.0F);
    const float aboutWidth = std::min(840.0F, size.width - 80.0F);
    const float aboutHeight = std::min(610.0F, size.height - 60.0F);
    const float aboutLeft = (size.width - aboutWidth) / 2.0F;
    const float aboutTop = (size.height - aboutHeight) / 2.0F;
    constexpr std::array<float, 4> contentHeights{148.0F, 154.0F, 142.0F, 78.0F};
    float sectionTop = aboutTop + 76.0F;
    for (std::size_t index = 0; index < layout.aboutSections.size(); ++index) {
        layout.aboutSections[index] = D2D1::RectF(
            aboutLeft + 24.0F, sectionTop, aboutLeft + aboutWidth - 24.0F, sectionTop + 40.0F);
        sectionTop += 48.0F;
        if (aboutSection_ == static_cast<int>(index)) {
            layout.aboutContents[index] = D2D1::RectF(
                aboutLeft + 34.0F, sectionTop - 2.0F,
                aboutLeft + aboutWidth - 34.0F, sectionTop + contentHeights[index] - 2.0F);
            sectionTop += contentHeights[index];
        }
    }
    layout.aboutClose = D2D1::RectF(
        aboutLeft + aboutWidth - 120.0F, aboutTop + aboutHeight - 58.0F,
        aboutLeft + aboutWidth - 24.0F, aboutTop + aboutHeight - 20.0F);
    const float advancedWidth = std::min(760.0F, size.width - 60.0F);
    const float advancedHeight = std::min(630.0F, size.height - 30.0F);
    const float advancedLeft = (size.width - advancedWidth) / 2.0F;
    const float advancedTop = (size.height - advancedHeight) / 2.0F;
    const float toggleWidth = (advancedWidth - 68.0F) / 3.0F;
    for (std::size_t index = 0; index < layout.advancedToggles.size(); ++index) {
        layout.advancedToggles[index] = D2D1::RectF(
            advancedLeft + 24.0F + static_cast<float>(index) * (toggleWidth + 10.0F),
            advancedTop + 100.0F,
            advancedLeft + 24.0F + static_cast<float>(index) * (toggleWidth + 10.0F) +
                toggleWidth,
            advancedTop + 140.0F);
    }
    for (std::size_t index = 0; index < layout.advancedSliders.size(); ++index) {
        const float top = advancedTop + 158.0F + static_cast<float>(index) * 70.0F;
        layout.advancedSliders[index] = D2D1::RectF(
            advancedLeft + 34.0F, top, advancedLeft + advancedWidth - 34.0F, top + 54.0F);
    }
    layout.advancedReset = D2D1::RectF(
        advancedLeft + 24.0F, advancedTop + advancedHeight - 58.0F,
        advancedLeft + 152.0F, advancedTop + advancedHeight - 20.0F);
    layout.advancedClose = D2D1::RectF(
        advancedLeft + advancedWidth - 120.0F, advancedTop + advancedHeight - 58.0F,
        advancedLeft + advancedWidth - 24.0F, advancedTop + advancedHeight - 20.0F);
    layout.status = D2D1::RectF(margin, size.height - 47.0F, size.width - margin, size.height - 16.0F);
    return layout;
}

void App::updateEditControls()
{
    if (window_ == nullptr) return;
    const auto layout = calculateLayout();
    const float scale = static_cast<float>(dpi_) / 96.0F;
    const bool visible =
        !testMode_ && !showHelp_ && !showAssets_ && !showAbout_ && !showAdvanced_;
    for (std::size_t index = 0; index < lightEdits_.size(); ++index) {
        if (lightEdits_[index] == nullptr) continue;
        const auto& rectangle = layout.lightEdits[index];
        SetWindowPos(lightEdits_[index], nullptr,
                     static_cast<int>(rectangle.left * scale),
                     static_cast<int>(rectangle.top * scale),
                     static_cast<int>((rectangle.right - rectangle.left) * scale),
                     static_cast<int>((rectangle.bottom - rectangle.top) * scale),
                     SWP_NOACTIVATE | SWP_NOZORDER |
                         (visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
    }
}

App::Target App::hitTest(const float x, const float y) const
{
    const auto layout = calculateLayout();
    if (showAdvanced_) {
        for (std::size_t index = 0; index < layout.advancedToggles.size(); ++index) {
            if (contains(layout.advancedToggles[index], x, y)) {
                return static_cast<Target>(static_cast<int>(Target::AdvancedCoherentFilter) +
                                           static_cast<int>(index));
            }
        }
        for (std::size_t index = 0; index < layout.advancedSliders.size(); ++index) {
            if (contains(layout.advancedSliders[index], x, y)) {
                return static_cast<Target>(static_cast<int>(Target::AdvancedGaussianRadius) +
                                           static_cast<int>(index));
            }
        }
        if (contains(layout.advancedReset, x, y)) return Target::AdvancedReset;
        return contains(layout.advancedClose, x, y) ? Target::AdvancedClose : Target::None;
    }
    if (showHelp_) return contains(layout.helpClose, x, y) ? Target::HelpClose : Target::None;
    if (showAbout_) {
        for (std::size_t index = 0; index < layout.aboutSections.size(); ++index) {
            if (contains(layout.aboutSections[index], x, y)) {
                return static_cast<Target>(
                    static_cast<int>(Target::AboutPurpose) + static_cast<int>(index));
            }
        }
        return contains(layout.aboutClose, x, y) ? Target::AboutClose : Target::None;
    }
    if (showAssets_) {
        if (contains(layout.assetsWarcraftBrowse, x, y)) return Target::AssetsWarcraftBrowse;
        if (contains(layout.assetsCascBrowse, x, y)) return Target::AssetsCascBrowse;
        if (contains(layout.assetsAutoDetect, x, y)) return Target::AssetsAutoDetect;
        if (contains(layout.assetsClose, x, y)) return Target::AssetsClose;
        return Target::None;
    }
    if (contains(layout.help, x, y)) return Target::Help;
    if (contains(layout.about, x, y)) return Target::About;
    if (contains(layout.logs, x, y)) return Target::Logs;
    if (contains(layout.assets, x, y)) return Target::Assets;
    if (contains(layout.testMode, x, y)) return Target::TestMode;
    if (contains(layout.browse, x, y)) return Target::Browse;
    if (testMode_) {
        for (std::size_t index = 0; index < layout.patterns.size(); ++index) {
            if (contains(layout.patterns[index], x, y)) {
                return static_cast<Target>(static_cast<int>(Target::PatternBlack) + static_cast<int>(index));
            }
        }
    } else {
        for (std::size_t index = 0; index < layout.calculationOptions.size(); ++index) {
            if (contains(layout.calculationOptions[index], x, y)) {
                return static_cast<Target>(static_cast<int>(Target::Terrain) + static_cast<int>(index));
            }
        }
        for (std::size_t index = 0; index < layout.qualityOptions.size(); ++index) {
            if (contains(layout.qualityOptions[index], x, y)) {
                return static_cast<Target>(
                    static_cast<int>(Target::ShadowSamples1) + static_cast<int>(index));
            }
        }
        if (contains(layout.advanced, x, y)) return Target::Advanced;
    }
    if (contains(layout.copyMode, x, y)) return Target::CopyMode;
    if (contains(layout.inPlaceMode, x, y)) return Target::InPlaceMode;
    if (contains(layout.exportShd, x, y)) return Target::ExportShd;
    if (contains(layout.exportPng, x, y)) return Target::ExportPng;
    if (contains(layout.buildMap, x, y)) return Target::BuildMap;
    if (contains(layout.saveMap, x, y)) return Target::SaveMap;
    return Target::None;
}

D2D1_POINT_2F App::mousePoint(const LPARAM lParam) const
{
    const float scale = static_cast<float>(dpi_) / 96.0F;
    return D2D1::Point2F(static_cast<float>(GET_X_LPARAM(lParam)) / scale,
                         static_cast<float>(GET_Y_LPARAM(lParam)) / scale);
}

bool App::isSlider(const Target target) const
{
    return target >= Target::AdvancedGaussianRadius && target <= Target::AdvancedThreads;
}

void App::updateSlider(const Target target, const float x)
{
    if (!isSlider(target)) return;
    const auto layout = calculateLayout();
    const auto index = static_cast<std::size_t>(
        static_cast<int>(target) - static_cast<int>(Target::AdvancedGaussianRadius));
    const auto& rectangle = layout.advancedSliders[index];
    const auto width = std::max(1.0F, rectangle.right - rectangle.left);
    const auto normalized = std::clamp((x - rectangle.left) / width, 0.0F, 1.0F);
    switch (target) {
    case Target::AdvancedGaussianRadius:
        gaussianRadius_ = static_cast<std::uint32_t>(std::lround(normalized * 3.0F));
        break;
    case Target::AdvancedCoverageThreshold:
        coverageThreshold_ = static_cast<float>(
            std::lround((0.20F + normalized * 0.60F) * 100.0F)) / 100.0F;
        break;
    case Target::AdvancedRayBias:
        rayOriginOffset_ = static_cast<float>(std::lround(normalized * 64.0F)) * 0.5F;
        break;
    case Target::AdvancedMinimumIsland:
        minimumShadowIslandPixels_ =
            static_cast<std::uint32_t>(std::lround(normalized * 16.0F));
        break;
    case Target::AdvancedThreads:
        workerThreads_ = static_cast<std::uint32_t>(std::lround(normalized * 32.0F));
        break;
    default:
        return;
    }
    if (target != Target::AdvancedThreads && previewKind_ == PreviewKind::Calculated) {
        calculationDirty_ = true;
    }
    InvalidateRect(window_, nullptr, FALSE);
}

void App::stepSlider(const Target target, const int direction)
{
    switch (target) {
    case Target::AdvancedGaussianRadius:
        gaussianRadius_ = static_cast<std::uint32_t>(std::clamp(
            static_cast<int>(gaussianRadius_) + direction, 0, 3));
        break;
    case Target::AdvancedCoverageThreshold:
        coverageThreshold_ = std::clamp(
            coverageThreshold_ + static_cast<float>(direction) * 0.01F, 0.20F, 0.80F);
        break;
    case Target::AdvancedRayBias:
        rayOriginOffset_ = std::clamp(
            rayOriginOffset_ + static_cast<float>(direction) * 0.5F, 0.0F, 32.0F);
        break;
    case Target::AdvancedMinimumIsland:
        minimumShadowIslandPixels_ = static_cast<std::uint32_t>(std::clamp(
            static_cast<int>(minimumShadowIslandPixels_) + direction, 0, 16));
        break;
    case Target::AdvancedThreads:
        workerThreads_ = static_cast<std::uint32_t>(std::clamp(
            static_cast<int>(workerThreads_) + direction, 0, 32));
        break;
    default:
        return;
    }
    if (target != Target::AdvancedThreads && previewKind_ == PreviewKind::Calculated) {
        calculationDirty_ = true;
    }
    InvalidateRect(window_, nullptr, FALSE);
}

void App::moveFocus(const bool backwards)
{
    if (showAdvanced_) {
        constexpr std::array<Target, 10> targets{
            Target::AdvancedCoherentFilter, Target::AdvancedAlphaTerrain,
            Target::AdvancedCliffWalls, Target::AdvancedGaussianRadius,
            Target::AdvancedCoverageThreshold, Target::AdvancedRayBias,
            Target::AdvancedMinimumIsland, Target::AdvancedThreads,
            Target::AdvancedReset, Target::AdvancedClose};
        auto iterator = std::find(targets.begin(), targets.end(), focused_);
        std::size_t index = iterator == targets.end()
            ? 0U : static_cast<std::size_t>(iterator - targets.begin());
        index = backwards ? (index == 0U ? targets.size() - 1U : index - 1U)
                          : (index + 1U) % targets.size();
        focused_ = targets[index];
        InvalidateRect(window_, nullptr, FALSE);
        return;
    }
    if (showAssets_) {
        constexpr std::array<Target, 4> targets{
            Target::AssetsWarcraftBrowse, Target::AssetsCascBrowse,
            Target::AssetsAutoDetect, Target::AssetsClose};
        auto iterator = std::find(targets.begin(), targets.end(), focused_);
        std::size_t index = iterator == targets.end()
            ? 0U : static_cast<std::size_t>(iterator - targets.begin());
        index = backwards ? (index == 0U ? targets.size() - 1U : index - 1U)
                          : (index + 1U) % targets.size();
        focused_ = targets[index];
        InvalidateRect(window_, nullptr, FALSE);
        return;
    }
    if (showHelp_) {
        focused_ = Target::HelpClose;
        InvalidateRect(window_, nullptr, FALSE);
        return;
    }
    if (showAbout_) {
        constexpr std::array<Target, 5> targets{
            Target::AboutPurpose, Target::AboutOrigin, Target::AboutCompatibility,
            Target::AboutCredits, Target::AboutClose};
        auto iterator = std::find(targets.begin(), targets.end(), focused_);
        std::size_t index = iterator == targets.end()
            ? 0U : static_cast<std::size_t>(iterator - targets.begin());
        index = backwards ? (index == 0U ? targets.size() - 1U : index - 1U)
                          : (index + 1U) % targets.size();
        focused_ = targets[index];
        InvalidateRect(window_, nullptr, FALSE);
        return;
    }
    const auto available = [this](const Target target) {
        if (target >= Target::Terrain && target <= Target::ShadowSamples4) return !testMode_;
        if (target == Target::Advanced) return !testMode_;
        if (target >= Target::PatternBlack && target <= Target::PatternQuadrants) return testMode_;
        return target >= Target::Browse && target <= Target::Help;
    };
    constexpr int first = static_cast<int>(Target::Browse);
    constexpr int last = static_cast<int>(Target::Help);
    int value = static_cast<int>(focused_);
    if (value < first || value > last) value = first;
    do {
        value = backwards ? (value == first ? last : value - 1)
                          : (value == last ? first : value + 1);
    } while (!available(static_cast<Target>(value)));
    focused_ = static_cast<Target>(value);
    InvalidateRect(window_, nullptr, FALSE);
}

void App::activate(const Target target)
{
    switch (target) {
    case Target::Browse: chooseMap(); break;
    case Target::Terrain:
        includeTerrain_ = !includeTerrain_;
        calculationDirty_ = previewKind_ == PreviewKind::Calculated;
        break;
    case Target::Doodads:
        includeDoodads_ = !includeDoodads_;
        calculationDirty_ = previewKind_ == PreviewKind::Calculated;
        break;
    case Target::Destructibles:
        includeDestructibles_ = !includeDestructibles_;
        calculationDirty_ = previewKind_ == PreviewKind::Calculated;
        break;
    case Target::IgnoreRegions:
        honorIgnoreRegions_ = !honorIgnoreRegions_;
        calculationDirty_ = previewKind_ == PreviewKind::Calculated;
        break;
    case Target::TerrainClassic:
        terrainGeometry_ = TerrainGeometryMode::ClassicTriangulated;
        calculationDirty_ = previewKind_ == PreviewKind::Calculated;
        break;
    case Target::TerrainSmooth:
        terrainGeometry_ = TerrainGeometryMode::SmoothSubTile;
        calculationDirty_ = previewKind_ == PreviewKind::Calculated;
        break;
    case Target::ShadowSamples1:
        shadowSampleGrid_ = 1U;
        calculationDirty_ = previewKind_ == PreviewKind::Calculated;
        break;
    case Target::ShadowSamples2:
        shadowSampleGrid_ = 2U;
        calculationDirty_ = previewKind_ == PreviewKind::Calculated;
        break;
    case Target::ShadowSamples4:
        shadowSampleGrid_ = 4U;
        calculationDirty_ = previewKind_ == PreviewKind::Calculated;
        break;
    case Target::Advanced:
        showAssets_ = false;
        showAbout_ = false;
        showHelp_ = false;
        showAdvanced_ = true;
        focused_ = Target::AdvancedCoherentFilter;
        updateEditControls();
        break;
    case Target::PatternBlack: selectPattern(Pattern::Black); break;
    case Target::PatternWhite: selectPattern(Pattern::White); break;
    case Target::PatternChecker: selectPattern(Pattern::Checker); break;
    case Target::PatternXGradient: selectPattern(Pattern::XGradient); break;
    case Target::PatternYGradient: selectPattern(Pattern::YGradient); break;
    case Target::PatternQuadrants: selectPattern(Pattern::Quadrants); break;
    case Target::CopyMode:
        outputMode_ = OutputMode::Copy;
        setStatus(L"Safe copy mode selected. The source map will not be changed.", StatusKind::Neutral);
        break;
    case Target::InPlaceMode:
        outputMode_ = OutputMode::InPlace;
        setStatus(L"In-place mode selected. A backup will be retained.", StatusKind::Warning);
        break;
    case Target::ExportShd: exportShadow(); break;
    case Target::ExportPng: exportPng(); break;
    case Target::BuildMap: calculateShadows(); break;
    case Target::SaveMap: saveShadowMap(); break;
    case Target::TestMode:
        testMode_ = !testMode_;
        rebuildPreview();
        updateEditControls();
        focused_ = Target::TestMode;
        setStatus(testMode_
                      ? L"Test mode enabled. Diagnostic patterns are active."
                      : L"Test mode disabled. Full-map shadow preview restored.",
                  StatusKind::Neutral);
        break;
    case Target::Assets:
        showAdvanced_ = false;
        showHelp_ = false;
        showAbout_ = false;
        showAssets_ = true;
        focused_ = Target::AssetsWarcraftBrowse;
        updateEditControls();
        break;
    case Target::Logs:
        openLogsFolder();
        break;
    case Target::About:
        showAdvanced_ = false;
        showAssets_ = false;
        showHelp_ = false;
        showAbout_ = true;
        focused_ = Target::AboutPurpose;
        updateEditControls();
        break;
    case Target::Help:
        showAdvanced_ = false;
        showAssets_ = false;
        showAbout_ = false;
        showHelp_ = true;
        focused_ = Target::HelpClose;
        updateEditControls();
        break;
    case Target::AboutPurpose:
    case Target::AboutOrigin:
    case Target::AboutCompatibility:
    case Target::AboutCredits: {
        const int selected = static_cast<int>(target) - static_cast<int>(Target::AboutPurpose);
        aboutSection_ = aboutSection_ == selected ? -1 : selected;
        break;
    }
    case Target::AboutClose:
        showAbout_ = false;
        focused_ = Target::About;
        updateEditControls();
        break;
    case Target::AssetsWarcraftBrowse: chooseWarcraftDirectory(); break;
    case Target::AssetsCascBrowse: chooseCascLibrary(); break;
    case Target::AssetsAutoDetect: autoDetectAssetSettings(true); break;
    case Target::AssetsClose:
        showAssets_ = false;
        focused_ = Target::Assets;
        updateEditControls();
        break;
    case Target::HelpClose:
        showHelp_ = false;
        focused_ = Target::Help;
        updateEditControls();
        break;
    case Target::AdvancedCoherentFilter:
        coverageMode_ = coverageMode_ == ShadowCoverageMode::CoherentFilter
            ? ShadowCoverageMode::ClassicMajority
            : ShadowCoverageMode::CoherentFilter;
        calculationDirty_ = previewKind_ == PreviewKind::Calculated;
        break;
    case Target::AdvancedAlphaTerrain:
        ignoreTransparentTerrain_ = !ignoreTransparentTerrain_;
        calculationDirty_ = previewKind_ == PreviewKind::Calculated;
        break;
    case Target::AdvancedCliffWalls:
        cliffWalls_ = !cliffWalls_;
        calculationDirty_ = previewKind_ == PreviewKind::Calculated;
        break;
    case Target::AdvancedGaussianRadius:
    case Target::AdvancedCoverageThreshold:
    case Target::AdvancedRayBias:
    case Target::AdvancedMinimumIsland:
    case Target::AdvancedThreads:
        break;
    case Target::AdvancedReset:
        coverageMode_ = ShadowCoverageMode::CoherentFilter;
        gaussianRadius_ = 1U;
        coverageThreshold_ = 0.45F;
        rayOriginOffset_ = 2.0F;
        minimumShadowIslandPixels_ = 4U;
        workerThreads_ = 0U;
        ignoreTransparentTerrain_ = true;
        cliffWalls_ = false;
        calculationDirty_ = previewKind_ == PreviewKind::Calculated;
        setStatus(L"Advanced shadow settings restored to recommended defaults.",
                  StatusKind::Neutral);
        break;
    case Target::AdvancedClose:
        showAdvanced_ = false;
        focused_ = Target::Advanced;
        updateEditControls();
        break;
    case Target::None:
    case Target::Count:
        break;
    }
    InvalidateRect(window_, nullptr, FALSE);
}

void App::chooseMap()
{
    try {
        const auto selected = openMapDialog(window_);
        if (selected) loadMap(*selected);
    } catch (const std::exception& error) {
        showError(L"Open map", error);
    }
}

void App::loadAssetSettings(const bool persistDetected)
{
    warcraftDirectory_ =
        readRegistryPath(HKEY_CURRENT_USER, SettingsRegistryKey, L"WarcraftDirectory");
    cascLibrary_ = readRegistryPath(HKEY_CURRENT_USER, SettingsRegistryKey, L"CascLibPath");
    if (!hasUsableAssetSettings(warcraftDirectory_, cascLibrary_)) {
        const auto storedWarcraft = warcraftDirectory_;
        const auto storedCasc = cascLibrary_;
        autoDetectAssetSettings(false);
        if (storedWarcraft && isWarcraftDirectory(*storedWarcraft)) {
            warcraftDirectory_ = storedWarcraft;
        }
        if (storedCasc && isCascLibrary(*storedCasc)) cascLibrary_ = storedCasc;
        if (persistDetected) saveAssetSettings();
    }
    if (!hasUsableAssetSettings(warcraftDirectory_, cascLibrary_)) {
        showAssets_ = true;
        focused_ = Target::AssetsWarcraftBrowse;
        updateEditControls();
        setStatus(L"Set a Warcraft III CASC or classic MPQ location for object shadows.",
                  StatusKind::Warning);
    } else if (warcraftDirectory_) {
        logEvent(StatusKind::Success,
                 (isLegacyWarcraftDirectory(*warcraftDirectory_)
                      ? L"Using classic MPQ assets: " : L"Using CASC assets: ") +
                     warcraftDirectory_->wstring());
    }
}

void App::saveAssetSettings() const
{
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, SettingsRegistryKey, 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return;
    }
    const auto write = [&](const wchar_t* name,
                           const std::optional<std::filesystem::path>& value) {
        if (!value) {
            RegDeleteValueW(key, name);
            return;
        }
        const auto text = value->wstring();
        RegSetValueExW(
            key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(text.c_str()),
            static_cast<DWORD>((text.size() + 1U) * sizeof(wchar_t)));
    };
    write(L"WarcraftDirectory", warcraftDirectory_);
    write(L"CascLibPath", cascLibrary_);
    RegCloseKey(key);
}

void App::autoDetectAssetSettings(const bool notify)
{
    warcraftDirectory_.reset();
    cascLibrary_.reset();

    const std::array<std::filesystem::path, 2> standardDirectories{
        L"C:\\Program Files (x86)\\Warcraft III", L"C:\\Program Files\\Warcraft III"};
    for (const auto& directory : standardDirectories) {
        if (isWarcraftDirectory(directory)) {
            warcraftDirectory_ = directory;
            break;
        }
    }
    if (!warcraftDirectory_) {
        constexpr std::array<const wchar_t*, 2> registryKeys{
            L"SOFTWARE\\WOW6432Node\\Blizzard Entertainment\\Warcraft III",
            L"SOFTWARE\\Blizzard Entertainment\\Warcraft III"};
        for (const auto* key : registryKeys) {
            auto candidate = readRegistryPath(HKEY_LOCAL_MACHINE, key, L"InstallPath");
            if (candidate && std::filesystem::is_regular_file(*candidate)) {
                candidate = candidate->parent_path();
            }
            if (candidate && isWarcraftDirectory(*candidate)) {
                warcraftDirectory_ = std::move(candidate);
                break;
            }
        }
    }

    if (const auto directory = executableDirectory()) {
        const auto candidate = *directory / L"CascLib.dll";
        if (isCascLibrary(candidate)) cascLibrary_ = candidate;
    }
    saveAssetSettings();
    if (notify) {
        if (warcraftDirectory_ && isLegacyWarcraftDirectory(*warcraftDirectory_)) {
            setStatus(L"Classic Warcraft III MPQ installation detected and saved.",
                      StatusKind::Success);
        } else if (hasUsableAssetSettings(warcraftDirectory_, cascLibrary_)) {
            setStatus(L"Warcraft III CASC and CascLib locations detected and saved.",
                      StatusKind::Success);
        } else {
            setStatus(L"Auto-detection was incomplete. Choose the missing asset locations.",
                      StatusKind::Warning);
        }
    }
    InvalidateRect(window_, nullptr, FALSE);
}

void App::chooseWarcraftDirectory()
{
    try {
        const auto selected = chooseFolderDialog(window_, L"Choose the Warcraft III folder");
        if (!selected) return;
        if (!isWarcraftDirectory(*selected)) {
            throw std::runtime_error(
                "the selected folder contains neither CASC .build.info nor classic War3.mpq");
        }
        warcraftDirectory_ = *selected;
        saveAssetSettings();
        setStatus(isLegacyWarcraftDirectory(*selected)
                      ? L"Classic Warcraft III MPQ location saved; CascLib is not required."
                      : L"Warcraft III CASC location saved.",
                  StatusKind::Success);
    } catch (const std::exception& error) {
        showError(L"Set Warcraft III location", error);
    }
}

void App::chooseCascLibrary()
{
    try {
        const auto selected = openLibraryDialog(window_);
        if (!selected) return;
        if (!isCascLibrary(*selected)) {
            throw std::runtime_error("the selected CascLib.dll does not exist");
        }
        cascLibrary_ = *selected;
        saveAssetSettings();
        setStatus(L"CascLib location saved.", StatusKind::Success);
    } catch (const std::exception& error) {
        showError(L"Set CascLib location", error);
    }
}

void App::loadMap(const std::filesystem::path& path)
{
    try {
        MapArchive archive(path);
        const auto parsed = parseW3E(archive.read("war3map.w3e"));
        if (!parsed) throw std::runtime_error(parsed.error);

        mapPath_ = path;
        mapInfo_ = parsed.info;
        rebuildPreview();
        logEvent(StatusKind::Neutral, L"Loaded map: " + path.wstring());

        const auto expected = static_cast<std::uint64_t>(parsed.info.tileWidth) *
                              parsed.info.tileHeight * 16U;
        std::wostringstream message;
        message << L"Ready: " << parsed.info.tileWidth << L" × " << parsed.info.tileHeight
                << L" tiles · " << formatBytes(expected) << L" SHD";
        if (archive.contains("war3map.shd")) message << L" · existing shadow found";
        setStatus(message.str(), StatusKind::Success);
    } catch (const std::exception& error) {
        showError(L"Load map", error);
    }
}

void App::selectPattern(const Pattern pattern)
{
    if (!testMode_) return;
    pattern_ = pattern;
    try {
        rebuildPreview();
        setStatus(patternLabel(pattern) + L" pattern selected.", StatusKind::Neutral);
    } catch (const std::exception& error) {
        showError(L"Generate preview", error);
    }
}

void App::rebuildPreview()
{
    const std::uint32_t width = mapInfo_ ? mapInfo_->tileWidth : 32U;
    const std::uint32_t height = mapInfo_ ? mapInfo_->tileHeight : 32U;
    shadowMap_.emplace(width, height);
    if (testMode_) {
        generatePattern(*shadowMap_, pattern_);
        previewKind_ = PreviewKind::Test;
    } else if (mapPath_ && mapInfo_) {
        const MapArchive archive(*mapPath_);
        if (archive.contains("war3map.shd")) {
            shadowMap_->loadWarcraftBytes(archive.read("war3map.shd"));
            previewKind_ = PreviewKind::Existing;
        } else {
            previewKind_ = PreviewKind::Empty;
        }
    } else {
        previewKind_ = PreviewKind::Empty;
    }
    calculationDirty_ = false;
    refreshPreviewBitmap();
}

void App::refreshPreviewBitmap()
{
    previewBitmap_.Reset();
    if (!renderTarget_ || !shadowMap_) return;

    constexpr std::uint32_t MaxPreviewDimension = 1024U;
    const auto previewWidth = std::min(shadowMap_->widthPixels(), MaxPreviewDimension);
    const auto previewHeight = std::min(shadowMap_->heightPixels(), MaxPreviewDimension);
    std::vector<std::uint32_t> pixels(
        static_cast<std::size_t>(previewWidth) * previewHeight);
    for (std::uint32_t y = 0; y < previewHeight; ++y) {
        const auto sourceY = static_cast<std::uint32_t>(
            static_cast<std::uint64_t>(y) * shadowMap_->heightPixels() / previewHeight);
        for (std::uint32_t x = 0; x < previewWidth; ++x) {
            const auto sourceX = static_cast<std::uint32_t>(
                static_cast<std::uint64_t>(x) * shadowMap_->widthPixels() / previewWidth);
            const auto shadow = shadowMap_->value(sourceX, sourceY);
            const auto grayscale = static_cast<std::uint32_t>(255U - shadow);
            pixels[static_cast<std::size_t>(y) * previewWidth + x] =
                0xFF000000U | grayscale | (grayscale << 8U) | (grayscale << 16U);
        }
    }

    const auto properties = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
    const HRESULT result = renderTarget_->CreateBitmap(
        D2D1::SizeU(previewWidth, previewHeight), pixels.data(), previewWidth * 4U,
        properties, &previewBitmap_);
    if (FAILED(result)) throw std::runtime_error("Direct2D could not create the preview bitmap");
    InvalidateRect(window_, nullptr, FALSE);
}

void App::exportShadow()
{
    if (!mapPath_ || !shadowMap_) {
        setStatus(L"Choose a map before exporting SHD data.", StatusKind::Warning);
        return;
    }
    try {
        const auto suggested = mapPath_->parent_path() /
            (mapPath_->stem().wstring() + L"." +
             (previewKind_ == PreviewKind::Test ? patternFilePart(pattern_) : L"shadowmap") +
             L".shd");
        const auto output = saveFileDialog(window_, L"Export shadow map", suggested,
                                           L"Warcraft III shadow map", L"*.shd", L"shd");
        if (!output) return;
        const auto warcraftBytes = shadowMap_->warcraftBytes();
        writeBinaryFileAtomic(*output, warcraftBytes, true);
        setStatus(L"Exported SHD: " + output->filename().wstring(), StatusKind::Success);
    } catch (const std::exception& error) {
        showError(L"Export SHD", error);
    }
}

void App::exportPng()
{
    if (!mapPath_ || !shadowMap_) {
        setStatus(L"Choose a map before exporting a preview.", StatusKind::Warning);
        return;
    }
    try {
        const auto suggested = mapPath_->parent_path() /
            (mapPath_->stem().wstring() + L"." +
             (previewKind_ == PreviewKind::Test ? patternFilePart(pattern_) : L"shadowmap") +
             L".png");
        const auto output = saveFileDialog(window_, L"Export PNG preview", suggested,
                                           L"PNG image", L"*.png", L"png");
        if (!output) return;
        const auto png = makeShadowPreviewPng(
            shadowMap_->widthPixels(), shadowMap_->heightPixels(), shadowMap_->bytes());
        writeBinaryFileAtomic(*output, png, true);
        setStatus(L"Exported preview: " + output->filename().wstring(), StatusKind::Success);
    } catch (const std::exception& error) {
        showError(L"Export PNG", error);
    }
}

Vec3 App::readLightDirection() const
{
    Vec3 direction;
    float* values[] = {&direction.x, &direction.y, &direction.z};
    for (std::size_t index = 0; index < lightEdits_.size(); ++index) {
        wchar_t text[64]{};
        GetWindowTextW(lightEdits_[index], text, static_cast<int>(std::size(text)));
        wchar_t* end = nullptr;
        const float value = std::wcstof(text, &end);
        while (end != nullptr && std::iswspace(*end)) ++end;
        if (end == text || (end != nullptr && *end != L'\0') || !std::isfinite(value)) {
            throw std::invalid_argument("light vector values must be finite numbers");
        }
        *values[index] = value;
    }
    return direction;
}

void App::calculateShadows()
{
    if (!mapPath_ || !mapInfo_ || !shadowMap_) {
        setStatus(L"Choose a map before calculating its shadowmap.", StatusKind::Warning);
        return;
    }
    try {
        setStatus(L"Calculating terrain and object shadows…", StatusKind::Neutral);
        UpdateWindow(window_);
        const MapArchive source(*mapPath_);
        CompositeAssetProvider assets;
        assets.add(std::make_shared<MapAssetProvider>(source));
        std::shared_ptr<CascAssetProvider> casc;
        std::shared_ptr<LegacyMpqAssetProvider> legacy;
        const bool objectAssetsRequired = includeDoodads_ || includeDestructibles_;
        const bool assetSettingsUsable =
            hasUsableAssetSettings(warcraftDirectory_, cascLibrary_);
        if (objectAssetsRequired && !assetSettingsUsable) {
            showAssets_ = true;
            focused_ = Target::AssetsWarcraftBrowse;
            updateEditControls();
            throw std::runtime_error(
                "set a valid Warcraft III CASC or classic MPQ location in Assets");
        }
        if (assetSettingsUsable) {
            if (isLegacyWarcraftDirectory(*warcraftDirectory_)) {
                legacy = std::make_shared<LegacyMpqAssetProvider>(*warcraftDirectory_);
                if (!legacy->available()) {
                    throw std::runtime_error(legacy->error() + "; update the path in Assets");
                }
                assets.add(legacy);
                logEvent(StatusKind::Neutral,
                         L"Calculation asset source: classic MPQ installation.");
            } else {
                casc = std::make_shared<CascAssetProvider>(*warcraftDirectory_, *cascLibrary_);
                if (!casc->available()) {
                    throw std::runtime_error(casc->error() + "; update the paths in Assets");
                }
                assets.add(casc);
                logEvent(StatusKind::Neutral, L"Calculation asset source: CASC installation.");
            }
        }
        GenerationOptions options;
        options.lightDirection = readLightDirection();
        options.threadCount = workerThreads_;
        options.shadowSampleGrid = shadowSampleGrid_;
        options.rayOriginOffset = rayOriginOffset_;
        options.gaussianRadius = gaussianRadius_;
        options.coverageThreshold = coverageThreshold_;
        options.minimumShadowIslandPixels = minimumShadowIslandPixels_;
        options.terrainGeometry = terrainGeometry_;
        options.coverageMode = coverageMode_;
        options.terrain = includeTerrain_;
        options.cliffWalls = cliffWalls_;
        options.doodads = includeDoodads_;
        options.destructibles = includeDestructibles_;
        options.honorIgnoreShadowRegions = honorIgnoreRegions_;
        options.ignoreTransparentTerrain = ignoreTransparentTerrain_;
        auto generated = generateShadowMap(source, assets, options);
        for (const auto& warning : generated.warnings) {
            logEvent(StatusKind::Warning, widen(warning));
        }
        std::wostringstream calculationLog;
        calculationLog << L"Calculation completed: map=" << mapPath_->wstring()
                       << L", terrain=" << (includeTerrain_ ? L"on" : L"off")
                       << L", doodads=" << (includeDoodads_ ? L"on" : L"off")
                       << L", destructibles=" << (includeDestructibles_ ? L"on" : L"off")
                       << L", geometry="
                       << (terrainGeometry_ == TerrainGeometryMode::SmoothSubTile
                               ? L"smooth-sub-tile" : L"classic-triangles")
                       << L", samples=" << generated.stats.shadowSampleGrid << L"x"
                       << L", filter="
                       << (coverageMode_ == ShadowCoverageMode::CoherentFilter
                               ? L"coherent" : L"hard-majority")
                       << L", gaussian-radius=" << gaussianRadius_
                       << L", coverage-threshold=" << std::fixed << std::setprecision(2)
                       << coverageThreshold_
                       << L", ray-bias=" << rayOriginOffset_
                       << L", min-island=" << minimumShadowIslandPixels_
                       << L", alpha-mask=" << (ignoreTransparentTerrain_ ? L"on" : L"off")
                       << L", cliff-walls=" << (cliffWalls_ ? L"on" : L"off")
                       << L", threads=" << (workerThreads_ == 0U ? L"auto" : std::to_wstring(workerThreads_))
                       << L", placements=" << generated.stats.placements
                       << L", resolved=" << generated.stats.resolvedPlacements
                       << L", unresolved=" << generated.stats.unresolvedPlacements
                       << L", models=" << generated.stats.uniqueModels
                       << L", triangles=" << generated.stats.triangles
                       << L", cliff-wall-triangles=" << generated.stats.cliffWallTriangles
                       << L", transparent-terrain-types=" << generated.stats.transparentTerrainTypes
                       << L", partial-coverage-pixels=" << generated.stats.partialCoveragePixels
                       << L", removed-small-island-pixels="
                       << generated.stats.removedSmallIslandPixels
                       << L", rays=" << generated.stats.rays
                       << L", seconds=" << std::fixed << std::setprecision(3)
                       << (generated.stats.loadSeconds + generated.stats.bvhSeconds +
                           generated.stats.raySeconds);
        logEvent(StatusKind::Success, calculationLog.str());
        shadowMap_ = std::move(generated.shadow);
        previewKind_ = PreviewKind::Calculated;
        calculationDirty_ = false;
        refreshPreviewBitmap();
        InvalidateRect(window_, nullptr, FALSE);
        std::wostringstream message;
        message << L"Preview rendered · " << generated.stats.shadowSampleGrid << L"x edge quality · "
                << generated.stats.triangles << L" triangles · "
                << generated.warnings.size() << L" warnings · inspect it, then Save to map";
        setStatus(message.str(), generated.warnings.empty() ? StatusKind::Success : StatusKind::Warning);
    } catch (const std::invalid_argument& error) {
        showError(L"Calculate shadows", error);
    } catch (const std::exception& error) {
        showError(L"Calculate shadows", error);
    }
}

void App::saveShadowMap()
{
    if (!mapPath_ || !shadowMap_ || previewKind_ != PreviewKind::Calculated ||
        calculationDirty_) {
        setStatus(L"Calculate shadows first, inspect the rendered preview, then save it.",
                  StatusKind::Warning);
        return;
    }
    try {
        const auto warcraftBytes = shadowMap_->warcraftBytes();
        if (outputMode_ == OutputMode::InPlace) {
            const int answer = MessageBoxW(
                window_,
                L"Replace war3map.shd in the selected map?\n\nThe original map will be retained as a numbered .w3shadow.bak file.",
                L"Confirm in-place replacement", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
            if (answer != IDYES) {
                setStatus(L"In-place replacement cancelled.", StatusKind::Neutral);
                return;
            }
            const auto backup = MapArchive::replaceShadowInPlace(*mapPath_, warcraftBytes);
            setStatus(L"Map updated · backup " + backup.filename().wstring(), StatusKind::Success);
            return;
        }

        const auto extension = mapPath_->extension().wstring();
        const auto suggested = mapPath_->parent_path() /
            (mapPath_->stem().wstring() + L".shadowed" + extension);
        const wchar_t* pattern = extension == L".w3m" ? L"*.w3m" : L"*.w3x";
        const wchar_t* defaultExtension = extension == L".w3m" ? L"w3m" : L"w3x";
        const auto output = saveFileDialog(window_, L"Save calculated shadow map", suggested,
                                           L"Warcraft III map", pattern, defaultExtension);
        if (!output) {
            setStatus(L"Map save cancelled; the rendered preview is still available.",
                      StatusKind::Neutral);
            return;
        }
        MapArchive::replaceShadowInCopy(*mapPath_, *output, warcraftBytes, true);
        setStatus(L"Created shadowed map: " + output->filename().wstring(), StatusKind::Success);
    } catch (const std::exception& error) {
        showError(L"Save shadowmap", error);
    }
}

void App::initializeSessionLog()
{
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
    localtime_s(&local, &time);
    std::ostringstream name;
    name << "shadowmaptool-" << std::put_time(&local, "%Y%m%d-%H%M%S")
         << '-' << GetCurrentProcessId() << ".log";

    std::vector<std::filesystem::path> candidates;
    if (const auto executable = executableDirectory()) {
        candidates.push_back(*executable / L"logs");
    }
    std::array<wchar_t, 32768> localAppData{};
    const auto length = GetEnvironmentVariableW(
        L"LOCALAPPDATA", localAppData.data(), static_cast<DWORD>(localAppData.size()));
    if (length > 0U && length < localAppData.size()) {
        candidates.emplace_back(
            std::filesystem::path(localAppData.data()) / L"ShadowMapTool" / L"logs");
    }

    for (const auto& directory : candidates) {
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        if (error) continue;
        const auto path = directory / name.str();
        std::ofstream stream(path, std::ios::out | std::ios::app);
        if (!stream) continue;
        logsDirectory_ = directory;
        sessionLogPath_ = path;
        sessionLog_ = std::move(stream);
        break;
    }
}

void App::logEvent(const StatusKind kind, const std::wstring_view message)
{
    if (!sessionLog_) return;
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
    localtime_s(&local, &time);
    const char* level = "INFO";
    if (kind == StatusKind::Success) level = "SUCCESS";
    if (kind == StatusKind::Warning) level = "WARNING";
    if (kind == StatusKind::Error) level = "ERROR";
    sessionLog_ << std::put_time(&local, "%Y-%m-%d %H:%M:%S")
                << " [" << level << "] " << narrow(message) << '\n';
    sessionLog_.flush();
}

void App::openLogsFolder()
{
    if (logsDirectory_.empty() || !std::filesystem::is_directory(logsDirectory_)) {
        const std::runtime_error error("the session log directory could not be created");
        showError(L"Open logs", error);
        return;
    }
    const auto result = reinterpret_cast<INT_PTR>(
        ShellExecuteW(window_, L"open", logsDirectory_.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (result <= 32) {
        const std::runtime_error error("Windows could not open the logs directory");
        showError(L"Open logs", error);
        return;
    }
    setStatus(L"Opened logs folder: " + logsDirectory_.wstring(), StatusKind::Success);
}

void App::setStatus(std::wstring message, const StatusKind kind)
{
    logEvent(kind, message);
    status_ = std::move(message);
    statusKind_ = kind;
    InvalidateRect(window_, nullptr, FALSE);
}

void App::showError(const std::wstring& action, const std::exception& error)
{
    const auto details = widen(error.what());
    setStatus(action + L" failed: " + details, StatusKind::Error);
    MessageBoxW(window_, details.c_str(), (action + L" failed").c_str(), MB_OK | MB_ICONERROR);
}

void App::drawText(
    const std::wstring_view text,
    const D2D1_RECT_F& rectangle,
    IDWriteTextFormat* format,
    ID2D1Brush* brush)
{
    renderTarget_->DrawTextW(text.data(), static_cast<UINT32>(text.size()), format,
                             rectangle, brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void App::drawCard(const D2D1_RECT_F& rectangle)
{
    const auto rounded = D2D1::RoundedRect(rectangle, 14.0F, 14.0F);
    renderTarget_->FillRoundedRectangle(rounded, panelBrush_.Get());
    renderTarget_->DrawRoundedRectangle(rounded, borderBrush_.Get(), 1.0F);
}

void App::drawButton(
    const D2D1_RECT_F& rectangle,
    const std::wstring_view label,
    const Target target,
    const bool selected,
    const bool primary)
{
    const auto rounded = D2D1::RoundedRect(rectangle, 9.0F, 9.0F);
    if (primary) {
        accentBrush_->SetOpacity(1.0F);
        renderTarget_->FillRoundedRectangle(rounded, accentBrush_.Get());
    } else {
        renderTarget_->FillRoundedRectangle(rounded, surfaceBrush_.Get());
        if (selected || hovered_ == target) {
            accentBrush_->SetOpacity(selected ? 0.28F : 0.15F);
            renderTarget_->FillRoundedRectangle(rounded, accentBrush_.Get());
            accentBrush_->SetOpacity(1.0F);
        }
        renderTarget_->DrawRoundedRectangle(
            rounded, selected ? accentBrush_.Get() : borderBrush_.Get(), selected ? 1.5F : 1.0F);
    }
    if (focused_ == target) {
        const auto focus = D2D1::RoundedRect(
            D2D1::RectF(rectangle.left - 2.0F, rectangle.top - 2.0F,
                        rectangle.right + 2.0F, rectangle.bottom + 2.0F),
            11.0F, 11.0F);
        renderTarget_->DrawRoundedRectangle(focus, accentBrush_.Get(), 1.0F);
    }
    drawText(label, rectangle, buttonFormat_.Get(), textBrush_.Get());
}

void App::drawSlider(
    const D2D1_RECT_F& rectangle,
    const std::wstring_view label,
    const std::wstring_view value,
    const Target target,
    const float normalizedValue)
{
    const auto normalized = std::clamp(normalizedValue, 0.0F, 1.0F);
    drawText(label, D2D1::RectF(rectangle.left, rectangle.top,
                                rectangle.right - 120.0F, rectangle.top + 24.0F),
             bodyFormat_.Get(), textBrush_.Get());
    drawText(value, D2D1::RectF(rectangle.right - 112.0F, rectangle.top,
                                rectangle.right, rectangle.top + 24.0F),
             buttonFormat_.Get(), accentBrush_.Get());
    const float trackTop = rectangle.bottom - 13.0F;
    const auto track = D2D1::RoundedRect(
        D2D1::RectF(rectangle.left, trackTop, rectangle.right, trackTop + 5.0F), 2.5F, 2.5F);
    renderTarget_->FillRoundedRectangle(track, borderBrush_.Get());
    const float thumbX = rectangle.left +
        (rectangle.right - rectangle.left) * normalized;
    if (thumbX > rectangle.left) {
        renderTarget_->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(rectangle.left, trackTop, thumbX, trackTop + 5.0F),
                              2.5F, 2.5F),
            accentBrush_.Get());
    }
    renderTarget_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(thumbX, trackTop + 2.5F),
                                              7.0F, 7.0F),
                               accentBrush_.Get());
    if (focused_ == target || hovered_ == target) {
        renderTarget_->DrawRoundedRectangle(D2D1::RoundedRect(rectangle, 7.0F, 7.0F),
                                             accentBrush_.Get(), 1.0F);
    }
}

void App::paint()
{
    if (!createDeviceResources()) return;
    const auto layout = calculateLayout();
    const auto size = renderTarget_->GetSize();

    renderTarget_->BeginDraw();
    renderTarget_->Clear(rgb(0x0B1020));

    const auto logoRect = D2D1::RectF(28.0F, 24.0F, 68.0F, 64.0F);
    if (appIconBitmap_) {
        renderTarget_->DrawBitmap(appIconBitmap_.Get(), logoRect, 1.0F,
                                  D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    } else {
        const auto logo = D2D1::RoundedRect(logoRect, 11.0F, 11.0F);
        renderTarget_->FillRoundedRectangle(logo, accentBrush_.Get());
        drawText(L"S", D2D1::RectF(28.0F, 23.0F, 68.0F, 65.0F),
                 titleFormat_.Get(), textBrush_.Get());
    }
    drawText(L"ShadowMap Tool", D2D1::RectF(82.0F, 19.0F, 390.0F, 53.0F),
             titleFormat_.Get(), textBrush_.Get());
    drawText(L"Warcraft III static-shadow generation workspace",
             D2D1::RectF(83.0F, 52.0F, 520.0F, 75.0F), smallFormat_.Get(), mutedBrush_.Get());

    drawButton(layout.testMode, testMode_ ? L"Test mode ON" : L"Test mode",
               Target::TestMode, testMode_);
    drawButton(layout.assets, L"Assets", Target::Assets, false);
    drawButton(layout.logs, L"Logs", Target::Logs, false);
    drawButton(layout.about, L"About", Target::About, false);
    drawButton(layout.help, L"?  Help", Target::Help, false);

    drawCard(layout.mapCard);
    drawText(L"Map", D2D1::RectF(layout.mapCard.left + 16.0F, layout.mapCard.top + 14.0F,
                                  layout.mapCard.right, layout.mapCard.top + 38.0F),
             headingFormat_.Get(), textBrush_.Get());
    const auto mapName = mapPath_ ? mapPath_->filename().wstring() : L"No map selected";
    drawText(mapName, D2D1::RectF(layout.mapCard.left + 16.0F, layout.mapCard.top + 49.0F,
                                  layout.browse.left - 10.0F, layout.mapCard.top + 72.0F),
             bodyFormat_.Get(), mapPath_ ? textBrush_.Get() : mutedBrush_.Get());
    std::wstring mapDetails = L"Drop a .w3x or .w3m here";
    if (mapInfo_) {
        mapDetails = std::to_wstring(mapInfo_->tileWidth) + L" × " +
                     std::to_wstring(mapInfo_->tileHeight) + L" tiles  ·  " +
                     std::to_wstring(mapInfo_->tileWidth * 4U) + L" × " +
                     std::to_wstring(mapInfo_->tileHeight * 4U) + L" SHD";
    }
    drawText(mapDetails, D2D1::RectF(layout.mapCard.left + 16.0F, layout.mapCard.top + 77.0F,
                                     layout.browse.left - 8.0F, layout.mapCard.top + 101.0F),
             smallFormat_.Get(), mutedBrush_.Get());
    drawButton(layout.browse, L"Browse", Target::Browse, false);

    drawCard(layout.patternCard);
    drawText(testMode_ ? L"Diagnostic preview pattern" : L"Shadow calculation",
             D2D1::RectF(layout.patternCard.left + 16.0F, layout.patternCard.top + 14.0F,
                         layout.patternCard.right, layout.patternCard.top + 38.0F),
             headingFormat_.Get(), textBrush_.Get());
    if (testMode_) {
        const std::array<Pattern, 6> patterns{
            Pattern::Black, Pattern::White, Pattern::Checker,
            Pattern::XGradient, Pattern::YGradient, Pattern::Quadrants};
        for (std::size_t index = 0; index < patterns.size(); ++index) {
            const auto target = static_cast<Target>(static_cast<int>(Target::PatternBlack) +
                                                    static_cast<int>(index));
            drawButton(layout.patterns[index], patternLabel(patterns[index]), target,
                       pattern_ == patterns[index]);
        }
    } else {
        const std::array<std::wstring_view, 6> labels{
            L"Terrain", L"Doodads", L"Destructibles", L"IgnoreShadow rects",
            L"Classic triangles", L"Smooth sub-tile"};
        const std::array<bool, 6> selected{
            includeTerrain_, includeDoodads_, includeDestructibles_, honorIgnoreRegions_,
            terrainGeometry_ == TerrainGeometryMode::ClassicTriangulated,
            terrainGeometry_ == TerrainGeometryMode::SmoothSubTile};
        drawButton(layout.advanced, L"Tuning...", Target::Advanced, false);
        for (std::size_t index = 0; index < labels.size(); ++index) {
            const auto target = static_cast<Target>(static_cast<int>(Target::Terrain) +
                                                    static_cast<int>(index));
            drawButton(layout.calculationOptions[index], labels[index], target, selected[index]);
        }
        drawText(L"Shadow edge quality",
                 D2D1::RectF(layout.patternCard.left + 16.0F, layout.patternCard.top + 180.0F,
                             layout.patternCard.right - 16.0F, layout.patternCard.top + 201.0F),
                 smallFormat_.Get(), mutedBrush_.Get());
        constexpr std::array<std::wstring_view, 3> qualityLabels{
            L"Fast 1x", L"Smooth 2x", L"Ultra 4x"};
        constexpr std::array<std::uint32_t, 3> qualityValues{1U, 2U, 4U};
        for (std::size_t index = 0; index < qualityLabels.size(); ++index) {
            const auto target = static_cast<Target>(
                static_cast<int>(Target::ShadowSamples1) + static_cast<int>(index));
            drawButton(layout.qualityOptions[index], qualityLabels[index], target,
                       shadowSampleGrid_ == qualityValues[index]);
        }
        drawText(L"Light vector (X, Y, Z) · default 1, 1, -1",
                 D2D1::RectF(layout.patternCard.left + 16.0F, layout.patternCard.top + 250.0F,
                             layout.patternCard.right - 16.0F, layout.patternCard.top + 271.0F),
                 smallFormat_.Get(), mutedBrush_.Get());
        constexpr std::array<std::wstring_view, 3> axes{L"X", L"Y", L"Z"};
        for (std::size_t index = 0; index < axes.size(); ++index) {
            drawText(axes[index],
                     D2D1::RectF(layout.lightEdits[index].right + 4.0F,
                                 layout.lightEdits[index].top + 7.0F,
                                 layout.lightEdits[index].right + 20.0F,
                                 layout.lightEdits[index].bottom),
                     smallFormat_.Get(), mutedBrush_.Get());
        }
    }

    drawCard(layout.outputCard);
    drawText(L"Output safety",
             D2D1::RectF(layout.outputCard.left + 16.0F, layout.outputCard.top + 12.0F,
                         layout.outputCard.right, layout.outputCard.top + 36.0F),
             headingFormat_.Get(), textBrush_.Get());
    drawButton(layout.copyMode, L"Save as copy", Target::CopyMode,
               outputMode_ == OutputMode::Copy);
    drawButton(layout.inPlaceMode, L"In place + backup", Target::InPlaceMode,
               outputMode_ == OutputMode::InPlace);

    drawCard(layout.previewCard);
    drawText(L"Shadow preview",
             D2D1::RectF(layout.previewCard.left + 18.0F, layout.previewCard.top + 15.0F,
                         layout.previewCard.right, layout.previewCard.top + 40.0F),
             headingFormat_.Get(), textBrush_.Get());
    std::wstring previewMeta;
    switch (previewKind_) {
    case PreviewKind::Empty: previewMeta = mapPath_ ? L"No shadowmap in map" : L"No map selected"; break;
    case PreviewKind::Existing: previewMeta = L"Existing full shadowmap"; break;
    case PreviewKind::Calculated: previewMeta = L"Calculated full shadowmap"; break;
    case PreviewKind::Test: previewMeta = L"Test pattern: " + patternLabel(pattern_); break;
    }
    if (previewKind_ == PreviewKind::Calculated && calculationDirty_) {
        previewMeta += L" (settings changed — recalculate)";
    }
    if (shadowMap_) {
        previewMeta += L"  ·  " + std::to_wstring(shadowMap_->widthPixels()) + L" × " +
                       std::to_wstring(shadowMap_->heightPixels()) + L" pixels  ·  " +
                       formatBytes(shadowMap_->bytes().size());
    }
    drawText(previewMeta,
             D2D1::RectF(layout.previewCard.left + 18.0F, layout.previewCard.top + 40.0F,
                         layout.previewCard.right - 18.0F, layout.previewCard.top + 61.0F),
             smallFormat_.Get(), mutedBrush_.Get());

    renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(layout.previewCanvas, 10.0F, 10.0F),
                                         surfaceBrush_.Get());
    if (previewBitmap_) {
        const auto bitmapSize = previewBitmap_->GetSize();
        const float availableWidth = layout.previewCanvas.right - layout.previewCanvas.left - 20.0F;
        const float availableHeight = layout.previewCanvas.bottom - layout.previewCanvas.top - 20.0F;
        const float scale = std::min(availableWidth / bitmapSize.width,
                                     availableHeight / bitmapSize.height);
        const float width = bitmapSize.width * scale;
        const float height = bitmapSize.height * scale;
        const float left = layout.previewCanvas.left +
                           (layout.previewCanvas.right - layout.previewCanvas.left - width) / 2.0F;
        const float top = layout.previewCanvas.top +
                          (layout.previewCanvas.bottom - layout.previewCanvas.top - height) / 2.0F;
        const auto destination = D2D1::RectF(left, top, left + width, top + height);
        renderTarget_->DrawBitmap(previewBitmap_.Get(), destination, 1.0F,
                                  D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
        renderTarget_->DrawRectangle(destination, borderBrush_.Get(), 1.0F);
    }
    drawButton(layout.exportShd, L"Export SHD", Target::ExportShd, false);
    drawButton(layout.exportPng, L"Export PNG", Target::ExportPng, false);
    drawButton(layout.buildMap, L"Calculate shadows", Target::BuildMap, false, true);
    drawButton(layout.saveMap, L"Save to map", Target::SaveMap, false,
               previewKind_ == PreviewKind::Calculated && !calculationDirty_);

    ID2D1SolidColorBrush* statusBrush = mutedBrush_.Get();
    if (statusKind_ == StatusKind::Success) statusBrush = successBrush_.Get();
    if (statusKind_ == StatusKind::Warning) statusBrush = warningBrush_.Get();
    if (statusKind_ == StatusKind::Error) statusBrush = errorBrush_.Get();
    renderTarget_->FillEllipse(D2D1::Ellipse(
        D2D1::Point2F(layout.status.left + 5.0F, layout.status.top + 12.0F), 4.0F, 4.0F),
        statusBrush);
    drawText(status_, D2D1::RectF(layout.status.left + 18.0F, layout.status.top,
                                  layout.status.right, layout.status.bottom),
             smallFormat_.Get(), statusBrush);

    if (showAdvanced_) {
        backgroundBrush_->SetOpacity(0.86F);
        renderTarget_->FillRectangle(D2D1::RectF(0.0F, 0.0F, size.width, size.height),
                                     backgroundBrush_.Get());
        backgroundBrush_->SetOpacity(1.0F);

        const float advancedWidth = std::min(760.0F, size.width - 60.0F);
        const float advancedHeight = std::min(630.0F, size.height - 30.0F);
        const float advancedLeft = (size.width - advancedWidth) / 2.0F;
        const float advancedTop = (size.height - advancedHeight) / 2.0F;
        const auto advancedCard = D2D1::RectF(
            advancedLeft, advancedTop, advancedLeft + advancedWidth,
            advancedTop + advancedHeight);
        drawCard(advancedCard);
        drawText(L"Advanced shadow tuning",
                 D2D1::RectF(advancedLeft + 24.0F, advancedTop + 18.0F,
                             advancedCard.right - 24.0F, advancedTop + 54.0F),
                 titleFormat_.Get(), textBrush_.Get());
        drawText(
            L"SHD stores binary cells. These settings control sampling, spatial filtering, "
            L"and the final filled-cell decision.",
            D2D1::RectF(advancedLeft + 25.0F, advancedTop + 58.0F,
                        advancedCard.right - 25.0F, advancedTop + 88.0F),
            helpBodyFormat_.Get(), mutedBrush_.Get());

        drawButton(layout.advancedToggles[0], L"Coherent filter",
                   Target::AdvancedCoherentFilter,
                   coverageMode_ == ShadowCoverageMode::CoherentFilter);
        drawButton(layout.advancedToggles[1], L"Ignore alpha terrain",
                   Target::AdvancedAlphaTerrain, ignoreTransparentTerrain_);
        drawButton(layout.advancedToggles[2], L"Cliff wall casters",
                   Target::AdvancedCliffWalls, cliffWalls_);

        const auto radiusValue = gaussianRadius_ == 0U
            ? std::wstring(L"Off")
            : std::to_wstring(gaussianRadius_) + L" cell" +
                  (gaussianRadius_ == 1U ? L"" : L"s");
        const auto coverageValue =
            std::to_wstring(static_cast<int>(std::lround(coverageThreshold_ * 100.0F))) + L"%";
        std::wostringstream biasValue;
        biasValue << std::fixed << std::setprecision(1) << rayOriginOffset_;
        const auto islandValue = minimumShadowIslandPixels_ == 0U
            ? std::wstring(L"Off")
            : std::to_wstring(minimumShadowIslandPixels_) + L" cells";
        const auto threadsValue = workerThreads_ == 0U
            ? std::wstring(L"Auto") : std::to_wstring(workerThreads_);
        drawSlider(layout.advancedSliders[0], L"Gaussian radius — wider softens and joins edges",
                   radiusValue, Target::AdvancedGaussianRadius,
                   static_cast<float>(gaussianRadius_) / 3.0F);
        drawSlider(layout.advancedSliders[1], L"Sub-cell coverage cutoff — lower keeps more shadow",
                   coverageValue, Target::AdvancedCoverageThreshold,
                   (coverageThreshold_ - 0.20F) / 0.60F);
        drawSlider(layout.advancedSliders[2], L"Terrain ray bias — prevents raised-ground acne",
                   biasValue.str(), Target::AdvancedRayBias, rayOriginOffset_ / 32.0F);
        drawSlider(layout.advancedSliders[3], L"Minimum shadow island — removes isolated dots",
                   islandValue, Target::AdvancedMinimumIsland,
                   static_cast<float>(minimumShadowIslandPixels_) / 16.0F);
        drawSlider(layout.advancedSliders[4], L"Worker threads — performance only",
                   threadsValue, Target::AdvancedThreads,
                   static_cast<float>(workerThreads_) / 32.0F);
        drawText(
            L"Recommended: coherent filter on, radius 1, cutoff 45%, bias 2.0, island 4. "
            L"Cliff walls are experimental and may over-darken decorative cliffs.",
            D2D1::RectF(advancedLeft + 34.0F, advancedTop + advancedHeight - 103.0F,
                        advancedCard.right - 34.0F, advancedTop + advancedHeight - 65.0F),
            helpBodyFormat_.Get(), mutedBrush_.Get());
        drawButton(layout.advancedReset, L"Reset defaults", Target::AdvancedReset, false);
        drawButton(layout.advancedClose, L"Close", Target::AdvancedClose, false, true);
    }

    if (showAssets_) {
        backgroundBrush_->SetOpacity(0.86F);
        renderTarget_->FillRectangle(D2D1::RectF(0.0F, 0.0F, size.width, size.height),
                                     backgroundBrush_.Get());
        backgroundBrush_->SetOpacity(1.0F);

        const float assetsWidth = std::min(760.0F, size.width - 80.0F);
        const float assetsHeight = std::min(410.0F, size.height - 80.0F);
        const float assetsLeft = (size.width - assetsWidth) / 2.0F;
        const float assetsTop = (size.height - assetsHeight) / 2.0F;
        const auto assetsCard = D2D1::RectF(
            assetsLeft, assetsTop, assetsLeft + assetsWidth, assetsTop + assetsHeight);
        drawCard(assetsCard);

        drawText(L"Asset locations",
                 D2D1::RectF(assetsLeft + 24.0F, assetsTop + 20.0F,
                             assetsCard.right - 24.0F, assetsTop + 56.0F),
                 titleFormat_.Get(), textBrush_.Get());
        drawText(
            L"Locations are auto-detected on first start and saved for your Windows user. "
            L"Choose a current CASC installation or a classic MPQ installation such as 1.27b.",
            D2D1::RectF(assetsLeft + 25.0F, assetsTop + 59.0F,
                        assetsCard.right - 25.0F, assetsTop + 103.0F),
            helpBodyFormat_.Get(), mutedBrush_.Get());

        drawText(L"Warcraft III installation",
                 D2D1::RectF(assetsLeft + 24.0F, assetsTop + 109.0F,
                             assetsCard.right - 150.0F, assetsTop + 133.0F),
                 headingFormat_.Get(), textBrush_.Get());
        drawText(warcraftDirectory_ ? warcraftDirectory_->wstring() : L"Not set",
                 D2D1::RectF(assetsLeft + 24.0F, assetsTop + 133.0F,
                             assetsCard.right - 150.0F, assetsTop + 161.0F),
                 smallFormat_.Get(),
                 warcraftDirectory_ && isWarcraftDirectory(*warcraftDirectory_)
                     ? successBrush_.Get() : warningBrush_.Get());
        drawButton(layout.assetsWarcraftBrowse, L"Browse", Target::AssetsWarcraftBrowse, false);

        const bool legacyAssets =
            warcraftDirectory_ && isLegacyWarcraftDirectory(*warcraftDirectory_);
        drawText(L"CascLib library (CASC installations only)",
                 D2D1::RectF(assetsLeft + 24.0F, assetsTop + 204.0F,
                             assetsCard.right - 150.0F, assetsTop + 228.0F),
                 headingFormat_.Get(), textBrush_.Get());
        drawText(legacyAssets
                     ? L"Not required — classic Warcraft MPQ archives detected"
                     : (cascLibrary_ ? cascLibrary_->wstring() : L"Not set"),
                 D2D1::RectF(assetsLeft + 24.0F, assetsTop + 232.0F,
                             assetsCard.right - 150.0F, assetsTop + 260.0F),
                 smallFormat_.Get(),
                 legacyAssets || (cascLibrary_ && isCascLibrary(*cascLibrary_))
                     ? successBrush_.Get() : warningBrush_.Get());
        drawButton(layout.assetsCascBrowse, L"Browse", Target::AssetsCascBrowse, false);

        drawText(
            L"Map-imported models remain first priority. Installed Warcraft assets are needed "
            L"for stock object shadows. Classic MPQs are read in patch/local/expansion/base order.",
            D2D1::RectF(assetsLeft + 24.0F, assetsTop + 282.0F,
                        assetsCard.right - 24.0F, assetsTop + 326.0F),
            helpBodyFormat_.Get(), mutedBrush_.Get());
        drawButton(layout.assetsAutoDetect, L"Auto-detect", Target::AssetsAutoDetect, false);
        drawButton(layout.assetsClose, L"Close", Target::AssetsClose, false, true);
    }

    if (showAbout_) {
        backgroundBrush_->SetOpacity(0.86F);
        renderTarget_->FillRectangle(D2D1::RectF(0.0F, 0.0F, size.width, size.height),
                                     backgroundBrush_.Get());
        backgroundBrush_->SetOpacity(1.0F);

        const auto aboutCard = D2D1::RectF(
            layout.aboutSections[0].left - 24.0F,
            layout.aboutSections[0].top - 76.0F,
            layout.aboutSections[0].right + 24.0F,
            layout.aboutClose.bottom + 20.0F);
        drawCard(aboutCard);
        drawText(L"About ShadowMap Tool",
                 D2D1::RectF(aboutCard.left + 24.0F, aboutCard.top + 18.0F,
                             aboutCard.right - 150.0F, aboutCard.top + 54.0F),
                 titleFormat_.Get(), textBrush_.Get());
        drawText(L"Version 1.3 · expand a section to read more",
                 D2D1::RectF(aboutCard.left + 25.0F, aboutCard.top + 52.0F,
                             aboutCard.right - 24.0F, aboutCard.top + 73.0F),
                 smallFormat_.Get(), mutedBrush_.Get());

        constexpr std::array<std::wstring_view, 4> labels{
            L"Description and purpose", L"Why this tool was created",
            L"Compatibility and limitations", L"Credits"};
        constexpr std::array<Target, 4> targets{
            Target::AboutPurpose, Target::AboutOrigin,
            Target::AboutCompatibility, Target::AboutCredits};
        for (std::size_t index = 0; index < labels.size(); ++index) {
            const bool expanded = aboutSection_ == static_cast<int>(index);
            drawButton(layout.aboutSections[index],
                       (expanded ? L"▼  " : L"▶  ") + std::wstring(labels[index]),
                       targets[index], expanded);
        }

        if (aboutSection_ == 0) {
            drawText(
                L"ShadowMap Tool calculates, previews, exports, and safely writes Warcraft III "
                L"static war3map.shd data. It reconstructs terrain plus shadow-enabled doodad "
                L"and destructible geometry, then ray casts a configurable light vector. It "
                L"offers classic or smoother terrain, coherent 1x/2x/4x edge filtering, IgnoreShadow "
                L"regions, source filters, safe copies, and backups. The result remains Warcraft's "
                L"fixed binary four-cells-per-tile shadowmap rather than a higher-resolution texture. "
                L"Transparent terrain receivers are automatically excluded. Smooth terrain uses "
                L"matched caster/receiver surfaces plus slope-aware bias to avoid self-shadow acne. "
                L"Exact decorative cliff models and alpha-tested model materials remain compatibility work.",
                layout.aboutContents[0], helpBodyFormat_.Get(), textBrush_.Get());
        } else if (aboutSection_ == 1) {
            drawText(
                L"The tool was created because Oger-Lord's old Shadow Calculator has not worked "
                L"for many years, although map authors still urgently need this workflow. Path of "
                L"the Shaman is a 480 × 480 map with more than 60,000 doodads; World Editor's "
                L"native Calculate Shadows and Save Map was too inefficient and slow, and often "
                L"ended by crashing the editor. Temporarily removing doodads and destructibles "
                L"before calculation can work, but it is an amusing and fragile workaround. This "
                L"project provides a practical, repeatable, preview-first replacement.",
                layout.aboutContents[1], helpBodyFormat_.Get(), textBrush_.Get());
        } else if (aboutSection_ == 2) {
            drawText(
                L"Intended target: current Warcraft III 3.0, with direct classic MPQ asset support "
                L"validated against patch 1.27b. Older W3E v11, DOO v7/v8, W3R v5, object-data "
                L"v1–v3, and classic MDX 800 inputs are supported. CASC installs require CascLib; "
                L"classic MPQ installs do not. The input map must already run on the target client: "
                L"the tool does not down-convert modern scripts, formats, or assets. Workarounds "
                L"include terrain-only mode, map-imported models, or CLI --asset-dir with extracted assets.",
                layout.aboutContents[2], helpBodyFormat_.Get(), textBrush_.Get());
        } else if (aboutSection_ == 3) {
            drawText(
                L"Inspired by Zwiebelchen's upload “Shadowmap Calculator”, created by Oger-Lord "
                L"as “Shadow Calculator”. ShadowMap Tool is an independent modern reimplementation. "
                L"Archive access uses StormLib and CascLib by Ladislav Zezula.",
                layout.aboutContents[3], helpBodyFormat_.Get(), textBrush_.Get());
        }
        drawButton(layout.aboutClose, L"Close", Target::AboutClose, false, true);
    }

    if (showHelp_) {
        backgroundBrush_->SetOpacity(0.86F);
        renderTarget_->FillRectangle(D2D1::RectF(0.0F, 0.0F, size.width, size.height),
                                     backgroundBrush_.Get());
        backgroundBrush_->SetOpacity(1.0F);

        const float helpWidth = std::min(760.0F, size.width - 80.0F);
        const float helpHeight = std::min(570.0F, size.height - 80.0F);
        const float helpLeft = (size.width - helpWidth) / 2.0F;
        const float helpTop = (size.height - helpHeight) / 2.0F;
        const auto helpCard = D2D1::RectF(helpLeft, helpTop, helpLeft + helpWidth, helpTop + helpHeight);
        drawCard(helpCard);

        drawText(L"How to use ShadowMap Tool",
                 D2D1::RectF(helpLeft + 28.0F, helpTop + 22.0F,
                             helpCard.right - 28.0F, helpTop + 58.0F),
                 titleFormat_.Get(), textBrush_.Get());
        drawText(L"Press F1 to open Help · Esc to close",
                 D2D1::RectF(helpLeft + 29.0F, helpTop + 58.0F,
                             helpCard.right - 28.0F, helpTop + 80.0F),
                 smallFormat_.Get(), mutedBrush_.Get());

        drawText(L"Calculate a full map shadow",
                 D2D1::RectF(helpLeft + 28.0F, helpTop + 96.0F,
                             helpCard.right - 28.0F, helpTop + 122.0F),
                 headingFormat_.Get(), warningBrush_.Get());
        drawText(
            L"Choose shadow sources and terrain mode. Ultra 4x is the default edge quality; "
            L"Smooth 2x and Fast 1x trade quality for speed. Open Tuning for Gaussian radius, "
            L"coverage cutoff, terrain ray bias, alpha terrain, cleanup, and threads. Set the "
            L"light vector, calculate, inspect the rendered preview, then Save to map.",
            D2D1::RectF(helpLeft + 28.0F, helpTop + 126.0F,
                        helpCard.right - 28.0F, helpTop + 192.0F),
            helpBodyFormat_.Get(), textBrush_.Get());

        drawText(L"Map-authoring controls",
                 D2D1::RectF(helpLeft + 28.0F, helpTop + 210.0F,
                             helpCard.right - 28.0F, helpTop + 236.0F),
                 headingFormat_.Get(), textBrush_.Get());
        drawText(
            L"• A region whose name starts with IgnoreShadow excludes its contents when the "
            L"IgnoreShadow rects option is selected.\n"
            L"• To suppress an unwanted doodad shadow, set the doodad's Has shadow field to "
            L"False in Object Editor before calculating.\n"
            L"• Fully transparent alpha terrain is detected from its imported/installed BLP and "
            L"is ignored by default; switch off Ignore alpha terrain in Tuning to include it.\n"
            L"• Open Assets to select a current CASC installation plus CascLib.dll, or a classic "
            L"MPQ installation such as 1.27b, used to resolve installed stock models.",
            D2D1::RectF(helpLeft + 28.0F, helpTop + 240.0F,
                        helpCard.right - 28.0F, helpTop + 370.0F),
            helpBodyFormat_.Get(), textBrush_.Get());

        drawText(L"Preview and diagnostics",
                 D2D1::RectF(helpLeft + 28.0F, helpTop + 388.0F,
                             helpCard.right - 28.0F, helpTop + 414.0F),
                 headingFormat_.Get(), textBrush_.Get());
        drawText(
            L"Opening a map displays its existing SHD, even when it is empty. Calculate shadows "
            L"replaces it with the proposed rendered result before saving. Changed settings "
            L"require recalculation. Test patterns appear only in Test mode; Export SHD/PNG "
            L"exports exactly what is shown.",
            D2D1::RectF(helpLeft + 28.0F, helpTop + 418.0F,
                        helpCard.right - 150.0F, helpTop + 474.0F),
            helpBodyFormat_.Get(), mutedBrush_.Get());
        drawButton(layout.helpClose, L"Close", Target::HelpClose, false, true);
    }

    const HRESULT result = renderTarget_->EndDraw();
    if (result == D2DERR_RECREATE_TARGET) discardDeviceResources();
}

} // namespace w3shadow::gui

