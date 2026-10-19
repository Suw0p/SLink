// 远程编辑协调器实现：按服务器串行、跨服务器并行执行文档操作，再把结果投递回 UI。
#include "app/editor_controller.hpp"
#include "app/editor_work_scheduler.hpp"

#ifndef _WIN32
#error EditorController is a Windows-only implementation
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace SLink::app {
namespace {

constexpr UINT kDispatchMessage = WM_APP + 0x534;
constexpr std::chrono::milliseconds kCheckpointDelay{400};
constexpr wchar_t kDispatchClass[] = L"SLink.EditorController.Dispatch.1";

std::wstring Utf8ToWide(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) {
        return L"Invalid UTF-8 text";
    }
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8,
                        MB_ERR_INVALID_CHARS,
                        text.data(),
                        static_cast<int>(text.size()),
                        result.data(),
                        length);
    return result;
}

std::string WideToUtf8(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int length = WideCharToMultiByte(CP_UTF8,
                                           WC_ERR_INVALID_CHARS,
                                           text.data(),
                                           static_cast<int>(text.size()),
                                           nullptr,
                                           0,
                                           nullptr,
                                           nullptr);
    if (length <= 0) {
        throw std::invalid_argument("Text is not valid Unicode");
    }
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8,
                        WC_ERR_INVALID_CHARS,
                        text.data(),
                        static_cast<int>(text.size()),
                        result.data(),
                        length,
                        nullptr,
                        nullptr);
    return result;
}

ui::EditorTextEncoding ToUiEncoding(editor::TextEncoding value) {
    switch (value) {
    case editor::TextEncoding::Utf8:
        return ui::EditorTextEncoding::Utf8;
    case editor::TextEncoding::Utf8Bom:
        return ui::EditorTextEncoding::Utf8Bom;
    case editor::TextEncoding::Unknown8Bit:
        return ui::EditorTextEncoding::Legacy8Bit;
    case editor::TextEncoding::Gb2312:
        return ui::EditorTextEncoding::Gb2312;
    }
    return ui::EditorTextEncoding::Legacy8Bit;
}

editor::TextEncoding ToDocumentEncoding(const ui::EditorTextEncoding value) {
    switch (value) {
    case ui::EditorTextEncoding::Utf8:
        return editor::TextEncoding::Utf8;
    case ui::EditorTextEncoding::Utf8Bom:
        return editor::TextEncoding::Utf8Bom;
    case ui::EditorTextEncoding::Legacy8Bit:
        return editor::TextEncoding::Unknown8Bit;
    case ui::EditorTextEncoding::Gb2312:
        return editor::TextEncoding::Gb2312;
    }
    return editor::TextEncoding::Utf8;
}

editor::EditorStatus CheckpointForSave(editor::RemoteDocument& document,
                                       ui::EditorSaveRequest& request) {
    auto status = document.UpdateText(request.content, ToDocumentEncoding(request.encoding));
    if (!status && status.code == editor::EditorErrc::Unsupported) {
        // 保存编码无法表示新字符时，远端文件保持不变；最新编辑仍要写入恢复缓存。
        // 保存请求已经取消之前防抖的缓存任务，所以在此同步补记。
        if (auto cached = document.UpdateText(std::move(request.content)); !cached)
            return cached;
    }
    return status;
}

ui::EditorEolStyle ToUiEol(editor::EolStyle value) {
    switch (value) {
    case editor::EolStyle::None:
        return ui::EditorEolStyle::None;
    case editor::EolStyle::Lf:
        return ui::EditorEolStyle::Lf;
    case editor::EolStyle::CrLf:
        return ui::EditorEolStyle::CrLf;
    case editor::EolStyle::Cr:
        return ui::EditorEolStyle::Cr;
    case editor::EolStyle::Mixed:
        return ui::EditorEolStyle::Mixed;
    }
    return ui::EditorEolStyle::None;
}

ui::EditorDocumentKind ToUiKind(editor::DocumentMode value) {
    switch (value) {
    case editor::DocumentMode::Editable:
        return ui::EditorDocumentKind::Text;
    case editor::DocumentMode::ReadOnlyLarge:
        return ui::EditorDocumentKind::ReadOnlyLarge;
    case editor::DocumentMode::ReadOnlyBinary:
        return ui::EditorDocumentKind::Binary;
    }
    return ui::EditorDocumentKind::Text;
}

std::wstring DocumentTitle(const transfer::FileUri& uri) {
    auto title = Utf8ToWide(uri.filename());
    return title.empty() ? L"(remote file)" : std::move(title);
}

