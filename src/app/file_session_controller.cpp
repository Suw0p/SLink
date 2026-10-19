// 文件浏览/编辑 SFTP 会话实现：按服务器合并连接请求、维护消费者计数，并保证所有
// libssh2 对象始终在所属 NetworkReactor 上创建、调用和销毁。
#include "app/file_session_controller.hpp"

#include "app/remote_initial_directory.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "ssh/session.hpp"
#include "transfer/sftp_endpoint.hpp"

#include <Windows.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <winsock2.h>
#include <ws2tcpip.h>

namespace SLink::app {
namespace {

using namespace std::chrono_literals;
constexpr auto kKeepaliveInterval = 60s;

Status CancelledStatus() {
    return {ErrorCode::cancelled, L"SFTP file session was cancelled."};
}

std::wstring WidenError(std::string_view text) {
    if (text.empty()) {
        return L"Unknown file-session error.";
    }
    if (text.size() <= static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        const auto size = static_cast<int>(text.size());
        const int required =
            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), size, nullptr, 0);
        if (required > 0) {
            std::wstring result(static_cast<std::size_t>(required), L'\0');
            if (MultiByteToWideChar(
                    CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), size, result.data(), required) >
                0) {
                return result;
            }
        }
    }
    return L"File-session operation failed.";
}

class SensitiveString final {
public:
    explicit SensitiveString(const storage::SecureBuffer& bytes) {
        if (!bytes.empty()) {
            value_.assign(reinterpret_cast<const char*>(bytes.bytes().data()),
                          bytes.bytes().size());
        }
    }

    ~SensitiveString() {
        if (!value_.empty()) {
            SecureZeroMemory(value_.data(), value_.size());
        }
    }

    SensitiveString(const SensitiveString&) = delete;
    SensitiveString& operator=(const SensitiveString&) = delete;

    [[nodiscard]] const std::string& value() const noexcept {
        return value_;
    }

private:
    std::string value_;
};

void SecureWipeString(std::string& value) noexcept {
    if (!value.empty()) {
        SecureZeroMemory(value.data(), value.size());
        value.clear();
    }
}

void ClearAuthentication(FileSessionAuthentication& authentication) noexcept {
    std::visit(
        [](auto& value) noexcept {
            using Authentication = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Authentication, FilePasswordAuthentication>) {
                value.password.clear();
            } else if constexpr (std::is_same_v<Authentication, FilePrivateKeyAuthentication>) {
                value.public_key.clear();
                value.private_key.clear();
                value.passphrase.clear();
            } else if constexpr (std::is_same_v<Authentication,
                                                FileKeyboardInteractiveAuthentication>) {
                value.respond = {};
            }
        },
        authentication);
}

class AuthenticationGuard final {
public:
    explicit AuthenticationGuard(std::shared_ptr<FileSessionAuthentication> authentication)
        : authentication_(std::move(authentication)) {}

    ~AuthenticationGuard() {
        if (authentication_) {
            ClearAuthentication(*authentication_);
        }
    }

private:
    std::shared_ptr<FileSessionAuthentication> authentication_;
};

bool AuthenticationMatches(storage::AuthenticationKind kind,
                           const FileSessionAuthentication& authentication) noexcept {
    switch (kind) {
    case storage::AuthenticationKind::Password:
        return std::holds_alternative<FilePasswordAuthentication>(authentication);
    case storage::AuthenticationKind::PrivateKey:
        return std::holds_alternative<FilePrivateKeyAuthentication>(authentication);
    case storage::AuthenticationKind::OpenSshAgent:
        return std::holds_alternative<FileAgentAuthentication>(authentication);
    case storage::AuthenticationKind::KeyboardInteractive:
        return std::holds_alternative<FileKeyboardInteractiveAuthentication>(authentication);
    }
    return false;
}

Status Authenticate(ssh::SshSession& session,
                    const storage::ServerProfile& profile,
                    FileSessionAuthentication& authentication) {
    return std::visit(
        [&session, &profile](auto& value) -> Status {
            using Authentication = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Authentication, FilePasswordAuthentication>) {
                SensitiveString password(value.password);
                ssh::PasswordAuth request{profile.username, password.value()};
                const auto status = session.authenticate(request);
                SecureWipeString(request.password);
                return status;
            } else if constexpr (std::is_same_v<Authentication, FilePrivateKeyAuthentication>) {
                SensitiveString public_key(value.public_key);
                SensitiveString private_key(value.private_key);
                SensitiveString passphrase(value.passphrase);
                ssh::PrivateKeyAuth request{
                    profile.username, public_key.value(), private_key.value(), passphrase.value()};
                const auto status = session.authenticate(request);
                SecureWipeString(request.public_key);
                SecureWipeString(request.private_key);
                SecureWipeString(request.passphrase);
                return status;
            } else if constexpr (std::is_same_v<Authentication, FileAgentAuthentication>) {
                return session.authenticate(ssh::AgentAuth{profile.username});
            } else {
                if (!value.respond) {
                    return {ErrorCode::invalid_argument,
                            L"Keyboard-interactive authentication requires "
                            L"pre-collected responses."};
                }
                return session.authenticate(
                    ssh::KeyboardInteractiveAuth{profile.username, value.respond});
            }
        },
        authentication);
}

