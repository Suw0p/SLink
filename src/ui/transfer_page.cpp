// 文件传输页面实现：动态标签/左右分屏、虚拟目录表、拖放操作、上下文菜单、异步枚举、
// 冲突交互和可展开的传输队列详情。
#include "ui/transfer_page.hpp"
#include "core/perf_trace.hpp"

#include "transfer/local_endpoint.hpp"
#include "ui/background_executor.hpp"
#include "ui/file_icon_provider.hpp"
#include "ui/file_operation_dialogs.hpp"
#include "ui/local_shortcut.hpp"
#include "ui/retained_ui.hpp"
#include "ui/table_control.hpp"
#include "ui/transfer_conflict_dialog.hpp"
#include "ui/transfer_file_rows.hpp"
#include "ui/transfer_page_surface.hpp"
#include "ui/transfer_pane_history.hpp"
#include "ui/transfer_queue_model.hpp"
#include "ui/win32_ui_internal.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <functional>
#include <mutex>
#include <numeric>
#include <shellapi.h>
#include <shlobj_core.h>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <windowsx.h>

#pragma comment(lib, "Shell32.lib")

namespace SLink::ui {
namespace {

#ifndef WM_DPICHANGED_AFTERPARENT
#define WM_DPICHANGED_AFTERPARENT 0x02E3
#endif

constexpr wchar_t kTransferPageClass[] = L"SLink.TransferPage";
constexpr wchar_t kTransferPaneClass[] = L"SLink.TransferPane";
constexpr wchar_t kTransferPathFrameClass[] = L"SLink.TransferPathFrame";
constexpr wchar_t kResizeGuideClass[] = L"SLink.TransferResizeGuide";

constexpr int kQueueListId = 3101;
constexpr int kQueuePauseId = 3102;
constexpr int kQueueResumeId = 3103;
constexpr int kQueueCancelId = 3104;
constexpr int kQueueRetryId = 3105;
constexpr int kQueueClearId = 3114;
constexpr int kConcurrencyEditId = 3117;
constexpr int kResizeGuideId = 3118;
constexpr int kInlineNameEditId = 3119;
constexpr COLORREF kResizeGuideTransparentColor = RGB(1, 2, 3);
constexpr int kPaneListBaseId = 3200;
constexpr int kPaneHiddenBaseId = 3300;
constexpr int kPaneTabBaseId = detail::kTransferPaneTabBaseId;
constexpr int kPaneTabCloseBaseId = detail::kTransferPaneTabCloseBaseId;

constexpr UINT_PTR kQueueTimerId = 1;
constexpr UINT_PTR kLayoutTimerId = 2;
constexpr UINT_PTR kTabMotionTimerId = 3;
constexpr UINT kLayoutIntervalMs = 16;
constexpr ULONGLONG kTabMotionDurationMs = 120;
constexpr UINT kDirectoryResultMessage = WM_APP + 120;
constexpr UINT kClosePaneMessage = WM_APP + 121;
constexpr UINT kRefreshPaneMessage = WM_APP + 122;
constexpr UINT kMutationResultMessage = WM_APP + 123;
constexpr UINT kCommitInlineEditMessage = WM_APP + 124;
constexpr UINT kContextRefresh = 4201;
constexpr UINT kContextCopy = 4202;
constexpr UINT kContextPaste = 4203;
constexpr UINT kContextRemoteEdit = 4204;
constexpr UINT kContextCut = 4205;
constexpr UINT kContextDelete = 4206;
constexpr UINT kContextRename = 4207;
constexpr UINT kContextPermissions = 4208;
constexpr UINT kContextCreateFile = 4209;
constexpr UINT kContextCreateDirectory = 4210;
constexpr UINT kContextTransferBase = 4300;
constexpr UINT kContextMoveBase = 4400;
constexpr UINT kQueueContextPause = 4601;
constexpr UINT kQueueContextResume = 4602;
constexpr UINT kQueueContextCancel = 4603;
constexpr UINT kQueueContextRetry = 4604;
constexpr UINT kQueueContextClear = 4605;

bool ClientAnimationsEnabled() noexcept {
    BOOL enabled = TRUE;
    return SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0) == FALSE ||
           enabled != FALSE;
}

enum class SplitDropZone {
    none,
    left,
    right,
    center,
};

LRESULT CALLBACK ResizeGuideWindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_ERASEBKGND) {
        return 1;
    }
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        const HDC dc = BeginPaint(window, &paint);
        RECT rect{};
        GetClientRect(window, &rect);
        const HBRUSH transparent = CreateSolidBrush(kResizeGuideTransparentColor);
        FillRect(dc, &rect, transparent);
        DeleteObject(transparent);
        const HBRUSH dash = CreateSolidBrush(detail::kMutedTextColor);
        const bool vertical = (rect.right - rect.left) <= (rect.bottom - rect.top);
        const int dash_size = std::max(2, detail::ScaleForDpi(3, detail::WindowDpi(window)));
        const int step = dash_size + std::max(2, detail::ScaleForDpi(3, detail::WindowDpi(window)));
        if (vertical) {
            for (int y = rect.top; y < rect.bottom; y += step) {
                RECT mark{rect.left, y, rect.right, std::min<LONG>(y + dash_size, rect.bottom)};
                FillRect(dc, &mark, dash);
            }
        } else {
            for (int x = rect.left; x < rect.right; x += step) {
                RECT mark{x, rect.top, std::min<LONG>(x + dash_size, rect.right), rect.bottom};
                FillRect(dc, &mark, dash);
            }
        }
        DeleteObject(dash);
        EndPaint(window, &paint);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

bool RegisterResizeGuideClass(HINSTANCE instance) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = &ResizeGuideWindowProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_SIZEWE);
    wc.lpszClassName = kResizeGuideClass;
    const ATOM atom = RegisterClassExW(&wc);
    return atom != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

using detail::FromUtf8;
using detail::PermissionsText;

std::string ToUtf8(const std::wstring_view input) {
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
        std::string fallback;
        fallback.reserve(input.size());
        for (const wchar_t value : input) {
            fallback.push_back(value >= 0 && value <= 0x7f ? static_cast<char>(value) : '?');
        }
        return fallback;
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

std::wstring ExceptionText(const std::exception& error) {
    const char* text = error.what();
    return text ? FromUtf8(text) : L"未知错误";
}

void ApplyPermissions(transfer::IFileEndpoint& endpoint,
                      const transfer::FileUri& root,
                      const std::uint32_t permissions,
                      const bool recursive) {
    if (!recursive) {
        endpoint.set_permissions(root, permissions);
        return;
    }

    std::vector<transfer::FileUri> pending{root};
    std::vector<transfer::FileUri> ordered;
    std::unordered_set<std::string> visited;
    while (!pending.empty()) {
        transfer::FileUri current = std::move(pending.back());
        pending.pop_back();
        if (!visited.insert(current.to_string()).second) {
            continue;
        }
        const transfer::FileStat stat = endpoint.stat(current);
        ordered.push_back(current);
        if (stat.kind != transfer::FileKind::Directory) {
            continue;
        }
        for (auto& entry : endpoint.list(current)) {
            if (entry.name == "." || entry.name == "..") {
                continue;
            }
            pending.push_back(std::move(entry.uri));
        }
    }

    // Apply directory permissions after their children. Removing execute/read
    // bits from a directory must not prevent the remaining traversal.
    for (auto item = ordered.rbegin(); item != ordered.rend(); ++item) {
        endpoint.set_permissions(*item, permissions);
    }
}

std::wstring PathText(const transfer::FileUri& uri) {
    if (uri.is_local() && uri.path() == "/") {
        return L"此电脑";
    }
    return FromUtf8(uri.path());
}

transfer::FileUri DesktopLocation() {
    PWSTR desktop = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, KF_FLAG_DEFAULT, nullptr, &desktop)) &&
        desktop && *desktop) {
        const std::wstring path(desktop);
        CoTaskMemFree(desktop);
        return transfer::FileUri::local(ToUtf8(path));
    }
    if (desktop) {
        CoTaskMemFree(desktop);
    }
    return transfer::FileUri::local("/");
}

std::wstring TransferStateText(const transfer::TransferState state) {
    switch (state) {
    case transfer::TransferState::Queued:
        return L"等待";
    case transfer::TransferState::Running:
        return L"传输中";
    case transfer::TransferState::Paused:
        return L"已暂停";
    case transfer::TransferState::Completed:
        return L"已完成";
    case transfer::TransferState::Failed:
        return L"失败";
    case transfer::TransferState::Cancelled:
        return L"已取消";
    }
    return L"未知";
}

std::wstring PercentageText(const std::uint64_t transferred, const std::uint64_t total) {
    if (total == 0)
        return L"0%";
    return std::to_wstring(std::min<std::uint64_t>(100, transferred * 100 / total)) + L"%";
}

std::wstring TransferredSizeText(const std::uint64_t transferred, const std::uint64_t total) {
    if (total == 0) {
        return transferred == 0 ? L"—" : detail::FormatFileSize(transferred);
    }
    return detail::FormatFileSize(transferred) + L" / " + detail::FormatFileSize(total);
}

std::string SafeServerId(const std::wstring_view display_name) {
    std::string result = ToUtf8(display_name);
    if (result.empty()) {
        result = "remote";
    }
    std::replace_if(
        result.begin(),
        result.end(),
        [](const char value) {
            return value == '/' || value == '\\' || value == '?' || value == '#';
        },
        '_');
    return result;
}

ATOM RegisterPageClass(HINSTANCE instance, WNDPROC proc) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(detail::kWindowColor);
    wc.lpszClassName = kTransferPageClass;
    const ATOM atom = RegisterClassExW(&wc);
    return atom ? atom : static_cast<ATOM>(GetLastError() == ERROR_CLASS_ALREADY_EXISTS);
}

LRESULT CALLBACK PaneWindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_COMMAND:
    case WM_NOTIFY:
    case WM_DRAWITEM:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        return SendMessageW(GetParent(window), message, wparam, lparam);
    case WM_ERASEBKGND:
        detail::PaintDarkControl(window, reinterpret_cast<HDC>(wparam));
        return 1;
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

bool RegisterPaneClass(HINSTANCE instance) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &PaneWindowProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(detail::kPanelColor);
    wc.lpszClassName = kTransferPaneClass;
    const ATOM atom = RegisterClassExW(&wc);
    return atom != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

LRESULT CALLBACK PathFrameWindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_COMMAND:
    case WM_NOTIFY:
        return SendMessageW(GetParent(window), message, wparam, lparam);
    case WM_CTLCOLOREDIT: {
        const HDC dc = reinterpret_cast<HDC>(wparam);
        SetTextColor(dc, detail::kTextColor);
        SetBkColor(dc, detail::kPanelAltColor);
        return reinterpret_cast<LRESULT>(detail::PanelAltThemeBrush());
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        const HDC dc = BeginPaint(window, &paint);
        RECT rect{};
        GetClientRect(window, &rect);
        const UINT dpi = detail::WindowDpi(window);
        const int diameter = detail::ScaleForDpi(7, dpi);
        const HBRUSH fill = CreateSolidBrush(detail::kPanelAltColor);
        const HPEN border =
            CreatePen(PS_SOLID, std::max(1, detail::ScaleForDpi(1, dpi)), detail::kBorderColor);
        const HGDIOBJ old_brush = SelectObject(dc, fill);
        const HGDIOBJ old_pen = SelectObject(dc, border);
        RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, diameter, diameter);
        SelectObject(dc, old_pen);
        SelectObject(dc, old_brush);
        DeleteObject(border);
        DeleteObject(fill);
        EndPaint(window, &paint);
        return 0;
    }
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

bool RegisterPathFrameClass(HINSTANCE instance) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &PathFrameWindowProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_IBEAM);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kTransferPathFrameClass;
    const ATOM atom = RegisterClassExW(&wc);
    return atom != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

} // namespace

// 页面核心状态集中在一个 UI 线程对象中。Pane::load_generation 用于丢弃路径切换前
// 返回的过期异步枚举结果；AsyncLifetime 让析构等待已投递任务越过安全边界，避免
// 后台线程向已销毁 HWND 发送消息。目录行只保存快照，端点操作从不在绘制回调中执行。
struct TransferPage::Impl {
    enum class ResizeMode {
        none,
        pane,
        queue,
    };

    enum class InlineEditMode {
        none,
        rename,
        create_file,
        create_directory,
    };

    using FileRow = detail::TransferFileRow;

    struct Pane {
        TransferPaneId id{};
        bool local{};
        bool showing_this_pc{};
        bool loading{};
        ViewState view_state;
        std::uint64_t load_generation{};
        std::wstring title;
        transfer::FileUri location;
        std::shared_ptr<transfer::IFileEndpoint> endpoint;
        HWND container{};
        HWND title_control{};
        HWND up_button{};
        HWND path_frame{};
        HWND path_control{};
        HWND hidden_checkbox{};
        HWND refresh_button{};
        HWND close_button{};
        HWND list{};
        std::vector<FileRow> rows;
        detail::TransferPaneHistory history;
        bool show_hidden{};
        std::size_t sort_column{};
        bool sort_ascending{true};
        std::array<int, 6> column_widths{{-1, -1, -1, -1, -1, -1}};
        SIZE path_frame_region{};
        SIZE path_edit_region{};
    };

    struct TabInsertionTarget {
        std::size_t slot{};
        float x{};
        float top{};
        float bottom{};
    };

    struct AsyncLifetime {
        std::mutex mutex;
        std::condition_variable idle;
        std::size_t active_jobs{};
        bool alive{true};
        HWND window{};
    };

    struct DirectoryResult {
        TransferPaneId pane_id{};
        std::uint64_t generation{};
        transfer::FileUri location;
        std::vector<FileRow> rows;
        std::wstring error;
    };

    struct MutationResult {
        TransferPaneId pane_id{};
        std::wstring success;
        std::wstring error;
    };

    struct QueueDetailCache {
        // TransferManager 提供增量 revision；页面合并变化而不是每 100ms 重建大目录树。
        std::uint64_t revision{};
        std::vector<transfer::TransferItemProgress> items;
        std::unordered_map<transfer::TransferItemId, std::size_t> indexes;
    };

    struct QueueDetailUpdate {
        bool topology_changed{};
        std::vector<transfer::TransferItemId> changed_items;
    };

    struct QueueSpeedSample {
        std::uint64_t bytes{};
        double bytes_per_second{};
        std::chrono::steady_clock::time_point sampled_at{};
    };

    using QueueRow = detail::TransferQueueRow;

    HINSTANCE instance{};
    HWND hwnd{};
    detail::UiSurface surface;
    HWND queue_list{};
    HWND concurrency_edit{};
    HWND inline_name_edit{};
    HWND resize_guide{};
    HFONT font{};
    HFONT file_name_font{};
    HFONT title_font{};
    HFONT path_font{};
    HIMAGELIST file_icons{};
    int folder_icon{};
    int file_icon{};
    std::vector<Pane> panes;
    std::vector<transfer::TransferJob> queue_jobs;
    std::vector<QueueRow> queue_rows;
    std::unordered_map<transfer::TransferId, std::size_t> queue_job_rows;
    std::unordered_map<transfer::TransferId,
                       std::unordered_map<transfer::TransferItemId, std::size_t>>
        queue_visible_item_rows;
    std::unordered_map<transfer::TransferId, std::unordered_set<transfer::TransferItemId>>
        queue_running_visible_items;
    bool queue_layout_dirty{true};
    std::unordered_map<transfer::TransferId, QueueDetailCache> queue_details;
    std::unordered_set<transfer::TransferId> expanded_queue_jobs;
    std::unordered_set<std::string> expanded_queue_items;
    std::unordered_set<transfer::TransferId> observed_terminal_jobs;
    std::unordered_map<transfer::TransferId, QueueSpeedSample> queue_job_speeds;
    std::unordered_map<transfer::TransferId,
                       std::unordered_map<transfer::TransferItemId, QueueSpeedSample>>
        queue_item_speeds;
    std::vector<transfer::FileUri> clipboard_uris;
    DWORD clipboard_sequence{};
    bool clipboard_move{};
    std::vector<transfer::FileUri> drag_uris;
    std::vector<transfer::FileUri> drag_directory_uris;
    std::optional<transfer::FileUri> drag_target_base;
    std::optional<TransferPaneId> drag_target_pane_id;
    int drag_target_row{-1};
    bool drag_drop_allowed{};
    std::vector<TransferServerOption> server_options;
    TransferPaneId next_id{2};
    TransferPaneId active_pane_id{1};
    bool internal_drag{};
    ResizeMode resize_mode{ResizeMode::none};
    std::size_t resize_index{};
    POINT resize_origin{};
    int resize_queue_start{};
    int queue_height_override{};
    int queue_splitter_y{};
    int resize_preview_position{-1};
    RECT concurrency_frame{};
    SIZE concurrency_edit_region{};
    std::vector<float> pane_weights;
    std::vector<float> resize_start_weights;
    std::vector<RECT> pane_rects;
    std::vector<unsigned> pane_groups;
    std::array<std::size_t, 2> group_active{0, 0};
    std::size_t active_pane_index{};
    TransferPaneId animated_tab_id{};
    bool tab_motion_enabled{ClientAnimationsEnabled()};
    bool tab_motion_active{};
    ULONGLONG tab_motion_start{};
    float tab_motion_progress{1.0F};
    bool split_active{};
    bool tab_drag_armed{};
    bool tab_dragging{};
    int dragged_tab{-1};
    POINT tab_drag_origin{};
    SplitDropZone visible_drop_zone{SplitDropZone::none};
    std::optional<std::size_t> visible_drop_pane;
    std::optional<TabInsertionTarget> visible_tab_insertion;
    SIZE pending_layout_size{};
    bool layout_timer_pending{};
    int concurrency_value{3};
    bool updating_concurrency_edit{};
    DWORD last_history_input_tick{};
    bool last_history_input_forward{};
    bool last_history_input_was_app_command{};
    bool closing_inline_edit{};
    InlineEditMode inline_edit_mode{InlineEditMode::none};
    TransferPaneId inline_edit_pane_id{};
    std::size_t inline_edit_row{};
    transfer::FileUri inline_edit_source;
    std::wstring inline_edit_original;
    std::shared_ptr<transfer::TransferManager> transfer_manager;
    AsyncDirectoryList async_directory_list;
    RemoteEditCallback remote_edit_callback;
    PaneClosedCallback pane_closed_callback;
    PaneNavigatedCallback pane_navigated_callback;
    OpenRemoteSessionCallback open_remote_session_callback;
    StatusCallback status_callback;
    PaneActionCallback pane_action_callback;
    std::shared_ptr<AsyncLifetime> async_lifetime{std::make_shared<AsyncLifetime>()};
    detail::BackgroundExecutor background_executor;

    static LRESULT CALLBACK InlineNameEditSubclass(HWND control,
                                                   UINT message,
                                                   WPARAM wparam,
                                                   LPARAM lparam,
                                                   UINT_PTR subclass_id,
                                                   DWORD_PTR reference) {
        auto* self = reinterpret_cast<Impl*>(reference);
        if (!self) {
            return DefSubclassProc(control, message, wparam, lparam);
        }
        if (message == WM_GETDLGCODE) {
            return DefSubclassProc(control, message, wparam, lparam) | DLGC_WANTALLKEYS;
        }
        if (message == WM_KEYDOWN) {
            if (wparam == VK_RETURN) {
                self->CommitInlineEdit();
                return 0;
            }
            if (wparam == VK_ESCAPE) {
                self->CancelInlineEdit();
                return 0;
            }
        }
        if (message == WM_KILLFOCUS && !self->closing_inline_edit) {
            // 默认窗口过程仍在处理焦点切换时不能销毁 EDIT。旧的同步提交不仅会
            // 形成重入，还可能让底层表格在用户选择文字前抢走焦点。
            PostMessageW(
                self->hwnd, kCommitInlineEditMessage, reinterpret_cast<WPARAM>(control), 0);
        }
        if (message == WM_NCDESTROY) {
            RemoveWindowSubclass(control, &InlineNameEditSubclass, subclass_id);
        }
        return DefSubclassProc(control, message, wparam, lparam);
    }

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            self = static_cast<Impl*>(create->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->hwnd = window;
            {
                std::scoped_lock lock(self->async_lifetime->mutex);
                self->async_lifetime->alive = true;
                self->async_lifetime->window = window;
            }
        }
        if (!self) {
            return DefWindowProcW(window, message, wparam, lparam);
        }

