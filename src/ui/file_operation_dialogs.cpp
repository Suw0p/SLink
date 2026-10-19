// 权限与重命名模态对话框的自绘布局、输入校验及 owner 窗口激活行为实现。
#include "ui/file_operation_dialogs.hpp"

#include "ui/win32_ui_internal.hpp"

#include <array>
#include <cwchar>
#include <string_view>
#include <utility>

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

        detail::EnableDarkTitleBar(window_);
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

        detail::EnableDarkTitleBar(window_);
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
