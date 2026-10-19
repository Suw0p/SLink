// 无传统标题栏的主窗口、顶层菜单、自绘导航、页面切换、DPI/主题变化和 UI 任务消息
// 分发实现。
#include "ui/main_window.hpp"

#include "resource.h"
#include "slink_version.hpp"
#include "ui/editor_page.hpp"
#include "ui/port_forward_page.hpp"
#include "ui/retained_ui.hpp"
#include "ui/sidebar_control.hpp"
#include "ui/terminal_page.hpp"
#include "ui/transfer_page.hpp"
#include "ui/win32_ui_internal.hpp"

#include <algorithm>
#include <array>
#include <commctrl.h>
#include <cwctype>
#include <deque>
#include <mutex>
#include <optional>
#include <ranges>
#include <unordered_set>
#include <utility>
#include <windowsx.h>

namespace SLink::ui {
namespace {

constexpr wchar_t kMainWindowClass[] = L"SLink.MainWindow";
constexpr int kNavigationFirstId = 1100;
constexpr int kManageServersId = 1200;
constexpr int kToggleServersId = 1201;
constexpr int kDarkThemeId = 1202;
constexpr int kLightThemeId = 1203;
constexpr int kSharedServerSearchId = 1204;
constexpr int kSharedServerListId = 1205;
// 分组 ID 只允许字母、数字和 -_.；该保留值可安全表示未分组标题的折叠状态。
constexpr std::string_view kUngroupedCollapseKey = "@ungrouped";
constexpr int kMinimizeWindowId = 1210;
constexpr int kMaximizeWindowId = 1211;
constexpr int kCloseWindowId = 1212;
constexpr UINT kUiTaskMessage = WM_APP + 0x51;
constexpr UINT_PTR kLayoutTimerId = 1;
constexpr UINT_PTR kMotionTimerId = 2;
constexpr UINT kLayoutIntervalMs = 16;
constexpr ULONGLONG kNavigationMotionMs = 120;
constexpr ULONGLONG kSidebarMotionMs = 100;
constexpr wchar_t kSidebarResizeGuideClass[] = L"SLink.SharedSidebarResizeGuide";
constexpr COLORREF kResizeGuideTransparentColor = RGB(1, 2, 3);

bool ClientAnimationsEnabled() noexcept {
    BOOL enabled = TRUE;
    return SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0) == FALSE ||
           enabled != FALSE;
}

LRESULT CALLBACK SidebarResizeGuideProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        const HDC dc = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        const HBRUSH transparent = CreateSolidBrush(kResizeGuideTransparentColor);
        FillRect(dc, &client, transparent);
        DeleteObject(transparent);
        const HBRUSH dash = CreateSolidBrush(detail::kMutedTextColor);
        const int dash_height = std::max(2, detail::ScaleForDpi(3, detail::WindowDpi(window)));
        const int step =
            dash_height + std::max(2, detail::ScaleForDpi(3, detail::WindowDpi(window)));
        for (int y = 0; y < client.bottom; y += step) {
            RECT mark{
                0, y, client.right, std::min(static_cast<int>(client.bottom), y + dash_height)};
            FillRect(dc, &mark, dash);
        }
        DeleteObject(dash);
        EndPaint(window, &paint);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

struct NavigationItem {
    MainPage page;
    const wchar_t* label;
};

constexpr std::array kNavigationItems{
    NavigationItem{MainPage::Terminal, L"终端"},
    NavigationItem{MainPage::Transfer, L"传输"},
    NavigationItem{MainPage::Editor, L"编辑"},
    NavigationItem{MainPage::PortForward, L"转发"},
    NavigationItem{MainPage::Settings, L"设置"},
};
constexpr int kNavigationCount = static_cast<int>(kNavigationItems.size());

void EnablePerMonitorDpi() {
    using SetDpiAwarenessFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    const auto set_awareness = reinterpret_cast<SetDpiAwarenessFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"));
    if (set_awareness) {
        set_awareness(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    }
}

} // namespace

struct MainWindow::Impl {
    HINSTANCE instance{};
    HWND hwnd{};
    MainPage active_page{MainPage::Terminal};
    detail::UiSurface surface;
    std::unique_ptr<TerminalPage> terminal = std::make_unique<TerminalPage>();
    std::unique_ptr<TransferPage> transfer = std::make_unique<TransferPage>();
    std::unique_ptr<EditorPage> editor = std::make_unique<EditorPage>();
    std::unique_ptr<PortForwardPage> port_forward = std::make_unique<PortForwardPage>();
    HWND server_search{};
    HWND server_list{};
    HWND sidebar_resize_guide{};
    HFONT server_font{};
    HFONT search_font{};
    struct VisibleServerRow {
        std::optional<std::size_t> server_index;
        std::string group_id;
    };
    std::vector<ServerSidebarGroup> server_groups;
    std::vector<ServerSidebarItem> server_items;
    std::vector<VisibleServerRow> visible_server_rows;
    std::unordered_set<std::string> collapsed_server_groups;
    std::function<bool(const std::vector<std::string>&)> collapsed_groups_changed;
    std::string selected_server_id;
    std::size_t visible_server_count{};
    bool rebuilding_server_list{};
    std::mutex ui_task_mutex;
    std::deque<std::function<void()>> ui_tasks;
    std::function<void()> server_manager_callback;
    std::function<bool(theme::Mode)> theme_changed_callback;
    theme::Mode selected_theme{theme::CurrentMode()};
    int title_bar_height{};
    SIZE pending_layout_size{};
    bool layout_timer_pending{};
    bool motion_enabled{ClientAnimationsEnabled()};
    bool nav_motion_active{};
    bool sidebar_motion_active{};
    ULONGLONG nav_motion_start{};
    ULONGLONG sidebar_motion_start{};
    float nav_indicator_x{-1.0F};
    float nav_motion_from{};
    float nav_motion_target{};
    float sidebar_indicator_width{-1.0F};
    float sidebar_motion_from{};
    float sidebar_motion_target{};
    bool server_sidebar_visible{true};
    int server_sidebar_width_dip{theme::kServerSidebarWidth};
    RECT server_search_frame{};
    RECT server_manager_button{};
    RECT server_list_frame{};
    SIZE server_search_region{};
    bool resizing_server_sidebar{};
    int sidebar_resize_start{};
    int sidebar_resize_preview{-1};

    ~Impl() {
        if (sidebar_resize_guide && IsWindow(sidebar_resize_guide)) {
            DestroyWindow(sidebar_resize_guide);
        }
        if (hwnd && IsWindow(hwnd)) {
            DestroyWindow(hwnd);
        }
        if (server_font) {
            DeleteObject(server_font);
        }
        if (search_font) {
            DeleteObject(search_font);
        }
    }

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            self = static_cast<Impl*>(create->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->hwnd = window;
        }
        if (!self) {
            return DefWindowProcW(window, message, wparam, lparam);
        }

