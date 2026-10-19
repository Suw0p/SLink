// 传输专用 SFTP 连接池实现：按服务器隔离、按等待租约从 1 扩到 4 条连接，定时保活、
// 淘汰损坏连接并在空闲超时后回收线程和会话。
#include "app/transfer_session_pool.hpp"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace SLink::app {
namespace {

using namespace std::chrono_literals;

struct ConnectionSlot final {
    std::shared_ptr<transfer::IFileEndpoint> endpoint;
    bool leased{};
    bool close_on_release{};
    std::chrono::steady_clock::time_point idle_since{};
    std::chrono::steady_clock::time_point last_keepalive{};
};

struct ServerPool final {
    std::vector<std::shared_ptr<ConnectionSlot>> slots;
    std::size_t waiters{};
    bool connecting{};
    bool closing{};
    std::size_t connection_failures{};
    std::chrono::steady_clock::time_point retry_after{};
};

bool retryable_connection_error(const ErrorCode code) noexcept {
    return code == ErrorCode::network_error || code == ErrorCode::io_error ||
           code == ErrorCode::protocol_error;
}

std::chrono::seconds retry_delay(const std::size_t failure) noexcept {
    static constexpr std::array delays{1s, 2s, 5s, 10s, 30s};
    return delays[(std::min)(failure - 1, delays.size() - 1)];
}

std::string status_message_utf8(const Status& status) {
    if (status.message().empty()) {
        return "Unable to create a dedicated SFTP transfer connection";
    }
    const int length = WideCharToMultiByte(CP_UTF8,
                                           WC_ERR_INVALID_CHARS,
                                           status.message().data(),
                                           static_cast<int>(status.message().size()),
                                           nullptr,
                                           0,
                                           nullptr,
                                           nullptr);
    if (length <= 0) {
        return "Unable to create a dedicated SFTP transfer connection";
    }
    std::string result(static_cast<std::size_t>(length), '\0');
    if (WideCharToMultiByte(CP_UTF8,
                            WC_ERR_INVALID_CHARS,
                            status.message().data(),
                            static_cast<int>(status.message().size()),
                            result.data(),
                            length,
                            nullptr,
                            nullptr) != length) {
        return "Unable to create a dedicated SFTP transfer connection";
    }
    return result;
}

} // namespace