Result<std::shared_ptr<ssh::SshSession>>
ConnectSession(const ssh::SessionConfig& config,
               ssh::NetworkReactor& reactor,
               const ssh::CancellationToken& cancellation) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* addresses = nullptr;
    const std::string service = std::to_string(config.port);
    const int lookup = getaddrinfo(config.host.c_str(), service.c_str(), &hints, &addresses);
    if (lookup != 0) {
        return Status{ErrorCode::network_error, L"Cannot resolve the SSH server address."};
    }
    struct AddressGuard final {
        addrinfo* addresses{};
        ~AddressGuard() {
            if (addresses != nullptr) {
                freeaddrinfo(addresses);
            }
        }
    } address_guard{addresses};

    const auto deadline = std::chrono::steady_clock::now() + config.connect_timeout;
    for (auto* address = addresses; address != nullptr; address = address->ai_next) {
        if (cancellation.is_cancelled()) {
            return CancelledStatus();
        }
        if (address->ai_addrlen > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
            continue;
        }

        const SOCKET candidate =
            socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (candidate == INVALID_SOCKET) {
            continue;
        }
        u_long nonblocking = 1;
        if (ioctlsocket(candidate, FIONBIO, &nonblocking) != 0) {
            closesocket(candidate);
            continue;
        }

        const int connected =
            ::connect(candidate, address->ai_addr, static_cast<int>(address->ai_addrlen));
        bool ready = connected == 0;
        if (!ready && WSAGetLastError() == WSAEWOULDBLOCK) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            if (remaining > 0ms) {
                ready = reactor.wait_socket(static_cast<std::uintptr_t>(candidate),
                                            ssh::WaitDirection::write,
                                            remaining,
                                            cancellation);
            }
        }
        if (ready) {
            int socket_error = 0;
            int length = sizeof(socket_error);
            if (getsockopt(candidate,
                           SOL_SOCKET,
                           SO_ERROR,
                           reinterpret_cast<char*>(&socket_error),
                           &length) == 0 &&
                socket_error == 0) {
                return ssh::SshSession::adopt_connected_socket(
                    static_cast<std::uintptr_t>(candidate), true, config);
            }
        }
        closesocket(candidate);
        if (std::chrono::steady_clock::now() >= deadline) {
            break;
        }
    }

    if (cancellation.is_cancelled()) {
        return CancelledStatus();
    }
    return Status{ErrorCode::network_error, L"Cannot connect to the SSH server before timeout."};
}

class SessionGuard final {
public:
    explicit SessionGuard(std::shared_ptr<ssh::SshSession> session)
        : session_(std::move(session)) {}

    ~SessionGuard() {
        if (session_) {
            (void)session_->disconnect();
        }
    }

    void release() noexcept {
        session_.reset();
    }

private:
    std::shared_ptr<ssh::SshSession> session_;
};

Status ExceptionStatus() noexcept {
    try {
        throw;
    } catch (const ssh::ReactorEndpointError& error) {
        return error.status();
    } catch (const std::exception& error) {
        return {ErrorCode::io_error, WidenError(error.what() != nullptr ? error.what() : "")};
    } catch (...) {
        return {ErrorCode::io_error, L"Unhandled file-session operation error."};
    }
}

} // namespace

// 控制器使用一个 Reactor 串行承载浏览/编辑会话。SessionRecord 的 mutex 只保护跨线程
// 快照和生命周期标记，真正的 session/endpoint 调用仍只能发生在 Reactor 线程。
struct FileSessionController::Impl {
    enum class Phase {
        connecting,
        ready,
        closing,
    };

    struct SessionRecord final {
        explicit SessionRecord(storage::ServerProfile value) : profile(std::move(value)) {}

        mutable std::mutex mutex;
        storage::ServerProfile profile;
        Phase phase{Phase::connecting};
        std::size_t consumers{1};
        bool close_requested{};
        bool keepalive_pending{};
        std::vector<OpenCompletion> completions;
        ssh::ReactorTaskHandle connect_task;

        // session 只能由 NetworkReactor 解引用；赋值和 shared_ptr 快照受 mutex 保护，
        // EndpointFor 与关闭流程因此可以安全延长对象寿命，但不能直接调用 session。
        std::shared_ptr<ssh::SshSession> session;
        std::shared_ptr<ssh::ReactorEndpoint> endpoint;
        std::optional<ssh::HostKeyInfo> host_key;
        std::string initial_directory;
    };

    struct ControlState final {
        mutable std::mutex mutex;
        bool shutting_down{};
        ssh::NetworkReactor* reactor{};
        std::shared_ptr<ssh::IUiDispatcher> dispatcher;
        std::unordered_map<std::string, std::shared_ptr<SessionRecord>> sessions;
    };

    struct ListPayload final {
        std::mutex mutex;
        std::vector<transfer::DirectoryEntry> entries;
    };

    struct Barrier final {
        std::mutex mutex;
        std::condition_variable changed;
        bool reached{};

        void signal() noexcept {
            try {
                {
                    std::scoped_lock lock(mutex);
                    reached = true;
                }
                changed.notify_all();
            } catch (...) {}
        }

        void wait() {
            std::unique_lock lock(mutex);
            changed.wait(lock, [this] {
                return reached;
            });
        }
    };

    std::shared_ptr<ssh::NetworkReactor> reactor;
    std::shared_ptr<ControlState> state;
    std::filesystem::path known_hosts;
    std::mutex keepalive_mutex;
    std::condition_variable keepalive_changed;
    bool keepalive_stopping{};
    std::thread keepalive_worker;

    Impl(std::shared_ptr<ssh::IUiDispatcher> dispatcher, std::filesystem::path known_hosts_path)
        : known_hosts(std::move(known_hosts_path)) {
        if (!dispatcher) {
            throw std::invalid_argument("FileSessionController requires a UI dispatcher");
        }
        if (known_hosts.empty() || !known_hosts.is_absolute()) {
            throw std::invalid_argument("known_hosts must be an absolute application-data path");
        }
        reactor = std::make_shared<ssh::NetworkReactor>(dispatcher);
        state = std::make_shared<ControlState>();
        state->reactor = reactor.get();
        state->dispatcher = std::move(dispatcher);
        keepalive_worker = std::thread([this] {
            KeepaliveLoop();
        });
    }

    ~Impl() {
        Shutdown();
    }

    static void PostOpen(const std::shared_ptr<ControlState>& state,
                         OpenCompletion completion,
                         FileSessionOpenResult result) {
        if (!completion) {
            return;
        }
        const std::weak_ptr<ControlState> weak = state;
        try {
            state->dispatcher->post(
                [weak, completion = std::move(completion), result = std::move(result)]() mutable {
                    const auto locked = weak.lock();
                    if (!locked) {
                        return;
                    }
                    {
                        std::scoped_lock lock(locked->mutex);
                        if (locked->shutting_down) {
                            return;
                        }
                    }
                    try {
                        completion(std::move(result));
                    } catch (...) {}
                });
        } catch (...) {}
    }

