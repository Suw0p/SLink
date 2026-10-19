#pragma once

#include "core/status.hpp"

#include <memory>

namespace SLink::ui {
class TerminalControl;
}

namespace SLink::app {

// 管理一个 Windows ConPTY 及其 cmd.exe 进程树。输入、输出分别在工作线程搬运，
// 大段粘贴或高频输出不会阻塞 UI；Stop 会终止整棵本地子进程树并回收管道句柄。
class LocalTerminalSession final {
public:
    explicit LocalTerminalSession(ui::TerminalControl& terminal);
    ~LocalTerminalSession();

    LocalTerminalSession(const LocalTerminalSession&) = delete;
    LocalTerminalSession& operator=(const LocalTerminalSession&) = delete;

    [[nodiscard]] Status Start();
    void Stop() noexcept;

    [[nodiscard]] ui::TerminalControl* terminal() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace SLink::app
