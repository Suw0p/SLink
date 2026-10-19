#pragma once

#include <windows.h>

#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace SLink::editor {

inline constexpr UINT kGb2312CodePage = 20936;

// 编辑器始终保存 UTF-8 文本；只有读写远端文件时才与 GB2312 字节互转。
// 回转检查拦截 Windows 解码器对无效字节的隐式替换。
[[nodiscard]] inline std::optional<std::string> DecodeGb2312(std::string_view bytes) {
    if (bytes.empty())
        return std::string{};
    if (bytes.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
        return std::nullopt;
    const int count = static_cast<int>(bytes.size());
    const int length = MultiByteToWideChar(kGb2312CodePage, 0, bytes.data(), count, nullptr, 0);
    if (length <= 0)
        return std::nullopt;
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(kGb2312CodePage, 0, bytes.data(), count, wide.data(), length) != length)
        return std::nullopt;
    BOOL substituted = FALSE;
    const int roundtrip_length = WideCharToMultiByte(kGb2312CodePage,
                                                     WC_NO_BEST_FIT_CHARS,
                                                     wide.data(),
                                                     length,
                                                     nullptr,
                                                     0,
                                                     nullptr,
                                                     &substituted);
    if (roundtrip_length != count || substituted)
        return std::nullopt;
    std::string roundtrip(static_cast<std::size_t>(roundtrip_length), '\0');
    substituted = FALSE;
    if (WideCharToMultiByte(kGb2312CodePage,
                            WC_NO_BEST_FIT_CHARS,
                            wide.data(),
                            length,
                            roundtrip.data(),
                            roundtrip_length,
                            nullptr,
                            &substituted) != roundtrip_length ||
        substituted || roundtrip != bytes)
        return std::nullopt;
    const int utf8_length = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), length, nullptr, 0, nullptr, nullptr);
    if (utf8_length <= 0)
        return std::nullopt;
    std::string utf8(static_cast<std::size_t>(utf8_length), '\0');
    if (WideCharToMultiByte(CP_UTF8,
                            WC_ERR_INVALID_CHARS,
                            wide.data(),
                            length,
                            utf8.data(),
                            utf8_length,
                            nullptr,
                            nullptr) != utf8_length)
        return std::nullopt;
    return utf8;
}

[[nodiscard]] inline std::optional<std::string> EncodeGb2312(std::string_view utf8) {
    if (utf8.empty())
        return std::string{};
    if (utf8.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
        return std::nullopt;
    const int count = static_cast<int>(utf8.size());
    const int length =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), count, nullptr, 0);
    if (length <= 0)
        return std::nullopt;
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), count, wide.data(), length) != length)
        return std::nullopt;
    BOOL substituted = FALSE;
    const int byte_count = WideCharToMultiByte(kGb2312CodePage,
                                               WC_NO_BEST_FIT_CHARS,
                                               wide.data(),
                                               length,
                                               nullptr,
                                               0,
                                               nullptr,
                                               &substituted);
    if (byte_count <= 0 || substituted)
        return std::nullopt;
    std::string bytes(static_cast<std::size_t>(byte_count), '\0');
    substituted = FALSE;
    if (WideCharToMultiByte(kGb2312CodePage,
                            WC_NO_BEST_FIT_CHARS,
                            wide.data(),
                            length,
                            bytes.data(),
                            byte_count,
                            nullptr,
                            &substituted) != byte_count ||
        substituted)
        return std::nullopt;
    return bytes;
}

} // namespace SLink::editor
