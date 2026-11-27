#pragma once

#include "ssh/network_reactor.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace SLink::app {

enum class FirewallHelperState {
    off,
    starting,
    running,
    stopping,
    error,
};

struct FirewallHelperUpdate {
    FirewallHelperState state{FirewallHelperState::off};
    std::wstring message;
    std::string session_id;
};

struct FirewallRuleUpdate {
    std::uint16_t local_port{};
    bool enabled{};
    bool succeeded{};
    std::wstring message;
    std::string session_id;
    std::string rule_name;
};

// 只有辅助进程明确确认已删除/不存在的规则才能产生回执；运行状态不是清理凭据。
struct FirewallRuleIdentity {
    std::string session_id;
    std::uint16_t local_port{};
    auto operator<=>(const FirewallRuleIdentity&) const = default;
};

// 管理由用户显式启停的管理员防火墙辅助进程。SLink 主进程始终保持非管理员权限；
// 辅助进程只接受本次 SLink 会话的 TCP 入站规则增删请求，不提供任意命令执行能力。
// 关闭辅助进程或主程序时，会同步删除本次运行创建的全部规则。
class FirewallHelperClient final {
public:
    using StateCallback = std::function<void(FirewallHelperUpdate)>;
    using RuleCallback = std::function<void(FirewallRuleUpdate)>;
    using RecoveryProvider = std::function<std::vector<FirewallRuleIdentity>()>;
    using CleanupCallback = std::function<void()>;

    FirewallHelperClient(std::shared_ptr<ssh::IUiDispatcher> dispatcher,
                         StateCallback state_callback,
                         RuleCallback rule_callback,
                         RecoveryProvider recovery_provider,
                         CleanupCallback cleanup_callback);
    ~FirewallHelperClient();

    FirewallHelperClient(const FirewallHelperClient&) = delete;
    FirewallHelperClient& operator=(const FirewallHelperClient&) = delete;

    void SetEnabled(bool enabled);
    void SetRule(std::uint16_t local_port, bool enabled);
    [[nodiscard]] FirewallHelperState state() const noexcept;

    // 同步要求已提权的辅助进程删除本会话全部规则，并保证辅助进程已经退出。
    [[nodiscard]] bool Shutdown() noexcept;
    // UI 正常回调和退出同步收尾共用此队列，交换后清空，避免迟到回调重复删除新记录。
    [[nodiscard]] std::vector<FirewallRuleIdentity> TakeConfirmedCleanups();

private:
    friend struct FirewallHelperTestAccess;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// 仅在当前进程通过私有参数以防火墙辅助模式启动时返回退出码；main.cpp 随后直接
// 退出，不创建普通 UI。普通启动返回空值。
[[nodiscard]] std::optional<int> RunFirewallHelperIfRequested();

} // namespace SLink::app
