#include "gui/App.hpp"

#include <objbase.h>
#include <shellapi.h>
#include <windows.h>

#include <filesystem>
#include <optional>
#include <string_view>
#include <utility>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(comResult)) {
        MessageBoxW(nullptr, L"Unable to initialize Windows COM services.",
                    L"ShadowMap Tool", MB_OK | MB_ICONERROR);
        return 1;
    }

    std::optional<std::filesystem::path> startupMap;
    bool smokeTest = false;
    int argumentCount = 0;
    PWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    if (arguments != nullptr) {
        for (int index = 1; index < argumentCount; ++index) {
            if (std::wstring_view(arguments[index]) == L"--smoke-test") smokeTest = true;
            else if (!startupMap) startupMap = std::filesystem::path(arguments[index]);
        }
        LocalFree(arguments);
    }

    w3shadow::gui::App app;
    const int result = app.run(instance, showCommand, std::move(startupMap), smokeTest);
    CoUninitialize();
    return result;
}

