// 文件级并行传输引擎：目录扫描作为生产者，文件工作线程作为消费者；提供背压、公平
// 父任务调度、暂停/取消、断点续传、冲突策略和父子进度汇总。
#include "transfer/transfer.hpp"

#include "core/perf_trace.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace SLink::transfer {
namespace {

bool terminal_state(const TransferState state) {
    return state == TransferState::Completed || state == TransferState::Failed ||
           state == TransferState::Cancelled;
}

char folded_path_character(const char value) noexcept {
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
}

bool same_path_prefix(std::string_view left,
                      std::string_view right,
                      const bool case_insensitive) noexcept {
    if (left.size() > right.size())
        return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        const char lhs = case_insensitive ? folded_path_character(left[index]) : left[index];
        const char rhs = case_insensitive ? folded_path_character(right[index]) : right[index];
        if (lhs != rhs)
            return false;
    }
    return true;
}

bool same_endpoint(const FileUri& left, const FileUri& right) noexcept {
    if (left.scheme() != right.scheme())
        return false;
    return left.is_local() || left.authority() == right.authority();
}

bool same_path(const FileUri& left, const FileUri& right) noexcept {
    return same_endpoint(left, right) && left.path().size() == right.path().size() &&
           same_path_prefix(left.path(), right.path(), left.is_local());
}

bool target_is_descendant(const FileUri& source, const FileUri& target) noexcept {
    if (!same_endpoint(source, target) || target.path().size() <= source.path().size() ||
        !same_path_prefix(source.path(), target.path(), source.is_local())) {
        return false;
    }
    return source.path().back() == '/' || target.path()[source.path().size()] == '/';
}

void validate_transfer_paths(const TransferSpec& spec) {
    if (same_path(spec.source, spec.target)) {
        throw std::invalid_argument("Source and target paths are identical");
    }
    if (target_is_descendant(spec.source, spec.target)) {
        throw std::invalid_argument(
            "Cannot transfer a directory into itself or one of its subdirectories");
    }
}

class Sha256 {
public:
    void update(std::span<const std::byte> bytes) {
        for (const auto value : bytes) {
            block_[block_size_++] = std::to_integer<std::uint8_t>(value);
            ++byte_count_;
            if (block_size_ == block_.size()) {
                transform();
                block_size_ = 0;
            }
        }
    }

    std::array<std::uint8_t, 32> finish() {
        const auto bit_count = byte_count_ * 8;
        block_[block_size_++] = 0x80;
        if (block_size_ > 56) {
            while (block_size_ < 64)
                block_[block_size_++] = 0;
            transform();
            block_size_ = 0;
        }
        while (block_size_ < 56)
            block_[block_size_++] = 0;
        for (int shift = 56; shift >= 0; shift -= 8) {
            block_[block_size_++] = static_cast<std::uint8_t>(bit_count >> shift);
        }
        transform();

        std::array<std::uint8_t, 32> result{};
        for (std::size_t index = 0; index < state_.size(); ++index) {
            result[index * 4] = static_cast<std::uint8_t>(state_[index] >> 24);
            result[index * 4 + 1] = static_cast<std::uint8_t>(state_[index] >> 16);
            result[index * 4 + 2] = static_cast<std::uint8_t>(state_[index] >> 8);
            result[index * 4 + 3] = static_cast<std::uint8_t>(state_[index]);
        }
        return result;
    }

private:
    void transform() {
        static constexpr std::array<std::uint32_t, 64> constants{
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
            0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
            0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
            0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
            0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
            0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
            0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
            0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
            0xc67178f2};
        std::array<std::uint32_t, 64> words{};
        for (std::size_t index = 0; index < 16; ++index) {
            words[index] = (static_cast<std::uint32_t>(block_[index * 4]) << 24) |
                           (static_cast<std::uint32_t>(block_[index * 4 + 1]) << 16) |
                           (static_cast<std::uint32_t>(block_[index * 4 + 2]) << 8) |
                           block_[index * 4 + 3];
        }
        for (std::size_t index = 16; index < 64; ++index) {
            const auto s0 = std::rotr(words[index - 15], 7) ^ std::rotr(words[index - 15], 18) ^
                            (words[index - 15] >> 3);
            const auto s1 = std::rotr(words[index - 2], 17) ^ std::rotr(words[index - 2], 19) ^
                            (words[index - 2] >> 10);
            words[index] = words[index - 16] + s0 + words[index - 7] + s1;
        }
        auto [a, b, c, d, e, f, g, h] = state_;
        for (std::size_t index = 0; index < 64; ++index) {
            const auto s1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
            const auto choice = (e & f) ^ (~e & g);
            const auto temp1 = h + s1 + choice + constants[index] + words[index];
            const auto s0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
            const auto majority = (a & b) ^ (a & c) ^ (b & c);
            const auto temp2 = s0 + majority;
            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }
        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    std::array<std::uint32_t, 8> state_{0x6a09e667,
                                        0xbb67ae85,
                                        0x3c6ef372,
                                        0xa54ff53a,
                                        0x510e527f,
                                        0x9b05688c,
                                        0x1f83d9ab,
                                        0x5be0cd19};
    std::array<std::uint8_t, 64> block_{};
    std::size_t block_size_{};
    std::uint64_t byte_count_{};
};

template <typename Checkpoint>
std::array<std::uint8_t, 32> digest(IFileEndpoint& endpoint,
                                    const FileUri& uri,
                                    const std::size_t buffer_size,
                                    Checkpoint checkpoint) {
    Sha256 hash;
    auto stream = endpoint.open_read(uri);
    std::vector<std::byte> buffer(buffer_size);
    for (;;) {
        checkpoint();
        const auto count = stream->read(buffer);
        if (count == 0)
            break;
        if (count > buffer.size()) {
            throw std::runtime_error("Endpoint returned an invalid digest read length");
        }
        hash.update(std::span<const std::byte>(buffer.data(), count));
    }
    return hash.finish();
}

FileUri auto_rename(IFileEndpoint& endpoint, const FileUri& target) {
    const auto name = target.filename();
    const auto dot = name.find_last_of('.');
    const auto base = dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
    const auto extension = dot == std::string::npos || dot == 0 ? std::string{} : name.substr(dot);
    for (std::size_t index = 1; index < 100000; ++index) {
        auto candidate =
            target.parent().joined(base + " (" + std::to_string(index) + ")" + extension);
        if (!endpoint.exists(candidate))
            return candidate;
    }
    throw std::runtime_error("Cannot find a free auto-rename target");
}

} // namespace

