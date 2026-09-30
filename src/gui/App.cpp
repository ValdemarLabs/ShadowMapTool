#include "gui/App.hpp"

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
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cwctype>
#include <exception>
#include <memory>
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
        showHelp_ = true;
        updateEditControls();
        if (!createDeviceResources()) return 2;
        paint();
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
        if (wParam == VK_ESCAPE && showHelp_) {
            showHelp_ = false;
            focused_ = Target::Help;
            updateEditControls();
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        }
        if (wParam == VK_F1) {
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
                                     margin + leftWidth, contentTop + 374.0F);
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
    const float editWidth = 62.0F;
    for (std::size_t index = 0; index < layout.lightEdits.size(); ++index) {
        layout.lightEdits[index] = D2D1::RectF(
            optionLeft + static_cast<float>(index) * (editWidth + 22.0F),
            layout.patternCard.top + 181.0F,
            optionLeft + static_cast<float>(index) * (editWidth + 22.0F) + editWidth,
            layout.patternCard.top + 217.0F);
    }

    layout.outputCard = D2D1::RectF(margin, contentTop + 388.0F,
                                    margin + leftWidth, contentTop + 484.0F);
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
    layout.testMode = D2D1::RectF(size.width - 386.0F, 29.0F, size.width - 284.0F, 61.0F);
    layout.help = D2D1::RectF(size.width - 274.0F, 29.0F, size.width - 184.0F, 61.0F);
    const float helpWidth = std::min(760.0F, size.width - 80.0F);
    const float helpHeight = std::min(570.0F, size.height - 80.0F);
    const float helpLeft = (size.width - helpWidth) / 2.0F;
    const float helpTop = (size.height - helpHeight) / 2.0F;
    layout.helpClose = D2D1::RectF(helpLeft + helpWidth - 120.0F, helpTop + helpHeight - 58.0F,
                                   helpLeft + helpWidth - 24.0F, helpTop + helpHeight - 20.0F);
    layout.status = D2D1::RectF(margin, size.height - 47.0F, size.width - margin, size.height - 16.0F);
    return layout;
}

void App::updateEditControls()
{
    if (window_ == nullptr) return;
    const auto layout = calculateLayout();
    const float scale = static_cast<float>(dpi_) / 96.0F;
    const bool visible = !testMode_ && !showHelp_;
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
    if (showHelp_) return contains(layout.helpClose, x, y) ? Target::HelpClose : Target::None;
    if (contains(layout.help, x, y)) return Target::Help;
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

void App::moveFocus(const bool backwards)
{
    if (showHelp_) {
        focused_ = Target::HelpClose;
        InvalidateRect(window_, nullptr, FALSE);
        return;
    }
    const auto available = [this](const Target target) {
        if (target >= Target::Terrain && target <= Target::IgnoreRegions) return !testMode_;
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
    case Target::Help:
        showHelp_ = true;
        focused_ = Target::HelpClose;
        updateEditControls();
        break;
    case Target::HelpClose:
        showHelp_ = false;
        focused_ = Target::Help;
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
        const std::array<std::filesystem::path, 2> gameDirectories{
            L"C:\\Program Files (x86)\\Warcraft III", L"C:\\Program Files\\Warcraft III"};
        for (const auto& directory : gameDirectories) {
            if (!std::filesystem::exists(directory / L".build.info")) continue;
            casc = std::make_shared<CascAssetProvider>(directory);
            if (casc->available()) assets.add(casc);
            break;
        }
        GenerationOptions options;
        options.lightDirection = readLightDirection();
        options.terrain = includeTerrain_;
        options.doodads = includeDoodads_;
        options.destructibles = includeDestructibles_;
        options.honorIgnoreShadowRegions = honorIgnoreRegions_;
        auto generated = generateShadowMap(source, assets, options);
        shadowMap_ = std::move(generated.shadow);
        previewKind_ = PreviewKind::Calculated;
        calculationDirty_ = false;
        refreshPreviewBitmap();
        InvalidateRect(window_, nullptr, FALSE);
        std::wostringstream message;
        message << L"Preview rendered · " << generated.stats.triangles << L" triangles · "
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
    drawText(L"Warcraft III static-shadow generation workspace",
             D2D1::RectF(83.0F, 52.0F, 520.0F, 75.0F), smallFormat_.Get(), mutedBrush_.Get());

    const auto badgeRect = D2D1::RectF(size.width - 174.0F, 29.0F, size.width - 28.0F, 61.0F);
    const auto badge = D2D1::RoundedRect(badgeRect, 16.0F, 16.0F);
    warningBrush_->SetOpacity(0.14F);
    renderTarget_->FillRoundedRectangle(badge, warningBrush_.Get());
    warningBrush_->SetOpacity(1.0F);
    renderTarget_->DrawRoundedRectangle(badge, warningBrush_.Get(), 1.0F);
    drawText(L"●  FULL GENERATOR", badgeRect, buttonFormat_.Get(), warningBrush_.Get());

    drawButton(layout.testMode, testMode_ ? L"Test mode ON" : L"Test mode",
               Target::TestMode, testMode_);
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
        const std::array<std::wstring_view, 4> labels{
            L"Terrain", L"Doodads", L"Destructibles", L"IgnoreShadow rects"};
        const std::array<bool, 4> selected{
            includeTerrain_, includeDoodads_, includeDestructibles_, honorIgnoreRegions_};
        for (std::size_t index = 0; index < labels.size(); ++index) {
            const auto target = static_cast<Target>(static_cast<int>(Target::Terrain) +
                                                    static_cast<int>(index));
            drawButton(layout.calculationOptions[index], labels[index], target, selected[index]);
        }
        drawText(L"Light vector (X, Y, Z) · default 1, 1, -1",
                 D2D1::RectF(layout.patternCard.left + 16.0F, layout.patternCard.top + 146.0F,
                             layout.patternCard.right - 16.0F, layout.patternCard.top + 170.0F),
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
    const auto originBadge = D2D1::RectF(layout.previewCanvas.left + 10.0F,
                                         layout.previewCanvas.bottom - 30.0F,
                                         layout.previewCanvas.left + 260.0F,
                                         layout.previewCanvas.bottom - 9.0F);
    drawText(L"Preview top · SHD rows bottom-up", originBadge,
             smallFormat_.Get(), mutedBrush_.Get());

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
            L"Choose Terrain, Doodads, and Destructibles independently. Set the light vector "
            L"(default 1, 1, -1), then choose Calculate shadows. Calculation does not write a "
            L"map: inspect the newly rendered full preview, choose Save as copy, then select "
            L"Save to map.",
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
            L"• Alpha terrain tiles should not receive static shadow. Automatic alpha-BLP "
            L"detection is not yet available: temporarily replace the alpha tile, calculate, "
            L"then restore it, or cover it with an IgnoreShadow region.\n"
            L"• Keep the bundled CascLib.dll beside the executable so installed stock models "
            L"can be resolved.",
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

