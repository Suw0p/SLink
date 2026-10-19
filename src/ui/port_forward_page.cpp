// 端口转发规则输入、表格显示、代理开关和防火墙辅助进程开关的页面实现。
#include "ui/port_forward_page.hpp"

#include "ui/retained_ui.hpp"
#include "ui/win32_ui_internal.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cwctype>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>
#include <windowsx.h>

namespace SLink::ui {
namespace {

constexpr wchar_t kPageClass[] = L"SLink.PortForwardPage";
constexpr wchar_t kInputFrameClass[] = L"SLink.PortForwardInputFrame";
constexpr int kBindEditId = 5101;
constexpr int kTargetEditId = 5102;
constexpr int kHelperToggleId = 5103;
constexpr int kAddForwardId = 5104;
constexpr int kStopFirstId = 5200;
constexpr int kFirewallFirstId = 15200;
constexpr int kProxyFirstId = 25200;

bool RegisterClassOnce(HINSTANCE instance, const wchar_t* name, WNDPROC procedure) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = procedure;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = name;
    const ATOM atom = RegisterClassExW(&wc);
    return atom != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

std::wstring Trim(std::wstring value) {
    const auto non_space = [](wchar_t ch) {
        return !iswspace(ch);
    };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), non_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), non_space).base(), value.end());
    return value;
}

std::optional<std::uint16_t> ParsePort(std::wstring_view text) {
    if (text.empty() || text.size() > 5)
        return std::nullopt;
    unsigned value = 0;
    for (const wchar_t digit : text) {
        if (!iswdigit(digit))
            return std::nullopt;
        value = value * 10U + static_cast<unsigned>(digit - L'0');
    }
    if (value == 0 || value > 65535U)
        return std::nullopt;
    return static_cast<std::uint16_t>(value);
}

bool IsIpv4(std::wstring_view text) {
    int groups = 0;
    std::size_t begin = 0;
    while (begin <= text.size()) {
        const std::size_t end = text.find(L'.', begin);
        const std::wstring_view group = text.substr(
            begin, end == std::wstring_view::npos ? std::wstring_view::npos : end - begin);
        if (group.empty() || group.size() > 3)
            return false;
        unsigned value = 0;
        for (const wchar_t digit : group) {
            if (!iswdigit(digit))
                return false;
            value = value * 10U + static_cast<unsigned>(digit - L'0');
        }
        if (value > 255U)
            return false;
        ++groups;
        if (end == std::wstring_view::npos)
            break;
        begin = end + 1;
    }
    return groups == 4;
}

std::string NarrowIpv4(std::wstring_view value) {
    std::string result;
    result.reserve(value.size());
    for (const wchar_t character : value) {
        result.push_back(static_cast<char>(character));
    }
    return result;
}

LRESULT CALLBACK InputFrameProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_COMMAND:
        return SendMessageW(GetParent(window), message, wparam, lparam);
    case WM_CTLCOLOREDIT: {
        const HDC dc = reinterpret_cast<HDC>(wparam);
        SetTextColor(dc, detail::kTextColor);
        SetBkColor(dc, detail::kPanelAltColor);
        return reinterpret_cast<LRESULT>(detail::PanelAltThemeBrush());
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        const HDC dc = BeginPaint(window, &paint);
        RECT rect{};
        GetClientRect(window, &rect);
        const UINT dpi = detail::WindowDpi(window);
        const int diameter = detail::ScaleForDpi(8, dpi);
        const HBRUSH fill = CreateSolidBrush(detail::kPanelAltColor);
        const HPEN border =
            CreatePen(PS_SOLID, std::max(1, detail::ScaleForDpi(1, dpi)), detail::kBorderColor);
        const HGDIOBJ old_brush = SelectObject(dc, fill);
        const HGDIOBJ old_pen = SelectObject(dc, border);
        RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, diameter, diameter);
        SelectObject(dc, old_pen);
        SelectObject(dc, old_brush);
        DeleteObject(border);
        DeleteObject(fill);
        EndPaint(window, &paint);
        return 0;
    }
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

} // namespace

