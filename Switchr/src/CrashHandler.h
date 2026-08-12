#pragma once

// Installs a top-level unhandled-exception filter so a crash doesn't leave
// windows stranded in inactive namespaces. On crash it un-hides every managed
// window (see Ns::EmergencyRestore), writes a minidump + text summary to
// %APPDATA%\Switchr\crashes, and terminates the process.
namespace CrashHandler {

void Install();

} // namespace CrashHandler
