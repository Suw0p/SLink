// 有界终端状态模型及备用 ANSI/CSI/OSC 解析器实现，包含备用屏幕、鼠标模式、
// Bracketed Paste、宽字符和双重回滚上限管理。
#include "terminal/terminal_buffer.hpp"

#include <algorithm>
#include <charconv>
#include <limits>
#include <stdexcept>
#include <utility>

namespace SLink::terminal {
namespace {

constexpr std::size_t kMaximumControlSequenceBytes = 64 * 1024;

std::size_t CodepointWidth(char32_t codepoint) noexcept {
    if (codepoint == 0) {
        return 0;
    }
    // Compact wcwidth approximation covering the common East Asian and emoji
    // ranges. Rendering can refine grapheme shaping without changing the model.
    if ((codepoint >= 0x1100 && codepoint <= 0x115F) ||
        (codepoint >= 0x2329 && codepoint <= 0x232A) ||
        (codepoint >= 0x2E80 && codepoint <= 0xA4CF) ||
        (codepoint >= 0xAC00 && codepoint <= 0xD7A3) ||
        (codepoint >= 0xF900 && codepoint <= 0xFAFF) ||
        (codepoint >= 0xFE10 && codepoint <= 0xFE6F) ||
        (codepoint >= 0xFF00 && codepoint <= 0xFF60) ||
        (codepoint >= 0x1F300 && codepoint <= 0x1FAFF) ||
        (codepoint >= 0x20000 && codepoint <= 0x3FFFD)) {
        return 2;
    }
    return 1;
}

std::vector<int> ParseParameters(std::string_view input) {
    std::vector<int> values;
    std::size_t begin = 0;
    for (;;) {
        const std::size_t end = input.find(';', begin);
        const std::string_view token =
            input.substr(begin, end == std::string_view::npos ? input.size() - begin : end - begin);
        int value = 0;
        if (!token.empty()) {
            const auto result = std::from_chars(token.data(), token.data() + token.size(), value);
            if (result.ec != std::errc{} || result.ptr != token.data() + token.size()) {
                value = 0;
            }
        }
        values.push_back(value);
        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1;
    }
    return values;
}

} // namespace

TerminalColor TerminalColor::Indexed(std::uint8_t value) noexcept {
    TerminalColor color;
    color.kind = Kind::Indexed;
    color.index = value;
    return color;
}

TerminalColor TerminalColor::Rgb(std::uint8_t red_value,
                                 std::uint8_t green_value,
                                 std::uint8_t blue_value) noexcept {
    TerminalColor color;
    color.kind = Kind::Rgb;
    color.red = red_value;
    color.green = green_value;
    color.blue = blue_value;
    return color;
}

struct TerminalBuffer::Impl {
    enum class ParseState {
        Ground,
        Escape,
        EscapeIntermediate,
        Csi,
        Osc,
        OscEscape,
        DiscardCsi,
        DiscardOsc,
        DiscardOscEscape,
    };

    std::size_t row_count;
    std::size_t column_count;
    std::vector<TerminalLine> main_screen;
    std::vector<TerminalLine> alternate_screen;
    CursorPosition main_cursor;
    CursorPosition alternate_cursor;
    CursorPosition saved_cursor;
    std::deque<TerminalLine> history;
    std::size_t history_bytes{};
    CellAttributes attributes;
    ParseState parse_state{ParseState::Ground};
    std::string control;
    std::uint32_t utf8_value{};
    std::uint32_t utf8_minimum{};
    std::uint8_t utf8_remaining{};
    bool alternate{};
    bool bracketed_paste{};
    bool application_cursor_keys{};
    MouseMode mouse{MouseMode::None};
    bool sgr_mouse{};
    bool wrap_pending{};
    bool state_tracking_only{};
    TerminalEventCallback event_callback;

    explicit Impl(std::size_t rows, std::size_t columns)
        : row_count(std::max<std::size_t>(1, rows)),
          column_count(std::max<std::size_t>(1, columns)),
          main_screen(MakeScreen(row_count, column_count)),
          alternate_screen(MakeScreen(row_count, column_count)) {}

    static TerminalLine BlankLine(std::size_t columns) {
        TerminalLine line;
        line.cells.resize(columns);
        return line;
    }

