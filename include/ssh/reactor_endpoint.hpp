#pragma once

#include "ssh/network_reactor.hpp"
#include "transfer/endpoint.hpp"

#include <functional>
#include <memory>
#include <stdexcept>

namespace SLink::ssh {

// 每个封送操作开始前调用。SFTP 通常在这里给所属 SshSession 绑定可取消等待器：
//
//   [&reactor, session](const CancellationToken& cancellation) {
//       return reactor.bind_session_waiter(session, cancellation);
//   }
//
// Reactor 线程内的调用直接执行以避免自锁；此类嵌套调用已属于活动任务，调用方负责
// 事先为任务安装等待器。
using ReactorEndpointBinder = std::function<Status(const CancellationToken& cancellation)>;

class ReactorEndpointError final : public std::runtime_error {
public:
    explicit ReactorEndpointError(Status status);

    [[nodiscard]] const Status& status() const noexcept {
        return status_;
    }

private:
    Status status_;
};

// 提供给工作线程的同步 IFileEndpoint 门面。端点操作、流操作以及包装对象的最终销毁
// 全部封送到 NetworkReactor 所在线程。
//
// 生命周期约束：ReactorEndpoint 及其产生的流必须早于 Reactor 销毁。关闭与排队调用
// 竞争时，调用以 cancelled 异常解除阻塞；具有线程亲和性的对象不会在错误线程析构。
class ReactorEndpoint final : public transfer::IFileEndpoint {
public:
    ReactorEndpoint(NetworkReactor& reactor,
                    std::shared_ptr<transfer::IFileEndpoint> endpoint,
                    ReactorEndpointBinder binder = {});
    ~ReactorEndpoint() override;

    ReactorEndpoint(const ReactorEndpoint&) = delete;
    ReactorEndpoint& operator=(const ReactorEndpoint&) = delete;
    ReactorEndpoint(ReactorEndpoint&&) = delete;
    ReactorEndpoint& operator=(ReactorEndpoint&&) = delete;

    [[nodiscard]] bool accepts(const transfer::FileUri& uri) const noexcept override;
    [[nodiscard]] bool available() const noexcept override;
    [[nodiscard]] std::vector<transfer::DirectoryEntry>
    list(const transfer::FileUri& directory) override;
    [[nodiscard]] transfer::FileStat stat(const transfer::FileUri& uri) override;
    [[nodiscard]] bool exists(const transfer::FileUri& uri) override;
    [[nodiscard]] std::unique_ptr<transfer::IReadStream>
    open_read(const transfer::FileUri& uri, std::uint64_t offset = 0) override;
    [[nodiscard]] std::unique_ptr<transfer::IWriteStream> open_write(const transfer::FileUri& uri,
                                                                     std::uint64_t offset = 0,
                                                                     bool truncate = true) override;
    void rename(const transfer::FileUri& from, const transfer::FileUri& to, bool replace) override;
    void remove(const transfer::FileUri& uri, bool recursive) override;
    void mkdir(const transfer::FileUri& uri, bool recursive) override;
    void set_permissions(const transfer::FileUri& uri, std::uint32_t permissions) override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace SLink::ssh
