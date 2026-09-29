#include "gui/App.hpp"

#include "archive/MapArchive.hpp"
#include "formats/Png.hpp"
#include "util/FileIO.hpp"

#include <d2d1helper.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cwctype>
#include <exception>
#include <sstream>
#include <string_view>
#include <vector>

namespace w3shadow::gui {
namespace {

using Microsoft::WRL::ComPtr;

constexpr wchar_t WindowClassName[] = L"W3ShadowModernWindow";
constexpr wchar_t WindowTitle[] = L"ShadowMap Tool";

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
    if (!initializeFactories() || !createWindow(instance, smokeTest ? SW_HIDE : showCommand)) return 1;
    if (startupMap) loadMap(*startupMap);
    if (smokeTest) {
        if (!createDeviceResources()) return 2;
        SetTimer(window_, 1U, 100U, nullptr);
    }

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
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
    windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
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
    BOOL darkMode = TRUE;
    constexpr DWORD UseImmersiveDarkMode = 20;
    DwmSetWindowAttribute(window_, UseImmersiveDarkMode, &darkMode, sizeof(darkMode));
    DragAcceptFiles(window_, TRUE);
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
    case WM_MOUSEMOVE: {
        if (!trackingMouse_) {
            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window_, 0};
            TrackMouseEvent(&tracking);
            trackingMouse_ = true;
        }
        const auto point = mousePoint(lParam);
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
        const auto target = hitTest(point.x, point.y);
        if (target != Target::None) {
            focused_ = target;
            activate(target);
        }
        return 0;
    }
    case WM_KEYDOWN:
        if (wParam == VK_TAB) {
            moveFocus((GetKeyState(VK_SHIFT) & 0x8000) != 0);
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
            buildTestMap();
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
        FAILED(makeFormat(13.0F, DWRITE_FONT_WEIGHT_SEMI_BOLD, buttonFormat_))) {
        discardDeviceResources();
        return false;
    }
    buttonFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    buttonFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    for (auto* format : {bodyFormat_.Get(), smallFormat_.Get()}) {
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }

    rebuildPreview();
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
    titleFormat_.Reset();
    headingFormat_.Reset();
    bodyFormat_.Reset();
    smallFormat_.Reset();
    buttonFormat_.Reset();
    renderTarget_.Reset();
}

void App::resize(const UINT width, const UINT height)
{
    if (renderTarget_) renderTarget_->Resize(D2D1::SizeU(width, height));
    InvalidateRect(window_, nullptr, FALSE);
}

void App::updateDpi(const UINT dpi)
{
    dpi_ = dpi;
    if (renderTarget_) renderTarget_->SetDpi(static_cast<float>(dpi), static_cast<float>(dpi));
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
                                     margin + leftWidth, contentTop + 348.0F);
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

    layout.outputCard = D2D1::RectF(margin, contentTop + 362.0F,
                                    margin + leftWidth, contentTop + 458.0F);
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
    layout.buildMap = D2D1::RectF(layout.previewCard.right - 190.0F, actionsTop,
                                  layout.previewCard.right - 18.0F, actionsTop + 40.0F);
    layout.status = D2D1::RectF(margin, size.height - 47.0F, size.width - margin, size.height - 16.0F);
    return layout;
}

App::Target App::hitTest(const float x, const float y) const
{
    const auto layout = calculateLayout();
    if (contains(layout.browse, x, y)) return Target::Browse;
    for (std::size_t index = 0; index < layout.patterns.size(); ++index) {
        if (contains(layout.patterns[index], x, y)) {
            return static_cast<Target>(static_cast<int>(Target::PatternBlack) + static_cast<int>(index));
        }
    }
    if (contains(layout.copyMode, x, y)) return Target::CopyMode;
    if (contains(layout.inPlaceMode, x, y)) return Target::InPlaceMode;
    if (contains(layout.exportShd, x, y)) return Target::ExportShd;
    if (contains(layout.exportPng, x, y)) return Target::ExportPng;
    if (contains(layout.buildMap, x, y)) return Target::BuildMap;
    return Target::None;
}

D2D1_POINT_2F App::mousePoint(const LPARAM lParam) const
{
    const float scale = static_cast<float>(dpi_) / 96.0F;
    return D2D1::Point2F(static_cast<float>(GET_X_LPARAM(lParam)) / scale,
                         static_cast<float>(GET_Y_LPARAM(lParam)) / scale);
}

void App::moveFocus(const bool backwards)
{
    constexpr int first = static_cast<int>(Target::Browse);
    constexpr int count = static_cast<int>(Target::Count);
    int value = static_cast<int>(focused_);
    if (value < first || value >= count) value = first;
    value = backwards ? (value - 1 + count) % count : (value + 1) % count;
    focused_ = static_cast<Target>(value);
    InvalidateRect(window_, nullptr, FALSE);
}

