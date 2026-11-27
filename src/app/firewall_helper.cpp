// 防火墙管理员辅助进程及主进程客户端实现。双方仅通过受限命名管道协议增删本会话的
// TCP 入站规则，退出时执行会话级清理。
#include "app/firewall_helper.hpp"

#include <windows.h>
#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstdint>
#include <cwctype>
#include <deque>
#include <mutex>
#include <netfw.h>
#include <set>
#include <shellapi.h>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace SLink::app {
namespace {

constexpr std::uint32_t kProtocolMagic = 0x4B4E4C53U; // SLNK
constexpr std::uint32_t kProtocolVersion = 2;
constexpr wchar_t kPipePrefix[] = LR"(\\.\pipe\SLink.Firewall.)";
constexpr wchar_t kRulePrefix[] = L"SLink Port Forward ";
constexpr wchar_t kRuleGroup[] = L"SLink";
constexpr DWORD kIpcTimeoutMs = 15'000;

enum class HelperOperation : std::uint32_t {
    initialize = 1,
    add_rule = 2,
    remove_rule = 3,
    shutdown_if_clean = 4,
    terminate_after_cleanup = 5,
    cleanup_recovery_rule = 6,
};

struct HelperRequest {
    std::uint32_t magic{kProtocolMagic};
    std::uint32_t version{kProtocolVersion};
    HelperOperation operation{HelperOperation::initialize};
    std::uint16_t local_port{};
    std::uint16_t reserved{};
    wchar_t cleanup_session[33]{};
};

struct HelperResponse {
    std::uint32_t magic{kProtocolMagic};
    std::uint32_t version{kProtocolVersion};
    std::int32_t result{};
    std::uint32_t enabled{};
    std::uint32_t cleanup_confirmed{};
    wchar_t message[192]{};
};

static_assert(std::is_trivially_copyable_v<HelperRequest>);
static_assert(std::is_trivially_copyable_v<HelperResponse>);

template <typename T> class ComPtr final {
public:
    ComPtr() = default;
    ~ComPtr() {
        Reset();
    }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ComPtr(ComPtr&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}
    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this != &other) {
            Reset();
            value_ = std::exchange(other.value_, nullptr);
        }
        return *this;
    }
    T* operator->() const noexcept {
        return value_;
    }
    T* get() const noexcept {
        return value_;
    }
    T** put() noexcept {
        Reset();
        return &value_;
    }
    explicit operator bool() const noexcept {
        return value_ != nullptr;
    }
    void Reset() noexcept {
        if (value_)
            value_->Release();
        value_ = nullptr;
    }

private:
    T* value_{};
};

class UniqueHandle final {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE value) : value_(value) {}
    ~UniqueHandle() {
        Reset();
    }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    UniqueHandle(UniqueHandle&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            Reset();
            value_ = std::exchange(other.value_, nullptr);
        }
        return *this;
    }
    HANDLE get() const noexcept {
        return value_;
    }
    HANDLE release() noexcept {
        return std::exchange(value_, nullptr);
    }
    explicit operator bool() const noexcept {
        return value_ && value_ != INVALID_HANDLE_VALUE;
    }
    void Reset(HANDLE value = nullptr) noexcept {
        if (value_ && value_ != INVALID_HANDLE_VALUE)
            CloseHandle(value_);
        value_ = value;
    }

private:
    HANDLE value_{};
};

std::wstring WindowsMessage(HRESULT result) {
    wchar_t* raw{};
    const DWORD size = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                          FORMAT_MESSAGE_IGNORE_INSERTS,
                                      nullptr,
                                      static_cast<DWORD>(result),
                                      0,
                                      reinterpret_cast<wchar_t*>(&raw),
                                      0,
                                      nullptr);
    std::wstring text = size && raw ? std::wstring(raw, size) : L"Windows 防火墙操作失败。";
    if (raw)
        LocalFree(raw);
    while (!text.empty() &&
           (text.back() == L'\r' || text.back() == L'\n' || iswspace(text.back()))) {
        text.pop_back();
    }
    return text;
}

void SetResponseMessage(HelperResponse& response, std::wstring_view text) {
    const std::size_t count = std::min(text.size(), std::size(response.message) - 1);
    std::copy_n(text.data(), count, response.message);
    response.message[count] = L'\0';
}

std::wstring ExecutablePath() {
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size())
        return {};
    path.resize(length);
    return path;
}