    static void PostList(const std::shared_ptr<ControlState>& state,
                         ListCompletion completion,
                         FileDirectoryListResult result) {
        if (!completion) {
            return;
        }
        const std::weak_ptr<ControlState> weak = state;
        try {
            state->dispatcher->post(
                [weak, completion = std::move(completion), result = std::move(result)]() mutable {
                    const auto locked = weak.lock();
                    if (!locked) {
                        return;
                    }
                    {
                        std::scoped_lock lock(locked->mutex);
                        if (locked->shutting_down) {
                            return;
                        }
                    }
                    try {
                        completion(std::move(result));
                    } catch (...) {}
                });
        } catch (...) {}
    }

    static void ReleaseRecordOnReactor(const std::shared_ptr<SessionRecord>& record,
                                       ssh::NetworkReactor& owner,
                                       const ssh::CancellationToken& cancellation) noexcept {
        std::shared_ptr<ssh::ReactorEndpoint> endpoint;
        std::shared_ptr<ssh::SshSession> session;
        {
            std::scoped_lock lock(record->mutex);
            record->phase = Phase::closing;
            endpoint = std::move(record->endpoint);
            session = std::move(record->session);
            record->host_key.reset();
        }

        // If this is the last proxy reference, ReactorEndpoint and the wrapped
        // SFTP adapter are destroyed here on their owning thread.
        endpoint.reset();
        if (session) {
            (void)owner.bind_session_waiter(session, cancellation);
            (void)session->disconnect();
            session.reset();
        }
    }

    static void QueueRelease(const std::shared_ptr<ssh::NetworkReactor>& reactor,
                             const std::shared_ptr<SessionRecord>& record) noexcept {
        if (!reactor || !record) {
            return;
        }
        try {
            const auto ignored =
                reactor->submit([reactor, record](const ssh::CancellationToken& cancellation) {
                    ReleaseRecordOnReactor(record, *reactor, cancellation);
                    return Status::ok();
                });
            (void)ignored;
        } catch (...) {
            // Shutdown owns a final reactor barrier and will retry cleanup for
            // every record still present in its snapshot.
        }
    }

    struct DedicatedOwner final {
        std::shared_ptr<ssh::NetworkReactor> reactor;
        std::shared_ptr<SessionRecord> record;

        ~DedicatedOwner() {
            if (!reactor || !record)
                return;
            auto barrier = std::make_shared<Barrier>();
            try {
                const auto task =
                    reactor->submit([local_reactor = reactor, local_record = record, barrier](
                                        const ssh::CancellationToken& cancellation) {
                        ReleaseRecordOnReactor(local_record, *local_reactor, cancellation);
                        barrier->signal();
                        return Status::ok();
                    });
                (void)task;
                barrier->wait();
            } catch (...) {
                // NetworkReactor's shutdown path owns any still-live task
                // objects. No exception may escape a shared_ptr deleter.
            }
            reactor->shutdown();
            record.reset();
            reactor.reset();
        }
    };

    struct DedicatedEndpoint final : transfer::IFileEndpoint, IDedicatedFileEndpoint {
        explicit DedicatedEndpoint(std::shared_ptr<DedicatedOwner> value)
            : owner(std::move(value)) {}

        std::shared_ptr<ssh::ReactorEndpoint> endpoint() const {
            if (!owner || !owner->record)
                return {};
            std::scoped_lock lock(owner->record->mutex);
            return owner->record->endpoint;
        }

        bool accepts(const transfer::FileUri& uri) const noexcept override {
            const auto value = endpoint();
            return value && value->accepts(uri);
        }
        bool available() const noexcept override {
            const auto value = endpoint();
            return value && value->available();
        }
        std::vector<transfer::DirectoryEntry> list(const transfer::FileUri& uri) override {
            return endpoint_or_throw()->list(uri);
        }
        transfer::FileStat stat(const transfer::FileUri& uri) override {
            return endpoint_or_throw()->stat(uri);
        }
        bool exists(const transfer::FileUri& uri) override {
            return endpoint_or_throw()->exists(uri);
        }
        std::unique_ptr<transfer::IReadStream> open_read(const transfer::FileUri& uri,
                                                         const std::uint64_t offset) override {
            return endpoint_or_throw()->open_read(uri, offset);
        }
        std::unique_ptr<transfer::IWriteStream> open_write(const transfer::FileUri& uri,
                                                           const std::uint64_t offset,
                                                           const bool truncate) override {
            return endpoint_or_throw()->open_write(uri, offset, truncate);
        }
        void rename(const transfer::FileUri& from,
                    const transfer::FileUri& to,
                    const bool replace) override {
            endpoint_or_throw()->rename(from, to, replace);
        }
        void remove(const transfer::FileUri& uri, const bool recursive) override {
            endpoint_or_throw()->remove(uri, recursive);
        }
        void mkdir(const transfer::FileUri& uri, const bool recursive) override {
            endpoint_or_throw()->mkdir(uri, recursive);
        }
        void set_permissions(const transfer::FileUri& uri,
                             const std::uint32_t permissions) override {
            endpoint_or_throw()->set_permissions(uri, permissions);
        }

        Status SendKeepalive() override {
            if (!owner || !owner->record || !owner->reactor) {
                return {ErrorCode::not_found, L"传输连接已经关闭。"};
            }
            std::shared_ptr<ssh::SshSession> session;
            {
                std::scoped_lock lock(owner->record->mutex);
                session = owner->record->session;
            }
            if (!session) {
                return {ErrorCode::not_found, L"传输 SSH 会话已经关闭。"};
            }
            struct KeepaliveState final {
                Barrier barrier;
                std::mutex mutex;
                Status status{ErrorCode::io_error, L"SFTP keepalive 未完成。"};
            };
            auto keepalive_state = std::make_shared<KeepaliveState>();
            try {
                const auto task =
                    owner->reactor->submit([reactor = owner->reactor, session, keepalive_state](
                                               const ssh::CancellationToken& cancellation) {
                        Status status = reactor->bind_session_waiter(session, cancellation);
                        if (status)
                            status = session->send_keepalive();
                        {
                            std::scoped_lock lock(keepalive_state->mutex);
                            keepalive_state->status = status;
                        }
                        keepalive_state->barrier.signal();
                        return status;
                    });
                (void)task;
                keepalive_state->barrier.wait();
                std::scoped_lock lock(keepalive_state->mutex);
                return keepalive_state->status;
            } catch (...) {
                return {ErrorCode::io_error, L"无法调度 SFTP keepalive。"};
            }
        }

