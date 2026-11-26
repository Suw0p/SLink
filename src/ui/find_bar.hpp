#pragma once

#include "ui/retained_ui.hpp"
#include "ui/win32_ui_internal.hpp"

#include <functional>
#include <utility>
#include <windowsx.h>

namespace SLink::ui::detail {

// 两个页面共用有界、非模态的查找条。只保存查询条件，不接触文件、SSH 或正文。
// 按钮仍使用现有自绘层，原生 EDIT 负责 Unicode、IME 和剪贴板输入。
class FindBar final {
public:
    enum class Action {
        Next,
        Previous,
        Replace,
        ReplaceAll,
        Changed,
        Closed
    };
    std::function<void(Action)> action;

    bool Create(HWND parent, HINSTANCE instance, bool allow_replace) {
        destroying_ = false;
        allow_replace_ = allow_replace;
        WNDCLASSW wc{};
        wc.lpfnWndProc = WindowProc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"SLink.FindBar";
        if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return false;
        window_ = CreateWindowExW(0,
                                  wc.lpszClassName,
                                  L"查找",
                                  WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                                  0,
                                  0,
                                  0,
                                  0,
                                  parent,
                                  nullptr,
                                  instance,
                                  this);
        if (!window_ || !surface_.Initialize(window_)) {
            AbortCreate();
            return false;
        }
        for (int id : {1, 2}) {
            HWND edit = CreateWindowExW(0,
                                        L"EDIT",
                                        L"",
                                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                        0,
                                        0,
                                        0,
                                        0,
                                        window_,
                                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                        instance,
                                        nullptr);
            if (!edit) {
                AbortCreate();
                return false;
            }
            (id == 1 ? query_ : replacement_) = edit;
            SendMessageW(edit, EM_SETLIMITTEXT, 4096, 0);
            SendMessageW(
                edit, EM_SETCUEBANNER, 0, reinterpret_cast<LPARAM>(id == 1 ? L"查找" : L"替换为"));
            SetWindowSubclass(edit, EditProc, 1, reinterpret_cast<DWORD_PTR>(this));
            ApplySelfDrawnTheme(edit);
        }
        ApplyTheme();
        return true;
    }