std::wstring MakeSessionId() {
    GUID id{};
    if (FAILED(CoCreateGuid(&id)))
        return {};
    wchar_t value[40]{};
    if (StringFromGUID2(id, value, static_cast<int>(std::size(value))) <= 0)
        return {};
    std::wstring result;
    result.reserve(32);
    for (const wchar_t ch : std::wstring_view(value)) {
        if (iswxdigit(ch))
            result.push_back(static_cast<wchar_t>(towlower(ch)));
    }
    return result.size() == 32 ? result : std::wstring{};
}

bool ValidSessionId(std::wstring_view value) {
    return value.size() == 32 && std::ranges::all_of(value, [](wchar_t ch) {
               return iswxdigit(ch);
           });
}

std::wstring RuleName(std::wstring_view session, std::uint16_t port) {
    return std::wstring(kRulePrefix) + std::wstring(session) + L" " + std::to_wstring(port);
}

std::wstring SessionMutexName(std::wstring_view session) {
    return L"Local\\SLink.Firewall.Session." + std::wstring(session);
}

// 访问被拒绝等错误不能解释为会话不存在，否则会误删仍在使用的其他实例规则。
bool SessionDefinitelyInactive(std::wstring_view session) {
    UniqueHandle active(OpenMutexW(SYNCHRONIZE, FALSE, SessionMutexName(session).c_str()));
    return !active && GetLastError() == ERROR_FILE_NOT_FOUND;
}

// 先完整、可靠地枚举，再开始删除。枚举中断、接口/名称读取失败均不能返回成功。
template <typename Next, typename ReadName>
HRESULT CollectRuleNames(Next next, ReadName read_name, std::vector<std::wstring>& names) {
    for (;;) {
        const HRESULT result = next();
        if (result == S_FALSE)
            return S_OK;
        if (result != S_OK)
            return FAILED(result) ? result : E_UNEXPECTED;
        std::wstring name;
        const HRESULT read = read_name(name);
        if (read != S_OK)
            return FAILED(read) ? read : E_UNEXPECTED;
        names.push_back(std::move(name));
    }
}

class FirewallPolicy final {
public:
    HRESULT Open() {
        if (rules_)
            return S_OK;
        HRESULT result = CoCreateInstance(__uuidof(NetFwPolicy2),
                                          nullptr,
                                          CLSCTX_INPROC_SERVER,
                                          __uuidof(INetFwPolicy2),
                                          reinterpret_cast<void**>(policy_.put()));
        if (FAILED(result))
            return result;
        return policy_->get_Rules(rules_.put());
    }

    HRESULT Add(std::wstring_view session, std::uint16_t port) {
        HRESULT result = Open();
        if (FAILED(result))
            return result;
        ComPtr<INetFwRule> rule;
        result = CoCreateInstance(__uuidof(NetFwRule),
                                  nullptr,
                                  CLSCTX_INPROC_SERVER,
                                  __uuidof(INetFwRule),
                                  reinterpret_cast<void**>(rule.put()));
        if (FAILED(result))
            return result;

        const std::wstring name = RuleName(session, port);
        const std::wstring port_text = std::to_wstring(port);
        const std::wstring executable = ExecutablePath();
        if (executable.empty())
            return E_UNEXPECTED;

        const auto set_string = [](auto setter, std::wstring_view value) {
            BSTR text = SysAllocStringLen(value.data(), static_cast<UINT>(value.size()));
            if (!text)
                return E_OUTOFMEMORY;
            const HRESULT set_result = setter(text);
            SysFreeString(text);
            return set_result;
        };
        if (FAILED(result = set_string(
                       [&](BSTR text) {
                           return rule->put_Name(text);
                       },
                       name)) ||
            FAILED(result = set_string(
                       [&](BSTR text) {
                           return rule->put_Description(text);
                       },
                       L"SLink 当前运行会话创建的临时端口转发规则")) ||
            FAILED(result = set_string(
                       [&](BSTR text) {
                           return rule->put_ApplicationName(text);
                       },
                       executable))) {
            return result;
        }

        // LocalPorts is protocol-dependent. INetFwRule defaults to ANY and
        // returns E_INVALIDARG if the port list is assigned before TCP/UDP.
        if (FAILED(result = rule->put_Protocol(NET_FW_IP_PROTOCOL_TCP)) ||
            FAILED(result = set_string(
                       [&](BSTR text) {
                           return rule->put_LocalPorts(text);
                       },
                       port_text)) ||
            FAILED(result = set_string(
                       [&](BSTR text) {
                           return rule->put_RemoteAddresses(text);
                       },
                       L"LocalSubnet")) ||
            FAILED(result = set_string(
                       [&](BSTR text) {
                           return rule->put_Grouping(text);
                       },
                       kRuleGroup))) {
            return result;
        }
        if (FAILED(result = rule->put_Direction(NET_FW_RULE_DIR_IN)) ||
            FAILED(result = rule->put_Action(NET_FW_ACTION_ALLOW)) ||
            FAILED(result = rule->put_Profiles(NET_FW_PROFILE2_DOMAIN | NET_FW_PROFILE2_PRIVATE)) ||
            FAILED(result = rule->put_EdgeTraversal(VARIANT_FALSE)) ||
            FAILED(result = rule->put_Enabled(VARIANT_TRUE))) {
            return result;
        }
        return rules_->Add(rule.get());
    }

