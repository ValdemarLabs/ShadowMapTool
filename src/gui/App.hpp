#pragma once

#include "formats/W3E.hpp"
#include "shadow/Pattern.hpp"
#include "shadow/ShadowMap.hpp"
#include "shadow/ShadowGenerator.hpp"

#include <d2d1.h>
#include <dwrite.h>
#include <windows.h>
#include <wrl/client.h>

#include <array>
#include <filesystem>
#include <fstream>
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
    enum class PreviewKind { Empty, Existing, Calculated, Test };
    enum class Target : int {
        None = -1,
        Browse,
        Terrain,
        Doodads,
        Destructibles,
        IgnoreRegions,
        TerrainClassic,
        TerrainSmooth,
        ShadowSamples1,
        ShadowSamples2,
        ShadowSamples4,
        Advanced,
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
        SaveMap,
        TestMode,
        Assets,
        Logs,
        About,
        Help,
        AssetsWarcraftBrowse,
        AssetsCascBrowse,
        AssetsAutoDetect,
        AssetsClose,
        AboutPurpose,
        AboutOrigin,
        AboutCompatibility,
        AboutCredits,
        AboutClose,
        HelpClose,
        AdvancedCoherentFilter,
        AdvancedAlphaTerrain,
        AdvancedCliffWalls,
        AdvancedGaussianRadius,
        AdvancedCoverageThreshold,
        AdvancedRayBias,
        AdvancedMinimumIsland,
        AdvancedMaximumCasterSpan,
        AdvancedThreads,
        AdvancedReset,
        AdvancedClose,
        Count
    };

    struct Layout {
        D2D1_RECT_F mapCard{};
        D2D1_RECT_F browse{};
        D2D1_RECT_F patternCard{};
        std::array<D2D1_RECT_F, 6> calculationOptions{};
        std::array<D2D1_RECT_F, 3> qualityOptions{};
        std::array<D2D1_RECT_F, 6> patterns{};
        std::array<D2D1_RECT_F, 3> lightEdits{};
        D2D1_RECT_F outputCard{};
        D2D1_RECT_F advanced{};
        D2D1_RECT_F copyMode{};
        D2D1_RECT_F inPlaceMode{};
        D2D1_RECT_F previewCard{};
        D2D1_RECT_F previewCanvas{};
        D2D1_RECT_F exportShd{};
        D2D1_RECT_F exportPng{};
        D2D1_RECT_F buildMap{};
        D2D1_RECT_F saveMap{};
        D2D1_RECT_F testMode{};
        D2D1_RECT_F assets{};
        D2D1_RECT_F logs{};
        D2D1_RECT_F about{};
        D2D1_RECT_F help{};
        D2D1_RECT_F assetsWarcraftBrowse{};
        D2D1_RECT_F assetsCascBrowse{};
        D2D1_RECT_F assetsAutoDetect{};
        D2D1_RECT_F assetsClose{};
        std::array<D2D1_RECT_F, 4> aboutSections{};
        std::array<D2D1_RECT_F, 4> aboutContents{};
        D2D1_RECT_F aboutClose{};
        D2D1_RECT_F helpClose{};
        D2D1_RECT_F status{};
        std::array<D2D1_RECT_F, 3> advancedToggles{};
        std::array<D2D1_RECT_F, 6> advancedSliders{};
        D2D1_RECT_F advancedReset{};
        D2D1_RECT_F advancedClose{};
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
    void updateEditControls();

    [[nodiscard]] Layout calculateLayout() const;
    [[nodiscard]] Target hitTest(float x, float y) const;
    [[nodiscard]] D2D1_POINT_2F mousePoint(LPARAM lParam) const;
    void activate(Target target);
    void moveFocus(bool backwards);

    [[nodiscard]] bool isSlider(Target target) const;
    void updateSlider(Target target, float x);
    void stepSlider(Target target, int direction);
    void chooseMap();
    void loadMap(const std::filesystem::path& path);
    void selectPattern(Pattern pattern);
    void rebuildPreview();
    void refreshPreviewBitmap();
    void exportShadow();
    void exportPng();
    void calculateShadows();
    void saveShadowMap();
    void loadAssetSettings(bool persistDetected);
    void saveAssetSettings() const;
    void autoDetectAssetSettings(bool notify);
    void chooseWarcraftDirectory();
    void chooseCascLibrary();
    void initializeSessionLog();
    void logEvent(StatusKind kind, std::wstring_view message);
    void openLogsFolder();
    [[nodiscard]] Vec3 readLightDirection() const;

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
    void drawSlider(
        const D2D1_RECT_F& rectangle,
        std::wstring_view label,
        std::wstring_view value,
        Target target,
        float normalizedValue);
    std::array<HWND, 3> lightEdits_{};
    HBRUSH editBrush_ = nullptr;
    UINT dpi_ = 96;
    bool trackingMouse_ = false;
    Target hovered_ = Target::None;
    Target focused_ = Target::Browse;

    Microsoft::WRL::ComPtr<ID2D1Factory> d2dFactory_;
    Microsoft::WRL::ComPtr<IDWriteFactory> writeFactory_;
    Target draggedSlider_ = Target::None;
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
    Microsoft::WRL::ComPtr<ID2D1Bitmap> appIconBitmap_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> previewBitmap_;

    Microsoft::WRL::ComPtr<IDWriteTextFormat> titleFormat_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> headingFormat_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> bodyFormat_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> smallFormat_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> buttonFormat_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> helpBodyFormat_;

    std::optional<std::filesystem::path> mapPath_;
    std::optional<W3EInfo> mapInfo_;
    std::optional<ShadowMap> shadowMap_;
    Pattern pattern_ = Pattern::Quadrants;
    OutputMode outputMode_ = OutputMode::Copy;
    bool showHelp_ = false;
    bool showAssets_ = false;
    bool showAbout_ = false;
    int aboutSection_ = 0;
    bool testMode_ = false;
    bool includeTerrain_ = true;
    bool showAdvanced_ = false;
    bool includeDoodads_ = true;
    bool includeDestructibles_ = true;
    bool honorIgnoreRegions_ = true;
    TerrainGeometryMode terrainGeometry_ = TerrainGeometryMode::SmoothSubTile;
    std::uint32_t shadowSampleGrid_ = 4;
    bool calculationDirty_ = false;
    std::optional<std::filesystem::path> warcraftDirectory_;
    std::optional<std::filesystem::path> cascLibrary_;
    ShadowCoverageMode coverageMode_ = ShadowCoverageMode::CoherentFilter;
    std::uint32_t gaussianRadius_ = 1;
    float coverageThreshold_ = 0.45F;
    float rayOriginOffset_ = 2.0F;
    std::uint32_t minimumShadowIslandPixels_ = 4;
    float maximumCasterHorizontalSpan_ = 16384.0F;
    std::uint32_t workerThreads_ = 0;
    bool ignoreTransparentTerrain_ = true;
    bool cliffWalls_ = false;
    std::filesystem::path logsDirectory_;
    std::filesystem::path sessionLogPath_;
    std::ofstream sessionLog_;
    PreviewKind previewKind_ = PreviewKind::Empty;
    std::wstring status_ = L"Choose a Warcraft III map to begin.";
    StatusKind statusKind_ = StatusKind::Neutral;
};

} // namespace w3shadow::gui