std::optional<PortForwardRequest> ParsePortForwardRequest(std::wstring bind_text,
                                                          std::wstring target_text) {
    const std::wstring bind = Trim(std::move(bind_text));
    const std::wstring target = Trim(std::move(target_text));
    PortForwardRequest request;
    const std::size_t bind_colon = bind.rfind(L':');
    if (bind_colon == std::wstring::npos) {
        const auto port = ParsePort(bind);
        if (!port)
            return std::nullopt;
        request.bind_address = "127.0.0.1";
        request.local_port = *port;
    } else {
        const std::wstring_view address(bind.data(), bind_colon);
        const auto port = ParsePort(std::wstring_view(bind).substr(bind_colon + 1));
        if (!port || (address != L"127.0.0.1" && address != L"0.0.0.0"))
            return std::nullopt;
        request.bind_address = NarrowIpv4(address);
        request.local_port = *port;
    }
    const std::size_t target_colon = target.rfind(L':');
    if (target_colon == std::wstring::npos)
        return std::nullopt;
    const std::wstring_view target_address(target.data(), target_colon);
    const auto target_port = ParsePort(std::wstring_view(target).substr(target_colon + 1));
    if (!target_port || !IsIpv4(target_address))
        return std::nullopt;
    request.target_address = NarrowIpv4(target_address);
    request.target_port = *target_port;
    return request;
}

// 页面 entries 是数据库/控制器状态的 UI 镜像。开关处于 busy 时禁止重复点击，异步
// 回调按 bind_address + port 更新对应行，避免同端口不同监听地址相互覆盖。
struct PortForwardPage::Impl {
    HINSTANCE instance{};
    HWND hwnd{};
    HWND bind_frame{};
    HWND bind_edit{};
    HWND target_frame{};
    HWND target_edit{};
    HFONT font{};
    detail::UiSurface surface;
    StartRequestedCallback start_requested;
    DeleteRequestedCallback delete_requested;
    ProxyRequestedCallback proxy_requested;
    FirewallHelperRequestedCallback helper_requested;
    FirewallRuleRequestedCallback firewall_requested;
    FirewallHelperUiState helper_state{FirewallHelperUiState::off};
    std::wstring helper_message;
    std::vector<PortForwardEntry> entries;
    std::size_t first_visible_entry{};
    std::size_t visible_entry_count{};
    int wheel_delta_remainder{};
    bool dragging_scroll_thumb{};
    int scroll_drag_offset{};
    RECT scroll_track{};
    RECT scroll_thumb{};

    [[nodiscard]] bool HasVerticalScroll() const noexcept {
        return visible_entry_count > 0 && entries.size() > visible_entry_count;
    }

    [[nodiscard]] std::size_t MaximumFirstVisible() const noexcept {
        return entries.size() > visible_entry_count ? entries.size() - visible_entry_count : 0U;
    }

    void ScrollRows(int rows) {
        const auto maximum = MaximumFirstVisible();
        const auto current = static_cast<long long>(first_visible_entry);
        first_visible_entry = static_cast<std::size_t>(
            std::clamp<long long>(current + rows, 0, static_cast<long long>(maximum)));
        RECT rect{};
        GetClientRect(hwnd, &rect);
        Layout(rect.right, rect.bottom);
    }

    [[nodiscard]] bool PointIn(const RECT& rect, POINT point) const noexcept {
        return point.x >= rect.left && point.x < rect.right && point.y >= rect.top &&
               point.y < rect.bottom;
    }