        switch (message) {
        case WM_SIZE:
            self->CancelInlineEdit();
            self->ScheduleLayout(LOWORD(lparam), HIWORD(lparam));
            return 0;
        case WM_DPICHANGED_AFTERPARENT: {
            self->CancelInlineEdit();
            self->surface.DpiChanged();
            self->CommitLayoutNow();
            return 0;
        }
        case WM_TIMER:
            if (wparam == kQueueTimerId) {
                self->RefreshQueue();
                return 0;
            }
            if (wparam == kLayoutTimerId) {
                self->CommitScheduledLayout();
                return 0;
            }
            if (wparam == kTabMotionTimerId) {
                self->TickTabMotion();
                return 0;
            }
            break;
        case WM_SETTINGCHANGE:
            self->tab_motion_enabled = ClientAnimationsEnabled();
            if (!self->tab_motion_enabled) {
                self->tab_motion_active = false;
                self->tab_motion_progress = 1.0F;
                KillTimer(window, kTabMotionTimerId);
                self->UpdateTabAccent();
                self->InvalidateTabStrip();
            }
            break;
        case WM_COMMAND:
            if (self->OnCommand(LOWORD(wparam), HIWORD(wparam), reinterpret_cast<HWND>(lparam))) {
                return 0;
            }
            break;
        case WM_NOTIFY:
            return self->OnNotify(*reinterpret_cast<const NMHDR*>(lparam), lparam);
        case WM_MOUSEMOVE:
            if (self->resize_mode != ResizeMode::none) {
                self->UpdateResize({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
                return 0;
            }
            if (self->internal_drag) {
                POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                ClientToScreen(window, &point);
                self->UpdateInternalDragTarget(point);
                return 0;
            }
            self->HandlePointerMove({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)}, wparam);
            return 0;
        case WM_MOUSELEAVE:
            self->surface.PointerLeave();
            return 0;
        case WM_LBUTTONDOWN:
            if (self->BeginResize({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)})) {
                return 0;
            }
            if (!self->internal_drag &&
                self->HandlePointerDown({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)})) {
                return 0;
            }
            break;
        case WM_LBUTTONUP:
            if (self->resize_mode != ResizeMode::none) {
                self->EndResize();
                return 0;
            }
            if (self->internal_drag) {
                POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                ClientToScreen(window, &point);
                self->CompleteInternalDrag(point);
                return 0;
            }
            self->HandlePointerUp({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
            return 0;
        case WM_KEYDOWN:
            if (wparam == VK_ESCAPE && self->internal_drag) {
                self->CancelInternalDrag();
                return 0;
            }
            break;
        case WM_CAPTURECHANGED:
            if (self->resize_mode != ResizeMode::none) {
                ShowWindow(self->resize_guide, SW_HIDE);
                if (self->resize_mode == ResizeMode::pane &&
                    self->resize_start_weights.size() == 2) {
                    self->pane_weights = self->resize_start_weights;
                } else if (self->resize_mode == ResizeMode::queue) {
                    self->queue_height_override = self->resize_queue_start;
                }
            }
            self->resize_preview_position = -1;
            self->resize_mode = ResizeMode::none;
            self->resize_start_weights.clear();
            if (self->internal_drag && reinterpret_cast<HWND>(lparam) != window) {
                self->CancelInternalDrag();
            }
            self->CancelTabDrag();
            self->surface.CancelPointer();
            return 0;
        case WM_SETCURSOR:
            if (self->internal_drag) {
                SetCursor(LoadCursorW(nullptr, self->drag_drop_allowed ? IDC_ARROW : IDC_NO));
                return TRUE;
            }
            if (self->SetResizeCursor()) {
                return TRUE;
            }
            if (self->tab_dragging) {
                SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
                return TRUE;
            }
            if (self->surface.hovered_id() != 0) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            break;
        case kDirectoryResultMessage:
            self->ApplyDirectoryResult(
                std::unique_ptr<DirectoryResult>(reinterpret_cast<DirectoryResult*>(lparam)));
            return 0;
        case kMutationResultMessage:
            self->ApplyMutationResult(
                std::unique_ptr<MutationResult>(reinterpret_cast<MutationResult*>(lparam)));
            return 0;
        case kCommitInlineEditMessage:
            // 同时核对投递时的 HWND，避免旧编辑框的延迟失焦消息误关新编辑框。
            if (self->inline_name_edit == reinterpret_cast<HWND>(wparam) &&
                GetFocus() != self->inline_name_edit) {
                self->CommitInlineEdit();
            }
            return 0;
        case kClosePaneMessage:
            self->RemovePane(static_cast<TransferPaneId>(wparam));
            return 0;
        case kRefreshPaneMessage:
            self->RefreshPaneNow(static_cast<TransferPaneId>(wparam));
            return 0;
        case WM_CTLCOLORSTATIC: {
            const HDC dc = reinterpret_cast<HDC>(wparam);
            SetTextColor(dc, detail::kTextColor);
            SetBkColor(dc, detail::kPanelColor);
            return reinterpret_cast<LRESULT>(detail::PanelThemeBrush());
        }
        case WM_DRAWITEM: {
            const auto* draw = reinterpret_cast<const DRAWITEMSTRUCT*>(lparam);
            if (draw && draw->CtlID == kResizeGuideId && draw->hDC) {
                const HBRUSH background = CreateSolidBrush(kResizeGuideTransparentColor);
                FillRect(draw->hDC, &draw->rcItem, background);
                DeleteObject(background);
                const HBRUSH dash = CreateSolidBrush(detail::kMutedTextColor);
                const bool vertical = (draw->rcItem.right - draw->rcItem.left) <=
                                      (draw->rcItem.bottom - draw->rcItem.top);
                const int dash_size =
                    std::max(2, detail::ScaleForDpi(3, detail::WindowDpi(window)));
                const int step =
                    dash_size + std::max(2, detail::ScaleForDpi(3, detail::WindowDpi(window)));
                if (vertical) {
                    for (int y = draw->rcItem.top; y < draw->rcItem.bottom; y += step) {
                        RECT mark{draw->rcItem.left,
                                  y,
                                  draw->rcItem.right,
                                  std::min<LONG>(y + dash_size, draw->rcItem.bottom)};
                        FillRect(draw->hDC, &mark, dash);
                    }
                } else {
                    for (int x = draw->rcItem.left; x < draw->rcItem.right; x += step) {
                        RECT mark{x,
                                  draw->rcItem.top,
                                  std::min<LONG>(x + dash_size, draw->rcItem.right),
                                  draw->rcItem.bottom};
                        FillRect(draw->hDC, &mark, dash);
                    }
                }
                DeleteObject(dash);
                return TRUE;
            }
            const bool hidden_checkbox =
                draw && std::ranges::any_of(self->panes, [draw](const Pane& pane) {
                    return pane.hidden_checkbox == draw->hwndItem;
                });
            if (draw && detail::DrawSelfDrawnButton(*draw, false, hidden_checkbox)) {
                return TRUE;
            }
            break;
        }
        case WM_CTLCOLOREDIT: {
            const HDC dc = reinterpret_cast<HDC>(wparam);
            SetTextColor(dc, detail::kTextColor);
            const HWND control = reinterpret_cast<HWND>(lparam);
            const COLORREF background =
                control == self->concurrency_edit ? detail::kPanelAltColor : detail::kPanelColor;
            SetBkColor(dc, background);
            return reinterpret_cast<LRESULT>(control == self->concurrency_edit
                                                 ? detail::PanelAltThemeBrush()
                                                 : detail::PanelThemeBrush());
        }
        case WM_CTLCOLORLISTBOX: {
            const HDC dc = reinterpret_cast<HDC>(wparam);
            SetTextColor(dc, detail::kTextColor);
            SetBkColor(dc, detail::kPanelAltColor);
            return reinterpret_cast<LRESULT>(detail::PanelAltThemeBrush());
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            self->surface.Paint(detail::UiColor(detail::kWindowColor));
            return 0;
        case WM_NCDESTROY:
            self->OnWindowDestroyed(window);
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            self->hwnd = nullptr;
            return DefWindowProcW(window, message, wparam, lparam);
        default:
            break;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    static LRESULT CALLBACK ControlSubclassProc(HWND control,
                                                UINT message,
                                                WPARAM wparam,
                                                LPARAM lparam,
                                                UINT_PTR subclass_id,
                                                DWORD_PTR reference_data) {
        auto* self = reinterpret_cast<Impl*>(reference_data);
        if (!self) {
            return DefSubclassProc(control, message, wparam, lparam);
        }

        if (message == WM_XBUTTONDOWN && (subclass_id == 1 || subclass_id == 2)) {
            Pane* pane =
                subclass_id == 1 ? self->FindPaneByList(control) : self->FindPaneByPath(control);
            if (pane) {
                const WORD button = GET_XBUTTON_WPARAM(wparam);
                if (button == XBUTTON1 || button == XBUTTON2) {
                    const bool forward = button == XBUTTON2;
                    if (self->AcceptHistoryInput(forward, false)) {
                        self->NavigateHistory(*pane, forward);
                    }
                    return TRUE;
                }
            }
        }
        if (message == WM_APPCOMMAND && (subclass_id == 1 || subclass_id == 2)) {
            const int command = GET_APPCOMMAND_LPARAM(lparam);
            if (command == APPCOMMAND_BROWSER_BACKWARD || command == APPCOMMAND_BROWSER_FORWARD) {
                Pane* pane = subclass_id == 1 ? self->FindPaneByList(control)
                                              : self->FindPaneByPath(control);
                if (pane) {
                    const bool forward = command == APPCOMMAND_BROWSER_FORWARD;
                    if (self->AcceptHistoryInput(forward, true)) {
                        self->NavigateHistory(*pane, forward);
                    }
                    return TRUE;
                }
            }
        }

        if (subclass_id == 1) {
            if (message == WM_SETFOCUS) {
                if (Pane* pane = self->FindPaneByList(control)) {
                    self->active_pane_id = pane->id;
                    const auto iterator = std::find_if(
                        self->panes.begin(), self->panes.end(), [pane](const Pane& candidate) {
                            return candidate.id == pane->id;
                        });
                    if (iterator != self->panes.end()) {
                        self->active_pane_index =
                            static_cast<std::size_t>(iterator - self->panes.begin());
                        self->group_active[self->GroupOf(self->active_pane_index)] =
                            self->active_pane_index;
                    }
                }
            } else if (message == WM_KEYDOWN) {
                if (Pane* pane = self->FindPaneByList(control)) {
                    const bool control_down = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                    if (control_down && (wparam == 'C' || wparam == 'c')) {
                        self->CopySelection(*pane);
                        return 0;
                    }
                    if (control_down && (wparam == 'X' || wparam == 'x')) {
                        self->CutSelection(*pane);
                        return 0;
                    }
                    if (control_down && (wparam == 'V' || wparam == 'v')) {
                        self->PasteInto(*pane);
                        return 0;
                    }
                    if (wparam == VK_F4) {
                        self->OpenSelectedRemoteFile(*pane);
                        return 0;
                    }
                    if (wparam == VK_F2) {
                        self->RenameSelected(*pane);
                        return 0;
                    }
                    if (wparam == VK_F5) {
                        self->RequestList(*pane, pane->location);
                        return 0;
                    }
                    if (wparam == VK_ESCAPE && self->internal_drag) {
                        self->CancelInternalDrag();
                        return 0;
                    }
                    if (wparam == VK_BACK) {
                        self->NavigateParent(*pane);
                        return 0;
                    }
                    if (wparam == VK_RETURN) {
                        self->OpenSelectedDirectory(*pane);
                        return 0;
                    }
                }
            } else if (message == WM_DROPFILES) {
                if (Pane* pane = self->FindPaneByList(control)) {
                    self->AcceptDroppedFiles(*pane, reinterpret_cast<HDROP>(wparam));
                    return 0;
                }
            } else if (message == WM_NCDESTROY) {
                DragAcceptFiles(control, FALSE);
                RemoveWindowSubclass(control, &ControlSubclassProc, 1);
            }
        } else if (subclass_id == 2) {
            if (message == WM_GETDLGCODE) {
                return DefSubclassProc(control, message, wparam, lparam) | DLGC_WANTALLKEYS;
            }
            if (message == WM_KEYDOWN && wparam == VK_RETURN) {
                if (Pane* pane = self->FindPaneByPath(control)) {
                    self->NavigateFromPathControl(*pane);
                    return 0;
                }
            } else if (message == WM_NCDESTROY) {
                RemoveWindowSubclass(control, &ControlSubclassProc, 2);
            }
        } else if (subclass_id == 3) {
            if (message == WM_NCDESTROY) {
                RemoveWindowSubclass(control, &ControlSubclassProc, 3);
                return DefSubclassProc(control, message, wparam, lparam);
            }
            if (message == WM_LBUTTONDOWN || message == WM_LBUTTONUP) {
                TVHITTESTINFO hit{};
                hit.pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                TreeView_HitTest(control, &hit);
                constexpr UINT item_flags =
                    TVHT_ONITEMICON | TVHT_ONITEMLABEL | TVHT_ONITEMSTATEICON | TVHT_ONITEMRIGHT;
                if (hit.hItem && (hit.flags & TVHT_ONITEMRIGHT) != 0) {
                    TreeView_SelectItem(control, hit.hItem);
                }
                const bool blank = !hit.hItem || (hit.flags & item_flags) == 0;
                const LRESULT result = DefSubclassProc(control, message, wparam, lparam);
                if (blank) {
                    TreeView_SelectItem(control, nullptr);
                }
                return result;
            }
        } else if (subclass_id == 4) {
            if (message == WM_LBUTTONDOWN) {
                SetCapture(control);
                return 0;
            }
            if (message == WM_LBUTTONUP) {
                if (GetCapture() == control) {
                    ReleaseCapture();
                }
                POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                RECT client{};
                GetClientRect(control, &client);
                if (PtInRect(&client, point)) {
                    for (const Pane& pane : self->panes) {
                        if (pane.refresh_button == control) {
                            PostMessageW(
                                self->hwnd, kRefreshPaneMessage, static_cast<WPARAM>(pane.id), 0);
                            break;
                        }
                    }
                }
                return 0;
            }
            if (message == WM_SETCURSOR) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            if (message == WM_NCDESTROY) {
                if (GetCapture() == control) {
                    ReleaseCapture();
                }
                RemoveWindowSubclass(control, &ControlSubclassProc, 4);
            }
        } else if (subclass_id == 5) {
            if (message == WM_GETDLGCODE) {
                return DefSubclassProc(control, message, wparam, lparam) | DLGC_WANTALLKEYS;
            }
            if (message == WM_KEYDOWN && wparam == VK_RETURN) {
                self->CommitConcurrencyEdit(true);
                SetFocus(self->queue_list);
                return 0;
            }
            if (message == WM_NCDESTROY) {
                RemoveWindowSubclass(control, &ControlSubclassProc, 5);
            }
        }
        return DefSubclassProc(control, message, wparam, lparam);
    }

    bool AcceptHistoryInput(const bool forward, const bool app_command) noexcept {
        constexpr DWORD kDuplicateMessageWindowMs = 250;
        const DWORD now = GetTickCount();
        const bool duplicate = last_history_input_tick != 0 &&
                               forward == last_history_input_forward &&
                               app_command != last_history_input_was_app_command &&
                               now - last_history_input_tick <= kDuplicateMessageWindowMs;
        if (duplicate) {
            return false;
        }
        last_history_input_tick = now;
        last_history_input_forward = forward;
        last_history_input_was_app_command = app_command;
        return true;
    }

    void OnWindowDestroyed(HWND window) {
        KillTimer(window, kQueueTimerId);
        KillTimer(window, kLayoutTimerId);
        KillTimer(window, kTabMotionTimerId);
        layout_timer_pending = false;
        {
            std::unique_lock lock(async_lifetime->mutex);
            async_lifetime->alive = false;
            async_lifetime->window = nullptr;
            async_lifetime->idle.wait(lock, [this] {
                return async_lifetime->active_jobs == 0;
            });
        }
        MSG pending{};
        while (PeekMessageW(
            &pending, window, kDirectoryResultMessage, kDirectoryResultMessage, PM_REMOVE)) {
            delete reinterpret_cast<DirectoryResult*>(pending.lParam);
        }
        while (PeekMessageW(
            &pending, window, kMutationResultMessage, kMutationResultMessage, PM_REMOVE)) {
            delete reinterpret_cast<MutationResult*>(pending.lParam);
        }
    }

    Pane* FindPane(const TransferPaneId id) {
        const auto it = std::find_if(panes.begin(), panes.end(), [id](const Pane& pane) {
            return pane.id == id;
        });
        return it == panes.end() ? nullptr : &*it;
    }

    const Pane* FindPane(const TransferPaneId id) const {
        const auto it = std::find_if(panes.begin(), panes.end(), [id](const Pane& pane) {
            return pane.id == id;
        });
        return it == panes.end() ? nullptr : &*it;
    }

    unsigned GroupOf(const std::size_t index) const noexcept {
        return split_active && index < pane_groups.size() ? std::min(pane_groups[index], 1U) : 0U;
    }

    std::vector<std::size_t> TabsInGroup(const unsigned group) const {
        std::vector<std::size_t> result;
        for (std::size_t index = 0; index < panes.size(); ++index) {
            if (GroupOf(index) == group) {
                result.push_back(index);
            }
        }
        return result;
    }

    std::optional<std::size_t> FirstTabInGroup(const unsigned group) const noexcept {
        for (std::size_t index = 0; index < panes.size(); ++index) {
            if (GroupOf(index) == group) {
                return index;
            }
        }
        return std::nullopt;
    }

    std::size_t ActiveTabInGroup(const unsigned group) const noexcept {
        const std::size_t candidate = group_active[std::min(group, 1U)];
        if (candidate < panes.size() && GroupOf(candidate) == group) {
            return candidate;
        }
        return FirstTabInGroup(group).value_or(0);
    }

    void NormalizeGroups() {
        pane_groups.resize(panes.size(), 0U);
        if (panes.empty()) {
            split_active = false;
            active_pane_index = 0;
            group_active = {0, 0};
            pane_weights.assign(1, 1.0F);
            return;
        }
        active_pane_index = std::min(active_pane_index, panes.size() - 1);
        if (!split_active || panes.size() < 2) {
            split_active = false;
            std::fill(pane_groups.begin(), pane_groups.end(), 0U);
            group_active[0] = active_pane_index;
            group_active[1] = 0;
            pane_weights.assign(1, 1.0F);
            active_pane_id = panes[active_pane_index].id;
            return;
        }
        for (unsigned& group : pane_groups) {
            group = std::min(group, 1U);
        }
        const auto left = FirstTabInGroup(0);
        const auto right = FirstTabInGroup(1);
        if (!left || !right) {
            split_active = false;
            std::fill(pane_groups.begin(), pane_groups.end(), 0U);
            group_active[0] = active_pane_index;
            group_active[1] = 0;
            pane_weights.assign(1, 1.0F);
            active_pane_id = panes[active_pane_index].id;
            return;
        }
        for (unsigned group = 0; group < 2; ++group) {
            if (group_active[group] >= panes.size() || GroupOf(group_active[group]) != group) {
                group_active[group] = group == 0 ? *left : *right;
            }
        }
        if (pane_weights.size() != 2) {
            pane_weights.assign(2, 1.0F);
        }
        active_pane_id = panes[active_pane_index].id;
    }

    void FocusPaneIndex(const std::size_t index) {
        if (index >= panes.size()) {
            return;
        }
        active_pane_index = index;
        active_pane_id = panes[index].id;
        group_active[GroupOf(index)] = index;
        Relayout();
        SetFocus(panes[index].list);
    }

    Pane* FindPaneByList(const HWND list) {
        const auto it = std::find_if(panes.begin(), panes.end(), [list](const Pane& pane) {
            return pane.list == list;
        });
        return it == panes.end() ? nullptr : &*it;
    }

    Pane* FindPaneByPath(const HWND path) {
        const auto it = std::find_if(panes.begin(), panes.end(), [path](const Pane& pane) {
            return pane.path_control == path;
        });
        return it == panes.end() ? nullptr : &*it;
    }

    Pane* FindPaneAtScreenPoint(const POINT point) {
        HWND candidate = WindowFromPoint(point);
        for (Pane& pane : panes) {
            if (candidate == pane.list || candidate == pane.container ||
                IsChild(pane.container, candidate)) {
                return &pane;
            }
        }
        return nullptr;
    }

    void Report(std::wstring message) const {
        if (status_callback) {
            status_callback(std::move(message));
        }
    }

    int ElementAt(const POINT point) const noexcept {
        for (auto iterator = surface.elements().rbegin(); iterator != surface.elements().rend();
             ++iterator) {
            const detail::UiElement& element = *iterator;
            if (element.id != 0 && element.visible && element.enabled &&
                point.x >= element.bounds.left && point.x < element.bounds.right &&
                point.y >= element.bounds.top && point.y < element.bounds.bottom) {
                return element.id;
            }
        }
        return 0;
    }

    std::optional<std::size_t> PaneAt(const POINT point) const noexcept {
        if (panes.empty() || pane_rects.size() != panes.size()) {
            return std::nullopt;
        }
        const unsigned groups = split_active ? 2U : 1U;
        for (unsigned group = 0; group < groups; ++group) {
            const std::size_t index = split_active ? ActiveTabInGroup(group)
                                                   : std::min(active_pane_index, panes.size() - 1);
            if (index < pane_rects.size() && PtInRect(&pane_rects[index], point)) {
                return index;
            }
        }
        return std::nullopt;
    }

    std::optional<std::size_t> TabStripAt(const POINT point) const noexcept {
        if (panes.empty() || pane_rects.size() != panes.size()) {
            return std::nullopt;
        }
        const int tab_height = detail::ScaleForDpi(34, detail::WindowDpi(hwnd));
        const unsigned groups = split_active ? 2U : 1U;
        for (unsigned group = 0; group < groups; ++group) {
            const std::size_t index = split_active ? ActiveTabInGroup(group)
                                                   : std::min(active_pane_index, panes.size() - 1);
            if (index >= pane_rects.size()) {
                continue;
            }
            RECT strip = pane_rects[index];
            strip.bottom = std::min(strip.bottom, strip.top + tab_height);
            if (PtInRect(&strip, point)) {
                return index;
            }
        }
        return std::nullopt;
    }

    static bool SameInsertion(const std::optional<TabInsertionTarget>& left,
                              const std::optional<TabInsertionTarget>& right) noexcept {
        if (left.has_value() != right.has_value()) {
            return false;
        }
        return !left || (left->slot == right->slot && left->x == right->x &&
                         left->top == right->top && left->bottom == right->bottom);
    }

    static std::size_t DestinationForSlot(const std::size_t source,
                                          const std::size_t slot) noexcept {
        return slot > source ? slot - 1 : slot;
    }

    std::optional<TabInsertionTarget> TabInsertionAt(const POINT point,
                                                     const std::size_t source) const {
        const auto strip = TabStripAt(point);
        if (!strip || source >= panes.size() || GroupOf(*strip) != GroupOf(source)) {
            return std::nullopt;
        }

        std::vector<const detail::UiElement*> tabs;
        for (const auto& element : surface.elements()) {
            if (element.role != detail::UiRole::file_tab || element.id < kPaneTabBaseId ||
                element.id >= kPaneTabBaseId + static_cast<int>(panes.size())) {
                continue;
            }
            const std::size_t index = static_cast<std::size_t>(element.id - kPaneTabBaseId);
            if (GroupOf(index) == GroupOf(source)) {
                tabs.push_back(&element);
            }
        }
        if (tabs.empty()) {
            return std::nullopt;
        }
        std::ranges::sort(tabs, {}, [](const detail::UiElement* element) {
            return element->bounds.left;
        });

        TabInsertionTarget target{};
        const detail::UiElement* edge = tabs.back();
        target.slot = static_cast<std::size_t>(edge->id - kPaneTabBaseId) + 1;
        target.x = edge->bounds.right;
        target.top = edge->bounds.top;
        target.bottom = edge->bounds.bottom;
        for (const detail::UiElement* tab : tabs) {
            const float midpoint = (tab->bounds.left + tab->bounds.right) / 2.0F;
            if (static_cast<float>(point.x) < midpoint) {
                target.slot = static_cast<std::size_t>(tab->id - kPaneTabBaseId);
                target.x = tab->bounds.left;
                target.top = tab->bounds.top;
                target.bottom = tab->bounds.bottom;
                break;
            }
        }
        if (DestinationForSlot(source, target.slot) == source) {
            return std::nullopt;
        }
        return target;
    }

    SplitDropZone DropZoneAt(const POINT point, const int source_tab = -1) const noexcept {
        const int effective_source = source_tab >= 0 ? source_tab : dragged_tab;
        if (const auto target = TabStripAt(point)) {
            if (split_active && effective_source >= 0 &&
                GroupOf(*target) != GroupOf(static_cast<std::size_t>(effective_source))) {
                return SplitDropZone::center;
            }
            return SplitDropZone::none;
        }
        const auto target = PaneAt(point);
        if (!target) {
            return SplitDropZone::none;
        }
        const RECT& rect = pane_rects[*target];
        return point.x - rect.left <= rect.right - point.x ? SplitDropZone::left
                                                           : SplitDropZone::right;
    }

    RECT DropZoneRect() const noexcept {
        RECT result = visible_drop_pane && *visible_drop_pane < pane_rects.size()
                          ? pane_rects[*visible_drop_pane]
                          : RECT{};
        switch (visible_drop_zone) {
        case SplitDropZone::left:
            result.right = result.left + (result.right - result.left) / 2;
            break;
        case SplitDropZone::right:
            result.left = result.left + (result.right - result.left) / 2;
            break;
        case SplitDropZone::center:
            result.bottom = std::min(result.bottom,
                                     result.top + detail::ScaleForDpi(40, detail::WindowDpi(hwnd)));
            break;
        case SplitDropZone::none:
            result = {};
            break;
        }
        return result;
    }

    void HandleTabDrop(const std::size_t index, const POINT point) {
        if (index >= panes.size() || panes.size() < 2) {
            return;
        }
        if (const auto insertion = TabInsertionAt(point, index)) {
            ReorderTab(index, insertion->slot);
            return;
        }
        const SplitDropZone zone = DropZoneAt(point, static_cast<int>(index));
        if (zone == SplitDropZone::none) {
            return;
        }
        if (zone == SplitDropZone::center) {
            const auto target = TabStripAt(point);
            if (split_active && target && GroupOf(*target) != GroupOf(index)) {
                pane_groups[index] = GroupOf(*target);
                active_pane_index = index;
                group_active[GroupOf(index)] = index;
                NormalizeGroups();
                FocusPaneIndex(index);
            }
            return;
        }
        if (!split_active) {
            pane_groups.assign(panes.size(), 0U);
            const unsigned dragged_group = zone == SplitDropZone::left ? 0U : 1U;
            const unsigned remaining_group = 1U - dragged_group;
            std::fill(pane_groups.begin(), pane_groups.end(), remaining_group);
            pane_groups[index] = dragged_group;
            split_active = true;
            pane_weights.assign(2, 1.0F);
        } else {
            const unsigned target_group = zone == SplitDropZone::left ? 0U : 1U;
            if (GroupOf(index) == target_group) {
                return;
            }
            pane_groups[index] = target_group;
        }
        active_pane_index = index;
        group_active[GroupOf(index)] = index;
        NormalizeGroups();
        FocusPaneIndex(index);
    }

    template <typename Value>
    static void
    MoveItem(std::vector<Value>& values, const std::size_t source, const std::size_t destination) {
        if (source == destination || source >= values.size() || destination >= values.size()) {
            return;
        }
        if (source < destination) {
            std::rotate(values.begin() + static_cast<std::ptrdiff_t>(source),
                        values.begin() + static_cast<std::ptrdiff_t>(source + 1),
                        values.begin() + static_cast<std::ptrdiff_t>(destination + 1));
        } else {
            std::rotate(values.begin() + static_cast<std::ptrdiff_t>(destination),
                        values.begin() + static_cast<std::ptrdiff_t>(source),
                        values.begin() + static_cast<std::ptrdiff_t>(source + 1));
        }
    }

    void ReorderTab(const std::size_t source, const std::size_t slot) {
        if (source >= panes.size() || slot > panes.size()) {
            return;
        }
        const std::size_t destination = DestinationForSlot(source, slot);
        if (destination == source || destination >= panes.size()) {
            return;
        }

        std::array<TransferPaneId, 2> selected_by_group{};
        for (unsigned group = 0; group < 2; ++group) {
            const std::size_t selected = ActiveTabInGroup(group);
            if (selected < panes.size() && GroupOf(selected) == group) {
                selected_by_group[group] = panes[selected].id;
            }
        }
        const TransferPaneId moved_id = panes[source].id;
        MoveItem(panes, source, destination);
        MoveItem(pane_groups, source, destination);

        const auto index_of = [&](const TransferPaneId id) {
            const auto found = std::ranges::find(panes, id, &Pane::id);
            return found == panes.end()
                       ? panes.size()
                       : static_cast<std::size_t>(std::distance(panes.begin(), found));
        };
        for (unsigned group = 0; group < 2; ++group) {
            group_active[group] = index_of(selected_by_group[group]);
        }
        active_pane_index = index_of(moved_id);
        if (active_pane_index >= panes.size()) {
            return;
        }
        active_pane_id = moved_id;
        group_active[GroupOf(active_pane_index)] = active_pane_index;
        NormalizeGroups();
        Relayout();
        SetFocus(panes[active_pane_index].list);
    }

    bool HandlePointerDown(const POINT point) {
        const int id = ElementAt(point);
        if (id >= kPaneTabBaseId && id < kPaneTabBaseId + static_cast<int>(panes.size())) {
            dragged_tab = id - kPaneTabBaseId;
            tab_drag_origin = point;
            tab_drag_armed = true;
            tab_dragging = false;
        }
        return surface.PointerDown(point);
    }

    void HandlePointerMove(const POINT point, const WPARAM key_state) {
        surface.PointerMove(point);
        if (!tab_drag_armed || (key_state & MK_LBUTTON) == 0) {
            return;
        }
        if (!tab_dragging &&
            (std::abs(point.x - tab_drag_origin.x) >= GetSystemMetrics(SM_CXDRAG) ||
             std::abs(point.y - tab_drag_origin.y) >= GetSystemMetrics(SM_CYDRAG))) {
            tab_dragging = true;
        }
        if (!tab_dragging) {
            return;
        }
        const auto next_insertion = TabInsertionAt(point, static_cast<std::size_t>(dragged_tab));
        const SplitDropZone next = next_insertion ? SplitDropZone::none : DropZoneAt(point);
        const auto next_pane = next_insertion ? std::nullopt : PaneAt(point);
        if (next != visible_drop_zone || next_pane != visible_drop_pane ||
            !SameInsertion(next_insertion, visible_tab_insertion)) {
            visible_drop_zone = next;
            visible_drop_pane = next_pane;
            visible_tab_insertion = next_insertion;
            Relayout();
        }
    }

    void CancelTabDrag() {
        tab_drag_armed = false;
        tab_dragging = false;
        dragged_tab = -1;
        visible_drop_zone = SplitDropZone::none;
        visible_drop_pane.reset();
        visible_tab_insertion.reset();
        surface.CancelPointer();
    }

    void HandlePointerUp(const POINT point) {
        const bool dragged = tab_dragging;
        const int tab = dragged_tab;
        const auto clicked = surface.PointerUp(point);
        if (dragged && tab >= 0) {
            HandleTabDrop(static_cast<std::size_t>(tab), point);
            CancelTabDrag();
            Relayout();
            return;
        }
        CancelTabDrag();
        if (clicked) {
            OnCommand(*clicked, BN_CLICKED, nullptr);
        }
    }

    std::pair<ResizeMode, std::size_t> ResizeHandleAt(POINT point) const noexcept {
        const int hit = detail::ScaleForDpi(5, detail::WindowDpi(hwnd));
        if (queue_splitter_y > 0 && point.x >= 0 && std::abs(point.y - queue_splitter_y) <= hit) {
            return {ResizeMode::queue, 0};
        }
        if (split_active) {
            const auto left = FirstTabInGroup(0);
            if (left && *left < pane_rects.size()) {
                const RECT& pane = pane_rects[*left];
                if (std::abs(point.x - pane.right) <= hit && point.y >= pane.top &&
                    point.y <= pane.bottom) {
                    return {ResizeMode::pane, 0};
                }
            }
        }
        return {ResizeMode::none, 0};
    }

    bool BeginResize(POINT point) {
        if (internal_drag) {
            return false;
        }
        const auto [mode, index] = ResizeHandleAt(point);
        if (mode == ResizeMode::none) {
            return false;
        }
        resize_mode = mode;
        resize_index = index;
        resize_origin = point;
        resize_queue_start = queue_height_override;
        resize_start_weights.clear();
        if (mode == ResizeMode::pane) {
            resize_start_weights.reserve(2);
            for (unsigned group = 0; group < 2; ++group) {
                const std::size_t selected = ActiveTabInGroup(group);
                if (selected < pane_rects.size()) {
                    const RECT& pane = pane_rects[selected];
                    resize_start_weights.push_back(static_cast<float>(pane.right - pane.left));
                }
            }
            const auto left = FirstTabInGroup(0);
            if (left && *left < pane_rects.size()) {
                resize_preview_position = pane_rects[*left].right;
            }
        } else if (mode == ResizeMode::queue) {
            resize_preview_position = queue_splitter_y;
        }
        SetCapture(hwnd);
        SetResizeCursor();
        ToggleResizeGuide();
        return true;
    }

    void ToggleResizeGuide() const {
        if (!resize_guide) {
            return;
        }
        if (resize_mode == ResizeMode::none || resize_preview_position < 0) {
            ShowWindow(resize_guide, SW_HIDE);
            return;
        }
        RECT client{};
        GetClientRect(hwnd, &client);
        const int thickness = std::max(1, detail::ScaleForDpi(2, detail::WindowDpi(hwnd)));
        int x{};
        int y{};
        int width{};
        int height{};
        if (resize_mode == ResizeMode::pane) {
            const auto left = FirstTabInGroup(0);
            if (!left || *left >= pane_rects.size()) {
                return;
            }
            x = resize_preview_position - thickness / 2;
            y = pane_rects[*left].top;
            width = thickness;
            height = std::max(1L, pane_rects[*left].bottom - pane_rects[*left].top);
        } else {
            x = 0;
            y = resize_preview_position - thickness / 2;
            width = std::max(1L, client.right);
            height = thickness;
        }
        POINT origin{x, y};
        ClientToScreen(hwnd, &origin);
        SetWindowPos(resize_guide,
                     HWND_TOPMOST,
                     origin.x,
                     origin.y,
                     width,
                     height,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        InvalidateRect(resize_guide, nullptr, FALSE);
        UpdateWindow(resize_guide);
    }

    void UpdateResize(POINT point) {
        RECT client{};
        GetClientRect(hwnd, &client);
        const int minimum = detail::ScaleForDpi(110, detail::WindowDpi(hwnd));
        if (resize_mode == ResizeMode::queue) {
            queue_height_override =
                std::clamp(resize_queue_start - static_cast<int>(point.y - resize_origin.y),
                           minimum,
                           std::max(minimum,
                                    static_cast<int>(client.bottom) -
                                        detail::ScaleForDpi(180, detail::WindowDpi(hwnd))));
            const int pad = detail::ScaleForDpi(8, detail::WindowDpi(hwnd));
            const int gap = detail::ScaleForDpi(6, detail::WindowDpi(hwnd));
            const int next_preview =
                static_cast<int>(client.bottom) - queue_height_override - pad - gap / 2;
            if (next_preview != resize_preview_position) {
                resize_preview_position = next_preview;
                ToggleResizeGuide();
            }
        } else if (resize_mode == ResizeMode::pane &&
                   resize_index + 1 < resize_start_weights.size()) {
            const float combined =
                resize_start_weights[resize_index] + resize_start_weights[resize_index + 1];
            const float first = std::clamp(
                resize_start_weights[resize_index] + static_cast<float>(point.x - resize_origin.x),
                static_cast<float>(minimum),
                std::max(static_cast<float>(minimum), combined - minimum));
            pane_weights = resize_start_weights;
            pane_weights[resize_index] = first;
            pane_weights[resize_index + 1] = combined - first;
            const auto left = FirstTabInGroup(0);
            if (left && *left < pane_rects.size()) {
                const int next_preview =
                    pane_rects[*left].left + static_cast<int>(std::lround(first));
                if (next_preview != resize_preview_position) {
                    resize_preview_position = next_preview;
                    ToggleResizeGuide();
                }
            }
        }
    }

    void EndResize() {
        ShowWindow(resize_guide, SW_HIDE);
        resize_preview_position = -1;
        resize_mode = ResizeMode::none;
        CommitLayoutNow();
        resize_start_weights.clear();
        if (GetCapture() == hwnd) {
            ReleaseCapture();
        }
    }

    bool SetResizeCursor() const {
        POINT point{};
        GetCursorPos(&point);
        ScreenToClient(hwnd, &point);
        const auto [mode, index] = ResizeHandleAt(point);
        (void)index;
        if (resize_mode == ResizeMode::pane || mode == ResizeMode::pane) {
            SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
            return true;
        }
        if (resize_mode == ResizeMode::queue || mode == ResizeMode::queue) {
            SetCursor(LoadCursorW(nullptr, IDC_SIZENS));
            return true;
        }
        return false;
    }

    bool OnCommand(const int id, const int notification, const HWND sender) {
        if (id == kConcurrencyEditId) {
            if (notification == EN_CHANGE) {
                CommitConcurrencyEdit(false);
                return true;
            }
            if (notification == EN_KILLFOCUS) {
                CommitConcurrencyEdit(true);
                return true;
            }
        }
        if (notification == BN_CLICKED) {
            if (id >= kPaneTabCloseBaseId &&
                id < kPaneTabCloseBaseId + static_cast<int>(panes.size())) {
                const std::size_t index = static_cast<std::size_t>(id - kPaneTabCloseBaseId);
                if (!panes[index].local) {
                    PostMessageW(hwnd, kClosePaneMessage, static_cast<WPARAM>(panes[index].id), 0);
                }
                return true;
            }
            if (id >= kPaneTabBaseId && id < kPaneTabBaseId + static_cast<int>(panes.size())) {
                const std::size_t index = static_cast<std::size_t>(id - kPaneTabBaseId);
                FocusPaneIndex(index);
                return true;
            }
            for (Pane& pane : panes) {
                if (pane.up_button && sender == pane.up_button) {
                    NavigateParent(pane);
                    return true;
                }
                if (sender == pane.hidden_checkbox) {
                    pane.show_hidden = !pane.show_hidden;
                    detail::SetSelfDrawnCheckboxChecked(pane.hidden_checkbox, pane.show_hidden);

                    // 正在返回的旧目录结果仍按切换前的筛选状态构建。递增 generation
                    // 使旧结果失效，并立即以新状态重新枚举；应用层的远程加载器也会
                    // 取消同一窗格上一次尚未完成的 Reactor 任务。
                    pane.loading = false;
                    ++pane.load_generation;
                    RequestList(pane, pane.location);
                    return true;
                }
                if (sender == pane.refresh_button) {
                    if (pane.view_state.action != ViewRecoveryAction::None) {
                        if (pane_action_callback) {
                            pane_action_callback(pane.id, pane.view_state.action);
                        }
                    } else {
                        PostMessageW(hwnd, kRefreshPaneMessage, static_cast<WPARAM>(pane.id), 0);
                    }
                    return true;
                }
            }
            const auto pane = std::find_if(panes.begin(), panes.end(), [sender](const Pane& item) {
                return item.close_button == sender;
            });
            if (pane != panes.end() && !pane->local) {
                PostMessageW(hwnd, kClosePaneMessage, static_cast<WPARAM>(pane->id), 0);
                return true;
            }
            if (id == kQueueClearId && transfer_manager) {
                try {
                    const std::size_t removed = transfer_manager->clear_terminal();
                    observed_terminal_jobs.clear();
                    RefreshQueue();
                    Report(removed == 0 ? L"没有可清空的传输记录"
                                        : L"已清空 " + std::to_wstring(removed) + L" 条传输记录");
                } catch (const std::exception& error) {
                    Report(ExceptionText(error));
                }
                return true;
            }
            const auto selected = SelectedQueueId();
            if (!selected || !transfer_manager) {
                if (id >= kQueuePauseId && id <= kQueueRetryId) {
                    Report(L"请先在队列中选择一个任务");
                    return true;
                }
            } else {
                try {
                    if (id == kQueuePauseId) {
                        transfer_manager->pause(*selected);
                    } else if (id == kQueueResumeId) {
                        transfer_manager->resume(*selected);
                    } else if (id == kQueueCancelId) {
                        transfer_manager->cancel(*selected);
                    } else if (id == kQueueRetryId) {
                        transfer_manager->retry(*selected);
                    } else {
                        return false;
                    }
                    RefreshQueue();
                    return true;
                } catch (const std::exception& error) {
                    Report(ExceptionText(error));
                    return true;
                }
            }
        }
        return false;
    }

    LRESULT OnNotify(const NMHDR& header, const LPARAM payload) {
        if (header.hwndFrom == queue_list) {
            if (header.code == NM_DBLCLK) {
                const auto* click = reinterpret_cast<const NMITEMACTIVATE*>(payload);
                const int visible_row = click ? click->iItem : table::SelectedRow(queue_list);
                if (visible_row >= 0 &&
                    ToggleQueueExpansion(static_cast<std::size_t>(visible_row))) {
                    return 0;
                }
                if (visible_row >= 0 && static_cast<std::size_t>(visible_row) < queue_rows.size() &&
                    !queue_rows[static_cast<std::size_t>(visible_row)].is_job()) {
                    return 0;
                }
                const auto selected = SelectedQueueId();
                if (selected && transfer_manager) {
                    try {
                        const auto job = transfer_manager->get(*selected);
                        if (job && job->state == transfer::TransferState::Paused) {
                            transfer_manager->resume(*selected);
                        } else if (job && job->state == transfer::TransferState::Running) {
                            transfer_manager->pause(*selected);
                        }
                    } catch (const std::exception& error) {
                        Report(ExceptionText(error));
                    }
                }
            }
            if (header.code == NM_RCLICK) {
                const auto* click = reinterpret_cast<const NMITEMACTIVATE*>(payload);
                if (click->iItem >= 0 && !table::IsRowSelected(queue_list, click->iItem)) {
                    table::SelectRow(queue_list, click->iItem);
                }
                ShowQueueContextMenu();
            }
            return 0;
        }

        Pane* pane = FindPaneByList(header.hwndFrom);
        if (!pane) {
            return 0;
        }
        if (header.code == LVN_ITEMCHANGED) {
            return 0;
        }
        if (header.code == NM_DBLCLK) {
            OpenSelectedDirectory(*pane);
            return 0;
        }
        if (header.code == LVN_BEGINDRAG) {
            BeginInternalDrag(*pane);
            return 0;
        }
        if (header.code == NM_RCLICK) {
            const auto* click = reinterpret_cast<const NMITEMACTIVATE*>(payload);
            if (click->iItem >= 0 && !table::IsRowSelected(pane->list, click->iItem)) {
                table::SelectRow(pane->list, click->iItem);
            }
            active_pane_id = pane->id;
            ShowPaneContextMenu(*pane);
            return 0;
        }
        if (header.code == NM_SETFOCUS) {
            active_pane_id = pane->id;
        }
        return 0;
    }

    int FileIconFor(const FileRow& row) {
        if (row.directory) {
            return folder_icon;
        }
        if (row.local_shortcut && row.uri.is_local()) {
            return SLink::ui::file_icons::IconForPath(FromUtf8(row.uri.path()));
        }
        return SLink::ui::file_icons::IconForFileName(row.name);
    }

    std::wstring PaneCellText(const TransferPaneId pane_id,
                              const std::size_t row_index,
                              const std::size_t column) const {
        const Pane* pane = FindPane(pane_id);
        if (!pane || row_index >= pane->rows.size()) {
            return {};
        }
        const FileRow& row = pane->rows[row_index];
        const std::wstring* text = &row.name;
        std::wstring display_name;
        if (column == 0 && row.local_shortcut && row.name.size() > 4) {
            display_name = local_shortcut::DisplayName(row.name);
            text = &display_name;
        }
        switch (column) {
        case 1:
            text = &row.status;
            break;
        case 2:
            text = &row.size;
            break;
        case 3:
            text = &row.modified;
            break;
        case 4:
            text = &row.permissions;
            break;
        case 5:
            text = &row.owner;
            break;
        default:
            break;
        }
        return *text;
    }

    static std::string QueueItemKey(const transfer::TransferId job_id,
                                    const transfer::TransferItemId item_id) {
        return detail::TransferQueueItemKey(job_id, item_id);
    }

    const transfer::TransferItemProgress* QueueItem(const QueueRow& row) const {
        if (row.is_job() || row.job_index >= queue_jobs.size())
            return nullptr;
        const auto cache = queue_details.find(queue_jobs[row.job_index].id);
        if (cache == queue_details.end())
            return nullptr;
        const auto item = cache->second.indexes.find(row.item_id);
        return item == cache->second.indexes.end() ? nullptr : &cache->second.items[item->second];
    }

    std::wstring ServerDisplayName(const std::string_view id) const {
        const auto found = std::ranges::find(server_options, id, &TransferServerOption::id);
        return found == server_options.end() ? FromUtf8(id) : found->label;
    }

    std::wstring DisplayEndpoint(const transfer::FileUri& uri) const {
        if (uri.is_local())
            return FromUtf8(uri.path());
        return ServerDisplayName(uri.authority()) + L":" + FromUtf8(uri.path());
    }

    struct QueueEndpoints {
        transfer::FileUri local_side;
        transfer::FileUri remote_side;
        std::wstring direction;
    };

    QueueEndpoints ArrangeEndpoints(const transfer::FileUri& source,
                                    const transfer::FileUri& target) const {
        if (source.is_local() && target.is_sftp()) {
            return {source, target, L"→"};
        }
        if (source.is_sftp() && target.is_local()) {
            return {target, source, L"←"};
        }
        return {source, target, L"→"};
    }

    std::wstring QueueCellText(const std::size_t row, const std::size_t column) const {
        if (row >= queue_rows.size() || queue_rows[row].job_index >= queue_jobs.size()) {
            return {};
        }
        const QueueRow& visible = queue_rows[row];
        const transfer::TransferJob& job = queue_jobs[visible.job_index];
        const transfer::TransferItemProgress* item = QueueItem(visible);
        const transfer::FileUri source = item ? item->source : job.spec.source;
        const transfer::FileUri target =
            item ? item->target
                 : (job.effective_target.filename().empty() ? job.spec.target
                                                            : job.effective_target);
        const std::uint64_t transferred = item ? item->bytes_transferred : job.bytes_transferred;
        const std::uint64_t total = item ? item->bytes_total : job.bytes_total;
        transfer::TransferState state = item ? item->state : job.state;
        if (item && state == transfer::TransferState::Running &&
            job.state != transfer::TransferState::Running) {
            state = job.state;
        }
        const QueueEndpoints endpoints = ArrangeEndpoints(source, target);
        switch (column) {
        case 0: {
            const std::string name = source.filename();
            return name.empty() ? FromUtf8(source.path()) : FromUtf8(name);
        }
        case 1: {
            std::wstring text;
            if (!item && job.source_is_directory && state == transfer::TransferState::Running) {
                if (!job.discovery_complete) {
                    text = L"扫描中 · 已发现 " + std::to_wstring(job.file_count) + L" 个文件";
                } else {
                    text = L"传输中 · 已完成 " + std::to_wstring(job.completed_file_count) + L"/" +
                           std::to_wstring(job.file_count);
                }
            } else {
                text = TransferStateText(state);
            }
            const std::string& error = item ? item->error : job.error;
            if (!error.empty())
                text += L" · " + FromUtf8(error);
            return text;
        }
        case 2:
            return PercentageText(transferred, total);
        case 3:
            return TransferredSizeText(transferred, total);
        case 4:
            return QueueSpeedText(visible, state);
        case 5:
            return DisplayEndpoint(endpoints.local_side);
        case 6:
            return endpoints.direction;
        case 7:
            return DisplayEndpoint(endpoints.remote_side);
        default:
            return {};
        }
    }

    int QueueIcon(const std::size_t row) const {
        if (row >= queue_rows.size() || queue_rows[row].job_index >= queue_jobs.size())
            return -1;
        const QueueRow& visible = queue_rows[row];
        const auto* item = QueueItem(visible);
        const auto& job = queue_jobs[visible.job_index];
        const bool directory = item ? item->directory : job.source_is_directory;
        if (directory)
            return folder_icon;
        const std::string name = (item ? item->source : job.spec.source).filename();
        return SLink::ui::file_icons::IconForFileName(FromUtf8(name));
    }

    std::optional<table::TreeCell> QueueTreeCell(const std::size_t row) const {
        if (row >= queue_rows.size())
            return std::nullopt;
        const auto& visible = queue_rows[row];
        return table::TreeCell{visible.depth,
                               visible.expandable,
                               visible.expanded,
                               visible.last_sibling,
                               visible.ancestor_lines};
    }

    std::optional<unsigned> QueueProgress(const std::size_t row, const std::size_t column) const {
        if (column != 2 || row >= queue_rows.size() ||
            queue_rows[row].job_index >= queue_jobs.size()) {
            return std::nullopt;
        }
        const QueueRow& visible = queue_rows[row];
        const auto* item = QueueItem(visible);
        const auto& job = queue_jobs[visible.job_index];
        const std::uint64_t transferred = item ? item->bytes_transferred : job.bytes_transferred;
        const std::uint64_t total = item ? item->bytes_total : job.bytes_total;
        return total == 0
                   ? 0U
                   : static_cast<unsigned>(std::min<std::uint64_t>(100, transferred * 100 / total));
    }

    static void UpdateSpeedSample(QueueSpeedSample& sample,
                                  const std::uint64_t bytes,
                                  const transfer::TransferState state,
                                  const std::chrono::steady_clock::time_point now) {
        if (sample.sampled_at == std::chrono::steady_clock::time_point{} || bytes < sample.bytes) {
            sample = {bytes, 0.0, now};
            return;
        }
        const double seconds = std::chrono::duration<double>(now - sample.sampled_at).count();
        if (seconds < 0.1)
            return;
        const std::uint64_t delta = bytes - sample.bytes;
        if (delta != 0 || state == transfer::TransferState::Running) {
            const double instant = static_cast<double>(delta) / seconds;
            sample.bytes_per_second = sample.bytes_per_second <= 0.0
                                          ? instant
                                          : sample.bytes_per_second * 0.60 + instant * 0.40;
        }
        sample.bytes = bytes;
        sample.sampled_at = now;
    }

    void UpdateQueueSpeeds() {
        const auto now = std::chrono::steady_clock::now();
        for (const auto& job : queue_jobs) {
            UpdateSpeedSample(queue_job_speeds[job.id], job.bytes_transferred, job.state, now);
            const auto detail = queue_details.find(job.id);
            const auto running = queue_running_visible_items.find(job.id);
            if (detail == queue_details.end() || running == queue_running_visible_items.end())
                continue;
            for (const auto item_id : running->second) {
                const auto index = detail->second.indexes.find(item_id);
                if (index == detail->second.indexes.end()) {
                    continue;
                }
                const auto& item = detail->second.items[index->second];
                UpdateSpeedSample(
                    queue_item_speeds[job.id][item.id], item.bytes_transferred, item.state, now);
            }
        }
    }

    std::wstring QueueSpeedText(const QueueRow& row, const transfer::TransferState state) const {
        if (state != transfer::TransferState::Running)
            return L"—";
        const QueueSpeedSample* sample{};
        if (row.job_index >= queue_jobs.size())
            return L"—";
        const auto& job = queue_jobs[row.job_index];
        if (row.is_job()) {
            const auto found = queue_job_speeds.find(job.id);
            if (found != queue_job_speeds.end())
                sample = &found->second;
        } else {
            const auto job_samples = queue_item_speeds.find(job.id);
            if (job_samples != queue_item_speeds.end()) {
                const auto found = job_samples->second.find(row.item_id);
                if (found != job_samples->second.end()) {
                    sample = &found->second;
                }
            }
        }
        if (!sample || sample->bytes_per_second < 1.0) {
            return L"0 B/s";
        }
        const double bounded =
            (std::min)(sample->bytes_per_second,
                       static_cast<double>((std::numeric_limits<std::uint64_t>::max)()));
        return detail::FormatFileSize(static_cast<std::uint64_t>(bounded)) + L"/s";
    }

    std::optional<transfer::TransferId> SelectedQueueId() const {
        if (!queue_list) {
            return std::nullopt;
        }
        const int selected = table::SelectedRow(queue_list);
        if (selected < 0 || static_cast<std::size_t>(selected) >= queue_rows.size()) {
            return std::nullopt;
        }
        const QueueRow& row = queue_rows[static_cast<std::size_t>(selected)];
        return row.job_index < queue_jobs.size()
                   ? std::optional<transfer::TransferId>(queue_jobs[row.job_index].id)
                   : std::nullopt;
    }

    void ShowQueueContextMenu() {
        HMENU menu = CreatePopupMenu();
        if (!menu) {
            return;
        }

        const auto selected = SelectedQueueId();
        std::optional<transfer::TransferJob> job;
        if (selected && transfer_manager) {
            try {
                job = transfer_manager->get(*selected);
            } catch (const std::exception& error) {
                Report(ExceptionText(error));
            }
        }
        const auto enabled = [](const bool value) {
            return value ? MF_ENABLED : MF_GRAYED;
        };
        const bool unfinished = job && job->state != transfer::TransferState::Completed &&
                                job->state != transfer::TransferState::Failed &&
                                job->state != transfer::TransferState::Cancelled;
        AppendMenuW(menu,
                    MF_STRING | enabled(job && job->state == transfer::TransferState::Running),
                    kQueueContextPause,
                    L"暂停");
        AppendMenuW(menu,
                    MF_STRING | enabled(job && job->state == transfer::TransferState::Paused),
                    kQueueContextResume,
                    L"恢复");
        AppendMenuW(menu, MF_STRING | enabled(unfinished), kQueueContextCancel, L"取消任务");
        AppendMenuW(menu,
                    MF_STRING | enabled(job && (job->state == transfer::TransferState::Failed ||
                                                job->state == transfer::TransferState::Cancelled)),
                    kQueueContextRetry,
                    L"重试");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu,
                    MF_STRING | enabled(transfer_manager != nullptr),
                    kQueueContextClear,
                    L"清空已结束记录");

        POINT point{};
        GetCursorPos(&point);
        const UINT command = TrackPopupMenu(
            menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, hwnd, nullptr);
        DestroyMenu(menu);
        if (!command || !transfer_manager) {
            return;
        }
        try {
            if (command == kQueueContextClear) {
                const std::size_t removed = transfer_manager->clear_terminal();
                observed_terminal_jobs.clear();
                Report(removed == 0 ? L"没有可清空的传输记录"
                                    : L"已清空 " + std::to_wstring(removed) + L" 条传输记录");
            } else if (selected) {
                if (command == kQueueContextPause) {
                    transfer_manager->pause(*selected);
                } else if (command == kQueueContextResume) {
                    transfer_manager->resume(*selected);
                } else if (command == kQueueContextCancel) {
                    transfer_manager->cancel(*selected);
                } else if (command == kQueueContextRetry) {
                    transfer_manager->retry(*selected);
                }
            }
            RefreshQueue();
        } catch (const std::exception& error) {
            Report(ExceptionText(error));
        }
    }

    void SyncConcurrencyEdit() {
        if (!concurrency_edit) {
            return;
        }
        updating_concurrency_edit = true;
        SetWindowTextW(concurrency_edit, std::to_wstring(concurrency_value).c_str());
        updating_concurrency_edit = false;
    }

    void CommitConcurrencyEdit(const bool restore_invalid) {
        if (!concurrency_edit || updating_concurrency_edit) {
            return;
        }
        wchar_t text[3]{};
        GetWindowTextW(concurrency_edit, text, 3);
        const bool valid = text[0] >= L'1' && text[0] <= L'8' && text[1] == L'\0';
        if (!valid) {
            if (restore_invalid) {
                SyncConcurrencyEdit();
            }
            return;
        }
        const int value = text[0] - L'0';
        if (value == concurrency_value) {
            return;
        }
        try {
            if (transfer_manager) {
                transfer_manager->set_concurrency(static_cast<std::size_t>(value));
            }
            concurrency_value = value;
        } catch (const std::exception& error) {
            SyncConcurrencyEdit();
            Report(ExceptionText(error));
        }
    }

    std::vector<transfer::FileUri> SelectedUris(const Pane& pane) const {
        std::vector<transfer::FileUri> result;
        for (const int index : table::SelectedRows(pane.list)) {
            if (static_cast<std::size_t>(index) >= pane.rows.size()) {
                continue;
            }
            const FileRow& row = pane.rows[index];
            if (row.transferable) {
                result.push_back(row.uri);
            }
        }
        return result;
    }

    FileRow* SelectedRow(Pane& pane) {
        const int selected = table::SelectedRow(pane.list);
        if (selected < 0 || static_cast<std::size_t>(selected) >= pane.rows.size()) {
            return nullptr;
        }
        return &pane.rows[selected];
    }

    transfer::FileUri DropBase(Pane& pane, const POINT screen_point) {
        POINT point = screen_point;
        ScreenToClient(pane.list, &point);
        const int index = table::HitTest(pane.list, point);
        if (index >= 0 && static_cast<std::size_t>(index) < pane.rows.size()) {
            const FileRow& row = pane.rows[index];
            if (row.directory && row.transferable) {
                return row.uri;
            }
        }
        return pane.location;
    }

    [[nodiscard]] bool IsInternalDropAllowed(const transfer::FileUri& target) const {
        if (drag_uris.empty())
            return false;
        for (const auto& source : drag_uris) {
            if (source == target || source.parent() == target)
                return false;
        }
        for (const auto& directory : drag_directory_uris) {
            if (directory.scheme() != target.scheme() ||
                directory.authority() != target.authority()) {
                continue;
            }
            std::string prefix = directory.path();
            if (prefix.empty() || prefix.back() != '/')
                prefix.push_back('/');
            if (target.path().starts_with(prefix))
                return false;
        }
        return true;
    }

    void ClearInternalDragTarget() {
        if (drag_target_pane_id && drag_target_row >= 0) {
            if (Pane* pane = FindPane(*drag_target_pane_id)) {
                table::SetDropTargetRow(pane->list, -1);
            }
        }
        drag_target_base.reset();
        drag_target_pane_id.reset();
        drag_target_row = -1;
        drag_drop_allowed = false;
    }

    void UpdateInternalDragTarget(const POINT screen_point) {
        Pane* target = FindPaneAtScreenPoint(screen_point);
        std::optional<transfer::FileUri> next_base;
        std::optional<TransferPaneId> next_pane_id;
        int next_row = -1;
        bool next_allowed = false;

        if (target && !drag_uris.empty()) {
            POINT point = screen_point;
            ScreenToClient(target->list, &point);
            const int index = table::HitTest(target->list, point);
            transfer::FileUri base = target->location;
            if (index >= 0 && static_cast<std::size_t>(index) < target->rows.size()) {
                const FileRow& row = target->rows[static_cast<std::size_t>(index)];
                if (row.directory && row.transferable) {
                    base = row.uri;
                    next_row = index;
                }
            }
            next_allowed = IsInternalDropAllowed(base);
            if (next_allowed) {
                next_base = std::move(base);
                next_pane_id = target->id;
            }
        }

        const bool target_changed =
            drag_target_pane_id != next_pane_id || drag_target_row != next_row;
        if (target_changed && drag_target_pane_id && drag_target_row >= 0) {
            if (Pane* previous = FindPane(*drag_target_pane_id)) {
                table::SetDropTargetRow(previous->list, -1);
            }
        }
        drag_target_base = std::move(next_base);
        drag_target_pane_id = next_pane_id;
        drag_target_row = next_allowed ? next_row : -1;
        drag_drop_allowed = next_allowed;
        if (target_changed && next_allowed && next_row >= 0) {
            table::SetDropTargetRow(target->list, next_row);
        }
        SetCursor(LoadCursorW(nullptr, drag_drop_allowed ? IDC_ARROW : IDC_NO));
    }

    void BeginInternalDrag(Pane& pane) {
        drag_uris = SelectedUris(pane);
        if (drag_uris.empty()) {
            return;
        }
        drag_directory_uris.clear();
        for (const int index : table::SelectedRows(pane.list)) {
            if (index >= 0 && static_cast<std::size_t>(index) < pane.rows.size() &&
                pane.rows[static_cast<std::size_t>(index)].directory &&
                pane.rows[static_cast<std::size_t>(index)].transferable) {
                drag_directory_uris.push_back(pane.rows[static_cast<std::size_t>(index)].uri);
            }
        }
        active_pane_id = pane.id;
        internal_drag = true;
        SetCapture(hwnd);
        POINT point{};
        GetCursorPos(&point);
        UpdateInternalDragTarget(point);
    }

    void CancelInternalDrag() {
        ClearInternalDragTarget();
        internal_drag = false;
        drag_uris.clear();
        drag_directory_uris.clear();
        if (GetCapture() == hwnd) {
            ReleaseCapture();
        }
    }

    void CompleteInternalDrag(const POINT screen_point) {
        UpdateInternalDragTarget(screen_point);
        const std::vector<transfer::FileUri> sources = drag_uris;
        const auto target_base = drag_target_base;
        ClearInternalDragTarget();
        internal_drag = false;
        drag_uris.clear();
        drag_directory_uris.clear();
        if (GetCapture() == hwnd) {
            ReleaseCapture();
        }
        if (!target_base || sources.empty()) {
            return;
        }
        Enqueue(sources, *target_base);
    }

    void CopySelection(Pane& pane) {
        clipboard_uris = SelectedUris(pane);
        clipboard_move = false;
        if (clipboard_uris.empty()) {
            Report(L"没有可复制的文件");
            return;
        }

        const bool all_local = std::all_of(
            clipboard_uris.begin(), clipboard_uris.end(), [](const transfer::FileUri& uri) {
                return uri.is_local();
            });
        if (all_local && OpenClipboard(hwnd)) {
            EmptyClipboard();
            std::vector<std::wstring> paths;
            std::size_t characters = 1;
            for (const auto& uri : clipboard_uris) {
                paths.push_back(FromUtf8(uri.path()));
                characters += paths.back().size() + 1;
            }
            const SIZE_T bytes = sizeof(DROPFILES) + characters * sizeof(wchar_t);
            HGLOBAL memory = GlobalAlloc(GHND, bytes);
            if (memory) {
                auto* drop = static_cast<DROPFILES*>(GlobalLock(memory));
                drop->pFiles = sizeof(DROPFILES);
                drop->fWide = TRUE;
                auto* output = reinterpret_cast<wchar_t*>(reinterpret_cast<std::byte*>(drop) +
                                                          sizeof(DROPFILES));
                for (const auto& path : paths) {
                    std::copy(path.begin(), path.end(), output);
                    output += path.size();
                    *output++ = L'\0';
                }
                *output = L'\0';
                GlobalUnlock(memory);
                if (!SetClipboardData(CF_HDROP, memory)) {
                    GlobalFree(memory);
                }
            }
            CloseClipboard();
        }
        clipboard_sequence = GetClipboardSequenceNumber();
        Report(L"已复制 " + std::to_wstring(clipboard_uris.size()) + L" 个项目");
    }

    void CutSelection(Pane& pane) {
        CopySelection(pane);
        if (!clipboard_uris.empty()) {
            clipboard_move = true;
            Report(L"已剪切 " + std::to_wstring(clipboard_uris.size()) + L" 个项目");
        }
    }

    std::vector<transfer::FileUri> ClipboardSources() {
        if (!clipboard_uris.empty() && GetClipboardSequenceNumber() == clipboard_sequence) {
            return clipboard_uris;
        }
        clipboard_uris.clear();
        clipboard_move = false;
        std::vector<transfer::FileUri> result;
        if (!IsClipboardFormatAvailable(CF_HDROP) || !OpenClipboard(hwnd)) {
            return result;
        }
        const auto drop = static_cast<HDROP>(GetClipboardData(CF_HDROP));
        if (drop) {
            const UINT count = DragQueryFileW(drop, 0xFFFFFFFFU, nullptr, 0);
            for (UINT index = 0; index < count; ++index) {
                const UINT length = DragQueryFileW(drop, index, nullptr, 0);
                std::wstring path(static_cast<std::size_t>(length) + 1, L'\0');
                DragQueryFileW(drop, index, path.data(), length + 1);
                path.resize(length);
                result.push_back(transfer::FileUri::local(ToUtf8(path)));
            }
        }
        CloseClipboard();
        return result;
    }

    void PasteInto(Pane& pane) {
        const bool move = clipboard_move && !clipboard_uris.empty() &&
                          GetClipboardSequenceNumber() == clipboard_sequence;
        const auto sources = ClipboardSources();
        if (sources.empty()) {
            Report(L"剪贴板中没有可传输的文件");
            return;
        }
        Enqueue(sources, pane.location, move);
        if (move) {
            clipboard_uris.clear();
            clipboard_move = false;
        }
    }

    void AcceptDroppedFiles(Pane& pane, const HDROP drop) {
        std::vector<transfer::FileUri> sources;
        const UINT count = DragQueryFileW(drop, 0xFFFFFFFFU, nullptr, 0);
        for (UINT index = 0; index < count; ++index) {
            const UINT length = DragQueryFileW(drop, index, nullptr, 0);
            std::wstring path(static_cast<std::size_t>(length) + 1, L'\0');
            DragQueryFileW(drop, index, path.data(), length + 1);
            path.resize(length);
            sources.push_back(transfer::FileUri::local(ToUtf8(path)));
        }
        POINT point{};
        DragQueryPoint(drop, &point);
        ClientToScreen(pane.list, &point);
        DragFinish(drop);
        if (!sources.empty()) {
            Enqueue(sources, DropBase(pane, point));
        }
    }

    bool VisibleTargetExists(const transfer::FileUri& directory,
                             const std::string_view filename) const {
        const std::wstring name = FromUtf8(filename);
        for (const Pane& pane : panes) {
            if (pane.location != directory) {
                continue;
            }
            return std::any_of(pane.rows.begin(), pane.rows.end(), [&](const FileRow& row) {
                if (row.name == L"..") {
                    return false;
                }
                return pane.local ? _wcsicmp(row.name.c_str(), name.c_str()) == 0
                                  : row.name == name;
            });
        }
        return false;
    }

    void Enqueue(const std::vector<transfer::FileUri>& sources,
                 const transfer::FileUri& target_directory,
                 const bool move = false) {
        if (!transfer_manager) {
            Report(L"传输管理器尚未绑定");
            return;
        }
        std::size_t count = 0;
        std::optional<TransferConflictChoice> batch_choice;
        for (const transfer::FileUri& source : sources) {
            const std::string name = source.filename();
            if (name.empty()) {
                continue;
            }
            const transfer::FileUri target = target_directory.joined(name);
            if (target == source) {
                Report(L"源与目标路径相同，已跳过");
                continue;
            }
            TransferConflictChoice choice;
            if (VisibleTargetExists(target_directory, name)) {
                if (batch_choice) {
                    choice = *batch_choice;
                } else {
                    const auto selected = ShowTransferConflictDialog(
                        hwnd, source, target, DisplayEndpoint(source), DisplayEndpoint(target));
                    if (!selected) {
                        break;
                    }
                    choice = *selected;
                    if (choice.apply_all) {
                        batch_choice = choice;
                    }
                }
            }
            try {
                transfer::TransferSpec spec;
                spec.source = source;
                spec.target = target;
                spec.conflict = choice.policy;
                spec.move = move;
                spec.verify_sha256 = choice.verify_sha256;
                (void)transfer_manager->enqueue(std::move(spec));
                ++count;
            } catch (const std::exception& error) {
                Report(ExceptionText(error));
            }
        }
        if (count != 0) {
            Report(L"已加入 " + std::to_wstring(count) + L" 个传输任务");
            RefreshQueue();
        }
    }

    void ShowPaneContextMenu(Pane& pane) {
        const std::vector<transfer::FileUri> selected = SelectedUris(pane);
        HMENU menu = CreatePopupMenu();
        HMENU targets = CreatePopupMenu();
        HMENU move_targets = CreatePopupMenu();
        HMENU create_menu = CreatePopupMenu();
        if (!menu || !targets || !move_targets || !create_menu) {
            if (menu)
                DestroyMenu(menu);
            if (targets)
                DestroyMenu(targets);
            if (move_targets)
                DestroyMenu(move_targets);
            if (create_menu)
                DestroyMenu(create_menu);
            return;
        }

        for (std::size_t index = 0; index < panes.size(); ++index) {
            const Pane& target = panes[index];
            std::wstring label = target.title + L"  [" + PathText(target.location) + L"]";
            AppendMenuW(
                targets, MF_STRING, kContextTransferBase + static_cast<UINT>(index), label.c_str());
            AppendMenuW(move_targets,
                        MF_STRING,
                        kContextMoveBase + static_cast<UINT>(index),
                        label.c_str());
        }
        AppendMenuW(menu,
                    MF_POPUP | (selected.empty() || !transfer_manager ? MF_GRAYED : MF_ENABLED),
                    reinterpret_cast<UINT_PTR>(targets),
                    L"传输到");
        AppendMenuW(menu,
                    MF_POPUP | (selected.empty() || !transfer_manager ? MF_GRAYED : MF_ENABLED),
                    reinterpret_cast<UINT_PTR>(move_targets),
                    L"移动到");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kContextRefresh, L"刷新\tF5");
        AppendMenuW(menu,
                    MF_STRING | (selected.empty() ? MF_GRAYED : MF_ENABLED),
                    kContextCopy,
                    L"复制\tCtrl+C");
        AppendMenuW(menu,
                    MF_STRING | (selected.empty() ? MF_GRAYED : MF_ENABLED),
                    kContextCut,
                    L"剪切\tCtrl+X");
        AppendMenuW(menu,
                    MF_STRING | ((!transfer_manager ||
                                  (clipboard_uris.empty() && !IsClipboardFormatAvailable(CF_HDROP)))
                                     ? MF_GRAYED
                                     : MF_ENABLED),
                    kContextPaste,
                    L"粘贴\tCtrl+V");
        const bool can_create =
            pane.endpoint && !pane.showing_this_pc && (!pane.local || pane.location.path() != "/");
        AppendMenuW(create_menu, MF_STRING, kContextCreateDirectory, L"新建文件夹");
        AppendMenuW(create_menu, MF_STRING, kContextCreateFile, L"新建文件");
        AppendMenuW(menu,
                    MF_POPUP | (can_create ? MF_ENABLED : MF_GRAYED),
                    reinterpret_cast<UINT_PTR>(create_menu),
                    L"新建");
        FileRow* selected_row = SelectedRow(pane);
        const bool can_change_item = selected.size() == 1 && selected_row &&
                                     selected_row->transferable && selected_row->name != L"..";
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu,
                    MF_STRING | (can_change_item ? MF_ENABLED : MF_GRAYED),
                    kContextRename,
                    L"重命名\tF2");
        AppendMenuW(menu,
                    MF_STRING | (can_change_item ? MF_ENABLED : MF_GRAYED),
                    kContextPermissions,
                    L"修改权限…");
        AppendMenuW(menu,
                    MF_STRING | (selected.empty() ? MF_GRAYED : MF_ENABLED),
                    kContextDelete,
                    L"删除\tDelete");

        bool can_edit = false;
        if (FileRow* row = selected_row) {
            can_edit = !row->directory && row->transferable && row->uri.is_sftp() &&
                       static_cast<bool>(remote_edit_callback);
        }
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu,
                    MF_STRING | (can_edit ? MF_ENABLED : MF_GRAYED),
                    kContextRemoteEdit,
                    L"远程编辑\tF4");

        POINT point{};
        GetCursorPos(&point);
        const UINT command = TrackPopupMenu(
            menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, hwnd, nullptr);
        if (command == kContextRefresh) {
            RequestList(pane, pane.location);
        } else if (command == kContextCopy) {
            CopySelection(pane);
        } else if (command == kContextCut) {
            CutSelection(pane);
        } else if (command == kContextPaste) {
            PasteInto(pane);
        } else if (command == kContextDelete) {
            const std::wstring prompt = L"确定删除选中的 " + std::to_wstring(selected.size()) +
                                        L" 个项目吗？\n目录将递归删除，此操作无法撤销。";
            if (!selected.empty() &&
                MessageBoxW(
                    hwnd, prompt.c_str(), L"确认删除", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) ==
                    IDYES) {
                RunMutation(
                    pane,
                    selected,
                    [](transfer::IFileEndpoint& endpoint, const transfer::FileUri& uri) {
                        endpoint.remove(uri, true);
                    },
                    L"已删除 " + std::to_wstring(selected.size()) + L" 个项目");
            }
        } else if (command == kContextRemoteEdit) {
            OpenSelectedRemoteFile(pane);
        } else if (command == kContextRename) {
            RenameSelected(pane);
        } else if (command == kContextPermissions) {
            ChangeSelectedPermissions(pane);
        } else if (command == kContextCreateDirectory) {
            BeginCreate(pane, true);
        } else if (command == kContextCreateFile) {
            BeginCreate(pane, false);
        } else if (command >= kContextTransferBase &&
                   command < kContextTransferBase + panes.size()) {
            const std::size_t target_index = command - kContextTransferBase;
            Enqueue(selected, panes[target_index].location);
        } else if (command >= kContextMoveBase && command < kContextMoveBase + panes.size()) {
            const std::size_t target_index = command - kContextMoveBase;
            Enqueue(selected, panes[target_index].location, true);
        }
        DestroyMenu(menu);
    }

