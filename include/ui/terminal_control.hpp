#pragma once

#include "terminal/terminal_buffer.hpp"
#include "terminal/terminal_session_codec.hpp"

#include <windows.h>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace SLink::ui {

enum class TerminalInputOrigin {
    User,
    Protocol,
};

struct TerminalInputEvent {
    std::string_view bytes;
    TerminalInputOrigin origin{TerminalInputOrigin::User};
};

class TerminalControl final {
public:
    using InputCallback = std::function<void(TerminalInputEvent)>;
    using ResizeCallback = std::function<void(std::size_t rows, std::size_t columns)>;
    // 只有用户明确同意把解码后的 OSC 52 内容写入 Windows 剪贴板时才返回 true。
    // 未设置回调时控件自行显示确认框；远端读取剪贴板的请求始终拒绝。
    using Osc52PromptCallback = std::function<bool(const terminal::TerminalEvent&)>;

    TerminalControl();
    ~TerminalControl();

    TerminalControl(const TerminalControl&) = delete;
    TerminalControl& operator=(const TerminalControl&) = delete;

    bool Create(HWND parent, HINSTANCE instance, int control_id);
    void SetBounds(int x, int y, int width, int height);
    void Show(bool visible);
    void ApplyTheme();

    // SSH 等远端传输字节先按当前会话编码解码；SLink 自己生成的 UTF-8 提示必须使用
    // FeedUtf8，避免在 GB18030 会话中被重复转码。两者均可从非 UI 线程调用。
    void FeedTransportBytes(std::string_view bytes);
    void FeedUtf8(std::string_view bytes);
    void SetTransportEncoding(terminal::TerminalEncoding encoding);
    [[nodiscard]] terminal::TerminalEncoding transport_encoding() const noexcept;
    void SetInputCallback(InputCallback callback);
    void SetResizeCallback(ResizeCallback callback);
    void SetOsc52PromptCallback(Osc52PromptCallback callback);

    void CopySelection();
    void PasteClipboard();
    // 搜索当前屏幕及有上限的回滚缓冲区，选中结果并滚动到可见区域；重复调用会从
    // 当前匹配之后继续，并在末尾循环一次。
    bool FindNext(std::wstring_view query, bool backwards = false);
    HWND hwnd() const noexcept;
    std::size_t rows() const noexcept;
    std::size_t columns() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace SLink::ui