    HRESULT Remove(std::wstring_view session, std::uint16_t port) {
        return RemoveByName(RuleName(session, port));
    }

    HRESULT CleanupSession(std::wstring_view session) {
        return RemoveMatching([&](std::wstring_view name) {
            return name.starts_with(std::wstring(kRulePrefix) + std::wstring(session) + L" ");
        });
    }

    HRESULT CleanupStaleSessions(std::wstring_view current_session) {
        return RemoveMatching([&](std::wstring_view name) {
            if (!name.starts_with(kRulePrefix))
                return false;
            const std::wstring_view rest = name.substr(std::size(kRulePrefix) - 1);
            const std::size_t separator = rest.find(L' ');
            if (separator == std::wstring_view::npos)
                return false;
            const std::wstring_view session = rest.substr(0, separator);
            if (!ValidSessionId(session) || session == current_session)
                return false;
            return SessionDefinitelyInactive(session);
        });
    }

private:
    template <typename Predicate> HRESULT RemoveMatching(Predicate predicate) {
        HRESULT result = Open();
        if (FAILED(result))
            return result;
        ComPtr<IUnknown> unknown;
        result = rules_->get__NewEnum(unknown.put());
        if (FAILED(result))
            return result;
        ComPtr<IEnumVARIANT> values;
        result =
            unknown->QueryInterface(__uuidof(IEnumVARIANT), reinterpret_cast<void**>(values.put()));
        if (FAILED(result))
            return result;

        std::vector<std::wstring> names;
        VARIANT value{};
        VariantInit(&value);
        ULONG fetched{};
        result = CollectRuleNames(
            [&] {
                VariantClear(&value);
                VariantInit(&value);
                fetched = 0;
                const auto status = values->Next(1, &value, &fetched);
                if ((status == S_OK && fetched == 1) || (status == S_FALSE && fetched == 0))
                    return status;
                return FAILED(status) ? status : E_UNEXPECTED;
            },
            [&](std::wstring& name) -> HRESULT {
                if (value.vt != VT_DISPATCH || !value.pdispVal)
                    return E_UNEXPECTED;
                ComPtr<INetFwRule> rule;
                auto status = value.pdispVal->QueryInterface(__uuidof(INetFwRule),
                                                             reinterpret_cast<void**>(rule.put()));
                if (status != S_OK || !rule)
                    return FAILED(status) ? status : E_UNEXPECTED;
                BSTR raw_name{};
                status = rule->get_Name(&raw_name);
                if (status == S_OK && raw_name)
                    name.assign(raw_name, SysStringLen(raw_name));
                else if (SUCCEEDED(status))
                    status = E_UNEXPECTED;
                SysFreeString(raw_name);
                return status;
            },
            names);
        VariantClear(&value);
        if (FAILED(result))
            return result;

        HRESULT first_failure = S_OK;
        for (const auto& name : names) {
            if (!predicate(name))
                continue;
            const HRESULT removed = RemoveByName(name);
            if (FAILED(removed) && SUCCEEDED(first_failure))
                first_failure = removed;
        }
        return first_failure;
    }

    HRESULT RemoveByName(std::wstring_view name) {
        HRESULT result = Open();
        if (FAILED(result))
            return result;
        BSTR text = SysAllocStringLen(name.data(), static_cast<UINT>(name.size()));
        if (!text)
            return E_OUTOFMEMORY;
        result = rules_->Remove(text);
        SysFreeString(text);
        if (result == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))
            return S_OK;
        return result;
    }

    ComPtr<INetFwPolicy2> policy_;
    ComPtr<INetFwRules> rules_;
};

bool ReadExact(HANDLE pipe, void* data, DWORD size) {
    DWORD read{};
    return ReadFile(pipe, data, size, &read, nullptr) && read == size;
}