class TransferManager::Impl {
public:
    static constexpr std::size_t kMaximumReadyItems = 2048;
    static constexpr std::size_t kMaximumDetailChanges = 4096;
    // 详情树只服务界面展开，不参与传输正确性。大型目录超过上限后仍继续扫描和传输，
    // 但不再为每个后续子项永久保存两份 URI 和状态字符串。
    static constexpr std::size_t kMaximumRetainedDetails = 8192;
    static constexpr std::size_t kMaximumSameServerCopies = 2;
    static constexpr std::size_t kNoDetail = (std::numeric_limits<std::size_t>::max)();

    struct FileWork {
        FileUri source;
        FileUri target;
        FileStat metadata;
        std::size_t detail{kNoDetail};
        std::size_t aggregate_detail{kNoDetail};
        bool root{};
    };

    struct DirectoryFinalize {
        FileUri target;
        std::uint32_t permissions{};
        std::size_t detail{kNoDetail};
    };

    struct Control {
        struct DetailChange {
            std::uint64_t revision{};
            std::size_t index{};
        };

        TransferJob job;
        bool pause_requested{};
        bool cancel_requested{};
        bool restored{};
        bool discovery_started{};
        bool discovery_active{};
        bool discovery_complete{};
        bool finalization_active{};
        bool finalization_complete{};
        bool root_skipped{};
        std::size_t active_files{};
        std::string fatal_error;
        std::shared_ptr<std::atomic_bool> cancellation_flag{
            std::make_shared<std::atomic_bool>(false)};
        std::chrono::steady_clock::time_point last_progress_notification{};
        std::chrono::steady_clock::time_point last_discovery_notification{};
        std::deque<FileWork> ready_files;
        std::vector<DirectoryFinalize> directories;
        std::vector<TransferItemProgress> details;
        std::deque<DetailChange> detail_changes;
        std::uint64_t detail_revision{};
        std::uint64_t detail_reset_revision{};
        std::uint64_t oldest_incremental_revision{};
    };

    Impl(CancellableEndpointResolver endpoint_resolver,
         const std::size_t requested_concurrency,
         const std::size_t requested_buffer)
        : resolver(std::move(endpoint_resolver)), concurrency_limit(requested_concurrency),
          configured_buffer_size(
              requested_buffer == kAdaptiveBufferBytes
                  ? kAdaptiveBufferBytes
                  : std::clamp(requested_buffer, kMinBufferBytes, kMaxBufferBytes)) {
        if (!resolver) {
            throw std::invalid_argument("TransferManager requires an endpoint resolver");
        }
        if (requested_concurrency < kMinConcurrency || requested_concurrency > kMaxConcurrency) {
            throw std::out_of_range("Transfer concurrency must be between 1 and 8");
        }
    }

    ~Impl() {
        try {
            Stop();
        } catch (...) {
            std::terminate();
        }
    }

    bool active_locked(const Control& control) const noexcept {
        return control.discovery_active || control.finalization_active || control.active_files != 0;
    }

    std::shared_ptr<Control> find_locked(const TransferId id) const {
        const auto found = controls.find(id);
        if (found == controls.end()) {
            throw std::out_of_range("Unknown transfer id");
        }
        return found->second;
    }

    void add_job_order_locked(const TransferId id) {
        if (std::ranges::find(job_order, id) == job_order.end()) {
            job_order.push_back(id);
        }
    }

    void ensure_threads_locked() {
        if (!discovery_thread.joinable()) {
            discovery_thread = std::thread([this] {
                discovery_loop();
            });
        }
        while (workers.size() < concurrency_limit) {
            workers.emplace_back([this] {
                worker_loop();
            });
        }
    }

    void queue_notification_locked(const TransferJob& snapshot) {
        if (observer)
            notifications.push_back(snapshot);
    }

    void dispatch_notifications() noexcept {
        {
            std::scoped_lock lock(mutex);
            if (dispatching_notifications || notifications.empty())
                return;
            dispatching_notifications = true;
        }
        for (;;) {
            TransferJob snapshot;
            TransferObserver callback;
            {
                std::scoped_lock lock(mutex);
                if (notifications.empty()) {
                    dispatching_notifications = false;
                    return;
                }
                snapshot = std::move(notifications.front());
                notifications.pop_front();
                callback = observer;
            }
            if (!callback)
                continue;
            try {
                callback(std::move(snapshot));
            } catch (...) {}
        }
    }

    bool set_state_locked(const std::shared_ptr<Control>& control, const TransferState state) {
        if (control->job.state == state)
            return false;
        control->job.state = state;
        queue_notification_locked(control->job);
        return true;
    }

    void mark_detail_changed_locked(const std::shared_ptr<Control>& control,
                                    const std::size_t index) {
        if (control->detail_revision == (std::numeric_limits<std::uint64_t>::max)()) {
            control->detail_revision = 0;
            control->detail_changes.clear();
            control->oldest_incremental_revision = 0;
            control->detail_reset_revision = 1;
        }
        ++control->detail_revision;
        if (!control->detail_changes.empty() && control->detail_changes.back().index == index) {
            control->detail_changes.back().revision = control->detail_revision;
        } else {
            control->detail_changes.push_back({control->detail_revision, index});
        }
        while (control->detail_changes.size() > kMaximumDetailChanges) {
            control->oldest_incremental_revision = control->detail_changes.front().revision;
            control->detail_changes.pop_front();
        }
    }

    void reset_details_locked(const std::shared_ptr<Control>& control) {
        control->ready_files.clear();
        control->directories.clear();
        control->details.clear();
        control->detail_changes.clear();
        control->discovery_started = false;
        control->discovery_active = false;
        control->discovery_complete = false;
        control->finalization_active = false;
        control->finalization_complete = false;
        control->root_skipped = false;
        control->active_files = 0;
        control->fatal_error.clear();
        control->job.source_is_directory = false;
        control->job.item_count = 0;
        control->job.file_count = 0;
        control->job.completed_file_count = 0;
        control->job.discovery_complete = false;
        if (control->detail_revision == (std::numeric_limits<std::uint64_t>::max)()) {
            control->detail_revision = 0;
        }
        ++control->detail_revision;
        control->detail_reset_revision = control->detail_revision;
        control->oldest_incremental_revision = 0;
    }

    std::size_t append_detail(const std::shared_ptr<Control>& control,
                              const FileUri& source,
                              const FileUri& target,
                              const TransferItemId parent_id,
                              const bool directory) {
        std::scoped_lock lock(mutex);
        ++control->job.item_count;
        if (!directory)
            ++control->job.file_count;
        if (control->details.size() >= kMaximumRetainedDetails && parent_id != 0) {
            return kNoDetail;
        }
        TransferItemProgress item;
        item.id = static_cast<TransferItemId>(control->details.size()) + 1;
        item.parent_id = parent_id;
        item.source = source;
        item.target = target;
        item.directory = directory;
        control->details.push_back(std::move(item));
        const std::size_t index = control->details.size() - 1;
        if (parent_id == 0)
            control->job.source_is_directory = directory;
        mark_detail_changed_locked(control, index);
        return index;
    }

