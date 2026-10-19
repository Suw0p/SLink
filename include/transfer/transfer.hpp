#pragma once

#include "transfer/endpoint.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace SLink::transfer {

using TransferId = std::uint64_t;

enum class ConflictPolicy {
    Overwrite,
    Skip,
    AutoRename,
    Resume,
};

enum class TransferState {
    Queued,
    Running,
    Paused,
    Completed,
    Failed,
    Cancelled,
};

struct TransferSpec {
    FileUri source;
    FileUri target;
    ConflictPolicy conflict{ConflictPolicy::Overwrite};
    bool move{false};
    bool verify_sha256{false};
};

struct TransferJob {
    TransferId id{};
    TransferSpec spec;
    FileUri effective_target;
    TransferState state{TransferState::Queued};
    std::uint64_t bytes_transferred{};
    std::uint64_t bytes_total{};
    std::size_t buffer_bytes{};
    bool skipped{};
    bool source_is_directory{};
    std::size_t item_count{};
    std::size_t file_count{};
    std::size_t completed_file_count{};
    bool discovery_complete{};
    std::string error;
};

using TransferItemId = std::uint64_t;

// 递归传输中单个文件/目录的瞬时进度。子项数量可能达到十万级，因此这些记录只在
// 内存中维护，不逐条写入 state.db；父任务恢复时会重新扫描源目录并重建子项。
struct TransferItemProgress {
    TransferItemId id{};
    TransferItemId parent_id{};
    FileUri source;
    FileUri target;
    bool directory{};
    TransferState state{TransferState::Queued};
    std::uint64_t bytes_transferred{};
    std::uint64_t bytes_total{};
    bool skipped{};
    std::string error;
};

struct TransferDetailSnapshot {
    std::uint64_t revision{};
    // reset=true 表示 items 是完整替换；否则只包含请求 revision 之后变化的子项，
    // 用于避免大目录进度刷新时反复复制整棵树。
    bool reset{};
    std::vector<TransferItemProgress> items;
};

using EndpointResolver = std::function<std::shared_ptr<IFileEndpoint>(const FileUri& uri)>;
using CancellableEndpointResolver = std::function<std::shared_ptr<IFileEndpoint>(
    const FileUri& uri, std::shared_ptr<const std::atomic_bool> cancellation)>;
using TransferObserver = std::function<void(TransferJob snapshot)>;

// 文件级并行传输调度器。
//
// TransferJob 是持久化父任务；递归目录会被拆成仅驻内存的扫描、建目录、复制文件和
// 收尾工作项。concurrency 表示同时传输的文件数，而不是父任务数。多个父任务轮转
// 领取空闲槽位，单个大目录也可利用全部并发。远程端点应由连接池 resolver 以 RAII
// 租约形式提供，避免多个工作线程共享同一个 libssh2 会话。
class TransferManager {
public:
    static constexpr std::size_t kMinConcurrency = 1;
    static constexpr std::size_t kDefaultConcurrency = 3;
    static constexpr std::size_t kMaxConcurrency = 8;
    static constexpr std::size_t kMinBufferBytes = 1U * 1024U * 1024U;
    static constexpr std::size_t kMediumBufferBytes = 2U * 1024U * 1024U;
    static constexpr std::size_t kMaxBufferBytes = 4U * 1024U * 1024U;
    // 传入 0 启用按文件大小选择 1/2/4 MB 缓冲；每个活动文件独占自己的有界缓冲。
    static constexpr std::size_t kAdaptiveBufferBytes = 0;
    static constexpr auto kProgressNotificationInterval = std::chrono::milliseconds(100);

    [[nodiscard]] static constexpr std::size_t
    adaptive_buffer_size(std::uint64_t bytes_total) noexcept {
        constexpr std::uint64_t kSmallJobLimit = 16ULL * 1024ULL * 1024ULL;
        constexpr std::uint64_t kMediumJobLimit = 256ULL * 1024ULL * 1024ULL;
        if (bytes_total <= kSmallJobLimit) {
            return kMinBufferBytes;
        }
        if (bytes_total <= kMediumJobLimit) {
            return kMediumBufferBytes;
        }
        return kMaxBufferBytes;
    }

    explicit TransferManager(EndpointResolver resolver,
                             std::size_t concurrency = kDefaultConcurrency,
                             std::size_t buffer_bytes = kAdaptiveBufferBytes);
    explicit TransferManager(CancellableEndpointResolver resolver,
                             std::size_t concurrency = kDefaultConcurrency,
                             std::size_t buffer_bytes = kAdaptiveBufferBytes);
    ~TransferManager();

    TransferManager(const TransferManager&) = delete;
    TransferManager& operator=(const TransferManager&) = delete;

    [[nodiscard]] TransferId enqueue(TransferSpec spec);
    // 用原 id 恢复持久化的暂停/失败父任务。暂停任务保持暂停，失败任务等待 retry；
    // next_id 同步前移，保证新任务不会复用历史 id。
    [[nodiscard]] TransferId restore(TransferJob snapshot);
    void pause(TransferId id);
    void resume(TransferId id);
    void cancel(TransferId id);
    void retry(TransferId id);
    // 只清除已完成、失败和取消记录，不影响排队或活动任务。移除前会通过 observer
    // 发布最终状态，使数据库中的恢复记录能同步删除。
    [[nodiscard]] std::size_t clear_terminal();

    // observer 可从非析构线程更换。状态变化立即通知，字节进度按固定间隔节流；
    // 回调按变更顺序且在释放管理器互斥锁后执行，因此可以安全调用 get/jobs。
    // 注意：回调可能运行在工作线程或调用线程，UI 必须自行投递回主线程。
    void set_observer(TransferObserver observer);

    // 停止全部工作线程，并把未完成且未取消的任务转为 Paused。之后仍可查询，但不能
    // 再入队或恢复。禁止从 observer 内调用 stop()，否则工作线程可能等待自身退出。
    void stop();

    void set_concurrency(std::size_t concurrency);
    [[nodiscard]] std::size_t concurrency() const noexcept;
    [[nodiscard]] std::optional<TransferJob> get(TransferId id) const;
    [[nodiscard]] std::vector<TransferJob> jobs() const;
    [[nodiscard]] TransferDetailSnapshot details(TransferId id,
                                                 std::uint64_t after_revision = 0) const;
    [[nodiscard]] bool wait(TransferId id, std::chrono::milliseconds timeout) const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace SLink::transfer
