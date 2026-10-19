// Windows 本地文件端点实现：长路径、Unicode、目录枚举、流式读写及递归文件操作。
#include "transfer/local_endpoint.hpp"

#include <algorithm>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#endif

namespace SLink::transfer {
namespace {

std::filesystem::path native_path(const FileUri& uri) {
    if (!uri.is_local()) {
        throw std::invalid_argument("LocalEndpoint received a non-local URI");
    }
#ifdef _WIN32
    if (uri.path() == "/") {
        throw std::invalid_argument("The virtual local root is not a filesystem directory");
    }
    const auto& text = uri.path();
    const int size = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) {
        throw std::system_error(
            static_cast<int>(GetLastError()), std::system_category(), "Invalid UTF-8 local path");
    }
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8,
                        MB_ERR_INVALID_CHARS,
                        text.data(),
                        static_cast<int>(text.size()),
                        wide.data(),
                        size);
    return std::filesystem::path(wide);
#else
    return std::filesystem::path(uri.path());
#endif
}

std::string utf8_name(const std::filesystem::path& path) {
#ifdef _WIN32
    const auto wide = path.filename().wstring();
    const int size = WideCharToMultiByte(CP_UTF8,
                                         WC_ERR_INVALID_CHARS,
                                         wide.data(),
                                         static_cast<int>(wide.size()),
                                         nullptr,
                                         0,
                                         nullptr,
                                         nullptr);
    if (size <= 0) {
        throw std::system_error(static_cast<int>(GetLastError()),
                                std::system_category(),
                                "Cannot encode local filename as UTF-8");
    }
    std::string text(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8,
                        WC_ERR_INVALID_CHARS,
                        wide.data(),
                        static_cast<int>(wide.size()),
                        text.data(),
                        size,
                        nullptr,
                        nullptr);
    return text;
#else
    return path.filename().string();
#endif
}

const std::string& local_owner_name() {
    static const std::string owner = [] {
#ifdef _WIN32
        std::wstring value(1024, L'\0');
        DWORD count = static_cast<DWORD>(value.size());
        if (GetUserNameW(value.data(), &count) != FALSE && count > 1) {
            value.resize(static_cast<std::size_t>(count - 1));
        } else {
            count = GetEnvironmentVariableW(
                L"USERNAME", value.data(), static_cast<DWORD>(value.size()));
            if (count == 0 || count >= value.size()) {
                return std::string{};
            }
            value.resize(count);
        }
        const int size = WideCharToMultiByte(CP_UTF8,
                                             WC_ERR_INVALID_CHARS,
                                             value.data(),
                                             static_cast<int>(value.size()),
                                             nullptr,
                                             0,
                                             nullptr,
                                             nullptr);
        if (size <= 0) {
            return std::string{};
        }
        std::string result(static_cast<std::size_t>(size), '\0');
        WideCharToMultiByte(CP_UTF8,
                            WC_ERR_INVALID_CHARS,
                            value.data(),
                            static_cast<int>(value.size()),
                            result.data(),
                            size,
                            nullptr,
                            nullptr);
        return result;
#else
        return std::string{};
#endif
    }();
    return owner;
}

std::vector<DirectoryEntry> local_roots(const FileUri& directory) {
    (void)directory;
#ifdef _WIN32
    const DWORD required = GetLogicalDriveStringsW(0, nullptr);
    if (required == 0) {
        throw std::system_error(static_cast<int>(GetLastError()),
                                std::system_category(),
                                "Cannot enumerate local drives");
    }
    std::wstring drives(required, L'\0');
    if (GetLogicalDriveStringsW(required, drives.data()) == 0) {
        throw std::system_error(static_cast<int>(GetLastError()),
                                std::system_category(),
                                "Cannot enumerate local drives");
    }
    std::vector<DirectoryEntry> result;
    for (const wchar_t* drive = drives.c_str(); *drive != L'\0'; drive += std::wcslen(drive) + 1) {
        std::wstring_view value(drive);
        if (value.size() < 2)
            continue;
        std::string name;
        name += static_cast<char>(std::toupper(static_cast<unsigned char>(value[0])));
        name += ':';
        auto uri = FileUri::local(name + "/");
        FileStat stat{FileKind::Directory, 0, {}, 0};
        stat.owner = local_owner_name();
        result.push_back({name, std::move(uri), std::move(stat)});
    }
    return result;
#else
    return {};
#endif
}

FileStat make_stat(const std::filesystem::path& path) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error) {
        throw std::system_error(error, "Cannot stat local path");
    }

    FileStat result;
    if (std::filesystem::is_regular_file(status)) {
        result.kind = FileKind::Regular;
        result.size = std::filesystem::file_size(path, error);
        if (error) {
            throw std::system_error(error, "Cannot get local file size");
        }
    } else if (std::filesystem::is_directory(status)) {
        result.kind = FileKind::Directory;
    } else if (std::filesystem::is_symlink(status)) {
        result.kind = FileKind::Symlink;
    }
    result.permissions = static_cast<std::uint32_t>(status.permissions());
    result.owner = local_owner_name();

    const auto changed = std::filesystem::last_write_time(path, error);
    if (!error) {
        result.modified = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            changed - std::filesystem::file_time_type::clock::now() +
            std::chrono::system_clock::now());
    }
    return result;
}

