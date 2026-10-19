// 终端页面标签组和左右分屏实现，包含标签拖放合并/拆分、分隔条落点预览及活动终端焦点。
#include "ui/terminal_page.hpp"

#include "ui/retained_ui.hpp"
#include "ui/selector_control.hpp"
#include "ui/terminal_tab_viewport.hpp"
#include "ui/terminal_control.hpp"
#include "ui/win32_ui_internal.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <windowsx.h>
#include <commctrl.h>

namespace SLink::ui {
namespace {

#ifndef WM_DPICHANGED_AFTERPARENT
#define WM_DPICHANGED_AFTERPARENT 0x02E3
#endif

constexpr wchar_t kTerminalPageClass[] = L"SLink.TerminalPage";
constexpr wchar_t kTabDragPreviewClass[] = L"SLink.TerminalTabDragPreview";
constexpr wchar_t kResizeGuideClass[] = L"SLink.TerminalResizeGuide";
constexpr int kResizeGuideId = 2106;
constexpr COLORREF kResizeGuideTransparentColor = RGB(1, 2, 3);
constexpr int kTabActionFirstId = 2600;
constexpr int kNativeControlFirstId = 3000;
constexpr UINT_PTR kTabMotionTimerId = 1;
constexpr UINT_PTR kDragScrollTimerId = 2;
constexpr ULONGLONG kTabMotionDurationMs = 120;

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

enum class TabHitKind { activate, close, scroll_left, scroll_right };

struct TabHitAction {
    int id{};
    TabHitKind kind{};
    std::size_t pane_index{};
    unsigned group{};
};

struct PaneTabIds {
    int activate{};
    int close{};
};

ATOM RegisterPageClass(HINSTANCE instance, WNDPROC proc) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kTerminalPageClass;
    const ATOM atom = RegisterClassExW(&wc);
    return atom != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

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
        const int dash_height = std::max(2, detail::ScaleForDpi(3, detail::WindowDpi(window)));
        const int step =
            dash_height + std::max(2, detail::ScaleForDpi(3, detail::WindowDpi(window)));
        for (int y = rect.top; y < rect.bottom; y += step) {
            RECT mark{rect.left, y, rect.right, std::min<LONG>(y + dash_height, rect.bottom)};
            FillRect(dc, &mark, dash);
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

} // namespace

// 每个 Pane 代表一个标签组而不是一个连接；标签可在组间移动。分屏拖动只记录最终比例
// 并绘制一次参考线，松开后才重排终端控件，避免连续 resize PTY 和昂贵重绘。
struct TerminalPage::Impl {
    enum class ResizeMode {
        none,
        columns,
    };

    struct TabInsertionTarget {
        std::size_t slot{};
        float x{};
        float top{};
        float bottom{};
    };

    HINSTANCE instance{};
    HWND hwnd{};
    detail::UiSurface surface;
    HWND tab_drag_preview{};
    detail::UiSurface tab_drag_surface;
    SIZE tab_drag_preview_size{};
    POINT tab_drag_preview_offset{};
    unsigned columns{1};
    bool split_active{};
    std::vector<std::string> server_ids;
    std::vector<std::wstring> server_tab_names;
    std::vector<bool> server_fixed_first;
    HWND resize_guide{};
    HWND tab_tooltip{};
    std::wstring tab_tooltip_text;
    std::optional<std::size_t> selected_server;
    std::vector<std::unique_ptr<TerminalControl>> panes;
    std::vector<std::wstring> pane_titles;
    std::vector<ViewState> pane_view_states;
    std::vector<TabHitAction> tab_hit_actions;
    std::vector<int> pane_tab_element_ids;
    // 稳定的自绘点击 ID 跟随标签移动；连接状态重绘时不会把一次点击映射到别的标签。
    std::vector<PaneTabIds> pane_tab_ids;
    int next_tab_action_id{kTabActionFirstId + 4};
    std::array<std::size_t, 2> tab_scroll_first{};
    std::array<TerminalControl*, 2> reveal_tab{};
    std::array<RECT, 2> tab_visible_regions{};
    std::array<bool, 2> tab_overflow{};
    std::array<std::size_t, 2> tab_visible_counts{};
    int next_native_control_id{kNativeControlFirstId};
    POINT tab_drag_current{};
    std::optional<std::pair<unsigned, int>> drag_scroll_direction;
    ULONGLONG drag_scroll_next{};
    TerminalControl* animated_tab{};
    bool tab_motion_enabled{ClientAnimationsEnabled()};
    bool tab_motion_active{};
    ULONGLONG tab_motion_start{};
    float tab_motion_progress{1.0F};
    std::vector<bool> pane_assigned;
    std::vector<bool> pane_encoding_selectable;
    std::vector<HWND> encoding_selectors;
    std::vector<unsigned> pane_groups;
    HFONT selector_font{};
    mutable std::array<std::size_t, 2> group_active{0, 0};
    mutable std::size_t active_pane{};
    mutable RECT pane_area{};
    mutable std::vector<RECT> pane_rects;
    std::vector<float> column_weights;
    ResizeMode resize_mode{ResizeMode::none};
    std::size_t resize_index{};
    POINT resize_origin{};
    std::vector<float> resize_start_weights;
    int resize_preview_position{-1};
    bool tab_drag_armed{};
    bool tab_dragging{};
    int dragged_tab{-1};
    POINT tab_drag_origin{};
    SplitDropZone visible_drop_zone{SplitDropZone::none};
    std::optional<std::size_t> visible_drop_pane;
    std::optional<TabInsertionTarget> visible_tab_insertion;
    ServerActivatedCallback server_activated;
    PaneClosingCallback pane_closing;
    PaneCreatedCallback pane_created;

    ~Impl() {
        if (tab_drag_preview && IsWindow(tab_drag_preview)) {
            DestroyWindow(tab_drag_preview);
        }
        DeleteObject(selector_font);
    }

    void RecreateSelectorFont() {
        DeleteObject(selector_font);
        selector_font = detail::CreateUiFont(detail::WindowDpi(hwnd), 8);
        for (const HWND selector : encoding_selectors) {
            if (selector) {
                SendMessageW(selector, WM_SETFONT, reinterpret_cast<WPARAM>(selector_font), TRUE);
            }
        }
    }

    bool EncodingSelectable(const std::size_t index) const noexcept {
        return index < pane_encoding_selectable.size() && pane_encoding_selectable[index];
    }

    void SetPaneEncoding(const std::size_t index, const terminal::TerminalEncoding encoding) {
        if (index >= panes.size()) {
            return;
        }
        panes[index]->SetTransportEncoding(encoding);
        if (index < encoding_selectors.size() && encoding_selectors[index]) {
            SendMessageW(encoding_selectors[index],
                         CB_SETCURSEL,
                         encoding == terminal::TerminalEncoding::Utf8 ? 0 : 1,
                         0);
        }
    }

    unsigned GroupOf(std::size_t index) const noexcept {
        return index < pane_groups.size() && split_active ? std::min(pane_groups[index], 1U) : 0U;
    }

    std::vector<std::size_t> TabsInGroup(unsigned group) const {
        std::vector<std::size_t> result;
        for (std::size_t index = 0; index < panes.size(); ++index) {
            if (GroupOf(index) == group) {
                result.push_back(index);
            }
        }
        return result;
    }

    std::optional<std::size_t> FirstTabInGroup(unsigned group) const noexcept {
        for (std::size_t index = 0; index < panes.size(); ++index) {
            if (GroupOf(index) == group) {
                return index;
            }
        }
        return std::nullopt;
    }

    std::size_t ActiveTabInGroup(unsigned group) const noexcept {
        const std::size_t candidate = group_active[std::min(group, 1U)];
        if (candidate < panes.size() && GroupOf(candidate) == group) {
            return candidate;
        }
        const auto first = FirstTabInGroup(group);
        return first.value_or(0);
    }

    void NormalizeGroups() {
        pane_groups.resize(panes.size(), 0U);
        if (panes.empty()) {
            split_active = false;
            columns = 1;
            tab_scroll_first = {};
            reveal_tab = {};
            group_active = {0, 0};
            column_weights.assign(1, 1.0F);
            return;
        }

        active_pane = std::min(active_pane, panes.size() - 1);
        if (!split_active || panes.size() < 2) {
            split_active = false;
            columns = 1;
            tab_scroll_first[1] = 0;
            reveal_tab[1] = nullptr;
            std::fill(pane_groups.begin(), pane_groups.end(), 0U);
            group_active[0] = active_pane;
            group_active[1] = 0;
            column_weights.assign(1, 1.0F);
            return;
        }

        for (unsigned& group : pane_groups) {
            group = std::min(group, 1U);
        }
        const auto left = FirstTabInGroup(0);
        const auto right = FirstTabInGroup(1);
        if (!left || !right) {
            split_active = false;
            columns = 1;
            tab_scroll_first[1] = 0;
            reveal_tab[1] = nullptr;
            std::fill(pane_groups.begin(), pane_groups.end(), 0U);
            group_active[0] = active_pane;
            group_active[1] = 0;
            column_weights.assign(1, 1.0F);
            return;
        }

        columns = 2;
        for (unsigned group = 0; group < 2; ++group) {
            if (group_active[group] >= panes.size() || GroupOf(group_active[group]) != group) {
                group_active[group] = group == 0 ? *left : *right;
            }
        }
        if (column_weights.size() != 2) {
            column_weights.assign(2, 1.0F);
        }
    }