bool WriteExact(HANDLE pipe, const void* data, DWORD size) {
    DWORD written{};
    return WriteFile(pipe, data, size, &written, nullptr) && written == size;
}

bool SameExecutable(HANDLE process) {
    std::wstring parent_path(32768, L'\0');
    DWORD length = static_cast<DWORD>(parent_path.size());
    if (!QueryFullProcessImageNameW(process, 0, parent_path.data(), &length))
        return false;
    parent_path.resize(length);
    const std::wstring current = ExecutablePath();
    return !current.empty() && _wcsicmp(current.c_str(), parent_path.c_str()) == 0;
}

int RunElevatedHelper(DWORD parent_pid, std::wstring pipe_name, std::wstring session) {
    // 提权边界只接收固定大小、带版本和 magic 的私有协议。辅助进程同时校验父进程
    // 可执行文件、管道名前缀和会话标识，不能被当作通用管理员命令代理使用。
    if (!pipe_name.starts_with(kPipePrefix) || pipe_name.size() > 240 || !ValidSessionId(session) ||
        parent_pid == 0) {
        return ERROR_INVALID_PARAMETER;
    }
    UniqueHandle parent(
        OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, parent_pid));
    if (!parent || !SameExecutable(parent.get()))
        return ERROR_ACCESS_DENIED;

    UniqueHandle session_mutex(CreateMutexW(nullptr, FALSE, SessionMutexName(session).c_str()));
    if (!session_mutex)
        return static_cast<int>(GetLastError());

    UniqueHandle pipe;
    const ULONGLONG deadline = GetTickCount64() + kIpcTimeoutMs;
    while (GetTickCount64() < deadline) {
        if (WaitForSingleObject(parent.get(), 0) == WAIT_OBJECT_0)
            return ERROR_BROKEN_PIPE;
        HANDLE connected = CreateFileW(pipe_name.c_str(),
                                       GENERIC_READ | GENERIC_WRITE,
                                       0,
                                       nullptr,
                                       OPEN_EXISTING,
                                       SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
                                       nullptr);
        if (connected != INVALID_HANDLE_VALUE) {
            pipe.Reset(connected);
            break;
        }
        if (GetLastError() != ERROR_PIPE_BUSY)
            return GetLastError();
        WaitNamedPipeW(pipe_name.c_str(), 200);
    }
    if (!pipe)
        return ERROR_TIMEOUT;
    DWORD mode = PIPE_READMODE_MESSAGE;
    if (!SetNamedPipeHandleState(pipe.get(), &mode, nullptr, nullptr))
        return static_cast<int>(GetLastError());

    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com) && com != RPC_E_CHANGED_MODE)
        return static_cast<int>(com);
    const bool uninitialize = SUCCEEDED(com);
    FirewallPolicy policy;
    std::set<std::uint16_t> active_ports;

    for (;;) {
        HelperRequest request{};
        if (!ReadExact(pipe.get(), &request, sizeof(request)))
            break;
        HelperResponse response;
        bool exit_after_response = false;
        if (request.magic != kProtocolMagic || request.version != kProtocolVersion) {
            response.result = E_INVALIDARG;
            SetResponseMessage(response, L"辅助进程通信协议无效。");
            exit_after_response = true;
        } else {
            HRESULT result = S_OK;
            switch (request.operation) {
            case HelperOperation::initialize:
                result = policy.CleanupStaleSessions(session);
                break;
            case HelperOperation::add_rule:
                if (request.local_port == 0) {
                    result = E_INVALIDARG;
                } else {
                    result = policy.Add(session, request.local_port);
                    if (SUCCEEDED(result))
                        active_ports.insert(request.local_port);
                }
                response.enabled = SUCCEEDED(result) ? 1U : 0U;
                break;
            case HelperOperation::remove_rule:
                if (request.local_port == 0) {
                    result = E_INVALIDARG;
                } else {
                    result = policy.Remove(session, request.local_port);
                    if (SUCCEEDED(result))
                        active_ports.erase(request.local_port);
                }
                break;
            case HelperOperation::shutdown_if_clean:
                result = policy.CleanupSession(session);
                if (SUCCEEDED(result)) {
                    response.cleanup_confirmed = 1;
                    active_ports.clear();
                    exit_after_response = true;
                }
                break;
            case HelperOperation::terminate_after_cleanup:
                result = policy.CleanupSession(session);
                response.cleanup_confirmed = SUCCEEDED(result) ? 1U : 0U;
                active_ports.clear();
                exit_after_response = true;
                break;
            case HelperOperation::cleanup_recovery_rule: {
                const auto end = std::find(
                    std::begin(request.cleanup_session), std::end(request.cleanup_session), L'\0');
                const std::wstring_view old_session(request.cleanup_session,
                                                    end - std::begin(request.cleanup_session));
                if (!ValidSessionId(old_session) || request.local_port == 0 ||
                    old_session == session) {
                    result = E_INVALIDARG;
                } else if (!SessionDefinitelyInactive(old_session)) {
                    result = S_FALSE; // 未确认失活，不触碰其他实例，也不确认清理。
                } else {
                    result = policy.Remove(old_session, request.local_port);
                    response.cleanup_confirmed = result == S_OK ? 1U : 0U;
                }
                break;
            }
            default:
                result = E_INVALIDARG;
                break;
            }
            response.result = result;
            if (FAILED(result)) {
                std::wstring context;
                if (request.operation == HelperOperation::add_rule) {
                    context =
                        L"无法创建端口 " + std::to_wstring(request.local_port) + L" 的防火墙规则：";
                } else if (request.operation == HelperOperation::remove_rule) {
                    context =
                        L"无法删除端口 " + std::to_wstring(request.local_port) + L" 的防火墙规则：";
                } else {
                    context = L"无法清理 SLink 防火墙规则：";
                }
                SetResponseMessage(response, context + WindowsMessage(result));
            }
        }
        if (!WriteExact(pipe.get(), &response, sizeof(response)))
            break;
        if (exit_after_response) {
            if (uninitialize)
                CoUninitialize();
            return response.result;
        }
    }

    (void)policy.CleanupSession(session);
    if (uninitialize)
        CoUninitialize();
    return ERROR_BROKEN_PIPE;
}

