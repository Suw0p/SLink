// Windows Shell 外拖：本地使用 CF_HDROP，远程使用延迟呈现的虚拟文件流。
// COM 对象跨线程访问由 FTM 封送；所有端点操作集中到受控工作线程，再由端点
// 门面转投对应 Reactor。UI 等待时允许 COM/窗口消息调度，但不执行网络操作。
#include "ui/shell_file_drag.hpp"
#include "transfer/local_endpoint.hpp"

#include <algorithm>
#include <atomic>
#include <commctrl.h>
#include <condition_variable>
#include <cstring>
#include <cwctype>
#include <deque>
#include <limits>
#include <mutex>
#include <new>
#include <shellapi.h>
#include <shldisp.h>
#include <shlobj.h>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>
#include <wrl/client.h>

namespace SLink::ui::detail {
namespace {
using Microsoft::WRL::ComPtr;
using transfer::FileKind;
using transfer::FileUri;

UINT SelectionFormat() {
    return RegisterClipboardFormatW(L"SLink.FileSelection.v1");
}
UINT DescriptorFormat() {
    return RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW);
}
UINT ContentsFormat() {
    return RegisterClipboardFormatW(CFSTR_FILECONTENTS);
}
UINT PreferredFormat() {
    return RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
}
struct SelectionToken {
    DWORD process;
    std::uint64_t id;
};
std::atomic<std::uint64_t> next_export_id{};

std::wstring Wide(const std::string_view text) {
    const int count = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (count <= 0)
        throw std::runtime_error("Invalid UTF-8 file name");
    std::wstring result(count, L'\0');
    MultiByteToWideChar(CP_UTF8,
                        MB_ERR_INVALID_CHARS,
                        text.data(),
                        static_cast<int>(text.size()),
                        result.data(),
                        count);
    return result;
}
std::string Utf8(const std::wstring_view text) {
    const int count = WideCharToMultiByte(CP_UTF8,
                                          WC_ERR_INVALID_CHARS,
                                          text.data(),
                                          static_cast<int>(text.size()),
                                          nullptr,
                                          0,
                                          nullptr,
                                          nullptr);
    if (count <= 0)
        throw std::runtime_error("Invalid local path");
    std::string result(count, '\0');
    WideCharToMultiByte(CP_UTF8,
                        WC_ERR_INVALID_CHARS,
                        text.data(),
                        static_cast<int>(text.size()),
                        result.data(),
                        count,
                        nullptr,
                        nullptr);
    return result;
}
// 不允许远程名称逃逸目标目录；Windows 无法表示的名称明确报错，不悄悄覆盖或改名。
void ValidateComponent(const std::wstring& name) {
    if (name.empty() || name == L"." || name == L".." || name.back() == L'.' ||
        name.back() == L' ' || name.find_first_of(L"<>:\"/\\|?*") != std::wstring::npos ||
        std::any_of(name.begin(), name.end(), [](wchar_t c) {
            return c < 32;
        }))
        throw std::runtime_error("File name is not supported by Windows");
    auto base = name.substr(0, name.find(L'.'));
    std::transform(base.begin(), base.end(), base.begin(), towupper);
    if (base == L"CON" || base == L"PRN" || base == L"AUX" || base == L"NUL" ||
        (base.size() == 4 && (base.starts_with(L"COM") || base.starts_with(L"LPT")) &&
         base[3] >= L'1' && base[3] <= L'9'))
        throw std::runtime_error("Reserved Windows file name");
}
HRESULT GlobalData(const void* data, SIZE_T bytes, STGMEDIUM* output) {
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
    if (!memory)
        return E_OUTOFMEMORY;
    void* target = GlobalLock(memory);
    if (!target) {
        GlobalFree(memory);
        return E_OUTOFMEMORY;
    }
    std::memcpy(target, data, bytes);
    GlobalUnlock(memory);
    output->tymed = TYMED_HGLOBAL;
    output->hGlobal = memory;
    output->pUnkForRelease = nullptr;
    return S_OK;
}
class ByteBudget final {
public:
    explicit ByteBudget(std::size_t maximum) : maximum_(maximum) {}
    bool Reserve(std::size_t bytes) noexcept {
        auto used = used_.load();
        do {
            if (bytes > maximum_ || used > maximum_ - bytes)
                return false;
        } while (!used_.compare_exchange_weak(used, used + bytes));
        return true;
    }
    void Release(std::size_t bytes) noexcept {
        used_.fetch_sub(bytes);
    }
    std::size_t used() const noexcept {
        return used_.load();
    }

private:
    const std::size_t maximum_;
    std::atomic_size_t used_{};
};
struct ManifestMemory {
    std::shared_ptr<ByteBudget> budget;
    std::size_t bytes{};
    explicit ManifestMemory(std::shared_ptr<ByteBudget> value) : budget(std::move(value)) {}
    ~ManifestMemory() {
        budget->Release(bytes);
    }
    bool Reserve(std::size_t count) noexcept {
        if (count > ShellFileDrag::kMaximumManifestBytes ||
            bytes > ShellFileDrag::kMaximumManifestBytes - count || !budget->Reserve(count))
            return false;
        bytes += count;
        return true;
    }
};
struct Entry {
    FileUri uri;
    std::wstring relative;
    std::uint64_t size{};
    bool directory{};
    std::size_t MemoryBytes() const noexcept {
        // 同时为最终描述块、容器容量增长和排队节点预留预算，不能只数文件数量。
        return sizeof(Entry) * 2 + sizeof(FILEDESCRIPTORW) + 128 + uri.path().capacity() +
               uri.authority().capacity() + (relative.capacity() + 1) * sizeof(wchar_t);
    }
};
struct Reader {
    std::shared_ptr<transfer::IFileEndpoint> endpoint;
    std::unique_ptr<transfer::IReadStream> stream;
    std::uint64_t position{};
};
struct Export {
    ShellFileSelection selection;
    std::shared_ptr<std::atomic_bool> cancelled{std::make_shared<std::atomic_bool>(false)};
    std::vector<Entry> entries;
    std::vector<std::weak_ptr<Reader>> readers;
    bool listed{};
    HGLOBAL descriptors{};
    std::unique_ptr<ManifestMemory> manifest_memory;
    ~Export() {
        if (descriptors)
            GlobalFree(descriptors);
    }
};

void WaitWithComDispatch(HANDLE event) noexcept {
    APTTYPE apartment{};
    APTTYPEQUALIFIER qualifier{};
    if (SUCCEEDED(CoGetApartmentType(&apartment, &qualifier)) &&
        (apartment == APTTYPE_STA || apartment == APTTYPE_MAINSTA)) {
        // CoWait 的 COM 模态循环不保证分发普通窗口消息。UI STA 显式使用消息等待，
        // 同时保留退出请求，不能在后台仍访问连接池时提前退出主消息循环。
        bool quit{};
        WPARAM exit_code{};
        while (WaitForSingleObject(event, 0) != WAIT_OBJECT_0) {
            const auto result = MsgWaitForMultipleObjectsEx(
                1, &event, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE | MWMO_ALERTABLE);
            if (result == WAIT_OBJECT_0)
                break;
            if (result == WAIT_FAILED) {
                WaitForSingleObject(event, INFINITE);
                break;
            }
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_QUIT) {
                    quit = true;
                    exit_code = message.wParam;
                } else {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
                if (WaitForSingleObject(event, 0) == WAIT_OBJECT_0)
                    break;
            }
        }
        if (quit)
            PostQuitMessage(static_cast<int>(exit_code));
        return;
    }
    DWORD index{};
    const auto waited = CoWaitForMultipleHandles(
        COWAIT_DISPATCH_CALLS | COWAIT_DISPATCH_WINDOW_MESSAGES, INFINITE, 1, &event, &index);
    if (FAILED(waited))
        WaitForSingleObject(event, INFINITE);
}