    void MoveTabToGroup(std::size_t index, unsigned group) {
        if (index >= pane_groups.size()) {
            return;
        }
        pane_groups[index] = std::min(group, 1U);
        group_active[pane_groups[index]] = index;
        active_pane = index;
        NormalizeGroups();
    }

    static LRESULT CALLBACK TabDragPreviewProc(HWND window,
                                               UINT message,
                                               WPARAM wparam,
                                               LPARAM lparam) {
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            self = static_cast<Impl*>(create->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) {
            return DefWindowProcW(window, message, wparam, lparam);
        }
        switch (message) {
        case WM_SIZE:
            self->tab_drag_surface.Resize(LOWORD(lparam), HIWORD(lparam));
            return 0;
        case WM_DPICHANGED_AFTERPARENT:
            self->tab_drag_surface.DpiChanged();
            return 0;
        case WM_PAINT:
            self->tab_drag_surface.Paint(detail::UiColor(detail::kWindowColor));
            return 0;
        case WM_ERASEBKGND:
            return 1;
        default:
            return DefWindowProcW(window, message, wparam, lparam);
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
        case WM_TIMER:
            if (wparam == kTabMotionTimerId) {
                self->TickTabMotion();
                return 0;
            }
            if (wparam == kDragScrollTimerId) {
                self->TickDragScroll();
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
        case WM_DESTROY:
            KillTimer(window, kTabMotionTimerId);
            KillTimer(window, kDragScrollTimerId);
            break;
        case WM_NOTIFY:
            if (self->tab_tooltip && reinterpret_cast<NMHDR*>(lparam)->hwndFrom ==
                                         self->tab_tooltip &&
                reinterpret_cast<NMHDR*>(lparam)->code == TTN_GETDISPINFOW) {
                auto* info = reinterpret_cast<NMTTDISPINFOW*>(lparam);
                POINT cursor{};
                GetCursorPos(&cursor);
                ScreenToClient(window, &cursor);
                const auto* action = self->ActionForId(self->ElementAt(cursor));
                self->tab_tooltip_text.clear();
                if (!self->tab_dragging && action && action->kind == TabHitKind::activate &&
                    action->pane_index < self->pane_titles.size()) {
                    self->tab_tooltip_text = self->pane_titles[action->pane_index];
                }
                info->lpszText = self->tab_tooltip_text.data();
                return 0;
            }
            break;
        case WM_SIZE:
            self->surface.Resize(LOWORD(lparam), HIWORD(lparam));
            self->Layout(LOWORD(lparam), HIWORD(lparam));
            return 0;
        case WM_DPICHANGED_AFTERPARENT: {
            self->surface.DpiChanged();
            self->RecreateSelectorFont();
            RECT rect{};
            GetClientRect(window, &rect);
            self->Layout(rect.right, rect.bottom);
            return 0;
        }
        case WM_COMMAND:
            if (HIWORD(wparam) == CBN_SELCHANGE) {
                const HWND selector = reinterpret_cast<HWND>(lparam);
                const auto found = std::find(
                    self->encoding_selectors.begin(), self->encoding_selectors.end(), selector);
                if (found != self->encoding_selectors.end()) {
                    const std::size_t index = static_cast<std::size_t>(
                        std::distance(self->encoding_selectors.begin(), found));
                    const LRESULT selected = SendMessageW(selector, CB_GETCURSEL, 0, 0);
                    self->SetPaneEncoding(index,
                                          selected == 1 ? terminal::TerminalEncoding::Gb18030
                                                        : terminal::TerminalEncoding::Utf8);
                    if (index < self->panes.size()) {
                        SetFocus(self->panes[index]->hwnd());
                    }
                    return 0;
                }
            }
            break;
        case WM_MOUSEMOVE:
            if (self->resize_mode != ResizeMode::none) {
                self->UpdateResize({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
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
            if (self->HandlePointerDown({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)})) {
                return 0;
            }
            break;
        case WM_LBUTTONUP:
            if (self->resize_mode != ResizeMode::none) {
                self->EndResize();
                return 0;
            }
            self->HandlePointerUp({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
            return 0;
        case WM_CAPTURECHANGED:
            if (self->resize_mode != ResizeMode::none) {
                ShowWindow(self->resize_guide, SW_HIDE);
                if (self->resize_start_weights.size() == 2) {
                    self->column_weights = self->resize_start_weights;
                }
            }
            self->resize_preview_position = -1;
            self->resize_mode = ResizeMode::none;
            self->resize_start_weights.clear();
            self->CancelDrag();
            self->surface.CancelPointer();
            return 0;
        case WM_SETCURSOR:
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
        case WM_DRAWITEM: {
            const auto* draw = reinterpret_cast<const DRAWITEMSTRUCT*>(lparam);
            if (draw && draw->CtlID == kResizeGuideId && draw->hDC) {
                const HBRUSH background = CreateSolidBrush(kResizeGuideTransparentColor);
                FillRect(draw->hDC, &draw->rcItem, background);
                DeleteObject(background);
                const HBRUSH dash = CreateSolidBrush(detail::kMutedTextColor);
                const int dash_height =
                    std::max(2, detail::ScaleForDpi(3, detail::WindowDpi(window)));
                const int step =
                    dash_height + std::max(2, detail::ScaleForDpi(3, detail::WindowDpi(window)));
                for (int y = draw->rcItem.top; y < draw->rcItem.bottom; y += step) {
                    RECT mark{draw->rcItem.left,
                              y,
                              draw->rcItem.right,
                              std::min<LONG>(y + dash_height, draw->rcItem.bottom)};
                    FillRect(draw->hDC, &mark, dash);
                }
                DeleteObject(dash);
                return TRUE;
            }
            break;
        }
        case WM_PAINT:
            self->surface.Paint(detail::UiColor(detail::kWindowColor));
            return 0;
        case WM_CTLCOLOREDIT: {
            const HDC dc = reinterpret_cast<HDC>(wparam);
            SetTextColor(dc, detail::kTextColor);
            SetBkColor(dc, detail::kPanelColor);
            return reinterpret_cast<LRESULT>(detail::PanelThemeBrush());
        }
        case WM_CTLCOLORBTN: {
            const HDC dc = reinterpret_cast<HDC>(wparam);
            SetTextColor(dc, detail::kTextColor);
            SetBkColor(dc, detail::kPanelColor);
            return reinterpret_cast<LRESULT>(detail::PanelThemeBrush());
        }
        case WM_ERASEBKGND:
            return 1;
        default:
            break;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    int ElementAt(POINT point) const noexcept {
        for (auto iterator = surface.elements().rbegin(); iterator != surface.elements().rend();
             ++iterator) {
            const auto& element = *iterator;
            if (element.id != 0 && element.visible && element.enabled &&
                point.x >= element.bounds.left && point.x < element.bounds.right &&
                point.y >= element.bounds.top && point.y < element.bounds.bottom) {
                return element.id;
            }
        }
        return 0;
    }

    int AddTabAction(TabHitAction action) {
        switch (action.kind) {
        case TabHitKind::activate:
            action.id = pane_tab_ids[action.pane_index].activate;
            break;
        case TabHitKind::close:
            action.id = pane_tab_ids[action.pane_index].close;
            break;
        case TabHitKind::scroll_left:
            action.id = kTabActionFirstId + static_cast<int>(action.group) * 2;
            break;
        case TabHitKind::scroll_right:
            action.id = kTabActionFirstId + static_cast<int>(action.group) * 2 + 1;
            break;
        }
        tab_hit_actions.push_back(action);
        return action.id;
    }

    [[nodiscard]] const TabHitAction* ActionForId(const int id) const noexcept {
        const auto found = std::ranges::find(tab_hit_actions, id, &TabHitAction::id);
        return found == tab_hit_actions.end() ? nullptr : &*found;
    }

    void RequestReveal(const std::size_t index) noexcept {
        if (index < panes.size()) {
            for (TerminalControl*& pending : reveal_tab) {
                if (pending == panes[index].get()) {
                    pending = nullptr;
                }
            }
            reveal_tab[GroupOf(index)] = panes[index].get();
        }
    }

    void ShowPaneCreationFailure() const {
        const HWND previous_focus = GetFocus();
        MessageBoxW(hwnd, L"无法创建终端，请检查系统资源。", L"SLink", MB_OK | MB_ICONERROR);
        if (previous_focus && IsWindow(previous_focus)) {
            SetFocus(previous_focus);
        }
    }

    std::pair<ResizeMode, std::size_t> ResizeHandleAt(POINT point) const noexcept {
        const UINT dpi = detail::WindowDpi(hwnd);
        const int hit = detail::ScaleForDpi(9, dpi);
        if (!split_active || pane_rects.size() < 2) {
            return {ResizeMode::none, 0};
        }
        if (const auto left = FirstTabInGroup(0)) {
            const RECT& pane = pane_rects[*left];
            if (std::abs(point.x - pane.right) <= hit && point.y >= pane_area.top &&
                point.y <= pane_area.bottom) {
                return {ResizeMode::columns, 0};
            }
        }
        return {ResizeMode::none, 0};
    }

    bool BeginResize(POINT point) {
        const auto [mode, index] = ResizeHandleAt(point);
        if (mode == ResizeMode::none) {
            return false;
        }
        resize_mode = mode;
        resize_index = index;
        resize_origin = point;
        resize_start_weights.clear();
        if (mode == ResizeMode::columns) {
            resize_start_weights.assign(2, 1.0F);
            for (unsigned group = 0; group < 2; ++group) {
                const auto tab = FirstTabInGroup(group);
                if (tab) {
                    resize_start_weights[group] =
                        static_cast<float>(pane_rects[*tab].right - pane_rects[*tab].left);
                }
            }
            if (const auto left = FirstTabInGroup(0)) {
                resize_preview_position = pane_rects[*left].right;
            }
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
        if (resize_mode != ResizeMode::columns || resize_preview_position < 0) {
            ShowWindow(resize_guide, SW_HIDE);
            return;
        }
        const int thickness = std::max(1, detail::ScaleForDpi(2, detail::WindowDpi(hwnd)));
        const int guide_top = pane_area.top;
        const int guide_bottom = pane_area.bottom;
        POINT origin{resize_preview_position - thickness / 2, guide_top};
        ClientToScreen(hwnd, &origin);
        SetWindowPos(resize_guide,
                     HWND_TOPMOST,
                     origin.x,
                     origin.y,
                     thickness,
                     std::max(1, guide_bottom - guide_top),
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        InvalidateRect(resize_guide, nullptr, FALSE);
        UpdateWindow(resize_guide);
    }

    void UpdateResize(POINT point) {
        RECT client{};
        GetClientRect(hwnd, &client);
        const UINT dpi = detail::WindowDpi(hwnd);
        const int minimum_pane = detail::ScaleForDpi(120, dpi);
        if (resize_mode == ResizeMode::columns && resize_index + 1 < resize_start_weights.size()) {
            const int delta = point.x - resize_origin.x;
            const float combined =
                resize_start_weights[resize_index] + resize_start_weights[resize_index + 1];
            const float first =
                std::clamp(resize_start_weights[resize_index] + static_cast<float>(delta),
                           static_cast<float>(minimum_pane),
                           std::max(static_cast<float>(minimum_pane), combined - minimum_pane));
            const int next_preview = pane_area.left + static_cast<int>(std::lround(first));
            if (next_preview != resize_preview_position) {
                resize_preview_position = next_preview;
                ToggleResizeGuide();
            }
            auto weights = resize_start_weights;
            weights[resize_index] = first;
            weights[resize_index + 1] = combined - first;
            column_weights = std::move(weights);
        }
        // Keep terminal grids stable while the divider is dragged and commit
        // the final geometry on mouse-up.
    }

    void EndResize() {
        ShowWindow(resize_guide, SW_HIDE);
        resize_preview_position = -1;
        resize_mode = ResizeMode::none;
        RECT client{};
        GetClientRect(hwnd, &client);
        Layout(client.right, client.bottom);
        RedrawWindow(hwnd, &pane_area, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_NOERASE);
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
        if (resize_mode == ResizeMode::columns || mode == ResizeMode::columns) {
            SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
            return true;
        }
        return false;
    }

    bool HandlePointerDown(POINT point) {
        const int id = ElementAt(point);
        if (const auto* action = ActionForId(id);
            action && action->kind == TabHitKind::activate) {
            dragged_tab = static_cast<int>(action->pane_index);
            tab_drag_origin = point;
            tab_drag_current = point;
            tab_drag_armed = true;
            tab_dragging = false;
        }
        return surface.PointerDown(point);
    }

    void HandlePointerMove(POINT point, WPARAM key_state) {
        surface.PointerMove(point);
        if (!tab_drag_armed || (key_state & MK_LBUTTON) == 0) {
            return;
        }
        if (!tab_dragging &&
            (std::abs(point.x - tab_drag_origin.x) >= GetSystemMetrics(SM_CXDRAG) ||
             std::abs(point.y - tab_drag_origin.y) >= GetSystemMetrics(SM_CYDRAG))) {
            tab_dragging = true;
            BeginTabDragPreview(point);
        }
        if (!tab_dragging) {
            return;
        }
        tab_drag_current = point;
        UpdateEdgeScroll(point);
        MoveTabDragPreview(point);
        const auto next_insertion = TabInsertionAt(point, static_cast<std::size_t>(dragged_tab));
        const auto next_pane = next_insertion ? std::nullopt : PaneAt(point);
        const SplitDropZone next = next_insertion ? SplitDropZone::none : DropZoneAt(point);
        if (next != visible_drop_zone || next_pane != visible_drop_pane ||
            !SameInsertion(next_insertion, visible_tab_insertion)) {
            const bool strip_only = next == SplitDropZone::none &&
                                    visible_drop_zone == SplitDropZone::none;
            visible_drop_zone = next;
            visible_drop_pane = next_pane;
            visible_tab_insertion = next_insertion;
            BuildElements();
            if (strip_only) {
                InvalidateTabStrip();
            } else {
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
    }

    void HandlePointerUp(POINT point) {
        const bool dragged = tab_dragging;
        const int tab = dragged_tab;
        const auto clicked = surface.PointerUp(point);
        if (dragged && tab >= 0) {
            HandleTabDrop(static_cast<std::size_t>(tab), point);
            CancelDrag();
            return;
        }
        CancelDrag();
        if (clicked) {
            HandleClick(*clicked);
        }
    }

    void HandleClick(int id) {
        const auto* found = ActionForId(id);
        if (!found) {
            return;
        }
        const TabHitAction action = *found;
        switch (action.kind) {
        case TabHitKind::activate:
            Focus(action.pane_index);
            break;
        case TabHitKind::close:
            ClosePane(action.pane_index);
            break;
        case TabHitKind::scroll_left:
        case TabHitKind::scroll_right:
            ScrollGroup(action.group, action.kind == TabHitKind::scroll_left ? -1 : 1, true);
            break;
        }
    }

    bool ScrollGroup(unsigned group, int direction, bool restore_focus) {
        if (group > 1 || !tab_overflow[group]) {
            return false;
        }
        const std::size_t total = TabsInGroup(group).size();
        const std::size_t count = tab_visible_counts[group];
        const std::size_t maximum = total > count ? total - count : 0;
        std::size_t& first = tab_scroll_first[group];
        if (direction < 0 && first > 0) {
            --first;
        } else if (direction > 0 && first < maximum) {
            ++first;
        } else {
            return false;
        }
        BuildElements();
        InvalidateTabStrip();
        if (restore_focus && active_pane < panes.size()) {
            SetFocus(panes[active_pane]->hwnd());
        }
        return true;
    }

    [[nodiscard]] std::optional<std::pair<unsigned, int>> ScrollZoneAt(POINT point) const {
        const auto strip = TabStripAt(point);
        if (!strip) {
            return std::nullopt;
        }
        const unsigned group = GroupOf(*strip);
        if (!tab_overflow[group]) {
            return std::nullopt;
        }
        if (const auto* action = ActionForId(ElementAt(point)); action &&
            (action->kind == TabHitKind::scroll_left ||
             action->kind == TabHitKind::scroll_right)) {
            return {{group, action->kind == TabHitKind::scroll_left ? -1 : 1}};
        }
        const RECT& region = tab_visible_regions[group];
        const int threshold = detail::ScaleForDpi(16, detail::WindowDpi(hwnd));
        if (point.x <= region.left + threshold) {
            return {{group, -1}};
        }
        if (point.x >= region.right - threshold) {
            return {{group, 1}};
        }
        return std::nullopt;
    }

    void UpdateEdgeScroll(POINT point) {
        const auto requested = ScrollZoneAt(point);
        if (requested == drag_scroll_direction) {
            return;
        }
        drag_scroll_direction = requested;
        KillTimer(hwnd, kDragScrollTimerId);
        if (requested) {
            drag_scroll_next = GetTickCount64() + 300;
            SetTimer(hwnd, kDragScrollTimerId, 50, nullptr);
        }
    }

    void TickDragScroll() {
        if (!tab_dragging || !drag_scroll_direction) {
            KillTimer(hwnd, kDragScrollTimerId);
            return;
        }
        const ULONGLONG now = GetTickCount64();
        if (now < drag_scroll_next) {
            return;
        }
        const auto [group, direction] = *drag_scroll_direction;
        if (!ScrollGroup(group, direction, false)) {
            KillTimer(hwnd, kDragScrollTimerId);
            drag_scroll_direction.reset();
            return;
        }
        drag_scroll_next = now + 150;
        HandlePointerMove(tab_drag_current, MK_LBUTTON);
    }

    void CancelDrag() {
        const bool had_drop_visual =
            visible_drop_zone != SplitDropZone::none || visible_tab_insertion.has_value();
        tab_drag_armed = false;
        tab_dragging = false;
        drag_scroll_direction.reset();
        KillTimer(hwnd, kDragScrollTimerId);
        dragged_tab = -1;
        visible_drop_pane.reset();
        visible_tab_insertion.reset();
        if (tab_drag_preview) {
            ShowWindow(tab_drag_preview, SW_HIDE);
        }
        if (had_drop_visual) {
            visible_drop_zone = SplitDropZone::none;
            BuildElements();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
    }

    bool EnsureTabDragPreview() {
        if (tab_drag_preview) {
            return true;
        }
        WNDCLASSEXW wc{sizeof(wc)};
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = &TabDragPreviewProc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_SIZEALL);
        wc.lpszClassName = kTabDragPreviewClass;
        const ATOM atom = RegisterClassExW(&wc);
        if (atom == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return false;
        }
        tab_drag_preview =
            CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
                            kTabDragPreviewClass,
                            L"",
                            WS_POPUP,
                            0,
                            0,
                            1,
                            1,
                            hwnd,
                            nullptr,
                            instance,
                            this);
        if (!tab_drag_preview || !tab_drag_surface.Initialize(tab_drag_preview)) {
            return false;
        }
        SetLayeredWindowAttributes(tab_drag_preview, 0, 224, LWA_ALPHA);
        return true;
    }

    void BeginTabDragPreview(POINT point) {
        if (dragged_tab < 0 || static_cast<std::size_t>(dragged_tab) >= pane_titles.size() ||
            !EnsureTabDragPreview()) {
            return;
        }
        const UINT dpi = detail::WindowDpi(hwnd);
        int preview_width = detail::ScaleForDpi(110, dpi);
        int preview_height = detail::ScaleForDpi(30, dpi);
        tab_drag_preview_offset = {detail::ScaleForDpi(30, dpi), detail::ScaleForDpi(18, dpi)};
        const int tab_id = static_cast<std::size_t>(dragged_tab) < pane_tab_element_ids.size()
                               ? pane_tab_element_ids[dragged_tab]
                               : 0;
        for (const auto& element : surface.elements()) {
            if (element.id != tab_id || element.role != detail::UiRole::file_tab) {
                continue;
            }
            preview_width =
                std::max(1, static_cast<int>(element.bounds.right - element.bounds.left));
            preview_height =
                std::max(1, static_cast<int>(element.bounds.bottom - element.bounds.top));
            tab_drag_preview_offset.x = std::clamp<int>(static_cast<int>(tab_drag_origin.x) -
                                                            static_cast<int>(element.bounds.left),
                                                        0,
                                                        preview_width - 1);
            tab_drag_preview_offset.y = std::clamp<int>(static_cast<int>(tab_drag_origin.y) -
                                                            static_cast<int>(element.bounds.top),
                                                        0,
                                                        preview_height - 1);
            break;
        }
        tab_drag_preview_size = {preview_width, preview_height};
        tab_drag_surface.ClearElements();
        tab_drag_surface.Add({
            .role = detail::UiRole::file_tab,
            .bounds = detail::UiRect(
                0.0F, 0.0F, static_cast<float>(preview_width), static_cast<float>(preview_height)),
            .text = pane_titles[static_cast<std::size_t>(dragged_tab)],
            .icon = detail::UiIcon::terminal,
            .alignment = detail::UiTextAlign::leading,
            .selected = true,
            .compact_text = true,
        });
        const float close_size = static_cast<float>(detail::ScaleForDpi(18, dpi));
        tab_drag_surface.Add({
            .role = detail::UiRole::tab_close,
            .bounds = detail::UiRect(
                static_cast<float>(preview_width) - close_size - detail::ScaleForDpi(4, dpi),
                (preview_height - close_size) / 2.0F,
                static_cast<float>(preview_width) - detail::ScaleForDpi(4, dpi),
                (preview_height + close_size) / 2.0F),
            .icon = detail::UiIcon::close,
            .alignment = detail::UiTextAlign::center,
        });
        MoveTabDragPreview(point);
    }

    void MoveTabDragPreview(POINT point) {
        if (!tab_drag_preview || tab_drag_preview_size.cx <= 0 || tab_drag_preview_size.cy <= 0) {
            return;
        }
        POINT screen = point;
        ClientToScreen(hwnd, &screen);
        SetWindowPos(tab_drag_preview,
                     HWND_TOPMOST,
                     screen.x - tab_drag_preview_offset.x,
                     screen.y - tab_drag_preview_offset.y,
                     tab_drag_preview_size.cx,
                     tab_drag_preview_size.cy,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOCOPYBITS);
        RedrawWindow(
            tab_drag_preview, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
    }

    std::optional<std::size_t> PaneAt(POINT point) const noexcept {
        if (pane_rects.size() != panes.size() || panes.empty()) {
            return std::nullopt;
        }
        if (!split_active) {
            const std::size_t selected = std::min(active_pane, panes.size() - 1);
            return PtInRect(&pane_rects[selected], point) ? std::optional<std::size_t>(selected)
                                                          : std::nullopt;
        }
        for (unsigned group = 0; group < 2; ++group) {
            const std::size_t index = ActiveTabInGroup(group);
            if (index < pane_rects.size() && PtInRect(&pane_rects[index], point)) {
                return index;
            }
        }
        return std::nullopt;
    }

    std::optional<std::size_t> TabStripAt(POINT point) const noexcept {
        if (pane_rects.size() != panes.size() || panes.empty()) {
            return std::nullopt;
        }
        const int tab_height = detail::ScaleForDpi(34, detail::WindowDpi(hwnd));
        const unsigned group_count = split_active ? 2U : 1U;
        for (unsigned group = 0; group < group_count; ++group) {
            const std::size_t index =
                split_active ? ActiveTabInGroup(group) : std::min(active_pane, panes.size() - 1);
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
        const RECT& visible_region = tab_visible_regions[GroupOf(source)];
        if (point.x < visible_region.left || point.x >= visible_region.right) {
            return std::nullopt;
        }

        std::vector<const detail::UiElement*> tabs;
        for (const auto& element : surface.elements()) {
            const auto* action = ActionForId(element.id);
            if (element.role != detail::UiRole::file_tab || !action ||
                action->kind != TabHitKind::activate) {
                continue;
            }
            const std::size_t index = action->pane_index;
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
        target.slot = ActionForId(edge->id)->pane_index + 1;
        target.x = edge->bounds.right;
        target.top = edge->bounds.top;
        target.bottom = edge->bounds.bottom;
        for (const detail::UiElement* tab : tabs) {
            const float midpoint = (tab->bounds.left + tab->bounds.right) / 2.0F;
            if (static_cast<float>(point.x) < midpoint) {
                target.slot = ActionForId(tab->id)->pane_index;
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

    SplitDropZone DropZoneAt(POINT point, int source_tab = -1) const noexcept {
        const int effective_source = source_tab >= 0 ? source_tab : dragged_tab;
        if (const auto tab_target = TabStripAt(point)) {
            if (split_active && effective_source >= 0 &&
                GroupOf(*tab_target) != GroupOf(static_cast<std::size_t>(effective_source))) {
                return SplitDropZone::center;
            }
            return SplitDropZone::none;
        }
        const auto target = PaneAt(point);
        if (!target) {
            return SplitDropZone::none;
        }
        const RECT& target_rect = pane_rects[*target];
        const int left = point.x - target_rect.left;
        const int right = target_rect.right - point.x;
        return left <= right ? SplitDropZone::left : SplitDropZone::right;
    }

    RECT DropZoneRect() const noexcept {
        RECT result = visible_drop_pane && *visible_drop_pane < pane_rects.size()
                          ? pane_rects[*visible_drop_pane]
                          : pane_area;
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

    void HandleTabDrop(std::size_t index, POINT point) {
        if (index >= panes.size()) {
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
        const auto target = zone == SplitDropZone::center ? TabStripAt(point) : PaneAt(point);
        if (zone == SplitDropZone::center) {
            if (split_active && target) {
                const unsigned target_group = GroupOf(*target);
                if (target_group != GroupOf(index)) {
                    MoveTabToGroup(index, target_group);
                }
                RECT rect{};
                GetClientRect(hwnd, &rect);
                Layout(rect.right, rect.bottom);
                Focus(index);
            }
            return;
        }

        if (!split_active) {
            if (panes.size() == 1) {
                if (!AppendPane()) {
                    ShowPaneCreationFailure();
                    return;
                }
                pane_groups.assign(panes.size(), 0U);
                pane_groups[1] = 1U;
                if (zone == SplitDropZone::right) {
                    pane_groups[0] = 1U;
                    pane_groups[1] = 0U;
                }
            } else {
                pane_groups.assign(panes.size(), 0U);
                const unsigned dragged_group = zone == SplitDropZone::left ? 0U : 1U;
                const unsigned remaining_group = 1U - dragged_group;
                std::fill(pane_groups.begin(), pane_groups.end(), remaining_group);
                pane_groups[index] = dragged_group;
            }
            split_active = true;
            columns = 2;
            column_weights.assign(2, 1.0F);
            group_active[0] = FirstTabInGroup(0).value_or(index);
            group_active[1] = FirstTabInGroup(1).value_or(index);
        } else {
            const unsigned target_group = zone == SplitDropZone::left ? 0U : 1U;
            if (GroupOf(index) == target_group) {
                return;
            }
            pane_groups[index] = target_group;
        }
        group_active[GroupOf(index)] = index;
        active_pane = index;
        NormalizeGroups();
        RECT rect{};
        GetClientRect(hwnd, &rect);
        Layout(rect.right, rect.bottom);
        Focus(index);
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

        std::array<TerminalControl*, 2> selected_by_group{};
        for (unsigned group = 0; group < 2; ++group) {
            const std::size_t selected = ActiveTabInGroup(group);
            if (selected < panes.size() && GroupOf(selected) == group) {
                selected_by_group[group] = panes[selected].get();
            }
        }
        TerminalControl* moved = panes[source].get();
        MoveItem(panes, source, destination);
        MoveItem(pane_titles, source, destination);
        MoveItem(pane_view_states, source, destination);
        MoveItem(pane_tab_ids, source, destination);
        MoveItem(pane_assigned, source, destination);
        MoveItem(pane_encoding_selectable, source, destination);
        MoveItem(encoding_selectors, source, destination);
        MoveItem(pane_groups, source, destination);

        const auto index_of = [&](const TerminalControl* pane) {
            const auto found = std::ranges::find_if(panes, [pane](const auto& candidate) {
                return candidate.get() == pane;
            });
            return found == panes.end()
                       ? panes.size()
                       : static_cast<std::size_t>(std::distance(panes.begin(), found));
        };
        for (unsigned group = 0; group < 2; ++group) {
            group_active[group] = index_of(selected_by_group[group]);
        }
        active_pane = index_of(moved);
        if (active_pane >= panes.size()) {
            return;
        }
        group_active[GroupOf(active_pane)] = active_pane;
        NormalizeGroups();
        RequestReveal(active_pane);
        RECT rect{};
        GetClientRect(hwnd, &rect);
        Layout(rect.right, rect.bottom);
        SetFocus(panes[active_pane]->hwnd());
    }

    bool AppendPane() {
        // 先准备全部资源，任何一步失败都不能占用或替换现有标签。
        if (next_native_control_id > std::numeric_limits<int>::max() - 2 ||
            next_tab_action_id > std::numeric_limits<int>::max() - 2) {
            return false;
        }
        const std::size_t index = panes.size();
        std::unique_ptr<TerminalControl> pane;
        std::wstring title;
        try {
            title = L"终端 " + std::to_wstring(index + 1);
            panes.reserve(index + 1);
            pane_titles.reserve(index + 1);
            pane_view_states.reserve(index + 1);
            pane_tab_ids.reserve(index + 1);
            pane_assigned.reserve(index + 1);
            pane_encoding_selectable.reserve(index + 1);
            encoding_selectors.reserve(index + 1);
            pane_groups.reserve(index + 1);
            pane = std::make_unique<TerminalControl>();
        } catch (const std::bad_alloc&) {
            return false;
        }
        if (!pane->Create(hwnd, instance, next_native_control_id)) {
            return false;
        }
        HWND encoding_selector = CreateWindowExW(
            0, selector::kClassName, L"", WS_CHILD | WS_TABSTOP | WS_CLIPSIBLINGS,
            0, 0, 0, 0, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(next_native_control_id + 1)),
            instance, nullptr);
        if (!encoding_selector) {
            return false;
        }
        SendMessageW(encoding_selector, WM_SETFONT, reinterpret_cast<WPARAM>(selector_font), FALSE);
        SendMessageW(encoding_selector, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"UTF-8"));
        SendMessageW(encoding_selector, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"GB18030"));
        SendMessageW(encoding_selector, CB_SETCURSEL, 0, 0);
        SendMessageW(encoding_selector,
                     CB_SETITEMHEIGHT,
                     0,
                     detail::ScaleForDpi(19, detail::WindowDpi(hwnd)));
        selector::SetTerminalOverlayStyle(encoding_selector, true);
        detail::ApplySelfDrawnTheme(encoding_selector);
        try {
            pane->FeedUtf8("请在左侧选择服务器并双击连接。\r\n");
        } catch (const std::bad_alloc&) {
            DestroyWindow(encoding_selector);
            return false;
        }
        TerminalControl* created = pane.get();
        const unsigned group =
            split_active && active_pane < pane_groups.size() ? GroupOf(active_pane) : 0U;
        // 上面的 reserve 保证以下追加不会重新分配；控件 ID 只在成功提交后递增。
        panes.push_back(std::move(pane));
        pane_titles.push_back(std::move(title));
        pane_view_states.push_back({ViewPhase::Idle, {}, ViewRecoveryAction::None});
        pane_tab_ids.push_back({next_tab_action_id, next_tab_action_id + 1});
        pane_assigned.push_back(false);
        pane_encoding_selectable.push_back(false);
        encoding_selectors.push_back(encoding_selector);
        pane_groups.push_back(group);
        next_native_control_id += 2;
        next_tab_action_id += 2;
        if (pane_created) {
            pane_created(index, *created);
        }
        return true;
    }

    void BuildPanes() {
        if (panes.empty()) {
            AppendPane();
        }
        active_pane = panes.empty() ? 0 : std::min(active_pane, panes.size() - 1);
        NormalizeGroups();
        RequestReveal(active_pane);
        RECT rect{};
        GetClientRect(hwnd, &rect);
        Layout(rect.right, rect.bottom);
    }

    std::size_t ActivePane() const noexcept {
        const HWND focused = GetFocus();
        for (std::size_t index = 0; index < panes.size(); ++index) {
            const HWND pane = panes[index]->hwnd();
            if (focused == pane || IsChild(pane, focused)) {
                active_pane = index;
                group_active[GroupOf(index)] = index;
                return index;
            }
        }
        return panes.empty() ? 0 : std::min(active_pane, panes.size() - 1);
    }

    void Focus(std::size_t index) {
        if (index >= panes.size()) {
            return;
        }
        active_pane = index;
        group_active[GroupOf(index)] = index;
        RequestReveal(index);
        RECT rect{};
        GetClientRect(hwnd, &rect);
        Layout(rect.right, rect.bottom);
        SetFocus(panes[index]->hwnd());
    }

    bool ActivateSelectedServer() {
        if (!selected_server || *selected_server >= server_ids.size() || !server_activated) {
            return false;
        }
        const std::size_t server_index = *selected_server;
        std::size_t pane_index = ActivePane();
        if (pane_index < pane_assigned.size() && pane_assigned[pane_index]) {
            if (!AppendPane()) {
                ShowPaneCreationFailure();
                return false;
            }
            pane_index = panes.size() - 1;
            active_pane = pane_index;
        }
        if (server_index < server_tab_names.size()) {
            if (pane_index < pane_titles.size()) {
                pane_titles[pane_index] = server_tab_names[server_index];
            }
            if (pane_index < pane_assigned.size()) {
                pane_assigned[pane_index] = true;
            }
            if (pane_index < pane_encoding_selectable.size()) {
                pane_encoding_selectable[pane_index] =
                    server_index >= server_fixed_first.size() || !server_fixed_first[server_index];
            }
            SetPaneEncoding(pane_index, terminal::TerminalEncoding::Utf8);
        }
        pane_view_states[pane_index] =
            {ViewPhase::Connecting, {}, ViewRecoveryAction::None};
        server_activated(server_ids[server_index], pane_index);
        Focus(pane_index);
        return true;
    }

    void AddHorizontal() {
        const std::size_t previous_active = ActivePane();
        const std::size_t previous_left_active = group_active[0];
        const bool was_split = split_active;
        if (AppendPane()) {
            const std::size_t added = panes.size() - 1;
            if (!split_active) {
                pane_groups.assign(panes.size(), 0U);
                pane_groups[added] = 1U;
            } else {
                pane_groups[added] = 1U;
            }
            split_active = true;
            columns = 2;
            column_weights.assign(2, 1.0F);
            // 新增右侧分屏时保持左侧当前标签不变。旧逻辑总是跳到左组第一个标签，
            // 不但改变用户正在看的终端，本地标签还会让远程编码选择器随之消失。
            if (!was_split && previous_active < added && GroupOf(previous_active) == 0U) {
                group_active[0] = previous_active;
            } else if (was_split && previous_left_active < added &&
                       GroupOf(previous_left_active) == 0U) {
                group_active[0] = previous_left_active;
            } else {
                group_active[0] = FirstTabInGroup(0).value_or(0);
            }
            group_active[1] = added;
            Focus(added);
        } else {
            ShowPaneCreationFailure();
        }
    }

    void CloseActive() {
        ClosePane(ActivePane());
    }

    void ClosePane(std::size_t active) {
        if (active >= panes.size()) {
            return;
        }
        const bool was_split = split_active;
        for (TerminalControl*& pending : reveal_tab) {
            if (pending == panes[active].get()) {
                pending = nullptr;
            }
        }
        const unsigned closing_group = GroupOf(active);
        const std::size_t previous_active = active_pane;
        if (pane_closing) {
            pane_closing(active);
        }
        if (panes.size() == 1) {
            for (const HWND selector : encoding_selectors) {
                if (selector && IsWindow(selector)) {
                    DestroyWindow(selector);
                }
            }
            panes.clear();
            reveal_tab = {};
            tab_scroll_first = {};
            pane_titles.clear();
            pane_view_states.clear();
            pane_tab_ids.clear();
            pane_assigned.clear();
            pane_encoding_selectable.clear();
            encoding_selectors.clear();
            pane_groups.clear();
            active_pane = 0;
            split_active = false;
            columns = 1;
            column_weights.assign(1, 1.0F);
            AppendPane();
            RECT rect{};
            GetClientRect(hwnd, &rect);
            Layout(rect.right, rect.bottom);
            return;
        }
        if (active < encoding_selectors.size() && encoding_selectors[active] &&
            IsWindow(encoding_selectors[active])) {
            DestroyWindow(encoding_selectors[active]);
        }
        panes.erase(panes.begin() + static_cast<std::ptrdiff_t>(active));
        pane_titles.erase(pane_titles.begin() + static_cast<std::ptrdiff_t>(active));
        pane_view_states.erase(pane_view_states.begin() + static_cast<std::ptrdiff_t>(active));
        pane_tab_ids.erase(pane_tab_ids.begin() + static_cast<std::ptrdiff_t>(active));
        pane_assigned.erase(pane_assigned.begin() + static_cast<std::ptrdiff_t>(active));
        pane_encoding_selectable.erase(pane_encoding_selectable.begin() +
                                       static_cast<std::ptrdiff_t>(active));
        encoding_selectors.erase(encoding_selectors.begin() + static_cast<std::ptrdiff_t>(active));
        pane_groups.erase(pane_groups.begin() + static_cast<std::ptrdiff_t>(active));
        for (std::size_t& selected : group_active) {
            if (selected == active) {
                selected = panes.size();
            } else if (selected > active) {
                --selected;
            }
        }
        if (previous_active != active) {
            active_pane = previous_active > active ? previous_active - 1 : previous_active;
        } else if (was_split) {
            const auto same_group = FirstTabInGroup(closing_group);
            const unsigned surviving_group = same_group ? closing_group : 1U - closing_group;
            const std::size_t selected = group_active[surviving_group];
            active_pane = selected < panes.size() && GroupOf(selected) == surviving_group
                              ? selected
                              : FirstTabInGroup(surviving_group).value_or(0);
        } else {
            active_pane = std::min(active, panes.size() - 1);
        }
        NormalizeGroups();
        RequestReveal(active_pane);
        RECT rect{};
        GetClientRect(hwnd, &rect);
        Layout(rect.right, rect.bottom);
    }

    void BuildElements() {
        surface.ClearElements();
        tab_hit_actions.clear();
        pane_tab_element_ids.assign(panes.size(), 0);
        tab_visible_regions = {};
        tab_overflow = {};
        tab_visible_counts = {};
        const UINT dpi = detail::WindowDpi(hwnd);
        const float scale = static_cast<float>(dpi) / 96.0F;
        const float width = static_cast<float>(std::max(0L, pane_area.right));
        const float strip_height = 34.0F * scale;
        const auto add_tab = [&](const std::size_t index,
                                 const float left,
                                 const float top,
                                 const float right,
                                 const bool selected) {
            const float inset = 3.0F * scale;
            const float tab_bottom = top + strip_height - 1.0F * scale;
            const unsigned group = GroupOf(index);
            const int tab_id = AddTabAction({0, TabHitKind::activate, index, group});
            pane_tab_element_ids[index] = tab_id;
            surface.Add({
                .id = tab_id,
                .role = detail::UiRole::file_tab,
                .bounds = detail::UiRect(left, top + inset, right, tab_bottom),
                .text = pane_titles[index] +
                        (pane_view_states[index].phase == ViewPhase::Connecting
                             ? L" · 连接中"
                         : pane_view_states[index].phase == ViewPhase::Disconnected
                             ? L" · 已断开"
                         : pane_view_states[index].phase == ViewPhase::Error
                             ? L" · 连接失败"
                             : L""),
                .icon = detail::UiIcon::terminal,
                .alignment = detail::UiTextAlign::leading,
                .selected = selected,
                .compact_text = true,
            });
            if (right - left >= 48.0F * scale) {
                const float close_size = 18.0F * scale;
                surface.Add({
                    .id = AddTabAction({0, TabHitKind::close, index, group}),
                    .role = detail::UiRole::tab_close,
                    .bounds = detail::UiRect(right - close_size - 5.0F * scale,
                                             top + (strip_height - close_size) / 2.0F,
                                             right - 5.0F * scale,
                                             top + (strip_height + close_size) / 2.0F),
                    .icon = detail::UiIcon::close,
                    .alignment = detail::UiTextAlign::center,
                });
            }
        };
        const auto has_area = [](const RECT& rect) noexcept {
            return rect.right > rect.left && rect.bottom > rect.top;
        };
        const auto add_strip = [&](const unsigned group_id,
                                   const RECT& group_rect,
                                   const std::vector<std::size_t>& tabs) {
            if (!has_area(group_rect) || tabs.empty()) {
                return;
            }
            const float left = static_cast<float>(group_rect.left);
            const float top = static_cast<float>(group_rect.top);
            const float right = static_cast<float>(group_rect.right);
            surface.Add({
                .role = detail::UiRole::panel,
                .bounds = detail::UiRect(left, top, right, top + strip_height),
            });
            const float margin = std::min(5.0F * scale, (right - left) / 8.0F);
            const float usable_width = std::max(1.0F, right - left - 2.0F * margin);
            const float button_width = std::min(22.0F * scale, usable_width / 4.0F);
            const float button_gap = std::min(2.0F * scale, usable_width / 16.0F);
            const float arrow_pair_width = 2.0F * button_width + button_gap;
            const int inner_width = std::max(1, static_cast<int>(usable_width));
            auto viewport = ComputeTerminalTabViewport(
                inner_width,
                detail::ScaleForDpi(78, dpi),
                detail::ScaleForDpi(147, dpi),
                detail::ScaleForDpi(2, dpi),
                static_cast<int>(arrow_pair_width),
                tabs.size(),
                tab_scroll_first[group_id]);
            if (TerminalControl* target = reveal_tab[group_id]) {
                const auto found = std::ranges::find_if(tabs, [this, target](std::size_t index) {
                    return panes[index].get() == target;
                });
                if (found != tabs.end()) {
                    viewport.Reveal(static_cast<std::size_t>(found - tabs.begin()), tabs.size());
                }
                reveal_tab[group_id] = nullptr;
            }
            tab_scroll_first[group_id] = viewport.first;
            tab_overflow[group_id] = viewport.overflow;
            tab_visible_counts[group_id] = viewport.count;
            const float tab_right_edge =
                right - margin - (viewport.overflow ? arrow_pair_width : 0.0F);
            tab_visible_regions[group_id] = {
                static_cast<LONG>(left + margin), static_cast<LONG>(top),
                static_cast<LONG>(tab_right_edge), static_cast<LONG>(top + strip_height)};
            float tab_left = left + margin;
            for (std::size_t ordinal = viewport.first;
                 ordinal < viewport.first + viewport.count; ++ordinal) {
                const std::size_t index = tabs[ordinal];
                if (index >= panes.size() || tab_left >= tab_right_edge) {
                    break;
                }
                const float tab_right = std::min(tab_left + viewport.tab_width, tab_right_edge);
                add_tab(index, tab_left, top, tab_right, index == ActiveTabInGroup(GroupOf(index)));
                tab_left = tab_right + 2.0F * scale;
            }
            if (viewport.overflow) {
                const float arrows_left = right - margin - arrow_pair_width;
                const auto add_arrow = [&](TabHitKind kind, float arrow_left, bool enabled,
                                           detail::UiIcon icon) {
                    surface.Add({
                        .id = AddTabAction({0, kind, 0, group_id}),
                        .role = detail::UiRole::icon_button,
                        .bounds = detail::UiRect(arrow_left, top + 5.0F * scale,
                                                 arrow_left + button_width,
                                                 top + strip_height - 5.0F * scale),
                        .icon = icon,
                        .alignment = detail::UiTextAlign::center,
                        .enabled = enabled,
                    });
                };
                add_arrow(TabHitKind::scroll_left, arrows_left, viewport.first > 0,
                          detail::UiIcon::chevron_left);
                add_arrow(TabHitKind::scroll_right, arrows_left + button_width + button_gap,
                          viewport.first + viewport.count < tabs.size(),
                          detail::UiIcon::chevron);
            }
        };
        const bool geometry_ready = !panes.empty() && pane_rects.size() == panes.size();
        if (geometry_ready) {
            if (split_active) {
                for (unsigned group = 0; group < 2; ++group) {
                    const auto tabs = TabsInGroup(group);
                    if (!tabs.empty() && has_area(pane_rects[tabs.front()])) {
                        add_strip(group, pane_rects[tabs.front()], tabs);
                    }
                }
            } else {
                const std::size_t selected = std::min(active_pane, panes.size() - 1);
                if (has_area(pane_rects[selected])) {
                    std::vector<std::size_t> tabs(panes.size());
                    for (std::size_t index = 0; index < tabs.size(); ++index) {
                        tabs[index] = index;
                    }
                    add_strip(0, pane_rects[selected], tabs);
                }
            }
        }

        if (split_active && !pane_rects.empty()) {
            const float splitter = std::max(2.0F, 2.0F * scale);
            if (const auto left = FirstTabInGroup(0)) {
                const float x = static_cast<float>(pane_rects[*left].right);
                surface.Add({
                    .role = detail::UiRole::separator,
                    .bounds = detail::UiRect(x,
                                             static_cast<float>(pane_area.top),
                                             x + splitter,
                                             static_cast<float>(pane_area.bottom)),
                });
            }
        }

        if (visible_drop_zone != SplitDropZone::none) {
            const RECT drop = DropZoneRect();
            surface.Add({
                .role = detail::UiRole::drop_target,
                .bounds = detail::UiRect(static_cast<float>(drop.left + 8),
                                         static_cast<float>(drop.top + 8),
                                         static_cast<float>(drop.right - 8),
                                         static_cast<float>(drop.bottom - 8)),
                .text = visible_drop_zone == SplitDropZone::center ? L"" : L"拖放到此处分屏",
                .alignment = detail::UiTextAlign::center,
            });
        }
        if (visible_tab_insertion) {
            const float half_width = std::max(1.0F, 1.5F * scale);
            surface.Add({
                .role = detail::UiRole::tab_insert,
                .bounds = detail::UiRect(visible_tab_insertion->x - half_width,
                                         visible_tab_insertion->top + 2.0F * scale,
                                         visible_tab_insertion->x + half_width,
                                         visible_tab_insertion->bottom - 2.0F * scale),
            });
        }
        StartTabMotionIfNeeded();
        (void)width;
    }

    void InvalidateTabStrip() const {
        RECT client{};
        GetClientRect(hwnd, &client);
        const RECT strip{0, 0, client.right, detail::ScaleForDpi(35, detail::WindowDpi(hwnd))};
        InvalidateRect(hwnd, &strip, FALSE);
    }

    void UpdateTabAccent() {
        if (panes.empty() || active_pane >= panes.size()) {
            surface.SetOverlayAccent(std::nullopt);
            return;
        }
        const int tab_id = active_pane < pane_tab_element_ids.size()
                               ? pane_tab_element_ids[active_pane]
                               : 0;
        if (tab_id == 0) {
            surface.SetOverlayAccent(std::nullopt);
            return;
        }
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
        TerminalControl* current =
            panes.empty() || active_pane >= panes.size() ? nullptr : panes[active_pane].get();
        if (animated_tab != current) {
            animated_tab = current;
            tab_motion_start = GetTickCount64();
            tab_motion_active = current != nullptr && tab_motion_enabled &&
                                SetTimer(hwnd, kTabMotionTimerId, 16, nullptr) != 0;
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
        const float fraction = std::min(1.0F, static_cast<float>(GetTickCount64() -
                                                                tab_motion_start) /
                                                      static_cast<float>(kTabMotionDurationMs));
        tab_motion_progress = 1.0F - (1.0F - fraction) * (1.0F - fraction);
        tab_motion_active = fraction < 1.0F;
        if (!tab_motion_active) {
            KillTimer(hwnd, kTabMotionTimerId);
        }
        UpdateTabAccent();
        InvalidateTabStrip();
    }

    void Layout(int width, int height) {
        if (width <= 0 || height <= 0) {
            pane_area = {};
            pane_rects.clear();
            return;
        }
        const UINT dpi = detail::WindowDpi(hwnd);
        const int padding = detail::ScaleForDpi(10, dpi);
        const int gap = detail::ScaleForDpi(5, dpi);
        const int tab_height = detail::ScaleForDpi(34, dpi);
        pane_area = {0, 0, width - padding, height - padding};

        const int content_width = std::max(0, static_cast<int>(pane_area.right - pane_area.left));
        const int content_height = std::max(0, static_cast<int>(pane_area.bottom - pane_area.top));
        NormalizeGroups();
        pane_rects.assign(panes.size(), RECT{});
        for (const HWND selector : encoding_selectors) {
            if (selector && IsWindowVisible(selector)) {
                ShowWindow(selector, SW_HIDE);
            }
        }
        if (!split_active) {
            const std::size_t selected =
                panes.empty() ? 0 : std::min(active_pane, panes.size() - 1);
            for (std::size_t index = 0; index < panes.size(); ++index) {
                const bool should_show = index == selected;
                if ((IsWindowVisible(panes[index]->hwnd()) != FALSE) != should_show) {
                    panes[index]->Show(should_show);
                }
            }
            if (!panes.empty()) {
                panes[selected]->SetBounds(pane_area.left,
                                           pane_area.top + tab_height - 1,
                                           content_width,
                                           std::max(1, content_height - tab_height + 1));
                pane_rects[selected] = pane_area;
                if (EncodingSelectable(selected) && selected < encoding_selectors.size()) {
                    const int selector_width = detail::ScaleForDpi(80, dpi);
                    const int selector_height = detail::ScaleForDpi(19, dpi);
                    const int margin = detail::ScaleForDpi(1, dpi);
                    SetWindowPos(encoding_selectors[selected],
                                 HWND_TOP,
                                 pane_area.right - selector_width - margin,
                                 pane_area.bottom - selector_height - margin,
                                 selector_width,
                                 selector_height,
                                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
                }
            }
        } else {
            if (column_weights.size() != 2) {
                column_weights.assign(2, 1.0F);
            }
            const int available = std::max(2, content_width - gap);
            const float left_weight = std::max(0.01F, column_weights[0]);
            const float right_weight = std::max(0.01F, column_weights[1]);
            const int left_width =
                std::clamp(static_cast<int>(available * left_weight / (left_weight + right_weight)),
                           1,
                           std::max(1, available - 1));
            const std::array<RECT, 2> group_rects{{
                {pane_area.left, pane_area.top, pane_area.left + left_width, pane_area.bottom},
                {pane_area.left + left_width + gap,
                 pane_area.top,
                 pane_area.right,
                 pane_area.bottom},
            }};
            std::array<std::size_t, 2> selected_tabs{ActiveTabInGroup(0), ActiveTabInGroup(1)};
            for (unsigned group = 0; group < 2; ++group) {
                for (const std::size_t index : TabsInGroup(group)) {
                    pane_rects[index] = group_rects[group];
                    const bool should_show = index == selected_tabs[group];
                    if (!should_show && IsWindowVisible(panes[index]->hwnd()) != FALSE) {
                        panes[index]->Show(false);
                    }
                }
            }
            HDWP positions = BeginDeferWindowPos(4);
            for (unsigned group = 0; group < 2; ++group) {
                const std::size_t index = selected_tabs[group];
                if (index >= panes.size()) {
                    continue;
                }
                const RECT& target = group_rects[group];
                positions =
                    positions
                        ? DeferWindowPos(positions,
                                         panes[index]->hwnd(),
                                         nullptr,
                                         target.left,
                                         target.top + tab_height - 1,
                                         std::max(1L, target.right - target.left),
                                         std::max(1L, target.bottom - target.top - tab_height + 1),
                                         SWP_NOACTIVATE | SWP_NOZORDER)
                        : nullptr;
                if (positions && EncodingSelectable(index) && index < encoding_selectors.size()) {
                    const int selector_width = detail::ScaleForDpi(80, dpi);
                    const int selector_height = detail::ScaleForDpi(19, dpi);
                    const int margin = detail::ScaleForDpi(1, dpi);
                    positions = DeferWindowPos(positions,
                                               encoding_selectors[index],
                                               HWND_TOP,
                                               target.right - selector_width - margin,
                                               target.bottom - selector_height - margin,
                                               selector_width,
                                               selector_height,
                                               SWP_NOACTIVATE | SWP_SHOWWINDOW);
                }
            }
            if (positions) {
                EndDeferWindowPos(positions);
            } else {
                for (unsigned group = 0; group < 2; ++group) {
                    const std::size_t index = selected_tabs[group];
                    if (index >= panes.size()) {
                        continue;
                    }
                    const RECT& target = group_rects[group];
                    panes[index]->SetBounds(
                        target.left,
                        target.top + tab_height - 1,
                        std::max(1L, target.right - target.left),
                        std::max(1L, target.bottom - target.top - tab_height + 1));
                    if (EncodingSelectable(index) && index < encoding_selectors.size()) {
                        const int selector_width = detail::ScaleForDpi(80, dpi);
                        const int selector_height = detail::ScaleForDpi(19, dpi);
                        const int margin = detail::ScaleForDpi(1, dpi);
                        SetWindowPos(encoding_selectors[index],
                                     HWND_TOP,
                                     target.right - selector_width - margin,
                                     target.bottom - selector_height - margin,
                                     selector_width,
                                     selector_height,
                                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
                    }
                }
            }
            for (const std::size_t index : selected_tabs) {
                if (index < panes.size() && IsWindowVisible(panes[index]->hwnd()) == FALSE) {
                    panes[index]->Show(true);
                }
            }
            RedrawWindow(hwnd, &pane_area, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_NOERASE);
        }
        BuildElements();
        InvalidateRect(hwnd, nullptr, FALSE);
    }
};

TerminalPage::TerminalPage() : impl_(std::make_unique<Impl>()) {}

TerminalPage::~TerminalPage() {
    if (impl_->hwnd && IsWindow(impl_->hwnd)) {
        DestroyWindow(impl_->hwnd);
    }
}

bool TerminalPage::Create(HWND parent, HINSTANCE instance) {
    impl_->instance = instance;
    if (!RegisterPageClass(instance, &Impl::WindowProc) || !RegisterResizeGuideClass(instance) ||
        !selector::RegisterClass(instance)) {
        return false;
    }
    impl_->hwnd = CreateWindowExW(0,
                                  kTerminalPageClass,
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
    if (!impl_->hwnd || !impl_->surface.Initialize(impl_->hwnd)) {
        return false;
    }
    impl_->RecreateSelectorFont();
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
    impl_->tab_tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                                         WS_POPUP | TTS_ALWAYSTIP, CW_USEDEFAULT,
                                         CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                                         impl_->hwnd, nullptr, instance, nullptr);
    if (impl_->tab_tooltip) {
        TOOLINFOW tool{sizeof(tool)};
        tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS | TTF_TRANSPARENT;
        tool.hwnd = impl_->hwnd;
        tool.uId = reinterpret_cast<UINT_PTR>(impl_->hwnd);
        tool.lpszText = LPSTR_TEXTCALLBACKW;
        SendMessageW(impl_->tab_tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
        SendMessageW(impl_->tab_tooltip, TTM_SETMAXTIPWIDTH, 0,
                     detail::ScaleForDpi(480, detail::WindowDpi(impl_->hwnd)));
    }
    impl_->BuildPanes();
    return !impl_->panes.empty();
}

void TerminalPage::SetBounds(int x, int y, int width, int height) {
    SetWindowPos(
        impl_->hwnd, nullptr, x, y, width, height, SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOREDRAW);
}

void TerminalPage::Show(bool visible) {
    if (!visible) {
        for (const auto& pane : impl_->panes) {
            pane->Show(false);
        }
        ShowWindow(impl_->hwnd, SW_HIDE);
        return;
    }
    RECT rect{};
    GetClientRect(impl_->hwnd, &rect);
    impl_->Layout(rect.right, rect.bottom);
    ShowWindow(impl_->hwnd, SW_SHOW);
    RedrawWindow(impl_->hwnd,
                 nullptr,
                 nullptr,
                 RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW | RDW_NOERASE);
}

void TerminalPage::ApplyTheme() {
    detail::ApplyThemeToWindowTree(impl_->hwnd);
    for (const auto& pane : impl_->panes) {
        pane->ApplyTheme();
    }
    for (const HWND selector : impl_->encoding_selectors) {
        detail::ApplySelfDrawnTheme(selector);
        InvalidateRect(selector, nullptr, TRUE);
    }
    impl_->BuildElements();
    RedrawWindow(impl_->hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

void TerminalPage::AddServer(std::string server_id,
                             std::wstring display_name,
                             std::wstring tab_name,
                             const bool fixed_first) {
    if (server_id.empty()) {
        return;
    }
    impl_->server_ids.push_back(std::move(server_id));
    impl_->server_tab_names.push_back(tab_name.empty() ? std::move(display_name)
                                                       : std::move(tab_name));
    impl_->server_fixed_first.push_back(fixed_first);
    if (!impl_->selected_server) {
        impl_->selected_server = 0;
    }
    impl_->BuildElements();
    InvalidateRect(impl_->hwnd, nullptr, FALSE);
}

void TerminalPage::ClearServers() {
    impl_->server_ids.clear();
    impl_->server_tab_names.clear();
    impl_->server_fixed_first.clear();
    impl_->selected_server.reset();
    impl_->BuildElements();
    InvalidateRect(impl_->hwnd, nullptr, FALSE);
}

bool TerminalPage::ActivateServer(const std::string_view server_id) {
    const auto found = std::find(impl_->server_ids.begin(), impl_->server_ids.end(), server_id);
    if (found == impl_->server_ids.end()) {
        return false;
    }
    impl_->selected_server =
        static_cast<std::size_t>(std::distance(impl_->server_ids.begin(), found));
    return impl_->ActivateSelectedServer();
}

void TerminalPage::SetPaneViewState(TerminalControl& pane, ViewState state) {
    const auto found = std::ranges::find_if(impl_->panes, [&pane](const auto& candidate) {
        return candidate.get() == &pane;
    });
    if (found == impl_->panes.end()) {
        return;
    }
    const std::size_t index = static_cast<std::size_t>(found - impl_->panes.begin());
    impl_->pane_view_states[index] = std::move(state);
    impl_->BuildElements();
    InvalidateRect(impl_->hwnd, nullptr, FALSE);
}

void TerminalPage::SetServerActivatedCallback(ServerActivatedCallback callback) {
    impl_->server_activated = std::move(callback);
}

void TerminalPage::SetPaneClosingCallback(PaneClosingCallback callback) {
    impl_->pane_closing = std::move(callback);
}

void TerminalPage::SetPaneCreatedCallback(PaneCreatedCallback callback) {
    impl_->pane_created = std::move(callback);
    if (impl_->pane_created) {
        for (std::size_t index = 0; index < impl_->panes.size(); ++index) {
            impl_->pane_created(index, *impl_->panes[index]);
        }
    }
}

void TerminalPage::AddHorizontalPane() {
    impl_->AddHorizontal();
}

void TerminalPage::CloseActivePane() {
    impl_->CloseActive();
}

void TerminalPage::FocusPane(std::size_t index) {
    impl_->Focus(index);
}

std::size_t TerminalPage::active_pane_index() const noexcept {
    return impl_->ActivePane();
}

std::size_t TerminalPage::pane_count() const noexcept {
    return impl_->panes.size();
}

TerminalControl* TerminalPage::pane_at(std::size_t index) noexcept {
    return index < impl_->panes.size() ? impl_->panes[index].get() : nullptr;
}

const TerminalControl* TerminalPage::pane_at(std::size_t index) const noexcept {
    return index < impl_->panes.size() ? impl_->panes[index].get() : nullptr;
}

HWND TerminalPage::hwnd() const noexcept {
    return impl_->hwnd;
}

} // namespace SLink::ui