    std::wstring ValidateInlineName(const std::wstring_view value, const bool local_path) const {
        if (value.empty())
            return L"名称不能为空";
        if (value == L"." || value == L"..")
            return L"不能使用此名称";
        if (value.find_first_of(L"/\\") != std::wstring_view::npos) {
            return L"名称不能包含 / 或 \\";
        }
        if (local_path) {
            if (value.find_first_of(L"<>:\"|?*") != std::wstring_view::npos) {
                return L"本地名称包含 Windows 不允许的字符";
            }
            if (value.back() == L'.' || value.back() == L' ') {
                return L"本地名称不能以句点或空格结尾";
            }
        }
        return {};
    }

    std::wstring InlineEditText() const {
        if (!inline_name_edit)
            return {};
        const int length = GetWindowTextLengthW(inline_name_edit);
        std::wstring value(static_cast<std::size_t>(std::max(0, length)) + 1, L'\0');
        if (length > 0) {
            GetWindowTextW(inline_name_edit, value.data(), length + 1);
        }
        value.resize(static_cast<std::size_t>(std::max(0, length)));
        return value;
    }

    void CloseInlineEditWindow() {
        if (!inline_name_edit)
            return;
        closing_inline_edit = true;
        HWND edit = std::exchange(inline_name_edit, nullptr);
        RemoveWindowSubclass(edit, &InlineNameEditSubclass, kInlineNameEditId);
        DestroyWindow(edit);
        closing_inline_edit = false;
    }

