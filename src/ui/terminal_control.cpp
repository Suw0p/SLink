// 终端自绘控件实现：接收网络字节、维护终端模型、DirectWrite 单元格绘制、选择/回滚、
// IME、剪贴板、鼠标协议及尺寸变更通知。
#include "ui/terminal_control.hpp"
#include "core/perf_trace.hpp"

#include "terminal/libvterm_adapter.hpp"
#include "terminal/terminal_input.hpp"
#include "ui/retained_ui.hpp"
#include "ui/win32_ui_internal.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <cwctype>
#include <d2d1.h>
#include <deque>
#include <dwrite.h>
#include <imm.h>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <windowsx.h>

#pragma comment(lib, "Imm32.lib")

#ifndef SLINK_USE_LIBVTERM
#error "SLink terminal controls must be compiled with libvterm enabled"
#endif

#ifndef WM_DPICHANGED_AFTERPARENT
#define WM_DPICHANGED_AFTERPARENT 0x02E3
#endif

namespace SLink::ui {
namespace {

constexpr wchar_t kTerminalControlClass[] = L"SLink.TerminalControl";
constexpr UINT kFeedMessage = WM_APP + 0x351;
constexpr UINT_PTR kResizeCommitTimerId = 1;
constexpr UINT kResizeCommitDelayMs = 120;
constexpr std::size_t kMaximumPendingFeedBytes = 2U * 1024U * 1024U;
COLORREF TerminalForeground() noexcept {
    return theme::TerminalForegroundColor();
}

COLORREF TerminalBackground() noexcept {
    return theme::TerminalBackgroundColor();
}

COLORREF TerminalSelectionForeground() noexcept {
    return theme::IsDark() ? RGB(245, 248, 250) : RGB(20, 20, 20);
}

COLORREF TerminalSelectionBackground() noexcept {
    return theme::kSelectedRowColor;
}

std::string WideToUtf8(std::wstring_view value) {
    if (value.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8,
                                         WC_ERR_INVALID_CHARS,
                                         value.data(),
                                         static_cast<int>(value.size()),
                                         nullptr,
                                         0,
                                         nullptr,
                                         nullptr);
    if (size <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8,
                        WC_ERR_INVALID_CHARS,
                        value.data(),
                        static_cast<int>(value.size()),
                        result.data(),
                        size,
                        nullptr,
                        nullptr);
    return result;
}

std::wstring CodepointText(char32_t codepoint) {
    if (codepoint == 0 || codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
        return {};
    }
    if (codepoint <= 0xFFFF) {
        return std::wstring(1, static_cast<wchar_t>(codepoint));
    }
    codepoint -= 0x10000;
    std::wstring result;
    result.push_back(static_cast<wchar_t>(0xD800 + ((codepoint >> 10) & 0x3FF)));
    result.push_back(static_cast<wchar_t>(0xDC00 + (codepoint & 0x3FF)));
    return result;
}

bool NeedsCjkFont(char32_t codepoint) noexcept {
    return (codepoint >= 0x2E80 && codepoint <= 0x4DBF) ||
           (codepoint >= 0x4E00 && codepoint <= 0x9FFF) ||
           (codepoint >= 0x3040 && codepoint <= 0x30FF) ||
           (codepoint >= 0xAC00 && codepoint <= 0xD7AF) ||
           (codepoint >= 0xF900 && codepoint <= 0xFAFF) ||
           (codepoint >= 0xFF00 && codepoint <= 0xFFEF);
}

COLORREF IndexedColor(std::uint8_t index) {
    static constexpr std::array<COLORREF, 16> colors = {
        RGB(0, 0, 0),
        RGB(205, 49, 49),
        RGB(13, 188, 121),
        RGB(229, 229, 16),
        RGB(36, 114, 200),
        RGB(188, 63, 188),
        RGB(17, 168, 205),
        RGB(229, 229, 229),
        RGB(102, 102, 102),
        RGB(241, 76, 76),
        RGB(35, 209, 139),
        RGB(245, 245, 67),
        RGB(59, 142, 234),
        RGB(214, 112, 214),
        RGB(41, 184, 219),
        RGB(255, 255, 255),
    };
    if (index < colors.size()) {
        return colors[index];
    }
    if (index < 232) {
        const int value = index - 16;
        const int red = value / 36;
        const int green = (value / 6) % 6;
        const int blue = value % 6;
        const auto channel = [](int component) {
            return component == 0 ? 0 : 55 + component * 40;
        };
        return RGB(channel(red), channel(green), channel(blue));
    }
    const int gray = 8 + (index - 232) * 10;
    return RGB(gray, gray, gray);
}

COLORREF ResolveColor(const terminal::TerminalColor& color, COLORREF default_color) {
    switch (color.kind) {
    case terminal::TerminalColor::Kind::Indexed:
        return IndexedColor(color.index);
    case terminal::TerminalColor::Kind::Rgb:
        return RGB(color.red, color.green, color.blue);
    case terminal::TerminalColor::Kind::Default:
    default:
        return default_color;
    }
}

std::optional<std::string> DecodeBase64(std::string_view encoded) {
    if (encoded.empty() || encoded.size() > 64 * 1024) {
        return std::nullopt;
    }
    static constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve(encoded.size() * 3 / 4);
    std::uint32_t accumulator = 0;
    int bits = 0;
    bool padding = false;
    for (const unsigned char character : encoded) {
        if (character == '=') {
            padding = true;
            continue;
        }
        if (character == '\r' || character == '\n' || character == ' ' || character == '\t') {
            continue;
        }
        if (padding) {
            return std::nullopt;
        }
        const std::size_t position = alphabet.find(character);
        if (position == std::string_view::npos) {
            return std::nullopt;
        }
        accumulator = (accumulator << 6) | static_cast<std::uint32_t>(position);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            result.push_back(static_cast<char>((accumulator >> bits) & 0xFF));
        }
    }
    return result;
}

bool WriteUtf8Clipboard(HWND owner, std::string_view utf8) {
    if (utf8.empty()) {
        return false;
    }
    const int length = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (length <= 0) {
        return false;
    }
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8,
                        MB_ERR_INVALID_CHARS,
                        utf8.data(),
                        static_cast<int>(utf8.size()),
                        wide.data(),
                        length);
    if (!OpenClipboard(owner)) {
        return false;
    }
    EmptyClipboard();
    const SIZE_T bytes = (wide.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) {
        CloseClipboard();
        return false;
    }
    void* destination = GlobalLock(memory);
    if (!destination) {
        GlobalFree(memory);
        CloseClipboard();
        return false;
    }
    memcpy(destination, wide.c_str(), bytes);
    GlobalUnlock(memory);
    if (!SetClipboardData(CF_UNICODETEXT, memory)) {
        GlobalFree(memory);
        CloseClipboard();
        return false;
    }
    CloseClipboard();
    return true;
}

} // namespace

// 控件模型和 GDI/DirectWrite 资源只由 ui_thread 修改。Feed 可跨线程调用，但只把字节
// 复制进 pending_feeds 并投递一次消息；UI 批量消费后再解析和重绘，避免高吞吐输出
// 造成每个网络包一次 WM_PAINT。回滚位置非零时新键盘输入会主动回到底部。
struct TerminalControl::Impl {
    struct TextPoint {
        std::size_t line{};
        std::size_t column{};
        friend bool operator==(const TextPoint&, const TextPoint&) = default;
    };

    // 终端队列同时承载内部 UTF-8 提示和远端传输字节。远端数据在入队时记录
    // 当前会话编码，确保编码切换前已到达的数据仍按旧编码顺序解码。
    enum class FeedKind {
        utf8,
        transport,
        encoding_switch,
    };

    struct PendingFeed {
        FeedKind kind{FeedKind::utf8};
        terminal::TerminalEncoding encoding{terminal::TerminalEncoding::Utf8};
        std::string bytes;
    };

