// The few native_win32_posix.h functions the port's hooks call on Android.
#include <dlfcn.h>

#include "platform/native_win32_posix.h"

HWND GetForegroundWindow() { return nullptr; }  // no keyboard or mouse look: the hooks compare it with their window
SHORT GetAsyncKeyState(int) { return 0; }
HMODULE GetModuleHandleW(const wchar_t*) { return dlopen("librexruntime.so", RTLD_NOW | RTLD_NOLOAD); }
void* GetProcAddress(HMODULE module, const char* name) { return module ? dlsym(module, name) : nullptr; }
