// 便携崩溃转储实现：使用进程级异常过滤器生成受路径约束的 minidump，并避免在异常
// 路径中进行不必要的复杂分配或访问应用其他子系统。
#include "storage/crash_reporter.h"

#ifndef _WIN32
#error CrashReporter is a Windows-only implementation
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <DbgHelp.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cwchar>
#include <mutex>
#include <string_view>

namespace SLink::storage {
namespace {

constexpr std::size_t kMaximumCrashDirectoryCharacters = 32767;
constexpr unsigned int kMaximumCreateAttempts = 16;

std::mutex g_install_mutex;
std::array<wchar_t, kMaximumCrashDirectoryCharacters + 1> g_crash_directory{};
std::array<wchar_t, kMaximumCrashDirectoryCharacters + 96> g_dump_path{};
std::size_t g_crash_directory_length = 0;
std::atomic<bool> g_installed{false};
std::atomic<bool> g_handling_exception{false};
std::atomic<std::uint32_t> g_dump_sequence{0};
LPTOP_LEVEL_EXCEPTION_FILTER g_previous_filter = nullptr;
HANDLE g_logs_directory_handle = INVALID_HANDLE_VALUE;
HANDLE g_crash_directory_handle = INVALID_HANDLE_VALUE;

bool EqualPath(const std::filesystem::path& left, const std::filesystem::path& right) {
    const std::wstring left_text = left.lexically_normal().wstring();
    const std::wstring right_text = right.lexically_normal().wstring();
    return CompareStringOrdinal(left_text.data(),
                                static_cast<int>(left_text.size()),
                                right_text.data(),
                                static_cast<int>(right_text.size()),
                                TRUE) == CSTR_EQUAL;
}

HANDLE OpenSecuredDirectory(const std::filesystem::path& directory) noexcept {
    HANDLE handle = CreateFileW(directory.c_str(),
                                FILE_READ_ATTRIBUTES,
                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr,
                                OPEN_EXISTING,
                                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return INVALID_HANDLE_VALUE;
    }

    FILE_ATTRIBUTE_TAG_INFO tag_information{};
    if (GetFileInformationByHandleEx(handle,
                                     FileAttributeTagInfo,
                                     &tag_information,
                                     static_cast<DWORD>(sizeof(tag_information))) == FALSE ||
        (tag_information.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (tag_information.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        CloseHandle(handle);
        return INVALID_HANDLE_VALUE;
    }
    return handle;
}

CrashTimestamp CurrentUtcTimestamp() noexcept {
    SYSTEMTIME current{};
    GetSystemTime(&current);
    return CrashTimestamp{current.wYear,
                          current.wMonth,
                          current.wDay,
                          current.wHour,
                          current.wMinute,
                          current.wSecond,
                          current.wMilliseconds};
}

int FormatDumpFileName(const CrashTimestamp& timestamp,
                       std::uint32_t process_id,
                       std::uint32_t sequence,
                       wchar_t* output,
                       std::size_t output_characters) noexcept {
    if (output == nullptr || output_characters == 0) {
        return -1;
    }
    return swprintf_s(output,
                      output_characters,
                      L"SLink-crash-%04u%02u%02u-%02u%02u%02u-%03u-p%lu-s%lu.dmp",
                      static_cast<unsigned int>(timestamp.year),
                      static_cast<unsigned int>(timestamp.month),
                      static_cast<unsigned int>(timestamp.day),
                      static_cast<unsigned int>(timestamp.hour),
                      static_cast<unsigned int>(timestamp.minute),
                      static_cast<unsigned int>(timestamp.second),
                      static_cast<unsigned int>(timestamp.millisecond),
                      static_cast<unsigned long>(process_id),
                      static_cast<unsigned long>(sequence));
}

bool BuildDumpPathInPlace(const CrashTimestamp& timestamp,
                          std::uint32_t process_id,
                          std::uint32_t sequence,
                          wchar_t* output,
                          std::size_t output_characters) noexcept {
    if (output == nullptr || output_characters == 0 || g_crash_directory_length == 0 ||
        g_crash_directory_length >= output_characters) {
        return false;
    }

    std::wmemcpy(output, g_crash_directory.data(), g_crash_directory_length);
    std::size_t offset = g_crash_directory_length;
    if (output[offset - 1] != L'\\' && output[offset - 1] != L'/') {
        if (offset + 1 >= output_characters) {
            return false;
        }
        output[offset++] = L'\\';
    }

    const int written = FormatDumpFileName(
        timestamp, process_id, sequence, output + offset, output_characters - offset);
    return written > 0 && offset + static_cast<std::size_t>(written) < output_characters;
}

LONG WINAPI WriteUnhandledExceptionDump(EXCEPTION_POINTERS* exception_pointers) noexcept {
    if (!g_installed.load(std::memory_order_acquire) ||
        g_handling_exception.exchange(true, std::memory_order_acq_rel)) {
        return EXCEPTION_EXECUTE_HANDLER;
    }

    const DWORD process_id = GetCurrentProcessId();
    HANDLE dump_file = INVALID_HANDLE_VALUE;

    for (unsigned int attempt = 0; attempt < kMaximumCreateAttempts; ++attempt) {
        const auto sequence = g_dump_sequence.fetch_add(1, std::memory_order_relaxed);
        if (!BuildDumpPathInPlace(CurrentUtcTimestamp(),
                                  static_cast<std::uint32_t>(process_id),
                                  sequence,
                                  g_dump_path.data(),
                                  g_dump_path.size())) {
            break;
        }

        dump_file = CreateFileW(g_dump_path.data(),
                                GENERIC_WRITE,
                                0,
                                nullptr,
                                CREATE_NEW,
                                FILE_ATTRIBUTE_NORMAL,
                                nullptr);
        if (dump_file != INVALID_HANDLE_VALUE || GetLastError() != ERROR_FILE_EXISTS) {
            break;
        }
    }

    if (dump_file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION exception_information{};
        exception_information.ThreadId = GetCurrentThreadId();
        exception_information.ExceptionPointers = exception_pointers;
        exception_information.ClientPointers = FALSE;

        const auto dump_type = static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithThreadInfo |
                                                          MiniDumpWithUnloadedModules);
        const BOOL wrote_dump =
            MiniDumpWriteDump(GetCurrentProcess(),
                              process_id,
                              dump_file,
                              dump_type,
                              exception_pointers != nullptr ? &exception_information : nullptr,
                              nullptr,
                              nullptr);
        if (wrote_dump != FALSE) {
            FlushFileBuffers(dump_file);
        }
        CloseHandle(dump_file);

        // A partial dump is misleading. This is the same validated portable
        // path used for creation; no alternate path is ever attempted.
        if (wrote_dump == FALSE) {
            DeleteFileW(g_dump_path.data());
        }
    }

    return EXCEPTION_EXECUTE_HANDLER;
}

} // namespace

StorageStatus CrashReporter::Install(const AppPaths& paths) {
    const auto& crash_directory = paths.crash_dir();
    if (crash_directory.empty() || !crash_directory.is_absolute() ||
        !paths.ContainsInternalPath(crash_directory) ||
        !EqualPath(crash_directory.parent_path(), paths.logs_dir())) {
        return StorageStatus::Error(
            StorageErrc::invalid_argument,
            L"Crash directory is outside the portable application directory");
    }

    std::error_code error;
    const auto attributes = GetFileAttributesW(crash_directory.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return StorageStatus::Error(
            StorageErrc::io_error,
            L"Portable crash path must be an existing non-reparse directory: " +
                crash_directory.wstring());
    }

    const auto canonical_directory = std::filesystem::weakly_canonical(crash_directory, error);
    if (error || !paths.ContainsInternalPath(canonical_directory)) {
        return StorageStatus::Error(StorageErrc::io_error,
                                    L"Portable crash directory could not be resolved safely");
    }

    const std::wstring directory_text = canonical_directory.wstring();
    if (directory_text.empty() || directory_text.size() > kMaximumCrashDirectoryCharacters) {
        return StorageStatus::Error(StorageErrc::invalid_argument,
                                    L"Portable crash directory path is too long");
    }

    std::lock_guard lock(g_install_mutex);
    if (g_installed.load(std::memory_order_acquire)) {
        const std::wstring_view installed_directory(g_crash_directory.data(),
                                                    g_crash_directory_length);
        if (CompareStringOrdinal(installed_directory.data(),
                                 static_cast<int>(installed_directory.size()),
                                 directory_text.data(),
                                 static_cast<int>(directory_text.size()),
                                 TRUE) == CSTR_EQUAL) {
            return StorageStatus::Ok();
        }
        return StorageStatus::Error(StorageErrc::invalid_argument,
                                    L"Crash reporter is already installed for another directory");
    }

    // Keep both ancestors without FILE_SHARE_DELETE for the lifetime of the
    // filter. This prevents logs or logs/crash from being renamed and replaced
    // with a junction after validation but before an exception occurs.
    HANDLE logs_directory_handle = OpenSecuredDirectory(paths.logs_dir());
    if (logs_directory_handle == INVALID_HANDLE_VALUE) {
        return StorageStatus::Error(StorageErrc::io_error,
                                    L"Cannot secure the portable logs directory");
    }
    HANDLE crash_directory_handle = OpenSecuredDirectory(canonical_directory);
    if (crash_directory_handle == INVALID_HANDLE_VALUE) {
        CloseHandle(logs_directory_handle);
        return StorageStatus::Error(StorageErrc::io_error,
                                    L"Cannot secure the portable crash directory");
    }

    std::fill(g_crash_directory.begin(), g_crash_directory.end(), L'\0');
    std::copy(directory_text.begin(), directory_text.end(), g_crash_directory.begin());
    g_crash_directory_length = directory_text.size();
    g_dump_sequence.store(0, std::memory_order_release);
    g_handling_exception.store(false, std::memory_order_release);
    std::fill(g_dump_path.begin(), g_dump_path.end(), L'\0');
    g_logs_directory_handle = logs_directory_handle;
    g_crash_directory_handle = crash_directory_handle;
    g_previous_filter = SetUnhandledExceptionFilter(&WriteUnhandledExceptionDump);
    g_installed.store(true, std::memory_order_release);
    return StorageStatus::Ok();
}

void CrashReporter::Uninstall() noexcept {
    std::lock_guard lock(g_install_mutex);
    if (!g_installed.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    SetUnhandledExceptionFilter(g_previous_filter);
    g_previous_filter = nullptr;
    if (g_crash_directory_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(g_crash_directory_handle);
        g_crash_directory_handle = INVALID_HANDLE_VALUE;
    }
    if (g_logs_directory_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(g_logs_directory_handle);
        g_logs_directory_handle = INVALID_HANDLE_VALUE;
    }
    g_handling_exception.store(false, std::memory_order_release);
    g_crash_directory_length = 0;
    std::fill(g_crash_directory.begin(), g_crash_directory.end(), L'\0');
    std::fill(g_dump_path.begin(), g_dump_path.end(), L'\0');
}

bool CrashReporter::IsInstalled() noexcept {
    return g_installed.load(std::memory_order_acquire);
}

std::wstring CrashReporter::BuildDumpFileName(const CrashTimestamp& timestamp,
                                              std::uint32_t process_id,
                                              std::uint32_t sequence) {
    std::array<wchar_t, 128> filename{};
    const int written =
        FormatDumpFileName(timestamp, process_id, sequence, filename.data(), filename.size());
    if (written <= 0) {
        return {};
    }
    return std::wstring(filename.data(), static_cast<std::size_t>(written));
}

std::filesystem::path CrashReporter::BuildDumpPath(const AppPaths& paths,
                                                   const CrashTimestamp& timestamp,
                                                   std::uint32_t process_id,
                                                   std::uint32_t sequence) {
    return paths.crash_dir() / BuildDumpFileName(timestamp, process_id, sequence);
}

bool CrashReporter::IsAllowedDumpPath(const AppPaths& paths,
                                      const std::filesystem::path& candidate) {
    if (candidate.empty() || !candidate.is_absolute() || candidate.extension() != L".dmp" ||
        !EqualPath(candidate.parent_path(), paths.crash_dir()) ||
        !paths.ContainsInternalPath(candidate)) {
        return false;
    }

    const auto filename = candidate.filename().wstring();
    constexpr std::wstring_view prefix = L"SLink-crash-";
    return filename.size() > prefix.size() && filename.compare(0, prefix.size(), prefix) == 0;
}

} // namespace SLink::storage