class LocalReadStream final : public IReadStream {
public:
    LocalReadStream(const std::filesystem::path& path, const std::uint64_t offset)
        : stream_(path, std::ios::binary) {
        if (!stream_) {
            throw std::runtime_error("Cannot open local file for reading");
        }
        stream_.seekg(static_cast<std::streamoff>(offset));
        if (!stream_) {
            throw std::runtime_error("Cannot seek local input file");
        }
    }

    std::size_t read(std::span<std::byte> output) override {
        stream_.read(reinterpret_cast<char*>(output.data()),
                     static_cast<std::streamsize>(output.size()));
        if (stream_.bad()) {
            throw std::runtime_error("Cannot read local file");
        }
        return static_cast<std::size_t>(stream_.gcount());
    }

private:
    std::ifstream stream_;
};

class LocalWriteStream final : public IWriteStream {
public:
    LocalWriteStream(const std::filesystem::path& path,
                     const std::uint64_t offset,
                     const bool truncate) {
        if (truncate) {
            stream_.open(path, std::ios::binary | std::ios::out | std::ios::trunc);
        } else {
            stream_.open(path, std::ios::binary | std::ios::in | std::ios::out);
            if (!stream_) {
                stream_.clear();
                stream_.open(path, std::ios::binary | std::ios::out);
                stream_.close();
                stream_.open(path, std::ios::binary | std::ios::in | std::ios::out);
            }
        }
        if (!stream_) {
            throw std::runtime_error("Cannot open local file for writing");
        }
        stream_.seekp(static_cast<std::streamoff>(offset));
        if (!stream_) {
            throw std::runtime_error("Cannot seek local output file");
        }
    }

    void write(std::span<const std::byte> input) override {
        stream_.write(reinterpret_cast<const char*>(input.data()),
                      static_cast<std::streamsize>(input.size()));
        if (!stream_) {
            throw std::runtime_error("Cannot write local file");
        }
    }

    void flush() override {
        stream_.flush();
        if (!stream_) {
            throw std::runtime_error("Cannot flush local file");
        }
    }

private:
    std::fstream stream_;
};

} // namespace

bool LocalEndpoint::accepts(const FileUri& uri) const noexcept {
    return uri.is_local();
}

std::vector<DirectoryEntry> LocalEndpoint::list(const FileUri& directory) {
#ifdef _WIN32
    if (directory.path() == "/") {
        return local_roots(directory);
    }
#endif
    const auto root = native_path(directory);
    std::vector<DirectoryEntry> result;
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        const auto name = utf8_name(entry.path());
        auto stat = make_stat(entry.path());
        bool hidden = false;
#ifdef _WIN32
        const DWORD attributes = GetFileAttributesW(entry.path().c_str());
        hidden = attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_HIDDEN) != 0;
#endif
        result.push_back({name, directory.joined(name), std::move(stat), hidden});
    }
    std::ranges::sort(result, [](const DirectoryEntry& left, const DirectoryEntry& right) {
        if (left.stat.kind != right.stat.kind) {
            return left.stat.kind == FileKind::Directory;
        }
        return left.name < right.name;
    });
    return result;
}

FileStat LocalEndpoint::stat(const FileUri& uri) {
#ifdef _WIN32
    if (uri.path() == "/") {
        return {FileKind::Directory, 0, {}, 0};
    }
#endif
    return make_stat(native_path(uri));
}

bool LocalEndpoint::exists(const FileUri& uri) {
#ifdef _WIN32
    if (uri.path() == "/")
        return true;
#endif
    std::error_code error;
    const bool result = std::filesystem::exists(native_path(uri), error);
    if (error) {
        throw std::system_error(error, "Cannot query local path");
    }
    return result;
}

std::unique_ptr<IReadStream> LocalEndpoint::open_read(const FileUri& uri,
                                                      const std::uint64_t offset) {
    return std::make_unique<LocalReadStream>(native_path(uri), offset);
}

std::unique_ptr<IWriteStream>
LocalEndpoint::open_write(const FileUri& uri, const std::uint64_t offset, const bool truncate) {
    return std::make_unique<LocalWriteStream>(native_path(uri), offset, truncate);
}

void LocalEndpoint::rename(const FileUri& from, const FileUri& to, const bool replace) {
    auto source = native_path(from);
    auto target = native_path(to);
    if (replace && std::filesystem::exists(target)) {
        std::filesystem::remove_all(target);
    } else if (!replace && std::filesystem::exists(target)) {
        throw std::runtime_error("Rename target already exists");
    }
    std::filesystem::rename(source, target);
}

void LocalEndpoint::remove(const FileUri& uri, const bool recursive) {
    if (recursive) {
        std::filesystem::remove_all(native_path(uri));
    } else if (!std::filesystem::remove(native_path(uri))) {
        throw std::runtime_error("Local path does not exist");
    }
}

void LocalEndpoint::mkdir(const FileUri& uri, const bool recursive) {
    const bool created = recursive ? std::filesystem::create_directories(native_path(uri))
                                   : std::filesystem::create_directory(native_path(uri));
    if (!created && !std::filesystem::is_directory(native_path(uri))) {
        throw std::runtime_error("Cannot create local directory");
    }
}

void LocalEndpoint::set_permissions(const FileUri& uri, const std::uint32_t permissions) {
    std::error_code error;
    std::filesystem::permissions(native_path(uri),
                                 static_cast<std::filesystem::perms>(permissions),
                                 std::filesystem::perm_options::replace,
                                 error);
    if (error) {
        throw std::system_error(error, "Cannot set local file permissions");
    }
}

} // namespace SLink::transfer