std::wstring OperationMessage(std::wstring_view operation, std::wstring_view message) {
    std::wstring result(operation);
    if (!result.empty()) {
        result.append(L"\r\n\r\n");
    }
    result.append(message);
    return result;
}

std::vector<std::byte> ReadBinaryProbe(transfer::IFileEndpoint& endpoint,
                                       const transfer::FileUri& uri) {
    auto input = endpoint.open_read(uri);
    std::vector<std::byte> bytes;
    bytes.reserve(editor::RemoteEditorService::kBinaryProbeBytes);
    std::vector<std::byte> chunk(16 * 1024);
    while (bytes.size() < editor::RemoteEditorService::kBinaryProbeBytes) {
        const auto remaining = editor::RemoteEditorService::kBinaryProbeBytes - bytes.size();
        const auto read =
            input->read(std::span<std::byte>(chunk).first((std::min)(remaining, chunk.size())));
        if (read == 0) {
            break;
        }
        if (read > chunk.size() || read > remaining) {
            throw std::runtime_error("Endpoint returned an invalid binary probe length");
        }
        bytes.insert(bytes.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(read));
    }
    return bytes;
}

} // namespace

struct EditorController::Impl {
    struct DocumentState {
        ui::EditorDocumentId id{};
        transfer::FileUri uri;
        std::string lane;
        std::shared_ptr<transfer::IFileEndpoint> endpoint;
        std::unique_ptr<editor::RemoteDocument> document;
        std::optional<editor::ConflictSnapshot> conflict;
        std::optional<transfer::FileUri> save_as_conflict_target;
        std::mutex mutex;
        std::atomic<std::uint64_t> edit_generation{};
        std::atomic<bool> ui_dirty{};
        std::atomic<bool> closing{};
    };

    struct OpenPayload {
        std::shared_ptr<DocumentState> state;
        ui::EditorDocumentDescriptor descriptor;
        std::vector<std::byte> binary_probe;
    };

    ui::EditorPage& page;
    editor::RemoteEditorService service;
    EndpointResolver endpoint_resolver;
    FileActivationResolver file_activation_resolver;
    SaveAsTargetResolver save_as_target_resolver;
    CloseConfirmation close_confirmation;
    DirectoryActivationCallback directory_activation;
    ConflictDiffCallback conflict_diff;
    ErrorCallback error_callback;

    DWORD ui_thread_id{GetCurrentThreadId()};
    HWND dispatch_window{};
    std::unique_ptr<EditorWorkScheduler> scheduler;

    std::mutex completion_mutex;
    std::deque<std::function<void()>> completions;
    std::atomic<bool> stopping{};

    // Accessed on the UI thread only.
    ui::EditorDocumentId next_document_id{1};
    std::unordered_map<ui::EditorDocumentId, std::shared_ptr<DocumentState>> documents;
    std::unordered_map<std::string, ui::EditorDocumentId> uri_index;

    Impl(ui::EditorPage& editor_page, const storage::AppPaths& paths, EndpointResolver resolver)
        : page(editor_page), service(paths), endpoint_resolver(std::move(resolver)) {
        if (!endpoint_resolver) {
            throw std::invalid_argument("EditorController requires an endpoint resolver");
        }
        if (page.hwnd() == nullptr ||
            GetWindowThreadProcessId(page.hwnd(), nullptr) != ui_thread_id) {
            throw std::invalid_argument(
                "EditorController must be created after EditorPage on its UI thread");
        }
        if (!paths.ContainsInternalPath(paths.editor_cache_dir()) ||
            !std::filesystem::is_directory(paths.editor_cache_dir())) {
            throw std::invalid_argument("Editor cache directory is not a valid AppPaths directory");
        }

        RegisterDispatchClass();
        dispatch_window = CreateWindowExW(0,
                                          kDispatchClass,
                                          L"",
                                          0,
                                          0,
                                          0,
                                          0,
                                          0,
                                          HWND_MESSAGE,
                                          nullptr,
                                          GetModuleHandleW(nullptr),
                                          this);
        if (dispatch_window == nullptr) {
            throw std::runtime_error("Cannot create the editor UI dispatch window");
        }

        try {
            scheduler = std::make_unique<EditorWorkScheduler>(
                4, [this](std::exception_ptr failure) {
                    try {
                        std::rethrow_exception(failure);
                    } catch (const std::exception& error) {
                        ReportErrorAsync(L"编辑器后台任务", error.what());
                    } catch (...) {
                        ReportErrorAsync(L"编辑器后台任务", "Unknown background error");
                    }
                });
            BindPageCallbacks();
        } catch (...) {
            UnbindPageCallbacks();
            DestroyWindow(dispatch_window);
            dispatch_window = nullptr;
            throw;
        }
    }