class Runtime final {
public:
    Runtime() = default;
    ~Runtime() {
        Shutdown();
        if (worker_done_)
            CloseHandle(worker_done_);
    }
    void SetResolver(transfer::CancellableEndpointResolver resolver) {
        std::scoped_lock lock(mutex_);
        if (!stopping_)
            resolver_ = std::move(resolver);
    }
    std::shared_ptr<Export> NewExport(ShellFileSelection selection, std::uint64_t& id) {
        auto data = std::make_shared<Export>();
        data->selection = std::move(selection);
        std::scoped_lock lock(mutex_);
        if (stopping_)
            throw std::runtime_error("Drag service stopped");
        id = ++next_export_id;
        std::erase_if(exports_, [](const auto& pair) {
            return pair.second.expired();
        });
        exports_[id] = data;
        return data;
    }
    std::shared_ptr<Export> Find(std::uint64_t id) {
        std::scoped_lock lock(mutex_);
        const auto it = exports_.find(id);
        return it == exports_.end() ? nullptr : it->second.lock();
    }
    // 执行队列的生命周期不依赖 HWND。等待完成前，调用者始终持有任务数据。
    template <typename Function> HRESULT Invoke(Function function) noexcept {
        try {
            std::shared_ptr<Completion> completed;
            {
                std::scoped_lock lock(mutex_);
                if (stopping_)
                    return E_ABORT;
                if (!worker_.joinable()) {
                    if (!worker_done_)
                        worker_done_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                    if (!worker_done_)
                        return E_OUTOFMEMORY;
                    worker_ = std::thread([this] {
                        Work();
                    });
                    worker_started_ = true;
                }
                if (completions_.empty()) {
                    completed = std::make_shared<Completion>();
                    if (!completed->event)
                        return E_OUTOFMEMORY;
                    ++completion_events_;
                } else {
                    completed = std::move(completions_.back());
                    completions_.pop_back();
                    ResetEvent(completed->event);
                }
                ++work_requests_;
                tasks_.push_back([function = std::move(function), completed] {
                    try {
                        completed->result = function();
                    } catch (const std::bad_alloc&) {
                        completed->result = E_OUTOFMEMORY;
                    } catch (...) {
                        completed->result = STG_E_READFAULT;
                    }
                    SetEvent(completed->event);
                });
            }
            changed_.notify_one();
            WaitWithComDispatch(completed->event);
            const auto result = completed->result;
            {
                std::scoped_lock lock(mutex_);
                if (!stopping_ && completions_.size() < 8)
                    completions_.push_back(std::move(completed));
            }
            return result;
        } catch (const std::bad_alloc&) {
            return E_OUTOFMEMORY;
        } catch (...) {
            return E_FAIL;
        }
    }
    // Explorer 可能轮流读取多个未结束的 IStream。工作线程串行执行，切换流时
    // 归还旧租约并保留偏移，避免多个半读流占满池后互相等待。
    void ActivateReader(const std::shared_ptr<Reader>& reader) {
        if (auto old = active_reader_.lock(); old && old != reader) {
            old->stream.reset();
            old->endpoint.reset();
        }
        active_reader_ = reader;
    }
    std::shared_ptr<transfer::IFileEndpoint> Acquire(const FileUri& uri,
                                                     const std::shared_ptr<Export>& data) {
        if (data->cancelled->load())
            throw std::runtime_error("Drag cancelled");
        transfer::CancellableEndpointResolver resolver;
        {
            std::scoped_lock lock(mutex_);
            resolver = resolver_;
        }
        auto endpoint = uri.is_local() ? std::make_shared<transfer::LocalEndpoint>()
                                       : (resolver ? resolver(uri, data->cancelled) : nullptr);
        if (!endpoint || !endpoint->available())
            throw std::runtime_error("Endpoint unavailable");
        return endpoint;
    }
    HRESULT Manifest(const std::shared_ptr<Export>& data) {
        if (data->cancelled->load())
            return E_ABORT;
        if (data->listed)
            return S_OK;
        ActivateReader({});
        auto memory = std::make_unique<ManifestMemory>(manifest_budget_);
        if (!memory->Reserve(FIELD_OFFSET(FILEGROUPDESCRIPTORW, fgd)))
            return E_OUTOFMEMORY;
        std::deque<Entry> pending;
        for (const auto& uri : data->selection.sources) {
            auto endpoint = Acquire(uri, data);
            const auto stat = endpoint->stat(uri);
            if (stat.kind != FileKind::Regular && stat.kind != FileKind::Directory)
                continue;
            Entry entry{uri, Wide(uri.filename()), stat.size, stat.kind == FileKind::Directory};
            if (!memory->Reserve(entry.MemoryBytes()))
                return E_OUTOFMEMORY;
            pending.push_back(std::move(entry));
        }
        std::vector<Entry> entries;
        while (!pending.empty()) {
            if (data->cancelled->load())
                return E_ABORT;
            auto item = std::move(pending.front());
            pending.pop_front();
            ValidateComponent(item.relative.substr(item.relative.find_last_of(L'\\') + 1));
            if (item.relative.size() >= MAX_PATH)
                return HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
            if (item.directory) {
                auto endpoint = Acquire(item.uri, data);
                for (auto& child : endpoint->list(item.uri)) {
                    if (child.stat.kind != FileKind::Regular &&
                        child.stat.kind != FileKind::Directory)
                        continue;
                    if (child.name == "." || child.name == "..")
                        continue;
                    const auto name = Wide(child.name);
                    ValidateComponent(name);
                    Entry entry{item.uri.joined(child.name),
                                item.relative + L"\\" + name,
                                child.stat.size,
                                child.stat.kind == FileKind::Directory};
                    if (!memory->Reserve(entry.MemoryBytes()))
                        return E_OUTOFMEMORY;
                    pending.push_back(std::move(entry));
                }
            }
            entries.push_back(std::move(item));
        }
        if (entries.empty())
            return DV_E_FORMATETC;
        // 直接构建最终 HGLOBAL；同一只读描述块通过 pUnkForRelease 共享给 Shell，
        // 不再同时保留 vector 描述副本和 GlobalData 的第二份拷贝。
        const auto bytes =
            FIELD_OFFSET(FILEGROUPDESCRIPTORW, fgd) + entries.size() * sizeof(FILEDESCRIPTORW);
        const HGLOBAL block = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
        if (!block)
            return E_OUTOFMEMORY;
        auto* group = static_cast<FILEGROUPDESCRIPTORW*>(GlobalLock(block));
        if (!group) {
            GlobalFree(block);
            return E_OUTOFMEMORY;
        }
        group->cItems = static_cast<UINT>(entries.size());
        for (std::size_t i = 0; i < entries.size(); ++i) {
            auto& item = entries[i];
            auto& descriptor = group->fgd[i];
            descriptor.dwFlags = static_cast<DWORD>(FD_ATTRIBUTES | FD_UNICODE | FD_PROGRESSUI);
            descriptor.dwFileAttributes =
                item.directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
            if (!item.directory) {
                descriptor.dwFlags |= FD_FILESIZE;
                descriptor.nFileSizeHigh = static_cast<DWORD>(item.size >> 32);
                descriptor.nFileSizeLow = static_cast<DWORD>(item.size);
            }
            std::copy(item.relative.begin(), item.relative.end(), descriptor.cFileName);
            std::wstring{}.swap(item.relative);
            if (item.directory)
                item.uri = FileUri{};
        }
        GlobalUnlock(block);
        data->descriptors = block;
        data->manifest_memory = std::move(memory);
        data->entries = std::move(entries);
        data->listed = true;
        return S_OK;
    }
    void Shutdown() noexcept {
        {
            std::scoped_lock lock(mutex_);
            if (stopping_)
                return;
            stopping_ = true;
            for (const auto& [id, weak] : exports_)
                if (auto data = weak.lock())
                    data->cancelled->store(true);
        }
        changed_.notify_all();
        if (worker_.joinable()) {
            // 等待期间仍调度 COM/UI 消息；工作线程先清理流及租约，最后才发完成事件。
            WaitWithComDispatch(worker_done_);
            worker_.join();
        }
        std::scoped_lock lock(mutex_);
        worker_started_ = false;
        resolver_ = {};
        completions_.clear();
    }
    ShellFileDrag::Diagnostics Diagnostics() const noexcept {
        std::scoped_lock lock(mutex_);
        return {worker_started_,
                work_requests_,
                completion_events_,
                manifest_budget_->used(),
                read_ahead_budget_->used()};
    }
    std::shared_ptr<ByteBudget> ReadAheadBudget() const {
        return read_ahead_budget_;
    }

private:
    void ReleaseReaders() {
        std::scoped_lock lock(mutex_);
        for (const auto& [id, weak] : exports_)
            if (auto data = weak.lock())
                for (auto& reader : data->readers)
                    if (auto current = reader.lock()) {
                        current->stream.reset();
                        current->endpoint.reset();
                    }
    }
    void Work() {
        const auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock lock(mutex_);
                changed_.wait(lock, [this] {
                    return stopping_ || !tasks_.empty();
                });
                if (tasks_.empty() && stopping_)
                    break;
                task = std::move(tasks_.front());
                tasks_.pop_front();
            }
            task();
        }
        ReleaseReaders();
        if (SUCCEEDED(com))
            CoUninitialize();
        SetEvent(worker_done_);
    }
    struct Completion {
        HANDLE event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        HRESULT result{E_FAIL};
        ~Completion() {
            if (event)
                CloseHandle(event);
        }
    };
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<std::function<void()>> tasks_;
    std::unordered_map<std::uint64_t, std::weak_ptr<Export>> exports_;
    transfer::CancellableEndpointResolver resolver_;
    std::weak_ptr<Reader> active_reader_;
    const std::shared_ptr<ByteBudget> manifest_budget_{
        std::make_shared<ByteBudget>(ShellFileDrag::kMaximumTotalManifestBytes)};
    const std::shared_ptr<ByteBudget> read_ahead_budget_{
        std::make_shared<ByteBudget>(ShellFileDrag::kMaximumReadAheadBytes)};
    std::vector<std::shared_ptr<Completion>> completions_;
    std::uint64_t work_requests_{}, completion_events_{};
    HANDLE worker_done_{};
    bool stopping_{};
    bool worker_started_{};
    std::thread worker_;
};

