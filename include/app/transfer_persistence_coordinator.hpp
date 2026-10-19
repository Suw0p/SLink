#pragma once

#include "storage/application_repository.h"
#include "transfer/transfer.hpp"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace SLink::app {

// 传输工作线程只投递父任务快照；专用线程合并同一任务的更新并串行写入 Repository。
// 必须在 TransferManager 停止后调用 Stop，确保最后的暂停/失败状态也写入数据库。
class TransferPersistenceCoordinator {
public:
    explicit TransferPersistenceCoordinator(storage::ApplicationRepository& repository);
    ~TransferPersistenceCoordinator();

    TransferPersistenceCoordinator(const TransferPersistenceCoordinator&) = delete;
    TransferPersistenceCoordinator& operator=(const TransferPersistenceCoordinator&) = delete;

    void Start();
    void Queue(transfer::TransferJob snapshot);
    void Stop();

private:
    struct RecoveryStamp {
        transfer::TransferState state{transfer::TransferState::Queued};
        std::uint64_t bytes_total{};
        std::string effective_target_uri;
        std::chrono::steady_clock::time_point persisted_at{};
    };

    void Run();
    void Persist(const transfer::TransferJob& job);

    storage::ApplicationRepository& repository_;
    std::mutex queue_mutex_;
    std::condition_variable queue_changed_;
    std::unordered_map<transfer::TransferId, transfer::TransferJob> pending_;
    std::thread worker_;
    bool stopping_{};
    std::unordered_map<transfer::TransferId, RecoveryStamp> recovery_stamps_;
};

} // namespace SLink::app
