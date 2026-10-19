// 虚拟化自绘表格实现：只绘制可见行，统一表头、列宽、树形展开、进度条、框选和深浅
// 主题滚动条，不依赖原生 ListView/Explorer 主题。
#include "ui/table_control.hpp"

#include "ui/theme.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <numeric>
#include <utility>
#include <windowsx.h>

#pragma comment(lib, "Comctl32.lib")

namespace SLink::ui::table {
namespace {

constexpr wchar_t kTableClass[] = L"SLink.TableControl";

enum class ScrollPart {
    none,
    horizontal_thumb,
    horizontal_track,
    vertical_thumb,
    vertical_track,
};

struct CreateParameters {
    bool single_selection{};
    bool border{};
};

// 每个表格 HWND 的轻量状态。rows 只是逻辑行数，文本/图标按可见范围通过 Provider
// 即时获取；selected 保存行号而非复制整行数据。滚动位置和列宽都在此统一裁剪。
struct State {
    HWND window{};
    HWND parent{};
    int control_id{};
    bool single_selection{};
    bool border{};
    bool fill_last_column{};
    std::vector<Column> columns;
    std::size_t rows{};
    TextProvider text_provider;
    ImageProvider image_provider;
    IndentProvider indent_provider;
    TreeProvider tree_provider;
    TreeToggleHandler tree_toggle_handler;
    HeaderClickHandler header_click_handler;
    ProgressProvider progress_provider;
    HIMAGELIST images{};
    HFONT font{};
    std::vector<HFONT> column_fonts;
    std::vector<int> selected;
    int focused_row{-1};
    int anchor_row{-1};
    int hover_row{-1};
    int drop_target_row{-1};
    int scroll_x{};
    int scroll_y{};
    bool show_horizontal_scrollbar{};
    bool show_vertical_scrollbar{};
    ScrollPart hovered_scroll_part{ScrollPart::none};
    ScrollPart dragged_scroll_part{ScrollPart::none};
    int scroll_drag_offset{};
    int row_height{22};
    int header_height{26};
    bool pointer_down{};
    bool drag_sent{};
    int pointer_row{-1};
    POINT pointer_origin{};
    int resizing_column{-1};
    int resize_origin_x{};
    int resize_origin_width{};
    std::optional<std::size_t> sort_column;
    bool sort_ascending{true};
    bool marquee_enabled{};
    bool marquee_armed{};
    bool marquee_active{};
    POINT marquee_origin{};
    POINT marquee_current{};
    std::vector<int> marquee_base_selection;
};

// 所有表格在同一个 UI 线程按顺序绘制，因此共享可增长后备位图，避免每个窗格常驻一份
// 大尺寸 bitmap；一次 WM_PAINT 结束前不会被另一个表格并发使用。
struct SharedBackBuffer {
    HDC dc{};
    HBITMAP bitmap{};
    HGDIOBJ previous_bitmap{};
    SIZE capacity{};

    ~SharedBackBuffer() {
        if (dc && previous_bitmap) {
            SelectObject(dc, previous_bitmap);
        }
        if (bitmap) {
            DeleteObject(bitmap);
        }
        if (dc) {
            DeleteDC(dc);
        }
    }
};

SharedBackBuffer& TableBackBuffer() {
    static SharedBackBuffer buffer;
    return buffer;
}

struct TablePaintResources {
    COLORREF panel_color{CLR_INVALID};
    COLORREF panel_alt_color{CLR_INVALID};
    COLORREF selected_color{CLR_INVALID};
    COLORREF hover_color{CLR_INVALID};
    COLORREF border_color{CLR_INVALID};
    COLORREF accent_color{CLR_INVALID};
    HBRUSH panel{};
    HBRUSH panel_alt{};
    HBRUSH selected{};
    HBRUSH hover{};
    HBRUSH border{};
    HBRUSH accent{};
    HPEN separator{};

    ~TablePaintResources() {
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
        if (accent)
            DeleteObject(accent);
        if (separator)
            DeleteObject(separator);
        panel = nullptr;
        panel_alt = nullptr;
        selected = nullptr;
        hover = nullptr;
        border = nullptr;
        accent = nullptr;
        separator = nullptr;
    }

