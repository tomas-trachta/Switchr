#include "CrashHandler.h"
#include "Namespaces.h"

#include <windows.h>
#include <dbghelp.h>
#include <shlobj.h>
#include <malloc.h>
#include <exception>
#include <string>

#pragma comment(lib, "dbghelp.lib")

namespace {

std::wstring CrashDir() {
    PWSTR roaming = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &roaming)))
        return {};

    std::wstring dir(roaming);
    CoTaskMemFree(roaming);

    dir += L"\\Switchr";
    CreateDirectoryW(dir.c_str(), nullptr);
    dir += L"\\crashes";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::wstring TimestampNow() {
    SYSTEMTIME t;
    GetLocalTime(&t);

    wchar_t buf[32];
    swprintf_s(buf, L"%04u%02u%02u-%02u%02u%02u",
        t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return buf;
}

std::wstring ExceptionCodeName(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:      return L"EXCEPTION_ACCESS_VIOLATION";
        case EXCEPTION_STACK_OVERFLOW:        return L"EXCEPTION_STACK_OVERFLOW";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return L"EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:    return L"EXCEPTION_INT_DIVIDE_BY_ZERO";
        case EXCEPTION_ILLEGAL_INSTRUCTION:   return L"EXCEPTION_ILLEGAL_INSTRUCTION";
        case EXCEPTION_PRIV_INSTRUCTION:      return L"EXCEPTION_PRIV_INSTRUCTION";
        case EXCEPTION_IN_PAGE_ERROR:         return L"EXCEPTION_IN_PAGE_ERROR";
        default: {
            wchar_t buf[16];
            swprintf_s(buf, L"0x%08X", code);
            return buf;
        }
    }
}

// A crashing address' owning module and its offset within it; HMODULE is the
// module's load base, so the subtraction below is just pointer arithmetic.
std::wstring ModuleAndOffset(void* address) {
    HMODULE mod = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            static_cast<LPCWSTR>(address), &mod) || !mod) {
        return L"<unknown module>";
    }

    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(mod, path, MAX_PATH);

    const wchar_t* name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;

    wchar_t buf[64];
    swprintf_s(buf, L"+0x%p",
        reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(address) -
                                 reinterpret_cast<uintptr_t>(mod)));
    return std::wstring(name) + buf;
}

bool WriteTextFile(const std::wstring& path, const std::wstring& text) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(),
                                 nullptr, 0, nullptr, nullptr);
    std::string utf8((size_t)n, 0);
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(),
                         utf8.data(), n, nullptr, nullptr);

    DWORD written = 0;
    WriteFile(h, utf8.data(), (DWORD)utf8.size(), &written, nullptr);
    CloseHandle(h);
    return true;
}

void WriteCrashLog(const std::wstring& path, EXCEPTION_POINTERS* info) {
    OSVERSIONINFOEXW os{ sizeof(os) };
#pragma warning(push)
#pragma warning(disable : 4996) // GetVersionEx is deprecated but fine for a best-effort report.
    GetVersionExW(reinterpret_cast<OSVERSIONINFOW*>(&os));
#pragma warning(pop)

    std::wstring text = L"Switchr crash report\r\n";
    text += L"Time: " + TimestampNow() + L"\r\n";
    text += L"OS: " + std::to_wstring(os.dwMajorVersion) + L"." +
             std::to_wstring(os.dwMinorVersion) + L" build " +
             std::to_wstring(os.dwBuildNumber) + L"\r\n";

    if (info && info->ExceptionRecord) {
        void* addr = info->ExceptionRecord->ExceptionAddress;
        text += L"Exception: " + ExceptionCodeName(info->ExceptionRecord->ExceptionCode) + L"\r\n";
        text += L"Location: " + ModuleAndOffset(addr) + L"\r\n";
    } else {
        text += L"Exception: uncaught C++ exception (no SEH context available)\r\n";
    }

    WriteTextFile(path, text);
}

void WriteMiniDump(const std::wstring& path, EXCEPTION_POINTERS* info) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;

    MINIDUMP_EXCEPTION_INFORMATION mei{};
    mei.ThreadId          = GetCurrentThreadId();
    mei.ExceptionPointers = info;
    mei.ClientPointers    = FALSE;

    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), h,
        static_cast<MINIDUMP_TYPE>(MiniDumpWithDataSegs | MiniDumpWithUnloadedModules),
        info ? &mei : nullptr, nullptr, nullptr);

    CloseHandle(h);
}

void RestoreWindows() {
    __try {
        Ns::EmergencyRestore();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // A second fault here means Ns state itself is corrupted; nothing
        // more we can safely do for window visibility.
    }
}

void ReportAndDie(EXCEPTION_POINTERS* info) {
    RestoreWindows();

    std::wstring dir  = CrashDir();
    std::wstring stamp = TimestampNow();

    if (!dir.empty()) {
        WriteMiniDump(dir + L"\\crash-" + stamp + L".dmp", info);
        WriteCrashLog(dir + L"\\crash-" + stamp + L".txt", info);
    }

    std::wstring msg =
        L"Switchr crashed and had to close.\n\n"
        L"Any windows it had hidden in inactive namespaces have been restored.\n\n";
    msg += dir.empty()
        ? L"A crash report could not be saved."
        : L"A crash report was saved to:\n" + dir + L"\n\nPlease attach it when filing a bug report.";

    MessageBoxW(nullptr, msg.c_str(), L"Switchr", MB_ICONERROR | MB_OK);

    TerminateProcess(GetCurrentProcess(), 1);
}

LONG WINAPI TopLevelFilter(EXCEPTION_POINTERS* info) {
    if (info && info->ExceptionRecord &&
        info->ExceptionRecord->ExceptionCode == EXCEPTION_STACK_OVERFLOW) {
        _resetstkoflw();
    }

    ReportAndDie(info);
    return EXCEPTION_EXECUTE_HANDLER; // unreachable — ReportAndDie terminates.
}

void TerminateHandler() {
    ReportAndDie(nullptr);
}

} // namespace

void CrashHandler::Install() {
    SetUnhandledExceptionFilter(TopLevelFilter);
    std::set_terminate(TerminateHandler);
}
