#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace SLink::terminal {

struct TerminalColor {
    enum class Kind : std::uint8_t {
        Default,
        Indexed,
        Rgb,
    };

    Kind kind{Kind::Default};
    std::uint8_t index{};
    std::uint8_t red{};
    std::uint8_t green{};
    std::uint8_t blue{};

    static TerminalColor Indexed(std::uint8_t value) noexcept;
    static TerminalColor Rgb(std::uint8_t red, std::uint8_t green, std::uint8_t blue) noexcept;
    friend bool operator==(const TerminalColor&, const TerminalColor&) = default;
};

struct CellAttributes {
    TerminalColor foreground;
    TerminalColor background;
    std::uint8_t bold : 1 {};
    std::uint8_t italic : 1 {};
    std::uint8_t underline : 1 {};
    std::uint8_t inverse : 1 {};
    std::uint8_t strike : 1 {};

    friend bool operator==(const CellAttributes&, const CellAttributes&) = default;
};

struct TerminalCell {
    char32_t codepoint{U' '};
    CellAttributes attributes;
    std::uint8_t width{1};

    friend bool operator==(const TerminalCell&, const TerminalCell&) = default;
};

// 回滚区可能保存数百万个单元格，修改字段时必须关注结构体大小，避免每个终端额外
// 占用数十 MB。
static_assert(sizeof(TerminalCell) <= 16,
              "TerminalCell must remain compact because scrollback stores "
              "millions of cells");

struct TerminalLine {
    std::vector<TerminalCell> cells;
    bool wrapped{};
};

struct CursorPosition {
    std::size_t row{};
    std::size_t column{};
    bool visible{true};
};

enum class MouseMode {
    None,
    Normal,
    ButtonEvent,
    AnyEvent,
};

enum class TerminalEventType {
    Osc52ClipboardRequest,
};

struct TerminalEvent {
    TerminalEventType type{TerminalEventType::Osc52ClipboardRequest};
    std::string selection;
    std::string encoded_data;
};

using TerminalEventCallback = std::function<void(const TerminalEvent&)>;

// 有界终端模型。备用解析器只实现安全且常用的 ANSI/CSI 序列，未知或畸形控制序列
// 直接忽略。OSC 52 仅转为安全事件，本层绝不直接读写系统剪贴板。
// 行数和字节数同时设限，防止超长宽字符行绕过单纯的行数限制。
class TerminalBuffer final {
public:
    static constexpr std::size_t kMaximumScrollbackLines = 10'000;
    static constexpr std::size_t kMaximumScrollbackBytes = 32ULL * 1024ULL * 1024ULL;

    explicit TerminalBuffer(std::size_t rows = 24, std::size_t columns = 80);
    ~TerminalBuffer();

    TerminalBuffer(const TerminalBuffer&) = delete;
    TerminalBuffer& operator=(const TerminalBuffer&) = delete;
    TerminalBuffer(TerminalBuffer&&) noexcept;
    TerminalBuffer& operator=(TerminalBuffer&&) noexcept;

    void feed(std::string_view bytes);
    [[nodiscard]] std::string take_output();
    void resize(std::size_t rows, std::size_t columns);
    void reset();
    void set_event_callback(TerminalEventCallback callback);

    // libvterm 构建中仅跟踪安全相关模式、OSC 52 和有界回滚；可打印字节不再进入第二套
    // 屏幕解析器，回滚行由 libvterm 回调提供。
    void enable_state_tracking_only();
    void push_scrollback_line(TerminalLine line);

    [[nodiscard]] std::size_t rows() const noexcept;
    [[nodiscard]] std::size_t columns() const noexcept;
    [[nodiscard]] CursorPosition cursor() const noexcept;
    [[nodiscard]] const TerminalCell& cell(std::size_t row, std::size_t column) const;
    [[nodiscard]] const std::vector<TerminalLine>& screen() const noexcept;
    [[nodiscard]] const std::deque<TerminalLine>& scrollback() const noexcept;
    [[nodiscard]] std::size_t scrollback_line_count() const noexcept;
    [[nodiscard]] std::size_t scrollback_bytes() const noexcept;

    [[nodiscard]] bool alternate_screen_active() const noexcept;
    [[nodiscard]] bool bracketed_paste_enabled() const noexcept;
    [[nodiscard]] bool application_cursor_keys_enabled() const noexcept;
    [[nodiscard]] MouseMode mouse_mode() const noexcept;
    [[nodiscard]] bool sgr_mouse_encoding_enabled() const noexcept;

private:
    struct Impl;
    Impl* impl_;
};

} // namespace SLink::terminal
