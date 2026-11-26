// 远程编辑页面实现：文件卡片列表、文档标签、Scintilla 宿主、状态栏和
// 分隔条均只维护 UI 状态，远程 I/O 通过 EditorController 回调执行。
#include "ui/editor_page.hpp"
#include "editor/gb2312_codec.hpp"
#include "find_bar.hpp"

#include "ui/file_icon_provider.hpp"
#include "ui/local_shortcut.hpp"
#include "ui/retained_ui.hpp"
#include "ui/selector_control.hpp"
#include "ui/table_control.hpp"
#include "ui/terminal_tab_viewport.hpp"
#include "ui/win32_ui_internal.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <commctrl.h>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <limits>
#include <numeric>
#include <string_view>
#include <utility>
#include <windowsx.h>

namespace SLink::ui {
namespace {

#ifndef WM_DPICHANGED_AFTERPARENT
#define WM_DPICHANGED_AFTERPARENT 0x02E3
#endif

constexpr wchar_t kEditorPageClass[] = L"SLink.EditorPage";
constexpr wchar_t kChangesViewClass[] = L"SLink.EditorChanges";

struct ChangesViewContext {
    HWND editor{};
    HWND horizontal{}, vertical{};
    HMODULE module{};
    bool window_owned{};
    std::string report;
    struct ScrollMetrics {
        int maximum{}, position{}, length{}, thumb_start{}, thumb_length{};
    };
    int drag_offset{};
    HWND dragging{};
    std::size_t maximum_columns{};
    LRESULT measured_cell_width{};
    LRESULT measured_tab_width{};
    UINT measured_dpi{};
    bool updating_scrollbars{};
    void RefreshScrollWidth() {
        if (!editor || !horizontal)
            return;
        const auto cell = std::max<LRESULT>(
            1, SendMessageA(editor, 2276 /* TEXTWIDTH */, 32, reinterpret_cast<LPARAM>("M")));
        const auto dpi = detail::WindowDpi(editor);
        const auto tab = SendMessageW(editor, 2121 /* GETTABWIDTH */, 0, 0);
        const bool font_changed =
            cell != measured_cell_width || dpi != measured_dpi || tab != measured_tab_width;
        const auto limit = static_cast<LRESULT>((std::numeric_limits<int>::max)());
        auto width = font_changed ? std::min<LRESULT>(limit,
                                                      static_cast<LRESULT>(maximum_columns) * cell +
                                                          detail::ScaleForDpi(24, dpi))
                                  : SendMessageW(editor, 2275 /* GETSCROLLWIDTH */, 0, 0);
        // 仅测量已可见行的真实排版位置，修正字体回退等宽度差异；不在 UI 线程遍历
        // 十万行重新排版。预估宽度覆盖未显示行，滚动到新行时再扩大真实范围。
        const auto first = SendMessageW(editor, 2152 /* GETFIRSTVISIBLELINE */, 0, 0);
        const auto last = std::min<LRESULT>(
            SendMessageW(editor, 2154 /* GETLINECOUNT */, 0, 0),
            first + std::max<LRESULT>(1, SendMessageW(editor, 2370 /* LINESONSCREEN */, 0, 0)) + 1);
        const auto offset = SendMessageW(editor, 2398 /* GETXOFFSET */, 0, 0);
        for (auto line = first; line < last; ++line) {
            const auto end = SendMessageW(editor, 2136 /* GETLINEENDPOSITION */, line, 0);
            width = std::max(
                width,
                std::min<LRESULT>(limit,
                                  SendMessageW(editor, 2164 /* POINTXFROMPOSITION */, 0, end) +
                                      offset + detail::ScaleForDpi(12, dpi)));
        }
        measured_cell_width = cell;
        measured_tab_width = tab;
        measured_dpi = dpi;
        if (width != SendMessageW(editor, 2275, 0, 0))
            SendMessageW(editor, 2274 /* SETSCROLLWIDTH */, width, 0);
    }
    ScrollMetrics Metrics(HWND bar) const {
        const bool across = bar == horizontal;
        RECT rc{};
        GetClientRect(bar, &rc);
        RECT body{};
        GetClientRect(editor, &body);
        ScrollMetrics m;
        m.length = std::max(1L, across ? rc.right : rc.bottom);
        const int page =
            across ? std::max(1L, body.right - detail::ScaleForDpi(24, detail::WindowDpi(editor)))
                   : std::max(
                         1, static_cast<int>(SendMessageW(editor, 2370 /* LINESONSCREEN */, 0, 0)));
        const int total = std::max(
            page,
            static_cast<int>(SendMessageW(
                editor, across ? 2275 /* GETSCROLLWIDTH */ : 2154 /* GETLINECOUNT */, 0, 0)));
        m.maximum = total - page;
        m.position = std::clamp(
            static_cast<int>(SendMessageW(
                editor, across ? 2398 /* GETXOFFSET */ : 2152 /* GETFIRSTVISIBLELINE */, 0, 0)),
            0,
            m.maximum);
        m.thumb_length =
            std::clamp(MulDiv(m.length, page, total),
                       std::min(m.length, detail::ScaleForDpi(20, detail::WindowDpi(editor))),
                       m.length);
        m.thumb_start =
            m.maximum > 0 ? MulDiv(m.length - m.thumb_length, m.position, m.maximum) : 0;
        return m;
    }
    void UpdateScrollbars() {
        if (updating_scrollbars)
            return;
        updating_scrollbars = true;
        RefreshScrollWidth();
        for (auto bar : {horizontal, vertical}) {
            if (!bar)
                continue;
            ShowWindow(bar, Metrics(bar).maximum > 0 ? SW_SHOWNOACTIVATE : SW_HIDE);
            InvalidateRect(bar, nullptr, FALSE);
        }
        updating_scrollbars = false;
    }
    void ScrollTo(HWND bar, int coordinate) {
        const auto m = Metrics(bar);
        const auto value =
            MulDiv(std::clamp(coordinate - drag_offset, 0, std::max(0, m.length - m.thumb_length)),
                   m.maximum,
                   std::max(1, m.length - m.thumb_length));
        SendMessageW(editor,
                     bar == horizontal ? 2397 /* SETXOFFSET */ : 2613 /* SETFIRSTVISIBLELINE */,
                     value,
                     0);
        UpdateScrollbars();
    }
    void Layout(HWND view) {
        RECT rc{};
        GetClientRect(view, &rc);
        const int size = detail::ScaleForDpi(8, detail::WindowDpi(view));
        const int width = std::max(0L, rc.right - size), height = std::max(0L, rc.bottom - size);
        MoveWindow(editor, 0, 0, width, height, TRUE);
        MoveWindow(horizontal, 0, height, width, size, TRUE);
        MoveWindow(vertical, width, 0, size, height, TRUE);
        UpdateScrollbars();
    }
    static LRESULT CALLBACK
    ScrollProc(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR value) {
        auto* self = reinterpret_cast<ChangesViewContext*>(value);
        const bool across = window == self->horizontal;
        if (message == WM_PAINT || message == WM_PRINTCLIENT) {
            PAINTSTRUCT paint{};
            const auto dc =
                message == WM_PAINT ? BeginPaint(window, &paint) : reinterpret_cast<HDC>(wp);
            RECT rc{};
            GetClientRect(window, &rc);
            const auto background = CreateSolidBrush(theme::TerminalBackgroundColor());
            FillRect(dc, &rc, background);
            DeleteObject(background);
            const auto m = self->Metrics(window);
            RECT thumb =
                across
                    ? RECT{m.thumb_start + 1, 1, m.thumb_start + m.thumb_length - 1, rc.bottom - 1}
                    : RECT{1, m.thumb_start + 1, rc.right - 1, m.thumb_start + m.thumb_length - 1};
            const auto fill =
                CreateSolidBrush(theme::IsDark() ? RGB(80, 83, 89) : RGB(164, 169, 178));
            const auto previous_brush = SelectObject(dc, fill);
            const auto previous_pen = SelectObject(dc, GetStockObject(NULL_PEN));
            RoundRect(dc, thumb.left, thumb.top, thumb.right, thumb.bottom, 6, 6);
            SelectObject(dc, previous_pen);
            SelectObject(dc, previous_brush);
            DeleteObject(fill);
            if (message == WM_PAINT)
                EndPaint(window, &paint);
            return 0;
        }
        if (message == WM_ERASEBKGND)
            return 1;
        if (message == WM_LBUTTONDOWN) {
            const auto m = self->Metrics(window);
            const int coordinate = across ? GET_X_LPARAM(lp) : GET_Y_LPARAM(lp);
            self->drag_offset =
                coordinate >= m.thumb_start && coordinate < m.thumb_start + m.thumb_length
                    ? coordinate - m.thumb_start
                    : m.thumb_length / 2;
            self->dragging = window;
            SetCapture(window);
            self->ScrollTo(window, coordinate);
            return 0;
        }
        if (message == WM_MOUSEMOVE && GetCapture() == window && self->dragging == window) {
            self->ScrollTo(window, across ? GET_X_LPARAM(lp) : GET_Y_LPARAM(lp));
            return 0;
        }
        if (message == WM_LBUTTONUP && GetCapture() == window) {
            ReleaseCapture();
            return 0;
        }
        if (message == WM_CAPTURECHANGED)
            self->dragging = nullptr;
        if (message == WM_MOUSEWHEEL) {
            SendMessageW(self->editor, message, wp, lp);
            return 0;
        }
        if (message == WM_NCDESTROY)
            RemoveWindowSubclass(window, ScrollProc, id);
        return DefSubclassProc(window, message, wp, lp);
    }
    static LRESULT CALLBACK
    EditorProc(HWND editor, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR value) {
        auto* self = reinterpret_cast<ChangesViewContext*>(value);
        if (message == WM_MOUSEHWHEEL) {
            const auto m = self->Metrics(self->horizontal);
            SendMessageW(
                editor,
                2397,
                std::clamp(m.position + MulDiv(GET_WHEEL_DELTA_WPARAM(wp),
                                               detail::ScaleForDpi(48, detail::WindowDpi(editor)),
                                               WHEEL_DELTA),
                           0,
                           m.maximum),
                0);
            self->UpdateScrollbars();
            return 0;
        }
        if (message == WM_NCDESTROY)
            RemoveWindowSubclass(editor, EditorProc, id);
        return DefSubclassProc(editor, message, wp, lp);
    }
    ~ChangesViewContext() {
        if (module)
            FreeLibrary(module);
    }
};

LRESULT CALLBACK ChangesViewProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<ChangesViewContext*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        state = static_cast<ChangesViewContext*>(
            reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (!state)
        return DefWindowProcW(window, message, wparam, lparam);
    if (message == WM_CREATE) {
        state->editor = CreateWindowExW(0,
                                        L"Scintilla",
                                        L"",
                                        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                                        0,
                                        0,
                                        0,
                                        0,
                                        window,
                                        reinterpret_cast<HMENU>(1),
                                        GetModuleHandleW(nullptr),
                                        nullptr);
        if (!state->editor)
            return -1;
        SetWindowSubclass(
            state->editor, ChangesViewContext::EditorProc, 1, reinterpret_cast<DWORD_PTR>(state));
        const auto control = state->editor;
        SendMessageW(control, 2037 /* SETCODEPAGE */, 65001, 0);
        SendMessageA(control, 2056 /* STYLESETFONT */, 32, reinterpret_cast<LPARAM>("Consolas"));
        SendMessageW(control, 2055 /* STYLESETSIZE */, 32, 11);
        SendMessageW(control, 2051 /* STYLESETFORE */, 32, theme::kTextColor);
        SendMessageW(control, 2052 /* STYLESETBACK */, 32, theme::TerminalBackgroundColor());
        SendMessageW(control, 2050 /* STYLECLEARALL */, 0, 0);
        SendMessageW(control, 2051, 1, theme::IsDark() ? RGB(239, 142, 142) : RGB(166, 40, 40));
        SendMessageW(control, 2051, 2, theme::IsDark() ? RGB(147, 207, 155) : RGB(31, 115, 48));
        for (int margin = 0; margin < 5; ++margin)
            SendMessageW(control, 2242 /* SETMARGINWIDTHN */, margin, 0);
        SendMessageW(control,
                     2155 /* SETMARGINLEFT */,
                     0,
                     detail::ScaleForDpi(12, detail::WindowDpi(window)));
        SendMessageA(
            control, 2181 /* SETTEXT */, 0, reinterpret_cast<LPARAM>(state->report.c_str()));
        std::size_t start = 0;
        const auto tab_width =
            std::max<LRESULT>(1, SendMessageW(control, 2121 /* GETTABWIDTH */, 0, 0));
        while (start < state->report.size()) {
            const auto newline = state->report.find('\n', start);
            const auto end = newline == std::string::npos ? state->report.size() : newline + 1;
            const int style = state->report[start] == '-' ? 1 : state->report[start] == '+' ? 2 : 0;
            SendMessageW(control, 2032 /* STARTSTYLING */, start, 0);
            SendMessageW(control, 2033 /* SETSTYLING */, end - start, style);
            std::size_t columns = 0;
            for (auto at = start; at < end; ++at) {
                const auto byte = static_cast<unsigned char>(state->report[at]);
                if (byte == '\t')
                    columns += static_cast<std::size_t>(tab_width) -
                               columns % static_cast<std::size_t>(tab_width);
                else if (byte != '\r' && byte != '\n' && (byte & 0xc0U) != 0x80U)
                    columns += byte < 0x80U ? 1 : 2;
            }
            state->maximum_columns = std::max(state->maximum_columns, columns);
            start = end;
        }
        SendMessageW(control, 2075 /* EMPTYUNDOBUFFER */, 0, 0);
        SendMessageW(control, 2171 /* SETREADONLY */, TRUE, 0);
        // 改动快照与主编辑区一样禁用系统滚动条，避免底部/右侧留下白色非客户区。
        SendMessageW(control, 2130 /* SETHSCROLLBAR */, FALSE, 0);
        SendMessageW(control, 2280 /* SETVSCROLLBAR */, FALSE, 0);
        // 隐藏原生横条后控件不会总是自动跟踪宽度，预估最宽行以保留横向浏览能力。
        const auto cell_width = std::max<LRESULT>(
            1, SendMessageA(control, 2276 /* TEXTWIDTH */, 32, reinterpret_cast<LPARAM>("M")));
        SendMessageW(
            control,
            2274 /* SETSCROLLWIDTH */,
            std::max<std::size_t>(1,
                                  state->maximum_columns * static_cast<std::size_t>(cell_width) +
                                      detail::ScaleForDpi(24, detail::WindowDpi(window))),
            0);
        SendMessageW(control, 2516 /* SETSCROLLWIDTHTRACKING */, TRUE, 0);
        for (int id : {2, 3}) {
            const auto bar = CreateWindowExW(0,
                                             L"STATIC",
                                             L"",
                                             WS_CHILD | WS_CLIPSIBLINGS,
                                             0,
                                             0,
                                             0,
                                             0,
                                             window,
                                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                             GetModuleHandleW(nullptr),
                                             nullptr);
            if (!bar)
                return -1;
            (id == 2 ? state->horizontal : state->vertical) = bar;
            SetWindowSubclass(
                bar, ChangesViewContext::ScrollProc, id, reinterpret_cast<DWORD_PTR>(state));
        }
        std::string{}.swap(state->report);
        detail::ApplyPopupWindowTheme(window);
        return 0;
    }
    if (message == WM_SIZE) {
        state->Layout(window);
        return 0;
    }
    if (message == WM_DPICHANGED) {
        const auto* bounds = reinterpret_cast<RECT*>(lparam);
        SetWindowPos(window,
                     nullptr,
                     bounds->left,
                     bounds->top,
                     bounds->right - bounds->left,
                     bounds->bottom - bounds->top,
                     SWP_NOACTIVATE | SWP_NOZORDER);
        state->measured_dpi = 0;
        state->Layout(window);
        return 0;
    }
    if (message == WM_NOTIFY && reinterpret_cast<NMHDR*>(lparam)->hwndFrom == state->editor) {
        state->UpdateScrollbars();
        return 0;
    }
    if (message == WM_MOUSEHWHEEL) {
        SendMessageW(
            state->editor,
            2397,
            std::max<LRESULT>(0,
                              SendMessageW(state->editor, 2398, 0, 0) +
                                  MulDiv(GET_WHEEL_DELTA_WPARAM(wparam),
                                         detail::ScaleForDpi(48, detail::WindowDpi(window)),
                                         WHEEL_DELTA)),
            0);
        state->UpdateScrollbars();
        return 0;
    }
    if (message == WM_ERASEBKGND)
        return 1;
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        const auto dc = BeginPaint(window, &paint);
        const auto fill = CreateSolidBrush(theme::TerminalBackgroundColor());
        FillRect(dc, &paint.rcPaint, fill);
        DeleteObject(fill);
        EndPaint(window, &paint);
        return 0;
    }
    if (message == WM_SETFOCUS) {
        SetFocus(state->editor);
        return 0;
    }
    if (message == WM_THEMECHANGED) {
        for (int style : {0, 1, 2, 32})
            SendMessageW(
                state->editor, 2052 /* STYLESETBACK */, style, theme::TerminalBackgroundColor());
        for (int style : {0, 32})
            SendMessageW(state->editor, 2051 /* STYLESETFORE */, style, theme::kTextColor);
        SendMessageW(
            state->editor, 2051, 1, theme::IsDark() ? RGB(239, 142, 142) : RGB(166, 40, 40));
        SendMessageW(
            state->editor, 2051, 2, theme::IsDark() ? RGB(147, 207, 155) : RGB(31, 115, 48));
        state->UpdateScrollbars();
        RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_FRAME);
        return 0;
    }
    if (message == WM_NCDESTROY) {
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        if (state->window_owned)
            delete state;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
constexpr wchar_t kEditorPathFrameClass[] = L"SLink.EditorPathFrame";
constexpr wchar_t kEditorResizeGuideClass[] = L"SLink.EditorResizeGuide";
constexpr COLORREF kResizeGuideTransparentColor = RGB(1, 2, 3);

constexpr int kFileListId = 4102;
constexpr int kSaveId = 4105;
constexpr int kDiffId = 4106;
constexpr int kOverwriteId = 4111;
constexpr int kSaveAsId = 4112;
constexpr int kDismissConflictId = 4114;
constexpr int kPathEditId = 4119;
constexpr int kPathRefreshId = 4120;
constexpr int kEncodingSelectorId = 4121;
constexpr int kTabPreviousId = 4122;
constexpr int kTabNextId = 4123;
constexpr int kStatusDetailsId = 4124;
constexpr int kChangesId = 4125;
constexpr int kDocumentTabFirstId = 4500;
constexpr int kDocumentCloseFirstId = 5500;
constexpr UINT kUpdateHorizontalScrollbarMessage = WM_APP + 0x231;
constexpr UINT_PTR kTabMotionTimerId = 1;
constexpr ULONGLONG kTabMotionDurationMs = 120;

bool ClientAnimationsEnabled() noexcept {
    BOOL enabled = TRUE;
    return SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0) == FALSE ||
           enabled != FALSE;
}

constexpr UINT kSciSetCodePage = 2037;
constexpr UINT kSciSetWrapMode = 2268;
constexpr UINT kSciWrapCount = 2235;
constexpr UINT kSciSetReadOnly = 2171;
constexpr UINT kSciSetText = 2181;
constexpr UINT kSciGetText = 2182;
constexpr UINT kSciGetTextLength = 2183;
constexpr UINT kSciStyleClearAll = 2050;
constexpr UINT kSciStyleSetFore = 2051;
constexpr UINT kSciStyleSetBack = 2052;
constexpr UINT kSciStyleSetBold = 2053;
constexpr UINT kSciStyleSetFont = 2056;
constexpr UINT kSciStyleSetSize = 2055;
constexpr UINT kSciSetCaretFore = 2069;
constexpr UINT kSciSetElementColour = 2753;
constexpr UINT kSciSetSelectionLayer = 2763;
constexpr UINT kSciSetCaretLineVisible = 2096;
constexpr UINT kSciSetCaretLineBack = 2098;
constexpr UINT kSciSetCaretLineLayer = 2765;
constexpr UINT kSciSetMarginLeft = 2155;
constexpr UINT kSciSetMarginRight = 2157;
constexpr UINT kSciSetExtraAscent = 2526;
constexpr UINT kSciSetExtraDescent = 2528;
constexpr UINT kSciSetMarginWidthN = 2242;
constexpr UINT kSciGetMarginWidthN = 2243;
constexpr UINT kSciGetMargins = 2253;
constexpr UINT kSciGetMarginLeft = 2156;
constexpr UINT kSciGetMarginRight = 2158;
constexpr UINT kSciGetLineCount = 2154;
constexpr UINT kSciTextWidth = 2276;
constexpr UINT kSciSetHScrollBar = 2130;
constexpr UINT kSciSetVScrollBar = 2280;
constexpr UINT kSciSetScrollWidth = 2274;
constexpr UINT kSciGetScrollWidth = 2275;
constexpr UINT kSciSetScrollWidthTracking = 2516;
constexpr UINT kSciGetFirstVisibleLine = 2152;
constexpr UINT kSciSetFirstVisibleLine = 2613;
constexpr UINT kSciVisibleFromDocLine = 2220;
constexpr UINT kSciEnsureVisible = 2232;
constexpr UINT kSciPointYFromPosition = 2165;
constexpr UINT kSciPointXFromPosition = 2164;
constexpr UINT kSciPositionFromPointClose = 2023;
constexpr UINT kSciTextHeight = 2279;
constexpr UINT kSciScrollRange = 2569;
constexpr UINT kSciLinesOnScreen = 2370;
constexpr UINT kSciLineScroll = 2168;
constexpr UINT kSciGetXOffset = 2398;
constexpr UINT kSciSetXOffset = 2397;
constexpr UINT kSciSetEolMode = 2031;
constexpr UINT kSciSetSavePoint = 2014;
constexpr UINT kSciSetChangeHistory = 2780;
constexpr UINT kSciMarkerDefine = 2040;
constexpr UINT kSciMarkerSetBack = 2042;
constexpr UINT kSciMarkerAdd = 2043;
constexpr UINT kSciMarkerDeleteAll = 2045;
constexpr UINT kSciMarkerNext = 2047;
// 25–31 留给 Scintilla 折叠标记，不能放恢复后的改动锚点，否则整栏会使用折叠底色。
constexpr int kRestoredChangeMarker = 20;
constexpr unsigned kChangeMarkerMask = (1U << 23U) | (1U << 24U) | (1U << kRestoredChangeMarker);
constexpr UINT kSciGetCurrentPos = 2008;
constexpr UINT kSciLineFromPosition = 2166;
constexpr UINT kSciGetColumn = 2129;
constexpr UINT kSciLineLength = 2350;
constexpr UINT kSciGetLine = 2153;
constexpr UINT kSciSetILexer = 4033;
constexpr UINT kSciColourise = 4003;
constexpr UINT kSciGetSelectionStart = 2143;
constexpr UINT kSciGetSelectionEnd = 2145;
constexpr UINT kSciSetSel = 2160;
constexpr UINT kSciSetTargetStart = 2190;
constexpr UINT kSciGetTargetStart = 2191;
constexpr UINT kSciSetTargetEnd = 2192;
constexpr UINT kSciGetTargetEnd = 2193;
constexpr UINT kSciReplaceTarget = 2194;
constexpr UINT kSciReplaceTargetRe = 2195;
constexpr UINT kSciSearchInTarget = 2197;
constexpr UINT kSciSetSearchFlags = 2198;
constexpr UINT kSciBeginUndoAction = 2078;
constexpr UINT kSciEndUndoAction = 2079;
constexpr UINT kSciPositionAfter = 2418;
constexpr UINT kSciPositionBefore = 2417;
constexpr UINT kSciSetStatus = 2382;
constexpr UINT kSciGetStatus = 2383;

constexpr UINT kScCpUtf8 = 65001;
constexpr UINT_PTR kStyleDefault = 32;
constexpr UINT_PTR kStyleLineNumber = 33;
constexpr int kScEolCrLf = 0;
constexpr int kScEolCr = 1;
constexpr int kScEolLf = 2;
constexpr int kScnSavePointReached = 2002;
constexpr int kScnSavePointLeft = 2003;
constexpr int kScnUpdateUi = 2007;
constexpr int kScnModified = 2008;
constexpr int kScModInsertText = 0x1;
constexpr int kScModDeleteText = 0x2;

constexpr std::size_t kHexPreviewLimit = 256U * 1024U;

ATOM RegisterPageClass(HINSTANCE instance, WNDPROC proc) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kEditorPageClass;
    const ATOM atom = RegisterClassExW(&wc);
    return atom ? atom : static_cast<ATOM>(GetLastError() == ERROR_CLASS_ALREADY_EXISTS);
}