bool OverlappedTransfer(HANDLE pipe, bool write, void* data, DWORD size, DWORD timeout_ms) {
    UniqueHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event)
        return false;
    OVERLAPPED overlapped{};
    overlapped.hEvent = event.get();
    DWORD transferred{};
    BOOL started = write ? WriteFile(pipe, data, size, &transferred, &overlapped)
                         : ReadFile(pipe, data, size, &transferred, &overlapped);
    if (!started && GetLastError() != ERROR_IO_PENDING)
        return false;
    if (!started) {
        if (WaitForSingleObject(event.get(), timeout_ms) != WAIT_OBJECT_0) {
            CancelIoEx(pipe, &overlapped);
            // 取消是异步的；内核不再引用栈上 OVERLAPPED/请求缓冲后才能返回。
            DWORD ignored{};
            GetOverlappedResult(pipe, &overlapped, &ignored, TRUE);
            return false;
        }
        if (!GetOverlappedResult(pipe, &overlapped, &transferred, FALSE))
            return false;
    }
    return transferred == size;
}

bool Exchange(HANDLE pipe,
              HelperOperation operation,
              std::uint16_t port,
              HelperResponse& response,
              std::wstring_view cleanup_session = {}) {
    HelperRequest request;
    request.operation = operation;
    request.local_port = port;
    if (!cleanup_session.empty()) {
        if (!ValidSessionId(cleanup_session))
            return false;
        std::copy(cleanup_session.begin(), cleanup_session.end(), request.cleanup_session);
    }
    if (!OverlappedTransfer(pipe, true, &request, sizeof(request), kIpcTimeoutMs)) {
        return false;
    }
    if (!OverlappedTransfer(pipe, false, &response, sizeof(response), kIpcTimeoutMs)) {
        return false;
    }
    return response.magic == kProtocolMagic && response.version == kProtocolVersion;
}

bool ConnectPipe(HANDLE pipe, HANDLE process, std::stop_token stop) {
    UniqueHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event)
        return false;
    OVERLAPPED overlapped{};
    overlapped.hEvent = event.get();
    BOOL connected = ConnectNamedPipe(pipe, &overlapped);
    if (!connected) {
        const DWORD error = GetLastError();
        if (error == ERROR_PIPE_CONNECTED)
            return true;
        if (error != ERROR_IO_PENDING)
            return false;
    } else {
        return true;
    }
    const ULONGLONG deadline = GetTickCount64() + kIpcTimeoutMs;
    while (!stop.stop_requested() && GetTickCount64() < deadline) {
        HANDLE handles[] = {event.get(), process};
        const DWORD wait = WaitForMultipleObjects(2, handles, FALSE, 100);
        if (wait == WAIT_OBJECT_0) {
            DWORD ignored{};
            return GetOverlappedResult(pipe, &overlapped, &ignored, FALSE) != FALSE;
        }
        if (wait == WAIT_OBJECT_0 + 1)
            break;
    }
    CancelIoEx(pipe, &overlapped);
    DWORD ignored{};
    GetOverlappedResult(pipe, &overlapped, &ignored, TRUE);
    return false;
}

} // namespace

