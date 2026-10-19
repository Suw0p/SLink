// 本机终端实现：创建 ConPTY 与 cmd.exe 作业对象，并通过独立读写线程连接到
// TerminalControl，确保关闭会终止完整子进程树。
#include "app/local_terminal_session.hpp"

#include "ui/terminal_control.hpp"

#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace SLink::app {
namespace {

constexpr std::size_t kMaximumQueuedInput = 1U * 1024U * 1024U;
constexpr SHORT kMinimumRows = 2;
constexpr SHORT kMinimumColumns = 8;

class UniqueHandle final {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE value) noexcept : value_(value) {}
    ~UniqueHandle() {
        reset();
    }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            reset(std::exchange(other.value_, nullptr));
        }
        return *this;
    }

    [[nodiscard]] HANDLE get() const noexcept {
        return value_;
    }
    [[nodiscard]] explicit operator bool() const noexcept {
        return value_ && value_ != INVALID_HANDLE_VALUE;
    }
    HANDLE release() noexcept {
        return std::exchange(value_, nullptr);
    }
    void reset(HANDLE value = nullptr) noexcept {
        if (*this) {
            CloseHandle(value_);
        }
        value_ = value;
    }

private:
    HANDLE value_{};
};

std::wstring WindowsError(const DWORD error) {
    wchar_t* message = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error,
        0,
        reinterpret_cast<LPWSTR>(&message),
        0,
        nullptr);
    std::wstring result = length && message ? std::wstring(message, length)
                                            : L"Windows error " + std::to_wstring(error);
    if (message) {
        LocalFree(message);
    }
    while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n')) {
        result.pop_back();
    }
    return result;
}

Status WindowsStatus(std::wstring_view operation, const DWORD error = GetLastError()) {
    return {ErrorCode::io_error, std::wstring(operation) + L"：" + WindowsError(error)};
}

std::wstring CmdPath() {
    std::array<wchar_t, MAX_PATH> system_directory{};
    const UINT length =
        GetSystemDirectoryW(system_directory.data(), static_cast<UINT>(system_directory.size()));
    if (length == 0 || length >= system_directory.size()) {
        return L"cmd.exe";
    }
    std::wstring result(system_directory.data(), length);
    result += L"\\cmd.exe";
    return result;
}

std::wstring UserProfileDirectory() {
    const DWORD required = GetEnvironmentVariableW(L"USERPROFILE", nullptr, 0);
    if (required <= 1) {
        return {};
    }
    std::wstring result(required, L'\0');
    const DWORD copied = GetEnvironmentVariableW(L"USERPROFILE", result.data(), required);
    if (copied == 0 || copied >= required) {
        return {};
    }
    result.resize(copied);
    return result;
}

void CancelThreadIo(std::jthread& thread) noexcept {
    if (thread.joinable()) {
        (void)CancelSynchronousIo(thread.native_handle());
        thread.request_stop();
    }
}

} // namespace

struct LocalTerminalSession::Impl {
    explicit Impl(ui::TerminalControl& value) : terminal(&value) {}

    ui::TerminalControl* terminal{};
    HPCON pseudo_console{};
    UniqueHandle input_write;
    UniqueHandle output_read;
    UniqueHandle process;
    UniqueHandle job;
    std::mutex input_mutex;
    std::condition_variable input_changed;
    std::deque<std::string> input_queue;
    std::size_t queued_input_bytes{};
    std::jthread reader;
    std::jthread writer;
    std::atomic_bool stopping{};

    void QueueInput(const std::string_view input) {
        if (input.empty() || stopping.load(std::memory_order_acquire)) {
            return;
        }
        {
            std::scoped_lock lock(input_mutex);
            if (input.size() > kMaximumQueuedInput ||
                queued_input_bytes > kMaximumQueuedInput - input.size()) {
                return;
            }
            input_queue.emplace_back(input);
            queued_input_bytes += input.size();
        }
        input_changed.notify_one();
    }

