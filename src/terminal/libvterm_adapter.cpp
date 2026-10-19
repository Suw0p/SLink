// libvterm 回调到 SLink TerminalLine/Cell 模型的适配实现，负责宽字符、属性、光标和
// 回滚行同步，同时把 OSC 52 等安全事件交回 TerminalBuffer。
#include "terminal/libvterm_adapter.hpp"

#ifdef SLINK_USE_LIBVTERM

#include <algorithm>
#include <limits>
#include <utility>
#include <vterm.h>

namespace SLink::terminal {
namespace {

TerminalColor ConvertColor(const VTermColor& source) {
    if (VTERM_COLOR_IS_DEFAULT_FG(&source) || VTERM_COLOR_IS_DEFAULT_BG(&source)) {
        return {};
    }
    if (VTERM_COLOR_IS_INDEXED(&source)) {
        return TerminalColor::Indexed(source.indexed.idx);
    }
    if (VTERM_COLOR_IS_RGB(&source)) {
        return TerminalColor::Rgb(source.rgb.red, source.rgb.green, source.rgb.blue);
    }
    return {};
}

bool IsWideCodepoint(const char32_t codepoint) noexcept {
    return (codepoint >= 0x1100 && codepoint <= 0x115F) ||
           (codepoint >= 0x2329 && codepoint <= 0x232A) ||
           (codepoint >= 0x2E80 && codepoint <= 0xA4CF) ||
           (codepoint >= 0xAC00 && codepoint <= 0xD7A3) ||
           (codepoint >= 0xF900 && codepoint <= 0xFAFF) ||
           (codepoint >= 0xFE10 && codepoint <= 0xFE19) ||
           (codepoint >= 0xFE30 && codepoint <= 0xFE6F) ||
           (codepoint >= 0xFF00 && codepoint <= 0xFF60) ||
           (codepoint >= 0xFFE0 && codepoint <= 0xFFE6) ||
           (codepoint >= 0x1F300 && codepoint <= 0x1FAFF) ||
           (codepoint >= 0x20000 && codepoint <= 0x3FFFD);
}

TerminalCell ConvertCell(const VTermScreenCell& source) {
    TerminalCell target;
    const char32_t codepoint = source.chars[0];
    target.codepoint =
        codepoint <= 0x10FFFF && !(codepoint >= 0xD800 && codepoint <= 0xDFFF) ? codepoint : U' ';
    if (target.codepoint == 0) {
        // libvterm uses width == 0 only for the continuation cell of a
        // double-width glyph. An ordinary cleared/blank cell still has width
        // 1 and must remain drawable so its attributes and background are
        // preserved.
        target.width = static_cast<std::uint8_t>(std::clamp<int>(source.width, 0, 1));
    } else if (target.codepoint <= 0x7F) {
        // A printable ASCII cell is always one terminal column. Some
        // libvterm overwrite paths temporarily retain the width of the wide
        // glyph that previously occupied this cell; trusting that stale width
        // makes readline edits appear to paint over the following text until
        // the shell redraws the entire command line.
        target.width = 1;
    } else {
        const int reported_width = std::clamp<int>(source.width, 1, 2);
        const int unicode_width = IsWideCodepoint(target.codepoint) ? 2 : 1;
        target.width = static_cast<std::uint8_t>(std::max(reported_width, unicode_width));
    }
    target.attributes.foreground = ConvertColor(source.fg);
    target.attributes.background = ConvertColor(source.bg);
    target.attributes.bold = source.attrs.bold != 0;
    target.attributes.italic = source.attrs.italic != 0;
    target.attributes.underline = source.attrs.underline != 0;
    target.attributes.inverse = source.attrs.reverse != 0;
    target.attributes.strike = source.attrs.strike != 0;
    return target;
}

} // namespace

struct LibVTermAdapter::Impl {
    VTerm* terminal{};
    VTermScreen* vterm_screen{};
    TerminalBuffer fallback;
    std::vector<TerminalLine> visible;
    CursorPosition cursor;
    std::size_t rows;
    std::size_t columns;
    std::string pending_vterm_input;
    std::vector<unsigned char> dirty_rows;
    bool full_damage{true};
    VTermScreenCallbacks screen_callbacks{};