    static std::vector<TerminalLine> MakeScreen(std::size_t rows, std::size_t columns) {
        std::vector<TerminalLine> result;
        result.reserve(rows);
        for (std::size_t row = 0; row < rows; ++row) {
            result.push_back(BlankLine(columns));
        }
        return result;
    }

    std::vector<TerminalLine>& ActiveScreen() noexcept {
        return alternate ? alternate_screen : main_screen;
    }

    const std::vector<TerminalLine>& ActiveScreen() const noexcept {
        return alternate ? alternate_screen : main_screen;
    }

    CursorPosition& ActiveCursor() noexcept {
        return alternate ? alternate_cursor : main_cursor;
    }

    const CursorPosition& ActiveCursor() const noexcept {
        return alternate ? alternate_cursor : main_cursor;
    }

    static std::size_t LineBytes(const TerminalLine& line) noexcept {
        return sizeof(TerminalLine) + line.cells.capacity() * sizeof(TerminalCell);
    }

    void TrimHistory() {
        while (!history.empty() && (history.size() > TerminalBuffer::kMaximumScrollbackLines ||
                                    history_bytes > TerminalBuffer::kMaximumScrollbackBytes)) {
            history_bytes -= LineBytes(history.front());
            history.pop_front();
        }
    }

    void PushHistory(TerminalLine line) {
        if (alternate) {
            return;
        }
        const TerminalCell blank{};
        while (!line.cells.empty() && line.cells.back() == blank) {
            line.cells.pop_back();
        }
        if (line.cells.size() < line.cells.capacity()) {
            std::vector<TerminalCell> compact(line.cells.begin(), line.cells.end());
            line.cells.swap(compact);
        }
        history_bytes += LineBytes(line);
        history.push_back(std::move(line));
        TrimHistory();
    }

    void LineFeed() {
        wrap_pending = false;
        CursorPosition& cursor = ActiveCursor();
        auto& screen = ActiveScreen();
        if (cursor.row + 1 < row_count) {
            ++cursor.row;
            return;
        }
        TerminalLine top = std::move(screen.front());
        screen.erase(screen.begin());
        PushHistory(std::move(top));
        screen.push_back(BlankLine(column_count));
        cursor.row = row_count - 1;
    }

    void PutCodepoint(char32_t codepoint) {
        if (codepoint < 0x20 || codepoint == 0x7F) {
            return;
        }
        CursorPosition& cursor = ActiveCursor();
        auto& screen = ActiveScreen();
        if (wrap_pending) {
            screen[cursor.row].wrapped = true;
            cursor.column = 0;
            LineFeed();
        }
        const std::size_t width = std::min(CodepointWidth(codepoint), column_count);
        if (width == 2 && cursor.column + 1 >= column_count) {
            screen[cursor.row].wrapped = true;
            cursor.column = 0;
            LineFeed();
        }
        TerminalCell& first = screen[cursor.row].cells[cursor.column];
        first.codepoint = codepoint;
        first.attributes = attributes;
        first.width = static_cast<std::uint8_t>(width);
        if (width == 2) {
            TerminalCell& continuation = screen[cursor.row].cells[cursor.column + 1];
            continuation.codepoint = 0;
            continuation.attributes = attributes;
            continuation.width = 0;
        }
        cursor.column += std::max<std::size_t>(1, width);
        if (cursor.column >= column_count) {
            cursor.column = column_count - 1;
            wrap_pending = true;
        }
    }

    void ProcessUtf8Byte(unsigned char byte) {
        if (utf8_remaining != 0) {
            if ((byte & 0xC0) == 0x80) {
                utf8_value = (utf8_value << 6) | (byte & 0x3F);
                if (--utf8_remaining == 0) {
                    const char32_t value = static_cast<char32_t>(utf8_value);
                    const bool valid = utf8_value >= utf8_minimum && utf8_value <= 0x10FFFF &&
                                       !(utf8_value >= 0xD800 && utf8_value <= 0xDFFF);
                    PutCodepoint(valid ? value : U'\uFFFD');
                    utf8_value = 0;
                    utf8_minimum = 0;
                }
                return;
            }
            utf8_remaining = 0;
            utf8_value = 0;
            utf8_minimum = 0;
            PutCodepoint(U'\uFFFD');
            ProcessGroundByte(byte);
            return;
        }
        if (byte < 0x80) {
            ProcessGroundByte(byte);
        } else if (byte >= 0xC2 && byte <= 0xDF) {
            utf8_value = byte & 0x1F;
            utf8_minimum = 0x80;
            utf8_remaining = 1;
        } else if (byte >= 0xE0 && byte <= 0xEF) {
            utf8_value = byte & 0x0F;
            utf8_minimum = 0x800;
            utf8_remaining = 2;
        } else if (byte >= 0xF0 && byte <= 0xF4) {
            utf8_value = byte & 0x07;
            utf8_minimum = 0x10000;
            utf8_remaining = 3;
        } else {
            PutCodepoint(U'\uFFFD');
        }
    }