        std::shared_ptr<ssh::ReactorEndpoint> endpoint_or_throw() const {
            auto value = endpoint();
            if (!value) {
                throw std::runtime_error("Dedicated SFTP endpoint is no longer available");
            }
            return value;
        }

        std::shared_ptr<DedicatedOwner> owner;
    };

    Result<std::shared_ptr<transfer::IFileEndpoint>>
    OpenDedicated(storage::ServerProfile profile,
                  FileSessionAuthentication authentication,
                  const FileHostKeyPolicy host_key_policy) {
        std::shared_ptr<ssh::IUiDispatcher> dispatcher;
        {
            std::scoped_lock lock(state->mutex);
            if (state->shutting_down) {
                ClearAuthentication(authentication);
                return Status{ErrorCode::cancelled, L"SFTP 文件会话控制器正在关闭。"};
            }
            dispatcher = state->dispatcher;
        }
        if (profile.id.empty() || profile.host.empty() || profile.username.empty() ||
            profile.port == 0) {
            ClearAuthentication(authentication);
            return Status{ErrorCode::invalid_argument,
                          L"传输连接缺少服务器 ID、主机、端口或用户名。"};
        }

        auto dedicated_reactor = std::make_shared<ssh::NetworkReactor>(dispatcher);
        auto record = std::make_shared<SessionRecord>(std::move(profile));
        auto authentication_state =
            std::make_shared<FileSessionAuthentication>(std::move(authentication));

        struct OpenState final {
            std::mutex mutex;
            std::condition_variable changed;
            bool complete{};
            Status status{ErrorCode::io_error, L"独立 SFTP 连接未完成。"};
        };
        auto opened = std::make_shared<OpenState>();
        try {
            const auto task =
                dedicated_reactor->submit([record,
                                           dedicated_reactor,
                                           authentication_state,
                                           known_hosts = known_hosts,
                                           host_key_policy,
                                           opened](const ssh::CancellationToken& cancellation) {
                    Status status = RunOpen(record,
                                            dedicated_reactor,
                                            cancellation,
                                            known_hosts,
                                            host_key_policy,
                                            authentication_state);
                    {
                        std::scoped_lock lock(opened->mutex);
                        opened->status = status;
                        opened->complete = true;
                    }
                    opened->changed.notify_all();
                    return status;
                });
            (void)task;
        } catch (const std::exception&) {
            dedicated_reactor->shutdown();
            return Status{ErrorCode::io_error, L"无法调度独立 SFTP 连接任务。"};
        }

        Status status;
        {
            std::unique_lock lock(opened->mutex);
            opened->changed.wait(lock, [&opened] {
                return opened->complete;
            });
            status = std::move(opened->status);
        }
        if (!status) {
            auto cleanup = std::make_shared<DedicatedOwner>();
            cleanup->reactor = dedicated_reactor;
            cleanup->record = record;
            cleanup.reset();
            return status;
        }

        std::shared_ptr<ssh::ReactorEndpoint> endpoint;
        {
            std::scoped_lock lock(record->mutex);
            endpoint = record->endpoint;
        }
        if (!endpoint) {
            auto cleanup = std::make_shared<DedicatedOwner>();
            cleanup->reactor = dedicated_reactor;
            cleanup->record = record;
            cleanup.reset();
            return Status{ErrorCode::io_error, L"独立 SFTP 连接没有创建文件端点。"};
        }

        auto owner = std::make_shared<DedicatedOwner>();
        owner->reactor = std::move(dedicated_reactor);
        owner->record = std::move(record);
        return std::shared_ptr<transfer::IFileEndpoint>(
            std::make_shared<DedicatedEndpoint>(std::move(owner)));
    }

    static void FinishKeepalive(const std::weak_ptr<ControlState>& weak_state,
                                const std::shared_ptr<SessionRecord>& record,
                                Status status,
                                const std::shared_ptr<ssh::NetworkReactor>& reactor) noexcept {
        const auto state = weak_state.lock();
        if (!state || !record) {
            return;
        }
        bool release{};
        {
            std::scoped_lock state_lock(state->mutex);
            std::scoped_lock record_lock(record->mutex);
            record->keepalive_pending = false;
            if (state->shutting_down || status || status.code() == ErrorCode::cancelled) {
                return;
            }
            const auto found = state->sessions.find(record->profile.id);
            if (found == state->sessions.end() || found->second != record ||
                record->close_requested || record->phase != Phase::ready) {
                return;
            }
            record->close_requested = true;
            record->phase = Phase::closing;
            record->consumers = 0;
            state->sessions.erase(found);
            release = true;
        }
        if (release) {
            QueueRelease(reactor, record);
        }
    }

    void QueueKeepaliveSweep() noexcept {
        const auto local_state = state;
        const auto local_reactor = reactor;
        if (!local_state || !local_reactor) {
            return;
        }
        std::vector<std::shared_ptr<SessionRecord>> records;
        {
            std::scoped_lock state_lock(local_state->mutex);
            if (local_state->shutting_down) {
                return;
            }
            records.reserve(local_state->sessions.size());
            for (const auto& [id, record] : local_state->sessions) {
                (void)id;
                records.push_back(record);
            }
        }

        const std::weak_ptr<ControlState> weak_state = local_state;
        for (const auto& record : records) {
            bool should_queue{};
            {
                std::scoped_lock record_lock(record->mutex);
                if (record->phase == Phase::ready && !record->close_requested && record->session &&
                    !record->keepalive_pending) {
                    record->keepalive_pending = true;
                    should_queue = true;
                }
            }
            if (!should_queue) {
                continue;
            }
            try {
                const auto ignored = local_reactor->submit(
                    [record, local_reactor](const ssh::CancellationToken& cancellation) {
                        std::shared_ptr<ssh::SshSession> session;
                        {
                            std::scoped_lock record_lock(record->mutex);
                            if (record->close_requested || record->phase != Phase::ready ||
                                !record->session) {
                                return CancelledStatus();
                            }
                            session = record->session;
                        }
                        if (cancellation.is_cancelled()) {
                            return CancelledStatus();
                        }
                        if (auto status = local_reactor->bind_session_waiter(session, cancellation);
                            !status) {
                            return status;
                        }
                        return session->send_keepalive();
                    },
                    [weak_state, record, local_reactor](ssh::ReactorTaskResult result) {
                        FinishKeepalive(
                            weak_state, record, std::move(result.status), local_reactor);
                    });
                (void)ignored;
            } catch (...) {
                std::scoped_lock record_lock(record->mutex);
                record->keepalive_pending = false;
            }
        }
    }

