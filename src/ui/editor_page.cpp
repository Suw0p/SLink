// 远程编辑页面实现：文件卡片列表、文档标签、Scintilla 宿主、状态栏和
// 分隔条均只维护 UI 状态，远程 I/O 通过 EditorController 回调执行。
#include "ui/editor_page.hpp"
#include "editor/gb2312_codec.hpp"

#include "ui/file_icon_provider.hpp"
#include "ui/local_shortcut.hpp"
#include "ui/retained_ui.hpp"
#include "ui/selector_control.hpp"
#include "ui/table_control.hpp"
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
constexpr UINT kSciLinesOnScreen = 2370;
constexpr UINT kSciLineScroll = 2168;
constexpr UINT kSciGetXOffset = 2398;
constexpr UINT kSciSetXOffset = 2397;
constexpr UINT kSciSetEolMode = 2031;
constexpr UINT kSciSetSavePoint = 2014;
constexpr UINT kSciGetCurrentPos = 2008;
constexpr UINT kSciLineFromPosition = 2166;
constexpr UINT kSciGetColumn = 2129;
constexpr UINT kSciLineLength = 2350;
constexpr UINT kSciGetLine = 2153;
constexpr UINT kSciSetILexer = 4033;
constexpr UINT kSciColourise = 4003;

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
        bool saving{};
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
    HWND editor_horizontal_scrollbar{};
    HWND editor_vertical_scrollbar{};
    HWND preview{};
    HWND status_bar{};
    HWND resize_guide{};
    HMODULE scintilla_module{};
    HMODULE lexilla_module{};
    HFONT font{};
    HFONT title_font{};
    HFONT mono_font{};
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
    std::array<int, 6> file_column_widths{{-1, -1, -1, -1, -1, -1}};

    SaveRequestCallback save_requested;
    SaveRequestCallback encoding_changed;
    SaveRequestCallback overwrite_requested;
    SaveRequestCallback save_as_requested;
    DocumentActionCallback diff_requested;
    CloseRequestCallback close_requested;
    DocumentChangedCallback document_changed;
    ActiveDocumentChangedCallback active_document_changed;
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
            if (const auto* header = reinterpret_cast<const NMHDR*>(lparam);
                header && self->HandleNotify(*header)) {
                return 0;
            }
            break;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX: {
            const HDC dc = reinterpret_cast<HDC>(wparam);
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
            const bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
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
        if (message == WM_CHAR && self && (GetKeyState(VK_CONTROL) & 0x8000) != 0 &&
            (wparam == 19 || wparam == 23)) {
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

    void NavigateFromPath() {
        if (selected_server_id.empty()) {
            return;
        }
        InvokeSafely(path_changed, selected_server_id, WindowText(path_edit));
    }

    void ChangeEncoding() {
        auto* document = ActiveDocument();
        if (!document || document->kind != EditorDocumentKind::Text || document->read_only ||
            document->saving) {
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
                    MessageBoxW(hwnd,
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
                MessageBoxW(hwnd,
                            L"文件内容不是有效的 UTF-8，请选择 GB2312 或保持原编码。",
                            L"SLink",
                            MB_OK | MB_ICONERROR);
                SendMessageW(encoding_selector, CB_SETCURSEL, static_cast<WPARAM>(-1), 0);
                return;
            }
        }
        if (target == EditorTextEncoding::Gb2312 && !editor::EncodeGb2312(text)) {
            MessageBoxW(hwnd,
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
        metrics.horizontal_max = std::max(0, content_width - metrics.horizontal_page);
        metrics.horizontal_position =
            std::clamp(static_cast<int>(SendMessageW(editor, kSciGetXOffset, 0, 0)),
                       0,
                       metrics.horizontal_max);
        metrics.horizontal_visible = metrics.horizontal_max > 0;

        const int line_count =
            std::max(1, static_cast<int>(SendMessageW(editor, kSciGetLineCount, 0, 0)));
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
        const COLORREF margin_background = theme::kPanelColor;
        const COLORREF margin_foreground = theme::kMutedTextColor;
        SendMessageW(editor, kSciSetCodePage, kScCpUtf8, 0);
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
        SendMessageW(editor, kSciSetCaretFore, dark ? RGB(92, 207, 130) : RGB(0, 95, 184), 0);
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
    }

    void UpdateStatus() const {
        const auto* document = ActiveDocument();
        if (!document) {
            SetWindowTextW(status_bar, L"未打开文件");
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
            status = EncodingLabel(document->encoding);
            status.append(L" • ");
            status.append(EolLabel(document->eol));
            status.append(document->read_only ? L" • 只读" : L" • 可编辑");
            if (document->dirty) {
                status.append(L" • 已修改");
            }
            if (scintilla_available && editor && IsWindowVisible(editor)) {
                const LRESULT position = SendMessageW(editor, kSciGetCurrentPos, 0, 0);
                const LRESULT line = SendMessageW(editor, kSciLineFromPosition, position, 0);
                const LRESULT column = SendMessageW(editor, kSciGetColumn, position, 0);
                status.append(L" • 行 ");
                status.append(std::to_wstring(line + 1));
                status.append(L"，列 ");
                status.append(std::to_wstring(column + 1));
            }
        }
        if (document->saving) {
            status.append(L" • 正在保存…");
        }
        if (!document->server_name.empty()) {
            std::wstring location = L"服务器 " + document->server_name;
            if (!document->remote_path.empty()) {
                location.append(L" · ");
                location.append(document->remote_path);
            }
            status.insert(0, location + L"  |  ");
        }
        SetWindowTextW(status_bar, status.c_str());
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
        auto* document = ActiveDocument();
        if (encoding_selector) {
            const bool editable =
                document && document->kind == EditorDocumentKind::Text && !document->read_only;
            ShowWindow(encoding_selector, editable ? SW_SHOWNOACTIVATE : SW_HIDE);
            EnableWindow(encoding_selector, editable && !document->saving);
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
                SendMessageW(editor, kSciSetReadOnly, FALSE, 0);
                SendMessageA(editor, kSciSetText, 0, reinterpret_cast<LPARAM>(""));
                SendMessageW(editor, kSciSetScrollWidth, 1, 0);
                SendMessageW(editor, kSciSetReadOnly, TRUE, 0);
                SendMessageW(editor, kSciSetHScrollBar, FALSE, 0);
                SendMessageW(editor, kSciSetVScrollBar, FALSE, 0);
            } else {
                ShowWindow(editor, SW_HIDE);
                ShowWindow(preview, SW_SHOW);
                SetWindowTextW(preview, L"");
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
                SendMessageW(editor, kSciSetSavePoint, 0, 0);
                SendMessageW(editor,
                             kSciSetReadOnly,
                             document->kind == EditorDocumentKind::Text && !document->read_only
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
            document->saving) {
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
        if (!document) {
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
        if (active_id && *active_id == id) {
            SyncActiveContent();
        }
        documents.erase(documents.begin() + index);
        if (documents.empty()) {
            active_id.reset();
            LoadActiveDocument();
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
            UpdateEditorScrollbarWindows();
            return false;
        }
        if (header.hwndFrom == editor && active_id) {
            auto* document = ActiveDocument();
            if (!document || loading_document) {
                return false;
            }
            if (header.code == kScnModified) {
                const auto& notification =
                    reinterpret_cast<const ScintillaNotificationPrefix&>(header);
                if ((notification.modification_type & (kScModInsertText | kScModDeleteText)) == 0) {
                    return false;
                }
                document->dirty = true;
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
                document->dirty = true;
                UpdateStatus();
                return false;
            }
            if (header.code == kScnSavePointReached && !document->saving) {
                UpdateStatus();
                return false;
            }
            if (header.code == kScnUpdateUi) {
                UpdateStatus();
                return false;
            }
        }
        return false;
    }

    std::wstring FileCellText(const std::size_t row, const std::size_t column) const {
        if (row >= file_entries.size())
            return {};
        const EditorFileEntry& entry = file_entries[row];
        switch (column) {
        case 0:
            return entry.local_shortcut ? local_shortcut::DisplayName(entry.name) : entry.name;
        case 1:
            return entry.status;
        case 2:
            return entry.size;
        case 3:
            return entry.modified;
        case 4:
            return entry.permissions;
        case 5:
            return entry.owner;
        default:
            return {};
        }
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
        const int gap = detail::ScaleForDpi(8, dpi);
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
            const int gap = detail::ScaleForDpi(8, dpi);
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
            const int pad = detail::ScaleForDpi(12, dpi);
            const int gap = detail::ScaleForDpi(8, dpi);
            const int next_position = static_cast<int>(client.bottom) - pad - next_value - gap / 2;
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

    void HandleSurfaceClick(int id) {
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
        const int pad = detail::ScaleForDpi(12, dpi);
        const int gap = detail::ScaleForDpi(8, dpi);
        const int row = detail::ScaleForDpi(32, dpi);
        const int button = detail::ScaleForDpi(68, dpi);
        const int available_width = std::max(0, width);
        const int min_left =
            (std::min)(detail::ScaleForDpi(theme::kServerSidebarWidth, dpi), available_width / 2);
        const int max_left = (std::max)(min_left, available_width / 2);
        if (left_width_override == 0) {
            left_width_override = min_left;
        }
        const int left_width = std::clamp(left_width_override, min_left, max_left);
        const int file_left = 0;
        const int right_x = file_left + left_width + gap;
        const int right_width = (std::max)(0, width - right_x - pad);

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
        surface.Add({
            .role = detail::UiRole::separator,
            .bounds = detail::UiRect(static_cast<float>(file_left + left_width + gap / 2),
                                     0.0F,
                                     static_cast<float>(file_left + left_width + gap / 2 + 2),
                                     static_cast<float>(height)),
        });
        const float refresh_width = static_cast<float>(detail::ScaleForDpi(26, dpi));
        surface.Add({
            .id = kPathRefreshId,
            .role = detail::UiRole::icon_button,
            .bounds =
                detail::UiRect(static_cast<float>(file_left + left_width - pad) - refresh_width,
                               static_cast<float>(pad),
                               static_cast<float>(file_left + left_width - pad),
                               static_cast<float>(pad + detail::ScaleForDpi(24, dpi))),
            .icon = detail::UiIcon::refresh,
            .alignment = detail::UiTextAlign::center,
            .enabled = !selected_server_id.empty(),
        });
        // Match the terminal page: the tab strip starts at the top edge and
        // the active document surface joins it without an extra gap.
        const int tab_y = 0;
        const float strip_height = 34.0F * scale;
        const int close_x = right_x + right_width;
        const float tab_available = static_cast<float>(std::max(0, right_width)) - 10.0F * scale;
        const float tab_width =
            documents.empty() ? 0.0F
                              : std::clamp(tab_available / static_cast<float>(documents.size()),
                                           78.0F * scale,
                                           147.0F * scale);
        surface.Add({
            .role = detail::UiRole::panel,
            .bounds = detail::UiRect(static_cast<float>(right_x),
                                     static_cast<float>(tab_y),
                                     static_cast<float>(right_x + right_width),
                                     static_cast<float>(tab_y) + strip_height),
        });
        float tab_x = static_cast<float>(right_x) + 5.0F * scale;
        for (std::size_t index = 0; index < documents.size(); ++index) {
            if (tab_x >= close_x) {
                break;
            }
            const auto& document = documents[index];
            std::wstring label = document.title.empty() ? L"未命名" : document.title;
            if (document.saving) {
                label.append(L"  保存中…");
            } else if (document.dirty) {
                label.append(L"  ●");
            }
            const float right = std::min(tab_x + tab_width, static_cast<float>(close_x));
            surface.Add({
                .id = kDocumentTabFirstId + static_cast<int>(index),
                .role = detail::UiRole::file_tab,
                .bounds = detail::UiRect(tab_x,
                                         static_cast<float>(tab_y) + 3.0F * scale,
                                         right,
                                         static_cast<float>(tab_y) + strip_height - 1.0F * scale),
                .text = std::move(label),
                .icon = detail::UiIcon::edit,
                .alignment = detail::UiTextAlign::leading,
                .selected = active_id && *active_id == document.id,
                .compact_text = true,
            });
            const float close_width = 18.0F * scale;
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
            });
            tab_x = right + 2.0F * scale;
        }

        const int status_height = row;
        const int conflict_height =
            conflict_visible ? (conflict_height_override > 0 ? conflict_height_override
                                                             : detail::ScaleForDpi(74, dpi))
                             : 0;
        const int status_base_y =
            height - pad - status_height - conflict_height - (conflict_visible ? gap : 0);
        const int editor_frame_y = detail::ScaleForDpi(34, dpi) - 1;
        if (right_width > 0 && status_base_y > editor_frame_y) {
            // Use the application theme border instead of the bright native
            // WS_EX_CLIENTEDGE frame. The native editor is inset in Layout().
            const float border = static_cast<float>(std::max(1, detail::ScaleForDpi(1, dpi)));
            const float left = static_cast<float>(right_x);
            const float top = static_cast<float>(editor_frame_y);
            const float right = static_cast<float>(right_x + right_width);
            const float bottom = static_cast<float>(status_base_y);
            surface.Add({.role = detail::UiRole::separator,
                         .bounds = detail::UiRect(left, top, right, top + border)});
            surface.Add({.role = detail::UiRole::separator,
                         .bounds = detail::UiRect(left, bottom - border, right, bottom)});
            surface.Add({.role = detail::UiRole::separator,
                         .bounds = detail::UiRect(left, top, left + border, bottom)});
            surface.Add({.role = detail::UiRole::separator,
                         .bounds = detail::UiRect(right - border, top, right, bottom)});
        }
        const int status_y = status_base_y + (conflict_visible ? 0 : pad / 2);
        surface.Add({
            .id = kSaveId,
            .role = detail::UiRole::primary_button,
            .bounds = detail::UiRect(static_cast<float>(right_x + right_width - button),
                                     static_cast<float>(status_y),
                                     static_cast<float>(right_x + right_width),
                                     static_cast<float>(status_y + status_height)),
            .text = L"保存",
            .alignment = detail::UiTextAlign::center,
            .enabled = ActiveDocument() && ActiveDocument()->kind == EditorDocumentKind::Text &&
                       !ActiveDocument()->read_only && !ActiveDocument()->saving,
        });

        if (conflict_visible) {
            const int visible_conflict_height = conflict_height_override > 0
                                                    ? conflict_height_override
                                                    : detail::ScaleForDpi(74, dpi);
            const int conflict_y = height - pad - visible_conflict_height;
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
            surface.Add({
                .role = detail::UiRole::status,
                .bounds = detail::UiRect(
                    static_cast<float>(right_x + pad),
                    static_cast<float>(conflict_y),
                    static_cast<float>(right_x + right_width - detail::ScaleForDpi(350, dpi)),
                    static_cast<float>(conflict_y + visible_conflict_height)),
                .text = document ? document->conflict : L"远程文件发生变化",
            });
            const int action_width = detail::ScaleForDpi(82, dpi);
            const int close_width = detail::ScaleForDpi(30, dpi);
            int action_x = right_x + right_width - (action_width * 3 + close_width + pad * 4);
            static constexpr std::array<int, 3> conflict_ids = {kDiffId, kOverwriteId, kSaveAsId};
            static constexpr std::array<const wchar_t*, 3> conflict_labels = {
                L"查看差异", L"覆盖远端", L"另存为"};
            for (std::size_t index = 0; index < conflict_ids.size(); ++index) {
                surface.Add({
                    .id = conflict_ids[index],
                    .role = detail::UiRole::ghost_button,
                    .bounds = detail::UiRect(
                        static_cast<float>(action_x),
                        static_cast<float>(conflict_y + (visible_conflict_height - row) / 2),
                        static_cast<float>(action_x + action_width),
                        static_cast<float>(conflict_y + (visible_conflict_height + row) / 2)),
                    .text = conflict_labels[index],
                    .alignment = detail::UiTextAlign::center,
                });
                action_x += action_width + pad;
            }
            surface.Add({
                .id = kDismissConflictId,
                .role = detail::UiRole::icon_button,
                .bounds = detail::UiRect(
                    static_cast<float>(right_x + right_width - close_width - pad),
                    static_cast<float>(conflict_y + (visible_conflict_height - row) / 2),
                    static_cast<float>(right_x + right_width - pad),
                    static_cast<float>(conflict_y + (visible_conflict_height + row) / 2)),
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
        const int right_x = left_splitter_x + detail::ScaleForDpi(8, dpi);
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
        const int tab_id = kDocumentTabFirstId +
                           static_cast<int>(std::distance(documents.begin(), found));
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
        if (!file_list || width <= 0 || height <= 0) {
            return;
        }
        const UINT dpi = detail::WindowDpi(hwnd);
        const int pad = detail::ScaleForDpi(12, dpi);
        const int gap = detail::ScaleForDpi(8, dpi);
        const int row = detail::ScaleForDpi(32, dpi);
        const int button = detail::ScaleForDpi(68, dpi);
        // 服务器侧栏由 MainWindow 统一创建和布局。EditorPage 收到的客户区已经是
        // 扣除共享侧栏后的页面区域，这里不能再次预留侧栏宽度，否则切换侧栏可见性时
        // 文件列表会被移出客户区，看起来像是“消失”了。
        const int available_width = std::max(0, width);
        const int min_left =
            (std::min)(detail::ScaleForDpi(theme::kServerSidebarWidth, dpi), available_width / 2);
        const int max_left = (std::max)(min_left, available_width / 2);
        if (left_width_override == 0) {
            left_width_override = min_left;
        }
        const int left_width = std::clamp(left_width_override, min_left, max_left);
        left_width_override = left_width;
        const int file_left = 0;
        const int right_x = file_left + left_width + gap;
        const int right_width = (std::max)(0, width - right_x - pad);
        left_splitter_x = file_left + left_width + gap / 2;

        const int path_height = detail::ScaleForDpi(24, dpi);
        const int file_x = file_left + pad;
        const int file_width = std::max(1, left_width - pad * 2);
        const int path_width = std::max(1, file_width - detail::ScaleForDpi(32, dpi));
        const int path_top = pad;
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
        const std::array<int, 6> minimum_widths = {detail::ScaleForDpi(180, dpi),
                                                   detail::ScaleForDpi(78, dpi),
                                                   detail::ScaleForDpi(72, dpi),
                                                   detail::ScaleForDpi(112, dpi),
                                                   detail::ScaleForDpi(80, dpi),
                                                   detail::ScaleForDpi(90, dpi)};
        const std::array<int, 6> preferred_widths = {std::clamp(file_width * 43 / 100,
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
        if (file_width > minimum_total) {
            const int available_growth = file_width - minimum_total;
            const int preferred_growth = preferred_total - minimum_total;
            if (preferred_growth > 0) {
                const double ratio = std::min(1.0,
                                              static_cast<double>(available_growth) /
                                                  static_cast<double>(preferred_growth));
                for (std::size_t column = 0; column < desired_widths.size(); ++column) {
                    desired_widths[column] += static_cast<int>(
                        std::lround((preferred_widths[column] - minimum_widths[column]) * ratio));
                }
            }
            const int used = std::accumulate(desired_widths.begin(), desired_widths.end(), 0);
            desired_widths.back() += std::max(0, file_width - used);
        }
        if (file_column_widths != desired_widths) {
            table::SetColumnWidths(file_list, desired_widths);
            file_column_widths = desired_widths;
        }

        const int tab_y = 0;
        const int tab_height = detail::ScaleForDpi(34, dpi);

        const int editor_y = tab_y + tab_height - 1;
        const int status_height = row;
        if (conflict_height_override == 0) {
            conflict_height_override = detail::ScaleForDpi(74, dpi);
        }
        const int conflict_height =
            conflict_visible ? std::clamp(conflict_height_override,
                                          detail::ScaleForDpi(60, dpi),
                                          std::max(detail::ScaleForDpi(60, dpi), height / 2))
                             : 0;
        const int editor_height = (std::max)(0,
                                             height - editor_y - pad - status_height -
                                                 conflict_height - (conflict_visible ? gap : 0));
        const int editor_border = std::max(1, detail::ScaleForDpi(1, dpi));
        const int editor_content_width = std::max(0, right_width - editor_border * 2);
        const int editor_content_height = std::max(0, editor_height - editor_border * 2);
        MoveWindow(editor,
                   right_x + editor_border,
                   editor_y + editor_border,
                   editor_content_width,
                   editor_content_height,
                   TRUE);
        UpdateHorizontalScrollbar();
        MoveWindow(preview,
                   right_x + editor_border,
                   editor_y + editor_border,
                   editor_content_width,
                   editor_content_height,
                   TRUE);
        const int status_base_y = editor_y + editor_height;
        const int status_y = status_base_y + (conflict_visible ? 0 : pad / 2);
        MoveWindow(status_bar,
                   right_x,
                   status_y,
                   std::max(0, right_width - button - gap - detail::ScaleForDpi(110, dpi)),
                   status_height,
                   TRUE);
        MoveWindow(encoding_selector,
                   right_x +
                       std::max(0, right_width - button - gap - detail::ScaleForDpi(104, dpi)),
                   status_y + detail::ScaleForDpi(3, dpi),
                   detail::ScaleForDpi(96, dpi),
                   std::max(1, status_height - detail::ScaleForDpi(6, dpi)),
                   TRUE);

        if (conflict_visible) {
            const int conflict_y = status_base_y + status_height + gap;
            conflict_splitter_y = conflict_y - gap / 2;
        } else {
            conflict_splitter_y = 0;
        }
    }
};

EditorPage::EditorPage() : impl_(std::make_unique<Impl>()) {}

EditorPage::~EditorPage() {
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
    const std::array file_columns{
        table::Column{L"名称", detail::ScaleForDpi(220, dpi), detail::ScaleForDpi(180, dpi)},
        table::Column{L"状态", detail::ScaleForDpi(100, dpi), detail::ScaleForDpi(78, dpi)},
        table::Column{L"大小", detail::ScaleForDpi(88, dpi), detail::ScaleForDpi(72, dpi)},
        table::Column{L"修改时间", detail::ScaleForDpi(132, dpi), detail::ScaleForDpi(112, dpi)},
        table::Column{L"权限", detail::ScaleForDpi(92, dpi), detail::ScaleForDpi(80, dpi)},
        table::Column{L"所有者", detail::ScaleForDpi(110, dpi), detail::ScaleForDpi(90, dpi)}};
    table::SetColumns(impl_->file_list, file_columns);
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
                          GetWindowLongPtrW(impl_->editor, GWL_STYLE) | WS_CLIPCHILDREN);
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
    impl_->preview = CreateWindowExW(0,
                                     L"EDIT",
                                     impl_->scintilla_available
                                         ? L""
                                         : L"未找到 Scintilla.dll。\r\n请将 Scintilla.dll 和 "
                                           L"Lexilla.dll 放在应用程序目录。",
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE |
                                         ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL,
                                     0,
                                     0,
                                     0,
                                     0,
                                     impl_->hwnd,
                                     nullptr,
                                     instance,
                                     nullptr);
    impl_->status_bar = CreateWindowExW(0,
                                        L"STATIC",
                                        L"未打开文件",
                                        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE |
                                            SS_ENDELLIPSIS,
                                        0,
                                        0,
                                        0,
                                        0,
                                        impl_->hwnd,
                                        nullptr,
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

    for (HWND control : {impl_->path_frame,
                         impl_->path_edit,
                         impl_->file_list,
                         impl_->editor,
                         impl_->preview,
                         impl_->status_bar,
                         impl_->encoding_selector}) {
        detail::ApplySelfDrawnTheme(control);
        detail::SetControlFont(control, impl_->font);
    }
    table::ApplyTheme(impl_->file_list);
    detail::SetControlFont(impl_->path_edit, impl_->title_font);
    detail::SetControlFont(impl_->preview, impl_->mono_font);
    for (HWND control : {impl_->editor, impl_->preview, impl_->file_list, impl_->path_edit}) {
        impl_->Subclass(control);
    }
    SetDirectoryState(
        {ViewPhase::Idle, L"双击侧栏服务器以打开文件", ViewRecoveryAction::None});
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
    RedrawWindow(impl_->hwnd,
                 nullptr,
                 nullptr,
                 RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

void EditorPage::ApplyTheme() {
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
        EnableWindow(impl_->encoding_selector, !saving);
    }
    InvalidateRect(impl_->hwnd, nullptr, FALSE);
    if (impl_->active_id && *impl_->active_id == id) {
        impl_->UpdateStatus();
    }
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
    document->conflict.clear();
    if (impl_->active_id && *impl_->active_id == id && impl_->encoding_selector) {
        EnableWindow(impl_->encoding_selector, true);
    }
    InvalidateRect(impl_->hwnd, nullptr, FALSE);
    impl_->UpdateStatus();
    impl_->UpdateConflictBar();
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
