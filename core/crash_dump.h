#pragma once

// Installs a last-chance exception handler that writes a minidump (.dmp) to
// %APPDATA%\SyncPlay next to syncplay.log, so crashes in the field leave
// something to debug. Call once at startup.
void InstallCrashHandler();