class FileStream final : public IStream {
public:
    FileStream(std::shared_ptr<Runtime> runtime, std::shared_ptr<Export> data, std::size_t index)
        : runtime_(std::move(runtime)), data_(std::move(data)), index_(index),
          reader_(std::make_shared<Reader>()), cache_budget_(runtime_->ReadAheadBudget()),
          size_(data_->entries[index].size), name_(Wide(data_->entries[index].uri.filename())) {
        if (FAILED(CoCreateFreeThreadedMarshaler(this, &marshaler_)))
            throw std::runtime_error("Cannot marshal drag stream");
        data_->readers.push_back(reader_); // 仅在 Runtime 工作线程构造。
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override {
        if (!output)
            return E_POINTER;
        *output = nullptr;
        if (iid == IID_IUnknown || iid == IID_ISequentialStream || iid == IID_IStream) {
            *output = static_cast<IStream*>(this);
            AddRef();
            return S_OK;
        }
        return iid == IID_IMarshal && marshaler_ ? marshaler_->QueryInterface(iid, output)
                                                 : E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++refs_;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto n = --refs_;
        if (!n)
            delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE Read(void* buffer, ULONG bytes, ULONG* read) override {
        if (read)
            *read = 0;
        if (!buffer && bytes)
            return STG_E_INVALIDPOINTER;
        try {
            std::scoped_lock lock(cursor_mutex_);
            if (data_->cancelled->load()) {
                ReleaseCache();
                return E_ABORT;
            }
            ULONG total{};
            while (total < bytes && position_ < size_) {
                if (data_->cancelled->load()) {
                    ReleaseCache();
                    return E_ABORT;
                }
                if (cache_position_ < cache_size_) {
                    const auto count = static_cast<ULONG>(
                        std::min<std::size_t>(bytes - total, cache_size_ - cache_position_));
                    std::memcpy(static_cast<std::byte*>(buffer) + total,
                                cache_.data() + cache_position_,
                                count);
                    cache_position_ += count;
                    position_ += count;
                    total += count;
                    if (read)
                        *read = total;
                    continue;
                }
                const auto wanted =
                    static_cast<ULONG>(std::min<std::uint64_t>(bytes - total, size_ - position_));
                // 大块直接写入 Shell 缓冲；小块在有界预读缓存中消费，命中时不投递任务。
                if (wanted >= kReadAheadBytes || !EnsureCache()) {
                    ULONG got{};
                    const auto hr =
                        ReadRemote(static_cast<std::byte*>(buffer) + total, wanted, &got);
                    position_ += got;
                    total += got;
                    if (read)
                        *read = total;
                    if (FAILED(hr))
                        return hr;
                    if (!got)
                        return STG_E_READFAULT;
                } else {
                    ULONG got{};
                    const auto hr = ReadRemote(cache_.data(),
                                               static_cast<ULONG>(std::min<std::uint64_t>(
                                                   kReadAheadBytes, size_ - position_)),
                                               &got);
                    cache_position_ = 0;
                    cache_size_ = got;
                    if (FAILED(hr)) {
                        ReleaseCache();
                        return hr;
                    }
                    if (!got)
                        return STG_E_READFAULT;
                }
            }
            if (position_ >= size_)
                ReleaseCache();
            return total == bytes ? S_OK : S_FALSE;
        } catch (...) {
            return E_FAIL;
        }
    }
    HRESULT ReadRemote(void* buffer, ULONG bytes, ULONG* read) {
        return runtime_->Invoke([this, buffer, bytes, read] {
            if (data_->cancelled->load())
                return E_ABORT;
            runtime_->ActivateReader(reader_);
            const auto& entry = data_->entries[index_];
            ULONG total{};
            try {
                while (total < bytes && reader_->position < entry.size) {
                    if (data_->cancelled->load())
                        return E_ABORT;
                    if (!reader_->stream) {
                        reader_->endpoint = runtime_->Acquire(entry.uri, data_);
                        reader_->stream =
                            reader_->endpoint->open_read(entry.uri, reader_->position);
                    }
                    const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(
                        {bytes - total, entry.size - reader_->position, 1024U * 1024U}));
                    const auto got =
                        reader_->stream->read({static_cast<std::byte*>(buffer) + total, count});
                    if (!got || got > count)
                        throw std::runtime_error("File truncated during drag");
                    total += static_cast<ULONG>(got);
                    reader_->position += got;
                    if (read)
                        *read = total;
                }
                if (reader_->position >= entry.size) {
                    reader_->stream.reset();
                    reader_->endpoint.reset();
                }
                return total == bytes ? S_OK : S_FALSE;
            } catch (...) {
                reader_->stream.reset();
                reader_->endpoint.reset();
                return STG_E_READFAULT;
            }
        });
    }
    HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER offset,
                                   DWORD origin,
                                   ULARGE_INTEGER* result) override {
        std::scoped_lock lock(cursor_mutex_);
        if (data_->cancelled->load())
            return E_ABORT;
        std::uint64_t base{};
        if (origin == STREAM_SEEK_CUR)
            base = position_;
        else if (origin == STREAM_SEEK_END)
            base = size_;
        else if (origin != STREAM_SEEK_SET)
            return STG_E_INVALIDFUNCTION;
        if (offset.QuadPart < 0 && static_cast<ULONGLONG>(-(offset.QuadPart + 1)) + 1 > base)
            return STG_E_INVALIDFUNCTION;
        if (offset.QuadPart >= 0 && base > UINT64_MAX - offset.QuadPart)
            return STG_E_INVALIDFUNCTION;
        const auto next = offset.QuadPart < 0
                              ? base - (static_cast<ULONGLONG>(-(offset.QuadPart + 1)) + 1)
                              : base + offset.QuadPart;
        const auto hr = runtime_->Invoke([this, next] {
            if (data_->cancelled->load())
                return E_ABORT;
            reader_->position = next;
            reader_->stream.reset();
            reader_->endpoint.reset();
            return S_OK;
        });
        if (FAILED(hr))
            return hr;
        position_ = next;
        ReleaseCache();
        if (result)
            result->QuadPart = next;
        return hr;
    }
    HRESULT STDMETHODCALLTYPE Stat(STATSTG* stat, DWORD flags) override {
        if (!stat)
            return E_POINTER;
        *stat = {};
        try {
            if (data_->cancelled->load())
                return E_ABORT;
            stat->type = STGTY_STREAM;
            stat->cbSize.QuadPart = size_;
            stat->grfMode = STGM_READ;
            if (!(flags & STATFLAG_NONAME)) {
                const auto& name = name_;
                stat->pwcsName =
                    static_cast<wchar_t*>(CoTaskMemAlloc((name.size() + 1) * sizeof(wchar_t)));
                if (!stat->pwcsName)
                    return E_OUTOFMEMORY;
                std::memcpy(stat->pwcsName, name.c_str(), (name.size() + 1) * sizeof(wchar_t));
            }
            return S_OK;
        } catch (...) {
            return E_OUTOFMEMORY;
        }
    }
    HRESULT STDMETHODCALLTYPE CopyTo(IStream* target,
                                     ULARGE_INTEGER count,
                                     ULARGE_INTEGER* read,
                                     ULARGE_INTEGER* written) override {
        try {
            if (!target)
                return STG_E_INVALIDPOINTER;
            if (read)
                read->QuadPart = 0;
            if (written)
                written->QuadPart = 0;
            std::vector<std::byte> buffer(64 * 1024);
            ULONGLONG total{};
            while (total < count.QuadPart) {
                ULONG got{}, put{};
                const HRESULT hr = Read(
                    buffer.data(),
                    static_cast<ULONG>(std::min<ULONGLONG>(buffer.size(), count.QuadPart - total)),
                    &got);
                if (read)
                    read->QuadPart += got;
                if (FAILED(hr))
                    return hr;
                const HRESULT saved = target->Write(buffer.data(), got, &put);
                if (written)
                    written->QuadPart += put;
                if (FAILED(saved))
                    return saved;
                if (put != got)
                    return STG_E_MEDIUMFULL;
                total += got;
                if (hr == S_FALSE || !got)
                    return S_FALSE;
            }
            return S_OK;
        } catch (const std::bad_alloc&) {
            return E_OUTOFMEMORY;
        } catch (...) {
            return E_FAIL;
        }
    }
    HRESULT STDMETHODCALLTYPE Write(const void*, ULONG, ULONG*) override {
        return STG_E_ACCESSDENIED;
    }
    HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER) override {
        return STG_E_ACCESSDENIED;
    }
    HRESULT STDMETHODCALLTYPE Commit(DWORD) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Revert() override {
        return STG_E_INVALIDFUNCTION;
    }
    HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override {
        return STG_E_INVALIDFUNCTION;
    }
    HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override {
        return STG_E_INVALIDFUNCTION;
    }
    HRESULT STDMETHODCALLTYPE Clone(IStream**) override {
        return E_NOTIMPL;
    }