    void KeepaliveLoop() noexcept {
        std::unique_lock lock(keepalive_mutex);
        while (!keepalive_changed.wait_for(lock, kKeepaliveInterval, [this] {
            return keepalive_stopping;
        })) {
            lock.unlock();
            QueueKeepaliveSweep();
            lock.lock();
        }
    }

    void StopKeepaliveWorker() noexcept {
        {
            std::scoped_lock lock(keepalive_mutex);
            keepalive_stopping = true;
        }
        keepalive_changed.notify_all();
        if (keepalive_worker.joinable() &&
            keepalive_worker.get_id() != std::this_thread::get_id()) {
            keepalive_worker.join();
        }
    }

    static void FinishOpen(std::weak_ptr<ControlState> weak_state,
                           const std::shared_ptr<SessionRecord>& record,
                           Status status,
                           const std::shared_ptr<ssh::NetworkReactor>& reactor) {
        const auto state = weak_state.lock();
        if (!state) {
            QueueRelease(reactor, record);
            return;
        }

        std::vector<OpenCompletion> completions;
        std::shared_ptr<ssh::ReactorEndpoint> endpoint;
        std::optional<ssh::HostKeyInfo> host_key;
        std::string initial_directory;
        bool release{};
        {
            std::scoped_lock state_lock(state->mutex);
            if (state->shutting_down) {
                return;
            }

            const auto found = state->sessions.find(record->profile.id);
            std::scoped_lock record_lock(record->mutex);
            host_key = record->host_key;
            initial_directory = record->initial_directory;
            completions.swap(record->completions);
            if (found == state->sessions.end() || found->second != record ||
                record->close_requested || record->consumers == 0) {
                if (status) {
                    status = CancelledStatus();
                }
                if (found != state->sessions.end() && found->second == record) {
                    state->sessions.erase(found);
                }
                release = record->session != nullptr || record->endpoint != nullptr;
            } else if (status && record->endpoint) {
                record->phase = Phase::ready;
                endpoint = record->endpoint;
            } else {
                state->sessions.erase(found);
                record->consumers = 0;
                release = record->session != nullptr || record->endpoint != nullptr;
            }
        }

        if (release) {
            QueueRelease(reactor, record);
        }
        for (auto& completion : completions) {
            if (!completion) {
                continue;
            }
            try {
                completion({status,
                            status ? endpoint : nullptr,
                            host_key,
                            status ? initial_directory : std::string{}});
            } catch (...) {}
        }
    }

    static Status RunOpen(const std::shared_ptr<SessionRecord>& record,
                          const std::shared_ptr<ssh::NetworkReactor>& reactor,
                          const ssh::CancellationToken& cancellation,
                          const std::filesystem::path& known_hosts,
                          FileHostKeyPolicy host_key_policy,
                          const std::shared_ptr<FileSessionAuthentication>& authentication) {
        AuthenticationGuard authentication_guard(authentication);
        if (cancellation.is_cancelled()) {
            return CancelledStatus();
        }

        ssh::SessionConfig config;
        config.host = record->profile.host;
        config.port = record->profile.port;
        auto connected = ConnectSession(config, *reactor, cancellation);
        if (!connected) {
            return connected.status();
        }
        auto session = std::move(connected).value();
        SessionGuard session_guard(session);
        if (auto status = reactor->bind_session_waiter(session, cancellation); !status) {
            return status;
        }

        const auto handshake_deadline = std::chrono::steady_clock::now() + config.io_timeout;
        while (!session->handshake_complete()) {
            if (cancellation.is_cancelled()) {
                return CancelledStatus();
            }
            if (std::chrono::steady_clock::now() >= handshake_deadline) {
                return {ErrorCode::network_error, L"SSH handshake timed out."};
            }
            auto progress = session->advance_handshake();
            if (!progress) {
                return progress.status();
            }
            if (progress.value().state == ssh::AsyncState::would_block) {
                const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                    handshake_deadline - std::chrono::steady_clock::now());
                (void)reactor->wait_socket(session->native_socket(),
                                           progress.value().wait_for,
                                           (std::min)(remaining, 100ms),
                                           cancellation);
            }
        }

        auto inspected = session->inspect_known_host(known_hosts);
        if (!inspected) {
            return inspected.status();
        }
        {
            std::scoped_lock lock(record->mutex);
            record->host_key = inspected.value();
        }
        if (inspected.value().state == ssh::HostKeyState::mismatch) {
            return {ErrorCode::host_key_rejected,
                    L"The SSH host key changed. The connection was blocked; "
                    L"verify known_hosts through a trusted channel."};
        }
        if (inspected.value().state == ssh::HostKeyState::not_found) {
            if (host_key_policy != FileHostKeyPolicy::accept_new) {
                return {ErrorCode::host_key_rejected,
                        L"The SSH host key is not trusted yet. Confirm the "
                        L"reported fingerprint before connecting."};
            }
            if (auto status = session->accept_host_key(known_hosts, false); !status) {
                return status;
            }
        }

        if (cancellation.is_cancelled()) {
            return CancelledStatus();
        }
        if (auto status = Authenticate(*session, record->profile, *authentication); !status) {
            return status;
        }
        if (auto status = session->configure_keepalive(kKeepaliveInterval, true); !status) {
            return status;
        }
        if (cancellation.is_cancelled()) {
            return CancelledStatus();
        }