    void ProcessGroundByte(unsigned char byte) {
        switch (byte) {
        case 0x1B:
            parse_state = ParseState::Escape;
            control.clear();
            break;
        case '\r':
            ActiveCursor().column = 0;
            wrap_pending = false;
            break;
        case '\n':
            LineFeed();
            break;
        case '\b':
            if (ActiveCursor().column > 0) {
                --ActiveCursor().column;
            }
            wrap_pending = false;
            break;
        case '\t': {
            const std::size_t next =
                std::min(column_count - 1, (ActiveCursor().column / 8 + 1) * 8);
            ActiveCursor().column = next;
            wrap_pending = false;
            break;
        }
        default:
            PutCodepoint(static_cast<char32_t>(byte));
            break;
        }
    }

    void ProcessEscapeByte(unsigned char byte) {
        parse_state = ParseState::Ground;
        switch (byte) {
        case '[':
            control.clear();
            parse_state = ParseState::Csi;
            break;
        case ']':
            control.clear();
            parse_state = ParseState::Osc;
            break;
        case '(':
        case ')':
        case '*':
        case '+':
        case '-':
        case '.':
        case '/':
        case '%':
            // UTF-8 mode does not use the legacy G0-G3/character-set
            // designation, but the complete ESC sequence must still be
            // consumed. Treating its final byte (usually 'B') as text
            // produces one stray line per ncurses row refresh.
            parse_state = ParseState::EscapeIntermediate;
            break;
        case '7':
            saved_cursor = ActiveCursor();
            break;
        case '8':
            ActiveCursor() = saved_cursor;
            ClampCursor();
            break;
        case 'c':
            Reset();
            break;
        default:
            break;
        }
    }

    void ProcessEscapeIntermediateByte(unsigned char byte) {
        if (byte == 0x1B) {
            parse_state = ParseState::Escape;
            return;
        }
        if (byte >= 0x20 && byte <= 0x2F) {
            return;
        }
        // ECMA-48 escape sequences end with a byte in 0x30-0x7e. Invalid
        // bytes are also discarded so they can never leak into the screen.
        parse_state = ParseState::Ground;
    }

    void ProcessCsiByte(unsigned char byte) {
        if (byte >= 0x40 && byte <= 0x7E) {
            HandleCsi(static_cast<char>(byte), control);
            control.clear();
            parse_state = ParseState::Ground;
            return;
        }
        if (control.size() >= kMaximumControlSequenceBytes) {
            control.clear();
            parse_state = ParseState::DiscardCsi;
            return;
        }
        if (byte >= 0x20 && byte <= 0x3F) {
            control.push_back(static_cast<char>(byte));
        } else if (byte == 0x1B) {
            control.clear();
            parse_state = ParseState::Escape;
        }
    }

    void ProcessDiscardCsiByte(unsigned char byte) {
        if (byte >= 0x40 && byte <= 0x7E) {
            parse_state = ParseState::Ground;
        } else if (byte == 0x1B) {
            parse_state = ParseState::Escape;
        }
    }

    void ProcessOscByte(unsigned char byte) {
        if (byte == 0x07) {
            HandleOsc(control);
            control.clear();
            parse_state = ParseState::Ground;
        } else if (byte == 0x1B) {
            parse_state = ParseState::OscEscape;
        } else if (control.size() >= kMaximumControlSequenceBytes) {
            control.clear();
            parse_state = ParseState::DiscardOsc;
        } else {
            control.push_back(static_cast<char>(byte));
        }
    }

