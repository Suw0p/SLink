// 服务器管理及新建/编辑模态窗口实现，同时负责稳定凭据容器的编解码和 Vault 提交。
// profile 与 credential 的写入按业务顺序处理，任何敏感字节都不进入 state.db。
#include "app/server_manager_dialog.hpp"

#include "ui/selector_control.hpp"
#include "ui/table_control.hpp"
#include "ui/theme.hpp"
#include "ui/win32_ui_internal.hpp"

#include <algorithm>
#include <array>
#include <commctrl.h>
#include <commdlg.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <dwmapi.h>
#include <limits>
#include <objbase.h>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <uxtheme.h>
#include <vector>

#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "Comdlg32.lib")
#pragma comment(lib, "Dwmapi.lib")
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "Uxtheme.lib")

namespace SLink::app {
namespace {

using ui::theme::kAccentColor;
using ui::theme::kBorderColor;
using ui::theme::kMutedTextColor;
using ui::theme::kPanelAltColor;
using ui::theme::kPanelColor;
using ui::theme::kTextColor;
using ui::theme::kWindowColor;

HBRUSH WindowBrush() {
    static const HBRUSH dark = CreateSolidBrush(ui::theme::kDarkPalette.window);
    static const HBRUSH light = CreateSolidBrush(ui::theme::kLightPalette.window);
    return ui::theme::IsDark() ? dark : light;
}

HBRUSH PanelAltBrush() {
    static const HBRUSH dark = CreateSolidBrush(ui::theme::kDarkPalette.panel_alt);
    static const HBRUSH light = CreateSolidBrush(ui::theme::kLightPalette.panel_alt);
    return ui::theme::IsDark() ? dark : light;
}

constexpr std::array<std::byte, 8> kCredentialMagic{std::byte{'S'},
                                                    std::byte{'W'},
                                                    std::byte{'B'},
                                                    std::byte{'C'},
                                                    std::byte{'R'},
                                                    std::byte{'E'},
                                                    std::byte{'D'},
                                                    std::byte{0}};
constexpr std::uint16_t kCredentialVersion = 1;
constexpr std::size_t kMaximumPasswordBytes = 64U * 1024U;
constexpr std::size_t kMaximumPrivateKeyBytes = 8U * 1024U * 1024U;
constexpr std::size_t kMaximumCredentialBytes =
    kMaximumPrivateKeyBytes + (2U * kMaximumPasswordBytes) + 256U;

enum class CredentialField : std::uint8_t {
    Password = 1,
    PrivateKey = 2,
    PrivateKeyPassphrase = 3,
};

storage::StorageStatus InvalidCredential(std::wstring message) {
    return storage::StorageStatus::Error(storage::StorageErrc::invalid_argument,
                                         std::move(message));
}

storage::StorageStatus CorruptCredential(std::wstring message) {
    return storage::StorageStatus::Error(storage::StorageErrc::corrupt_data, std::move(message));
}

template <typename UInt>
void WriteUnsigned(std::span<std::byte> destination, std::size_t& cursor, UInt value) {
    static_assert(std::is_unsigned_v<UInt>);
    for (std::size_t index = 0; index < sizeof(UInt); ++index) {
        destination[cursor++] = static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
    }
}

template <typename UInt>
bool ReadUnsigned(std::span<const std::byte> source, std::size_t& cursor, UInt& value) {
    static_assert(std::is_unsigned_v<UInt>);
    if (cursor > source.size() || source.size() - cursor < sizeof(UInt)) {
        return false;
    }
    value = 0;
    for (std::size_t index = 0; index < sizeof(UInt); ++index) {
        value |= static_cast<UInt>(std::to_integer<unsigned char>(source[cursor + index]))
                 << (index * 8U);
    }
    cursor += sizeof(UInt);
    return true;
}

storage::StorageResult<storage::SecureBuffer> CopySecure(std::span<const std::byte> source) {
    try {
        storage::SecureBuffer destination(source.size());
        if (!source.empty()) {
            std::memcpy(destination.bytes().data(), source.data(), source.size());
        }
        return storage::StorageResult<storage::SecureBuffer>::Success(std::move(destination));
    } catch (...) {
        return storage::StorageResult<storage::SecureBuffer>::Failure(storage::StorageStatus::Error(
            storage::StorageErrc::backend_error, L"无法分配锁定内存来保存凭据。"));
    }
}

std::wstring Utf8ToWide(std::string_view value) {
    if (value.empty()) {
        return {};
    }
    const int required = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(
            (std::min)(value.size(), static_cast<std::size_t>((std::numeric_limits<int>::max)()))),
        nullptr,
        0);
    if (required <= 0) {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8,
                            MB_ERR_INVALID_CHARS,
                            value.data(),
                            static_cast<int>(value.size()),
                            result.data(),
                            required) != required) {
        return {};
    }
    return result;
}

std::optional<std::string> WideToUtf8(std::wstring_view value) {
    if (value.empty()) {
        return std::string{};
    }
    if (value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return std::nullopt;
    }
    const int required = WideCharToMultiByte(CP_UTF8,
                                             WC_ERR_INVALID_CHARS,
                                             value.data(),
                                             static_cast<int>(value.size()),
                                             nullptr,
                                             0,
                                             nullptr,
                                             nullptr);
    if (required <= 0) {
        return std::nullopt;
    }
    std::string result(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8,
                            WC_ERR_INVALID_CHARS,
                            value.data(),
                            static_cast<int>(value.size()),
                            result.data(),
                            required,
                            nullptr,
                            nullptr) != required) {
        return std::nullopt;
    }
    return result;
}

std::wstring WindowText(HWND control) {
    const int length = GetWindowTextLengthW(control);
    if (length <= 0) {
        return {};
    }
    std::wstring text(static_cast<std::size_t>(length) + 1U, L'\0');
    const int copied = GetWindowTextW(control, text.data(), length + 1);
    text.resize(copied > 0 ? static_cast<std::size_t>(copied) : 0U);
    return text;
}

storage::StorageResult<storage::SecureBuffer> SecretControlUtf8(HWND control) {
    const int length = GetWindowTextLengthW(control);
    if (length <= 0) {
        SetWindowTextW(control, L"");
        return storage::StorageResult<storage::SecureBuffer>::Success({});
    }

    try {
        storage::SecureBuffer wide_bytes((static_cast<std::size_t>(length) + 1U) * sizeof(wchar_t));
        auto* wide_text = reinterpret_cast<wchar_t*>(wide_bytes.bytes().data());
        const int copied = GetWindowTextW(control, wide_text, length + 1);
        SetWindowTextW(control, L"");
        if (copied <= 0) {
            return storage::StorageResult<storage::SecureBuffer>::Failure(
                InvalidCredential(L"无法读取密码输入框。"));
        }

        const int required = WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, wide_text, copied, nullptr, 0, nullptr, nullptr);
        if (required <= 0) {
            return storage::StorageResult<storage::SecureBuffer>::Failure(
                InvalidCredential(L"密码包含无效的 Unicode 字符。"));
        }
        storage::SecureBuffer utf8(static_cast<std::size_t>(required));
        if (WideCharToMultiByte(CP_UTF8,
                                WC_ERR_INVALID_CHARS,
                                wide_text,
                                copied,
                                reinterpret_cast<char*>(utf8.bytes().data()),
                                required,
                                nullptr,
                                nullptr) != required) {
            return storage::StorageResult<storage::SecureBuffer>::Failure(
                InvalidCredential(L"无法转换密码编码。"));
        }
        return storage::StorageResult<storage::SecureBuffer>::Success(std::move(utf8));
    } catch (...) {
        SetWindowTextW(control, L"");
        return storage::StorageResult<storage::SecureBuffer>::Failure(storage::StorageStatus::Error(
            storage::StorageErrc::backend_error, L"无法分配锁定内存来读取密码。"));
    }
}

void ShowStorageError(HWND owner,
                      std::wstring_view operation,
                      const storage::StorageStatus& status) {
    std::wstring message(operation);
    if (!status.message.empty()) {
        message += L"\r\n\r\n";
        message += status.message;
    }
    MessageBoxW(owner, message.c_str(), L"SLink", MB_OK | MB_ICONERROR);
}

UINT WindowDpi(HWND window) {
    using GetDpiForWindowFunction = UINT(WINAPI*)(HWND);
    const auto function = reinterpret_cast<GetDpiForWindowFunction>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    return function ? function(window) : 96U;
}

int Scale(int value, UINT dpi) {
    return MulDiv(value, static_cast<int>(dpi), 96);
}

// The profile editor intentionally uses a denser layout than the manager.
// Keep this separate from DPI scaling so its physical size remains 75% of
// the original design at every DPI.
int ProfileLayoutScale(int value, UINT dpi) {
    return MulDiv(value, static_cast<int>(dpi) * 3, 96 * 4);
}

HFONT CreateDialogFont(UINT dpi, bool bold = false) {
    return CreateFontW(-Scale(14, dpi),
                       0,
                       0,
                       0,
                       bold ? FW_SEMIBOLD : FW_NORMAL,
                       FALSE,
                       FALSE,
                       FALSE,
                       DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE,
                       L"Segoe UI");
}