    void Show(bool replace, HWND return_focus) {
        const bool changed = !visible_ || (allow_replace_ && replace && !replace_);
        if (!visible_ || replace)
            replace_ = allow_replace_ && replace;
        return_focus_ = return_focus;
        visible_ = true;
        const bool had_error = std::exchange(error_, false);
        if (changed && action)
            action(Action::Changed); // 宿主据此重新放置查找条。
        else if (changed)
            ShowWindow(window_, SW_SHOWNOACTIVATE);
        else if (had_error)
            Layout(false);
        if (GetFocus() != query_)
            SetFocus(query_);
        SendMessageW(query_, EM_SETSEL, 0, -1);
    }
    void Hide(bool restore_focus = true) {
        visible_ = false;
        ShowWindow(window_, SW_HIDE);
        if (restore_focus && return_focus_ && IsWindow(return_focus_))
            SetFocus(return_focus_);
        if (action)
            action(Action::Closed);
    }
    bool visible() const noexcept {
        return visible_;
    }
    bool match_case() const noexcept {
        return case_;
    }
    bool regex() const noexcept {
        return regex_ && regex_enabled_;
    }
    void EnableRegex(bool enabled) {
        if (std::exchange(regex_enabled_, enabled) != enabled)
            Layout(false);
    }
    HWND hwnd() const noexcept {
        return window_;
    }
    std::wstring query() const {
        return Text(query_);
    }
    std::wstring replacement() const {
        return Text(replacement_);
    }
    void Status(std::wstring, bool error = false) {
        if (std::exchange(error_, error) != error)
            Layout(false);
    }
    int height() const {
        return ScaleForDpi(replace_ ? 72 : 40, WindowDpi(window_));
    }
    void Place(int left, int top, int width) {
        if (!window_ || !visible_)
            return;
        RECT previous{};
        GetWindowRect(window_, &previous);
        MapWindowPoints(HWND_DESKTOP, GetParent(window_), reinterpret_cast<POINT*>(&previous), 2);
        const RECT current{left, top, left + width, top + height()};
        const bool changed = !EqualRect(&previous, &current) || !IsWindowVisible(window_);
        if (!changed && region_size_.cx == width && region_size_.cy == height())
            return;
        placing_ = true;
        SetWindowPos(window_,
                     HWND_TOP,
                     left,
                     top,
                     std::max(0, width),
                     height(),
                     SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOREDRAW);
        if (region_size_.cx != width || region_size_.cy != height()) {
            const int radius = ScaleForDpi(14, WindowDpi(window_));
            HRGN region = CreateRoundRectRgn(0, 0, width + 1, height() + 1, radius, radius);
            if (region && !SetWindowRgn(window_, region, FALSE))
                DeleteObject(region);
            region_size_ = {width, height()};
        }
        Layout();
        placing_ = false;
        RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_NOERASE);
        if (!EqualRect(&previous, &current) && !IsRectEmpty(&previous))
            RedrawWindow(GetParent(window_),
                         &previous,
                         nullptr,
                         RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_NOERASE);
    }
    void ApplyTheme() {
        if (!window_)
            return;
        const auto dpi = WindowDpi(window_);
        HFONT next = CreateFontW(-ScaleForDpi(14, dpi),
                                 0,
                                 0,
                                 0,
                                 FW_NORMAL,
                                 FALSE,
                                 FALSE,
                                 FALSE,
                                 DEFAULT_CHARSET,
                                 0,
                                 0,
                                 CLEARTYPE_QUALITY,
                                 0,
                                 L"Segoe UI");
        for (HWND edit : {query_, replacement_}) {
            ApplySelfDrawnTheme(edit);
            SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(next), TRUE);
        }
        if (font_)
            DeleteObject(font_);
        font_ = next;
        surface_.DpiChanged();
        Layout();
    }
    ~FindBar() {
        destroying_ = true;
        if (window_ && IsWindow(window_))
            DestroyWindow(window_);
        if (font_)
            DeleteObject(font_);
    }