    void set_detail_state_locked(const std::shared_ptr<Control>& control,
                                 const std::size_t index,
                                 const TransferState state,
                                 std::string error = {}) {
        if (index == kNoDetail) {
            if (state == TransferState::Completed) {
                ++control->job.completed_file_count;
            }
            return;
        }
        if (index >= control->details.size())
            return;
        auto& item = control->details[index];
        if (!item.directory && item.state == TransferState::Completed &&
            state != TransferState::Completed && control->job.completed_file_count != 0) {
            --control->job.completed_file_count;
        } else if (!item.directory && item.state != TransferState::Completed &&
                   state == TransferState::Completed) {
            ++control->job.completed_file_count;
        }
        item.state = state;
        item.error = std::move(error);
        mark_detail_changed_locked(control, index);
    }

    void set_detail_state(const std::shared_ptr<Control>& control,
                          const std::size_t index,
                          const TransferState state,
                          std::string error = {}) {
        std::scoped_lock lock(mutex);
        set_detail_state_locked(control, index, state, std::move(error));
    }

    void set_detail_target(const std::shared_ptr<Control>& control,
                           const std::size_t index,
                           const FileUri& target) {
        std::scoped_lock lock(mutex);
        if (index == kNoDetail || index >= control->details.size())
            return;
        control->details[index].target = target;
        mark_detail_changed_locked(control, index);
    }

    void add_total(const std::shared_ptr<Control>& control,
                   const std::size_t detail,
                   const std::uint64_t bytes) {
        bool notify{};
        {
            std::scoped_lock lock(mutex);
            if (bytes > (std::numeric_limits<std::uint64_t>::max)() - control->job.bytes_total) {
                throw std::overflow_error("Transfer size counter overflow");
            }
            control->job.bytes_total += bytes;
            std::size_t current = detail;
            while (current != kNoDetail && current < control->details.size()) {
                auto& item = control->details[current];
                if (bytes > (std::numeric_limits<std::uint64_t>::max)() - item.bytes_total) {
                    throw std::overflow_error("Transfer item size counter overflow");
                }
                item.bytes_total += bytes;
                mark_detail_changed_locked(control, current);
                if (item.parent_id == 0)
                    break;
                current = static_cast<std::size_t>(item.parent_id - 1);
                if (current >= control->details.size())
                    break;
            }
            const auto now = std::chrono::steady_clock::now();
            if (control->last_discovery_notification == std::chrono::steady_clock::time_point{} ||
                now - control->last_discovery_notification >= kProgressNotificationInterval) {
                control->last_discovery_notification = now;
                queue_notification_locked(control->job);
                notify = true;
            }
        }
        if (notify) {
            changed.notify_all();
            dispatch_notifications();
        }
    }

    void add_progress(const std::shared_ptr<Control>& control,
                      const std::uint64_t bytes,
                      const std::size_t detail) {
        bool notify{};
#ifdef SLINK_ENABLE_PERF_TRACE
        std::uint64_t progress_lock_wait_us{};
#endif
        {
#ifdef SLINK_ENABLE_PERF_TRACE
            const auto lock_wait_started = std::chrono::steady_clock::now();
#endif
            std::scoped_lock lock(mutex);
#ifdef SLINK_ENABLE_PERF_TRACE
            progress_lock_wait_us =
                static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                               std::chrono::steady_clock::now() -
                                               lock_wait_started)
                                               .count());
#endif
            if (bytes >
                (std::numeric_limits<std::uint64_t>::max)() - control->job.bytes_transferred) {
                throw std::overflow_error("Transfer progress counter overflow");
            }
            control->job.bytes_transferred += bytes;
            std::size_t current = detail;
            while (current != kNoDetail && current < control->details.size()) {
                auto& item = control->details[current];
                if (bytes > (std::numeric_limits<std::uint64_t>::max)() - item.bytes_transferred) {
                    throw std::overflow_error("Transfer item progress counter overflow");
                }
                item.bytes_transferred += bytes;
                mark_detail_changed_locked(control, current);
                if (item.parent_id == 0)
                    break;
                current = static_cast<std::size_t>(item.parent_id - 1);
                if (current >= control->details.size())
                    break;
            }
            const auto now = std::chrono::steady_clock::now();
            if (control->last_progress_notification == std::chrono::steady_clock::time_point{} ||
                now - control->last_progress_notification >= kProgressNotificationInterval) {
                control->last_progress_notification = now;
                queue_notification_locked(control->job);
                notify = true;
            }
            changed.notify_all();
        }
#ifdef SLINK_ENABLE_PERF_TRACE
        core::RecordPerfSample("transfer_progress_lock_wait", progress_lock_wait_us);
