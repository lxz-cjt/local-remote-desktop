#pragma once

#include <windows.h>

#include "protocol.h"

class InputInjector {
public:
    InputInjector();
    void UpdateDesktopResolution();
    void InjectInput(const protocol::InputEvent& event);

private:
    uint32_t desktop_w_ = 0;
    uint32_t desktop_h_ = 0;

    POINT MapToDesktop(float x, float y) const;
    void InjectMouse(const protocol::InputEvent& event);
    void InjectKeyboard(const protocol::InputEvent& event);
    void InjectScroll(const protocol::InputEvent& event);
};
