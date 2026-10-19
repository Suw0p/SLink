// 便携目录解析与写入探测实现，所有内部路径以模块文件目录为信任根，并验证重解析点
// 不会把数据写出应用目录。
#include "storage/app_paths.h"

#ifndef _WIN32
#error AppPaths is a Windows-only implementation
#endif

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <limits>
#include <system_error>
#include <vector>

namespace SLink::storage {
namespace {

std::wstring Widen(const std::string& text) {
    return std::wstring(text.begin(), text.end());
}

bool EqualPathComponent(const std::filesystem::path& left, const std::filesystem::path& right) {
    const auto left_text = left.wstring();
    const auto right_text = right.wstring();
    return CompareStringOrdinal(left_text.data(),
                                static_cast<int>(left_text.size()),
                                right_text.data(),
                                static_cast<int>(right_text.size()),
                                TRUE) == CSTR_EQUAL;
}

bool PathWithin(const std::filesystem::path& parent, const std::filesystem::path& candidate) {
    std::error_code parent_error;
    std::error_code candidate_error;
    const auto resolved_parent = std::filesystem::weakly_canonical(parent, parent_error);
    const auto resolved_candidate = std::filesystem::weakly_canonical(candidate, candidate_error);
    if (parent_error || candidate_error) {
        return false;
    }
    auto parent_part = resolved_parent.begin();
    auto candidate_part = resolved_candidate.begin();
    for (; parent_part != resolved_parent.end(); ++parent_part, ++candidate_part) {
        if (candidate_part == resolved_candidate.end() ||
            !EqualPathComponent(*parent_part, *candidate_part)) {
            return false;
        }
    }
    return true;
}

std::wstring WindowsErrorMessage(const wchar_t* operation, DWORD error) {
    wchar_t* system_message = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error,
        0,
        reinterpret_cast<wchar_t*>(&system_message),
        0,
        nullptr);
    std::wstring message(operation);
    message.append(L": ");
    if (length != 0 && system_message != nullptr) {
        message.append(system_message, length);
        LocalFree(system_message);
    } else {
        message.append(L"Windows error ");
        message.append(std::to_wstring(error));
    }
    while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n')) {
        message.pop_back();
    }
    return message;
}

StorageStatus EnsureDirectory(const std::filesystem::path& path) {
    std::error_code error;
    if (std::filesystem::exists(path, error)) {
        if (error) {
            return StorageStatus::Error(StorageErrc::io_error, Widen(error.message()));
        }
        if (!std::filesystem::is_directory(path, error) || error) {
            return StorageStatus::Error(StorageErrc::io_error,
                                        L"Required portable data path is not a directory: " +
                                            path.wstring());
        }
        return StorageStatus::Ok();
    }

    std::filesystem::create_directories(path, error);
    if (error) {
        const auto detail = Widen(error.message());
        return StorageStatus::Error(StorageErrc::io_error,
                                    L"Cannot create portable data directory " + path.wstring() +
                                        L": " + detail);
    }
    return StorageStatus::Ok();
}

StorageStatus ProbeWritable(const std::filesystem::path& directory) {
    static std::atomic<unsigned long> sequence{0};
    const auto probe_name = L".SLink-write-probe-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                            std::to_wstring(sequence.fetch_add(1, std::memory_order_relaxed));
    const auto probe_path = directory / probe_name;

    HANDLE file = CreateFileW(probe_path.c_str(),
                              GENERIC_WRITE,
                              0,
                              nullptr,
                              CREATE_NEW,
                              FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_TEMPORARY,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return StorageStatus::Error(
            StorageErrc::io_error,
            WindowsErrorMessage(
                (L"Portable directory is not writable: " + directory.wstring()).c_str(),
                GetLastError()));
    }

    constexpr std::byte probe_byte{0x53};
    DWORD written = 0;
    const bool write_ok =
        WriteFile(file, &probe_byte, sizeof(probe_byte), &written, nullptr) != FALSE &&
        written == sizeof(probe_byte) && FlushFileBuffers(file) != FALSE;
    const DWORD write_error = write_ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);

    if (!DeleteFileW(probe_path.c_str())) {
        return StorageStatus::Error(
            StorageErrc::io_error,
            WindowsErrorMessage(L"Cannot remove portable directory write probe", GetLastError()));
    }
    if (!write_ok) {
        return StorageStatus::Error(
            StorageErrc::io_error,
            WindowsErrorMessage(L"Cannot write portable directory probe", write_error));
    }
    return StorageStatus::Ok();
}