private:
    ~FileStream() {
        ReleaseCache();
        const auto reader = reader_;
        runtime_->Invoke([reader] {
            reader->stream.reset();
            reader->endpoint.reset();
            return S_OK;
        });
    }
    std::atomic<ULONG> refs_{1};
    ComPtr<IUnknown> marshaler_;
    std::shared_ptr<Runtime> runtime_;
    std::shared_ptr<Export> data_;
    std::size_t index_;
    std::shared_ptr<Reader> reader_;
    static constexpr std::size_t kReadAheadBytes = 256U * 1024U;
    std::shared_ptr<ByteBudget> cache_budget_;
    const std::uint64_t size_;
    const std::wstring name_;
    std::mutex cursor_mutex_;
    std::uint64_t position_{};
    std::vector<std::byte> cache_;
    std::size_t cache_position_{}, cache_size_{};
    bool EnsureCache() noexcept {
        if (!cache_.empty())
            return true;
        if (!cache_budget_->Reserve(kReadAheadBytes))
            return false;
        try {
            cache_.resize(kReadAheadBytes);
            return true;
        } catch (...) {
            cache_budget_->Release(kReadAheadBytes);
            return false;
        }
    }
    void ReleaseCache() noexcept {
        if (!cache_.empty()) {
            std::vector<std::byte>{}.swap(cache_);
            cache_budget_->Release(kReadAheadBytes);
        }
        cache_position_ = cache_size_ = 0;
    }
};

