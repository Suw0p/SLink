// Windows 进程入口：优先识别私有防火墙辅助模式，普通模式再创建 Application。
// 入口不依赖当前工作目录，所有内部路径由 Application 通过 EXE 位置解析。
#include "app/application.hpp"
#include "app/firewall_helper.hpp"

#include <windows.h>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
    if (const auto helper_exit = SLink::app::RunFirewallHelperIfRequested()) {
        return *helper_exit;
    }
    SLink::app::Application application;
    return application.Run(instance, show_command);
}