std::wstring Utf8ToWide(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int source_size = static_cast<int>(
        (std::min<std::size_t>)(text.size(),
                                static_cast<std::size_t>((std::numeric_limits<int>::max)())));
    int count =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), source_size, nullptr, 0);
    UINT code_page = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    if (count <= 0) {
        code_page = CP_ACP;
        flags = 0;
        count = MultiByteToWideChar(code_page, flags, text.data(), source_size, nullptr, 0);
    }
    if (count <= 0) {
        return L"无法显示此文本编码。";
    }
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(code_page, flags, text.data(), source_size, result.data(), count);
    return result;
}

std::string WideToUtf8(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int source_size = static_cast<int>(
        (std::min<std::size_t>)(text.size(),
                                static_cast<std::size_t>((std::numeric_limits<int>::max)())));
    const int count =
        WideCharToMultiByte(CP_UTF8, 0, text.data(), source_size, nullptr, 0, nullptr, nullptr);
    if (count <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(count), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, text.data(), source_size, result.data(), count, nullptr, nullptr);
    return result;
}

std::wstring WindowText(HWND window) {
    const int length = GetWindowTextLengthW(window);
    if (length <= 0) {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(length + 1), L'\0');
    const int copied = GetWindowTextW(window, result.data(), length + 1);
    result.resize(static_cast<std::size_t>((std::max)(0, copied)));
    return result;
}

std::wstring EncodingLabel(EditorTextEncoding encoding) {
    switch (encoding) {
    case EditorTextEncoding::Utf8:
        return L"UTF-8";
    case EditorTextEncoding::Utf8Bom:
        return L"UTF-8 BOM";
    case EditorTextEncoding::Legacy8Bit:
        return L"8 位原始编码";
    case EditorTextEncoding::Gb2312:
        return L"GB2312";
    }
    return L"未知编码";
}

std::wstring EolLabel(EditorEolStyle eol) {
    switch (eol) {
    case EditorEolStyle::None:
        return L"无换行";
    case EditorEolStyle::Lf:
        return L"LF";
    case EditorEolStyle::CrLf:
        return L"CRLF";
    case EditorEolStyle::Cr:
        return L"CR";
    case EditorEolStyle::Mixed:
        return L"混合换行";
    }
    return L"未知换行";
}

int ScintillaEol(EditorEolStyle eol) {
    switch (eol) {
    case EditorEolStyle::Cr:
        return kScEolCr;
    case EditorEolStyle::CrLf:
        return kScEolCrLf;
    case EditorEolStyle::None:
    case EditorEolStyle::Lf:
    case EditorEolStyle::Mixed:
        return kScEolLf;
    }
    return kScEolLf;
}

std::wstring BuildHexPreview(std::span<const std::byte> bytes, bool& truncated) {
    constexpr wchar_t digits[] = L"0123456789ABCDEF";
    const std::size_t count = (std::min)(bytes.size(), kHexPreviewLimit);
    truncated = bytes.size() > count;
    std::wstring result;
    result.reserve((count / 16U + 1U) * 78U);
    for (std::size_t offset = 0; offset < count; offset += 16U) {
        for (int shift = 28; shift >= 0; shift -= 4) {
            result.push_back(digits[(static_cast<std::uint32_t>(offset) >> shift) & 0x0fU]);
        }
        result.append(L"  ");
        for (std::size_t column = 0; column < 16U; ++column) {
            if (offset + column < count) {
                const auto value = std::to_integer<unsigned char>(bytes[offset + column]);
                result.push_back(digits[value >> 4U]);
                result.push_back(digits[value & 0x0fU]);
            } else {
                result.append(L"  ");
            }
            result.push_back(column == 7U ? L'-' : L' ');
        }
        result.push_back(L' ');
        for (std::size_t column = 0; column < 16U && offset + column < count; ++column) {
            const auto value = std::to_integer<unsigned char>(bytes[offset + column]);
            result.push_back(value >= 0x20U && value <= 0x7eU ? static_cast<wchar_t>(value) : L'.');
        }
        result.append(L"\r\n");
    }
    if (truncated) {
        result.append(L"\r\n……预览已截断，仅显示前 256 KB。");
    }
    return result;
}

const char* LexerNameFor(std::wstring_view title) {
    const auto dot = title.find_last_of(L'.');
    if (dot == std::wstring_view::npos) {
        return nullptr;
    }
    std::wstring extension(title.substr(dot + 1));
    std::transform(extension.begin(), extension.end(), extension.begin(), [](wchar_t value) {
        return static_cast<wchar_t>(towlower(value));
    });
    if (extension == L"c" || extension == L"h" || extension == L"cc" || extension == L"cpp" ||
        extension == L"cxx" || extension == L"hpp") {
        return "cpp";
    }
    if (extension == L"py")
        return "python";
    if (extension == L"sh" || extension == L"bash")
        return "bash";
    if (extension == L"js" || extension == L"mjs" || extension == L"ts") {
        return "cpp";
    }
    if (extension == L"json")
        return "json";
    if (extension == L"xml" || extension == L"html" || extension == L"htm") {
        return "hypertext";
    }
    if (extension == L"sql")
        return "sql";
    if (extension == L"ini" || extension == L"conf" || extension == L"properties") {
        return "props";
    }
    if (extension == L"lua")
        return "lua";
    if (extension == L"rs")
        return "rust";
    if (extension == L"yaml" || extension == L"yml")
        return "yaml";
    return nullptr;
}

template <typename Callback, typename... Args>
void InvokeSafely(const Callback& callback, Args&&... args) noexcept {
    if (!callback) {
        return;
    }
    try {
        callback(std::forward<Args>(args)...);
    } catch (...) {
        // Never unwind through a Win32 window procedure.
    }
}

LRESULT CALLBACK PathFrameWindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_COMMAND:
        if (HIWORD(wparam) == EN_SETFOCUS || HIWORD(wparam) == EN_KILLFOCUS)
            InvalidateRect(window, nullptr, FALSE);
        [[fallthrough]];
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
            CreatePen(PS_SOLID,
                      std::max(1, detail::ScaleForDpi(1, dpi)),
                      GetFocus() == GetWindow(window, GW_CHILD) ? detail::kAccentColor
                                                                : detail::kPanelAltColor);
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
    wc.lpszClassName = kEditorPathFrameClass;
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
    wc.lpszClassName = kEditorResizeGuideClass;
    const ATOM atom = RegisterClassExW(&wc);
    return atom != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

} // namespace

// 编辑页所有 HWND、文档标签和滚动条镜像状态均属于 UI 线程。Scintilla 负责文本编辑
// 内核，页面自绘标签和滚动条；DocumentState 只是显示快照，网络文档仍由
// EditorController 独占，避免控件回调直接访问远端。
struct EditorPage::Impl {
    enum class ResizeMode {
        none,
        file_sidebar,
        conflict,
    };

    enum class EditorScrollDrag {
        none,
        horizontal,
        vertical,
    };

    struct ScintillaNotificationPrefix {
        NMHDR header;
        std::intptr_t position;
        int character;
        int modifiers;
        int modification_type;
        const char* text;
        std::intptr_t length;
        std::intptr_t lines_added;
    };

    struct DocumentState {
        EditorDocumentId id{};
        std::wstring title;
        std::string server_id;
        std::wstring server_name;
        std::wstring remote_path;
        std::string content;
        std::wstring preview;
        EditorTextEncoding encoding{EditorTextEncoding::Utf8};
        EditorEolStyle eol{EditorEolStyle::None};
        EditorDocumentKind kind{EditorDocumentKind::Text};
        bool read_only{};
        bool dirty{};
        bool trusted_savepoint{};
        EditorTextEncoding saved_encoding{EditorTextEncoding::Utf8};
        std::vector<int> changed_lines;
        std::size_t touched_mark_count{};
        bool saving{};
        bool closing{};
        bool preview_truncated{};
        std::wstring conflict;
    };

    struct EditorScrollMetrics {
        // 镜像 Scintilla 内部滚动范围，用项目统一样式绘制滚动条；同步期间通过标记
        // 防止自绘滚动消息和 Scintilla 通知互相递归。
        RECT horizontal_track{};
        RECT horizontal_thumb{};
        RECT vertical_track{};
        RECT vertical_thumb{};
        int horizontal_max{};
        int vertical_max{};
        int horizontal_position{};
        int vertical_position{};
        int horizontal_page{};
        int vertical_page{};
        bool horizontal_visible{};
        bool vertical_visible{};
    };

    struct EditorScrollbarWindowState {
        RECT bounds{};
        RECT thumb{};
        bool visible{};
        bool initialized{};
    };

    HINSTANCE instance{};
    HWND hwnd{};
    detail::UiSurface surface;
    HWND path_frame{};
    HWND path_edit{};
    HWND encoding_selector{};
    HWND file_list{};
    HWND editor{};
    detail::FindBar find_bar;
    bool batch_replacing{};
    bool word_wrap{};
    struct WrapViewportAnchor {
        EditorDocumentId document{};
        LRESULT position{};
        int y{};
        UINT dpi{96};
    };
    std::optional<WrapViewportAnchor> pending_wrap_viewport;
    bool restoring_wrap_viewport{};
    std::optional<int> find_bar_top;
    bool suppress_search_escape{};
    WPARAM suppress_search_character{};
    std::optional<EditorDocumentId> last_search_document;
    std::wstring last_search_query;
    LRESULT last_search_position{-1};
    bool last_search_case{}, last_search_regex{};
    HWND editor_horizontal_scrollbar{};
    HWND editor_vertical_scrollbar{};
    HWND preview{};
    HWND status_bar{};
    HWND status_details{};
    HWND tooltip{};
    std::wstring tooltip_text;
    std::size_t first_tab{}, visible_tabs{}, previous_tab_count{};
    int previous_tab_room{};
    std::optional<EditorDocumentId> previous_tab_active;
    HWND resize_guide{};
    HMODULE scintilla_module{};
    HMODULE lexilla_module{};
    HFONT font{};
    HFONT title_font{};
    HFONT mono_font{};
    HFONT caption_font{};
    bool conflict_visible{};
    bool scintilla_available{};
    bool loading_document{};
    bool horizontal_scroll_update_pending{};
    mutable EditorScrollbarWindowState horizontal_scrollbar_state;
    mutable EditorScrollbarWindowState vertical_scrollbar_state;
    EditorScrollDrag editor_scroll_drag{EditorScrollDrag::none};
    int editor_scroll_drag_offset{};
    ResizeMode resize_mode{ResizeMode::none};
    POINT resize_origin{};
    int resize_start{};
    int left_width_override{};
    int conflict_height_override{};
    int resize_preview_value{};
    int resize_preview_position{-1};
    int left_splitter_x{};
    int conflict_splitter_y{};
    SIZE path_frame_region{};
    SIZE path_region{};
    int folder_icon{};
    int file_icon{};
    std::string selected_server_id;
    EditorDocumentId next_document_id{1};
    std::optional<EditorDocumentId> active_id;
    std::optional<EditorDocumentId> animated_tab_id;
    bool tab_motion_enabled{ClientAnimationsEnabled()};
    bool tab_motion_active{};
    ULONGLONG tab_motion_start{};
    float tab_motion_progress{1.0F};
    std::vector<DocumentState> documents;
    std::vector<EditorFileEntry> file_entries;
    ViewState directory_state;
    ViewActionCallback view_action;
    int file_column_width{-1};

