#include "ui/selector_control.hpp"

#include "ui/win32_ui_internal.hpp"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>
#include <windowsx.h>

namespace SLink::ui::selector {
namespace {

inline constexpr wchar_t kPopupClassName[] = L"SLink.SelectorPopup";
inline constexpr UINT kSetTerminalOverlayStyleMessage = WM_APP + 0x227;

struct State {
    HWND window{};
    HWND popup{};
    HFONT font{};
    std::vector<std::wstring> items;
    int selected{-1};
    int hover{-1};
    int item_height{28};
    int popup_first{};
    int popup_visible_rows{};
    bool custom_item_height{};
    bool terminal_overlay{};
};

State* GetState(HWND window) {
    return reinterpret_cast<State*>(GetWindowLongPtrW(window, GWLP_USERDATA));
}

void NotifySelection(HWND window) {
    const int id = GetDlgCtrlID(window);
    SendMessageW(GetParent(window),
                 WM_COMMAND,
                 MAKEWPARAM(static_cast<WORD>(id), CBN_SELCHANGE),
                 reinterpret_cast<LPARAM>(window));
}

COLORREF BackgroundColor(const State& state) noexcept {
    return state.terminal_overlay ? theme::TerminalBackgroundColor() : detail::kPanelAltColor;
}

COLORREF TextColor(const State& state) noexcept {
    return state.terminal_overlay ? theme::TerminalForegroundColor() : detail::kTextColor;
}

void PaintFrame(HDC dc, const RECT& rect, bool focused, const State& state) {
    HBRUSH background = CreateSolidBrush(BackgroundColor(state));
    FillRect(dc, &rect, background);
    DeleteObject(background);
    // 终端浮层依靠与画布相同的底色自然融合，不绘制矩形边框。
    if (state.terminal_overlay) {
        return;
    }
    const COLORREF border_color = focused ? detail::kAccentColor : detail::kBorderColor;
    HPEN border = CreatePen(PS_SOLID, 1, border_color);
    const HGDIOBJ old_pen = SelectObject(dc, border);
    const HGDIOBJ old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, rect.left, rect.top, rect.right, rect.bottom);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(border);
}

LRESULT CALLBACK PopupProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    State* state = GetState(window);
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        state = static_cast<State*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (!state) {
        return DefWindowProcW(window, message, wparam, lparam);
    }
    switch (message) {
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        RECT bounds{};
        GetClientRect(window, &bounds);
        PaintFrame(dc, bounds, false, *state);
        const HGDIOBJ old_font = state->font ? SelectObject(dc, state->font) : nullptr;
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, TextColor(*state));
        const int last = (std::min)(static_cast<int>(state->items.size()),
                                    state->popup_first + state->popup_visible_rows);
        for (int index = state->popup_first; index < last; ++index) {
            RECT row{1,
                     1 + static_cast<LONG>(index - state->popup_first) * state->item_height,
                     bounds.right - 1,
                     1 + static_cast<LONG>(index - state->popup_first + 1) * state->item_height};
            if (index == state->selected || index == state->hover) {
                const bool selected = index == state->selected;
                const COLORREF row_color =
                    selected ? theme::kSelectedRowColor : theme::kPanelHoverColor;
                HBRUSH brush = CreateSolidBrush(row_color);
                FillRect(dc, &row, brush);
                DeleteObject(brush);
            }
            row.left += 10;
            DrawTextW(dc,
                      state->items[index].c_str(),
                      -1,
                      &row,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        }
        if (old_font) {
            SelectObject(dc, old_font);
        }
        EndPaint(window, &paint);
        return 0;
    }
    case WM_MOUSEMOVE: {
        RECT bounds{};
        GetClientRect(window, &bounds);
        const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        const int index = state->popup_first + (point.y - 1) / std::max(1, state->item_height);
        const int next = PtInRect(&bounds, point) && index >= state->popup_first &&
                                 index < static_cast<int>(state->items.size())
                             ? index
                             : -1;
        if (next != state->hover) {
            state->hover = next;
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        RECT bounds{};
        GetClientRect(window, &bounds);
        if (PtInRect(&bounds, point)) {
            const int index = state->popup_first + (point.y - 1) / std::max(1, state->item_height);
            if (index >= 0 && index < static_cast<int>(state->items.size()) &&
                index != state->selected) {
                state->selected = index;
                NotifySelection(state->window);
                InvalidateRect(state->window, nullptr, FALSE);
            }
        }
        ReleaseCapture();
        DestroyWindow(window);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
        const int rows = (std::max)(1, std::abs(delta) / WHEEL_DELTA) * 3;
        const int maximum =
            (std::max)(0, static_cast<int>(state->items.size()) - state->popup_visible_rows);
        state->popup_first =
            std::clamp(state->popup_first + (delta > 0 ? -rows : rows), 0, maximum);
        state->hover = -1;
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (IsWindow(window)) {
            DestroyWindow(window);
        }
        return 0;
    case WM_NCDESTROY:
        if (state->popup == window) {
            state->popup = nullptr;
        }
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        return 0;
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

void ShowPopup(State& state) {
    if (state.items.empty() || state.popup) {
        return;
    }
    RECT control{};
    GetWindowRect(state.window, &control);
    if (!state.custom_item_height) {
        state.item_height = detail::ScaleForDpi(28, detail::WindowDpi(state.window));
    }
    HMONITOR monitor = MonitorFromRect(&control, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{sizeof(info)};
    GetMonitorInfoW(monitor, &info);
    const int below = (std::max)(0, static_cast<int>(info.rcWork.bottom - control.bottom));
    const int above = (std::max)(0, static_cast<int>(control.top - info.rcWork.top));
    const int usable = (std::max)(below, above);
    const int rows_that_fit = (std::max)(1, (usable - 2) / state.item_height);
    state.popup_visible_rows = (std::min)({8, static_cast<int>(state.items.size()), rows_that_fit});
    const int height = state.item_height * state.popup_visible_rows + 2;
    const bool open_below = below >= height || below >= above;
    const int top = std::clamp(open_below ? static_cast<int>(control.bottom)
                                          : static_cast<int>(control.top) - height,
                               static_cast<int>(info.rcWork.top),
                               (std::max)(static_cast<int>(info.rcWork.top),
                                          static_cast<int>(info.rcWork.bottom) - height));
    state.popup_first = std::clamp(state.selected - state.popup_visible_rows / 2,
                                   0,
                                   static_cast<int>(state.items.size()) - state.popup_visible_rows);
    state.popup = CreateWindowExW(
        WS_EX_TOOLWINDOW,
        kPopupClassName,
        L"",
        WS_POPUP,
        control.left,
        top,
        control.right - control.left,
        height,
        GetAncestor(state.window, GA_ROOT),
        nullptr,
        reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(state.window, GWLP_HINSTANCE)),
        &state);
    if (!state.popup) {
        return;
    }
    ShowWindow(state.popup, SW_SHOWNOACTIVATE);
    UpdateWindow(state.popup);
    SetCapture(state.popup);
}

LRESULT CALLBACK SelectorProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    State* state = GetState(window);
    if (message == WM_NCCREATE) {
        state = new State{};
        state->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (!state) {
        return DefWindowProcW(window, message, wparam, lparam);
    }
    switch (message) {
    case CB_ADDSTRING:
        state->items.emplace_back(reinterpret_cast<const wchar_t*>(lparam));
        InvalidateRect(window, nullptr, FALSE);
        return static_cast<LRESULT>(state->items.size() - 1);
    case CB_RESETCONTENT:
        state->items.clear();
        state->selected = -1;
        InvalidateRect(window, nullptr, FALSE);
        return CB_OKAY;
    case CB_GETCOUNT:
        return static_cast<LRESULT>(state->items.size());
    case CB_GETCURSEL:
        return state->selected < 0 ? CB_ERR : state->selected;
    case CB_SETCURSEL:
        if (static_cast<std::size_t>(wparam) >= state->items.size()) {
            state->selected = -1;
            return CB_ERR;
        }
        state->selected = static_cast<int>(wparam);
        InvalidateRect(window, nullptr, FALSE);
        return state->selected;
    case CB_GETLBTEXTLEN:
        return static_cast<std::size_t>(wparam) < state->items.size()
                   ? static_cast<LRESULT>(state->items[wparam].size())
                   : CB_ERR;
    case CB_GETLBTEXT:
        if (static_cast<std::size_t>(wparam) >= state->items.size()) {
            return CB_ERR;
        }
        std::copy(state->items[wparam].begin(),
                  state->items[wparam].end(),
                  reinterpret_cast<wchar_t*>(lparam));
        reinterpret_cast<wchar_t*>(lparam)[state->items[wparam].size()] = L'\0';
        return static_cast<LRESULT>(state->items[wparam].size());
    case CB_SETITEMHEIGHT:
        state->item_height = static_cast<int>(lparam);
        state->custom_item_height = true;
        return CB_OKAY;
    case kSetTerminalOverlayStyleMessage:
        state->terminal_overlay = wparam != 0;
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_SETFONT:
        state->font = reinterpret_cast<HFONT>(wparam);
        if (lparam) {
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case WM_GETFONT:
        return reinterpret_cast<LRESULT>(state->font);
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS | DLGC_WANTCHARS;
    case WM_LBUTTONDOWN:
        SetFocus(window);
        // 不要在按下阶段创建并捕获弹层。否则用于打开选择器的同一次
        // WM_LBUTTONUP 会直接送到弹层并把它关闭，用户只能靠一直按住鼠标查看内容。
        // 在完整单击结束后再展开，认证方式和冲突策略选择器可正常单击使用。
        SetCapture(window);
        return 0;
    case WM_LBUTTONUP: {
        RECT bounds{};
        GetClientRect(window, &bounds);
        const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        const bool open = GetCapture() == window && PtInRect(&bounds, point);
        if (GetCapture() == window) {
            ReleaseCapture();
        }
        if (open) {
            ShowPopup(*state);
        }
        return 0;
    }
    case WM_KEYDOWN:
        if (wparam == VK_F4 || wparam == VK_SPACE || wparam == VK_RETURN) {
            ShowPopup(*state);
            return 0;
        }
        if ((wparam == VK_UP || wparam == VK_DOWN) && !state->items.empty()) {
            const int delta = wparam == VK_UP ? -1 : 1;
            const int next =
                std::clamp(state->selected + delta, 0, static_cast<int>(state->items.size()) - 1);
            if (next != state->selected) {
                state->selected = next;
                if (state->popup) {
                    if (next < state->popup_first) {
                        state->popup_first = next;
                    } else if (next >= state->popup_first + state->popup_visible_rows) {
                        state->popup_first = next - state->popup_visible_rows + 1;
                    }
                    InvalidateRect(state->popup, nullptr, FALSE);
                }
                NotifySelection(window);
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;
        }
        break;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_ENABLE:
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        RECT bounds{};
        GetClientRect(window, &bounds);
        PaintFrame(dc, bounds, GetFocus() == window, *state);
        const HGDIOBJ old_font = state->font ? SelectObject(dc, state->font) : nullptr;
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, IsWindowEnabled(window) ? TextColor(*state) : detail::kMutedTextColor);
        RECT text = bounds;
        text.left += 11;
        text.right -= 32;
        if (state->selected >= 0 && state->selected < static_cast<int>(state->items.size())) {
            DrawTextW(dc,
                      state->items[state->selected].c_str(),
                      -1,
                      &text,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        } else {
            wchar_t placeholder[64]{};
            if (GetWindowTextW(window, placeholder, 64) > 0) {
                DrawTextW(dc,
                          placeholder,
                          -1,
                          &text,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            }
        }
        const int center_x = bounds.right - 17;
        const int center_y = (bounds.top + bounds.bottom) / 2;
        HPEN pen = CreatePen(PS_SOLID, 1, detail::kMutedTextColor);
        const HGDIOBJ old_pen = SelectObject(dc, pen);
        MoveToEx(dc, center_x - 4, center_y - 2, nullptr);
        LineTo(dc, center_x, center_y + 2);
        LineTo(dc, center_x + 4, center_y - 2);
        SelectObject(dc, old_pen);
        DeleteObject(pen);
        if (old_font) {
            SelectObject(dc, old_font);
        }
        EndPaint(window, &paint);
        return 0;
    }
    case WM_NCDESTROY:
        if (state->popup) {
            DestroyWindow(state->popup);
        }
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        delete state;
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

} // namespace

bool RegisterClass(HINSTANCE instance) {
    WNDCLASSEXW selector{sizeof(selector)};
    selector.lpfnWndProc = SelectorProc;
    selector.hInstance = instance;
    selector.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    selector.lpszClassName = kClassName;
    if (!RegisterClassExW(&selector) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }
    WNDCLASSEXW popup{sizeof(popup)};
    popup.lpfnWndProc = PopupProc;
    popup.hInstance = instance;
    popup.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    popup.lpszClassName = kPopupClassName;
    return RegisterClassExW(&popup) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

void SetTerminalOverlayStyle(const HWND window, const bool enabled) {
    if (window) {
        SendMessageW(window, kSetTerminalOverlayStyleMessage, enabled ? 1 : 0, 0);
    }
}

} // namespace SLink::ui::selector
