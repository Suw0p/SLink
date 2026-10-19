#pragma once

#include "core/status.hpp"

#include <string>
#include <string_view>

namespace SLink::terminal {

// 传输字符集只描述终端会话与远端之间的字节编码。终端模型和画布始终使用
// UTF-8/Unicode，不随这里的选择改变。
enum class TerminalEncoding {
    Utf8,
    Gb18030,
};

class TerminalSessionCodec final {
public:
    void SetEncoding(TerminalEncoding encoding) noexcept;
    [[nodiscard]] TerminalEncoding encoding() const noexcept;

    // 把远端原始字节转换为 libvterm 接受的 UTF-8。GB18030 的残缺尾部会保留到
    // 下一次调用，最多三个字节。
    [[nodiscard]] std::string DecodeTransport(std::string_view bytes);

    // 把键盘、IME 和剪贴板产生的 Unicode 文本转换为当前远端字符集。控制序列不应
    // 经过此接口，而应由调用方按原始协议字节发送。
    [[nodiscard]] Result<std::string> EncodeText(std::wstring_view text) const;

    void Reset() noexcept;

private:
    TerminalEncoding encoding_{TerminalEncoding::Utf8};
    std::string pending_gb18030_;
};

} // namespace SLink::terminal