struct FirewallHelperClient::Impl {
    struct Task {
        HelperOperation operation{HelperOperation::add_rule};
        std::uint16_t port{};
    };

    std::shared_ptr<ssh::IUiDispatcher> dispatcher;
    StateCallback state_callback;
    RuleCallback rule_callback;
    RecoveryProvider recovery_provider;
    CleanupCallback cleanup_callback;
    std::vector<FirewallRuleIdentity> recovery_rules;
    std::vector<FirewallRuleIdentity> confirmed_cleanups;
    bool cleanup_notification_pending{};
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::deque<Task> tasks;
    FirewallHelperState current_state{FirewallHelperState::off};
    std::string last_session;
    bool cleanup_succeeded{true};
    std::jthread worker;

    void ConfirmCleanup(const HelperResponse& response, std::vector<FirewallRuleIdentity> rules) {
        if (response.result != S_OK || response.cleanup_confirmed != 1 || rules.empty())
            return;
        std::erase_if(rules, [](const auto& rule) {
            return rule.local_port == 0 ||
                   !ValidSessionId(std::wstring(rule.session_id.begin(), rule.session_id.end()));
        });
        if (rules.empty())
            return;
        bool notify{};
        {
            std::scoped_lock lock(mutex);
            confirmed_cleanups.insert(confirmed_cleanups.end(), rules.begin(), rules.end());
            notify = !std::exchange(cleanup_notification_pending, true);
        }
        if (notify && dispatcher && cleanup_callback)
            dispatcher->post(cleanup_callback);
    }

    void PostState(FirewallHelperState state, std::wstring message = {}) {
        std::string session;
        {
            std::scoped_lock lock(mutex);
            current_state = state;
            session = last_session;
        }
        if (dispatcher && state_callback) {
            dispatcher->post([callback = state_callback,
                              state,
                              message = std::move(message),
                              session = std::move(session)]() mutable {
                callback({state, std::move(message), std::move(session)});
            });
        }
    }

    void PostRule(std::uint16_t port, bool enabled, bool succeeded, std::wstring message = {}) {
        std::string session;
        {
            std::scoped_lock lock(mutex);
            session = last_session;
        }
        const std::string rule_name = "SLink Port Forward " + session + " " + std::to_string(port);
        if (dispatcher && rule_callback) {
            dispatcher->post([callback = rule_callback,
                              port,
                              enabled,
                              succeeded,
                              message = std::move(message),
                              session,
                              rule_name]() mutable {
                callback({port,
                          enabled,
                          succeeded,
                          std::move(message),
                          std::move(session),
                          std::move(rule_name)});
            });
        }
    }