    void DragScrollThumb(int mouse_y) {
        if (!HasVerticalScroll())
            return;
        const int thumb_height = static_cast<int>(scroll_thumb.bottom - scroll_thumb.top);
        const int travel =
            std::max(0, static_cast<int>(scroll_track.bottom - scroll_track.top) - thumb_height);
        if (travel == 0)
            return;
        const int thumb_top = std::clamp(mouse_y - scroll_drag_offset,
                                         static_cast<int>(scroll_track.top),
                                         static_cast<int>(scroll_track.bottom) - thumb_height);
        const auto maximum = MaximumFirstVisible();
        const auto next =
            static_cast<std::size_t>((static_cast<long long>(thumb_top - scroll_track.top) *
                                          static_cast<long long>(maximum) +
                                      travel / 2) /
                                     travel);
        if (next == first_visible_entry)
            return;
        first_visible_entry = next;
        RECT rect{};
        GetClientRect(hwnd, &rect);
        Layout(rect.right, rect.bottom);
    }

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            self = static_cast<Impl*>(create->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->hwnd = window;
        }
        if (!self)
            return DefWindowProcW(window, message, wparam, lparam);
        switch (message) {
        case WM_SIZE:
            self->Layout(LOWORD(lparam), HIWORD(lparam));
            return 0;
        case WM_MOUSEMOVE:
            if (self->dragging_scroll_thumb) {
                self->DragScrollThumb(GET_Y_LPARAM(lparam));
                return 0;
            }
            self->surface.PointerMove({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
            return 0;
        case WM_MOUSELEAVE:
            self->surface.PointerLeave();
            return 0;
        case WM_LBUTTONDOWN: {
            const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            if (self->HasVerticalScroll() && self->PointIn(self->scroll_thumb, point)) {
                self->dragging_scroll_thumb = true;
                self->scroll_drag_offset = point.y - self->scroll_thumb.top;
                SetCapture(window);
                return 0;
            }
            if (self->HasVerticalScroll() && self->PointIn(self->scroll_track, point)) {
                self->ScrollRows(point.y < self->scroll_thumb.top
                                     ? -static_cast<int>(self->visible_entry_count)
                                     : static_cast<int>(self->visible_entry_count));
                return 0;
            }
            self->surface.PointerDown({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
            return 0;
        }
        case WM_LBUTTONUP: {
            if (self->dragging_scroll_thumb) {
                self->dragging_scroll_thumb = false;
                if (GetCapture() == window)
                    ReleaseCapture();
                return 0;
            }
            const auto clicked =
                self->surface.PointerUp({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
            if (!clicked)
                return 0;
            if (*clicked == kAddForwardId) {
                self->Submit();
            } else if (*clicked == kHelperToggleId) {
                const bool enable = self->helper_state == FirewallHelperUiState::off ||
                                    self->helper_state == FirewallHelperUiState::error;
                self->helper_state =
                    enable ? FirewallHelperUiState::starting : FirewallHelperUiState::stopping;
                if (self->helper_requested)
                    self->helper_requested(enable);
                RECT rect{};
                GetClientRect(self->hwnd, &rect);
                self->Layout(rect.right, rect.bottom);
            } else if (*clicked >= kProxyFirstId) {
                const std::size_t index = static_cast<std::size_t>(*clicked - kProxyFirstId);
                if (index < self->entries.size() && self->proxy_requested) {
                    auto& entry = self->entries[index];
                    entry.proxy_busy = true;
                    self->proxy_requested(
                        entry.bind_address, entry.local_port, !entry.proxy_enabled);
                    RECT rect{};
                    GetClientRect(self->hwnd, &rect);
                    self->Layout(rect.right, rect.bottom);
                }
            } else if (*clicked >= kFirewallFirstId) {
                const std::size_t index = static_cast<std::size_t>(*clicked - kFirewallFirstId);
                if (index < self->entries.size() && self->firewall_requested) {
                    auto& entry = self->entries[index];
                    entry.firewall_busy = true;
                    self->firewall_requested(
                        entry.bind_address, entry.local_port, !entry.firewall_open);
                    RECT rect{};
                    GetClientRect(self->hwnd, &rect);
                    self->Layout(rect.right, rect.bottom);
                }
            } else if (*clicked >= kStopFirstId) {
                const std::size_t index = static_cast<std::size_t>(*clicked - kStopFirstId);
                if (index < self->entries.size() && self->delete_requested)
                    self->delete_requested(self->entries[index].bind_address,
                                           self->entries[index].local_port);
            }
            return 0;
        }
        case WM_CAPTURECHANGED:
            self->dragging_scroll_thumb = false;
            self->surface.CancelPointer();
            return 0;
        case WM_MOUSEWHEEL: {
            if (!self->HasVerticalScroll())
                return 0;
            self->wheel_delta_remainder += GET_WHEEL_DELTA_WPARAM(wparam);
            const int steps = self->wheel_delta_remainder / WHEEL_DELTA;
            self->wheel_delta_remainder %= WHEEL_DELTA;
            if (steps != 0)
                self->ScrollRows(-steps * 3);
            return 0;
        }
        case WM_SETCURSOR:
            if (self->surface.hovered_id() == kAddForwardId ||
                self->surface.hovered_id() == kHelperToggleId ||
                self->surface.hovered_id() >= kStopFirstId) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            break;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            self->surface.Paint(detail::UiColor(detail::kWindowColor));
            return 0;
        case WM_NCDESTROY:
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            self->hwnd = nullptr;
            return DefWindowProcW(window, message, wparam, lparam);
        default:
            break;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    void Submit() {
        std::array<wchar_t, 128> bind_buffer{};
        std::array<wchar_t, 128> target_buffer{};
        GetWindowTextW(bind_edit, bind_buffer.data(), static_cast<int>(bind_buffer.size()));
        GetWindowTextW(target_edit, target_buffer.data(), static_cast<int>(target_buffer.size()));
        auto request = ParsePortForwardRequest(bind_buffer.data(), target_buffer.data());
        if (!request) {
            MessageBoxW(hwnd,
                        L"本机监听可填写：3000、127.0.0.1:3000 或 0.0.0.0:3000。\r\n"
                        L"转发目标请填写服务器 IPv4 和端口，例如 10.0.0.20:8080。",
                        L"端口转发地址无效",
                        MB_OK | MB_ICONWARNING);
            return;
        }
        if (start_requested)
            start_requested(std::move(*request));
    }

    void BuildSurface(int width, int height) {
        surface.ClearElements();
        const UINT dpi = detail::WindowDpi(hwnd);
        const float scale = static_cast<float>(dpi) / 96.0F;
        const float pad = 18.0F * scale;
        const float top = 16.0F * scale;
        const float input_height = 30.0F * scale;
        const float header_top = top + input_height + 18.0F * scale;
        const float header_height = 34.0F * scale;
        const float table_right = std::max(pad, static_cast<float>(width) - pad);
        const float table_width = std::max(0.0F, table_right - pad);
        const float first = pad + table_width * 0.26F;
        const float second = pad + table_width * 0.51F;
        const float third = pad + table_width * 0.78F;
        const float toggle_width = 42.0F * scale;
        const float toggle_height = 22.0F * scale;
        const float input_gap = 8.0F * scale;
        const float input_available =
            std::max(0.0F, static_cast<float>(width) - pad * 2.0F - input_gap);
        const float bind_width = input_available * 0.26F;
        const float target_width = input_available * 0.25F;
        const float add_left = pad + bind_width + input_gap + target_width + input_gap;
        const float add_right = add_left + input_height;
        const bool helper_on = helper_state == FirewallHelperUiState::running;
        const bool helper_transition = helper_state == FirewallHelperUiState::starting ||
                                       helper_state == FirewallHelperUiState::stopping;
        std::wstring state_text = helper_message;
        if (state_text.empty() && helper_state == FirewallHelperUiState::starting) {
            state_text = L"正在请求管理员授权...";
        } else if (state_text.empty() && helper_state == FirewallHelperUiState::stopping) {
            state_text = L"正在清理防火墙规则...";
        }
        surface.Add(
            {.role = helper_state == FirewallHelperUiState::error ? detail::UiRole::status
                                                                  : detail::UiRole::muted,
             .bounds = detail::UiRect((std::max)(second + 8.0F * scale, add_right + input_gap),
                                      top,
                                      table_right - 150.0F * scale,
                                      top + input_height),
             .text = std::move(state_text)});
        surface.Add({.role = detail::UiRole::label,
                     .bounds = detail::UiRect(table_right - 142.0F * scale,
                                              top,
                                              table_right - 54.0F * scale,
                                              top + input_height),
                     .text = L"防火墙管理",
                     .alignment = detail::UiTextAlign::trailing});
        surface.Add({.id = kHelperToggleId,
                     .role = detail::UiRole::toggle,
                     .bounds = detail::UiRect(table_right - toggle_width,
                                              top + (input_height - toggle_height) / 2.0F,
                                              table_right,
                                              top + (input_height + toggle_height) / 2.0F),
                     .selected = helper_on || helper_transition,
                     .enabled = !helper_transition});
        surface.Add({.id = kAddForwardId,
                     .role = detail::UiRole::icon_button,
                     .bounds = detail::UiRect(add_left, top, add_right, top + input_height),
                     .icon = detail::UiIcon::plus,
                     .alignment = detail::UiTextAlign::center,
                     .enabled = true});
        surface.Add({.role = detail::UiRole::panel,
                     .bounds = detail::UiRect(
                         pad, header_top, table_right, static_cast<float>(height) - pad)});
        surface.Add(
            {.role = detail::UiRole::label,
             .bounds = detail::UiRect(
                 pad + 10.0F * scale, header_top, first - 8.0F * scale, header_top + header_height),
             .text = L"本机监听"});
        surface.Add({.role = detail::UiRole::label,
                     .bounds = detail::UiRect(first + 10.0F * scale,
                                              header_top,
                                              second - 8.0F * scale,
                                              header_top + header_height),
                     .text = L"转发目标"});
        surface.Add({.role = detail::UiRole::label,
                     .bounds = detail::UiRect(second + 10.0F * scale,
                                              header_top,
                                              third - 8.0F * scale,
                                              header_top + header_height),
                     .text = L"代理状态"});
        surface.Add({.role = detail::UiRole::label,
                     .bounds = detail::UiRect(third + 10.0F * scale,
                                              header_top,
                                              table_right - 38.0F * scale,
                                              header_top + header_height),
                     .text = L"防火墙"});
        for (const float x : {first, second, third}) {
            surface.Add(
                {.role = detail::UiRole::separator,
                 .bounds = detail::UiRect(x, header_top, x + 1.0F, header_top + header_height)});
        }
        surface.Add(
            {.role = detail::UiRole::separator,
             .bounds = detail::UiRect(
                 pad, header_top + header_height - 1.0F, table_right, header_top + header_height)});
        float row_top = header_top + header_height;
        const float row_height = 38.0F * scale;
        const float body_bottom = static_cast<float>(height) - pad;
        visible_entry_count = static_cast<std::size_t>(
            std::max(0.0F, std::floor((body_bottom - row_top) / row_height)));
        first_visible_entry = std::min(first_visible_entry, MaximumFirstVisible());
        const bool show_scrollbar = HasVerticalScroll();
        const float scrollbar_width = 8.0F * scale;
        const float row_right =
            show_scrollbar ? table_right - scrollbar_width - 3.0F * scale : table_right;
        const std::size_t last_entry =
            std::min(entries.size(), first_visible_entry + visible_entry_count);
        for (std::size_t index = first_visible_entry; index < last_entry; ++index) {
            const auto& entry = entries[index];
            surface.Add(
                {.role = detail::UiRole::label,
                 .bounds = detail::UiRect(
                     pad + 10.0F * scale, row_top, first - 8.0F * scale, row_top + row_height),
                 .text = entry.local_endpoint});
            surface.Add(
                {.role = detail::UiRole::muted,
                 .bounds = detail::UiRect(
                     first + 10.0F * scale, row_top, second - 8.0F * scale, row_top + row_height),
                 .text = entry.target_endpoint});
            surface.Add(
                {.role = entry.failed ? detail::UiRole::status : detail::UiRole::muted,
                 .bounds = detail::UiRect(
                     second + 62.0F * scale, row_top, third - 8.0F * scale, row_top + row_height),
                 .text = entry.process,
                 .status_online = entry.running});
            surface.Add({.id = kProxyFirstId + static_cast<int>(index),
                         .role = detail::UiRole::toggle,
                         .bounds = detail::UiRect(second + 12.0F * scale,
                                                  row_top + 8.0F * scale,
                                                  second + 50.0F * scale,
                                                  row_top + row_height - 8.0F * scale),
                         .selected = entry.proxy_enabled,
                         .enabled = !entry.proxy_busy});
            if (entry.lan_listener) {
                surface.Add({.id = kFirewallFirstId + static_cast<int>(index),
                             .role = detail::UiRole::toggle,
                             .bounds = detail::UiRect(third + 12.0F * scale,
                                                      row_top + 8.0F * scale,
                                                      third + 50.0F * scale,
                                                      row_top + row_height - 8.0F * scale),
                             .selected = entry.firewall_open,
                             .enabled = helper_on && entry.running && !entry.firewall_busy});
            } else {
                surface.Add({.role = detail::UiRole::muted,
                             .bounds = detail::UiRect(third + 10.0F * scale,
                                                      row_top,
                                                      row_right - 38.0F * scale,
                                                      row_top + row_height),
                             .text = L"仅本机"});
            }
            surface.Add({.id = kStopFirstId + static_cast<int>(index),
                         .role = detail::UiRole::icon_button,
                         .bounds = detail::UiRect(row_right - 36.0F * scale,
                                                  row_top + 5.0F * scale,
                                                  row_right - 6.0F * scale,
                                                  row_top + row_height - 5.0F * scale),
                         .icon = detail::UiIcon::close,
                         .alignment = detail::UiTextAlign::center,
                         .enabled = true});
            row_top += row_height;
        }
        scroll_track = {};
        scroll_thumb = {};
        if (show_scrollbar) {
            const int track_left =
                static_cast<int>(std::lround(table_right - scrollbar_width - 1.0F * scale));
            scroll_track = {
                track_left,
                static_cast<int>(std::lround(header_top + header_height + 2.0F * scale)),
                static_cast<int>(std::lround(table_right - 1.0F * scale)),
                static_cast<int>(std::lround(body_bottom - 2.0F * scale))};
            const int track_height =
                std::max(1, static_cast<int>(scroll_track.bottom - scroll_track.top));
            const int minimum_thumb = detail::ScaleForDpi(18, dpi);
            const int thumb_height =
                std::clamp(static_cast<int>(static_cast<long long>(track_height) *
                                            static_cast<long long>(visible_entry_count) /
                                            static_cast<long long>(entries.size())),
                           std::min(minimum_thumb, track_height),
                           track_height);
            const int travel = std::max(0, track_height - thumb_height);
            const auto maximum = MaximumFirstVisible();
            const int thumb_offset =
                maximum > 0 ? static_cast<int>(static_cast<long long>(travel) *
                                               static_cast<long long>(first_visible_entry) /
                                               static_cast<long long>(maximum))
                            : 0;
            scroll_thumb = {scroll_track.left + 1,
                            scroll_track.top + thumb_offset + 1,
                            scroll_track.right - 1,
                            scroll_track.top + thumb_offset + thumb_height - 1};
            surface.Add({.role = detail::UiRole::panel,
                         .bounds = detail::UiRect(static_cast<float>(scroll_track.left),
                                                  static_cast<float>(scroll_track.top),
                                                  static_cast<float>(scroll_track.right),
                                                  static_cast<float>(scroll_track.bottom))});
            surface.Add({.role = detail::UiRole::icon_button,
                         .bounds = detail::UiRect(static_cast<float>(scroll_thumb.left),
                                                  static_cast<float>(scroll_thumb.top),
                                                  static_cast<float>(scroll_thumb.right),
                                                  static_cast<float>(scroll_thumb.bottom))});
        }
    }

    void Layout(int width, int height) {
        if (width <= 0 || height <= 0)
            return;
        const UINT dpi = detail::WindowDpi(hwnd);
        const int pad = detail::ScaleForDpi(18, dpi);
        const int top = detail::ScaleForDpi(16, dpi);
        const int gap = detail::ScaleForDpi(8, dpi);
        const int frame_height = detail::ScaleForDpi(30, dpi);
        const int available = std::max(0, width - pad * 2 - gap);
        const int bind_width = available * 26 / 100;
        const int target_width = available * 25 / 100;
        MoveWindow(bind_frame, pad, top, bind_width, frame_height, TRUE);
        MoveWindow(target_frame, pad + bind_width + gap, top, target_width, frame_height, TRUE);
        const auto place_edit = [&](HWND edit, int frame_width) {
            const int inset = detail::ScaleForDpi(2, dpi);
            HDC dc = GetDC(edit);
            const HGDIOBJ previous = dc && font ? SelectObject(dc, font) : nullptr;
            TEXTMETRICW metrics{};
            if (dc)
                GetTextMetricsW(dc, &metrics);
            if (dc && previous)
                SelectObject(dc, previous);
            if (dc)
                ReleaseDC(edit, dc);
            const int text_height =
                metrics.tmHeight > 0 ? metrics.tmHeight : detail::ScaleForDpi(16, dpi);
            const int edit_height =
                std::min(frame_height - inset * 2, text_height + detail::ScaleForDpi(4, dpi));
            const int edit_top = std::max(0, (frame_height - edit_height) / 2);
            MoveWindow(edit,
                       inset + detail::ScaleForDpi(7, dpi),
                       edit_top,
                       std::max(0, frame_width - inset * 2 - detail::ScaleForDpi(7, dpi)),
                       std::max(0, edit_height),
                       TRUE);
        };
        place_edit(bind_edit, bind_width);
        place_edit(target_edit, target_width);
        detail::SetRoundedControlRegion(
            bind_frame, bind_width, frame_height, detail::ScaleForDpi(8, dpi));
        detail::SetRoundedControlRegion(
            target_frame, target_width, frame_height, detail::ScaleForDpi(8, dpi));
        surface.Resize(width, height);
        BuildSurface(width, height);
        InvalidateRect(hwnd, nullptr, FALSE);
    }
};

PortForwardPage::PortForwardPage() : impl_(std::make_unique<Impl>()) {}
PortForwardPage::~PortForwardPage() {
    if (impl_->hwnd)
        DestroyWindow(impl_->hwnd);
    DeleteObject(impl_->font);
}

bool PortForwardPage::Create(HWND parent, HINSTANCE instance) {
    impl_->instance = instance;
    if (!RegisterClassOnce(instance, kPageClass, &Impl::WindowProc) ||
        !RegisterClassOnce(instance, kInputFrameClass, &InputFrameProc))
        return false;
    impl_->hwnd = CreateWindowExW(0,
                                  kPageClass,
                                  L"",
                                  WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                                  0,
                                  0,
                                  0,
                                  0,
                                  parent,
                                  nullptr,
                                  instance,
                                  impl_.get());
    if (!impl_->hwnd || !impl_->surface.Initialize(impl_->hwnd))
        return false;

    const auto create_input = [&](int id, HWND& frame, HWND& edit, const wchar_t* cue) {
        frame = CreateWindowExW(0,
                                kInputFrameClass,
                                L"",
                                WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
                                0,
                                0,
                                0,
                                0,
                                impl_->hwnd,
                                nullptr,
                                instance,
                                nullptr);
        edit = CreateWindowExW(0,
                               L"EDIT",
                               L"",
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                               0,
                               0,
                               0,
                               0,
                               frame,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                               instance,
                               nullptr);
        if (edit) {
            detail::ApplySelfDrawnTheme(edit);
            SendMessageW(edit, EM_SETLIMITTEXT, 63, 0);
            SendMessageW(edit, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(cue));
        }
        return frame && edit;
    };
    if (!create_input(kBindEditId, impl_->bind_frame, impl_->bind_edit, L"本地监听地址") ||
        !create_input(
            kTargetEditId, impl_->target_frame, impl_->target_edit, L"服务器地址 ip:port"))
        return false;
    const UINT dpi = detail::WindowDpi(parent);
    impl_->font = detail::CreateUiFont(dpi, 10);
    detail::SetControlFont(impl_->bind_edit, impl_->font);
    detail::SetControlFont(impl_->target_edit, impl_->font);
    return true;
}

void PortForwardPage::SetBounds(int x, int y, int width, int height) {
    MoveWindow(impl_->hwnd, x, y, width, height, TRUE);
}
void PortForwardPage::Show(bool visible) {
    ShowWindow(impl_->hwnd, visible ? SW_SHOW : SW_HIDE);
}
void PortForwardPage::ApplyTheme() {
    detail::ApplyThemeToWindowTree(impl_->hwnd);
    RECT rect{};
    GetClientRect(impl_->hwnd, &rect);
    impl_->Layout(rect.right, rect.bottom);
    RedrawWindow(impl_->hwnd,
                 nullptr,
                 nullptr,
                 RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}
bool PortForwardPage::HandleEnterKey(HWND source) {
    if (source != impl_->bind_edit && source != impl_->target_edit && source != impl_->bind_frame &&
        source != impl_->target_frame)
        return false;
    impl_->Submit();
    return true;
}
void PortForwardPage::SetStartRequestedCallback(StartRequestedCallback callback) {
    impl_->start_requested = std::move(callback);
}
void PortForwardPage::SetDeleteRequestedCallback(DeleteRequestedCallback callback) {
    impl_->delete_requested = std::move(callback);
}
void PortForwardPage::SetProxyRequestedCallback(ProxyRequestedCallback callback) {
    impl_->proxy_requested = std::move(callback);
}
void PortForwardPage::SetFirewallHelperRequestedCallback(FirewallHelperRequestedCallback callback) {
    impl_->helper_requested = std::move(callback);
}
void PortForwardPage::SetFirewallRuleRequestedCallback(FirewallRuleRequestedCallback callback) {
    impl_->firewall_requested = std::move(callback);
}
void PortForwardPage::SetFirewallHelperState(FirewallHelperUiState state, std::wstring message) {
    impl_->helper_state = state;
    impl_->helper_message = std::move(message);
    if (state == FirewallHelperUiState::off || state == FirewallHelperUiState::error) {
        for (auto& entry : impl_->entries) {
            entry.firewall_open = false;
            entry.firewall_busy = false;
        }
    }
    RECT rect{};
    GetClientRect(impl_->hwnd, &rect);
    impl_->Layout(rect.right, rect.bottom);
}
void PortForwardPage::SetFirewallRuleState(std::string_view bind_address,
                                           std::uint16_t local_port,
                                           bool enabled,
                                           bool succeeded,
                                           std::wstring message) {
    const auto found = std::find_if(
        impl_->entries.begin(), impl_->entries.end(), [&](const PortForwardEntry& value) {
            return value.bind_address == bind_address && value.local_port == local_port;
        });
    if (found != impl_->entries.end()) {
        found->firewall_busy = false;
        if (succeeded)
            found->firewall_open = enabled;
    }
    if (!succeeded && !message.empty()) {
        MessageBoxW(impl_->hwnd, message.c_str(), L"Windows 防火墙", MB_OK | MB_ICONERROR);
    }
    RECT rect{};
    GetClientRect(impl_->hwnd, &rect);
    impl_->Layout(rect.right, rect.bottom);
}
void PortForwardPage::SetProxyState(std::string_view bind_address,
                                    std::uint16_t local_port,
                                    bool enabled,
                                    bool running,
                                    bool failed,
                                    std::wstring message) {
    const auto found = std::find_if(
        impl_->entries.begin(), impl_->entries.end(), [&](const PortForwardEntry& value) {
            return value.bind_address == bind_address && value.local_port == local_port;
        });
    if (found != impl_->entries.end()) {
        found->proxy_enabled = enabled;
        found->proxy_busy = false;
        found->running = running;
        found->failed = failed;
        found->process = std::move(message);
    }
    RECT rect{};
    GetClientRect(impl_->hwnd, &rect);
    impl_->Layout(rect.right, rect.bottom);
}
void PortForwardPage::UpsertEntry(PortForwardEntry entry) {
    const auto found = std::find_if(
        impl_->entries.begin(), impl_->entries.end(), [&](const PortForwardEntry& value) {
            return value.bind_address == entry.bind_address && value.local_port == entry.local_port;
        });
    if (found == impl_->entries.end()) {
        impl_->entries.push_back(std::move(entry));
        std::ranges::sort(impl_->entries,
                          [](const PortForwardEntry& left, const PortForwardEntry& right) {
                              if (left.local_port != right.local_port)
                                  return left.local_port < right.local_port;
                              return left.bind_address < right.bind_address;
                          });
    } else {
        entry.firewall_open = entry.lan_listener && found->firewall_open;
        entry.firewall_busy = entry.lan_listener && found->firewall_busy;
        *found = std::move(entry);
    }
    RECT rect{};
    GetClientRect(impl_->hwnd, &rect);
    impl_->Layout(rect.right, rect.bottom);
}
void PortForwardPage::RemoveEntry(std::string_view bind_address, std::uint16_t local_port) {
    std::erase_if(impl_->entries, [&](const PortForwardEntry& value) {
        return value.bind_address == bind_address && value.local_port == local_port;
    });
    RECT rect{};
    GetClientRect(impl_->hwnd, &rect);
    impl_->Layout(rect.right, rect.bottom);
}
HWND PortForwardPage::hwnd() const noexcept {
    return impl_->hwnd;
}

} // namespace SLink::ui