        switch (message) {
        case kUiTaskMessage:
            self->DrainUiTasks();
            return 0;
        case WM_CREATE:
            return self->CreateChildren() ? 0 : -1;
        case WM_SIZE:
            self->ScheduleLayout(LOWORD(lparam), HIWORD(lparam));
            return 0;
        case WM_TIMER:
            if (wparam == kLayoutTimerId) {
                self->CommitScheduledLayout();
                return 0;
            }
            if (wparam == kMotionTimerId) {
                self->TickMotion();
                return 0;
            }
            break;
        case WM_SETTINGCHANGE:
            self->motion_enabled = ClientAnimationsEnabled();
            // 顶层窗口收到系统动画偏好变化后同步给子页面；子页面各自停止标签计时器。
            if (self->terminal->hwnd()) {
                SendMessageW(self->terminal->hwnd(), message, wparam, lparam);
            }
            if (self->transfer->hwnd()) {
                SendMessageW(self->transfer->hwnd(), message, wparam, lparam);
            }
            if (self->editor->hwnd()) {
                SendMessageW(self->editor->hwnd(), message, wparam, lparam);
            }
            if (!self->motion_enabled) {
                self->nav_motion_active = false;
                self->sidebar_motion_active = false;
                self->nav_indicator_x = self->nav_motion_target;
                self->sidebar_indicator_width = self->sidebar_motion_target;
                KillTimer(window, kMotionTimerId);
                self->CommitLayoutNow();
            }
            break;
        case WM_COMMAND:
            if (LOWORD(wparam) == kSharedServerSearchId && HIWORD(wparam) == EN_CHANGE) {
                self->PopulateServerList();
                return 0;
            }
            break;
        case WM_NOTIFY: {
            const auto* header = reinterpret_cast<const NMHDR*>(lparam);
            if (header && header->hwndFrom == self->server_list) {
                if (header->code == TVN_SELCHANGEDW && !self->rebuilding_server_list) {
                    if (const auto selected = self->SelectedServerIndex()) {
                        self->selected_server_id = self->server_items[*selected].id;
                    }
                }
                if (header->code == NM_CLICK) {
                    self->ToggleSelectedGroup();
                }
                if (header->code == NM_RETURN) {
                    self->ToggleSelectedGroup();
                }
                if (header->code == NM_DBLCLK) {
                    if (self->SelectedServerIndex()) {
                        self->ActivateSelectedServer();
                    }
                }
                return 0;
            }
            break;
        }
        case WM_DPICHANGED: {
            const RECT* suggested = reinterpret_cast<const RECT*>(lparam);
            SetWindowPos(window,
                         nullptr,
                         suggested->left,
                         suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOACTIVATE | SWP_NOZORDER);
            self->surface.DpiChanged();
            self->RecreateSidebarFonts();
            self->CommitLayoutNow();
            return 0;
        }
        case WM_MOUSEMOVE:
            if (self->resizing_server_sidebar) {
                self->UpdateSidebarResize({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
                return 0;
            }
            self->surface.PointerMove({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
            return 0;
        case WM_MOUSELEAVE:
            self->surface.PointerLeave();
            return 0;
        case WM_LBUTTONDOWN:
            if (self->BeginSidebarResize({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)})) {
                return 0;
            }
            if (self->surface.PointerDown({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)})) {
                return 0;
            }
            if (self->IsTitleBarDragPoint({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)})) {
                POINT screen{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                ClientToScreen(window, &screen);
                ReleaseCapture();
                SendMessageW(window, WM_NCLBUTTONDOWN, HTCAPTION, MAKELPARAM(screen.x, screen.y));
                return 0;
            }
            break;
        case WM_LBUTTONDBLCLK:
            if (self->IsTitleBarDragPoint({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)})) {
                self->ToggleMaximize();
                return 0;
            }
            break;
        case WM_LBUTTONUP: {
            if (self->resizing_server_sidebar) {
                self->EndSidebarResize(true);
                return 0;
            }
            const auto clicked =
                self->surface.PointerUp({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
            if (clicked) {
                self->HandleClick(*clicked);
                return 0;
            }
            break;
        }
        case WM_CAPTURECHANGED:
            if (self->resizing_server_sidebar) {
                self->EndSidebarResize(false);
            }
            self->surface.CancelPointer();
            return 0;
        case WM_SETCURSOR:
            if (self->SetSidebarResizeCursor()) {
                return TRUE;
            }
            if (self->surface.hovered_id() != 0) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            break;
        case WM_CTLCOLOREDIT: {
            const HDC dc = reinterpret_cast<HDC>(wparam);
            if (reinterpret_cast<HWND>(lparam) == self->server_search) {
                SetTextColor(dc, detail::kTextColor);
                SetBkColor(dc, detail::kPanelAltColor);
                return reinterpret_cast<LRESULT>(detail::PanelAltThemeBrush());
            }
            break;
        }
        case WM_NCHITTEST:
            return self->NonClientHitTest({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
        case WM_PAINT:
            self->surface.Paint(detail::UiColor(detail::kWindowColor));
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_CLOSE:
            DestroyWindow(window);
            PostQuitMessage(0);
            return 0;
        case WM_DESTROY:
            KillTimer(window, kLayoutTimerId);
            self->layout_timer_pending = false;
            return 0;
        default:
            break;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    bool IsTitleBarDragPoint(const POINT point) const noexcept {
        if (point.y < 0 || point.y >= title_bar_height) {
            return false;
        }
        for (const detail::UiElement& element : surface.elements()) {
            if (element.id == 0 || !element.visible || !element.enabled) {
                continue;
            }
            if (point.x >= element.bounds.left && point.x < element.bounds.right &&
                point.y >= element.bounds.top && point.y < element.bounds.bottom) {
                return false;
            }
        }
        return true;
    }

    LRESULT NonClientHitTest(const POINT screen) const noexcept {
        if (!hwnd || IsZoomed(hwnd)) {
            return HTCLIENT;
        }
        RECT window_rect{};
        GetWindowRect(hwnd, &window_rect);
        const UINT dpi = detail::WindowDpi(hwnd);
        const int frame = GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi) +
                          GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
        const bool left = screen.x < window_rect.left + frame;
        const bool right = screen.x >= window_rect.right - frame;
        const bool top = screen.y < window_rect.top + frame;
        const bool bottom = screen.y >= window_rect.bottom - frame;
        if (top && left)
            return HTTOPLEFT;
        if (top && right)
            return HTTOPRIGHT;
        if (bottom && left)
            return HTBOTTOMLEFT;
        if (bottom && right)
            return HTBOTTOMRIGHT;
        if (left)
            return HTLEFT;
        if (right)
            return HTRIGHT;
        if (top)
            return HTTOP;
        if (bottom)
            return HTBOTTOM;
        return HTCLIENT;
    }

    void ToggleMaximize() const {
        ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
    }

    bool RegisterWindowClass() const {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
        wc.lpfnWndProc = &WindowProc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = static_cast<HICON>(LoadImageW(instance,
                                                 MAKEINTRESOURCEW(IDI_SLINK),
                                                 IMAGE_ICON,
                                                 GetSystemMetrics(SM_CXICON),
                                                 GetSystemMetrics(SM_CYICON),
                                                 LR_DEFAULTCOLOR));
        wc.hIconSm = static_cast<HICON>(LoadImageW(instance,
                                                   MAKEINTRESOURCEW(IDI_SLINK),
                                                   IMAGE_ICON,
                                                   GetSystemMetrics(SM_CXSMICON),
                                                   GetSystemMetrics(SM_CYSMICON),
                                                   LR_DEFAULTCOLOR));
        wc.lpszClassName = kMainWindowClass;
        const ATOM atom = RegisterClassExW(&wc);
        if (atom == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return false;
        }
        WNDCLASSEXW guide{sizeof(guide)};
        guide.lpfnWndProc = &SidebarResizeGuideProc;
        guide.hInstance = instance;
        guide.hCursor = LoadCursorW(nullptr, IDC_SIZEWE);
        guide.lpszClassName = kSidebarResizeGuideClass;
        const ATOM guide_atom = RegisterClassExW(&guide);
        return guide_atom != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }

    void DrainUiTasks() {
        std::deque<std::function<void()>> pending;
        {
            std::scoped_lock lock(ui_task_mutex);
            pending.swap(ui_tasks);
        }
        for (auto& callback : pending) {
            if (!callback) {
                continue;
            }
            try {
                callback();
            } catch (...) {
                MessageBoxW(hwnd, L"后台任务的界面回调执行失败。", L"SLink", MB_OK | MB_ICONERROR);
            }
        }
    }

    [[nodiscard]] bool UsesSharedServerSidebar() const noexcept {
        return server_sidebar_visible &&
               (active_page == MainPage::Terminal || active_page == MainPage::Transfer ||
                active_page == MainPage::Editor);
    }

    [[nodiscard]] std::optional<std::size_t> SelectedServerIndex() const {
        const HTREEITEM selected = server_list ? TreeView_GetSelection(server_list) : nullptr;
        if (!selected) {
            return std::nullopt;
        }
        const auto row = static_cast<std::size_t>(reinterpret_cast<INT_PTR>(selected) - 1);
        if (row >= visible_server_rows.size()) {
            return std::nullopt;
        }
        return visible_server_rows[row].server_index;
    }

    bool NotifyCollapsedGroupsChanged() {
        if (!collapsed_groups_changed) {
            return true;
        }
        std::vector<std::string> ids;
        ids.reserve(collapsed_server_groups.size());
        for (const auto& id : collapsed_server_groups) {
            ids.push_back(id.empty() ? std::string(kUngroupedCollapseKey) : id);
        }
        std::ranges::sort(ids);
        return collapsed_groups_changed(ids);
    }

    void ToggleSelectedGroup() {
        if (rebuilding_server_list || !server_list || GetWindowTextLengthW(server_search) != 0) {
            return;
        }
        const auto selected = TreeView_GetSelection(server_list);
        if (!selected) {
            return;
        }
        const auto row = static_cast<std::size_t>(reinterpret_cast<INT_PTR>(selected) - 1);
        if (row >= visible_server_rows.size() || visible_server_rows[row].server_index) {
            return;
        }
        const auto id = visible_server_rows[row].group_id;
        if (!collapsed_server_groups.erase(id)) {
            collapsed_server_groups.insert(id);
        }
        if (!NotifyCollapsedGroupsChanged()) {
            if (!collapsed_server_groups.erase(id)) {
                collapsed_server_groups.insert(id);
            }
            return;
        }
        PopulateServerList(true);
    }

    void PopulateServerList(bool preserve_scroll = false) {
        if (!server_list) {
            return;
        }
        const int previous_scroll = preserve_scroll ? sidebar::ScrollOffset(server_list) : 0;
        const int query_length = server_search ? GetWindowTextLengthW(server_search) : 0;
        std::wstring query(static_cast<std::size_t>(std::max(0, query_length)) + 1, L'\0');
        if (query_length > 0) {
            GetWindowTextW(server_search, query.data(), query_length + 1);
        }
        query.resize(static_cast<std::size_t>(std::max(0, query_length)));
        std::ranges::transform(query, query.begin(), [](const wchar_t value) {
            return static_cast<wchar_t>(std::towlower(value));
        });

        rebuilding_server_list = true;
        SendMessageW(server_list, WM_SETREDRAW, FALSE, 0);
        TreeView_DeleteAllItems(server_list);
        visible_server_rows.clear();
        HTREEITEM selected_item{};
        visible_server_count = 0;
        const auto contains_query = [&](std::wstring candidate) {
            std::ranges::transform(candidate, candidate.begin(), [](const wchar_t value) {
                return static_cast<wchar_t>(std::towlower(value));
            });
            return query.empty() || candidate.find(query) != std::wstring::npos;
        };
        std::unordered_set<std::string> known_groups;
        for (const auto& group : server_groups) {
            known_groups.insert(group.id);
        }
        const auto append_group = [&](const std::string& id, const std::wstring& label) {
            std::vector<std::size_t> members;
            std::vector<std::size_t> matches;
            const bool group_match = !query.empty() && contains_query(label);
            for (std::size_t index = 0; index < server_items.size(); ++index) {
                const auto& item = server_items[index];
                const bool belongs =
                    id.empty() ? (!item.group_id || !known_groups.contains(*item.group_id))
                               : item.group_id == id;
                if (!belongs) {
                    continue;
                }
                members.push_back(index);
                if (query.empty() || group_match || contains_query(item.label)) {
                    matches.push_back(index);
                }
            }
            if (id.empty() && members.empty()) {
                return;
            }
            if (!query.empty() && !group_match && matches.empty()) {
                return;
            }
            const bool expanded = !query.empty() || !collapsed_server_groups.contains(id);
            std::wstring heading = expanded ? L"▾  " : L"▸  ";
            heading += label + L" (" + std::to_wstring(members.size()) + L")";
            TVINSERTSTRUCTW group_insert{};
            group_insert.hParent = TVI_ROOT;
            group_insert.hInsertAfter = TVI_LAST;
            group_insert.item.mask = TVIF_TEXT | TVIF_PARAM;
            group_insert.item.pszText = heading.data();
            group_insert.item.lParam = 0;
            TreeView_InsertItem(server_list, &group_insert);
            visible_server_rows.push_back({std::nullopt, id});
            ++visible_server_count;
            if (!expanded) {
                return;
            }
            for (const auto index : matches) {
                std::wstring text = L"    " + server_items[index].label;
                TVINSERTSTRUCTW insert{};
                insert.hParent = TVI_ROOT;
                insert.hInsertAfter = TVI_LAST;
                insert.item.mask = TVIF_TEXT | TVIF_PARAM;
                insert.item.pszText = text.data();
                insert.item.lParam = static_cast<LPARAM>(index + 1);
                const HTREEITEM inserted = TreeView_InsertItem(server_list, &insert);
                visible_server_rows.push_back({index, id});
                ++visible_server_count;
                if (server_items[index].id == selected_server_id) {
                    selected_item = inserted;
                }
            }
        };
        append_group({}, L"未分组");
        for (const auto& group : server_groups) {
            append_group(group.id, group.label);
        }
        TreeView_SelectItem(server_list, selected_item);
        if (preserve_scroll) {
            // SelectItem 会自动滚到旧服务器；分组操作应以用户正在看的位置为准。
            sidebar::SetScrollOffset(server_list, previous_scroll);
        }
        SendMessageW(server_list, WM_SETREDRAW, TRUE, 0);
        rebuilding_server_list = false;
        if (UsesSharedServerSidebar()) {
            ShowWindow(server_list, visible_server_count == 0 ? SW_HIDE : SW_SHOWNOACTIVATE);
        }
        InvalidateRect(server_list, nullptr, FALSE);
        InvalidateRect(hwnd, &server_list_frame, FALSE);
    }

    bool SelectServer(std::string_view server_id) {
        const auto found = std::ranges::find(server_items, server_id, &ServerSidebarItem::id);
        if (found == server_items.end()) {
            return false;
        }
        selected_server_id.assign(server_id);
        const std::size_t target =
            static_cast<std::size_t>(std::distance(server_items.begin(), found));
        if (server_search && GetWindowTextLengthW(server_search) == 0) {
            const std::string group = found->group_id.value_or(std::string{});
            if (collapsed_server_groups.erase(group) != 0) {
                if (NotifyCollapsedGroupsChanged()) {
                    PopulateServerList();
                } else {
                    collapsed_server_groups.insert(group);
                }
            }
        }
        for (HTREEITEM item = TreeView_GetRoot(server_list); item;
             item = TreeView_GetNextSibling(server_list, item)) {
            TVITEMW value{};
            value.mask = TVIF_PARAM;
            value.hItem = item;
            if (TreeView_GetItem(server_list, &value) &&
                value.lParam == static_cast<LPARAM>(target + 1)) {
                TreeView_SelectItem(server_list, item);
                TreeView_EnsureVisible(server_list, item);
                return true;
            }
        }
        // 当前搜索词可能隐藏目标；保留选择，待过滤解除后恢复高亮。
        return true;
    }

    void ActivateSelectedServer() {
        const auto selected = SelectedServerIndex();
        if (!selected) {
            return;
        }
        const std::string_view server_id = server_items[*selected].id;
        switch (active_page) {
        case MainPage::Terminal:
            terminal->ActivateServer(server_id);
            break;
        case MainPage::Transfer:
            transfer->ActivateServer(server_id);
            break;
        case MainPage::Editor:
            editor->ActivateServer(server_id);
            break;
        case MainPage::PortForward:
        case MainPage::Settings:
            break;
        }
    }

    [[nodiscard]] bool SidebarResizeHandleAt(const POINT point) const noexcept {
        if (!UsesSharedServerSidebar()) {
            return false;
        }
        const UINT dpi = detail::WindowDpi(hwnd);
        const int edge = detail::ScaleForDpi(server_sidebar_width_dip, dpi);
        const int hit = detail::ScaleForDpi(6, dpi);
        return point.y >= title_bar_height && std::abs(point.x - edge) <= hit;
    }

    bool BeginSidebarResize(const POINT point) {
        if (!SidebarResizeHandleAt(point)) {
            return false;
        }
        resizing_server_sidebar = true;
        sidebar_resize_start =
            detail::ScaleForDpi(server_sidebar_width_dip, detail::WindowDpi(hwnd));
        sidebar_resize_preview = sidebar_resize_start;
        SetCapture(hwnd);
        (void)SetSidebarResizeCursor();
        UpdateSidebarResizeGuide();
        return true;
    }

    void UpdateSidebarResize(const POINT point) {
        if (!resizing_server_sidebar) {
            return;
        }
        RECT client{};
        GetClientRect(hwnd, &client);
        const UINT dpi = detail::WindowDpi(hwnd);
        const int preferred_maximum = detail::ScaleForDpi(theme::kServerSidebarMaximumWidth, dpi);
        const int content_minimum =
            detail::ScaleForDpi(theme::kServerSidebarContentMinimumWidth, dpi);
        const int maximum = std::max(
            0, std::min(preferred_maximum, static_cast<int>(client.right) - content_minimum));
        const int next = std::clamp(static_cast<int>(point.x), 0, maximum);
        if (next != sidebar_resize_preview) {
            sidebar_resize_preview = next;
            UpdateSidebarResizeGuide();
        }
    }

    void UpdateSidebarResizeGuide() const {
        if (!sidebar_resize_guide || !resizing_server_sidebar || sidebar_resize_preview < 0) {
            if (sidebar_resize_guide) {
                ShowWindow(sidebar_resize_guide, SW_HIDE);
            }
            return;
        }
        RECT client{};
        GetClientRect(hwnd, &client);
        const int thickness = std::max(1, detail::ScaleForDpi(2, detail::WindowDpi(hwnd)));
        POINT origin{std::max(0, sidebar_resize_preview - thickness / 2), title_bar_height};
        ClientToScreen(hwnd, &origin);
        SetWindowPos(sidebar_resize_guide,
                     HWND_TOPMOST,
                     origin.x,
                     origin.y,
                     thickness,
                     std::max(1, static_cast<int>(client.bottom) - title_bar_height),
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        InvalidateRect(sidebar_resize_guide, nullptr, FALSE);
        UpdateWindow(sidebar_resize_guide);
    }

    void EndSidebarResize(const bool commit) {
        if (!resizing_server_sidebar) {
            return;
        }
        ShowWindow(sidebar_resize_guide, SW_HIDE);
        if (commit && sidebar_resize_preview >= 0) {
            // 靠近窗口左边缘即折叠；保留先前宽度，点图标时可按原宽度展开。
            if (sidebar_resize_preview <= detail::ScaleForDpi(24, detail::WindowDpi(hwnd))) {
                server_sidebar_visible = false;
                StartSidebarMotion();
            } else {
                server_sidebar_width_dip =
                    MulDiv(sidebar_resize_preview, 96, detail::WindowDpi(hwnd));
            }
        }
        resizing_server_sidebar = false;
        sidebar_resize_preview = -1;
        if (GetCapture() == hwnd) {
            ReleaseCapture();
        }
        if (commit) {
            CommitLayoutNow();
            if (active_page == MainPage::Editor) {
                editor->RefreshLayout();
            }
        }
    }

    [[nodiscard]] bool SetSidebarResizeCursor() const {
        POINT point{};
        GetCursorPos(&point);
        ScreenToClient(hwnd, &point);
        if (resizing_server_sidebar || SidebarResizeHandleAt(point)) {
            SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
            return true;
        }
        return false;
    }

    bool CreateChildren() {
        if (!surface.Initialize(hwnd) || !sidebar::RegisterClass(instance)) {
            return false;
        }
        server_list = CreateWindowExW(
            0,
            sidebar::kClassName,
            L"",
            WS_CHILD | WS_TABSTOP | TVS_SHOWSELALWAYS | TVS_FULLROWSELECT | TVS_NONEVENHEIGHT,
            0,
            0,
            0,
            0,
            hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSharedServerListId)),
            instance,
            nullptr);
        server_search =
            CreateWindowExW(0,
                            L"EDIT",
                            L"",
                            WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL,
                            0,
                            0,
                            0,
                            0,
                            hwnd,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSharedServerSearchId)),
                            instance,
                            nullptr);
        sidebar_resize_guide =
            CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                            kSidebarResizeGuideClass,
                            L"",
                            WS_POPUP,
                            0,
                            0,
                            0,
                            0,
                            hwnd,
                            nullptr,
                            instance,
                            nullptr);
        if (!server_list || !server_search || !sidebar_resize_guide) {
            return false;
        }
        SetLayeredWindowAttributes(
            sidebar_resize_guide, kResizeGuideTransparentColor, 0, LWA_COLORKEY);
        RecreateSidebarFonts();
        detail::ApplySelfDrawnTheme(server_list);
        detail::ApplySelfDrawnTheme(server_search);
        if (!terminal->Create(hwnd, instance) || !transfer->Create(hwnd, instance) ||
            !editor->Create(hwnd, instance) || !port_forward->Create(hwnd, instance)) {
            return false;
        }
        terminal->ClearServers();
        Navigate(MainPage::Terminal);
        return true;
    }

    void RecreateSidebarFonts() {
        if (!hwnd || !server_list || !server_search) {
            return;
        }
        const UINT dpi = detail::WindowDpi(hwnd);
        HFONT next_server_font = detail::CreateUiFont(dpi, 10, false);
        HFONT next_search_font = detail::CreateUiFont(dpi, 10, false);
        detail::SetControlFont(server_list, next_server_font);
        detail::SetControlFont(server_search, next_search_font);
        TreeView_SetItemHeight(server_list, detail::ScaleForDpi(theme::kServerListItemHeight, dpi));
        SendMessageW(server_search,
                     EM_SETMARGINS,
                     EC_LEFTMARGIN | EC_RIGHTMARGIN,
                     MAKELPARAM(detail::ScaleForDpi(6, dpi), detail::ScaleForDpi(6, dpi)));
        if (server_font) {
            DeleteObject(server_font);
        }
        if (search_font) {
            DeleteObject(search_font);
        }
        server_font = next_server_font;
        search_font = next_search_font;
    }

    void HandleClick(int id) {
        if (id == kMinimizeWindowId) {
            ShowWindow(hwnd, SW_MINIMIZE);
            return;
        }
        if (id == kMaximizeWindowId) {
            ToggleMaximize();
            return;
        }
        if (id == kCloseWindowId) {
            SendMessageW(hwnd, WM_CLOSE, 0, 0);
            return;
        }
        if (id >= kNavigationFirstId && id < kNavigationFirstId + kNavigationCount) {
            Navigate(kNavigationItems[static_cast<std::size_t>(id - kNavigationFirstId)].page);
            return;
        }
        if (id == kToggleServersId) {
            server_sidebar_visible = !server_sidebar_visible;
            StartSidebarMotion();
            CommitLayoutNow();
            // 页面边界为了窗口缩放性能使用 SWP_NOREDRAW。侧栏是一次离散切换，
            // 因此在新边界提交后立即刷新当前编辑页，确保文件列表不会保留旧剪裁区。
            if (active_page == MainPage::Editor) {
                editor->RefreshLayout();
            }
            return;
        }
        if (id == kManageServersId && server_manager_callback) {
            try {
                server_manager_callback();
            } catch (...) {
                MessageBoxW(hwnd, L"无法打开服务器管理界面。", L"SLink", MB_OK | MB_ICONERROR);
            }
            return;
        }
        if (id == kDarkThemeId || id == kLightThemeId) {
            const theme::Mode next = id == kLightThemeId ? theme::Mode::Light : theme::Mode::Dark;
            if (next == selected_theme) {
                return;
            }
            if (theme_changed_callback && !theme_changed_callback(next)) {
                return;
            }
            selected_theme = next;
            ApplyThemeNow(next);
            return;
        }
    }

    void ApplyThemeNow(const theme::Mode mode) {
        theme::SetMode(mode);
        detail::EnableDarkTitleBar(hwnd);
        detail::ApplyThemeToWindowTree(hwnd);
        terminal->ApplyTheme();
        transfer->ApplyTheme();
        editor->ApplyTheme();
        port_forward->ApplyTheme();
        CommitLayoutNow();
        RedrawWindow(hwnd,
                     nullptr,
                     nullptr,
                     RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW);
    }

    void Navigate(MainPage page) {
        if (page != active_page) {
            StartNavigationMotion(page);
        }
        active_page = page;
        // Size the destination while all page windows are hidden.  Showing a
        // page before its new bounds were committed left newly exposed areas
        // unpainted, so sibling terminal pixels could remain visible beneath
        // the editor page until the next full repaint.
        terminal->Show(false);
        transfer->Show(false);
        editor->Show(false);
        port_forward->Show(false);
        CommitLayoutNow();
        switch (page) {
        case MainPage::Terminal:
            terminal->Show(true);
            break;
        case MainPage::Transfer:
            transfer->Show(true);
            break;
        case MainPage::Editor:
            editor->Show(true);
            break;
        case MainPage::PortForward:
            port_forward->Show(true);
            break;
        case MainPage::Settings:
            break;
        }
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
        InvalidateRect(hwnd, nullptr, FALSE);
    }

    void CommitScheduledLayout() {
        KillTimer(hwnd, kLayoutTimerId);
        layout_timer_pending = false;
        surface.Resize(pending_layout_size.cx, pending_layout_size.cy);
        Layout(pending_layout_size.cx, pending_layout_size.cy);
        InvalidateRect(hwnd, nullptr, FALSE);
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
        InvalidateRect(hwnd, nullptr, FALSE);
    }

    void StartNavigationMotion(const MainPage page) {
        const auto index = std::ranges::find(kNavigationItems, page, &NavigationItem::page);
        if (index == kNavigationItems.end()) {
            return;
        }
        const int id =
            kNavigationFirstId + static_cast<int>(std::distance(kNavigationItems.begin(), index));
        const float indicator_width = 28.0F * detail::WindowDpi(hwnd) / 96.0F;
        for (const auto& element : surface.elements()) {
            if (element.id != id) {
                continue;
            }
            nav_motion_target =
                element.bounds.left +
                (element.bounds.right - element.bounds.left - indicator_width) / 2.0F;
            nav_motion_from = nav_indicator_x < 0.0F ? nav_motion_target : nav_indicator_x;
            nav_motion_start = GetTickCount64();
            nav_motion_active = motion_enabled && nav_motion_from != nav_motion_target;
            if (!nav_motion_active) {
                nav_indicator_x = nav_motion_target;
            } else {
                SetTimer(hwnd, kMotionTimerId, kLayoutIntervalMs, nullptr);
            }
            return;
        }
    }

    void StartSidebarMotion() {
        sidebar_motion_target =
            server_sidebar_visible ? 16.0F * detail::WindowDpi(hwnd) / 96.0F : 0.0F;
        sidebar_motion_from =
            sidebar_indicator_width < 0.0F ? sidebar_motion_target : sidebar_indicator_width;
        sidebar_motion_start = GetTickCount64();
        sidebar_motion_active = motion_enabled && sidebar_motion_from != sidebar_motion_target;
        if (!sidebar_motion_active) {
            sidebar_indicator_width = sidebar_motion_target;
        } else {
            SetTimer(hwnd, kMotionTimerId, kLayoutIntervalMs, nullptr);
        }
    }

    void TickMotion() {
        const ULONGLONG now = GetTickCount64();
        const auto advance = [now](const ULONGLONG start,
                                   const ULONGLONG duration,
                                   const float from,
                                   const float target,
                                   float& current,
                                   bool& active) {
            if (!active) {
                return;
            }
            const float fraction =
                std::min(1.0F, static_cast<float>(now - start) / static_cast<float>(duration));
            const float eased = 1.0F - (1.0F - fraction) * (1.0F - fraction);
            current = from + (target - from) * eased;
            active = fraction < 1.0F;
        };
        advance(nav_motion_start,
                kNavigationMotionMs,
                nav_motion_from,
                nav_motion_target,
                nav_indicator_x,
                nav_motion_active);
        advance(sidebar_motion_start,
                kSidebarMotionMs,
                sidebar_motion_from,
                sidebar_motion_target,
                sidebar_indicator_width,
                sidebar_motion_active);
        if (!nav_motion_active && !sidebar_motion_active) {
            KillTimer(hwnd, kMotionTimerId);
        }
        RECT client{};
        GetClientRect(hwnd, &client);
        BuildChrome(client.right, client.bottom, title_bar_height);
        RECT navigation{0, 0, client.right, title_bar_height};
        InvalidateRect(hwnd, &navigation, FALSE);
    }

    void BuildChrome(int width, int height, int nav_height) {
        surface.ClearElements();
        surface.Add({
            .role = detail::UiRole::panel,
            .bounds = detail::UiRect(
                0.0F, 0.0F, static_cast<float>(width), static_cast<float>(nav_height)),
        });
        surface.Add({
            .role = detail::UiRole::separator,
            .bounds = detail::UiRect(0.0F,
                                     static_cast<float>(nav_height - 1),
                                     static_cast<float>(width),
                                     static_cast<float>(nav_height)),
        });

        const UINT dpi = detail::WindowDpi(hwnd);
        const float scale = static_cast<float>(dpi) / 96.0F;
        // 服务器是侧栏而非页面：左侧独立折叠图标，后面是紧凑的双字纯文字导航。
        const float window_button_width = 42.0F * scale;
        const float window_buttons_left =
            std::max(0.0F, static_cast<float>(width) - window_button_width * 3.0F);
        const float left = 8.0F * scale;
        const float gap = 3.0F * scale;
        const float sidebar_button_width = 36.0F * scale;
        surface.Add({
            .id = kToggleServersId,
            .role = detail::UiRole::navigation,
            .bounds = detail::UiRect(left,
                                     3.0F * scale,
                                     left + sidebar_button_width,
                                     static_cast<float>(nav_height - 3.0F * scale)),
            .icon = detail::UiIcon::sidebar_toggle,
            .alignment = detail::UiTextAlign::center,
            .selected = server_sidebar_visible,
        });
        float x = left + sidebar_button_width + 8.0F * scale;
        const float item_width =
            std::max(0.0F,
                     std::min(54.0F * scale,
                              (window_buttons_left - x - gap * (kNavigationCount - 1)) /
                                  static_cast<float>(kNavigationCount)));
        for (std::size_t index = 0; index < kNavigationItems.size(); ++index) {
            const NavigationItem& item = kNavigationItems[index];
            surface.Add({
                .id = kNavigationFirstId + static_cast<int>(index),
                .role = detail::UiRole::navigation,
                .bounds = detail::UiRect(
                    x, 3.0F * scale, x + item_width, static_cast<float>(nav_height - 3.0F * scale)),
                .text = item.label,
                .alignment = detail::UiTextAlign::center,
                .selected = active_page == item.page,
            });
            x += item_width + gap;
        }
        const float underline_width = 28.0F * scale;
        const float selected_x =
            left + sidebar_button_width + 8.0F * scale +
            static_cast<float>(static_cast<int>(active_page)) * (item_width + gap) +
            (item_width - underline_width) / 2.0F;
        if (!nav_motion_active) {
            nav_indicator_x = selected_x;
        }
        surface.Add({
            .role = detail::UiRole::accent_line,
            .bounds = detail::UiRect(nav_indicator_x,
                                     static_cast<float>(nav_height) - 3.0F * scale,
                                     nav_indicator_x + underline_width,
                                     static_cast<float>(nav_height) - 1.0F * scale),
        });
        if (sidebar_indicator_width < 0.0F) {
            sidebar_indicator_width = server_sidebar_visible ? 16.0F * scale : 0.0F;
        }
        if (sidebar_indicator_width > 0.0F) {
            const float indicator_x =
                left + (sidebar_button_width - sidebar_indicator_width) / 2.0F;
            surface.Add({
                .role = detail::UiRole::accent_line,
                .bounds = detail::UiRect(indicator_x,
                                         static_cast<float>(nav_height) - 3.0F * scale,
                                         indicator_x + sidebar_indicator_width,
                                         static_cast<float>(nav_height) - 1.0F * scale),
            });
        }

        surface.Add({
            .id = kMinimizeWindowId,
            .role = detail::UiRole::window_button,
            .bounds = detail::UiRect(window_buttons_left,
                                     0.0F,
                                     window_buttons_left + window_button_width,
                                     static_cast<float>(nav_height)),
            .icon = detail::UiIcon::minimize,
            .alignment = detail::UiTextAlign::center,
        });
        surface.Add({
            .id = kMaximizeWindowId,
            .role = detail::UiRole::window_button,
            .bounds = detail::UiRect(window_buttons_left + window_button_width,
                                     0.0F,
                                     window_buttons_left + window_button_width * 2.0F,
                                     static_cast<float>(nav_height)),
            .icon = IsZoomed(hwnd) ? detail::UiIcon::restore : detail::UiIcon::maximize,
            .alignment = detail::UiTextAlign::center,
        });
        surface.Add({
            .id = kCloseWindowId,
            .role = detail::UiRole::window_close_button,
            .bounds = detail::UiRect(window_buttons_left + window_button_width * 2.0F,
                                     0.0F,
                                     static_cast<float>(width),
                                     static_cast<float>(nav_height)),
            .icon = detail::UiIcon::close,
            .alignment = detail::UiTextAlign::center,
        });

        if (UsesSharedServerSidebar()) {
            const float sidebar_right = static_cast<float>(
                detail::ScaleForDpi(server_sidebar_width_dip, detail::WindowDpi(hwnd)));
            surface.Add({
                .role = detail::UiRole::panel,
                .bounds = detail::UiRect(0.0F,
                                         static_cast<float>(nav_height),
                                         sidebar_right,
                                         static_cast<float>(height)),
            });
            surface.Add({
                .role = detail::UiRole::separator,
                .bounds = detail::UiRect(sidebar_right - 1.0F,
                                         static_cast<float>(nav_height),
                                         sidebar_right,
                                         static_cast<float>(height)),
            });
            if (server_search_frame.right > server_search_frame.left) {
                surface.Add({
                    .role = detail::UiRole::card,
                    .bounds = detail::UiRect(static_cast<float>(server_search_frame.left),
                                             static_cast<float>(server_search_frame.top),
                                             static_cast<float>(server_search_frame.right),
                                             static_cast<float>(server_search_frame.bottom)),
                });
                surface.Add({
                    .id = kManageServersId,
                    .role = detail::UiRole::window_button,
                    .bounds = detail::UiRect(static_cast<float>(server_manager_button.left),
                                             static_cast<float>(server_manager_button.top),
                                             static_cast<float>(server_manager_button.right),
                                             static_cast<float>(server_manager_button.bottom)),
                    .icon = detail::UiIcon::server,
                    .alignment = detail::UiTextAlign::center,
                });
            }
            if (visible_server_count == 0) {
                surface.Add({
                    .role = detail::UiRole::muted,
                    .bounds = detail::UiRect(static_cast<float>(server_list_frame.left + 6),
                                             static_cast<float>(server_list_frame.top + 14),
                                             static_cast<float>(server_list_frame.right - 6),
                                             static_cast<float>(server_list_frame.top + 44)),
                    .text = server_items.empty() ? L"暂无服务器" : L"没有匹配的服务器",
                    .alignment = detail::UiTextAlign::center,
                });
            }
        }

        if (active_page != MainPage::Settings) {
            return;
        }

        const float content_top = static_cast<float>(nav_height);
        const float card_width = std::min(680.0F * scale, static_cast<float>(width) - 64.0F);
        const float card_height = 340.0F * scale;
        const float card_left = (static_cast<float>(width) - card_width) / 2.0F;
        const float card_top =
            content_top +
            std::max(36.0F * scale, (static_cast<float>(height - nav_height) - card_height) / 2.0F);
        surface.Add({
            .role = detail::UiRole::card,
            .bounds =
                detail::UiRect(card_left, card_top, card_left + card_width, card_top + card_height),
        });
        surface.Add({
            .role = detail::UiRole::heading,
            .bounds = detail::UiRect(card_left + 30.0F * scale,
                                     card_top + 24.0F * scale,
                                     card_left + card_width - 30.0F * scale,
                                     card_top + 62.0F * scale),
            .text = L"设置",
            .alignment = detail::UiTextAlign::leading,
        });
        const float version_right = card_left + card_width - 30.0F * scale;
        const float version_left =
            std::max(card_left + 90.0F * scale, version_right - 120.0F * scale);
        if (version_right - version_left >= 56.0F * scale) {
            surface.Add({
                .role = detail::UiRole::muted,
                .bounds = detail::UiRect(version_left,
                                         card_top + 24.0F * scale,
                                         version_right,
                                         card_top + 62.0F * scale),
                .text = version::kDisplay,
                .alignment = detail::UiTextAlign::trailing,
            });
        }
        surface.Add({
            .role = detail::UiRole::heading,
            .bounds = detail::UiRect(card_left + 30.0F * scale,
                                     card_top + 76.0F * scale,
                                     card_left + card_width - 30.0F * scale,
                                     card_top + 108.0F * scale),
            .text = L"外观",
            .alignment = detail::UiTextAlign::leading,
        });
        surface.Add({
            .role = detail::UiRole::label,
            .bounds = detail::UiRect(card_left + 30.0F * scale,
                                     card_top + 116.0F * scale,
                                     card_left + 116.0F * scale,
                                     card_top + 156.0F * scale),
            .text = L"主题",
            .alignment = detail::UiTextAlign::leading,
        });
        const float theme_left = card_left + 126.0F * scale;
        const float theme_width = 132.0F * scale;
        surface.Add({
            .id = kDarkThemeId,
            .role = detail::UiRole::tab,
            .bounds = detail::UiRect(theme_left,
                                     card_top + 116.0F * scale,
                                     theme_left + theme_width,
                                     card_top + 156.0F * scale),
            .text = L"深色",
            .alignment = detail::UiTextAlign::center,
            .selected = selected_theme == theme::Mode::Dark,
        });
        surface.Add({
            .id = kLightThemeId,
            .role = detail::UiRole::tab,
            .bounds = detail::UiRect(theme_left + theme_width + 10.0F * scale,
                                     card_top + 116.0F * scale,
                                     theme_left + theme_width * 2.0F + 10.0F * scale,
                                     card_top + 156.0F * scale),
            .text = L"浅色",
            .alignment = detail::UiTextAlign::center,
            .selected = selected_theme == theme::Mode::Light,
        });
        surface.Add({
            .role = detail::UiRole::muted,
            .bounds = detail::UiRect(theme_left,
                                     card_top + 162.0F * scale,
                                     card_left + card_width - 30.0F * scale,
                                     card_top + 192.0F * scale),
            .text = L"当前正在使用此主题",
            .alignment = detail::UiTextAlign::leading,
        });
        surface.Add({
            .role = detail::UiRole::separator,
            .bounds = detail::UiRect(card_left + 30.0F * scale,
                                     card_top + 212.0F * scale,
                                     card_left + card_width - 30.0F * scale,
                                     card_top + 213.0F * scale),
        });
        surface.Add({
            .role = detail::UiRole::heading,
            .bounds = detail::UiRect(card_left + 30.0F * scale,
                                     card_top + 228.0F * scale,
                                     card_left + card_width - 30.0F * scale,
                                     card_top + 260.0F * scale),
            .text = L"服务器",
            .alignment = detail::UiTextAlign::leading,
        });
        surface.Add({
            .role = detail::UiRole::muted,
            .bounds = detail::UiRect(card_left + 30.0F * scale,
                                     card_top + 266.0F * scale,
                                     card_left + card_width - 210.0F * scale,
                                     card_top + 308.0F * scale),
            .text = L"管理连接地址、认证方式和便携凭据。",
            .alignment = detail::UiTextAlign::leading,
        });
        surface.Add({
            .id = kManageServersId,
            .role = detail::UiRole::primary_button,
            .bounds = detail::UiRect(card_left + card_width - 190.0F * scale,
                                     card_top + 264.0F * scale,
                                     card_left + card_width - 30.0F * scale,
                                     card_top + 306.0F * scale),
            .text = L"服务器管理",
            .icon = detail::UiIcon::server,
            .alignment = detail::UiTextAlign::center,
        });
    }

    void Layout(int width, int height) {
        if (width <= 0 || height <= 0) {
            return;
        }
        const UINT dpi = detail::WindowDpi(hwnd);
        const int nav_height = detail::ScaleForDpi(40, dpi);
        title_bar_height = nav_height;
        const int content_height = std::max(0, height - nav_height);
        const bool show_sidebar = UsesSharedServerSidebar();
        const int sidebar_width =
            show_sidebar ? detail::ScaleForDpi(server_sidebar_width_dip, dpi) : 0;
        const int content_gap = show_sidebar ? detail::ScaleForDpi(1, dpi) : 0;
        const int content_x = sidebar_width + content_gap;
        const int content_width = std::max(0, width - content_x);
        if (show_sidebar) {
            const int pad = detail::ScaleForDpi(theme::kServerSidebarPadding, dpi);
            int list_top = nav_height + pad;
            if (sidebar_width >= detail::ScaleForDpi(150, dpi)) {
                const int inset = detail::ScaleForDpi(theme::kServerSearchInset, dpi);
                const int search_height = detail::ScaleForDpi(theme::kServerSearchHeight, dpi);
                const int button_gap = detail::ScaleForDpi(theme::kCompactControlGap, dpi);
                const int button_size = detail::ScaleForDpi(26, dpi);
                const int search_top = nav_height + detail::ScaleForDpi(11, dpi);
                const int search_width = sidebar_width - inset * 2 - button_size - button_gap;
                server_search_frame = {
                    inset, search_top, inset + search_width, search_top + search_height};
                server_manager_button = {sidebar_width - inset - button_size,
                                         search_top + (search_height - button_size) / 2,
                                         sidebar_width - inset,
                                         search_top + (search_height + button_size) / 2};
                const int border = std::max(1, detail::ScaleForDpi(1, dpi));
                const int edit_width = search_width - border * 2;
                const int edit_height = search_height - border * 2;
                MoveWindow(server_search,
                           server_search_frame.left + border,
                           server_search_frame.top + border,
                           edit_width,
                           edit_height,
                           FALSE);
                if (server_search_region.cx != edit_width ||
                    server_search_region.cy != edit_height) {
                    detail::SetRoundedControlRegion(
                        server_search, edit_width, edit_height, detail::ScaleForDpi(9, dpi));
                    server_search_region = {edit_width, edit_height};
                }
                list_top = server_search_frame.bottom + button_gap;
                ShowWindow(server_search, SW_SHOWNOACTIVATE);
            } else {
                // 拖窄时先收起搜索和管理控件，避免它们越过侧栏边界。
                server_search_frame = {};
                server_manager_button = {};
                ShowWindow(server_search, SW_HIDE);
            }
            MoveWindow(server_list,
                       pad,
                       list_top,
                       std::max(1, sidebar_width - pad * 2),
                       std::max(1, height - list_top - pad),
                       FALSE);
            server_list_frame = {
                pad, list_top, std::max(pad + 1, sidebar_width - pad), height - pad};
            ShowWindow(server_list, visible_server_count == 0 ? SW_HIDE : SW_SHOWNOACTIVATE);
        } else {
            server_search_frame = {};
            server_manager_button = {};
            server_list_frame = {};
            ShowWindow(server_search, SW_HIDE);
            ShowWindow(server_list, SW_HIDE);
        }
        switch (active_page) {
        case MainPage::Terminal:
            terminal->SetBounds(content_x, nav_height, content_width, content_height);
            break;
        case MainPage::Transfer:
            transfer->SetBounds(content_x, nav_height, content_width, content_height);
            break;
        case MainPage::Editor:
            editor->SetBounds(content_x, nav_height, content_width, content_height);
            break;
        case MainPage::PortForward:
            port_forward->SetBounds(0, nav_height, width, content_height);
            break;
        case MainPage::Settings:
            break;
        }
        BuildChrome(width, height, nav_height);
    }
};

MainWindow::MainWindow() : impl_(std::make_unique<Impl>()) {}
MainWindow::~MainWindow() = default;

bool MainWindow::Create(HINSTANCE instance, int show_command) {
    // 先确保 DPI 与通用控件环境，再注册项目窗口类；这里只创建顶层窗口，具体页面在
    // WM_CREATE 中按同一 UI 线程构造，避免页面持有尚未有效的父 HWND。
    EnablePerMonitorDpi();
    INITCOMMONCONTROLSEX controls{sizeof(controls)};
    controls.dwICC = ICC_WIN95_CLASSES | ICC_TAB_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&controls);