    void RemoveProvisionalRow(const TransferPaneId pane_id, const std::size_t row_index) {
        Pane* pane = FindPane(pane_id);
        if (!pane || row_index >= pane->rows.size())
            return;
        pane->rows.erase(pane->rows.begin() + static_cast<std::ptrdiff_t>(row_index));
        table::SetRowCount(pane->list, pane->rows.size());
        table::ClearSelection(pane->list);
        table::Refresh(pane->list);
    }

    void CancelInlineEdit() {
        if (!inline_name_edit && inline_edit_mode == InlineEditMode::none) {
            return;
        }
        const InlineEditMode mode = inline_edit_mode;
        const TransferPaneId pane_id = inline_edit_pane_id;
        const std::size_t row = inline_edit_row;
        inline_edit_mode = InlineEditMode::none;
        CloseInlineEditWindow();
        if (mode == InlineEditMode::create_file || mode == InlineEditMode::create_directory) {
            RemoveProvisionalRow(pane_id, row);
        }
        inline_edit_original.clear();
    }

    bool BeginInlineEdit(Pane& pane,
                         const std::size_t row_index,
                         const InlineEditMode mode,
                         const transfer::FileUri& source,
                         const std::wstring_view initial_name) {
        CancelInlineEdit();
        if (row_index >= pane.rows.size())
            return false;

        // TableControl 会为文件拖动和框选捕获鼠标。F2 或右键重命名可能在捕获
        // 状态尚未完全结束时触发，因此创建行内 EDIT 前先释放表格捕获。
        if (GetCapture() == pane.list) {
            ReleaseCapture();
        }
        table::SelectRow(pane.list, static_cast<int>(row_index));
        table::EnsureCellVisible(pane.list, row_index, 0);
        const auto cell = table::CellTextRect(pane.list, row_index, 0);
        if (!cell)
            return false;
        RECT edit_rect = *cell;
        MapWindowPoints(pane.list, pane.container, reinterpret_cast<POINT*>(&edit_rect), 2);
        const int inset = detail::ScaleForDpi(1, detail::WindowDpi(hwnd));
        InflateRect(&edit_rect, inset, -inset);
        if (edit_rect.right - edit_rect.left < detail::ScaleForDpi(60, detail::WindowDpi(hwnd))) {
            return false;
        }
        inline_name_edit = CreateWindowExW(
            0,
            L"EDIT",
            std::wstring(initial_name).c_str(),
            WS_CHILD | WS_TABSTOP | WS_BORDER | WS_CLIPSIBLINGS | ES_AUTOHSCROLL | ES_NOHIDESEL,
            edit_rect.left,
            edit_rect.top,
            edit_rect.right - edit_rect.left,
            edit_rect.bottom - edit_rect.top,
            pane.container,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kInlineNameEditId)),
            instance,
            nullptr);
        if (!inline_name_edit)
            return false;
        inline_edit_mode = mode;
        inline_edit_pane_id = pane.id;
        inline_edit_row = row_index;
        inline_edit_source = source;
        inline_edit_original.assign(initial_name);
        detail::ApplySelfDrawnTheme(inline_name_edit);
        detail::SetControlFont(inline_name_edit, font);
        SetWindowSubclass(inline_name_edit,
                          &InlineNameEditSubclass,
                          kInlineNameEditId,
                          reinterpret_cast<DWORD_PTR>(this));
        SetWindowPos(inline_name_edit,
                     HWND_TOP,
                     0,
                     0,
                     0,
                     0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        const std::size_t dot = mode == InlineEditMode::rename
                                    ? inline_edit_original.find_last_of(L'.')
                                    : std::wstring::npos;
        const LPARAM selection_end = dot != std::wstring::npos && dot != 0
                                         ? static_cast<LPARAM>(dot)
                                         : static_cast<LPARAM>(-1);
        SetFocus(inline_name_edit);
        // WM_SETFOCUS 可能调整插入光标，因此获得焦点后再按资源管理器方式选中
        // 文件基本名，保证选区可见且可以继续用鼠标调整。
        SendMessageW(inline_name_edit, EM_SETSEL, 0, selection_end);
        // 控件在完成主题、字体和选区初始化后才显示。此处同步刷新一次客户区和
        // 非客户区，避免选区要等到下一次鼠标移动才出现在屏幕上。
        RedrawWindow(inline_name_edit,
                     nullptr,
                     nullptr,
                     RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_UPDATENOW);
        return true;
    }

    void CommitInlineEdit() {
        if (!inline_name_edit || inline_edit_mode == InlineEditMode::none) {
            return;
        }
        Pane* pane = FindPane(inline_edit_pane_id);
        if (!pane) {
            CancelInlineEdit();
            return;
        }
        std::wstring name = InlineEditText();
        const std::wstring error = ValidateInlineName(name, pane->local);
        if (!error.empty()) {
            Report(error);
            MessageBeep(MB_ICONWARNING);
            SetFocus(inline_name_edit);
            SendMessageW(inline_name_edit, EM_SETSEL, 0, -1);
            return;
        }

        const InlineEditMode mode = inline_edit_mode;
        const TransferPaneId pane_id = inline_edit_pane_id;
        const std::size_t row = inline_edit_row;
        const transfer::FileUri source = inline_edit_source;
        if (mode == InlineEditMode::rename && name == inline_edit_original) {
            CancelInlineEdit();
            return;
        }

        transfer::FileUri target;
        try {
            target = mode == InlineEditMode::rename ? source.parent().joined(ToUtf8(name))
                                                    : pane->location.joined(ToUtf8(name));
        } catch (const std::exception& exception) {
            Report(ExceptionText(exception));
            return;
        }

        inline_edit_mode = InlineEditMode::none;
        CloseInlineEditWindow();
        if (mode == InlineEditMode::create_file || mode == InlineEditMode::create_directory) {
            RemoveProvisionalRow(pane_id, row);
        }
        inline_edit_original.clear();
        pane = FindPane(pane_id);
        if (!pane)
            return;

        if (mode == InlineEditMode::rename) {
            RunMutation(
                *pane,
                {source},
                [target](transfer::IFileEndpoint& endpoint, const transfer::FileUri& from) {
                    endpoint.rename(from, target, false);
                },
                L"已重命名为 " + name);
        } else if (mode == InlineEditMode::create_directory) {
            RunMutation(
                *pane,
                {target},
                [](transfer::IFileEndpoint& endpoint, const transfer::FileUri& uri) {
                    endpoint.mkdir(uri, false);
                },
                L"已新建文件夹 " + name);
        } else {
            RunMutation(
                *pane,
                {target},
                [](transfer::IFileEndpoint& endpoint, const transfer::FileUri& uri) {
                    if (endpoint.exists(uri)) {
                        throw std::runtime_error("Create file target already exists");
                    }
                    auto output = endpoint.open_write(uri, 0, true);
                    output->flush();
                },
                L"已新建文件 " + name);
        }
    }

    void RenameSelected(Pane& pane) {
        const int row_index = table::SelectedRow(pane.list);
        const FileRow* selected = SelectedRow(pane);
        if (row_index < 0 || !selected || !selected->transferable || selected->name == L"..") {
            Report(L"请选择一个要重命名的文件或文件夹");
            return;
        }
        const FileRow row = *selected;
        BeginInlineEdit(
            pane, static_cast<std::size_t>(row_index), InlineEditMode::rename, row.uri, row.name);
    }

    std::wstring UniqueNewName(const Pane& pane, const std::wstring_view base) const {
        const auto exists = [&pane](const std::wstring_view candidate) {
            return std::ranges::any_of(pane.rows, [candidate, &pane](const FileRow& row) {
                return pane.local ? _wcsicmp(row.name.c_str(), std::wstring(candidate).c_str()) == 0
                                  : row.name == candidate;
            });
        };
        if (!exists(base))
            return std::wstring(base);
        for (unsigned suffix = 2; suffix < 10000; ++suffix) {
            std::wstring candidate = std::wstring(base) + L" (" + std::to_wstring(suffix) + L")";
            if (!exists(candidate))
                return candidate;
        }
        return std::wstring(base) + L"-" + std::to_wstring(GetTickCount64());
    }

    void BeginCreate(Pane& pane, const bool directory) {
        CancelInlineEdit();
        if (!pane.endpoint || pane.showing_this_pc || (pane.local && pane.location.path() == "/")) {
            Report(L"当前目录不能新建项目");
            return;
        }
        const std::wstring name = UniqueNewName(pane, directory ? L"新建文件夹" : L"新建文件");
        FileRow provisional;
        provisional.name = name;
        provisional.status = directory ? L"文件夹" : L"文件";
        provisional.directory = directory;
        provisional.transferable = false;
        provisional.uri = pane.location.joined(ToUtf8(name));
        const std::size_t row = pane.rows.size();
        pane.rows.push_back(std::move(provisional));
        table::SetRowCount(pane.list, pane.rows.size());
        table::Refresh(pane.list);
        if (!BeginInlineEdit(pane,
                             row,
                             directory ? InlineEditMode::create_directory
                                       : InlineEditMode::create_file,
                             pane.rows[row].uri,
                             name)) {
            RemoveProvisionalRow(pane.id, row);
        }
    }

    void ChangeSelectedPermissions(Pane& pane) {
        const FileRow* selected = SelectedRow(pane);
        if (!selected || !selected->transferable || selected->name == L"..") {
            Report(L"请选择一个文件或文件夹");
            return;
        }
        const FileRow row = *selected;
        const auto choice =
            ShowPermissionDialog(hwnd, row.name, row.permission_bits, row.directory);
        if (!choice) {
            return;
        }
        const std::uint32_t mode = choice->permissions;
        const bool recursive = choice->recursive;
        RunMutation(
            pane,
            {row.uri},
            [mode, recursive](transfer::IFileEndpoint& endpoint, const transfer::FileUri& uri) {
                ApplyPermissions(endpoint, uri, mode, recursive);
            },
            L"权限已修改为 " + PermissionsText(mode));
    }

    void OpenSelectedRemoteFile(Pane& pane) {
        FileRow* row = SelectedRow(pane);
        if (!row || row->directory || !row->transferable || !row->uri.is_sftp()) {
            Report(L"请选择一个远程文件");
            return;
        }
        if (!remote_edit_callback) {
            Report(L"远程编辑器尚未绑定");
            return;
        }
        remote_edit_callback(row->uri);
    }

    void OpenSelectedDirectory(Pane& pane) {
        FileRow* row = SelectedRow(pane);
        if (row && !row->transferable && pane.view_state.action != ViewRecoveryAction::None) {
            if (pane_action_callback) {
                pane_action_callback(pane.id, pane.view_state.action);
            }
            return;
        }
        if (row && row->directory && row->transferable) {
            RequestList(pane, row->uri);
            return;
        }
        if (!row || !row->local_shortcut || !row->transferable || !pane.local ||
            !row->uri.is_local()) {
            return;
        }

        const local_shortcut::Target target = local_shortcut::Resolve(FromUtf8(row->uri.path()));
        if (target.kind == local_shortcut::TargetKind::this_pc) {
            RequestList(pane, transfer::FileUri::local("/"));
        } else if (target.kind == local_shortcut::TargetKind::directory) {
            RequestList(pane, transfer::FileUri::local(ToUtf8(target.path)));
        } else {
            Report(L"该快捷方式的目标不是可进入的文件夹");
        }
    }

    void NavigateParent(Pane& pane) {
        if (pane.showing_this_pc) {
            return;
        }
        RequestList(pane, pane.location.parent());
    }

    void NavigateFromPathControl(Pane& pane) {
        const int length = GetWindowTextLengthW(pane.path_control);
        std::wstring text(static_cast<std::size_t>(std::max(0, length)) + 1, L'\0');
        GetWindowTextW(pane.path_control, text.data(), length + 1);
        text.resize(static_cast<std::size_t>(std::max(0, length)));
        try {
            if (pane.local) {
                if (text.empty() || text == L"此电脑") {
                    RequestList(pane, transfer::FileUri::local("/"));
                } else {
                    RequestList(pane, transfer::FileUri::local(ToUtf8(text)));
                }
            } else {
                RequestList(pane, pane.location.with_path(ToUtf8(text.empty() ? L"/" : text)));
            }
        } catch (const std::exception& error) {
            Report(ExceptionText(error));
            SetWindowTextW(pane.path_control, PathText(pane.location).c_str());
        }
    }

    void InitializeColumns(const HWND list) const {
        const UINT dpi = detail::WindowDpi(hwnd);
        const std::array columns{
            table::Column{L"名称", detail::ScaleForDpi(220, dpi), detail::ScaleForDpi(180, dpi)},
            table::Column{L"状态", detail::ScaleForDpi(100, dpi), detail::ScaleForDpi(78, dpi)},
            table::Column{L"大小", detail::ScaleForDpi(88, dpi), detail::ScaleForDpi(72, dpi)},
            table::Column{
                L"修改时间", detail::ScaleForDpi(132, dpi), detail::ScaleForDpi(112, dpi)},
            table::Column{L"权限", detail::ScaleForDpi(92, dpi), detail::ScaleForDpi(80, dpi)},
            table::Column{L"所有者", detail::ScaleForDpi(110, dpi), detail::ScaleForDpi(90, dpi)}};
        table::SetColumns(list, columns);
    }

    Pane MakePane(const TransferPaneId id,
                  const bool local,
                  std::wstring title,
                  transfer::FileUri location) {
        Pane pane;
        pane.id = id;
        pane.local = local;
        pane.title = std::move(title);
        pane.location = std::move(location);
        pane.history.Reset(pane.location);
        pane.container = CreateWindowExW(WS_EX_CONTROLPARENT,
                                         kTransferPaneClass,
                                         L"",
                                         WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
                                         0,
                                         0,
                                         0,
                                         0,
                                         hwnd,
                                         nullptr,
                                         instance,
                                         nullptr);
        pane.title_control = CreateWindowExW(0,
                                             L"STATIC",
                                             pane.title.c_str(),
                                             WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
                                             0,
                                             0,
                                             0,
                                             0,
                                             pane.container,
                                             nullptr,
                                             instance,
                                             nullptr);
        pane.path_frame = CreateWindowExW(0,
                                          kTransferPathFrameClass,
                                          L"",
                                          WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
                                          0,
                                          0,
                                          0,
                                          0,
                                          pane.container,
                                          nullptr,
                                          instance,
                                          nullptr);
        pane.path_control = CreateWindowExW(0,
                                            L"EDIT",
                                            PathText(pane.location).c_str(),
                                            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                            0,
                                            0,
                                            0,
                                            0,
                                            pane.path_frame,
                                            nullptr,
                                            instance,
                                            nullptr);
        SendMessageW(pane.path_control,
                     EM_SETMARGINS,
                     EC_LEFTMARGIN | EC_RIGHTMARGIN,
                     MAKELPARAM(detail::ScaleForDpi(6, detail::WindowDpi(hwnd)),
                                detail::ScaleForDpi(6, detail::WindowDpi(hwnd))));
        pane.refresh_button =
            CreateWindowExW(0,
                            L"STATIC",
                            L"↻",
                            WS_CHILD | WS_VISIBLE | SS_NOTIFY | SS_CENTER | SS_CENTERIMAGE,
                            0,
                            0,
                            0,
                            0,
                            pane.container,
                            nullptr,
                            instance,
                            nullptr);
        pane.hidden_checkbox = CreateWindowExW(
            0,
            L"BUTTON",
            L"显示隐藏文件",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0,
            0,
            0,
            0,
            pane.container,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPaneHiddenBaseId + panes.size())),
            instance,
            nullptr);
        detail::SetSelfDrawnCheckboxChecked(pane.hidden_checkbox, pane.show_hidden);
        if (!local) {
            pane.close_button = CreateWindowExW(0,
                                                L"BUTTON",
                                                L"×",
                                                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                                0,
                                                0,
                                                0,
                                                0,
                                                pane.container,
                                                nullptr,
                                                instance,
                                                nullptr);
        }
        // 行内重命名 EDIT 会与文件表格短暂重叠。WS_CLIPSIBLINGS 保证表格随后
        // 处理选择高亮或滚动重绘时，不会覆盖位于其上方的输入框。
        pane.list = table::Create(instance,
                                  pane.container,
                                  kPaneListBaseId + static_cast<int>(panes.size()),
                                  false,
                                  WS_CLIPSIBLINGS);
        table::SetMarqueeSelection(pane.list, true);
        if (file_icons) {
            table::SetImageList(pane.list, file_icons);
        }
        InitializeColumns(pane.list);
        table::SetColumnFont(pane.list, 0, file_name_font);
        table::SetTextProvider(pane.list,
                               [this, id](const std::size_t row, const std::size_t column) {
                                   return PaneCellText(id, row, column);
                               });
        table::SetImageProvider(pane.list, [this, id](const std::size_t row) {
            Pane* current = FindPane(id);
            return current && row < current->rows.size() ? FileIconFor(current->rows[row]) : -1;
        });
        table::SetHeaderClickHandler(pane.list, [this, id](const std::size_t column) {
            if (column != 0 && column != 3) {
                return;
            }
            Pane* current = FindPane(id);
            if (!current) {
                return;
            }
            if (current->sort_column == column) {
                current->sort_ascending = !current->sort_ascending;
            } else {
                current->sort_column = column;
                // Newest files first is the useful initial direction for
                // modification time; names initially use A-to-Z.
                current->sort_ascending = column == 0;
            }
            SortPaneRows(*current, true);
        });
        table::SetSortIndicator(pane.list, pane.sort_column, pane.sort_ascending);
        SetWindowSubclass(pane.list, &ControlSubclassProc, 1, reinterpret_cast<DWORD_PTR>(this));
        SetWindowSubclass(
            pane.path_control, &ControlSubclassProc, 2, reinterpret_cast<DWORD_PTR>(this));
        SetWindowSubclass(
            pane.refresh_button, &ControlSubclassProc, 4, reinterpret_cast<DWORD_PTR>(this));
        DragAcceptFiles(pane.list, TRUE);
        for (HWND control : {pane.container,
                             pane.title_control,
                             pane.up_button,
                             pane.path_frame,
                             pane.path_control,
                             pane.hidden_checkbox,
                             pane.refresh_button,
                             pane.close_button}) {
            if (control) {
                detail::ApplySelfDrawnTheme(control);
                detail::SetControlFont(control, font);
            }
        }
        detail::SetControlFont(pane.list, font);
        table::ApplyTheme(pane.list);
        detail::SetControlFont(pane.title_control, title_font);
        detail::SetControlFont(pane.path_control, path_font);
        ShowWindow(pane.title_control, SW_HIDE);
        if (pane.close_button) {
            ShowWindow(pane.close_button, SW_HIDE);
        }
        return pane;
    }

    void SetRows(Pane& pane, std::vector<FileRow> rows) {
        if (inline_name_edit && inline_edit_pane_id == pane.id) {
            CancelInlineEdit();
        }
        SendMessageW(pane.list, WM_SETREDRAW, FALSE, 0);
        const auto previous_count = pane.rows.size();
        std::vector<FileRow> retired = std::move(pane.rows);
        pane.rows = std::move(rows);
        SortPaneRows(pane, false);
        if (pane.rows.size() != previous_count) {
            table::SetRowCount(pane.list, pane.rows.size());
        }
        SendMessageW(pane.list, WM_SETREDRAW, TRUE, 0);
        table::Refresh(pane.list);
        ReleaseRowsAsync(std::move(retired));
    }

    void ReleaseRowsAsync(std::vector<FileRow> rows) {
        if (rows.size() < 256) {
            return;
        }
        try {
            (void)background_executor.Submit([rows = std::move(rows)]() mutable {
                std::vector<FileRow>().swap(rows);
            });
        } catch (...) {
            // Falling back to destruction on this thread is safe.
        }
    }

    void PopulateThisPc(Pane& pane) {
        pane.loading = false;
        pane.showing_this_pc = true;
        pane.location = transfer::FileUri::local("/");
        SetWindowTextW(pane.path_control, L"此电脑");
        std::vector<FileRow> rows;
        wchar_t drives[512]{};
        const DWORD length = GetLogicalDriveStringsW(static_cast<DWORD>(std::size(drives)), drives);
        const wchar_t* drive = drives;
        while (drive < drives + length && *drive) {
            FileRow row;
            row.name = drive;
            row.status = L"本地磁盘";
            row.directory = true;
            row.uri = transfer::FileUri::local(ToUtf8(drive));
            rows.push_back(std::move(row));
            drive += wcslen(drive) + 1;
        }
        SetRows(pane, std::move(rows));
        if (pane_navigated_callback) {
            pane_navigated_callback(pane.id, pane.location);
        }
    }

    void PopulateUnboundRemote(Pane& pane) {
        FileRow row;
        row.name =
            pane.view_state.message.empty() ? L"等待 SFTP 文件会话" : pane.view_state.message;
        row.status = pane.view_state.action == ViewRecoveryAction::Retry
                         ? L"双击重试"
                         : (pane.view_state.action == ViewRecoveryAction::ManageServer ? L"双击管理"
                                                                                       : L"未连接");
        row.transferable = false;
        SetRows(pane, {std::move(row)});
    }

    void ShowLoading(Pane& pane) {
        pane.view_state = {ViewPhase::Loading, L"正在加载目录…", ViewRecoveryAction::None};
        FileRow row;
        row.name = L"正在加载目录…";
        row.status = L"读取中";
        row.transferable = false;
        SetRows(pane, {std::move(row)});
    }

    void SortPaneRows(Pane& pane, const bool preserve_selection) {
        std::unordered_set<std::string> selected_uris;
        if (preserve_selection) {
            for (const int row : table::SelectedRows(pane.list)) {
                if (row >= 0 && static_cast<std::size_t>(row) < pane.rows.size()) {
                    selected_uris.insert(pane.rows[static_cast<std::size_t>(row)].uri.to_string());
                }
            }
        }

        detail::SortFileRows(pane.rows, pane.sort_column, pane.sort_ascending);

        table::SetSortIndicator(pane.list, pane.sort_column, pane.sort_ascending);
        if (!preserve_selection) {
            return;
        }
        table::ClearSelection(pane.list);
        bool first = true;
        for (std::size_t row = 0; row < pane.rows.size(); ++row) {
            if (!selected_uris.contains(pane.rows[row].uri.to_string())) {
                continue;
            }
            table::SelectRow(pane.list, static_cast<int>(row), first);
            first = false;
        }
        table::Refresh(pane.list);
    }

    static void PostDirectoryResult(const std::weak_ptr<AsyncLifetime>& weak,
                                    std::unique_ptr<DirectoryResult> result) {
        const auto lifetime = weak.lock();
        if (!lifetime) {
            return;
        }
        std::scoped_lock lock(lifetime->mutex);
        if (!lifetime->alive || !lifetime->window) {
            return;
        }
        DirectoryResult* raw = result.release();
        if (!PostMessageW(
                lifetime->window, kDirectoryResultMessage, 0, reinterpret_cast<LPARAM>(raw))) {
            delete raw;
        }
    }

    static void PostMutationResult(const std::weak_ptr<AsyncLifetime>& weak,
                                   std::unique_ptr<MutationResult> result) {
        const auto lifetime = weak.lock();
        if (!lifetime) {
            return;
        }
        std::scoped_lock lock(lifetime->mutex);
        if (!lifetime->alive || !lifetime->window) {
            return;
        }
        MutationResult* raw = result.release();
        if (!PostMessageW(
                lifetime->window, kMutationResultMessage, 0, reinterpret_cast<LPARAM>(raw))) {
            delete raw;
        }
    }

    static std::shared_ptr<AsyncLifetime> BeginAsyncWork(const std::weak_ptr<AsyncLifetime>& weak) {
        const auto lifetime = weak.lock();
        if (!lifetime) {
            return {};
        }
        std::scoped_lock lock(lifetime->mutex);
        if (!lifetime->alive) {
            return {};
        }
        ++lifetime->active_jobs;
        return lifetime;
    }

    static void EndAsyncWork(const std::shared_ptr<AsyncLifetime>& lifetime) {
        if (!lifetime) {
            return;
        }
        std::scoped_lock lock(lifetime->mutex);
        if (lifetime->active_jobs != 0) {
            --lifetime->active_jobs;
        }
        if (lifetime->active_jobs == 0) {
            lifetime->idle.notify_all();
        }
    }

    void
    RunMutation(Pane& pane,
                std::vector<transfer::FileUri> uris,
                std::function<void(transfer::IFileEndpoint&, const transfer::FileUri&)> operation,
                std::wstring success) {
        if (!pane.endpoint || uris.empty()) {
            Report(L"没有可操作的文件");
            return;
        }
        const auto endpoint = pane.endpoint;
        const TransferPaneId pane_id = pane.id;
        const std::weak_ptr<AsyncLifetime> weak = async_lifetime;
        const auto work = BeginAsyncWork(weak);
        if (!work) {
            return;
        }
        try {
            const bool submitted =
                background_executor.Submit([weak,
                                            work,
                                            endpoint,
                                            pane_id,
                                            uris = std::move(uris),
                                            operation = std::move(operation),
                                            success = std::move(success)]() mutable {
                    auto result = std::make_unique<MutationResult>();
                    result->pane_id = pane_id;
                    result->success = std::move(success);
                    try {
                        for (const auto& uri : uris) {
                            operation(*endpoint, uri);
                        }
                    } catch (const std::exception& error) {
                        result->error = ExceptionText(error);
                    } catch (...) {
                        result->error = L"文件操作发生未知错误";
                    }
                    PostMutationResult(weak, std::move(result));
                    EndAsyncWork(work);
                });
            if (!submitted) {
                EndAsyncWork(work);
                Report(L"文件操作后台执行器已停止");
            }
        } catch (const std::exception& error) {
            EndAsyncWork(work);
            Report(ExceptionText(error));
        } catch (...) {
            EndAsyncWork(work);
            Report(L"无法启动文件操作任务");
        }
    }

    void NavigateHistory(Pane& pane, const bool forward) {
        pane.history.Navigate(forward, [this, &pane](const transfer::FileUri& target) {
            return RequestList(pane, target);
        });
    }

    bool RequestList(Pane& pane, transfer::FileUri location) {
        // 步骤 1：处理无需 I/O 的特殊入口，并校验端点/URI 匹配关系。重复刷新同一正在
        // 加载的目录直接复用当前请求，避免连续点击刷新造成任务排队。
        if (!hwnd) {
            return false;
        }
        if (pane.loading && pane.location == location) {
            return true;
        }
        if (pane.local && location.is_local() && location.path() == "/") {
            pane.history.Record(pane.location, location);
            ++pane.load_generation;
            PopulateThisPc(pane);
            return true;
        }
        if (!pane.endpoint) {
            PopulateUnboundRemote(pane);
            Report(L"该面板尚未绑定文件端点");
            return false;
        }
        if (!pane.endpoint->accepts(location)) {
            Report(L"文件端点与目标路径不匹配");
            return false;
        }

        // 步骤 2：先更新路径显示和 generation，再启动异步枚举。旧请求即使稍后完成，
        // 应用结果时也会因 generation 不匹配被丢弃。
        const bool location_changed = pane.location != location;
        pane.history.Record(pane.location, location);
        pane.location = location;
        pane.showing_this_pc = false;
        pane.loading = true;
        pane.view_state = {ViewPhase::Loading, L"正在加载目录…", ViewRecoveryAction::None};
        const std::uint64_t generation = ++pane.load_generation;
        SetWindowTextW(pane.path_control, PathText(location).c_str());
        if (pane.rows.empty() || location_changed) {
            ShowLoading(pane);
        }

        const TransferPaneId pane_id = pane.id;
        const bool show_hidden = pane.show_hidden;
        const auto endpoint = pane.endpoint;
        const std::weak_ptr<AsyncLifetime> weak = async_lifetime;
        // 步骤 3：枚举完成后仍不直接修改 HWND；先在共享后台池整理、排序和转换行快照，
        // 再通过窗口消息交回 UI，避免十万项目录处理卡住绘制。
        DirectoryListCompletion completion =
            [this, weak, pane_id, generation, location, show_hidden](
                std::vector<transfer::DirectoryEntry> entries, std::wstring error) mutable {
                const auto work = BeginAsyncWork(weak);
                if (!work) {
                    return;
                }
                try {
                    const bool submitted =
                        background_executor.Submit([weak,
                                                    work,
                                                    pane_id,
                                                    generation,
                                                    location,
                                                    show_hidden,
                                                    entries = std::move(entries),
                                                    error = std::move(error)]() mutable {
                            auto result = std::make_unique<DirectoryResult>();
                            result->pane_id = pane_id;
                            result->generation = generation;
                            result->location = location;
                            result->error = std::move(error);
                            if (result->error.empty()) {
                                try {
                                    result->rows = detail::PrepareDirectoryRows(
                                        location, std::move(entries), show_hidden);
                                } catch (const std::exception& prepare_error) {
                                    result->error = ExceptionText(prepare_error);
                                } catch (...) {
                                    result->error = L"整理目录结果时发生未知错误";
                                }
                            }
                            PostDirectoryResult(weak, std::move(result));
                            EndAsyncWork(work);
                        });
                    if (!submitted) {
                        EndAsyncWork(work);
                    }
                } catch (const std::exception& thread_error) {
                    auto result = std::make_unique<DirectoryResult>();
                    result->pane_id = pane_id;
                    result->generation = generation;
                    result->location = location;
                    result->error = ExceptionText(thread_error);
                    PostDirectoryResult(weak, std::move(result));
                    EndAsyncWork(work);
                } catch (...) {
                    auto result = std::make_unique<DirectoryResult>();
                    result->pane_id = pane_id;
                    result->generation = generation;
                    result->location = location;
                    result->error = L"无法启动目录结果处理任务";
                    PostDirectoryResult(weak, std::move(result));
                    EndAsyncWork(work);
                }
            };

        // 步骤 4：SFTP 必须交给应用安装的 Reactor 异步枚举器；本地目录才允许使用页面
        // 工作池直接调用 endpoint->list，确保 libssh2 句柄不会逃离所属线程。
        if (async_directory_list && location.is_sftp()) {
            try {
                async_directory_list(pane_id, endpoint, location, completion);
            } catch (const std::exception& error) {
                completion({}, ExceptionText(error));
            } catch (...) {
                completion({}, L"目录加载调度失败");
            }
            return true;
        }

        if (location.is_sftp()) {
            completion({}, L"SFTP 目录需要绑定 NetworkReactor 异步目录加载器");
            return false;
        }

        const auto work = BeginAsyncWork(weak);
        if (!work) {
            pane.loading = false;
            return false;
        }
        try {
            const bool submitted = background_executor.Submit(
                [weak, endpoint, pane_id, generation, location, show_hidden, work]() mutable {
                    auto result = std::make_unique<DirectoryResult>();
                    result->pane_id = pane_id;
                    result->generation = generation;
                    result->location = location;
                    try {
                        result->rows = detail::PrepareDirectoryRows(
                            location, endpoint->list(location), show_hidden);
                    } catch (const std::exception& error) {
                        result->error = ExceptionText(error);
                    } catch (...) {
                        result->error = L"读取目录时发生未知错误";
                    }
                    PostDirectoryResult(weak, std::move(result));
                    EndAsyncWork(work);
                });
            if (!submitted) {
                EndAsyncWork(work);
                pane.loading = false;
                Report(L"目录后台执行器已停止");
                return false;
            }
        } catch (...) {
            EndAsyncWork(work);
            pane.loading = false;
            Report(L"无法启动本地目录加载任务");
            return false;
        }
        return true;
    }

    void RefreshPaneNow(const TransferPaneId pane_id) {
        Pane* pane = FindPane(pane_id);
        if (!pane || pane->loading) {
            return;
        }
        RequestList(*pane, pane->location);
    }

    void ApplyDirectoryResult(std::unique_ptr<DirectoryResult> result) {
        if (!result) {
            return;
        }
        Pane* pane = FindPane(result->pane_id);
        if (!pane || result->generation != pane->load_generation) {
            ReleaseRowsAsync(std::move(result->rows));
            return;
        }
        pane->loading = false;
        if (!result->error.empty()) {
            pane->view_state = {ViewPhase::Error, result->error, ViewRecoveryAction::Retry};
            FileRow error;
            error.name = result->error;
            error.status = L"双击重试";
            error.transferable = false;
            SetRows(*pane, {std::move(error)});
            return;
        }

        const bool empty_directory = result->rows.empty() || (result->rows.size() == 1 &&
                                                              result->rows.front().name == L"..");
        pane->view_state = {empty_directory ? ViewPhase::Empty : ViewPhase::Ready,
                            empty_directory ? L"文件夹为空" : L"",
                            ViewRecoveryAction::None};
        if (empty_directory) {
            FileRow empty;
            empty.name = L"文件夹为空";
            empty.transferable = false;
            result->rows.push_back(std::move(empty));
        }

        const bool same_rows = detail::SameFileRows(pane->rows, result->rows);
        if (!same_rows) {
            SetRows(*pane, std::move(result->rows));
        }
        if (pane_navigated_callback) {
            pane_navigated_callback(pane->id, pane->location);
        }
    }

    void ApplyMutationResult(std::unique_ptr<MutationResult> result) {
        if (!result) {
            return;
        }
        if (!result->error.empty()) {
            Report(L"文件操作失败：" + result->error);
            return;
        }
        if (!result->success.empty()) {
            Report(result->success);
        }
        if (Pane* pane = FindPane(result->pane_id)) {
            RefreshPaneNow(pane->id);
        }
    }

    TransferPaneId AppendRemote(std::wstring title,
                                std::string server_id,
                                std::string path,
                                std::shared_ptr<transfer::IFileEndpoint> endpoint) {
        const TransferPaneId id = next_id++;
        transfer::FileUri location =
            transfer::FileUri::sftp(std::move(server_id), path.empty() ? "/" : std::move(path));
        panes.push_back(MakePane(id, false, std::move(title), location));
        const std::size_t added = panes.size() - 1;
        const unsigned target_group = split_active && active_pane_index < pane_groups.size()
                                          ? GroupOf(active_pane_index)
                                          : 0U;
        pane_groups.push_back(target_group);
        active_pane_index = added;
        active_pane_id = id;
        group_active[target_group] = added;
        panes.back().endpoint = std::move(endpoint);
        if (panes.back().endpoint) {
            RequestList(panes.back(), location);
        } else {
            PopulateUnboundRemote(panes.back());
        }
        NormalizeGroups();
        Relayout();
        return id;
    }

    TransferPaneId AppendLocal(std::wstring title) {
        for (std::size_t index = 0; index < panes.size(); ++index) {
            if (!panes[index].local)
                continue;
            const transfer::FileUri desktop = DesktopLocation();
            RequestList(panes[index], desktop);
            FocusPaneIndex(index);
            return panes[index].id;
        }
        const TransferPaneId id = next_id++;
        const transfer::FileUri desktop = DesktopLocation();
        panes.push_back(MakePane(id, true, std::move(title), desktop));
        const std::size_t added = panes.size() - 1;
        const unsigned target_group = split_active && active_pane_index < pane_groups.size()
                                          ? GroupOf(active_pane_index)
                                          : 0U;
        pane_groups.push_back(target_group);
        active_pane_index = added;
        active_pane_id = id;
        group_active[target_group] = added;
        panes.back().endpoint = std::make_shared<transfer::LocalEndpoint>();
        if (desktop.path() == "/") {
            PopulateThisPc(panes.back());
        } else {
            RequestList(panes.back(), desktop);
        }
        NormalizeGroups();
        Relayout();
        return id;
    }

    bool RemovePane(const TransferPaneId id) {
        if (inline_name_edit && inline_edit_pane_id == id) {
            CancelInlineEdit();
        }
        const auto it = std::find_if(panes.begin(), panes.end(), [id](const Pane& pane) {
            return pane.id == id;
        });
        if (it == panes.end() || it->local) {
            return false;
        }
        const std::size_t index = static_cast<std::size_t>(it - panes.begin());
        const bool was_split = split_active;
        const unsigned closing_group = GroupOf(index);
        const TransferPaneId previous_active_id = active_pane_id;
        DestroyWindow(it->container);
        panes.erase(it);
        if (index < pane_groups.size()) {
            pane_groups.erase(pane_groups.begin() + static_cast<std::ptrdiff_t>(index));
        }
        for (std::size_t& selected : group_active) {
            if (selected == index) {
                selected = panes.size();
            } else if (selected > index) {
                --selected;
            }
        }
        if (panes.empty()) {
            active_pane_index = 0;
            active_pane_id = 0;
            NormalizeGroups();
            Relayout();
            if (pane_closed_callback) {
                pane_closed_callback(id);
            }
            return true;
        }
        const auto previous =
            std::find_if(panes.begin(), panes.end(), [previous_active_id](const Pane& pane) {
                return pane.id == previous_active_id;
            });
        if (previous != panes.end()) {
            active_pane_index = static_cast<std::size_t>(previous - panes.begin());
        } else if (was_split) {
            const auto same_group = FirstTabInGroup(closing_group);
            const unsigned surviving_group = same_group ? closing_group : 1U - closing_group;
            const std::size_t selected = group_active[surviving_group];
            active_pane_index = selected < panes.size() && GroupOf(selected) == surviving_group
                                    ? selected
                                    : FirstTabInGroup(surviving_group).value_or(0);
        } else {
            active_pane_index = std::min(index, panes.size() - 1);
        }
        NormalizeGroups();
        Relayout();
        if (pane_closed_callback) {
            pane_closed_callback(id);
        }
        return true;
    }

    QueueDetailUpdate UpdateQueueDetail(const transfer::TransferJob& job) {
        QueueDetailUpdate update;
        if (!transfer_manager || !expanded_queue_jobs.contains(job.id) ||
            !job.source_is_directory) {
            return update;
        }
        QueueDetailCache& cache = queue_details[job.id];
        const auto snapshot = transfer_manager->details(job.id, cache.revision);
        cache.revision = snapshot.revision;
        if (snapshot.reset) {
            update.topology_changed = true;
            cache.items = snapshot.items;
            cache.indexes.clear();
            cache.indexes.reserve(cache.items.size());
            for (std::size_t index = 0; index < cache.items.size(); ++index) {
                cache.indexes[cache.items[index].id] = index;
            }
            auto& speeds = queue_item_speeds[job.id];
            std::erase_if(speeds, [&cache](const auto& sample) {
                return !cache.indexes.contains(sample.first);
            });
            return update;
        }
        update.changed_items.reserve(snapshot.items.size());
        for (const auto& item : snapshot.items) {
            update.changed_items.push_back(item.id);
            const auto found = cache.indexes.find(item.id);
            if (found == cache.indexes.end()) {
                update.topology_changed = true;
                cache.indexes[item.id] = cache.items.size();
                cache.items.push_back(item);
            } else {
                const auto& previous = cache.items[found->second];
                if (previous.parent_id != item.parent_id || previous.directory != item.directory) {
                    update.topology_changed = true;
                }
                cache.items[found->second] = item;
            }
        }
        return update;
    }

    void BuildQueueRows() {
        auto tree = detail::BuildTransferQueueTree(
            queue_jobs,
            [this](transfer::TransferId job_id) -> std::span<const transfer::TransferItemProgress> {
                const auto found = queue_details.find(job_id);
                return found == queue_details.end()
                           ? std::span<const transfer::TransferItemProgress>{}
                           : std::span<const transfer::TransferItemProgress>(found->second.items);
            },
            expanded_queue_jobs,
            expanded_queue_items);
        queue_rows = std::move(tree.rows);
        queue_job_rows = std::move(tree.job_rows);
        queue_visible_item_rows = std::move(tree.visible_item_rows);
        queue_running_visible_items = std::move(tree.running_visible_items);
    }

    bool ToggleQueueExpansion(const std::size_t visible_row) {
        if (visible_row >= queue_rows.size())
            return false;
        const QueueRow row = queue_rows[visible_row];
        if (row.job_index >= queue_jobs.size())
            return false;
        const auto& job = queue_jobs[row.job_index];
        if (row.is_job()) {
            if (!job.source_is_directory)
                return false;
            if (!expanded_queue_jobs.erase(job.id)) {
                expanded_queue_jobs.insert(job.id);
            }
        } else {
            const auto* item = QueueItem(row);
            if (!item || !item->directory)
                return false;
            const std::string key = QueueItemKey(job.id, row.item_id);
            if (!expanded_queue_items.erase(key)) {
                expanded_queue_items.insert(key);
            }
        }
        queue_layout_dirty = true;
        RefreshQueue();
        return true;
    }

    void RefreshQueue() {
        core::PerfScope timing("transfer_queue_refresh");
        if (!queue_list) {
            return;
        }
        if (!transfer_manager) {
            queue_jobs.clear();
            queue_rows.clear();
            queue_details.clear();
            queue_job_rows.clear();
            queue_visible_item_rows.clear();
            queue_running_visible_items.clear();
            if (table::RowCount(queue_list) != 0) {
                table::SetRowCount(queue_list, 0);
            }
            queue_layout_dirty = true;
            return;
        }
        std::vector<transfer::TransferJob> previous_jobs = std::move(queue_jobs);
        try {
            queue_jobs = transfer_manager->jobs();
        } catch (const std::exception& error) {
            queue_jobs = std::move(previous_jobs);
            Report(ExceptionText(error));
            return;
        }
        bool structure_changed = queue_layout_dirty || queue_jobs.size() != previous_jobs.size();
        if (!structure_changed) {
            for (std::size_t index = 0; index < queue_jobs.size(); ++index) {
                if (queue_jobs[index].id != previous_jobs[index].id ||
                    queue_jobs[index].source_is_directory !=
                        previous_jobs[index].source_is_directory) {
                    structure_changed = true;
                    break;
                }
            }
        }
        std::vector<std::pair<transfer::TransferId, transfer::TransferItemId>> changed_items;
        std::unordered_set<transfer::TransferId> current_ids;
        current_ids.reserve(queue_jobs.size());
        for (const auto& job : queue_jobs) {
            current_ids.insert(job.id);
            try {
                auto update = UpdateQueueDetail(job);
                structure_changed |= update.topology_changed;
                for (const auto id : update.changed_items) {
                    changed_items.emplace_back(job.id, id);
                }
            } catch (const std::exception&) {
                // A terminal entry can be cleared between jobs() and the
                // detail query. The next timer tick rebuilds the view.
            }
        }
        std::erase_if(queue_details, [&current_ids](const auto& item) {
            return !current_ids.contains(item.first);
        });
        std::erase_if(expanded_queue_jobs, [&current_ids](const auto id) {
            return !current_ids.contains(id);
        });
        std::erase_if(queue_job_speeds, [&current_ids](const auto& item) {
            return !current_ids.contains(item.first);
        });
        std::erase_if(queue_item_speeds, [&current_ids](const auto& item) {
            return !current_ids.contains(item.first);
        });
        std::erase_if(queue_running_visible_items, [&current_ids](const auto& item) {
            return !current_ids.contains(item.first);
        });
        if (structure_changed) {
            BuildQueueRows();
            queue_layout_dirty = false;
        } else {
            for (const auto [job_id, item_id] : changed_items) {
                const auto visible = queue_visible_item_rows.find(job_id);
                const auto details = queue_details.find(job_id);
                if (visible == queue_visible_item_rows.end() ||
                    !visible->second.contains(item_id) || details == queue_details.end()) {
                    continue;
                }
                const auto index = details->second.indexes.find(item_id);
                if (index != details->second.indexes.end() &&
                    details->second.items[index->second].state ==
                        transfer::TransferState::Running) {
                    queue_running_visible_items[job_id].insert(item_id);
                } else {
                    queue_running_visible_items[job_id].erase(item_id);
                    queue_item_speeds[job_id].erase(item_id);
                }
            }
        }
        UpdateQueueSpeeds();
        if (structure_changed) {
            if (table::RowCount(queue_list) != queue_rows.size()) {
                table::SetRowCount(queue_list, queue_rows.size());
            } else {
                table::Refresh(queue_list);
            }
        } else {
            std::vector<std::size_t> dirty_rows;
            dirty_rows.reserve(queue_jobs.size() + changed_items.size() + 8);
            for (const auto& job : queue_jobs) {
                if (const auto found = queue_job_rows.find(job.id); found != queue_job_rows.end()) {
                    dirty_rows.push_back(found->second);
                }
            }
            for (const auto [job_id, item_id] : changed_items) {
                const auto job_rows = queue_visible_item_rows.find(job_id);
                if (job_rows == queue_visible_item_rows.end()) {
                    continue;
                }
                if (const auto found = job_rows->second.find(item_id);
                    found != job_rows->second.end()) {
                    dirty_rows.push_back(found->second);
                }
            }
            table::RefreshRows(queue_list, dirty_rows);
        }

        for (const auto& job : queue_jobs) {
            const bool terminal = job.state == transfer::TransferState::Completed ||
                                  job.state == transfer::TransferState::Failed ||
                                  job.state == transfer::TransferState::Cancelled;
            if (!terminal) {
                observed_terminal_jobs.erase(job.id);
                continue;
            }
            const bool first_observation = observed_terminal_jobs.insert(job.id).second;
            if (!first_observation || job.state != transfer::TransferState::Completed) {
                continue;
            }
            const transfer::FileUri target =
                job.effective_target.filename().empty() ? job.spec.target : job.effective_target;
            for (Pane& pane : panes) {
                if (!pane.loading && pane.location == target.parent()) {
                    RequestList(pane, pane.location);
                }
            }
        }
    }

    void Relayout() {
        if (!hwnd) {
            return;
        }
        CommitLayoutNow();
    }

    void ScheduleLayout(const int width, const int height) {
        pending_layout_size = {width, height};
        if (layout_timer_pending) {
            return;
        }
        if (SetTimer(hwnd, kLayoutTimerId, kLayoutIntervalMs, nullptr) != 0) {
            layout_timer_pending = true;
            return;
        }
        surface.Resize(width, height);
        Layout(width, height);
    }

    void CommitScheduledLayout() {
        KillTimer(hwnd, kLayoutTimerId);
        layout_timer_pending = false;
        surface.Resize(pending_layout_size.cx, pending_layout_size.cy);
        Layout(pending_layout_size.cx, pending_layout_size.cy);
    }

    void CommitLayoutNow() {
        if (layout_timer_pending) {
            KillTimer(hwnd, kLayoutTimerId);
            layout_timer_pending = false;
        }
        RECT rect{};
        GetClientRect(hwnd, &rect);
        pending_layout_size = {rect.right, rect.bottom};
        surface.Resize(rect.right, rect.bottom);
        Layout(rect.right, rect.bottom);
    }

    void LayoutPane(Pane& pane, const int width, const int height) const {
        const UINT dpi = detail::WindowDpi(hwnd);
        const int pad = detail::ScaleForDpi(10, dpi);
        const int path_y = detail::ScaleForDpi(5, dpi);
        const int inner_gap = detail::ScaleForDpi(6, dpi);
        const int path_height = detail::ScaleForDpi(22, dpi);
        const int refresh_size = path_height;
        const int refresh_x = std::max(pad, width - pad - refresh_size);
        MoveWindow(pane.refresh_button, refresh_x, path_y, refresh_size, refresh_size, FALSE);
        const int hidden_width = detail::ScaleForDpi(116, dpi);
        const int hidden_x = std::max(pad, refresh_x - inner_gap - hidden_width);
        MoveWindow(pane.hidden_checkbox, hidden_x, path_y, hidden_width, path_height, FALSE);
        const int path_x = pad;
        const int path_width = std::max(0, hidden_x - inner_gap - path_x);
        const int border = std::max(1, detail::ScaleForDpi(1, dpi));
        MoveWindow(pane.path_frame, path_x, path_y, path_width, path_height, FALSE);
        if (pane.path_frame_region.cx != path_width || pane.path_frame_region.cy != path_height) {
            detail::SetRoundedControlRegion(
                pane.path_frame, path_width, path_height, detail::ScaleForDpi(7, dpi));
            pane.path_frame_region = {path_width, path_height};
        }
        const int edit_width = std::max(1, path_width - border * 2);
        const int edit_height = std::max(1, path_height - border * 2);
        MoveWindow(pane.path_control, border, border, edit_width, edit_height, FALSE);
        if (pane.path_edit_region.cx != edit_width || pane.path_edit_region.cy != edit_height) {
            detail::SetRoundedControlRegion(
                pane.path_control, edit_width, edit_height, detail::ScaleForDpi(5, dpi));
            pane.path_edit_region = {edit_width, edit_height};
        }
        const int list_y = path_y + path_height + inner_gap;
        MoveWindow(pane.list,
                   pad,
                   list_y,
                   std::max(0, width - pad * 2),
                   std::max(0, height - list_y - pad),
                   FALSE);

        // The header can still report its previous width while the pane is
        // being resized. Base the columns on the list client instead so the
        // visible columns always consume the complete row and no trailing
        // empty column divider remains.
        RECT list_client{};
        GetClientRect(pane.list, &list_client);
        const int list_width = std::max(1, static_cast<int>(list_client.right));
        // Keep every column present.  Columns shrink together until their
        // minimum readable widths are reached; below that point their total
        // width deliberately exceeds the client area so the ListView exposes
        // horizontal scrolling instead of hiding or overlapping headers.
        const std::array<int, 6> minimum_widths = {detail::ScaleForDpi(180, dpi),
                                                   detail::ScaleForDpi(78, dpi),
                                                   detail::ScaleForDpi(72, dpi),
                                                   detail::ScaleForDpi(112, dpi),
                                                   detail::ScaleForDpi(80, dpi),
                                                   detail::ScaleForDpi(90, dpi)};
        const std::array<int, 6> preferred_widths = {std::clamp(list_width * 43 / 100,
                                                                detail::ScaleForDpi(220, dpi),
                                                                detail::ScaleForDpi(520, dpi)),
                                                     detail::ScaleForDpi(110, dpi),
                                                     detail::ScaleForDpi(88, dpi),
                                                     detail::ScaleForDpi(132, dpi),
                                                     detail::ScaleForDpi(92, dpi),
                                                     detail::ScaleForDpi(110, dpi)};
        std::array<int, 6> desired_widths = minimum_widths;
        const int minimum_total = std::accumulate(minimum_widths.begin(), minimum_widths.end(), 0);
        const int preferred_total =
            std::accumulate(preferred_widths.begin(), preferred_widths.end(), 0);
        if (list_width > minimum_total) {
            const int available_growth = list_width - minimum_total;
            const int preferred_growth = preferred_total - minimum_total;
            if (preferred_growth > 0) {
                const double ratio = std::min(1.0,
                                              static_cast<double>(available_growth) /
                                                  static_cast<double>(preferred_growth));
                for (std::size_t column = 0; column < desired_widths.size(); ++column) {
                    const int growth = preferred_widths[column] - minimum_widths[column];
                    desired_widths[column] += static_cast<int>(std::lround(growth * ratio));
                }
            }
            const int used_width = std::accumulate(desired_widths.begin(), desired_widths.end(), 0);
            // The final owner column absorbs rounding and any room beyond the
            // preferred layout, keeping the row flush with the pane edge.
            desired_widths.back() += std::max(0, list_width - used_width);
        }
        if (pane.column_widths != desired_widths) {
            table::SetColumnWidths(pane.list, desired_widths);
            pane.column_widths = desired_widths;
        }
    }

    void BuildSurface(const int width,
                      const int content_left,
                      const int queue_y,
                      const int queue_toolbar_height,
                      const int concurrency_label_x,
                      const int concurrency_label_width) {
        detail::TransferSurfaceLayout layout;
        layout.dpi = detail::WindowDpi(hwnd);
        layout.width = width;
        layout.content_left = content_left;
        layout.queue_y = queue_y;
        layout.queue_toolbar_height = queue_toolbar_height;
        layout.queue_splitter_y = queue_splitter_y;
        layout.concurrency_label_x = concurrency_label_x;
        layout.concurrency_label_width = concurrency_label_width;
        layout.concurrency_frame = concurrency_frame;
        RECT client{};
        GetClientRect(hwnd, &client);
        layout.client_bottom = client.bottom;
        layout.tabs.reserve(panes.size());
        for (std::size_t index = 0; index < panes.size(); ++index) {
            const Pane& pane = panes[index];
            layout.tabs.push_back({pane.title,
                                   pane.view_state.phase,
                                   pane.local,
                                   index == ActiveTabInGroup(GroupOf(index))});
        }
        if (!panes.empty() && pane_rects.size() == panes.size()) {
            if (split_active) {
                for (unsigned group = 0; group < 2; ++group) {
                    auto tabs = TabsInGroup(group);
                    if (!tabs.empty()) {
                        layout.groups.push_back({pane_rects[tabs.front()], std::move(tabs)});
                    }
                }
            } else {
                std::vector<std::size_t> tabs(panes.size());
                std::iota(tabs.begin(), tabs.end(), std::size_t{0});
                const std::size_t selected = std::min(active_pane_index, panes.size() - 1);
                layout.groups.push_back({pane_rects[selected], std::move(tabs)});
            }
        }
        if (split_active) {
            const auto left = FirstTabInGroup(0);
            if (left && *left < pane_rects.size()) {
                layout.split_separator = pane_rects[*left];
            }
        }
        if (visible_drop_zone != SplitDropZone::none) {
            layout.drop_zone = DropZoneRect();
            layout.drop_center = visible_drop_zone == SplitDropZone::center;
        }
        if (visible_tab_insertion) {
            layout.insertion = detail::TransferTabInsertion{visible_tab_insertion->x,
                                                            visible_tab_insertion->top,
                                                            visible_tab_insertion->bottom};
        }
        detail::BuildTransferPageSurface(surface, layout);
        StartTabMotionIfNeeded();
    }

    void InvalidateTabStrip() const {
        RECT client{};
        GetClientRect(hwnd, &client);
        const RECT strip{0, 0, client.right, detail::ScaleForDpi(35, detail::WindowDpi(hwnd))};
        InvalidateRect(hwnd, &strip, FALSE);
    }

    void UpdateTabAccent() {
        if (panes.empty() || active_pane_index >= panes.size()) {
            surface.SetOverlayAccent(std::nullopt);
            return;
        }
        const int tab_id = kPaneTabBaseId + static_cast<int>(active_pane_index);
        for (const auto& element : surface.elements()) {
            if (element.id != tab_id || element.role != detail::UiRole::file_tab) {
                continue;
            }
            const float scale = static_cast<float>(detail::WindowDpi(hwnd)) / 96.0F;
            const float half_width = 14.0F * scale * tab_motion_progress;
            const float center = (element.bounds.left + element.bounds.right) / 2.0F;
            surface.SetOverlayAccent(detail::UiRect(center - half_width,
                                                    element.bounds.bottom - 2.0F * scale,
                                                    center + half_width,
                                                    element.bounds.bottom));
            return;
        }
        surface.SetOverlayAccent(std::nullopt);
    }

    void StartTabMotionIfNeeded() {
        const TransferPaneId current =
            panes.empty() || active_pane_index >= panes.size() ? 0 : panes[active_pane_index].id;
        if (animated_tab_id != current) {
            animated_tab_id = current;
            tab_motion_start = GetTickCount64();
            tab_motion_active = current != 0 && tab_motion_enabled &&
                                SetTimer(hwnd, kTabMotionTimerId, kLayoutIntervalMs, nullptr) != 0;
            tab_motion_progress = tab_motion_active ? 0.0F : 1.0F;
            if (!tab_motion_active) {
                KillTimer(hwnd, kTabMotionTimerId);
            }
        }
        UpdateTabAccent();
    }

    void TickTabMotion() {
        if (!tab_motion_active) {
            KillTimer(hwnd, kTabMotionTimerId);
            return;
        }
        const float fraction = std::min(1.0F,
                                        static_cast<float>(GetTickCount64() - tab_motion_start) /
                                            static_cast<float>(kTabMotionDurationMs));
        tab_motion_progress = 1.0F - (1.0F - fraction) * (1.0F - fraction);
        tab_motion_active = fraction < 1.0F;
        if (!tab_motion_active) {
            KillTimer(hwnd, kTabMotionTimerId);
        }
        UpdateTabAccent();
        InvalidateTabStrip();
    }

    void Layout(const int width, const int height) {
        if (!queue_list || width <= 0 || height <= 0) {
            return;
        }
        const UINT dpi = detail::WindowDpi(hwnd);
        const int pad = detail::ScaleForDpi(8, dpi);
        const int gap = detail::ScaleForDpi(6, dpi);
        if (queue_height_override == 0) {
            queue_height_override = detail::ScaleForDpi(190, dpi);
        }
        const int queue_height = std::clamp(
            queue_height_override,
            detail::ScaleForDpi(110, dpi),
            std::max(detail::ScaleForDpi(110, dpi), height - detail::ScaleForDpi(190, dpi)));
        queue_height_override = queue_height;
        const int queue_toolbar_height = detail::ScaleForDpi(32, dpi);
        const int pane_tab_height = detail::ScaleForDpi(34, dpi);
        const int pane_left = 0;
        const int pane_area_height = std::max(1, height - queue_height - gap - pad);

        const int panes_y = 0;
        NormalizeGroups();
        pane_rects.assign(panes.size(), RECT{});
        const RECT content{
            pane_left, panes_y, std::max(pane_left + 1, width - pad), panes_y + pane_area_height};
        const auto place_pane = [&](const std::size_t index, const RECT& target) {
            if (index >= panes.size()) {
                return;
            }
            Pane& pane = panes[index];
            const int pane_width = std::max(1L, target.right - target.left);
            const int pane_height = std::max(1L, target.bottom - target.top - pane_tab_height + 1);
            MoveWindow(pane.container,
                       target.left,
                       target.top + pane_tab_height - 1,
                       pane_width,
                       pane_height,
                       FALSE);
            LayoutPane(pane, pane_width, pane_height);
            if (IsWindowVisible(pane.container) == FALSE) {
                ShowWindow(pane.container, SW_SHOW);
            }
        };
        if (panes.empty()) {
            split_active = false;
        } else if (!split_active) {
            const std::size_t selected = std::min(active_pane_index, panes.size() - 1);
            for (std::size_t index = 0; index < panes.size(); ++index) {
                const bool visible = index == selected;
                if (!visible && IsWindowVisible(panes[index].container) != FALSE) {
                    ShowWindow(panes[index].container, SW_HIDE);
                }
            }
            pane_rects[selected] = content;
            place_pane(selected, content);
        } else {
            if (pane_weights.size() != 2) {
                pane_weights.assign(2, 1.0F);
            }
            const int available = std::max(2L, content.right - content.left - gap);
            const float left_weight = std::max(0.01F, pane_weights[0]);
            const float right_weight = std::max(0.01F, pane_weights[1]);
            const int left_width =
                std::clamp(static_cast<int>(available * left_weight / (left_weight + right_weight)),
                           1,
                           std::max(1, available - 1));
            const std::array<RECT, 2> groups{{
                {content.left, content.top, content.left + left_width, content.bottom},
                {content.left + left_width + gap, content.top, content.right, content.bottom},
            }};
            const std::array<std::size_t, 2> selected{ActiveTabInGroup(0), ActiveTabInGroup(1)};
            for (unsigned group = 0; group < 2; ++group) {
                for (const std::size_t index : TabsInGroup(group)) {
                    pane_rects[index] = groups[group];
                    if (index != selected[group] &&
                        IsWindowVisible(panes[index].container) != FALSE) {
                        ShowWindow(panes[index].container, SW_HIDE);
                    }
                }
                place_pane(selected[group], groups[group]);
            }
        }

        const int queue_y = panes_y + pane_area_height + gap;
        queue_splitter_y = queue_y - gap / 2;
        const int combo_width = detail::ScaleForDpi(50, dpi);
        const int combo_height = detail::ScaleForDpi(24, dpi);
        const int label_width = detail::ScaleForDpi(42, dpi);
        int control_x = width - pad;
        control_x -= combo_width;
        const int combo_y = queue_y + std::max(0, (queue_toolbar_height - combo_height) / 2);
        concurrency_frame = {control_x, combo_y, control_x + combo_width, combo_y + combo_height};
        const int edit_inset = std::max(1, detail::ScaleForDpi(1, dpi));
        const int edit_width = std::max(0, combo_width - edit_inset * 2);
        const int edit_height = std::max(0, combo_height - edit_inset * 2);
        MoveWindow(concurrency_edit,
                   concurrency_frame.left + edit_inset,
                   concurrency_frame.top + edit_inset,
                   edit_width,
                   edit_height,
                   FALSE);
        if (concurrency_edit_region.cx != edit_width || concurrency_edit_region.cy != edit_height) {
            detail::SetRoundedControlRegion(
                concurrency_edit, edit_width, edit_height, detail::ScaleForDpi(5, dpi));
            concurrency_edit_region = {edit_width, edit_height};
        }
        control_x -= label_width;
        const int concurrency_label_x = control_x;
        MoveWindow(queue_list,
                   pane_left + 1,
                   queue_y + queue_toolbar_height + 1,
                   std::max(0, width - pane_left - pad - 1),
                   std::max(0, height - queue_y - queue_toolbar_height - pad - 2),
                   FALSE);
        RECT queue_client{};
        GetClientRect(queue_list, &queue_client);
        const int queue_width =
            std::max(1, static_cast<int>(queue_client.right - queue_client.left));
        std::array<int, 8> queue_widths{detail::ScaleForDpi(160, dpi),
                                        detail::ScaleForDpi(90, dpi),
                                        detail::ScaleForDpi(90, dpi),
                                        detail::ScaleForDpi(120, dpi),
                                        detail::ScaleForDpi(90, dpi),
                                        detail::ScaleForDpi(190, dpi),
                                        detail::ScaleForDpi(40, dpi),
                                        detail::ScaleForDpi(190, dpi)};
        const int queue_minimum = std::accumulate(queue_widths.begin(), queue_widths.end(), 0);
        if (queue_width >= queue_minimum) {
            static constexpr std::array<int, 8> growth_weights{18, 9, 9, 11, 8, 21, 3, 21};
            const int growth = queue_width - queue_minimum;
            int distributed = 0;
            for (std::size_t column = 0; column + 1 < queue_widths.size(); ++column) {
                const int addition = growth * growth_weights[column] / 100;
                queue_widths[column] += addition;
                distributed += addition;
            }
            queue_widths.back() += growth - distributed;
        }
        table::SetColumnWidths(queue_list, queue_widths);
        BuildSurface(
            width, pane_left, queue_y, queue_toolbar_height, concurrency_label_x, label_width);
        RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_NOERASE);
    }
};

