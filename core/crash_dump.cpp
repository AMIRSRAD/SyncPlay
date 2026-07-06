#include "crash_dump.h"

#include <windows.h>
#include <dbghelp.h>

#include <cstdio>
#include <ctime>

// Crash handling runs when the process is already in an undefined state, so
// everything here sticks to static buffers and straight Win32 calls — no CRT
// allocations, no C++ objects.

static LONG WINAPI WriteCrashDump(EXCEPTION_POINTERS* info) {
    wchar_t dir[MAX_PATH]{};
    const DWORD n = GetEnvironmentVariableW(L"APPDATA", dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return EXCEPTION_CONTINUE_SEARCH;
    wchar_t path[MAX_PATH]{};

    _snwprintf_s(path, _TRUNCATE, L"%s\\SyncPlay", dir);
    CreateDirectoryW(path, nullptr);

    SYSTEMTIME st{};
    GetLocalTime(&st);
    _snwprintf_s(path, _TRUNCATE, L"%s\\SyncPlay\\crash-%04u%02u%02u-%02u%02u%02u.dmp",
                 dir, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return EXCEPTION_CONTINUE_SEARCH;

    MINIDUMP_EXCEPTION_INFORMATION mei{};
    mei.ThreadId = GetCurrentThreadId();
    mei.ExceptionPointers = info;
    mei.ClientPointers = FALSE;

    // Normal + data segments + handle data: small dumps that still resolve a
    // stack trace and globals in a debugger.
    const MINIDUMP_TYPE type = static_cast<MINIDUMP_TYPE>(
        MiniDumpNormal | MiniDumpWithDataSegs | MiniDumpWithHandleData);
    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, type,
                      info ? &mei : nullptr, nullptr, nullptr);
    CloseHandle(file);
    return EXCEPTION_EXECUTE_HANDLER; // terminate; the dump is written
}

void InstallCrashHandler() {
    SetUnhandledExceptionFilter(WriteCrashDump);
}