        auto adapter = session->create_sftp_adapter(record->profile.id);
        if (!adapter) {
            return adapter.status();
        }
        auto sftp_adapter = std::move(adapter).value();
        std::string initial_directory;
        try {
            const std::string home = sftp_adapter->realpath(".");
            if (home.empty()) {
                return {ErrorCode::io_error, L"SFTP 服务器没有返回有效的用户主目录。"};
            }
            if (!record->profile.initial_directory.empty()) {
                const std::string requested =
                    ResolveConfiguredInitialPath(home, record->profile.initial_directory);
                initial_directory = sftp_adapter->realpath(requested);
                if (sftp_adapter->stat(initial_directory).kind != transfer::FileKind::Directory) {
                    return {ErrorCode::invalid_argument, L"配置的远端初始目录不是文件夹。"};
                }
            } else {
                initial_directory = home;
                if (IsWindowsSftpPath(home)) {
                    for (const auto& candidate : WindowsDesktopCandidates(home)) {
                        try {
                            const std::string canonical = sftp_adapter->realpath(candidate);
                            if (sftp_adapter->stat(canonical).kind ==
                                transfer::FileKind::Directory) {
                                initial_directory = canonical;
                                break;
                            }
                        } catch (...) {
                            // Desktop redirection differs across Windows
                            // installations. Try the next known candidate.
                        }
                    }
                }
            }
        } catch (const std::exception& error) {
            return {ErrorCode::io_error,
                    L"无法解析远端初始目录：" +
                        WidenError(error.what() != nullptr ? error.what() : "")};
        }
        auto underlying = std::make_shared<transfer::SftpEndpoint>(std::move(sftp_adapter));

        const std::weak_ptr<ssh::NetworkReactor> weak_reactor = reactor;
        const std::weak_ptr<ssh::SshSession> weak_session = session;
        ssh::ReactorEndpointBinder binder =
            [weak_reactor, weak_session](const ssh::CancellationToken& operation_cancellation) {
                const auto locked_reactor = weak_reactor.lock();
                const auto locked_session = weak_session.lock();
                if (!locked_reactor || !locked_session) {
                    return Status{ErrorCode::not_found,
                                  L"The SFTP file session is no longer available."};
                }
                return locked_reactor->bind_session_waiter(locked_session, operation_cancellation);
            };

