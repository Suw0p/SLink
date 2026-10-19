#pragma once

#include <string>
#include <string_view>

namespace SLink::terminal {

// Windows 剪贴板通常使用 CRLF。Bracketed Paste 中的 CR 是普通字符，readline 会显示
// 为 ^M，因此无论远端平台如何，终端粘贴统一规范化为 Unix LF。
[[nodiscard]] std::wstring NormalizePasteLineEndings(std::wstring_view text);

// 终端在 WM_KEYDOWN 处理 Ctrl+Shift 剪贴板快捷键后，TranslateMessage 仍可能产生
// Ctrl+C/Ctrl+V 对应 WM_CHAR；这些控制字符必须拦截，不能再发送给远端 PTY。
[[nodiscard]] bool
IsClipboardShortcutCharacter(wchar_t character, bool control, bool shift) noexcept;

} // namespace SLink::terminal
