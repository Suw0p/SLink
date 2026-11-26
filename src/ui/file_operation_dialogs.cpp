// 权限与重命名模态对话框的自绘布局、输入校验及 owner 窗口激活行为实现。
#include "ui/file_operation_dialogs.hpp"

#include "ui/retained_ui.hpp"
#include "ui/win32_ui_internal.hpp"

#include <array>
#include <cwchar>
#include <span>
#include <string_view>
#include <utility>
#include <windowsx.h>

namespace SLink::ui {
namespace {

constexpr int kPermissionEditId = 5101;
constexpr int kPermissionCheckBase = 5110;
constexpr int kRecursiveId = 5120;

constexpr std::array<std::uint32_t, 9> kPermissionBits{
    0400U,
    0200U,
    0100U,
    0040U,
    0020U,
    0010U,
    0004U,
    0002U,
    0001U,
};

bool IsPermissionCheckbox(const int id) {
    return id >= kPermissionCheckBase &&
           id < kPermissionCheckBase + static_cast<int>(kPermissionBits.size());
}

HFONT ChildFont(const UINT dpi) {
    return detail::CreateUiFont(dpi, 10);
}

void SetFont(const HWND control, const HFONT font) {
    if (control && font) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
}

// 程序内通知与文件关闭确认共用一个模态宿主，统一圆角、焦点及退出消息处理。
class FilePromptDialog final {
public:
    FilePromptDialog(HWND owner, std::wstring label)
        : owner_(detail::RootWindowForModalOwner(owner)), text_(std::move(label)) {
        text_ += L"\r\n\r\n保存写回原文件；放弃会删除此文件的本地草稿。"
                 L"\r\n保留草稿不写回原文件，下次打开时恢复修改。";
    }
    FilePromptDialog(HWND owner, const wchar_t* message, const wchar_t* title, UINT flags)
        : owner_(detail::RootWindowForModalOwner(owner)), text_(message ? message : L""),
          title_(title ? title : L"SLink"), message_flags_(flags), unsaved_(false) {}
    ~FilePromptDialog() {
        if (window_)
            DestroyWindow(window_);
        if (font_)
            DeleteObject(font_);
        if (small_font_)
            DeleteObject(small_font_);
    }
    int Run() {
        WNDCLASSW wc{};
        wc.lpfnWndProc = Proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = unsaved_ ? L"SLink.UnsavedFileDialog" : L"SLink.MessageDialog";
        if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return 0;
        const auto dpi = owner_ ? detail::WindowDpi(owner_) : 96U;
        RECT bounds{};
        GetWindowRect(owner_, &bounds);
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromWindow(owner_, MONITOR_DEFAULTTONEAREST), &monitor);
        if (!owner_ || !IsWindow(owner_))
            bounds = monitor.rcWork;
        const int width = std::min(detail::ScaleForDpi(520, dpi),
                                   static_cast<int>(monitor.rcWork.right - monitor.rcWork.left));
        // 初始尺寸只用于在目标屏幕创建隐藏窗口。最终尺寸必须按窗口实际 DPI 测量，
        // 无 owner 的启动错误框也不能把 96 DPI 的尺寸与高 DPI 字体混用。
        const int height = std::min(detail::ScaleForDpi(220, dpi),
                                    static_cast<int>(monitor.rcWork.bottom - monitor.rcWork.top) -
                                        detail::ScaleForDpi(20, dpi));
        const int x = std::clamp(static_cast<int>((bounds.left + bounds.right - width) / 2),
                                 static_cast<int>(monitor.rcWork.left),
                                 static_cast<int>(monitor.rcWork.right) - width);
        const int y = std::clamp(static_cast<int>((bounds.top + bounds.bottom - height) / 2),
                                 static_cast<int>(monitor.rcWork.top),
                                 static_cast<int>(monitor.rcWork.bottom) - height);
        window_ = CreateWindowExW(0,
                                  wc.lpszClassName,
                                  title_.c_str(),
                                  WS_POPUP | WS_SYSMENU | WS_CLIPCHILDREN,
                                  x,
                                  y,
                                  width,
                                  height,
                                  owner_,
                                  nullptr,
                                  wc.hInstance,
                                  this);
        if (!window_)
            return 0;
        if (!FitToWorkArea(detail::WindowDpi(window_), true))
            return 0;
        detail::ApplyPopupWindowTheme(window_);
        const HWND previous_focus = GetFocus();
        const bool enabled = owner_ && IsWindowEnabled(owner_);
        owner_was_enabled_ = enabled;
        if (enabled)
            EnableWindow(owner_, FALSE);
        ShowWindow(window_, SW_SHOW);
        SetFocus(GetDlgItem(window_, DefaultId()));
        UpdateWindow(window_);
        MSG message{};
        while (window_) {
            const int read = GetMessageW(&message, nullptr, 0, 0);
            if (read <= 0) {
                if (read == 0)
                    PostQuitMessage(static_cast<int>(message.wParam));
                break;
            }
            if (message.message == WM_KEYDOWN &&
                (message.hwnd == window_ || IsChild(window_, message.hwnd))) {
                if (message.wParam == VK_ESCAPE || message.wParam == VK_RETURN) {
                    // 按键属于它投递到的控件；焦点可能在排队期间变化，不能据此把
                    // 原本给某个按钮的回车改派给另一个按钮。正文/宿主上的回车用默认值。
                    int id = DefaultId();
                    if (message.wParam == VK_ESCAPE)
                        id = CancelId();
                    else
                        for (int button : ButtonIds())
                            if (message.hwnd == GetDlgItem(window_, button)) {
                                id = button;
                                break;
                            }
                    Choose(id);
                    continue;
                }
            }
            if (!IsDialogMessageW(window_, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        // 异常退出消息循环也销毁窗口，不能留下指向栈上对话框对象的回调。
        if (enabled && IsWindow(owner_))
            EnableWindow(owner_, TRUE);
        if (window_)
            DestroyWindow(window_);
        if (enabled && IsWindow(owner_)) {
            detail::ActivateModalOwner(owner_);
            if (IsWindow(previous_focus))
                SetFocus(previous_focus);
        }
        return choice_ ? choice_ : CancelId();
    }

private:
    HWND owner_{}, window_{};
    HFONT font_{}, small_font_{};
    detail::UiSurface surface_;
    std::wstring text_;
    std::wstring title_{L"关闭文件"};
    UINT message_flags_{};
    bool unsaved_{true};
    UINT layout_dpi_{};
    bool placing_{};
    int choice_{};
    bool owner_was_enabled_{};
    bool YesNo() const {
        return !unsaved_ && (message_flags_ & MB_TYPEMASK) == MB_YESNO;
    }
    int CancelId() const {
        return unsaved_ ? IDCANCEL : YesNo() ? IDNO : IDOK;
    }
    int DefaultId() const {
        return unsaved_  ? IDCANCEL
               : YesNo() ? ((message_flags_ & MB_DEFMASK) == MB_DEFBUTTON2 ? IDNO : IDYES)
                         : IDOK;
    }
    static constexpr std::array<const wchar_t*, 3> titles{
        L"保存并关闭", L"放弃修改", L"保留本地草稿"};
    std::span<const int> ButtonIds() const {
        static constexpr std::array close_ids{100, 101, 102, IDCANCEL};
        static constexpr std::array confirmation_ids{IDYES, IDNO};
        static constexpr std::array notification_ids{IDOK};
        return unsaved_  ? std::span<const int>(close_ids)
               : YesNo() ? std::span<const int>(confirmation_ids)
                         : std::span<const int>(notification_ids);
    }
    int ButtonColumns(int width, UINT dpi) const {
        const int available = std::max(1, width - detail::ScaleForDpi(40, dpi));
        if (!unsaved_)
            return available >= detail::ScaleForDpi(184, dpi) ? (YesNo() ? 2 : 1) : 1;
        return available >= detail::ScaleForDpi(416, dpi)   ? 4
               : available >= detail::ScaleForDpi(204, dpi) ? 2
                                                            : 1;
    }
    bool EnsureFonts(UINT dpi) {
        if (layout_dpi_ == dpi && font_ && small_font_)
            return true;
        const auto font = detail::CreateUiFont(dpi, 10),
                   caption_font = detail::CreateUiFont(dpi, 9);
        if (!font || !caption_font) {
            if (font)
                DeleteObject(font);
            if (caption_font)
                DeleteObject(caption_font);
            return false;
        }
        SendMessageW(GetDlgItem(window_, 10),
                     WM_SETFONT,
                     reinterpret_cast<WPARAM>(unsaved_ ? caption_font : font),
                     FALSE);
        for (int id : ButtonIds())
            SendMessageW(
                GetDlgItem(window_, id), WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
        if (font_)
            DeleteObject(font_);
        if (small_font_)
            DeleteObject(small_font_);
        font_ = font;
        small_font_ = caption_font;
        layout_dpi_ = dpi;
        return true;
    }
    bool FitToWorkArea(UINT dpi, bool center) {
        if (placing_ || !window_)
            return true;
        if (!EnsureFonts(std::max(96U, dpi)))
            return false;
        MONITORINFO monitor{sizeof(monitor)};
        if (!GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST), &monitor))
            return false;
        const auto s = [this](int n) {
            return detail::ScaleForDpi(n, layout_dpi_);
        };
        const int width =
            std::min(s(520), static_cast<int>(monitor.rcWork.right - monitor.rcWork.left));
        const auto dc = GetDC(window_);
        if (!dc)
            return false;
        const auto previous = SelectObject(dc, unsaved_ ? small_font_ : font_);
        RECT text{0, 0, std::max(1, width - s(40) - 4), 0};
        DrawTextW(dc,
                  text_.c_str(),
                  static_cast<int>(text_.size()),
                  &text,
                  DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_EXPANDTABS | DT_EDITCONTROL);
        SelectObject(dc, previous);
        ReleaseDC(window_, dc);
        const int count = static_cast<int>(ButtonIds().size()),
                  columns = ButtonColumns(width, layout_dpi_);
        const int rows = (count + columns - 1) / columns;
        const int footer = s(40 + rows * 30 + (rows - 1) * 8);
        const int preferred =
            (unsaved_ ? s(71) : s(52)) +
            std::max(unsaved_ ? s(72) : s(54), static_cast<int>(text.bottom) + s(4)) + footer;
        const int height = std::min<int>(
            preferred, std::max(1L, monitor.rcWork.bottom - monitor.rcWork.top - s(20)));
        RECT origin{};
        GetWindowRect(window_, &origin);
        if (center) {
            if (!owner_ || !GetWindowRect(owner_, &origin))
                origin = monitor.rcWork;
            origin.left += (origin.right - origin.left - width) / 2;
            origin.top += (origin.bottom - origin.top - height) / 2;
        }
        const int x = std::clamp(static_cast<int>(origin.left),
                                 static_cast<int>(monitor.rcWork.left),
                                 static_cast<int>(monitor.rcWork.right) - width);
        const int y = std::clamp(static_cast<int>(origin.top),
                                 static_cast<int>(monitor.rcWork.top),
                                 static_cast<int>(monitor.rcWork.bottom) - height);
        placing_ = true;
        SetWindowPos(window_, nullptr, x, y, width, height, SWP_NOACTIVATE | SWP_NOZORDER);
        placing_ = false;
        Layout();
        return true;
    }
    void Choose(int id) {
        choice_ = id == IDCANCEL ? CancelId() : id;
        // 先恢复 owner 再销毁弹窗，避免 Windows 激活其他程序。
        if (owner_was_enabled_ && IsWindow(owner_)) {
            EnableWindow(owner_, TRUE);
            detail::RaiseOwnedWindowGroup(window_);
        }
        DestroyWindow(window_);
    }
    void Layout() {
        if (!layout_dpi_)
            return; // WM_CREATE 后、首次显示前统一测量，避免使用临时尺寸。
        const auto dpi = layout_dpi_;
        const auto s = [dpi](int n) {
            return detail::ScaleForDpi(n, dpi);
        };
        RECT rc{};
        GetClientRect(window_, &rc);
        const int pad = s(20), width = rc.right;
        surface_.ClearElements();
        const auto rect = [&](int l, int t, int r, int b) {
            return detail::UiRect(static_cast<float>(l),
                                  static_cast<float>(t),
                                  static_cast<float>(r),
                                  static_cast<float>(b));
        };
        surface_.Add({.role = detail::UiRole::card, .bounds = rect(0, 0, width, rc.bottom)});
        surface_.Add({.role = detail::UiRole::section_heading,
                      .bounds = rect(pad, s(10), width - s(48), s(40)),
                      .text = title_});
        surface_.Add({.id = IDCANCEL,
                      .role = detail::UiRole::tab_close,
                      .bounds = rect(width - s(42), s(8), width - s(10), s(40)),
                      .icon = detail::UiIcon::close});
        if (unsaved_)
            surface_.Add({.role = detail::UiRole::label,
                          .bounds = rect(pad, s(44), width - pad, s(70)),
                          .text = L"此文件有未保存修改"});
        const auto body = GetDlgItem(window_, 10);
        SendMessageW(
            body, WM_SETFONT, reinterpret_cast<WPARAM>(unsaved_ ? small_font_ : font_), FALSE);
        const auto ids = ButtonIds();
        const int columns = ButtonColumns(width, dpi),
                  rows = (static_cast<int>(ids.size()) + columns - 1) / columns;
        const int buttons_top = rc.bottom - s(20 + rows * 30 + (rows - 1) * 8);
        const int body_y = unsaved_ ? s(71) : s(52);
        const int body_height = std::max(1, buttons_top - s(20) - body_y);
        MoveWindow(body, pad, body_y, width - pad * 2, body_height, TRUE);
        const auto scrollbar = GetDlgItem(window_, 11);
        const auto metrics = BodyMetrics();
        const bool needs_scroll = metrics.maximum > 0;
        if (needs_scroll)
            MoveWindow(body, pad, body_y, width - pad * 2 - s(10), body_height, TRUE);
        MoveWindow(scrollbar, width - pad - s(7), body_y, s(7), body_height, FALSE);
        ShowWindow(scrollbar, needs_scroll ? SW_SHOWNOACTIVATE : SW_HIDE);
        const std::array widths{s(104), s(92), s(124), s(72)};
        const auto button_width = [&](int index) {
            const int available = std::max(1, width - pad * 2);
            return columns == 1 && ids.size() > 1
                       ? available
                       : std::min(available, unsaved_ ? widths[index] : s(88));
        };
        for (int row = 0; row < rows; ++row) {
            const int first = row * columns,
                      last = std::min(first + columns, static_cast<int>(ids.size()));
            int left = width - pad - s(8) * (last - first - 1);
            for (int index = first; index < last; ++index)
                left -= button_width(index);
            for (int index = first; index < last; ++index) {
                const int size = button_width(index);
                MoveWindow(GetDlgItem(window_, ids[index]),
                           left,
                           buttons_top + row * s(38),
                           size,
                           s(30),
                           TRUE);
                left += size + s(8);
            }
        }
        InvalidateRect(window_, nullptr, FALSE);
    }
    struct BodyScroll {
        int maximum{}, position{}, length{1}, thumb_size{1}, thumb_start{};
    };
    int body_drag_offset_{};
    BodyScroll BodyMetrics() const {
        BodyScroll m;
        const auto body = GetDlgItem(window_, 10), bar = GetDlgItem(window_, 11);
        RECT rc{}, track{};
        GetClientRect(body, &rc);
        GetClientRect(bar, &track);
        const auto dc = GetDC(body);
        const auto previous = SelectObject(dc, unsaved_ ? small_font_ : font_);
        TEXTMETRICW metrics{};
        GetTextMetricsW(dc, &metrics);
        SelectObject(dc, previous);
        ReleaseDC(body, dc);
        const int page = std::max(1L, rc.bottom / std::max(1L, metrics.tmHeight));
        const int total =
            std::max(page, static_cast<int>(SendMessageW(body, EM_GETLINECOUNT, 0, 0)));
        m.maximum = total - page;
        m.position = std::clamp(
            static_cast<int>(SendMessageW(body, EM_GETFIRSTVISIBLELINE, 0, 0)), 0, m.maximum);
        m.length = std::max(1L, track.bottom);
        m.thumb_size =
            std::clamp(MulDiv(m.length, page, total),
                       std::min(m.length, detail::ScaleForDpi(18, detail::WindowDpi(window_))),
                       m.length);
        m.thumb_start = m.maximum > 0 ? MulDiv(m.length - m.thumb_size, m.position, m.maximum) : 0;
        return m;
    }
    static LRESULT CALLBACK
    BodyScrollProc(HWND bar, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR value) {
        auto* self = reinterpret_cast<FilePromptDialog*>(value);
        if (message == WM_PAINT || message == WM_PRINTCLIENT) {
            PAINTSTRUCT paint{};
            const auto dc =
                message == WM_PAINT ? BeginPaint(bar, &paint) : reinterpret_cast<HDC>(wp);
            RECT rc{};
            GetClientRect(bar, &rc);
            FillRect(dc, &rc, detail::PanelThemeBrush());
            const auto m = self->BodyMetrics();
            const auto fill =
                CreateSolidBrush(theme::IsDark() ? RGB(80, 83, 89) : RGB(164, 169, 178));
            const auto old_brush = SelectObject(dc, fill),
                       old_pen = SelectObject(dc, GetStockObject(NULL_PEN));
            RoundRect(dc, 1, m.thumb_start, rc.right - 1, m.thumb_start + m.thumb_size, 5, 5);
            SelectObject(dc, old_pen);
            SelectObject(dc, old_brush);
            DeleteObject(fill);
            if (message == WM_PAINT)
                EndPaint(bar, &paint);
            return 0;
        }
        if (message == WM_ERASEBKGND)
            return 1;
        if (message == WM_LBUTTONDOWN || (message == WM_MOUSEMOVE && GetCapture() == bar)) {
            const auto m = self->BodyMetrics();
            const int y = GET_Y_LPARAM(lp);
            if (message == WM_LBUTTONDOWN) {
                self->body_drag_offset_ = y >= m.thumb_start && y < m.thumb_start + m.thumb_size
                                              ? y - m.thumb_start
                                              : m.thumb_size / 2;
                SetCapture(bar);
            }
            const int target = MulDiv(
                std::clamp(y - self->body_drag_offset_, 0, std::max(0, m.length - m.thumb_size)),
                m.maximum,
                std::max(1, m.length - m.thumb_size));
            SendMessageW(GetDlgItem(self->window_, 10), EM_LINESCROLL, 0, target - m.position);
            InvalidateRect(bar, nullptr, FALSE);
            return 0;
        }
        if (message == WM_LBUTTONUP && GetCapture() == bar) {
            ReleaseCapture();
            return 0;
        }
        if (message == WM_MOUSEWHEEL) {
            SendMessageW(GetDlgItem(self->window_, 10), message, wp, lp);
            return 0;
        }
        if (message == WM_NCDESTROY)
            RemoveWindowSubclass(bar, BodyScrollProc, id);
        return DefSubclassProc(bar, message, wp, lp);
    }
    static LRESULT CALLBACK Proc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<FilePromptDialog*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<FilePromptDialog*>(
                reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            self->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self)
            return DefWindowProcW(window, message, wp, lp);
        switch (message) {
        case WM_CREATE:
            if (!self->surface_.Initialize(window))
                return -1;
            for (int id : self->ButtonIds()) {
                const auto button =
                    CreateWindowExW(0,
                                    L"BUTTON",
                                    self->unsaved_ ? (id == IDCANCEL ? L"取消" : titles[id - 100])
                                    : id == IDYES  ? L"是"
                                    : id == IDNO   ? L"否"
                                                   : L"确定",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                    0,
                                    0,
                                    0,
                                    0,
                                    window,
                                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                    GetModuleHandleW(nullptr),
                                    nullptr);
                if (!button)
                    return -1;
                detail::ApplySelfDrawnTheme(button);
            }
            {
                const auto body = CreateWindowExW(0,
                                                  L"EDIT",
                                                  self->text_.c_str(),
                                                  WS_CHILD | WS_VISIBLE | ES_MULTILINE |
                                                      ES_READONLY | ES_AUTOVSCROLL,
                                                  0,
                                                  0,
                                                  0,
                                                  0,
                                                  window,
                                                  reinterpret_cast<HMENU>(10),
                                                  GetModuleHandleW(nullptr),
                                                  nullptr);
                if (!body)
                    return -1;
                detail::ApplySelfDrawnTheme(body);
                const auto bar = CreateWindowExW(0,
                                                 L"STATIC",
                                                 L"",
                                                 WS_CHILD,
                                                 0,
                                                 0,
                                                 0,
                                                 0,
                                                 window,
                                                 reinterpret_cast<HMENU>(11),
                                                 GetModuleHandleW(nullptr),
                                                 nullptr);
                if (!bar)
                    return -1;
                SetWindowSubclass(bar, BodyScrollProc, 1, reinterpret_cast<DWORD_PTR>(self));
            }
            return 0;
        case WM_COMMAND:
            if (HIWORD(wp) == BN_CLICKED)
                self->Choose(LOWORD(wp));
            else if (HIWORD(wp) == EN_VSCROLL)
                InvalidateRect(GetDlgItem(window, 11), nullptr, FALSE);
            return 0;
        case WM_CLOSE:
            self->Choose(self->CancelId());
            return 0;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
            SetTextColor(reinterpret_cast<HDC>(wp),
                         self->unsaved_ ? theme::kMutedTextColor : theme::kTextColor);
            SetBkColor(reinterpret_cast<HDC>(wp), theme::kPanelColor);
            return reinterpret_cast<LRESULT>(detail::PanelThemeBrush());
        case WM_NCDESTROY:
            self->window_ = nullptr;
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            break;
        case WM_SIZE:
            self->surface_.Resize(LOWORD(lp), HIWORD(lp));
            if (!self->placing_)
                self->Layout();
            return 0;
        case WM_DPICHANGED: {
            const auto* bounds = reinterpret_cast<RECT*>(lp);
            self->placing_ = true;
            SetWindowPos(window,
                         nullptr,
                         bounds->left,
                         bounds->top,
                         bounds->right - bounds->left,
                         bounds->bottom - bounds->top,
                         SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOREDRAW);
            self->placing_ = false;
            self->surface_.DpiChanged();
            self->FitToWorkArea(HIWORD(wp), false);
            return 0;
        }
        case WM_PAINT:
            self->surface_.Paint(detail::UiColor(theme::kPanelColor));
            return 0;
        case WM_PRINTCLIENT: {
            RECT rc{};
            GetClientRect(window, &rc);
            self->surface_.PaintTo(
                reinterpret_cast<HDC>(wp), rc, detail::UiColor(theme::kPanelColor));
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_MOUSEMOVE:
            self->surface_.PointerMove({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            return 0;
        case WM_MOUSELEAVE:
            self->surface_.PointerLeave();
            return 0;
        case WM_LBUTTONDOWN:
            self->surface_.PointerDown({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            return 0;
        case WM_LBUTTONUP:
            if (auto id = self->surface_.PointerUp({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}))
                self->Choose(*id);
            return 0;
        case WM_NCHITTEST: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(window, &pt);
            RECT rc{};
            GetClientRect(window, &rc);
            if (pt.y < detail::ScaleForDpi(40, detail::WindowDpi(window)) &&
                pt.x < rc.right - detail::ScaleForDpi(48, detail::WindowDpi(window)))
                return HTCAPTION;
            break;
        }
        case WM_DRAWITEM: {
            const auto* draw = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
            if (draw && draw->CtlType == ODT_BUTTON) {
                detail::DrawSelfDrawnButton(*draw, draw->CtlID == 100);
                if (draw->itemState & ODS_FOCUS) {
                    const auto pen = CreatePen(PS_SOLID, 1, theme::kAccentColor);
                    const auto previous_pen = SelectObject(draw->hDC, pen);
                    const auto previous_brush = SelectObject(draw->hDC, GetStockObject(NULL_BRUSH));
                    RoundRect(draw->hDC,
                              draw->rcItem.left,
                              draw->rcItem.top,
                              draw->rcItem.right - 1,
                              draw->rcItem.bottom - 1,
                              7,
                              7);
                    SelectObject(draw->hDC, previous_pen);
                    SelectObject(draw->hDC, previous_brush);
                    DeleteObject(pen);
                }
                return TRUE;
            }
            break;
        }
        }
        return DefWindowProcW(window, message, wp, lp);
    }
};

class PermissionDialog final {
public:
    PermissionDialog(HWND owner,
                     std::wstring item_name,
                     const std::uint32_t permissions,
                     const bool directory)
        : owner_(detail::RootWindowForModalOwner(owner)), item_name_(std::move(item_name)),
          initial_permissions_(permissions & 0777U), directory_(directory) {}

    std::optional<PermissionDialogResult> Run() {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = &WindowProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = detail::PanelThemeBrush();
        wc.lpszClassName = kClassName;
        const ATOM atom = RegisterClassExW(&wc);
        if (atom == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return std::nullopt;
        }

        const UINT dpi = detail::WindowDpi(owner_);
        const int width = detail::ScaleForDpi(490, dpi);
        const int height = detail::ScaleForDpi(380, dpi);
        RECT owner_rect{};
        GetWindowRect(owner_, &owner_rect);
        const int x = owner_rect.left + (owner_rect.right - owner_rect.left - width) / 2;
        const int y = owner_rect.top + (owner_rect.bottom - owner_rect.top - height) / 2;
        window_ = CreateWindowExW(WS_EX_DLGMODALFRAME,
                                  kClassName,
                                  L"更改权限",
                                  WS_POPUP | WS_CAPTION | WS_SYSMENU,
                                  x,
                                  y,
                                  width,
                                  height,
                                  owner_,
                                  nullptr,
                                  GetModuleHandleW(nullptr),
                                  this);
        if (!window_)
            return std::nullopt;

        detail::ApplyPopupWindowTheme(window_);
        EnableWindow(owner_, FALSE);
        ShowWindow(window_, SW_SHOW);
        UpdateWindow(window_);

        MSG message{};
        while (!done_ && GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (!IsDialogMessageW(window_, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        EnableWindow(owner_, TRUE);
        detail::ActivateModalOwner(owner_);
        if (font_)
            DeleteObject(font_);
        return accepted_ ? std::optional<PermissionDialogResult>(result_) : std::nullopt;
    }

private:
    static constexpr wchar_t kClassName[] = L"SLink.PermissionDialog";

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* self = reinterpret_cast<PermissionDialog*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            self = static_cast<PermissionDialog*>(create->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->window_ = window;
        }
        if (!self)
            return DefWindowProcW(window, message, wparam, lparam);

        switch (message) {
        case WM_ACTIVATE:
            if (LOWORD(wparam) != WA_INACTIVE) {
                detail::RaiseOwnedWindowGroup(window);
            }
            return 0;
        case WM_CREATE:
            self->CreateControls();
            return 0;
        case WM_COMMAND:
            self->OnCommand(LOWORD(wparam), HIWORD(wparam));
            return 0;
        case WM_DRAWITEM: {
            const auto* draw = reinterpret_cast<const DRAWITEMSTRUCT*>(lparam);
            if (draw && detail::DrawSelfDrawnButton(*draw,
                                                    draw->CtlID == IDOK,
                                                    IsPermissionCheckbox(draw->CtlID) ||
                                                        draw->CtlID == kRecursiveId)) {
                return TRUE;
            }
            break;
        }
        case WM_PAINT:
            self->PaintGroups();
            return 0;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN: {
            const HDC dc = reinterpret_cast<HDC>(wparam);
            SetTextColor(dc, detail::kTextColor);
            SetBkColor(dc, detail::kPanelColor);
            return reinterpret_cast<LRESULT>(detail::PanelThemeBrush());
        }
        case WM_CTLCOLOREDIT: {
            const HDC dc = reinterpret_cast<HDC>(wparam);
            SetTextColor(dc, detail::kTextColor);
            SetBkColor(dc, detail::kPanelAltColor);
            return reinterpret_cast<LRESULT>(detail::PanelAltThemeBrush());
        }
        case WM_CLOSE:
            self->Cancel();
            return 0;
        case WM_DESTROY:
            self->done_ = true;
            self->window_ = nullptr;
            return 0;
        default:
            break;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    void CreateControls() {
        const UINT dpi = detail::WindowDpi(window_);
        const auto s = [dpi](const int value) {
            return detail::ScaleForDpi(value, dpi);
        };
        font_ = ChildFont(dpi);

        MakeStatic(L"文件/文件夹权限：", 20, 20, 180, 25);
        permission_edit_ = CreateWindowExW(
            0,
            L"EDIT",
            L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_CENTER | ES_MULTILINE | ES_NUMBER,
            s(350),
            s(18),
            s(100),
            s(25),
            window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPermissionEditId)),
            GetModuleHandleW(nullptr),
            nullptr);
        SendMessageW(permission_edit_, EM_SETLIMITTEXT, 4, 0);

        constexpr std::array<const wchar_t*, 3> headings{L"所有者", L"组", L"其他"};
        constexpr std::array<const wchar_t*, 3> labels{L"读取", L"写入", L"执行"};
        for (std::size_t group = 0; group < 3; ++group) {
            const int x = 20 + static_cast<int>(group) * 150;
            MakeStatic(headings[group], x + 10, 59, 90, 22);
            for (std::size_t access = 0; access < 3; ++access) {
                const std::size_t index = group * 3 + access;
                checks_[index] = MakeCheckbox(labels[access],
                                              kPermissionCheckBase + static_cast<int>(index),
                                              x + 14,
                                              84 + static_cast<int>(access) * 36,
                                              105,
                                              26);
            }
        }

        recursive_ = MakeCheckbox(L"包含子目录", kRecursiveId, 20, 213, 170, 28);
        EnableWindow(recursive_, directory_ ? TRUE : FALSE);
        MakeStatic(L"此命令仅适用于支持 UNIX 权限的主机。", 20, 251, 410, 24);
        ok_ = MakeButton(L"确定", IDOK, 270, 292, 88, 32);
        MakeButton(L"取消", IDCANCEL, 368, 292, 88, 32);

        EnumChildWindows(
            window_,
            [](HWND child, LPARAM value) -> BOOL {
                detail::ApplySelfDrawnTheme(child);
                SetFont(child, reinterpret_cast<HFONT>(value));
                return TRUE;
            },
            reinterpret_cast<LPARAM>(font_));

        RECT edit_client{};
        GetClientRect(permission_edit_, &edit_client);
        const HDC edit_dc = GetDC(permission_edit_);
        if (edit_dc) {
            const HGDIOBJ previous = SelectObject(edit_dc, font_);
            TEXTMETRICW metrics{};
            GetTextMetricsW(edit_dc, &metrics);
            if (previous)
                SelectObject(edit_dc, previous);
            ReleaseDC(permission_edit_, edit_dc);
            const int text_height = metrics.tmHeight;
            const int top = std::max(0, (static_cast<int>(edit_client.bottom) - text_height) / 2);
            RECT formatting{edit_client.left + s(2),
                            top,
                            edit_client.right - s(2),
                            std::min<int>(edit_client.bottom, top + text_height + s(1))};
            SendMessageW(permission_edit_, EM_SETRECTNP, 0, reinterpret_cast<LPARAM>(&formatting));
        }

        updating_ = true;
        wchar_t mode[8]{};
        swprintf_s(mode, L"%03o", initial_permissions_);
        SetWindowTextW(permission_edit_, mode);
        UpdateChecks(initial_permissions_);
        updating_ = false;
        SendMessageW(permission_edit_, EM_SETSEL, 0, -1);
        SetFocus(permission_edit_);
    }

    HWND MakeStatic(
        const wchar_t* text, const int x, const int y, const int width, const int height) const {
        const UINT dpi = detail::WindowDpi(window_);
        return CreateWindowExW(0,
                               L"STATIC",
                               text,
                               WS_CHILD | WS_VISIBLE | SS_LEFT,
                               detail::ScaleForDpi(x, dpi),
                               detail::ScaleForDpi(y, dpi),
                               detail::ScaleForDpi(width, dpi),
                               detail::ScaleForDpi(height, dpi),
                               window_,
                               nullptr,
                               GetModuleHandleW(nullptr),
                               nullptr);
    }

    HWND MakeButton(const wchar_t* text,
                    const int id,
                    const int x,
                    const int y,
                    const int width,
                    const int height) const {
        const UINT dpi = detail::WindowDpi(window_);
        return CreateWindowExW(0,
                               L"BUTTON",
                               text,
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                               detail::ScaleForDpi(x, dpi),
                               detail::ScaleForDpi(y, dpi),
                               detail::ScaleForDpi(width, dpi),
                               detail::ScaleForDpi(height, dpi),
                               window_,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                               GetModuleHandleW(nullptr),
                               nullptr);
    }

    HWND MakeCheckbox(const wchar_t* text,
                      const int id,
                      const int x,
                      const int y,
                      const int width,
                      const int height) const {
        return MakeButton(text, id, x, y, width, height);
    }

    void PaintGroups() const {
        PAINTSTRUCT paint{};
        const HDC dc = BeginPaint(window_, &paint);
        const UINT dpi = detail::WindowDpi(window_);
        const auto s = [dpi](const int value) {
            return detail::ScaleForDpi(value, dpi);
        };
        const HPEN pen = CreatePen(PS_SOLID, 1, detail::kBorderColor);
        const HGDIOBJ old_pen = SelectObject(dc, pen);
        const HGDIOBJ old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        for (int group = 0; group < 3; ++group) {
            const int x = 20 + group * 150;
            RoundRect(dc, s(x), s(73), s(x + 130), s(200), s(7), s(7));
        }
        SelectObject(dc, old_brush);
        SelectObject(dc, old_pen);
        DeleteObject(pen);
        EndPaint(window_, &paint);
    }

    void OnCommand(const int id, const int notification) {
        if (id == kPermissionEditId && notification == EN_CHANGE) {
            if (!updating_)
                SyncChecksFromEdit();
            return;
        }
        if ((IsPermissionCheckbox(id) || id == kRecursiveId) && notification == BN_CLICKED) {
            HWND control = id == kRecursiveId
                               ? recursive_
                               : checks_[static_cast<std::size_t>(id - kPermissionCheckBase)];
            detail::SetSelfDrawnCheckboxChecked(control,
                                                !detail::SelfDrawnCheckboxChecked(control));
            if (id != kRecursiveId)
                SyncEditFromChecks();
            return;
        }
        if (id == IDOK && notification == BN_CLICKED) {
            Accept();
            return;
        }
        if (id == IDCANCEL && notification == BN_CLICKED) {
            Cancel();
        }
    }

    std::optional<std::uint32_t> ParsedMode() const {
        wchar_t text[8]{};
        GetWindowTextW(permission_edit_, text, static_cast<int>(std::size(text)));
        std::wstring_view value(text);
        if (value.size() == 4 && value.front() == L'0') {
            value.remove_prefix(1);
        }
        if (value.size() != 3)
            return std::nullopt;
        std::uint32_t mode = 0;
        for (const wchar_t digit : value) {
            if (digit < L'0' || digit > L'7')
                return std::nullopt;
            mode = (mode << 3U) | static_cast<std::uint32_t>(digit - L'0');
        }
        return mode;
    }

    void UpdateChecks(const std::uint32_t mode) {
        for (std::size_t index = 0; index < checks_.size(); ++index) {
            detail::SetSelfDrawnCheckboxChecked(checks_[index],
                                                (mode & kPermissionBits[index]) != 0);
        }
    }

    void SyncChecksFromEdit() {
        const auto mode = ParsedMode();
        EnableWindow(ok_, mode ? TRUE : FALSE);
        if (mode)
            UpdateChecks(*mode);
    }

    void SyncEditFromChecks() {
        std::uint32_t mode = 0;
        for (std::size_t index = 0; index < checks_.size(); ++index) {
            if (detail::SelfDrawnCheckboxChecked(checks_[index])) {
                mode |= kPermissionBits[index];
            }
        }
        wchar_t text[8]{};
        swprintf_s(text, L"%03o", mode);
        updating_ = true;
        SetWindowTextW(permission_edit_, text);
        updating_ = false;
        EnableWindow(ok_, TRUE);
    }

    void Accept() {
        const auto mode = ParsedMode();
        if (!mode) {
            MessageBeep(MB_ICONWARNING);
            SetFocus(permission_edit_);
            SendMessageW(permission_edit_, EM_SETSEL, 0, -1);
            return;
        }
        result_.permissions = *mode;
        result_.recursive = directory_ && detail::SelfDrawnCheckboxChecked(recursive_);
        accepted_ = true;
        detail::PrepareModalWindowClose(window_);
        DestroyWindow(window_);
    }

    void Cancel() {
        accepted_ = false;
        detail::PrepareModalWindowClose(window_);
        DestroyWindow(window_);
    }

    HWND owner_{};
    HWND window_{};
    HWND permission_edit_{};
    HWND recursive_{};
    HWND ok_{};
    std::array<HWND, 9> checks_{};
    HFONT font_{};
    std::wstring item_name_;
    std::uint32_t initial_permissions_{};
    bool directory_{};
    bool updating_{};
    bool accepted_{};
    bool done_{};
    PermissionDialogResult result_{};
};

class RenameDialog final {
public:
    RenameDialog(HWND owner, std::wstring current_name, const bool local_path)
        : owner_(owner), current_name_(std::move(current_name)), local_path_(local_path) {}

    std::optional<std::wstring> Run() {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = &WindowProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = detail::PanelThemeBrush();
        wc.lpszClassName = kClassName;
        const ATOM atom = RegisterClassExW(&wc);
        if (atom == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return std::nullopt;
        }

        const UINT dpi = detail::WindowDpi(owner_);
        const int width = detail::ScaleForDpi(460, dpi);
        const int height = detail::ScaleForDpi(205, dpi);
        RECT owner_rect{};
        GetWindowRect(owner_, &owner_rect);
        const int x = owner_rect.left + (owner_rect.right - owner_rect.left - width) / 2;
        const int y = owner_rect.top + (owner_rect.bottom - owner_rect.top - height) / 2;
        window_ = CreateWindowExW(WS_EX_DLGMODALFRAME,
                                  kClassName,
                                  L"重命名",
                                  WS_POPUP | WS_CAPTION | WS_SYSMENU,
                                  x,
                                  y,
                                  width,
                                  height,
                                  owner_,
                                  nullptr,
                                  GetModuleHandleW(nullptr),
                                  this);
        if (!window_)
            return std::nullopt;

        detail::ApplyPopupWindowTheme(window_);
        EnableWindow(owner_, FALSE);
        ShowWindow(window_, SW_SHOW);
        UpdateWindow(window_);
        MSG message{};
        while (!done_ && GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (!IsDialogMessageW(window_, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        EnableWindow(owner_, TRUE);
        SetActiveWindow(owner_);
        if (font_)
            DeleteObject(font_);
        return accepted_ ? std::optional<std::wstring>(result_) : std::nullopt;
    }

private:
    static constexpr wchar_t kClassName[] = L"SLink.RenameDialog";
    static constexpr int kEditId = 5201;

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* self = reinterpret_cast<RenameDialog*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            self = static_cast<RenameDialog*>(create->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->window_ = window;
        }
        if (!self)
            return DefWindowProcW(window, message, wparam, lparam);
        switch (message) {
        case WM_CREATE:
            self->CreateControls();
            return 0;
        case WM_COMMAND:
            if (LOWORD(wparam) == IDOK && HIWORD(wparam) == BN_CLICKED) {
                self->Accept();
                return 0;
            }
            if (LOWORD(wparam) == IDCANCEL && HIWORD(wparam) == BN_CLICKED) {
                self->Cancel();
                return 0;
            }
            if (LOWORD(wparam) == kEditId && HIWORD(wparam) == EN_CHANGE) {
                SetWindowTextW(self->error_, L"");
                return 0;
            }
            break;
        case WM_DRAWITEM: {
            const auto* draw = reinterpret_cast<const DRAWITEMSTRUCT*>(lparam);
            if (draw && detail::DrawSelfDrawnButton(*draw, draw->CtlID == IDOK)) {
                return TRUE;
            }
            break;
        }
        case WM_CTLCOLORSTATIC: {
            const HDC dc = reinterpret_cast<HDC>(wparam);
            const HWND control = reinterpret_cast<HWND>(lparam);
            SetTextColor(dc, control == self->error_ ? RGB(232, 93, 93) : detail::kTextColor);
            SetBkColor(dc, detail::kPanelColor);
            return reinterpret_cast<LRESULT>(detail::PanelThemeBrush());
        }
        case WM_CTLCOLORBTN: {
            const HDC dc = reinterpret_cast<HDC>(wparam);
            SetTextColor(dc, detail::kTextColor);
            SetBkColor(dc, detail::kPanelColor);
            return reinterpret_cast<LRESULT>(detail::PanelThemeBrush());
        }
        case WM_CTLCOLOREDIT: {
            const HDC dc = reinterpret_cast<HDC>(wparam);
            SetTextColor(dc, detail::kTextColor);
            SetBkColor(dc, detail::kPanelAltColor);
            return reinterpret_cast<LRESULT>(detail::PanelAltThemeBrush());
        }
        case WM_CLOSE:
            self->Cancel();
            return 0;
        case WM_DESTROY:
            self->done_ = true;
            self->window_ = nullptr;
            return 0;
        default:
            break;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    void CreateControls() {
        const UINT dpi = detail::WindowDpi(window_);
        const auto s = [dpi](const int value) {
            return detail::ScaleForDpi(value, dpi);
        };
        font_ = ChildFont(dpi);
        HWND label = CreateWindowExW(0,
                                     L"STATIC",
                                     L"新名称：",
                                     WS_CHILD | WS_VISIBLE | SS_LEFT,
                                     s(20),
                                     s(23),
                                     s(90),
                                     s(27),
                                     window_,
                                     nullptr,
                                     GetModuleHandleW(nullptr),
                                     nullptr);
        edit_ = CreateWindowExW(0,
                                L"EDIT",
                                current_name_.c_str(),
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL,
                                s(105),
                                s(19),
                                s(320),
                                s(30),
                                window_,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEditId)),
                                GetModuleHandleW(nullptr),
                                nullptr);
        SendMessageW(edit_, EM_SETLIMITTEXT, 255, 0);
        error_ = CreateWindowExW(0,
                                 L"STATIC",
                                 L"",
                                 WS_CHILD | WS_VISIBLE | SS_LEFT,
                                 s(105),
                                 s(55),
                                 s(320),
                                 s(24),
                                 window_,
                                 nullptr,
                                 GetModuleHandleW(nullptr),
                                 nullptr);
        HWND ok = MakeButton(L"确定", IDOK, 235, 105, 90, 34);
        HWND cancel = MakeButton(L"取消", IDCANCEL, 335, 105, 90, 34);
        for (HWND control : {label, edit_, error_, ok, cancel}) {
            detail::ApplySelfDrawnTheme(control);
            SetFont(control, font_);
        }
        const std::size_t dot = current_name_.find_last_of(L'.');
        const LPARAM selection_end = dot != std::wstring::npos && dot != 0
                                         ? static_cast<LPARAM>(dot)
                                         : static_cast<LPARAM>(-1);
        SendMessageW(edit_, EM_SETSEL, 0, selection_end);
        SetFocus(edit_);
    }

    HWND MakeButton(const wchar_t* text,
                    const int id,
                    const int x,
                    const int y,
                    const int width,
                    const int height) const {
        const UINT dpi = detail::WindowDpi(window_);
        return CreateWindowExW(0,
                               L"BUTTON",
                               text,
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                               detail::ScaleForDpi(x, dpi),
                               detail::ScaleForDpi(y, dpi),
                               detail::ScaleForDpi(width, dpi),
                               detail::ScaleForDpi(height, dpi),
                               window_,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                               GetModuleHandleW(nullptr),
                               nullptr);
    }

    std::wstring ReadName() const {
        const int length = GetWindowTextLengthW(edit_);
        std::wstring value(static_cast<std::size_t>(std::max(0, length)) + 1, L'\0');
        if (length > 0)
            GetWindowTextW(edit_, value.data(), length + 1);
        value.resize(static_cast<std::size_t>(std::max(0, length)));
        return value;
    }

    std::wstring Validate(const std::wstring_view value) const {
        if (value.empty())
            return L"名称不能为空";
        if (value == L"." || value == L"..")
            return L"不能使用此名称";
        if (value.find_first_of(L"/\\") != std::wstring_view::npos) {
            return L"名称不能包含 / 或 \\";
        }
        if (local_path_) {
            if (value.find_first_of(L"<>:\"|?*") != std::wstring_view::npos) {
                return L"本地名称包含 Windows 不允许的字符";
            }
            if (value.back() == L'.' || value.back() == L' ') {
                return L"本地名称不能以句点或空格结尾";
            }
        }
        return {};
    }

    void Accept() {
        std::wstring name = ReadName();
        const std::wstring error = Validate(name);
        if (!error.empty()) {
            SetWindowTextW(error_, error.c_str());
            MessageBeep(MB_ICONWARNING);
            SetFocus(edit_);
            return;
        }
        result_ = std::move(name);
        accepted_ = true;
        DestroyWindow(window_);
    }

    void Cancel() {
        accepted_ = false;
        DestroyWindow(window_);
    }

    HWND owner_{};
    HWND window_{};
    HWND edit_{};
    HWND error_{};
    HFONT font_{};
    std::wstring current_name_;
    std::wstring result_;
    bool local_path_{};
    bool accepted_{};
    bool done_{};
};

} // namespace

UnsavedFileChoice ShowUnsavedFileDialog(HWND owner, std::wstring document_label) {
    const auto result = FilePromptDialog(owner, std::move(document_label)).Run();
    return result == 100   ? UnsavedFileChoice::Save
           : result == 101 ? UnsavedFileChoice::Discard
           : result == 102 ? UnsavedFileChoice::KeepDraft
                           : UnsavedFileChoice::Cancel;
}

int ShowMessageDialog(HWND owner, const wchar_t* message, const wchar_t* title, UINT flags) {
    const auto buttons = flags & MB_TYPEMASK;
    if (buttons != MB_OK && buttons != MB_YESNO)
        return ::MessageBoxW(
            owner, message, title, flags); // 不把未来的取消/重试等按钮误解释为确认。
    const auto result = FilePromptDialog(owner, message, title, flags).Run();
    // 仅在连自绘窗口都无法创建的资源耗尽场景，保留系统错误提示作为最后兜底。
    return result ? result : ::MessageBoxW(owner, message, title, flags);
}

std::optional<PermissionDialogResult> ShowPermissionDialog(const HWND owner,
                                                           std::wstring item_name,
                                                           const std::uint32_t current_permissions,
                                                           const bool directory) {
    return PermissionDialog(owner, std::move(item_name), current_permissions, directory).Run();
}

std::optional<std::wstring>
ShowRenameDialog(const HWND owner, std::wstring current_name, const bool local_path) {
    return RenameDialog(owner, std::move(current_name), local_path).Run();
}

} // namespace SLink::ui