#endif
        if (notify)
            dispatch_notifications();
    }

    struct Cancelled {};
    struct Stopped {};
    struct Aborted {};

    void checkpoint(const std::shared_ptr<Control>& control,
                    const std::optional<std::size_t> detail = std::nullopt) {
        std::unique_lock lock(mutex);
        if (control->cancel_requested)
            throw Cancelled{};
        if (stopping)
            throw Stopped{};
        if (!control->fatal_error.empty())
            throw Aborted{};
        while (control->pause_requested && !control->cancel_requested && !stopping &&
               control->fatal_error.empty()) {
            (void)set_state_locked(control, TransferState::Paused);
            if (detail && *detail != kNoDetail && *detail < control->details.size()) {
                set_detail_state_locked(control, *detail, TransferState::Paused);
            }
            changed.notify_all();
            lock.unlock();
            dispatch_notifications();
            lock.lock();
            changed.wait(lock, [&] {
                return !control->pause_requested || control->cancel_requested || stopping ||
                       !control->fatal_error.empty();
            });
        }
        if (control->cancel_requested)
            throw Cancelled{};
        if (stopping)
            throw Stopped{};
        if (!control->fatal_error.empty())
            throw Aborted{};
        (void)set_state_locked(control, TransferState::Running);
        if (detail && *detail != kNoDetail && *detail < control->details.size() &&
            control->details[*detail].state == TransferState::Paused) {
            set_detail_state_locked(control, *detail, TransferState::Running);
        }
        lock.unlock();
        dispatch_notifications();
    }

    std::shared_ptr<IFileEndpoint> resolve_endpoint(const std::shared_ptr<Control>& control,
                                                    const FileUri& uri) {
        auto endpoint = resolver(uri, control->cancellation_flag);
        if (!endpoint || !endpoint->accepts(uri) || !endpoint->available()) {
            throw std::runtime_error(uri.is_local() ? "Local endpoint is unavailable"
                                                    : "Remote endpoint is unavailable");
        }
        return endpoint;
    }

    static void restore_permissions_best_effort(IFileEndpoint& endpoint,
                                                const FileUri& target,
                                                const std::uint32_t permissions) noexcept {
        const auto portable_permissions = permissions & 07777U;
        if (portable_permissions == 0)
            return;
        try {
            endpoint.set_permissions(target, portable_permissions);
        } catch (...) {}
    }

    void wait_for_capacity(const std::shared_ptr<Control>& control) {
        // 扫描线程最多提前生产 kMaximumReadyItems 个文件工作项。达到上限后在此背压，
        // 避免十万文件目录一次性建立全部路径和详情对象而放大内存占用。
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] {
            return stopping || control->cancel_requested || !control->fatal_error.empty() ||
                   (control->ready_files.size() < kMaximumReadyItems && !control->pause_requested);
        });
        if (control->cancel_requested)
            throw Cancelled{};
        if (stopping)
            throw Stopped{};
        if (!control->fatal_error.empty())
            throw Aborted{};
    }

    void enqueue_file(const std::shared_ptr<Control>& control, FileWork work) {
        wait_for_capacity(control);
        {
            std::scoped_lock lock(mutex);
            control->ready_files.push_back(std::move(work));
        }
        changed.notify_all();
    }

    FileUri prepare_directory(const std::shared_ptr<Control>& control,
                              const FileUri& source,
                              FileUri target,
                              const bool root,
                              const TransferItemId parent_id,
                              const FileStat* metadata_hint = nullptr) {
        checkpoint(control);
        FileStat metadata;
        std::vector<DirectoryEntry> entries;
        {
            auto source_endpoint = resolve_endpoint(control, source);
            metadata = metadata_hint ? *metadata_hint : source_endpoint->stat(source);
            if (metadata.kind == FileKind::Directory) {
                entries = source_endpoint->list(source);
            }
        }
        if (metadata.kind != FileKind::Directory) {
            throw std::runtime_error("Expected a source directory");
        }
        const std::size_t detail = append_detail(control, source, target, parent_id, true);
        set_detail_state(control, detail, TransferState::Running);

        // 必须先取得源目录快照再创建目标目录。如果目标位于源目录内部，反过来的顺序
        // 会让新建目标被下一次枚举看见，最终形成无限递归复制。
        FileUri actual_target = target;
        {
            auto target_endpoint = resolve_endpoint(control, actual_target);
            if (target_endpoint->exists(actual_target)) {
                if (root && control->job.spec.conflict == ConflictPolicy::Skip) {
                    std::scoped_lock lock(mutex);
                    control->job.skipped = true;
                    control->root_skipped = true;
                    control->job.effective_target = actual_target;
                    set_detail_state_locked(control, detail, TransferState::Completed);
                    return actual_target;
                }
                if (root && control->job.spec.conflict == ConflictPolicy::AutoRename) {
                    actual_target = auto_rename(*target_endpoint, actual_target);
                    set_detail_target(control, detail, actual_target);
                    std::scoped_lock lock(mutex);
                    control->job.effective_target = actual_target;
                    target_endpoint->mkdir(actual_target, true);
                } else if (target_endpoint->stat(actual_target).kind != FileKind::Directory) {
                    if (control->job.spec.conflict != ConflictPolicy::Overwrite) {
                        throw std::runtime_error("Directory target conflicts with a file");
                    }
                    target_endpoint->remove(actual_target, false);
                    target_endpoint->mkdir(actual_target, true);
                }
            } else {
                target_endpoint->mkdir(actual_target, true);
            }
        }

        {
            std::scoped_lock lock(mutex);
            // 目录权限恢复属于正确性数据，不能和仅供 UI 展示的详情树一起截断。
            control->directories.push_back({actual_target, metadata.permissions, detail});
        }

        if (root) {
            std::scoped_lock lock(mutex);
            if (control->job.skipped)
                return actual_target;
        }

        for (const auto& entry : entries) {
            checkpoint(control);
            const FileUri child_target = actual_target.joined(entry.name);
            const TransferItemId child_parent_id =
                detail == kNoDetail ? parent_id : static_cast<TransferItemId>(detail) + 1;
            if (entry.stat.kind == FileKind::Directory) {
                (void)prepare_directory(
                    control, entry.uri, child_target, false, child_parent_id, &entry.stat);
                continue;
            }
            if (entry.stat.kind == FileKind::Symlink) {
                // 链接目标可能位于源树之外、形成环，或者在另一端没有等价语义。
                // 传输任务只复制真实目录和普通文件；移动任务同时标记 skipped，
                // 使收尾阶段保留源树，避免把未复制的链接随源目录一起删除。
                std::scoped_lock lock(mutex);
                control->job.skipped = true;
                continue;
            }
            if (entry.stat.kind != FileKind::Regular) {
                throw std::runtime_error("Special files are not transferred");
            }
            const std::size_t child_detail =
                append_detail(control, entry.uri, child_target, child_parent_id, false);
            const std::size_t aggregate_detail =
                child_detail != kNoDetail
                    ? child_detail
                    : (child_parent_id == 0 ? kNoDetail
                                            : static_cast<std::size_t>(child_parent_id - 1));
            add_total(control, aggregate_detail, entry.stat.size);
            enqueue_file(
                control,
                {entry.uri, child_target, entry.stat, child_detail, aggregate_detail, false});
        }
        return actual_target;
    }

    void discover(const std::shared_ptr<Control>& control) {
        validate_transfer_paths(control->job.spec);
        auto source_endpoint = resolve_endpoint(control, control->job.spec.source);
        const FileStat root = source_endpoint->stat(control->job.spec.source);
        source_endpoint.reset();
        if (root.kind == FileKind::Regular) {
            const auto detail = append_detail(
                control, control->job.spec.source, control->job.spec.target, 0, false);
            add_total(control, detail, root.size);
            enqueue_file(
                control,
                {control->job.spec.source, control->job.spec.target, root, detail, detail, true});
            return;
        }
        if (root.kind == FileKind::Symlink) {
            // 用户直接提交符号链接时也按“忽略”完成，而不是把它解析为目标文件
            // 或作为不支持类型报错。
            std::scoped_lock lock(mutex);
            control->job.skipped = true;
            control->root_skipped = true;
            return;
        }
        if (root.kind != FileKind::Directory) {
            throw std::runtime_error("Only regular files and directories can be transferred");
        }
        (void)prepare_directory(
            control, control->job.spec.source, control->job.spec.target, true, 0, &root);
    }

    void copy_file(const std::shared_ptr<Control>& control,
                   IFileEndpoint& source_endpoint,
                   IFileEndpoint& target_endpoint,
                   const FileWork& work) {
        FileUri target = work.target;
        std::uint64_t offset = 0;
        if (control->job.spec.conflict != ConflictPolicy::Overwrite &&
            target_endpoint.exists(target)) {
            switch (control->job.spec.conflict) {
            case ConflictPolicy::Skip: {
                add_progress(control, work.metadata.size, work.aggregate_detail);
                std::scoped_lock lock(mutex);
                if (work.detail != kNoDetail && work.detail < control->details.size()) {
                    auto& detail = control->details[work.detail];
                    detail.skipped = true;
                    mark_detail_changed_locked(control, work.detail);
                }
                if (work.root)
                    control->job.skipped = true;
                return;
            }
            case ConflictPolicy::AutoRename:
                target = auto_rename(target_endpoint, target);
                set_detail_target(control, work.detail, target);
                if (work.root) {
                    std::scoped_lock lock(mutex);
                    control->job.effective_target = target;
                }
                break;
            case ConflictPolicy::Resume: {
                const auto current = target_endpoint.stat(target);
                if (current.kind != FileKind::Regular || current.size > work.metadata.size) {
                    throw std::runtime_error("Resume target is not a compatible file");
                }
                offset = current.size;
                add_progress(control, offset, work.aggregate_detail);
                break;
            }
            case ConflictPolicy::Overwrite:
                break;
            }
        }

        checkpoint(control, work.detail);
        const auto task_buffer_size =
            configured_buffer_size == kAdaptiveBufferBytes
                ? TransferManager::adaptive_buffer_size(work.metadata.size)
                : configured_buffer_size;
        {
            auto input = source_endpoint.open_read(work.source, offset);
            auto output = target_endpoint.open_write(target, offset, offset == 0);
            output->set_cancellation_flag(control->cancellation_flag);
            std::vector<std::byte> buffer(task_buffer_size);
            for (;;) {
                checkpoint(control, work.detail);
                const auto count = input->read(buffer);
                if (count == 0)
                    break;
                if (count > buffer.size()) {
                    throw std::runtime_error("Source endpoint returned an invalid read length");
                }
                output->write(std::span<const std::byte>(buffer.data(), count));
                add_progress(control, count, work.aggregate_detail);
            }
            output->flush();
        }

        checkpoint(control, work.detail);
        const auto target_stat = target_endpoint.stat(target);
        if (target_stat.kind != FileKind::Regular || target_stat.size != work.metadata.size) {
            throw std::runtime_error("Target size verification failed");
        }
        if (control->job.spec.verify_sha256 &&
            digest(source_endpoint,
                   work.source,
                   task_buffer_size,
                   [this, &control, detail = work.detail] {
                       checkpoint(control, detail);
                   }) != digest(target_endpoint,
                                target,
                                task_buffer_size,
                                [this, &control, detail = work.detail] {
                                    checkpoint(control, detail);
                                })) {
            throw std::runtime_error("SHA-256 verification failed");
        }
        checkpoint(control, work.detail);
        restore_permissions_best_effort(target_endpoint, target, work.metadata.permissions);
    }

    void finalize(const std::shared_ptr<Control>& control) {
        if (!control->directories.empty()) {
            auto target_endpoint = resolve_endpoint(control, control->job.effective_target);
            for (auto iterator = control->directories.rbegin();
                 iterator != control->directories.rend();
                 ++iterator) {
                checkpoint(control, iterator->detail);
                restore_permissions_best_effort(
                    *target_endpoint, iterator->target, iterator->permissions);
                set_detail_state(control, iterator->detail, TransferState::Completed);
            }
        }
        if (control->job.spec.move && !control->job.skipped) {
            checkpoint(control);
            auto source_endpoint = resolve_endpoint(control, control->job.spec.source);
            source_endpoint->remove(control->job.spec.source, true);
        }
    }

    void fail_job_locked(const std::shared_ptr<Control>& control, std::string error) {
        if (control->fatal_error.empty()) {
            control->fatal_error = std::move(error);
            control->job.error = control->fatal_error;
            control->cancellation_flag->store(true, std::memory_order_release);
            for (const auto& work : control->ready_files) {
                set_detail_state_locked(
                    control, work.detail, TransferState::Failed, control->fatal_error);
            }
            control->ready_files.clear();
        }
    }

    void finish_if_possible_locked(const std::shared_ptr<Control>& control) {
        if (active_locked(*control))
            return;
        if (control->cancel_requested) {
            for (std::size_t index = 0; index < control->details.size(); ++index) {
                if (!terminal_state(control->details[index].state)) {
                    set_detail_state_locked(control, index, TransferState::Cancelled);
                }
            }
            (void)set_state_locked(control, TransferState::Cancelled);
            return;
        }
        if (stopping) {
            (void)set_state_locked(control, TransferState::Paused);
            return;
        }
        if (!control->fatal_error.empty()) {
            (void)set_state_locked(control, TransferState::Failed);
            return;
        }
        if (control->discovery_complete && control->finalization_complete) {
            control->job.buffer_bytes =
                configured_buffer_size == kAdaptiveBufferBytes
                    ? TransferManager::adaptive_buffer_size(control->job.bytes_total)
                    : configured_buffer_size;
            (void)set_state_locked(control, TransferState::Completed);
        }
    }

    void discovery_loop() {
        // 单独的发现线程一次只扫描一个父任务。扫描过程中发现普通文件便立即入队，
        // 文件工作线程可同步开始复制；ready 队列上限通过 wait_for_capacity 提供背压。
        for (;;) {
            std::shared_ptr<Control> control;
            {
                std::unique_lock lock(mutex);
                changed.wait(lock, [this] {
                    if (stopping)
                        return true;
                    return std::ranges::any_of(job_order, [this](const TransferId id) {
                        const auto found = controls.find(id);
                        if (found == controls.end())
                            return false;
                        const auto& item = *found->second;
                        return !terminal_state(item.job.state) && !item.discovery_started &&
                               !item.pause_requested && !item.cancel_requested;
                    });
                });
                if (stopping)
                    return;
                for (const auto id : job_order) {
                    const auto found = controls.find(id);
                    if (found == controls.end())
                        continue;
                    auto candidate = found->second;
                    if (terminal_state(candidate->job.state) || candidate->discovery_started ||
                        candidate->pause_requested || candidate->cancel_requested) {
                        continue;
                    }
                    control = std::move(candidate);
                    control->discovery_started = true;
                    control->discovery_active = true;
                    (void)set_state_locked(control, TransferState::Running);
                    break;
                }
            }
            dispatch_notifications();

            try {
                discover(control);
            } catch (const Cancelled&) {
            } catch (const Stopped&) {
            } catch (const Aborted&) {
            } catch (const std::exception& error) {
                std::scoped_lock lock(mutex);
                fail_job_locked(control, error.what());
            } catch (...) {
                std::scoped_lock lock(mutex);
                fail_job_locked(control, "Unknown directory discovery error");
            }
            {
                std::scoped_lock lock(mutex);
                control->discovery_active = false;
                control->discovery_complete = true;
                control->job.discovery_complete = true;
                if (control->root_skipped && control->ready_files.empty()) {
                    control->finalization_complete = true;
                }
                finish_if_possible_locked(control);
            }
            changed.notify_all();
            dispatch_notifications();
        }
    }

    enum class WorkKind {
        None,
        File,
        Finalize
    };

    bool can_start_file_locked(const FileWork& file) const {
        if (!file.source.is_sftp() || !file.target.is_sftp() ||
            file.source.authority() != file.target.authority()) {
            return true;
        }
        const auto found = active_same_server_copies.find(file.source.authority());
        return found == active_same_server_copies.end() || found->second < kMaximumSameServerCopies;
    }

    bool has_worker_work_locked() const {
        if (stopping)
            return true;
        for (const auto id : job_order) {
            const auto found = controls.find(id);
            if (found == controls.end())
                continue;
            const auto& control = *found->second;
            if (terminal_state(control.job.state) || control.pause_requested ||
                control.cancel_requested || !control.fatal_error.empty()) {
                continue;
            }
            if (active_file_workers < concurrency_limit && !control.ready_files.empty() &&
                can_start_file_locked(control.ready_files.front())) {
                return true;
            }
            if (control.discovery_complete && control.ready_files.empty() &&
                control.active_files == 0 && !control.finalization_active &&
                !control.finalization_complete) {
                return true;
            }
        }
        return false;
    }

    bool select_work_locked(std::shared_ptr<Control>& control, FileWork& file, WorkKind& kind) {
        // 从上次位置开始轮转父任务。文件工作受全局 concurrency_limit 限制，而目录
        // finalization 不占文件并发槽；这既避免大目录饿死小任务，也允许单任务吃满并发。
        if (job_order.empty())
            return false;
        for (std::size_t offset = 0; offset < job_order.size(); ++offset) {
            const std::size_t index = (round_robin_index + offset) % job_order.size();
            const auto found = controls.find(job_order[index]);
            if (found == controls.end())
                continue;
            auto candidate = found->second;
            if (terminal_state(candidate->job.state) || candidate->pause_requested ||
                candidate->cancel_requested || !candidate->fatal_error.empty()) {
                continue;
            }
            if (active_file_workers < concurrency_limit && !candidate->ready_files.empty()) {
                const auto& next = candidate->ready_files.front();
                const std::string same_server =
                    next.source.is_sftp() && next.target.is_sftp() &&
                            next.source.authority() == next.target.authority()
                        ? next.source.authority()
                        : std::string{};
                if (!can_start_file_locked(next)) {
                    continue;
                }
                control = std::move(candidate);
                file = std::move(control->ready_files.front());
                control->ready_files.pop_front();
                ++control->active_files;
                ++active_file_workers;
                if (!same_server.empty()) {
                    ++active_same_server_copies[same_server];
                }
                set_detail_state_locked(control, file.detail, TransferState::Running);
                kind = WorkKind::File;
                round_robin_index = (index + 1) % job_order.size();
                return true;
            }
            if (candidate->discovery_complete && candidate->ready_files.empty() &&
                candidate->active_files == 0 && !candidate->finalization_active &&
                !candidate->finalization_complete) {
                control = std::move(candidate);
                control->finalization_active = true;
                kind = WorkKind::Finalize;
                round_robin_index = (index + 1) % job_order.size();
                return true;
            }
        }
        return false;
    }

    void worker_loop() {
        // 工作线程只在锁内领取/更新状态，实际端点获取和文件 I/O 全部在锁外执行。
        // 完成后再汇总父任务与详情状态，observer 也始终在释放管理器锁后调用。
        for (;;) {
            std::shared_ptr<Control> control;
            FileWork file;
            WorkKind kind{WorkKind::None};
            {
                std::unique_lock lock(mutex);
                changed.wait(lock, [this] {
                    return has_worker_work_locked();
                });
                if (stopping)
                    return;
                if (!select_work_locked(control, file, kind))
                    continue;
            }
            changed.notify_all();
            dispatch_notifications();

            std::string failure;
            bool cancelled{};
            bool stopped{};
            try {
                if (kind == WorkKind::File) {
                    core::PerfScope timing("transfer_file_work");
                    std::shared_ptr<IFileEndpoint> source_endpoint;
                    std::shared_ptr<IFileEndpoint> target_endpoint;
                    // 远程到远程按 server-id 固定顺序获取两端连接。相反方向任务因此不会
                    // 各占满一端连接池后互相等待，避免连接租约死锁。
                    if (file.source.is_sftp() && file.target.is_sftp() &&
                        file.target.authority() < file.source.authority()) {
                        target_endpoint = resolve_endpoint(control, file.target);
                        source_endpoint = resolve_endpoint(control, file.source);
                    } else {
                        source_endpoint = resolve_endpoint(control, file.source);
                        target_endpoint = resolve_endpoint(control, file.target);
                    }
                    copy_file(control, *source_endpoint, *target_endpoint, file);
                } else {
                    finalize(control);
                }
            } catch (const Cancelled&) {
                cancelled = true;
            } catch (const Stopped&) {
                stopped = true;
            } catch (const Aborted&) {
            } catch (const std::exception& error) {
                failure = error.what();
            } catch (...) {
                failure = "Unknown transfer error";
            }

            {
                std::scoped_lock lock(mutex);
                if (kind == WorkKind::File) {
                    --control->active_files;
                    --active_file_workers;
                    if (file.source.is_sftp() && file.target.is_sftp() &&
                        file.source.authority() == file.target.authority()) {
                        const auto found = active_same_server_copies.find(file.source.authority());
                        if (found != active_same_server_copies.end() && --found->second == 0) {
                            active_same_server_copies.erase(found);
                        }
                    }
                    if (failure.empty() && !cancelled && !stopped && control->fatal_error.empty()) {
                        set_detail_state_locked(control, file.detail, TransferState::Completed);
                    } else if (cancelled || control->cancel_requested) {
                        set_detail_state_locked(control, file.detail, TransferState::Cancelled);
                    } else if (stopped || stopping) {
                        set_detail_state_locked(control, file.detail, TransferState::Paused);
                    } else if (!failure.empty()) {
                        set_detail_state_locked(
                            control, file.detail, TransferState::Failed, failure);
                    }
                } else {
                    control->finalization_active = false;
                    if (failure.empty() && !cancelled && !stopped && control->fatal_error.empty()) {
                        control->finalization_complete = true;
                    }
                }
                if (!failure.empty()) {
                    fail_job_locked(control, std::move(failure));
                }
                finish_if_possible_locked(control);
            }
            changed.notify_all();
            dispatch_notifications();
        }
    }

    void SetObserver(TransferObserver callback) {
        std::scoped_lock lock(mutex);
        observer = std::move(callback);
        notifications.clear();
    }

    void Stop() {
        const auto caller = std::this_thread::get_id();
        {
            std::scoped_lock lock(mutex);
            if ((discovery_thread.joinable() && discovery_thread.get_id() == caller) ||
                std::ranges::any_of(workers, [caller](const std::thread& worker) {
                    return worker.joinable() && worker.get_id() == caller;
                })) {
                throw std::logic_error(
                    "TransferManager::stop cannot be called from its worker thread");
            }
        }
        std::call_once(stop_once, [this] {
            {
                std::scoped_lock lock(mutex);
                stopping = true;
                for (auto& [id, control] : controls) {
                    (void)id;
                    if (terminal_state(control->job.state))
                        continue;
                    control->pause_requested = true;
                    control->cancellation_flag->store(true, std::memory_order_release);
                    (void)set_state_locked(control, TransferState::Paused);
                }
            }
            changed.notify_all();
            dispatch_notifications();
            if (discovery_thread.joinable())
                discovery_thread.join();
            for (auto& worker : workers) {
                if (worker.joinable())
                    worker.join();
            }
            {
                std::scoped_lock lock(mutex);
                for (auto& [id, control] : controls) {
                    (void)id;
                    if (!terminal_state(control->job.state)) {
                        (void)set_state_locked(control, TransferState::Paused);
                    }
                }
            }
            dispatch_notifications();
        });
    }

    CancellableEndpointResolver resolver;
    std::size_t concurrency_limit;
    std::size_t configured_buffer_size;
    mutable std::mutex mutex;
    mutable std::condition_variable changed;
    bool stopping{};
    std::once_flag stop_once;
    std::size_t active_file_workers{};
    std::unordered_map<std::string, std::size_t> active_same_server_copies;
    TransferId next_id{1};
    std::unordered_map<TransferId, std::shared_ptr<Control>> controls;
    std::vector<TransferId> job_order;
    std::size_t round_robin_index{};
    std::thread discovery_thread;
    std::vector<std::thread> workers;
    TransferObserver observer;
    std::deque<TransferJob> notifications;
    bool dispatching_notifications{};
};

