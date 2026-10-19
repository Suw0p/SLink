#pragma once

#include <windows.h>
#include <commctrl.h>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <span>
#include <string>
#include <vector>

namespace SLink::ui::table {

// 项目统一使用的虚拟化自绘表格。
//
// 控件不为每一行创建 Win32 子窗口，绘制时只向 Provider 索取当前可见单元格数据，
// 因而可以承载十万级目录。Provider 在 UI 线程调用，必须快速且不得执行磁盘、网络
// 或数据库 I/O；异步结果应先写入页面模型，再调用 SetRowCount/Refresh 刷新。
enum class Alignment {
    left,
    center,
    right,
};

struct Column {
    std::wstring title;
    int width{};
    int minimum_width{24};
    Alignment alignment{Alignment::left};
};

struct TreeCell {
    std::size_t depth{};
    bool expandable{};
    bool expanded{};
    bool last_sibling{true};
    std::uint64_t ancestor_lines{};
};

using TextProvider = std::function<std::wstring(std::size_t row, std::size_t column)>;
using ImageProvider = std::function<int(std::size_t row)>;
using IndentProvider = std::function<int(std::size_t row)>;
using TreeProvider = std::function<std::optional<TreeCell>(std::size_t row)>;
using TreeToggleHandler = std::function<void(std::size_t row)>;
using HeaderClickHandler = std::function<void(std::size_t column)>;
using ProgressProvider =
    std::function<std::optional<unsigned>(std::size_t row, std::size_t column)>;

// 创建后由本模块统一负责深浅主题、选择态、滚动条、表头和树形连线绘制，页面不要
// 再向控件发送原生 ListView 专用消息，以免 Win10/Win11 出现不同的系统主题样式。
HWND Create(HINSTANCE instance,
            HWND parent,
            int control_id,
            bool single_selection = false,
            DWORD extra_style = 0);

void SetColumns(HWND table, std::span<const Column> columns);
void SetFillLastColumn(HWND table, bool enabled);
void SetColumnWidths(HWND table, std::span<const int> widths);
void SetColumnWidth(HWND table, std::size_t column, int width);
void SetColumnFont(HWND table, std::size_t column, HFONT font);
[[nodiscard]] int ColumnWidth(HWND table, std::size_t column);
[[nodiscard]] std::size_t ColumnCount(HWND table);

void SetTextProvider(HWND table, TextProvider provider);
void SetImageProvider(HWND table, ImageProvider provider);
void SetIndentProvider(HWND table, IndentProvider provider);
void SetTreeProvider(HWND table, TreeProvider provider, TreeToggleHandler toggle_handler = {});
void SetHeaderClickHandler(HWND table, HeaderClickHandler handler);
void SetSortIndicator(HWND table, std::optional<std::size_t> column, bool ascending = true);
void SetMarqueeSelection(HWND table, bool enabled);
void SetProgressProvider(HWND table, ProgressProvider provider);
void SetImageList(HWND table, HIMAGELIST images);
void SetRowCount(HWND table, std::size_t rows, bool preserve_scroll = true);
[[nodiscard]] std::size_t RowCount(HWND table);

[[nodiscard]] std::vector<int> SelectedRows(HWND table);
[[nodiscard]] int SelectedRow(HWND table);
[[nodiscard]] bool IsRowSelected(HWND table, int row);
void SelectRow(HWND table, int row, bool clear_existing = true);
void EnsureCellVisible(HWND table, std::size_t row, std::size_t column);
void ClearSelection(HWND table);
void SetDropTargetRow(HWND table, int row);
[[nodiscard]] int HitTest(HWND table, POINT client_point);
[[nodiscard]] std::optional<RECT> CellTextRect(HWND table, std::size_t row, std::size_t column);

void Refresh(HWND table, bool erase = false);
// 仅使当前可见的指定行失效；连续行合并为一个绘制区域。
void RefreshRows(HWND table, std::span<const std::size_t> rows);
void ApplyTheme(HWND table);

} // namespace SLink::ui::table