    HINSTANCE instance{};
    HWND hwnd{};
    DWORD ui_thread{};
    HFONT font{};
    HFONT cjk_font{};
    HDC buffer_dc{};
    HBITMAP buffer_bitmap{};
    HBITMAP buffer_previous_bitmap{};
    int buffer_width{};
    int buffer_height{};
    HBRUSH background_brush{CreateSolidBrush(TerminalBackground())};
    HBRUSH caret_brush{CreateSolidBrush(RGB(92, 207, 130))};
    IDWriteTextFormat* mono_formats[2]{};
    IDWriteTextFormat* cjk_formats[2]{};
    int cell_width{8};
    int cell_height{16};
    terminal::DefaultTerminalModel model{24, 80};
    InputCallback input_callback;
    ResizeCallback resize_callback;
    std::optional<std::pair<std::size_t, std::size_t>> pending_pty_resize;
    Osc52PromptCallback osc52_callback;
    std::size_t view_offset{};
    bool selecting{};
    // 选区端点采用“包含首尾单元格”的模型。单字符选区的两个端点相同，因此需要单独
    // 记录鼠标是否真的发生过拖动，避免把普通单击误画成一个字符的选区。
    bool selection_has_extent{};
    POINT selection_anchor_pixel{};
    std::optional<TextPoint> selection_start;
    std::optional<TextPoint> selection_end;
    bool remote_left_down{};
    std::optional<std::pair<std::size_t, std::size_t>> last_remote_mouse_cell;
    wchar_t pending_high_surrogate{};
    std::atomic<terminal::TerminalEncoding> transport_encoding{terminal::TerminalEncoding::Utf8};
    terminal::TerminalSessionCodec output_codec;
    terminal::TerminalSessionCodec input_codec;
    std::mutex feed_mutex;
    // 队列设有字节上限；超限时记录 overflow 并触发安全重同步，而不是无限增长内存。
    std::deque<PendingFeed> pending_feeds;
    std::size_t pending_feed_bytes{};
    bool pending_feed_overflow{};
    bool shutting_down{};

    Impl() {
        model.set_event_callback([this](const terminal::TerminalEvent& event) {
            OnTerminalEvent(event);
        });
    }

    ~Impl() {
        ReleaseBackBuffer();
        for (IDWriteTextFormat*& format : mono_formats) {
            if (format) {
                format->Release();
                format = nullptr;
            }
        }
        for (IDWriteTextFormat*& format : cjk_formats) {
            if (format) {
                format->Release();
                format = nullptr;
            }
        }
        DeleteObject(background_brush);
        DeleteObject(caret_brush);
        DeleteObject(font);
        DeleteObject(cjk_font);
    }

    void ReleaseBackBuffer() noexcept {
        if (buffer_dc && buffer_previous_bitmap) {
            SelectObject(buffer_dc, buffer_previous_bitmap);
        }
        if (buffer_bitmap) {
            DeleteObject(buffer_bitmap);
        }
        if (buffer_dc) {
            DeleteDC(buffer_dc);
        }
        buffer_dc = nullptr;
        buffer_bitmap = nullptr;
        buffer_previous_bitmap = nullptr;
        buffer_width = 0;
        buffer_height = 0;
    }

    void ProcessPendingFeeds() {
        std::deque<PendingFeed> pending;
        bool overflow{};
        {
            std::scoped_lock lock(feed_mutex);
            pending.swap(pending_feeds);
            pending_feed_bytes = 0;
            overflow = std::exchange(pending_feed_overflow, false);
        }
        if (overflow) {
            FeedUtf8OnUiThread("\r\n[SLink：终端输出过快，"
                               "已丢弃最早的待绘制数据以限制内存占用。]\r\n");
        }
        std::size_t interactive_bytes = 0;
        for (const auto& item : pending) {
            interactive_bytes += item.bytes.size();
            if (item.kind == FeedKind::encoding_switch) {
                // SetEncoding 会清掉旧字符集未完成的尾部。事件与传输数据使用同一
                // 队列排序，因此切换不会反向影响已经到达的旧编码字节。
                output_codec.SetEncoding(item.encoding);
                continue;
            }
            if (item.kind == FeedKind::utf8) {
                FeedUtf8OnUiThread(item.bytes);
                continue;
            }
            // 每个数据块在入队时记录当时的字符集，因此切换之前已到达的字节仍按旧
            // 字符集处理，切换后的块才会重置解码器并使用新字符集。
            output_codec.SetEncoding(item.encoding);
            if (std::string decoded = output_codec.DecodeTransport(item.bytes); !decoded.empty()) {
                FeedUtf8OnUiThread(decoded);
            }
        }

        constexpr std::size_t kImmediateInteractivePaintBytes = 4096;
        if (interactive_bytes != 0 && interactive_bytes <= kImmediateInteractivePaintBytes &&
            hwnd && IsWindowVisible(hwnd)) {
            RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
        }
    }

    void QueueEncodingSwitch(const terminal::TerminalEncoding encoding) {
        if (!hwnd) {
            transport_encoding.store(encoding, std::memory_order_release);
            return;
        }
        const bool on_ui_thread = GetCurrentThreadId() == ui_thread;
        bool notification_needed{};
        {
            std::scoped_lock lock(feed_mutex);
            if (shutting_down || !hwnd) {
                return;
            }
            const terminal::TerminalEncoding previous =
                transport_encoding.exchange(encoding, std::memory_order_acq_rel);
            if (previous == encoding) {
                return;
            }
            notification_needed = pending_feeds.empty();
            pending_feeds.push_back({FeedKind::encoding_switch, encoding, {}});
        }
        if (on_ui_thread) {
            ProcessPendingFeeds();
            return;
        }
        if (notification_needed && !PostMessageW(hwnd, kFeedMessage, 0, 0)) {
            std::scoped_lock lock(feed_mutex);
            pending_feeds.clear();
            pending_feed_bytes = 0;
        }
    }

    void QueueFeed(const FeedKind kind, const std::string_view bytes) {
        if (!hwnd || bytes.empty()) {
            return;
        }
        const bool on_ui_thread = GetCurrentThreadId() == ui_thread;
        bool notification_needed{};
        {
            std::scoped_lock lock(feed_mutex);
            if (shutting_down || !hwnd) {
                return;
            }
            notification_needed = pending_feeds.empty();
            if (bytes.size() >= kMaximumPendingFeedBytes) {
                pending_feeds.clear();
                pending_feed_bytes = 0;
                pending_feeds.push_back(
                    {kind,
                     transport_encoding.load(std::memory_order_acquire),
                     std::string(bytes.substr(bytes.size() - kMaximumPendingFeedBytes))});
                pending_feed_bytes = kMaximumPendingFeedBytes;
                pending_feed_overflow = true;
            } else {
                while (!pending_feeds.empty() &&
                       pending_feed_bytes + bytes.size() > kMaximumPendingFeedBytes) {
                    pending_feed_bytes -= pending_feeds.front().bytes.size();
                    pending_feeds.pop_front();
                    pending_feed_overflow = true;
                }
                pending_feeds.push_back(
                    {kind, transport_encoding.load(std::memory_order_acquire), std::string(bytes)});
                pending_feed_bytes += bytes.size();
            }
        }
        if (on_ui_thread) {
            ProcessPendingFeeds();
            return;
        }
        if (notification_needed && !PostMessageW(hwnd, kFeedMessage, 0, 0)) {
            std::scoped_lock lock(feed_mutex);
            pending_feeds.clear();
            pending_feed_bytes = 0;
        }
    }

    void ApplyTheme() {
        DeleteObject(background_brush);
        background_brush = CreateSolidBrush(TerminalBackground());
        ReleaseBackBuffer();
        InvalidateRect(hwnd, nullptr, TRUE);
    }

    bool EnsureD2dResources() {
        if (!mono_formats[0]) {
            RecreateDirectWriteFormats();
        }
        return mono_formats[0] && cjk_formats[0];
    }