TransferManager::TransferManager(EndpointResolver resolver,
                                 const std::size_t concurrency,
                                 const std::size_t buffer_bytes)
    : TransferManager(
          [resolver = std::move(resolver)](const FileUri& uri,
                                           std::shared_ptr<const std::atomic_bool>) {
              return resolver(uri);
          },
          concurrency,
          buffer_bytes) {}

TransferManager::TransferManager(CancellableEndpointResolver resolver,
                                 const std::size_t concurrency,
                                 const std::size_t buffer_bytes)
    : impl_(std::make_unique<Impl>(std::move(resolver), concurrency, buffer_bytes)) {}

TransferManager::~TransferManager() = default;

TransferId TransferManager::enqueue(TransferSpec spec) {
    validate_transfer_paths(spec);
    TransferId id{};
    {
        std::scoped_lock lock(impl_->mutex);
        if (impl_->stopping) {
            throw std::logic_error("Cannot enqueue after TransferManager::stop");
        }
        if (impl_->next_id == 0) {
            throw std::overflow_error("Transfer id space is exhausted");
        }
        id = impl_->next_id;
        impl_->next_id = id == (std::numeric_limits<TransferId>::max)() ? 0 : id + 1;
        auto control = std::make_shared<Impl::Control>();
        control->job.id = id;
        control->job.effective_target = spec.target;
        control->job.spec = std::move(spec);
        impl_->controls.emplace(id, control);
        impl_->add_job_order_locked(id);
        impl_->ensure_threads_locked();
        impl_->queue_notification_locked(control->job);
    }
    impl_->changed.notify_all();
    impl_->dispatch_notifications();
    return id;
}