    Impl(std::size_t row_count, std::size_t column_count)
        : fallback(row_count, column_count), rows(std::max<std::size_t>(1, row_count)),
          columns(std::max<std::size_t>(1, column_count)) {
        visible = MakeScreen(rows, columns);
        dirty_rows.resize(rows);
        terminal = vterm_new(SafeInt(rows), SafeInt(columns));
        if (!terminal) {
            return;
        }
        vterm_set_utf8(terminal, 1);
        vterm_screen = vterm_obtain_screen(terminal);

        screen_callbacks.damage = &OnDamage;
        screen_callbacks.moverect = &OnMoveRect;
        screen_callbacks.movecursor = &OnMoveCursor;
        screen_callbacks.settermprop = &OnSetTermProp;
        screen_callbacks.bell = &OnBell;
        screen_callbacks.resize = &OnResize;
        screen_callbacks.sb_pushline = &OnScrollbackPush;
        screen_callbacks.sb_popline = &OnScrollbackPop;
        vterm_screen_set_callbacks(vterm_screen, &screen_callbacks, this);
        // Keep each cached row coherent. Interactive line editors frequently
        // overwrite or insert at a wide-character boundary; cell-sized damage
        // can otherwise leave the continuation half of the old glyph cached.
        vterm_screen_set_damage_merge(vterm_screen, VTERM_DAMAGE_ROW);
        vterm_screen_enable_altscreen(vterm_screen, 1);
        // libvterm 0.3.3 的历史行重排会在某些首行仍为 continuation 的
        // 屏幕状态下进入危险的 resize_buffer 路径。SLink 的 PTY 仍会按
        // 新尺寸调整，只有已经绘制的旧行不再重新换行，从而避免缩放导致
        // UI 主线程越界崩溃。库内同时保留边界修复作为第二层保护。
        vterm_screen_enable_reflow(vterm_screen, false);
        vterm_screen_reset(vterm_screen, 1);
        fallback.enable_state_tracking_only();
    }

    ~Impl() {
        if (terminal) {
            vterm_free(terminal);
        }
    }

    static int SafeInt(std::size_t value) noexcept {
        return static_cast<int>(std::min<std::size_t>(value, std::numeric_limits<int>::max()));
    }

    static std::vector<TerminalLine> MakeScreen(std::size_t rows, std::size_t columns) {
        std::vector<TerminalLine> result(rows);
        for (auto& line : result) {
            line.cells.resize(columns);
        }
        return result;
    }

    static bool IsCharsetDesignationIntroducer(const unsigned char byte) noexcept {
        switch (byte) {
        case '(':
        case ')':
        case '*':
        case '+':
        case '-':
        case '.':
        case '/':
        case '%':
            return true;
        default:
            return false;
        }
    }

    void WriteInput(std::string_view bytes) {
        if (!terminal || bytes.empty()) {
            return;
        }
        if (pending_vterm_input.empty() && bytes.find('\x1B') == std::string_view::npos) {
            vterm_input_write(terminal, bytes.data(), bytes.size());
            vterm_screen_flush_damage(vterm_screen);
            return;
        }
        pending_vterm_input.append(bytes);
        std::string filtered;
        filtered.reserve(pending_vterm_input.size());
        std::size_t offset = 0;
        while (offset < pending_vterm_input.size()) {
            const auto byte = static_cast<unsigned char>(pending_vterm_input[offset]);
            if (byte != 0x1B) {
                filtered.push_back(pending_vterm_input[offset++]);
                continue;
            }
            if (offset + 1 >= pending_vterm_input.size()) {
                break;
            }
            const auto introducer = static_cast<unsigned char>(pending_vterm_input[offset + 1]);
            if (!IsCharsetDesignationIntroducer(introducer)) {
                filtered.push_back(pending_vterm_input[offset++]);
                continue;
            }
            std::size_t final = offset + 2;
            while (final < pending_vterm_input.size()) {
                const auto candidate = static_cast<unsigned char>(pending_vterm_input[final]);
                if (candidate < 0x20 || candidate > 0x2F) {
                    break;
                }
                ++final;
            }
            if (final >= pending_vterm_input.size()) {
                // Preserve a short incomplete sequence across SSH reads.
                if (pending_vterm_input.size() - offset <= 8) {
                    break;
                }
                filtered.push_back(pending_vterm_input[offset++]);
                continue;
            }
            const auto final_byte = static_cast<unsigned char>(pending_vterm_input[final]);
            if (final_byte >= 0x30 && final_byte <= 0x7E) {
                // libvterm is already forced to UTF-8. Discarding a legacy
                // designation is semantically neutral and avoids a bundled
                // libvterm fault that otherwise exposes its final 'B'.
                offset = final + 1;
                continue;
            }
            filtered.push_back(pending_vterm_input[offset++]);
        }
        pending_vterm_input.erase(0, offset);
        if (!filtered.empty()) {
            vterm_input_write(terminal, filtered.data(), filtered.size());
            vterm_screen_flush_damage(vterm_screen);
        }
    }