void SetFont(HWND control, HFONT font) {
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

void EnableDarkWindow(HWND window) {
    BOOL enabled = ui::theme::IsDark() ? TRUE : FALSE;
    constexpr DWORD kUseImmersiveDarkMode = 20;
    if (FAILED(DwmSetWindowAttribute(window, kUseImmersiveDarkMode, &enabled, sizeof(enabled)))) {
        constexpr DWORD kLegacyUseImmersiveDarkMode = 19;
        DwmSetWindowAttribute(window, kLegacyUseImmersiveDarkMode, &enabled, sizeof(enabled));
    }
    // Windows 11 otherwise keeps a light one-pixel DWM frame around these
    // compact dialogs even though their caption and client area are dark.
    constexpr DWORD kDwmBorderColor = 34;
    const COLORREF border_color = kBorderColor;
    (void)DwmSetWindowAttribute(window, kDwmBorderColor, &border_color, sizeof(border_color));
}

void ApplyDarkControl(HWND control) {
    ui::detail::ApplySelfDrawnTheme(control);
}

bool DrawDialogButton(const DRAWITEMSTRUCT& draw, HFONT font, bool primary) {
    if (!draw.hDC || draw.CtlType != ODT_BUTTON) {
        return false;
    }
    RECT rect = draw.rcItem;
    const bool disabled = (draw.itemState & ODS_DISABLED) != 0;
    const bool pressed = (draw.itemState & ODS_SELECTED) != 0;
    const COLORREF fill_color = disabled ? kPanelColor
                                : primary
                                    ? (pressed ? RGB(59, 103, 174) : kAccentColor)
                                    : (pressed ? ui::theme::kPanelHoverColor : kPanelAltColor);
    const COLORREF border_color = primary && !disabled ? kAccentColor : kBorderColor;
    const HBRUSH fill = CreateSolidBrush(fill_color);
    const HPEN border = CreatePen(PS_SOLID, 1, border_color);
    const HGDIOBJ old_brush = SelectObject(draw.hDC, fill);
    const HGDIOBJ old_pen = SelectObject(draw.hDC, border);
    RoundRect(draw.hDC, rect.left, rect.top, rect.right, rect.bottom, 8, 8);
    SelectObject(draw.hDC, old_pen);
    SelectObject(draw.hDC, old_brush);
    DeleteObject(border);
    DeleteObject(fill);

    wchar_t text[128]{};
    GetWindowTextW(draw.hwndItem, text, static_cast<int>(std::size(text)));
    SetBkMode(draw.hDC, TRANSPARENT);
    SetTextColor(draw.hDC, disabled ? kMutedTextColor : kTextColor);
    const HGDIOBJ old_font = font ? SelectObject(draw.hDC, font) : nullptr;
    DrawTextW(draw.hDC, text, -1, &rect, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
    if (old_font) {
        SelectObject(draw.hDC, old_font);
    }
    if ((draw.itemState & ODS_FOCUS) != 0 && !disabled) {
        RECT focus = rect;
        InflateRect(&focus, -4, -4);
        DrawFocusRect(draw.hDC, &focus);
    }
    return true;
}

LRESULT DialogControlColor(UINT message, WPARAM wparam) {
    const HDC dc = reinterpret_cast<HDC>(wparam);
    SetTextColor(dc, kTextColor);
    if (message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX) {
        SetBkColor(dc, kPanelAltColor);
        return reinterpret_cast<LRESULT>(PanelAltBrush());
    }
    SetBkMode(dc, TRANSPARENT);
    SetBkColor(dc, kWindowColor);
    return reinterpret_cast<LRESULT>(WindowBrush());
}

bool DrawDarkComboItem(const DRAWITEMSTRUCT& draw, HFONT font) {
    if (!draw.hDC || draw.CtlType != ODT_COMBOBOX) {
        return false;
    }

    const bool selected = (draw.itemState & ODS_SELECTED) != 0;
    const bool disabled = (draw.itemState & ODS_DISABLED) != 0;
    const COLORREF background = selected ? RGB(47, 79, 128) : kPanelAltColor;
    const HBRUSH brush = CreateSolidBrush(background);
    FillRect(draw.hDC, &draw.rcItem, brush);
    DeleteObject(brush);

    if (draw.itemID != static_cast<UINT>(-1)) {
        wchar_t text[128]{};
        SendMessageW(draw.hwndItem, CB_GETLBTEXT, draw.itemID, reinterpret_cast<LPARAM>(text));
        RECT text_rect = draw.rcItem;
        text_rect.left += Scale(8, WindowDpi(draw.hwndItem));
        text_rect.right -= Scale(4, WindowDpi(draw.hwndItem));
        SetBkMode(draw.hDC, TRANSPARENT);
        SetTextColor(draw.hDC, disabled ? kMutedTextColor : kTextColor);
        const HGDIOBJ old_font = font ? SelectObject(draw.hDC, font) : nullptr;
        DrawTextW(draw.hDC,
                  text,
                  -1,
                  &text_rect,
                  DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
        if (old_font) {
            SelectObject(draw.hDC, old_font);
        }
    }
    return true;
}

void CenterWindow(HWND window, HWND owner) {
    RECT window_rect{};
    RECT owner_rect{};
    GetWindowRect(window, &window_rect);
    if (!owner || !GetWindowRect(owner, &owner_rect)) {
        owner_rect = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    }
    const int width = window_rect.right - window_rect.left;
    const int height = window_rect.bottom - window_rect.top;
    const int left = owner_rect.left + ((owner_rect.right - owner_rect.left - width) / 2);
    const int top = owner_rect.top + ((owner_rect.bottom - owner_rect.top - height) / 2);
    SetWindowPos(window, HWND_TOP, left, top, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
}

void RaiseModalOwnerChain(HWND window) {
    ui::detail::RaiseOwnedWindowGroup(window);
}

void CloseModalWindow(HWND window) {
    if (!window || !IsWindow(window))
        return;
    ui::detail::PrepareModalWindowClose(window);
    DestroyWindow(window);
}

void RunModalWindow(HWND window, HWND owner) {
    const bool owner_enabled = owner && IsWindowEnabled(owner);
    if (owner_enabled) {
        EnableWindow(owner, FALSE);
    }
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);

    MSG message{};
    bool quit_received = false;
    int quit_code = 0;
    while (IsWindow(window)) {
        const BOOL result = GetMessageW(&message, nullptr, 0, 0);
        if (result <= 0) {
            if (result == 0) {
                quit_received = true;
                quit_code = static_cast<int>(message.wParam);
            }
            break;
        }
        if (!IsDialogMessageW(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    if (IsWindow(window)) {
        CloseModalWindow(window);
    }
    if (owner_enabled && owner && IsWindow(owner)) {
        EnableWindow(owner, TRUE);
    }
    if (!quit_received) {
        ui::detail::ActivateModalOwner(owner);
    }
    if (quit_received) {
        PostQuitMessage(quit_code);
    }
}

bool EnsureVaultUnlocked(HWND owner, storage::Vault* vault) {
    if (!vault || !storage::Vault::IsSupported()) {
        MessageBoxW(owner,
                    L"当前构建没有可用的加密凭据库，无法保存密码或私钥。\r\n"
                    L"请使用包含 libsodium 的正式构建。",
                    L"SLink",
                    MB_OK | MB_ICONERROR);
        return false;
    }
    if (vault->is_unlocked()) {
        return true;
    }
    const auto status = vault->UnlockPortable();
    if (!status) {
        ShowStorageError(owner, L"无法打开便携凭据库。", status);
        return false;
    }
    return true;
}

std::string NewServerId() {
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid))) {
        const ULONGLONG ticks = GetTickCount64();
        return "server-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(ticks);
    }
    constexpr char digits[] = "0123456789abcdef";
    const auto bytes =
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(&guid), sizeof(guid));
    std::string result = "server-";
    result.reserve(result.size() + (bytes.size() * 2U));
    for (const std::byte value : bytes) {
        const unsigned char raw = std::to_integer<unsigned char>(value);
        result.push_back(digits[raw >> 4U]);
        result.push_back(digits[raw & 0x0fU]);
    }
    return result;
}

std::wstring AuthenticationName(storage::AuthenticationKind kind) {
    switch (kind) {
    case storage::AuthenticationKind::Password:
        return L"密码";
    case storage::AuthenticationKind::PrivateKey:
        return L"私钥";
    case storage::AuthenticationKind::OpenSshAgent:
        return L"OpenSSH Agent";
    case storage::AuthenticationKind::KeyboardInteractive:
        return L"Keyboard-interactive";
    }
    return L"未知";
}

storage::AuthenticationKind AuthenticationFromCombo(int selection) {
    switch (selection) {
    case 1:
        return storage::AuthenticationKind::PrivateKey;
    case 2:
        return storage::AuthenticationKind::OpenSshAgent;
    case 3:
        return storage::AuthenticationKind::KeyboardInteractive;
    default:
        return storage::AuthenticationKind::Password;
    }
}

int AuthenticationComboIndex(storage::AuthenticationKind kind) {
    switch (kind) {
    case storage::AuthenticationKind::Password:
        return 0;
    case storage::AuthenticationKind::PrivateKey:
        return 1;
    case storage::AuthenticationKind::OpenSshAgent:
        return 2;
    case storage::AuthenticationKind::KeyboardInteractive:
        return 3;
    }
    return 0;
}

storage::StorageResult<storage::SecureBuffer> ReadPrivateKeyFile(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(),
                              GENERIC_READ,
                              FILE_SHARE_READ,
                              nullptr,
                              OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return storage::StorageResult<storage::SecureBuffer>::Failure(storage::StorageStatus::Error(
            storage::StorageErrc::io_error,
            L"无法打开所选私钥文件。Windows 错误：" + std::to_wstring(GetLastError())));
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 ||
        static_cast<unsigned long long>(size.QuadPart) > kMaximumPrivateKeyBytes) {
        CloseHandle(file);
        return storage::StorageResult<storage::SecureBuffer>::Failure(
            InvalidCredential(L"私钥文件必须大于 0 且不超过 8 MB。"));
    }
    try {
        storage::SecureBuffer content(static_cast<std::size_t>(size.QuadPart));
        std::size_t offset = 0;
        while (offset < content.size()) {
            const DWORD chunk = static_cast<DWORD>(
                (std::min)(content.size() - offset,
                           static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
            DWORD read = 0;
            if (!ReadFile(file, content.bytes().data() + offset, chunk, &read, nullptr) ||
                read == 0) {
                const DWORD error = GetLastError();
                CloseHandle(file);
                return storage::StorageResult<storage::SecureBuffer>::Failure(
                    storage::StorageStatus::Error(storage::StorageErrc::io_error,
                                                  L"读取私钥文件失败。Windows 错误：" +
                                                      std::to_wstring(error)));
            }
            offset += read;
        }
        CloseHandle(file);
        return storage::StorageResult<storage::SecureBuffer>::Success(std::move(content));
    } catch (...) {
        CloseHandle(file);
        return storage::StorageResult<storage::SecureBuffer>::Failure(storage::StorageStatus::Error(
            storage::StorageErrc::backend_error, L"无法分配锁定内存来读取私钥。"));
    }
}

enum class CredentialUpdate {
    Keep,
    Clear,
    Password,
    PrivateKey,
};

struct ProfileEditResult {
    storage::ServerProfile profile;
    CredentialUpdate credential_update{CredentialUpdate::Keep};
    storage::SecureBuffer primary_secret;
    storage::SecureBuffer secondary_secret;
};

constexpr wchar_t kProfileEditorClass[] = L"SLink.ServerManager.ProfileEditor";
constexpr int kNameId = 3001;
constexpr int kHostId = 3002;
constexpr int kPortId = 3003;
constexpr int kUsernameId = 3004;
constexpr int kAuthenticationId = 3005;
constexpr int kPasswordId = 3006;
constexpr int kPrivateKeyPathId = 3007;
constexpr int kBrowsePrivateKeyId = 3008;
constexpr int kPassphraseId = 3009;
constexpr int kInitialDirectoryId = 3010;
constexpr int kProfileGroupId = 3011;

// 新建/编辑窗口的短生命周期状态。敏感输入在提交时编码进 SecureBuffer，窗口销毁前
// 清理临时字节；组合框和按钮只负责 UI，不直接修改数据库。
struct ProfileEditorState {
    HWND owner{};
    storage::ServerProfile initial;
    std::vector<storage::ServerGroup> groups;
    bool editing{};
    HWND name{};
    HWND host{};
    HWND port{};
    HWND username{};
    HWND initial_directory{};
    HWND group{};
    HWND authentication{};
    HWND credential_label{};
    HWND password{};
    HWND private_key_path{};
    HWND browse_private_key{};
    HWND passphrase_label{};
    HWND passphrase{};
    HFONT font{};
    storage::SecureBuffer selected_private_key;
    std::optional<ProfileEditResult> result;

    ~ProfileEditorState() {
        if (password && IsWindow(password)) {
            SetWindowTextW(password, L"");
        }
        if (passphrase && IsWindow(passphrase)) {
            SetWindowTextW(passphrase, L"");
        }
        DeleteObject(font);
    }
};

void ShowAuthenticationFields(ProfileEditorState& state) {
    const auto authentication = AuthenticationFromCombo(
        static_cast<int>(SendMessageW(state.authentication, CB_GETCURSEL, 0, 0)));
    const bool password = authentication == storage::AuthenticationKind::Password;
    const bool private_key = authentication == storage::AuthenticationKind::PrivateKey;
    ShowWindow(state.credential_label, password || private_key ? SW_SHOW : SW_HIDE);
    ShowWindow(state.password, password ? SW_SHOW : SW_HIDE);
    ShowWindow(state.private_key_path, private_key ? SW_SHOW : SW_HIDE);
    ShowWindow(state.browse_private_key, private_key ? SW_SHOW : SW_HIDE);
    ShowWindow(state.passphrase_label, private_key ? SW_SHOW : SW_HIDE);
    ShowWindow(state.passphrase, private_key ? SW_SHOW : SW_HIDE);
    const UINT dpi = WindowDpi(state.authentication);
    if (password) {
        MoveWindow(state.credential_label,
                   ProfileLayoutScale(20, dpi),
                   ProfileLayoutScale(344, dpi),
                   ProfileLayoutScale(122, dpi),
                   Scale(23, dpi),
                   TRUE);
        SetWindowTextW(state.credential_label, L"密码");
    } else if (private_key) {
        MoveWindow(state.credential_label,
                   ProfileLayoutScale(20, dpi),
                   ProfileLayoutScale(344, dpi),
                   ProfileLayoutScale(122, dpi),
                   Scale(23, dpi),
                   TRUE);
        SetWindowTextW(state.credential_label, L"私钥文件");
    }
}

void ClearProfileSecretControls(ProfileEditorState& state) {
    if (state.password) {
        SetWindowTextW(state.password, L"");
    }
    if (state.passphrase) {
        SetWindowTextW(state.passphrase, L"");
    }
}

bool ValidateAndCloseProfileEditor(HWND window, ProfileEditorState& state) {
    auto name = WideToUtf8(WindowText(state.name));
    auto host = WideToUtf8(WindowText(state.host));
    auto username = WideToUtf8(WindowText(state.username));
    auto initial_directory = WideToUtf8(WindowText(state.initial_directory));
    const std::wstring port_text = WindowText(state.port);
    if (!name || name->empty()) {
        MessageBoxW(window, L"请输入服务器名称。", L"SLink", MB_OK | MB_ICONWARNING);
        SetFocus(state.name);
        return false;
    }
    if (!host || host->empty()) {
        MessageBoxW(window, L"请输入服务器地址。", L"SLink", MB_OK | MB_ICONWARNING);
        SetFocus(state.host);
        return false;
    }
    if (!username || username->empty()) {
        MessageBoxW(window, L"请输入用户名。", L"SLink", MB_OK | MB_ICONWARNING);
        SetFocus(state.username);
        return false;
    }
    if (!initial_directory) {
        MessageBoxW(window, L"初始目录包含无效字符。", L"SLink", MB_OK | MB_ICONWARNING);
        SetFocus(state.initial_directory);
        return false;
    }
    wchar_t* port_end = nullptr;
    const unsigned long parsed_port = std::wcstoul(port_text.c_str(), &port_end, 10);
    if (port_text.empty() || !port_end || *port_end != L'\0' || parsed_port == 0 ||
        parsed_port > 65535UL) {
        MessageBoxW(window, L"端口必须是 1–65535 之间的数字。", L"SLink", MB_OK | MB_ICONWARNING);
        SetFocus(state.port);
        return false;
    }

    ProfileEditResult output;
    output.profile = state.initial;
    output.profile.name = std::move(*name);
    output.profile.host = std::move(*host);
    output.profile.username = std::move(*username);
    output.profile.initial_directory = std::move(*initial_directory);
    output.profile.port = static_cast<std::uint16_t>(parsed_port);
    const auto group_selection = static_cast<int>(SendMessageW(state.group, CB_GETCURSEL, 0, 0));
    if (group_selection < 0 || static_cast<std::size_t>(group_selection) > state.groups.size()) {
        MessageBoxW(window, L"请选择有效的服务器分组。", L"SLink", MB_OK | MB_ICONWARNING);
        return false;
    }
    output.profile.group_id =
        group_selection == 0 ? std::nullopt
                             : std::optional<std::string>(
                                   state.groups[static_cast<std::size_t>(group_selection - 1)].id);
    output.profile.authentication = AuthenticationFromCombo(
        static_cast<int>(SendMessageW(state.authentication, CB_GETCURSEL, 0, 0)));

    switch (output.profile.authentication) {
    case storage::AuthenticationKind::Password: {
        auto password = SecretControlUtf8(state.password);
        if (!password) {
            ShowStorageError(window, L"无法读取密码。", password.status());
            return false;
        }
        if (!password.value().empty()) {
            output.credential_update = CredentialUpdate::Password;
            output.primary_secret = std::move(password).value();
        } else if (state.editing &&
                   state.initial.authentication == storage::AuthenticationKind::Password &&
                   state.initial.credential_ref) {
            output.credential_update = CredentialUpdate::Keep;
        } else {
            output.credential_update = CredentialUpdate::Clear;
        }
        break;
    }
    case storage::AuthenticationKind::PrivateKey: {
        if (!state.selected_private_key.empty()) {
            auto passphrase = SecretControlUtf8(state.passphrase);
            if (!passphrase) {
                ShowStorageError(window, L"无法读取私钥口令。", passphrase.status());
                return false;
            }
            output.credential_update = CredentialUpdate::PrivateKey;
            output.primary_secret = std::move(state.selected_private_key);
            output.secondary_secret = std::move(passphrase).value();
        } else if (state.editing &&
                   state.initial.authentication == storage::AuthenticationKind::PrivateKey &&
                   state.initial.credential_ref) {
            output.credential_update = CredentialUpdate::Keep;
        } else {
            MessageBoxW(window, L"请选择私钥文件。", L"SLink", MB_OK | MB_ICONWARNING);
            SetFocus(state.browse_private_key);
            return false;
        }
        break;
    }
    case storage::AuthenticationKind::OpenSshAgent:
    case storage::AuthenticationKind::KeyboardInteractive:
        output.credential_update = CredentialUpdate::Clear;
        break;
    }
    ClearProfileSecretControls(state);
    state.result.emplace(std::move(output));
    CloseModalWindow(window);
    return true;
}

LRESULT CALLBACK ProfileEditorProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<ProfileEditorState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        state = static_cast<ProfileEditorState*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (!state) {
        return DefWindowProcW(window, message, wparam, lparam);
    }

    switch (message) {
    case WM_ACTIVATE:
        if (LOWORD(wparam) != WA_INACTIVE) {
            RaiseModalOwnerChain(window);
        }
        break;
    case WM_CREATE: {
        const UINT dpi = WindowDpi(window);
        EnableDarkWindow(window);
        state->font = CreateDialogFont(dpi);
        constexpr std::array labels{
            L"名称", L"主机", L"端口", L"用户名", L"初始目录", L"分组", L"认证方式"};
        constexpr std::array<int, labels.size()> label_y{22, 67, 112, 157, 202, 247, 292};
        for (std::size_t index = 0; index < labels.size(); ++index) {
            HWND label = CreateWindowExW(0,
                                         L"STATIC",
                                         labels[index],
                                         WS_CHILD | WS_VISIBLE,
                                         ProfileLayoutScale(20, dpi),
                                         ProfileLayoutScale(label_y[index], dpi),
                                         ProfileLayoutScale(122, dpi),
                                         Scale(24, dpi),
                                         window,
                                         nullptr,
                                         nullptr,
                                         nullptr);
            SetFont(label, state->font);
        }
        const auto create_edit = [&](int id, int y, DWORD style = 0) {
            HWND control = CreateWindowExW(0,
                                           L"EDIT",
                                           L"",
                                           WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL |
                                               WS_BORDER | style,
                                           ProfileLayoutScale(150, dpi),
                                           ProfileLayoutScale(y, dpi),
                                           ProfileLayoutScale(420, dpi),
                                           Scale(27, dpi),
                                           window,
                                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                           nullptr,
                                           nullptr);
            SetFont(control, state->font);
            return control;
        };
        state->name = create_edit(kNameId, 18);
        state->host = create_edit(kHostId, 63);
        state->port = create_edit(kPortId, 108, ES_NUMBER);
        state->username = create_edit(kUsernameId, 153);
        state->initial_directory = create_edit(kInitialDirectoryId, 198);
        state->group =
            CreateWindowExW(0,
                            ui::selector::kClassName,
                            L"",
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                            ProfileLayoutScale(150, dpi),
                            ProfileLayoutScale(243, dpi),
                            ProfileLayoutScale(420, dpi),
                            Scale(27, dpi),
                            window,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kProfileGroupId)),
                            nullptr,
                            nullptr);
        SetFont(state->group, state->font);
        ui::detail::ApplySelfDrawnTheme(state->group);
        SendMessageW(state->group, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"未分组"));
        for (const auto& group : state->groups) {
            const auto label = Utf8ToWide(group.name);
            SendMessageW(state->group, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        }
        state->authentication =
            CreateWindowExW(0,
                            ui::selector::kClassName,
                            L"",
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                            ProfileLayoutScale(150, dpi),
                            ProfileLayoutScale(288, dpi),
                            ProfileLayoutScale(420, dpi),
                            Scale(27, dpi),
                            window,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAuthenticationId)),
                            nullptr,
                            nullptr);
        SetFont(state->authentication, state->font);
        ui::detail::ApplySelfDrawnTheme(state->authentication);
        SendMessageW(state->authentication, CB_SETITEMHEIGHT, 0, Scale(24, dpi));
        SendMessageW(
            state->authentication, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), Scale(25, dpi));
        for (const wchar_t* label :
             {L"密码", L"私钥", L"Windows OpenSSH Agent", L"Keyboard-interactive"}) {
            SendMessageW(state->authentication, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
        }

        state->credential_label = CreateWindowExW(0,
                                                  L"STATIC",
                                                  L"密码",
                                                  WS_CHILD | WS_VISIBLE,
                                                  ProfileLayoutScale(20, dpi),
                                                  ProfileLayoutScale(344, dpi),
                                                  ProfileLayoutScale(250, dpi),
                                                  Scale(23, dpi),
                                                  window,
                                                  nullptr,
                                                  nullptr,
                                                  nullptr);
        state->password = create_edit(kPasswordId, 340, ES_PASSWORD);
        state->private_key_path =
            CreateWindowExW(0,
                            L"EDIT",
                            L"",
                            WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL | ES_READONLY | WS_BORDER,
                            ProfileLayoutScale(150, dpi),
                            ProfileLayoutScale(340, dpi),
                            ProfileLayoutScale(324, dpi),
                            Scale(27, dpi),
                            window,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPrivateKeyPathId)),
                            nullptr,
                            nullptr);
        state->browse_private_key =
            CreateWindowExW(0,
                            L"BUTTON",
                            L"选择…",
                            WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
                            ProfileLayoutScale(482, dpi),
                            ProfileLayoutScale(339, dpi),
                            ProfileLayoutScale(88, dpi),
                            Scale(29, dpi),
                            window,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kBrowsePrivateKeyId)),
                            nullptr,
                            nullptr);
        state->passphrase_label = CreateWindowExW(0,
                                                  L"STATIC",
                                                  L"私钥口令",
                                                  WS_CHILD,
                                                  ProfileLayoutScale(20, dpi),
                                                  ProfileLayoutScale(388, dpi),
                                                  ProfileLayoutScale(122, dpi),
                                                  Scale(24, dpi),
                                                  window,
                                                  nullptr,
                                                  nullptr,
                                                  nullptr);
        state->passphrase = create_edit(kPassphraseId, 384, ES_PASSWORD);
        for (HWND control : {state->credential_label,
                             state->private_key_path,
                             state->browse_private_key,
                             state->passphrase_label}) {
            SetFont(control, state->font);
        }
        HWND save = CreateWindowExW(0,
                                    L"BUTTON",
                                    L"保存",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                    ProfileLayoutScale(386, dpi),
                                    ProfileLayoutScale(434, dpi),
                                    ProfileLayoutScale(88, dpi),
                                    Scale(30, dpi),
                                    window,
                                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDOK)),
                                    nullptr,
                                    nullptr);
        HWND cancel = CreateWindowExW(0,
                                      L"BUTTON",
                                      L"取消",
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                      ProfileLayoutScale(482, dpi),
                                      ProfileLayoutScale(434, dpi),
                                      ProfileLayoutScale(88, dpi),
                                      Scale(30, dpi),
                                      window,
                                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDCANCEL)),
                                      nullptr,
                                      nullptr);
        for (HWND control : {save, cancel}) {
            SetFont(control, state->font);
        }
        for (HWND control : {state->name,
                             state->host,
                             state->port,
                             state->username,
                             state->initial_directory,
                             state->password,
                             state->private_key_path,
                             state->browse_private_key,
                             state->passphrase}) {
            ApplyDarkControl(control);
        }
        ui::detail::ApplySelfDrawnTheme(state->authentication);
        ui::detail::ApplySelfDrawnTheme(state->group);
        SendMessageW(state->name, EM_SETLIMITTEXT, 256, 0);
        SendMessageW(state->host, EM_SETLIMITTEXT, 1024, 0);
        SendMessageW(state->port, EM_SETLIMITTEXT, 5, 0);
        SendMessageW(state->username, EM_SETLIMITTEXT, 256, 0);
        SendMessageW(state->initial_directory, EM_SETLIMITTEXT, 4096, 0);
        SendMessageW(state->password, EM_SETLIMITTEXT, 65535, 0);
        SendMessageW(state->passphrase, EM_SETLIMITTEXT, 65535, 0);
        SetWindowTextW(state->name, Utf8ToWide(state->initial.name).c_str());
        SetWindowTextW(state->host, Utf8ToWide(state->initial.host).c_str());
        SetWindowTextW(state->port, std::to_wstring(state->initial.port).c_str());
        SetWindowTextW(state->username, Utf8ToWide(state->initial.username).c_str());
        SetWindowTextW(state->initial_directory,
                       Utf8ToWide(state->initial.initial_directory).c_str());
        SendMessageW(state->authentication,
                     CB_SETCURSEL,
                     static_cast<WPARAM>(AuthenticationComboIndex(state->initial.authentication)),
                     0);
        int group_index = 0;
        for (std::size_t index = 0; index < state->groups.size(); ++index) {
            if (state->initial.group_id == state->groups[index].id) {
                group_index = static_cast<int>(index + 1);
                break;
            }
        }
        SendMessageW(state->group, CB_SETCURSEL, static_cast<WPARAM>(group_index), 0);
        if (state->editing &&
            state->initial.authentication == storage::AuthenticationKind::PrivateKey &&
            state->initial.credential_ref) {
            SetWindowTextW(state->private_key_path, L"已保存私钥");
        }
        ShowAuthenticationFields(*state);
        SetFocus(state->name);
        return 0;
    }
    case WM_DRAWITEM: {
        const auto* draw = reinterpret_cast<const DRAWITEMSTRUCT*>(lparam);
        if (draw) {
            if (DrawDarkComboItem(*draw, state->font) ||
                DrawDialogButton(*draw, state->font, draw->CtlID == static_cast<UINT>(IDOK))) {
                return TRUE;
            }
        }
        break;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORBTN:
        return DialogControlColor(message, wparam);
    case WM_ERASEBKGND: {
        RECT rect{};
        GetClientRect(window, &rect);
        FillRect(reinterpret_cast<HDC>(wparam), &rect, WindowBrush());
        return 1;
    }
    case WM_COMMAND: {
        const int id = LOWORD(wparam);
        if (id == kAuthenticationId && HIWORD(wparam) == CBN_SELCHANGE) {
            ShowAuthenticationFields(*state);
            return 0;
        }
        if (id == kBrowsePrivateKeyId) {
            std::array<wchar_t, 32768> path{};
            OPENFILENAMEW dialog{sizeof(dialog)};
            dialog.hwndOwner = window;
            dialog.lpstrFilter = L"私钥文件\0id_*;*.pem;*.key;*\0所有文件\0*\0";
            dialog.lpstrFile = path.data();
            dialog.nMaxFile = static_cast<DWORD>(path.size());
            dialog.Flags =
                OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_DONTADDTORECENT;
            if (GetOpenFileNameW(&dialog)) {
                auto content = ReadPrivateKeyFile(path.data());
                if (!content) {
                    ShowStorageError(window, L"无法读取私钥。", content.status());
                    return 0;
                }
                state->selected_private_key = std::move(content).value();
                SetWindowTextW(state->private_key_path, path.data());
            }
            return 0;
        }
        if (id == IDOK) {
            (void)ValidateAndCloseProfileEditor(window, *state);
            return 0;
        }
        if (id == IDCANCEL) {
            ClearProfileSecretControls(*state);
            CloseModalWindow(window);
            return 0;
        }
        break;
    }
    case WM_CLOSE:
        ClearProfileSecretControls(*state);
        CloseModalWindow(window);
        return 0;
    case WM_DESTROY:
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

bool RegisterProfileEditorClass(HINSTANCE instance) {
    if (!ui::selector::RegisterClass(instance)) {
        return false;
    }
    WNDCLASSEXW window_class{sizeof(window_class)};
    window_class.lpfnWndProc = &ProfileEditorProc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = WindowBrush();
    window_class.lpszClassName = kProfileEditorClass;
    const ATOM atom = RegisterClassExW(&window_class);
    return atom != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

std::optional<ProfileEditResult> ShowProfileEditor(HWND owner,
                                                   storage::ServerProfile profile,
                                                   bool editing,
                                                   std::vector<storage::ServerGroup> groups) {
    HINSTANCE instance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(owner, GWLP_HINSTANCE));
    if (!RegisterProfileEditorClass(instance)) {
        return std::nullopt;
    }
    ProfileEditorState state;
    state.owner = owner;
    state.initial = std::move(profile);
    state.groups = std::move(groups);
    state.editing = editing;
    const UINT dpi = WindowDpi(owner);
    constexpr DWORD profile_style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    HWND window = CreateWindowExW(WS_EX_DLGMODALFRAME,
                                  kProfileEditorClass,
                                  editing ? L"编辑服务器" : L"新建服务器",
                                  profile_style,
                                  0,
                                  0,
                                  ProfileLayoutScale(610, dpi),
                                  ProfileLayoutScale(545, dpi),
                                  owner,
                                  nullptr,
                                  instance,
                                  &state);
    if (!window) {
        return std::nullopt;
    }
    CenterWindow(window, owner);
    RunModalWindow(window, owner);
    return std::move(state.result);
}

constexpr wchar_t kServerManagerClass[] = L"SLink.ServerManager";
constexpr int kServerListId = 4001;
constexpr int kNewServerId = 4002;
constexpr int kEditServerId = 4003;
constexpr int kDeleteServerId = 4004;
constexpr int kManageGroupsId = 4005;
constexpr int kCloseManagerId = IDCANCEL;

// 管理器列表使用快照展示。新增、编辑和删除成功提交后重新查询仓库并通知主窗口，
// 保证三个业务页面使用同一份服务器配置来源。
struct ServerManagerState {
    storage::ApplicationRepository* repository{};
    storage::Vault* vault{};
    ServerProfilesChangedCallback changed_callback;
    HWND list{};
    HWND new_button{};
    HWND edit_button{};
    HWND delete_button{};
    HWND groups_button{};
    HWND close_button{};
    HFONT font{};
    std::vector<storage::ServerProfile> profiles;
    std::vector<storage::ServerGroup> groups;
    bool changed{};

    ~ServerManagerState() {
        DeleteObject(font);
    }
};

int SelectedProfileIndex(const ServerManagerState& state) {
    return ui::table::SelectedRow(state.list);
}

void UpdateManagerButtons(ServerManagerState& state) {
    const bool selected = SelectedProfileIndex(state) >= 0;
    EnableWindow(state.edit_button, selected);
    EnableWindow(state.delete_button, selected);
}

bool RefreshProfiles(HWND owner, ServerManagerState& state) {
    auto groups = state.repository->ListServerGroups();
    if (!groups) {
        ShowStorageError(owner, L"无法读取服务器分组。", groups.status());
        return false;
    }
    auto profiles = state.repository->ListServerProfiles();
    if (!profiles) {
        ShowStorageError(owner, L"无法读取服务器配置。", profiles.status());
        return false;
    }
    state.profiles = std::move(profiles).value();
    state.groups = std::move(groups).value();
    std::ranges::sort(state.groups, [](const auto& left, const auto& right) {
        return left.sort_order != right.sort_order ? left.sort_order < right.sort_order
                                                   : left.name < right.name;
    });
    std::ranges::sort(state.profiles, {}, [](const storage::ServerProfile& profile) {
        return profile.name;
    });
    ui::table::ClearSelection(state.list);
    ui::table::SetRowCount(state.list, state.profiles.size(), false);
    ui::table::Refresh(state.list);
    UpdateManagerButtons(state);
    return true;
}

void NotifyProfilesChanged(ServerManagerState& state) {
    state.changed = true;
    if (state.changed_callback) {
        try {
            state.changed_callback();
        } catch (...) {
            MessageBoxW(state.list,
                        L"服务器配置已保存，但刷新主界面时发生错误。",
                        L"SLink",
                        MB_OK | MB_ICONWARNING);
        }
    }
}

bool CommitProfile(HWND owner, ServerManagerState& state, ProfileEditResult& edit) {
    const auto old_profile = state.repository->FindServerProfile(edit.profile.id);
    if (!old_profile) {
        ShowStorageError(owner, L"无法读取原服务器配置。", old_profile.status());
        return false;
    }
    const std::optional<std::string> old_reference =
        old_profile.value() ? old_profile.value()->credential_ref : std::nullopt;
    std::optional<storage::SecureBuffer> rollback_secret;
    std::optional<std::string> written_reference;

    if (edit.credential_update == CredentialUpdate::Keep) {
        edit.profile.credential_ref = old_reference;
    } else if (edit.credential_update == CredentialUpdate::Password ||
               edit.credential_update == CredentialUpdate::PrivateKey) {
        if (!EnsureVaultUnlocked(owner, state.vault)) {
            return false;
        }
        const std::string credential_reference =
            old_reference.value_or("credential-" + edit.profile.id);
        if (old_reference) {
            auto old_secret = state.vault->Get(*old_reference);
            if (old_secret) {
                rollback_secret.emplace(std::move(old_secret).value());
            } else if (old_secret.status().code != storage::StorageErrc::not_found) {
                ShowStorageError(owner, L"无法读取原凭据。", old_secret.status());
                return false;
            }
        }
        StoredCredentialView view;
        if (edit.credential_update == CredentialUpdate::Password) {
            view.kind = StoredCredentialKind::Password;
            view.password = edit.primary_secret.bytes();
        } else {
            view.kind = StoredCredentialKind::PrivateKey;
            view.private_key = edit.primary_secret.bytes();
            view.private_key_passphrase = edit.secondary_secret.bytes();
        }
        auto encoded = EncodeStoredCredential(view);
        if (!encoded) {
            ShowStorageError(owner, L"无法编码凭据。", encoded.status());
            return false;
        }
        const auto stored = state.vault->Put(credential_reference, encoded.value().bytes());
        encoded.value().clear();
        if (!stored) {
            ShowStorageError(owner, L"无法保存加密凭据。", stored);
            return false;
        }
        edit.profile.credential_ref = credential_reference;
        written_reference = credential_reference;
    } else {
        edit.profile.credential_ref.reset();
        if (old_reference && state.vault && !EnsureVaultUnlocked(owner, state.vault)) {
            return false;
        }
    }

    const auto saved = state.repository->UpsertServerProfile(edit.profile);
    if (!saved) {
        if (written_reference && state.vault && state.vault->is_unlocked()) {
            if (rollback_secret) {
                (void)state.vault->Put(*written_reference, rollback_secret->bytes());
            } else {
                (void)state.vault->Remove(*written_reference);
            }
        }
        ShowStorageError(owner, L"无法保存服务器配置。", saved);
        return false;
    }

    if (edit.credential_update == CredentialUpdate::Clear && old_reference && state.vault &&
        state.vault->is_unlocked()) {
        const auto removed = state.vault->Remove(*old_reference);
        if (!removed) {
            ShowStorageError(owner, L"服务器配置已保存，但无法清理原加密凭据。", removed);
        }
    }
    edit.primary_secret.clear();
    edit.secondary_secret.clear();
    if (rollback_secret) {
        rollback_secret->clear();
    }
    NotifyProfilesChanged(state);
    return true;
}

void CreateServer(HWND owner, ServerManagerState& state) {
    storage::ServerProfile profile;
    profile.id = NewServerId();
    profile.port = 22;
    profile.authentication = storage::AuthenticationKind::Password;
    auto edited = ShowProfileEditor(owner, std::move(profile), false, state.groups);
    if (edited && CommitProfile(owner, state, *edited)) {
        (void)RefreshProfiles(owner, state);
    }
}

void EditSelectedServer(HWND owner, ServerManagerState& state) {
    const int index = SelectedProfileIndex(state);
    if (index < 0 || static_cast<std::size_t>(index) >= state.profiles.size()) {
        return;
    }
    auto edited = ShowProfileEditor(
        owner, state.profiles[static_cast<std::size_t>(index)], true, state.groups);
    if (edited && CommitProfile(owner, state, *edited)) {
        (void)RefreshProfiles(owner, state);
    }
}

void DeleteSelectedServer(HWND owner, ServerManagerState& state) {
    const int index = SelectedProfileIndex(state);
    if (index < 0 || static_cast<std::size_t>(index) >= state.profiles.size()) {
        return;
    }
    const auto profile = state.profiles[static_cast<std::size_t>(index)];
    const std::wstring message =
        L"确定删除服务器“" + Utf8ToWide(profile.name) + L"”吗？\r\n\r\n此操作不会删除远端文件。";
    if (MessageBoxW(
            owner, message.c_str(), L"删除服务器", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) !=
        IDYES) {
        return;
    }
    if (profile.credential_ref && state.vault && !EnsureVaultUnlocked(owner, state.vault)) {
        return;
    }
    const auto removed = state.repository->RemoveServerProfile(profile.id);
    if (!removed) {
        ShowStorageError(owner, L"无法删除服务器配置。", removed);
        return;
    }
    if (profile.credential_ref && state.vault && state.vault->is_unlocked()) {
        const auto secret_removed = state.vault->Remove(*profile.credential_ref);
        if (!secret_removed) {
            ShowStorageError(owner, L"服务器已删除，但无法清理对应的加密凭据。", secret_removed);
        }
    }
    NotifyProfilesChanged(state);
    (void)RefreshProfiles(owner, state);
}

constexpr wchar_t kGroupManagerClass[] = L"SLink.ServerManager.Groups";
constexpr int kGroupListId = 5001;
constexpr int kGroupNameId = 5002;
constexpr int kNewGroupId = 5003;
constexpr int kRenameGroupId = 5004;
constexpr int kDeleteGroupId = 5005;

struct GroupManagerState {
    storage::ApplicationRepository* repository{};
    HWND list{};
    HWND name{};
    HWND rename_button{};
    HWND delete_button{};
    HFONT font{};
    std::vector<storage::ServerGroup> groups;
    bool changed{};

    ~GroupManagerState() {
        DeleteObject(font);
    }
};

bool RefreshGroupManager(HWND window, GroupManagerState& state) {
    auto groups = state.repository->ListServerGroups();
    if (!groups) {
        ShowStorageError(window, L"无法读取服务器分组。", groups.status());
        return false;
    }
    state.groups = std::move(groups).value();
    std::ranges::sort(state.groups, [](const auto& left, const auto& right) {
        return left.sort_order != right.sort_order ? left.sort_order < right.sort_order
                                                   : left.name < right.name;
    });
    ui::table::ClearSelection(state.list);
    ui::table::SetRowCount(state.list, state.groups.size(), false);
    ui::table::Refresh(state.list);
    EnableWindow(state.rename_button, FALSE);
    EnableWindow(state.delete_button, FALSE);
    return true;
}

std::optional<std::string>
EnteredGroupName(HWND window, const GroupManagerState& state, const std::string& except_id = {}) {
    std::wstring name = WindowText(state.name);
    const auto first = name.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) {
        MessageBoxW(window, L"请输入分组名称。", L"SLink", MB_OK | MB_ICONWARNING);
        SetFocus(state.name);
        return std::nullopt;
    }
    name = name.substr(first, name.find_last_not_of(L" \t\r\n") - first + 1);
    for (const auto& group : state.groups) {
        if (group.id != except_id &&
            CompareStringOrdinal(name.c_str(), -1, Utf8ToWide(group.name).c_str(), -1, TRUE) ==
                CSTR_EQUAL) {
            MessageBoxW(window, L"该分组名称已存在。", L"SLink", MB_OK | MB_ICONWARNING);
            SetFocus(state.name);
            return std::nullopt;
        }
    }
    auto encoded = WideToUtf8(name);
    if (!encoded || encoded->empty()) {
        MessageBoxW(window, L"分组名称包含无效字符。", L"SLink", MB_OK | MB_ICONWARNING);
        return std::nullopt;
    }
    return encoded;
}