    void Resize(const std::size_t rows, const std::size_t columns) const {
        if (!pseudo_console || stopping.load(std::memory_order_acquire)) {
            return;
        }
        const COORD size{
            static_cast<SHORT>(std::clamp<std::size_t>(columns, kMinimumColumns, 32767)),
            static_cast<SHORT>(std::clamp<std::size_t>(rows, kMinimumRows, 32767))};
        (void)ResizePseudoConsole(pseudo_console, size);
    }

    void ReadOutput(const std::stop_token stop) {
        std::array<char, 64 * 1024> buffer{};
        while (!stop.stop_requested() && !stopping.load(std::memory_order_acquire)) {
            DWORD received = 0;
            if (!ReadFile(output_read.get(),
                          buffer.data(),
                          static_cast<DWORD>(buffer.size()),
                          &received,
                          nullptr)) {
                const DWORD error = GetLastError();
                if (error == ERROR_BROKEN_PIPE || error == ERROR_OPERATION_ABORTED ||
                    stopping.load(std::memory_order_acquire)) {
                    break;
                }
                break;
            }
            if (received == 0) {
                break;
            }
            if (terminal) {
                terminal->FeedUtf8(std::string_view(buffer.data(), received));
            }
        }
        if (!stopping.load(std::memory_order_acquire) && terminal) {
            terminal->FeedUtf8("\r\n\x1b[36m本机 CMD 已结束。\x1b[0m\r\n");
        }
    }

    void WriteInput(const std::stop_token stop) {
        while (!stop.stop_requested()) {
            std::string input;
            {
                std::unique_lock lock(input_mutex);
                input_changed.wait(lock, [&] {
                    return stop.stop_requested() || stopping.load(std::memory_order_acquire) ||
                           !input_queue.empty();
                });
                if (stop.stop_requested() || stopping.load(std::memory_order_acquire)) {
                    break;
                }
                input = std::move(input_queue.front());
                input_queue.pop_front();
                queued_input_bytes -= input.size();
            }
            std::size_t offset = 0;
            while (offset < input.size() && !stop.stop_requested() &&
                   !stopping.load(std::memory_order_acquire)) {
                DWORD written = 0;
                const DWORD remaining =
                    static_cast<DWORD>(std::min<std::size_t>(input.size() - offset, 64U * 1024U));
                if (!WriteFile(
                        input_write.get(), input.data() + offset, remaining, &written, nullptr) ||
                    written == 0) {
                    return;
                }
                offset += written;
            }
        }
    }