    void Ensure() {
        if (panel && panel_color == theme::kPanelColor &&
            panel_alt_color == theme::kPanelAltColor &&
            selected_color == theme::kSelectedRowColor && hover_color == theme::kPanelHoverColor &&
            border_color == theme::kBorderColor && accent_color == theme::kAccentColor) {
            return;
        }
        Reset();
        panel_color = theme::kPanelColor;
        panel_alt_color = theme::kPanelAltColor;
        selected_color = theme::kSelectedRowColor;
        hover_color = theme::kPanelHoverColor;
        border_color = theme::kBorderColor;
        accent_color = theme::kAccentColor;
        panel = CreateSolidBrush(panel_color);
        panel_alt = CreateSolidBrush(panel_alt_color);
        selected = CreateSolidBrush(selected_color);
        hover = CreateSolidBrush(hover_color);
        border = CreateSolidBrush(border_color);
        accent = CreateSolidBrush(accent_color);
        separator = CreatePen(PS_SOLID, 1, border_color);
    }
};

TablePaintResources& TableResources() {
    static TablePaintResources resources;
    resources.Ensure();
    return resources;
}

UINT WindowDpi(HWND window) {
    using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
    static const auto get_dpi = reinterpret_cast<GetDpiForWindowFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    return get_dpi ? get_dpi(window) : 96U;
}

int Scale(HWND window, int value) {
    return MulDiv(value, static_cast<int>(WindowDpi(window)), 96);
}

State* GetState(HWND window) {
    return reinterpret_cast<State*>(GetWindowLongPtrW(window, GWLP_USERDATA));
}

int TotalWidth(const State& state) {
    return std::accumulate(
        state.columns.begin(), state.columns.end(), 0, [](const int total, const Column& column) {
            return total + std::max(1, column.width);
        });
}

int ContentHeight(const State& state) {
    const std::size_t maximum_rows =
        static_cast<std::size_t>((std::numeric_limits<int>::max)() / std::max(1, state.row_height));
    return static_cast<int>(std::min(state.rows, maximum_rows)) * state.row_height;
}

int ScrollbarThickness(const State& state) {
    return Scale(state.window, 8);
}

int ViewportWidth(const State& state, const RECT& client) {
    (void)state;
    return std::max(1, static_cast<int>(client.right - client.left));
}

int ViewportBodyHeight(const State& state, const RECT& client) {
    return std::max(1,
                    static_cast<int>(client.bottom - client.top) - state.header_height -
                        (state.show_horizontal_scrollbar ? ScrollbarThickness(state) : 0));
}

void FitLastColumn(State& state) {
    if (!state.fill_last_column || state.columns.empty()) {
        return;
    }
    RECT client{};
    GetClientRect(state.window, &client);
    int preceding_width = 0;
    for (std::size_t column = 0; column + 1 < state.columns.size(); ++column) {
        preceding_width += std::max(1, state.columns[column].width);
    }
    Column& last = state.columns.back();
    const int available =
        std::max(0, static_cast<int>(client.right - client.left) - preceding_width);
    last.width = std::max(last.minimum_width, available);
}

void UpdateScrollbars(State& state) {
    FitLastColumn(state);
    RECT client{};
    GetClientRect(state.window, &client);
    const int full_width = std::max(1, static_cast<int>(client.right - client.left));
    const int full_body_height =
        std::max(1, static_cast<int>(client.bottom - client.top) - state.header_height);
    const int thickness = ScrollbarThickness(state);
    bool horizontal = TotalWidth(state) > full_width;
    bool vertical = ContentHeight(state) > full_body_height - (horizontal ? thickness : 0);
    if (horizontal && !vertical && ContentHeight(state) > full_body_height - thickness) {
        vertical = true;
    }
    state.show_horizontal_scrollbar = horizontal;
    state.show_vertical_scrollbar = vertical;
    const int viewport_width = ViewportWidth(state, client);
    const int viewport_height = ViewportBodyHeight(state, client);
    state.scroll_x = std::clamp(state.scroll_x, 0, std::max(0, TotalWidth(state) - viewport_width));
    state.scroll_y =
        std::clamp(state.scroll_y, 0, std::max(0, ContentHeight(state) - viewport_height));
    if (!horizontal && (state.hovered_scroll_part == ScrollPart::horizontal_thumb ||
                        state.hovered_scroll_part == ScrollPart::horizontal_track)) {
        state.hovered_scroll_part = ScrollPart::none;
    }
    if (!vertical && (state.hovered_scroll_part == ScrollPart::vertical_thumb ||
                      state.hovered_scroll_part == ScrollPart::vertical_track)) {
        state.hovered_scroll_part = ScrollPart::none;
    }
}

void Notify(State& state, UINT code, int row = -1, POINT point = {}) {
    NMITEMACTIVATE notification{};
    notification.hdr.hwndFrom = state.window;
    notification.hdr.idFrom = static_cast<UINT_PTR>(state.control_id);
    notification.hdr.code = code;
    notification.iItem = row;
    notification.ptAction = point;
    SendMessageW(state.parent,
                 WM_NOTIFY,
                 static_cast<WPARAM>(state.control_id),
                 reinterpret_cast<LPARAM>(&notification));
}

bool ContainsSelection(const State& state, int row) {
    return std::binary_search(state.selected.begin(), state.selected.end(), row);
}

void SelectionChanged(State& state) {
    InvalidateRect(state.window, nullptr, FALSE);
    Notify(state, LVN_ITEMCHANGED, state.focused_row);
}

void ClearSelectionInternal(State& state, bool notify) {
    if (state.selected.empty() && state.focused_row < 0) {
        return;
    }
    state.selected.clear();
    state.focused_row = -1;
    state.anchor_row = -1;
    if (notify) {
        SelectionChanged(state);
    }
}

void SelectInternal(State& state, int row, bool clear_existing, bool toggle, bool range) {
    if (row < 0 || static_cast<std::size_t>(row) >= state.rows) {
        ClearSelectionInternal(state, true);
        return;
    }
    const std::vector<int> previous = state.selected;
    if (state.single_selection) {
        clear_existing = true;
        toggle = false;
        range = false;
    }
    if (range && state.anchor_row >= 0) {
        if (clear_existing) {
            state.selected.clear();
        }
        const int first = std::min(state.anchor_row, row);
        const int last = std::max(state.anchor_row, row);
        for (int current = first; current <= last; ++current) {
            state.selected.push_back(current);
        }
    } else if (toggle) {
        const auto found = std::lower_bound(state.selected.begin(), state.selected.end(), row);
        if (found != state.selected.end() && *found == row) {
            state.selected.erase(found);
        } else {
            state.selected.insert(found, row);
        }
        state.anchor_row = row;
    } else {
        if (clear_existing) {
            state.selected.assign(1, row);
        } else if (!ContainsSelection(state, row)) {
            state.selected.push_back(row);
        }
        state.anchor_row = row;
    }
    std::ranges::sort(state.selected);
    state.selected.erase(std::unique(state.selected.begin(), state.selected.end()),
                         state.selected.end());
    state.focused_row = row;
    if (state.selected != previous) {
        SelectionChanged(state);
    } else {
        InvalidateRect(state.window, nullptr, FALSE);
    }
}

RECT MarqueeRect(const State& state) {
    RECT client{};
    GetClientRect(state.window, &client);
    const int body_bottom = state.header_height + ViewportBodyHeight(state, client);
    return {std::clamp<LONG>((std::min)(state.marquee_origin.x, state.marquee_current.x),
                             client.left,
                             ViewportWidth(state, client)),
            std::clamp<LONG>((std::min)(state.marquee_origin.y, state.marquee_current.y),
                             state.header_height,
                             body_bottom),
            std::clamp<LONG>((std::max)(state.marquee_origin.x, state.marquee_current.x),
                             client.left,
                             ViewportWidth(state, client)),
            std::clamp<LONG>((std::max)(state.marquee_origin.y, state.marquee_current.y),
                             state.header_height,
                             body_bottom)};
}

void UpdateMarqueeSelection(State& state, const POINT point) {
    state.marquee_current = point;
    const RECT rectangle = MarqueeRect(state);
    std::vector<int> selected = state.marquee_base_selection;
    if (rectangle.bottom > rectangle.top && rectangle.right > rectangle.left && state.rows != 0) {
        const int content_top = rectangle.top - state.header_height + state.scroll_y;
        const int content_bottom = rectangle.bottom - state.header_height + state.scroll_y - 1;
        const int first = std::clamp(
            content_top / std::max(1, state.row_height), 0, static_cast<int>(state.rows) - 1);
        const int last = std::clamp(
            content_bottom / std::max(1, state.row_height), 0, static_cast<int>(state.rows) - 1);
        for (int row = first; row <= last; ++row) {
            selected.push_back(row);
        }
    }
    std::ranges::sort(selected);
    selected.erase(std::unique(selected.begin(), selected.end()), selected.end());
    if (selected == state.selected) {
        InvalidateRect(state.window, nullptr, FALSE);
        return;
    }
    state.selected = std::move(selected);
    state.focused_row = state.selected.empty() ? -1 : state.selected.back();
    state.anchor_row = state.focused_row;
    SelectionChanged(state);
}

int HitRow(const State& state, POINT point) {
    RECT client{};
    GetClientRect(state.window, &client);
    if (point.y < state.header_height || point.x >= ViewportWidth(state, client) ||
        point.y >= state.header_height + ViewportBodyHeight(state, client)) {
        return -1;
    }
    const int body_y = point.y - state.header_height + state.scroll_y;
    if (body_y < 0) {
        return -1;
    }
    const int row = body_y / std::max(1, state.row_height);
    return row >= 0 && static_cast<std::size_t>(row) < state.rows ? row : -1;
}

int TreeSlotWidth(const State& state) {
    return Scale(state.window, 18);
}

std::optional<RECT> TreeToggleRect(const State& state, const int row) {
    if (!state.tree_provider || row < 0 || static_cast<std::size_t>(row) >= state.rows) {
        return std::nullopt;
    }
    const auto tree = state.tree_provider(static_cast<std::size_t>(row));
    if (!tree || !tree->expandable)
        return std::nullopt;
    const int row_top = state.header_height + row * state.row_height - state.scroll_y;
    const int slot = TreeSlotWidth(state);
    const int left =
        -state.scroll_x + Scale(state.window, 7) + static_cast<int>(tree->depth) * slot;
    return RECT{left, row_top, left + slot, row_top + state.row_height};
}

int HeaderSeparatorAt(const State& state, POINT point) {
    RECT client{};
    GetClientRect(state.window, &client);
    if (point.y < 0 || point.y >= state.header_height || point.x >= ViewportWidth(state, client)) {
        return -1;
    }
    int edge = -state.scroll_x;
    const int tolerance = Scale(state.window, 4);
    for (std::size_t index = 0; index + 1 < state.columns.size(); ++index) {
        edge += state.columns[index].width;
        if (std::abs(point.x - edge) <= tolerance) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

int HeaderColumnAt(const State& state, POINT point) {
    RECT client{};
    GetClientRect(state.window, &client);
    if (point.y < 0 || point.y >= state.header_height || point.x < client.left ||
        point.x >= ViewportWidth(state, client)) {
        return -1;
    }
    const int content_x = point.x + state.scroll_x;
    int right = 0;
    for (std::size_t index = 0; index < state.columns.size(); ++index) {
        right += std::max(1, state.columns[index].width);
        if (content_x < right) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

RECT HorizontalTrack(const State& state, const RECT& client) {
    if (!state.show_horizontal_scrollbar) {
        return {};
    }
    const int thickness = ScrollbarThickness(state);
    return {client.left,
            client.bottom - thickness,
            client.right - (state.show_vertical_scrollbar ? thickness : 0),
            client.bottom};
}

RECT VerticalTrack(const State& state, const RECT& client) {
    if (!state.show_vertical_scrollbar) {
        return {};
    }
    const int thickness = ScrollbarThickness(state);
    return {client.right - thickness,
            client.top + state.header_height,
            client.right,
            client.bottom - (state.show_horizontal_scrollbar ? thickness : 0)};
}

RECT HorizontalThumb(const State& state, const RECT& client) {
    RECT track = HorizontalTrack(state, client);
    if (track.right <= track.left) {
        return {};
    }
    const int track_length = track.right - track.left;
    const int viewport = ViewportWidth(state, client);
    const int total = std::max(viewport, TotalWidth(state));
    const int thumb_length =
        std::clamp(MulDiv(track_length, viewport, total), Scale(state.window, 22), track_length);
    const int maximum_scroll = std::max(0, total - viewport);
    const int travel = std::max(0, track_length - thumb_length);
    const int offset = maximum_scroll > 0 ? MulDiv(state.scroll_x, travel, maximum_scroll) : 0;
    const int inset = Scale(state.window, 1);
    return {track.left + offset + inset,
            track.top + inset,
            track.left + offset + thumb_length - inset,
            track.bottom - inset};
}

RECT VerticalThumb(const State& state, const RECT& client) {
    RECT track = VerticalTrack(state, client);
    if (track.bottom <= track.top) {
        return {};
    }
    const int track_length = track.bottom - track.top;
    const int viewport = ViewportBodyHeight(state, client);
    const int total = std::max(viewport, ContentHeight(state));
    const int thumb_length =
        std::clamp(MulDiv(track_length, viewport, total), Scale(state.window, 22), track_length);
    const int maximum_scroll = std::max(0, total - viewport);
    const int travel = std::max(0, track_length - thumb_length);
    const int offset = maximum_scroll > 0 ? MulDiv(state.scroll_y, travel, maximum_scroll) : 0;
    const int inset = Scale(state.window, 1);
    return {track.left + inset,
            track.top + offset + inset,
            track.right - inset,
            track.top + offset + thumb_length - inset};
}

ScrollPart ScrollPartAt(const State& state, POINT point) {
    RECT client{};
    GetClientRect(state.window, &client);
    RECT thumb = VerticalThumb(state, client);
    if (state.show_vertical_scrollbar && PtInRect(&thumb, point)) {
        return ScrollPart::vertical_thumb;
    }
    RECT track = VerticalTrack(state, client);
    if (state.show_vertical_scrollbar && PtInRect(&track, point)) {
        return ScrollPart::vertical_track;
    }
    thumb = HorizontalThumb(state, client);
    if (state.show_horizontal_scrollbar && PtInRect(&thumb, point)) {
        return ScrollPart::horizontal_thumb;
    }
    track = HorizontalTrack(state, client);
    if (state.show_horizontal_scrollbar && PtInRect(&track, point)) {
        return ScrollPart::horizontal_track;
    }
    return ScrollPart::none;
}

void SetScrollFromThumb(State& state, POINT point) {
    RECT client{};
    GetClientRect(state.window, &client);
    if (state.dragged_scroll_part == ScrollPart::vertical_thumb) {
        const RECT track = VerticalTrack(state, client);
        const RECT thumb = VerticalThumb(state, client);
        const int thumb_length = thumb.bottom - thumb.top;
        const int travel = std::max(1, static_cast<int>(track.bottom - track.top) - thumb_length);
        const int position =
            std::clamp(static_cast<int>(point.y - track.top) - state.scroll_drag_offset, 0, travel);
        const int maximum_scroll =
            std::max(0, ContentHeight(state) - ViewportBodyHeight(state, client));
        state.scroll_y = MulDiv(position, maximum_scroll, travel);
    } else if (state.dragged_scroll_part == ScrollPart::horizontal_thumb) {
        const RECT track = HorizontalTrack(state, client);
        const RECT thumb = HorizontalThumb(state, client);
        const int thumb_length = thumb.right - thumb.left;
        const int travel = std::max(1, static_cast<int>(track.right - track.left) - thumb_length);
        const int position = std::clamp(
            static_cast<int>(point.x - track.left) - state.scroll_drag_offset, 0, travel);
        const int maximum_scroll = std::max(0, TotalWidth(state) - ViewportWidth(state, client));
        state.scroll_x = MulDiv(position, maximum_scroll, travel);
    }
    InvalidateRect(state.window, nullptr, FALSE);
}

void ReleaseBackBuffer(SharedBackBuffer& buffer) {
    if (buffer.dc && buffer.previous_bitmap) {
        SelectObject(buffer.dc, buffer.previous_bitmap);
    }
    if (buffer.bitmap) {
        DeleteObject(buffer.bitmap);
    }
    if (buffer.dc) {
        DeleteDC(buffer.dc);
    }
    buffer.dc = nullptr;
    buffer.bitmap = nullptr;
    buffer.previous_bitmap = nullptr;
    buffer.capacity = {};
}

bool EnsureBackBuffer(SharedBackBuffer& buffer, HDC target, int width, int height) {
    if (buffer.dc && buffer.bitmap && buffer.capacity.cx >= width && buffer.capacity.cy >= height) {
        return true;
    }
    const int capacity_width = std::max(width, static_cast<int>(buffer.capacity.cx));
    const int capacity_height = std::max(height, static_cast<int>(buffer.capacity.cy));
    ReleaseBackBuffer(buffer);
    buffer.dc = CreateCompatibleDC(target);
    buffer.bitmap = CreateCompatibleBitmap(target, capacity_width, capacity_height);
    if (!buffer.dc || !buffer.bitmap) {
        ReleaseBackBuffer(buffer);
        return false;
    }
    buffer.previous_bitmap = SelectObject(buffer.dc, buffer.bitmap);
    buffer.capacity = {capacity_width, capacity_height};
    return true;
}

void EnsureRowVisible(State& state, int row) {
    if (row < 0) {
        return;
    }
    RECT client{};
    GetClientRect(state.window, &client);
    const int body_height = ViewportBodyHeight(state, client);
    const int top = row * state.row_height;
    const int bottom = top + state.row_height;
    if (top < state.scroll_y) {
        state.scroll_y = top;
    } else if (bottom > state.scroll_y + body_height) {
        state.scroll_y = bottom - body_height;
    }
    UpdateScrollbars(state);
}

void Draw(State& state, HDC target, const RECT& client, const RECT& dirty) {
    const int width = std::max(1, static_cast<int>(client.right));
    const int height = std::max(1, static_cast<int>(client.bottom));
    auto& shared_buffer = TableBackBuffer();
    const bool buffered = EnsureBackBuffer(shared_buffer, target, width, height);
    HDC buffer = buffered ? shared_buffer.dc : target;
    const int saved_dirty_dc = SaveDC(buffer);
    IntersectClipRect(buffer, dirty.left, dirty.top, dirty.right, dirty.bottom);
    auto& resources = TableResources();
    const HGDIOBJ previous_font = state.font ? SelectObject(buffer, state.font) : nullptr;
    SetBkMode(buffer, TRANSPARENT);

    FillRect(buffer, &client, resources.panel);

    const int viewport_right = client.left + ViewportWidth(state, client);
    RECT header{client.left,
                client.top,
                viewport_right,
                std::min(static_cast<int>(client.bottom), state.header_height)};
    FillRect(buffer, &header, resources.panel);

    const HGDIOBJ previous_pen = SelectObject(buffer, resources.separator);
    SetTextColor(buffer, theme::kMutedTextColor);
    int x = -state.scroll_x;
    for (std::size_t column = 0; column < state.columns.size(); ++column) {
        const int column_width = std::max(1, state.columns[column].width);
        RECT cell{x, header.top, x + column_width, header.bottom};
        if (cell.right > client.left && cell.left < viewport_right) {
            RECT text = cell;
            text.left += Scale(state.window, 8);
            text.right =
                std::min(static_cast<int>(text.right) - Scale(state.window, 8), viewport_right);
            if (state.sort_column && *state.sort_column == column) {
                RECT indicator = text;
                const int indicator_width = Scale(state.window, 14);
                indicator.left = std::max(static_cast<int>(indicator.left),
                                          static_cast<int>(indicator.right) - indicator_width);
                text.right = std::max(static_cast<int>(text.left),
                                      static_cast<int>(indicator.left) - Scale(state.window, 2));
                DrawTextW(buffer,
                          state.sort_ascending ? L"▲" : L"▼",
                          -1,
                          &indicator,
                          DT_SINGLELINE | DT_RIGHT | DT_VCENTER | DT_NOPREFIX);
            }
            UINT format = DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX;
            format |= state.columns[column].alignment == Alignment::right    ? DT_RIGHT
                      : state.columns[column].alignment == Alignment::center ? DT_CENTER
                                                                             : DT_LEFT;
            DrawTextW(buffer, state.columns[column].title.c_str(), -1, &text, format);
        }
        x += column_width;
    }
    MoveToEx(buffer, client.left, header.bottom - 1, nullptr);
    LineTo(buffer, viewport_right, header.bottom - 1);
    SelectObject(buffer, previous_pen);

    RECT body{client.left,
              header.bottom,
              viewport_right,
              header.bottom + ViewportBodyHeight(state, client)};
    const int first_row = std::max(
        0, (static_cast<int>(std::max(body.top, dirty.top) - body.top) + state.scroll_y) /
               state.row_height);
    const int visible_rows = std::max(
        0, (static_cast<int>(std::min(body.bottom, dirty.bottom) - body.top) + state.scroll_y +
            state.row_height - 1) /
                   state.row_height -
               first_row);
    const int last_row =
        std::min(static_cast<int>((std::min)(
                     state.rows, static_cast<std::size_t>((std::numeric_limits<int>::max)()))),
                 first_row + visible_rows);
    const int saved_body_dc = SaveDC(buffer);
    IntersectClipRect(buffer, body.left, body.top, body.right, body.bottom);
    for (int row = first_row; row < last_row; ++row) {
        const int row_top = body.top + row * state.row_height - state.scroll_y;
        RECT row_rect{body.left, row_top, body.right, row_top + state.row_height};
        if (ContainsSelection(state, row)) {
            FillRect(buffer, &row_rect, resources.selected);
        } else if (row == state.drop_target_row || row == state.hover_row) {
            FillRect(buffer, &row_rect, resources.hover);
        }
        SetTextColor(buffer, theme::kTextColor);
        x = -state.scroll_x;
        for (std::size_t column = 0; column < state.columns.size(); ++column) {
            const HFONT column_font =
                column < state.column_fonts.size() ? state.column_fonts[column] : nullptr;
            const HGDIOBJ previous_column_font =
                column_font ? SelectObject(buffer, column_font) : nullptr;
            const int column_width = std::max(1, state.columns[column].width);
            RECT cell{x, row_rect.top, x + column_width, row_rect.bottom};
            if (cell.right > body.left && cell.left < body.right) {
                if (state.progress_provider) {
                    const auto progress =
                        state.progress_provider(static_cast<std::size_t>(row), column);
                    if (progress) {
                        RECT track = cell;
                        InflateRect(&track, -Scale(state.window, 2), -Scale(state.window, 3));
                        track.right = std::min<LONG>(track.right, body.right);
                        if (track.right > track.left && track.bottom > track.top) {
                            FillRect(buffer, &track, resources.panel_alt);
                            RECT fill = track;
                            fill.right =
                                fill.left + static_cast<int>((fill.right - fill.left) *
                                                             std::min(100U, *progress) / 100U);
                            if (fill.right > fill.left) {
                                FillRect(buffer, &fill, resources.accent);
                            }
                        }
                    }
                }
                RECT text = cell;
                text.left += Scale(state.window, 7);
                text.right = std::min(static_cast<int>(text.right) - Scale(state.window, 7),
                                      static_cast<int>(body.right));
                if (column == 0 && state.images && state.image_provider) {
                    if (state.tree_provider) {
                        const auto tree = state.tree_provider(static_cast<std::size_t>(row));
                        if (tree) {
                            const int slot = TreeSlotWidth(state);
                            const int half = slot / 2;
                            const int center_y = row_rect.top + state.row_height / 2;
                            const int base_x = text.left;
                            const HGDIOBJ old_tree_pen = SelectObject(buffer, resources.separator);
                            for (std::size_t level = 0; level + 1 < tree->depth && level < 64;
                                 ++level) {
                                if ((tree->ancestor_lines & (std::uint64_t{1} << level)) == 0) {
                                    continue;
                                }
                                const int line_x = base_x + static_cast<int>(level) * slot + half;
                                MoveToEx(buffer, line_x, row_rect.top, nullptr);
                                LineTo(buffer, line_x, row_rect.bottom);
                            }
                            if (tree->depth > 0) {
                                const int parent_x =
                                    base_x + (static_cast<int>(tree->depth) - 1) * slot + half;
                                MoveToEx(buffer, parent_x, row_rect.top, nullptr);
                                LineTo(buffer,
                                       parent_x,
                                       tree->last_sibling ? center_y : row_rect.bottom);
                                LineTo(buffer,
                                       base_x + static_cast<int>(tree->depth) * slot + half,
                                       center_y);
                            }
                            if (tree->expandable) {
                                const int box_size = Scale(state.window, 11);
                                const int box_left = base_x + static_cast<int>(tree->depth) * slot +
                                                     (slot - box_size) / 2;
                                RECT box{box_left,
                                         center_y - box_size / 2,
                                         box_left + box_size,
                                         center_y - box_size / 2 + box_size};
                                FillRect(buffer, &box, resources.panel_alt);
                                FrameRect(buffer, &box, resources.border);
                                const int glyph_pad = Scale(state.window, 3);
                                MoveToEx(buffer, box.left + glyph_pad, center_y, nullptr);
                                LineTo(buffer, box.right - glyph_pad, center_y);
                                if (!tree->expanded) {
                                    const int center_x = (box.left + box.right) / 2;
                                    MoveToEx(buffer, center_x, box.top + glyph_pad, nullptr);
                                    LineTo(buffer, center_x, box.bottom - glyph_pad);
                                }
                            }
                            SelectObject(buffer, old_tree_pen);
                            text.left += (static_cast<int>(tree->depth) + 1) * slot;
                        }
                    } else if (state.indent_provider) {
                        text.left +=
                            std::max(0, state.indent_provider(static_cast<std::size_t>(row)));
                    }
                    const int image = state.image_provider(static_cast<std::size_t>(row));
                    if (image >= 0) {
                        int icon_width = Scale(state.window, 16);
                        int icon_height = icon_width;
                        ImageList_GetIconSize(state.images, &icon_width, &icon_height);
                        const int icon_y = row_rect.top + (state.row_height - icon_height) / 2;
                        ImageList_Draw(
                            state.images, image, buffer, text.left, icon_y, ILD_TRANSPARENT);
                        text.left += icon_width + Scale(state.window, 5);
                    }
                }
                const std::wstring value =
                    state.text_provider ? state.text_provider(static_cast<std::size_t>(row), column)
                                        : std::wstring{};
                UINT format = DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX;
                format |= state.columns[column].alignment == Alignment::right    ? DT_RIGHT
                          : state.columns[column].alignment == Alignment::center ? DT_CENTER
                                                                                 : DT_LEFT;
                DrawTextW(buffer, value.c_str(), -1, &text, format);
            }
            if (previous_column_font) {
                SelectObject(buffer, previous_column_font);
            }
            x += column_width;
        }
    }
    RestoreDC(buffer, saved_body_dc);

    if (state.marquee_active) {
        RECT marquee = MarqueeRect(state);
        if (marquee.right > marquee.left && marquee.bottom > marquee.top) {
            FrameRect(buffer, &marquee, resources.accent);
        }
    }

    const bool hot_scrollbar = state.dragged_scroll_part != ScrollPart::none ||
                               state.hovered_scroll_part != ScrollPart::none;
    const HBRUSH scroll_thumb = hot_scrollbar ? resources.border : resources.hover;
    const HGDIOBJ old_brush = SelectObject(buffer, scroll_thumb);
    const HGDIOBJ old_scroll_pen = SelectObject(buffer, GetStockObject(NULL_PEN));
    const auto draw_scrollbar = [&](const RECT track, const RECT thumb) {
        if (track.right <= track.left || track.bottom <= track.top) {
            return;
        }
        FillRect(buffer, &track, resources.panel_alt);
        const int radius = Scale(state.window, 5);
        RoundRect(buffer, thumb.left, thumb.top, thumb.right, thumb.bottom, radius, radius);
    };
    if (state.show_vertical_scrollbar) {
        draw_scrollbar(VerticalTrack(state, client), VerticalThumb(state, client));
    }
    if (state.show_horizontal_scrollbar) {
        draw_scrollbar(HorizontalTrack(state, client), HorizontalThumb(state, client));
    }
    SelectObject(buffer, old_scroll_pen);
    SelectObject(buffer, old_brush);

    if (state.border) {
        FrameRect(buffer, &client, resources.border);
    }

    if (buffered) {
        BitBlt(target, dirty.left, dirty.top, dirty.right - dirty.left,
               dirty.bottom - dirty.top, buffer, dirty.left, dirty.top, SRCCOPY);
    }
    if (previous_font) {
        SelectObject(buffer, previous_font);
    }
    RestoreDC(buffer, saved_dirty_dc);
}

void ScrollBy(State& state, bool vertical, int delta) {
    RECT client{};
    GetClientRect(state.window, &client);
    if (vertical) {
        const int maximum = std::max(0, ContentHeight(state) - ViewportBodyHeight(state, client));
        state.scroll_y = std::clamp(state.scroll_y + delta, 0, maximum);
    } else {
        const int maximum = std::max(0, TotalWidth(state) - ViewportWidth(state, client));
        state.scroll_x = std::clamp(state.scroll_x + delta, 0, maximum);
    }
    InvalidateRect(state.window, nullptr, FALSE);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    State* state = GetState(window);
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        state = new State{};
        state->window = window;
        state->parent = create->hwndParent;
        state->control_id = static_cast<int>(reinterpret_cast<INT_PTR>(create->hMenu));
        const auto* parameters = static_cast<const CreateParameters*>(create->lpCreateParams);
        state->single_selection = parameters && parameters->single_selection;
        state->border = parameters && parameters->border;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (!state) {
        return DefWindowProcW(window, message, wparam, lparam);
    }
    switch (message) {
    case WM_CREATE:
    case WM_SIZE:
        state->row_height = Scale(window, 22);
        state->header_height = Scale(window, 27);
        UpdateScrollbars(*state);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_SETFONT:
        state->font = reinterpret_cast<HFONT>(wparam);
        if (LOWORD(lparam)) {
            InvalidateRect(window, nullptr, FALSE);
        }
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
        Draw(*state, dc, client, paint.rcPaint);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        const int lines = std::max(1, std::abs(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA * 3);
        const int direction = GET_WHEEL_DELTA_WPARAM(wparam) > 0 ? -1 : 1;
        const bool horizontal =
            (GET_KEYSTATE_WPARAM(wparam) & MK_SHIFT) != 0 ||
            (!state->show_vertical_scrollbar && state->show_horizontal_scrollbar);
        ScrollBy(*state,
                 !horizontal,
                 direction * lines * (horizontal ? Scale(window, 32) : state->row_height));
        return 0;
    }
    case WM_SETCURSOR: {
        POINT point{};
        GetCursorPos(&point);
        ScreenToClient(window, &point);
        if (ScrollPartAt(*state, point) != ScrollPart::none) {
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            return TRUE;
        }
        if (state->resizing_column >= 0 || HeaderSeparatorAt(*state, point) >= 0) {
            SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
            return TRUE;
        }
        break;
    }
    case WM_LBUTTONDOWN: {
        SetFocus(window);
        POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        const ScrollPart scroll_part = ScrollPartAt(*state, point);
        if (scroll_part != ScrollPart::none) {
            RECT client{};
            GetClientRect(window, &client);
            if (scroll_part == ScrollPart::vertical_thumb) {
                const RECT thumb = VerticalThumb(*state, client);
                state->dragged_scroll_part = scroll_part;
                state->scroll_drag_offset = point.y - thumb.top;
                SetCapture(window);
            } else if (scroll_part == ScrollPart::horizontal_thumb) {
                const RECT thumb = HorizontalThumb(*state, client);
                state->dragged_scroll_part = scroll_part;
                state->scroll_drag_offset = point.x - thumb.left;
                SetCapture(window);
            } else if (scroll_part == ScrollPart::vertical_track) {
                const RECT thumb = VerticalThumb(*state, client);
                ScrollBy(*state,
                         true,
                         point.y < thumb.top ? -ViewportBodyHeight(*state, client)
                                             : ViewportBodyHeight(*state, client));
            } else {
                const RECT thumb = HorizontalThumb(*state, client);
                ScrollBy(*state,
                         false,
                         point.x < thumb.left ? -ViewportWidth(*state, client)
                                              : ViewportWidth(*state, client));
            }
            return 0;
        }
        const int separator = HeaderSeparatorAt(*state, point);
        if (separator >= 0) {
            state->resizing_column = separator;
            state->resize_origin_x = point.x;
            state->resize_origin_width = state->columns[static_cast<std::size_t>(separator)].width;
            SetCapture(window);
            return 0;
        }
        if (point.y < state->header_height) {
            const int column = HeaderColumnAt(*state, point);
            if (column >= 0 && state->header_click_handler) {
                state->header_click_handler(static_cast<std::size_t>(column));
            }
            return 0;
        }
        const int row = HitRow(*state, point);
        const bool control = (wparam & MK_CONTROL) != 0;
        const bool shift = (wparam & MK_SHIFT) != 0;
        if (row >= 0) {
            if (const auto toggle = TreeToggleRect(*state, row);
                toggle && PtInRect(&*toggle, point)) {
                SelectInternal(*state, row, true, false, false);
                if (state->tree_toggle_handler) {
                    state->tree_toggle_handler(static_cast<std::size_t>(row));
                }
                return 0;
            }
            SelectInternal(*state, row, !control, control, shift);
            state->pointer_down = true;
            state->drag_sent = false;
            state->pointer_row = row;
            state->pointer_origin = point;
            SetCapture(window);
        } else if (point.y >= state->header_height) {
            const std::vector<int> previous_selection =
                control ? state->selected : std::vector<int>{};
            if (!control) {
                ClearSelectionInternal(*state, true);
            }
            if (state->marquee_enabled && !state->single_selection) {
                state->marquee_armed = true;
                state->marquee_active = false;
                state->marquee_origin = point;
                state->marquee_current = point;
                state->marquee_base_selection = previous_selection;
                SetCapture(window);
            }
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        if (state->dragged_scroll_part != ScrollPart::none) {
            SetScrollFromThumb(*state, point);
            return 0;
        }
        if (state->marquee_armed) {
            if (!state->marquee_active &&
                (std::abs(point.x - state->marquee_origin.x) >= GetSystemMetrics(SM_CXDRAG) ||
                 std::abs(point.y - state->marquee_origin.y) >= GetSystemMetrics(SM_CYDRAG))) {
                state->marquee_active = true;
            }
            if (state->marquee_active) {
                RECT client{};
                GetClientRect(window, &client);
                const int body_bottom = state->header_height + ViewportBodyHeight(*state, client);
                if (point.y < state->header_height) {
                    ScrollBy(*state, true, -state->row_height);
                } else if (point.y > body_bottom) {
                    ScrollBy(*state, true, state->row_height);
                }
                UpdateMarqueeSelection(*state, point);
            }
            return 0;
        }
        if (state->resizing_column >= 0) {
            Column& column = state->columns[static_cast<std::size_t>(state->resizing_column)];
            int width = std::max(column.minimum_width,
                                 state->resize_origin_width + static_cast<int>(point.x) -
                                     state->resize_origin_x);
            if (state->fill_last_column &&
                static_cast<std::size_t>(state->resizing_column) + 1 < state->columns.size()) {
                RECT client{};
                GetClientRect(window, &client);
                int reserved = state->columns.back().minimum_width;
                for (std::size_t index = 0; index + 1 < state->columns.size(); ++index) {
                    if (index != static_cast<std::size_t>(state->resizing_column)) {
                        reserved += std::max(1, state->columns[index].width);
                    }
                }
                const int maximum = std::max(
                    column.minimum_width, static_cast<int>(client.right - client.left) - reserved);
                width = std::min(width, maximum);
            }
            column.width = width;
            UpdateScrollbars(*state);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        const ScrollPart hovered_scroll = ScrollPartAt(*state, point);
        if (hovered_scroll != state->hovered_scroll_part) {
            state->hovered_scroll_part = hovered_scroll;
            InvalidateRect(window, nullptr, FALSE);
        }
        const int row = HitRow(*state, point);
        if (row != state->hover_row) {
            state->hover_row = row;
            InvalidateRect(window, nullptr, FALSE);
        }
        if (state->pointer_down && !state->drag_sent && state->pointer_row >= 0 &&
            (std::abs(point.x - state->pointer_origin.x) >= GetSystemMetrics(SM_CXDRAG) ||
             std::abs(point.y - state->pointer_origin.y) >= GetSystemMetrics(SM_CYDRAG))) {
            state->drag_sent = true;
            state->pointer_down = false;
            if (GetCapture() == window) {
                ReleaseCapture();
            }
            Notify(*state, LVN_BEGINDRAG, state->pointer_row, point);
        }
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
        TrackMouseEvent(&tracking);
        return 0;
    }
    case WM_MOUSELEAVE:
        state->hover_row = -1;
        state->hovered_scroll_part = ScrollPart::none;
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_LBUTTONUP:
        if (state->marquee_armed) {
            state->marquee_armed = false;
            state->marquee_active = false;
            state->marquee_base_selection.clear();
            InvalidateRect(window, nullptr, FALSE);
        }
        state->pointer_down = false;
        state->dragged_scroll_part = ScrollPart::none;
        if (state->resizing_column >= 0) {
            state->resizing_column = -1;
        }
        if (GetCapture() == window) {
            ReleaseCapture();
        }
        return 0;
    case WM_CAPTURECHANGED:
        state->pointer_down = false;
        state->marquee_armed = false;
        state->marquee_active = false;
        state->marquee_base_selection.clear();
        state->resizing_column = -1;
        state->dragged_scroll_part = ScrollPart::none;
        return 0;
    case WM_LBUTTONDBLCLK: {
        POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        const int row = HitRow(*state, point);
        if (row >= 0) {
            Notify(*state, NM_DBLCLK, row, point);
        }
        return 0;
    }
    case WM_RBUTTONUP: {
        SetFocus(window);
        POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        const int row = HitRow(*state, point);
        if (row >= 0 && !ContainsSelection(*state, row)) {
            SelectInternal(*state, row, true, false, false);
        }
        Notify(*state, NM_RCLICK, row, point);
        return 0;
    }
    case WM_KEYDOWN: {
        if (state->rows == 0) {
            return 0;
        }
        if ((GetKeyState(VK_CONTROL) & 0x8000) != 0 && (wparam == 'A' || wparam == 'a') &&
            !state->single_selection) {
            state->selected.resize(state->rows);
            std::iota(state->selected.begin(), state->selected.end(), 0);
            state->focused_row = state->selected.empty() ? -1 : state->selected.back();
            SelectionChanged(*state);
            return 0;
        }
        int row = state->focused_row >= 0 ? state->focused_row : 0;
        RECT client{};
        GetClientRect(window, &client);
        const int page = std::max(
            1, (static_cast<int>(client.bottom) - state->header_height) / state->row_height);
        switch (wparam) {
        case VK_UP:
            --row;
            break;
        case VK_DOWN:
            ++row;
            break;
        case VK_HOME:
            row = 0;
            break;
        case VK_END:
            row = static_cast<int>(state->rows - 1);
            break;
        case VK_PRIOR:
            row -= page;
            break;
        case VK_NEXT:
            row += page;
            break;
        default:
            return DefWindowProcW(window, message, wparam, lparam);
        }
        row = std::clamp(row, 0, static_cast<int>(state->rows - 1));
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        SelectInternal(*state, row, !shift, false, shift);
        EnsureRowVisible(*state, row);
        return 0;
    }
    case WM_SETFOCUS:
        Notify(*state, NM_SETFOCUS, state->focused_row);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_KILLFOCUS:
    case WM_THEMECHANGED:
    case WM_DPICHANGED_AFTERPARENT:
        UpdateScrollbars(*state);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS | DLGC_WANTCHARS;
    case WM_NCDESTROY:
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        delete state;
        return DefWindowProcW(window, message, wparam, lparam);
    default:
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

bool Register(HINSTANCE instance) {
    WNDCLASSEXW window_class{sizeof(window_class)};
    window_class.style = CS_DBLCLKS;
    window_class.lpfnWndProc = &WindowProc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = nullptr;
    window_class.lpszClassName = kTableClass;
    const ATOM atom = RegisterClassExW(&window_class);
    return atom != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

} // namespace

HWND Create(
    HINSTANCE instance, HWND parent, int control_id, bool single_selection, DWORD extra_style) {
    if (!Register(instance)) {
        return nullptr;
    }
    const CreateParameters parameters{single_selection, (extra_style & WS_BORDER) != 0};
    return CreateWindowExW(0,
                           kTableClass,
                           L"",
                           WS_CHILD | WS_VISIBLE | WS_TABSTOP | (extra_style & ~WS_BORDER),
                           0,
                           0,
                           0,
                           0,
                           parent,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(control_id)),
                           instance,
                           const_cast<CreateParameters*>(&parameters));
}

void SetColumns(HWND table, std::span<const Column> columns) {
    if (State* state = GetState(table)) {
        state->columns.assign(columns.begin(), columns.end());
        state->column_fonts.resize(state->columns.size(), nullptr);
        for (Column& column : state->columns) {
            column.minimum_width = std::max(1, column.minimum_width);
            column.width = std::max(column.minimum_width, column.width);
        }
        UpdateScrollbars(*state);
        InvalidateRect(table, nullptr, FALSE);
    }
}

void SetColumnFont(HWND table, std::size_t column, HFONT font) {
    if (State* state = GetState(table); state && column < state->columns.size()) {
        if (state->column_fonts.size() < state->columns.size()) {
            state->column_fonts.resize(state->columns.size(), nullptr);
        }
        state->column_fonts[column] = font;
        InvalidateRect(table, nullptr, FALSE);
    }
}

void SetFillLastColumn(HWND table, bool enabled) {
    if (State* state = GetState(table)) {
        state->fill_last_column = enabled;
        UpdateScrollbars(*state);
        InvalidateRect(table, nullptr, FALSE);
    }
}

void SetColumnWidths(HWND table, std::span<const int> widths) {
    if (State* state = GetState(table)) {
        const std::size_t count = std::min(widths.size(), state->columns.size());
        bool changed = false;
        for (std::size_t index = 0; index < count; ++index) {
            const int width = std::max(state->columns[index].minimum_width, widths[index]);
            changed = changed || state->columns[index].width != width;
            state->columns[index].width = width;
        }
        if (changed) {
            UpdateScrollbars(*state);
            InvalidateRect(table, nullptr, FALSE);
        }
    }
}

void SetColumnWidth(HWND table, std::size_t column, int width) {
    if (State* state = GetState(table); state && column < state->columns.size()) {
        state->columns[column].width = std::max(state->columns[column].minimum_width, width);
        UpdateScrollbars(*state);
        InvalidateRect(table, nullptr, FALSE);
    }
}

int ColumnWidth(HWND table, std::size_t column) {
    if (State* state = GetState(table); state && column < state->columns.size()) {
        return state->columns[column].width;
    }
    return 0;
}

std::size_t ColumnCount(HWND table) {
    const State* state = GetState(table);
    return state ? state->columns.size() : 0U;
}

void SetTextProvider(HWND table, TextProvider provider) {
    if (State* state = GetState(table)) {
        state->text_provider = std::move(provider);
        InvalidateRect(table, nullptr, FALSE);
    }
}

void SetImageProvider(HWND table, ImageProvider provider) {
    if (State* state = GetState(table)) {
        state->image_provider = std::move(provider);
        InvalidateRect(table, nullptr, FALSE);
    }
}

void SetIndentProvider(HWND table, IndentProvider provider) {
    if (State* state = GetState(table)) {
        state->indent_provider = std::move(provider);
        InvalidateRect(table, nullptr, FALSE);
    }
}

void SetTreeProvider(HWND table, TreeProvider provider, TreeToggleHandler toggle_handler) {
    if (State* state = GetState(table)) {
        state->tree_provider = std::move(provider);
        state->tree_toggle_handler = std::move(toggle_handler);
        InvalidateRect(table, nullptr, FALSE);
    }
}

void SetMarqueeSelection(HWND table, const bool enabled) {
    if (State* state = GetState(table)) {
        state->marquee_enabled = enabled;
        if (!enabled) {
            state->marquee_armed = false;
            state->marquee_active = false;
            state->marquee_base_selection.clear();
        }
        InvalidateRect(table, nullptr, FALSE);
    }
}

void SetProgressProvider(HWND table, ProgressProvider provider) {
    if (State* state = GetState(table)) {
        state->progress_provider = std::move(provider);
        InvalidateRect(table, nullptr, FALSE);
    }
}

void SetHeaderClickHandler(HWND table, HeaderClickHandler handler) {
    if (State* state = GetState(table)) {
        state->header_click_handler = std::move(handler);
    }
}

void SetSortIndicator(HWND table, const std::optional<std::size_t> column, const bool ascending) {
    if (State* state = GetState(table)) {
        state->sort_column = column && *column < state->columns.size() ? column : std::nullopt;
        state->sort_ascending = ascending;
        InvalidateRect(table, nullptr, FALSE);
    }
}

void SetImageList(HWND table, HIMAGELIST images) {
    if (State* state = GetState(table)) {
        state->images = images;
        InvalidateRect(table, nullptr, FALSE);
    }
}

void SetRowCount(HWND table, std::size_t rows, bool preserve_scroll) {
    if (State* state = GetState(table)) {
        state->rows = rows;
        std::erase_if(state->selected, [rows](const int row) {
            return row < 0 || static_cast<std::size_t>(row) >= rows;
        });
        if (state->focused_row >= 0 && static_cast<std::size_t>(state->focused_row) >= rows) {
            state->focused_row = state->selected.empty() ? -1 : state->selected.back();
        }
        if (state->drop_target_row >= 0 &&
            static_cast<std::size_t>(state->drop_target_row) >= rows) {
            state->drop_target_row = -1;
        }
        if (!preserve_scroll) {
            state->scroll_y = 0;
        }
        UpdateScrollbars(*state);
        InvalidateRect(table, nullptr, FALSE);
    }
}

std::size_t RowCount(HWND table) {
    const State* state = GetState(table);
    return state ? state->rows : 0U;
}

std::vector<int> SelectedRows(HWND table) {
    const State* state = GetState(table);
    return state ? state->selected : std::vector<int>{};
}

int SelectedRow(HWND table) {
    const State* state = GetState(table);
    return state && !state->selected.empty() ? state->selected.front() : -1;
}

bool IsRowSelected(HWND table, int row) {
    const State* state = GetState(table);
    return state && ContainsSelection(*state, row);
}

void SelectRow(HWND table, int row, bool clear_existing) {
    if (State* state = GetState(table)) {
        SelectInternal(*state, row, clear_existing, false, false);
        EnsureRowVisible(*state, row);
    }
}

void EnsureCellVisible(HWND table, const std::size_t row, const std::size_t column) {
    if (State* state = GetState(table)) {
        if (row >= state->rows || column >= state->columns.size())
            return;
        EnsureRowVisible(*state, static_cast<int>(row));
        RECT client{};
        GetClientRect(table, &client);
        int left = 0;
        for (std::size_t index = 0; index < column; ++index) {
            left += std::max(1, state->columns[index].width);
        }
        const int right = left + std::max(1, state->columns[column].width);
        const int viewport = ViewportWidth(*state, client);
        if (left < state->scroll_x) {
            state->scroll_x = left;
        } else if (right > state->scroll_x + viewport) {
            state->scroll_x = std::max(0, right - viewport);
        }
        UpdateScrollbars(*state);
        InvalidateRect(table, nullptr, FALSE);
    }
}

void ClearSelection(HWND table) {
    if (State* state = GetState(table)) {
        ClearSelectionInternal(*state, true);
    }
}

void SetDropTargetRow(HWND table, int row) {
    if (State* state = GetState(table)) {
        if (row < 0 || static_cast<std::size_t>(row) >= state->rows) {
            row = -1;
        }
        if (state->drop_target_row == row)
            return;
        state->drop_target_row = row;
        InvalidateRect(table, nullptr, FALSE);
    }
}

int HitTest(HWND table, POINT client_point) {
    const State* state = GetState(table);
    return state ? HitRow(*state, client_point) : -1;
}

std::optional<RECT> CellTextRect(HWND table, const std::size_t row, const std::size_t column) {
    const State* state = GetState(table);
    if (!state || row >= state->rows || column >= state->columns.size()) {
        return std::nullopt;
    }
    RECT client{};
    GetClientRect(table, &client);
    const int viewport_right = client.left + ViewportWidth(*state, client);
    const int body_bottom = state->header_height + ViewportBodyHeight(*state, client);
    const int row_top =
        state->header_height + static_cast<int>(row) * state->row_height - state->scroll_y;
    if (row_top + state->row_height <= state->header_height || row_top >= body_bottom) {
        return std::nullopt;
    }

    int x = -state->scroll_x;
    for (std::size_t index = 0; index < column; ++index) {
        x += std::max(1, state->columns[index].width);
    }
    RECT text{x + Scale(table, 7),
              row_top,
              x + std::max(1, state->columns[column].width) - Scale(table, 7),
              row_top + state->row_height};
    if (column == 0 && state->images && state->image_provider) {
        if (state->tree_provider) {
            if (const auto tree = state->tree_provider(row)) {
                text.left += (static_cast<int>(tree->depth) + 1) * TreeSlotWidth(*state);
            }
        } else if (state->indent_provider) {
            text.left += std::max(0, state->indent_provider(row));
        }
        const int image = state->image_provider(row);
        if (image >= 0) {
            int icon_width = Scale(table, 16);
            int icon_height = icon_width;
            ImageList_GetIconSize(state->images, &icon_width, &icon_height);
            text.left += icon_width + Scale(table, 5);
        }
    }
    text.left = std::max<LONG>(text.left, client.left);
    text.right = std::min<LONG>(text.right, viewport_right);
    text.top = std::max<LONG>(text.top, state->header_height);
    text.bottom = std::min<LONG>(text.bottom, body_bottom);
    if (text.right <= text.left || text.bottom <= text.top) {
        return std::nullopt;
    }
    return text;
}

void Refresh(HWND table, bool erase) {
    InvalidateRect(table, nullptr, erase ? TRUE : FALSE);
}

void RefreshRows(HWND table, std::span<const std::size_t> rows) {
    const State* state = GetState(table);
    if (!state || rows.empty()) {
        return;
    }
    RECT client{};
    GetClientRect(table, &client);
    const int body_bottom = state->header_height + ViewportBodyHeight(*state, client);
    std::vector<std::size_t> visible;
    visible.reserve(rows.size());
    for (const auto row : rows) {
        if (row >= state->rows || row > static_cast<std::size_t>(INT_MAX / state->row_height)) {
            continue;
        }
        const int top = state->header_height + static_cast<int>(row) * state->row_height -
                        state->scroll_y;
        if (top < body_bottom && top + state->row_height > state->header_height) {
            visible.push_back(row);
        }
    }
    std::sort(visible.begin(), visible.end());
    visible.erase(std::unique(visible.begin(), visible.end()), visible.end());
    for (std::size_t index = 0; index < visible.size();) {
        std::size_t end = index + 1;
        while (end < visible.size() && visible[end] == visible[end - 1] + 1) {
            ++end;
        }
        const int first = state->header_height +
                          static_cast<int>(visible[index]) * state->row_height - state->scroll_y;
        const int last = state->header_height +
                         static_cast<int>(visible[end - 1] + 1) * state->row_height -
                         state->scroll_y;
        RECT region{client.left, std::max(first, state->header_height), client.right,
                    std::min(last, body_bottom)};
        InvalidateRect(table, &region, FALSE);
        index = end;
    }
}

void ApplyTheme(HWND table) {
    if (State* state = GetState(table)) {
        UpdateScrollbars(*state);
        InvalidateRect(table, nullptr, TRUE);
    }
}

} // namespace SLink::ui::table
