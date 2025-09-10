#include <string.h>
#include <windows.h>
#include <stdint.h>

struct KeyboardState {
    uint8_t curr[256];
    uint8_t prev[256];
};

KeyboardState g_keys;

int Input_NormalizeVK(WPARAM vk, LPARAM lParam) {
    if (vk == VK_SHIFT) {
        unsigned int sc = (lParam >> 16) & 0xFF;
        unsigned int ex = MapVirtualKey(sc, MAPVK_VSC_TO_VK_EX);
        if (ex == VK_LSHIFT || ex == VK_RSHIFT) return (int)ex;
        return VK_SHIFT;
    }
    if (vk == VK_CONTROL) {
        bool extended = ((lParam >> 24) & 1) ? TRUE : FALSE;
        return extended ? VK_RCONTROL : VK_LCONTROL;
    }
    if (vk == VK_MENU) { // ALT
        bool extended = ((lParam >> 24) & 1) ? TRUE : FALSE;
        return extended ? VK_RMENU : VK_LMENU;
    }
    return (int)vk;
}

void Input_UpdateAggregates(void) {
    g_keys.curr[VK_SHIFT] = (g_keys.curr[VK_LSHIFT] || g_keys.curr[VK_RSHIFT]) ? 1 : 0;
    g_keys.curr[VK_CONTROL] = (g_keys.curr[VK_LCONTROL] || g_keys.curr[VK_RCONTROL]) ? 1 : 0;
    g_keys.curr[VK_MENU] = (g_keys.curr[VK_LMENU] || g_keys.curr[VK_RMENU]) ? 1 : 0;
}

void Input_SetKey(int vk, int down) {
    if (vk < 0 || vk > 255) return;
    g_keys.curr[vk] = (down ? 1 : 0);

    if (vk == VK_LSHIFT || vk == VK_RSHIFT ||
        vk == VK_LCONTROL || vk == VK_RCONTROL ||
        vk == VK_LMENU || vk == VK_RMENU) {
        Input_UpdateAggregates();
    }
}

void Input_Init(void) {
    memset(&g_keys, 0, sizeof(g_keys));
}

void Input_BeginFrame(void) {
    memcpy(g_keys.prev, g_keys.curr, sizeof(g_keys.curr));
}

void Input_Shutdown(void) {
    memset(&g_keys, 0, sizeof(g_keys));
}

void Input_ClearAll(void) {
    memset(g_keys.curr, 0, sizeof(g_keys.curr));
    memset(g_keys.prev, 0, sizeof(g_keys.prev));
}

void Input_HandleKeyMsg(unsigned int msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        int vk = Input_NormalizeVK(wParam, lParam);
        Input_SetKey(vk, 1);
    } break;
    case WM_KEYUP:
    case WM_SYSKEYUP: {
        int vk = Input_NormalizeVK(wParam, lParam);
        Input_SetKey(vk, 0);
    } break;
    default:
        break;
    }
}

void Input_HandleKeyWParam(UINT msg, WPARAM wParam) {
    switch (msg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        Input_SetKey((int)wParam, 1);
        break;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        Input_SetKey((int)wParam, 0);
        break;
    default:
        break;
    }
}

int Input_IsDown(int vk) {
    if (vk < 0 || vk > 255) return 0;
    return g_keys.curr[vk] ? 1 : 0;
}

int Input_WasDown(int vk) {
    if (vk < 0 || vk > 255) return 0;
    return g_keys.prev[vk] ? 1 : 0;
}

int Input_IsPressed(int vk) {
    if (vk < 0 || vk > 255) return 0;
    return (g_keys.curr[vk] && !g_keys.prev[vk]) ? 1 : 0;
}

int Input_IsReleased(int vk) {
    if (vk < 0 || vk > 255) return 0;
    return (!g_keys.curr[vk] && g_keys.prev[vk]) ? 1 : 0;
}