    void ProcessOscEscapeByte(unsigned char byte) {
        if (byte == '\\') {
            HandleOsc(control);
            control.clear();
            parse_state = ParseState::Ground;
        } else {
            if (control.size() + 2 >= kMaximumControlSequenceBytes) {
                control.clear();
                parse_state = ParseState::DiscardOsc;
                return;
            }
            control.push_back('\x1B');
            control.push_back(static_cast<char>(byte));
            parse_state = ParseState::Osc;
        }
    }

    void ProcessDiscardOscByte(unsigned char byte) {
        if (byte == 0x07) {
            parse_state = ParseState::Ground;
        } else if (byte == 0x1B) {
            parse_state = ParseState::DiscardOscEscape;
        }
    }

    void ProcessDiscardOscEscapeByte(unsigned char byte) {
        parse_state = byte == '\\' ? ParseState::Ground : ParseState::DiscardOsc;
    }

    void Feed(std::string_view bytes) {
        for (const unsigned char byte : bytes) {
            if (parse_state == ParseState::Ground) {
                if (state_tracking_only) {
                    if (byte == 0x1B) {
                        parse_state = ParseState::Escape;
                        control.clear();
                    }
                    continue;
                }
                ProcessUtf8Byte(byte);
                continue;
            }
            // An incomplete UTF-8 character cannot legally span into an ANSI
            // control sequence. Flush it before processing the control.
            if (utf8_remaining != 0) {
                utf8_remaining = 0;
                utf8_value = 0;
                utf8_minimum = 0;
                PutCodepoint(U'\uFFFD');
            }
            switch (parse_state) {
            case ParseState::Escape:
                ProcessEscapeByte(byte);
                break;
            case ParseState::EscapeIntermediate:
                ProcessEscapeIntermediateByte(byte);
                break;
            case ParseState::Csi:
                ProcessCsiByte(byte);
                break;
            case ParseState::Osc:
                ProcessOscByte(byte);
                break;
            case ParseState::OscEscape:
                ProcessOscEscapeByte(byte);
                break;
            case ParseState::DiscardCsi:
                ProcessDiscardCsiByte(byte);
                break;
            case ParseState::DiscardOsc:
                ProcessDiscardOscByte(byte);
                break;
            case ParseState::DiscardOscEscape:
                ProcessDiscardOscEscapeByte(byte);
                break;
            case ParseState::Ground:
                break;
            }
        }
    }

    void ClampCursor() {
        CursorPosition& cursor = ActiveCursor();
        cursor.row = std::min(cursor.row, row_count - 1);
        cursor.column = std::min(cursor.column, column_count - 1);
    }

    void MoveCursorRelative(int row_delta, int column_delta) {
        CursorPosition& cursor = ActiveCursor();
        const auto row = static_cast<long long>(cursor.row) + static_cast<long long>(row_delta);
        const auto column =
            static_cast<long long>(cursor.column) + static_cast<long long>(column_delta);
        cursor.row = static_cast<std::size_t>(std::clamp<long long>(row, 0, row_count - 1));
        cursor.column =
            static_cast<std::size_t>(std::clamp<long long>(column, 0, column_count - 1));
        wrap_pending = false;
    }

    void EraseLine(int mode) {
        auto& cells = ActiveScreen()[ActiveCursor().row].cells;
        const std::size_t column = ActiveCursor().column;
        const std::size_t begin = mode == 0 ? column : 0;
        const std::size_t end = mode == 1 ? column + 1 : cells.size();
        std::fill(cells.begin() + begin, cells.begin() + end, TerminalCell{});
    }

    void EraseDisplay(int mode) {
        auto& screen = ActiveScreen();
        if (mode == 2 || mode == 3) {
            for (auto& line : screen) {
                std::fill(line.cells.begin(), line.cells.end(), TerminalCell{});
                line.wrapped = false;
            }
            if (mode == 3) {
                history.clear();
                history_bytes = 0;
            }
            return;
        }
        if (mode == 0) {
            EraseLine(0);
            for (std::size_t row = ActiveCursor().row + 1; row < row_count; ++row) {
                std::fill(screen[row].cells.begin(), screen[row].cells.end(), TerminalCell{});
            }
        } else if (mode == 1) {
            EraseLine(1);
            for (std::size_t row = 0; row < ActiveCursor().row; ++row) {
                std::fill(screen[row].cells.begin(), screen[row].cells.end(), TerminalCell{});
            }
        }
    }