        // The custom deleter pins NetworkReactor until ReactorEndpoint has
        // marshalled destruction of all thread-affine SFTP objects.
        std::shared_ptr<ssh::ReactorEndpoint> endpoint(
            new ssh::ReactorEndpoint(*reactor, std::move(underlying), std::move(binder)),
            [reactor](ssh::ReactorEndpoint* value) noexcept {
                delete value;
            });
        {
            std::scoped_lock lock(record->mutex);
            record->session = session;
            record->endpoint = std::move(endpoint);
            record->initial_directory = std::move(initial_directory);
        }
        session_guard.release();
        return Status::ok();
    }

    ssh::ReactorTaskHandle AsyncOpen(storage::ServerProfile profile,
                                     FileSessionAuthentication authentication,
                                     FileHostKeyPolicy host_key_policy,
                                     OpenCompletion completion) {
        // 第一步先做不接触网络的参数、认证类型和构建能力校验。失败结果也统一经
        // Dispatcher 回到 UI，并在返回前清零调用方传入的认证材料。
        if (!completion) {
            ClearAuthentication(authentication);
            return {};
        }
        if (profile.id.empty() || profile.host.empty() || profile.username.empty() ||
            profile.port == 0) {
            ClearAuthentication(authentication);
            PostOpen(state,
                     std::move(completion),
                     {{ErrorCode::invalid_argument, L"服务器 ID、地址、端口和用户名不能为空。"},
                      nullptr,
                      std::nullopt});
            return {};
        }
        if (!AuthenticationMatches(profile.authentication, authentication)) {
            ClearAuthentication(authentication);
            PostOpen(state,
                     std::move(completion),
                     {{ErrorCode::invalid_argument, L"认证数据与服务器配置的认证方式不匹配。"},
                      nullptr,
                      std::nullopt});
            return {};
        }
        if (!ssh::libssh2_enabled()) {
            ClearAuthentication(authentication);
            PostOpen(state,
                     std::move(completion),
                     {{ErrorCode::unsupported, L"当前构建未包含 libssh2，无法建立 SFTP 连接。"},
                      nullptr,
                      std::nullopt});
            return {};
        }

        std::shared_ptr<SessionRecord> record;
        std::shared_ptr<ssh::ReactorEndpoint> reused_endpoint;
        std::string reused_initial_directory;
        ssh::ReactorTaskHandle coalesced_task;
        bool reused{};
        bool coalesced{};
        bool profile_conflict{};
        {
            // 第二步在控制器锁内按 server-id 决定复用策略：ready 会话增加消费者；
            // connecting 会话合并回调；同 id 但配置不同则拒绝，避免连到错误服务器。
            std::scoped_lock state_lock(state->mutex);
            if (state->shutting_down) {
                ClearAuthentication(authentication);
                return {};
            }
            const auto found = state->sessions.find(profile.id);
            if (found != state->sessions.end()) {
                record = found->second;
                std::scoped_lock record_lock(record->mutex);
                if (record->profile != profile) {
                    profile_conflict = true;
                } else if (record->phase == Phase::ready && record->endpoint) {
                    ++record->consumers;
                    reused_endpoint = record->endpoint;
                    reused_initial_directory = record->initial_directory;
                    reused = true;
                } else if (record->phase == Phase::connecting) {
                    ++record->consumers;
                    record->completions.push_back(std::move(completion));
                    coalesced_task = record->connect_task;
                    coalesced = true;
                } else {
                    profile_conflict = true;
                }
            } else {
                record = std::make_shared<SessionRecord>(std::move(profile));
                record->completions.push_back(std::move(completion));
                state->sessions.emplace(record->profile.id, record);
            }
        }

        if (profile_conflict) {
            ClearAuthentication(authentication);
            PostOpen(state,
                     std::move(completion),
                     {{ErrorCode::conflict,
                       L"A different or closing profile already uses this "
                       L"server id."},
                      nullptr,
                      std::nullopt});
            return {};
        }
        if (reused) {
            ClearAuthentication(authentication);
            PostOpen(state,
                     std::move(completion),
                     {Status::ok(),
                      std::move(reused_endpoint),
                      std::nullopt,
                      std::move(reused_initial_directory)});
            return {};
        }
        if (coalesced) {
            ClearAuthentication(authentication);
            return coalesced_task;
        }

        // 第三步仅首个调用创建 Reactor 任务。认证对象通过 shared_ptr 延长到任务结束，
        // RunOpen 负责建立 TCP/SSH/SFTP 并清零凭据，FinishOpen 再一次性通知全部合并调用。
        auto authentication_ptr =
            std::make_shared<FileSessionAuthentication>(std::move(authentication));
        const std::weak_ptr<ControlState> weak_state = state;
        try {
            auto task = reactor->submit(
                [record,
                 reactor = reactor,
                 known_hosts = known_hosts,
                 host_key_policy,
                 authentication =
                     authentication_ptr](const ssh::CancellationToken& cancellation) mutable {
                    return RunOpen(record,
                                   reactor,
                                   cancellation,
                                   known_hosts,
                                   host_key_policy,
                                   authentication);
                },
                [weak_state, record, reactor = reactor](ssh::ReactorTaskResult result) {
                    FinishOpen(weak_state, record, std::move(result.status), reactor);
                });
            {
                std::scoped_lock lock(record->mutex);
                record->connect_task = task;
            }
            return task;
        } catch (const std::exception& error) {
            // submit 自身失败时撤销尚未可见的 SessionRecord，并向所有已合并调用发布
            // 同一个失败结果，保证消费者计数不会留下无法关闭的幽灵会话。
            ClearAuthentication(*authentication_ptr);
            {
                std::scoped_lock state_lock(state->mutex);
                const auto found = state->sessions.find(record->profile.id);
                if (found != state->sessions.end() && found->second == record) {
                    state->sessions.erase(found);
                }
            }
            std::vector<OpenCompletion> completions;
            {
                std::scoped_lock lock(record->mutex);
                completions.swap(record->completions);
                record->consumers = 0;
            }
            const Status failure{ErrorCode::io_error,
                                 WidenError(error.what() != nullptr ? error.what() : "")};
            for (auto& callback : completions) {
                PostOpen(state, std::move(callback), {failure, nullptr, std::nullopt});
            }
            return {};
        }
    }

    void Close(std::string_view server_id) {
        if (server_id.empty()) {
            return;
        }
        std::shared_ptr<SessionRecord> record;
        ssh::ReactorTaskHandle connect_task;
        bool release{};
        {
            std::scoped_lock state_lock(state->mutex);
            if (state->shutting_down) {
                return;
            }
            const auto found = state->sessions.find(std::string(server_id));
            if (found == state->sessions.end()) {
                return;
            }
            record = found->second;
            std::scoped_lock record_lock(record->mutex);
            if (record->consumers != 0) {
                --record->consumers;
            }
            if (record->consumers == 0) {
                record->close_requested = true;
                record->completions.clear();
                connect_task = record->connect_task;
                state->sessions.erase(found);
                release = true;
            }
        }
        if (release) {
            connect_task.cancel();
            QueueRelease(reactor, record);
        }
    }

    void Invalidate(std::string_view server_id) {
        if (server_id.empty()) {
            return;
        }
        std::shared_ptr<SessionRecord> record;
        ssh::ReactorTaskHandle connect_task;
        {
            std::scoped_lock state_lock(state->mutex);
            if (state->shutting_down) {
                return;
            }
            const auto found = state->sessions.find(std::string(server_id));
            if (found == state->sessions.end()) {
                return;
            }
            record = found->second;
            {
                std::scoped_lock record_lock(record->mutex);
                record->close_requested = true;
                record->phase = Phase::closing;
                record->consumers = 0;
                record->completions.clear();
                connect_task = record->connect_task;
            }
            // 先从索引中移除，使紧接着的 AsyncOpen 可以创建新会话；
            // 旧句柄的断开工作仍保持在同一 Reactor 队列中有序执行。
            state->sessions.erase(found);
        }
        connect_task.cancel();
        QueueRelease(reactor, record);
    }

    std::shared_ptr<ssh::ReactorEndpoint> EndpointFor(const transfer::FileUri& uri) const {
        if (!uri.is_sftp()) {
            return {};
        }
        std::scoped_lock state_lock(state->mutex);
        if (state->shutting_down) {
            return {};
        }
        const auto found = state->sessions.find(uri.authority());
        if (found == state->sessions.end()) {
            return {};
        }
        std::scoped_lock record_lock(found->second->mutex);
        if (found->second->phase != Phase::ready || found->second->close_requested) {
            return {};
        }
        return found->second->endpoint;
    }

    ssh::ReactorTaskHandle AsyncList(transfer::FileUri directory, ListCompletion completion) {
        if (!completion) {
            return {};
        }
        if (!directory.is_sftp()) {
            PostList(state,
                     std::move(completion),
                     {{ErrorCode::invalid_argument, L"AsyncList requires an sftp:// URI."},
                      std::move(directory),
                      {}});
            return {};
        }

        std::shared_ptr<SessionRecord> record;
        std::shared_ptr<ssh::ReactorEndpoint> endpoint;
        {
            std::scoped_lock state_lock(state->mutex);
            if (state->shutting_down) {
                return {};
            }
            const auto found = state->sessions.find(directory.authority());
            if (found != state->sessions.end()) {
                record = found->second;
                std::scoped_lock record_lock(record->mutex);
                if (record->phase == Phase::ready && !record->close_requested) {
                    endpoint = record->endpoint;
                }
            }
        }
        if (!record || !endpoint) {
            PostList(state,
                     std::move(completion),
                     {{ErrorCode::not_found, L"No connected SFTP endpoint exists for this URI."},
                      std::move(directory),
                      {}});
            return {};
        }

        auto payload = std::make_shared<ListPayload>();
        const std::weak_ptr<ControlState> weak_state = state;
        try {
            return reactor->submit(
                [record, endpoint, directory, payload, reactor = reactor](
                    const ssh::CancellationToken& cancellation) {
                    if (cancellation.is_cancelled()) {
                        return CancelledStatus();
                    }
                    std::shared_ptr<ssh::SshSession> session;
                    {
                        std::scoped_lock lock(record->mutex);
                        if (record->close_requested || record->phase != Phase::ready) {
                            return Status{ErrorCode::not_found,
                                          L"The SFTP file session was closed."};
                        }
                        session = record->session;
                    }
                    if (!session) {
                        return Status{ErrorCode::not_found,
                                      L"The SFTP file session is unavailable."};
                    }
                    // Do not interrupt a libssh2 SFTP request after it has
                    // entered its EAGAIN state machine. The reactor still
                    // marks this task cancelled after list() returns, so stale
                    // directory results are discarded without damaging the
                    // shared SFTP channel used by the other panes.
                    if (auto status = reactor->bind_session_waiter_non_interruptible(session);
                        !status) {
                        return status;
                    }
                    try {
                        auto entries = endpoint->list(directory);
                        {
                            std::scoped_lock lock(payload->mutex);
                            payload->entries = std::move(entries);
                        }
                        return Status::ok();
                    } catch (...) {
                        return ExceptionStatus();
                    }
                },
                [weak_state, completion, directory, payload](
                    ssh::ReactorTaskResult result) mutable {
                    const auto state = weak_state.lock();
                    if (!state) {
                        return;
                    }
                    FileDirectoryListResult list_result;
                    list_result.status = std::move(result.status);
                    list_result.directory = std::move(directory);
                    if (list_result.status) {
                        std::scoped_lock lock(payload->mutex);
                        list_result.entries = std::move(payload->entries);
                    }
                    {
                        std::scoped_lock lock(state->mutex);
                        if (state->shutting_down) {
                            return;
                        }
                    }
                    try {
                        completion(std::move(list_result));
                    } catch (...) {}
                });
        } catch (const std::exception& error) {
            PostList(
                state,
                std::move(completion),
                {{ErrorCode::io_error, WidenError(error.what() != nullptr ? error.what() : "")},
                 std::move(directory),
                 {}});
            return {};
        }
    }

    std::size_t ConsumerCount(std::string_view server_id) const noexcept {
        try {
            std::scoped_lock state_lock(state->mutex);
            const auto found = state->sessions.find(std::string(server_id));
            if (found == state->sessions.end()) {
                return 0;
            }
            std::scoped_lock record_lock(found->second->mutex);
            return found->second->consumers;
        } catch (...) {
            return 0;
        }
    }

    void Shutdown() noexcept {
        StopKeepaliveWorker();
        const auto local_state = state;
        auto local_reactor = reactor;
        if (!local_state || !local_reactor) {
            return;
        }

        std::vector<std::shared_ptr<SessionRecord>> records;
        std::vector<ssh::ReactorTaskHandle> connect_tasks;
        {
            std::scoped_lock state_lock(local_state->mutex);
            if (local_state->shutting_down) {
                return;
            }
            local_state->shutting_down = true;
            records.reserve(local_state->sessions.size());
            connect_tasks.reserve(local_state->sessions.size());
            for (auto& [id, record] : local_state->sessions) {
                (void)id;
                {
                    std::scoped_lock record_lock(record->mutex);
                    record->close_requested = true;
                    record->consumers = 0;
                    record->completions.clear();
                    connect_tasks.push_back(record->connect_task);
                }
                records.push_back(record);
            }
            local_state->sessions.clear();
        }
        for (const auto& task : connect_tasks) {
            task.cancel();
        }

        auto barrier = std::make_shared<Barrier>();
        try {
            const auto cleanup =
                local_reactor->submit([local_reactor, records = std::move(records)](
                                          const ssh::CancellationToken& cancellation) mutable {
                    for (const auto& record : records) {
                        ReleaseRecordOnReactor(record, *local_reactor, cancellation);
                    }
                    records.clear();
                    return Status::ok();
                });
            (void)cleanup;
            // This sentinel intentionally does not capture NetworkReactor. By
            // the time it starts, every earlier task (including cleanup tasks
            // that did capture the reactor) has destroyed its queue item. The
            // controller can then release its owner reference without making
            // NetworkReactor's destructor run on its own worker thread.
            const auto sentinel = local_reactor->submit([barrier](const ssh::CancellationToken&) {
                barrier->signal();
                return Status::ok();
            });
            (void)sentinel;
            barrier->wait();
        } catch (...) {
            // NetworkReactor accepts work until its destructor initiates
            // shutdown. Reaching this path means construction or ownership was
            // already violated; keep teardown noexcept.
        }

        {
            std::scoped_lock lock(local_state->mutex);
            local_state->reactor = nullptr;
        }
        reactor.reset();
    }
};