    void Refresh(const VTermRect& rect) {
        if (!vterm_screen) {
            return;
        }
        const int start_row = std::clamp(rect.start_row, 0, SafeInt(rows));
        const int end_row = std::clamp(rect.end_row, start_row, SafeInt(rows));
        const int start_col = std::clamp(rect.start_col, 0, SafeInt(columns));
        const int end_col = std::clamp(rect.end_col, start_col, SafeInt(columns));
        for (int row = start_row; row < end_row; ++row) {
            bool continuation =
                start_col > 0 && visible[static_cast<std::size_t>(row)]
                                         .cells[static_cast<std::size_t>(start_col - 1)]
                                         .width > 1;
            for (int column = start_col; column < end_col; ++column) {
                VTermScreenCell source{};
                const VTermPos position{row, column};
                if (vterm_screen_get_cell(vterm_screen, position, &source)) {
                    TerminalCell converted = ConvertCell(source);
                    if (continuation) {
                        converted.codepoint = 0;
                        converted.width = 0;
                        continuation = false;
                    } else if (converted.width == 0) {
                        // The preceding wide glyph may have just been
                        // overwritten by a narrow one. libvterm can retain a
                        // zero-width continuation marker in the following
                        // cell until a later full repaint; normalize that
                        // orphan to a drawable blank immediately.
                        converted.codepoint = U' ';
                        converted.width = 1;
                    } else {
                        continuation = converted.width > 1;
                    }
                    visible[static_cast<std::size_t>(row)].cells[static_cast<std::size_t>(column)] =
                        converted;
                }
            }
        }
    }

    void MarkRows(int begin, int end) {
        begin = std::clamp(begin, 0, SafeInt(rows));
        end = std::clamp(end, begin, SafeInt(rows));
        for (int row = begin; row < end; ++row) {
            dirty_rows[static_cast<std::size_t>(row)] = 1;
        }
    }

    static int OnDamage(VTermRect rect, void* user) {
        auto* self = static_cast<Impl*>(user);
        self->Refresh(VTermRect{rect.start_row, rect.end_row, 0, SafeInt(self->columns)});
        self->MarkRows(rect.start_row, rect.end_row);
        return 1;
    }

    static int OnMoveRect(VTermRect destination, VTermRect source, void* user) {
        auto* self = static_cast<Impl*>(user);
        self->Refresh(VTermRect{std::min(source.start_row, destination.start_row),
                                std::max(source.end_row, destination.end_row),
                                0,
                                SafeInt(self->columns)});
        self->MarkRows(std::min(source.start_row, destination.start_row),
                       std::max(source.end_row, destination.end_row));
        return 1;
    }

    static int OnMoveCursor(VTermPos position, VTermPos, int visible, void* user) {
        auto* self = static_cast<Impl*>(user);
        self->MarkRows(SafeInt(self->cursor.row), SafeInt(self->cursor.row) + 1);
        self->cursor.row =
            static_cast<std::size_t>(std::clamp(position.row, 0, SafeInt(self->rows) - 1));
        self->cursor.column =
            static_cast<std::size_t>(std::clamp(position.col, 0, SafeInt(self->columns) - 1));
        self->cursor.visible = visible != 0;
        self->MarkRows(SafeInt(self->cursor.row), SafeInt(self->cursor.row) + 1);
        return 1;
    }

    static int OnSetTermProp(const VTermProp property, VTermValue* value, void* user) {
        auto* self = static_cast<Impl*>(user);
        if (self && value && property == VTERM_PROP_CURSORVISIBLE) {
            self->MarkRows(SafeInt(self->cursor.row), SafeInt(self->cursor.row) + 1);
            // Windows OpenSSH cmd.exe commonly hides the cursor while it
            // repaints an input line and restores it without moving it.
            // movecursor is therefore not guaranteed to run for the final
            // ESC[?25h; keep the cached visibility in sync with the terminal
            // property callback as well.
            self->cursor.visible = value->boolean != 0;
        }
        // Security-sensitive modes and OSC 52 are intentionally taken from
        // TerminalBuffer's bounded parser, not applied by this callback.
        return 1;
    }

    static int OnBell(void*) {
        return 1;
    }

    static int OnResize(int rows, int columns, void* user) {
        auto* self = static_cast<Impl*>(user);
        self->rows = std::max(1, rows);
        self->columns = std::max(1, columns);
        self->visible = MakeScreen(self->rows, self->columns);
        self->dirty_rows.assign(self->rows, static_cast<unsigned char>(0));
        self->full_damage = true;
        return 1;
    }