struct TransferSessionPool::State final
    : public std::enable_shared_from_this<TransferSessionPool::State> {
    explicit State(ConnectionFactory value) : factory(std::move(value)) {
        if (!factory) {
            throw std::invalid_argument("TransferSessionPool requires a connection factory");
        }
    }

    struct LeaseOwner final {
        std::weak_ptr<State> state;
        std::string server_id;
        std::shared_ptr<ConnectionSlot> slot;

        ~LeaseOwner() {
            // aliasing shared_ptr 的控制块由 LeaseOwner 持有；最后一个端点副本析构时
            // 才把槽位归还池中。关闭端点放在锁外执行，避免 Reactor 退出等待阻塞池锁。
            const auto locked = state.lock();
            if (!locked || !slot)
                return;
            std::shared_ptr<transfer::IFileEndpoint> close;
            {
                std::scoped_lock guard(locked->mutex);
                const auto found = locked->servers.find(server_id);
                slot->leased = false;
                slot->idle_since = std::chrono::steady_clock::now();
                if (slot->close_on_release || !slot->endpoint || !slot->endpoint->available() ||
                    locked->stopping || found == locked->servers.end() || found->second.closing) {
                    close = std::move(slot->endpoint);
                    if (found != locked->servers.end()) {
                        std::erase(found->second.slots, slot);
                        if (found->second.slots.empty() && !found->second.connecting &&
                            found->second.waiters == 0) {
                            locked->servers.erase(found);
                        }
                    }
                }
            }
            locked->changed.notify_all();
            close.reset();
        }
    };

    std::shared_ptr<transfer::IFileEndpoint>
    lease_locked(const std::string& server_id, const std::shared_ptr<ConnectionSlot>& slot) {
        // 返回指向 endpoint 的别名 shared_ptr，同时以 LeaseOwner 作为真实控制块。
        // 调用方因此只能通过正常释放 shared_ptr 归还连接，不会忘记手工 unlock。
        slot->leased = true;
        auto owner = std::make_shared<LeaseOwner>();
        owner->state = weak_from_this();
        owner->server_id = server_id;
        owner->slot = slot;
        return std::shared_ptr<transfer::IFileEndpoint>(owner, slot->endpoint.get());
    }

    void ensure_maintenance_locked() {
        if (maintenance_running)
            return;
        if (maintenance.joinable())
            maintenance.join();
        maintenance_running = true;
        maintenance = std::thread([this] {
            maintenance_loop();
        });
    }

    void maintenance_loop() noexcept {
        std::unique_lock lock(mutex);
        for (;;) {
            changed.wait_for(lock, 5s, [this] {
                return stopping;
            });
            if (stopping)
                break;
            const auto now = std::chrono::steady_clock::now();
            std::vector<std::shared_ptr<transfer::IFileEndpoint>> close;
            std::vector<std::pair<std::shared_ptr<ConnectionSlot>,
                                  std::shared_ptr<transfer::IFileEndpoint>>>
                keepalive;
            for (auto pool = servers.begin(); pool != servers.end();) {
                auto& slots = pool->second.slots;
                std::erase_if(slots, [&close, &keepalive, now](const auto& slot) {
                    if (!slot->endpoint)
                        return true;
                    if (slot->last_keepalive == std::chrono::steady_clock::time_point{} ||
                        now - slot->last_keepalive >= 60s) {
                        slot->last_keepalive = now;
                        keepalive.emplace_back(slot, slot->endpoint);
                    }
                    if (slot->leased ||
                        slot->idle_since == std::chrono::steady_clock::time_point{} ||
                        now - slot->idle_since < 120s) {
                        return false;
                    }
                    close.push_back(std::move(slot->endpoint));
                    return true;
                });
                if (slots.empty() && !pool->second.connecting && pool->second.waiters == 0) {
                    pool = servers.erase(pool);
                } else {
                    ++pool;
                }
            }
            if (!close.empty() || !keepalive.empty()) {
                lock.unlock();
                close.clear();
                std::vector<std::shared_ptr<ConnectionSlot>> failed;
                for (const auto& [slot, endpoint] : keepalive) {
                    const auto dedicated = dynamic_cast<IDedicatedFileEndpoint*>(endpoint.get());
                    if (dedicated && !dedicated->SendKeepalive()) {
                        failed.push_back(slot);
                    }
                }
                lock.lock();
                for (const auto& slot : failed) {
                    slot->close_on_release = true;
                    if (!slot->leased && slot->endpoint) {
                        close.push_back(std::move(slot->endpoint));
                    }
                }
                if (!close.empty()) {
                    lock.unlock();
                    close.clear();
                    lock.lock();
                }
            }
            if (servers.empty())
                break;
        }
        maintenance_running = false;
    }

    std::shared_ptr<transfer::IFileEndpoint>
    Acquire(const transfer::FileUri& uri,
            const std::shared_ptr<const std::atomic_bool>& cancellation) {
        // 步骤 1：按 server-id 登记等待者。WaiterGuard 确保取消、异常或建连失败时也会
        // 归还 waiters 计数，避免连接池误以为仍有需求而持续扩容。
        if (!uri.is_sftp() || uri.authority().empty()) {
            throw std::invalid_argument("TransferSessionPool only accepts sftp:// URIs");
        }
        const std::string server_id(uri.authority());
        std::unique_lock lock(mutex);
        auto& initial = servers[server_id];
        if (initial.closing)
            initial.closing = false;
        ++initial.waiters;
        struct WaiterGuard final {
            State& state;
            std::string server_id;
            bool active{true};
            ~WaiterGuard() {
                if (!active)
                    return;
                const auto found = state.servers.find(server_id);
                if (found != state.servers.end() && found->second.waiters != 0) {
                    --found->second.waiters;
                }
            }
        } waiter{*this, server_id};

        for (;;) {
            // 步骤 2：每轮先淘汰不可用的空闲端点，再优先复用健康连接。端点析构可能
            // 等待 Reactor 退出，因此 stale 必须移动到锁外释放。
            if (stopping || (cancellation && cancellation->load(std::memory_order_acquire))) {
                throw std::runtime_error("SFTP transfer lease cancelled");
            }
            auto found = servers.find(server_id);
            if (found == servers.end()) {
                found = servers.emplace(server_id, ServerPool{}).first;
                found->second.waiters = 1;
            }
            auto& pool = found->second;
            pool.closing = false;
            std::vector<std::shared_ptr<transfer::IFileEndpoint>> stale;
            std::erase_if(pool.slots, [&stale](const auto& slot) {
                if (slot->leased || (slot->endpoint && slot->endpoint->available())) {
                    return false;
                }
                if (slot->endpoint) {
                    stale.push_back(std::move(slot->endpoint));
                }
                return true;
            });
            if (!stale.empty()) {
                lock.unlock();
                stale.clear();
                lock.lock();
                continue;
            }
            for (const auto& slot : pool.slots) {
                if (!slot->leased && slot->endpoint && slot->endpoint->available()) {
                    --pool.waiters;
                    waiter.active = false;
                    return lease_locked(server_id, slot);
                }
            }

            // 步骤 3：没有空闲连接时按“现有连接 + 等待者”计算目标规模，且每台服务器
            // 同时只允许一个建连操作，最大不超过 4，避免并发认证风暴。
            const std::size_t desired =
                std::min(kMaximumConnectionsPerServer, pool.slots.size() + pool.waiters);
            const auto now = std::chrono::steady_clock::now();
            if (!pool.connecting && pool.slots.size() < desired && now >= pool.retry_after) {
                pool.connecting = true;
                lock.unlock();
                Result<std::shared_ptr<transfer::IFileEndpoint>> opened =
                    Status{ErrorCode::io_error, L"无法创建传输专用 SFTP 连接。"};
                try {
                    opened = factory(server_id);
                } catch (...) {}
                lock.lock();
                auto current = servers.find(server_id);
                if (current == servers.end()) {
                    current = servers.emplace(server_id, ServerPool{}).first;
                    current->second.waiters = 1;
                }
                current->second.connecting = false;
                if (!opened) {
                    // 网络/协议错误按 1、2、5、10、30 秒退避；认证和主机密钥错误直接
                    // 返回，不进行无意义重试或重复提示用户。
                    const ErrorCode code = opened.status().code();
                    const std::string failure_message = status_message_utf8(opened.status());
                    if (retryable_connection_error(code) &&
                        current->second.connection_failures < 5) {
                        ++current->second.connection_failures;
                        current->second.retry_after =
                            std::chrono::steady_clock::now() +
                            retry_delay(current->second.connection_failures);
                        changed.notify_all();
                        continue;
                    }
                    if (current->second.waiters != 0)
                        --current->second.waiters;
                    waiter.active = false;
                    changed.notify_all();
                    throw std::runtime_error(failure_message);
                }
                current->second.connection_failures = 0;
                current->second.retry_after = {};
                auto slot = std::make_shared<ConnectionSlot>();
                slot->endpoint = std::move(opened).value();
                slot->last_keepalive = std::chrono::steady_clock::now();
                if (stopping || current->second.closing) {
                    auto close = std::move(slot->endpoint);
                    if (current->second.waiters != 0) {
                        --current->second.waiters;
                    }
                    waiter.active = false;
                    lock.unlock();
                    close.reset();
                    throw std::runtime_error("SFTP transfer pool is closing");
                }
                current->second.slots.push_back(slot);
                // 步骤 4：新连接先进入池，再启动维护线程并立即以 RAII 租约返回。
                // 维护线程负责 60 秒保活和 120 秒空闲回收。
                ensure_maintenance_locked();
                --current->second.waiters;
                waiter.active = false;
                changed.notify_all();
                return lease_locked(server_id, slot);
            }
            changed.wait_for(lock, 100ms);
        }
    }

    ConnectionFactory factory;
    mutable std::mutex mutex;
    std::condition_variable changed;
    bool stopping{};
    bool maintenance_running{};
    std::thread maintenance;
    std::unordered_map<std::string, ServerPool> servers;
};