FileSessionController::FileSessionController(std::shared_ptr<ssh::IUiDispatcher> ui_dispatcher,
                                             std::filesystem::path known_hosts)
    : impl_(std::make_unique<Impl>(std::move(ui_dispatcher), std::move(known_hosts))) {}

FileSessionController::~FileSessionController() = default;

ssh::ReactorTaskHandle FileSessionController::AsyncOpen(storage::ServerProfile profile,
                                                        FileSessionAuthentication authentication,
                                                        FileHostKeyPolicy host_key_policy,
                                                        OpenCompletion completion) {
    return impl_->AsyncOpen(
        std::move(profile), std::move(authentication), host_key_policy, std::move(completion));
}

void FileSessionController::Close(std::string_view server_id) {
    impl_->Close(server_id);
}

void FileSessionController::Invalidate(std::string_view server_id) {
    impl_->Invalidate(server_id);
}

std::shared_ptr<ssh::ReactorEndpoint>
FileSessionController::EndpointFor(const transfer::FileUri& uri) const {
    return impl_->EndpointFor(uri);
}

Result<std::shared_ptr<transfer::IFileEndpoint>>
FileSessionController::OpenDedicated(storage::ServerProfile profile,
                                     FileSessionAuthentication authentication,
                                     const FileHostKeyPolicy host_key_policy) {
    return impl_->OpenDedicated(std::move(profile), std::move(authentication), host_key_policy);
}

ssh::ReactorTaskHandle FileSessionController::AsyncList(transfer::FileUri directory,
                                                        ListCompletion completion) {
    return impl_->AsyncList(std::move(directory), std::move(completion));
}

std::size_t FileSessionController::consumer_count(std::string_view server_id) const noexcept {
    return impl_->ConsumerCount(server_id);
}

void FileSessionController::Shutdown() noexcept {
    if (impl_) {
        impl_->Shutdown();
    }
}

} // namespace SLink::app