StorageResult<std::filesystem::path> CurrentExecutablePath() {
    std::vector<wchar_t> buffer(512);
    for (;;) {
        const DWORD copied =
            GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (copied == 0) {
            return StorageResult<std::filesystem::path>::Failure(StorageStatus::Error(
                StorageErrc::io_error,
                WindowsErrorMessage(L"GetModuleFileNameW failed", GetLastError())));
        }
        if (copied < buffer.size() - 1) {
            return StorageResult<std::filesystem::path>::Success(
                std::filesystem::path(std::wstring_view(buffer.data(), copied)));
        }
        if (buffer.size() >= 32768) {
            return StorageResult<std::filesystem::path>::Failure(StorageStatus::Error(
                StorageErrc::io_error, L"Executable path exceeds the Windows path limit"));
        }
        buffer.resize((std::min)(buffer.size() * 2, static_cast<std::size_t>(32768)));
    }
}

} // namespace

StorageResult<AppPaths> AppPaths::InitializeForCurrentExecutable() {
    auto executable = CurrentExecutablePath();
    if (!executable) {
        return StorageResult<AppPaths>::Failure(executable.status());
    }
    return InitializeFromExecutablePath(std::move(executable).value());
}

StorageResult<AppPaths>
AppPaths::InitializeFromExecutablePath(const std::filesystem::path& executable_path) {
    if (executable_path.empty() || !executable_path.is_absolute() ||
        executable_path.filename().empty()) {
        return StorageResult<AppPaths>::Failure(StorageStatus::Error(
            StorageErrc::invalid_argument,
            L"Executable path must be an absolute path containing a filename"));
    }

    AppPaths paths;
    paths.executable_path_ = executable_path.lexically_normal();
    paths.application_dir_ = paths.executable_path_.parent_path();
    paths.data_dir_ = paths.application_dir_ / L"data";
    paths.cache_dir_ = paths.application_dir_ / L"cache";
    paths.editor_cache_dir_ = paths.cache_dir_ / L"editor";
    paths.logs_dir_ = paths.application_dir_ / L"logs";
    paths.crash_dir_ = paths.logs_dir_ / L"crash";

    const std::array directories{
        paths.data_dir_,
        paths.cache_dir_,
        paths.editor_cache_dir_,
        paths.logs_dir_,
        paths.crash_dir_,
    };
    for (const auto& directory : directories) {
        // Validate the nearest existing parent before creating anything. This
        // prevents an existing junction such as cache\ -> another drive from
        // causing create_directories(cache\editor) to write outside the
        // portable application tree before we reject it.
        if (!PathWithin(paths.application_dir_, directory)) {
            return StorageResult<AppPaths>::Failure(StorageStatus::Error(
                StorageErrc::io_error,
                L"Portable data directory resolves outside the executable directory: " +
                    directory.wstring()));
        }
        if (auto status = EnsureDirectory(directory); !status) {
            return StorageResult<AppPaths>::Failure(std::move(status));
        }
        if (!PathWithin(paths.application_dir_, directory)) {
            return StorageResult<AppPaths>::Failure(StorageStatus::Error(
                StorageErrc::io_error,
                L"Portable data directory resolves outside the executable directory: " +
                    directory.wstring()));
        }
    }
    for (const auto& directory : directories) {
        if (auto status = ProbeWritable(directory); !status) {
            return StorageResult<AppPaths>::Failure(std::move(status));
        }
    }
    return StorageResult<AppPaths>::Success(std::move(paths));
}

bool AppPaths::ContainsInternalPath(const std::filesystem::path& path) const {
    if (path.empty() || !path.is_absolute()) {
        return false;
    }
    return PathWithin(application_dir_, path);
}

} // namespace SLink::storage