    impl_->instance = instance;
    if (!impl_->RegisterWindowClass()) {
        return false;
    }
    impl_->hwnd = CreateWindowExW(WS_EX_APPWINDOW,
                                  kMainWindowClass,
                                  L"SLink",
                                  WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX |
                                      WS_SYSMENU | WS_CLIPCHILDREN,
                                  CW_USEDEFAULT,
                                  CW_USEDEFAULT,
                                  1440,
                                  900,
                                  nullptr,
                                  nullptr,
                                  instance,
                                  impl_.get());
    if (!impl_->hwnd) {
        return false;
    }
    detail::EnableDarkTitleBar(impl_->hwnd);
    ShowWindow(impl_->hwnd, show_command);
    UpdateWindow(impl_->hwnd);
    return true;
}

int MainWindow::RunMessageLoop() {
    // 消息循环先处理跨页面全局快捷键和 Enter 路由，再交给 IsDialogMessage 处理焦点
    // 导航，最后才 Translate/Dispatch。这样快捷键不会额外产生 WM_CHAR 发送到终端。
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        const bool split_shortcut =
            impl_->active_page == MainPage::Terminal && message.message == WM_KEYDOWN &&
            (GetKeyState(VK_CONTROL) & 0x8000) != 0 && (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        if (split_shortcut) {
            if (message.wParam == 'D') {
                impl_->terminal->AddHorizontalPane();
                continue;
            }
            if (message.wParam == 'W') {
                impl_->terminal->CloseActivePane();
                continue;
            }
        }
        if (impl_->active_page == MainPage::Transfer && message.message == WM_KEYDOWN &&
            message.wParam == VK_RETURN && impl_->transfer->HandleEnterKey(message.hwnd)) {
            continue;
        }
        if (impl_->active_page == MainPage::PortForward && message.message == WM_KEYDOWN &&
            message.wParam == VK_RETURN && impl_->port_forward->HandleEnterKey(message.hwnd)) {
            continue;
        }
        if (!IsDialogMessageW(impl_->hwnd, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    return static_cast<int>(message.wParam);
}

HWND MainWindow::hwnd() const noexcept {
    return impl_->hwnd;
}

MainPage MainWindow::active_page() const noexcept {
    return impl_->active_page;
}

void MainWindow::ShowPage(MainPage page) {
    impl_->Navigate(page);
}

void MainWindow::SetServerManagerCallback(std::function<void()> callback) {
    impl_->server_manager_callback = std::move(callback);
}

void MainWindow::SetServerSidebarItems(std::vector<ServerSidebarGroup> groups,
                                       std::vector<ServerSidebarItem> servers) {
    std::ranges::sort(groups, [](const ServerSidebarGroup& left, const ServerSidebarGroup& right) {
        if (left.sort_order != right.sort_order) {
            return left.sort_order < right.sort_order;
        }
        const int compared =
            CompareStringOrdinal(left.label.c_str(), -1, right.label.c_str(), -1, TRUE);
        return compared == CSTR_LESS_THAN || (compared == CSTR_EQUAL && left.id < right.id);
    });
    std::ranges::sort(servers, [](const ServerSidebarItem& left, const ServerSidebarItem& right) {
        if (left.fixed_first != right.fixed_first) {
            return left.fixed_first;
        }
        return CompareStringOrdinal(left.label.c_str(), -1, right.label.c_str(), -1, TRUE) ==
               CSTR_LESS_THAN;
    });
    if (!impl_->selected_server_id.empty() &&
        std::ranges::none_of(servers, [this](const ServerSidebarItem& item) {
            return item.id == impl_->selected_server_id;
        })) {
        impl_->selected_server_id.clear();
    }
    TreeView_SelectItem(impl_->server_list, nullptr);
    impl_->server_groups = std::move(groups);
    impl_->server_items = std::move(servers);
    impl_->PopulateServerList(true);
}

void MainWindow::SetCollapsedServerGroups(
    std::vector<std::string> group_ids,
    std::function<bool(const std::vector<std::string>&)> changed) {
    impl_->collapsed_server_groups.clear();
    for (auto& id : group_ids) {
        impl_->collapsed_server_groups.insert(id == kUngroupedCollapseKey ? std::string{}
                                                                          : std::move(id));
    }
    impl_->collapsed_groups_changed = std::move(changed);
    impl_->PopulateServerList();
}

bool MainWindow::SelectServerSidebarItem(const std::string_view server_id) {
    return impl_->SelectServer(server_id);
}

void MainWindow::SetThemePreference(const theme::Mode selected,
                                    std::function<bool(theme::Mode)> changed_callback) {
    impl_->selected_theme = selected;
    impl_->theme_changed_callback = std::move(changed_callback);
    impl_->CommitLayoutNow();
}

bool MainWindow::PostUiTask(std::function<void()> callback) {
    if (!callback || !impl_->hwnd || !IsWindow(impl_->hwnd)) {
        return false;
    }
    {
        std::scoped_lock lock(impl_->ui_task_mutex);
        impl_->ui_tasks.push_back(std::move(callback));
    }
    return PostMessageW(impl_->hwnd, kUiTaskMessage, 0, 0) != FALSE;
}

TerminalPage& MainWindow::terminal_page() noexcept {
    return *impl_->terminal;
}

TransferPage& MainWindow::transfer_page() noexcept {
    return *impl_->transfer;
}

EditorPage& MainWindow::editor_page() noexcept {
    return *impl_->editor;
}

PortForwardPage& MainWindow::port_forward_page() noexcept {
    return *impl_->port_forward;
}

} // namespace SLink::ui
