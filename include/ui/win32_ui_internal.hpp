#pragma once

#include "ui/theme.hpp"

#include <windows.h>
#include <algorithm>
#include <commctrl.h>
#include <dwmapi.h>
#include <filesystem>
#include <string>
#include <string_view>
#include <uxtheme.h>

#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "Dwmapi.lib")
#pragma comment(lib, "UxTheme.lib")

namespace SLink::ui::detail {

// Win32 自绘控件的内部公共工具：DPI 缩放、主题刷、模态窗口 z-order、输入框/组合框
// 边框以及 owner-draw 按钮。这里只保留视觉和窗口辅助逻辑，不承载业务状态。
inline COLORREF& kWindowColor = theme::kWindowColor;
inline COLORREF& kPanelColor = theme::kPanelColor;
inline COLORREF& kPanelAltColor = theme::kPanelAltColor;
inline COLORREF& kBorderColor = theme::kBorderColor;
inline COLORREF& kTextColor = theme::kTextColor;
inline COLORREF& kMutedTextColor = theme::kMutedTextColor;
inline COLORREF& kAccentColor = theme::kAccentColor;

inline int ScaleForDpi(int value, UINT dpi) {
    return MulDiv(value, static_cast<int>(dpi), 96);
}

inline UINT WindowDpi(HWND hwnd) {
    using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
    static auto get_dpi = reinterpret_cast<GetDpiForWindowFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    return get_dpi ? get_dpi(hwnd) : 96;
}

inline void EnableDarkTitleBar(HWND hwnd) {
    BOOL enabled = theme::IsDark() ? TRUE : FALSE;
    constexpr DWORD kDwmUseImmersiveDarkMode = 20;
    if (FAILED(DwmSetWindowAttribute(hwnd, kDwmUseImmersiveDarkMode, &enabled, sizeof(enabled)))) {
        constexpr DWORD kLegacyDwmUseImmersiveDarkMode = 19;
        DwmSetWindowAttribute(hwnd, kLegacyDwmUseImmersiveDarkMode, &enabled, sizeof(enabled));
    }
}

inline HWND RootWindowForModalOwner(const HWND candidate) {
    if (!candidate || !IsWindow(candidate))
        return candidate;
    const HWND root = GetAncestor(candidate, GA_ROOT);
    return root && IsWindow(root) ? root : candidate;
}

inline void RaiseOwnedWindowGroup(const HWND dialog) {
    if (!dialog || !IsWindow(dialog))
        return;
    const HWND root_owner = GetAncestor(dialog, GA_ROOTOWNER);
    if (!root_owner || root_owner == dialog || !IsWindow(root_owner))
        return;
    // This only adjusts the ordinary z-order. It deliberately does not use
    // HWND_TOPMOST, so the application remains in front after the user
    // activates it but does not become permanently always-on-top.
    SetWindowPos(root_owner, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

inline HWND PrepareModalWindowClose(const HWND dialog) {
    if (!dialog || !IsWindow(dialog))
        return nullptr;
    const HWND owner = GetWindow(dialog, GW_OWNER);
    if (owner && IsWindow(owner)) {
        if (!IsWindowEnabled(owner))
            EnableWindow(owner, TRUE);
        RaiseOwnedWindowGroup(dialog);
    }
    return owner;
}

inline void ActivateModalOwner(const HWND owner) {
    if (!owner || !IsWindow(owner))
        return;
    if (!IsWindowEnabled(owner))
        EnableWindow(owner, TRUE);
    const HWND root = RootWindowForModalOwner(owner);
    SetWindowPos(root, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetActiveWindow(owner);
}

inline void PaintSelfDrawnFrame(HWND hwnd) {
    const HDC dc = GetWindowDC(hwnd);
    if (!dc) {
        return;
    }
    RECT window_rect{};
    RECT client_rect{};
    GetWindowRect(hwnd, &window_rect);
    GetClientRect(hwnd, &client_rect);
    MapWindowPoints(hwnd, nullptr, reinterpret_cast<POINT*>(&client_rect), 2);
    OffsetRect(&client_rect, -window_rect.left, -window_rect.top);
    const int width = window_rect.right - window_rect.left;
    const int height = window_rect.bottom - window_rect.top;
    window_rect = {0, 0, width, height};
    const COLORREF color = GetFocus() == hwnd ? kAccentColor : kBorderColor;
    const HBRUSH border = CreateSolidBrush(color);
    // Cover the complete non-client strip. FrameRect only paints its outermost
    // pixel, leaving the native light inner edge visible on some Win10/Win11
    // builds and DPI combinations.
    RECT strip{0, 0, width, std::max(0L, client_rect.top)};
    FillRect(dc, &strip, border);
    strip = {0, std::min<LONG>(height, client_rect.bottom), width, height};
    FillRect(dc, &strip, border);
    strip = {0,
             std::max(0L, client_rect.top),
             std::max(0L, client_rect.left),
             std::min<LONG>(height, client_rect.bottom)};
    FillRect(dc, &strip, border);
    strip = {std::min<LONG>(width, client_rect.right),
             std::max(0L, client_rect.top),
             width,
             std::min<LONG>(height, client_rect.bottom)};
    FillRect(dc, &strip, border);
    DeleteObject(border);
    ReleaseDC(hwnd, dc);
}

inline LRESULT CALLBACK SelfDrawnVisualSubclassProc(HWND hwnd,
                                                    UINT message,
                                                    WPARAM wparam,
                                                    LPARAM lparam,
                                                    UINT_PTR subclass_id,
                                                    DWORD_PTR reference) {
    const bool combo = reference != 0;
    switch (message) {
    case WM_NCPAINT: {
        const LRESULT result = DefSubclassProc(hwnd, message, wparam, lparam);
        PaintSelfDrawnFrame(hwnd);
        return result;
    }
    case WM_PAINT: {
        const LRESULT result = DefSubclassProc(hwnd, message, wparam, lparam);
        if (combo) {
            const HDC dc = GetDC(hwnd);
            if (dc) {
                RECT client{};
                GetClientRect(hwnd, &client);
                const HBRUSH client_background = CreateSolidBrush(kPanelAltColor);
                FillRect(dc, &client, client_background);
                DeleteObject(client_background);
                const int arrow_width = ScaleForDpi(24, WindowDpi(hwnd));
                RECT arrow{std::max(client.left, client.right - arrow_width),
                           client.top,
                           client.right,
                           client.bottom};
                const LRESULT selected = SendMessageW(hwnd, CB_GETCURSEL, 0, 0);
                if (selected != CB_ERR) {
                    const LRESULT length = SendMessageW(hwnd, CB_GETLBTEXTLEN, selected, 0);
                    std::wstring text(static_cast<std::size_t>(std::max<LRESULT>(0, length)) + 1,
                                      L'\0');
                    SendMessageW(
                        hwnd, CB_GETLBTEXT, selected, reinterpret_cast<LPARAM>(text.data()));
                    RECT text_rect = client;
                    text_rect.left += ScaleForDpi(8, WindowDpi(hwnd));
                    text_rect.right = arrow.left - ScaleForDpi(4, WindowDpi(hwnd));
                    const HGDIOBJ old_font = SelectObject(
                        dc, reinterpret_cast<HFONT>(SendMessageW(hwnd, WM_GETFONT, 0, 0)));
                    SetBkMode(dc, TRANSPARENT);
                    SetTextColor(dc, kTextColor);
                    DrawTextW(dc,
                              text.c_str(),
                              -1,
                              &text_rect,
                              DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
                    if (old_font) {
                        SelectObject(dc, old_font);
                    }
                }
                const int cx = (arrow.left + arrow.right) / 2;
                const int cy = (arrow.top + arrow.bottom) / 2;
                const HPEN pen = CreatePen(PS_SOLID, 1, kTextColor);
                const HGDIOBJ old_pen = SelectObject(dc, pen);
                MoveToEx(dc, cx - 4, cy - 2, nullptr);
                LineTo(dc, cx, cy + 2);
                LineTo(dc, cx + 4, cy - 2);
                SelectObject(dc, old_pen);
                DeleteObject(pen);
                const COLORREF frame_color = GetFocus() == hwnd ? kAccentColor : kBorderColor;
                const HBRUSH frame = CreateSolidBrush(frame_color);
                FrameRect(dc, &client, frame);
                DeleteObject(frame);
                ReleaseDC(hwnd, dc);
            }
        }
        return result;
    }
    case WM_SETFOCUS:
    case WM_KILLFOCUS: {
        const LRESULT result = DefSubclassProc(hwnd, message, wparam, lparam);
        RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME | RDW_UPDATENOW);
        return result;
    }
    case WM_THEMECHANGED:
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, &SelfDrawnVisualSubclassProc, subclass_id);
        return DefSubclassProc(hwnd, message, wparam, lparam);
    default:
        return DefSubclassProc(hwnd, message, wparam, lparam);
    }
}

inline void ApplySelfDrawnTheme(HWND hwnd) {
    if (!hwnd) {
        return;
    }
    // Keep the native input/IME/accessibility engine, but detach the OS visual
    // style. The application paints every visible color, border and item state.
    SetWindowTheme(hwnd, L"", L"");
    wchar_t class_name[64]{};
    GetClassNameW(hwnd, class_name, static_cast<int>(std::size(class_name)));
    const bool edit = _wcsicmp(class_name, L"EDIT") == 0;
    const bool combo = _wcsicmp(class_name, WC_COMBOBOXW) == 0;
    if (edit || combo) {
        LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
        LONG_PTR extended = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        const bool had_frame =
            combo || (style & WS_BORDER) != 0 || (extended & WS_EX_CLIENTEDGE) != 0;
        if ((extended & WS_EX_CLIENTEDGE) != 0) {
            extended &= ~static_cast<LONG_PTR>(WS_EX_CLIENTEDGE);
            SetWindowLongPtrW(hwnd, GWL_EXSTYLE, extended);
        }
        if (had_frame) {
            // A dropdown-list combo paints part of WS_BORDER itself during
            // WM_PAINT, after WM_NCPAINT. On some Windows versions that leaves
            // a light right/bottom edge. Keep EDIT's native layout border, but
            // make COMBOBOX borderless and paint all four client edges above.
            if (combo) {
                style &= ~static_cast<LONG_PTR>(WS_BORDER);
            } else {
                style |= WS_BORDER;
            }
            SetWindowLongPtrW(hwnd, GWL_STYLE, style);
            SetWindowPos(hwnd,
                         nullptr,
                         0,
                         0,
                         0,
                         0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                             SWP_FRAMECHANGED);
            SetWindowSubclass(hwnd, &SelfDrawnVisualSubclassProc, 0x5344, combo ? 1 : 0);
        }
    }
}

inline HBRUSH WindowThemeBrush() {
    static HBRUSH dark = CreateSolidBrush(theme::kDarkPalette.window);
    static HBRUSH light = CreateSolidBrush(theme::kLightPalette.window);
    return theme::IsDark() ? dark : light;
}

inline HBRUSH PanelThemeBrush() {
    static HBRUSH dark = CreateSolidBrush(theme::kDarkPalette.panel);
    static HBRUSH light = CreateSolidBrush(theme::kLightPalette.panel);
    return theme::IsDark() ? dark : light;
}

inline HBRUSH PanelAltThemeBrush() {
    static HBRUSH dark = CreateSolidBrush(theme::kDarkPalette.panel_alt);
    static HBRUSH light = CreateSolidBrush(theme::kLightPalette.panel_alt);
    return theme::IsDark() ? dark : light;
}

inline constexpr wchar_t kSelfDrawnCheckedProperty[] = L"SLink.SelfDrawnChecked";

inline bool SelfDrawnCheckboxChecked(const HWND button) {
    return button && GetPropW(button, kSelfDrawnCheckedProperty) != nullptr;
}

inline void SetSelfDrawnCheckboxChecked(const HWND button, const bool checked) {
    if (!button)
        return;
    if (checked) {
        SetPropW(
            button, kSelfDrawnCheckedProperty, reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(1)));
    } else {
        RemovePropW(button, kSelfDrawnCheckedProperty);
    }
    InvalidateRect(button, nullptr, FALSE);
}

inline bool DrawSelfDrawnButton(const DRAWITEMSTRUCT& draw,
                                bool primary = false,
                                bool checkbox_override = false) {
    if (!draw.hDC || draw.CtlType != ODT_BUTTON) {
        return false;
    }
    const LONG_PTR style = GetWindowLongPtrW(draw.hwndItem, GWL_STYLE);
    const UINT kind = static_cast<UINT>(style) & BS_TYPEMASK;
    const bool checkbox = checkbox_override || kind == BS_CHECKBOX || kind == BS_AUTOCHECKBOX ||
                          kind == BS_3STATE || kind == BS_AUTO3STATE;
    const bool disabled = (draw.itemState & ODS_DISABLED) != 0;
    const bool pressed = (draw.itemState & ODS_SELECTED) != 0;
    const HBRUSH background = CreateSolidBrush(kPanelColor);
    FillRect(draw.hDC, &draw.rcItem, background);
    DeleteObject(background);
    wchar_t text[256]{};
    GetWindowTextW(draw.hwndItem, text, static_cast<int>(std::size(text)));
    RECT text_rect = draw.rcItem;
    if (checkbox) {
        const int box = ScaleForDpi(15, WindowDpi(draw.hwndItem));
        RECT check{draw.rcItem.left,
                   draw.rcItem.top + (draw.rcItem.bottom - draw.rcItem.top - box) / 2,
                   draw.rcItem.left + box,
                   draw.rcItem.top + (draw.rcItem.bottom - draw.rcItem.top + box) / 2};
        const HBRUSH fill = CreateSolidBrush(kPanelAltColor);
        const HPEN border = CreatePen(PS_SOLID, 1, kBorderColor);
        const HGDIOBJ old_brush = SelectObject(draw.hDC, fill);
        const HGDIOBJ old_pen = SelectObject(draw.hDC, border);
        Rectangle(draw.hDC, check.left, check.top, check.right, check.bottom);
        const bool checked = checkbox_override
                                 ? SelfDrawnCheckboxChecked(draw.hwndItem)
                                 : SendMessageW(draw.hwndItem, BM_GETCHECK, 0, 0) == BST_CHECKED;
        if (checked) {
            const HPEN mark = CreatePen(PS_SOLID, 2, kAccentColor);
            SelectObject(draw.hDC, mark);
            MoveToEx(draw.hDC, check.left + 3, check.top + box / 2, nullptr);
            LineTo(draw.hDC, check.left + box / 2 - 1, check.bottom - 4);
            LineTo(draw.hDC, check.right - 3, check.top + 3);
            SelectObject(draw.hDC, border);
            DeleteObject(mark);
        }
        SelectObject(draw.hDC, old_pen);
        SelectObject(draw.hDC, old_brush);
        DeleteObject(border);
        DeleteObject(fill);
        text_rect.left = check.right + ScaleForDpi(7, WindowDpi(draw.hwndItem));
    } else {
                const COLORREF fill_color = disabled ? kPanelColor
                                     : primary
                                         ? (pressed ? (theme::IsDark() ? RGB(105, 131, 192)
                                                                       : RGB(58, 84, 145))
                                                    : kAccentColor)
                                         : (pressed ? theme::kPanelHoverColor : kPanelAltColor);
        const HBRUSH fill = CreateSolidBrush(fill_color);
        const HPEN border =
            CreatePen(PS_SOLID, 1, primary && !disabled ? kAccentColor : kBorderColor);
        const HGDIOBJ old_brush = SelectObject(draw.hDC, fill);
        const HGDIOBJ old_pen = SelectObject(draw.hDC, border);
        RoundRect(draw.hDC,
                  draw.rcItem.left,
                  draw.rcItem.top,
                  draw.rcItem.right,
                  draw.rcItem.bottom,
                  ScaleForDpi(7, WindowDpi(draw.hwndItem)),
                  ScaleForDpi(7, WindowDpi(draw.hwndItem)));
        SelectObject(draw.hDC, old_pen);
        SelectObject(draw.hDC, old_brush);
        DeleteObject(border);
        DeleteObject(fill);
    }
    const HGDIOBJ old_font = SelectObject(
        draw.hDC, reinterpret_cast<HFONT>(SendMessageW(draw.hwndItem, WM_GETFONT, 0, 0)));
    SetBkMode(draw.hDC, TRANSPARENT);
    SetTextColor(draw.hDC,
                 disabled ? kMutedTextColor
                 : primary ? (theme::IsDark() ? kWindowColor : RGB(255, 255, 255))
                           : kTextColor);
    DrawTextW(draw.hDC,
              text,
              -1,
              &text_rect,
              DT_SINGLELINE | DT_VCENTER | (checkbox ? DT_LEFT : DT_CENTER) | DT_NOPREFIX);
    if (old_font) {
        SelectObject(draw.hDC, old_font);
    }
    return true;
}

inline bool DrawSelfDrawnComboItem(const DRAWITEMSTRUCT& draw) {
    if (!draw.hDC || draw.CtlType != ODT_COMBOBOX) {
        return false;
    }
    const bool selected = (draw.itemState & ODS_SELECTED) != 0;
    const HBRUSH background =
        CreateSolidBrush(selected ? theme::kSelectedRowColor : kPanelAltColor);
    FillRect(draw.hDC, &draw.rcItem, background);
    DeleteObject(background);
    if (draw.itemID != static_cast<UINT>(-1)) {
        const LRESULT length = SendMessageW(draw.hwndItem, CB_GETLBTEXTLEN, draw.itemID, 0);
        std::wstring text(static_cast<std::size_t>(std::max<LRESULT>(0, length)) + 1, L'\0');
        SendMessageW(
            draw.hwndItem, CB_GETLBTEXT, draw.itemID, reinterpret_cast<LPARAM>(text.data()));
        RECT text_rect = draw.rcItem;
        text_rect.left += ScaleForDpi(8, WindowDpi(draw.hwndItem));
        text_rect.right -= ScaleForDpi(26, WindowDpi(draw.hwndItem));
        const HGDIOBJ old_font = SelectObject(
            draw.hDC, reinterpret_cast<HFONT>(SendMessageW(draw.hwndItem, WM_GETFONT, 0, 0)));
        SetBkMode(draw.hDC, TRANSPARENT);
        SetTextColor(draw.hDC, kTextColor);
        DrawTextW(draw.hDC,
                  text.c_str(),
                  -1,
                  &text_rect,
                  DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        if (old_font) {
            SelectObject(draw.hDC, old_font);
        }
    }
    return true;
}

inline void ApplyThemeToControl(HWND control) {
    if (!control || !IsWindow(control)) {
        return;
    }
    wchar_t class_name[64]{};
    GetClassNameW(control, class_name, static_cast<int>(std::size(class_name)));
    if (_wcsicmp(class_name, L"SLink.TableControl") == 0) {
        SendMessageW(control, WM_THEMECHANGED, 0, 0);
        InvalidateRect(control, nullptr, TRUE);
        return;
    }
    ApplySelfDrawnTheme(control);
    SendMessageW(control, WM_THEMECHANGED, 0, 0);
    InvalidateRect(control, nullptr, TRUE);
}

inline void ApplyThemeToWindowTree(HWND root) {
    if (!root || !IsWindow(root)) {
        return;
    }
    ApplyThemeToControl(root);
    EnumChildWindows(
        root,
        [](HWND child, LPARAM) -> BOOL {
            ApplyThemeToControl(child);
            return TRUE;
        },
        0);
    RedrawWindow(root, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

inline HFONT CreateUiFont(UINT dpi, int point_size = 10, bool semibold = false) {
    LOGFONTW font{};
    font.lfHeight = -MulDiv(point_size, static_cast<int>(dpi), 72);
    font.lfWeight = semibold ? FW_SEMIBOLD : FW_NORMAL;
    font.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(font.lfFaceName, L"Segoe UI");
    return CreateFontIndirectW(&font);
}

inline void SetControlFont(HWND hwnd, HFONT font) {
    SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

inline void SetRoundedControlRegion(HWND hwnd, int width, int height, int radius) {
    if (!hwnd || width <= 0 || height <= 0) {
        return;
    }
    HRGN region =
        CreateRoundRectRgn(0, 0, width + 1, height + 1, std::max(2, radius), std::max(2, radius));
    if (region && SetWindowRgn(hwnd, region, TRUE) == 0) {
        DeleteObject(region);
    }
}

inline std::filesystem::path ExecutableDirectory() {
    std::wstring buffer(32768, L'\0');
    const DWORD length =
        GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    buffer.resize(length);
    return std::filesystem::path(buffer).parent_path();
}

inline std::wstring FormatFileSize(std::uintmax_t bytes) {
    wchar_t buffer[64]{};
    if (bytes >= 1024ULL * 1024ULL * 1024ULL) {
        swprintf_s(buffer, L"%.1f GB", static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
    } else if (bytes >= 1024ULL * 1024ULL) {
        swprintf_s(buffer, L"%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    } else if (bytes >= 1024ULL) {
        swprintf_s(buffer, L"%.1f KB", static_cast<double>(bytes) / 1024.0);
    } else {
        swprintf_s(buffer, L"%llu B", static_cast<unsigned long long>(bytes));
    }
    return buffer;
}

inline LRESULT PaintDarkControl(HWND hwnd, HDC dc) {
    RECT rect{};
    GetClientRect(hwnd, &rect);
    HBRUSH brush = CreateSolidBrush(kPanelColor);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
    SetTextColor(dc, kTextColor);
    SetBkColor(dc, kPanelColor);
    return 0;
}

} // namespace SLink::ui::detail