class FileDataObject final : public IDataObject, public IDataObjectAsyncCapability {
public:
    std::shared_ptr<std::atomic_bool> Cancellation() const {
        return data_->cancelled;
    }
    FileDataObject(std::shared_ptr<Runtime> runtime, std::shared_ptr<Export> data, std::uint64_t id)
        : runtime_(std::move(runtime)), data_(std::move(data)), id_(id) {
        if (FAILED(CoCreateFreeThreadedMarshaler(static_cast<IDataObject*>(this), &marshaler_)))
            throw std::runtime_error("Cannot marshal drag data");
        local_ = std::all_of(
            data_->selection.sources.begin(), data_->selection.sources.end(), [](const auto& uri) {
                return uri.is_local();
            });
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override {
        if (!output)
            return E_POINTER;
        *output = nullptr;
        if (iid == IID_IUnknown || iid == IID_IDataObject)
            *output = static_cast<IDataObject*>(this);
        else if (iid == __uuidof(IDataObjectAsyncCapability))
            *output = static_cast<IDataObjectAsyncCapability*>(this);
        else
            return iid == IID_IMarshal && marshaler_ ? marshaler_->QueryInterface(iid, output)
                                                     : E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++refs_;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto n = --refs_;
        if (!n)
            delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* format) override {
        if (!format)
            return E_POINTER;
        if (format->dwAspect != DVASPECT_CONTENT)
            return DV_E_DVASPECT;
        if (format->ptd)
            return DV_E_DVTARGETDEVICE;
        if (format->cfFormat == ContentsFormat() && !local_)
            return (format->tymed & TYMED_ISTREAM) ? (format->lindex >= 0 ? S_OK : DV_E_LINDEX)
                                                   : DV_E_TYMED;
        const bool known =
            format->cfFormat == SelectionFormat() || format->cfFormat == PreferredFormat() ||
            (local_ ? format->cfFormat == CF_HDROP : format->cfFormat == DescriptorFormat());
        if (known)
            return format->lindex != -1 ? DV_E_LINDEX
                                        : ((format->tymed & TYMED_HGLOBAL) ? S_OK : DV_E_TYMED);
        std::scoped_lock lock(extra_mutex_);
        return extra_.contains(format->cfFormat) && (format->tymed & TYMED_HGLOBAL)
                   ? S_OK
                   : DV_E_FORMATETC;
    }
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* format, STGMEDIUM* output) override {
        if (!output)
            return E_POINTER;
        *output = {};
        const HRESULT checked = QueryGetData(format);
        if (FAILED(checked))
            return checked;
        try {
            if (data_->cancelled->load())
                return E_ABORT;
            if (format->cfFormat == SelectionFormat()) {
                const SelectionToken token{GetCurrentProcessId(), id_};
                return GlobalData(&token, sizeof(token), output);
            }
            if (format->cfFormat == PreferredFormat()) {
                const DWORD effect = DROPEFFECT_COPY;
                return GlobalData(&effect, sizeof(effect), output);
            }
            if (format->cfFormat == CF_HDROP && local_) {
                std::wstring paths;
                for (const auto& uri : data_->selection.sources) {
                    auto path = Wide(uri.path());
                    std::replace(path.begin(), path.end(), L'/', L'\\');
                    paths += path;
                    paths += L'\0';
                }
                paths += L'\0';
                std::vector<std::byte> buffer(sizeof(DROPFILES) + paths.size() * sizeof(wchar_t));
                auto* drop = reinterpret_cast<DROPFILES*>(buffer.data());
                drop->pFiles = sizeof(DROPFILES);
                drop->fWide = TRUE;
                std::memcpy(buffer.data() + sizeof(DROPFILES),
                            paths.data(),
                            paths.size() * sizeof(wchar_t));
                return GlobalData(buffer.data(), buffer.size(), output);
            }
            if (format->cfFormat == DescriptorFormat() || format->cfFormat == ContentsFormat()) {
                return runtime_->Invoke([this, format = *format, output] {
                    const HRESULT listed = runtime_->Manifest(data_);
                    if (FAILED(listed))
                        return listed;
                    if (format.cfFormat == ContentsFormat()) {
                        if (static_cast<std::size_t>(format.lindex) >= data_->entries.size())
                            return DV_E_LINDEX;
                        if (data_->entries[format.lindex].directory)
                            return DV_E_LINDEX;
                        output->pstm = new FileStream(runtime_, data_, format.lindex);
                        output->tymed = TYMED_ISTREAM;
                        return S_OK;
                    }
                    output->tymed = TYMED_HGLOBAL;
                    output->hGlobal = data_->descriptors;
                    output->pUnkForRelease = static_cast<IDataObject*>(this);
                    AddRef();
                    return S_OK;
                });
            }
            std::scoped_lock lock(extra_mutex_);
            const auto& bytes = extra_.at(format->cfFormat);
            return GlobalData(bytes.data(), bytes.size(), output);
        } catch (const std::bad_alloc&) {
            return E_OUTOFMEMORY;
        } catch (...) {
            return E_FAIL;
        }
    }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override {
        return DATA_E_FORMATETC;
    }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* out) override {
        if (out)
            out->ptd = nullptr;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC* format, STGMEDIUM* medium, BOOL release) override {
        if (!format || !medium)
            return E_POINTER;
        if (medium->tymed != TYMED_HGLOBAL || !medium->hGlobal)
            return DV_E_TYMED;
        const SIZE_T size = GlobalSize(medium->hGlobal);
        if (!size || size > 1024 * 1024)
            return E_INVALIDARG;
        try {
            std::vector<std::byte> copy(size);
            const void* source = GlobalLock(medium->hGlobal);
            if (!source)
                return E_OUTOFMEMORY;
            std::memcpy(copy.data(), source, size);
            GlobalUnlock(medium->hGlobal);
            {
                std::scoped_lock lock(extra_mutex_);
                extra_[format->cfFormat] = std::move(copy);
            }
            if (release)
                ReleaseStgMedium(medium);
            return S_OK;
        } catch (...) {
            return E_OUTOFMEMORY;
        }
    }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC** output) override {
        if (direction != DATADIR_GET)
            return E_NOTIMPL;
        FORMATETC formats[] = {{static_cast<CLIPFORMAT>(SelectionFormat()),
                                nullptr,
                                DVASPECT_CONTENT,
                                -1,
                                TYMED_HGLOBAL},
                               {static_cast<CLIPFORMAT>(PreferredFormat()),
                                nullptr,
                                DVASPECT_CONTENT,
                                -1,
                                TYMED_HGLOBAL},
                               {static_cast<CLIPFORMAT>(local_ ? CF_HDROP : DescriptorFormat()),
                                nullptr,
                                DVASPECT_CONTENT,
                                -1,
                                TYMED_HGLOBAL},
                               {static_cast<CLIPFORMAT>(ContentsFormat()),
                                nullptr,
                                DVASPECT_CONTENT,
                                0,
                                TYMED_ISTREAM}};
        return SHCreateStdEnumFmtEtc(local_ ? 3 : 4, formats, output);
    }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override {
        return OLE_E_ADVISENOTSUPPORTED;
    }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override {
        return OLE_E_ADVISENOTSUPPORTED;
    }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override {
        return OLE_E_ADVISENOTSUPPORTED;
    }
    HRESULT STDMETHODCALLTYPE SetAsyncMode(BOOL value) override {
        async_.store(value);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetAsyncMode(BOOL* value) override {
        if (!value)
            return E_POINTER;
        *value = async_.load();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE StartOperation(IBindCtx*) override {
        active_.store(true);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE InOperation(BOOL* value) override {
        if (!value)
            return E_POINTER;
        *value = active_.load();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE EndOperation(HRESULT result, IBindCtx*, DWORD) override {
        active_.store(false);
        if (FAILED(result)) {
            data_->cancelled->store(true);
            runtime_->Invoke([data = data_] {
                for (auto& reader : data->readers)
                    if (auto current = reader.lock()) {
                        current->stream.reset();
                        current->endpoint.reset();
                    }
                return S_OK;
            });
        }
        return S_OK;
    }

private:
    std::atomic<ULONG> refs_{1};
    std::atomic<BOOL> async_{TRUE}, active_{FALSE};
    ComPtr<IUnknown> marshaler_;
    std::shared_ptr<Runtime> runtime_;
    std::shared_ptr<Export> data_;
    std::uint64_t id_;
    bool local_{};
    std::mutex extra_mutex_;
    std::unordered_map<CLIPFORMAT, std::vector<std::byte>> extra_;
};

class DropSource final : public IDropSource {
public:
    explicit DropSource(std::shared_ptr<std::atomic_bool> cancelled)
        : cancelled_(std::move(cancelled)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out)
            return E_POINTER;
        *out = nullptr;
        if (iid != IID_IUnknown && iid != IID_IDropSource)
            return E_NOINTERFACE;
        *out = static_cast<IDropSource*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++refs_;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto n = --refs_;
        if (!n)
            delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escape, DWORD keys) override {
        return (escape || cancelled_->load()) ? DRAGDROP_S_CANCEL
                                              : ((keys & MK_LBUTTON) ? S_OK : DRAGDROP_S_DROP);
    }
    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override {
        return DRAGDROP_S_USEDEFAULTCURSORS;
    }

private:
    std::atomic<ULONG> refs_{1};
    std::shared_ptr<std::atomic_bool> cancelled_;
};

class DropTarget final : public IDropTarget {
public:
    DropTarget(std::shared_ptr<Runtime> runtime,
               ShellFileDrag::DropCallback callback,
               std::function<void()> leave)
        : runtime_(std::move(runtime)), callback_(std::move(callback)), leave_(std::move(leave)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out)
            return E_POINTER;
        *out = nullptr;
        if (iid != IID_IUnknown && iid != IID_IDropTarget)
            return E_NOINTERFACE;
        *out = static_cast<IDropTarget*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++refs_;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto n = --refs_;
        if (!n)
            delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* object,
                                        DWORD,
                                        POINTL point,
                                        DWORD* effect) override {
        selection_ = {};
        try {
            FORMATETC format{static_cast<CLIPFORMAT>(SelectionFormat()),
                             nullptr,
                             DVASPECT_CONTENT,
                             -1,
                             TYMED_HGLOBAL};
            STGMEDIUM medium{};
            if (SUCCEEDED(object->GetData(&format, &medium))) {
                if (medium.tymed == TYMED_HGLOBAL &&
                    GlobalSize(medium.hGlobal) >= sizeof(SelectionToken)) {
                    const auto* token =
                        static_cast<const SelectionToken*>(GlobalLock(medium.hGlobal));
                    if (token) {
                        if (token->process == GetCurrentProcessId())
                            if (auto data = runtime_->Find(token->id))
                                selection_ = data->selection;
                        GlobalUnlock(medium.hGlobal);
                    }
                }
                ReleaseStgMedium(&medium);
            }
            if (selection_.sources.empty()) {
                format.cfFormat = CF_HDROP;
                if (SUCCEEDED(object->GetData(&format, &medium))) {
                    if (medium.tymed == TYMED_HGLOBAL) {
                        const auto drop = static_cast<HDROP>(medium.hGlobal);
                        const UINT count = DragQueryFileW(drop, 0xffffffff, nullptr, 0);
                        for (UINT i = 0; i < count; ++i) {
                            const UINT length = DragQueryFileW(drop, i, nullptr, 0);
                            std::wstring name(length + 1, L'\0');
                            DragQueryFileW(drop, i, name.data(), length + 1);
                            name.resize(length);
                            auto uri = FileUri::local(Utf8(name));
                            selection_.sources.push_back(uri);
                            const DWORD attributes = GetFileAttributesW(name.c_str());
                            if (attributes != INVALID_FILE_ATTRIBUTES &&
                                (attributes & FILE_ATTRIBUTE_DIRECTORY))
                                selection_.directories.push_back(uri);
                        }
                    }
                    ReleaseStgMedium(&medium);
                }
            }
            return Update(point, effect, false);
        } catch (...) {
            *effect = DROPEFFECT_NONE;
            return E_FAIL;
        }
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL point, DWORD* effect) override {
        return Update(point, effect, false);
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override {
        selection_ = {};
        try {
            leave_();
            return S_OK;
        } catch (...) {
            return E_FAIL;
        }
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject*, DWORD, POINTL point, DWORD* effect) override {
        const auto result = Update(point, effect, true);
        DragLeave();
        return result;
    }

private:
    HRESULT Update(POINTL point, DWORD* effect, bool drop) noexcept {
        if (!effect)
            return E_POINTER;
        const bool allowed = (*effect & DROPEFFECT_COPY) != 0;
        *effect = DROPEFFECT_NONE;
        try {
            if (allowed && !selection_.sources.empty() &&
                callback_(selection_, {point.x, point.y}, drop))
                *effect = DROPEFFECT_COPY;
            return S_OK;
        } catch (...) {
            return E_FAIL;
        }
    }
    std::atomic<ULONG> refs_{1};
    std::shared_ptr<Runtime> runtime_;
    ShellFileDrag::DropCallback callback_;
    std::function<void()> leave_;
    ShellFileSelection selection_;
};
constexpr UINT_PTR kDropTargetSubclass = 0x534c4444;
LRESULT CALLBACK TargetSubclass(
    HWND window, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR context) {
    if (message == WM_NCDESTROY)
        reinterpret_cast<ShellFileDrag*>(context)->UnregisterTarget(window);
    return DefSubclassProc(window, message, wparam, lparam);
}
} // namespace

struct ShellFileDrag::Impl {
    HRESULT ole{OleInitialize(nullptr)};
    std::shared_ptr<Runtime> runtime{std::make_shared<Runtime>()};
    std::vector<HWND> targets;
};
ShellFileDrag::ShellFileDrag() : impl_(std::make_unique<Impl>()) {}
ShellFileDrag::~ShellFileDrag() {
    Shutdown();
    if (SUCCEEDED(impl_->ole))
        OleUninitialize();
}
void ShellFileDrag::SetResolver(transfer::CancellableEndpointResolver resolver) {
    impl_->runtime->SetResolver(std::move(resolver));
}
HRESULT ShellFileDrag::CreateDataObject(ShellFileSelection selection, IDataObject** output) {
    if (!output)
        return E_POINTER;
    *output = nullptr;
    if (FAILED(impl_->ole))
        return impl_->ole;
    if (selection.sources.empty())
        return E_INVALIDARG;
    try {
        std::uint64_t id{};
        auto data = impl_->runtime->NewExport(std::move(selection), id);
        *output = new FileDataObject(impl_->runtime, std::move(data), id);
        return S_OK;
    } catch (...) {
        return E_FAIL;
    }
}
HRESULT ShellFileDrag::Run(ShellFileSelection selection) {
    ComPtr<IDataObject> object;
    const auto created = CreateDataObject(std::move(selection), &object);
    if (FAILED(created))
        return created;
    ComPtr<IDropSource> source;
    source.Attach(new (std::nothrow)
                      DropSource(static_cast<FileDataObject*>(object.Get())->Cancellation()));
    if (!source)
        return E_OUTOFMEMORY;
    DWORD effect{};
    const auto result = DoDragDrop(object.Get(), source.Get(), DROPEFFECT_COPY, &effect);
    if (result != DRAGDROP_S_DROP || effect == DROPEFFECT_NONE) {
        ComPtr<IDataObjectAsyncCapability> async;
        if (SUCCEEDED(object.As(&async)))
            async->EndOperation(E_ABORT, nullptr, 0);
    }
    return result;
}
HRESULT
ShellFileDrag::RegisterTarget(HWND window, DropCallback callback, std::function<void()> leave) {
    if (FAILED(impl_->ole))
        return impl_->ole;
    ComPtr<IDropTarget> target;
    const auto created = CreateDropTarget(std::move(callback), std::move(leave), &target);
    if (FAILED(created))
        return created;
    const auto result = RegisterDragDrop(window, target.Get());
    if (SUCCEEDED(result)) {
        try {
            impl_->targets.push_back(window);
        } catch (...) {
            RevokeDragDrop(window);
            return E_OUTOFMEMORY;
        }
        if (!SetWindowSubclass(
                window, TargetSubclass, kDropTargetSubclass, reinterpret_cast<DWORD_PTR>(this))) {
            UnregisterTarget(window);
            return E_FAIL;
        }
    }
    return result;
}
HRESULT ShellFileDrag::CreateDropTarget(DropCallback callback,
                                        std::function<void()> leave,
                                        IDropTarget** output) {
    if (!output)
        return E_POINTER;
    *output = nullptr;
    if (!callback || !leave)
        return E_INVALIDARG;
    try {
        *output = new DropTarget(impl_->runtime, std::move(callback), std::move(leave));
        return S_OK;
    } catch (...) {
        return E_OUTOFMEMORY;
    }
}
void ShellFileDrag::Shutdown() noexcept {
    for (const auto window : impl_->targets)
        if (IsWindow(window)) {
            RemoveWindowSubclass(window, TargetSubclass, kDropTargetSubclass);
            RevokeDragDrop(window);
        }
    impl_->targets.clear();
    const auto runtime = impl_->runtime;
    runtime->Shutdown();
}
ShellFileDrag::Diagnostics ShellFileDrag::diagnostics() const noexcept {
    return impl_->runtime->Diagnostics();
}
void ShellFileDrag::UnregisterTarget(HWND window) noexcept {
    if (std::erase(impl_->targets, window) != 0) {
        RemoveWindowSubclass(window, TargetSubclass, kDropTargetSubclass);
        RevokeDragDrop(window);
    }
}
} // namespace SLink::ui::detail