    ~Impl() {
        stopping.store(true, std::memory_order_release);
        UnbindPageCallbacks();

        struct PendingClose {
            std::shared_ptr<DocumentState> state;
            std::optional<std::string> text;
        };
        std::vector<PendingClose> pending;
        pending.reserve(documents.size());
        for (const auto& [id, state] : documents) {
            std::optional<std::string> text;
            if (state->ui_dirty.load(std::memory_order_acquire)) {
                text = page.DocumentText(id);
            }
            pending.push_back({state, std::move(text)});
        }

        scheduler->Stop();
        for (auto& item : pending) {
            std::scoped_lock lock(item.state->mutex);
            if (!item.state->document) {
                continue;
            }
            if (item.text) {
                item.state->document->UpdateText(std::move(*item.text));
            }
            item.state->document->Close();
        }
        documents.clear();
        uri_index.clear();

        {
            std::scoped_lock lock(completion_mutex);
            completions.clear();
        }
        if (dispatch_window != nullptr) {
            DestroyWindow(dispatch_window);
            dispatch_window = nullptr;
        }
    }

    static LRESULT CALLBACK DispatchWindowProc(HWND window,
                                               UINT message,
                                               WPARAM wparam,
                                               LPARAM lparam) {
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
            auto* self = static_cast<Impl*>(create->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == kDispatchMessage && self != nullptr) {
            self->DrainCompletions();
            return 0;
        }
        if (message == WM_NCDESTROY) {
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    static void RegisterDispatchClass() {
        static std::once_flag once;
        static DWORD registration_error{};
        std::call_once(once, [] {
            WNDCLASSEXW type{};
            type.cbSize = sizeof(type);
            type.lpfnWndProc = &DispatchWindowProc;
            type.hInstance = GetModuleHandleW(nullptr);
            type.lpszClassName = kDispatchClass;
            if (RegisterClassExW(&type) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
                registration_error = GetLastError();
            }
        });
        if (registration_error != ERROR_SUCCESS) {
            throw std::runtime_error("Cannot register the editor UI dispatch window");
        }
    }

    void BindPageCallbacks() {
        page.SetFileActivationCallback([this](ui::EditorFileActivation activation) {
            OnFileActivated(std::move(activation));
        });
        page.SetSaveRequestedCallback([this](ui::EditorSaveRequest request) {
            OnSave(std::move(request));
        });
        page.SetEncodingChangedCallback([this](ui::EditorSaveRequest request) {
            OnEncodingChanged(std::move(request));
        });
        page.SetOverwriteRequestedCallback([this](ui::EditorSaveRequest request) {
            OnOverwrite(std::move(request));
        });
        page.SetSaveAsRequestedCallback([this](ui::EditorSaveRequest request) {
            OnSaveAs(std::move(request));
        });
        page.SetDiffRequestedCallback([this](ui::EditorDocumentId id) {
            OnDiff(id);
        });
        page.SetCloseRequestedCallback([this](ui::EditorDocumentId id, bool dirty) {
            OnClose(id, dirty);
        });
        page.SetDocumentChangedCallback([this](ui::EditorDocumentId id) {
            OnDocumentChanged(id);
        });
    }

    void UnbindPageCallbacks() {
        page.SetFileActivationCallback({});
        page.SetSaveRequestedCallback({});
        page.SetEncodingChangedCallback({});
        page.SetOverwriteRequestedCallback({});
        page.SetSaveAsRequestedCallback({});
        page.SetDiffRequestedCallback({});
        page.SetCloseRequestedCallback({});
        page.SetDocumentChangedCallback({});
    }

    static std::string LaneFor(const transfer::FileUri& uri) {
        return uri.is_sftp() ? "sftp:" + uri.authority() : "local";
    }

    void Enqueue(std::string lane,
                 std::function<void()> action,
                 std::chrono::milliseconds delay = {}) {
        if (stopping.load(std::memory_order_acquire)) {
            return;
        }
        scheduler->Enqueue(std::move(lane), std::move(action), delay);
    }

    void PostToUi(std::function<void()> action) {
        if (stopping.load(std::memory_order_acquire)) {
            return;
        }
        {
            std::scoped_lock lock(completion_mutex);
            if (stopping.load(std::memory_order_relaxed)) {
                return;
            }
            completions.push_back(std::move(action));
        }
        PostMessageW(dispatch_window, kDispatchMessage, 0, 0);
    }

    void DrainCompletions() {
        std::deque<std::function<void()>> ready;
        {
            std::scoped_lock lock(completion_mutex);
            ready.swap(completions);
        }
        for (auto& completion : ready) {
            try {
                completion();
            } catch (const std::exception& error) {
                ReportError(L"编辑器界面", Utf8ToWide(error.what()));
            } catch (...) {
                ReportError(L"编辑器界面", L"发生未知错误。");
            }
        }
    }

    bool IsCurrent(const std::shared_ptr<DocumentState>& state) const {
        const auto item = documents.find(state->id);
        return item != documents.end() && item->second == state;
    }

    void ReportError(std::wstring_view operation, std::wstring_view message) {
        if (error_callback) {
            error_callback(operation, message);
            return;
        }
        const auto text = OperationMessage(operation, message);
        MessageBoxW(page.hwnd(), text.c_str(), L"SLink", MB_OK | MB_ICONERROR);
    }

    void ReportErrorAsync(std::wstring operation, std::string message) {
        PostToUi([this, operation = std::move(operation), message = std::move(message)] {
            ReportError(operation, Utf8ToWide(message));
        });
    }

    std::shared_ptr<transfer::IFileEndpoint> ResolveEndpoint(const transfer::FileUri& uri) {
        auto endpoint = endpoint_resolver(uri);
        if (!endpoint || !endpoint->accepts(uri) || !endpoint->available()) {
            throw std::runtime_error("No connected file endpoint accepts the requested URI");
        }
        return endpoint;
    }

    ui::EditorDocumentDescriptor Describe(const DocumentState& state,
                                          const editor::RemoteDocument& document,
                                          bool dirty = false) const {
        ui::EditorDocumentDescriptor result;
        const auto& document_uri = document.uri();
        result.id = state.id;
        result.title = DocumentTitle(document_uri);
        result.server_id = document_uri.is_sftp() ? document_uri.authority() : "local://host";
        result.remote_path = Utf8ToWide(document_uri.path());
        result.content = document.text();
        result.encoding = ToUiEncoding(document.encoding());
        result.eol = ToUiEol(document.eol());
        result.kind = ToUiKind(document.mode());
        result.read_only = document.mode() != editor::DocumentMode::Editable;
        result.dirty = dirty;
        return result;
    }

    void Open(transfer::FileUri uri) {
        if (GetCurrentThreadId() != ui_thread_id) {
            PostToUi([this, uri = std::move(uri)]() mutable {
                Open(std::move(uri));
            });
            return;
        }
        const auto key = uri.to_string();
        if (const auto found = uri_index.find(key); found != uri_index.end()) {
            page.ActivateDocument(found->second);
            return;
        }

        auto state = std::make_shared<DocumentState>();
        state->id = next_document_id++;
        state->uri = std::move(uri);
        state->lane = LaneFor(state->uri);
        documents.emplace(state->id, state);
        uri_index.emplace(key, state->id);

        Enqueue(state->lane, [this, state] {
            try {
                auto endpoint = ResolveEndpoint(state->uri);
                auto opened = service.Open(endpoint, state->uri);
                if (!opened) {
                    throw std::runtime_error(opened.status().message);
                }

                auto payload = std::make_shared<OpenPayload>();
                payload->state = state;
                {
                    std::scoped_lock lock(state->mutex);
                    state->endpoint = std::move(endpoint);
                    state->document = std::move(opened).value();
                    payload->descriptor = Describe(*state, *state->document);
                    if (state->document->mode() == editor::DocumentMode::ReadOnlyBinary) {
                        payload->binary_probe = ReadBinaryProbe(*state->endpoint, state->uri);
                    }
                }
                PostToUi([this, payload] {
                    if (!IsCurrent(payload->state)) {
                        return;
                    }
                    if (payload->descriptor.kind == ui::EditorDocumentKind::Binary) {
                        page.OpenBinaryDocument(std::move(payload->descriptor),
                                                payload->binary_probe);
                    } else {
                        page.OpenDocument(std::move(payload->descriptor));
                    }
                });
            } catch (const std::exception& error) {
                const auto message = std::string(error.what());
                PostToUi([this, state, message] {
                    if (IsCurrent(state)) {
                        uri_index.erase(state->uri.to_string());
                        documents.erase(state->id);
                    }
                    ReportError(L"打开远程文件", Utf8ToWide(message));
                });
            }
        });
    }

    void RecoverUnsaved() {
        Enqueue("control", [this] {
            auto entries = service.RecoverableDocuments();
            if (!entries) {
                ReportErrorAsync(L"恢复未保存文件", entries.status().message);
                return;
            }
            for (const auto& entry : entries.value()) {
                try {
                    auto endpoint = endpoint_resolver(entry.uri);
                    // Recovery is attempted incrementally as file sessions
                    // become available. Cache entries for other, currently
                    // disconnected servers remain untouched and are not
                    // reported as errors.
                    if (!endpoint || !endpoint->accepts(entry.uri) || !endpoint->available()) {
                        continue;
                    }
                    auto recovered = service.Recover(endpoint, entry);
                    if (!recovered) {
                        throw std::runtime_error(recovered.status().message);
                    }
                    auto document =
                        std::shared_ptr<editor::RemoteDocument>(std::move(recovered).value());
                    auto endpoint_copy = std::move(endpoint);
                    const auto uri = entry.uri;
                    PostToUi([this,
                              document = std::move(document),
                              endpoint = std::move(endpoint_copy),
                              uri] {
                        const auto key = uri.to_string();
                        if (const auto found = uri_index.find(key); found != uri_index.end()) {
                            page.ActivateDocument(found->second);
                            return;
                        }
                        auto state = std::make_shared<DocumentState>();
                        state->id = next_document_id++;
                        state->uri = uri;
                        state->lane = LaneFor(state->uri);
                        state->endpoint = endpoint;
                        state->document =
                            std::make_unique<editor::RemoteDocument>(std::move(*document));
                        state->ui_dirty.store(true, std::memory_order_release);
                        auto descriptor = Describe(*state, *state->document, true);
                        documents.emplace(state->id, state);
                        uri_index.emplace(key, state->id);
                        page.OpenDocument(std::move(descriptor));
                    });
                } catch (const std::exception& error) {
                    ReportErrorAsync(L"恢复未保存文件", error.what());
                }
            }
        });
    }

    void RebindEndpoint(std::string server_id, std::shared_ptr<transfer::IFileEndpoint> endpoint) {
        if (GetCurrentThreadId() != ui_thread_id) {
            PostToUi(
                [this, server_id = std::move(server_id), endpoint = std::move(endpoint)]() mutable {
                    RebindEndpoint(std::move(server_id), std::move(endpoint));
                });
            return;
        }
        if (server_id.empty() || !endpoint) {
            return;
        }

        // 一个端点可能对应多个已打开标签。在 UI 线程拍快照，再按文档
        // 工作队列顺序替换，使重连前已排队的缓存更新不会与端点切换并发。
        std::vector<std::shared_ptr<DocumentState>> matching;
        matching.reserve(documents.size());
        for (const auto& [id, state] : documents) {
            (void)id;
            if (state && state->uri.is_sftp() && state->uri.authority() == server_id &&
                !state->closing.load(std::memory_order_acquire)) {
                matching.push_back(state);
            }
        }
        for (const auto& state : matching) {
            Enqueue(state->lane, [this, state, endpoint] {
                editor::EditorStatus status;
                {
                    std::scoped_lock lock(state->mutex);
                    if (!state->document || state->closing.load(std::memory_order_acquire)) {
                        return;
                    }
                    status = state->document->RebindEndpoint(endpoint);
                    if (status) {
                        state->endpoint = endpoint;
                    }
                }
                if (!status) {
                    ReportErrorAsync(L"恢复远程编辑连接", status.message);
                }
            });
        }
    }

    void OnFileActivated(ui::EditorFileActivation activation) {
        if (activation.entry.directory || activation.entry.local_shortcut) {
            if (directory_activation) {
                directory_activation(activation);
            }
            return;
        }
        try {
            std::optional<transfer::FileUri> uri;
            if (file_activation_resolver) {
                uri = file_activation_resolver(activation);
            } else {
                auto directory = WideToUtf8(activation.directory);
                auto name = WideToUtf8(activation.entry.name);
                uri = transfer::FileUri::sftp(activation.server_id, directory).joined(name);
            }
            if (uri) {
                Open(std::move(*uri));
            }
        } catch (const std::exception& error) {
            ReportError(L"打开远程文件", Utf8ToWide(error.what()));
        }
    }

    void OnDocumentChanged(ui::EditorDocumentId id) {
        const auto found = documents.find(id);
        if (found == documents.end() || found->second->closing.load(std::memory_order_acquire)) {
            return;
        }
        auto state = found->second;
        state->ui_dirty.store(true, std::memory_order_release);
        const auto generation = state->edit_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
        Enqueue(
            state->lane,
            [this, state, id, generation] {
                if (state->closing.load(std::memory_order_acquire) ||
                    state->edit_generation.load(std::memory_order_acquire) != generation) {
                    return;
                }
                PostToUi([this, state, id, generation] {
                    if (state->closing.load(std::memory_order_acquire) ||
                        state->edit_generation.load(std::memory_order_acquire) != generation) {
                        return;
                    }
                    auto text = page.DocumentText(id);
                    if (!text) {
                        return;
                    }
                    Enqueue(state->lane,
                            [this, state, generation, text = std::move(*text)]() mutable {
                        if (state->closing.load(std::memory_order_acquire) ||
                            state->edit_generation.load(std::memory_order_acquire) != generation) {
                            return;
                        }
                        editor::EditorStatus status;
                        {
                            std::scoped_lock lock(state->mutex);
                            if (!state->document) {
                                return;
                            }
                            status = state->document->UpdateText(std::move(text));
                        }
                        if (!status) {
                            ReportErrorAsync(L"保存编辑器恢复缓存", status.message);
                        }
                            });
                });
            },
            kCheckpointDelay);
    }

    void OnSave(ui::EditorSaveRequest request) {
        const auto found = documents.find(request.id);
        if (found == documents.end()) {
            return;
        }
        auto state = found->second;
        if (state->closing.load(std::memory_order_acquire)) {
            return;
        }
        state->edit_generation.fetch_add(1, std::memory_order_acq_rel);
        state->ui_dirty.store(true, std::memory_order_release);
        page.SetDocumentSaving(request.id, true);

        Enqueue(state->lane, [this, state, request = std::move(request)]() mutable {
            editor::EditorStatus update;
            std::optional<editor::SaveResult> saved;
            editor::EditorStatus failure;
            {
                std::scoped_lock lock(state->mutex);
                if (!state->document || state->closing.load(std::memory_order_acquire)) {
                    return;
                }
                update = CheckpointForSave(*state->document, request);
                if (update) {
                    auto result = state->document->Save();
                    if (result) {
                        saved = std::move(result).value();
                        state->conflict = saved->conflict;
                        state->save_as_conflict_target.reset();
                    } else {
                        failure = result.status();
                    }
                }
            }

            PostToUi([this,
                      state,
                      update = std::move(update),
                      saved = std::move(saved),
                      failure = std::move(failure)] {
                if (!IsCurrent(state)) {
                    return;
                }
                if (!update) {
                    page.SetDocumentSaving(state->id, false);
                    ReportError(L"保存远程文件", Utf8ToWide(update.message));
                    return;
                }
                if (!saved) {
                    page.SetDocumentSaving(state->id, false);
                    ReportError(L"保存远程文件", Utf8ToWide(failure.message));
                    return;
                }
                if (saved->outcome == editor::SaveOutcome::Conflict) {
                    page.SetDocumentSaving(state->id, false);
                    page.SetDocumentConflict(state->id,
                                             L"远端文件已变化；请选择查看差异、覆盖或另存为。");
                    return;
                }
                state->ui_dirty.store(false, std::memory_order_release);
                page.MarkDocumentSaved(state->id);
            });
        });
    }

    void OnEncodingChanged(ui::EditorSaveRequest request) {
        const auto found = documents.find(request.id);
        if (found == documents.end() || found->second->closing.load(std::memory_order_acquire)) {
            return;
        }
        auto state = found->second;
        state->edit_generation.fetch_add(1, std::memory_order_acq_rel);
        state->ui_dirty.store(true, std::memory_order_release);
        Enqueue(state->lane, [this, state, request = std::move(request)]() mutable {
            editor::EditorStatus status;
            {
                std::scoped_lock lock(state->mutex);
                if (!state->document || state->closing.load(std::memory_order_acquire))
                    return;
                status = state->document->UpdateText(std::move(request.content),
                                                     ToDocumentEncoding(request.encoding));
            }
            if (!status)
                ReportErrorAsync(L"切换文件编码", status.message);
        });
    }

    void OnOverwrite(ui::EditorSaveRequest request) {
        const auto found = documents.find(request.id);
        if (found == documents.end()) {
            return;
        }
        auto state = found->second;
        if (state->closing.load(std::memory_order_acquire)) {
            return;
        }
        state->edit_generation.fetch_add(1, std::memory_order_acq_rel);
        state->ui_dirty.store(true, std::memory_order_release);
        page.SetDocumentSaving(request.id, true);
        const auto old_uri = state->uri;
        Enqueue(state->lane, [this, state, old_uri, request = std::move(request)]() mutable {
            editor::EditorStatus update;
            std::optional<editor::SaveResult> saved;
            editor::EditorStatus failure;
            std::optional<ui::EditorDocumentDescriptor> descriptor;
            transfer::FileUri new_uri;
            {
                std::scoped_lock lock(state->mutex);
                if (!state->document || state->closing.load(std::memory_order_acquire)) {
                    return;
                }
                update = CheckpointForSave(*state->document, request);
                if (update) {
                    editor::EditorResult<editor::SaveResult> result =
                        state->save_as_conflict_target
                            ? state->document->SaveAs(*state->save_as_conflict_target, true)
                            : state->document->Save(editor::SaveMode::OverwriteRemote);
                    if (result) {
                        saved = std::move(result).value();
                        state->conflict.reset();
                        state->save_as_conflict_target.reset();
                        new_uri = state->document->uri();
                        descriptor = Describe(*state, *state->document, false);
                    } else {
                        failure = result.status();
                    }
                }
            }
            PostToUi([this,
                      state,
                      old_uri,
                      new_uri = std::move(new_uri),
                      descriptor = std::move(descriptor),
                      update = std::move(update),
                      saved = std::move(saved),
                      failure = std::move(failure)]() mutable {
                if (!IsCurrent(state)) {
                    return;
                }
                if (!update || !saved) {
                    page.SetDocumentSaving(state->id, false);
                    ReportError(L"覆盖远端文件",
                                Utf8ToWide(!update ? update.message : failure.message));
                    return;
                }
                if (new_uri != old_uri) {
                    uri_index.erase(old_uri.to_string());
                    uri_index[new_uri.to_string()] = state->id;
                    state->uri = new_uri;
                }
                state->ui_dirty.store(false, std::memory_order_release);
                page.ClearDocumentConflict(state->id);
                if (descriptor) {
                    page.OpenDocument(std::move(*descriptor));
                }
                page.MarkDocumentSaved(state->id);
            });
        });
    }

    void OnSaveAs(ui::EditorSaveRequest request) {
        const auto found = documents.find(request.id);
        if (found == documents.end()) {
            return;
        }
        if (!save_as_target_resolver) {
            ReportError(L"远程另存为", L"尚未配置远程目标路径选择器，本次未写入远端。");
            return;
        }
        auto target = save_as_target_resolver(found->second->uri);
        if (!target) {
            return;
        }
        auto state = found->second;
        if (state->closing.load(std::memory_order_acquire)) {
            return;
        }
        const auto old_uri = state->uri;
        state->edit_generation.fetch_add(1, std::memory_order_acq_rel);
        state->ui_dirty.store(true, std::memory_order_release);
        page.SetDocumentSaving(request.id, true);
        Enqueue(state->lane,
                [this,
                 state,
                 old_uri,
                 target = std::move(*target),
                 request = std::move(request)]() mutable {
                    editor::EditorStatus update;
                    std::optional<editor::SaveResult> saved;
                    editor::EditorStatus failure;
                    std::optional<ui::EditorDocumentDescriptor> descriptor;
                    transfer::FileUri new_uri;
                    {
                        std::scoped_lock lock(state->mutex);
                        if (!state->document || state->closing.load(std::memory_order_acquire)) {
                            return;
                        }
                        update = CheckpointForSave(*state->document, request);
                        if (update) {
                            const auto selected_target = target;
                            auto result = state->document->SaveAs(std::move(target), false);
                            if (result) {
                                saved = std::move(result).value();
                                state->conflict = saved->conflict;
                                if (saved->outcome == editor::SaveOutcome::Saved) {
                                    state->save_as_conflict_target.reset();
                                    new_uri = state->document->uri();
                                    descriptor = Describe(*state, *state->document, false);
                                } else {
                                    state->save_as_conflict_target = selected_target;
                                }
                            } else {
                                failure = result.status();
                            }
                        }
                    }
                    PostToUi([this,
                              state,
                              old_uri,
                              new_uri = std::move(new_uri),
                              update = std::move(update),
                              saved = std::move(saved),
                              failure = std::move(failure),
                              descriptor = std::move(descriptor)]() mutable {
                        if (!IsCurrent(state)) {
                            return;
                        }
                        if (!update || !saved) {
                            page.SetDocumentSaving(state->id, false);
                            ReportError(L"远程另存为",
                                        Utf8ToWide(!update ? update.message : failure.message));
                            return;
                        }
                        if (saved->outcome == editor::SaveOutcome::Conflict) {
                            page.SetDocumentSaving(state->id, false);
                            page.SetDocumentConflict(
                                state->id, L"另存为目标已存在；请选择其他名称，或确认覆盖后重试。");
                            return;
                        }
                        uri_index.erase(old_uri.to_string());
                        uri_index[new_uri.to_string()] = state->id;
                        state->uri = new_uri;
                        state->ui_dirty.store(false, std::memory_order_release);
                        page.ClearDocumentConflict(state->id);
                        if (descriptor) {
                            page.OpenDocument(std::move(*descriptor));
                        }
                        page.MarkDocumentSaved(state->id);
                    });
                });
    }

    void OnDiff(ui::EditorDocumentId id) {
        const auto found = documents.find(id);
        if (found == documents.end()) {
            return;
        }
        auto state = found->second;
        std::optional<editor::ConflictSnapshot> snapshot;
        {
            std::scoped_lock lock(state->mutex);
            snapshot = state->conflict;
        }
        if (!snapshot) {
            return;
        }
        if (conflict_diff) {
            conflict_diff(id, state->uri, *snapshot);
            return;
        }
        std::wstring message = L"已检测到远端版本变化。";
        if (snapshot->remote_missing) {
            message = L"远端文件已被删除。";
        } else if (snapshot->remote_too_large) {
            message = L"远端版本超过 20 MB，未载入差异内容。";
        } else if (snapshot->remote_is_binary) {
            message = L"远端版本为二进制内容，不能进行文本差异比较。";
        } else {
            message.append(L"\r\n本地字符数：");
            message.append(std::to_wstring(snapshot->local_text.size()));
            message.append(L"\r\n远端字符数：");
            message.append(std::to_wstring(snapshot->remote_text.size()));
        }
        MessageBoxW(page.hwnd(), message.c_str(), L"远端差异", MB_OK | MB_ICONINFORMATION);
    }

    void OnClose(ui::EditorDocumentId id, bool dirty) {
        const auto found = documents.find(id);
        if (found == documents.end()) {
            page.CloseDocument(id);
            return;
        }
        auto state = found->second;
        if (dirty) {
            bool allow = false;
            if (close_confirmation) {
                allow = close_confirmation(state->uri);
            } else {
                allow = MessageBoxW(page.hwnd(),
                                    L"未保存内容会保留在应用程序目录的恢复缓存中。"
                                    L"\r\n是否关闭此标签？",
                                    L"关闭远程文件",
                                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES;
            }
            if (!allow) {
                return;
            }
        }

        auto text = dirty ? page.DocumentText(id) : std::optional<std::string>{};
        state->closing.store(true, std::memory_order_release);
        state->edit_generation.fetch_add(1, std::memory_order_acq_rel);
        uri_index.erase(state->uri.to_string());
        documents.erase(found);
        page.CloseDocument(id);

        Enqueue(state->lane, [this, state, text = std::move(text)]() mutable {
            editor::EditorStatus update;
            editor::EditorStatus closed;
            {
                std::scoped_lock lock(state->mutex);
                if (!state->document) {
                    return;
                }
                if (text) {
                    update = state->document->UpdateText(std::move(*text));
                }
                if (!text || update) {
                    closed = state->document->Close();
                }
            }
            if (text && !update) {
                ReportErrorAsync(L"保留未保存内容", update.message);
            } else if (!closed) {
                ReportErrorAsync(L"关闭远程文件", closed.message);
            }
        });
    }
};

EditorController::EditorController(ui::EditorPage& page,
                                   const storage::AppPaths& paths,
                                   EndpointResolver endpoint_resolver)
    : impl_(std::make_unique<Impl>(page, paths, std::move(endpoint_resolver))) {}

EditorController::~EditorController() = default;

void EditorController::Open(transfer::FileUri uri) {
    impl_->Open(std::move(uri));
}

void EditorController::RecoverUnsaved() {
    impl_->RecoverUnsaved();
}

void EditorController::RebindEndpoint(std::string server_id,
                                      std::shared_ptr<transfer::IFileEndpoint> endpoint) {
    impl_->RebindEndpoint(std::move(server_id), std::move(endpoint));
}

void EditorController::SetFileActivationResolver(FileActivationResolver resolver) {
    impl_->file_activation_resolver = std::move(resolver);
}

void EditorController::SetSaveAsTargetResolver(SaveAsTargetResolver resolver) {
    impl_->save_as_target_resolver = std::move(resolver);
}

void EditorController::SetCloseConfirmation(CloseConfirmation confirmation) {
    impl_->close_confirmation = std::move(confirmation);
}

void EditorController::SetDirectoryActivationCallback(DirectoryActivationCallback callback) {
    impl_->directory_activation = std::move(callback);
}

void EditorController::SetConflictDiffCallback(ConflictDiffCallback callback) {
    impl_->conflict_diff = std::move(callback);
}

void EditorController::SetErrorCallback(ErrorCallback callback) {
    impl_->error_callback = std::move(callback);
}

std::size_t EditorController::document_count() const noexcept {
    return impl_->documents.size();
}

} // namespace SLink::app