TransferPage::TransferPage() : impl_(std::make_unique<Impl>()) {}

TransferPage::~TransferPage() {
    if (impl_->hwnd) {
        DestroyWindow(impl_->hwnd);
    }
    DeleteObject(impl_->font);
    DeleteObject(impl_->file_name_font);
    DeleteObject(impl_->title_font);
    DeleteObject(impl_->path_font);
}

bool TransferPage::Create(HWND parent, HINSTANCE instance) {
    impl_->instance = instance;
    if (!RegisterPageClass(instance, &Impl::WindowProc) || !RegisterPaneClass(instance) ||
        !RegisterPathFrameClass(instance) || !RegisterResizeGuideClass(instance)) {
        return false;
    }
    impl_->hwnd = CreateWindowExW(0,
                                  kTransferPageClass,
                                  L"",
                                  WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                                  0,
                                  0,
                                  0,
                                  0,
                                  parent,
                                  nullptr,
                                  instance,
                                  impl_.get());
    if (!impl_->hwnd) {
        return false;
    }
    if (!impl_->surface.Initialize(impl_->hwnd)) {
        return false;
    }

    const UINT dpi = detail::WindowDpi(parent);
    impl_->font = detail::CreateUiFont(dpi, 9);
    impl_->file_name_font = detail::CreateUiFont(dpi, 10);
    impl_->title_font = detail::CreateUiFont(dpi, 10, true);
    impl_->path_font = detail::CreateUiFont(dpi, 10);
    const file_icons::SystemFileIcons& system_icons = file_icons::SharedSystemFileIcons();
    impl_->file_icons = system_icons.image_list;
    impl_->folder_icon = system_icons.folder;
    impl_->file_icon = system_icons.file;
    impl_->resize_guide =
        CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                        kResizeGuideClass,
                        L"",
                        WS_POPUP,
                        0,
                        0,
                        0,
                        0,
                        impl_->hwnd,
                        nullptr,
                        instance,
                        nullptr);
    if (impl_->resize_guide) {
        SetLayeredWindowAttributes(
            impl_->resize_guide, kResizeGuideTransparentColor, 0, LWA_COLORKEY);
    }

    impl_->concurrency_edit =
        CreateWindowExW(0,
                        L"EDIT",
                        L"3",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER | ES_CENTER | ES_AUTOHSCROLL,
                        0,
                        0,
                        0,
                        0,
                        impl_->hwnd,
                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kConcurrencyEditId)),
                        instance,
                        nullptr);
    SendMessageW(impl_->concurrency_edit, EM_SETLIMITTEXT, 1, 0);
    SendMessageW(impl_->concurrency_edit,
                 EM_SETMARGINS,
                 EC_LEFTMARGIN | EC_RIGHTMARGIN,
                 MAKELPARAM(detail::ScaleForDpi(4, dpi), detail::ScaleForDpi(4, dpi)));
    SetWindowSubclass(impl_->concurrency_edit,
                      &Impl::ControlSubclassProc,
                      5,
                      reinterpret_cast<DWORD_PTR>(impl_.get()));
    impl_->queue_list = table::Create(instance, impl_->hwnd, kQueueListId, true);
    const std::array queue_columns{
        table::Column{L"名称", detail::ScaleForDpi(210, dpi), detail::ScaleForDpi(130, dpi)},
        table::Column{L"状态", detail::ScaleForDpi(120, dpi), detail::ScaleForDpi(90, dpi)},
        table::Column{L"进度",
                      detail::ScaleForDpi(120, dpi),
                      detail::ScaleForDpi(90, dpi),
                      table::Alignment::center},
        table::Column{L"大小", detail::ScaleForDpi(150, dpi), detail::ScaleForDpi(110, dpi)},
        table::Column{L"速度", detail::ScaleForDpi(100, dpi), detail::ScaleForDpi(80, dpi)},
        table::Column{L"本地路径", detail::ScaleForDpi(260, dpi), detail::ScaleForDpi(150, dpi)},
        table::Column{L"<->",
                      detail::ScaleForDpi(46, dpi),
                      detail::ScaleForDpi(38, dpi),
                      table::Alignment::center},
        table::Column{L"远程路径", detail::ScaleForDpi(260, dpi), detail::ScaleForDpi(150, dpi)}};
    table::SetColumns(impl_->queue_list, queue_columns);
    table::SetTextProvider(impl_->queue_list,
                           [page = impl_.get()](const std::size_t row, const std::size_t column) {
                               return page->QueueCellText(row, column);
                           });
    table::SetImageList(impl_->queue_list, impl_->file_icons);
    table::SetImageProvider(impl_->queue_list, [page = impl_.get()](const std::size_t row) {
        return page->QueueIcon(row);
    });
    table::SetTreeProvider(
        impl_->queue_list,
        [page = impl_.get()](const std::size_t row) {
            return page->QueueTreeCell(row);
        },
        [page = impl_.get()](const std::size_t row) {
            page->ToggleQueueExpansion(row);
        });
    table::SetProgressProvider(
        impl_->queue_list, [page = impl_.get()](const std::size_t row, const std::size_t column) {
            return page->QueueProgress(row, column);
        });

    detail::ApplySelfDrawnTheme(impl_->concurrency_edit);
    detail::SetControlFont(impl_->concurrency_edit, impl_->font);
    detail::SetControlFont(impl_->queue_list, impl_->font);
    table::ApplyTheme(impl_->queue_list);
    detail::SetControlFont(impl_->concurrency_edit, impl_->path_font);

    const transfer::FileUri desktop = DesktopLocation();
    impl_->panes.push_back(impl_->MakePane(1, true, L"本地主机", desktop));
    impl_->pane_groups.push_back(0U);
    impl_->group_active[0] = 0;
    impl_->active_pane_index = 0;
    impl_->panes.front().endpoint = std::make_shared<transfer::LocalEndpoint>();
    if (desktop.path() == "/") {
        impl_->PopulateThisPc(impl_->panes.front());
    } else {
        impl_->RequestList(impl_->panes.front(), desktop);
    }
    return true;
}