    static int OnScrollbackPush(int columns, const VTermScreenCell* cells, void* user) {
        auto* self = static_cast<Impl*>(user);
        if (!self || columns <= 0 || !cells) {
            return 1;
        }
        const TerminalCell blank{};
        int used_columns = columns;
        while (used_columns > 0 && ConvertCell(cells[used_columns - 1]) == blank) {
            --used_columns;
        }
        TerminalLine line;
        line.cells.reserve(static_cast<std::size_t>(used_columns));
        for (int column = 0; column < used_columns; ++column) {
            line.cells.push_back(ConvertCell(cells[column]));
        }
        self->fallback.push_scrollback_line(std::move(line));
        self->full_damage = true;
        return 1;
    }

    static int OnScrollbackPop(int, VTermScreenCell*, void*) {
        return 0;
    }
};

LibVTermAdapter::LibVTermAdapter(std::size_t rows, std::size_t columns)
    : impl_(std::make_unique<Impl>(rows, columns)) {}
LibVTermAdapter::~LibVTermAdapter() = default;
LibVTermAdapter::LibVTermAdapter(LibVTermAdapter&&) noexcept = default;
LibVTermAdapter& LibVTermAdapter::operator=(LibVTermAdapter&&) noexcept = default;

void LibVTermAdapter::feed(std::string_view bytes) {
    // The fallback pass retains bounded scrollback and rejects dangerous
    // controls. libvterm remains authoritative for the visible screen.
    impl_->fallback.feed(bytes);
    if (impl_->terminal) {
        impl_->WriteInput(bytes);
    } else {
        impl_->full_damage = true;
    }
}

std::string LibVTermAdapter::take_output() {
    if (!impl_->terminal) {
        return {};
    }
    constexpr std::size_t kMaximumResponseBytes = 64U * 1024U;
    std::string output;
    output.reserve(
        (std::min)(vterm_output_get_buffer_current(impl_->terminal), kMaximumResponseBytes));
    char buffer[4096]{};
    while (output.size() < kMaximumResponseBytes) {
        const std::size_t available = kMaximumResponseBytes - output.size();
        const std::size_t count =
            vterm_output_read(impl_->terminal, buffer, (std::min)(available, sizeof(buffer)));
        if (count == 0) {
            break;
        }
        output.append(buffer, count);
    }
    return output;
}

void LibVTermAdapter::resize(std::size_t rows, std::size_t columns) {
    rows = std::max<std::size_t>(1, rows);
    columns = std::max<std::size_t>(1, columns);
    if (impl_->terminal) {
        impl_->full_damage = true;
        vterm_set_size(impl_->terminal, Impl::SafeInt(rows), Impl::SafeInt(columns));
        vterm_screen_flush_damage(impl_->vterm_screen);
        impl_->Refresh(VTermRect{0, Impl::SafeInt(rows), 0, Impl::SafeInt(columns)});
    } else {
        impl_->fallback.resize(rows, columns);
        impl_->full_damage = true;
    }
}

void LibVTermAdapter::synchronize_cursor_row() {
    if (!impl_->vterm_screen || impl_->rows == 0 || impl_->columns == 0) {
        return;
    }
    const int row = static_cast<int>(std::min(impl_->cursor.row, impl_->rows - 1));
    impl_->Refresh(VTermRect{row, row + 1, 0, Impl::SafeInt(impl_->columns)});
    impl_->MarkRows(row, row + 1);
}

TerminalDamage LibVTermAdapter::take_damage() {
    TerminalDamage result;
    result.full = std::exchange(impl_->full_damage, false);
    if (!result.full) {
        for (std::size_t row = 0; row < impl_->dirty_rows.size(); ++row) {
            if (impl_->dirty_rows[row]) {
                result.rows.push_back(row);
            }
        }
    }
    std::fill(impl_->dirty_rows.begin(), impl_->dirty_rows.end(), static_cast<unsigned char>(0));
    return result;
}

void LibVTermAdapter::set_event_callback(TerminalEventCallback callback) {
    impl_->fallback.set_event_callback(std::move(callback));
}

std::size_t LibVTermAdapter::rows() const noexcept {
    return impl_->rows;
}

std::size_t LibVTermAdapter::columns() const noexcept {
    return impl_->columns;
}

const std::vector<TerminalLine>& LibVTermAdapter::screen() const noexcept {
    return impl_->terminal ? impl_->visible : impl_->fallback.screen();
}

CursorPosition LibVTermAdapter::cursor() const noexcept {
    return impl_->terminal ? impl_->cursor : impl_->fallback.cursor();
}

const TerminalBuffer& LibVTermAdapter::fallback_state() const noexcept {
    return impl_->fallback;
}

} // namespace SLink::terminal

#endif
