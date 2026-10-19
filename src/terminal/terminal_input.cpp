// 终端键盘和剪贴板输入规范化辅助，避免 CRLF 粘贴产生 ^M，并过滤已处理快捷键对应的
// 重复 WM_CHAR 控制字符。
#include "terminal/terminal_input.hpp"

namespace SLink::terminal {

std::wstring NormalizePasteLineEndings(const std::wstring_view text) {
    std::wstring normalized;
    normalized.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] != L'\r') {
            normalized.push_back(text[index]);
            continue;
        }
        normalized.push_back(L'\n');
        if (index + 1 < text.size() && text[index + 1] == L'\n') {
            ++index;
        }
    }
    return normalized;
}

bool IsClipboardShortcutCharacter(const wchar_t character,
                                  const bool control,
                                  const bool shift) noexcept {
    return control && shift && (character == L'\x03' || character == L'\x16');
}

} // namespace SLink::terminal