TransferId TransferManager::restore(TransferJob snapshot) {
    if (snapshot.id == 0) {
        throw std::invalid_argument("A restored transfer requires a non-zero id");
    }
    if (snapshot.state != TransferState::Paused && snapshot.state != TransferState::Failed) {
        throw std::invalid_argument("Only paused or failed transfers can be restored");
    }
    if (snapshot.bytes_total != 0 && snapshot.bytes_transferred > snapshot.bytes_total) {
        throw std::invalid_argument("Restored transfer progress exceeds its total size");
    }
    if (snapshot.effective_target == FileUri{}) {
        snapshot.effective_target = snapshot.spec.target;
    }
    validate_transfer_paths(snapshot.spec);
    const auto id = snapshot.id;
    {
        std::scoped_lock lock(impl_->mutex);
        if (impl_->stopping) {
            throw std::logic_error("Cannot restore after TransferManager::stop");
        }
        if (impl_->controls.contains(id)) {
            throw std::invalid_argument("A transfer with this persistent id already exists");
        }
        auto control = std::make_shared<Impl::Control>();
        control->job = std::move(snapshot);
        control->job.id = id;
        control->restored = true;
        control->pause_requested = control->job.state == TransferState::Paused;
        impl_->controls.emplace(id, control);
        impl_->add_job_order_locked(id);
        if (impl_->next_id != 0 && id >= impl_->next_id) {
            impl_->next_id = id == (std::numeric_limits<TransferId>::max)() ? 0 : id + 1;
        }
        impl_->queue_notification_locked(control->job);
    }
    impl_->changed.notify_all();
    impl_->dispatch_notifications();
    return id;
}