private:
    void AbortCreate() {
        if (window_)
            DestroyWindow(window_);
        window_ = query_ = replacement_ = nullptr;
    }
    HWND window_{}, query_{}, replacement_{}, return_focus_{};
    HFONT font_{};
    SIZE region_size_{};
    UiSurface surface_;
    bool allow_replace_{}, replace_{}, visible_{}, case_{}, regex_{};
    bool regex_enabled_{true};
    bool error_{};
    bool destroying_{};
    bool placing_{};

    static std::wstring Text(HWND edit) {
        std::wstring text(static_cast<std::size_t>(GetWindowTextLengthW(edit)) + 1, L'\0');
        text.resize(static_cast<std::size_t>(
            GetWindowTextW(edit, text.data(), static_cast<int>(text.size()))));
        return text;
    }
    void Invoke(Action value) {
        if (action)
            action(value);
    }
    void PositionInput(HWND control, int left, int top, int width, int height) {
        RECT previous{};
        GetWindowRect(control, &previous);
        MapWindowPoints(HWND_DESKTOP, window_, reinterpret_cast<POINT*>(&previous), 2);
        const RECT desired{left, top, left + width, top + height};
        if (!EqualRect(&previous, &desired)) {
            MoveWindow(control, left, top, width, height, FALSE);
            InvalidateRect(control, nullptr, FALSE);
        }
    }
    void Layout(bool update_controls = true) {
        if (!query_ || !window_ || destroying_)
            return;
        RECT rc{};
        GetClientRect(window_, &rc);
        const float scale = WindowDpi(window_) / 96.0F;
        const float width = rc.right / scale;
        // 窄分屏只收起上下箭头，大小写/正则状态仍可见，不隐藏已经启用的选项。
        const bool compact = width < 200;
        const float input_left = allow_replace_ ? 24.0F : 6.0F;
        const float end = std::max(input_left + 24, width - 8 - (compact ? 24 : 72));
        const float option_width = allow_replace_ ? 48.0F : 24.0F;
        const float text_end = std::max(input_left + 10, end - option_width - 2);
        const float replace_end = std::max(input_left + 20, std::min(end, width - 56));
        if (update_controls) {
            PositionInput(query_,
                          static_cast<int>((input_left + 6) * scale),
                          static_cast<int>(10 * scale),
                          std::max(1, static_cast<int>((text_end - input_left - 8) * scale)),
                          static_cast<int>(20 * scale));
            // 搜索和替换输入框始终一起创建、预布局。展开只提交高度和可见性，
            // 不在旧客户区先绘一遍替换区，也不触发子控件同步擦背景。
            PositionInput(replacement_,
                          static_cast<int>((input_left + 6) * scale),
                          static_cast<int>(42 * scale),
                          std::max(1, static_cast<int>((replace_end - input_left - 12) * scale)),
                          static_cast<int>(20 * scale));
            const bool replacement_visible =
                (GetWindowLongPtrW(replacement_, GWL_STYLE) & WS_VISIBLE) != 0;
            if (replacement_visible != replace_)
                SetWindowPos(replacement_,
                             nullptr,
                             0,
                             0,
                             0,
                             0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                                 SWP_NOREDRAW | (replace_ ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
        }
        surface_.ClearElements();
        surface_.Add(
            {.role = UiRole::card,
             .bounds = UiRect(scale, scale, (width - 1) * scale, (replace_ ? 71 : 39) * scale)});
        surface_.Add({.role = UiRole::input_field,
                      .bounds = UiRect(input_left * scale, 6 * scale, end * scale, 34 * scale),
                      .selected = GetFocus() == query_,
                      .error = error_});
        const auto button = [&](int id,
                                float x,
                                float y,
                                float w,
                                std::wstring text,
                                bool selected = false,
                                UiIcon icon = UiIcon::none) -> UiElement& {
            return surface_.Add(
                {.id = id,
                 .role = UiRole::search_button,
                 .bounds = UiRect(x * scale, y * scale, (x + w) * scale, (y + 26) * scale),
                 .text = std::move(text),
                 .icon = icon,
                 .alignment = UiTextAlign::center,
                 .selected = selected,
                 .compact_text = true});
        };
        if (allow_replace_)
            button(17, 2, 6, 20, replace_ ? L"⌄" : L"›");
        button(12, end - option_width, 8, 24, L"Aa", case_);
        if (allow_replace_)
            button(13, end - 24, 8, 24, L".*", regex()).enabled = regex_enabled_;
        float x = end + 2;
        if (!compact) {
            button(10, x, 8, 24, L"↑");
            x += 24;
            button(11, x, 8, 24, L"↓");
            x += 24;
        }
        button(14, x, 8, 24, L"×");
        if (replace_) {
            surface_.Add(
                {.role = UiRole::input_field,
                 .bounds = UiRect(input_left * scale, 38 * scale, replace_end * scale, 66 * scale),
                 .selected = GetFocus() == replacement_});
            button(15, replace_end + 2, 38, 24, L"", false, UiIcon::replace_one);
            button(16, replace_end + 26, 38, 24, L"", false, UiIcon::replace_all);
        }
        InvalidateRect(window_, nullptr, FALSE);
    }
    static LRESULT CALLBACK
    EditProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR data) {
        auto* self = reinterpret_cast<FindBar*>(data);
        if (message == WM_GETDLGCODE)
            return DLGC_WANTALLKEYS;
        if (message == WM_SETFOCUS || message == WM_KILLFOCUS) {
            const auto result = DefSubclassProc(window, message, wparam, lparam);
            self->Layout(false);
            return result;
        }
        if (message == WM_KEYDOWN) {
            if ((GetKeyState(VK_CONTROL) & 0x8000) && (wparam == 'F' || wparam == 'H')) {
                if (self->allow_replace_ && wparam == 'F')
                    self->Show(false, self->return_focus_);
                return 0;
            }
            if (wparam == VK_ESCAPE) {
                self->Hide();
                return 0;
            }
            if (wparam == VK_RETURN || wparam == VK_F3) {
                self->Invoke((GetKeyState(VK_SHIFT) & 0x8000) ? Action::Previous : Action::Next);
                return 0;
            }
            if (wparam == VK_TAB) {
                SetFocus(window == self->query_ && self->replace_ ? self->replacement_
                                                                  : self->query_);
                return 0;
            }
        }
        if (message == WM_CHAR && (wparam == VK_RETURN || wparam == VK_ESCAPE || wparam == VK_TAB))
            return 0;
        if (message == WM_CHAR && (GetKeyState(VK_CONTROL) & 0x8000) &&
            (wparam == 6 || wparam == 8))
            return 0;
        if (message == WM_NCDESTROY)
            RemoveWindowSubclass(window, EditProc, 1);
        return DefSubclassProc(window, message, wparam, lparam);
    }
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* self = reinterpret_cast<FindBar*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<FindBar*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            self->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self)
            return DefWindowProcW(window, message, wparam, lparam);
        switch (message) {
        case WM_DESTROY:
            self->destroying_ = true;
            return 0;
        case WM_NCDESTROY:
            self->window_ = nullptr;
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            break;
        case WM_PAINT:
            self->surface_.Paint(UiColor(kPanelColor));
            return 0;
        case WM_PRINTCLIENT: {
            RECT rc{};
            GetClientRect(window, &rc);
            self->surface_.PaintTo(reinterpret_cast<HDC>(wparam), rc, UiColor(kPanelColor));
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            self->surface_.Resize(LOWORD(lparam), HIWORD(lparam));
            if (!self->placing_)
                self->Layout();
            return 0;
        case WM_MOUSEMOVE:
            self->surface_.PointerMove({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
            return 0;
        case WM_MOUSELEAVE:
            self->surface_.PointerLeave();
            return 0;
        case WM_LBUTTONDOWN:
            self->surface_.PointerDown({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
            return 0;
        case WM_LBUTTONUP:
            if (auto id = self->surface_.PointerUp({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)})) {
                switch (*id) {
                case 10:
                    self->Invoke(Action::Previous);
                    break;
                case 11:
                    self->Invoke(Action::Next);
                    break;
                case 12:
                    self->case_ = !self->case_;
                    self->error_ = false;
                    self->Layout(false);
                    break;
                case 13:
                    self->regex_ = !self->regex_;
                    self->error_ = false;
                    self->Layout(false);
                    break;
                case 14:
                    self->Hide();
                    break;
                case 15:
                    self->Invoke(Action::Replace);
                    break;
                case 16:
                    self->Invoke(Action::ReplaceAll);
                    break;
                case 17:
                    self->replace_ = !self->replace_;
                    if (self->action)
                        self->Invoke(Action::Changed);
                    else
                        self->Layout();
                    SetFocus(self->query_);
                    break;
                }
            }
            return 0;
        case WM_COMMAND:
            if (HIWORD(wparam) == EN_CHANGE) {
                if (std::exchange(self->error_, false))
                    self->Layout(false);
            }
            return 0;
        case WM_CTLCOLOREDIT:
            SetTextColor(reinterpret_cast<HDC>(wparam), kTextColor);
            SetBkColor(reinterpret_cast<HDC>(wparam), kWindowColor);
            return reinterpret_cast<LRESULT>(WindowThemeBrush());
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }
};
} // namespace SLink::ui::detail
