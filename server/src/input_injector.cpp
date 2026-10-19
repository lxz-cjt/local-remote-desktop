#include "input_injector.h"

#include <algorithm>
#include <cmath>

InputInjector::InputInjector() {
    UpdateDesktopResolution();
}

void InputInjector::UpdateDesktopResolution() {
    desktop_w_ = static_cast<uint32_t>(std::max(1, GetSystemMetrics(SM_CXSCREEN)));
    desktop_h_ = static_cast<uint32_t>(std::max(1, GetSystemMetrics(SM_CYSCREEN)));
}

POINT InputInjector::MapToDesktop(float x, float y) const {
    if (!std::isfinite(x)) x = 0.0f;
    if (!std::isfinite(y)) y = 0.0f;
    x = std::max(0.0f, std::min(1.0f, x));
    y = std::max(0.0f, std::min(1.0f, y));
    POINT point{};
    point.x = static_cast<LONG>(x * static_cast<float>(desktop_w_ - 1));
    point.y = static_cast<LONG>(y * static_cast<float>(desktop_h_ - 1));
    return point;
}

void InputInjector::InjectInput(const protocol::InputEvent& event) {
    switch (event.event_type) {
        case 0: InjectMouse(event); break;
        case 1: InjectKeyboard(event); break;
        case 2: InjectScroll(event); break;
        default: break;
    }
}

void InputInjector::InjectMouse(const protocol::InputEvent& event) {
    if (event.mouse_action > 2 || event.mouse_button > 2) return;
    const POINT point = MapToDesktop(event.norm_x, event.norm_y);
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dx = static_cast<LONG>((static_cast<uint64_t>(point.x) * 65535) / (desktop_w_ - 1));
    input.mi.dy = static_cast<LONG>((static_cast<uint64_t>(point.y) * 65535) / (desktop_h_ - 1));
    input.mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE;

    if (event.mouse_action != 2) {
        static const DWORD down_flags[] = {MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_MIDDLEDOWN};
        static const DWORD up_flags[] = {MOUSEEVENTF_LEFTUP, MOUSEEVENTF_RIGHTUP, MOUSEEVENTF_MIDDLEUP};
        input.mi.dwFlags |= event.mouse_action == 0
            ? down_flags[event.mouse_button] : up_flags[event.mouse_button];
    }
    SendInput(1, &input, sizeof(input));
}

void InputInjector::InjectKeyboard(const protocol::InputEvent& event) {
    if (event.key_code == 0 || event.key_code > 0xFFFF) return;
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = static_cast<WORD>(event.key_code);
    input.ki.dwFlags = event.key_pressed == 0 ? KEYEVENTF_KEYUP : 0;
    SendInput(1, &input, sizeof(input));
}

void InputInjector::InjectScroll(const protocol::InputEvent& event) {
    const POINT point = MapToDesktop(event.norm_x, event.norm_y);
    INPUT move{};
    move.type = INPUT_MOUSE;
    move.mi.dx = static_cast<LONG>((static_cast<uint64_t>(point.x) * 65535) / (desktop_w_ - 1));
    move.mi.dy = static_cast<LONG>((static_cast<uint64_t>(point.y) * 65535) / (desktop_h_ - 1));
    move.mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE;
    SendInput(1, &move, sizeof(move));

    const int vertical = std::max(-100, std::min(100, static_cast<int>(event.scroll_dy)));
    const int horizontal = std::max(-100, std::min(100, static_cast<int>(event.scroll_dx)));
    INPUT scroll{};
    scroll.type = INPUT_MOUSE;
    if (vertical != 0) {
        scroll.mi.dwFlags = MOUSEEVENTF_WHEEL;
        scroll.mi.mouseData = static_cast<DWORD>(vertical * WHEEL_DELTA);
        SendInput(1, &scroll, sizeof(scroll));
    }
    if (horizontal != 0) {
        scroll.mi.dwFlags = MOUSEEVENTF_HWHEEL;
        scroll.mi.mouseData = static_cast<DWORD>(horizontal * WHEEL_DELTA);
        SendInput(1, &scroll, sizeof(scroll));
    }
}
