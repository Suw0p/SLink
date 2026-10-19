// 服务器侧栏自绘窗口类实现，提供虚拟行、实时过滤、键鼠命中和统一选择态。
#include "ui/sidebar_control.hpp"

#include "ui/win32_ui_internal.hpp"

#include <algorithm>
#include <commctrl.h>
#include <string>
#include <uxtheme.h>
#include <vector>
#include <windowsx.h>

namespace SLink::ui::sidebar {
namespace {

constexpr UINT kGetScrollOffsetMessage = WM_APP + 0x153;
constexpr UINT kSetScrollOffsetMessage = WM_APP + 0x154;

struct Item {
    std::wstring text;
    LPARAM data{};
    int image{-1};
};

struct State {
    HWND window{};
    HWND parent{};
    int control_id{};
    HFONT font{};
    HIMAGELIST images{};
    std::vector<Item> items;
    int selected{-1};
    int hover{-1};
    int scroll_y{};
    int item_height{40};
};

struct PaintResources {
    COLORREF panel_color{CLR_INVALID};
    COLORREF panel_alt_color{CLR_INVALID};
    COLORREF selected_color{CLR_INVALID};
    COLORREF hover_color{CLR_INVALID};
    COLORREF border_color{CLR_INVALID};
    HBRUSH panel{};
    HBRUSH panel_alt{};
    HBRUSH selected{};
    HBRUSH hover{};
    HPEN border{};

    ~PaintResources() {
        Reset();
    }

    void Reset() noexcept {
        if (panel)
            DeleteObject(panel);
        if (panel_alt)
            DeleteObject(panel_alt);
        if (selected)
            DeleteObject(selected);
        if (hover)
            DeleteObject(hover);
        if (border)
            DeleteObject(border);
        panel = panel_alt = selected = hover = nullptr;
        border = nullptr;
    }

    void Ensure() {
        if (panel && panel_color == theme::kPanelColor &&
            panel_alt_color == theme::kPanelAltColor &&
            selected_color == theme::kSelectedRowColor && hover_color == theme::kPanelHoverColor &&
            border_color == theme::kBorderColor) {
            return;
        }
        Reset();
        panel_color = theme::kPanelColor;
        panel_alt_color = theme::kPanelAltColor;
        selected_color = theme::kSelectedRowColor;
        hover_color = theme::kPanelHoverColor;
        border_color = theme::kBorderColor;
        panel = CreateSolidBrush(panel_color);
        panel_alt = CreateSolidBrush(panel_alt_color);
        selected = CreateSolidBrush(selected_color);
        hover = CreateSolidBrush(hover_color);
        border = CreatePen(PS_SOLID, 1, border_color);
    }

