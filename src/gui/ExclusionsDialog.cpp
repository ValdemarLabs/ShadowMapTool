#include "gui/ExclusionsDialog.hpp"
#include "gui/Resources.h"

#include <commctrl.h>
#include <algorithm>
#include <array>
#include <cwctype>
#include <stdexcept>

namespace w3shadow::gui {
namespace {
std::wstring wide(const std::string& text)
{
    if (text.empty()) return {};
    const auto size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size);
    return result;
}
std::wstring lower(std::wstring text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](wchar_t ch) { return std::towlower(ch); });
    return text;
}
struct DialogState {
    std::filesystem::path map;
    std::vector<ShadowObjectRow> rows;
    ShadowExclusions pending;
    bool filling = false;
    bool smoke = false;
};
void populate(HWND dialog, DialogState& state)
{
    state.filling = true;
    const auto list = GetDlgItem(dialog, IDC_EXCLUSION_LIST);
    ListView_DeleteAllItems(list);
    std::array<wchar_t, 512> search{};
    GetDlgItemTextW(dialog, IDC_EXCLUSION_SEARCH, search.data(), static_cast<int>(search.size()));
    const auto query = lower(search.data());
    for (std::size_t i = 0; i < state.rows.size(); ++i) {
        const auto& row = state.rows[i];
        if (!query.empty() && lower(wide(row.rawcode + " " + row.name + " " + row.modelPath)).find(query) == std::wstring::npos) continue;
        auto id = wide(row.rawcode);
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = ListView_GetItemCount(list);
        item.pszText = id.data();
        item.lParam = static_cast<LPARAM>(i);
        const auto index = ListView_InsertItem(list, &item);
        const auto setting = row.unresolved ? L"Unknown" : row.anyEnabled && row.anyDisabled
            ? L"Mixed skins" : row.anyEnabled ? L"True" : L"False";
        std::array<std::wstring, 4> values{wide(row.name), std::to_wstring(row.placements), setting, wide(row.modelPath)};
        for (int column = 0; column < 4; ++column) {
            ListView_SetItemText(list, index, column + 1, values[static_cast<std::size_t>(column)].data());
        }
        ListView_SetCheckState(list, index, state.pending.contains(row.rawcode));
    }
    const auto summary = std::to_wstring(state.pending.size()) + L" excluded types. Check = exclude; uncheck = follow object data.";
    SetDlgItemTextW(dialog, IDC_EXCLUSION_SUMMARY, summary.c_str());
    state.filling = false;
}
INT_PTR CALLBACK procedure(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto* state = reinterpret_cast<DialogState*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG) {
        state = reinterpret_cast<DialogState*>(lParam);
        SetWindowLongPtrW(dialog, DWLP_USER, lParam);
        const auto list = GetDlgItem(dialog, IDC_EXCLUSION_LIST);
        ListView_SetExtendedListViewStyle(list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES);
        constexpr std::array<const wchar_t*, 5> titles{L"Exclude / ID", L"Name", L"Placed", L"Has shadow", L"Model"};
        constexpr std::array<int, 5> widths{100, 175, 60, 100, 350};
        const auto scale = static_cast<int>(GetDpiForWindow(dialog));
        for (int i = 0; i < 5; ++i) {
            LVCOLUMNW column{};
            column.mask = LVCF_TEXT | LVCF_WIDTH;
            column.pszText = const_cast<wchar_t*>(titles[static_cast<std::size_t>(i)]);
            column.cx = MulDiv(widths[static_cast<std::size_t>(i)], scale, 96);
            ListView_InsertColumn(list, i, &column);
        }
        SendDlgItemMessageW(dialog, IDC_EXCLUSION_SEARCH, EM_SETLIMITTEXT, 511, 0);
        populate(dialog, *state);
        if (state->smoke) {
            // Exercise checkbox notifications, filtering, and cancel without touching a map.
            ListView_SetCheckState(list, 0, TRUE);
            SetDlgItemTextW(dialog, IDC_EXCLUSION_SEARCH, L"Test");
            PostMessageW(dialog, WM_COMMAND, IDCANCEL, 0);
        }
        return TRUE;
    }
    if (!state) return FALSE;
    if (message == WM_NOTIFY && !state->filling) {
        const auto* header = reinterpret_cast<NMHDR*>(lParam);
        if (header->idFrom == IDC_EXCLUSION_LIST && header->code == LVN_ITEMCHANGED) {
            const auto* change = reinterpret_cast<NMLISTVIEW*>(lParam);
            if ((change->uChanged & LVIF_STATE) && ((change->uOldState ^ change->uNewState) & LVIS_STATEIMAGEMASK)) {
                const auto& row = state->rows.at(static_cast<std::size_t>(change->lParam));
                if (ListView_GetCheckState(header->hwndFrom, change->iItem)) state->pending.insert(row.rawcode);
                else state->pending.erase(row.rawcode);
                const auto summary = std::to_wstring(state->pending.size()) + L" excluded types. Check = exclude; uncheck = follow object data.";
                SetDlgItemTextW(dialog, IDC_EXCLUSION_SUMMARY, summary.c_str());
            }
        }
    }
    if (message == WM_COMMAND) {
        if (LOWORD(wParam) == IDC_EXCLUSION_SEARCH && HIWORD(wParam) == EN_CHANGE) {
            populate(dialog, *state); return TRUE;
        }
        if (LOWORD(wParam) == IDC_EXCLUSION_CLEAR) {
            state->pending.clear(); populate(dialog, *state); return TRUE;
        }
        if (LOWORD(wParam) == IDOK) {
            try {
                saveShadowExclusions(state->map, state->pending);
                EndDialog(dialog, IDOK);
            } catch (const std::exception& error) {
                MessageBoxW(dialog, wide(error.what()).c_str(), L"Could not save exclusions", MB_OK | MB_ICONERROR);
            }
            return TRUE;
        }
        if (LOWORD(wParam) == IDCANCEL) { EndDialog(dialog, IDCANCEL); return TRUE; }
    }
    if (message == WM_CLOSE) { EndDialog(dialog, IDCANCEL); return TRUE; }
    return FALSE;
}
INT_PTR show(HWND owner, DialogState& state)
{
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES};
    if (!InitCommonControlsEx(&controls)) throw std::runtime_error("could not initialize object list");
    const auto result = DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_EXCLUSIONS), owner, procedure, reinterpret_cast<LPARAM>(&state));
    if (result == -1) throw std::runtime_error("could not open shadow exclusions");
    return result;
}
} // namespace

bool editShadowExclusions(HWND owner, const std::filesystem::path& map,
    const std::vector<ShadowObjectRow>& rows, ShadowExclusions& exclusions)
{
    DialogState state{map, rows, exclusions};
    // Preserve saved exclusions whose object types are temporarily absent from the map.
    for (const auto& id : exclusions) {
        if (std::none_of(state.rows.begin(), state.rows.end(), [&](const auto& row) { return row.rawcode == id; })) {
            state.rows.push_back({id, "(not currently placed)", {}, 0, false, false, true});
        }
    }
    if (show(owner, state) != IDOK) return false;
    const bool changed = state.pending != exclusions;
    exclusions = std::move(state.pending);
    return changed;
}

void smokeTestExclusionsDialog(HWND owner)
{
    DialogState state;
    state.rows.push_back({"D000", "Test doodad", "Test.mdx", 2, true, false, false});
    state.smoke = true;
    if (show(owner, state) != IDCANCEL || !state.pending.contains("D000")) {
        throw std::runtime_error("exclusions dialog smoke test failed");
    }
}
} // namespace w3shadow::gui
