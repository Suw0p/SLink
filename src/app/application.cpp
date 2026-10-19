// 应用组合根：初始化便携路径、数据库、凭据库、UI、SSH/SFTP 控制器和传输调度器，
// 并集中定义启动失败及退出时的资源销毁顺序。
#include "app/application.hpp"

#include "app/editor_controller.hpp"
#include "app/file_session_controller.hpp"
#include "app/firewall_helper.hpp"
#include "app/port_forward_controller.hpp"
#include "app/server_manager_dialog.hpp"
#include "app/terminal_session_controller.hpp"
#include "app/transfer_persistence_coordinator.hpp"
#include "app/transfer_session_pool.hpp"
#include "storage/app_paths.h"
#include "storage/application_repository.h"
#include "storage/crash_reporter.h"
#include "storage/rotating_log.h"
#include "storage/state_store.h"
#include "storage/vault.h"
#include "transfer/file_uri.hpp"
#include "transfer/local_endpoint.hpp"
#include "transfer/transfer.hpp"
#include "ui/background_executor.hpp"
#include "ui/editor_page.hpp"
#include "ui/local_shortcut.hpp"
#include "ui/main_window.hpp"
#include "ui/port_forward_page.hpp"
#include "ui/terminal_control.hpp"
#include "ui/terminal_page.hpp"
#include "ui/theme.hpp"
#include "ui/transfer_page.hpp"
#include "ui/win32_ui_internal.hpp"

#include <algorithm>
#include <chrono>
#include <commctrl.h>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <objbase.h>
#include <optional>
#include <shlobj.h>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "Ole32.lib")

namespace SLink::app {
namespace {

constexpr int kStartupFailureExitCode = 1;
constexpr std::string_view kLocalHostServerId = "local://host";

std::string WideToUtf8(std::wstring_view text);

transfer::FileUri DesktopDirectoryUri() {
    PWSTR desktop = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, KF_FLAG_DEFAULT, nullptr, &desktop)) &&
        desktop && *desktop) {
        const std::wstring path(desktop);
        CoTaskMemFree(desktop);
        return transfer::FileUri::local(WideToUtf8(path));
    }
    if (desktop) {
        CoTaskMemFree(desktop);
    }
    return transfer::FileUri::local("/");
}

std::wstring WindowsErrorText(const wchar_t* context, DWORD error) {
    wchar_t* system_message = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error,
        0,
        reinterpret_cast<wchar_t*>(&system_message),
        0,
        nullptr);
    std::wstring message(context);
    message.append(L"\r\n\r\n");
    if (length && system_message) {
        message.append(system_message, length);
        LocalFree(system_message);
    } else {
        message.append(L"Windows 错误：");
        message.append(std::to_wstring(error));
    }
    while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n')) {
        message.pop_back();
    }
    return message;
}