void TransferManager::pause(const TransferId id) {
    {
        std::scoped_lock lock(impl_->mutex);
        if (impl_->stopping)
            return;
        const auto control = impl_->find_locked(id);
        if (terminal_state(control->job.state))
            return;
        control->pause_requested = true;
        (void)impl_->set_state_locked(control, TransferState::Paused);
    }
    impl_->changed.notify_all();
    impl_->dispatch_notifications();
}

void TransferManager::resume(const TransferId id) {
    {
        std::scoped_lock lock(impl_->mutex);
        if (impl_->stopping) {
            throw std::logic_error("Cannot resume after TransferManager::stop");
        }
        const auto control = impl_->find_locked(id);
        if (terminal_state(control->job.state))
            return;
        if (control->restored) {
            control->job.bytes_transferred = 0;
            control->job.bytes_total = 0;
            control->job.buffer_bytes = 0;
            control->job.skipped = false;
            control->job.error.clear();
            control->job.effective_target = control->job.spec.target;
            control->last_progress_notification = {};
            control->last_discovery_notification = {};
            control->restored = false;
            impl_->reset_details_locked(control);
        }
        control->pause_requested = false;
        (void)impl_->set_state_locked(
            control, control->discovery_started ? TransferState::Running : TransferState::Queued);
        impl_->ensure_threads_locked();
    }
    impl_->changed.notify_all();
    impl_->dispatch_notifications();
}