    SaveRequestCallback save_requested;
    SaveRequestCallback encoding_changed;
    SaveRequestCallback overwrite_requested;
    SaveRequestCallback save_as_requested;
    DocumentActionCallback diff_requested;
    CloseRequestCallback close_requested;
    DocumentChangedCallback document_changed;
    ActiveDocumentChangedCallback active_document_changed;
    DocumentActionCallback changes_requested;
    FileActivationCallback file_activation;
    ServerChangedCallback server_changed;
    PathChangedCallback path_changed;
    HistoryNavigationCallback history_navigation;
    DWORD last_history_input_tick{};
    bool last_history_input_forward{};
    bool last_history_input_was_app_command{};
    std::vector<EditorServerOption> server_options;

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
            break;
        case WM_SIZE:
            self->surface.Resize(LOWORD(lparam), HIWORD(lparam));
            self->Layout(LOWORD(lparam), HIWORD(lparam));
            return 0;
        case WM_DPICHANGED_AFTERPARENT: {
            self->surface.DpiChanged();
            RECT rect{};
            GetClientRect(window, &rect);
            self->Layout(rect.right, rect.bottom);
            return 0;
        }
        case kUpdateHorizontalScrollbarMessage:
            self->horizontal_scroll_update_pending = false;
            // Scintilla updates its tracked scroll width while painting.
            // Force the pending text/layout paint before reading it.
            RedrawWindow(
                self->editor, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
            self->RestoreWrapViewport();
            self->UpdateHorizontalScrollbar();
            return 0;
        case WM_MOUSEMOVE:
            if (self->resize_mode != ResizeMode::none) {
                self->UpdateResize({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
                return 0;
            }
            self->surface.PointerMove({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
            return 0;
        case WM_MOUSELEAVE:
            self->surface.PointerLeave();
            return 0;
        case WM_LBUTTONDOWN:
            if (self->BeginResize({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)})) {
                return 0;
            }
            if (self->surface.PointerDown({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)})) {
                return 0;
            }
            break;
        case WM_LBUTTONUP:
            if (self->resize_mode != ResizeMode::none) {
                self->EndResize();
                return 0;
            }
            if (const auto clicked =
                    self->surface.PointerUp({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)})) {
                self->HandleSurfaceClick(*clicked);
                return 0;
            }
            break;
        case WM_CAPTURECHANGED:
            ShowWindow(self->resize_guide, SW_HIDE);
            self->resize_preview_value = 0;
            self->resize_preview_position = -1;
            self->resize_mode = ResizeMode::none;
            self->surface.CancelPointer();
            return 0;
        case WM_SETCURSOR:
            if (self->SetResizeCursor()) {
                return TRUE;
            }
            if (self->surface.hovered_id() != 0) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            break;
        case WM_COMMAND:
            if (reinterpret_cast<HWND>(lparam) == self->encoding_selector &&
                HIWORD(wparam) == CBN_SELCHANGE) {
                self->ChangeEncoding();
                return 0;
            }
            if (self->HandleCommand(LOWORD(wparam), HIWORD(wparam))) {
                return 0;
            }
            break;
        case WM_NOTIFY:
            if (self->tooltip && reinterpret_cast<NMHDR*>(lparam)->hwndFrom == self->tooltip &&
                reinterpret_cast<NMHDR*>(lparam)->code == TTN_GETDISPINFOW) {
                auto* info = reinterpret_cast<NMTTDISPINFOW*>(lparam);
                self->tooltip_text.clear();
                const auto* document = self->ActiveDocument();
                if (document) {
                    self->tooltip_text = document->server_name;
                    if (!document->remote_path.empty())
                        self->tooltip_text += L"\n" + document->remote_path;
                }
                POINT point{};
                GetCursorPos(&point);
                ScreenToClient(window, &point);
                if (info->hdr.idFrom == reinterpret_cast<UINT_PTR>(window)) {
                    self->tooltip_text.clear();
                    for (const auto& element : self->surface.elements()) {
                        if (element.role == detail::UiRole::file_tab &&
                            point.x >= element.bounds.left && point.x < element.bounds.right &&
                            point.y >= element.bounds.top && point.y < element.bounds.bottom) {
                            const auto index =
                                static_cast<std::size_t>(element.id - kDocumentTabFirstId);
                            if (index < self->documents.size()) {
                                const auto& item = self->documents[index];
                                self->tooltip_text =
                                    item.server_name + L"\n" +
                                    (item.remote_path.empty() ? item.title : item.remote_path);
                            }
                        }
                        if (element.id == kSaveId && point.x >= element.bounds.left &&
                            point.x < element.bounds.right && point.y >= element.bounds.top &&
                            point.y < element.bounds.bottom)
                            self->tooltip_text = L"保存 · Ctrl+S";
                    }
                }
                if (info->hdr.idFrom == reinterpret_cast<UINT_PTR>(self->path_edit))
                    self->tooltip_text = WindowText(self->path_edit);
                info->lpszText = self->tooltip_text.data();
                return 0;
            }
            if (const auto* header = reinterpret_cast<const NMHDR*>(lparam);
                header && self->HandleNotify(*header)) {
                return 0;
            }
            break;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX: {
            const HDC dc = reinterpret_cast<HDC>(wparam);
            if (reinterpret_cast<HWND>(lparam) == self->status_bar ||
                reinterpret_cast<HWND>(lparam) == self->status_details) {
                SetTextColor(dc, detail::kMutedTextColor);
                SetBkColor(dc, theme::TerminalBackgroundColor());
                return reinterpret_cast<LRESULT>(theme::IsDark() ? detail::WindowThemeBrush()
                                                                 : detail::PanelThemeBrush());
            }
            SetTextColor(dc, detail::kTextColor);
            SetBkColor(dc, detail::kPanelColor);
            return reinterpret_cast<LRESULT>(detail::PanelThemeBrush());
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            RECT rect{};
            GetClientRect(window, &rect);
            self->BuildSurface(rect.right, rect.bottom);
            self->surface.Paint(detail::UiColor(detail::kWindowColor));
            return 0;
        }
        case WM_PRINTCLIENT: {
            RECT bounds{};
            GetClientRect(window, &bounds);
            self->BuildSurface(bounds.right, bounds.bottom);
            self->surface.PaintTo(
                reinterpret_cast<HDC>(wparam), bounds, detail::UiColor(detail::kWindowColor));
            return 0;
        }
        default:
            break;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    static LRESULT CALLBACK EditorScrollbarSubclassProc(HWND window,
                                                        UINT message,
                                                        WPARAM wparam,
                                                        LPARAM lparam,
                                                        UINT_PTR subclass_id,
                                                        DWORD_PTR reference) {
        auto* self = reinterpret_cast<Impl*>(reference);
        const bool horizontal = subclass_id == 2;
        if (!self) {
            return DefSubclassProc(window, message, wparam, lparam);
        }
        switch (message) {
        case WM_PAINT:
            self->PaintEditorScrollbar(window, horizontal);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_LBUTTONDOWN: {
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            MapWindowPoints(window, self->editor, &point, 1);
            if (self->BeginEditorScroll(point)) {
                return 0;
            }
            break;
        }
        case WM_SETCURSOR:
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            return TRUE;
        case WM_NCDESTROY:
            RemoveWindowSubclass(window, &EditorScrollbarSubclassProc, subclass_id);
            return DefSubclassProc(window, message, wparam, lparam);
        default:
            break;
        }
        return DefSubclassProc(window, message, wparam, lparam);
    }

    static LRESULT CALLBACK ChildSubclassProc(HWND window,
                                              UINT message,
                                              WPARAM wparam,
                                              LPARAM lparam,
                                              UINT_PTR /*subclass_id*/,
                                              DWORD_PTR reference) {
        auto* self = reinterpret_cast<Impl*>(reference);
        if (self && message == WM_XBUTTONDOWN &&
            (window == self->file_list || window == self->path_edit)) {
            const WORD button = GET_XBUTTON_WPARAM(wparam);
            if (button == XBUTTON1 || button == XBUTTON2) {
                const bool forward = button == XBUTTON2;
                if (self->AcceptHistoryInput(forward, false)) {
                    InvokeSafely(self->history_navigation, forward);
                }
                return TRUE;
            }
        }
        if (self && message == WM_APPCOMMAND &&
            (window == self->file_list || window == self->path_edit)) {
            const int command = GET_APPCOMMAND_LPARAM(lparam);
            if (command == APPCOMMAND_BROWSER_BACKWARD || command == APPCOMMAND_BROWSER_FORWARD) {
                const bool forward = command == APPCOMMAND_BROWSER_FORWARD;
                if (self->AcceptHistoryInput(forward, true)) {
                    InvokeSafely(self->history_navigation, forward);
                }
                return TRUE;
            }
        }
        if (self && window == self->editor) {
            switch (message) {
            case WM_LBUTTONDOWN:
                if (self->BeginEditorScroll({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)})) {
                    return 0;
                }
                break;
            case WM_MOUSEMOVE:
                if (self->editor_scroll_drag != EditorScrollDrag::none) {
                    self->UpdateEditorScroll({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
                    return 0;
                }
                break;
            case WM_LBUTTONUP:
                if (self->editor_scroll_drag != EditorScrollDrag::none) {
                    self->EndEditorScroll();
                    return 0;
                }
                break;
            case WM_CAPTURECHANGED:
                self->editor_scroll_drag = EditorScrollDrag::none;
                break;
            case WM_MOUSEWHEEL:
                if ((GET_KEYSTATE_WPARAM(wparam) & MK_SHIFT) != 0) {
                    self->ScrollEditorHorizontally(-GET_WHEEL_DELTA_WPARAM(wparam));
                    return 0;
                }
                [[fallthrough]];
            case WM_SIZE: {
                const LRESULT result = DefSubclassProc(window, message, wparam, lparam);
                self->UpdateEditorScrollbarWindows();
                if (self->word_wrap)
                    self->ScheduleHorizontalScrollbarUpdate();
                return result;
            }
            case WM_MOUSEHWHEEL:
                self->ScrollEditorHorizontally(GET_WHEEL_DELTA_WPARAM(wparam));
                return 0;
            default:
                break;
            }
        }
        if (message == WM_KEYDOWN && self) {
            self->suppress_search_character = 0;
            const bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            if (control && wparam == 'F') {
                self->suppress_search_character = 6;
                self->ShowFindBar(false);
                return 0;
            }
            if (control && wparam == 'H') {
                // 替换只从查找条左侧箭头展开；不再执行旧快捷键。
                self->suppress_search_character = 8;
                return 0;
            }
            if (wparam == VK_F3) {
                if (self->find_bar.query().empty())
                    self->ShowFindBar(false);
                else
                    self->FindText(self->find_bar.query(),
                                   (GetKeyState(VK_SHIFT) & 0x8000) != 0,
                                   self->find_bar.match_case(),
                                   self->find_bar.regex());
                return 0;
            }
            if (wparam == VK_ESCAPE && self->find_bar.visible()) {
                self->suppress_search_escape = true;
                self->find_bar.Hide();
                return 0;
            }
            if (control) {
                switch (wparam) {
                case 'S':
                    self->RequestSave();
                    return 0;
                case 'W':
                    self->RequestCloseActive();
                    return 0;
                default:
                    break;
                }
            }
            if (wparam == VK_RETURN && window == self->path_edit) {
                self->NavigateFromPath();
                return 0;
            }
            if (wparam == VK_F4 && window == self->file_list) {
                self->ActivateSelectedFile();
                return 0;
            }
        }
        if (message == WM_CHAR && self && self->suppress_search_character == wparam &&
            std::exchange(self->suppress_search_character, 0) != 0)
            return 0;
        if (message == WM_CHAR && self && wparam == VK_ESCAPE &&
            std::exchange(self->suppress_search_escape, false))
            return 0;
        if (message == WM_CHAR && self && (GetKeyState(VK_CONTROL) & 0x8000) != 0 &&
            (wparam == 19 || wparam == 23 || wparam == 6 || wparam == 8)) {
            return 0;
        }
        return DefSubclassProc(window, message, wparam, lparam);
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

    void Subclass(HWND control) {
        if (control) {
            SetWindowSubclass(control, &ChildSubclassProc, 1, reinterpret_cast<DWORD_PTR>(this));
        }
    }

    void PlaceFindBar() {
        RECT rc{};
        GetClientRect(hwnd, &rc);
        const auto m = Measure(rc.right, rc.bottom);
        const int gap = detail::ScaleForDpi(6, detail::WindowDpi(hwnd));
        const int width = std::max(
            0,
            std::min(m.right_width - gap * 2, detail::ScaleForDpi(240, detail::WindowDpi(hwnd))));
        const int top = std::clamp(
            find_bar_top.value_or(gap), 0, std::max(0, m.editor_height - find_bar.height() - gap));
        find_bar.Place(m.right_x + m.right_width - gap - width, m.editor_y + top, width);
    }

    void ShowFindBar(bool replace) {
        if (!ActiveDocument() || !scintilla_available || !IsWindowVisible(editor))
            return;
        if (!find_bar.visible())
            find_bar_top.reset();
        find_bar.Show(replace, editor);
        PlaceFindBar();
    }

    std::optional<std::string> SearchBytes(std::wstring_view text) {
        auto bytes = WideToUtf8(text);
        const auto* document = ActiveDocument();
        if (document && document->encoding == EditorTextEncoding::Legacy8Bit) {
            auto encoded = editor::EncodeGb2312(bytes);
            if (!encoded) {
                find_bar.Status(L"当前文件编码无法表示这些字符", true);
                return std::nullopt;
            }
            return encoded;
        }
        return bytes;
    }

    LRESULT
    SearchRange(std::string_view query, LRESULT start, LRESULT end, bool match_case, bool regex) {
        SendMessageW(editor, kSciSetStatus, 0, 0);
        SendMessageW(editor,
                     kSciSetSearchFlags,
                     (match_case ? 4 : 0) | (regex ? 0x00200000 | 0x00800000 : 0),
                     0);
        const auto search = [&](LRESULT first, LRESULT last) {
            SendMessageW(editor, kSciSetTargetStart, first, 0);
            SendMessageW(editor, kSciSetTargetEnd, last, 0);
            return SendMessageA(
                editor, kSciSearchInTarget, query.size(), reinterpret_cast<LPARAM>(query.data()));
        };
        const bool needs_right_context = regex && (query.find("(?=") != std::string_view::npos ||
                                                   query.find("(?!") != std::string_view::npos ||
                                                   query.find("\\b") != std::string_view::npos ||
                                                   query.find("\\B") != std::string_view::npos ||
                                                   query.find('$') != std::string_view::npos);
        if (!needs_right_context || start <= end)
            return search(start, end);
        // C++ 正则的反向目标范围会截断右侧上下文。仅对依赖该上下文的表达式
        // 向前枚举候选，取光标前最近的一处，普通搜索仍走控件的直接反向查找。
        // 保留重叠和零长度匹配，避免前瞻/单词边界因此跳到错误结果。
        const auto length = SendMessageW(editor, kSciGetTextLength, 0, 0);
        LRESULT cursor = end, previous = -1;
        for (;;) {
            const auto found = search(cursor, length);
            if (SendMessageW(editor, kSciGetStatus, 0, 0) != 0)
                return -1;
            if (found < 0 || found > start)
                break;
            if (SendMessageW(editor, kSciGetTargetEnd, 0, 0) <= start)
                previous = found;
            const auto next = SendMessageW(editor, kSciPositionAfter, found, 0);
            if (next <= found || next > start)
                break;
            cursor = next;
        }
        // 重新绑定最终目标及捕获组，供调用方读取边界或执行单次正则替换。
        return previous < 0 ? -1 : search(previous, length);
    }

    bool SearchEncodingSupported(bool regex) {
        const auto* document = ActiveDocument();
        if (regex && document && document->encoding == EditorTextEncoding::Legacy8Bit) {
            // 未确认的八位内容仍按原始字节保存。不能把正则的单字符匹配应用到半个
            // 双字节汉字；用户先选择 UTF-8/GB2312 完成无损解码后才开放正则。
            find_bar.Status(L"请先选择文件编码，再使用正则表达式", true);
            return false;
        }
        return true;
    }

    LRESULT TextLeftInset() const {
        auto left = SendMessageW(editor, kSciGetMarginLeft, 0, 0);
        const auto count = std::clamp<LRESULT>(SendMessageW(editor, kSciGetMargins, 0, 0), 1, 32);
        for (LRESULT margin = 0; margin < count; ++margin)
            left += std::max<LRESULT>(0, SendMessageW(editor, kSciGetMarginWidthN, margin, 0));
        return left;
    }

    void CaptureWrapViewport() {
        if (!editor || !scintilla_available || !active_id) {
            pending_wrap_viewport.reset();
            return;
        }
        // 设置页内连续切换时，始终保留第一次切换前的锚点，不使用中间的过期几何。
        if (pending_wrap_viewport && pending_wrap_viewport->document == *active_id)
            return;
        RECT client{};
        GetClientRect(editor, &client);
        auto position = SendMessageW(editor, kSciGetCurrentPos, 0, 0);
        const auto x = SendMessageW(editor, kSciPointXFromPosition, 0, position);
        auto y = SendMessageW(editor, kSciPointYFromPosition, 0, position);
        const auto height = std::max<LRESULT>(1, SendMessageW(editor, kSciTextHeight, 0, 0));
        const auto left = TextLeftInset();
        if (x < left || x >= client.right || y < 0 || y + height > client.bottom) {
            // 光标已在屏幕外时，用户正在阅读别处，保留视口首个字符而非跳回光标。
            position =
                SendMessageW(editor,
                             kSciPositionFromPointClose,
                             std::clamp<LRESULT>(left + 1, 0, std::max(0L, client.right - 1)),
                             height / 2);
            y = 0;
        }
        pending_wrap_viewport = WrapViewportAnchor{*active_id,
                                                   std::max<LRESULT>(0, position),
                                                   static_cast<int>(y),
                                                   detail::WindowDpi(editor)};
    }

    void AvoidFindBarForMatch(LRESULT start, LRESULT end) {
        if (!find_bar.visible())
            return;
        RECT client{}, bar{};
        GetClientRect(editor, &client);
        GetClientRect(find_bar.hwnd(), &bar);
        const int gap = detail::ScaleForDpi(6, detail::WindowDpi(hwnd));
        const auto height =
            static_cast<LONG>(std::max<LRESULT>(1, SendMessageW(editor, kSciTextHeight, 0, 0)));
        const auto x = static_cast<LONG>(SendMessageW(editor, kSciPointXFromPosition, 0, start));
        const auto y = static_cast<LONG>(SendMessageW(editor, kSciPointYFromPosition, 0, start));
        const auto end_x = static_cast<LONG>(SendMessageW(editor, kSciPointXFromPosition, 0, end));
        const auto end_y = static_cast<LONG>(SendMessageW(editor, kSciPointYFromPosition, 0, end));
        RECT match{end_y == y ? x : 0,
                   std::max(0L, y),
                   end_y == y ? std::max(x + 2, end_x) : client.right,
                   std::min(client.bottom, std::max(y, end_y) + height)};
        const int max_top = std::max(0L, client.bottom - bar.bottom - gap);
        const int normal_top = std::min(gap, max_top);
        const auto at = [&](int top) -> RECT {
            return {std::max(0L, client.right - gap - bar.right),
                    top,
                    client.right - gap,
                    top + bar.bottom};
        };
        RECT intersection{};
        int top = normal_top;
        auto normal = at(top);
        if (IntersectRect(&intersection, &normal, &match)) {
            const auto choose = [&](RECT avoid) {
                if (avoid.bottom + gap <= max_top) {
                    top = avoid.bottom + gap;
                    return true;
                }
                if (avoid.top - gap - bar.bottom >= 0) {
                    top = avoid.top - gap - bar.bottom;
                    return true;
                }
                return false;
            };
            if (!choose(match)) {
                // 匹配跨越整个视口时优先露出开头；极矮视口保持零横向偏移。
                RECT first{x, std::max(0L, y), client.right, std::min(client.bottom, y + height)};
                (void)choose(first);
            }
        }
        find_bar_top = top == normal_top ? std::nullopt : std::optional<int>(top);
        PlaceFindBar();
    }

    void RestoreWrapViewport() {
        if (!pending_wrap_viewport || restoring_wrap_viewport)
            return;
        if (active_id != pending_wrap_viewport->document) {
            pending_wrap_viewport.reset();
            return;
        }
        RECT client{};
        GetClientRect(editor, &client);
        if (!IsWindowVisible(editor) || IsIconic(GetAncestor(hwnd, GA_ROOT)) || client.right <= 0 ||
            client.bottom <= 0)
            return;
        restoring_wrap_viewport = true;
        struct Guard {
            bool& flag;
            ~Guard() {
                flag = false;
            }
        } guard{restoring_wrap_viewport};
        const auto anchor = *std::exchange(pending_wrap_viewport, std::nullopt);
        const auto position =
            std::clamp<LRESULT>(anchor.position, 0, SendMessageW(editor, kSciGetTextLength, 0, 0));
        RedrawWindow(editor, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
        SendMessageW(editor, kSciScrollRange, position, position);
        if (word_wrap)
            SendMessageW(editor, kSciSetXOffset, 0, 0);
        // 先完成目标段落折行，再按字节位置映射回新的显示行，不能复用旧显示行号。
        RedrawWindow(editor, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
        const auto height = std::max<LRESULT>(1, SendMessageW(editor, kSciTextHeight, 0, 0));
        const auto display_line =
            SendMessageW(editor, kSciGetFirstVisibleLine, 0, 0) +
            SendMessageW(editor, kSciPointYFromPosition, 0, position) / height;
        const auto target_y =
            std::clamp<LRESULT>(MulDiv(anchor.y, detail::WindowDpi(editor), anchor.dpi),
                                0,
                                std::max<LRESULT>(0, client.bottom - height));
        SendMessageW(editor,
                     kSciSetFirstVisibleLine,
                     std::max<LRESULT>(0, display_line - target_y / height),
                     0);
        AvoidFindBarForMatch(position, position);
        UpdateEditorScrollbarWindows();
    }

    void RevealSearchMatch(LRESULT start, LRESULT end) {
        const auto line = SendMessageW(editor, kSciLineFromPosition, start, 0);
        SendMessageW(editor, kSciEnsureVisible, line, 0);
        if (end > start)
            SendMessageW(editor,
                         kSciEnsureVisible,
                         SendMessageW(editor, kSciLineFromPosition, end - 1, 0),
                         0);
        // 优先显示匹配的开头，而不是只滚动到选区末端；长行也不会把开头滚走。
        UpdateScrollWidthForCurrentLine();
        if (word_wrap)
            SendMessageW(editor, kSciSetXOffset, 0, 0);
        SendMessageW(editor, kSciScrollRange, end, start);
        RECT client{};
        GetClientRect(editor, &client);
        const int line_height =
            static_cast<int>(std::max<LRESULT>(1, SendMessageW(editor, kSciTextHeight, line, 0)));
        const auto y = SendMessageW(editor, kSciPointYFromPosition, 0, start);
        if (y < line_height || y + line_height > client.bottom - line_height) {
            // 自动换行时同一文档行有多个显示行，定位到命中所在的那一段而非行首。
            const auto display_line = SendMessageW(editor, kSciGetFirstVisibleLine, 0, 0) +
                                      std::max<LRESULT>(0, y) / line_height;
            const int preferred_y = static_cast<int>(client.bottom) / 2;
            const int usable_y = std::max(0, static_cast<int>(client.bottom) - line_height * 2);
            SendMessageW(
                editor,
                kSciSetFirstVisibleLine,
                std::max<LRESULT>(0, display_line - std::min(preferred_y, usable_y) / line_height),
                0);
        }
        // 只移动浮层避让匹配，尤其换行模式绝不能引入无法通过滚动条复位的 XOffset。
        if (word_wrap)
            SendMessageW(editor, kSciSetXOffset, 0, 0);
        AvoidFindBarForMatch(start, end);
        UpdateEditorScrollbarWindows();
        InvalidateRect(editor, nullptr, FALSE);
    }

    bool FindText(std::wstring_view query, bool backwards, bool match_case, bool regex) {
        if (!ActiveDocument() || !scintilla_available)
            return false;
        if (!SearchEncodingSupported(regex))
            return false;
        if (query.empty()) {
            find_bar.Status(L"请输入查找内容");
            return false;
        }
        auto bytes = SearchBytes(query);
        if (!bytes)
            return false;
        const LRESULT length = SendMessageW(editor, kSciGetTextLength, 0, 0);
        LRESULT start =
            SendMessageW(editor, backwards ? kSciGetSelectionStart : kSciGetSelectionEnd, 0, 0);
        bool wrap = false;
        if (regex && last_search_document == active_id && last_search_query == query &&
            last_search_case == match_case && last_search_regex && last_search_position == start &&
            SendMessageW(editor, kSciGetSelectionStart, 0, 0) ==
                SendMessageW(editor, kSciGetSelectionEnd, 0, 0)) {
            const auto next =
                SendMessageW(editor, backwards ? kSciPositionBefore : kSciPositionAfter, start, 0);
            wrap = next == start;
            start = next;
        }
        LRESULT found =
            wrap ? -1 : SearchRange(*bytes, start, backwards ? 0 : length, match_case, regex);
        if (SendMessageW(editor, kSciGetStatus, 0, 0) != 0) {
            find_bar.Status(L"正则表达式无效", true);
            return false;
        }
        if (found < 0)
            // 回绕必须搜索完整文档，不能在原光标处截断匹配正文。
            found = SearchRange(
                *bytes, backwards ? length : 0, backwards ? 0 : length, match_case, regex);
        if (found < 0) {
            find_bar.Status(L"未找到", true);
            return false;
        }
        const auto end = SendMessageW(editor, kSciGetTargetEnd, 0, 0);
        last_search_document = active_id;
        last_search_query = query;
        last_search_case = match_case;
        last_search_regex = regex;
        last_search_position = found == end ? found : -1;
        SendMessageW(editor, kSciSetSel, found, end);
        RevealSearchMatch(found, end);
        find_bar.Status(L"已定位");
        return true;
    }

    std::size_t ReplaceText(std::wstring_view query,
                            std::wstring_view replacement,
                            bool all,
                            bool match_case,
                            bool regex) {
        auto* document = ActiveDocument();
        if (!document || !scintilla_available || document->kind != EditorDocumentKind::Text ||
            document->read_only || document->saving || document->closing) {
            find_bar.Status(L"当前文件不可修改", true);
            return 0;
        }
        if (query.empty()) {
            find_bar.Status(L"请输入查找内容");
            return 0;
        }
        if (!SearchEncodingSupported(regex))
            return 0;
        auto needle = SearchBytes(query);
        auto text = SearchBytes(replacement);
        if (!needle || !text)
            return 0;
        LRESULT length = SendMessageW(editor, kSciGetTextLength, 0, 0);
        LRESULT start = 0;
        if (!all) {
            start = SendMessageW(editor, kSciGetSelectionStart, 0, 0);
            const auto end = SendMessageW(editor, kSciGetSelectionEnd, 0, 0);
            // 前瞻等正则需要选区右侧的上下文；先在完整后文中匹配，再验证边界。
            const auto selected = SearchRange(*needle, start, length, match_case, regex);
            if (selected != start || SendMessageW(editor, kSciGetTargetEnd, 0, 0) != end) {
                if (!FindText(query, false, match_case, regex))
                    return 0;
                start = SendMessageW(editor, kSciGetSelectionStart, 0, 0);
            }
        }
        // 批量修改只通知一次控制器，避免每个匹配都复制整篇正文并调度恢复缓存。
        const bool was_dirty = document->dirty;
        batch_replacing = true;
        SendMessageW(editor, kSciBeginUndoAction, 0, 0);
        std::size_t count{};
        bool invalid = false, too_large = false;
        for (;;) {
            const auto found = SearchRange(*needle, start, length, match_case, regex);
            if (SendMessageW(editor, kSciGetStatus, 0, 0) != 0) {
                invalid = true;
                break;
            }
            if (found < 0)
                break;
            const auto match_end = SendMessageW(editor, kSciGetTargetEnd, 0, 0);
            constexpr LRESULT maximum = 40 * 1024 * 1024;
            if (length - (match_end - found) + static_cast<LRESULT>(text->size()) > maximum) {
                too_large = true;
                break;
            }
            const auto written = SendMessageA(editor,
                                              regex ? kSciReplaceTargetRe : kSciReplaceTarget,
                                              text->size(),
                                              reinterpret_cast<LPARAM>(text->data()));
            if (written < 0)
                break;
            ++count;
            length = SendMessageW(editor, kSciGetTextLength, 0, 0);
            start = SendMessageW(editor, kSciGetTargetEnd, 0, 0);
            if (length > maximum) {
                too_large = true;
                break;
            }
            if (!all) {
                SendMessageW(editor, kSciSetSel, start, start);
                if (match_end == found) {
                    last_search_document = active_id;
                    last_search_query = query;
                    last_search_case = match_case;
                    last_search_regex = regex;
                    last_search_position = start;
                }
                break;
            }
            // 零长度正则必须跨过原文的一个完整字符，不搜索刚插入的内容。
            if (match_end == found) {
                const auto after = SendMessageW(editor, kSciPositionAfter, start, 0);
                if (after <= start || start >= length)
                    break;
                start = after;
            }
        }
        SendMessageW(editor, kSciEndUndoAction, 0, 0);
        if (too_large && count != 0) {
            SendMessageW(editor, 2176 /* SCI_UNDO */, 0, 0);
            count = 0;
            document->dirty = was_dirty;
        }
        batch_replacing = false;
        if (count != 0) {
            document->dirty = true;
            MarkTouchedLine(
                *document,
                SendMessageW(
                    editor, kSciLineFromPosition, SendMessageW(editor, kSciGetTargetEnd, 0, 0), 0));
            UpdateLineNumberMargin();
            ScheduleHorizontalScrollbarUpdate();
            UpdateStatus();
            InvokeSafely(document_changed, document->id);
            if (!all)
                FindText(query, false, match_case, regex);
        }
        find_bar.Status(invalid     ? L"正则表达式无效"
                        : too_large ? L"替换后超出编辑大小限制，操作已取消"
                        : count     ? L"已替换 " + std::to_wstring(count) + L" 处"
                                    : L"未找到",
                        invalid || too_large || count == 0);
        return count;
    }

    void NavigateFromPath() {
        if (selected_server_id.empty()) {
            return;
        }
        InvokeSafely(path_changed, selected_server_id, WindowText(path_edit));
    }

    void ChangeEncoding() {
        auto* document = ActiveDocument();
        if (!document || document->kind != EditorDocumentKind::Text || document->read_only ||
            document->saving || document->closing) {
            return;
        }
        const auto target = SendMessageW(encoding_selector, CB_GETCURSEL, 0, 0) == 1
                                ? EditorTextEncoding::Gb2312
                                : EditorTextEncoding::Utf8;
        if (document->encoding == target)
            return;
        SyncActiveContent();
        std::string text = document->content;
        if (document->encoding == EditorTextEncoding::Legacy8Bit) {
            if (target == EditorTextEncoding::Gb2312) {
                auto decoded = editor::DecodeGb2312(text);
                if (!decoded) {
                    ::SLink::ui::ShowMessageDialog(hwnd,
                                                   L"文件中含有无效的 GB2312 字节，无法切换编码。",
                                                   L"SLink",
                                                   MB_OK | MB_ICONERROR);
                    SendMessageW(encoding_selector, CB_SETCURSEL, static_cast<WPARAM>(-1), 0);
                    return;
                }
                text = std::move(*decoded);
            } else if (!text.empty() && MultiByteToWideChar(CP_UTF8,
                                                            MB_ERR_INVALID_CHARS,
                                                            text.data(),
                                                            static_cast<int>(text.size()),
                                                            nullptr,
                                                            0) <= 0) {
                ::SLink::ui::ShowMessageDialog(
                    hwnd,
                    L"文件内容不是有效的 UTF-8，请选择 GB2312 或保持原编码。",
                    L"SLink",
                    MB_OK | MB_ICONERROR);
                SendMessageW(encoding_selector, CB_SETCURSEL, static_cast<WPARAM>(-1), 0);
                return;
            }
        }
        if (target == EditorTextEncoding::Gb2312 && !editor::EncodeGb2312(text)) {
            ::SLink::ui::ShowMessageDialog(hwnd,
                                           L"文本含有 GB2312 无法表示的字符，切换后无法无损保存。",
                                           L"SLink",
                                           MB_OK | MB_ICONERROR);
            SendMessageW(encoding_selector,
                         CB_SETCURSEL,
                         document->encoding == EditorTextEncoding::Gb2312
                             ? 1
                             : (document->encoding == EditorTextEncoding::Legacy8Bit
                                    ? static_cast<WPARAM>(-1)
                                    : 0),
                         0);
            return;
        }
        const bool reload = document->encoding == EditorTextEncoding::Legacy8Bit;
        document->content = std::move(text);
        document->encoding = target;
        document->dirty = true;
        EditorSaveRequest request{
            document->id, document->content, document->encoding, document->eol};
        if (reload)
            LoadActiveDocument();
        InvokeSafely(encoding_changed, std::move(request));
        if (scintilla_available && !document->content.empty())
            std::string{}.swap(document->content);
        UpdateStatus();
        InvalidateRect(hwnd, nullptr, FALSE);
        if (editor && IsWindowVisible(editor))
            SetFocus(editor);
    }

    DocumentState* FindDocument(EditorDocumentId id) {
        const auto item =
            std::find_if(documents.begin(), documents.end(), [id](const DocumentState& document) {
                return document.id == id;
            });
        return item == documents.end() ? nullptr : &*item;
    }

    const DocumentState* FindDocument(EditorDocumentId id) const {
        const auto item =
            std::find_if(documents.begin(), documents.end(), [id](const DocumentState& document) {
                return document.id == id;
            });
        return item == documents.end() ? nullptr : &*item;
    }

    std::wstring ResolveServerName(std::string_view server_id) const {
        if (server_id.empty()) {
            return {};
        }
        const auto server = std::ranges::find(server_options, server_id, &EditorServerOption::id);
        if (server != server_options.end()) {
            return server->label;
        }
        return server_id == "local://host" ? L"本地主机" : Utf8ToWide(server_id);
    }

    void BindDocumentServer(DocumentState& document, std::string server_id) {
        // 显示名在文档首次打开时做快照；切换侧栏选择或刷新服务器列表不会串到其他文件。
        if (server_id.empty() && !document.server_id.empty()) {
            return;
        }
        if (document.server_id == server_id && !document.server_name.empty()) {
            return;
        }
        document.server_id = std::move(server_id);
        document.server_name = ResolveServerName(document.server_id);
    }

    DocumentState* ActiveDocument() {
        return active_id ? FindDocument(*active_id) : nullptr;
    }

    const DocumentState* ActiveDocument() const {
        return active_id ? FindDocument(*active_id) : nullptr;
    }

    int DocumentIndex(EditorDocumentId id) const {
        for (std::size_t index = 0; index < documents.size(); ++index) {
            if (documents[index].id == id) {
                return static_cast<int>(index);
            }
        }
        return -1;
    }

    void LoadEditorModules() {
        const std::filesystem::path directory = detail::ExecutableDirectory();
        const std::filesystem::path scintilla = directory / L"Scintilla.dll";
        const std::filesystem::path lexilla = directory / L"Lexilla.dll";
        scintilla_module = LoadLibraryW(scintilla.c_str());
        if (std::filesystem::exists(lexilla)) {
            lexilla_module = LoadLibraryW(lexilla.c_str());
        }
    }

    EditorScrollMetrics EditorScrollLayout() const {
        EditorScrollMetrics metrics;
        if (!editor || !IsWindow(editor) || !active_id) {
            return metrics;
        }
        RECT client{};
        GetClientRect(editor, &client);
        const int thickness = detail::ScaleForDpi(8, detail::WindowDpi(editor));
        const int minimum_thumb = detail::ScaleForDpi(18, detail::WindowDpi(editor));
        const int margin_count =
            std::clamp(static_cast<int>(SendMessageW(editor, kSciGetMargins, 0, 0)), 1, 32);
        int left_inset =
            std::max(0, static_cast<int>(SendMessageW(editor, kSciGetMarginLeft, 0, 0)));
        for (int margin = 0; margin < margin_count; ++margin) {
            left_inset +=
                std::max(0, static_cast<int>(SendMessageW(editor, kSciGetMarginWidthN, margin, 0)));
        }
        const int right_inset =
            std::max(0, static_cast<int>(SendMessageW(editor, kSciGetMarginRight, 0, 0)));
        const int edge_padding = detail::ScaleForDpi(2, detail::WindowDpi(editor));
        metrics.horizontal_page = std::max(1,
                                           static_cast<int>(client.right - client.left) -
                                               left_inset - right_inset - edge_padding * 2);
        const int content_width =
            std::max(1, static_cast<int>(SendMessageW(editor, kSciGetScrollWidth, 0, 0)));
        metrics.horizontal_max =
            word_wrap ? 0 : std::max(0, content_width - metrics.horizontal_page);
        metrics.horizontal_position =
            std::clamp(static_cast<int>(SendMessageW(editor, kSciGetXOffset, 0, 0)),
                       0,
                       metrics.horizontal_max);
        metrics.horizontal_visible = metrics.horizontal_max > 0;

        const int document_lines =
            std::max(1, static_cast<int>(SendMessageW(editor, kSciGetLineCount, 0, 0)));
        // 自绘滚动条使用显示行总数，视觉折行不能被误当成只有一行而隐藏竖向滚动条。
        const int line_count =
            word_wrap
                ? std::max(1,
                           static_cast<int>(
                               SendMessageW(editor, kSciVisibleFromDocLine, document_lines - 1, 0) +
                               SendMessageW(editor, kSciWrapCount, document_lines - 1, 0)))
                : document_lines;
        metrics.vertical_page =
            std::max(1, static_cast<int>(SendMessageW(editor, kSciLinesOnScreen, 0, 0)));
        metrics.vertical_max = std::max(0, line_count - metrics.vertical_page);
        metrics.vertical_position =
            std::clamp(static_cast<int>(SendMessageW(editor, kSciGetFirstVisibleLine, 0, 0)),
                       0,
                       metrics.vertical_max);
        metrics.vertical_visible = metrics.vertical_max > 0;

        if (metrics.horizontal_visible) {
            metrics.horizontal_track = {
                std::min<int>(client.right, client.left + left_inset + edge_padding),
                std::max<int>(client.top, client.bottom - thickness),
                std::max<int>(client.left,
                              client.right - right_inset -
                                  (metrics.vertical_visible ? thickness : 0) - edge_padding),
                client.bottom};
            const int length = std::max(
                1,
                static_cast<int>(metrics.horizontal_track.right - metrics.horizontal_track.left));
            const int thumb_length = std::clamp(length * metrics.horizontal_page / content_width,
                                                std::min(minimum_thumb, length),
                                                length);
            const int travel = std::max(0, length - thumb_length);
            const int offset = metrics.horizontal_max > 0
                                   ? travel * metrics.horizontal_position / metrics.horizontal_max
                                   : 0;
            metrics.horizontal_thumb = {metrics.horizontal_track.left + offset + 1,
                                        metrics.horizontal_track.top + 1,
                                        metrics.horizontal_track.left + offset + thumb_length - 1,
                                        metrics.horizontal_track.bottom - 1};
        }
        if (metrics.vertical_visible) {
            metrics.vertical_track = {
                std::max<int>(client.left, client.right - thickness),
                client.top,
                client.right,
                std::max<int>(client.top,
                              client.bottom - (metrics.horizontal_visible ? thickness : 0))};
            const int length = std::max(
                1, static_cast<int>(metrics.vertical_track.bottom - metrics.vertical_track.top));
            const int thumb_length = std::clamp(length * metrics.vertical_page / line_count,
                                                std::min(minimum_thumb, length),
                                                length);
            const int travel = std::max(0, length - thumb_length);
            const int offset = metrics.vertical_max > 0
                                   ? travel * metrics.vertical_position / metrics.vertical_max
                                   : 0;
            metrics.vertical_thumb = {metrics.vertical_track.left + 1,
                                      metrics.vertical_track.top + offset + 1,
                                      metrics.vertical_track.right - 1,
                                      metrics.vertical_track.top + offset + thumb_length - 1};
        }
        return metrics;
    }

    int EstimatedTextWidth(std::string_view text) const {
        std::size_t maximum_columns = 0;
        std::size_t columns = 0;
        for (std::size_t index = 0; index < text.size();) {
            const unsigned char lead = static_cast<unsigned char>(text[index]);
            if (lead == '\r') {
                ++index;
                continue;
            }
            if (lead == '\n') {
                maximum_columns = std::max(maximum_columns, columns);
                columns = 0;
                ++index;
                continue;
            }
            if (lead == '\t') {
                columns += 4 - columns % 4;
                ++index;
                continue;
            }
            std::uint32_t codepoint = lead;
            std::size_t length = 1;
            if ((lead & 0xE0U) == 0xC0U && index + 1 < text.size()) {
                codepoint =
                    ((lead & 0x1FU) << 6U) | (static_cast<unsigned char>(text[index + 1]) & 0x3FU);
                length = 2;
            } else if ((lead & 0xF0U) == 0xE0U && index + 2 < text.size()) {
                codepoint = ((lead & 0x0FU) << 12U) |
                            ((static_cast<unsigned char>(text[index + 1]) & 0x3FU) << 6U) |
                            (static_cast<unsigned char>(text[index + 2]) & 0x3FU);
                length = 3;
            } else if ((lead & 0xF8U) == 0xF0U && index + 3 < text.size()) {
                codepoint = ((lead & 0x07U) << 18U) |
                            ((static_cast<unsigned char>(text[index + 1]) & 0x3FU) << 12U) |
                            ((static_cast<unsigned char>(text[index + 2]) & 0x3FU) << 6U) |
                            (static_cast<unsigned char>(text[index + 3]) & 0x3FU);
                length = 4;
            }
            const bool wide =
                codepoint >= 0x1100U &&
                (codepoint <= 0x115FU || codepoint == 0x2329U || codepoint == 0x232AU ||
                 (codepoint >= 0x2E80U && codepoint <= 0xA4CFU) ||
                 (codepoint >= 0xAC00U && codepoint <= 0xD7A3U) ||
                 (codepoint >= 0xF900U && codepoint <= 0xFAFFU) ||
                 (codepoint >= 0xFE10U && codepoint <= 0xFE6FU) ||
                 (codepoint >= 0xFF00U && codepoint <= 0xFF60U) ||
                 (codepoint >= 0x1F300U && codepoint <= 0x1FAFFU));
            columns += wide ? 2U : 1U;
            index += length;
        }
        maximum_columns = std::max(maximum_columns, columns);
        const int cell_width =
            std::max(1,
                     static_cast<int>(SendMessageA(
                         editor, kSciTextWidth, kStyleDefault, reinterpret_cast<LPARAM>("0"))));
        const std::size_t maximum_pixels =
            static_cast<std::size_t>((std::numeric_limits<int>::max)() - 32);
        const std::size_t pixels =
            std::min(maximum_pixels, maximum_columns * static_cast<std::size_t>(cell_width));
        return std::max(1, static_cast<int>(pixels) + 16);
    }

    void UpdateScrollWidthForCurrentLine() const {
        if (!editor || !IsWindow(editor)) {
            return;
        }
        const LRESULT position = SendMessageW(editor, kSciGetCurrentPos, 0, 0);
        const LRESULT line = SendMessageW(editor, kSciLineFromPosition, position, 0);
        const LRESULT length = SendMessageW(editor, kSciLineLength, line, 0);
        if (length <= 0 || length > (std::numeric_limits<int>::max)() - 1) {
            return;
        }
        std::string text(static_cast<std::size_t>(length) + 1, '\0');
        const LRESULT copied =
            SendMessageA(editor, kSciGetLine, line, reinterpret_cast<LPARAM>(text.data()));
        if (copied <= 0) {
            return;
        }
        text.resize(static_cast<std::size_t>(copied));
        const int required = EstimatedTextWidth(text);
        const int current = static_cast<int>(SendMessageW(editor, kSciGetScrollWidth, 0, 0));
        if (required > current) {
            SendMessageW(editor, kSciSetScrollWidth, required, 0);
        }
    }

    void PaintEditorScrollbar(HWND scrollbar, bool horizontal) const {
        const EditorScrollMetrics metrics = EditorScrollLayout();
        PAINTSTRUCT paint{};
        const HDC dc = BeginPaint(scrollbar, &paint);
        RECT client{};
        GetClientRect(scrollbar, &client);
        const HBRUSH track = CreateSolidBrush(theme::kPanelAltColor);
        FillRect(dc, &client, track);
        const HBRUSH thumb_brush = CreateSolidBrush(theme::kPanelHoverColor);
        const HGDIOBJ old_brush = SelectObject(dc, thumb_brush);
        const HGDIOBJ old_pen = SelectObject(dc, GetStockObject(NULL_PEN));
        const int radius = detail::ScaleForDpi(5, detail::WindowDpi(editor));
        RECT thumb_rect = horizontal ? metrics.horizontal_thumb : metrics.vertical_thumb;
        const RECT origin = horizontal ? metrics.horizontal_track : metrics.vertical_track;
        OffsetRect(&thumb_rect, -origin.left, -origin.top);
        if ((horizontal && metrics.horizontal_visible) ||
            (!horizontal && metrics.vertical_visible)) {
            RoundRect(dc,
                      thumb_rect.left,
                      thumb_rect.top,
                      thumb_rect.right,
                      thumb_rect.bottom,
                      radius,
                      radius);
        }
        SelectObject(dc, old_pen);
        SelectObject(dc, old_brush);
        DeleteObject(thumb_brush);
        DeleteObject(track);
        EndPaint(scrollbar, &paint);
    }

    void UpdateEditorScrollbarWindows() const {
        if (!editor_horizontal_scrollbar || !editor_vertical_scrollbar) {
            return;
        }
        const EditorScrollMetrics metrics = EditorScrollLayout();
        const auto update = [](HWND window,
                               const RECT& bounds,
                               const RECT& thumb,
                               bool visible,
                               EditorScrollbarWindowState& state) {
            if (!visible) {
                if (!state.initialized || state.visible) {
                    ShowWindow(window, SW_HIDE);
                }
                state = {bounds, thumb, false, true};
                return;
            }
            const bool bounds_changed = !state.initialized || !EqualRect(&state.bounds, &bounds);
            const bool thumb_changed = !state.initialized || !EqualRect(&state.thumb, &thumb);
            const bool visibility_changed = !state.initialized || !state.visible;
            if (bounds_changed) {
                SetWindowPos(window,
                             HWND_TOP,
                             bounds.left,
                             bounds.top,
                             std::max<LONG>(1, bounds.right - bounds.left),
                             std::max<LONG>(1, bounds.bottom - bounds.top),
                             SWP_NOACTIVATE | SWP_NOOWNERZORDER);
            }
            if (visibility_changed) {
                ShowWindow(window, SW_SHOWNOACTIVATE);
            }
            if (bounds_changed || thumb_changed || visibility_changed) {
                InvalidateRect(window, nullptr, FALSE);
            }
            state = {bounds, thumb, true, true};
        };
        update(editor_horizontal_scrollbar,
               metrics.horizontal_track,
               metrics.horizontal_thumb,
               metrics.horizontal_visible,
               horizontal_scrollbar_state);
        update(editor_vertical_scrollbar,
               metrics.vertical_track,
               metrics.vertical_thumb,
               metrics.vertical_visible,
               vertical_scrollbar_state);
    }

    bool BeginEditorScroll(POINT point) {
        const EditorScrollMetrics metrics = EditorScrollLayout();
        if (metrics.horizontal_visible && PtInRect(&metrics.horizontal_track, point)) {
            if (PtInRect(&metrics.horizontal_thumb, point)) {
                editor_scroll_drag = EditorScrollDrag::horizontal;
                editor_scroll_drag_offset = point.x - metrics.horizontal_thumb.left;
                SetCapture(editor);
            } else {
                const int direction = point.x < metrics.horizontal_thumb.left ? -1 : 1;
                SendMessageW(
                    editor,
                    kSciSetXOffset,
                    std::clamp(metrics.horizontal_position + direction * metrics.horizontal_page,
                               0,
                               metrics.horizontal_max),
                    0);
            }
            UpdateEditorScrollbarWindows();
            return true;
        }
        if (metrics.vertical_visible && PtInRect(&metrics.vertical_track, point)) {
            if (PtInRect(&metrics.vertical_thumb, point)) {
                editor_scroll_drag = EditorScrollDrag::vertical;
                editor_scroll_drag_offset = point.y - metrics.vertical_thumb.top;
                SetCapture(editor);
            } else {
                const int direction = point.y < metrics.vertical_thumb.top ? -1 : 1;
                SendMessageW(editor, kSciLineScroll, 0, direction * metrics.vertical_page);
            }
            UpdateEditorScrollbarWindows();
            return true;
        }
        return false;
    }

    void UpdateEditorScroll(POINT point) {
        const EditorScrollMetrics metrics = EditorScrollLayout();
        if (editor_scroll_drag == EditorScrollDrag::horizontal && metrics.horizontal_visible) {
            const int thumb_length =
                static_cast<int>(metrics.horizontal_thumb.right - metrics.horizontal_thumb.left);
            const int travel = std::max(
                1,
                static_cast<int>(metrics.horizontal_track.right - metrics.horizontal_track.left) -
                    thumb_length);
            const int offset =
                std::clamp(static_cast<int>(point.x - metrics.horizontal_track.left) -
                               editor_scroll_drag_offset,
                           0,
                           travel);
            SendMessageW(editor, kSciSetXOffset, offset * metrics.horizontal_max / travel, 0);
        } else if (editor_scroll_drag == EditorScrollDrag::vertical && metrics.vertical_visible) {
            const int thumb_length =
                static_cast<int>(metrics.vertical_thumb.bottom - metrics.vertical_thumb.top);
            const int travel = std::max(
                1,
                static_cast<int>(metrics.vertical_track.bottom - metrics.vertical_track.top) -
                    thumb_length);
            const int offset = std::clamp(static_cast<int>(point.y - metrics.vertical_track.top) -
                                              editor_scroll_drag_offset,
                                          0,
                                          travel);
            const int target = offset * metrics.vertical_max / travel;
            SendMessageW(editor, kSciLineScroll, 0, target - metrics.vertical_position);
        }
        UpdateEditorScrollbarWindows();
    }

    void EndEditorScroll() {
        editor_scroll_drag = EditorScrollDrag::none;
        if (GetCapture() == editor) {
            ReleaseCapture();
        }
        UpdateEditorScrollbarWindows();
    }

    void ScrollEditorHorizontally(int wheel_delta) const {
        const EditorScrollMetrics metrics = EditorScrollLayout();
        if (!metrics.horizontal_visible || wheel_delta == 0) {
            return;
        }
        const int step = detail::ScaleForDpi(48, detail::WindowDpi(editor));
        int pixels = MulDiv(wheel_delta, step, WHEEL_DELTA);
        if (pixels == 0) {
            pixels = wheel_delta > 0 ? 1 : -1;
        }
        SendMessageW(editor,
                     kSciSetXOffset,
                     std::clamp(metrics.horizontal_position + pixels, 0, metrics.horizontal_max),
                     0);
        UpdateEditorScrollbarWindows();
    }

    void ConfigureScintilla() const {
        const bool dark = theme::IsDark();
        const COLORREF editor_background = theme::TerminalBackgroundColor();
        const COLORREF margin_background = editor_background;
        const COLORREF margin_foreground = dark ? RGB(105, 108, 115) : RGB(135, 139, 146);
        const auto* document = ActiveDocument();
        SendMessageW(editor, kSciSetWrapMode, word_wrap ? 1 /* SC_WRAP_WORD */ : 0, 0);
        SendMessageW(editor,
                     kSciSetCodePage,
                     document && document->encoding == EditorTextEncoding::Legacy8Bit ? 0
                                                                                      : kScCpUtf8,
                     0);
        SendMessageW(editor, kSciStyleSetFore, kStyleDefault, theme::kTextColor);
        SendMessageW(editor, kSciStyleSetBack, kStyleDefault, editor_background);
        SendMessageA(editor, kSciStyleSetFont, kStyleDefault, reinterpret_cast<LPARAM>("Consolas"));
        SendMessageW(editor, kSciStyleSetSize, kStyleDefault, 11);
        SendMessageW(editor, kSciStyleSetBold, kStyleDefault, FALSE);
        SendMessageW(editor, kSciStyleClearAll, 0, 0);
        SendMessageW(editor, kSciStyleSetFore, kStyleLineNumber, margin_foreground);
        SendMessageW(editor, kSciStyleSetBack, kStyleLineNumber, margin_background);
        SendMessageA(
            editor, kSciStyleSetFont, kStyleLineNumber, reinterpret_cast<LPARAM>("Consolas"));
        SendMessageW(editor, kSciStyleSetSize, kStyleLineNumber, 9);
        // 跟随 STYLE_DEFAULT 背景，深色主题下不再出现系统默认的白边。
        SendMessageW(editor, 2240 /* SCI_SETMARGINTYPEN */, 1, 2 /* SC_MARGIN_BACK */);
        SendMessageW(editor, 2244 /* SCI_SETMARGINMASKN */, 1, kChangeMarkerMask);
        SendMessageW(
            editor, kSciSetMarginWidthN, 1, detail::ScaleForDpi(4, detail::WindowDpi(hwnd)));
        for (int marker : {23, 24, kRestoredChangeMarker}) {
            SendMessageW(editor, kSciMarkerDefine, marker, 27 /* SC_MARK_LEFTRECT */);
            SendMessageW(
                editor, kSciMarkerSetBack, marker, dark ? RGB(112, 162, 226) : RGB(53, 108, 183));
        }
        SendMessageW(editor, kSciSetCaretFore, theme::kTextColor, 0);
        // 搜索时键盘焦点在查找框，仍需保留清晰的正文选区。用不透明底层配色，
        // 不再把高亮半透明地盖在字形上，也不让系统失焦灰色吞掉选区。
        SendMessageW(editor, kSciSetSelectionLayer, 0 /* SC_LAYER_BASE */, 0);
        const auto selection_text = dark ? RGB(255, 255, 255) : RGB(20, 35, 54);
        const auto selection_back = dark ? RGB(48, 88, 143) : RGB(185, 213, 249);
        const auto inactive_back = dark ? RGB(48, 76, 112) : RGB(202, 220, 244);
        for (const int element : {10, 12, 16, 18})
            SendMessageW(editor, kSciSetElementColour, element, selection_text | 0xff000000U);
        for (const int element : {11, 13})
            SendMessageW(editor, kSciSetElementColour, element, selection_back | 0xff000000U);
        for (const int element : {17, 19})
            SendMessageW(editor, kSciSetElementColour, element, inactive_back | 0xff000000U);
        SendMessageW(editor, kSciSetCaretLineVisible, TRUE, 0);
        SendMessageW(editor, kSciSetCaretLineBack, dark ? RGB(43, 45, 49) : RGB(237, 240, 244), 0);
        // 当前行底色也放到文字下层，不能再用半透明覆盖层压暗选区及选中文字。
        SendMessageW(editor, kSciSetCaretLineLayer, 0 /* SC_LAYER_BASE */, 0);
        SendMessageW(editor, kSciSetMarginLeft, 0, detail::ScaleForDpi(4, detail::WindowDpi(hwnd)));
        SendMessageW(
            editor, kSciSetMarginRight, 0, detail::ScaleForDpi(8, detail::WindowDpi(hwnd)));
        SendMessageW(
            editor, kSciSetExtraAscent, detail::ScaleForDpi(1, detail::WindowDpi(hwnd)), 0);
        SendMessageW(
            editor, kSciSetExtraDescent, detail::ScaleForDpi(1, detail::WindowDpi(hwnd)), 0);
        if (!active_id) {
            SendMessageW(editor, kSciSetScrollWidth, 1, 0);
        }
        SendMessageW(editor, kSciSetScrollWidthTracking, TRUE, 0);
        SendMessageW(editor, kSciSetHScrollBar, FALSE, 0);
        SendMessageW(editor, kSciSetVScrollBar, FALSE, 0);
        UpdateLineNumberMargin();
        UpdateHorizontalScrollbar();
    }

    void UpdateLineNumberMargin() const {
        if (!editor)
            return;
        const auto line_count = std::max<LRESULT>(1, SendMessageW(editor, kSciGetLineCount, 0, 0));
        const std::size_t digits = std::max<std::size_t>(2, std::to_string(line_count).size());
        const std::string sample(digits, '9');
        const int text_width = static_cast<int>(SendMessageA(
            editor, kSciTextWidth, kStyleLineNumber, reinterpret_cast<LPARAM>(sample.c_str())));
        SendMessageW(editor, kSciSetMarginWidthN, 0, std::max(24, text_width + 8));
    }

    void UpdateHorizontalScrollbar() const {
        if (!editor || !IsWindow(editor))
            return;
        RECT client{};
        GetClientRect(editor, &client);
        const int margin_width = static_cast<int>(SendMessageW(editor, kSciGetMarginWidthN, 0, 0));
        const int viewport_width =
            std::max(1, static_cast<int>(client.right - client.left) - margin_width);
        const int content_width = static_cast<int>(SendMessageW(editor, kSciGetScrollWidth, 0, 0));
        (void)content_width;
        (void)viewport_width;
        UpdateEditorScrollbarWindows();
    }

    void ScheduleHorizontalScrollbarUpdate() {
        if (!hwnd || horizontal_scroll_update_pending)
            return;
        horizontal_scroll_update_pending =
            PostMessageW(hwnd, kUpdateHorizontalScrollbarMessage, 0, 0) != FALSE;
    }

    void ConfigureLexer(const DocumentState& document) const {
        if (!lexilla_module || document.kind != EditorDocumentKind::Text) {
            SendMessageW(editor, kSciSetILexer, 0, 0);
            return;
        }
        const char* lexer_name = LexerNameFor(document.title);
        if (!lexer_name) {
            SendMessageW(editor, kSciSetILexer, 0, 0);
            return;
        }
        using CreateLexerFn = void*(__cdecl*)(const char*);
        const auto create_lexer =
            reinterpret_cast<CreateLexerFn>(GetProcAddress(lexilla_module, "CreateLexer"));
        if (!create_lexer) {
            return;
        }
        void* lexer = create_lexer(lexer_name);
        SendMessageW(editor, kSciSetILexer, 0, reinterpret_cast<LPARAM>(lexer));
        SendMessageW(editor, kSciColourise, 0, -1);
    }

    std::string EditorText() const {
        if (!scintilla_available || !editor) {
            return {};
        }
        const LRESULT raw_length = SendMessageW(editor, kSciGetTextLength, 0, 0);
        if (raw_length <= 0) {
            return {};
        }
        const auto length = static_cast<std::size_t>(raw_length);
        if (length >= static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
            return {};
        }
        std::string result(length + 1U, '\0');
        const LRESULT copied =
            SendMessageA(editor, kSciGetText, length + 1U, reinterpret_cast<LPARAM>(result.data()));
        if (copied <= 0) {
            return {};
        }
        // SCI_GETTEXTLENGTH is authoritative and excludes the terminator.
        result.resize(length);
        return result;
    }

    void SyncActiveContent() {
        auto* document = ActiveDocument();
        if (!document || document->kind != EditorDocumentKind::Text || !scintilla_available) {
            return;
        }
        document->content = EditorText();
        document->changed_lines.clear();
        for (LRESULT line = SendMessageW(editor, kSciMarkerNext, 0, kChangeMarkerMask); line >= 0;
             line = SendMessageW(editor, kSciMarkerNext, line + 1, kChangeMarkerMask))
            document->changed_lines.push_back(static_cast<int>(line));
    }

    void MarkTouchedLine(DocumentState& document, LRESULT line) {
        // 防止同一行每次输入都分配一个 marker handle；大量修改的完整标记由原生
        // change-history 绘制，跨标签锚点另设预算，避免额外内存随编辑次数无界增长。
        if (document.touched_mark_count < 16384 &&
            (SendMessageW(editor, 2046 /* SCI_MARKERGET */, line, 0) &
             (1U << kRestoredChangeMarker)) == 0) {
            if (SendMessageW(editor, kSciMarkerAdd, line, kRestoredChangeMarker) >= 0)
                ++document.touched_mark_count;
        }
    }

    void UpdateStatus() const {
        const auto* document = ActiveDocument();
        if (!document) {
            SetWindowTextW(status_bar, L"未打开文件");
            SetWindowTextW(status_details, L"");
            return;
        }
        std::wstring status;
        if (document->kind == EditorDocumentKind::Binary) {
            status = L"二进制 • 十六进制只读预览";
            if (document->preview_truncated) {
                status.append(L" • 前 256 KB");
            }
        } else if (document->kind == EditorDocumentKind::ReadOnlyLarge) {
            status = EncodingLabel(document->encoding);
            status.append(L" • ");
            status.append(EolLabel(document->eol));
            status.append(L" • 大文件只读（超过 20 MB）");
        } else {
            if (scintilla_available && editor && IsWindowVisible(editor)) {
                const LRESULT position = SendMessageW(editor, kSciGetCurrentPos, 0, 0);
                const LRESULT line = SendMessageW(editor, kSciLineFromPosition, position, 0);
                const LRESULT column = SendMessageW(editor, kSciGetColumn, position, 0);
                status.append(L"行 ");
                status.append(std::to_wstring(line + 1));
                status.append(L"，列 ");
                status.append(std::to_wstring(column + 1));
            }
            if (!status.empty())
                status += L" · ";
            status += EolLabel(document->eol);
            if (document->read_only)
                status += L" · 只读";
            else if (document->dirty)
                status += L" · 已修改";
            if (document->read_only || document->encoding == EditorTextEncoding::Utf8Bom ||
                document->encoding == EditorTextEncoding::Legacy8Bit)
                status += L" · " + EncodingLabel(document->encoding);
        }
        if (document->closing) {
            status.append(L" • 正在保留恢复缓存…");
        } else if (document->saving) {
            status.append(L" • 正在保存…");
        }
        const std::wstring location = document->server_name.empty()
                                          ? document->title
                                          : document->server_name + L" · " + document->title;
        SetWindowTextW(status_bar, location.c_str());
        SetWindowTextW(status_details, status.c_str());
    }

    void UpdateConflictBar() {
        const auto* document = ActiveDocument();
        conflict_visible = document && !document->conflict.empty();
        RECT rect{};
        GetClientRect(hwnd, &rect);
        Layout(rect.right, rect.bottom);
        InvalidateRect(hwnd, nullptr, FALSE);
    }

    void LoadActiveDocument() {
        pending_wrap_viewport.reset();
        find_bar_top.reset();
        last_search_document.reset();
        find_bar.Status(L"");
        if (!ActiveDocument() && find_bar.visible())
            find_bar.Hide(false);
        auto* document = ActiveDocument();
        find_bar.EnableRegex(document && document->encoding != EditorTextEncoding::Legacy8Bit);
        if (encoding_selector) {
            const bool editable =
                document && document->kind == EditorDocumentKind::Text && !document->read_only;
            ShowWindow(encoding_selector, editable ? SW_SHOWNOACTIVATE : SW_HIDE);
            EnableWindow(encoding_selector, editable && !document->saving && !document->closing);
            if (editable) {
                SendMessageW(encoding_selector,
                             CB_SETCURSEL,
                             document->encoding == EditorTextEncoding::Gb2312
                                 ? 1
                                 : (document->encoding == EditorTextEncoding::Legacy8Bit
                                        ? static_cast<WPARAM>(-1)
                                        : 0),
                             0);
            }
        }
        loading_document = true;
        if (!document) {
            if (scintilla_available) {
                ShowWindow(preview, SW_HIDE);
                ShowWindow(editor, SW_SHOW);
                SendMessageW(editor, kSciSetChangeHistory, 0, 0);
                SendMessageW(editor, kSciSetReadOnly, FALSE, 0);
                SendMessageA(editor, kSciSetText, 0, reinterpret_cast<LPARAM>(""));
                SendMessageW(editor, kSciSetScrollWidth, 1, 0);
                SendMessageW(editor, kSciSetReadOnly, TRUE, 0);
                SendMessageW(editor, kSciSetHScrollBar, FALSE, 0);
                SendMessageW(editor, kSciSetVScrollBar, FALSE, 0);
                ShowWindow(editor, SW_HIDE);
            } else {
                ShowWindow(editor, SW_HIDE);
                ShowWindow(preview, SW_SHOW);
                SetWindowTextW(preview, L"");
                ShowWindow(preview, SW_HIDE);
            }
        } else {
            if (scintilla_available) {
                ShowWindow(preview, SW_HIDE);
                ShowWindow(editor, SW_SHOW);
                SendMessageW(editor, kSciSetReadOnly, FALSE, 0);
                if (document->kind == EditorDocumentKind::Text) {
                    SendMessageW(editor,
                                 kSciSetCodePage,
                                 document->encoding == EditorTextEncoding::Legacy8Bit ? 0
                                                                                      : kScCpUtf8,
                                 0);
                    SendMessageW(editor, kSciSetEolMode, ScintillaEol(document->eol), 0);
                    SendMessageA(editor,
                                 kSciSetText,
                                 0,
                                 reinterpret_cast<LPARAM>(document->content.c_str()));
                    SendMessageW(
                        editor, kSciSetScrollWidth, EstimatedTextWidth(document->content), 0);
                } else {
                    std::wstring preview_text = document->preview;
                    if (document->kind == EditorDocumentKind::ReadOnlyLarge &&
                        preview_text.empty()) {
                        preview_text = L"此文件超过 20 MB，已按只读模式打开。\r\n"
                                       L"为控制内存占用，不会把完整内容载入编辑器。";
                    } else if (document->kind == EditorDocumentKind::Binary &&
                               preview_text.empty()) {
                        preview_text = L"检测到二进制内容，当前没有可显示的探测数据。";
                    }
                    const std::string preview_utf8 = WideToUtf8(preview_text);
                    SendMessageW(editor, kSciSetCodePage, kScCpUtf8, 0);
                    SendMessageA(
                        editor, kSciSetText, 0, reinterpret_cast<LPARAM>(preview_utf8.c_str()));
                    SendMessageW(editor, kSciSetScrollWidth, EstimatedTextWidth(preview_utf8), 0);
                }
                UpdateLineNumberMargin();
                ScheduleHorizontalScrollbarUpdate();
                ConfigureLexer(*document);
                SendMessageW(editor, 2075 /* SCI_EMPTYUNDOBUFFER */, 0, 0);
                SendMessageW(editor, kSciSetSavePoint, 0, 0);
                document->trusted_savepoint = !document->dirty;
                if (!document->dirty)
                    document->saved_encoding = document->encoding;
                SendMessageW(editor,
                             kSciSetChangeHistory,
                             document->kind == EditorDocumentKind::Text && !document->read_only ? 3
                                                                                                : 0,
                             0);
                SendMessageW(editor, kSciMarkerDeleteAll, kRestoredChangeMarker, 0);
                document->touched_mark_count = document->changed_lines.size();
                for (const int line : document->changed_lines)
                    SendMessageW(editor, kSciMarkerAdd, line, kRestoredChangeMarker);
                SendMessageW(editor,
                             kSciSetReadOnly,
                             document->kind == EditorDocumentKind::Text && !document->read_only &&
                                     !document->closing
                                 ? FALSE
                                 : TRUE,
                             0);
                // Scintilla owns the active text buffer. Keeping the same
                // bytes in DocumentState can double memory for files near the
                // 20 MB editable limit; rebuild the string only when saving,
                // switching tabs, or explicitly requesting the text.
                if (document->kind == EditorDocumentKind::Text) {
                    std::string{}.swap(document->content);
                }
            } else {
                ShowWindow(editor, SW_HIDE);
                ShowWindow(preview, SW_SHOW);
                std::wstring text = document->preview;
                if (document->kind == EditorDocumentKind::ReadOnlyLarge && text.empty()) {
                    text = L"此文件超过 20 MB，已按只读模式打开。\r\n"
                           L"为控制内存占用，不会把完整内容载入编辑器。";
                } else if (document->kind == EditorDocumentKind::Binary && text.empty()) {
                    text = L"检测到二进制内容，文件未载入 Scintilla。\r\n"
                           L"当前没有可显示的探测数据。";
                }
                SetWindowTextW(preview, text.c_str());
            }
        }
        loading_document = false;
        UpdateStatus();
        UpdateConflictBar();
    }

    EditorSaveRequest BuildSaveRequest(DocumentState& document) {
        const bool active = active_id && *active_id == document.id;
        if (active) {
            SyncActiveContent();
        }
        return {document.id,
                active ? std::move(document.content) : document.content,
                document.encoding,
                document.eol};
    }

    void RequestSave() {
        auto* document = ActiveDocument();
        if (!document || document->kind != EditorDocumentKind::Text || document->read_only ||
            document->saving || document->closing) {
            return;
        }
        InvokeSafely(save_requested, BuildSaveRequest(*document));
    }

    void RequestCloseActive() {
        auto* document = ActiveDocument();
        if (!document) {
            return;
        }
        RequestCloseDocument(document->id);
    }

    void RequestCloseDocument(EditorDocumentId id) {
        auto* document = FindDocument(id);
        if (!document || document->closing) {
            return;
        }
        if (close_requested) {
            if (active_id && *active_id == id) {
                SyncActiveContent();
            }
            InvokeSafely(close_requested, document->id, document->dirty);
        } else {
            CloseDocument(document->id);
        }
    }

    bool CloseDocument(EditorDocumentId id) {
        const int index = DocumentIndex(id);
        if (index < 0) {
            return false;
        }
        const bool closing_active = active_id && *active_id == id;
        if (closing_active) {
            SyncActiveContent();
        }
        documents.erase(documents.begin() + index);
        if (documents.empty()) {
            active_id.reset();
            LoadActiveDocument();
            return true;
        }
        if (!closing_active && active_id && FindDocument(*active_id)) {
            // 非活动标签的关闭只改变标签栏。当前文本由 Scintilla 持有，
            // DocumentState::content 此时为空；重新加载会把可见文件清空。
            UpdateConflictBar();
            return true;
        }
        const int next_index = (std::min)(index, static_cast<int>(documents.size()) - 1);
        active_id = documents[static_cast<std::size_t>(next_index)].id;
        LoadActiveDocument();
        InvokeSafely(active_document_changed, *active_id);
        return true;
    }

    bool ActivateDocument(EditorDocumentId id) {
        const int index = DocumentIndex(id);
        if (index < 0) {
            return false;
        }
        if (active_id && *active_id == id) {
            return true;
        }
        SyncActiveContent();
        active_id = id;
        LoadActiveDocument();
        InvokeSafely(active_document_changed, id);
        return true;
    }

    bool HandleCommand(int id, int notification) {
        if (notification != BN_CLICKED) {
            return false;
        }
        switch (id) {
        case kChangesId:
            if (const auto* document = ActiveDocument();
                document && document->dirty && document->kind == EditorDocumentKind::Text &&
                !document->read_only && !document->closing && !document->saving)
                InvokeSafely(changes_requested, document->id);
            return true;
        case kPathRefreshId:
            NavigateFromPath();
            return true;
        case kSaveId:
            RequestSave();
            return true;
        case kDiffId:
            if (const auto* document = ActiveDocument()) {
                InvokeSafely(diff_requested, document->id);
            }
            return true;
        case kOverwriteId:
            if (auto* document = ActiveDocument()) {
                InvokeSafely(overwrite_requested, BuildSaveRequest(*document));
            }
            return true;
        case kSaveAsId:
            if (auto* document = ActiveDocument()) {
                InvokeSafely(save_as_requested, BuildSaveRequest(*document));
            }
            return true;
        case kDismissConflictId:
            if (auto* document = ActiveDocument()) {
                document->conflict.clear();
                UpdateConflictBar();
            }
            return true;
        default:
            return false;
        }
    }

    bool HandleNotify(const NMHDR& header) {
        if (header.hwndFrom == file_list) {
            if (header.code == NM_DBLCLK) {
                ActivateSelectedFile();
                return true;
            }
        }
        if (header.hwndFrom == editor && header.code == kScnUpdateUi) {
            if (batch_replacing)
                return false;
            UpdateEditorScrollbarWindows();
            if (!loading_document)
                UpdateStatus();
            return false;
        }
        if (header.hwndFrom == editor && active_id) {
            auto* document = ActiveDocument();
            if (!document || loading_document) {
                return false;
            }
            if (header.code == kScnModified) {
                last_search_document.reset();
                const auto& notification =
                    reinterpret_cast<const ScintillaNotificationPrefix&>(header);
                if ((notification.modification_type & (kScModInsertText | kScModDeleteText)) == 0) {
                    return false;
                }
                document->dirty = true;
                if (batch_replacing)
                    return false;
                // 原生 change-history 标记按绘制实时生成，不属于 MARKERNEXT 的集合。
                // 另存少量真实“触及行”锚点供标签切换恢复；精确改动仍以原版本对比为准。
                const auto first =
                    SendMessageW(editor, kSciLineFromPosition, notification.position, 0);
                const auto added = std::clamp<std::intptr_t>(notification.lines_added, 0, 1024);
                for (std::intptr_t offset = 0; offset <= added; ++offset)
                    MarkTouchedLine(*document, first + offset);
                UpdateLineNumberMargin();
                UpdateScrollWidthForCurrentLine();
                // SCI_SETSCROLLWIDTHTRACKING updates the measured width after
                // SCN_MODIFIED returns. Query it on the next message turn so a
                // newly lengthened line can reveal the horizontal scrollbar.
                ScheduleHorizontalScrollbarUpdate();
                InvalidateRect(hwnd, nullptr, FALSE);
                UpdateStatus();
                InvokeSafely(document_changed, document->id);
                return false;
            }
            if (header.code == kScnSavePointLeft) {
                if (batch_replacing)
                    return false;
                document->dirty = true;
                UpdateStatus();
                return false;
            }
            if (header.code == kScnSavePointReached && !document->saving) {
                if (batch_replacing)
                    return false;
                if (document->trusted_savepoint && document->encoding == document->saved_encoding) {
                    document->dirty = false;
                    document->changed_lines.clear();
                    document->touched_mark_count = 0;
                    SendMessageW(editor, kSciMarkerDeleteAll, kRestoredChangeMarker, 0);
                    InvokeSafely(document_changed, document->id);
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                UpdateStatus();
                return false;
            }
        }
        return false;
    }

    std::wstring FileCellText(const std::size_t row, const std::size_t column) const {
        if (row >= file_entries.size() || column != 0)
            return {};
        const EditorFileEntry& entry = file_entries[row];
        if (entry.placeholder && !entry.status.empty()) {
            return entry.name + L" · " + entry.status;
        }
        return entry.local_shortcut ? local_shortcut::DisplayName(entry.name) : entry.name;
    }

    int FileIcon(const std::size_t row) const {
        if (row >= file_entries.size())
            return file_icon;
        const EditorFileEntry& entry = file_entries[row];
        if (entry.placeholder)
            return -1;
        if (entry.directory)
            return folder_icon;
        if (!entry.icon_path.empty()) {
            return file_icons::IconForPath(entry.icon_path);
        }
        return file_icons::IconForFileName(entry.name);
    }

    void ActivateSelectedFile() {
        const int selected = table::SelectedRow(file_list);
        if (selected < 0)
            return;
        const auto index = static_cast<std::size_t>(selected);
        if (index >= file_entries.size()) {
            return;
        }
        if (file_entries[index].placeholder) {
            if (directory_state.action != ViewRecoveryAction::None) {
                InvokeSafely(view_action, directory_state.action);
            }
            return;
        }
        EditorFileActivation activation;
        activation.server_id = SelectedServerId();
        activation.directory = WindowText(path_edit);
        activation.entry = file_entries[index];
        InvokeSafely(file_activation, std::move(activation));
    }

    std::string SelectedServerId() const {
        return selected_server_id;
    }

    ResizeMode ResizeHandleAt(POINT point) const noexcept {
        const int hit = detail::ScaleForDpi(5, detail::WindowDpi(hwnd));
        if (std::abs(point.x - left_splitter_x) <= hit) {
            return ResizeMode::file_sidebar;
        }
        if (conflict_visible && conflict_splitter_y > 0 &&
            std::abs(point.y - conflict_splitter_y) <= hit) {
            return ResizeMode::conflict;
        }
        return ResizeMode::none;
    }

    bool BeginResize(POINT point) {
        const ResizeMode mode = ResizeHandleAt(point);
        if (mode == ResizeMode::none) {
            return false;
        }
        resize_mode = mode;
        resize_origin = point;
        if (mode == ResizeMode::file_sidebar) {
            resize_start = left_width_override;
        } else {
            resize_start = conflict_height_override;
        }
        resize_preview_value = resize_start;
        resize_preview_position =
            mode == ResizeMode::file_sidebar ? left_splitter_x : conflict_splitter_y;
        SetCapture(hwnd);
        SetResizeCursor();
        ToggleResizeGuide();
        return true;
    }

    void ToggleResizeGuide() const {
        if (!resize_guide)
            return;
        if (resize_mode == ResizeMode::none || resize_preview_position < 0) {
            ShowWindow(resize_guide, SW_HIDE);
            return;
        }
        RECT client{};
        GetClientRect(hwnd, &client);
        const UINT dpi = detail::WindowDpi(hwnd);
        const int thickness = std::max(1, detail::ScaleForDpi(2, dpi));
        const int gap = detail::ScaleForDpi(2, dpi);
        int x{};
        int y{};
        int width{};
        int height{};
        if (resize_mode == ResizeMode::file_sidebar) {
            x = resize_preview_position - thickness / 2;
            y = 0;
            width = thickness;
            height = std::max(1L, client.bottom);
        } else {
            x = left_splitter_x + gap / 2;
            y = resize_preview_position - thickness / 2;
            width = std::max(1L, client.right - x);
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
        const UINT dpi = detail::WindowDpi(hwnd);
        if (resize_mode == ResizeMode::file_sidebar) {
            const int available = std::max(0, static_cast<int>(client.right));
            const int minimum =
                std::min(detail::ScaleForDpi(theme::kServerSidebarWidth, dpi), available / 2);
            const int maximum = std::max(minimum, available / 2);
            const int next_value = std::clamp(
                resize_start + static_cast<int>(point.x - resize_origin.x), minimum, maximum);
            const int gap = detail::ScaleForDpi(2, dpi);
            const int next_position = next_value + gap / 2;
            if (next_position != resize_preview_position) {
                resize_preview_value = next_value;
                resize_preview_position = next_position;
                ToggleResizeGuide();
            }
        } else if (resize_mode == ResizeMode::conflict) {
            const int next_value = std::clamp(
                resize_start - static_cast<int>(point.y - resize_origin.y),
                detail::ScaleForDpi(60, dpi),
                std::max(detail::ScaleForDpi(60, dpi), static_cast<int>(client.bottom / 2)));
            const int next_position =
                static_cast<int>(client.bottom) - next_value - detail::ScaleForDpi(11, dpi);
            if (next_position != resize_preview_position) {
                resize_preview_value = next_value;
                resize_preview_position = next_position;
                ToggleResizeGuide();
            }
        }
    }

    void EndResize() {
        const ResizeMode completed_mode = resize_mode;
        ShowWindow(resize_guide, SW_HIDE);
        if (completed_mode == ResizeMode::file_sidebar) {
            left_width_override = resize_preview_value;
        } else if (completed_mode == ResizeMode::conflict) {
            conflict_height_override = resize_preview_value;
        }
        resize_preview_value = 0;
        resize_preview_position = -1;
        resize_mode = ResizeMode::none;
        RECT client{};
        GetClientRect(hwnd, &client);
        Layout(client.right, client.bottom);
        RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_NOERASE);
        if (GetCapture() == hwnd) {
            ReleaseCapture();
        }
    }

    bool SetResizeCursor() const {
        POINT point{};
        GetCursorPos(&point);
        ScreenToClient(hwnd, &point);
        const ResizeMode mode = ResizeHandleAt(point);
        if (resize_mode == ResizeMode::file_sidebar || mode == ResizeMode::file_sidebar) {
            SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
            return true;
        }
        if (resize_mode == ResizeMode::conflict || mode == ResizeMode::conflict) {
            SetCursor(LoadCursorW(nullptr, IDC_SIZENS));
            return true;
        }
        return false;
    }

    struct PageMetrics {
        int pad{}, gap{}, row{}, button{}, left_width{}, right_x{}, right_width{};
        int header_height{}, path_height{}, path_top{}, editor_y{}, editor_height{};
        int status_y{}, status_height{}, conflict_height{}, conflict_y{};
        RECT save{}, changes{}, owner{}, details{}, encoding{};
    };

    PageMetrics Measure(const int width, const int height) {
        const auto dpi = detail::WindowDpi(hwnd);
        const auto px = [dpi](int value) {
            return detail::ScaleForDpi(value, dpi);
        };
        PageMetrics m;
        m.pad = px(10);
        m.gap = px(6);
        m.row = px(26);
        m.button = px(64);
        const int min_left = std::min(px(theme::kServerSidebarWidth), std::max(0, width / 2));
        const int max_left = std::max(min_left, width / 2);
        m.left_width =
            std::clamp(left_width_override > 0 ? left_width_override : px(232), min_left, max_left);
        left_width_override = m.left_width;
        m.right_x = m.left_width + px(2);
        m.right_width = std::max(0, width - m.right_x - px(8));
        m.header_height = px(34);
        m.path_height = px(28);
        m.path_top = px(3);
        m.editor_y = std::min(height, m.header_height);
        if (conflict_visible && conflict_height_override == 0)
            conflict_height_override = px(84);
        m.conflict_height =
            conflict_visible
                ? std::clamp(conflict_height_override > 0 ? conflict_height_override : px(84),
                             px(60),
                             std::max(px(60), height / 2))
                : 0;
        m.status_height = std::min(m.row, std::max(0, height - m.editor_y));
        m.status_y = std::max(m.editor_y,
                              height - m.status_height - m.conflict_height -
                                  (conflict_visible ? px(14) : 0));
        m.editor_height = std::max(0, m.status_y - m.editor_y);
        m.conflict_y = m.status_y + m.status_height + px(6);
        const int right = m.right_x + m.right_width;
        m.save = {std::max(m.right_x, right - m.button), px(3), right, px(31)};
        m.changes = {std::max<LONG>(m.right_x, m.save.left - px(60)),
                     px(3),
                     std::max<LONG>(m.right_x, m.save.left - px(4)),
                     px(31)};
        const auto* document = ActiveDocument();
        const bool editable =
            document && document->kind == EditorDocumentKind::Text && !document->read_only;
        const int encoding_width = editable ? std::min(px(88), m.right_width) : 0;
        m.encoding = {right - encoding_width,
                      m.status_y + px(2),
                      right,
                      m.status_y + m.status_height - px(2)};
        const int metadata_width =
            m.right_width >= px(620)
                ? px(240)
                : (m.right_width >= px(440) ? px(170) : (m.right_width >= px(320) ? px(100) : 0));
        const int metadata_right = right - encoding_width - (encoding_width ? px(8) : 0);
        m.details = {std::max(m.right_x, metadata_right - metadata_width),
                     m.status_y,
                     metadata_right,
                     m.status_y + m.status_height};
        m.owner = {m.right_x + std::min(px(8), m.right_width),
                   m.status_y,
                   std::max<LONG>(m.right_x, m.details.left - px(8)),
                   m.status_y + m.status_height};
        if (m.owner.right < m.owner.left)
            m.owner.right = m.owner.left;
        return m;
    }

    void HandleSurfaceClick(int id) {
        if (id == kTabPreviousId || id == kTabNextId) {
            if (id == kTabPreviousId && first_tab > 0)
                --first_tab;
            if (id == kTabNextId && first_tab + visible_tabs < documents.size())
                ++first_tab;
            InvalidateTabStrip();
            if (ActiveDocument() && editor && IsWindowVisible(editor))
                SetFocus(editor);
            return;
        }
        if (id >= kDocumentCloseFirstId &&
            id < kDocumentCloseFirstId + static_cast<int>(documents.size())) {
            RequestCloseDocument(
                documents[static_cast<std::size_t>(id - kDocumentCloseFirstId)].id);
            return;
        }
        if (id >= kDocumentTabFirstId &&
            id < kDocumentTabFirstId + static_cast<int>(documents.size())) {
            ActivateDocument(documents[static_cast<std::size_t>(id - kDocumentTabFirstId)].id);
            return;
        }
        HandleCommand(id, BN_CLICKED);
    }

    void BuildSurface(int width, int height) {
        surface.ClearElements();
        if (width <= 0 || height <= 0) {
            return;
        }
        const UINT dpi = detail::WindowDpi(hwnd);
        const float scale = static_cast<float>(dpi) / 96.0F;
        const auto m = Measure(width, height);
        const int pad = m.pad, gap = m.gap, row = m.row;
        const int left_width = m.left_width;
        const int file_left = 0;
        const int right_x = m.right_x, right_width = m.right_width;

        surface.Add({
            .role = detail::UiRole::panel,
            .bounds = detail::UiRect(static_cast<float>(file_left),
                                     0.0F,
                                     static_cast<float>(file_left + left_width),
                                     static_cast<float>(height)),
        });
        surface.Add({
            .role = detail::UiRole::separator,
            .bounds = detail::UiRect(static_cast<float>(file_left + left_width),
                                     0.0F,
                                     static_cast<float>(file_left + left_width + 1),
                                     static_cast<float>(height)),
        });
        const float refresh_width = static_cast<float>(detail::ScaleForDpi(26, dpi));
        surface.Add({
            .id = kPathRefreshId,
            .role = detail::UiRole::icon_button,
            .bounds =
                detail::UiRect(static_cast<float>(file_left + left_width - pad) - refresh_width,
                               static_cast<float>(m.path_top),
                               static_cast<float>(file_left + left_width - pad),
                               static_cast<float>(m.path_top + m.path_height)),
            .text = detail::kRefreshGlyph,
            .alignment = detail::UiTextAlign::center,
            .enabled = !selected_server_id.empty(),
        });
        // Match the terminal page: the tab strip starts at the top edge and
        // the active document surface joins it without an extra gap.
        const int tab_y = 0;
        const float strip_height = 34.0F * scale;
        const int tab_room =
            std::max(1, static_cast<int>(m.changes.left) - right_x - detail::ScaleForDpi(12, dpi));
        const int arrow = std::min(detail::ScaleForDpi(20, dpi), tab_room / 4);
        auto viewport = ComputeTerminalTabViewport(
            tab_room,
            detail::ScaleForDpi(90, dpi),
            detail::ScaleForDpi(170, dpi),
            detail::ScaleForDpi(2, dpi),
            arrow * 2,
            m.save.left - right_x > detail::ScaleForDpi(18, dpi) ? documents.size() : 0,
            first_tab);
        if (previous_tab_active != active_id || previous_tab_count != documents.size() ||
            previous_tab_room != tab_room) {
            if (const auto found =
                    std::ranges::find(documents, active_id.value_or(0), &DocumentState::id);
                found != documents.end())
                viewport.Reveal(static_cast<std::size_t>(std::distance(documents.begin(), found)),
                                documents.size());
        }
        first_tab = viewport.first;
        visible_tabs = viewport.count;
        previous_tab_active = active_id;
        previous_tab_count = documents.size();
        previous_tab_room = tab_room;
        const float tab_width = static_cast<float>(viewport.tab_width);
        surface.Add({
            .role = detail::UiRole::panel,
            .bounds = detail::UiRect(static_cast<float>(right_x),
                                     static_cast<float>(tab_y),
                                     static_cast<float>(right_x + right_width),
                                     static_cast<float>(tab_y) + strip_height),
        });
        float tab_x = static_cast<float>(right_x) + 5.0F * scale;
        for (std::size_t index = viewport.first; index < viewport.first + viewport.count; ++index) {
            const auto& document = documents[index];
            std::wstring label = document.title.empty() ? L"未命名" : document.title;
            if (document.saving) {
                label.insert(0, L"◌  ");
            } else if (document.dirty) {
                label.insert(0, L"●  ");
            }
            const float right = tab_x + tab_width;
            surface.Add({
                .id = kDocumentTabFirstId + static_cast<int>(index),
                .role = detail::UiRole::file_tab,
                .bounds = detail::UiRect(tab_x,
                                         static_cast<float>(tab_y) + 3.0F * scale,
                                         right,
                                         static_cast<float>(tab_y) + strip_height - 1.0F * scale),
                .text = std::move(label),
                .alignment = detail::UiTextAlign::leading,
                .selected = active_id && *active_id == document.id,
                .compact_text = true,
                .text_right_inset = 20.0F * scale,
            });
            const float close_width = 18.0F * scale;
            const bool show_close =
                (active_id && *active_id == document.id) ||
                surface.hovered_id() == kDocumentTabFirstId + static_cast<int>(index) ||
                surface.hovered_id() == kDocumentCloseFirstId + static_cast<int>(index);
            surface.Add({
                .id = kDocumentCloseFirstId + static_cast<int>(index),
                .role = detail::UiRole::tab_close,
                .bounds =
                    detail::UiRect(right - close_width - 5.0F * scale,
                                   static_cast<float>(tab_y) + (strip_height - close_width) / 2.0F,
                                   right - 5.0F * scale,
                                   static_cast<float>(tab_y) + (strip_height + close_width) / 2.0F),
                .icon = detail::UiIcon::close,
                .alignment = detail::UiTextAlign::center,
                .visible = show_close,
            });
            tab_x = right + 2.0F * scale;
        }
        if (viewport.overflow) {
            const float start = static_cast<float>(right_x + tab_room - arrow * 2) + 5.0F * scale;
            surface.Add(
                {.id = kTabPreviousId,
                 .role = detail::UiRole::tab_close,
                 .bounds = detail::UiRect(start, 5.0F * scale, start + arrow, 29.0F * scale),
                 .icon = detail::UiIcon::chevron_left,
                 .alignment = detail::UiTextAlign::center,
                 .enabled = viewport.first > 0});
            surface.Add({.id = kTabNextId,
                         .role = detail::UiRole::tab_close,
                         .bounds = detail::UiRect(
                             start + arrow, 5.0F * scale, start + arrow * 2, 29.0F * scale),
                         .icon = detail::UiIcon::chevron,
                         .alignment = detail::UiTextAlign::center,
                         .enabled = viewport.first + viewport.count < documents.size()});
        }

        const int status_height = m.status_height;
        surface.Add({.role = detail::UiRole::separator,
                     .bounds = detail::UiRect(static_cast<float>(right_x),
                                              static_cast<float>(m.status_y),
                                              static_cast<float>(right_x + right_width),
                                              static_cast<float>(m.status_y + 1))});
        const auto* current = ActiveDocument();
        surface.Add({.id = kChangesId,
                     .role = detail::UiRole::ghost_button,
                     .bounds = detail::UiRect(static_cast<float>(m.changes.left),
                                              static_cast<float>(m.changes.top),
                                              static_cast<float>(m.changes.right),
                                              static_cast<float>(m.changes.bottom)),
                     .text = L"改动",
                     .alignment = detail::UiTextAlign::center,
                     .enabled = current && current->dirty && !current->read_only &&
                                !current->closing && !current->saving});
        surface.Add({
            .id = kSaveId,
            .role = current && current->dirty ? detail::UiRole::primary_button
                                              : detail::UiRole::ghost_button,
            .bounds = detail::UiRect(static_cast<float>(m.save.left),
                                     static_cast<float>(m.save.top),
                                     static_cast<float>(m.save.right),
                                     static_cast<float>(m.save.bottom)),
            .text = current && current->saving ? L"保存中" : L"保存",
            .alignment = detail::UiTextAlign::center,
            .enabled = ActiveDocument() && ActiveDocument()->kind == EditorDocumentKind::Text &&
                       !ActiveDocument()->read_only && !ActiveDocument()->saving &&
                       !ActiveDocument()->closing,
        });
        if (!current && !scintilla_available) {
            const float middle = static_cast<float>(m.editor_y + m.editor_height / 2);
            surface.Add({.role = detail::UiRole::heading,
                         .bounds = detail::UiRect(static_cast<float>(right_x),
                                                  middle - 42.0F * scale,
                                                  static_cast<float>(right_x + right_width),
                                                  middle),
                         .text = L"编辑器组件未加载",
                         .alignment = detail::UiTextAlign::center});
            surface.Add({.role = detail::UiRole::muted,
                         .bounds = detail::UiRect(static_cast<float>(right_x),
                                                  middle + 5.0F * scale,
                                                  static_cast<float>(right_x + right_width),
                                                  middle + 38.0F * scale),
                         .text = L"请检查 Scintilla.dll 和 Lexilla.dll",
                         .alignment = detail::UiTextAlign::center});
        }

        if (conflict_visible) {
            const int visible_conflict_height = m.conflict_height;
            const int conflict_y = m.conflict_y;
            if (conflict_splitter_y > 0) {
                surface.Add({
                    .role = detail::UiRole::separator,
                    .bounds = detail::UiRect(static_cast<float>(right_x),
                                             static_cast<float>(conflict_splitter_y),
                                             static_cast<float>(right_x + right_width),
                                             static_cast<float>(conflict_splitter_y + 2)),
                });
            }
            surface.Add({
                .role = detail::UiRole::card,
                .bounds = detail::UiRect(static_cast<float>(right_x),
                                         static_cast<float>(conflict_y),
                                         static_cast<float>(right_x + right_width),
                                         static_cast<float>(conflict_y + visible_conflict_height)),
            });
            const auto* document = ActiveDocument();
            const bool stacked = right_width < detail::ScaleForDpi(640, dpi);
            const int close_width = detail::ScaleForDpi(26, dpi);
            const int action_width =
                stacked ? std::max(1, (right_width - close_width - pad * 2 - gap * 3) / 3)
                        : detail::ScaleForDpi(82, dpi);
            const int action_y = stacked ? conflict_y + visible_conflict_height - row - gap
                                         : conflict_y + (visible_conflict_height - row) / 2;
            surface.Add({
                .role = detail::UiRole::status,
                .bounds = detail::UiRect(
                    static_cast<float>(right_x + pad),
                    static_cast<float>(conflict_y),
                    static_cast<float>(stacked
                                           ? right_x + right_width - pad
                                           : right_x + right_width - detail::ScaleForDpi(350, dpi)),
                    static_cast<float>(stacked ? action_y : conflict_y + visible_conflict_height)),
                .text = document ? document->conflict : L"远程文件发生变化",
            });
            int action_x = stacked ? right_x + pad
                                   : right_x + right_width -
                                         (action_width * 3 + close_width + gap * 3 + pad * 2);
            static constexpr std::array<int, 3> conflict_ids = {kDiffId, kOverwriteId, kSaveAsId};
            static constexpr std::array<const wchar_t*, 3> conflict_labels = {
                L"查看差异", L"覆盖远端", L"另存为"};
            for (std::size_t index = 0; index < conflict_ids.size(); ++index) {
                surface.Add({
                    .id = conflict_ids[index],
                    .role = detail::UiRole::ghost_button,
                    .bounds = detail::UiRect(static_cast<float>(action_x),
                                             static_cast<float>(action_y),
                                             static_cast<float>(action_x + action_width),
                                             static_cast<float>(action_y + row)),
                    .text = conflict_labels[index],
                    .alignment = detail::UiTextAlign::center,
                });
                action_x += action_width + gap;
            }
            surface.Add({
                .id = kDismissConflictId,
                .role = detail::UiRole::icon_button,
                .bounds =
                    detail::UiRect(static_cast<float>(right_x + right_width - close_width - pad),
                                   static_cast<float>(action_y),
                                   static_cast<float>(right_x + right_width - pad),
                                   static_cast<float>(action_y + row)),
                .icon = detail::UiIcon::close,
                .alignment = detail::UiTextAlign::center,
            });
            (void)status_height;
        }
        StartTabMotionIfNeeded();
    }

    void InvalidateTabStrip() const {
        RECT client{};
        GetClientRect(hwnd, &client);
        const UINT dpi = detail::WindowDpi(hwnd);
        const int right_x = left_splitter_x + detail::ScaleForDpi(2, dpi) / 2;
        const RECT strip{right_x, 0, client.right, detail::ScaleForDpi(35, dpi)};
        InvalidateRect(hwnd, &strip, FALSE);
    }

    void UpdateTabAccent() {
        if (!active_id) {
            surface.SetOverlayAccent(std::nullopt);
            return;
        }
        const auto found = std::ranges::find(documents, *active_id, &DocumentState::id);
        if (found == documents.end()) {
            surface.SetOverlayAccent(std::nullopt);
            return;
        }
        const int tab_id =
            kDocumentTabFirstId + static_cast<int>(std::distance(documents.begin(), found));
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
        if (animated_tab_id != active_id) {
            animated_tab_id = active_id;
            tab_motion_start = GetTickCount64();
            tab_motion_active = active_id.has_value() && tab_motion_enabled &&
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

    void Layout(int width, int height) {
        if (!file_list || width <= 0 || height <= 0) {
            return;
        }
        const UINT dpi = detail::WindowDpi(hwnd);
        const auto m = Measure(width, height);
        const int pad = m.pad, gap = m.gap;
        // 服务器侧栏由 MainWindow 统一创建和布局。EditorPage 收到的客户区已经是
        // 扣除共享侧栏后的页面区域，这里不能再次预留侧栏宽度，否则切换侧栏可见性时
        // 文件列表会被移出客户区，看起来像是“消失”了。
        const int left_width = m.left_width;
        const int file_left = 0;
        const int right_x = m.right_x, right_width = m.right_width;
        left_splitter_x = file_left + left_width + (right_x - left_width) / 2;

        const int path_height = m.path_height;
        const int file_x = file_left + pad;
        const int file_width = std::max(1, left_width - pad - detail::ScaleForDpi(4, dpi));
        const int path_width = std::max(1, left_width - pad * 2 - detail::ScaleForDpi(32, dpi));
        const int path_top = m.path_top;
        MoveWindow(path_frame, file_x, path_top, path_width, path_height, TRUE);
        if (path_frame_region.cx != path_width || path_frame_region.cy != path_height) {
            detail::SetRoundedControlRegion(
                path_frame, path_width, path_height, detail::ScaleForDpi(7, dpi));
            path_frame_region = {path_width, path_height};
        }
        const int path_border = std::max(1, detail::ScaleForDpi(1, dpi));
        const int path_edit_width = std::max(1, path_width - path_border * 2);
        const int path_edit_height = std::max(1, path_height - path_border * 2);
        MoveWindow(path_edit, path_border, path_border, path_edit_width, path_edit_height, TRUE);
        if (path_region.cx != path_edit_width || path_region.cy != path_edit_height) {
            detail::SetRoundedControlRegion(
                path_edit, path_edit_width, path_edit_height, detail::ScaleForDpi(5, dpi));
            path_region = {path_edit_width, path_edit_height};
        }
        const int file_list_y = path_top + path_height + gap;
        MoveWindow(file_list,
                   file_x,
                   file_list_y,
                   file_width,
                   std::max(0, height - file_list_y - pad),
                   TRUE);
        if (file_column_width != file_width) {
            table::SetColumnWidth(file_list, 0, file_width);
            file_column_width = file_width;
        }

        MoveWindow(editor,
                   right_x,
                   m.editor_y,
                   right_width,
                   m.editor_height,
                   pending_wrap_viewport ? FALSE : TRUE);
        UpdateHorizontalScrollbar();
        MoveWindow(preview, right_x, m.editor_y, right_width, m.editor_height, TRUE);
        const auto place = [](HWND control, RECT bounds) {
            MoveWindow(control,
                       bounds.left,
                       bounds.top,
                       std::max(0L, bounds.right - bounds.left),
                       std::max(0L, bounds.bottom - bounds.top),
                       TRUE);
        };
        place(status_bar, m.owner);
        place(status_details, m.details);
        place(encoding_selector, m.encoding);
        PlaceFindBar();

        if (pending_wrap_viewport) {
            RestoreWrapViewport();
            if (pending_wrap_viewport)
                ScheduleHorizontalScrollbarUpdate();
        }

        if (conflict_visible) {
            conflict_splitter_y = m.conflict_y - gap / 2;
        } else {
            conflict_splitter_y = 0;
        }
    }
};

EditorPage::EditorPage() : impl_(std::make_unique<Impl>()) {}

EditorPage::~EditorPage() {
    if (impl_->tooltip && IsWindow(impl_->tooltip))
        DestroyWindow(impl_->tooltip);
    if (impl_->resize_guide && IsWindow(impl_->resize_guide)) {
        DestroyWindow(impl_->resize_guide);
    }
    if (impl_->hwnd && IsWindow(impl_->hwnd)) {
        DestroyWindow(impl_->hwnd);
    }
    if (impl_->lexilla_module) {
        FreeLibrary(impl_->lexilla_module);
    }
    if (impl_->scintilla_module) {
        FreeLibrary(impl_->scintilla_module);
    }
    DeleteObject(impl_->font);
    DeleteObject(impl_->title_font);
    DeleteObject(impl_->mono_font);
    DeleteObject(impl_->caption_font);
}

bool EditorPage::Create(HWND parent, HINSTANCE instance) {
    impl_->instance = instance;
    if (!RegisterPageClass(instance, &Impl::WindowProc) || !RegisterPathFrameClass(instance) ||
        !RegisterResizeGuideClass(instance) || !selector::RegisterClass(instance)) {
        return false;
    }
    impl_->hwnd = CreateWindowExW(0,
                                  kEditorPageClass,
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
    if (!impl_->find_bar.Create(impl_->hwnd, instance, true))
        return false;
    impl_->find_bar.action = [self = impl_.get()](detail::FindBar::Action action) {
        if (action == detail::FindBar::Action::Changed ||
            action == detail::FindBar::Action::Closed) {
            if (action == detail::FindBar::Action::Closed)
                self->find_bar_top.reset();
            self->PlaceFindBar();
            if (action == detail::FindBar::Action::Closed)
                InvalidateRect(self->editor, nullptr, FALSE);
        } else if (action == detail::FindBar::Action::Replace ||
                   action == detail::FindBar::Action::ReplaceAll) {
            self->ReplaceText(self->find_bar.query(),
                              self->find_bar.replacement(),
                              action == detail::FindBar::Action::ReplaceAll,
                              self->find_bar.match_case(),
                              self->find_bar.regex());
        } else {
            self->FindText(self->find_bar.query(),
                           action == detail::FindBar::Action::Previous,
                           self->find_bar.match_case(),
                           self->find_bar.regex());
        }
    };
    impl_->resize_guide =
        CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                        kEditorResizeGuideClass,
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

    const UINT dpi = detail::WindowDpi(parent);
    impl_->font = detail::CreateUiFont(dpi, 10);
    impl_->caption_font = detail::CreateUiFont(dpi, 9);
    impl_->title_font = detail::CreateUiFont(dpi, 11, true);
    impl_->mono_font = CreateFontW(-MulDiv(10, static_cast<int>(dpi), 72),
                                   0,
                                   0,
                                   0,
                                   FW_NORMAL,
                                   FALSE,
                                   FALSE,
                                   FALSE,
                                   DEFAULT_CHARSET,
                                   OUT_DEFAULT_PRECIS,
                                   CLIP_DEFAULT_PRECIS,
                                   CLEARTYPE_QUALITY,
                                   FIXED_PITCH | FF_MODERN,
                                   L"Consolas");

    impl_->path_frame = CreateWindowExW(0,
                                        kEditorPathFrameClass,
                                        L"",
                                        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
                                        0,
                                        0,
                                        0,
                                        0,
                                        impl_->hwnd,
                                        nullptr,
                                        instance,
                                        nullptr);
    impl_->path_edit = CreateWindowExW(0,
                                       L"EDIT",
                                       L"/",
                                       WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                       0,
                                       0,
                                       0,
                                       0,
                                       impl_->path_frame,
                                       reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPathEditId)),
                                       instance,
                                       nullptr);
    SendMessageW(impl_->path_edit,
                 EM_SETMARGINS,
                 EC_LEFTMARGIN | EC_RIGHTMARGIN,
                 MAKELPARAM(detail::ScaleForDpi(8, dpi), detail::ScaleForDpi(8, dpi)));
    impl_->file_list = table::Create(instance, impl_->hwnd, kFileListId, false);
    table::SetMarqueeSelection(impl_->file_list, true);
    const auto& system_file_icons = file_icons::SharedSystemFileIcons();
    impl_->folder_icon = system_file_icons.folder;
    impl_->file_icon = system_file_icons.file;
    table::SetImageList(impl_->file_list, system_file_icons.image_list);
    const std::array file_columns{table::Column{L"名称", detail::ScaleForDpi(220, dpi), 1}};
    table::SetColumns(impl_->file_list, file_columns);
    table::SetFillLastColumn(impl_->file_list, true);
    table::SetTextProvider(impl_->file_list,
                           [page = impl_.get()](const std::size_t row, const std::size_t column) {
                               return page->FileCellText(row, column);
                           });
    table::SetImageProvider(impl_->file_list, [page = impl_.get()](const std::size_t row) {
        return page->FileIcon(row);
    });

    impl_->LoadEditorModules();
    if (impl_->scintilla_module) {
        impl_->editor = CreateWindowExW(0,
                                        L"Scintilla",
                                        L"",
                                        WS_CHILD | WS_TABSTOP,
                                        0,
                                        0,
                                        0,
                                        0,
                                        impl_->hwnd,
                                        nullptr,
                                        instance,
                                        nullptr);
        impl_->scintilla_available = impl_->editor != nullptr;
    }
    if (impl_->editor) {
        SetWindowLongPtrW(impl_->editor,
                          GWL_STYLE,
                          GetWindowLongPtrW(impl_->editor, GWL_STYLE) | WS_CLIPCHILDREN |
                              WS_CLIPSIBLINGS);
        impl_->ConfigureScintilla();
        impl_->editor_horizontal_scrollbar = CreateWindowExW(
            0, L"STATIC", L"", WS_CHILD, 0, 0, 0, 0, impl_->editor, nullptr, instance, nullptr);
        impl_->editor_vertical_scrollbar = CreateWindowExW(
            0, L"STATIC", L"", WS_CHILD, 0, 0, 0, 0, impl_->editor, nullptr, instance, nullptr);
        if (!impl_->editor_horizontal_scrollbar || !impl_->editor_vertical_scrollbar) {
            return false;
        }
        SetWindowSubclass(impl_->editor_horizontal_scrollbar,
                          &Impl::EditorScrollbarSubclassProc,
                          2,
                          reinterpret_cast<DWORD_PTR>(impl_.get()));
        SetWindowSubclass(impl_->editor_vertical_scrollbar,
                          &Impl::EditorScrollbarSubclassProc,
                          3,
                          reinterpret_cast<DWORD_PTR>(impl_.get()));
    }
    impl_->preview = CreateWindowExW(
        0,
        L"EDIT",
        impl_->scintilla_available ? L""
                                   : L"未找到 Scintilla.dll。\r\n请将 Scintilla.dll 和 "
                                     L"Lexilla.dll 放在应用程序目录。",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS | ES_MULTILINE | ES_READONLY |
            ES_AUTOVSCROLL | ES_AUTOHSCROLL,
        0,
        0,
        0,
        0,
        impl_->hwnd,
        nullptr,
        instance,
        nullptr);
    impl_->status_bar =
        CreateWindowExW(0,
                        L"STATIC",
                        L"未打开文件",
                        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE | SS_ENDELLIPSIS,
                        0,
                        0,
                        0,
                        0,
                        impl_->hwnd,
                        nullptr,
                        instance,
                        nullptr);
    impl_->status_details =
        CreateWindowExW(0,
                        L"STATIC",
                        L"",
                        WS_CHILD | WS_VISIBLE | SS_RIGHT | SS_CENTERIMAGE | SS_ENDELLIPSIS,
                        0,
                        0,
                        0,
                        0,
                        impl_->hwnd,
                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kStatusDetailsId)),
                        instance,
                        nullptr);
    impl_->encoding_selector =
        CreateWindowExW(0,
                        selector::kClassName,
                        L"选择编码",
                        WS_CHILD | WS_TABSTOP | WS_CLIPSIBLINGS,
                        0,
                        0,
                        0,
                        0,
                        impl_->hwnd,
                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEncodingSelectorId)),
                        instance,
                        nullptr);
    if (!impl_->encoding_selector)
        return false;
    SendMessageW(impl_->encoding_selector, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"UTF-8"));
    SendMessageW(impl_->encoding_selector, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"GB2312"));
    SendMessageW(impl_->encoding_selector, CB_SETCURSEL, 0, 0);
    SendMessageW(impl_->encoding_selector, CB_SETITEMHEIGHT, 0, detail::ScaleForDpi(22, dpi));
    selector::SetTerminalOverlayStyle(impl_->encoding_selector, true);

    for (HWND control : {impl_->path_frame,
                         impl_->path_edit,
                         impl_->file_list,
                         impl_->editor,
                         impl_->preview,
                         impl_->status_bar,
                         impl_->status_details,
                         impl_->encoding_selector}) {
        detail::ApplySelfDrawnTheme(control);
        detail::SetControlFont(control, impl_->font);
    }
    table::ApplyTheme(impl_->file_list);
    detail::SetControlFont(impl_->path_edit, impl_->font);
    detail::SetControlFont(impl_->status_bar, impl_->caption_font);
    detail::SetControlFont(impl_->status_details, impl_->caption_font);
    detail::SetControlFont(impl_->encoding_selector, impl_->caption_font);
    detail::SetControlFont(impl_->preview, impl_->mono_font);
    for (HWND control : {impl_->editor, impl_->preview, impl_->file_list, impl_->path_edit}) {
        impl_->Subclass(control);
    }
    impl_->tooltip = CreateWindowExW(WS_EX_TOPMOST,
                                     TOOLTIPS_CLASSW,
                                     nullptr,
                                     WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                                     CW_USEDEFAULT,
                                     CW_USEDEFAULT,
                                     CW_USEDEFAULT,
                                     CW_USEDEFAULT,
                                     impl_->hwnd,
                                     nullptr,
                                     instance,
                                     nullptr);
    if (impl_->tooltip) {
        for (const auto control : {impl_->hwnd, impl_->status_bar, impl_->path_edit}) {
            TOOLINFOW info{sizeof(info)};
            info.uFlags = TTF_IDISHWND | TTF_SUBCLASS | TTF_TRANSPARENT;
            info.hwnd = impl_->hwnd;
            info.uId = reinterpret_cast<UINT_PTR>(control);
            info.lpszText = LPSTR_TEXTCALLBACKW;
            SendMessageW(impl_->tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&info));
        }
        SendMessageW(impl_->tooltip, TTM_SETMAXTIPWIDTH, 0, detail::ScaleForDpi(520, dpi));
    }
    SetDirectoryState({ViewPhase::Idle, L"双击侧栏服务器以打开文件", ViewRecoveryAction::None});
    impl_->LoadActiveDocument();
    return true;
}

void EditorPage::SetBounds(int x, int y, int width, int height) {
    SetWindowPos(
        impl_->hwnd, nullptr, x, y, width, height, SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOREDRAW);
}

void EditorPage::Show(bool visible) {
    if (!visible) {
        ShowWindow(impl_->hwnd, SW_HIDE);
        return;
    }
    ShowWindow(impl_->hwnd, SW_SHOWNOACTIVATE);
    SetWindowPos(impl_->hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    const bool restoring = impl_->pending_wrap_viewport.has_value();
    if (restoring)
        impl_->ScheduleHorizontalScrollbarUpdate();
    RedrawWindow(impl_->hwnd,
                 nullptr,
                 nullptr,
                 RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | (restoring ? 0 : RDW_UPDATENOW));
}

void EditorPage::ShowFindBar(bool replace) {
    impl_->ShowFindBar(replace);
}
bool EditorPage::FindText(std::wstring_view query, bool backwards, bool match_case, bool regex) {
    return impl_->FindText(query, backwards, match_case, regex);
}
std::size_t EditorPage::ReplaceText(
    std::wstring_view query, std::wstring_view replacement, bool all, bool match_case, bool regex) {
    return impl_->ReplaceText(query, replacement, all, match_case, regex);
}

void EditorPage::ApplyTheme() {
    impl_->find_bar.ApplyTheme();
    detail::ApplyThemeToWindowTree(impl_->hwnd);
    table::ApplyTheme(impl_->file_list);
    if (impl_->editor) {
        impl_->ConfigureScintilla();
    }
    RECT rect{};
    GetClientRect(impl_->hwnd, &rect);
    impl_->Layout(rect.right, rect.bottom);
    RedrawWindow(impl_->hwnd,
                 nullptr,
                 nullptr,
                 RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

void EditorPage::SetWordWrap(bool enabled) {
    if (impl_->word_wrap == enabled)
        return;
    impl_->CaptureWrapViewport();
    impl_->word_wrap = enabled;
    if (!impl_->editor || !impl_->scintilla_available)
        return;
    SendMessageW(impl_->editor, kSciSetWrapMode, enabled ? 1 : 0, 0);
    if (enabled)
        SendMessageW(impl_->editor, kSciSetXOffset, 0, 0);
    // 设置页操作时编辑控件是隐藏的，不能把未完成折行的几何当成最终结果。
    // 可见时立即恢复，隐藏时保留锚点，到页面重新显示后的消息轮次再应用。
    impl_->RestoreWrapViewport();
    impl_->ScheduleHorizontalScrollbarUpdate();
    impl_->UpdateEditorScrollbarWindows();
}

bool EditorPage::word_wrap() const noexcept {
    return impl_->word_wrap;
}

void EditorPage::RefreshLayout() {
    if (!impl_->hwnd || !IsWindow(impl_->hwnd)) {
        return;
    }
    RECT rect{};
    GetClientRect(impl_->hwnd, &rect);
    impl_->surface.Resize(rect.right, rect.bottom);
    impl_->Layout(rect.right, rect.bottom);

    // file_list 从未随共享侧栏隐藏；这里显式恢复可见状态并刷新自绘表格，防止
    // SWP_NOREDRAW 调整父页面边界后，旧区域被主窗口背景覆盖而没有产生新的 WM_PAINT。
    ShowWindow(impl_->path_frame, SW_SHOWNOACTIVATE);
    ShowWindow(impl_->file_list, SW_SHOWNOACTIVATE);
    table::Refresh(impl_->file_list, true);
    RedrawWindow(impl_->hwnd,
                 nullptr,
                 nullptr,
                 RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

void EditorPage::SetServer(EditorServerOption server) {
    SetServers({std::move(server)});
}

void EditorPage::SetServers(std::vector<EditorServerOption> servers,
                            std::string selected_server_id) {
    impl_->server_options = std::move(servers);
    std::ranges::stable_sort(
        impl_->server_options, [](const EditorServerOption& left, const EditorServerOption& right) {
            if (left.fixed_first != right.fixed_first) {
                return left.fixed_first;
            }
            return CompareStringOrdinal(left.label.c_str(), -1, right.label.c_str(), -1, TRUE) ==
                   CSTR_LESS_THAN;
        });
    if (!selected_server_id.empty()) {
        impl_->selected_server_id = std::move(selected_server_id);
    } else if (!impl_->selected_server_id.empty() &&
               std::ranges::none_of(impl_->server_options,
                                    [impl = impl_.get()](const EditorServerOption& option) {
                                        return option.id == impl->selected_server_id;
                                    })) {
        impl_->selected_server_id.clear();
    }
    InvalidateRect(impl_->hwnd, nullptr, FALSE);
}

void EditorPage::SetRemotePath(std::wstring remote_path) {
    SetWindowTextW(impl_->path_edit, remote_path.c_str());
}

void EditorPage::SetFileEntries(std::vector<EditorFileEntry> entries) {
    std::erase_if(entries, [](const EditorFileEntry& entry) {
        return entry.name.empty() || (entry.name != L".." && entry.name.front() == L'.');
    });
    std::ranges::sort(entries, [](const EditorFileEntry& left, const EditorFileEntry& right) {
        if (left.name == L".." || right.name == L"..") {
            return left.name == L".." && right.name != L"..";
        }
        if (left.directory != right.directory) {
            return left.directory && !right.directory;
        }
        return _wcsicmp(left.name.c_str(), right.name.c_str()) < 0;
    });
    const bool only_parent = entries.size() == 1 && entries.front().name == L"..";
    if (entries.empty() || only_parent) {
        impl_->directory_state = {ViewPhase::Empty, L"文件夹为空", ViewRecoveryAction::None};
        EditorFileEntry placeholder;
        placeholder.name = L"文件夹为空";
        placeholder.placeholder = true;
        entries.push_back(std::move(placeholder));
    } else {
        impl_->directory_state = {ViewPhase::Ready, {}, ViewRecoveryAction::None};
    }
    impl_->file_entries = std::move(entries);
    table::SetRowCount(impl_->file_list, impl_->file_entries.size());
    table::Refresh(impl_->file_list);
}

void EditorPage::SetDirectoryState(ViewState state) {
    impl_->directory_state = std::move(state);
    if (impl_->directory_state.phase == ViewPhase::Ready) {
        return;
    }
    EditorFileEntry placeholder;
    placeholder.placeholder = true;
    placeholder.name = impl_->directory_state.message;
    if (placeholder.name.empty()) {
        placeholder.name = impl_->directory_state.phase == ViewPhase::Loading
                               ? L"正在加载目录…"
                               : L"选择服务器打开文件";
    }
    if (impl_->directory_state.action == ViewRecoveryAction::Retry) {
        placeholder.status = L"双击重试";
    } else if (impl_->directory_state.action == ViewRecoveryAction::ManageServer) {
        placeholder.status = L"双击管理";
    }
    impl_->file_entries = {std::move(placeholder)};
    table::SetRowCount(impl_->file_list, 1);
    table::Refresh(impl_->file_list);
}

void EditorPage::SetViewActionCallback(ViewActionCallback callback) {
    impl_->view_action = std::move(callback);
}

bool EditorPage::ActivateServer(const std::string_view server_id) {
    if (server_id.empty()) {
        return false;
    }
    impl_->selected_server_id.assign(server_id);
    InvalidateRect(impl_->hwnd, nullptr, FALSE);
    InvokeSafely(impl_->server_changed, impl_->selected_server_id);
    return true;
}

EditorDocumentId EditorPage::OpenDocument(EditorDocumentDescriptor document) {
    if (document.kind == EditorDocumentKind::Binary) {
        return OpenBinaryDocument(std::move(document), {});
    }
    if (document.id == 0) {
        document.id = impl_->next_document_id++;
    } else {
        impl_->next_document_id = (std::max)(impl_->next_document_id, document.id + 1U);
    }
    if (auto* existing = impl_->FindDocument(document.id)) {
        existing->title = std::move(document.title);
        impl_->BindDocumentServer(*existing, std::move(document.server_id));
        existing->remote_path = std::move(document.remote_path);
        existing->content = std::move(document.content);
        existing->encoding = document.encoding;
        existing->eol = document.eol;
        existing->kind = document.kind;
        existing->read_only =
            document.read_only || document.kind == EditorDocumentKind::ReadOnlyLarge;
        existing->dirty = document.dirty;
        existing->changed_lines.clear();
        existing->touched_mark_count = 0;
        existing->preview = document.kind == EditorDocumentKind::ReadOnlyLarge
                                ? Utf8ToWide(existing->content)
                                : std::wstring{};
        InvalidateRect(impl_->hwnd, nullptr, FALSE);
        if (impl_->active_id && *impl_->active_id == existing->id) {
            impl_->LoadActiveDocument();
        } else {
            impl_->ActivateDocument(existing->id);
        }
        return existing->id;
    }

    Impl::DocumentState state;
    state.id = document.id;
    state.title = document.title.empty() ? L"未命名" : std::move(document.title);
    impl_->BindDocumentServer(state, std::move(document.server_id));
    state.remote_path = std::move(document.remote_path);
    state.content = std::move(document.content);
    state.encoding = document.encoding;
    state.eol = document.eol;
    state.kind = document.kind;
    state.read_only = document.read_only || document.kind == EditorDocumentKind::ReadOnlyLarge;
    state.dirty = document.dirty;
    if (state.kind == EditorDocumentKind::ReadOnlyLarge && !state.content.empty()) {
        state.preview = Utf8ToWide(state.content);
    }
    impl_->documents.push_back(std::move(state));

    impl_->ActivateDocument(document.id);
    return document.id;
}

EditorDocumentId EditorPage::OpenBinaryDocument(EditorDocumentDescriptor document,
                                                std::span<const std::byte> bytes) {
    document.kind = EditorDocumentKind::Binary;
    document.read_only = true;
    if (document.id == 0) {
        document.id = impl_->next_document_id++;
    } else {
        impl_->next_document_id = (std::max)(impl_->next_document_id, document.id + 1U);
    }
    bool truncated = false;
    std::wstring preview = BuildHexPreview(bytes, truncated);
    if (auto* existing = impl_->FindDocument(document.id)) {
        existing->title = std::move(document.title);
        impl_->BindDocumentServer(*existing, std::move(document.server_id));
        existing->remote_path = std::move(document.remote_path);
        existing->content.clear();
        existing->preview = std::move(preview);
        existing->encoding = document.encoding;
        existing->eol = document.eol;
        existing->kind = EditorDocumentKind::Binary;
        existing->read_only = true;
        existing->dirty = false;
        existing->preview_truncated = truncated;
        InvalidateRect(impl_->hwnd, nullptr, FALSE);
        if (impl_->active_id && *impl_->active_id == existing->id) {
            impl_->LoadActiveDocument();
        } else {
            impl_->ActivateDocument(existing->id);
        }
        return existing->id;
    }

    Impl::DocumentState state;
    state.id = document.id;
    state.title = document.title.empty() ? L"未命名" : std::move(document.title);
    impl_->BindDocumentServer(state, std::move(document.server_id));
    state.remote_path = std::move(document.remote_path);
    state.preview = std::move(preview);
    state.encoding = document.encoding;
    state.eol = document.eol;
    state.kind = EditorDocumentKind::Binary;
    state.read_only = true;
    state.preview_truncated = truncated;
    impl_->documents.push_back(std::move(state));
    impl_->ActivateDocument(document.id);
    return document.id;
}

bool EditorPage::OpenDocument(std::wstring tab_title,
                              const std::string& utf8_text,
                              bool read_only) {
    if (!impl_->scintilla_available) {
        return false;
    }
    EditorDocumentDescriptor document;
    document.title = std::move(tab_title);
    document.content = utf8_text;
    document.read_only = read_only;
    OpenDocument(std::move(document));
    return true;
}

bool EditorPage::ActivateDocument(EditorDocumentId id) {
    return impl_->ActivateDocument(id);
}

bool EditorPage::CloseDocument(EditorDocumentId id) {
    return impl_->CloseDocument(id);
}

void EditorPage::RequestCloseDocument(EditorDocumentId id) {
    impl_->RequestCloseDocument(id);
}

bool EditorPage::SetDocumentContent(EditorDocumentId id,
                                    std::string content,
                                    EditorTextEncoding encoding,
                                    EditorEolStyle eol,
                                    EditorDocumentKind kind,
                                    bool read_only,
                                    bool dirty) {
    auto* document = impl_->FindDocument(id);
    if (!document) {
        return false;
    }
    document->content = std::move(content);
    document->encoding = encoding;
    document->eol = eol;
    document->kind = kind;
    document->read_only = read_only || kind != EditorDocumentKind::Text;
    document->dirty = dirty;
    document->saving = false;
    if (kind == EditorDocumentKind::ReadOnlyLarge) {
        document->preview = Utf8ToWide(document->content);
    } else if (kind == EditorDocumentKind::Binary) {
        document->preview.clear();
        document->preview_truncated = false;
    }
    InvalidateRect(impl_->hwnd, nullptr, FALSE);
    if (impl_->active_id && *impl_->active_id == id) {
        impl_->LoadActiveDocument();
    }
    return true;
}

void EditorPage::SetDocumentSaving(EditorDocumentId id, bool saving) {
    auto* document = impl_->FindDocument(id);
    if (!document) {
        return;
    }
    document->saving = saving;
    if (impl_->active_id && *impl_->active_id == id && impl_->encoding_selector) {
        EnableWindow(impl_->encoding_selector,
                     !saving && !document->closing && !document->read_only);
    }
    InvalidateRect(impl_->hwnd, nullptr, FALSE);
    if (impl_->active_id && *impl_->active_id == id) {
        impl_->UpdateStatus();
    }
}

void EditorPage::SetDocumentClosing(EditorDocumentId id, bool closing) {
    auto* document = impl_->FindDocument(id);
    if (!document)
        return;
    document->closing = closing;
    if (impl_->active_id && *impl_->active_id == id) {
        SendMessageW(impl_->editor,
                     kSciSetReadOnly,
                     closing || document->read_only || document->kind != EditorDocumentKind::Text,
                     0);
        EnableWindow(impl_->encoding_selector,
                     !closing && !document->saving && !document->read_only);
        impl_->UpdateStatus();
    }
    InvalidateRect(impl_->hwnd, nullptr, FALSE);
}

void EditorPage::MarkDocumentSaved(EditorDocumentId id) {
    auto* document = impl_->FindDocument(id);
    if (!document) {
        return;
    }
    if (impl_->active_id && *impl_->active_id == id) {
        impl_->SyncActiveContent();
        if (impl_->scintilla_available && document->kind == EditorDocumentKind::Text) {
            impl_->loading_document = true;
            SendMessageW(impl_->editor, kSciSetSavePoint, 0, 0);
            impl_->loading_document = false;
        }
        std::string{}.swap(document->content);
    }
    document->dirty = false;
    document->saving = false;
    document->trusted_savepoint = true;
    document->saved_encoding = document->encoding;
    document->changed_lines.clear();
    document->touched_mark_count = 0;
    if (impl_->active_id == id)
        SendMessageW(impl_->editor, kSciMarkerDeleteAll, kRestoredChangeMarker, 0);
    document->conflict.clear();
    if (impl_->active_id && *impl_->active_id == id && impl_->encoding_selector) {
        EnableWindow(impl_->encoding_selector, true);
    }
    InvalidateRect(impl_->hwnd, nullptr, FALSE);
    impl_->UpdateStatus();
    impl_->UpdateConflictBar();
}

void EditorPage::SetDocumentLocation(EditorDocumentId id,
                                     std::wstring title,
                                     std::string server_id,
                                     std::wstring path) {
    if (auto* document = impl_->FindDocument(id)) {
        document->title = std::move(title);
        impl_->BindDocumentServer(*document, std::move(server_id));
        document->remote_path = std::move(path);
        if (impl_->active_id == id && impl_->scintilla_available)
            impl_->ConfigureLexer(*document);
        impl_->UpdateStatus();
        InvalidateRect(impl_->hwnd, nullptr, FALSE);
    }
}

bool EditorPage::DocumentDirty(EditorDocumentId id) const noexcept {
    const auto* document = impl_->FindDocument(id);
    return document && document->dirty;
}

void EditorPage::SetDocumentDirty(EditorDocumentId id, bool dirty) {
    if (!dirty) {
        MarkDocumentSaved(id);
    } else if (auto* document = impl_->FindDocument(id)) {
        document->dirty = true;
        impl_->UpdateStatus();
        InvalidateRect(impl_->hwnd, nullptr, FALSE);
    }
}

void EditorPage::SetDocumentConflict(EditorDocumentId id, std::wstring message) {
    if (auto* document = impl_->FindDocument(id)) {
        document->saving = false;
        document->conflict = std::move(message);
        if (impl_->active_id && *impl_->active_id == id && impl_->encoding_selector) {
            EnableWindow(impl_->encoding_selector, true);
        }
        InvalidateRect(impl_->hwnd, nullptr, FALSE);
        if (impl_->active_id && *impl_->active_id == id) {
            impl_->UpdateStatus();
            impl_->UpdateConflictBar();
        }
    }
}

void EditorPage::ClearDocumentConflict(EditorDocumentId id) {
    if (auto* document = impl_->FindDocument(id)) {
        document->conflict.clear();
        if (impl_->active_id && *impl_->active_id == id) {
            impl_->UpdateConflictBar();
        }
    }
}

std::optional<std::string> EditorPage::DocumentText(EditorDocumentId id) {
    auto* document = impl_->FindDocument(id);
    if (!document || document->kind != EditorDocumentKind::Text) {
        return std::nullopt;
    }
    if (impl_->active_id && *impl_->active_id == id) {
        impl_->SyncActiveContent();
    }
    return document->content;
}

bool EditorPage::DocumentSaving(EditorDocumentId id) const noexcept {
    const auto* document = impl_->FindDocument(id);
    return document && document->saving;
}

std::optional<EditorSaveRequest> EditorPage::DocumentSnapshot(EditorDocumentId id) {
    auto text = DocumentText(id);
    const auto* document = impl_->FindDocument(id);
    if (!document || !text)
        return std::nullopt;
    return EditorSaveRequest{id, std::move(*text), document->encoding, document->eol};
}

std::optional<EditorDocumentId> EditorPage::active_document() const noexcept {
    return impl_->active_id;
}

std::size_t EditorPage::document_count() const noexcept {
    return impl_->documents.size();
}

void EditorPage::SetSaveRequestedCallback(SaveRequestCallback callback) {
    impl_->save_requested = std::move(callback);
}

void EditorPage::SetEncodingChangedCallback(SaveRequestCallback callback) {
    impl_->encoding_changed = std::move(callback);
}

void EditorPage::SetOverwriteRequestedCallback(SaveRequestCallback callback) {
    impl_->overwrite_requested = std::move(callback);
}

void EditorPage::SetSaveAsRequestedCallback(SaveRequestCallback callback) {
    impl_->save_as_requested = std::move(callback);
}

void EditorPage::SetDiffRequestedCallback(DocumentActionCallback callback) {
    impl_->diff_requested = std::move(callback);
}

void EditorPage::SetChangesRequestedCallback(DocumentActionCallback callback) {
    impl_->changes_requested = std::move(callback);
}

void EditorPage::RequestChanges() {
    impl_->HandleCommand(kChangesId, BN_CLICKED);
}

HWND EditorPage::ShowChangesView(std::wstring title, std::string report) {
    WNDCLASSW wc{};
    wc.hInstance = impl_->instance;
    wc.lpfnWndProc = ChangesViewProc;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kChangesViewClass;
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return nullptr;
    auto state = std::make_unique<ChangesViewContext>();
    state->module = LoadLibraryW((detail::ExecutableDirectory() / L"Scintilla.dll").c_str());
    if (!state->module)
        return nullptr;
    state->report = std::move(report);
    const auto owner = GetAncestor(impl_->hwnd, GA_ROOT);
    const auto view = CreateWindowExW(WS_EX_TOOLWINDOW,
                                      kChangesViewClass,
                                      title.c_str(),
                                      WS_OVERLAPPEDWINDOW,
                                      CW_USEDEFAULT,
                                      CW_USEDEFAULT,
                                      detail::ScaleForDpi(850, detail::WindowDpi(owner)),
                                      detail::ScaleForDpi(600, detail::WindowDpi(owner)),
                                      owner,
                                      nullptr,
                                      impl_->instance,
                                      state.get());
    if (view) {
        // 创建成功后才移交上下文和 DLL 引用，资源不足时不影响源文件及撤销栈。
        state->window_owned = true;
        state.release();
        ShowWindow(view, SW_SHOW);
    }
    return view;
}

std::wstring EditorPage::DocumentDisplayName(EditorDocumentId id) const {
    const auto* document = impl_->FindDocument(id);
    if (!document)
        return {};
    return document->server_name.empty() ? document->title
                                         : document->server_name + L" " + document->title;
}

void EditorPage::SetCloseRequestedCallback(CloseRequestCallback callback) {
    impl_->close_requested = std::move(callback);
}

void EditorPage::SetDocumentChangedCallback(DocumentChangedCallback callback) {
    impl_->document_changed = std::move(callback);
}

void EditorPage::SetActiveDocumentChangedCallback(ActiveDocumentChangedCallback callback) {
    impl_->active_document_changed = std::move(callback);
}

void EditorPage::SetFileActivationCallback(FileActivationCallback callback) {
    impl_->file_activation = std::move(callback);
}

void EditorPage::SetServerChangedCallback(ServerChangedCallback callback) {
    impl_->server_changed = std::move(callback);
}

void EditorPage::SetPathChangedCallback(PathChangedCallback callback) {
    impl_->path_changed = std::move(callback);
}

void EditorPage::SetHistoryNavigationCallback(HistoryNavigationCallback callback) {
    impl_->history_navigation = std::move(callback);
}

void EditorPage::RequestSave() {
    impl_->RequestSave();
}

void EditorPage::ShowConflict(std::wstring message) {
    if (impl_->active_id) {
        SetDocumentConflict(*impl_->active_id, std::move(message));
    }
}

void EditorPage::HideConflict() {
    if (impl_->active_id) {
        ClearDocumentConflict(*impl_->active_id);
    }
}

HWND EditorPage::editor_hwnd() const noexcept {
    return impl_->editor;
}
HWND EditorPage::hwnd() const noexcept {
    return impl_->hwnd;
}
bool EditorPage::scintilla_available() const noexcept {
    return impl_->scintilla_available;
}

} // namespace SLink::ui
