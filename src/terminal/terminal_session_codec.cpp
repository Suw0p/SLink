#include "terminal/terminal_session_codec.hpp"

#include <algorithm>
#include <limits>
#include <windows.h>

namespace SLink::terminal {
namespace {

constexpr UINT kGb18030CodePage = 54936;

bool IsGbLead(const unsigned char value) noexcept {
    return value >= 0x81 && value <= 0xFE;
}

bool IsGbTwoByteTrail(const unsigned char value) noexcept {
    return value >= 0x40 && value <= 0xFE && value != 0x7F;
}

bool IsDigitByte(const unsigned char value) noexcept {
    return value >= 0x30 && value <= 0x39;
}

// 返回可安全交给 CP54936 的完整前缀长度。只在数据尾部可能构成合法双/四字节字符
// 时保留残余，非法的中间字节仍交给 Windows NLS 生成替换字符，避免解码器停滞。
std::size_t CompletePrefixLength(const std::string_view bytes) noexcept {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto first = static_cast<unsigned char>(bytes[offset]);
        if (!IsGbLead(first)) {
            ++offset;
            continue;
        }
        if (offset + 1 >= bytes.size()) {
            break;
        }
        const auto second = static_cast<unsigned char>(bytes[offset + 1]);
        if (IsDigitByte(second)) {
            if (offset + 3 >= bytes.size()) {
                break;
            }
            const auto third = static_cast<unsigned char>(bytes[offset + 2]);
            const auto fourth = static_cast<unsigned char>(bytes[offset + 3]);
            offset += IsGbLead(third) && IsDigitByte(fourth) ? 4 : 1;
            continue;
        }
        offset += IsGbTwoByteTrail(second) ? 2 : 1;
    }
    return offset;
}

std::wstring MultiByteToWide(const UINT code_page, const std::string_view bytes) {
    if (bytes.empty() || bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return {};
    }
    const int count = MultiByteToWideChar(
        code_page, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    if (count <= 0) {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    if (MultiByteToWideChar(code_page,
                            0,
                            bytes.data(),
                            static_cast<int>(bytes.size()),
                            result.data(),
                            count) != count) {
        return {};
    }
    return result;
}

std::string WideToMultiByte(const UINT code_page, const std::wstring_view text) {
    if (text.empty() || text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return {};
    }
    const int count = WideCharToMultiByte(code_page,
                                          WC_ERR_INVALID_CHARS,
                                          text.data(),
                                          static_cast<int>(text.size()),
                                          nullptr,
                                          0,
                                          nullptr,
                                          nullptr);
    if (count <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(count), '\0');
    if (WideCharToMultiByte(code_page,
                            WC_ERR_INVALID_CHARS,
                            text.data(),
                            static_cast<int>(text.size()),
                            result.data(),
                            count,
                            nullptr,
                            nullptr) != count) {
        return {};
    }
    return result;
}

} // namespace

void TerminalSessionCodec::SetEncoding(const TerminalEncoding encoding) noexcept {
    if (encoding_ == encoding) {
        return;
    }
    pending_gb18030_.clear();
    encoding_ = encoding;
}

TerminalEncoding TerminalSessionCodec::encoding() const noexcept {
    return encoding_;
}

std::string TerminalSessionCodec::DecodeTransport(const std::string_view bytes) {
    if (encoding_ == TerminalEncoding::Utf8) {
        return std::string(bytes);
    }

    std::string input;
    input.reserve(pending_gb18030_.size() + bytes.size());
    input.append(pending_gb18030_);
    input.append(bytes);
    pending_gb18030_.clear();

    const std::size_t complete = CompletePrefixLength(input);
    if (complete < input.size()) {
        pending_gb18030_.assign(input.data() + complete, input.size() - complete);
    }
    if (complete == 0) {
        return {};
    }
    const std::wstring wide = MultiByteToWide(kGb18030CodePage, {input.data(), complete});
    if (wide.empty()) {
        return "\xEF\xBF\xBD";
    }
    return WideToMultiByte(CP_UTF8, wide);
}

Result<std::string> TerminalSessionCodec::EncodeText(const std::wstring_view text) const {
    if (text.empty()) {
        return std::string{};
    }
    const UINT code_page = encoding_ == TerminalEncoding::Utf8 ? CP_UTF8 : kGb18030CodePage;
    std::string encoded = WideToMultiByte(code_page, text);
    if (encoded.empty()) {
        return Status{ErrorCode::invalid_argument, L"当前字符无法使用所选终端字符集编码。"};
    }
    return encoded;
}

void TerminalSessionCodec::Reset() noexcept {
    pending_gb18030_.clear();
}

} // namespace SLink::terminal
