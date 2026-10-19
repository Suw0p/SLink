#pragma once

#include "core/status.hpp"
#include "ssh/network_reactor.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace SLink::app {

struct TcpForwardSpec {
    std::string bind_address;
    std::uint16_t local_port{};
    std::string target_address;
    std::uint16_t target_port{};
};

enum class PortForwardState {
    starting,
    running,
    failed,
    stopped,
};

struct PortForwardUpdate {
    TcpForwardSpec spec;
    PortForwardState state{PortForwardState::starting};
    std::wstring message;
};

// 独立 TCP 端口代理。监听地址和目标地址全部由用户填写，不依赖终端或 SSH 会话。
// bind_address 决定暴露范围：127.0.0.1 仅本机可访问，0.0.0.0 可供局域网访问。
// 活动规则由 bind_address + local_port 唯一标识，不能只按端口合并不同监听地址。
class PortForwardController final {
public:
    using UpdateCallback = std::function<void(PortForwardUpdate)>;

    explicit PortForwardController(std::shared_ptr<ssh::IUiDispatcher> ui_dispatcher);
    ~PortForwardController();

    PortForwardController(const PortForwardController&) = delete;
    PortForwardController& operator=(const PortForwardController&) = delete;

    void Start(TcpForwardSpec spec, UpdateCallback callback);
    void Stop(std::uint16_t local_port);
    void Shutdown() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace SLink::app