TransferSessionPool::TransferSessionPool(ConnectionFactory factory)
    : state_(std::make_shared<State>(std::move(factory))) {}

TransferSessionPool::~TransferSessionPool() {
    Shutdown();
}

std::shared_ptr<transfer::IFileEndpoint>
TransferSessionPool::Acquire(const transfer::FileUri& uri,
                             std::shared_ptr<const std::atomic_bool> cancellation) {
    if (!state_) {
        throw std::runtime_error("SFTP transfer pool is unavailable");
    }
    return state_->Acquire(uri, cancellation);
}

void TransferSessionPool::CloseServer(const std::string_view server_id) noexcept {
    if (!state_)
        return;
    std::vector<std::shared_ptr<transfer::IFileEndpoint>> close;
    {
        std::scoped_lock lock(state_->mutex);
        const auto found = state_->servers.find(std::string(server_id));
        if (found == state_->servers.end())
            return;
        found->second.closing = true;
        for (const auto& slot : found->second.slots) {
            if (slot->leased) {
                slot->close_on_release = true;
            } else if (slot->endpoint) {
                close.push_back(std::move(slot->endpoint));
            }
        }
        std::erase_if(found->second.slots, [](const auto& slot) {
            return !slot->endpoint;
        });
        if (found->second.slots.empty() && !found->second.connecting &&
            found->second.waiters == 0) {
            state_->servers.erase(found);
        }
    }
    state_->changed.notify_all();
    close.clear();
}

void TransferSessionPool::Shutdown() noexcept {
    auto state = std::move(state_);
    if (!state)
        return;
    std::vector<std::shared_ptr<transfer::IFileEndpoint>> close;
    {
        std::scoped_lock lock(state->mutex);
        state->stopping = true;
        for (auto& [id, pool] : state->servers) {
            (void)id;
            pool.closing = true;
            for (const auto& slot : pool.slots) {
                if (slot->leased) {
                    slot->close_on_release = true;
                } else if (slot->endpoint) {
                    close.push_back(std::move(slot->endpoint));
                }
            }
        }
    }
    state->changed.notify_all();
    close.clear();
    if (state->maintenance.joinable() &&
        state->maintenance.get_id() != std::this_thread::get_id()) {
        state->maintenance.join();
    }
}

std::size_t TransferSessionPool::connection_count(const std::string_view server_id) const noexcept {
    if (!state_)
        return 0;
    std::scoped_lock lock(state_->mutex);
    const auto found = state_->servers.find(std::string(server_id));
    return found == state_->servers.end() ? 0 : found->second.slots.size();
}

std::size_t TransferSessionPool::leased_count(const std::string_view server_id) const noexcept {
    if (!state_)
        return 0;
    std::scoped_lock lock(state_->mutex);
    const auto found = state_->servers.find(std::string(server_id));
    if (found == state_->servers.end())
        return 0;
    return static_cast<std::size_t>(
        std::ranges::count_if(found->second.slots, [](const auto& slot) {
            return slot->leased;
        }));
}

} // namespace SLink::app
