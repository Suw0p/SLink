// IFileEndpoint 的 Reactor 封送门面实现。同步工作线程调用会转换为 Reactor 任务，
// 返回的读写流也保持相同线程亲和性和关闭语义。
#include "ssh/reactor_endpoint.hpp"

#include <condition_variable>
#include <exception>
#include <mutex>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>

namespace SLink::ssh {
namespace {

template <typename Value> class SyncState final {
public:
    template <typename Candidate> void complete(Candidate&& value) {
        std::scoped_lock lock(mutex_);
        if (ready_)
            return;
        value_.emplace(std::forward<Candidate>(value));
        ready_ = true;
        changed_.notify_all();
    }

    void fail(std::exception_ptr error) noexcept {
        try {
            std::scoped_lock lock(mutex_);
            if (ready_)
                return;
            error_ = std::move(error);
            ready_ = true;
            changed_.notify_all();
        } catch (...) {
            // std::mutex failures are unrecoverable in practice. Keeping this
            // path noexcept is essential because fail() is also used by a
            // task-lifetime destructor.
        }
    }

    Value wait() {
        std::unique_lock lock(mutex_);
        changed_.wait(lock, [this] {
            return ready_;
        });
        if (error_)
            std::rethrow_exception(error_);
        return std::move(*value_);
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::optional<Value> value_;
    std::exception_ptr error_;
    bool ready_{};
};

template <> class SyncState<void> final {
public:
    void complete() {
        std::scoped_lock lock(mutex_);
        if (ready_)
            return;
        ready_ = true;
        changed_.notify_all();
    }

    void fail(std::exception_ptr error) noexcept {
        try {
            std::scoped_lock lock(mutex_);
            if (ready_)
                return;
            error_ = std::move(error);
            ready_ = true;
            changed_.notify_all();
        } catch (...) {}
    }

    void wait() {
        std::unique_lock lock(mutex_);
        changed_.wait(lock, [this] {
            return ready_;
        });
        if (error_)
            std::rethrow_exception(error_);
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::exception_ptr error_;
    bool ready_{};
};

template <typename Value> class TaskLifetime final {
public:
    explicit TaskLifetime(std::shared_ptr<SyncState<Value>> state)
        : state_(std::move(state)),
          cancellation_(std::make_exception_ptr(ReactorEndpointError(
              {ErrorCode::cancelled, L"Network reactor cancelled the endpoint operation."}))) {}

    ~TaskLifetime() {
        state_->fail(cancellation_);
    }

private:
    std::shared_ptr<SyncState<Value>> state_;
    std::exception_ptr cancellation_;
};

template <typename Function> using InvokeResult = std::invoke_result_t<Function&>;

template <typename Function>
InvokeResult<Function>
invoke_sync(NetworkReactor& reactor, const ReactorEndpointBinder* binder, Function function) {
    using Value = InvokeResult<Function>;
    static_assert(!std::is_reference_v<Value>);

    if (reactor.is_reactor_thread()) {
        if constexpr (std::is_void_v<Value>) {
            function();
            return;
        } else {
            return function();
        }
    }

    auto state = std::make_shared<SyncState<Value>>();
    auto lifetime = std::make_shared<TaskLifetime<Value>>(state);
    ReactorTask task = [state, lifetime, binder, function = std::move(function)](
                           const CancellationToken& cancellation) mutable {
        try {
            if (binder && *binder) {
                auto status = (*binder)(cancellation);
                if (!status) {
                    throw ReactorEndpointError(std::move(status));
                }
            }
            if constexpr (std::is_void_v<Value>) {
                function();
                state->complete();
            } else {
                state->complete(function());
            }
        } catch (...) {
            state->fail(std::current_exception());
        }
        return Status::ok();
    };

    try {
        const auto handle = reactor.submit(std::move(task));
        (void)handle;
        lifetime.reset();
    } catch (...) {
        lifetime.reset();
        throw;
    }

    if constexpr (std::is_void_v<Value>) {
        state->wait();
    } else {
        return state->wait();
    }
}

template <typename Slot> void release_slot(NetworkReactor& reactor, Slot*& slot) noexcept {
    auto* released = std::exchange(slot, nullptr);
    if (!released)
        return;
    try {
        invoke_sync(reactor, nullptr, [released] {
            delete released;
        });
    } catch (...) {
        // The lifetime contract requires proxies to be gone before shutdown.
        // If it is violated, leaking is preferable to invoking a libssh2-backed
        // destructor on an arbitrary worker thread.
    }
}

struct ReadSlot final {
    std::unique_ptr<transfer::IReadStream> stream;
    ReactorEndpointBinder binder;
};

class ReactorReadStream final : public transfer::IReadStream {
public:
    ReactorReadStream(NetworkReactor& reactor, ReadSlot* slot) : reactor_(reactor), slot_(slot) {}

    ~ReactorReadStream() override {
        release_slot(reactor_, slot_);
    }

    [[nodiscard]] std::size_t read(std::span<std::byte> output) override {
        return invoke_sync(reactor_, &slot_->binder, [this, output] {
            return slot_->stream->read(output);
        });
    }

private:
    NetworkReactor& reactor_;
    ReadSlot* slot_{};
};

struct WriteSlot final {
    std::unique_ptr<transfer::IWriteStream> stream;
    ReactorEndpointBinder binder;
};

class ReactorWriteStream final : public transfer::IWriteStream {
public:
    ReactorWriteStream(NetworkReactor& reactor, WriteSlot* slot) : reactor_(reactor), slot_(slot) {}

    ~ReactorWriteStream() override {
        release_slot(reactor_, slot_);
    }

    void set_cancellation_flag(std::shared_ptr<const std::atomic_bool> flag) override {
        invoke_sync(reactor_, &slot_->binder, [this, flag = std::move(flag)]() mutable {
            slot_->stream->set_cancellation_flag(std::move(flag));
        });
    }

    void write(std::span<const std::byte> input) override {
        invoke_sync(reactor_, &slot_->binder, [this, input] {
            slot_->stream->write(input);
        });
    }

    void flush() override {
        invoke_sync(reactor_, &slot_->binder, [this] {
            slot_->stream->flush();
        });
    }

private:
    NetworkReactor& reactor_;
    WriteSlot* slot_{};
};

} // namespace

ReactorEndpointError::ReactorEndpointError(Status status)
    : std::runtime_error("Network reactor endpoint operation failed"), status_(std::move(status)) {}

class ReactorEndpoint::Impl final {
public:
    struct EndpointSlot final {
        std::shared_ptr<transfer::IFileEndpoint> endpoint;
        ReactorEndpointBinder binder;
    };

    Impl(NetworkReactor& owner,
         std::shared_ptr<transfer::IFileEndpoint> endpoint,
         ReactorEndpointBinder binder)
        : reactor(owner), slot(new EndpointSlot{std::move(endpoint), std::move(binder)}) {}

    ~Impl() {
        release_slot(reactor, slot);
    }

    NetworkReactor& reactor;
    EndpointSlot* slot{};
};

ReactorEndpoint::ReactorEndpoint(NetworkReactor& reactor,
                                 std::shared_ptr<transfer::IFileEndpoint> endpoint,
                                 ReactorEndpointBinder binder) {
    if (!endpoint) {
        throw std::invalid_argument("ReactorEndpoint requires an endpoint");
    }
    impl_ = std::make_unique<Impl>(reactor, std::move(endpoint), std::move(binder));
}

ReactorEndpoint::~ReactorEndpoint() = default;

bool ReactorEndpoint::accepts(const transfer::FileUri& uri) const noexcept {
    try {
        return invoke_sync(impl_->reactor, &impl_->slot->binder, [this, uri] {
            return impl_->slot->endpoint->accepts(uri);
        });
    } catch (...) {
        return false;
    }
}

bool ReactorEndpoint::available() const noexcept {
    try {
        return invoke_sync(impl_->reactor, &impl_->slot->binder, [this] {
            return impl_->slot->endpoint->available();
        });
    } catch (...) {
        return false;
    }
}

std::vector<transfer::DirectoryEntry> ReactorEndpoint::list(const transfer::FileUri& directory) {
    return invoke_sync(impl_->reactor, &impl_->slot->binder, [this, directory] {
        return impl_->slot->endpoint->list(directory);
    });
}

transfer::FileStat ReactorEndpoint::stat(const transfer::FileUri& uri) {
    return invoke_sync(impl_->reactor, &impl_->slot->binder, [this, uri] {
        return impl_->slot->endpoint->stat(uri);
    });
}

bool ReactorEndpoint::exists(const transfer::FileUri& uri) {
    return invoke_sync(impl_->reactor, &impl_->slot->binder, [this, uri] {
        return impl_->slot->endpoint->exists(uri);
    });
}

std::unique_ptr<transfer::IReadStream> ReactorEndpoint::open_read(const transfer::FileUri& uri,
                                                                  const std::uint64_t offset) {
    auto* slot = invoke_sync(impl_->reactor, &impl_->slot->binder, [this, uri, offset] {
        return new ReadSlot{impl_->slot->endpoint->open_read(uri, offset), impl_->slot->binder};
    });
    try {
        return std::make_unique<ReactorReadStream>(impl_->reactor, slot);
    } catch (...) {
        release_slot(impl_->reactor, slot);
        throw;
    }
}

std::unique_ptr<transfer::IWriteStream> ReactorEndpoint::open_write(const transfer::FileUri& uri,
                                                                    const std::uint64_t offset,
                                                                    const bool truncate) {
    auto* slot = invoke_sync(impl_->reactor, &impl_->slot->binder, [this, uri, offset, truncate] {
        return new WriteSlot{impl_->slot->endpoint->open_write(uri, offset, truncate),
                             impl_->slot->binder};
    });
    try {
        return std::make_unique<ReactorWriteStream>(impl_->reactor, slot);
    } catch (...) {
        release_slot(impl_->reactor, slot);
        throw;
    }
}

void ReactorEndpoint::rename(const transfer::FileUri& from,
                             const transfer::FileUri& to,
                             const bool replace) {
    invoke_sync(impl_->reactor, &impl_->slot->binder, [this, from, to, replace] {
        impl_->slot->endpoint->rename(from, to, replace);
    });
}

void ReactorEndpoint::remove(const transfer::FileUri& uri, const bool recursive) {
    invoke_sync(impl_->reactor, &impl_->slot->binder, [this, uri, recursive] {
        impl_->slot->endpoint->remove(uri, recursive);
    });
}

void ReactorEndpoint::mkdir(const transfer::FileUri& uri, const bool recursive) {
    invoke_sync(impl_->reactor, &impl_->slot->binder, [this, uri, recursive] {
        impl_->slot->endpoint->mkdir(uri, recursive);
    });
}

void ReactorEndpoint::set_permissions(const transfer::FileUri& uri,
                                      const std::uint32_t permissions) {
    invoke_sync(impl_->reactor, &impl_->slot->binder, [this, uri, permissions] {
        impl_->slot->endpoint->set_permissions(uri, permissions);
    });
}

} // namespace SLink::ssh
