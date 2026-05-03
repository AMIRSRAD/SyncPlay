#pragma once

#include <windows.h>
#include <string>

std::wstring openFileDialog(HWND owner, const wchar_t* filter, const wchar_t* title);