void ShowStartupFailure(const std::wstring& detail) {
    MessageBoxW(
        nullptr, detail.c_str(), L"SLink 无法启动", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
}

std::wstring HResultText(HRESULT result) {
    wchar_t buffer[16]{};
    swprintf_s(buffer, L"0x%08lX", static_cast<unsigned long>(result));
    return buffer;
}

bool InitializeDpiAwareness() {
    using SetProcessDpiAwarenessContextFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    const auto set_process_dpi = reinterpret_cast<SetProcessDpiAwarenessContextFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"));
    if (!set_process_dpi) {
        // Windows 10/11 always exports this function. Keeping a graceful
        // fallback makes diagnostic builds usable on older hosts.
        return true;
    }
    if (set_process_dpi(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        return true;
    }
    // A manifest or an embedding host may have established DPI awareness
    // before wWinMain. ERROR_ACCESS_DENIED is success for our purposes.
    return GetLastError() == ERROR_ACCESS_DENIED;
}

bool ProbeApplicationDirectoryWritable(const std::filesystem::path& application_directory,
                                       std::wstring& error_message) {
    const std::filesystem::path probe =
        application_directory /
        (L".SLink-root-write-probe-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(GetTickCount64()));
    HANDLE file = CreateFileW(probe.c_str(),
                              GENERIC_WRITE,
                              0,
                              nullptr,
                              CREATE_NEW,
                              FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_TEMPORARY,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error_message = WindowsErrorText(
            (L"应用程序目录不可写：\r\n" + application_directory.wstring()).c_str(),
            GetLastError());
        return false;
    }

    constexpr char kProbe[] = "SLink portable write probe";
    DWORD bytes_written = 0;
    const BOOL write_result =
        WriteFile(file, kProbe, static_cast<DWORD>(sizeof(kProbe) - 1), &bytes_written, nullptr);
    const BOOL flush_result = write_result ? FlushFileBuffers(file) : FALSE;
    DWORD write_error = write_result && flush_result ? ERROR_SUCCESS : GetLastError();
    if (write_result && flush_result && bytes_written != sizeof(kProbe) - 1) {
        write_error = ERROR_WRITE_FAULT;
    }
    CloseHandle(file);

    const BOOL delete_result = DeleteFileW(probe.c_str());
    const DWORD delete_error = delete_result ? ERROR_SUCCESS : GetLastError();
    if (!write_result || !flush_result || bytes_written != sizeof(kProbe) - 1) {
        if (!delete_result) {
            error_message =
                WindowsErrorText(L"便携目录写入检测失败，且探测文件无法删除", delete_error);
        } else {
            error_message = WindowsErrorText(L"无法写入便携应用程序目录", write_error);
        }
        return false;
    }
    if (!delete_result) {
        error_message = WindowsErrorText(L"便携目录探测文件无法删除", delete_error);
        return false;
    }
    return true;
}

std::wstring Utf8ToWide(std::string_view input) {
    if (input.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()), nullptr, 0);
    if (length <= 0) {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8,
                        MB_ERR_INVALID_CHARS,
                        input.data(),
                        static_cast<int>(input.size()),
                        result.data(),
                        length);
    return result;
}

std::string WideToUtf8(std::wstring_view input) {
    if (input.empty()) {
        return {};
    }
    const int length = WideCharToMultiByte(CP_UTF8,
                                           WC_ERR_INVALID_CHARS,
                                           input.data(),
                                           static_cast<int>(input.size()),
                                           nullptr,
                                           0,
                                           nullptr,
                                           nullptr);
    if (length <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8,
                        WC_ERR_INVALID_CHARS,
                        input.data(),
                        static_cast<int>(input.size()),
                        result.data(),
                        length,
                        nullptr,
                        nullptr);
    return result;
}

class SecurePrompt final {
public:
    static std::optional<std::wstring> Show(HWND owner,
                                            std::wstring title,
                                            std::wstring prompt,
                                            bool password = true,
                                            std::wstring initial_value = {}) {
        SecurePrompt dialog(
            owner, std::move(title), std::move(prompt), password, std::move(initial_value));
        return dialog.Run();
    }

private:
    static constexpr wchar_t kClassName[] = L"SLink.SecureCredentialPrompt";

    SecurePrompt(HWND owner,
                 std::wstring title,
                 std::wstring prompt,
                 bool password,
                 std::wstring initial_value)
        : owner_(owner), title_(std::move(title)), prompt_(std::move(prompt)),
          initial_value_(std::move(initial_value)), password_(password) {}

    ~SecurePrompt() {
        if (!value_.empty()) {
            SecureZeroMemory(value_.data(), value_.size() * sizeof(wchar_t));
        }
    }

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* self = reinterpret_cast<SecurePrompt*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            self = static_cast<SecurePrompt*>(create->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->window_ = window;
        }
        if (!self) {
            return DefWindowProcW(window, message, wparam, lparam);
        }
        switch (message) {
        case WM_DRAWITEM: {
            const auto* draw = reinterpret_cast<const DRAWITEMSTRUCT*>(lparam);
            if (draw && ui::detail::DrawSelfDrawnButton(*draw, draw->CtlID == IDOK)) {
                return TRUE;
            }
            break;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN: {
            const HDC dc = reinterpret_cast<HDC>(wparam);
            SetTextColor(dc, ui::theme::kTextColor);
            SetBkMode(dc, TRANSPARENT);
            return reinterpret_cast<LRESULT>(ui::detail::WindowThemeBrush());
        }
        case WM_CTLCOLOREDIT: {
            const HDC dc = reinterpret_cast<HDC>(wparam);
            SetTextColor(dc, ui::theme::kTextColor);
            SetBkColor(dc, ui::theme::kPanelAltColor);
            return reinterpret_cast<LRESULT>(ui::detail::PanelAltThemeBrush());
        }
        case WM_ERASEBKGND: {
            RECT rect{};
            GetClientRect(window, &rect);
            FillRect(reinterpret_cast<HDC>(wparam), &rect, ui::detail::WindowThemeBrush());
            return 1;
        }
        case WM_COMMAND:
            if (LOWORD(wparam) == IDOK) {
                self->Accept();
                return 0;
            }
            if (LOWORD(wparam) == IDCANCEL) {
                self->Cancel();
                return 0;
            }
            break;
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

    bool RegisterClass(HINSTANCE instance) const {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = &WindowProc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = ui::detail::WindowThemeBrush();
        wc.lpszClassName = kClassName;
        const ATOM atom = RegisterClassExW(&wc);
        return atom != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }

    void Accept() {
        const int length = std::clamp(GetWindowTextLengthW(edit_), 0, 16 * 1024);
        value_.assign(static_cast<std::size_t>(length) + 1, L'\0');
        const int copied = GetWindowTextW(edit_, value_.data(), length + 1);
        value_.resize(static_cast<std::size_t>(std::max(0, copied)));
        accepted_ = true;
        SetWindowTextW(edit_, L"");
        DestroyWindow(window_);
    }

    void Cancel() {
        accepted_ = false;
        SetWindowTextW(edit_, L"");
        DestroyWindow(window_);
    }

    std::optional<std::wstring> Run() {
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        if (!RegisterClass(instance)) {
            return std::nullopt;
        }
        constexpr int width = 440;
        constexpr int height = 184;
        RECT owner_rect{};
        GetWindowRect(owner_, &owner_rect);
        const int x = owner_rect.left + ((owner_rect.right - owner_rect.left) - width) / 2;
        const int y = owner_rect.top + ((owner_rect.bottom - owner_rect.top) - height) / 2;
        window_ = CreateWindowExW(WS_EX_DLGMODALFRAME,
                                  kClassName,
                                  title_.c_str(),
                                  WS_POPUP | WS_CAPTION | WS_SYSMENU,
                                  x,
                                  y,
                                  width,
                                  height,
                                  owner_,
                                  nullptr,
                                  instance,
                                  this);
        if (!window_) {
            return std::nullopt;
        }
        ui::detail::EnableDarkTitleBar(window_);
        HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        HWND label = CreateWindowExW(0,
                                     L"STATIC",
                                     prompt_.c_str(),
                                     WS_CHILD | WS_VISIBLE | SS_LEFT,
                                     20,
                                     18,
                                     392,
                                     38,
                                     window_,
                                     nullptr,
                                     instance,
                                     nullptr);
        edit_ = CreateWindowExW(WS_EX_CLIENTEDGE,
                                L"EDIT",
                                initial_value_.c_str(),
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL |
                                    (password_ ? ES_PASSWORD : 0),
                                20,
                                60,
                                392,
                                26,
                                window_,
                                nullptr,
                                instance,
                                nullptr);
        SendMessageW(edit_, EM_SETLIMITTEXT, 16 * 1024, 0);
        HWND ok = CreateWindowExW(0,
                                  L"BUTTON",
                                  L"确定",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                  246,
                                  104,
                                  78,
                                  28,
                                  window_,
                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDOK)),
                                  instance,
                                  nullptr);
        HWND cancel = CreateWindowExW(0,
                                      L"BUTTON",
                                      L"取消",
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                      334,
                                      104,
                                      78,
                                      28,
                                      window_,
                                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDCANCEL)),
                                      instance,
                                      nullptr);
        for (HWND control : {label, edit_, ok, cancel}) {
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
            ui::detail::ApplySelfDrawnTheme(control);
        }
        SendMessageW(window_, DM_SETDEFID, IDOK, 0);

        EnableWindow(owner_, FALSE);
        ShowWindow(window_, SW_SHOW);
        UpdateWindow(window_);
        SetFocus(edit_);
        MSG message{};
        while (!done_) {
            const BOOL result = GetMessageW(&message, nullptr, 0, 0);
            if (result <= 0) {
                if (result == 0) {
                    PostQuitMessage(static_cast<int>(message.wParam));
                }
                break;
            }
            if (!IsDialogMessageW(window_, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        EnableWindow(owner_, TRUE);
        SetForegroundWindow(owner_);
        if (!accepted_) {
            if (!value_.empty()) {
                SecureZeroMemory(value_.data(), value_.size() * sizeof(wchar_t));
            }
            return std::nullopt;
        }
        return std::move(value_);
    }

    HWND owner_{};
    HWND window_{};
    HWND edit_{};
    std::wstring title_;
    std::wstring prompt_;
    std::wstring initial_value_;
    std::wstring value_;
    bool password_{true};
    bool accepted_{};
    bool done_{};
};

class ComApartment final {
public:
    bool Initialize() {
        result_ = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        initialized_ = SUCCEEDED(result_);
        return initialized_;
    }

    ~ComApartment() {
        if (initialized_) {
            CoUninitialize();
        }
    }

    HRESULT result() const noexcept {
        return result_;
    }

private:
    HRESULT result_{E_UNEXPECTED};
    bool initialized_{};
};

struct KeyboardResponseState {
    std::mutex mutex;
    std::condition_variable changed;
    bool ready{};
    std::optional<std::vector<std::string>> responses;
    Status failure{ErrorCode::cancelled, L"用户取消了 keyboard-interactive 认证。"};
};

std::wstring FormatFileSize(const std::uint64_t bytes) {
    constexpr std::uint64_t kKilobyte = 1024;
    constexpr std::uint64_t kMegabyte = 1024 * kKilobyte;
    constexpr std::uint64_t kGigabyte = 1024 * kMegabyte;
    if (bytes >= kGigabyte) {
        return std::to_wstring(bytes / kGigabyte) + L" GB";
    }
    if (bytes >= kMegabyte) {
        return std::to_wstring(bytes / kMegabyte) + L" MB";
    }
    if (bytes >= kKilobyte) {
        return std::to_wstring(bytes / kKilobyte) + L" KB";
    }
    return std::to_wstring(bytes) + L" B";
}

std::wstring FormatModifiedTime(const std::chrono::system_clock::time_point value) {
    const std::time_t raw = std::chrono::system_clock::to_time_t(value);
    std::tm local{};
    if (localtime_s(&local, &raw) != 0) {
        return {};
    }
    wchar_t buffer[32]{};
    if (wcsftime(buffer, std::size(buffer), L"%Y-%m-%d %H:%M", &local) == 0) {
        return {};
    }
    return buffer;
}

std::wstring FormatPermissions(const std::uint32_t permissions) {
    if (permissions == 0)
        return L"—";
    const std::uint32_t mode = permissions & 0777U;
    std::wstring text(9, L'-');
    static constexpr std::array<wchar_t, 3> symbols{L'r', L'w', L'x'};
    for (std::size_t index = 0; index < text.size(); ++index) {
        const std::uint32_t bit = 1U << (8U - static_cast<unsigned>(index));
        if ((mode & bit) != 0) {
            text[index] = symbols[index % symbols.size()];
        }
    }
    return text;
}

transfer::ConflictPolicy FromRecoveryConflict(const storage::TransferRecoveryConflictPolicy value) {
    switch (value) {
    case storage::TransferRecoveryConflictPolicy::Overwrite:
        return transfer::ConflictPolicy::Overwrite;
    case storage::TransferRecoveryConflictPolicy::Skip:
        return transfer::ConflictPolicy::Skip;
    case storage::TransferRecoveryConflictPolicy::AutoRename:
        return transfer::ConflictPolicy::AutoRename;
    case storage::TransferRecoveryConflictPolicy::Resume:
        return transfer::ConflictPolicy::Resume;
    }
    return transfer::ConflictPolicy::Overwrite;
}

bool IsTerminalTransferState(const transfer::TransferState state) {
    return state == transfer::TransferState::Completed ||
           state == transfer::TransferState::Failed || state == transfer::TransferState::Cancelled;
}

} // namespace

// 成员声明顺序也表达所有权。业务控制器在 MainWindow 仍有效时停止，传输/编辑端点
// 在 FileSessionController 前释放，最后再关闭 Vault、state.db 和便携日志。
// 以下映射只保存运行期租约和页面关系，不作为第二套持久化来源。
struct Application::Impl {
    ~Impl() {
        if (transfer_persistence) {
            transfer_persistence->Stop();
        }
    }

    std::unique_ptr<storage::AppPaths> paths;
    std::unique_ptr<storage::StateStore> state_store;
    std::unique_ptr<storage::ApplicationRepository> repository;
    std::unique_ptr<storage::Vault> vault;
    std::unique_ptr<storage::RotatingLog> log;
    std::unique_ptr<ui::MainWindow> main_window;
    std::shared_ptr<ssh::IUiDispatcher> ui_dispatcher;
    std::unique_ptr<TerminalSessionController> terminal_sessions;
    std::unique_ptr<PortForwardController> port_forwards;
    std::unique_ptr<FirewallHelperClient> firewall_helper;
    std::unordered_map<std::string, storage::PortForwardRecord> port_forward_records;
    std::unique_ptr<FileSessionController> file_sessions;
    std::unique_ptr<TransferSessionPool> transfer_sessions;
    std::shared_ptr<transfer::LocalEndpoint> local_endpoint;
    std::unique_ptr<ui::detail::BackgroundExecutor> local_editor_directory_executor;
    std::shared_ptr<transfer::TransferManager> transfer_manager;
    std::unique_ptr<EditorController> editor_controller;
    mutable std::mutex endpoints_mutex;
    std::unordered_map<std::string, std::shared_ptr<transfer::IFileEndpoint>> remote_endpoints;
    std::unordered_map<std::string, std::string> remote_initial_directories;
    std::unordered_map<std::string, storage::ServerProfile> server_profiles;
    mutable std::mutex server_profiles_mutex;
    std::mutex transfer_authentication_mutex;
    std::unordered_map<ui::TransferPaneId, std::string> transfer_pane_servers;
    std::unordered_map<ui::TransferPaneId, bool> transfer_pane_owns_consumer;
    std::unordered_map<ui::TransferPaneId, ssh::ReactorTaskHandle> transfer_directory_tasks;
    std::unordered_set<std::string> transfer_server_leases;
    std::unordered_map<std::string, ui::TransferPaneId> pending_transfer_servers;
    std::unordered_set<std::string> editor_server_consumers;
    std::unordered_map<std::string, std::vector<std::function<void(Status)>>> editor_server_waiters;
    std::string active_editor_server_id;
    std::uint64_t editor_directory_generation{};
    struct EditorNavigationHistory {
        std::vector<transfer::FileUri> locations;
        std::size_t index{};
    };
    std::unordered_map<std::string, EditorNavigationHistory> editor_navigation_history;
    std::unique_ptr<TransferPersistenceCoordinator> transfer_persistence;

    bool InitializePortableState(ui::theme::Mode& configured_theme) {
        // 存储初始化必须先于窗口：路径是所有数据文件的唯一信任根，主题要在首帧前应用。
        auto paths_result = storage::AppPaths::InitializeForCurrentExecutable();
        if (!paths_result) {
            ShowStartupFailure(L"便携数据目录初始化失败。\r\n\r\n" + paths_result.status().message +
                               L"\r\n\r\n程序不会回退写入 AppData、TEMP 或注册表。");
            return false;
        }
        paths = std::make_unique<storage::AppPaths>(std::move(paths_result).value());
        SHChangeNotify(SHCNE_UPDATEITEM,
                       SHCNF_PATHW | SHCNF_FLUSHNOWAIT,
                       paths->executable_path().c_str(),
                       nullptr);

        const auto crash_reporter_status = storage::CrashReporter::Install(*paths);
        if (!crash_reporter_status) {
            ShowStartupFailure(L"无法在程序目录的 logs\\crash 下启用崩溃报告。\r\n\r\n" +
                               crash_reporter_status.message +
                               L"\r\n\r\n程序不会回退写入 TEMP、AppData 或其他目录。");
            return false;
        }

        std::wstring write_error;
        if (!ProbeApplicationDirectoryWritable(paths->application_dir(), write_error)) {
            ShowStartupFailure(write_error + L"\r\n\r\n请将整个应用目录移动到可写位置后重新启动。"
                                             L"程序不会在 C 盘其他目录创建替代数据。");
            return false;
        }

        auto opened_log = storage::RotatingLog::Open(*paths);
        if (!opened_log) {
            ShowStartupFailure(L"无法在程序目录的 logs 下创建运行日志。\r\n\r\n" +
                               opened_log.status().message +
                               L"\r\n\r\n程序不会回退写入 TEMP、AppData 或其他目录。");
            return false;
        }
        log = std::move(opened_log).value();
        (void)log->Write(storage::LogLevel::Info, "SLink startup");

        auto opened_store = storage::StateStore::Open(paths->state_database());
        if (!opened_store) {
            ShowStartupFailure(L"无法打开程序目录下的 data\\state.db。\r\n\r\n" +
                               opened_store.status().message);
            return false;
        }
        state_store = std::move(opened_store).value();

        auto opened_repository = storage::ApplicationRepository::Open(*state_store);
        if (!opened_repository) {
            ShowStartupFailure(L"无法初始化服务器配置仓储。\r\n\r\n" +
                               opened_repository.status().message);
            return false;
        }
        repository = std::move(opened_repository).value();

        configured_theme = ui::theme::Mode::Dark;
        const auto theme_setting = repository->GetSetting("appearance_theme");
        if (theme_setting && theme_setting.value() && *theme_setting.value() == "light") {
            configured_theme = ui::theme::Mode::Light;
        }
        ui::theme::SetMode(configured_theme);
        auto profiles = repository->ListServerProfiles();
        if (!profiles) {
            ShowStartupFailure(L"无法读取 data\\state.db 中的服务器配置。\r\n\r\n" +
                               profiles.status().message);
            return false;
        }
        for (auto& profile : profiles.value()) {
            server_profiles.emplace(profile.id, std::move(profile));
        }

        auto opened_vault = storage::Vault::Open(paths->vault_database());
        if (opened_vault) {
            vault = std::move(opened_vault).value();
            if (const auto unlocked = vault->UnlockPortable(); !unlocked) {
                ShowStartupFailure(L"无法初始化程序目录下的便携凭据库。\r\n\r\n" +
                                   unlocked.message);
                return false;
            }
        } else if (opened_vault.status().code != storage::StorageErrc::unsupported) {
            ShowStartupFailure(L"无法打开程序目录下的加密凭据库 data\\vault.db。\r\n\r\n" +
                               opened_vault.status().message);
            return false;
        }
        return true;
    }

    std::shared_ptr<transfer::IFileEndpoint> EndpointFor(const transfer::FileUri& uri) const {
        if (uri.is_local()) {
            return local_endpoint;
        }
        if (file_sessions) {
            if (auto endpoint = file_sessions->EndpointFor(uri)) {
                return endpoint;
            }
        }
        std::scoped_lock lock(endpoints_mutex);
        const auto found = remote_endpoints.find(uri.authority());
        return found == remote_endpoints.end() ? nullptr : found->second;
    }

    Result<std::shared_ptr<transfer::IFileEndpoint>>
    OpenTransferConnection(std::string_view server_id) {
        if (!file_sessions) {
            return Status{ErrorCode::unsupported, L"SFTP 文件会话控制器尚未初始化。"};
        }
        storage::ServerProfile profile;
        {
            std::scoped_lock lock(server_profiles_mutex);
            const auto found = server_profiles.find(std::string(server_id));
            if (found == server_profiles.end()) {
                return Status{ErrorCode::not_found, L"服务器配置已不存在。"};
            }
            profile = found->second;
        }

        // Vault 访问和未保存密码提示串行执行，防止并行扩容连接时弹出多次认证交互。
        // SecureBuffer 随后移动到独立连接，认证结束即由 FileSessionController 清零。
        auto authentication = [this, &profile] {
            std::scoped_lock authentication_lock(transfer_authentication_mutex);
            return FileAuthenticationFor(profile);
        }();
        if (!authentication)
            return authentication.status();
        return file_sessions->OpenDedicated(std::move(profile),
                                            std::move(authentication).value(),
                                            FileHostKeyPolicy::require_known);
    }

    void RestoreTransferJobs() {
        // 从 state.db 只恢复排队、暂停和失败的父任务。目录子项不落库，恢复后由
        // TransferManager 重新扫描；无效 URI 或策略记录会被拒绝并从恢复集合清理。
        if (!repository || !transfer_manager) {
            return;
        }
        auto records = repository->ListTransferRecovery();
        if (!records) {
            ShowOperationError(L"无法恢复传输任务",
                               {ErrorCode::io_error, records.status().message});
            return;
        }

        std::size_t rejected = 0;
        for (const auto& record : records.value()) {
            try {
                auto source = transfer::FileUri::parse(record.source_uri);
                auto target = transfer::FileUri::parse(record.target_uri);
                auto effective = transfer::FileUri::parse(record.effective_target_uri);
                if (target.scheme() != effective.scheme() ||
                    target.authority() != effective.authority()) {
                    throw std::invalid_argument("Recovery target endpoint does not match");
                }

                auto conflict = FromRecoveryConflict(record.conflict);
                // A non-empty partial destination must continue at the actual
                // effective target, including an automatically renamed target.
                if (record.bytes_transferred != 0) {
                    target = effective;
                    conflict = transfer::ConflictPolicy::Resume;
                }

                transfer::TransferJob job;
                job.id = record.id;
                job.spec = {std::move(source),
                            std::move(target),
                            conflict,
                            record.move,
                            record.verify_sha256};
                job.effective_target = std::move(effective);
                job.state = record.state == storage::TransferRecoveryState::Failed
                                ? transfer::TransferState::Failed
                                : transfer::TransferState::Paused;
                job.bytes_transferred = record.bytes_transferred;
                job.bytes_total = record.bytes_total;
                job.error = record.error;
                (void)transfer_manager->restore(std::move(job));
            } catch (...) {
                ++rejected;
                (void)repository->RemoveTransferRecovery(record.id);
            }
        }

        if (rejected != 0 && main_window) {
            const std::wstring message = L"有 " + std::to_wstring(rejected) +
                                         L" 条传输恢复记录无效，已安全移除；其他任务仍可继续。";
            MessageBoxW(main_window->hwnd(), message.c_str(), L"传输恢复", MB_OK | MB_ICONWARNING);
        }
    }

    bool TransferJobUsesServer(const transfer::TransferJob& job, std::string_view server_id) const {
        if (IsTerminalTransferState(job.state)) {
            return false;
        }
        return (job.spec.source.is_sftp() && job.spec.source.authority() == server_id) ||
               (job.spec.target.is_sftp() && job.spec.target.authority() == server_id);
    }

    void MaybeReleaseTransferLease(std::string_view server_id) {
        const std::string id(server_id);
        if (!file_sessions) {
            return;
        }
        for (const auto& [pane_id, pane_server] : transfer_pane_servers) {
            (void)pane_id;
            if (pane_server == id) {
                return;
            }
        }
        if (transfer_manager) {
            for (const auto& job : transfer_manager->jobs()) {
                if (TransferJobUsesServer(job, id)) {
                    return;
                }
            }
        }

        if (transfer_sessions) {
            transfer_sessions->CloseServer(id);
        }
        if (!transfer_server_leases.contains(id)) {
            return;
        }
        transfer_server_leases.erase(id);
        file_sessions->Close(id);
        if (file_sessions->consumer_count(id) == 0) {
            std::scoped_lock lock(endpoints_mutex);
            remote_endpoints.erase(id);
            remote_initial_directories.erase(id);
        }
    }

    Status EnsureVaultUnlocked() {
        if (!vault) {
            return {ErrorCode::unsupported, L"当前构建没有可用的 libsodium 凭据库。"};
        }
        if (vault->is_unlocked()) {
            return Status::ok();
        }
        const auto unlocked = vault->UnlockPortable();
        if (!unlocked) {
            return {ErrorCode::io_error, unlocked.message};
        }
        return Status::ok();
    }

    ssh::KeyboardResponseCallback KeyboardResponder(const storage::ServerProfile& profile) const {
        const auto dispatcher = ui_dispatcher;
        const HWND owner = main_window ? main_window->hwnd() : nullptr;
        const std::wstring server_name = Utf8ToWide(profile.name);
        return [dispatcher, owner, server_name](const std::string_view name,
                                                const std::string_view instruction,
                                                const std::span<const ssh::KeyboardPrompt> prompts)
                   -> Result<std::vector<std::string>> {
            if (!dispatcher || !IsWindow(owner)) {
                return Status{ErrorCode::cancelled, L"keyboard-interactive 认证窗口已不可用。"};
            }
            auto state = std::make_shared<KeyboardResponseState>();
            std::vector<ssh::KeyboardPrompt> prompt_copy(prompts.begin(), prompts.end());
            const std::wstring title =
                server_name.empty() ? L"SSH keyboard-interactive" : L"认证 " + server_name;
            const std::wstring name_text = Utf8ToWide(name);
            const std::wstring instruction_text = Utf8ToWide(instruction);
            try {
                dispatcher->post([state,
                                  owner,
                                  title,
                                  name_text,
                                  instruction_text,
                                  prompt_copy = std::move(prompt_copy)]() mutable {
                    std::vector<std::string> responses;
                    responses.reserve(prompt_copy.size());
                    Status failure = Status::ok();
                    for (const auto& prompt : prompt_copy) {
                        std::wstring description;
                        if (!name_text.empty()) {
                            description += name_text + L"\r\n";
                        }
                        if (!instruction_text.empty()) {
                            description += instruction_text + L"\r\n";
                        }
                        description += Utf8ToWide(prompt.text);
                        auto value =
                            SecurePrompt::Show(owner, title, std::move(description), !prompt.echo);
                        if (!value) {
                            failure = {ErrorCode::cancelled,
                                       L"用户取消了 keyboard-interactive "
                                       L"认证。"};
                            break;
                        }
                        responses.push_back(WideToUtf8(*value));
                        if (!value->empty()) {
                            SecureZeroMemory(value->data(), value->size() * sizeof(wchar_t));
                        }
                    }
                    {
                        std::scoped_lock lock(state->mutex);
                        if (failure) {
                            state->responses = std::move(responses);
                        } else {
                            for (auto& response : responses) {
                                if (!response.empty()) {
                                    SecureZeroMemory(response.data(), response.size());
                                }
                            }
                            state->failure = std::move(failure);
                        }
                        state->ready = true;
                    }
                    state->changed.notify_all();
                });
            } catch (...) {
                return Status{ErrorCode::io_error, L"无法调度 keyboard-interactive 认证窗口。"};
            }

            std::unique_lock lock(state->mutex);
            state->changed.wait(lock, [&state] {
                return state->ready;
            });
            if (!state->responses) {
                return state->failure;
            }
            return std::move(*state->responses);
        };
    }

    Result<TerminalAuthentication> AuthenticationFor(const storage::ServerProfile& profile) {
        // 把 credential_ref 解析为一次性终端认证对象。Vault 解密结果移动进 SecureBuffer，
        // 格式错误或用户取消都在进入 Reactor 前返回。
        TerminalAuthentication result;
        result.username = profile.username;
        switch (profile.authentication) {
        case storage::AuthenticationKind::OpenSshAgent:
            result.mode = TerminalAuthentication::Mode::open_ssh_agent;
            return std::move(result);
        case storage::AuthenticationKind::KeyboardInteractive:
            result.mode = TerminalAuthentication::Mode::keyboard_interactive;
            result.keyboard_respond = KeyboardResponder(profile);
            return std::move(result);
        case storage::AuthenticationKind::Password:
        case storage::AuthenticationKind::PrivateKey:
            break;
        }

        if (profile.credential_ref) {
            if (const auto unlocked = EnsureVaultUnlocked(); !unlocked) {
                return unlocked;
            }
            auto encoded = vault->Get(*profile.credential_ref);
            if (!encoded) {
                return Status{ErrorCode::authentication_failed, encoded.status().message};
            }
            auto decoded = DecodeStoredCredential(encoded.value().bytes());
            encoded.value().clear();
            if (!decoded) {
                return Status{ErrorCode::authentication_failed, decoded.status().message};
            }
            auto credential = std::move(decoded).value();
            if (profile.authentication == storage::AuthenticationKind::Password) {
                if (credential.kind != StoredCredentialKind::Password) {
                    return Status{ErrorCode::authentication_failed,
                                  L"服务器认证方式与加密凭据类型不匹配。"};
                }
                result.mode = TerminalAuthentication::Mode::password;
                result.secret = std::move(credential.password);
                return std::move(result);
            }
            if (credential.kind != StoredCredentialKind::PrivateKey ||
                credential.private_key.empty()) {
                return Status{ErrorCode::authentication_failed,
                              L"服务器认证方式与加密私钥凭据不匹配。"};
            }
            result.mode = TerminalAuthentication::Mode::private_key;
            result.private_key = std::move(credential.private_key);
            result.private_key_passphrase = std::move(credential.private_key_passphrase);
            return std::move(result);
        }

        if (profile.authentication == storage::AuthenticationKind::PrivateKey) {
            return Status{ErrorCode::authentication_failed,
                          L"该服务器没有关联加密私钥。请在“设置 → "
                          L"服务器管理”中保存私钥。"};
        }

        auto password = SecurePrompt::Show(
            main_window->hwnd(),
            L"连接 " + Utf8ToWide(profile.name),
            L"请输入 " + Utf8ToWide(profile.username) + L"@" + Utf8ToWide(profile.host) +
                L" 的 SSH 密码。\r\n此密码仅用于本次连接，不会保存。");
        if (!password) {
            return Status{ErrorCode::cancelled, L"用户取消了密码输入。"};
        }
        std::string password_utf8 = WideToUtf8(*password);
        if (!password->empty()) {
            SecureZeroMemory(password->data(), password->size() * sizeof(wchar_t));
        }
        try {
            result.secret = storage::SecureBuffer(password_utf8.size());
        } catch (...) {
            if (!password_utf8.empty()) {
                SecureZeroMemory(password_utf8.data(), password_utf8.size());
            }
            return Status{ErrorCode::io_error, L"无法分配锁定内存保存临时密码。"};
        }
        if (!password_utf8.empty()) {
            std::memcpy(result.secret.bytes().data(), password_utf8.data(), password_utf8.size());
            SecureZeroMemory(password_utf8.data(), password_utf8.size());
        }
        result.mode = TerminalAuthentication::Mode::password;
        return std::move(result);
    }

    Result<FileSessionAuthentication> FileAuthenticationFor(const storage::ServerProfile& profile) {
        auto terminal = AuthenticationFor(profile);
        if (!terminal) {
            return terminal.status();
        }
        auto value = std::move(terminal).value();
        switch (value.mode) {
        case TerminalAuthentication::Mode::password:
            return FileSessionAuthentication{FilePasswordAuthentication{std::move(value.secret)}};
        case TerminalAuthentication::Mode::private_key:
            return FileSessionAuthentication{FilePrivateKeyAuthentication{
                {}, std::move(value.private_key), std::move(value.private_key_passphrase)}};
        case TerminalAuthentication::Mode::open_ssh_agent:
            return FileSessionAuthentication{FileAgentAuthentication{}};
        case TerminalAuthentication::Mode::keyboard_interactive:
            return FileSessionAuthentication{
                FileKeyboardInteractiveAuthentication{std::move(value.keyboard_respond)}};
        }
        return Status{ErrorCode::unsupported, L"未知的 SSH 认证方式。"};
    }

    bool ConfirmHostKey(const storage::ServerProfile& profile, const ssh::HostKeyInfo& key) const {
        const std::wstring text = L"这是首次连接到该服务器：\r\n\r\n" + Utf8ToWide(profile.name) +
                                  L"\r\n" + Utf8ToWide(profile.host) + L":" +
                                  std::to_wstring(profile.port) + L"\r\n\r\n算法：" +
                                  Utf8ToWide(key.algorithm) + L"\r\nSHA-256 指纹：\r\n" +
                                  Utf8ToWide(key.sha256_fingerprint) +
                                  L"\r\n\r\n仅在你已经通过可信渠道核对指纹后选择“是”。";
        return MessageBoxW(main_window->hwnd(),
                           text.c_str(),
                           L"确认 SSH 主机密钥",
                           MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES;
    }

    void ShowOperationError(std::wstring_view title, const Status& status) const {
        if (status.code() == ErrorCode::cancelled || !main_window) {
            return;
        }
        MessageBoxW(main_window->hwnd(),
                    status.message().c_str(),
                    std::wstring(title).c_str(),
                    MB_OK | MB_ICONERROR);
    }

    using FileOpenCompletion =
        std::function<void(Status, std::shared_ptr<ssh::ReactorEndpoint>, std::string)>;

    void OpenFileSessionAttempt(storage::ServerProfile profile,
                                const FileHostKeyPolicy policy,
                                FileOpenCompletion completion) {
        // 每次尝试都重新从 Vault 取得凭据。首次 unknown host key 由上层确认后以
        // accept_new 重试；mismatch 没有自动覆盖路径。
        if (!file_sessions) {
            completion({ErrorCode::unsupported, L"SFTP 文件会话控制器尚未初始化。"}, nullptr, {});
            return;
        }
        auto authentication = FileAuthenticationFor(profile);
        if (!authentication) {
            completion(authentication.status(), nullptr, {});
            return;
        }
        // Keep the profile used by the completion separate from the value
        // passed to AsyncOpen. Function argument evaluation order is not
        // guaranteed; capturing `profile = std::move(profile)` in the lambda
        // from the same call could move its strings before AsyncOpen received
        // its copy, producing an empty id/host/username for SFTP only.
        auto completion_profile = profile;
        (void)file_sessions->AsyncOpen(
            std::move(profile),
            std::move(authentication).value(),
            policy,
            [this,
             profile = std::move(completion_profile),
             policy,
             completion = std::move(completion)](FileSessionOpenResult result) mutable {
                if (policy == FileHostKeyPolicy::require_known &&
                    result.host_key_confirmation_required()) {
                    if (ConfirmHostKey(profile, *result.host_key)) {
                        OpenFileSessionAttempt(std::move(profile),
                                               FileHostKeyPolicy::accept_new,
                                               std::move(completion));
                    } else {
                        completion({ErrorCode::host_key_rejected,
                                    L"首次连接的主机密钥未获确认，连接已取消。"},
                                   nullptr,
                                   {});
                    }
                    return;
                }
                if (result.status && result.endpoint) {
                    std::scoped_lock lock(endpoints_mutex);
                    remote_endpoints[profile.id] = result.endpoint;
                    remote_initial_directories[profile.id] = result.initial_directory;
                }
                completion(std::move(result.status),
                           std::move(result.endpoint),
                           std::move(result.initial_directory));
            });
    }

    void OpenTransferServer(std::string server_id,
                            std::optional<ui::TransferPaneId> existing_pane = std::nullopt) {
        // 防止同一服务器被连续双击并发打开。第一次成功连接保留服务器级传输租约，
        // 后续每个可见窗格另持消费者，使关闭窗格不会中断排队或活动传输。
        if (server_id == kLocalHostServerId) {
            if (main_window) {
                main_window->transfer_page().AddLocalPane(L"本地主机");
            }
            return;
        }
        const auto found = server_profiles.find(server_id);
        if (found == server_profiles.end() || !main_window) {
            return;
        }
        if (pending_transfer_servers.contains(server_id)) {
            return;
        }
        const auto profile = found->second;
        const ui::TransferPaneId pane_id = existing_pane
                                               ? *existing_pane
                                               : main_window->transfer_page().AddPendingRemotePane(
                                                     Utf8ToWide(profile.name), profile.id);
        pending_transfer_servers.emplace(server_id, pane_id);
        main_window->transfer_page().SetPaneViewState(
            pane_id, {ui::ViewPhase::Connecting, L"正在连接服务器…", ui::ViewRecoveryAction::None});
        OpenFileSessionAttempt(
            profile,
            FileHostKeyPolicy::require_known,
            [this, server_id = std::move(server_id), profile, pane_id](
                Status status,
                std::shared_ptr<ssh::ReactorEndpoint> endpoint,
                std::string initial_directory) {
                const auto pending = pending_transfer_servers.find(server_id);
                if (pending == pending_transfer_servers.end() || pending->second != pane_id ||
                    !main_window || !main_window->transfer_page().pane_location(pane_id)) {
                    if (status && endpoint && file_sessions) {
                        file_sessions->Close(server_id);
                    }
                    return;
                }
                pending_transfer_servers.erase(pending);
                const auto show_error = [&](const Status& error) {
                    const bool manage = error.code() == ErrorCode::authentication_failed ||
                                        error.code() == ErrorCode::host_key_rejected ||
                                        error.code() == ErrorCode::invalid_argument ||
                                        !server_profiles.contains(server_id);
                    main_window->transfer_page().SetPaneViewState(
                        pane_id,
                        {ui::ViewPhase::Error,
                         error.message().empty() ? L"无法连接服务器" : error.message(),
                         manage ? ui::ViewRecoveryAction::ManageServer
                                : ui::ViewRecoveryAction::Retry});
                };
                if (!status || !endpoint) {
                    show_error(status);
                    return;
                }
                const auto current = server_profiles.find(profile.id);
                if (current == server_profiles.end() || current->second != profile) {
                    if (file_sessions) {
                        file_sessions->Close(profile.id);
                    }
                    show_error({ErrorCode::conflict, L"连接期间服务器配置发生变化，请重新打开。"});
                    return;
                }
                if (!main_window->transfer_page().BindPaneEndpoint(
                        pane_id,
                        std::move(endpoint),
                        transfer::FileUri::sftp(profile.id, std::move(initial_directory)))) {
                    if (file_sessions) {
                        file_sessions->Close(profile.id);
                    }
                    show_error({ErrorCode::io_error, L"无法加载远程文件面板。"});
                    return;
                }
                transfer_pane_servers[pane_id] = profile.id;
                // The first successful open for a server becomes an
                // application-level transfer lease. It remains connected
                // until TransferManager has stopped at shutdown, so closing
                // the last visible pane cannot invalidate an active or queued
                // transfer. Later pane opens own an additional consumer.
                transfer_pane_owns_consumer[pane_id] =
                    !transfer_server_leases.insert(profile.id).second;
            });
    }

    void CloseTransferPane(const ui::TransferPaneId pane_id) {
        const auto directory_task = transfer_directory_tasks.find(pane_id);
        if (directory_task != transfer_directory_tasks.end()) {
            directory_task->second.cancel();
            transfer_directory_tasks.erase(directory_task);
        }
        for (auto pending = pending_transfer_servers.begin();
             pending != pending_transfer_servers.end();
             ++pending) {
            if (pending->second == pane_id) {
                pending_transfer_servers.erase(pending);
                break;
            }
        }
        const auto found = transfer_pane_servers.find(pane_id);
        if (found == transfer_pane_servers.end() || !file_sessions) {
            return;
        }
        const std::string server_id = found->second;
        transfer_pane_servers.erase(found);
        const auto ownership = transfer_pane_owns_consumer.find(pane_id);
        const bool owns_consumer =
            ownership != transfer_pane_owns_consumer.end() && ownership->second;
        if (ownership != transfer_pane_owns_consumer.end()) {
            transfer_pane_owns_consumer.erase(ownership);
        }
        if (owns_consumer) {
            file_sessions->Close(server_id);
        }
        if (file_sessions->consumer_count(server_id) == 0) {
            std::scoped_lock lock(endpoints_mutex);
            remote_endpoints.erase(server_id);
            remote_initial_directories.erase(server_id);
        }
        MaybeReleaseTransferLease(server_id);
    }

    void InvalidateSharedFileSession(const std::string& server_id) {
        // FileSessionController 的 consumer 计数只对活连接有意义。一旦 I/O 已经
        // 证实共享会话损坏，同时丢弃应用层所有“已持有”标记，避免新
        // 会话被旧面板的 Close 误减计数。可见旧面板仍保留，用户重新打开
        // 服务器时会取得新消费者。
        editor_server_consumers.erase(server_id);
        transfer_server_leases.erase(server_id);
        for (const auto& [pane_id, pane_server_id] : transfer_pane_servers) {
            if (pane_server_id == server_id) {
                transfer_pane_owns_consumer[pane_id] = false;
            }
        }
        if (file_sessions) {
            file_sessions->Invalidate(server_id);
        }
        std::scoped_lock lock(endpoints_mutex);
        remote_endpoints.erase(server_id);
        remote_initial_directories.erase(server_id);
    }

    static bool IsRecoverableEditorSessionFailure(const Status& status) noexcept {
        switch (status.code()) {
        case ErrorCode::not_found:
        case ErrorCode::io_error:
        case ErrorCode::network_error:
        case ErrorCode::protocol_error:
            return true;
        default:
            return false;
        }
    }

    void ShowEditorDirectoryError(const Status& status) {
        if (!main_window) {
            return;
        }
        const bool manage = status.code() == ErrorCode::authentication_failed ||
                            status.code() == ErrorCode::host_key_rejected ||
                            (!server_profiles.contains(active_editor_server_id) &&
                             active_editor_server_id != kLocalHostServerId);
        main_window->editor_page().SetDirectoryState(
            {ui::ViewPhase::Error,
             status.message().empty() ? L"无法读取目录" : status.message(),
             manage ? ui::ViewRecoveryAction::ManageServer : ui::ViewRecoveryAction::Retry});
    }

    void RetryEditorDirectory() {
        if (active_editor_server_id.empty()) {
            return;
        }
        const auto found = editor_navigation_history.find(active_editor_server_id);
        if (found == editor_navigation_history.end() || found->second.locations.empty()) {
            LoadEditorInitialDirectory(active_editor_server_id);
            return;
        }
        LoadEditorDirectory(
            active_editor_server_id, found->second.locations[found->second.index], false);
    }

    void EnsureEditorServer(std::string server_id, std::function<void(Status)> completion) {
        // 编辑器按服务器共享一个文件会话消费者。连接中的重复请求合并到 waiters，
        // 连接完成后一次性唤醒，避免同时打开多个文件时重复认证。
        if (server_id == kLocalHostServerId) {
            completion(Status::ok());
            return;
        }
        if (editor_server_consumers.contains(server_id)) {
            // keepalive 会在底层淘汰断线会话，但应用层的编辑器消费者
            // 标记不能因此就视为连接仍存在。用控制器索引做无阻塞校验，
            // 记录已消失时立即进入下方的正常重连流程。
            const auto probe = transfer::FileUri::sftp(server_id, "/");
            if (file_sessions && file_sessions->EndpointFor(probe)) {
                completion(Status::ok());
                return;
            }
            InvalidateSharedFileSession(server_id);
        }
        if (auto waiting = editor_server_waiters.find(server_id);
            waiting != editor_server_waiters.end()) {
            waiting->second.push_back(std::move(completion));
            return;
        }
        const auto profile = server_profiles.find(server_id);
        if (profile == server_profiles.end()) {
            completion({ErrorCode::not_found, L"服务器配置已不存在。"});
            return;
        }
        editor_server_waiters[server_id].push_back(std::move(completion));
        OpenFileSessionAttempt(profile->second,
                               FileHostKeyPolicy::require_known,
                               [this, server_id](Status status,
                                                 std::shared_ptr<ssh::ReactorEndpoint> endpoint,
                                                 std::string) mutable {
                                   auto waiting = editor_server_waiters.find(server_id);
                                   if (status && endpoint) {
                                       editor_server_consumers.insert(server_id);
                                       if (editor_controller) {
                                           editor_controller->RebindEndpoint(server_id, endpoint);
                                           editor_controller->RecoverUnsaved();
                                       }
                                   }
                                   std::vector<std::function<void(Status)>> callbacks;
                                   if (waiting != editor_server_waiters.end()) {
                                       callbacks = std::move(waiting->second);
                                       editor_server_waiters.erase(waiting);
                                   }
                                   for (auto& callback : callbacks) {
                                       callback(status);
                                   }
                               });
    }

    void PresentEditorDirectory(const std::string& server_id,
                                const transfer::FileUri& directory,
                                std::vector<transfer::DirectoryEntry> directory_entries,
                                const std::uint64_t generation) {
        if (!main_window || generation != editor_directory_generation ||
            server_id != active_editor_server_id) {
            return;
        }
        std::vector<ui::EditorFileEntry> entries;
        if (directory.parent() != directory) {
            ui::EditorFileEntry parent;
            parent.name = L"..";
            parent.status = L"上级目录";
            parent.directory = true;
            entries.push_back(std::move(parent));
        }
        entries.reserve(entries.size() + directory_entries.size());
        for (const auto& item : directory_entries) {
            if (item.name.empty() || item.name.front() == '.') {
                continue;
            }
            const bool is_directory = item.stat.kind == transfer::FileKind::Directory;
            ui::EditorFileEntry entry;
            entry.name = Utf8ToWide(item.name);
            entry.size = is_directory ? std::wstring{} : FormatFileSize(item.stat.size);
            entry.modified = FormatModifiedTime(item.stat.modified);
            entry.directory = is_directory;
            entry.local_shortcut =
                directory.is_local() && ui::local_shortcut::IsShortcutName(entry.name);
            if (entry.local_shortcut) {
                entry.status = L"快捷方式";
                entry.icon_path = Utf8ToWide(item.uri.path());
            } else {
                switch (item.stat.kind) {
                case transfer::FileKind::Directory:
                    entry.status = L"文件夹";
                    break;
                case transfer::FileKind::Regular:
                    entry.status = L"文件";
                    break;
                case transfer::FileKind::Symlink:
                    entry.status = L"符号链接";
                    break;
                case transfer::FileKind::Other:
                    entry.status = L"其他";
                    break;
                }
            }
            entry.permissions = FormatPermissions(item.stat.permissions);
            entry.owner = Utf8ToWide(item.stat.owner);
            entries.push_back(std::move(entry));
        }
        main_window->editor_page().SetRemotePath(Utf8ToWide(directory.path()));
        main_window->editor_page().SetFileEntries(std::move(entries));
    }

    void LoadEditorDirectoryConnected(const std::string& server_id,
                                      transfer::FileUri directory,
                                      const std::uint64_t generation,
                                      const bool reconnect_allowed = true) {
        if (!file_sessions || !main_window) {
            return;
        }
        const std::string requested_uri = directory.to_string();
        (void)file_sessions->AsyncList(
            std::move(directory),
            [this, server_id, generation, requested_uri, reconnect_allowed](
                FileDirectoryListResult result) {
                if (generation != editor_directory_generation ||
                    server_id != active_editor_server_id ||
                    result.directory.to_string() != requested_uri) {
                    return;
                }
                if (!result.status) {
                    if (reconnect_allowed && IsRecoverableEditorSessionFailure(result.status)) {
                        // 目录请求是最早能确认端点已失效的地方。强制淘汰旧
                        // 会话并只重试一次；连续失败直接报错，防止离线时循环建连。
                        auto retry_directory = std::move(result.directory);
                        InvalidateSharedFileSession(server_id);
                        EnsureEditorServer(
                            server_id,
                            [this,
                             server_id,
                             generation,
                             retry_directory = std::move(retry_directory)](Status status) mutable {
                                if (generation != editor_directory_generation ||
                                    server_id != active_editor_server_id) {
                                    return;
                                }
                                if (!status) {
                                    ShowEditorDirectoryError(status);
                                    return;
                                }
                                LoadEditorDirectoryConnected(
                                    server_id, std::move(retry_directory), generation, false);
                            });
                        return;
                    }
                    ShowEditorDirectoryError(result.status);
                    return;
                }
                PresentEditorDirectory(
                    server_id, result.directory, std::move(result.entries), generation);
            });
    }

    void LoadLocalEditorDirectory(transfer::FileUri directory, const std::uint64_t generation) {
        if (!main_window || !local_endpoint || !local_editor_directory_executor) {
            return;
        }
        const auto endpoint = local_endpoint;
        const std::string server_id(kLocalHostServerId);
        const bool submitted = local_editor_directory_executor->Submit(
            [this, endpoint, server_id, directory = std::move(directory), generation]() mutable {
                std::vector<transfer::DirectoryEntry> entries;
                std::wstring error;
                try {
                    entries = endpoint->list(directory);
                } catch (const std::exception& exception) {
                    error = Utf8ToWide(exception.what());
                } catch (...) {
                    error = L"读取本地目录时发生未知错误。";
                }
                if (!main_window) {
                    return;
                }
                main_window->PostUiTask([this,
                                         server_id,
                                         directory = std::move(directory),
                                         entries = std::move(entries),
                                         error = std::move(error),
                                         generation]() mutable {
                    if (generation != editor_directory_generation ||
                        server_id != active_editor_server_id) {
                        return;
                    }
                    if (!error.empty()) {
                        ShowEditorDirectoryError({ErrorCode::io_error, std::move(error)});
                        return;
                    }
                    PresentEditorDirectory(server_id, directory, std::move(entries), generation);
                });
            });
        if (!submitted) {
            ShowEditorDirectoryError({ErrorCode::io_error, L"本地目录后台执行器已停止。"});
        }
    }

    void RecordEditorNavigation(const std::string& server_id, const transfer::FileUri& directory) {
        auto& history = editor_navigation_history[server_id];
        if (!history.locations.empty() && history.locations[history.index] == directory) {
            return;
        }
        if (history.index + 1 < history.locations.size()) {
            history.locations.erase(history.locations.begin() +
                                        static_cast<std::ptrdiff_t>(history.index + 1),
                                    history.locations.end());
        }
        history.locations.push_back(directory);
        history.index = history.locations.size() - 1;
    }

    void ResetEditorNavigation(const std::string& server_id, const transfer::FileUri& directory) {
        auto& history = editor_navigation_history[server_id];
        history.locations = {directory};
        history.index = 0;
    }

    void NavigateEditorHistory(const bool forward) {
        const auto found = editor_navigation_history.find(active_editor_server_id);
        if (found == editor_navigation_history.end() || found->second.locations.empty()) {
            return;
        }
        auto& history = found->second;
        const std::size_t target =
            forward ? history.index + 1
                    : (history.index == 0 ? history.locations.size() : history.index - 1);
        if (target >= history.locations.size())
            return;
        history.index = target;
        LoadEditorDirectory(active_editor_server_id, history.locations[target], false);
    }

    void LoadEditorDirectory(const std::string& server_id,
                             transfer::FileUri directory,
                             const bool record_history = true) {
        // 本地目录交给单线程文件工作池；远端目录先确保共享 SFTP 会话，再由 Reactor
        // 枚举。editor_directory_generation 会丢弃服务器/路径切换前的迟到结果。
        if (record_history) {
            RecordEditorNavigation(server_id, directory);
        }
        active_editor_server_id = server_id;
        const std::uint64_t generation = ++editor_directory_generation;
        if (main_window) {
            main_window->editor_page().SetRemotePath(Utf8ToWide(directory.path()));
            main_window->editor_page().SetDirectoryState(
                {ui::ViewPhase::Loading, L"正在加载目录…", ui::ViewRecoveryAction::None});
        }
        if (server_id == kLocalHostServerId && directory.is_local()) {
            LoadLocalEditorDirectory(std::move(directory), generation);
            return;
        }
        EnsureEditorServer(
            server_id,
            [this, server_id, directory = std::move(directory), generation](Status status) mutable {
                if (generation != editor_directory_generation ||
                    server_id != active_editor_server_id) {
                    return;
                }
                if (!status) {
                    ShowEditorDirectoryError(status);
                    return;
                }
                LoadEditorDirectoryConnected(server_id, std::move(directory), generation);
            });
    }

    void LoadEditorInitialDirectory(std::string server_id) {
        active_editor_server_id = server_id;
        const std::uint64_t generation = ++editor_directory_generation;
        if (main_window) {
            main_window->editor_page().SetRemotePath({});
            main_window->editor_page().SetDirectoryState(
                {ui::ViewPhase::Connecting, L"正在连接服务器…", ui::ViewRecoveryAction::None});
        }
        if (server_id == kLocalHostServerId) {
            auto directory = DesktopDirectoryUri();
            ResetEditorNavigation(server_id, directory);
            if (main_window) {
                main_window->editor_page().SetRemotePath(Utf8ToWide(directory.path()));
            }
            LoadLocalEditorDirectory(std::move(directory), generation);
            return;
        }
        // Keep the lookup argument independent from the callback capture.
        // Function argument evaluation order is unspecified; moving the same
        // string into the lambda in this call could otherwise empty the ID
        // before EnsureEditorServer performs its profile lookup.
        const std::string requested_server_id = server_id;
        EnsureEditorServer(
            requested_server_id,
            [this, server_id = std::move(server_id), generation](Status status) mutable {
                if (generation != editor_directory_generation ||
                    server_id != active_editor_server_id) {
                    return;
                }
                if (!status) {
                    ShowEditorDirectoryError(status);
                    return;
                }
                std::string initial_directory = "/";
                {
                    std::scoped_lock lock(endpoints_mutex);
                    const auto found = remote_initial_directories.find(server_id);
                    if (found != remote_initial_directories.end() && !found->second.empty()) {
                        initial_directory = found->second;
                    }
                }
                auto directory = transfer::FileUri::sftp(server_id, std::move(initial_directory));
                ResetEditorNavigation(server_id, directory);
                main_window->editor_page().SetRemotePath(Utf8ToWide(directory.path()));
                LoadEditorDirectoryConnected(server_id, std::move(directory), generation);
            });
    }

    std::vector<const storage::ServerProfile*> SortedProfiles() const {
        std::vector<const storage::ServerProfile*> result;
        result.reserve(server_profiles.size());
        for (const auto& [id, profile] : server_profiles) {
            (void)id;
            result.push_back(&profile);
        }
        std::ranges::sort(result, {}, [](const storage::ServerProfile* profile) {
            return profile->name;
        });
        return result;
    }

    void RefreshProfileViews() {
        // 按名称排序内存快照，一次性重建页面的服务器选项和共享侧栏。共享侧栏只展示
        // 远端配置；本地主机的终端和文件传输能力继续保留在各页面内部。
        if (!main_window) {
            return;
        }
        auto stored_groups = repository->ListServerGroups();
        if (!stored_groups) {
            MessageBoxW(main_window->hwnd(),
                        stored_groups.status().message.c_str(),
                        L"无法读取服务器分组",
                        MB_OK | MB_ICONERROR);
            return;
        }
        const auto profiles = SortedProfiles();
        auto& terminal = main_window->terminal_page();
        terminal.ClearServers();
        terminal.AddServer(std::string(kLocalHostServerId), L"本地主机", L"CMD", true);
        std::vector<ui::ServerSidebarItem> shared_servers;
        std::vector<ui::ServerSidebarGroup> shared_groups;
        std::vector<ui::TransferServerOption> transfer_servers;
        std::vector<ui::EditorServerOption> editor_servers;
        shared_servers.reserve(profiles.size());
        shared_groups.reserve(stored_groups.value().size());
        for (const auto& group : stored_groups.value()) {
            shared_groups.push_back({group.id, Utf8ToWide(group.name), group.sort_order});
        }
        transfer_servers.reserve(profiles.size() + 1);
        editor_servers.reserve(profiles.size() + 1);
        transfer_servers.push_back({std::string(kLocalHostServerId), L"本地主机", true});
        editor_servers.push_back({std::string(kLocalHostServerId), L"本地主机", true});
        for (const auto* profile : profiles) {
            terminal.AddServer(profile->id, Utf8ToWide(profile->name), Utf8ToWide(profile->name));
            shared_servers.push_back(
                {profile->id, Utf8ToWide(profile->name), false, profile->group_id});
            transfer_servers.push_back({profile->id, Utf8ToWide(profile->name), false});
            editor_servers.push_back({profile->id, Utf8ToWide(profile->name), false});
        }
        main_window->SetServerSidebarItems(std::move(shared_groups), std::move(shared_servers));
        main_window->transfer_page().SetServerOptions(std::move(transfer_servers));
        main_window->editor_page().SetServers(std::move(editor_servers));
    }

    void ReloadProfiles() {
        // 服务器管理器提交后重新读取仓库。已删除配置从页面列表移除；活动会话不在此
        // 同步强制断开，而由页面关闭或控制器生命周期安全释放。
        if (!repository) {
            return;
        }
        auto profiles = repository->ListServerProfiles();
        if (!profiles) {
            MessageBoxW(main_window ? main_window->hwnd() : nullptr,
                        profiles.status().message.c_str(),
                        L"无法重新加载服务器配置",
                        MB_OK | MB_ICONERROR);
            return;
        }
        std::unordered_map<std::string, storage::ServerProfile> replacement;
        for (auto& profile : profiles.value()) {
            replacement.emplace(profile.id, std::move(profile));
        }
        std::vector<std::string> changed_servers;
        for (const auto& [id, old_profile] : server_profiles) {
            const auto replacement_profile = replacement.find(id);
            if (replacement_profile == replacement.end() ||
                replacement_profile->second.host != old_profile.host ||
                replacement_profile->second.port != old_profile.port ||
                replacement_profile->second.username != old_profile.username ||
                replacement_profile->second.initial_directory != old_profile.initial_directory ||
                replacement_profile->second.authentication != old_profile.authentication ||
                replacement_profile->second.credential_ref != old_profile.credential_ref) {
                changed_servers.push_back(id);
            }
        }
        if (main_window && file_sessions) {
            for (const auto& server_id : changed_servers) {
                std::vector<ui::TransferPaneId> pane_ids;
                for (const auto& [pane_id, pane_server] : transfer_pane_servers) {
                    if (pane_server == server_id) {
                        pane_ids.push_back(pane_id);
                    }
                }
                for (const auto pane_id : pane_ids) {
                    main_window->transfer_page().ClosePane(pane_id);
                }
                if (editor_server_consumers.erase(server_id) != 0) {
                    file_sessions->Close(server_id);
                }
                if (transfer_server_leases.erase(server_id) != 0) {
                    file_sessions->Close(server_id);
                }
                {
                    std::scoped_lock lock(endpoints_mutex);
                    remote_endpoints.erase(server_id);
                    remote_initial_directories.erase(server_id);
                }
                if (active_editor_server_id == server_id) {
                    active_editor_server_id.clear();
                    ++editor_directory_generation;
                    main_window->editor_page().SetFileEntries({});
                }
                editor_navigation_history.erase(server_id);
            }
        }
        {
            std::scoped_lock lock(server_profiles_mutex);
            server_profiles = std::move(replacement);
        }
        RefreshProfileViews();
    }

    void OpenServerManager() {
        if (!main_window || !repository) {
            return;
        }
        (void)ShowServerManagerDialog(main_window->hwnd(), *repository, vault.get(), [this] {
            ReloadProfiles();
        });
        ReloadProfiles();
    }

    void ConnectServer(std::string_view server_id, std::size_t pane_index) {
        auto* pane = main_window->terminal_page().pane_at(pane_index);
        if (!pane || !terminal_sessions) {
            return;
        }
        main_window->terminal_page().FocusPane(pane_index);
        if (server_id == kLocalHostServerId) {
            terminal_sessions->ConnectLocalCmd(*pane);
            return;
        }
        const auto profile = server_profiles.find(std::string(server_id));
        if (profile == server_profiles.end()) {
            return;
        }
        terminal_sessions->Connect(*pane, profile->second);
    }

    void DisconnectPane(std::size_t pane_index) {
        if (!terminal_sessions || !main_window) {
            return;
        }
        if (auto* pane = main_window->terminal_page().pane_at(pane_index)) {
            terminal_sessions->Disconnect(*pane);
        }
    }

    void ShowPortForwardStorageError(std::wstring_view action,
                                     const storage::StorageStatus& status) {
        MessageBoxW(main_window ? main_window->hwnd() : nullptr,
                    (std::wstring(action) + L"\r\n\r\n" + status.message).c_str(),
                    L"端口转发配置",
                    MB_OK | MB_ICONERROR);
    }

    static std::string PortForwardIdentity(std::string_view bind_address,
                                           std::uint16_t local_port) {
        return std::string(bind_address) + ":" + std::to_string(local_port);
    }

    void
    RemovePersistedFirewallRules(const std::optional<std::string_view> session = std::nullopt) {
        if (!repository)
            return;
        auto rules = repository->ListFirewallRules();
        if (!rules)
            return;
        for (const auto& rule : rules.value()) {
            if (!session || rule.session_id == *session) {
                (void)repository->RemoveFirewallRule(rule.session_id, rule.local_port);
            }
        }
    }

    void ResetPersistedFirewallFlags() {
        if (!repository)
            return;
        for (auto& [identity, record] : port_forward_records) {
            (void)identity;
            if (!record.firewall_enabled)
                continue;
            record.firewall_enabled = false;
            (void)repository->UpsertPortForward(record);
        }
    }

    void UpdatePortForwardEntry(const storage::PortForwardRecord& record,
                                PortForwardState state,
                                std::wstring message) {
        if (!main_window)
            return;
        ui::PortForwardEntry entry;
        entry.bind_address = record.bind_address;
        entry.local_port = record.local_port;
        entry.local_endpoint =
            Utf8ToWide(record.bind_address) + L":" + std::to_wstring(record.local_port);
        entry.target_endpoint =
            Utf8ToWide(record.target_address) + L":" + std::to_wstring(record.target_port);
        entry.process = std::move(message);
        entry.running = state == PortForwardState::running;
        entry.failed = state == PortForwardState::failed;
        entry.proxy_enabled = record.proxy_enabled;
        entry.lan_listener = record.bind_address == "0.0.0.0";
        entry.firewall_open = record.firewall_enabled;
        main_window->port_forward_page().UpsertEntry(std::move(entry));
    }

    void StartPortForward(std::string_view bind_address, std::uint16_t local_port) {
        const std::string identity = PortForwardIdentity(bind_address, local_port);
        const auto found = port_forward_records.find(identity);
        if (found == port_forward_records.end() || !found->second.proxy_enabled || !port_forwards) {
            return;
        }
        const auto record = found->second;
        UpdatePortForwardEntry(record, PortForwardState::starting, L"正在启动本机监听...");
        TcpForwardSpec spec;
        spec.bind_address = record.bind_address;
        spec.local_port = record.local_port;
        spec.target_address = record.target_address;
        spec.target_port = record.target_port;
        port_forwards->Start(std::move(spec), [this, identity](PortForwardUpdate update) {
            const auto current = port_forward_records.find(identity);
            if (current == port_forward_records.end())
                return;
            if (update.state == PortForwardState::stopped && current->second.proxy_enabled) {
                return;
            }
            if (update.state == PortForwardState::failed) {
                current->second.proxy_enabled = false;
                current->second.firewall_enabled = false;
                if (repository) {
                    (void)repository->UpsertPortForward(current->second);
                }
            }
            std::wstring message = std::move(update.message);
            if (update.state == PortForwardState::stopped)
                message = L"已关闭";
            UpdatePortForwardEntry(current->second, update.state, std::move(message));
        });
    }

    bool SetProxyEnabled(std::string_view bind_address, std::uint16_t local_port, bool enabled) {
        // 先调整持久化意图，再启动/停止实际监听。同一端口另一监听地址正在运行时先
        // 关闭它，但保留两条配置；控制器回调负责更新最终运行状态。
        const std::string identity = PortForwardIdentity(bind_address, local_port);
        const auto found = port_forward_records.find(identity);
        if (found == port_forward_records.end() || !repository)
            return false;

        if (enabled) {
            // Windows 不能同时绑定 0.0.0.0:port 和 127.0.0.1:port，因此配置可以并存，
            // 但运行状态必须互斥。
            if (port_forwards)
                port_forwards->Stop(local_port);
            for (auto& [other_identity, other] : port_forward_records) {
                if (other_identity == identity || other.local_port != local_port ||
                    !other.proxy_enabled) {
                    continue;
                }
                other.proxy_enabled = false;
                other.firewall_enabled = false;
                const auto stopped = repository->UpsertPortForward(other);
                if (!stopped) {
                    ShowPortForwardStorageError(L"无法关闭同端口的另一个代理。", stopped);
                    return false;
                }
                if (firewall_helper && other.bind_address == "0.0.0.0")
                    firewall_helper->SetRule(local_port, false);
                if (main_window) {
                    main_window->port_forward_page().SetProxyState(
                        other.bind_address, local_port, false, false, false, L"已关闭");
                    main_window->port_forward_page().SetFirewallRuleState(
                        other.bind_address, local_port, false, true);
                }
            }
        }
        auto replacement = found->second;
        replacement.proxy_enabled = enabled;
        if (!enabled)
            replacement.firewall_enabled = false;
        const auto saved = repository->UpsertPortForward(replacement);
        if (!saved) {
            ShowPortForwardStorageError(L"无法保存代理开关状态。", saved);
            if (main_window) {
                main_window->port_forward_page().SetProxyState(
                    found->second.bind_address,
                    local_port,
                    found->second.proxy_enabled,
                    found->second.proxy_enabled,
                    false,
                    found->second.proxy_enabled ? L"运行中" : L"已关闭");
            }
            return false;
        }
        found->second = replacement;
        if (enabled) {
            StartPortForward(found->second.bind_address, local_port);
        } else {
            if (firewall_helper && found->second.bind_address == "0.0.0.0")
                firewall_helper->SetRule(local_port, false);
            if (port_forwards)
                port_forwards->Stop(local_port);
            if (main_window) {
                main_window->port_forward_page().SetProxyState(
                    found->second.bind_address, local_port, false, false, false, L"已关闭");
                main_window->port_forward_page().SetFirewallRuleState(
                    found->second.bind_address, local_port, false, true);
            }
        }
        return true;
    }

    void DeletePortForward(std::string_view bind_address, std::uint16_t local_port) {
        const std::string identity = PortForwardIdentity(bind_address, local_port);
        const auto found = port_forward_records.find(identity);
        if (found == port_forward_records.end() || !repository)
            return;
        (void)SetProxyEnabled(bind_address, local_port, false);
        const auto removed = repository->RemovePortForward(std::string(bind_address), local_port);
        if (!removed) {
            ShowPortForwardStorageError(L"无法删除端口转发配置。", removed);
            return;
        }
        port_forward_records.erase(identity);
        if (main_window)
            main_window->port_forward_page().RemoveEntry(bind_address, local_port);
    }

    void StopFileProducers() {
        // 先停止会产生新文件/SFTP 工作的上层对象。EditorController 先停止编辑线程，
        // TransferManager 随后等待全部传输工作线程退出，连接池此后不会再收到新租约。
        local_editor_directory_executor.reset();
        editor_controller.reset();
        transfer_manager.reset();
        // TransferManager 析构会发出最后的暂停快照；等它停止后再刷新并关闭持久化线程。
        if (transfer_persistence) {
            transfer_persistence->Stop();
            transfer_persistence.reset();
        }
        if (transfer_sessions) {
            transfer_sessions->Shutdown();
            transfer_sessions.reset();
        }
    }

    void CloseFileSessions() {
        // 关闭页面持有的共享文件会话消费者，取消仍在枚举目录的 Reactor 任务，
        // 再清空端点快照；顺序相反会让回调访问已经释放的页面/端点关系。
        if (main_window) {
            main_window->transfer_page().ResetRemotePanes();
        }
        if (file_sessions) {
            for (const auto& server_id : editor_server_consumers) {
                file_sessions->Close(server_id);
            }
            for (const auto& server_id : transfer_server_leases) {
                file_sessions->Close(server_id);
            }
        }
        editor_server_consumers.clear();
        transfer_server_leases.clear();
        editor_server_waiters.clear();
        transfer_pane_servers.clear();
        transfer_pane_owns_consumer.clear();
        for (const auto& [pane_id, task] : transfer_directory_tasks) {
            (void)pane_id;
            task.cancel();
        }
        transfer_directory_tasks.clear();
        pending_transfer_servers.clear();
        {
            std::scoped_lock lock(endpoints_mutex);
            remote_endpoints.clear();
            remote_initial_directories.clear();
        }
        if (file_sessions) {
            file_sessions->Shutdown();
            file_sessions.reset();
        }
        local_endpoint.reset();
    }

    void StopPortForwarding() {
        // 先停止普通端口代理，再通知管理员辅助进程删除防火墙规则。只有辅助进程
        // 确认清理成功才删除恢复记录，异常失败的记录留给下次启动继续处理。
        if (port_forwards) {
            port_forwards->Shutdown();
            port_forwards.reset();
        }
        if (firewall_helper) {
            const auto helper_state = firewall_helper->state();
            const bool cleaned = firewall_helper->Shutdown();
            if (cleaned && (helper_state == FirewallHelperState::running ||
                            helper_state == FirewallHelperState::stopping)) {
                RemovePersistedFirewallRules();
            }
            firewall_helper.reset();
        }
        ResetPersistedFirewallFlags();
    }

    void StopTerminalAndFlushStorage() {
        // 终端 Reactor 是最后一批网络生产者。全部停止后释放 Dispatcher，保证之后
        // 不再有后台线程向主窗口投递任务。
        if (terminal_sessions) {
            terminal_sessions->Shutdown();
            terminal_sessions.reset();
        }
        ui_dispatcher.reset();
        // 最后清零 Vault 密钥并刷新数据库/日志。持久化对象晚于所有业务线程关闭，
        // 避免退出尾部回调写入已经关闭的后端。
        if (vault) {
            vault->Lock();
        }
        if (repository) {
            repository->Flush();
        }
        if (log) {
            (void)log->Flush();
            log.reset();
        }
    }

    void Shutdown() {
        // 退出协调只表达依赖顺序：先停生产者，再关文件会话、代理和终端，最后刷库。
        if (log) {
            (void)log->Write(storage::LogLevel::Info, "SLink shutdown");
        }
        StopFileProducers();
        CloseFileSessions();
        StopPortForwarding();
        StopTerminalAndFlushStorage();
    }
};

Application::Application() : impl_(std::make_unique<Impl>()) {}
Application::~Application() = default;

int Application::Run(HINSTANCE instance, int show_command) {
    // 阶段 1：初始化进程级 Windows 运行环境。
    // DPI 必须在创建任何 HWND 前设置；COM STA 是 Shell 图标、快捷方式和文件对话框的
    // 基础；通用控件类也必须先注册。任一步失败都直接终止，避免进入半初始化 UI。
    if (!InitializeDpiAwareness()) {
        ShowStartupFailure(
            WindowsErrorText(L"无法启用 Per-Monitor V2 高 DPI 模式", GetLastError()));
        return kStartupFailureExitCode;
    }

    ComApartment com;
    if (!com.Initialize()) {
        ShowStartupFailure(L"无法初始化 COM 单线程单元。\r\n\r\nHRESULT: " +
                           HResultText(com.result()));
        return kStartupFailureExitCode;
    }

    INITCOMMONCONTROLSEX common_controls{sizeof(common_controls)};
    common_controls.dwICC =
        ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES | ICC_TAB_CLASSES | ICC_PROGRESS_CLASS;
    if (!InitCommonControlsEx(&common_controls)) {
        ShowStartupFailure(WindowsErrorText(L"无法初始化 Windows 通用控件", GetLastError()));
        return kStartupFailureExitCode;
    }

    // 阶段 2–3：便携路径、崩溃日志、数据库、主题、服务器列表和 Vault 作为一个
    // 前置初始化单元；失败时不创建窗口，也不退回写入系统目录。
    ui::theme::Mode configured_theme{};
    if (!impl_->InitializePortableState(configured_theme)) {
        return kStartupFailureExitCode;
    }

    // 阶段 4：创建主窗口及跨线程 UI 投递桥。
    // 先有有效 HWND，NetworkReactor 才能安全通过 PostUiTask 投递完成回调。主题变更先
    // 写库，保存失败时回调返回 false，让页面保持原主题而不制造显示/持久化分歧。
    impl_->main_window = std::make_unique<ui::MainWindow>();
    if (!impl_->main_window->Create(instance, show_command)) {
        ShowStartupFailure(WindowsErrorText(L"无法创建 SLink 主窗口", GetLastError()));
        impl_->main_window.reset();
        return kStartupFailureExitCode;
    }

    auto* application_state = impl_.get();
    auto* main_window = impl_->main_window.get();
    impl_->main_window->SetThemePreference(
        configured_theme, [application_state](const ui::theme::Mode mode) {
            if (!application_state->repository || !application_state->main_window) {
                return false;
            }
            const auto saved = application_state->repository->SetSetting(
                "appearance_theme", mode == ui::theme::Mode::Light ? "light" : "dark");
            if (!saved) {
                MessageBoxW(application_state->main_window->hwnd(),
                            (L"无法保存主题设置。\r\n\r\n" + saved.message).c_str(),
                            L"SLink",
                            MB_OK | MB_ICONERROR);
                return false;
            }
            return true;
        });
    std::vector<std::string> collapsed_groups;
    if (auto stored = impl_->repository->GetSetting("server_sidebar_collapsed_groups");
        stored && stored.value()) {
        std::string_view ids = *stored.value();
        while (!ids.empty()) {
            const auto comma = ids.find(',');
            const auto id = ids.substr(0, comma);
            if (!id.empty()) {
                collapsed_groups.emplace_back(id);
            }
            if (comma == std::string_view::npos) {
                break;
            }
            ids.remove_prefix(comma + 1);
        }
    }
    impl_->main_window->SetCollapsedServerGroups(
        std::move(collapsed_groups), [application_state](const std::vector<std::string>& ids) {
            std::string serialized;
            for (const auto& id : ids) {
                if (!serialized.empty()) {
                    serialized += ',';
                }
                serialized += id;
            }
            const auto saved = application_state->repository->SetSetting(
                "server_sidebar_collapsed_groups", std::move(serialized));
            if (!saved) {
                MessageBoxW(application_state->main_window->hwnd(),
                            (L"无法保存服务器分组展开状态。\r\n\r\n" + saved.message).c_str(),
                            L"SLink",
                            MB_OK | MB_ICONERROR);
            }
            return saved.ok();
        });
    impl_->ui_dispatcher =
        std::make_shared<ssh::FunctionUiDispatcher>([main_window](std::function<void()> callback) {
            main_window->PostUiTask(std::move(callback));
        });
    // 阶段 5：按依赖顺序创建网络控制器。
    // 文件浏览控制器提供共享浏览/编辑会话；传输池通过工厂创建独立连接；终端每窗格
    // 独占 Reactor。这里不主动连接服务器，只注册后续用户操作需要的能力。
    impl_->file_sessions =
        std::make_unique<FileSessionController>(impl_->ui_dispatcher, impl_->paths->known_hosts());
    impl_->transfer_sessions =
        std::make_unique<TransferSessionPool>([application_state](std::string_view server_id) {
            return application_state->OpenTransferConnection(server_id);
        });
    impl_->RefreshProfileViews();
    auto& terminal = impl_->main_window->terminal_page();
    impl_->terminal_sessions = std::make_unique<TerminalSessionController>(
        impl_->main_window->hwnd(),
        impl_->ui_dispatcher,
        impl_->paths->known_hosts(),
        [application_state](const storage::ServerProfile& profile) {
            return application_state->AuthenticationFor(profile);
        },
        [application_state](const storage::ServerProfile& profile, const ssh::HostKeyInfo& key) {
            return application_state->ConfirmHostKey(profile, key);
        });
    impl_->terminal_sessions->SetStateCallback(
        [application_state](ui::TerminalControl& pane, ui::ViewState state) {
            if (application_state->main_window) {
                application_state->main_window->terminal_page().SetPaneViewState(pane,
                                                                                 std::move(state));
            }
        });
    impl_->port_forwards = std::make_unique<PortForwardController>(impl_->ui_dispatcher);

    // 阶段 6：绑定端口转发和管理员防火墙辅助进程。
    // UI 只发出请求；控制器回调再更新内存、数据库和页面。只有辅助进程确认清理完成后
    // 才删除持久化的规则记录，以便异常退出后的下一次启动仍能发现并清理残留规则。
    auto& port_forward_page = impl_->main_window->port_forward_page();
    impl_->firewall_helper = std::make_unique<FirewallHelperClient>(
        impl_->ui_dispatcher,
        [application_state](FirewallHelperUpdate update) {
            if (!application_state->main_window)
                return;
            ui::FirewallHelperUiState ui_state = ui::FirewallHelperUiState::off;
            switch (update.state) {
            case FirewallHelperState::off:
                ui_state = ui::FirewallHelperUiState::off;
                break;
            case FirewallHelperState::starting:
                ui_state = ui::FirewallHelperUiState::starting;
                break;
            case FirewallHelperState::running:
                ui_state = ui::FirewallHelperUiState::running;
                break;
            case FirewallHelperState::stopping:
                ui_state = ui::FirewallHelperUiState::stopping;
                break;
            case FirewallHelperState::error:
                ui_state = ui::FirewallHelperUiState::error;
                break;
            }
            if (update.state == FirewallHelperState::running) {
                // 辅助进程初始化时已删除所有陈旧 SLink 会话规则。只有收到成功状态后，
                // 才能丢弃用于异常恢复清理的持久化记录。
                application_state->RemovePersistedFirewallRules();
                application_state->ResetPersistedFirewallFlags();
                for (const auto& [identity, record] : application_state->port_forward_records) {
                    (void)identity;
                    application_state->main_window->port_forward_page().SetFirewallRuleState(
                        record.bind_address, record.local_port, false, true);
                }
            } else if (update.state == FirewallHelperState::off && !update.session_id.empty()) {
                application_state->RemovePersistedFirewallRules(update.session_id);
                application_state->ResetPersistedFirewallFlags();
            }
            application_state->main_window->port_forward_page().SetFirewallHelperState(
                ui_state, std::move(update.message));
        },
        [application_state](FirewallRuleUpdate update) {
            if (update.succeeded && application_state->repository) {
                const auto found = std::ranges::find_if(
                    application_state->port_forward_records, [&](const auto& item) {
                        return item.second.bind_address == "0.0.0.0" &&
                               item.second.local_port == update.local_port;
                    });
                if (found != application_state->port_forward_records.end()) {
                    found->second.firewall_enabled = update.enabled;
                    (void)application_state->repository->UpsertPortForward(found->second);
                }
                if (update.enabled) {
                    (void)application_state->repository->UpsertFirewallRule(
                        {update.session_id, update.local_port, update.rule_name});
                } else if (!update.session_id.empty()) {
                    (void)application_state->repository->RemoveFirewallRule(update.session_id,
                                                                            update.local_port);
                }
            }
            if (application_state->main_window) {
                application_state->main_window->port_forward_page().SetFirewallRuleState(
                    "0.0.0.0",
                    update.local_port,
                    update.enabled,
                    update.succeeded,
                    std::move(update.message));
            }
        });
    port_forward_page.SetFirewallHelperRequestedCallback([application_state](bool enabled) {
        if (application_state->firewall_helper)
            application_state->firewall_helper->SetEnabled(enabled);
    });
    port_forward_page.SetFirewallRuleRequestedCallback(
        [application_state](std::string bind_address, std::uint16_t local_port, bool enabled) {
            if (bind_address == "0.0.0.0" && application_state->firewall_helper)
                application_state->firewall_helper->SetRule(local_port, enabled);
        });
    port_forward_page.SetStartRequestedCallback(
        [application_state](ui::PortForwardRequest request) {
            if (!application_state->repository)
                return;
            const std::string identity =
                application_state->PortForwardIdentity(request.bind_address, request.local_port);
            const auto existing = application_state->port_forward_records.find(identity);
            if (existing != application_state->port_forward_records.end() &&
                existing->second.proxy_enabled) {
                (void)application_state->SetProxyEnabled(
                    request.bind_address, request.local_port, false);
            }
            storage::PortForwardRecord record;
            record.local_port = request.local_port;
            record.bind_address = std::move(request.bind_address);
            record.target_address = std::move(request.target_address);
            record.target_port = request.target_port;
            record.proxy_enabled = false;
            record.firewall_enabled = false;
            const auto saved = application_state->repository->UpsertPortForward(record);
            if (!saved) {
                application_state->ShowPortForwardStorageError(L"无法保存端口转发配置。", saved);
                return;
            }
            application_state->port_forward_records[identity] = record;
            application_state->UpdatePortForwardEntry(record, PortForwardState::stopped, L"已关闭");
        });
    port_forward_page.SetProxyRequestedCallback(
        [application_state](std::string bind_address, std::uint16_t local_port, bool enabled) {
            (void)application_state->SetProxyEnabled(bind_address, local_port, enabled);
        });
    port_forward_page.SetDeleteRequestedCallback(
        [application_state](std::string bind_address, std::uint16_t local_port) {
            application_state->DeletePortForward(bind_address, local_port);
        });

    // 恢复端口转发配置。代理开关可以恢复，但同一端口只允许启动一个监听地址；防火墙
    // 权限绝不静默恢复，必须由用户显式启动管理员辅助进程后重新开启。
    auto persisted_forwards = impl_->repository->ListPortForwards();
    if (!persisted_forwards) {
        impl_->ShowPortForwardStorageError(L"无法加载端口转发配置。", persisted_forwards.status());
    } else {
        std::unordered_set<std::uint16_t> enabled_ports;
        for (auto record : persisted_forwards.value()) {
            // 防火墙权限与规则不自动恢复。持久化清理记录保留到提权进程确认删除，
            // 但页面开关始终从关闭状态开始。
            if (record.firewall_enabled) {
                record.firewall_enabled = false;
                (void)impl_->repository->UpsertPortForward(record);
            }
            if (record.proxy_enabled && !enabled_ports.insert(record.local_port).second) {
                record.proxy_enabled = false;
                (void)impl_->repository->UpsertPortForward(record);
            }
            const std::string identity =
                impl_->PortForwardIdentity(record.bind_address, record.local_port);
            impl_->port_forward_records[identity] = record;
            impl_->UpdatePortForwardEntry(record,
                                          PortForwardState::stopped,
                                          record.proxy_enabled ? L"等待启动..." : L"已关闭");
        }
        for (const auto& [identity, record] : impl_->port_forward_records) {
            (void)identity;
            if (record.proxy_enabled) {
                impl_->StartPortForward(record.bind_address, record.local_port);
            }
        }
    }
    const auto pending_firewall = impl_->repository->ListFirewallRules();
    if (pending_firewall && !pending_firewall.value().empty()) {
        port_forward_page.SetFirewallHelperState(
            ui::FirewallHelperUiState::off,
            L"检测到 " + std::to_wstring(pending_firewall.value().size()) + L" 条待清理防火墙规则");
    }
    // 阶段 7：绑定终端页和服务器管理器。
    // 页面只传递 server-id 与 pane-index；认证、主机密钥确认和连接生命周期全部由
    // TerminalSessionController/Application 处理。配置提交后立即刷新三个模块的侧栏。
    terminal.SetServerActivatedCallback(
        [application_state](std::string_view server_id, std::size_t pane_index) {
            application_state->ConnectServer(server_id, pane_index);
        });
    terminal.SetPaneClosingCallback([application_state](std::size_t pane_index) {
        application_state->DisconnectPane(pane_index);
    });
    impl_->main_window->SetServerManagerCallback([application_state] {
        application_state->OpenServerManager();
    });

    // 阶段 8：创建文件级传输调度器并恢复父任务。
    // TransferPage 自带一个可关闭的本地窗格；启动时不恢复远程面板。远程文件工作项
    // 通过 TransferSessionPool 租用独立 SFTP 连接，本地工作项复用无状态 LocalEndpoint。
    auto& transfer = impl_->main_window->transfer_page();
    transfer.ResetRemotePanes();

    impl_->local_endpoint = std::make_shared<transfer::LocalEndpoint>();
    impl_->local_editor_directory_executor = std::make_unique<ui::detail::BackgroundExecutor>(1);
    impl_->transfer_manager = std::make_shared<transfer::TransferManager>(
        [application_state](const transfer::FileUri& uri,
                            std::shared_ptr<const std::atomic_bool> cancellation) {
            if (uri.is_local()) {
                return std::shared_ptr<transfer::IFileEndpoint>(application_state->local_endpoint);
            }
            if (!application_state->transfer_sessions) {
                return std::shared_ptr<transfer::IFileEndpoint>{};
            }
            return application_state->transfer_sessions->Acquire(uri, std::move(cancellation));
        });
    impl_->transfer_persistence =
        std::make_unique<TransferPersistenceCoordinator>(*impl_->repository);
    impl_->transfer_persistence->Start();
    // observer 可能从传输工作线程触发：这里只合并最新父任务快照并唤醒专用持久化线程；
    // 涉及页面和会话租约的操作仍必须再次投递回主窗口线程。
    impl_->transfer_manager->set_observer([application_state](transfer::TransferJob snapshot) {
        if (application_state->transfer_persistence) {
            application_state->transfer_persistence->Queue(snapshot);
        }
        if (!IsTerminalTransferState(snapshot.state) || !application_state->main_window) {
            return;
        }
        std::vector<std::string> servers;
        if (snapshot.spec.source.is_sftp()) {
            servers.push_back(snapshot.spec.source.authority());
        }
        if (snapshot.spec.target.is_sftp() &&
            (servers.empty() || servers.front() != snapshot.spec.target.authority())) {
            servers.push_back(snapshot.spec.target.authority());
        }
        if (!servers.empty()) {
            application_state->main_window->PostUiTask(
                [application_state, servers = std::move(servers)] {
                    for (const auto& server_id : servers) {
                        application_state->MaybeReleaseTransferLease(server_id);
                    }
                });
        }
    });
    impl_->RestoreTransferJobs();
    transfer.SetTransferManager(impl_->transfer_manager);
    transfer.SetOpenRemoteSessionCallback([application_state](std::string server_id) {
        application_state->OpenTransferServer(std::move(server_id));
    });
    transfer.SetPaneClosedCallback([application_state](ui::TransferPaneId pane_id) {
        application_state->CloseTransferPane(pane_id);
    });
    transfer.SetPaneActionCallback(
        [application_state](ui::TransferPaneId pane_id, ui::ViewRecoveryAction action) {
            if (action == ui::ViewRecoveryAction::ManageServer) {
                application_state->OpenServerManager();
                return;
            }
            if (action != ui::ViewRecoveryAction::Retry || !application_state->main_window) {
                return;
            }
            if (const auto connected = application_state->transfer_pane_servers.find(pane_id);
                connected != application_state->transfer_pane_servers.end()) {
                const auto probe = transfer::FileUri::sftp(connected->second, "/");
                if (!application_state->file_sessions ||
                    !application_state->file_sessions->EndpointFor(probe)) {
                    application_state->OpenTransferServer(connected->second, pane_id);
                    return;
                }
                (void)application_state->main_window->transfer_page().RefreshPane(pane_id);
                return;
            }
            const auto location =
                application_state->main_window->transfer_page().pane_location(pane_id);
            if (location && location->is_sftp()) {
                application_state->OpenTransferServer(location->authority(), pane_id);
            }
        });
    // 文件面板的远程目录枚举必须经 FileSessionController 所属 Reactor。每个 pane 只
    // 保留最新任务句柄；新请求先取消旧任务，完成回调再核对 task-id，防止旧目录覆盖
    // 用户刚刚进入的新路径。
    transfer.SetAsyncDirectoryList(
        [application_state](ui::TransferPaneId pane_id,
                            std::shared_ptr<transfer::IFileEndpoint> /*endpoint*/,
                            transfer::FileUri location,
                            ui::TransferPage::DirectoryListCompletion completion) {
            if (!application_state->file_sessions) {
                completion({}, L"SFTP 文件会话已关闭。");
                return;
            }
            const auto existing = application_state->transfer_directory_tasks.find(pane_id);
            if (existing != application_state->transfer_directory_tasks.end()) {
                existing->second.cancel();
                application_state->transfer_directory_tasks.erase(existing);
            }
            auto task_id = std::make_shared<ssh::ReactorTaskId>(0);
            auto task = application_state->file_sessions->AsyncList(
                std::move(location),
                [application_state, pane_id, task_id, completion = std::move(completion)](
                    FileDirectoryListResult result) mutable {
                    const auto active = application_state->transfer_directory_tasks.find(pane_id);
                    if (active != application_state->transfer_directory_tasks.end() &&
                        active->second.id() == *task_id) {
                        application_state->transfer_directory_tasks.erase(active);
                    }
                    if (!result.status) {
                        if (application_state->IsRecoverableEditorSessionFailure(result.status)) {
                            application_state->InvalidateSharedFileSession(
                                result.directory.authority());
                        }
                        completion({}, result.status.message());
                        return;
                    }
                    completion(std::move(result.entries), {});
                });
            *task_id = task.id();
            application_state->transfer_directory_tasks.insert_or_assign(pane_id, std::move(task));
        });
    // 阶段 9：创建远程编辑协调器并绑定文件导航。
    // 页面负责选择服务器、路径和文件；EditorController 负责后台缓存/保存。目录和 .lnk
    // 由这里解析成 FileUri，普通文件才交给文档控制器打开。
    impl_->editor_controller =
        std::make_unique<EditorController>(impl_->main_window->editor_page(),
                                           *impl_->paths,
                                           [application_state](const transfer::FileUri& uri) {
                                               return application_state->EndpointFor(uri);
                                           });
    impl_->editor_controller->SetFileActivationResolver(
        [application_state](
            const ui::EditorFileActivation& activation) -> std::optional<transfer::FileUri> {
            if (activation.entry.directory) {
                return std::nullopt;
            }
            if (activation.server_id == kLocalHostServerId) {
                return transfer::FileUri::local(WideToUtf8(activation.directory))
                    .joined(WideToUtf8(activation.entry.name));
            }
            const auto found = application_state->server_profiles.find(activation.server_id);
            if (found == application_state->server_profiles.end()) {
                return std::nullopt;
            }
            return transfer::FileUri::sftp(found->second.id, WideToUtf8(activation.directory))
                .joined(WideToUtf8(activation.entry.name));
        });
    impl_->editor_controller->SetDirectoryActivationCallback(
        [application_state](const ui::EditorFileActivation& activation) {
            const bool local = activation.server_id == kLocalHostServerId;
            if (!local && !application_state->server_profiles.contains(activation.server_id)) {
                return;
            }
            if (local && activation.entry.local_shortcut) {
                const auto shortcut_uri = transfer::FileUri::local(WideToUtf8(activation.directory))
                                              .joined(WideToUtf8(activation.entry.name));
                const auto target = ui::local_shortcut::Resolve(Utf8ToWide(shortcut_uri.path()));
                if (target.kind == ui::local_shortcut::TargetKind::this_pc) {
                    application_state->LoadEditorDirectory(activation.server_id,
                                                           transfer::FileUri::local("/"));
                } else if (target.kind == ui::local_shortcut::TargetKind::directory) {
                    application_state->LoadEditorDirectory(
                        activation.server_id, transfer::FileUri::local(WideToUtf8(target.path)));
                } else if (target.kind == ui::local_shortcut::TargetKind::file &&
                           application_state->editor_controller) {
                    application_state->editor_controller->Open(
                        transfer::FileUri::local(WideToUtf8(target.path)));
                }
                return;
            }
            auto directory = local ? transfer::FileUri::local(WideToUtf8(activation.directory))
                                   : transfer::FileUri::sftp(activation.server_id,
                                                             WideToUtf8(activation.directory));
            if (activation.entry.name == L"..") {
                directory = directory.parent();
            } else {
                directory = directory.joined(WideToUtf8(activation.entry.name));
            }
            application_state->LoadEditorDirectory(activation.server_id, std::move(directory));
        });
    impl_->main_window->editor_page().SetServerChangedCallback(
        [application_state](std::string server_id) {
            if (server_id != kLocalHostServerId &&
                !application_state->server_profiles.contains(server_id)) {
                application_state->RefreshProfileViews();
                return;
            }
            application_state->LoadEditorInitialDirectory(std::move(server_id));
        });
    impl_->main_window->editor_page().SetPathChangedCallback(
        [application_state](std::string server_id, std::wstring path) {
            const bool local = server_id == kLocalHostServerId;
            if (!local && !application_state->server_profiles.contains(server_id)) {
                application_state->RefreshProfileViews();
                return;
            }
            const std::string requested_path = WideToUtf8(path);
            application_state->LoadEditorDirectory(
                server_id,
                local ? transfer::FileUri::local(requested_path.empty() ? "/" : requested_path)
                      : transfer::FileUri::sftp(server_id,
                                                requested_path.empty() ? "/" : requested_path));
        });
    impl_->main_window->editor_page().SetHistoryNavigationCallback(
        [application_state](const bool forward) {
            application_state->NavigateEditorHistory(forward);
        });
    impl_->main_window->editor_page().SetViewActionCallback(
        [application_state](ui::ViewRecoveryAction action) {
            if (action == ui::ViewRecoveryAction::Retry) {
                application_state->RetryEditorDirectory();
            } else if (action == ui::ViewRecoveryAction::ManageServer) {
                application_state->OpenServerManager();
            }
        });
    // 保存、冲突差异和另存为仍由应用层协调：UI 只收集目标路径，文档服务执行冲突检查
    // 与原子写回。差异预览限制两侧大小，避免一次冲突复制过多文本到 Scintilla。
    impl_->editor_controller->SetSaveAsTargetResolver(
        [application_state](const transfer::FileUri& current) -> std::optional<transfer::FileUri> {
            if (!application_state->main_window || !current.is_sftp()) {
                return std::nullopt;
            }
            auto value = SecurePrompt::Show(application_state->main_window->hwnd(),
                                            L"远程另存为",
                                            L"请输入同一服务器上的完整远程路径：",
                                            false,
                                            Utf8ToWide(current.path()));
            if (!value || value->empty()) {
                return std::nullopt;
            }
            return current.with_path(WideToUtf8(*value));
        });
    impl_->editor_controller->SetConflictDiffCallback(
        [application_state](ui::EditorDocumentId source_id,
                            const transfer::FileUri& uri,
                            const editor::ConflictSnapshot& snapshot) {
            if (!application_state->main_window) {
                return;
            }
            if (snapshot.remote_missing || snapshot.remote_too_large || snapshot.remote_is_binary) {
                std::wstring message =
                    snapshot.remote_missing     ? L"远端文件已被删除，无法生成文本差异。"
                    : snapshot.remote_too_large ? L"远端文件超过 20 MB，未载入差异内容。"
                                                : L"远端文件是二进制内容，无法生成文本差异。";
                MessageBoxW(application_state->main_window->hwnd(),
                            message.c_str(),
                            L"查看远端差异",
                            MB_OK | MB_ICONINFORMATION);
                return;
            }

            constexpr std::size_t kMaximumSideBytes = 2U * 1024U * 1024U;
            const auto append_side = [kMaximumSideBytes](std::string& output,
                                                         std::string_view label,
                                                         const std::string& text) {
                output.append(label);
                output.push_back('\n');
                const std::size_t count = std::min(text.size(), kMaximumSideBytes);
                output.append(text.data(), count);
                if (count != 0 && output.back() != '\n') {
                    output.push_back('\n');
                }
                if (text.size() > count) {
                    output.append("... [内容过大，差异视图已截断]\n");
                }
            };
            std::string comparison;
            comparison.reserve(std::min(snapshot.local_text.size(), kMaximumSideBytes) +
                               std::min(snapshot.remote_text.size(), kMaximumSideBytes) + 256);
            append_side(comparison, "===== 当前编辑缓存 =====", snapshot.local_text);
            append_side(comparison, "===== 服务器当前版本 =====", snapshot.remote_text);

            ui::EditorDocumentDescriptor document;
            document.id = (std::uint64_t{1} << 63U) | source_id;
            document.title = L"差异 · " + Utf8ToWide(uri.filename());
            document.server_id = uri.is_sftp() ? uri.authority() : std::string(kLocalHostServerId);
            document.remote_path = L"本地缓存 ↔ " + Utf8ToWide(uri.path());
            document.content = std::move(comparison);
            document.encoding = ui::EditorTextEncoding::Utf8;
            document.eol = ui::EditorEolStyle::Lf;
            document.kind = ui::EditorDocumentKind::Text;
            document.read_only = true;
            application_state->main_window->editor_page().OpenDocument(std::move(document));
        });
    // 从文件传输页发起远程编辑时，先确保该服务器的共享文件会话已建立，再切换页面、
    // 加载父目录并打开文档；这样编辑器不会拿到尚未连接或即将销毁的端点。
    transfer.SetRemoteEditCallback([application_state](const transfer::FileUri& uri) {
        if (!application_state->editor_controller || !application_state->main_window ||
            !uri.is_sftp()) {
            return;
        }
        const auto profile = application_state->server_profiles.find(uri.authority());
        if (profile == application_state->server_profiles.end()) {
            return;
        }
        application_state->EnsureEditorServer(
            uri.authority(), [application_state, uri](Status status) {
                if (!status) {
                    application_state->ShowOperationError(L"无法打开远程编辑器", status);
                    return;
                }
                std::vector<ui::EditorServerOption> servers;
                servers.push_back({std::string(kLocalHostServerId), L"本地主机", true});
                for (const auto* item : application_state->SortedProfiles()) {
                    servers.push_back({item->id, Utf8ToWide(item->name), false});
                }
                application_state->main_window->editor_page().SetServers(std::move(servers),
                                                                         uri.authority());
                application_state->main_window->SelectServerSidebarItem(uri.authority());
                application_state->main_window->ShowPage(ui::MainPage::Editor);
                application_state->LoadEditorDirectory(uri.authority(), uri.parent());
                application_state->editor_controller->Open(uri);
            });
    });
    // 阶段 10：所有依赖和回调就绪后才进入消息循环。
    // 循环退出后先由 Impl::Shutdown 停止后台任务、Reactor、代理和辅助进程，再销毁窗口；
    // 最后关闭 Repository/StateStore/Vault，保证任何尾部回调都不会访问已关闭数据库。
    const int exit_code = impl_->main_window->RunMessageLoop();
    impl_->Shutdown();
    impl_->main_window.reset();
    impl_->repository.reset();
    impl_->state_store.reset();
    impl_->vault.reset();
    return exit_code;
}

} // namespace SLink::app