    void Run(std::stop_token stop) {
        const std::wstring session = MakeSessionId();
        if (session.empty()) {
            PostState(FirewallHelperState::error, L"无法创建防火墙管理会话。");
            return;
        }
        {
            std::scoped_lock lock(mutex);
            last_session.clear();
            last_session.reserve(session.size());
            for (const wchar_t character : session) {
                last_session.push_back(static_cast<char>(character));
            }
            cleanup_succeeded = false;
        }
        const std::wstring pipe_name =
            std::wstring(kPipePrefix) + std::to_wstring(GetCurrentProcessId()) + L"." + session;
        UniqueHandle pipe(CreateNamedPipeW(
            pipe_name.c_str(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            1,
            sizeof(HelperResponse),
            sizeof(HelperRequest),
            0,
            nullptr));
        if (!pipe || pipe.get() == INVALID_HANDLE_VALUE) {
            PostState(FirewallHelperState::error, L"无法创建防火墙辅助进程通信通道。");
            return;
        }

        const std::wstring executable = ExecutablePath();
        const std::wstring parameters = L"--firewall-helper " +
                                        std::to_wstring(GetCurrentProcessId()) + L" \"" +
                                        pipe_name + L"\" " + session;
        SHELLEXECUTEINFOW execute{sizeof(execute)};
        execute.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
        execute.lpVerb = L"runas";
        execute.lpFile = executable.c_str();
        execute.lpParameters = parameters.c_str();
        execute.nShow = SW_HIDE;
        if (!ShellExecuteExW(&execute)) {
            const DWORD error = GetLastError();
            PostState(error == ERROR_CANCELLED ? FirewallHelperState::off
                                               : FirewallHelperState::error,
                      error == ERROR_CANCELLED ? L"已取消管理员授权。"
                                               : WindowsMessage(HRESULT_FROM_WIN32(error)));
            return;
        }
        UniqueHandle process(execute.hProcess);
        if (!ConnectPipe(pipe.get(), process.get(), stop)) {
            PostState(FirewallHelperState::error, L"管理员辅助进程未能建立连接。");
            return;
        }

        HelperResponse initialized;
        if (!Exchange(pipe.get(), HelperOperation::initialize, 0, initialized) ||
            FAILED(initialized.result)) {
            PostState(FirewallHelperState::error,
                      initialized.message[0] ? initialized.message
                                             : L"无法初始化 Windows 防火墙接口。");
            return;
        }
        PostState(FirewallHelperState::running);

        // 初始化的“运行中”只代表辅助进程可用。恢复记录逐项请求确认；已不存在也可
        // 确认，而其他活跃实例或失败项必须保留。每条请求固定大小，不发送任意规则名。
        for (const auto& rule : recovery_rules) {
            if (stop.stop_requested())
                break;
            {
                std::scoped_lock lock(mutex);
                if (std::ranges::any_of(tasks, [](const auto& task) {
                        return task.operation == HelperOperation::shutdown_if_clean ||
                               task.operation == HelperOperation::terminate_after_cleanup;
                    }))
                    break;
            }
            const std::wstring old_session(rule.session_id.begin(), rule.session_id.end());
            if (!ValidSessionId(old_session) || rule.local_port == 0)
                continue;
            HelperResponse response;
            if (!Exchange(pipe.get(),
                          HelperOperation::cleanup_recovery_rule,
                          rule.local_port,
                          response,
                          old_session)) {
                PostState(FirewallHelperState::error,
                          L"管理员辅助进程通信已中断；未确认的恢复记录已保留。");
                return;
            }
            ConfirmCleanup(response, {rule});
            if (FAILED(response.result))
                PostState(FirewallHelperState::running, response.message);
        }

        bool done = false;
        std::set<std::uint16_t> session_ports;
        while (!done) {
            Task task;
            {
                std::unique_lock lock(mutex);
                changed.wait(lock, [&] {
                    return !tasks.empty() || stop.stop_requested();
                });
                if (tasks.empty() && stop.stop_requested()) {
                    task.operation = HelperOperation::terminate_after_cleanup;
                } else {
                    task = tasks.front();
                    tasks.pop_front();
                }
            }
            HelperResponse response;
            if (!Exchange(pipe.get(), task.operation, task.port, response)) {
                PostState(FirewallHelperState::error, L"管理员辅助进程通信已中断。");
                break;
            }
            const bool succeeded = SUCCEEDED(response.result);
            const std::wstring message = response.message;
            switch (task.operation) {
            case HelperOperation::add_rule:
                if (succeeded)
                    session_ports.insert(task.port);
                PostRule(task.port, succeeded, succeeded, message);
                break;
            case HelperOperation::remove_rule:
                if (succeeded)
                    session_ports.erase(task.port);
                PostRule(task.port, false, succeeded, message);
                break;
            case HelperOperation::shutdown_if_clean:
                if (succeeded) {
                    {
                        std::scoped_lock lock(mutex);
                        cleanup_succeeded = true;
                    }
                    PostState(FirewallHelperState::off);
                    done = true;
                } else {
                    PostState(FirewallHelperState::running, message);
                }
                break;
            case HelperOperation::terminate_after_cleanup: {
                std::scoped_lock lock(mutex);
                cleanup_succeeded = succeeded;
            }
                PostState(FirewallHelperState::off, message);
                done = true;
                break;
            default:
                break;
            }
            if (task.operation == HelperOperation::shutdown_if_clean ||
                task.operation == HelperOperation::terminate_after_cleanup) {
                std::vector<FirewallRuleIdentity> cleaned;
                std::string id;
                id.reserve(session.size());
                for (const auto ch : session)
                    id.push_back(static_cast<char>(ch)); // 会话名已限定为 ASCII 十六进制。
                for (auto port : session_ports)
                    cleaned.push_back({id, port});
                ConfirmCleanup(response, std::move(cleaned));
            }
        }
        DisconnectNamedPipe(pipe.get());
        if (process)
            WaitForSingleObject(process.get(), 3000);
    }

    void Queue(Task task) {
        {
            std::scoped_lock lock(mutex);
            tasks.push_back(task);
        }
        changed.notify_one();
    }

    bool Shutdown() noexcept {
        bool joinable{};
        {
            std::scoped_lock lock(mutex);
            joinable = worker.joinable();
            if (joinable && (current_state == FirewallHelperState::running ||
                             current_state == FirewallHelperState::stopping)) {
                tasks.push_front({HelperOperation::terminate_after_cleanup, 0});
            }
        }
        if (!joinable)
            return cleanup_succeeded;
        changed.notify_one();
        worker.request_stop();
        worker.join();
        std::scoped_lock lock(mutex);
        tasks.clear();
        current_state = FirewallHelperState::off;
        return cleanup_succeeded;
    }
};

FirewallHelperClient::FirewallHelperClient(std::shared_ptr<ssh::IUiDispatcher> dispatcher,
                                           StateCallback state_callback,
                                           RuleCallback rule_callback,
                                           RecoveryProvider recovery_provider,
                                           CleanupCallback cleanup_callback)
    : impl_(std::make_unique<Impl>()) {
    impl_->dispatcher = std::move(dispatcher);
    impl_->state_callback = std::move(state_callback);
    impl_->rule_callback = std::move(rule_callback);
    impl_->recovery_provider = std::move(recovery_provider);
    impl_->cleanup_callback = std::move(cleanup_callback);
}

FirewallHelperClient::~FirewallHelperClient() {
    (void)Shutdown();
}

void FirewallHelperClient::SetEnabled(bool enabled) {
    const FirewallHelperState current = state();
    if (enabled) {
        if (current != FirewallHelperState::off && current != FirewallHelperState::error) {
            return;
        }
        if (impl_->worker.joinable())
            impl_->worker.join();
        // 仓储只在发起操作的 UI 线程读取，工作线程使用非敏感的身份快照。
        auto recovery = impl_->recovery_provider ? impl_->recovery_provider()
                                                 : std::vector<FirewallRuleIdentity>{};
        {
            std::scoped_lock lock(impl_->mutex);
            impl_->tasks.clear();
            impl_->recovery_rules = std::move(recovery);
            impl_->current_state = FirewallHelperState::starting;
        }
        impl_->PostState(FirewallHelperState::starting);
        impl_->worker = std::jthread([state = impl_.get()](std::stop_token stop) {
            state->Run(stop);
        });
        return;
    }
    if (current != FirewallHelperState::running)
        return;
    impl_->PostState(FirewallHelperState::stopping);
    impl_->Queue({HelperOperation::shutdown_if_clean, 0});
}

void FirewallHelperClient::SetRule(std::uint16_t local_port, bool enabled) {
    if (local_port == 0 || state() != FirewallHelperState::running)
        return;
    impl_->Queue({enabled ? HelperOperation::add_rule : HelperOperation::remove_rule, local_port});
}

FirewallHelperState FirewallHelperClient::state() const noexcept {
    std::scoped_lock lock(impl_->mutex);
    return impl_->current_state;
}

bool FirewallHelperClient::Shutdown() noexcept {
    return !impl_ || impl_->Shutdown();
}

std::vector<FirewallRuleIdentity> FirewallHelperClient::TakeConfirmedCleanups() {
    std::scoped_lock lock(impl_->mutex);
    impl_->cleanup_notification_pending = false;
    return std::exchange(impl_->confirmed_cleanups, {});
}

std::optional<int> RunFirewallHelperIfRequested() {
    int count{};
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!arguments)
        return std::nullopt;
    const auto release = [&] {
        LocalFree(arguments);
    };
    if (count < 2 || std::wstring_view(arguments[1]) != L"--firewall-helper") {
        release();
        return std::nullopt;
    }
    if (count != 5) {
        release();
        return ERROR_INVALID_PARAMETER;
    }
    wchar_t* end{};
    const unsigned long parsed = wcstoul(arguments[2], &end, 10);
    if (!end || *end != L'\0' || parsed == 0 || parsed > MAXDWORD) {
        release();
        return ERROR_INVALID_PARAMETER;
    }
    std::wstring pipe = arguments[3];
    std::wstring session = arguments[4];
    release();
    return RunElevatedHelper(static_cast<DWORD>(parsed), std::move(pipe), std::move(session));
}

} // namespace SLink::app