LRESULT CALLBACK GroupManagerProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<GroupManagerState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        state = static_cast<GroupManagerState*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (!state) {
        return DefWindowProcW(window, message, wparam, lparam);
    }
    switch (message) {
    case WM_ACTIVATE:
        if (LOWORD(wparam) != WA_INACTIVE) {
            RaiseModalOwnerChain(window);
        }
        break;
    case WM_CREATE: {
        const UINT dpi = WindowDpi(window);
        EnableDarkWindow(window);
        state->font = CreateDialogFont(dpi);
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        state->list = ui::table::Create(create->hInstance, window, kGroupListId, true, WS_BORDER);
        MoveWindow(
            state->list, Scale(18, dpi), Scale(18, dpi), Scale(444, dpi), Scale(238, dpi), FALSE);
        const std::array columns{ui::table::Column{L"分组名称", Scale(438, dpi), Scale(150, dpi)}};
        ui::table::SetColumns(state->list, columns);
        ui::table::SetFillLastColumn(state->list, true);
        ui::table::SetTextProvider(state->list, [state](std::size_t row, std::size_t) {
            return row < state->groups.size() ? Utf8ToWide(state->groups[row].name)
                                              : std::wstring{};
        });
        HWND label = CreateWindowExW(0,
                                     L"STATIC",
                                     L"名称",
                                     WS_CHILD | WS_VISIBLE,
                                     Scale(18, dpi),
                                     Scale(274, dpi),
                                     Scale(56, dpi),
                                     Scale(26, dpi),
                                     window,
                                     nullptr,
                                     nullptr,
                                     nullptr);
        state->name =
            CreateWindowExW(0,
                            L"EDIT",
                            L"",
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL,
                            Scale(78, dpi),
                            Scale(269, dpi),
                            Scale(384, dpi),
                            Scale(28, dpi),
                            window,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kGroupNameId)),
                            nullptr,
                            nullptr);
        const auto button = [&](int id, const wchar_t* text, int x, int width) {
            HWND control = CreateWindowExW(0,
                                           L"BUTTON",
                                           text,
                                           WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                           Scale(x, dpi),
                                           Scale(318, dpi),
                                           Scale(width, dpi),
                                           Scale(30, dpi),
                                           window,
                                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                           nullptr,
                                           nullptr);
            SetFont(control, state->font);
            ApplyDarkControl(control);
            return control;
        };
        button(kNewGroupId, L"新建", 18, 82);
        state->rename_button = button(kRenameGroupId, L"重命名", 106, 88);
        state->delete_button = button(kDeleteGroupId, L"删除", 200, 82);
        button(IDCANCEL, L"关闭", 380, 82);
        SetFont(label, state->font);
        SetFont(state->list, state->font);
        SetFont(state->name, state->font);
        ApplyDarkControl(state->name);
        SendMessageW(state->name, EM_SETLIMITTEXT, 256, 0);
        ui::table::ApplyTheme(state->list);
        (void)RefreshGroupManager(window, *state);
        SetFocus(state->name);
        return 0;
    }
    case WM_COMMAND: {
        const int id = LOWORD(wparam);
        if (id == IDCANCEL) {
            CloseModalWindow(window);
            return 0;
        }
        if (id == kNewGroupId || id == kRenameGroupId) {
            const int selected = ui::table::SelectedRow(state->list);
            if (id == kRenameGroupId &&
                (selected < 0 || static_cast<std::size_t>(selected) >= state->groups.size())) {
                return 0;
            }
            const std::string old_id = id == kRenameGroupId
                                           ? state->groups[static_cast<std::size_t>(selected)].id
                                           : std::string{};
            auto name = EnteredGroupName(window, *state, old_id);
            if (!name) {
                return 0;
            }
            storage::ServerGroup group;
            if (id == kRenameGroupId) {
                group = state->groups[static_cast<std::size_t>(selected)];
            } else {
                group.id = NewServerId();
                group.id.replace(0, 7, "group-");
                std::int32_t highest = -1;
                for (const auto& existing : state->groups) {
                    highest = (std::max)(highest, existing.sort_order);
                }
                if (highest == (std::numeric_limits<std::int32_t>::max)()) {
                    MessageBoxW(
                        window, L"分组数量已达到排序上限。", L"SLink", MB_OK | MB_ICONWARNING);
                    return 0;
                }
                group.sort_order = highest + 1;
            }
            group.name = std::move(*name);
            const auto saved = state->repository->UpsertServerGroup(group);
            if (!saved) {
                ShowStorageError(window, L"无法保存服务器分组。", saved);
                return 0;
            }
            state->changed = true;
            (void)RefreshGroupManager(window, *state);
            SetWindowTextW(state->name, L"");
            return 0;
        }
        if (id == kDeleteGroupId) {
            const int selected = ui::table::SelectedRow(state->list);
            if (selected < 0 || static_cast<std::size_t>(selected) >= state->groups.size()) {
                return 0;
            }
            const auto& group = state->groups[static_cast<std::size_t>(selected)];
            auto profiles = state->repository->ListServerProfiles();
            if (!profiles) {
                ShowStorageError(window, L"无法读取分组中的服务器。", profiles.status());
                return 0;
            }
            const auto count = std::ranges::count_if(profiles.value(), [&](const auto& profile) {
                return profile.group_id == group.id;
            });
            const auto prompt = L"删除分组“" + Utf8ToWide(group.name) + L"”？\r\n" +
                                std::to_wstring(count) + L" 台服务器将移到“未分组”，不会被删除。";
            if (MessageBoxW(window,
                            prompt.c_str(),
                            L"删除分组",
                            MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
                return 0;
            }
            const auto removed = state->repository->RemoveServerGroupAndUngroup(group.id);
            if (!removed) {
                ShowStorageError(window, L"无法删除服务器分组。", removed);
                return 0;
            }
            state->changed = true;
            (void)RefreshGroupManager(window, *state);
            SetWindowTextW(state->name, L"");
            return 0;
        }
        break;
    }
    case WM_NOTIFY: {
        const auto* notification = reinterpret_cast<const NMHDR*>(lparam);
        if (notification && notification->idFrom == kGroupListId &&
            notification->code == LVN_ITEMCHANGED) {
            const int selected = ui::table::SelectedRow(state->list);
            const bool valid =
                selected >= 0 && static_cast<std::size_t>(selected) < state->groups.size();
            EnableWindow(state->rename_button, valid);
            EnableWindow(state->delete_button, valid);
            if (valid) {
                SetWindowTextW(
                    state->name,
                    Utf8ToWide(state->groups[static_cast<std::size_t>(selected)].name).c_str());
            }
            return 0;
        }
        break;
    }
    case WM_DRAWITEM: {
        const auto* draw = reinterpret_cast<const DRAWITEMSTRUCT*>(lparam);
        if (draw && DrawDialogButton(*draw, state->font, draw->CtlID == kNewGroupId)) {
            return TRUE;
        }
        break;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORBTN:
        return DialogControlColor(message, wparam);
    case WM_ERASEBKGND: {
        RECT rect{};
        GetClientRect(window, &rect);
        FillRect(reinterpret_cast<HDC>(wparam), &rect, WindowBrush());
        return 1;
    }
    case WM_CLOSE:
        CloseModalWindow(window);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

bool ShowGroupManagerDialog(HWND owner, storage::ApplicationRepository& repository) {
    HINSTANCE instance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(owner, GWLP_HINSTANCE));
    WNDCLASSEXW window_class{sizeof(window_class)};
    window_class.lpfnWndProc = &GroupManagerProc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = WindowBrush();
    window_class.lpszClassName = kGroupManagerClass;
    if (!RegisterClassExW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }
    GroupManagerState state;
    state.repository = &repository;
    const UINT dpi = WindowDpi(owner);
    HWND window = CreateWindowExW(WS_EX_DLGMODALFRAME,
                                  kGroupManagerClass,
                                  L"管理服务器分组",
                                  WS_POPUP | WS_CAPTION | WS_SYSMENU,
                                  0,
                                  0,
                                  Scale(480, dpi),
                                  Scale(400, dpi),
                                  owner,
                                  nullptr,
                                  instance,
                                  &state);
    if (!window) {
        return false;
    }
    CenterWindow(window, owner);
    RunModalWindow(window, owner);
    return state.changed;
}

LRESULT CALLBACK ServerManagerProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<ServerManagerState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        state = static_cast<ServerManagerState*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (!state) {
        return DefWindowProcW(window, message, wparam, lparam);
    }

    switch (message) {
    case WM_ACTIVATE:
        if (LOWORD(wparam) != WA_INACTIVE) {
            RaiseModalOwnerChain(window);
        }
        break;
    case WM_CREATE: {
        const UINT dpi = WindowDpi(window);
        EnableDarkWindow(window);
        state->font = CreateDialogFont(dpi);
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        state->list = ui::table::Create(create->hInstance, window, kServerListId, true, WS_BORDER);
        MoveWindow(
            state->list, Scale(18, dpi), Scale(18, dpi), Scale(760, dpi), Scale(382, dpi), FALSE);
        const std::array columns{
            ui::table::Column{L"名称", Scale(135, dpi), Scale(95, dpi)},
            ui::table::Column{L"分组", Scale(125, dpi), Scale(90, dpi)},
            ui::table::Column{L"主机", Scale(165, dpi), Scale(110, dpi)},
            ui::table::Column{L"端口", Scale(65, dpi), Scale(55, dpi), ui::table::Alignment::right},
            ui::table::Column{L"用户名", Scale(105, dpi), Scale(75, dpi)},
            ui::table::Column{L"认证", Scale(145, dpi), Scale(100, dpi)}};
        ui::table::SetColumns(state->list, columns);
        ui::table::SetFillLastColumn(state->list, true);
        ui::table::SetTextProvider(
            state->list, [state](const std::size_t row, const std::size_t column) {
                if (row >= state->profiles.size()) {
                    return std::wstring{};
                }
                const storage::ServerProfile& profile = state->profiles[row];
                switch (column) {
                case 0:
                    return Utf8ToWide(profile.name);
                case 1:
                    if (profile.group_id) {
                        const auto found = std::ranges::find(
                            state->groups, *profile.group_id, &storage::ServerGroup::id);
                        if (found != state->groups.end()) {
                            return Utf8ToWide(found->name);
                        }
                    }
                    return std::wstring(L"未分组");
                case 2:
                    return Utf8ToWide(profile.host);
                case 3:
                    return std::to_wstring(profile.port);
                case 4:
                    return Utf8ToWide(profile.username);
                case 5:
                    return AuthenticationName(profile.authentication);
                default:
                    return std::wstring{};
                }
            });
        state->new_button =
            CreateWindowExW(0,
                            L"BUTTON",
                            L"新建",
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                            Scale(18, dpi),
                            Scale(416, dpi),
                            Scale(88, dpi),
                            Scale(30, dpi),
                            window,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kNewServerId)),
                            nullptr,
                            nullptr);
        state->edit_button =
            CreateWindowExW(0,
                            L"BUTTON",
                            L"编辑",
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                            Scale(114, dpi),
                            Scale(416, dpi),
                            Scale(88, dpi),
                            Scale(30, dpi),
                            window,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEditServerId)),
                            nullptr,
                            nullptr);
        state->delete_button =
            CreateWindowExW(0,
                            L"BUTTON",
                            L"删除",
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                            Scale(210, dpi),
                            Scale(416, dpi),
                            Scale(88, dpi),
                            Scale(30, dpi),
                            window,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDeleteServerId)),
                            nullptr,
                            nullptr);
        state->groups_button =
            CreateWindowExW(0,
                            L"BUTTON",
                            L"管理分组",
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                            Scale(306, dpi),
                            Scale(416, dpi),
                            Scale(112, dpi),
                            Scale(30, dpi),
                            window,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kManageGroupsId)),
                            nullptr,
                            nullptr);
        state->close_button =
            CreateWindowExW(0,
                            L"BUTTON",
                            L"关闭",
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                            Scale(690, dpi),
                            Scale(416, dpi),
                            Scale(88, dpi),
                            Scale(30, dpi),
                            window,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCloseManagerId)),
                            nullptr,
                            nullptr);
        for (HWND control : {state->list,
                             state->new_button,
                             state->edit_button,
                             state->delete_button,
                             state->groups_button,
                             state->close_button}) {
            SetFont(control, state->font);
            if (control != state->list) {
                ApplyDarkControl(control);
            }
        }
        ui::table::ApplyTheme(state->list);
        (void)RefreshProfiles(window, *state);
        SetFocus(state->list);
        return 0;
    }
    case WM_DRAWITEM: {
        const auto* draw = reinterpret_cast<const DRAWITEMSTRUCT*>(lparam);
        if (draw && DrawDialogButton(*draw, state->font, draw->CtlID == kNewServerId)) {
            return TRUE;
        }
        break;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORBTN:
        return DialogControlColor(message, wparam);
    case WM_ERASEBKGND: {
        RECT rect{};
        GetClientRect(window, &rect);
        FillRect(reinterpret_cast<HDC>(wparam), &rect, WindowBrush());
        return 1;
    }
    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case kNewServerId:
            CreateServer(window, *state);
            return 0;
        case kEditServerId:
            EditSelectedServer(window, *state);
            return 0;
        case kDeleteServerId:
            DeleteSelectedServer(window, *state);
            return 0;
        case kManageGroupsId:
            if (ShowGroupManagerDialog(window, *state->repository)) {
                NotifyProfilesChanged(*state);
                (void)RefreshProfiles(window, *state);
            }
            return 0;
        case kCloseManagerId:
            CloseModalWindow(window);
            return 0;
        default:
            break;
        }
        break;
    case WM_NOTIFY: {
        const auto* notification = reinterpret_cast<const NMHDR*>(lparam);
        if (notification && notification->idFrom == kServerListId) {
            if (notification->code == LVN_ITEMCHANGED) {
                UpdateManagerButtons(*state);
                return 0;
            }
            if (notification->code == NM_DBLCLK) {
                EditSelectedServer(window, *state);
                return 0;
            }
        }
        break;
    }
    case WM_CLOSE:
        CloseModalWindow(window);
        return 0;
    case WM_DESTROY: {
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

bool RegisterServerManagerClass(HINSTANCE instance) {
    WNDCLASSEXW window_class{sizeof(window_class)};
    window_class.lpfnWndProc = &ServerManagerProc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = WindowBrush();
    window_class.lpszClassName = kServerManagerClass;
    const ATOM atom = RegisterClassExW(&window_class);
    return atom != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

} // namespace

storage::StorageResult<storage::SecureBuffer>
EncodeStoredCredential(const StoredCredentialView& credential) {
    struct Field {
        CredentialField tag;
        std::span<const std::byte> value;
    };
    std::array<Field, 2> fields{};
    std::size_t field_count = 0;
    if (credential.kind == StoredCredentialKind::Password) {
        if (credential.password.empty() || credential.password.size() > kMaximumPasswordBytes ||
            !credential.private_key.empty() || !credential.private_key_passphrase.empty()) {
            return storage::StorageResult<storage::SecureBuffer>::Failure(
                InvalidCredential(L"密码凭据字段无效。"));
        }
        fields[field_count++] = {CredentialField::Password, credential.password};
    } else if (credential.kind == StoredCredentialKind::PrivateKey) {
        if (credential.private_key.empty() ||
            credential.private_key.size() > kMaximumPrivateKeyBytes ||
            credential.private_key_passphrase.size() > kMaximumPasswordBytes ||
            !credential.password.empty()) {
            return storage::StorageResult<storage::SecureBuffer>::Failure(
                InvalidCredential(L"私钥凭据字段无效。"));
        }
        fields[field_count++] = {CredentialField::PrivateKey, credential.private_key};
        if (!credential.private_key_passphrase.empty()) {
            fields[field_count++] = {CredentialField::PrivateKeyPassphrase,
                                     credential.private_key_passphrase};
        }
    } else {
        return storage::StorageResult<storage::SecureBuffer>::Failure(
            InvalidCredential(L"不支持的凭据类型。"));
    }

    std::size_t total = kCredentialMagic.size() + sizeof(std::uint16_t) + 2U;
    for (std::size_t index = 0; index < field_count; ++index) {
        total += 1U + sizeof(std::uint32_t) + fields[index].value.size();
    }
    if (total > kMaximumCredentialBytes) {
        return storage::StorageResult<storage::SecureBuffer>::Failure(
            InvalidCredential(L"凭据数据过大。"));
    }
    try {
        storage::SecureBuffer encoded(total);
        auto destination = encoded.bytes();
        std::memcpy(destination.data(), kCredentialMagic.data(), kCredentialMagic.size());
        std::size_t cursor = kCredentialMagic.size();
        WriteUnsigned(destination, cursor, kCredentialVersion);
        WriteUnsigned(destination, cursor, static_cast<std::uint8_t>(credential.kind));
        WriteUnsigned(destination, cursor, static_cast<std::uint8_t>(field_count));
        for (std::size_t index = 0; index < field_count; ++index) {
            WriteUnsigned(destination, cursor, static_cast<std::uint8_t>(fields[index].tag));
            WriteUnsigned(
                destination, cursor, static_cast<std::uint32_t>(fields[index].value.size()));
            if (!fields[index].value.empty()) {
                std::memcpy(destination.data() + cursor,
                            fields[index].value.data(),
                            fields[index].value.size());
                cursor += fields[index].value.size();
            }
        }
        return storage::StorageResult<storage::SecureBuffer>::Success(std::move(encoded));
    } catch (...) {
        return storage::StorageResult<storage::SecureBuffer>::Failure(storage::StorageStatus::Error(
            storage::StorageErrc::backend_error, L"无法分配锁定内存来编码凭据。"));
    }
}

storage::StorageResult<DecodedStoredCredential>
DecodeStoredCredential(std::span<const std::byte> encoded) {
    if (encoded.size() < kCredentialMagic.size() + sizeof(std::uint16_t) + 2U ||
        encoded.size() > kMaximumCredentialBytes ||
        !std::equal(kCredentialMagic.begin(), kCredentialMagic.end(), encoded.begin())) {
        return storage::StorageResult<DecodedStoredCredential>::Failure(
            CorruptCredential(L"凭据容器标头无效。"));
    }
    std::size_t cursor = kCredentialMagic.size();
    std::uint16_t version = 0;
    std::uint8_t kind_raw = 0;
    std::uint8_t field_count = 0;
    if (!ReadUnsigned(encoded, cursor, version) || version != kCredentialVersion ||
        !ReadUnsigned(encoded, cursor, kind_raw) || !ReadUnsigned(encoded, cursor, field_count) ||
        field_count == 0 || field_count > 2) {
        return storage::StorageResult<DecodedStoredCredential>::Failure(
            CorruptCredential(L"凭据容器版本或字段数量无效。"));
    }
    if (kind_raw != static_cast<std::uint8_t>(StoredCredentialKind::Password) &&
        kind_raw != static_cast<std::uint8_t>(StoredCredentialKind::PrivateKey)) {
        return storage::StorageResult<DecodedStoredCredential>::Failure(
            CorruptCredential(L"凭据类型无效。"));
    }

    DecodedStoredCredential result;
    result.kind = static_cast<StoredCredentialKind>(kind_raw);
    bool has_password = false;
    bool has_private_key = false;
    bool has_passphrase = false;
    for (std::uint8_t index = 0; index < field_count; ++index) {
        std::uint8_t tag_raw = 0;
        std::uint32_t length = 0;
        if (!ReadUnsigned(encoded, cursor, tag_raw) || !ReadUnsigned(encoded, cursor, length) ||
            cursor > encoded.size() || encoded.size() - cursor < length) {
            return storage::StorageResult<DecodedStoredCredential>::Failure(
                CorruptCredential(L"凭据字段长度无效。"));
        }
        const auto field = encoded.subspan(cursor, length);
        cursor += length;
        storage::StorageResult<storage::SecureBuffer> copied = CopySecure(field);
        if (!copied) {
            return storage::StorageResult<DecodedStoredCredential>::Failure(copied.status());
        }
        switch (static_cast<CredentialField>(tag_raw)) {
        case CredentialField::Password:
            if (has_password || field.empty() || field.size() > kMaximumPasswordBytes) {
                return storage::StorageResult<DecodedStoredCredential>::Failure(
                    CorruptCredential(L"密码字段无效或重复。"));
            }
            has_password = true;
            result.password = std::move(copied).value();
            break;
        case CredentialField::PrivateKey:
            if (has_private_key || field.empty() || field.size() > kMaximumPrivateKeyBytes) {
                return storage::StorageResult<DecodedStoredCredential>::Failure(
                    CorruptCredential(L"私钥字段无效或重复。"));
            }
            has_private_key = true;
            result.private_key = std::move(copied).value();
            break;
        case CredentialField::PrivateKeyPassphrase:
            if (has_passphrase || field.size() > kMaximumPasswordBytes) {
                return storage::StorageResult<DecodedStoredCredential>::Failure(
                    CorruptCredential(L"私钥口令字段无效或重复。"));
            }
            has_passphrase = true;
            result.private_key_passphrase = std::move(copied).value();
            break;
        default:
            return storage::StorageResult<DecodedStoredCredential>::Failure(
                CorruptCredential(L"凭据包含未知字段。"));
        }
    }
    if (cursor != encoded.size()) {
        return storage::StorageResult<DecodedStoredCredential>::Failure(
            CorruptCredential(L"凭据容器包含尾随数据。"));
    }
    const bool valid_password = result.kind == StoredCredentialKind::Password && has_password &&
                                !has_private_key && !has_passphrase;
    const bool valid_private_key =
        result.kind == StoredCredentialKind::PrivateKey && !has_password && has_private_key;
    if (!valid_password && !valid_private_key) {
        return storage::StorageResult<DecodedStoredCredential>::Failure(
            CorruptCredential(L"凭据字段与凭据类型不匹配。"));
    }
    return storage::StorageResult<DecodedStoredCredential>::Success(std::move(result));
}

bool ShowServerManagerDialog(HWND owner,
                             storage::ApplicationRepository& repository,
                             storage::Vault* vault,
                             ServerProfilesChangedCallback on_profiles_changed) {
    if (!owner || !IsWindow(owner)) {
        return false;
    }
    HINSTANCE instance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(owner, GWLP_HINSTANCE));
    if (!RegisterServerManagerClass(instance)) {
        MessageBoxW(owner, L"无法注册服务器管理窗口。", L"SLink", MB_OK | MB_ICONERROR);
        return false;
    }
    ServerManagerState state;
    state.repository = &repository;
    state.vault = vault;
    state.changed_callback = std::move(on_profiles_changed);
    const UINT dpi = WindowDpi(owner);
    HWND window = CreateWindowExW(WS_EX_DLGMODALFRAME,
                                  kServerManagerClass,
                                  L"服务器管理",
                                  WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                  0,
                                  0,
                                  Scale(820, dpi),
                                  Scale(500, dpi),
                                  owner,
                                  nullptr,
                                  instance,
                                  &state);
    if (!window) {
        MessageBoxW(owner, L"无法创建服务器管理窗口。", L"SLink", MB_OK | MB_ICONERROR);
        return false;
    }
    CenterWindow(window, owner);
    RunModalWindow(window, owner);
    return state.changed;
}

} // namespace SLink::app
