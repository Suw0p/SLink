#include "app/transfer_persistence_coordinator.hpp"

#include <windows.h>

#include <utility>

namespace SLink::app {
namespace {

storage::TransferRecoveryConflictPolicy ToRecoveryConflict(const transfer::ConflictPolicy value) {
    switch (value) {
    case transfer::ConflictPolicy::Overwrite:
        return storage::TransferRecoveryConflictPolicy::Overwrite;
    case transfer::ConflictPolicy::Skip:
        return storage::TransferRecoveryConflictPolicy::Skip;
    case transfer::ConflictPolicy::AutoRename:
        return storage::TransferRecoveryConflictPolicy::AutoRename;
    case transfer::ConflictPolicy::Resume:
        return storage::TransferRecoveryConflictPolicy::Resume;
    }
    return storage::TransferRecoveryConflictPolicy::Overwrite;
}

} // namespace

TransferPersistenceCoordinator::TransferPersistenceCoordinator(
    storage::ApplicationRepository& repository)
    : repository_(repository) {}

TransferPersistenceCoordinator::~TransferPersistenceCoordinator() {
    Stop();
}

void TransferPersistenceCoordinator::Start() {
    std::scoped_lock lock(queue_mutex_);
    if (worker_.joinable()) {
        return;
    }
    stopping_ = false;
    worker_ = std::thread([this] {
        Run();
    });
}

void TransferPersistenceCoordinator::Queue(transfer::TransferJob snapshot) {
    {
        std::scoped_lock lock(queue_mutex_);
        if (stopping_) {
            return;
        }
        pending_.insert_or_assign(snapshot.id, std::move(snapshot));
    }
    queue_changed_.notify_one();
}

void TransferPersistenceCoordinator::Stop() {
    {
        std::scoped_lock lock(queue_mutex_);
        if (!worker_.joinable()) {
            return;
        }
        stopping_ = true;
    }
    queue_changed_.notify_one();
    worker_.join();
}

void TransferPersistenceCoordinator::Run() {
    for (;;) {
        std::unordered_map<transfer::TransferId, transfer::TransferJob> batch;
        {
            std::unique_lock lock(queue_mutex_);
            queue_changed_.wait(lock, [this] {
                return stopping_ || !pending_.empty();
            });
            batch.swap(pending_);
            if (batch.empty() && stopping_) {
                return;
            }
        }
        for (const auto& [id, snapshot] : batch) {
            (void)id;
            Persist(snapshot);
        }
    }
}

void TransferPersistenceCoordinator::Persist(const transfer::TransferJob& job) {
    if (job.id == 0) {
        return;
    }
    if (job.state == transfer::TransferState::Completed ||
        job.state == transfer::TransferState::Cancelled) {
        recovery_stamps_.erase(job.id);
        const auto removed = repository_.RemoveTransferRecovery(job.id);
        if (!removed) {
            OutputDebugStringW(
                (L"Unable to remove transfer recovery record: " + removed.message + L"\r\n")
                    .c_str());
        }
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const std::string effective_target = job.effective_target.to_string();
    auto& stamp = recovery_stamps_[job.id];
    const bool state_changed =
        stamp.persisted_at == std::chrono::steady_clock::time_point{} || stamp.state != job.state;
    const bool metadata_changed =
        stamp.bytes_total != job.bytes_total || stamp.effective_target_uri != effective_target;
    if (!state_changed && !metadata_changed && now - stamp.persisted_at < std::chrono::seconds(1)) {
        return;
    }
    stamp = {job.state, job.bytes_total, effective_target, now};

    storage::TransferRecoveryState persisted_state = storage::TransferRecoveryState::Queued;
    if (job.state == transfer::TransferState::Paused) {
        persisted_state = storage::TransferRecoveryState::Paused;
    } else if (job.state == transfer::TransferState::Failed) {
        persisted_state = storage::TransferRecoveryState::Failed;
    }

    const storage::TransferRecoveryRecord record{job.id,
                                                 job.spec.source.to_string(),
                                                 job.spec.target.to_string(),
                                                 effective_target,
                                                 job.bytes_transferred,
                                                 job.bytes_total,
                                                 persisted_state,
                                                 ToRecoveryConflict(job.spec.conflict),
                                                 job.spec.move,
                                                 job.spec.verify_sha256,
                                                 job.error};
    const auto saved = repository_.UpsertTransferRecovery(record);
    if (!saved) {
        OutputDebugStringW(
            (L"Unable to persist transfer recovery record: " + saved.message + L"\r\n").c_str());
    }
}

} // namespace SLink::app