    Status Start() {
        SECURITY_ATTRIBUTES security{};
        security.nLength = sizeof(security);
        security.bInheritHandle = FALSE;
        UniqueHandle input_read;
        UniqueHandle output_write;
        HANDLE raw_input_read = nullptr;
        HANDLE raw_input_write = nullptr;
        if (!CreatePipe(&raw_input_read, &raw_input_write, &security, 0)) {
            return WindowsStatus(L"无法创建 ConPTY 输入管道");
        }
        input_read.reset(raw_input_read);
        input_write.reset(raw_input_write);
        HANDLE raw_output_read = nullptr;
        HANDLE raw_output_write = nullptr;
        if (!CreatePipe(&raw_output_read, &raw_output_write, &security, 0)) {
            return WindowsStatus(L"无法创建 ConPTY 输出管道");
        }
        output_read.reset(raw_output_read);
        output_write.reset(raw_output_write);

        const COORD size{static_cast<SHORT>(std::clamp<std::size_t>(
                             terminal ? terminal->columns() : 80, kMinimumColumns, 32767)),
                         static_cast<SHORT>(std::clamp<std::size_t>(
                             terminal ? terminal->rows() : 24, kMinimumRows, 32767))};
        const HRESULT console_result =
            CreatePseudoConsole(size, input_read.get(), output_write.get(), 0, &pseudo_console);
        if (FAILED(console_result)) {
            return {ErrorCode::unsupported,
                    L"无法创建 Windows ConPTY，错误代码：" +
                        std::to_wstring(static_cast<unsigned long>(console_result))};
        }
        input_read.reset();
        output_write.reset();

        SIZE_T attribute_bytes = 0;
        (void)InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_bytes);
        std::vector<std::byte> attributes(attribute_bytes);
        auto* attribute_list = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
        if (!InitializeProcThreadAttributeList(attribute_list, 1, 0, &attribute_bytes)) {
            return WindowsStatus(L"无法初始化 ConPTY 进程属性");
        }
        struct AttributeGuard final {
            PPROC_THREAD_ATTRIBUTE_LIST value{};
            ~AttributeGuard() {
                if (value)
                    DeleteProcThreadAttributeList(value);
            }
        } attribute_guard{attribute_list};
        if (!UpdateProcThreadAttribute(attribute_list,
                                       0,
                                       PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                       pseudo_console,
                                       sizeof(pseudo_console),
                                       nullptr,
                                       nullptr)) {
            return WindowsStatus(L"无法绑定 ConPTY 进程属性");
        }

        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.lpAttributeList = attribute_list;
        PROCESS_INFORMATION process_info{};
        const std::wstring executable = CmdPath();
        std::wstring command_line = L"\"" + executable + L"\"";
        const std::wstring working_directory = UserProfileDirectory();
        if (!CreateProcessW(executable.c_str(),
                            command_line.data(),
                            nullptr,
                            nullptr,
                            FALSE,
                            EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
                            nullptr,
                            working_directory.empty() ? nullptr : working_directory.c_str(),
                            &startup.StartupInfo,
                            &process_info)) {
            return WindowsStatus(L"无法启动 cmd.exe");
        }
        UniqueHandle thread(process_info.hThread);
        process.reset(process_info.hProcess);

        job.reset(CreateJobObjectW(nullptr, nullptr));
        if (job) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if (!SetInformationJobObject(
                    job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
                !AssignProcessToJobObject(job.get(), process.get())) {
                job.reset();
            }
        }

        terminal->SetInputCallback([this](const ui::TerminalInputEvent event) {
            QueueInput(event.bytes);
        });
        terminal->SetResizeCallback([this](const std::size_t rows, const std::size_t columns) {
            Resize(rows, columns);
        });
        reader = std::jthread([this](const std::stop_token stop) {
            ReadOutput(stop);
        });
        writer = std::jthread([this](const std::stop_token stop) {
            WriteInput(stop);
        });
        return Status::ok();
    }

    void Stop() noexcept {
        if (stopping.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        if (terminal) {
            terminal->SetInputCallback({});
            terminal->SetResizeCallback({});
        }
        {
            std::scoped_lock lock(input_mutex);
            input_queue.clear();
            queued_input_bytes = 0;
        }
        input_changed.notify_all();
        if (job) {
            (void)TerminateJobObject(job.get(), 0);
        } else if (process) {
            (void)TerminateProcess(process.get(), 0);
        }
        if (pseudo_console) {
            ClosePseudoConsole(pseudo_console);
            pseudo_console = nullptr;
        }
        CancelThreadIo(reader);
        CancelThreadIo(writer);
        input_write.reset();
        output_read.reset();
        if (reader.joinable())
            reader.join();
        if (writer.joinable())
            writer.join();
        process.reset();
        job.reset();
        terminal = nullptr;
    }
};

LocalTerminalSession::LocalTerminalSession(ui::TerminalControl& terminal)
    : impl_(std::make_unique<Impl>(terminal)) {}

LocalTerminalSession::~LocalTerminalSession() {
    Stop();
}

Status LocalTerminalSession::Start() {
    return impl_->Start();
}

void LocalTerminalSession::Stop() noexcept {
    if (impl_)
        impl_->Stop();
}

ui::TerminalControl* LocalTerminalSession::terminal() const noexcept {
    return impl_ ? impl_->terminal : nullptr;
}

} // namespace SLink::app
