#pragma once

#include "formats/W3E.hpp"
#include "shadow/Pattern.hpp"
#include "shadow/ShadowMap.hpp"

#include <d2d1.h>
#include <dwrite.h>
#include <windows.h>
#include <wrl/client.h>

#include <array>
#include <filesystem>
#include <optional>
#include <string>

namespace w3shadow::gui {

class App {
public:
    int run(
        HINSTANCE instance,
        int showCommand,
        std::optional<std::filesystem::path> startupMap = std::nullopt,
        bool smokeTest = false);

private:
    enum class OutputMode { Copy, InPlace };
    enum class StatusKind { Neutral, Success, Warning, Error };
    enum class Target : int {
        None = -1,
        Browse,
        PatternBlack,
        PatternWhite,
        PatternChecker,
        PatternXGradient,
        PatternYGradient,
        PatternQuadrants,
        CopyMode,
        InPlaceMode,
        ExportShd,
        ExportPng,
        BuildMap,
        Count
    };

    struct Layout {
        D2D1_RECT_F mapCard{};
        D2D1_RECT_F browse{};
        D2D1_RECT_F patternCard{};
        std::array<D2D1_RECT_F, 6> patterns{};
        D2D1_RECT_F outputCard{};
        D2D1_RECT_F copyMode{};
        D2D1_RECT_F inPlaceMode{};
        D2D1_RECT_F previewCard{};
        D2D1_RECT_F previewCanvas{};
        D2D1_RECT_F exportShd{};
        D2D1_RECT_F exportPng{};
        D2D1_RECT_F buildMap{};
        D2D1_RECT_F status{};
    };

    static LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT handleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    bool initializeFactories();
    bool createWindow(HINSTANCE instance, int showCommand);
    bool createDeviceResources();
    void discardDeviceResources();
    void paint();
    void resize(UINT width, UINT height);
    void updateDpi(UINT dpi);

    [[nodiscard]] Layout calculateLayout() const;
    [[nodiscard]] Target hitTest(float x, float y) const;
    [[nodiscard]] D2D1_POINT_2F mousePoint(LPARAM lParam) const;
    void activate(Target target);
    void moveFocus(bool backwards);

    void chooseMap();
    void loadMap(const std::filesystem::path& path);
    void selectPattern(Pattern pattern);
    void rebuildPreview();
    void exportShadow();
    void exportPng();
    void buildTestMap();

    void setStatus(std::wstring message, StatusKind kind);
    void showError(const std::wstring& action, const std::exception& error);

    void drawText(
        std::wstring_view text,
        const D2D1_RECT_F& rectangle,
        IDWriteTextFormat* format,
        ID2D1Brush* brush);
    void drawCard(const D2D1_RECT_F& rectangle);
    void drawButton(
        const D2D1_RECT_F& rectangle,
        std::wstring_view label,
        Target target,
        bool selected,
        bool primary = false);

    HWND window_ = nullptr;
    UINT dpi_ = 96;
    bool trackingMouse_ = false;
    Target hovered_ = Target::None;
    Target focused_ = Target::Browse;

    Microsoft::WRL::ComPtr<ID2D1Factory> d2dFactory_;
    Microsoft::WRL::ComPtr<IDWriteFactory> writeFactory_;
    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> renderTarget_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> backgroundBrush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> panelBrush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> surfaceBrush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> borderBrush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> textBrush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> mutedBrush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> accentBrush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> successBrush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> warningBrush_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> errorBrush_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> previewBitmap_;

    Microsoft::WRL::ComPtr<IDWriteTextFormat> titleFormat_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> headingFormat_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> bodyFormat_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> smallFormat_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> buttonFormat_;

    std::optional<std::filesystem::path> mapPath_;
    std::optional<W3EInfo> mapInfo_;
    std::optional<ShadowMap> shadowMap_;
    Pattern pattern_ = Pattern::Quadrants;
    OutputMode outputMode_ = OutputMode::Copy;
    std::wstring status_ = L"Choose a Warcraft III map to begin.";
    StatusKind statusKind_ = StatusKind::Neutral;
};

} // namespace w3shadow::gui