void TransferPage::SetBounds(const int x, const int y, const int width, const int height) {
    SetWindowPos(
        impl_->hwnd, nullptr, x, y, width, height, SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOREDRAW);
}

void TransferPage::Show(const bool visible) {
    if (visible) {
        impl_->RefreshQueue();
        SetTimer(impl_->hwnd, kQueueTimerId, 250, nullptr);
    } else {
        KillTimer(impl_->hwnd, kQueueTimerId);
    }
    ShowWindow(impl_->hwnd, visible ? SW_SHOW : SW_HIDE);
}

void TransferPage::ApplyTheme() {
    detail::ApplyThemeToWindowTree(impl_->hwnd);
    RECT rect{};
    GetClientRect(impl_->hwnd, &rect);
    impl_->Layout(rect.right, rect.bottom);
    RedrawWindow(impl_->hwnd,
                 nullptr,
                 nullptr,
                 RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

TransferPaneId TransferPage::AddLocalPane(std::wstring title) {
    if (title.empty()) {
        title = L"本地主机";
    }
    return impl_->AppendLocal(std::move(title));
}

TransferPaneId TransferPage::AddRemotePane(std::wstring server_name, std::wstring initial_path) {
    if (server_name.empty()) {
        server_name = L"远程服务器";
    }
    return impl_->AppendRemote(server_name,
                               SafeServerId(server_name),
                               ToUtf8(initial_path.empty() ? L"/" : initial_path),
                               nullptr);
}

TransferPaneId TransferPage::AddRemotePane(std::wstring server_name,
                                           std::string server_id,
                                           std::shared_ptr<transfer::IFileEndpoint> endpoint,
                                           std::string initial_path) {
    if (server_name.empty()) {
        server_name = L"远程服务器";
    }
    if (server_id.empty()) {
        server_id = SafeServerId(server_name);
    }
    return impl_->AppendRemote(
        std::move(server_name), std::move(server_id), std::move(initial_path), std::move(endpoint));
}

TransferPaneId TransferPage::AddPendingRemotePane(std::wstring server_name, std::string server_id) {
    const TransferPaneId id =
        impl_->AppendRemote(std::move(server_name), std::move(server_id), "/", nullptr);
    SetPaneViewState(id, {ViewPhase::Connecting, L"正在连接服务器…", ViewRecoveryAction::None});
    return id;
}

bool TransferPage::ClosePane(const TransferPaneId id) {
    return impl_->RemovePane(id);
}

void TransferPage::ResetRemotePanes() {
    std::vector<TransferPaneId> ids;
    for (const auto& pane : impl_->panes) {
        if (!pane.local) {
            ids.push_back(pane.id);
        }
    }
    for (const TransferPaneId id : ids) {
        impl_->RemovePane(id);
    }
}

bool TransferPage::BindPaneEndpoint(const TransferPaneId id,
                                    std::shared_ptr<transfer::IFileEndpoint> endpoint,
                                    transfer::FileUri location) {
    Impl::Pane* pane = impl_->FindPane(id);
    if (!pane || !endpoint || !endpoint->accepts(location)) {
        return false;
    }
    pane->endpoint = std::move(endpoint);
    pane->view_state = {ViewPhase::Loading, L"正在加载目录…", ViewRecoveryAction::None};
    return impl_->RequestList(*pane, std::move(location));
}

void TransferPage::SetPaneViewState(const TransferPaneId id, ViewState state) {
    Impl::Pane* pane = impl_->FindPane(id);
    if (!pane) {
        return;
    }
    pane->view_state = std::move(state);
    if (pane->view_state.phase == ViewPhase::Connecting ||
        pane->view_state.phase == ViewPhase::Disconnected ||
        pane->view_state.phase == ViewPhase::Error) {
        impl_->PopulateUnboundRemote(*pane);
    }
    InvalidateRect(impl_->hwnd, nullptr, FALSE);
}

void TransferPage::SetPaneActionCallback(PaneActionCallback callback) {
    impl_->pane_action_callback = std::move(callback);
}

bool TransferPage::NavigatePane(const TransferPaneId id, transfer::FileUri location) {
    Impl::Pane* pane = impl_->FindPane(id);
    return pane && impl_->RequestList(*pane, std::move(location));
}

bool TransferPage::RefreshPane(const TransferPaneId id) {
    Impl::Pane* pane = impl_->FindPane(id);
    return pane && impl_->RequestList(*pane, pane->location);
}

bool TransferPage::HandleEnterKey(const HWND focused_control) {
    if (!focused_control || !impl_->hwnd || !IsChild(impl_->hwnd, focused_control)) {
        return false;
    }
    Impl::Pane* pane = impl_->FindPaneByPath(focused_control);
    if (!pane) {
        return false;
    }
    impl_->NavigateFromPathControl(*pane);
    return true;
}

std::optional<transfer::FileUri> TransferPage::pane_location(const TransferPaneId id) const {
    const Impl::Pane* pane = impl_->FindPane(id);
    if (!pane) {
        return std::nullopt;
    }
    return pane->location;
}

void TransferPage::SetAsyncDirectoryList(AsyncDirectoryList callback) {
    impl_->async_directory_list = std::move(callback);
}

void TransferPage::SetTransferManager(std::shared_ptr<transfer::TransferManager> manager) {
    impl_->transfer_manager = std::move(manager);
    if (impl_->transfer_manager) {
        const auto concurrency =
            std::clamp<std::size_t>(impl_->transfer_manager->concurrency(), 1, 8);
        impl_->concurrency_value = static_cast<int>(concurrency);
    }
    impl_->SyncConcurrencyEdit();
    impl_->RefreshQueue();
    impl_->Relayout();
}

void TransferPage::SetRemoteEditCallback(RemoteEditCallback callback) {
    impl_->remote_edit_callback = std::move(callback);
}

void TransferPage::SetPaneClosedCallback(PaneClosedCallback callback) {
    impl_->pane_closed_callback = std::move(callback);
}

void TransferPage::SetPaneNavigatedCallback(PaneNavigatedCallback callback) {
    impl_->pane_navigated_callback = std::move(callback);
}

void TransferPage::SetServerOptions(std::vector<TransferServerOption> servers) {
    impl_->server_options = std::move(servers);
    impl_->Relayout();
}

bool TransferPage::ActivateServer(const std::string_view server_id) {
    if (server_id.empty() || !impl_->open_remote_session_callback) {
        return false;
    }
    impl_->open_remote_session_callback(std::string(server_id));
    return true;
}

void TransferPage::SetOpenRemoteSessionCallback(OpenRemoteSessionCallback callback) {
    impl_->open_remote_session_callback = std::move(callback);
}

void TransferPage::SetStatusCallback(StatusCallback callback) {
    impl_->status_callback = std::move(callback);
}

std::size_t TransferPage::pane_count() const noexcept {
    return impl_->panes.size();
}

std::optional<TransferPaneId> TransferPage::pane_id_at(const std::size_t index) const noexcept {
    if (index >= impl_->panes.size()) {
        return std::nullopt;
    }
    return impl_->panes[index].id;
}

HWND TransferPage::hwnd() const noexcept {
    return impl_->hwnd;
}

} // namespace SLink::ui
