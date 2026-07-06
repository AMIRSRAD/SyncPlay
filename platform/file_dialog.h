#pragma once

#include <windows.h>
#include <string>
#include <vector>

std::wstring openFileDialog(HWND owner, const wchar_t* filter, const wchar_t* title);
// Multi-select variant: returns every chosen path (empty on cancel).
std::vector<std::wstring> openFileDialogMulti(HWND owner, const wchar_t* filter,
                                              const wchar_t* title);