    HBRUSH RowBrush(bool is_selected, bool is_hovered) const noexcept {
        return is_selected ? selected : is_hovered ? hover : panel;
    }
};

PaintResources& SharedPaintResources() {
    static PaintResources resources;
    resources.Ensure();
    return resources;
}

State* GetState(HWND window) {
    return reinterpret_cast<State*>(GetWindowLongPtrW(window, GWLP_USERDATA));
}

int Scale(HWND window, int value) {
    return detail::ScaleForDpi(value, detail::WindowDpi(window));
}

HTREEITEM ItemHandle(int index) {
    return index < 0 ? nullptr : reinterpret_cast<HTREEITEM>(static_cast<INT_PTR>(index + 1));
}

int ItemIndex(HTREEITEM item) {
    if (!item || item == TVI_ROOT || item == TVI_FIRST || item == TVI_LAST || item == TVI_SORT) {
        return -1;
    }
    return static_cast<int>(reinterpret_cast<INT_PTR>(item)) - 1;
}

int ContentHeight(const State& state) {
    return static_cast<int>(state.items.size()) * state.item_height;
}

int MaximumScroll(const State& state) {
    RECT client{};
    GetClientRect(state.window, &client);
    return std::max(0, ContentHeight(state) - static_cast<int>(client.bottom - client.top));
}

void ClampScroll(State& state) {
    state.scroll_y = std::clamp(state.scroll_y, 0, MaximumScroll(state));
}

RECT ItemRect(const State& state, int index) {
    const int gap = Scale(state.window, 4);
    const int top = index * state.item_height - state.scroll_y + gap / 2;
    RECT client{};
    GetClientRect(state.window, &client);
    const int scrollbar = MaximumScroll(state) > 0 ? Scale(state.window, 8) : 0;
    return {client.left + Scale(state.window, 2),
            top,
            std::max(client.left + Scale(state.window, 3),
                     client.right - Scale(state.window, 2) - scrollbar),
            top + state.item_height - gap};
}

int HitItem(const State& state, POINT point) {
    if (point.y < 0) {
        return -1;
    }
    const int index = (point.y + state.scroll_y) / state.item_height;
    if (index < 0 || index >= static_cast<int>(state.items.size())) {
        return -1;
    }
    const RECT rect = ItemRect(state, index);
    return PtInRect(&rect, point) ? index : -1;
}

void NotifySimple(const State& state, UINT code) {
    NMHDR notification{};
    notification.hwndFrom = state.window;
    notification.idFrom = static_cast<UINT_PTR>(state.control_id);
    notification.code = code;
    SendMessageW(
        state.parent, WM_NOTIFY, notification.idFrom, reinterpret_cast<LPARAM>(&notification));
}

void NotifySelectionChanged(const State& state, int old_index, int new_index) {
    NMTREEVIEWW notification{};
    notification.hdr.hwndFrom = state.window;
    notification.hdr.idFrom = static_cast<UINT_PTR>(state.control_id);
    notification.hdr.code = TVN_SELCHANGEDW;
    notification.itemOld.hItem = ItemHandle(old_index);
    notification.itemNew.hItem = ItemHandle(new_index);
    if (old_index >= 0 && old_index < static_cast<int>(state.items.size())) {
        notification.itemOld.lParam = state.items[old_index].data;
    }
    if (new_index >= 0 && new_index < static_cast<int>(state.items.size())) {
        notification.itemNew.lParam = state.items[new_index].data;
    }
    SendMessageW(
        state.parent, WM_NOTIFY, notification.hdr.idFrom, reinterpret_cast<LPARAM>(&notification));
}

void Select(State& state, int index, bool notify = true) {
    if (index < 0 || index >= static_cast<int>(state.items.size())) {
        index = -1;
    }
    if (state.selected == index) {
        return;
    }
    const int old = state.selected;
    state.selected = index;
    InvalidateRect(state.window, nullptr, FALSE);
    if (notify) {
        NotifySelectionChanged(state, old, index);
    }
}

void EnsureVisible(State& state, int index) {
    if (index < 0 || index >= static_cast<int>(state.items.size())) {
        return;
    }
    RECT client{};
    GetClientRect(state.window, &client);
    const int top = index * state.item_height;
    const int bottom = top + state.item_height;
    if (top < state.scroll_y) {
        state.scroll_y = top;
    } else if (bottom > state.scroll_y + client.bottom) {
        state.scroll_y = bottom - client.bottom;
    }
    ClampScroll(state);
}

void Draw(State& state, HDC target, const RECT& client) {
    PaintResources& resources = SharedPaintResources();
    HDC dc = target;
    HPAINTBUFFER paint_buffer =
        BeginBufferedPaint(target, &client, BPBF_COMPATIBLEBITMAP, nullptr, &dc);
    if (!paint_buffer) {
        dc = target;
    }

    FillRect(dc, &client, resources.panel);
    const HGDIOBJ old_font = state.font ? SelectObject(dc, state.font) : nullptr;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, theme::kTextColor);

