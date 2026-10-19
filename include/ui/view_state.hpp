#pragma once

#include <string>

namespace SLink::ui {

// 页面内短生命周期的展示状态；不写入数据库，也不代表服务器的全局在线状态。
enum class ViewPhase {
    Idle,
    Connecting,
    Loading,
    Ready,
    Empty,
    Disconnected,
    Error,
};

enum class ViewRecoveryAction {
    None,
    Retry,
    ManageServer,
};

struct ViewState {
    ViewPhase phase{ViewPhase::Idle};
    std::wstring message;
    ViewRecoveryAction action{ViewRecoveryAction::None};
};

} // namespace SLink::ui
