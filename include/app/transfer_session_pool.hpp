#pragma once

#include "app/file_session_controller.hpp"
#include "core/status.hpp"
#include "transfer/endpoint.hpp"

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <string_view>

namespace SLink::app {

// 传输专用连接池。每个工厂结果都必须是完全独立的 TCP/SSH/SFTP/Reactor 栈，
// 禁止把文件浏览或编辑器正在使用的端点放入池中，避免并发访问 libssh2 句柄。
// 每台服务器按需扩展到最多 4 条连接，租约释放后才允许复用对应端点。
class TransferSessionPool final {
public:
    static constexpr std::size_t kMaximumConnectionsPerServer = 4;

    using ConnectionFactory =
        std::function<Result<std::shared_ptr<transfer::IFileEndpoint>>(std::string_view server_id)>;

    explicit TransferSessionPool(ConnectionFactory factory);
    ~TransferSessionPool();

    TransferSessionPool(const TransferSessionPool&) = delete;
    TransferSessionPool& operator=(const TransferSessionPool&) = delete;

    // 阻塞到取得健康端点、建连失败或收到取消请求。返回的 shared_ptr 本身就是
    // RAII 租约；不得缓存其裸指针，最后一个副本释放时连接才归还池中。
    [[nodiscard]] std::shared_ptr<transfer::IFileEndpoint>
    Acquire(const transfer::FileUri& uri,
            std::shared_ptr<const std::atomic_bool> cancellation = {});

    // 阻止新租约并立即关闭空闲连接；正在使用的连接在最后一个租约归还后关闭。
    void CloseServer(std::string_view server_id) noexcept;
    void Shutdown() noexcept;

    [[nodiscard]] std::size_t connection_count(std::string_view server_id) const noexcept;
    [[nodiscard]] std::size_t leased_count(std::string_view server_id) const noexcept;

private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace SLink::app
