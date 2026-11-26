// FileUri 解析、规范化、父子路径运算和稳定序列化实现；本地与 SFTP 路径在这里统一
// 处理分隔符，避免各业务模块自行拼接 URI。
#include "transfer/file_uri.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <vector>

namespace SLink::transfer {
namespace {

int hex_value(const char value) {
    if (value >= '0' && value <= '9')
        return value - '0';
    if (value >= 'a' && value <= 'f')
        return value - 'a' + 10;
    if (value >= 'A' && value <= 'F')
        return value - 'A' + 10;
    return -1;
}

std::string percent_decode(std::string_view input) {
    std::string result;
    result.reserve(input.size());
    for (std::size_t index = 0; index < input.size(); ++index) {
        if (input[index] != '%') {
            result += input[index];
            continue;
        }
        if (index + 2 >= input.size()) {
            throw std::invalid_argument("Incomplete percent escape in file URI");
        }
        const auto high = hex_value(input[index + 1]);
        const auto low = hex_value(input[index + 2]);
        if (high < 0 || low < 0) {
            throw std::invalid_argument("Invalid percent escape in file URI");
        }
        result += static_cast<char>((high << 4) | low);
        index += 2;
    }
    return result;
}

std::string percent_encode(std::string_view input) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    for (const unsigned char value : input) {
        if (std::isalnum(value) || value == '/' || value == ':' || value == '-' || value == '_' ||
            value == '.' || value == '~') {
            result += static_cast<char>(value);
        } else {
            result += '%';
            result += hex[value >> 4];
            result += hex[value & 0x0f];
        }
    }
    return result;
}

std::string normalize_path(std::string_view input, const bool local) {
    std::string text(input);
    std::replace(text.begin(), text.end(), '\\', '/');
    const bool unc = local && text.starts_with("//");

    std::string drive;
    std::size_t begin = 0;
    if (local && text.size() >= 2 && std::isalpha(static_cast<unsigned char>(text[0])) &&
        text[1] == ':') {
        drive.assign(text.begin(), text.begin() + 2);
        drive[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(drive[0])));
        begin = 2;
    }

    const bool absolute = begin < text.size() && text[begin] == '/';
    while (begin < text.size() && text[begin] == '/') {
        ++begin;
    }

    std::vector<std::string> components;
    while (begin <= text.size()) {
        const auto end = text.find('/', begin);
        const auto part =
            text.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        if (!part.empty() && part != ".") {
            if (part == "..") {
                if (components.size() > (unc ? 2U : 0U) && components.back() != "..") {
                    components.pop_back();
                } else if (!absolute) {
                    components.emplace_back(part);
                }
            } else {
                components.emplace_back(part);
            }
        }
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }

    std::string result = unc ? "//" : drive;
    if (!unc && (absolute || !drive.empty() || !local)) {
        result += '/';
    }
    for (std::size_t i = 0; i < components.size(); ++i) {
        if (!result.empty() && result.back() != '/') {
            result += '/';
        }
        result += components[i];
    }
    if (result.empty()) {
        result = "/";
    }
    return result;
}

void validate_authority(std::string_view authority) {
    if (authority.empty()) {
        throw std::invalid_argument("An sftp URI requires a server id");
    }
    if (authority.find_first_of("/\\?#") != std::string_view::npos) {
        throw std::invalid_argument("Invalid character in sftp server id");
    }
}

} // namespace