    void ApplySgr(const std::vector<int>& parameters) {
        const std::vector<int> reset_if_empty =
            parameters.empty() ? std::vector<int>{0} : parameters;
        for (std::size_t index = 0; index < reset_if_empty.size(); ++index) {
            const int value = reset_if_empty[index];
            if (value == 0) {
                attributes = {};
            } else if (value == 1) {
                attributes.bold = true;
            } else if (value == 3) {
                attributes.italic = true;
            } else if (value == 4) {
                attributes.underline = true;
            } else if (value == 7) {
                attributes.inverse = true;
            } else if (value == 9) {
                attributes.strike = true;
            } else if (value == 22) {
                attributes.bold = false;
            } else if (value == 23) {
                attributes.italic = false;
            } else if (value == 24) {
                attributes.underline = false;
            } else if (value == 27) {
                attributes.inverse = false;
            } else if (value == 29) {
                attributes.strike = false;
            } else if (value >= 30 && value <= 37) {
                attributes.foreground =
                    TerminalColor::Indexed(static_cast<std::uint8_t>(value - 30));
            } else if (value == 39) {
                attributes.foreground = {};
            } else if (value >= 40 && value <= 47) {
                attributes.background =
                    TerminalColor::Indexed(static_cast<std::uint8_t>(value - 40));
            } else if (value == 49) {
                attributes.background = {};
            } else if (value >= 90 && value <= 97) {
                attributes.foreground =
                    TerminalColor::Indexed(static_cast<std::uint8_t>(value - 82));
            } else if (value >= 100 && value <= 107) {
                attributes.background =
                    TerminalColor::Indexed(static_cast<std::uint8_t>(value - 92));
            } else if ((value == 38 || value == 48) && index + 2 < reset_if_empty.size() &&
                       reset_if_empty[index + 1] == 5) {
                const auto color = TerminalColor::Indexed(
                    static_cast<std::uint8_t>(std::clamp(reset_if_empty[index + 2], 0, 255)));
                (value == 38 ? attributes.foreground : attributes.background) = color;
                index += 2;
            } else if ((value == 38 || value == 48) && index + 4 < reset_if_empty.size() &&
                       reset_if_empty[index + 1] == 2) {
                const auto color = TerminalColor::Rgb(
                    static_cast<std::uint8_t>(std::clamp(reset_if_empty[index + 2], 0, 255)),
                    static_cast<std::uint8_t>(std::clamp(reset_if_empty[index + 3], 0, 255)),
                    static_cast<std::uint8_t>(std::clamp(reset_if_empty[index + 4], 0, 255)));
                (value == 38 ? attributes.foreground : attributes.background) = color;
                index += 4;
            }
        }
    }

    void SetPrivateMode(int mode, bool enabled) {
        if (mode == 1) {
            application_cursor_keys = enabled;
        } else if (mode == 25) {
            ActiveCursor().visible = enabled;
        } else if (mode == 1000) {
            mouse = enabled ? MouseMode::Normal : MouseMode::None;
        } else if (mode == 1002) {
            mouse = enabled ? MouseMode::ButtonEvent : MouseMode::None;
        } else if (mode == 1003) {
            mouse = enabled ? MouseMode::AnyEvent : MouseMode::None;
        } else if (mode == 1006) {
            sgr_mouse = enabled;
        } else if (mode == 1049) {
            SetAlternateScreen(enabled);
        } else if (mode == 2004) {
            bracketed_paste = enabled;
        }
    }

    void SetAlternateScreen(bool enabled) {
        if (enabled == alternate) {
            return;
        }
        wrap_pending = false;
        if (enabled) {
            saved_cursor = main_cursor;
            alternate_screen = MakeScreen(row_count, column_count);
            alternate_cursor = {};
            alternate = true;
        } else {
            alternate = false;
            main_cursor = saved_cursor;
            ClampCursor();
        }
    }

