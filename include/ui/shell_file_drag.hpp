#pragma once

#include "transfer/transfer.hpp"

#include <windows.h>
#include <functional>
#include <memory>
#include <oleidl.h>
#include <vector>

namespace SLink::ui::detail {

struct ShellFileSelection {
    std::vector<transfer::FileUri> sources;
    std::vector<transfer::FileUri> directories;
};

// UI 线程拥有 OLE 注册与工作线程。Shell 持有的数据对象可以比原窗格活得更久，
// 但 Shutdown 后不能再调用应用的 resolver；所有租约在连接池销毁前释放。
class ShellFileDrag final {
public:
    static constexpr std::size_t kMaximumManifestBytes = 64ULL * 1024ULL * 1024ULL;
    static constexpr std::size_t kMaximumTotalManifestBytes = 128ULL * 1024ULL * 1024ULL;
    static constexpr std::size_t kMaximumReadAheadBytes = 4ULL * 1024ULL * 1024ULL;
    struct Diagnostics {
        bool worker_started{};
        std::uint64_t work_requests{};
        std::uint64_t completion_events{};
        std::size_t manifest_bytes{};
        std::size_t read_ahead_bytes{};
    };
    using DropCallback = std::function<bool(const ShellFileSelection&, POINT, bool drop)>;
    ShellFileDrag();
    ~ShellFileDrag();
    ShellFileDrag(const ShellFileDrag&) = delete;
    ShellFileDrag& operator=(const ShellFileDrag&) = delete;

    void SetResolver(transfer::CancellableEndpointResolver resolver);
    HRESULT CreateDataObject(ShellFileSelection selection, IDataObject** output);
    HRESULT Run(ShellFileSelection selection);
    HRESULT
    CreateDropTarget(DropCallback callback, std::function<void()> leave, IDropTarget** output);
    HRESULT RegisterTarget(HWND window, DropCallback callback, std::function<void()> leave);
    void UnregisterTarget(HWND window) noexcept;
    void Shutdown() noexcept;
    [[nodiscard]] Diagnostics diagnostics() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace SLink::ui::detail
