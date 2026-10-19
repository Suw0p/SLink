#pragma once

#include <windows.h>
#include <memory>

namespace SLink::app {

// 应用程序的顶层生命周期对象。
//
// 具体子系统隐藏在 Impl 中，并按“先停止业务任务，再断开会话，最后销毁窗口”的
// 顺序关闭，避免后台线程在 UI 或凭据库销毁后继续回调。Run 会一直阻塞到主窗口退出。
class Application final {
public:
    Application();
    ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    int Run(HINSTANCE instance, int show_command);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace SLink::app