void ValidateFileName(std::string_view name, const bool windows_target) {
    if (name.empty() || name == "." || name == ".." ||
        name.find_first_of("/\\") != std::string_view::npos || name.find('\0') != name.npos)
        throw std::invalid_argument("Directory entry is not a single safe file name");
    if (!windows_target)
        return;
    if (name.back() == '.' || name.back() == ' ' || name.find_first_of("<>:\"|?*") != name.npos ||
        std::ranges::any_of(name, [](const unsigned char c) {
            return c < 32;
        }))
        throw std::invalid_argument("File name is not supported by Windows");
    std::string base(name.substr(0, name.find('.')));
    while (!base.empty() && (base.back() == ' ' || base.back() == '.'))
        base.pop_back();
    std::ranges::transform(base, base.begin(), [](const unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    const bool port = base.starts_with("COM") || base.starts_with("LPT");
    if (base == "CON" || base == "PRN" || base == "AUX" || base == "NUL" ||
        (port &&
         ((base.size() == 4 && base[3] >= '1' && base[3] <= '9') || base.substr(3) == "\xC2\xB9" ||
          base.substr(3) == "\xC2\xB2" || base.substr(3) == "\xC2\xB3")))
        throw std::invalid_argument("Reserved Windows file name");
}

FileUri::FileUri(FileScheme scheme, std::string authority, std::string path)
    : scheme_(scheme), authority_(std::move(authority)),
      path_(normalize_path(path, scheme == FileScheme::Local)) {
    if (path.find('\0') != std::string::npos)
        throw std::invalid_argument("File path contains a NUL byte");
    if (scheme_ == FileScheme::Local) {
        if (!authority_.empty()) {
            throw std::invalid_argument("A local URI cannot have an authority");
        }
    } else {
        validate_authority(authority_);
        if (path_.empty() || path_.front() != '/') {
            path_.insert(path_.begin(), '/');
        }
    }
}

FileUri FileUri::parse(std::string_view value) {
    constexpr std::string_view local_prefix = "local://";
    constexpr std::string_view sftp_prefix = "sftp://";
    if (value.starts_with(local_prefix)) {
        auto path = value.substr(local_prefix.size());
        // local:///E:/dir maps to E:/dir. local:/// maps to the local root.
        if (path.size() >= 3 && path[0] == '/' &&
            std::isalpha(static_cast<unsigned char>(path[1])) && path[2] == ':') {
            path.remove_prefix(1);
        }
        return local(percent_decode(path));
    }
    if (value.starts_with(sftp_prefix)) {
        auto rest = value.substr(sftp_prefix.size());
        const auto slash = rest.find('/');
        if (slash == std::string_view::npos) {
            return sftp(rest, "/");
        }
        return sftp(rest.substr(0, slash), percent_decode(rest.substr(slash)));
    }
    throw std::invalid_argument("Unsupported file URI scheme");
}

FileUri FileUri::local(std::string_view path) {
    return FileUri(FileScheme::Local, {}, std::string(path));
}

FileUri FileUri::sftp(std::string_view server_id, std::string_view path) {
    return FileUri(FileScheme::Sftp, std::string(server_id), std::string(path));
}

FileUri FileUri::parent() const {
    // UNC 的共享根不能退成当前盘符根，也不能通过 .. 越过共享边界。
    if (is_local() && path_.starts_with("//")) {
        const auto share = path_.find('/', 2);
        if (share == std::string::npos || path_.find('/', share + 1) == std::string::npos)
            return *this;
    }
    if (path_ == "/" || (path_.size() == 3 && path_[1] == ':' && path_[2] == '/')) {
        return *this;
    }
    const auto slash = path_.find_last_of('/');
    if (slash == std::string::npos) {
        return with_path("/");
    }
    if (slash == 0) {
        return with_path("/");
    }
    if (slash == 2 && path_[1] == ':') {
        return with_path(path_.substr(0, 3));
    }
    return with_path(path_.substr(0, slash));
}

FileUri FileUri::joined(std::string_view child) const {
    if (child.empty()) {
        return *this;
    }
    std::string next = path_;
    if (next.empty() || next.back() != '/') {
        next += '/';
    }
    next.append(child);
    return with_path(next);
}

FileUri FileUri::with_path(std::string_view path) const {
    return FileUri(scheme_, authority_, std::string(path));
}

std::string FileUri::filename() const {
    if (is_local() && path_.starts_with("//") && parent() == *this)
        return {};
    if (path_ == "/") {
        return {};
    }
    auto end = path_.size();
    while (end > 1 && path_[end - 1] == '/') {
        --end;
    }
    const auto slash = path_.rfind('/', end - 1);
    return path_.substr(slash == std::string::npos ? 0 : slash + 1,
                        end - (slash == std::string::npos ? 0 : slash + 1));
}

std::string FileUri::to_string() const {
    if (is_sftp()) {
        return "sftp://" + authority_ + percent_encode(path_);
    }
    if (path_.size() >= 2 && path_[1] == ':') {
        return "local:///" + percent_encode(path_);
    }
    return "local://" + percent_encode(path_);
}

} // namespace SLink::transfer