    void HandleCsi(char final_byte, std::string_view raw) {
        bool private_mode = !raw.empty() && raw.front() == '?';
        if (private_mode) {
            raw.remove_prefix(1);
        }
        const std::vector<int> parameters = ParseParameters(raw);
        const int first = parameters.empty() || parameters[0] == 0 ? 1 : parameters[0];
        switch (final_byte) {
        case 'A':
            MoveCursorRelative(-first, 0);
            break;
        case 'B':
            MoveCursorRelative(first, 0);
            break;
        case 'C':
            MoveCursorRelative(0, first);
            break;
        case 'D':
            MoveCursorRelative(0, -first);
            break;
        case 'G':
            ActiveCursor().column = static_cast<std::size_t>(
                std::clamp(first - 1, 0, static_cast<int>(column_count - 1)));
            wrap_pending = false;
            break;
        case 'H':
        case 'f': {
            const int row = parameters.empty() || parameters[0] == 0 ? 1 : parameters[0];
            const int column = parameters.size() < 2 || parameters[1] == 0 ? 1 : parameters[1];
            ActiveCursor().row =
                static_cast<std::size_t>(std::clamp(row - 1, 0, static_cast<int>(row_count - 1)));
            ActiveCursor().column = static_cast<std::size_t>(
                std::clamp(column - 1, 0, static_cast<int>(column_count - 1)));
            wrap_pending = false;
            break;
        }
        case 'J':
            EraseDisplay(parameters.empty() ? 0 : parameters[0]);
            break;
        case 'K':
            EraseLine(parameters.empty() ? 0 : parameters[0]);
            break;
        case 'm':
            ApplySgr(parameters);
            break;
        case 'h':
        case 'l':
            if (private_mode) {
                for (const int parameter : parameters) {
                    SetPrivateMode(parameter, final_byte == 'h');
                }
            }
            break;
        default:
            break;
        }
    }

    void HandleOsc(std::string_view value) {
        if (!value.starts_with("52;")) {
            return;
        }
        value.remove_prefix(3);
        const std::size_t separator = value.find(';');
        if (separator == std::string_view::npos) {
            return;
        }
        if (event_callback) {
            TerminalEvent event;
            event.selection.assign(value.substr(0, separator));
            event.encoded_data.assign(value.substr(separator + 1));
            event_callback(event);
        }
    }

    void Resize(std::size_t rows, std::size_t columns) {
        rows = std::max<std::size_t>(1, rows);
        columns = std::max<std::size_t>(1, columns);
        auto resize_columns = [columns](std::vector<TerminalLine>& screen) {
            for (auto& line : screen) {
                if (line.cells.size() < columns) {
                    line.cells.resize(columns);
                }
            }
        };
        resize_columns(main_screen);
        resize_columns(alternate_screen);

        auto resize_rows =
            [&](std::vector<TerminalLine>& screen, CursorPosition& cursor, bool preserve_history) {
                while (screen.size() > rows) {
                    TerminalLine removed = std::move(screen.front());
                    screen.erase(screen.begin());
                    if (preserve_history) {
                        PushHistory(std::move(removed));
                    }
                    if (cursor.row > 0) {
                        --cursor.row;
                    }
                }
                while (screen.size() < rows) {
                    screen.push_back(BlankLine(columns));
                }
            };
        resize_rows(main_screen, main_cursor, !alternate);
        resize_rows(alternate_screen, alternate_cursor, false);
        row_count = rows;
        column_count = columns;
        main_cursor.row = std::min(main_cursor.row, rows - 1);
        main_cursor.column = std::min(main_cursor.column, columns - 1);
        alternate_cursor.row = std::min(alternate_cursor.row, rows - 1);
        alternate_cursor.column = std::min(alternate_cursor.column, columns - 1);
        wrap_pending = false;
        TrimHistory();
    }

    void Reset() {
        main_screen = MakeScreen(row_count, column_count);
        alternate_screen = MakeScreen(row_count, column_count);
        main_cursor = {};
        alternate_cursor = {};
        saved_cursor = {};
        history.clear();
        history_bytes = 0;
        attributes = {};
        parse_state = ParseState::Ground;
        control.clear();
        utf8_value = 0;
        utf8_minimum = 0;
        utf8_remaining = 0;
        alternate = false;
        bracketed_paste = false;
        application_cursor_keys = false;
        mouse = MouseMode::None;
        sgr_mouse = false;
        wrap_pending = false;
    }

