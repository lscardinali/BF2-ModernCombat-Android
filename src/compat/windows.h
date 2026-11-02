// The Win32 types the PC port's sources take from <windows.h> (DXVK native's on Linux).
#pragma once
#include <cstddef>
#include <cstdint>

using BYTE = uint8_t;
using WORD = uint16_t;
using SHORT = int16_t;
using DWORD = uint32_t;
using LONG = int32_t;
using UINT = unsigned int;
using BOOL = int;
using SIZE_T = size_t;
using HRESULT = int32_t;
using COLORREF = uint32_t;
using HANDLE = void*;
using HWND = void*;
using HDC = void*;
using HMODULE = void*;
#define TRUE 1
#define FALSE 0
#define WINAPI
struct RECT { LONG left, top, right, bottom; };
struct POINT { LONG x, y; };
struct SIZE { LONG cx, cy; };
