#include "Debug.hpp"
#include "LogRouter.hpp"
#include "Utils/UserDataPath.hpp"

static LogMessage CreateMessage(LogLevel level, const std::string& text, const std::string& channel) {
    LogMessage msg;
    msg.level = level;
    msg.text = text;
    msg.channel = channel;
    msg.timestamp = std::chrono::system_clock::now();
    msg.threadId = std::this_thread::get_id();
    return msg;
}

static void InstallCrashHandler();

void Debug::Initialize(const std::string& productName, bool consoleOutput) {
    UserDataPath::SetProductName(productName);
    LogRouter::Instance().SetProductName(productName);
    LogRouter::Instance().Start(consoleOutput);
    InstallCrashHandler();
}

void Debug::Shutdown() {
	LogRouter::Instance().Stop();
}

void Debug::Log(const std::string& msg, const std::string& channel) {
    LogRouter::Instance().Enqueue(CreateMessage(LogLevel::Info, msg, channel));
}

void Debug::LogWarning(const std::string& msg, const std::string& channel) {
    LogRouter::Instance().Enqueue(CreateMessage(LogLevel::Warning, msg, channel));
}

void Debug::LogError(const std::string& msg, const std::string& channel) {
    LogRouter::Instance().Enqueue(CreateMessage(LogLevel::Error, msg, channel));
}

void Debug::LogCritical(const std::string& msg, const std::string& channel) {
    LogRouter::Instance().Enqueue(CreateMessage(LogLevel::Critical, msg, channel));
}

// ---------------------------------------------------------------------------------------------------------------------
// Crash report: the log is written by a worker thread, so a crash loses its last lines and says nothing about where it
// happened. On an unhandled exception this writes, next to the logs, crash_<time>_PID<pid>.txt (exception, faulting
// module + offset, symbolised stack of the crashing thread; symbols come from the .pdb next to the .exe if it was
// copied along) and a .dmp minidump (open in Visual Studio with the same build's .pdb).
// ---------------------------------------------------------------------------------------------------------------------
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#include <cstdio>
#include <ctime>
#pragma comment(lib, "dbghelp.lib")

static wchar_t g_crashBase[MAX_PATH] = {}; // full path without extension, built at install time (no allocation on crash)
static volatile LONG g_crashing = 0;

static void WriteModuleOffset(FILE* f, DWORD64 addr)
{
    HMODULE mod = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(addr), &mod) && mod) {
        GetModuleFileNameA(mod, name, MAX_PATH);
        const char* base = strrchr(name, '\\');
        fprintf(f, "%s+0x%llx", base ? base + 1 : name,
            static_cast<unsigned long long>(addr - reinterpret_cast<DWORD64>(mod)));
    } else {
        fprintf(f, "0x%llx", static_cast<unsigned long long>(addr));
    }
}

static void WriteCrashReport(EXCEPTION_POINTERS* ep)
{
    if (InterlockedExchange(&g_crashing, 1) != 0) return; // a second thread crashing meanwhile: first report wins
    if (!g_crashBase[0]) return;

    wchar_t path[MAX_PATH + 8];
    swprintf(path, MAX_PATH + 8, L"%s.dmp", g_crashBase);
    HANDLE dmp = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (dmp != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION info{ GetCurrentThreadId(), ep, FALSE };
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), dmp,
            static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo),
            ep ? &info : nullptr, nullptr, nullptr);
        CloseHandle(dmp);
    }

    swprintf(path, MAX_PATH + 8, L"%s.txt", g_crashBase);
    FILE* f = _wfopen(path, L"w");
    if (!f) return;

    const EXCEPTION_RECORD* rec = ep ? ep->ExceptionRecord : nullptr;
    if (rec) {
        fprintf(f, "Exception 0x%08lX at ", rec->ExceptionCode);
        WriteModuleOffset(f, reinterpret_cast<DWORD64>(rec->ExceptionAddress));
        if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2)
            fprintf(f, " (%s 0x%llx)", rec->ExceptionInformation[0] == 0 ? "read" : rec->ExceptionInformation[0] == 1 ? "write" : "execute",
                static_cast<unsigned long long>(rec->ExceptionInformation[1]));
        fprintf(f, "\nThread %lu\n\nStack:\n", GetCurrentThreadId());
    }

    if (ep && ep->ContextRecord) {
        HANDLE proc = GetCurrentProcess();
        HANDLE thread = GetCurrentThread();
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS);
        SymInitialize(proc, nullptr, TRUE);

        CONTEXT ctx = *ep->ContextRecord;
        STACKFRAME64 frame{};
        frame.AddrPC.Offset = ctx.Rip;    frame.AddrPC.Mode = AddrModeFlat;
        frame.AddrFrame.Offset = ctx.Rbp; frame.AddrFrame.Mode = AddrModeFlat;
        frame.AddrStack.Offset = ctx.Rsp; frame.AddrStack.Mode = AddrModeFlat;

        alignas(SYMBOL_INFO) char symBuf[sizeof(SYMBOL_INFO) + 512];
        for (int i = 0; i < 64; ++i) {
            if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, thread, &frame, &ctx, nullptr,
                    SymFunctionTableAccess64, SymGetModuleBase64, nullptr) || frame.AddrPC.Offset == 0)
                break;
            const DWORD64 pc = frame.AddrPC.Offset;
            fprintf(f, "  #%02d ", i);
            WriteModuleOffset(f, pc);

            auto* sym = reinterpret_cast<SYMBOL_INFO*>(symBuf);
            sym->SizeOfStruct = sizeof(SYMBOL_INFO);
            sym->MaxNameLen = 511;
            DWORD64 symDisp = 0;
            if (SymFromAddr(proc, pc, &symDisp, sym))
                fprintf(f, "  %s+0x%llx", sym->Name, static_cast<unsigned long long>(symDisp));
            IMAGEHLP_LINE64 line{ sizeof(IMAGEHLP_LINE64) };
            DWORD lineDisp = 0;
            if (SymGetLineFromAddr64(proc, pc, &lineDisp, &line))
                fprintf(f, "  (%s:%lu)", line.FileName, line.LineNumber);
            fprintf(f, "\n");
        }
        SymCleanup(proc);
    }
    fclose(f);
}

static LONG WINAPI UnhandledFilter(EXCEPTION_POINTERS* ep)
{
    WriteCrashReport(ep);
    return EXCEPTION_CONTINUE_SEARCH;
}

// Heap corruption and /GS failures are raised as fail-fast exceptions that skip the unhandled filter: catch those two
// first-chance (nothing handles them anyway).
static LONG WINAPI VectoredFatal(EXCEPTION_POINTERS* ep)
{
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == 0xC0000374 /* STATUS_HEAP_CORRUPTION */ || code == 0xC0000409 /* STATUS_STACK_BUFFER_OVERRUN */)
        WriteCrashReport(ep);
    return EXCEPTION_CONTINUE_SEARCH;
}

static void InstallCrashHandler()
{
    const auto dir = UserDataPath::LogsDirectory();
    if (dir.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &now);
    wchar_t stamp[64];
    wcsftime(stamp, 64, L"%Y%m%d_%H%M%S", &tm);
    swprintf(g_crashBase, MAX_PATH, L"%s\\crash_%s_PID%lu", dir.wstring().c_str(), stamp, GetCurrentProcessId());

    SetUnhandledExceptionFilter(UnhandledFilter);
    AddVectoredExceptionHandler(1, VectoredFatal);
}
#else
static void InstallCrashHandler() {}
#endif