    void EnableStateTrackingOnly() {
        state_tracking_only = true;
        row_count = 1;
        column_count = 1;
        main_screen = MakeScreen(1, 1);
        alternate_screen = MakeScreen(1, 1);
        main_cursor = {};
        alternate_cursor = {};
        saved_cursor = {};
        history.clear();
        history_bytes = 0;
        attributes = {};
        parse_state = ParseState::Ground;
        control.clear();
        utf8_value = 0;
        utf8_minimum = 0;
        utf8_remaining = 0;
        alternate = false;
        bracketed_paste = false;
        application_cursor_keys = false;
        mouse = MouseMode::None;
        sgr_mouse = false;
        wrap_pending = false;
    }

    void PushExternalHistory(TerminalLine line) {
        const TerminalCell blank{};
        while (!line.cells.empty() && line.cells.back() == blank) {
            line.cells.pop_back();
        }
        if (line.cells.size() < line.cells.capacity()) {
            std::vector<TerminalCell> compact(line.cells.begin(), line.cells.end());
            line.cells.swap(compact);
        }
        history_bytes += LineBytes(line);
        history.push_back(std::move(line));
        TrimHistory();
    }
};

TerminalBuffer::TerminalBuffer(std::size_t rows, std::size_t columns)
    : impl_(new Impl(rows, columns)) {}
TerminalBuffer::~TerminalBuffer() {
    delete impl_;
}

TerminalBuffer::TerminalBuffer(TerminalBuffer&& other) noexcept
    : impl_(std::exchange(other.impl_, nullptr)) {}

TerminalBuffer& TerminalBuffer::operator=(TerminalBuffer&& other) noexcept {
    if (this != &other) {
        delete impl_;
        impl_ = std::exchange(other.impl_, nullptr);
    }
    return *this;
}

void TerminalBuffer::feed(std::string_view bytes) {
    impl_->Feed(bytes);
}
std::string TerminalBuffer::take_output() {
    return {};
}
void TerminalBuffer::resize(std::size_t rows, std::size_t columns) {
    impl_->Resize(rows, columns);
}
void TerminalBuffer::reset() {
    impl_->Reset();
}
void TerminalBuffer::set_event_callback(TerminalEventCallback callback) {
    impl_->event_callback = std::move(callback);
}
void TerminalBuffer::enable_state_tracking_only() {
    impl_->EnableStateTrackingOnly();
}
void TerminalBuffer::push_scrollback_line(TerminalLine line) {
    impl_->PushExternalHistory(std::move(line));
}
std::size_t TerminalBuffer::rows() const noexcept {
    return impl_->row_count;
}
std::size_t TerminalBuffer::columns() const noexcept {
    return impl_->column_count;
}
CursorPosition TerminalBuffer::cursor() const noexcept {
    return impl_->ActiveCursor();
}
const TerminalCell& TerminalBuffer::cell(std::size_t row, std::size_t column) const {
    if (row >= rows() || column >= columns()) {
        throw std::out_of_range("terminal cell is outside the visible screen");
    }
    return impl_->ActiveScreen()[row].cells[column];
}
const std::vector<TerminalLine>& TerminalBuffer::screen() const noexcept {
    return impl_->ActiveScreen();
}
const std::deque<TerminalLine>& TerminalBuffer::scrollback() const noexcept {
    return impl_->history;
}
std::size_t TerminalBuffer::scrollback_line_count() const noexcept {
    return impl_->history.size();
}
std::size_t TerminalBuffer::scrollback_bytes() const noexcept {
    return impl_->history_bytes;
}
bool TerminalBuffer::alternate_screen_active() const noexcept {
    return impl_->alternate;
}
bool TerminalBuffer::bracketed_paste_enabled() const noexcept {
    return impl_->bracketed_paste;
}
bool TerminalBuffer::application_cursor_keys_enabled() const noexcept {
    return impl_->application_cursor_keys;
}
MouseMode TerminalBuffer::mouse_mode() const noexcept {
    return impl_->mouse;
}
bool TerminalBuffer::sgr_mouse_encoding_enabled() const noexcept {
    return impl_->sgr_mouse;
}

} // namespace SLink::terminal