void App::activate(const Target target)
{
    switch (target) {
    case Target::Browse: chooseMap(); break;
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
    case Target::BuildMap: buildTestMap(); break;
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

void App::loadMap(const std::filesystem::path& path)
{
    try {
        MapArchive archive(path);
        const auto parsed = parseW3E(archive.read("war3map.w3e"));
        if (!parsed) throw std::runtime_error(parsed.error);

        mapPath_ = path;
        mapInfo_ = parsed.info;
        rebuildPreview();

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
    generatePattern(*shadowMap_, pattern_);
    previewBitmap_.Reset();
    if (!renderTarget_) return;

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
            (mapPath_->stem().wstring() + L"." + patternFilePart(pattern_) + L".shd");
        const auto output = saveFileDialog(window_, L"Export shadow map", suggested,
                                           L"Warcraft III shadow map", L"*.shd", L"shd");
        if (!output) return;
        writeBinaryFileAtomic(*output, shadowMap_->bytes(), true);
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
            (mapPath_->stem().wstring() + L"." + patternFilePart(pattern_) + L".png");
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

void App::buildTestMap()
{
    if (!mapPath_ || !mapInfo_ || !shadowMap_) {
        setStatus(L"Choose a map before building a shadow test map.", StatusKind::Warning);
        return;
    }
    try {
        setStatus(L"Writing and validating the map archive…", StatusKind::Neutral);
        UpdateWindow(window_);
        if (outputMode_ == OutputMode::InPlace) {
            const int answer = MessageBoxW(
                window_,
                L"Replace war3map.shd in the selected map?\n\nThe original map will be retained as a numbered .w3shadow.bak file.",
                L"Confirm in-place replacement", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
            if (answer != IDYES) {
                setStatus(L"In-place replacement cancelled.", StatusKind::Neutral);
                return;
            }
            const auto backup = MapArchive::replaceShadowInPlace(*mapPath_, shadowMap_->bytes());
            setStatus(L"Map updated. Backup: " + backup.filename().wstring(), StatusKind::Success);
            return;
        }

        const auto extension = mapPath_->extension().wstring();
        const auto suggested = mapPath_->parent_path() /
            (mapPath_->stem().wstring() + L"." + patternFilePart(pattern_) +
             L".shadowtest" + extension);
        const wchar_t* pattern = extension == L".w3m" ? L"*.w3m" : L"*.w3x";
        const wchar_t* defaultExtension = extension == L".w3m" ? L"w3m" : L"w3x";
        const auto output = saveFileDialog(window_, L"Save shadow test map", suggested,
                                           L"Warcraft III map", pattern, defaultExtension);
        if (!output) {
            setStatus(L"Map creation cancelled.", StatusKind::Neutral);
            return;
        }
        MapArchive::replaceShadowInCopy(*mapPath_, *output, shadowMap_->bytes(), true);
        setStatus(L"Created and validated: " + output->filename().wstring(), StatusKind::Success);
    } catch (const std::invalid_argument& error) {
        showError(L"Build test map", error);
    } catch (const std::exception& error) {
        showError(L"Build test map", error);
    }
}

void App::setStatus(std::wstring message, const StatusKind kind)
{
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

void App::paint()
{
    if (!createDeviceResources()) return;
    const auto layout = calculateLayout();
    const auto size = renderTarget_->GetSize();

    renderTarget_->BeginDraw();
    renderTarget_->Clear(rgb(0x0B1020));

    const auto logo = D2D1::RoundedRect(D2D1::RectF(28.0F, 24.0F, 68.0F, 64.0F), 11.0F, 11.0F);
    renderTarget_->FillRoundedRectangle(logo, accentBrush_.Get());
    drawText(L"S", D2D1::RectF(28.0F, 23.0F, 68.0F, 65.0F), titleFormat_.Get(), textBrush_.Get());
    drawText(L"ShadowMap Tool", D2D1::RectF(82.0F, 19.0F, 390.0F, 53.0F),
             titleFormat_.Get(), textBrush_.Get());
    drawText(L"Warcraft III static-shadow validation workspace",
             D2D1::RectF(83.0F, 52.0F, 520.0F, 75.0F), smallFormat_.Get(), mutedBrush_.Get());

    const auto badgeRect = D2D1::RectF(size.width - 174.0F, 29.0F, size.width - 28.0F, 61.0F);
    const auto badge = D2D1::RoundedRect(badgeRect, 16.0F, 16.0F);
    warningBrush_->SetOpacity(0.14F);
    renderTarget_->FillRoundedRectangle(badge, warningBrush_.Get());
    warningBrush_->SetOpacity(1.0F);
    renderTarget_->DrawRoundedRectangle(badge, warningBrush_.Get(), 1.0F);
    drawText(L"●  VALIDATION MODE", badgeRect, buttonFormat_.Get(), warningBrush_.Get());

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
    drawText(L"Validation pattern",
             D2D1::RectF(layout.patternCard.left + 16.0F, layout.patternCard.top + 14.0F,
                         layout.patternCard.right, layout.patternCard.top + 38.0F),
             headingFormat_.Get(), textBrush_.Get());
    const std::array<Pattern, 6> patterns{
        Pattern::Black, Pattern::White, Pattern::Checker,
        Pattern::XGradient, Pattern::YGradient, Pattern::Quadrants};
    for (std::size_t index = 0; index < patterns.size(); ++index) {
        const auto target = static_cast<Target>(static_cast<int>(Target::PatternBlack) +
                                                static_cast<int>(index));
        drawButton(layout.patterns[index], patternLabel(patterns[index]), target,
                   pattern_ == patterns[index]);
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
    std::wstring previewMeta = patternLabel(pattern_);
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
    const auto originBadge = D2D1::RectF(layout.previewCanvas.left + 10.0F,
                                         layout.previewCanvas.bottom - 30.0F,
                                         layout.previewCanvas.left + 151.0F,
                                         layout.previewCanvas.bottom - 9.0F);
    drawText(L"Stored origin: (0, 0)", originBadge, smallFormat_.Get(), mutedBrush_.Get());

    drawButton(layout.exportShd, L"Export SHD", Target::ExportShd, false);
    drawButton(layout.exportPng, L"Export PNG", Target::ExportPng, false);
    drawButton(layout.buildMap, L"Build test map", Target::BuildMap, false, true);

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

    const HRESULT result = renderTarget_->EndDraw();
    if (result == D2DERR_RECREATE_TARGET) discardDeviceResources();
}

} // namespace w3shadow::gui