void TransferManager::cancel(const TransferId id) {
    {
        std::scoped_lock lock(impl_->mutex);
        if (impl_->stopping)
            return;
        const auto control = impl_->find_locked(id);
        if (terminal_state(control->job.state))
            return;
        control->cancel_requested = true;
        control->cancellation_flag->store(true, std::memory_order_release);
        for (const auto& work : control->ready_files) {
            impl_->set_detail_state_locked(control, work.detail, TransferState::Cancelled);
        }
        control->ready_files.clear();
        impl_->finish_if_possible_locked(control);
    }
    impl_->changed.notify_all();
    impl_->dispatch_notifications();
}

void TransferManager::retry(const TransferId id) {
    {
        std::scoped_lock lock(impl_->mutex);
        if (impl_->stopping) {
            throw std::logic_error("Cannot retry after TransferManager::stop");
        }
        const auto control = impl_->find_locked(id);
        if (control->job.state != TransferState::Failed &&
            control->job.state != TransferState::Cancelled) {
            throw std::logic_error("Only failed or cancelled transfers can be retried");
        }
        control->pause_requested = false;
        control->cancel_requested = false;
        control->restored = false;
        control->cancellation_flag = std::make_shared<std::atomic_bool>(false);
        control->job.bytes_transferred = 0;
        control->job.bytes_total = 0;
        control->job.buffer_bytes = 0;
        control->job.skipped = false;
        control->job.error.clear();
        control->job.effective_target = control->job.spec.target;
        control->last_progress_notification = {};
        control->last_discovery_notification = {};
        impl_->reset_details_locked(control);
        (void)impl_->set_state_locked(control, TransferState::Queued);
        impl_->ensure_threads_locked();
    }
    impl_->changed.notify_all();
    impl_->dispatch_notifications();
}

std::size_t TransferManager::clear_terminal() {
    std::size_t removed{};
    {
        std::scoped_lock lock(impl_->mutex);
        for (auto item = impl_->controls.begin(); item != impl_->controls.end();) {
            const auto& control = item->second;
            if (!terminal_state(control->job.state) || impl_->active_locked(*control)) {
                ++item;
                continue;
            }
            if (control->job.state == TransferState::Failed) {
                (void)impl_->set_state_locked(control, TransferState::Cancelled);
            }
            item = impl_->controls.erase(item);
            ++removed;
        }
        std::erase_if(impl_->job_order, [this](const TransferId id) {
            return !impl_->controls.contains(id);
        });
        if (!impl_->job_order.empty()) {
            impl_->round_robin_index %= impl_->job_order.size();
        } else {
            impl_->round_robin_index = 0;
        }
    }
    if (removed != 0) {
        impl_->changed.notify_all();
        impl_->dispatch_notifications();
    }
    return removed;
}

void TransferManager::set_observer(TransferObserver observer) {
    impl_->SetObserver(std::move(observer));
}

void TransferManager::stop() {
    impl_->Stop();
}

void TransferManager::set_concurrency(const std::size_t concurrency) {
    if (concurrency < kMinConcurrency || concurrency > kMaxConcurrency) {
        throw std::out_of_range("Transfer concurrency must be between 1 and 8");
    }
    {
        std::scoped_lock lock(impl_->mutex);
        if (impl_->stopping) {
            throw std::logic_error("Cannot change concurrency after TransferManager::stop");
        }
        impl_->concurrency_limit = concurrency;
        if (!impl_->controls.empty())
            impl_->ensure_threads_locked();
    }
    impl_->changed.notify_all();
}

std::size_t TransferManager::concurrency() const noexcept {
    std::scoped_lock lock(impl_->mutex);
    return impl_->concurrency_limit;
}

std::optional<TransferJob> TransferManager::get(const TransferId id) const {
    std::scoped_lock lock(impl_->mutex);
    const auto found = impl_->controls.find(id);
    if (found == impl_->controls.end())
        return std::nullopt;
    return found->second->job;
}

std::vector<TransferJob> TransferManager::jobs() const {
    std::scoped_lock lock(impl_->mutex);
    std::vector<TransferJob> result;
    result.reserve(impl_->controls.size());
    for (const auto& [id, control] : impl_->controls) {
        (void)id;
        result.push_back(control->job);
    }
    std::ranges::sort(result, {}, &TransferJob::id);
    return result;
}

TransferDetailSnapshot TransferManager::details(const TransferId id,
                                                const std::uint64_t after_revision) const {
    std::scoped_lock lock(impl_->mutex);
    const auto found = impl_->controls.find(id);
    if (found == impl_->controls.end()) {
        throw std::out_of_range("Unknown transfer id");
    }
    const auto& control = found->second;
    TransferDetailSnapshot result;
    result.revision = control->detail_revision;
    if (after_revision == result.revision)
        return result;
    const bool requires_reset = after_revision == 0 ||
                                after_revision < control->detail_reset_revision ||
                                after_revision < control->oldest_incremental_revision;
    if (requires_reset) {
        result.reset = true;
        result.items = control->details;
        return result;
    }
    std::unordered_set<std::size_t> emitted;
    for (const auto& change : control->detail_changes) {
        if (change.revision <= after_revision || !emitted.insert(change.index).second ||
            change.index >= control->details.size()) {
            continue;
        }
        result.items.push_back(control->details[change.index]);
    }
    return result;
}

bool TransferManager::wait(const TransferId id, const std::chrono::milliseconds timeout) const {
    std::unique_lock lock(impl_->mutex);
    const auto found = impl_->controls.find(id);
    if (found == impl_->controls.end())
        return false;
    return impl_->changed.wait_for(lock, timeout, [&] {
        return terminal_state(found->second->job.state);
    });
}

} // namespace SLink::transfer
