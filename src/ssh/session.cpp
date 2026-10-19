// libssh2 会话、认证、主机密钥、Shell Channel 和 SFTP Adapter 的底层封装。
// 所有 EAGAIN 循环都委托给外部等待器，以便 Reactor 统一实现超时与取消。
#include "ssh/session.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>

#ifdef SLINK_USE_LIBSSH2

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <fstream>
#include <libssh2.h>
#include <libssh2_sftp.h>
#include <sstream>
#include <winsock2.h>
#include <ws2tcpip.h>

#endif

namespace SLink::ssh {
namespace {

[[maybe_unused]] Status unsupported_status() {
    return {ErrorCode::unsupported,
            L"SSH support is unavailable: this build does not include libssh2."};
}

#ifdef SLINK_USE_LIBSSH2

std::recursive_mutex known_hosts_mutex;
std::atomic<std::uint64_t> known_hosts_temp_sequence{};

std::wstring widen_ascii(std::string_view text) {
    return std::wstring(text.begin(), text.end());
}

std::wstring libssh_error_message(LIBSSH2_SESSION* session, std::string_view operation, int code) {
    char* detail = nullptr;
    int detail_length = 0;
    libssh2_session_last_error(session, &detail, &detail_length, 0);
    std::string message(operation);
    message += " failed (";
    message += std::to_string(code);
    message += ')';
    if (detail != nullptr && detail_length > 0) {
        message += ": ";
        message.append(detail, static_cast<std::size_t>(detail_length));
    }
    return widen_ascii(message);
}

ErrorCode map_error(int code) {
    switch (code) {
    case LIBSSH2_ERROR_AUTHENTICATION_FAILED:
    case LIBSSH2_ERROR_PUBLICKEY_UNVERIFIED:
        return ErrorCode::authentication_failed;
    case LIBSSH2_ERROR_PASSWORD_EXPIRED:
    case LIBSSH2_ERROR_METHOD_NOT_SUPPORTED:
        return ErrorCode::unsupported;
    case LIBSSH2_ERROR_SOCKET_DISCONNECT:
    case LIBSSH2_ERROR_SOCKET_SEND:
    case LIBSSH2_ERROR_SOCKET_RECV:
    case LIBSSH2_ERROR_SOCKET_TIMEOUT:
        return ErrorCode::network_error;
    case LIBSSH2_ERROR_ALLOC:
        return ErrorCode::io_error;
    default:
        return ErrorCode::protocol_error;
    }
}

std::string path_utf8(const std::filesystem::path& path) {
    const auto encoded = path.u8string();
    return {reinterpret_cast<const char*>(encoded.data()), encoded.size()};
}

class WinsockRuntime {
public:
    WinsockRuntime() {
        WSADATA data{};
        result_ = WSAStartup(MAKEWORD(2, 2), &data);
    }
    ~WinsockRuntime() {
        if (result_ == 0)
            WSACleanup();
    }
    int result() const noexcept {
        return result_;
    }

private:
    int result_{};
};

WinsockRuntime& winsock_runtime() {
    static WinsockRuntime runtime;
    return runtime;
}

class Libssh2Runtime {
public:
    Libssh2Runtime() : result_(libssh2_init(0)) {}
    ~Libssh2Runtime() {
        if (result_ == 0)
            libssh2_exit();
    }
    int result() const noexcept {
        return result_;
    }

private:
    int result_{};
};

Libssh2Runtime& libssh2_runtime() {
    static Libssh2Runtime runtime;
    return runtime;
}

std::string sha256_fingerprint(LIBSSH2_SESSION* session) {
    const auto* digest = reinterpret_cast<const unsigned char*>(
        libssh2_hostkey_hash(session, LIBSSH2_HOSTKEY_HASH_SHA256));
    if (digest == nullptr)
        return {};
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string result = "SHA256:";
    for (std::size_t i = 0; i < 32; ++i) {
        if (i != 0)
            result += ':';
        result += hex[digest[i] >> 4];
        result += hex[digest[i] & 0x0f];
    }
    return result;
}

int known_key_mask(int hostkey_type) {
    switch (hostkey_type) {
    case LIBSSH2_HOSTKEY_TYPE_RSA:
        return LIBSSH2_KNOWNHOST_KEY_SSHRSA;
    case LIBSSH2_HOSTKEY_TYPE_DSS:
        return LIBSSH2_KNOWNHOST_KEY_SSHDSS;
#ifdef LIBSSH2_HOSTKEY_TYPE_ECDSA_256
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_256:
        return LIBSSH2_KNOWNHOST_KEY_ECDSA_256;
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_384:
        return LIBSSH2_KNOWNHOST_KEY_ECDSA_384;
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_521:
        return LIBSSH2_KNOWNHOST_KEY_ECDSA_521;
#endif
#ifdef LIBSSH2_HOSTKEY_TYPE_ED25519
    case LIBSSH2_HOSTKEY_TYPE_ED25519:
        return LIBSSH2_KNOWNHOST_KEY_ED25519;
#endif
    default:
        return 0;
    }
}

std::string hostkey_algorithm(int hostkey_type) {
    switch (hostkey_type) {
    case LIBSSH2_HOSTKEY_TYPE_RSA:
        return "ssh-rsa";
    case LIBSSH2_HOSTKEY_TYPE_DSS:
        return "ssh-dss";
#ifdef LIBSSH2_HOSTKEY_TYPE_ECDSA_256
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_256:
        return "ecdsa-sha2-nistp256";
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_384:
        return "ecdsa-sha2-nistp384";
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_521:
        return "ecdsa-sha2-nistp521";
#endif
#ifdef LIBSSH2_HOSTKEY_TYPE_ED25519
    case LIBSSH2_HOSTKEY_TYPE_ED25519:
        return "ssh-ed25519";
#endif
    default:
        return "unknown";
    }
}

#endif

} // namespace

bool libssh2_enabled() noexcept {
#ifdef SLINK_USE_LIBSSH2
    return true;
#else
    return false;
#endif
}

#ifndef SLINK_USE_LIBSSH2

class SshSession::Impl {};
class ShellChannel::Impl {};

SshSession::SshSession(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
SshSession::~SshSession() = default;

Result<std::shared_ptr<SshSession>> SshSession::connect(SessionConfig, SocketWaitCallback) {
    return unsupported_status();
}

Result<std::shared_ptr<SshSession>>
SshSession::adopt_connected_socket(std::uintptr_t, bool, SessionConfig, SocketWaitCallback) {
    return unsupported_status();
}

Result<AsyncProgress> SshSession::advance_handshake() {
    return unsupported_status();
}
WaitDirection SshSession::blocked_directions() const {
    return WaitDirection::none;
}
bool SshSession::handshake_complete() const noexcept {
    return false;
}
bool SshSession::authenticated() const noexcept {
    return false;
}

Result<HostKeyInfo> SshSession::inspect_known_host(const std::filesystem::path&) {
    return unsupported_status();
}
Status SshSession::accept_host_key(const std::filesystem::path&, bool) {
    return unsupported_status();
}
Status SshSession::authenticate(const PasswordAuth&) {
    return unsupported_status();
}
Status SshSession::authenticate(const PrivateKeyAuth&) {
    return unsupported_status();
}
Status SshSession::authenticate(const AgentAuth&) {
    return unsupported_status();
}
Status SshSession::authenticate(const KeyboardInteractiveAuth&) {
    return unsupported_status();
}
Result<std::unique_ptr<ShellChannel>>
SshSession::open_shell(std::string, std::uint32_t, std::uint32_t) {
    return unsupported_status();
}
Result<std::shared_ptr<transfer::ISftpAdapter>> SshSession::create_sftp_adapter(std::string) {
    return unsupported_status();
}
Status SshSession::configure_keepalive(std::chrono::seconds, bool) {
    return unsupported_status();
}
Status SshSession::send_keepalive() {
    return unsupported_status();
}
Status SshSession::disconnect() {
    return unsupported_status();
}
const SessionConfig& SshSession::config() const noexcept {
    static const SessionConfig empty;
    return empty;
}
Status SshSession::set_socket_waiter(SocketWaitCallback) {
    return unsupported_status();
}
std::uintptr_t SshSession::native_socket() const noexcept {
    return 0;
}

ShellChannel::ShellChannel(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
ShellChannel::~ShellChannel() = default;
Result<ChannelIo> ShellChannel::read(std::span<std::byte>, bool) {
    return unsupported_status();
}
Result<ChannelIo> ShellChannel::write(std::span<const std::byte>) {
    return unsupported_status();
}
Status ShellChannel::resize_pty(std::uint32_t, std::uint32_t) {
    return unsupported_status();
}
Status ShellChannel::send_eof() {
    return unsupported_status();
}
bool ShellChannel::eof() const {
    return true;
}

#else

class SshSession::Impl {
public:
    Impl(SOCKET socket_value,
         bool owns,
         SessionConfig session_config,
         SocketWaitCallback socket_wait)
        : socket(socket_value), owns_socket(owns), config(std::move(session_config)),
          wait(std::move(socket_wait)), owner(std::this_thread::get_id()) {
        session = libssh2_session_init_ex(nullptr, nullptr, nullptr, this);
        if (session != nullptr)
            libssh2_session_set_blocking(session, 0);
        if (!wait) {
            wait = [this](WaitDirection directions, std::chrono::milliseconds timeout) {
                fd_set reads;
                fd_set writes;
                FD_ZERO(&reads);
                FD_ZERO(&writes);
                if (has_direction(directions, WaitDirection::read))
                    FD_SET(socket, &reads);
                if (has_direction(directions, WaitDirection::write))
                    FD_SET(socket, &writes);
                timeval value{static_cast<long>(timeout.count() / 1000),
                              static_cast<long>((timeout.count() % 1000) * 1000)};
                const auto result = select(0, &reads, &writes, nullptr, &value);
                return result > 0;
            };
        }
    }

    ~Impl() {
        if (session != nullptr) {
            libssh2_session_disconnect_ex(
                session, SSH_DISCONNECT_BY_APPLICATION, "SLink closing", "");
            libssh2_session_free(session);
        }
        if (owns_socket && socket != INVALID_SOCKET)
            closesocket(socket);
    }

    Status require_owner() const {
        if (owner != std::this_thread::get_id()) {
            return {ErrorCode::invalid_argument, L"SSH session accessed from a non-owning thread."};
        }
        if (disconnected) {
            return {ErrorCode::network_error, L"SSH session is disconnected."};
        }
        return Status::ok();
    }

    WaitDirection directions() const {
        if (session == nullptr)
            return WaitDirection::none;
        const auto mask = libssh2_session_block_directions(session);
        auto result = WaitDirection::none;
        if ((mask & LIBSSH2_SESSION_BLOCK_INBOUND) != 0)
            result = result | WaitDirection::read;
        if ((mask & LIBSSH2_SESSION_BLOCK_OUTBOUND) != 0)
            result = result | WaitDirection::write;
        return result;
    }

    int retry(const std::function<int()>& operation) {
        for (;;) {
            const auto result = operation();
            if (result != LIBSSH2_ERROR_EAGAIN)
                return result;
            if (!wait(directions(), config.io_timeout))
                return LIBSSH2_ERROR_SOCKET_TIMEOUT;
        }
    }

    template <typename T> T* retry_pointer(const std::function<T*()>& operation) {
        for (;;) {
            if (auto* result = operation(); result != nullptr)
                return result;
            if (libssh2_session_last_errno(session) != LIBSSH2_ERROR_EAGAIN)
                return nullptr;
            if (!wait(directions(), config.io_timeout))
                return nullptr;
        }
    }

    Status result_status(std::string_view operation, int result) const {
        if (result == 0)
            return Status::ok();
        return {map_error(result), libssh_error_message(session, operation, result)};
    }

    static void keyboard_callback(const char* name,
                                  int name_length,
                                  const char* instruction,
                                  int instruction_length,
                                  int prompt_count,
                                  const LIBSSH2_USERAUTH_KBDINT_PROMPT* prompts,
                                  LIBSSH2_USERAUTH_KBDINT_RESPONSE* responses,
                                  void** abstract) {
        auto* self = abstract != nullptr ? static_cast<Impl*>(*abstract) : nullptr;
        if (self == nullptr || self->active_keyboard == nullptr ||
            !self->active_keyboard->respond) {
            if (self != nullptr) {
                self->keyboard_error = {
                    ErrorCode::unsupported,
                    L"Keyboard-interactive authentication has no response callback."};
            }
            return;
        }

        std::vector<KeyboardPrompt> converted;
        converted.reserve(static_cast<std::size_t>(std::max(prompt_count, 0)));
        for (int index = 0; index < prompt_count; ++index) {
            converted.push_back({std::string(reinterpret_cast<const char*>(prompts[index].text),
                                             static_cast<std::size_t>(prompts[index].length)),
                                 prompts[index].echo != 0});
        }
        auto result = self->active_keyboard->respond(
            std::string_view(name != nullptr ? name : "",
                             static_cast<std::size_t>(std::max(name_length, 0))),
            std::string_view(instruction != nullptr ? instruction : "",
                             static_cast<std::size_t>(std::max(instruction_length, 0))),
            converted);
        if (!result) {
            self->keyboard_error = result.status();
            return;
        }
        if (result.value().size() != converted.size()) {
            self->keyboard_error = {ErrorCode::invalid_argument,
                                    L"Keyboard response count does not match prompts."};
            return;
        }
        for (std::size_t index = 0; index < converted.size(); ++index) {
            const auto& text = result.value()[index];
            auto* allocated = static_cast<char*>(std::malloc(text.size() + 1));
            if (allocated == nullptr) {
                self->keyboard_error = {ErrorCode::io_error, L"Cannot allocate keyboard response."};
                return;
            }
            std::memcpy(allocated, text.data(), text.size());
            allocated[text.size()] = '\0';
            responses[index].text = allocated;
            responses[index].length = static_cast<unsigned int>(text.size());
        }
    }

    SOCKET socket{INVALID_SOCKET};
    bool owns_socket{};
    SessionConfig config;
    SocketWaitCallback wait;
    std::thread::id owner;
    LIBSSH2_SESSION* session{};
    bool handshake_done{};
    bool host_key_verified{};
    bool authenticated{};
    bool disconnected{};
    const KeyboardInteractiveAuth* active_keyboard{};
    Status keyboard_error;
};

namespace {

Result<SOCKET> connect_socket(const SessionConfig& config) {
    if (winsock_runtime().result() != 0) {
        return Status{ErrorCode::network_error, L"WSAStartup failed."};
    }
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* addresses = nullptr;
    const auto service = std::to_string(config.port);
    const auto lookup = getaddrinfo(config.host.c_str(), service.c_str(), &hints, &addresses);
    if (lookup != 0) {
        return Status{ErrorCode::network_error, L"Cannot resolve SSH host."};
    }
    struct AddressGuard {
        addrinfo* value;
        ~AddressGuard() {
            freeaddrinfo(value);
        }
    } guard{addresses};

    for (auto* address = addresses; address != nullptr; address = address->ai_next) {
        const auto candidate =
            socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (candidate == INVALID_SOCKET)
            continue;
        u_long nonblocking = 1;
        if (ioctlsocket(candidate, FIONBIO, &nonblocking) != 0) {
            closesocket(candidate);
            continue;
        }
        const auto connected =
            ::connect(candidate, address->ai_addr, static_cast<int>(address->ai_addrlen));
        if (connected == 0)
            return candidate;
        if (WSAGetLastError() != WSAEWOULDBLOCK) {
            closesocket(candidate);
            continue;
        }

        fd_set writes;
        fd_set errors;
        FD_ZERO(&writes);
        FD_ZERO(&errors);
        FD_SET(candidate, &writes);
        FD_SET(candidate, &errors);
        timeval timeout{static_cast<long>(config.connect_timeout.count() / 1000),
                        static_cast<long>((config.connect_timeout.count() % 1000) * 1000)};
        if (select(0, nullptr, &writes, &errors, &timeout) <= 0) {
            closesocket(candidate);
            continue;
        }
        int socket_error = 0;
        int length = sizeof(socket_error);
        if (getsockopt(
                candidate, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&socket_error), &length) ==
                0 &&
            socket_error == 0) {
            return candidate;
        }
        closesocket(candidate);
    }
    return Status{ErrorCode::network_error,
                  L"Cannot connect to the SSH server within the configured timeout."};
}

} // namespace

SshSession::SshSession(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
SshSession::~SshSession() = default;

Result<std::shared_ptr<SshSession>> SshSession::connect(SessionConfig config,
                                                        SocketWaitCallback wait) {
    if (config.host.empty() || config.port == 0) {
        return Status{ErrorCode::invalid_argument, L"SSH host and port are required."};
    }
    if (libssh2_runtime().result() != 0) {
        return Status{ErrorCode::protocol_error, L"libssh2 initialization failed."};
    }
    auto connected = connect_socket(config);
    if (!connected)
        return connected.status();
    return adopt_connected_socket(
        static_cast<std::uintptr_t>(connected.value()), true, std::move(config), std::move(wait));
}

Result<std::shared_ptr<SshSession>> SshSession::adopt_connected_socket(std::uintptr_t native_socket,
                                                                       bool take_ownership,
                                                                       SessionConfig config,
                                                                       SocketWaitCallback wait) {
    if (libssh2_runtime().result() != 0) {
        return Status{ErrorCode::protocol_error, L"libssh2 initialization failed."};
    }
    if (native_socket == static_cast<std::uintptr_t>(INVALID_SOCKET)) {
        return Status{ErrorCode::invalid_argument, L"Invalid connected socket."};
    }
    u_long nonblocking = 1;
    if (ioctlsocket(static_cast<SOCKET>(native_socket), FIONBIO, &nonblocking) != 0) {
        if (take_ownership)
            closesocket(static_cast<SOCKET>(native_socket));
        return Status{ErrorCode::network_error,
                      L"Cannot put the connected socket into non-blocking mode."};
    }
    auto impl = std::make_unique<Impl>(
        static_cast<SOCKET>(native_socket), take_ownership, std::move(config), std::move(wait));
    if (impl->session == nullptr) {
        return Status{ErrorCode::protocol_error, L"Cannot allocate libssh2 session."};
    }
    return std::shared_ptr<SshSession>(new SshSession(std::move(impl)));
}

Result<AsyncProgress> SshSession::advance_handshake() {
    if (const auto owner = impl_->require_owner(); !owner)
        return owner;
    if (impl_->handshake_done)
        return AsyncProgress{};
    const auto result =
        libssh2_session_handshake(impl_->session, static_cast<libssh2_socket_t>(impl_->socket));
    if (result == 0) {
        impl_->handshake_done = true;
        return AsyncProgress{};
    }
    if (result == LIBSSH2_ERROR_EAGAIN) {
        return AsyncProgress{AsyncState::would_block, impl_->directions()};
    }
    return Status{map_error(result), libssh_error_message(impl_->session, "SSH handshake", result)};
}

WaitDirection SshSession::blocked_directions() const {
    if (impl_->owner != std::this_thread::get_id())
        return WaitDirection::none;
    return impl_->directions();
}
bool SshSession::handshake_complete() const noexcept {
    return impl_->handshake_done;
}
bool SshSession::authenticated() const noexcept {
    return impl_->authenticated;
}

Result<HostKeyInfo> SshSession::inspect_known_host(const std::filesystem::path& known_hosts_file) {
    if (const auto owner = impl_->require_owner(); !owner)
        return owner;
    std::scoped_lock known_hosts_lock(known_hosts_mutex);
    if (!impl_->handshake_done) {
        return Status{ErrorCode::protocol_error,
                      L"Host key cannot be checked before SSH handshake completes."};
    }
    std::size_t key_length = 0;
    int key_type = 0;
    const auto* key = libssh2_session_hostkey(impl_->session, &key_length, &key_type);
    const auto key_mask = known_key_mask(key_type);
    if (key == nullptr || key_mask == 0) {
        return Status{ErrorCode::unsupported, L"Unsupported SSH host-key algorithm."};
    }
    auto* hosts = libssh2_knownhost_init(impl_->session);
    if (hosts == nullptr) {
        return Status{ErrorCode::protocol_error, L"Cannot initialize known-hosts parser."};
    }
    struct Guard {
        LIBSSH2_KNOWNHOSTS* value;
        ~Guard() {
            libssh2_knownhost_free(value);
        }
    } guard{hosts};
    if (std::filesystem::exists(known_hosts_file)) {
        const auto filename = path_utf8(known_hosts_file);
        if (libssh2_knownhost_readfile(hosts, filename.c_str(), LIBSSH2_KNOWNHOST_FILE_OPENSSH) <
            0) {
            return Status{ErrorCode::io_error, L"Cannot read known_hosts."};
        }
    }
    struct libssh2_knownhost* matched = nullptr;
    const auto checked = libssh2_knownhost_checkp(hosts,
                                                  impl_->config.host.c_str(),
                                                  impl_->config.port,
                                                  key,
                                                  key_length,
                                                  LIBSSH2_KNOWNHOST_TYPE_PLAIN |
                                                      LIBSSH2_KNOWNHOST_KEYENC_RAW | key_mask,
                                                  &matched);
    HostKeyState state;
    if (checked == LIBSSH2_KNOWNHOST_CHECK_MATCH)
        state = HostKeyState::matched;
    else if (checked == LIBSSH2_KNOWNHOST_CHECK_NOTFOUND)
        state = HostKeyState::not_found;
    else if (checked == LIBSSH2_KNOWNHOST_CHECK_MISMATCH)
        state = HostKeyState::mismatch;
    else
        return Status{ErrorCode::protocol_error, L"Known-hosts verification failed."};
    impl_->host_key_verified = state == HostKeyState::matched;
    return HostKeyInfo{state, hostkey_algorithm(key_type), sha256_fingerprint(impl_->session)};
}

Status SshSession::accept_host_key(const std::filesystem::path& known_hosts_file,
                                   bool replace_changed) {
    std::scoped_lock known_hosts_lock(known_hosts_mutex);
    auto inspected = inspect_known_host(known_hosts_file);
    if (!inspected)
        return inspected.status();
    if (inspected.value().state == HostKeyState::matched)
        return Status::ok();
    if (inspected.value().state == HostKeyState::mismatch && !replace_changed) {
        return {ErrorCode::host_key_rejected,
                L"Host key changed; explicit replacement approval is required."};
    }

    std::size_t key_length = 0;
    int key_type = 0;
    const auto* key = libssh2_session_hostkey(impl_->session, &key_length, &key_type);
    const auto key_mask = known_key_mask(key_type);
    auto* hosts = libssh2_knownhost_init(impl_->session);
    if (hosts == nullptr) {
        return {ErrorCode::protocol_error, L"Cannot initialize known-hosts parser."};
    }
    struct Guard {
        LIBSSH2_KNOWNHOSTS* value;
        ~Guard() {
            libssh2_knownhost_free(value);
        }
    } guard{hosts};
    const auto filename = path_utf8(known_hosts_file);
    if (std::filesystem::exists(known_hosts_file) &&
        libssh2_knownhost_readfile(hosts, filename.c_str(), LIBSSH2_KNOWNHOST_FILE_OPENSSH) < 0) {
        return {ErrorCode::io_error, L"Cannot read known_hosts."};
    }
    struct libssh2_knownhost* existing = nullptr;
    const auto checked = libssh2_knownhost_checkp(hosts,
                                                  impl_->config.host.c_str(),
                                                  impl_->config.port,
                                                  key,
                                                  key_length,
                                                  LIBSSH2_KNOWNHOST_TYPE_PLAIN |
                                                      LIBSSH2_KNOWNHOST_KEYENC_RAW | key_mask,
                                                  &existing);
    if (checked == LIBSSH2_KNOWNHOST_CHECK_MISMATCH && replace_changed && existing != nullptr) {
        if (libssh2_knownhost_del(hosts, existing) != 0) {
            return {ErrorCode::io_error, L"Cannot remove changed known-host entry."};
        }
    } else if (checked == LIBSSH2_KNOWNHOST_CHECK_MISMATCH && replace_changed) {
        return {ErrorCode::protocol_error,
                L"Changed host key could not be located for safe replacement."};
    }
    std::string host = impl_->config.port == 22
                           ? impl_->config.host
                           : "[" + impl_->config.host + "]:" + std::to_string(impl_->config.port);
    if (libssh2_knownhost_addc(hosts,
                               host.data(),
                               nullptr,
                               const_cast<char*>(key),
                               key_length,
                               "SLink",
                               13,
                               LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW |
                                   key_mask,
                               nullptr) != 0) {
        return {ErrorCode::io_error, L"Cannot add known-host entry."};
    }
    if (!known_hosts_file.parent_path().empty()) {
        std::error_code directory_error;
        std::filesystem::create_directories(known_hosts_file.parent_path(), directory_error);
        if (directory_error) {
            return {ErrorCode::io_error, L"Cannot create known_hosts directory."};
        }
    }
    const auto temporary =
        known_hosts_file.parent_path() /
        (known_hosts_file.filename().wstring() + L".SLink.tmp." +
         std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetCurrentThreadId()) +
         L"." + std::to_wstring(known_hosts_temp_sequence.fetch_add(1, std::memory_order_relaxed)));
    const auto temporary_filename = path_utf8(temporary);
    if (libssh2_knownhost_writefile(
            hosts, temporary_filename.c_str(), LIBSSH2_KNOWNHOST_FILE_OPENSSH) != 0) {
        return {ErrorCode::io_error, L"Cannot write known_hosts."};
    }
    if (!MoveFileExW(temporary.c_str(),
                     known_hosts_file.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD move_error = GetLastError();
        DeleteFileW(temporary.c_str());
        return {ErrorCode::io_error,
                L"Cannot atomically replace known_hosts (Windows error " +
                    std::to_wstring(move_error) + L")."};
    }
    impl_->host_key_verified = true;
    return Status::ok();
}

Status SshSession::authenticate(const PasswordAuth& auth) {
    if (const auto owner = impl_->require_owner(); !owner)
        return owner;
    if (!impl_->handshake_done) {
        return {ErrorCode::protocol_error, L"Authentication requires a completed handshake."};
    }
    if (!impl_->host_key_verified) {
        return {ErrorCode::host_key_rejected,
                L"Authentication is blocked until the host key is verified."};
    }
    if (auth.username.empty()) {
        return {ErrorCode::invalid_argument, L"Authentication username is required."};
    }
    const auto result = impl_->retry([&] {
        return libssh2_userauth_password_ex(impl_->session,
                                            auth.username.data(),
                                            static_cast<unsigned int>(auth.username.size()),
                                            auth.password.data(),
                                            static_cast<unsigned int>(auth.password.size()),
                                            nullptr);
    });
    const auto status = impl_->result_status("Password authentication", result);
    if (status)
        impl_->authenticated = true;
    return status;
}

Status SshSession::authenticate(const PrivateKeyAuth& auth) {
    if (const auto owner = impl_->require_owner(); !owner)
        return owner;
    if (!impl_->handshake_done) {
        return {ErrorCode::protocol_error, L"Authentication requires a completed handshake."};
    }
    if (!impl_->host_key_verified) {
        return {ErrorCode::host_key_rejected,
                L"Authentication is blocked until the host key is verified."};
    }
    if (auth.username.empty() || auth.private_key.empty()) {
        return {ErrorCode::invalid_argument, L"Username and in-memory private key are required."};
    }
    const auto* public_data = auth.public_key.empty() ? nullptr : auth.public_key.data();
    const auto* passphrase = auth.passphrase.empty() ? nullptr : auth.passphrase.c_str();
    const auto result = impl_->retry([&] {
        return libssh2_userauth_publickey_frommemory(impl_->session,
                                                     auth.username.data(),
                                                     auth.username.size(),
                                                     public_data,
                                                     auth.public_key.size(),
                                                     auth.private_key.data(),
                                                     auth.private_key.size(),
                                                     passphrase);
    });
    const auto status = impl_->result_status("Private-key authentication", result);
    if (status)
        impl_->authenticated = true;
    return status;
}

Status SshSession::authenticate(const AgentAuth& auth) {
    if (const auto owner = impl_->require_owner(); !owner)
        return owner;
    if (!impl_->handshake_done) {
        return {ErrorCode::protocol_error, L"Authentication requires a completed handshake."};
    }
    if (!impl_->host_key_verified) {
        return {ErrorCode::host_key_rejected,
                L"Authentication is blocked until the host key is verified."};
    }
    if (auth.username.empty()) {
        return {ErrorCode::invalid_argument, L"Authentication username is required."};
    }
    auto* agent = libssh2_agent_init(impl_->session);
    if (agent == nullptr) {
        return {ErrorCode::unsupported,
                L"OpenSSH agent support is unavailable in this libssh2 build."};
    }
    struct Guard {
        LIBSSH2_AGENT* value;
        ~Guard() {
            libssh2_agent_disconnect(value);
            libssh2_agent_free(value);
        }
    } guard{agent};
    auto result = impl_->retry([&] {
        return libssh2_agent_connect(agent);
    });
    if (result != 0)
        return impl_->result_status("Agent connection", result);
    result = impl_->retry([&] {
        return libssh2_agent_list_identities(agent);
    });
    if (result != 0)
        return impl_->result_status("Agent identity listing", result);
    struct libssh2_agent_publickey* previous = nullptr;
    for (;;) {
        struct libssh2_agent_publickey* identity = nullptr;
        result = impl_->retry([&] {
            return libssh2_agent_get_identity(agent, &identity, previous);
        });
        if (result == 1)
            break;
        if (result < 0)
            return impl_->result_status("Agent identity lookup", result);
        result = impl_->retry([&] {
            return libssh2_agent_userauth(agent, auth.username.c_str(), identity);
        });
        if (result == 0) {
            impl_->authenticated = true;
            return Status::ok();
        }
        if (result != LIBSSH2_ERROR_AUTHENTICATION_FAILED) {
            return impl_->result_status("Agent authentication", result);
        }
        previous = identity;
    }
    return {ErrorCode::authentication_failed,
            L"No OpenSSH agent identity authenticated successfully."};
}

Status SshSession::authenticate(const KeyboardInteractiveAuth& auth) {
    if (const auto owner = impl_->require_owner(); !owner)
        return owner;
    if (!impl_->handshake_done) {
        return {ErrorCode::protocol_error, L"Authentication requires a completed handshake."};
    }
    if (!impl_->host_key_verified) {
        return {ErrorCode::host_key_rejected,
                L"Authentication is blocked until the host key is verified."};
    }
    if (!auth.respond) {
        return {ErrorCode::unsupported,
                L"Keyboard-interactive authentication requires a secure prompt callback."};
    }
    impl_->active_keyboard = &auth;
    impl_->keyboard_error = Status::ok();
    const auto result = impl_->retry([&] {
        return libssh2_userauth_keyboard_interactive_ex(
            impl_->session,
            auth.username.data(),
            static_cast<unsigned int>(auth.username.size()),
            &Impl::keyboard_callback);
    });
    impl_->active_keyboard = nullptr;
    if (!impl_->keyboard_error)
        return impl_->keyboard_error;
    const auto status = impl_->result_status("Keyboard-interactive authentication", result);
    if (status)
        impl_->authenticated = true;
    return status;
}

const SessionConfig& SshSession::config() const noexcept {
    return impl_->config;
}
Status SshSession::configure_keepalive(const std::chrono::seconds interval, const bool want_reply) {
    if (const auto owner = impl_->require_owner(); !owner)
        return owner;
    if (interval.count() < 0 ||
        static_cast<std::uint64_t>(interval.count()) > (std::numeric_limits<unsigned int>::max)()) {
        return {ErrorCode::invalid_argument,
                L"SSH keepalive interval is outside the supported range."};
    }
    libssh2_keepalive_config(
        impl_->session, want_reply ? 1 : 0, static_cast<unsigned int>(interval.count()));
    return Status::ok();
}

Status SshSession::send_keepalive() {
    if (const auto owner = impl_->require_owner(); !owner)
        return owner;
    if (!impl_->authenticated || impl_->disconnected) {
        return {ErrorCode::protocol_error, L"SSH keepalive requires a live authenticated session."};
    }
    int seconds_to_next = 0;
    const auto result = impl_->retry([&] {
        return libssh2_keepalive_send(impl_->session, &seconds_to_next);
    });
    return impl_->result_status("SSH keepalive", result);
}

Status SshSession::set_socket_waiter(SocketWaitCallback wait) {
    if (const auto owner = impl_->require_owner(); !owner)
        return owner;
    if (!wait) {
        return {ErrorCode::invalid_argument, L"Socket wait callback cannot be empty."};
    }
    impl_->wait = std::move(wait);
    return Status::ok();
}
std::uintptr_t SshSession::native_socket() const noexcept {
    return static_cast<std::uintptr_t>(impl_->socket);
}

Status SshSession::disconnect() {
    if (const auto owner = impl_->require_owner(); !owner)
        return owner;
    const auto result = impl_->retry([&] {
        return libssh2_session_disconnect_ex(
            impl_->session, SSH_DISCONNECT_BY_APPLICATION, "SLink disconnect", "");
    });
    if (result == 0)
        impl_->disconnected = true;
    return impl_->result_status("SSH disconnect", result);
}

class ShellChannel::Impl {
public:
    Impl(std::shared_ptr<SshSession> parent, LIBSSH2_CHANNEL* value)
        : session(std::move(parent)), channel(value) {}
    ~Impl() {
        if (channel != nullptr)
            libssh2_channel_free(channel);
    }
    std::shared_ptr<SshSession> session;
    LIBSSH2_CHANNEL* channel{};
};

ShellChannel::ShellChannel(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
ShellChannel::~ShellChannel() = default;

Result<std::unique_ptr<ShellChannel>>
SshSession::open_shell(std::string terminal_type, std::uint32_t columns, std::uint32_t rows) {
    if (const auto owner = impl_->require_owner(); !owner)
        return owner;
    if (!impl_->authenticated) {
        return Status{ErrorCode::authentication_failed,
                      L"Shell channel requires an authenticated session."};
    }
    auto* channel = impl_->retry_pointer<LIBSSH2_CHANNEL>([&] {
        return libssh2_channel_open_session(impl_->session);
    });
    if (channel == nullptr) {
        const auto error = libssh2_session_last_errno(impl_->session);
        return Status{map_error(error),
                      libssh_error_message(impl_->session, "Open shell channel", error)};
    }
    auto shell = std::unique_ptr<ShellChannel>(
        new ShellChannel(std::make_unique<ShellChannel::Impl>(shared_from_this(), channel)));
    auto result = impl_->retry([&] {
        return libssh2_channel_request_pty_ex(channel,
                                              terminal_type.data(),
                                              static_cast<unsigned int>(terminal_type.size()),
                                              nullptr,
                                              0,
                                              static_cast<int>(columns),
                                              static_cast<int>(rows),
                                              0,
                                              0);
    });
    if (result == 0) {
        result = impl_->retry([&] {
            return libssh2_channel_shell(channel);
        });
    }
    if (result != 0) {
        return Status{map_error(result),
                      libssh_error_message(impl_->session, "Start PTY shell", result)};
    }
    return shell;
}

Result<ChannelIo> ShellChannel::read(std::span<std::byte> output, bool stderr_stream) {
    if (const auto owner = impl_->session->impl_->require_owner(); !owner)
        return owner;
    const auto result = libssh2_channel_read_ex(impl_->channel,
                                                stderr_stream ? SSH_EXTENDED_DATA_STDERR : 0,
                                                reinterpret_cast<char*>(output.data()),
                                                output.size());
    if (result >= 0) {
        return ChannelIo{static_cast<std::size_t>(result),
                         libssh2_channel_eof(impl_->channel) != 0,
                         WaitDirection::none};
    }
    if (result == LIBSSH2_ERROR_EAGAIN) {
        return ChannelIo{0, false, impl_->session->impl_->directions()};
    }
    return Status{map_error(static_cast<int>(result)),
                  libssh_error_message(impl_->session->impl_->session,
                                       "Read shell channel",
                                       static_cast<int>(result))};
}

Result<ChannelIo> ShellChannel::write(std::span<const std::byte> input) {
    if (const auto owner = impl_->session->impl_->require_owner(); !owner)
        return owner;
    const auto result = libssh2_channel_write_ex(
        impl_->channel, 0, reinterpret_cast<const char*>(input.data()), input.size());
    if (result >= 0) {
        return ChannelIo{static_cast<std::size_t>(result), false, WaitDirection::none};
    }
    if (result == LIBSSH2_ERROR_EAGAIN) {
        return ChannelIo{0, false, impl_->session->impl_->directions()};
    }
    return Status{map_error(static_cast<int>(result)),
                  libssh_error_message(impl_->session->impl_->session,
                                       "Write shell channel",
                                       static_cast<int>(result))};
}

Status ShellChannel::resize_pty(std::uint32_t columns, std::uint32_t rows) {
    if (const auto owner = impl_->session->impl_->require_owner(); !owner)
        return owner;
    const auto result = impl_->session->impl_->retry([&] {
        return libssh2_channel_request_pty_size_ex(
            impl_->channel, static_cast<int>(columns), static_cast<int>(rows), 0, 0);
    });
    return impl_->session->impl_->result_status("Resize PTY", result);
}

Status ShellChannel::send_eof() {
    if (const auto owner = impl_->session->impl_->require_owner(); !owner)
        return owner;
    const auto result = impl_->session->impl_->retry([&] {
        return libssh2_channel_send_eof(impl_->channel);
    });
    return impl_->session->impl_->result_status("Send shell EOF", result);
}

bool ShellChannel::eof() const {
    if (impl_->session->impl_->owner != std::this_thread::get_id())
        return true;
    return libssh2_channel_eof(impl_->channel) != 0;
}

class Libssh2SftpAdapter final : public transfer::ISftpAdapter,
                                 public std::enable_shared_from_this<Libssh2SftpAdapter> {
public:
    class ReadStream;
    class WriteStream;

    Libssh2SftpAdapter(std::shared_ptr<SshSession> parent, std::string id, LIBSSH2_SFTP* value)
        : session_(std::move(parent)), server_id_(std::move(id)), sftp_(value) {}
    ~Libssh2SftpAdapter() override {
        // A wrong-thread destructor must not call into libssh2. The session owns
        // the underlying allocations and releases them at session teardown.
        if (sftp_ != nullptr && session_->impl_->owner == std::this_thread::get_id()) {
            libssh2_sftp_shutdown(sftp_);
        }
    }

    std::string_view server_id() const noexcept override {
        return server_id_;
    }
    bool connected() const noexcept override {
        return session_->impl_->owner == std::this_thread::get_id() && sftp_ != nullptr &&
               !session_->impl_->disconnected;
    }

    std::string realpath(const std::string_view path) override {
        require_owner();
        const std::string requested(path);
        std::array<char, 32 * 1024> resolved{};
        const auto length = session_->impl_->retry([&] {
            return libssh2_sftp_realpath(sftp_,
                                         requested.c_str(),
                                         resolved.data(),
                                         static_cast<unsigned int>(resolved.size()));
        });
        if (length < 0) {
            throw_last("Resolve SFTP path");
        }
        return std::string(resolved.data(), static_cast<std::size_t>(length));
    }

    void require_owner() const {
        const auto status = session_->impl_->require_owner();
        if (!status) {
            throw std::logic_error("SFTP adapter used outside its owning reactor thread");
        }
        if (sftp_ == nullptr)
            throw std::runtime_error("SFTP subsystem is closed");
    }

    static std::string owner_from_long_entry(const char* entry, std::size_t capacity) {
        if (entry == nullptr || capacity == 0)
            return {};
        std::size_t length = 0;
        while (length < capacity && entry[length] != '\0')
            ++length;
        std::istringstream stream(std::string(entry, std::min(length, capacity)));
        std::string permissions;
        std::string links;
        std::string owner;
        if (!(stream >> permissions >> links >> owner) || permissions.empty()) {
            return {};
        }
        const char type = permissions.front();
        if (type != '-' && type != 'd' && type != 'l' && type != 'b' && type != 'c' &&
            type != 'p' && type != 's') {
            return {};
        }
        return owner;
    }

    transfer::FileStat convert_stat(const LIBSSH2_SFTP_ATTRIBUTES& attributes,
                                    std::string_view reported_owner = {}) {
        transfer::FileStat result;
        if ((attributes.flags & LIBSSH2_SFTP_ATTR_SIZE) != 0)
            result.size = attributes.filesize;
        if ((attributes.flags & LIBSSH2_SFTP_ATTR_ACMODTIME) != 0)
            result.modified = std::chrono::system_clock::from_time_t(attributes.mtime);
        if ((attributes.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) != 0) {
            result.permissions = attributes.permissions;
            if (LIBSSH2_SFTP_S_ISREG(attributes.permissions))
                result.kind = transfer::FileKind::Regular;
            else if (LIBSSH2_SFTP_S_ISDIR(attributes.permissions))
                result.kind = transfer::FileKind::Directory;
            else if (LIBSSH2_SFTP_S_ISLNK(attributes.permissions))
                result.kind = transfer::FileKind::Symlink;
            else
                result.kind = transfer::FileKind::Other;
        }
        if ((attributes.flags & LIBSSH2_SFTP_ATTR_UIDGID) != 0) {
            if (!reported_owner.empty()) {
                result.owner.assign(reported_owner);
                owner_names_[attributes.uid] = result.owner;
            } else if (const auto known = owner_names_.find(attributes.uid);
                       known != owner_names_.end()) {
                result.owner = known->second;
            } else if (attributes.uid == 0) {
                result.owner = "root";
            }
        }
        return result;
    }

    std::vector<transfer::DirectoryEntry> list(std::string_view path) override {
        require_owner();
        auto* handle = session_->impl_->retry_pointer<LIBSSH2_SFTP_HANDLE>([&] {
            return libssh2_sftp_open_ex(sftp_,
                                        path.data(),
                                        static_cast<unsigned int>(path.size()),
                                        0,
                                        0,
                                        LIBSSH2_SFTP_OPENDIR);
        });
        if (handle == nullptr)
            throw_last("Open SFTP directory");
        struct Guard {
            Libssh2SftpAdapter* self;
            LIBSSH2_SFTP_HANDLE* value;
            ~Guard() {
                self->session_->impl_->retry([&] {
                    return libssh2_sftp_close_handle(value);
                });
            }
        } guard{this, handle};

        std::vector<transfer::DirectoryEntry> result;
        std::array<char, 32 * 1024> name{};
        std::array<char, 32 * 1024> long_entry{};
        for (;;) {
            LIBSSH2_SFTP_ATTRIBUTES attributes{};
            long_entry.fill('\0');
            const auto read = session_->impl_->retry([&] {
                return libssh2_sftp_readdir_ex(handle,
                                               name.data(),
                                               name.size(),
                                               long_entry.data(),
                                               static_cast<unsigned int>(long_entry.size()),
                                               &attributes);
            });
            if (read == 0)
                break;
            if (read < 0)
                throw_last("Read SFTP directory");
            std::string filename(name.data(), static_cast<std::size_t>(read));
            if (filename == "." || filename == "..")
                continue;
            result.push_back(
                {filename,
                 transfer::FileUri::sftp(
                     server_id_, std::string(path) + (path.ends_with('/') ? "" : "/") + filename),
                 convert_stat(attributes,
                              owner_from_long_entry(long_entry.data(), long_entry.size()))});
        }
        return result;
    }

    transfer::FileStat stat(std::string_view path) override {
        require_owner();
        LIBSSH2_SFTP_ATTRIBUTES attributes{};
        const auto result = session_->impl_->retry([&] {
            return libssh2_sftp_stat_ex(sftp_,
                                        path.data(),
                                        static_cast<unsigned int>(path.size()),
                                        LIBSSH2_SFTP_LSTAT,
                                        &attributes);
        });
        if (result != 0)
            throw_last("Stat SFTP path");
        return convert_stat(attributes);
    }

    bool exists(std::string_view path) override {
        require_owner();
        LIBSSH2_SFTP_ATTRIBUTES attributes{};
        const auto result = session_->impl_->retry([&] {
            return libssh2_sftp_stat_ex(sftp_,
                                        path.data(),
                                        static_cast<unsigned int>(path.size()),
                                        LIBSSH2_SFTP_LSTAT,
                                        &attributes);
        });
        if (result == 0)
            return true;
        const auto sftp_error = libssh2_sftp_last_error(sftp_);
        if (sftp_error == LIBSSH2_FX_NO_SUCH_FILE)
            return false;
#ifdef LIBSSH2_FX_NO_SUCH_PATH
        if (sftp_error == LIBSSH2_FX_NO_SUCH_PATH)
            return false;
#endif
        throw_last("Check SFTP path");
    }

    std::unique_ptr<transfer::IReadStream> open_read(std::string_view path,
                                                     std::uint64_t offset) override;
    std::unique_ptr<transfer::IWriteStream>
    open_write(std::string_view path, std::uint64_t offset, bool truncate) override;

    void rename(std::string_view from, std::string_view to, bool replace) override {
        require_owner();

        // SFTP v3's ordinary SSH_FXP_RENAME does not define portable
        // replacement semantics. In particular, OpenSSH rejects an ordinary
        // rename when the destination already exists even when libssh2's
        // OVERWRITE preference flag is supplied. Its posix-rename extension
        // provides the atomic replace operation required by remote editor
        // saves, so use that first whenever replacement was requested.
        if (replace) {
            const auto posix_result = session_->impl_->retry([&] {
                return libssh2_sftp_posix_rename_ex(
                    sftp_, from.data(), from.size(), to.data(), to.size());
            });
            if (posix_result == 0) {
                return;
            }
            const auto sftp_error = libssh2_sftp_last_error(sftp_);
            if (sftp_error != LIBSSH2_FX_OP_UNSUPPORTED) {
                throw_last("Atomically replace SFTP path");
            }
        }

        // Servers without posix-rename@openssh.com may still implement the
        // standard rename request and honor libssh2's overwrite preference.
        long flags = LIBSSH2_SFTP_RENAME_ATOMIC | LIBSSH2_SFTP_RENAME_NATIVE;
        if (replace)
            flags |= LIBSSH2_SFTP_RENAME_OVERWRITE;
        const auto result = session_->impl_->retry([&] {
            return libssh2_sftp_rename_ex(sftp_,
                                          from.data(),
                                          static_cast<unsigned int>(from.size()),
                                          to.data(),
                                          static_cast<unsigned int>(to.size()),
                                          flags);
        });
        if (result != 0) {
            throw_last(replace ? "Replace SFTP path" : "Rename SFTP path");
        }
    }

    void remove(std::string_view path, bool recursive) override {
        require_owner();
        const auto metadata = stat(path);
        if (metadata.kind == transfer::FileKind::Directory) {
            if (recursive) {
                for (const auto& entry : list(path))
                    remove(entry.uri.path(), true);
            }
            const auto result = session_->impl_->retry([&] {
                return libssh2_sftp_rmdir_ex(
                    sftp_, path.data(), static_cast<unsigned int>(path.size()));
            });
            if (result != 0)
                throw_last("Remove SFTP directory");
        } else {
            const auto result = session_->impl_->retry([&] {
                return libssh2_sftp_unlink_ex(
                    sftp_, path.data(), static_cast<unsigned int>(path.size()));
            });
            if (result != 0)
                throw_last("Remove SFTP file");
        }
    }

    void mkdir(std::string_view path, bool recursive) override {
        require_owner();
        if (!recursive) {
            mkdir_one(path);
            return;
        }
        std::string current = path.starts_with('/') ? "/" : "";
        std::size_t start = path.starts_with('/') ? 1 : 0;
        while (start <= path.size()) {
            const auto end = path.find('/', start);
            const auto component = path.substr(
                start, end == std::string_view::npos ? std::string_view::npos : end - start);
            if (!component.empty()) {
                if (!current.empty() && current.back() != '/')
                    current += '/';
                current.append(component);
                if (!exists(current))
                    mkdir_one(current);
            }
            if (end == std::string_view::npos)
                break;
            start = end + 1;
        }
    }

    void set_permissions(std::string_view path, std::uint32_t permissions) override {
        require_owner();
        LIBSSH2_SFTP_ATTRIBUTES attributes{};
        attributes.flags = LIBSSH2_SFTP_ATTR_PERMISSIONS;
        attributes.permissions = permissions;
        const auto result = session_->impl_->retry([&] {
            return libssh2_sftp_stat_ex(sftp_,
                                        path.data(),
                                        static_cast<unsigned int>(path.size()),
                                        LIBSSH2_SFTP_SETSTAT,
                                        &attributes);
        });
        if (result != 0)
            throw_last("Set SFTP permissions");
    }

    void close_handle(LIBSSH2_SFTP_HANDLE* handle) noexcept {
        if (handle == nullptr || session_->impl_->owner != std::this_thread::get_id())
            return;
        session_->impl_->retry([&] {
            return libssh2_sftp_close_handle(handle);
        });
    }

    [[noreturn]] void throw_last(std::string_view operation) const {
        const auto code = libssh2_session_last_errno(session_->impl_->session);
        const auto message = libssh_error_message(session_->impl_->session, operation, code);
        std::string narrow;
        narrow.reserve(message.size());
        for (const auto value : message) {
            narrow.push_back(value <= 0xff ? static_cast<char>(value) : '?');
        }
        throw std::runtime_error(narrow);
    }

private:
    void mkdir_one(std::string_view path) {
        const auto result = session_->impl_->retry([&] {
            return libssh2_sftp_mkdir_ex(
                sftp_, path.data(), static_cast<unsigned int>(path.size()), 0755);
        });
        if (result != 0)
            throw_last("Create SFTP directory");
    }

    std::shared_ptr<SshSession> session_;
    std::string server_id_;
    LIBSSH2_SFTP* sftp_{};
    std::unordered_map<unsigned long, std::string> owner_names_;
};

class Libssh2SftpAdapter::ReadStream final : public transfer::IReadStream {
public:
    ReadStream(std::shared_ptr<Libssh2SftpAdapter> parent, LIBSSH2_SFTP_HANDLE* handle)
        : parent_(std::move(parent)), handle_(handle) {}
    ~ReadStream() override {
        parent_->close_handle(handle_);
    }
    std::size_t read(std::span<std::byte> output) override {
        parent_->require_owner();
        if (output.empty())
            return 0;

        // libssh2 commonly returns one SFTP packet (roughly 32 KiB) even
        // when the caller supplies a multi-megabyte transfer buffer. Returning
        // after every packet forced a worker -> reactor -> worker round trip
        // for each packet. Aggregate packets that libssh2 already has ready,
        // but stop immediately at EAGAIN so one transfer never waits inside a
        // batch and starves terminal channels sharing the reactor.
        constexpr std::size_t kMaximumBatch = 256U * 1024U;
        const std::size_t requested = (std::min)(output.size(), kMaximumBatch);
        const auto first = parent_->session_->impl_->retry([&] {
            const auto value =
                libssh2_sftp_read(handle_, reinterpret_cast<char*>(output.data()), requested);
            return static_cast<int>(value);
        });
        if (first < 0)
            parent_->throw_last("Read SFTP file");
        if (first == 0)
            return 0;

        std::size_t total = static_cast<std::size_t>(first);
        while (total < requested) {
            const auto value = libssh2_sftp_read(
                handle_, reinterpret_cast<char*>(output.data() + total), requested - total);
            if (value == LIBSSH2_ERROR_EAGAIN || value == 0)
                break;
            if (value < 0)
                parent_->throw_last("Read SFTP file");
            total += static_cast<std::size_t>(value);
        }
        return total;
    }

private:
    std::shared_ptr<Libssh2SftpAdapter> parent_;
    LIBSSH2_SFTP_HANDLE* handle_{};
};

class Libssh2SftpAdapter::WriteStream final : public transfer::IWriteStream {
public:
    WriteStream(std::shared_ptr<Libssh2SftpAdapter> parent, LIBSSH2_SFTP_HANDLE* handle)
        : parent_(std::move(parent)), handle_(handle) {}
    ~WriteStream() override {
        parent_->close_handle(handle_);
    }
    void set_cancellation_flag(std::shared_ptr<const std::atomic_bool> flag) override {
        cancellation_flag_ = std::move(flag);
    }
    void write(std::span<const std::byte> input) override {
        parent_->require_owner();
        std::size_t offset = 0;
        while (offset < input.size()) {
            if (cancellation_flag_ && cancellation_flag_->load(std::memory_order_acquire)) {
                throw std::runtime_error("SFTP write cancelled");
            }
            const auto result = parent_->session_->impl_->retry([&] {
                const auto value =
                    libssh2_sftp_write(handle_,
                                       reinterpret_cast<const char*>(input.data() + offset),
                                       input.size() - offset);
                return static_cast<int>(value);
            });
            if (result <= 0)
                parent_->throw_last("Write SFTP file");
            offset += static_cast<std::size_t>(result);
        }
    }
    void flush() override {
        parent_->require_owner();
        // SFTP fsync is an optional OpenSSH extension. Some servers advertise
        // it but never complete the request, leaving libssh2 at EAGAIN until
        // the generic I/O timeout and incorrectly failing an otherwise fully
        // acknowledged upload. Closing the SFTP handle below already drains
        // pending WRITE replies and is the portable completion boundary.
        // Do not initiate the optional fsync extension here.
    }

private:
    std::shared_ptr<Libssh2SftpAdapter> parent_;
    LIBSSH2_SFTP_HANDLE* handle_{};
    std::shared_ptr<const std::atomic_bool> cancellation_flag_;
};

std::unique_ptr<transfer::IReadStream> Libssh2SftpAdapter::open_read(std::string_view path,
                                                                     std::uint64_t offset) {
    require_owner();
    auto* handle = session_->impl_->retry_pointer<LIBSSH2_SFTP_HANDLE>([&] {
        return libssh2_sftp_open_ex(sftp_,
                                    path.data(),
                                    static_cast<unsigned int>(path.size()),
                                    LIBSSH2_FXF_READ,
                                    0,
                                    LIBSSH2_SFTP_OPENFILE);
    });
    if (handle == nullptr)
        throw_last("Open SFTP file for reading");
    if (offset != 0)
        libssh2_sftp_seek64(handle, offset);
    return std::make_unique<ReadStream>(shared_from_this(), handle);
}

std::unique_ptr<transfer::IWriteStream>
Libssh2SftpAdapter::open_write(std::string_view path, std::uint64_t offset, bool truncate) {
    require_owner();
    unsigned long flags = LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT;
    if (truncate)
        flags |= LIBSSH2_FXF_TRUNC;
    auto* handle = session_->impl_->retry_pointer<LIBSSH2_SFTP_HANDLE>([&] {
        return libssh2_sftp_open_ex(sftp_,
                                    path.data(),
                                    static_cast<unsigned int>(path.size()),
                                    flags,
                                    0644,
                                    LIBSSH2_SFTP_OPENFILE);
    });
    if (handle == nullptr)
        throw_last("Open SFTP file for writing");
    if (offset != 0)
        libssh2_sftp_seek64(handle, offset);
    return std::make_unique<WriteStream>(shared_from_this(), handle);
}

Result<std::shared_ptr<transfer::ISftpAdapter>>
SshSession::create_sftp_adapter(std::string server_id) {
    if (const auto owner = impl_->require_owner(); !owner)
        return owner;
    if (!impl_->authenticated) {
        return Status{ErrorCode::authentication_failed, L"SFTP requires an authenticated session."};
    }
    if (server_id.empty()) {
        return Status{ErrorCode::invalid_argument, L"SFTP server id is required."};
    }
    auto* sftp = impl_->retry_pointer<LIBSSH2_SFTP>([&] {
        return libssh2_sftp_init(impl_->session);
    });
    if (sftp == nullptr) {
        const auto error = libssh2_session_last_errno(impl_->session);
        return Status{map_error(error),
                      libssh_error_message(impl_->session, "Start SFTP subsystem", error)};
    }
    return std::static_pointer_cast<transfer::ISftpAdapter>(
        std::make_shared<Libssh2SftpAdapter>(shared_from_this(), std::move(server_id), sftp));
}

#endif

} // namespace SLink::ssh
