#pragma once

#include <windows.h>

#include "common/types.h"

// Makes the title's top-level window behave like a native desktop game window: normal resizable
// frame, sensible initial size, F11 / Alt+Enter borderless fullscreen, closing the window exits.
//
// The title keeps rendering at its own (console) resolution; the presenter scales to the window's
// client area, so resizing never reaches the title's swapchain logic.

void window_init(const wchar_t* display_name);

HWND window_main();
void window_client_size(u32* width, u32* height);
bool window_is_fullscreen();
void window_toggle_fullscreen();