    const int first = std::max(0, state.scroll_y / state.item_height);
    const int visible =
        std::max(1, static_cast<int>(client.bottom - client.top) / state.item_height + 2);
    const int last = std::min(static_cast<int>(state.items.size()), first + visible);
    for (int index = first; index < last; ++index) {
        const RECT row = ItemRect(state, index);
        const bool selected = index == state.selected;
        const bool hovered = index == state.hover;
        if (selected || hovered) {
            const HGDIOBJ old_brush = SelectObject(dc, resources.RowBrush(selected, hovered));
            const HGDIOBJ old_pen = SelectObject(dc, GetStockObject(NULL_PEN));
            const int radius = Scale(state.window, 8);
            RoundRect(dc, row.left, row.top, row.right, row.bottom, radius, radius);
            SelectObject(dc, old_pen);
            SelectObject(dc, old_brush);
        }

        RECT text = row;
        text.left += Scale(state.window, 12);
        text.right -= Scale(state.window, 9);
        if (state.images && state.items[index].image >= 0) {
            int icon_width = Scale(state.window, 16);
            int icon_height = icon_width;
            ImageList_GetIconSize(state.images, &icon_width, &icon_height);
            const int icon_y =
                row.top + std::max(0, static_cast<int>(row.bottom - row.top) - icon_height) / 2;
            ImageList_Draw(
                state.images, state.items[index].image, dc, text.left, icon_y, ILD_TRANSPARENT);
            text.left += icon_width + Scale(state.window, 6);
        }
        DrawTextW(dc,
                  state.items[index].text.c_str(),
                  -1,
                  &text,
                  DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    }

    const int maximum = MaximumScroll(state);
    if (maximum > 0) {
        const int width = Scale(state.window, 8);
        RECT track{client.right - width, client.top, client.right, client.bottom};
        const int track_height = std::max(1, static_cast<int>(track.bottom - track.top));
        const int content_height = std::max(1, ContentHeight(state));
        const int thumb_height = std::clamp(MulDiv(track_height, track_height, content_height),
                                            Scale(state.window, 18),
                                            track_height);
        const int travel = std::max(0, track_height - thumb_height);
        const int offset = maximum > 0 ? MulDiv(state.scroll_y, travel, maximum) : 0;
        RECT thumb{track.left + 1,
                   track.top + offset + 1,
                   track.right - 1,
                   track.top + offset + thumb_height - 1};
        FillRect(dc, &track, resources.panel_alt);
        const HGDIOBJ old_brush = SelectObject(dc, resources.hover);
        const HGDIOBJ old_pen = SelectObject(dc, GetStockObject(NULL_PEN));
        const int radius = Scale(state.window, 5);
        RoundRect(dc, thumb.left, thumb.top, thumb.right, thumb.bottom, radius, radius);
        SelectObject(dc, old_pen);
        SelectObject(dc, old_brush);
    }

    const HGDIOBJ old_pen = SelectObject(dc, resources.border);
    MoveToEx(dc, client.right - 1, client.top, nullptr);
    LineTo(dc, client.right - 1, client.bottom);
    SelectObject(dc, old_pen);
    if (old_font) {
        SelectObject(dc, old_font);
    }
    if (paint_buffer) {
        EndBufferedPaint(paint_buffer, TRUE);
    }
}

LRESULT WindowMessage(State& state, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case TVM_DELETEITEM:
        if (reinterpret_cast<HTREEITEM>(lparam) == TVI_ROOT) {
            state.items.clear();
            state.selected = -1;
            state.hover = -1;
            state.scroll_y = 0;
            InvalidateRect(state.window, nullptr, FALSE);
            return TRUE;
        }
        return FALSE;
    case TVM_INSERTITEMW: {
        const auto* insert = reinterpret_cast<const TVINSERTSTRUCTW*>(lparam);
        if (!insert)
            return 0;
        Item item;
        if ((insert->item.mask & TVIF_TEXT) != 0 && insert->item.pszText) {
            item.text = insert->item.pszText;
        }
        if ((insert->item.mask & TVIF_PARAM) != 0) {
            item.data = insert->item.lParam;
        }
        if ((insert->item.mask & TVIF_IMAGE) != 0) {
            item.image = insert->item.iImage;
        }
        state.items.push_back(std::move(item));
        ClampScroll(state);
        InvalidateRect(state.window, nullptr, FALSE);
        return reinterpret_cast<LRESULT>(ItemHandle(static_cast<int>(state.items.size()) - 1));
    }
    case TVM_GETITEMW: {
        auto* item = reinterpret_cast<TVITEMW*>(lparam);
        const int index = item ? ItemIndex(item->hItem) : -1;
        if (!item || index < 0 || index >= static_cast<int>(state.items.size())) {
            return FALSE;
        }
        if ((item->mask & TVIF_PARAM) != 0) {
            item->lParam = state.items[index].data;
        }
        if ((item->mask & TVIF_TEXT) != 0 && item->pszText && item->cchTextMax > 0) {
            wcsncpy_s(item->pszText,
                      static_cast<std::size_t>(item->cchTextMax),
                      state.items[index].text.c_str(),
                      _TRUNCATE);
        }
        return TRUE;
    }
    case TVM_GETNEXTITEM:
        if (wparam == TVGN_CARET) {
            return reinterpret_cast<LRESULT>(ItemHandle(state.selected));
        }
        if (wparam == TVGN_ROOT) {
            return reinterpret_cast<LRESULT>(state.items.empty() ? nullptr : ItemHandle(0));
        }
        if (wparam == TVGN_NEXT) {
            const int index = ItemIndex(reinterpret_cast<HTREEITEM>(lparam));
            return reinterpret_cast<LRESULT>(index >= 0 && index + 1 <
                                                               static_cast<int>(state.items.size())
                                                 ? ItemHandle(index + 1)
                                                 : nullptr);
        }
        return 0;
    case TVM_SELECTITEM:
        if (wparam == TVGN_CARET) {
            Select(state, ItemIndex(reinterpret_cast<HTREEITEM>(lparam)));
            EnsureVisible(state, state.selected);
            return TRUE;
        }
        return FALSE;
    case TVM_GETITEMRECT: {
        auto* rect = reinterpret_cast<RECT*>(lparam);
        if (!rect)
            return FALSE;
        const HTREEITEM item = *reinterpret_cast<HTREEITEM*>(rect);
        const int index = ItemIndex(item);
        if (index < 0 || index >= static_cast<int>(state.items.size())) {
            return FALSE;
        }
        *rect = ItemRect(state, index);
        return TRUE;
    }
    case TVM_HITTEST: {
        auto* hit = reinterpret_cast<TVHITTESTINFO*>(lparam);
        if (!hit)
            return 0;
        const int index = HitItem(state, hit->pt);
        hit->hItem = ItemHandle(index);
        hit->flags = index >= 0 ? TVHT_ONITEM : TVHT_NOWHERE;
        return reinterpret_cast<LRESULT>(hit->hItem);
    }
    case TVM_SETITEMHEIGHT: {
        const int previous = state.item_height;
        state.item_height = std::max(Scale(state.window, 24), static_cast<int>(wparam));
        ClampScroll(state);
        InvalidateRect(state.window, nullptr, FALSE);
        return previous;
    }
    case TVM_GETITEMHEIGHT:
        return state.item_height;
    case TVM_GETCOUNT:
        return static_cast<LRESULT>(state.items.size());
    case TVM_SETIMAGELIST: {
        const HIMAGELIST previous = state.images;
        state.images = reinterpret_cast<HIMAGELIST>(lparam);
        InvalidateRect(state.window, nullptr, FALSE);
        return reinterpret_cast<LRESULT>(previous);
    }
    case TVM_GETIMAGELIST:
        return reinterpret_cast<LRESULT>(state.images);
    case TVM_ENSUREVISIBLE:
        EnsureVisible(state, ItemIndex(reinterpret_cast<HTREEITEM>(lparam)));
        InvalidateRect(state.window, nullptr, FALSE);
        return TRUE;
    case TVM_SETBKCOLOR:
    case TVM_SETTEXTCOLOR:
    case TVM_SETLINECOLOR:
    case TVM_SETEXTENDEDSTYLE:
        return 0;
    default:
        return DefWindowProcW(state.window, message, wparam, lparam);
    }
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    State* state = GetState(window);
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        state = new State{};
        state->window = window;
        state->parent = create->hwndParent;
        state->control_id = static_cast<int>(reinterpret_cast<INT_PTR>(create->hMenu));
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (!state) {
        return DefWindowProcW(window, message, wparam, lparam);
    }
    if (message >= TV_FIRST && message < TV_FIRST + 128) {
        return WindowMessage(*state, message, wparam, lparam);
    }
    switch (message) {
    case kGetScrollOffsetMessage:
        return state->scroll_y;
    case kSetScrollOffsetMessage:
        state->scroll_y = static_cast<int>(wparam);
        ClampScroll(*state);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_SETFONT:
        state->font = reinterpret_cast<HFONT>(wparam);
        if (LOWORD(lparam))
            InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_GETFONT:
        return reinterpret_cast<LRESULT>(state->font);
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        Draw(*state, dc, client);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_SIZE:
        ClampScroll(*state);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_MOUSEMOVE: {
        const int hover = HitItem(*state, {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
        if (hover != state->hover) {
            state->hover = hover;
            InvalidateRect(window, nullptr, FALSE);
        }
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
        TrackMouseEvent(&tracking);
        return 0;
    }
    case WM_MOUSELEAVE:
        state->hover = -1;
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN: {
        SetFocus(window);
        const int index = HitItem(*state, {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
        Select(*state, index);
        NotifySimple(*state, NM_CLICK);
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        const int index = HitItem(*state, {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
        Select(*state, index);
        NotifySimple(*state, NM_DBLCLK);
        return 0;
    }
    case WM_RBUTTONUP: {
        const int index = HitItem(*state, {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
        Select(*state, index);
        NotifySimple(*state, NM_RCLICK);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        const int steps = std::max(1, std::abs(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA);
        state->scroll_y +=
            (GET_WHEEL_DELTA_WPARAM(wparam) > 0 ? -1 : 1) * steps * state->item_height * 3;
        ClampScroll(*state);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    }
    case WM_KEYDOWN:
        if (wparam == VK_UP || wparam == VK_DOWN) {
            const int direction = wparam == VK_UP ? -1 : 1;
            const int start = state->selected < 0
                                  ? (direction > 0 ? -1 : static_cast<int>(state->items.size()))
                                  : state->selected;
            Select(*state, start + direction);
            EnsureVisible(*state, state->selected);
            return 0;
        }
        if ((wparam == VK_RETURN || wparam == VK_SPACE) && state->selected >= 0) {
            NotifySimple(*state, state->items[state->selected].data == 0 ? NM_RETURN : NM_DBLCLK);
            return 0;
        }
        break;
    case WM_SETFOCUS:
        NotifySimple(*state, NM_SETFOCUS);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_KILLFOCUS:
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_THEMECHANGED:
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_NCDESTROY:
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
    BufferedPaintInit();
    WNDCLASSEXW window_class{sizeof(window_class)};
    window_class.style = CS_DBLCLKS;
    window_class.lpfnWndProc = &WindowProc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.lpszClassName = kClassName;
    if (RegisterClassExW(&window_class) != 0) {
        return true;
    }
    return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

int ScrollOffset(HWND window) noexcept {
    return window ? static_cast<int>(SendMessageW(window, kGetScrollOffsetMessage, 0, 0)) : 0;
}

void SetScrollOffset(HWND window, int offset) noexcept {
    if (window) {
        SendMessageW(window, kSetScrollOffsetMessage, static_cast<WPARAM>(offset), 0);
    }
}

} // namespace SLink::ui::sidebar