    void RecreateDirectWriteFormats() {
        IDWriteFactory* dwrite_factory = detail::SharedDWriteFactory();
        if (!dwrite_factory) {
            return;
        }
        for (IDWriteTextFormat*& format : mono_formats) {
            if (format) {
                format->Release();
                format = nullptr;
            }
        }
        for (IDWriteTextFormat*& format : cjk_formats) {
            if (format) {
                format->Release();
                format = nullptr;
            }
        }
        const float size =
            static_cast<float>(MulDiv(11, static_cast<int>(detail::WindowDpi(hwnd)), 72));
        for (int bold = 0; bold < 2; ++bold) {
            const DWRITE_FONT_WEIGHT weight =
                bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL;
            dwrite_factory->CreateTextFormat(L"Consolas",
                                             nullptr,
                                             weight,
                                             DWRITE_FONT_STYLE_NORMAL,
                                             DWRITE_FONT_STRETCH_NORMAL,
                                             size,
                                             L"en-US",
                                             &mono_formats[bold]);
            dwrite_factory->CreateTextFormat(L"Microsoft YaHei UI",
                                             nullptr,
                                             weight,
                                             DWRITE_FONT_STYLE_NORMAL,
                                             DWRITE_FONT_STRETCH_NORMAL,
                                             size,
                                             L"zh-CN",
                                             &cjk_formats[bold]);
        }
        for (IDWriteTextFormat* format :
             {mono_formats[0], mono_formats[1], cjk_formats[0], cjk_formats[1]}) {
            if (format) {
                format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                format->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM,
                                       static_cast<float>(cell_height),
                                       static_cast<float>(cell_height) * 0.78F);
            }
        }
    }

    bool EnsureBackBuffer(HDC reference, int width, int height) {
        width = std::max(1, width);
        height = std::max(1, height);
        if (buffer_dc && buffer_bitmap && buffer_width == width && buffer_height == height) {
            return true;
        }
        ReleaseBackBuffer();
        buffer_dc = CreateCompatibleDC(reference);
        if (!buffer_dc) {
            return false;
        }
        buffer_bitmap = CreateCompatibleBitmap(reference, width, height);
        if (!buffer_bitmap) {
            ReleaseBackBuffer();
            return false;
        }
        buffer_previous_bitmap = reinterpret_cast<HBITMAP>(SelectObject(buffer_dc, buffer_bitmap));
        buffer_width = width;
        buffer_height = height;
        return true;
    }

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            self = static_cast<Impl*>(create->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->hwnd = window;
            self->ui_thread = GetCurrentThreadId();
        }
        if (!self) {
            return DefWindowProcW(window, message, wparam, lparam);
        }
        switch (message) {
        case WM_CREATE:
            self->RecreateFont();
            return 0;
        case WM_SIZE:
            self->ResizeModel(LOWORD(lparam), HIWORD(lparam));
            return 0;
        case WM_TIMER:
            if (wparam == kResizeCommitTimerId) {
                self->CommitPendingPtyResize();
                return 0;
            }
            break;
        case WM_DPICHANGED_AFTERPARENT:
            self->RecreateFont();
            self->ResizeFromClient();
            InvalidateRect(window, nullptr, TRUE);
            return 0;
        case WM_PAINT:
            self->Paint();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_GETDLGCODE:
            return DLGC_WANTALLKEYS | DLGC_WANTCHARS | DLGC_WANTARROWS;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (self->OnKeyDown(wparam)) {
                return 0;
            }
            break;
        case WM_CHAR:
            self->OnCharacter(static_cast<wchar_t>(wparam));
            return 0;
        case WM_SYSCHAR:
            self->OnSystemCharacter(static_cast<wchar_t>(wparam));
            return 0;
        case WM_IME_COMPOSITION:
            if (lparam & GCS_RESULTSTR) {
                self->OnImeResult();
                return 0;
            }
            break;
        case WM_MOUSEWHEEL: {
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            ScreenToClient(window, &point);
            self->OnMouseWheel(GET_WHEEL_DELTA_WPARAM(wparam), point.x, point.y);
        }
            return 0;
        case WM_LBUTTONDOWN:
            SetFocus(window);
            if (self->BeginRemoteMouse(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam))) {
                SetCapture(window);
                return 0;
            }
            SetCapture(window);
            self->selecting = true;
            self->selection_has_extent = false;
            self->selection_anchor_pixel = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            self->selection_start =
                self->PointFromMouse(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
            self->selection_end = self->selection_start;
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_MOUSEMOVE:
            if (self->OnRemoteMouseMove(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam))) {
                return 0;
            }
            if (self->selecting) {
                const int x = GET_X_LPARAM(lparam);
                const int y = GET_Y_LPARAM(lparam);
                self->selection_has_extent = self->selection_has_extent ||
                                             x != self->selection_anchor_pixel.x ||
                                             y != self->selection_anchor_pixel.y;
                self->selection_end = self->PointFromMouse(x, y);
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONUP:
            if (self->EndRemoteMouse(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam))) {
                ReleaseCapture();
                return 0;
            }
            if (self->selecting) {
                self->selecting = false;
                const int x = GET_X_LPARAM(lparam);
                const int y = GET_Y_LPARAM(lparam);
                self->selection_has_extent = self->selection_has_extent ||
                                             x != self->selection_anchor_pixel.x ||
                                             y != self->selection_anchor_pixel.y;
                self->selection_end = self->PointFromMouse(x, y);
                ReleaseCapture();
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;
        case WM_MBUTTONDOWN:
        case WM_RBUTTONDOWN:
            SetFocus(window);
            if (self->RemoteMouseButton(message == WM_MBUTTONDOWN ? 1 : 2,
                                        GET_X_LPARAM(lparam),
                                        GET_Y_LPARAM(lparam),
                                        false)) {
                return 0;
            }
            break;
        case WM_MBUTTONUP:
        case WM_RBUTTONUP:
            if (self->RemoteMouseButton(message == WM_MBUTTONUP ? 1 : 2,
                                        GET_X_LPARAM(lparam),
                                        GET_Y_LPARAM(lparam),
                                        true)) {
                return 0;
            }
            break;
        case WM_SETCURSOR:
            SetCursor(LoadCursorW(nullptr, IDC_IBEAM));
            return TRUE;
        case kFeedMessage: {
            self->ProcessPendingFeeds();
            return 0;
        }
        case WM_NCDESTROY:
            KillTimer(window, kResizeCommitTimerId);
            self->pending_pty_resize.reset();
            {
                std::scoped_lock lock(self->feed_mutex);
                self->shutting_down = true;
                self->pending_feeds.clear();
                self->pending_feed_bytes = 0;
            }
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            self->hwnd = nullptr;
            break;
        default:
            break;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    static bool Register(HINSTANCE instance) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
        wc.lpfnWndProc = &WindowProc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_IBEAM);
        // 画布自己按当前主题完整绘制，窗口类不持有可能在切换主题后过期的背景刷。
        wc.hbrBackground = nullptr;
        wc.lpszClassName = kTerminalControlClass;
        const ATOM atom = RegisterClassExW(&wc);
        return atom != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }

    void RecreateFont() {
        DeleteObject(font);
        DeleteObject(cjk_font);
        LOGFONTW description{};
        description.lfHeight = -MulDiv(11, static_cast<int>(detail::WindowDpi(hwnd)), 72);
        description.lfWeight = FW_NORMAL;
        description.lfQuality = CLEARTYPE_QUALITY;
        description.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
        wcscpy_s(description.lfFaceName, L"Consolas");
        font = CreateFontIndirectW(&description);
        description.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
        wcscpy_s(description.lfFaceName, L"Microsoft YaHei UI");
        cjk_font = CreateFontIndirectW(&description);

        HDC dc = GetDC(hwnd);
        const HFONT previous = reinterpret_cast<HFONT>(SelectObject(dc, font));
        SIZE size{};
        GetTextExtentPoint32W(dc, L"M", 1, &size);
        TEXTMETRICW metrics{};
        GetTextMetricsW(dc, &metrics);
        cell_width = std::max(1L, size.cx);
        cell_height = std::max(1L, metrics.tmHeight + metrics.tmExternalLeading);
        SelectObject(dc, previous);
        ReleaseDC(hwnd, dc);
        if (EnsureD2dResources()) {
            RecreateDirectWriteFormats();
        }
    }

    const terminal::TerminalBuffer& State() const noexcept {
#ifdef SLINK_USE_LIBVTERM
        return model.fallback_state();
#else
        return model;
#endif
    }

    std::size_t HistorySize() const noexcept {
        return State().scrollback_line_count();
    }

    const std::deque<terminal::TerminalLine>& History() const noexcept {
        return State().scrollback();
    }

    std::size_t RowCount() const noexcept {
        return model.rows();
    }

    std::size_t ColumnCount() const noexcept {
        return model.columns();
    }

    bool BracketedPaste() const noexcept {
        return State().bracketed_paste_enabled();
    }

    std::size_t ViewStart() const noexcept {
        const std::size_t history = HistorySize();
        return history >= view_offset ? history - view_offset : 0;
    }

    const terminal::TerminalLine* LineAt(std::size_t absolute_line) const {
        const std::size_t history = HistorySize();
        if (absolute_line < history) {
            return &History()[absolute_line];
        }
        const std::size_t screen_row = absolute_line - history;
        const auto& screen = model.screen();
        return screen_row < screen.size() ? &screen[screen_row] : nullptr;
    }

    bool IsSelected(std::size_t line, std::size_t column) const {
        if (!selection_has_extent || !selection_start || !selection_end) {
            return false;
        }
        TextPoint begin = *selection_start;
        TextPoint end = *selection_end;
        if (end.line < begin.line || (end.line == begin.line && end.column < begin.column)) {
            std::swap(begin, end);
        }
        if (line < begin.line || line > end.line) {
            return false;
        }
        if (begin.line == end.line) {
            return column >= begin.column && column <= end.column;
        }
        if (line == begin.line) {
            return column >= begin.column;
        }
        if (line == end.line) {
            return column <= end.column;
        }
        return true;
    }

    void Paint() {
        core::PerfScope timing("terminal_paint");
        if (!EnsureD2dResources() || !mono_formats[0] || !cjk_formats[0]) {
            PaintGdi();
            return;
        }
        model.synchronize_cursor_row();
        PAINTSTRUCT paint{};
        HDC window_dc = BeginPaint(hwnd, &paint);
        RECT client{};
        GetClientRect(hwnd, &client);
        const bool new_buffer = !buffer_bitmap || buffer_width != client.right ||
                                buffer_height != client.bottom;
        if (!EnsureBackBuffer(window_dc, client.right, client.bottom)) {
            EndPaint(hwnd, &paint);
            return;
        }
        const RECT dirty = new_buffer ? client : paint.rcPaint;
        // Render the complete terminal into an off-screen bitmap. Readline
        // repeatedly shifts a suffix by one cell while Backspace/Delete is
        // held; clearing and drawing directly on the window exposes those
        // intermediate frames and can leave the old final glyph behind.
        FillRect(buffer_dc, &dirty, background_brush);
        const detail::SharedDcRenderContext context =
            detail::BindSharedDcRenderTarget(buffer_dc, client);
        if (!context) {
            EndPaint(hwnd, &paint);
            return;
        }
        ID2D1DCRenderTarget* render_target = context.target;
        ID2D1SolidColorBrush* d2d_brush = context.brush;
        render_target->BeginDraw();
        render_target->SetTransform(D2D1::Matrix3x2F::Identity());
        render_target->PushAxisAlignedClip(
            D2D1::RectF(static_cast<float>(dirty.left), static_cast<float>(dirty.top),
                        static_cast<float>(dirty.right), static_cast<float>(dirty.bottom)),
            D2D1_ANTIALIAS_MODE_ALIASED);
        const COLORREF terminal_background = TerminalBackground();
        d2d_brush->SetColor(D2D1::ColorF(GetRValue(terminal_background) / 255.0F,
                                         GetGValue(terminal_background) / 255.0F,
                                         GetBValue(terminal_background) / 255.0F));
        render_target->FillRectangle(
            D2D1::RectF(static_cast<float>(dirty.left), static_cast<float>(dirty.top),
                        static_cast<float>(dirty.right), static_cast<float>(dirty.bottom)),
            d2d_brush);

        const std::size_t start = ViewStart();
        const std::size_t first_row =
            static_cast<std::size_t>(std::max(0L, dirty.top) / cell_height);
        const std::size_t last_row =
            std::min(RowCount(), static_cast<std::size_t>(
                                     (std::max(0L, dirty.bottom) + cell_height - 1) / cell_height));

        // 第一遍只绘制背景。中文等宽字符会跨两个终端列，若在同一循环中交替绘制
        // “背景、字形”，后续单元格的背景可能覆盖前一个字形的右侧像素，选区背景下
        // 尤其明显。先完成整屏背景，再绘制所有字形，可确保宽字符不会被后续背景擦除。
        for (std::size_t row = first_row; row < last_row; ++row) {
            const std::size_t absolute_line = start + row;
            const terminal::TerminalLine* line = LineAt(absolute_line);
            if (!line) {
                continue;
            }
            const std::size_t visible_columns = std::min(ColumnCount(), line->cells.size());
            for (std::size_t column = 0; column < visible_columns; ++column) {
                const terminal::TerminalCell& cell = line->cells[column];
                // A width-zero cell is the continuation half of the wide
                // glyph immediately to its left. The leading cell already
                // paints both columns. Painting this cell's background would
                // erase the right half of the glyph.
                if (cell.width == 0) {
                    continue;
                }
                COLORREF foreground =
                    ResolveColor(cell.attributes.foreground, TerminalForeground());
                COLORREF background = ResolveColor(cell.attributes.background, terminal_background);
                if (cell.attributes.inverse) {
                    std::swap(foreground, background);
                }
                if (IsSelected(absolute_line, column)) {
                    background = TerminalSelectionBackground();
                }
                const float left = static_cast<float>(column * cell_width);
                const float top = static_cast<float>(row * cell_height);
                const int display_columns =
                    std::max(std::max<int>(1, cell.width), NeedsCjkFont(cell.codepoint) ? 2 : 1);
                const float right = static_cast<float>((column + display_columns) * cell_width);
                const float bottom = static_cast<float>((row + 1) * cell_height);
                const D2D1_RECT_F cell_rect = D2D1::RectF(left, top, right, bottom);
                if (background != terminal_background) {
                    d2d_brush->SetColor(D2D1::ColorF(GetRValue(background) / 255.0F,
                                                     GetGValue(background) / 255.0F,
                                                     GetBValue(background) / 255.0F));
                    render_target->FillRectangle(cell_rect, d2d_brush);
                }
            }
        }

        // 第二遍只绘制文字和装饰线，背景已经稳定，不再有后绘制背景覆盖宽字形的问题。
        for (std::size_t row = first_row; row < last_row; ++row) {
            const std::size_t absolute_line = start + row;
            const terminal::TerminalLine* line = LineAt(absolute_line);
            if (!line) {
                continue;
            }
            const std::size_t visible_columns = std::min(ColumnCount(), line->cells.size());
            for (std::size_t column = 0; column < visible_columns; ++column) {
                const terminal::TerminalCell& cell = line->cells[column];
                if (cell.width == 0) {
                    continue;
                }
                COLORREF foreground =
                    ResolveColor(cell.attributes.foreground, TerminalForeground());
                COLORREF background = ResolveColor(cell.attributes.background, terminal_background);
                if (cell.attributes.inverse) {
                    std::swap(foreground, background);
                }
                if (IsSelected(absolute_line, column)) {
                    foreground = TerminalSelectionForeground();
                }
                const float left = static_cast<float>(column * cell_width);
                const float top = static_cast<float>(row * cell_height);
                const int display_columns =
                    std::max(std::max<int>(1, cell.width), NeedsCjkFont(cell.codepoint) ? 2 : 1);
                const float right = static_cast<float>((column + display_columns) * cell_width);
                const float bottom = static_cast<float>((row + 1) * cell_height);
                const D2D1_RECT_F cell_rect = D2D1::RectF(left, top, right, bottom);
                const std::wstring text = CodepointText(cell.codepoint);
                if (!text.empty() && cell.codepoint != U' ') {
                    const int bold = cell.attributes.bold ? 1 : 0;
                    IDWriteTextFormat* format =
                        NeedsCjkFont(cell.codepoint) ? cjk_formats[bold] : mono_formats[bold];
                    d2d_brush->SetColor(D2D1::ColorF(GetRValue(foreground) / 255.0F,
                                                     GetGValue(foreground) / 255.0F,
                                                     GetBValue(foreground) / 255.0F));
                    render_target->DrawTextW(text.c_str(),
                                             static_cast<UINT32>(text.size()),
                                             format,
                                             cell_rect,
                                             d2d_brush,
                                             D2D1_DRAW_TEXT_OPTIONS_CLIP);
                }
                if (cell.attributes.underline) {
                    d2d_brush->SetColor(D2D1::ColorF(GetRValue(foreground) / 255.0F,
                                                     GetGValue(foreground) / 255.0F,
                                                     GetBValue(foreground) / 255.0F));
                    render_target->DrawLine(D2D1::Point2F(left, bottom - 2.0F),
                                            D2D1::Point2F(right, bottom - 2.0F),
                                            d2d_brush,
                                            1.0F);
                }
            }
        }
        if (view_offset == 0 && model.cursor().visible && GetFocus() == hwnd) {
            const terminal::CursorPosition cursor = model.cursor();
            d2d_brush->SetColor(D2D1::ColorF(0x5ccf82));
            render_target->FillRectangle(
                D2D1::RectF(static_cast<float>(cursor.column * cell_width),
                            static_cast<float>(cursor.row * cell_height + cell_height - 2),
                            static_cast<float>((cursor.column + 1) * cell_width),
                            static_cast<float>((cursor.row + 1) * cell_height)),
                d2d_brush);
        }
        render_target->PopAxisAlignedClip();
        const HRESULT result = render_target->EndDraw();
        if (SUCCEEDED(result)) {
            BitBlt(window_dc, dirty.left, dirty.top, dirty.right - dirty.left,
                   dirty.bottom - dirty.top, buffer_dc, dirty.left, dirty.top, SRCCOPY);
        } else if (result == D2DERR_RECREATE_TARGET) {
            detail::ResetSharedDcRenderTarget();
            InvalidateRect(hwnd, nullptr, FALSE);
        } else {
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        EndPaint(hwnd, &paint);
    }

    void PaintGdi() {
        model.synchronize_cursor_row();
        PAINTSTRUCT paint{};
        HDC window_dc = BeginPaint(hwnd, &paint);
        RECT client{};
        GetClientRect(hwnd, &client);
        if (!EnsureBackBuffer(window_dc, client.right, client.bottom)) {
            EndPaint(hwnd, &paint);
            return;
        }
        HFONT previous_font = reinterpret_cast<HFONT>(SelectObject(buffer_dc, font));
        HFONT active_font = font;
        FillRect(buffer_dc, &client, background_brush);
        SetBkMode(buffer_dc, OPAQUE);
        const COLORREF terminal_background = TerminalBackground();

        const std::size_t start = ViewStart();
        const std::size_t visible_rows = std::min(
            RowCount(), static_cast<std::size_t>(std::max(0L, client.bottom) / cell_height));

        // GDI 备用路径同样先画完背景，避免 ETO_OPAQUE 在后续单元格中擦掉宽字形。
        for (std::size_t row = 0; row < visible_rows; ++row) {
            const std::size_t absolute_line = start + row;
            const terminal::TerminalLine* line = LineAt(absolute_line);
            if (!line) {
                continue;
            }
            const std::size_t visible_columns = std::min(ColumnCount(), line->cells.size());
            for (std::size_t column = 0; column < visible_columns; ++column) {
                const terminal::TerminalCell& cell = line->cells[column];
                if (cell.width == 0) {
                    continue;
                }
                COLORREF foreground =
                    ResolveColor(cell.attributes.foreground, TerminalForeground());
                COLORREF cell_background =
                    ResolveColor(cell.attributes.background, terminal_background);
                if (cell.attributes.inverse) {
                    std::swap(foreground, cell_background);
                }
                if (IsSelected(absolute_line, column)) {
                    cell_background = TerminalSelectionBackground();
                }
                if (cell_background == terminal_background) {
                    continue;
                }
                const int display_columns =
                    std::max(std::max<int>(1, cell.width), NeedsCjkFont(cell.codepoint) ? 2 : 1);
                RECT cell_rect{static_cast<LONG>(column * cell_width),
                               static_cast<LONG>(row * cell_height),
                               static_cast<LONG>((column + display_columns) * cell_width),
                               static_cast<LONG>((row + 1) * cell_height)};
                HBRUSH brush = CreateSolidBrush(cell_background);
                FillRect(buffer_dc, &cell_rect, brush);
                DeleteObject(brush);
            }
        }

        SetBkMode(buffer_dc, TRANSPARENT);
        for (std::size_t row = 0; row < visible_rows; ++row) {
            const std::size_t absolute_line = start + row;
            const terminal::TerminalLine* line = LineAt(absolute_line);
            if (!line) {
                continue;
            }
            const std::size_t visible_columns = std::min(ColumnCount(), line->cells.size());
            for (std::size_t column = 0; column < visible_columns; ++column) {
                const terminal::TerminalCell& cell = line->cells[column];
                if (cell.width == 0 || cell.codepoint == U' ') {
                    continue;
                }
                COLORREF foreground =
                    ResolveColor(cell.attributes.foreground, TerminalForeground());
                COLORREF cell_background =
                    ResolveColor(cell.attributes.background, terminal_background);
                if (cell.attributes.inverse) {
                    std::swap(foreground, cell_background);
                }
                if (IsSelected(absolute_line, column)) {
                    foreground = TerminalSelectionForeground();
                }
                HFONT desired_font = NeedsCjkFont(cell.codepoint) && cjk_font ? cjk_font : font;
                if (desired_font != active_font) {
                    SelectObject(buffer_dc, desired_font);
                    active_font = desired_font;
                }
                SetTextColor(buffer_dc, foreground);
                const int display_columns =
                    std::max(std::max<int>(1, cell.width), NeedsCjkFont(cell.codepoint) ? 2 : 1);
                RECT cell_rect{static_cast<LONG>(column * cell_width),
                               static_cast<LONG>(row * cell_height),
                               static_cast<LONG>((column + display_columns) * cell_width),
                               static_cast<LONG>((row + 1) * cell_height)};
                const std::wstring text = CodepointText(cell.codepoint);
                ExtTextOutW(buffer_dc,
                            cell_rect.left,
                            cell_rect.top,
                            ETO_CLIPPED,
                            &cell_rect,
                            text.c_str(),
                            static_cast<UINT>(text.size()),
                            nullptr);
            }
        }
        if (view_offset == 0 && model.cursor().visible && GetFocus() == hwnd) {
            const terminal::CursorPosition cursor = model.cursor();
            RECT caret{static_cast<LONG>(cursor.column * cell_width),
                       static_cast<LONG>(cursor.row * cell_height + cell_height - 2),
                       static_cast<LONG>((cursor.column + 1) * cell_width),
                       static_cast<LONG>((cursor.row + 1) * cell_height)};
            FillRect(buffer_dc, &caret, caret_brush);
        }
        BitBlt(window_dc, 0, 0, client.right, client.bottom, buffer_dc, 0, 0, SRCCOPY);
        SelectObject(buffer_dc, previous_font);
        EndPaint(hwnd, &paint);
    }

    void ResizeFromClient() {
        RECT client{};
        GetClientRect(hwnd, &client);
        ResizeModel(client.right, client.bottom);
    }

    void ResizeModel(int width, int height) {
        if (cell_width <= 0 || cell_height <= 0) {
            return;
        }
        // Layout changes can briefly collapse a terminal control to a few
        // pixels while its parent window or split geometry is being updated.
        // Sending that transient 1-column size to the remote PTY makes shells
        // reflow their output into one-character lines. Keep the last stable
        // grid until the control is large enough to be a usable terminal.
        constexpr std::size_t kMinimumStableColumns = 8;
        constexpr std::size_t kMinimumStableRows = 2;
        if (width < cell_width * static_cast<int>(kMinimumStableColumns) ||
            height < cell_height * static_cast<int>(kMinimumStableRows)) {
            return;
        }
        const std::size_t columns = std::max<std::size_t>(
            kMinimumStableColumns, static_cast<std::size_t>(std::max(0, width) / cell_width));
        const std::size_t rows = std::max<std::size_t>(
            kMinimumStableRows, static_cast<std::size_t>(std::max(0, height) / cell_height));
        const std::pair requested_size{rows, columns};
        if (rows == RowCount() && columns == ColumnCount()) {
            // The window may return to its original grid before the debounce
            // timer fires. In that case the queued intermediate size must not
            // be sent to the PTY after resizing has already finished.
            if (pending_pty_resize && *pending_pty_resize != requested_size) {
                KillTimer(hwnd, kResizeCommitTimerId);
                pending_pty_resize.reset();
            }
            return;
        }
        if (resize_callback) {
            // Keep the local libvterm grid and the remote PTY at the same size.
            // Resizing libvterm on every WM_SIZE while the PTY still uses the
            // old dimensions repeatedly reflows the same ANSI cells and can
            // turn styled blanks into visible colour blocks. The client area
            // itself is still repainted immediately; only grid reflow waits
            // for the final, debounced size.
            pending_pty_resize = requested_size;
            KillTimer(hwnd, kResizeCommitTimerId);
            if (SetTimer(hwnd, kResizeCommitTimerId, kResizeCommitDelayMs, nullptr) == 0) {
                CommitPendingPtyResize();
            }
        } else {
            model.resize(rows, columns);
            view_offset = std::min(view_offset, HistorySize());
        }
        InvalidateRect(hwnd, nullptr, FALSE);
    }

    void CommitPendingPtyResize() {
        if (hwnd) {
            KillTimer(hwnd, kResizeCommitTimerId);
        }
        if (!pending_pty_resize) {
            return;
        }
        const auto [rows, columns] = *pending_pty_resize;
        pending_pty_resize.reset();
        model.resize(rows, columns);
        view_offset = std::min(view_offset, HistorySize());
        if (resize_callback) {
            resize_callback(rows, columns);
        }
        InvalidateRect(hwnd, nullptr, FALSE);
    }

    void FeedUtf8OnUiThread(std::string_view bytes) {
        model.feed(bytes);
#ifdef SLINK_USE_LIBVTERM
        model.synchronize_cursor_row();
        const auto damage = model.take_damage();
#endif
        if (auto response = model.take_output(); !response.empty()) {
            // Protocol replies generated by the terminal model are not user
            // input and must not pull a user away from scrollback history.
            EmitProtocolBytes(response, false);
        }
        view_offset = std::min(view_offset, HistorySize());
#ifdef SLINK_USE_LIBVTERM
        if (view_offset == 0 && !damage.full) {
            RECT client{};
            GetClientRect(hwnd, &client);
            for (const auto row : damage.rows) {
                const LONG top = static_cast<LONG>(row * cell_height);
                RECT changed{client.left, top, client.right, top + cell_height};
                InvalidateRect(hwnd, &changed, FALSE);
            }
            return;
        }
#endif
        InvalidateRect(hwnd, nullptr, FALSE);
    }

    void EmitProtocolBytes(std::string_view bytes, bool reveal_current_line = true) {
        if (input_callback && !bytes.empty()) {
            if (reveal_current_line && view_offset != 0) {
                view_offset = 0;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            input_callback({bytes, TerminalInputOrigin::Protocol});
        }
    }

    void EmitUserBytes(std::string_view bytes, bool reveal_current_line = true) {
        if (input_callback && !bytes.empty()) {
            if (reveal_current_line && view_offset != 0) {
                view_offset = 0;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            input_callback({bytes, TerminalInputOrigin::User});
        }
    }

    bool EmitTextInput(const std::wstring_view text, const bool reveal_current_line = true) {
        input_codec.SetEncoding(transport_encoding.load(std::memory_order_acquire));
        auto encoded = input_codec.EncodeText(text);
        if (!encoded) {
            MessageBoxW(
                hwnd, encoded.status().message().c_str(), L"终端字符集", MB_OK | MB_ICONWARNING);
            return false;
        }
        EmitUserBytes(encoded.value(), reveal_current_line);
        return true;
    }

    static int XtermModifier(bool shift, bool alt, bool control) noexcept {
        return 1 + (shift ? 1 : 0) + (alt ? 2 : 0) + (control ? 4 : 0);
    }

    void EmitCursorKey(char final, bool shift, bool alt, bool control) {
        const int modifier = XtermModifier(shift, alt, control);
        if (modifier == 1) {
            const char sequence[]{
                '\x1b', State().application_cursor_keys_enabled() ? 'O' : '[', final};
            EmitUserBytes(std::string_view(sequence, sizeof(sequence)));
            return;
        }
        std::string sequence = "\x1b[1;";
        sequence += std::to_string(modifier);
        sequence.push_back(final);
        EmitUserBytes(sequence);
    }

    void EmitTildeKey(int code, bool shift, bool alt, bool control) {
        std::string sequence = "\x1b[";
        sequence += std::to_string(code);
        const int modifier = XtermModifier(shift, alt, control);
        if (modifier != 1) {
            sequence.push_back(';');
            sequence += std::to_string(modifier);
        }
        sequence.push_back('~');
        EmitUserBytes(sequence);
    }

    void EmitFunctionKey(int number, bool shift, bool alt, bool control) {
        const int modifier = XtermModifier(shift, alt, control);
        if (number >= 1 && number <= 4) {
            const char final = static_cast<char>('P' + number - 1);
            if (modifier == 1) {
                const char sequence[]{'\x1b', 'O', final};
                EmitUserBytes(std::string_view(sequence, sizeof(sequence)));
            } else {
                std::string sequence = "\x1b[1;";
                sequence += std::to_string(modifier);
                sequence.push_back(final);
                EmitUserBytes(sequence);
            }
            return;
        }
        constexpr int codes[]{15, 17, 18, 19, 20, 21, 23, 24};
        if (number >= 5 && number <= 12) {
            EmitTildeKey(codes[number - 5], shift, alt, control);
        }
    }

    bool OnKeyDown(WPARAM key) {
        const bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
        if ((control && shift && key == 'C') || (control && key == VK_INSERT)) {
            CopySelectionToClipboard();
            return true;
        }
        if ((control && shift && key == 'V') || (shift && key == VK_INSERT)) {
            PasteFromClipboard();
            return true;
        }
        if (control && key == 'C' &&
            (!selection_has_extent || !selection_start || !selection_end)) {
            EmitUserBytes(std::string_view("\x03", 1));
            return true;
        }
        if (shift && (key == VK_PRIOR || key == VK_NEXT)) {
            const std::size_t page = RowCount() > 1 ? RowCount() - 1 : 1;
            if (key == VK_PRIOR) {
                view_offset = std::min(HistorySize(), view_offset + page);
            } else {
                view_offset = view_offset > page ? view_offset - page : 0;
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return true;
        }
        switch (key) {
        case VK_UP:
            EmitCursorKey('A', shift, alt, control);
            return true;
        case VK_DOWN:
            EmitCursorKey('B', shift, alt, control);
            return true;
        case VK_RIGHT:
            EmitCursorKey('C', shift, alt, control);
            return true;
        case VK_LEFT:
            EmitCursorKey('D', shift, alt, control);
            return true;
        case VK_HOME:
            EmitCursorKey('H', shift, alt, control);
            return true;
        case VK_END:
            EmitCursorKey('F', shift, alt, control);
            return true;
        case VK_INSERT:
            EmitTildeKey(2, shift, alt, control);
            return true;
        case VK_DELETE:
            EmitTildeKey(3, shift, alt, control);
            return true;
        case VK_PRIOR:
            EmitTildeKey(5, shift, alt, control);
            return true;
        case VK_NEXT:
            EmitTildeKey(6, shift, alt, control);
            return true;
        case VK_TAB:
            if (shift) {
                EmitUserBytes("\x1b[Z");
                return true;
            }
            return false;
        case VK_F1:
        case VK_F2:
        case VK_F3:
        case VK_F4:
        case VK_F5:
        case VK_F6:
        case VK_F7:
        case VK_F8:
        case VK_F9:
        case VK_F10:
        case VK_F11:
        case VK_F12:
            EmitFunctionKey(static_cast<int>(key - VK_F1 + 1), shift, alt, control);
            return true;
        default:
            return false;
        }
    }

    void OnCharacter(wchar_t character) {
        const bool control_down = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool shift_down = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        if (terminal::IsClipboardShortcutCharacter(character, control_down, shift_down)) {
            return;
        }
        if (character == L'\r') {
            EmitUserBytes("\r");
            return;
        }
        if (character == L'\b') {
            EmitUserBytes(std::string_view("\x7f", 1));
            return;
        }
        if (character < 0x20) {
            if (character != L'\0') {
                const char control = static_cast<char>(character);
                EmitUserBytes(std::string_view(&control, 1));
            }
            return;
        }
        std::wstring text;
        if (character >= 0xD800 && character <= 0xDBFF) {
            pending_high_surrogate = character;
            return;
        }
        if (character >= 0xDC00 && character <= 0xDFFF && pending_high_surrogate) {
            text.push_back(pending_high_surrogate);
            pending_high_surrogate = 0;
        } else {
            pending_high_surrogate = 0;
        }
        text.push_back(character);
        EmitTextInput(text);
    }

    void OnSystemCharacter(wchar_t character) {
        // AltGr is reported as Ctrl+Alt on many keyboard layouts and
        // represents a printable character rather than an xterm Meta key.
        const bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (!control) {
            EmitUserBytes("\x1b");
        }
        OnCharacter(character);
    }

    void OnImeResult() {
        HIMC context = ImmGetContext(hwnd);
        if (!context) {
            return;
        }
        const LONG bytes = ImmGetCompositionStringW(context, GCS_RESULTSTR, nullptr, 0);
        if (bytes > 0) {
            std::wstring text(static_cast<std::size_t>(bytes) / sizeof(wchar_t), L'\0');
            ImmGetCompositionStringW(context, GCS_RESULTSTR, text.data(), bytes);
            EmitTextInput(text);
        }
        ImmReleaseContext(hwnd, context);
    }

    TextPoint PointFromMouse(int x, int y) const {
        const std::size_t row =
            std::min(RowCount() - 1, static_cast<std::size_t>(std::max(0, y) / cell_height));
        std::size_t column =
            std::min(ColumnCount() - 1, static_cast<std::size_t>(std::max(0, x) / cell_width));
        const std::size_t line_index = ViewStart() + row;
        // 中文等双列字符的右半部分是 width==0 的占位单元格。鼠标落在右半部分时必须
        // 回退到实际字形的起始列，否则选区端点会切进字符中间，出现只高亮/显示半个字。
        if (const terminal::TerminalLine* line = LineAt(line_index);
            line && column < line->cells.size() && line->cells[column].width == 0 && column > 0) {
            do {
                --column;
            } while (column > 0 && line->cells[column].width == 0);
        }
        return {line_index, column};
    }

    bool RemoteMouseEnabled() const noexcept {
        return view_offset == 0 && State().mouse_mode() != terminal::MouseMode::None &&
               (GetKeyState(VK_SHIFT) & 0x8000) == 0;
    }

    std::pair<std::size_t, std::size_t> MouseCell(int x, int y) const {
        const std::size_t column =
            std::min(ColumnCount(), static_cast<std::size_t>(std::max(0, x) / cell_width) + 1);
        const std::size_t row =
            std::min(RowCount(), static_cast<std::size_t>(std::max(0, y) / cell_height) + 1);
        return {column, row};
    }

    void EmitMouse(int button, int x, int y, bool release, bool motion = false) {
        const auto [column, row] = MouseCell(x, y);
        int code = button + (motion ? 32 : 0);
        if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) {
            code += 16;
        }
        if ((GetKeyState(VK_MENU) & 0x8000) != 0) {
            code += 8;
        }
        if (State().sgr_mouse_encoding_enabled()) {
            std::string sequence = "\x1b[<";
            sequence += std::to_string(code);
            sequence += ';';
            sequence += std::to_string(column);
            sequence += ';';
            sequence += std::to_string(row);
            sequence += release ? 'm' : 'M';
            EmitProtocolBytes(sequence);
            return;
        }
        if (release) {
            code = 3;
        }
        const auto encoded = [](int value) {
            return static_cast<char>(std::clamp(value + 32, 32, 255));
        };
        std::string sequence{"\x1b[M", 3};
        sequence.push_back(encoded(code));
        sequence.push_back(encoded(static_cast<int>(std::min<std::size_t>(column, 223))));
        sequence.push_back(encoded(static_cast<int>(std::min<std::size_t>(row, 223))));
        EmitProtocolBytes(sequence);
    }

    bool RemoteMouseButton(int button, int x, int y, bool release) {
        if (!RemoteMouseEnabled()) {
            return false;
        }
        selection_start.reset();
        selection_end.reset();
        selection_has_extent = false;
        EmitMouse(button, x, y, release);
        return true;
    }

    bool BeginRemoteMouse(int x, int y) {
        if (!RemoteMouseEnabled()) {
            return false;
        }
        selection_start.reset();
        selection_end.reset();
        selection_has_extent = false;
        remote_left_down = true;
        last_remote_mouse_cell = MouseCell(x, y);
        EmitMouse(0, x, y, false);
        InvalidateRect(hwnd, nullptr, FALSE);
        return true;
    }

    bool OnRemoteMouseMove(int x, int y) {
        if (!RemoteMouseEnabled()) {
            return false;
        }
        const auto mode = State().mouse_mode();
        if (mode == terminal::MouseMode::Normal ||
            (mode == terminal::MouseMode::ButtonEvent && !remote_left_down)) {
            return remote_left_down;
        }
        const auto cell = MouseCell(x, y);
        if (cell == last_remote_mouse_cell) {
            return true;
        }
        last_remote_mouse_cell = cell;
        EmitMouse(remote_left_down ? 0 : 3, x, y, false, true);
        return true;
    }

    bool EndRemoteMouse(int x, int y) {
        if (!remote_left_down) {
            return false;
        }
        remote_left_down = false;
        last_remote_mouse_cell = MouseCell(x, y);
        if (RemoteMouseEnabled()) {
            EmitMouse(0, x, y, true);
        }
        return true;
    }

    void OnMouseWheel(short delta, int x, int y) {
        if (RemoteMouseEnabled()) {
            const int steps = std::max(1, abs(delta) / WHEEL_DELTA);
            for (int index = 0; index < steps; ++index) {
                EmitMouse(delta > 0 ? 64 : 65, x, y, false);
            }
            return;
        }
        const int lines = std::max(1, abs(delta) / WHEEL_DELTA * 3);
        if (delta > 0) {
            view_offset = std::min(HistorySize(), view_offset + static_cast<std::size_t>(lines));
        } else {
            view_offset = view_offset > static_cast<std::size_t>(lines)
                              ? view_offset - static_cast<std::size_t>(lines)
                              : 0;
        }
        InvalidateRect(hwnd, nullptr, FALSE);
    }

    std::wstring SearchableLine(std::size_t absolute_line) const {
        const auto* line = LineAt(absolute_line);
        if (!line) {
            return {};
        }
        std::wstring result;
        result.reserve(line->cells.size());
        for (const auto& cell : line->cells) {
            if (cell.width != 0) {
                result += CodepointText(cell.codepoint);
            }
        }
        return result;
    }

    bool Find(std::wstring_view query, bool backwards) {
        if (query.empty()) {
            return false;
        }
        const std::size_t total_lines = HistorySize() + model.screen().size();
        if (total_lines == 0) {
            return false;
        }
        std::wstring needle(query);
        std::ranges::transform(needle, needle.begin(), [](wchar_t value) {
            return static_cast<wchar_t>(std::towlower(value));
        });

        std::size_t start_line = selection_end ? selection_end->line : ViewStart();
        start_line = std::min(start_line, total_lines - 1);
        for (std::size_t step = 0; step < total_lines; ++step) {
            const std::size_t line_index = backwards
                                               ? (start_line + total_lines - step) % total_lines
                                               : (start_line + step) % total_lines;
            std::wstring haystack = SearchableLine(line_index);
            std::ranges::transform(haystack, haystack.begin(), [](wchar_t value) {
                return static_cast<wchar_t>(std::towlower(value));
            });

            std::size_t position = std::wstring::npos;
            if (backwards) {
                const std::size_t before =
                    step == 0 && selection_start && selection_start->line == line_index
                        ? selection_start->column
                        : haystack.size();
                position = haystack.rfind(needle, before == 0 ? 0 : before - 1);
            } else {
                const std::size_t after =
                    step == 0 && selection_end && selection_end->line == line_index
                        ? selection_end->column + 1
                        : 0;
                position = haystack.find(needle, after);
            }
            if (position == std::wstring::npos) {
                continue;
            }
            selection_start = TextPoint{line_index, position};
            selection_end = TextPoint{line_index, position + needle.size() - 1};
            selection_has_extent = true;
            const std::size_t history = HistorySize();
            if (line_index < history) {
                view_offset = history - line_index;
            } else {
                view_offset = 0;
            }
            view_offset = std::min(view_offset, history);
            InvalidateRect(hwnd, nullptr, FALSE);
            return true;
        }
        MessageBeep(MB_ICONINFORMATION);
        return false;
    }

    std::wstring SelectedText() const {
        if (!selection_has_extent || !selection_start || !selection_end) {
            return {};
        }
        TextPoint begin = *selection_start;
        TextPoint end = *selection_end;
        if (end.line < begin.line || (end.line == begin.line && end.column < begin.column)) {
            std::swap(begin, end);
        }
        std::wstring result;
        for (std::size_t line_index = begin.line; line_index <= end.line; ++line_index) {
            const terminal::TerminalLine* line = LineAt(line_index);
            if (!line) {
                continue;
            }
            const std::size_t first = line_index == begin.line ? begin.column : 0;
            const std::size_t last =
                std::min(line->cells.size() - 1,
                         line_index == end.line ? end.column : line->cells.size() - 1);
            std::wstring line_text;
            for (std::size_t column = first; column <= last; ++column) {
                if (line->cells[column].width == 0) {
                    continue;
                }
                line_text += CodepointText(line->cells[column].codepoint);
            }
            while (!line_text.empty() && line_text.back() == L' ') {
                line_text.pop_back();
            }
            result += line_text;
            if (line_index != end.line) {
                result += L"\r\n";
            }
        }
        return result;
    }

    void CopySelectionToClipboard() {
        const std::wstring text = SelectedText();
        if (text.empty() || !OpenClipboard(hwnd)) {
            return;
        }
        EmptyClipboard();
        const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (memory) {
            if (void* destination = GlobalLock(memory)) {
                memcpy(destination, text.c_str(), bytes);
                GlobalUnlock(memory);
                if (!SetClipboardData(CF_UNICODETEXT, memory)) {
                    GlobalFree(memory);
                }
            } else {
                GlobalFree(memory);
            }
        }
        CloseClipboard();
    }

    void PasteFromClipboard() {
        if (!OpenClipboard(hwnd)) {
            return;
        }
        std::wstring normalized;
        HANDLE data = GetClipboardData(CF_UNICODETEXT);
        if (data) {
            const auto* text = static_cast<const wchar_t*>(GlobalLock(data));
            if (text) {
                normalized = terminal::NormalizePasteLineEndings(text);
                GlobalUnlock(data);
            }
        }
        CloseClipboard();
        if (normalized.empty()) {
            return;
        }

        input_codec.SetEncoding(transport_encoding.load(std::memory_order_acquire));
        auto encoded = input_codec.EncodeText(normalized);
        if (!encoded) {
            MessageBoxW(
                hwnd, encoded.status().message().c_str(), L"终端字符集", MB_OK | MB_ICONWARNING);
            return;
        }
        std::string payload = std::move(encoded).value();
        if (BracketedPaste()) {
            payload.insert(0, "\x1b[200~");
            payload.append("\x1b[201~");
        }
        EmitUserBytes(payload);
    }

    void OnTerminalEvent(const terminal::TerminalEvent& event) {
        if (event.type != terminal::TerminalEventType::Osc52ClipboardRequest ||
            event.encoded_data == "?") {
            return;
        }
        bool approved = false;
        if (osc52_callback) {
            approved = osc52_callback(event);
        } else {
            approved = MessageBoxW(hwnd,
                                   L"远程服务器请求写入 Windows 剪贴板。\r\n"
                                   L"是否允许本次操作？",
                                   L"SLink · OSC 52 安全确认",
                                   MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES;
        }
        if (!approved) {
            return;
        }
        const auto decoded = DecodeBase64(event.encoded_data);
        if (decoded) {
            WriteUtf8Clipboard(hwnd, *decoded);
        }
    }
};

TerminalControl::TerminalControl() : impl_(std::make_unique<Impl>()) {}
TerminalControl::~TerminalControl() {
    if (impl_->hwnd && IsWindow(impl_->hwnd)) {
        DestroyWindow(impl_->hwnd);
    }
}

bool TerminalControl::Create(HWND parent, HINSTANCE instance, int control_id) {
    impl_->instance = instance;
    if (!Impl::Register(instance)) {
        return false;
    }
    impl_->hwnd = CreateWindowExW(0,
                                  kTerminalControlClass,
                                  L"",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS,
                                  0,
                                  0,
                                  0,
                                  0,
                                  parent,
                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(control_id)),
                                  instance,
                                  impl_.get());
    return impl_->hwnd != nullptr;
}

void TerminalControl::SetBounds(int x, int y, int width, int height) {
    MoveWindow(impl_->hwnd, x, y, width, height, TRUE);
}

void TerminalControl::Show(bool visible) {
    if (!visible) {
        impl_->ReleaseBackBuffer();
    }
    ShowWindow(impl_->hwnd, visible ? SW_SHOW : SW_HIDE);
    if (visible) {
        InvalidateRect(impl_->hwnd, nullptr, FALSE);
    }
}

void TerminalControl::ApplyTheme() {
    impl_->ApplyTheme();
}

void TerminalControl::FeedTransportBytes(const std::string_view bytes) {
    impl_->QueueFeed(Impl::FeedKind::transport, bytes);
}

void TerminalControl::FeedUtf8(const std::string_view bytes) {
    impl_->QueueFeed(Impl::FeedKind::utf8, bytes);
}

void TerminalControl::SetTransportEncoding(const terminal::TerminalEncoding encoding) {
    impl_->QueueEncodingSwitch(encoding);
}

terminal::TerminalEncoding TerminalControl::transport_encoding() const noexcept {
    return impl_->transport_encoding.load(std::memory_order_acquire);
}

void TerminalControl::SetInputCallback(InputCallback callback) {
    impl_->input_callback = std::move(callback);
}

void TerminalControl::SetResizeCallback(ResizeCallback callback) {
    impl_->resize_callback = std::move(callback);
}

void TerminalControl::SetOsc52PromptCallback(Osc52PromptCallback callback) {
    impl_->osc52_callback = std::move(callback);
}

void TerminalControl::CopySelection() {
    impl_->CopySelectionToClipboard();
}
void TerminalControl::PasteClipboard() {
    impl_->PasteFromClipboard();
}
bool TerminalControl::FindNext(std::wstring_view query, bool backwards) {
    return impl_->Find(query, backwards);
}
HWND TerminalControl::hwnd() const noexcept {
    return impl_->hwnd;
}
std::size_t TerminalControl::rows() const noexcept {
    return impl_->RowCount();
}
std::size_t TerminalControl::columns() const noexcept {
    return impl_->ColumnCount();
}

} // namespace SLink::ui
