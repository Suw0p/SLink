// 文件冲突策略对话框实现，显示友好源/目标路径并返回单次或批量应用选择。
#include "ui/transfer_conflict_dialog.hpp"

#include "ui/selector_control.hpp"
#include "ui/win32_ui_internal.hpp"

#include <commctrl.h>
#include <string>
#include <utility>

namespace SLink::ui {
namespace {

std::wstring FromUtf8(const std::string_view input) {
    if (input.empty())
        return {};
    const int length = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()), nullptr, 0);
    if (length <= 0)
        return {};
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8,
                        MB_ERR_INVALID_CHARS,
                        input.data(),
                        static_cast<int>(input.size()),
                        result.data(),
                        length);
    return result;
}

class TransferConflictDialog final {
public:
    TransferConflictDialog(HWND owner,
                           const transfer::FileUri& source,
                           const transfer::FileUri& target,
                           std::wstring source_display,
                           std::wstring target_display)
        : owner_(detail::RootWindowForModalOwner(owner)),
          source_(source_display.empty() ? FromUtf8(source.to_string())
                                         : std::move(source_display)),
          target_(target_display.empty() ? FromUtf8(target.to_string())
                                         : std::move(target_display)),
          name_(FromUtf8(source.filename())) {}

    std::optional<TransferConflictChoice> Run() {
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
        if (!selector::RegisterClass(GetModuleHandleW(nullptr))) {
            return std::nullopt;
        }

        const UINT dpi = detail::WindowDpi(owner_);
        // 冲突信息只有三行内容和一行操作项，使用紧凑尺寸，避免信息之间出现大片空白。
        const int width = detail::ScaleForDpi(580, dpi);
        const int height = detail::ScaleForDpi(320, dpi);
        RECT owner_rect{};
        GetWindowRect(owner_, &owner_rect);
        const int x = owner_rect.left + (owner_rect.right - owner_rect.left - width) / 2;
        const int y = owner_rect.top + (owner_rect.bottom - owner_rect.top - height) / 2;
        window_ = CreateWindowExW(WS_EX_DLGMODALFRAME,
                                  kClassName,
                                  L"文件已存在",
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
        return accepted_ ? std::optional<TransferConflictChoice>(choice_) : std::nullopt;
    }

private:
    static constexpr wchar_t kClassName[] = L"SLink.TransferConflict";
    // IDOK and IDCANCEL are 1 and 2. Keep every child control outside that
    // range so a combo/checkbox notification cannot be mistaken for a dialog
    // action (and so Cancel is not painted as a checkbox).
    static constexpr int kPolicyId = 1001;
    static constexpr int kApplyAllId = 1002;
    static constexpr int kVerifyId = 1003;

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* self =
            reinterpret_cast<TransferConflictDialog*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            self = static_cast<TransferConflictDialog*>(create->lpCreateParams);
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
            if (LOWORD(wparam) == kApplyAllId || LOWORD(wparam) == kVerifyId) {
                if (HIWORD(wparam) != BN_CLICKED) {
                    return 0;
                }
                const HWND checkbox =
                    LOWORD(wparam) == kApplyAllId ? self->apply_all_ : self->verify_;
                detail::SetSelfDrawnCheckboxChecked(checkbox,
                                                    !detail::SelfDrawnCheckboxChecked(checkbox));
                return 0;
            }
            if (LOWORD(wparam) == IDOK) {
                self->Accept();
                return 0;
            }
            if (LOWORD(wparam) == IDCANCEL) {
                self->Cancel();
                return 0;
            }
            break;
        case WM_DRAWITEM: {
            const auto* draw = reinterpret_cast<const DRAWITEMSTRUCT*>(lparam);
            if (draw && (detail::DrawSelfDrawnComboItem(*draw) ||
                         detail::DrawSelfDrawnButton(*draw,
                                                     draw->CtlID == IDOK,
                                                     draw->CtlID == kApplyAllId ||
                                                         draw->CtlID == kVerifyId))) {
                return TRUE;
            }
            break;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
        case WM_CTLCOLORLISTBOX: {
            const HDC dc = reinterpret_cast<HDC>(wparam);
            SetTextColor(dc, detail::kTextColor);
            SetBkColor(dc, detail::kPanelColor);
            return reinterpret_cast<LRESULT>(detail::PanelThemeBrush());
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

    HWND MakeStatic(const wchar_t* text, int x, int y, int width, int height) {
        return CreateWindowExW(0,
                               L"STATIC",
                               text,
                               WS_CHILD | WS_VISIBLE | SS_LEFT,
                               x,
                               y,
                               width,
                               height,
                               window_,
                               nullptr,
                               GetModuleHandleW(nullptr),
                               nullptr);
    }

    HWND MakePathStatic(const wchar_t* text, int x, int y, int width, int height) {
        return CreateWindowExW(0,
                               L"STATIC",
                               text,
                               WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE | SS_PATHELLIPSIS,
                               x,
                               y,
                               width,
                               height,
                               window_,
                               nullptr,
                               GetModuleHandleW(nullptr),
                               nullptr);
    }

    void CreateControls() {
        const UINT dpi = detail::WindowDpi(window_);
        const auto s = [dpi](int value) {
            return detail::ScaleForDpi(value, dpi);
        };
        // 标签列和内容列使用固定起点，各信息行采用一致的紧凑节奏。
        const int left = s(20);
        const int content_left = s(88);
        MakeStatic(L"目标位置已经包含同名对象，请选择本次操作：", left, s(18), s(540), s(24));
        MakeStatic(L"名称：", left, s(54), s(60), s(24));
        MakeStatic(name_.c_str(), content_left, s(54), s(466), s(24));
        MakeStatic(L"目标：", left, s(88), s(60), s(28));
        MakePathStatic(target_.c_str(), content_left, s(86), s(466), s(28));
        MakeStatic(L"源：", left, s(126), s(60), s(28));
        MakePathStatic(source_.c_str(), content_left, s(124), s(466), s(28));
        MakeStatic(L"操作：", left, s(178), s(60), s(28));
        policy_ = CreateWindowExW(0,
                                  selector::kClassName,
                                  L"",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                  content_left,
                                  s(178),
                                  s(210),
                                  s(28),
                                  window_,
                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPolicyId)),
                                  GetModuleHandleW(nullptr),
                                  nullptr);
        for (const wchar_t* text : {L"覆盖", L"跳过", L"重命名"}) {
            SendMessageW(policy_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
        }
        SendMessageW(policy_, CB_SETCURSEL, 0, 0);
        apply_all_ = MakeButton(L"全部应用", kApplyAllId, 310, 178, 108, 28);
        verify_ = MakeButton(L"SHA-256 校验", kVerifyId, 426, 178, 130, 28);
        // 主操作按钮靠右对齐，并与上方内容保持适度分隔。
        MakeButton(L"确定", IDOK, 362, 238, 90, 34);
        MakeButton(L"取消", IDCANCEL, 464, 238, 90, 34);
        font_ = detail::CreateUiFont(dpi, 10);
        EnumChildWindows(
            window_,
            [](HWND child, LPARAM value) -> BOOL {
                detail::ApplySelfDrawnTheme(child);
                detail::SetControlFont(child, reinterpret_cast<HFONT>(value));
                return TRUE;
            },
            reinterpret_cast<LPARAM>(font_));
    }

    HWND MakeButton(const wchar_t* text, int id, int x, int y, int width, int height) const {
        const UINT dpi = detail::WindowDpi(window_);
        const auto s = [dpi](int value) {
            return detail::ScaleForDpi(value, dpi);
        };
        return CreateWindowExW(0,
                               L"BUTTON",
                               text,
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                               s(x),
                               s(y),
                               s(width),
                               s(height),
                               window_,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                               GetModuleHandleW(nullptr),
                               nullptr);
    }

    void Accept() {
        const LRESULT selected = SendMessageW(policy_, CB_GETCURSEL, 0, 0);
        choice_.policy = selected == 1   ? transfer::ConflictPolicy::Skip
                         : selected == 2 ? transfer::ConflictPolicy::AutoRename
                                         : transfer::ConflictPolicy::Overwrite;
        choice_.apply_all = detail::SelfDrawnCheckboxChecked(apply_all_);
        choice_.verify_sha256 = detail::SelfDrawnCheckboxChecked(verify_);
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
    HWND policy_{};
    HWND apply_all_{};
    HWND verify_{};
    HFONT font_{};
    std::wstring source_;
    std::wstring target_;
    std::wstring name_;
    TransferConflictChoice choice_;
    bool accepted_{};
    bool done_{};
};

} // namespace

std::optional<TransferConflictChoice> ShowTransferConflictDialog(const HWND owner,
                                                                 const transfer::FileUri& source,
                                                                 const transfer::FileUri& target,
                                                                 std::wstring source_display,
                                                                 std::wstring target_display) {
    return TransferConflictDialog(
               owner, source, target, std::move(source_display), std::move(target_display))
        .Run();
}

} // namespace SLink::ui
