#pragma once

#include "terminal/terminal_buffer.hpp"

#include <memory>
#include <vector>

namespace SLink::terminal {

#ifdef SLINK_USE_LIBVTERM

struct TerminalDamage {
    bool full{};
    std::vector<std::size_t> rows;
};

// 启用 libvterm 时的可见屏幕适配器。libvterm 负责 ANSI/VT 状态机和当前屏幕；
// TerminalBuffer 仍负责有界回滚、OSC 52 安全事件及输入模式，避免维护两套完整解析器。
class LibVTermAdapter final {
public:
    explicit LibVTermAdapter(std::size_t rows = 24, std::size_t columns = 80);
    ~LibVTermAdapter();

    LibVTermAdapter(const LibVTermAdapter&) = delete;
    LibVTermAdapter& operator=(const LibVTermAdapter&) = delete;
    LibVTermAdapter(LibVTermAdapter&&) noexcept;
    LibVTermAdapter& operator=(LibVTermAdapter&&) noexcept;

    void feed(std::string_view bytes);
    [[nodiscard]] std::string take_output();
    void resize(std::size_t rows, std::size_t columns);
    void synchronize_cursor_row();
    [[nodiscard]] TerminalDamage take_damage();
    void set_event_callback(TerminalEventCallback callback);

    [[nodiscard]] std::size_t rows() const noexcept;
    [[nodiscard]] std::size_t columns() const noexcept;
    [[nodiscard]] const std::vector<TerminalLine>& screen() const noexcept;
    [[nodiscard]] CursorPosition cursor() const noexcept;
    [[nodiscard]] const TerminalBuffer& fallback_state() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

using DefaultTerminalModel = LibVTermAdapter;

#else

using DefaultTerminalModel = TerminalBuffer;

#endif

} // namespace SLink::terminal
