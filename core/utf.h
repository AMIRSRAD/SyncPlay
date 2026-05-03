#pragma once

#include <string>

std::string Utf8FromWide(const std::wstring& value);
std::wstring WideFromUtf8(const std::string& value);
