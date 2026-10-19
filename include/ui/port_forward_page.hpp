#pragma once

#include <windows.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace SLink::ui {

struct PortForwardRequest {
    std::string bind_address;
    std::uint16_t local_port{};
    std::string target_address;
    std::uint16_t target_port{};
};

// 端口转发页只维护显示模型和用户请求，不直接创建监听 socket 或修改防火墙。
// 一条规则由 bind_address + local_port 唯一确定，因此 127.0.0.1:3306 和
// 0.0.0.0:3306 可以同时持久化，实际运行冲突由控制器处理。
struct PortForwardEntry {
    std::string bind_address;
    std::uint16_t local_port{};
    std::wstring local_endpoint;
    std::wstring target_endpoint;
    std::wstring process;
    bool running{};
    bool failed{};
    bool proxy_enabled{};
    bool proxy_busy{};
    bool lan_listener{};
    bool firewall_open{};
    bool firewall_busy{};
};

enum class FirewallHelperUiState {
    off,
    starting,
    running,
    stopping,
    error,
};

[[nodiscard]] std::optional<PortForwardRequest> ParsePortForwardRequest(std::wstring bind_text,
                                                                        std::wstring target_text);

class PortForwardPage final {
public:
    using StartRequestedCallback = std::function<void(PortForwardRequest)>;
    using DeleteRequestedCallback = std::function<void(std::string, std::uint16_t)>;
    using ProxyRequestedCallback = std::function<void(std::string, std::uint16_t, bool)>;
    using FirewallHelperRequestedCallback = std::function<void(bool)>;
    using FirewallRuleRequestedCallback = std::function<void(std::string, std::uint16_t, bool)>;

    PortForwardPage();
    ~PortForwardPage();

    PortForwardPage(const PortForwardPage&) = delete;
    PortForwardPage& operator=(const PortForwardPage&) = delete;

    bool Create(HWND parent, HINSTANCE instance);
    void SetBounds(int x, int y, int width, int height);
    void Show(bool visible);
    void ApplyTheme();
    bool HandleEnterKey(HWND source);

    void SetStartRequestedCallback(StartRequestedCallback callback);
    void SetDeleteRequestedCallback(DeleteRequestedCallback callback);
    void SetProxyRequestedCallback(ProxyRequestedCallback callback);
    void SetFirewallHelperRequestedCallback(FirewallHelperRequestedCallback callback);
    void SetFirewallRuleRequestedCallback(FirewallRuleRequestedCallback callback);
    void SetFirewallHelperState(FirewallHelperUiState state, std::wstring message = {});
    void SetFirewallRuleState(std::string_view bind_address,
                              std::uint16_t local_port,
                              bool enabled,
                              bool succeeded,
                              std::wstring message = {});
    void SetProxyState(std::string_view bind_address,
                       std::uint16_t local_port,
                       bool enabled,
                       bool running,
                       bool failed,
                       std::wstring message = {});
    void UpsertEntry(PortForwardEntry entry);
    void RemoveEntry(std::string_view bind_address, std::uint16_t local_port);
    HWND hwnd() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace SLink::ui
