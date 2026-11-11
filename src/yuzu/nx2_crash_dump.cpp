// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu/nx2_crash_dump.h"

#ifdef _WIN32

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <exception>
#include <filesystem>
#include <string>

#include <windows.h>
#include <psapi.h>

#include "common/fs/path_util.h"

namespace {

wchar_t g_directory[MAX_PATH]{};
LPTOP_LEVEL_EXCEPTION_FILTER g_previous_filter{};
std::terminate_handler g_previous_terminate{};
std::atomic<bool> g_written{};

/// Formats into a stack buffer and writes it: the heap may be what broke.
void Line(HANDLE file, const char* format, ...) {
    char text[1024];
    va_list args;
    va_start(args, format);
    const int length = std::vsnprintf(text, sizeof(text) - 1, format, args);
    va_end(args);
    if (length <= 0) {
        return;
    }
    const DWORD size = static_cast<DWORD>((std::min)(length, static_cast<int>(sizeof(text) - 2)));
    text[size] = '\n';
    DWORD written{};
    WriteFile(file, text, size + 1, &written, nullptr);
}

/// "module+offset" for a code address, or "?" outside every module.
void ModuleOf(DWORD64 address, char* out, size_t out_size) {
    HMODULE module{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(address), &module)) {
        std::snprintf(out, out_size, "?");
        return;
    }
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(module, path, MAX_PATH);
    const wchar_t* name = std::wcsrchr(path, L'\\');
    char narrow[MAX_PATH]{};
    WideCharToMultiByte(CP_UTF8, 0, name ? name + 1 : path, -1, narrow, sizeof(narrow), nullptr,
                        nullptr);
    std::snprintf(out, out_size, "%s+%#llx", narrow,
                  static_cast<unsigned long long>(address - reinterpret_cast<DWORD64>(module)));
}

bool IsCode(DWORD64 address) {
    MEMORY_BASIC_INFORMATION region{};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(address), &region, sizeof(region)) == 0) {
        return false;
    }
    constexpr DWORD executable =
        PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return region.State == MEM_COMMIT && (region.Protect & executable) != 0;
}

void Region(HANDLE file, const char* label, DWORD64 address) {
    MEMORY_BASIC_INFORMATION region{};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(address), &region, sizeof(region)) == 0) {
        Line(file, "%s %#llx: VirtualQuery failed (%lu)", label,
             static_cast<unsigned long long>(address), GetLastError());
        return;
    }
    const char* state = region.State == MEM_COMMIT    ? "committed"
                        : region.State == MEM_RESERVE ? "reserved"
                        : region.State == MEM_FREE    ? "free"
                                                      : "?";
    const char* type = region.Type == MEM_IMAGE    ? "image"
                       : region.Type == MEM_MAPPED ? "mapped"
                       : region.Type == MEM_PRIVATE ? "private"
                                                    : "-";
    char module[MAX_PATH + 32];
    ModuleOf(address, module, sizeof(module));
    Line(file, "%s %#llx (%s): %s %s, protect %#lx, region %p+%#llx, allocation %p", label,
         static_cast<unsigned long long>(address), module, state, type, region.Protect,
         region.BaseAddress, static_cast<unsigned long long>(region.RegionSize),
         region.AllocationBase);
}

/// The faulting thread's frames, unwound from the exception's context with the modules' unwind
/// data. A frame without unwind data (a leaf, or the bad address an indirect call reached)
/// returns to the address at its stack pointer.
void CallStack(HANDLE file, const CONTEXT& faulting) {
    const NT_TIB* const tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
    const auto stack_low = reinterpret_cast<DWORD64>(tib->StackLimit);
    const auto stack_high = reinterpret_cast<DWORD64>(tib->StackBase);
    CONTEXT context = faulting;
    Line(file, "Call stack:");
    for (int frame = 0; frame < 64 && context.Rip != 0; ++frame) {
        char module[MAX_PATH + 32];
        ModuleOf(context.Rip, module, sizeof(module));
        Line(file, "  #%02d %#llx %s (rsp %#llx)", frame,
             static_cast<unsigned long long>(context.Rip), module,
             static_cast<unsigned long long>(context.Rsp));
        DWORD64 image_base{};
        PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
        if (function == nullptr) {
            if (context.Rsp < stack_low || context.Rsp + 8 > stack_high) {
                break;
            }
            context.Rip = *reinterpret_cast<const DWORD64*>(context.Rsp);
            context.Rsp += 8;
            continue;
        }
        PVOID handler_data{};
        DWORD64 establisher{};
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function, &context,
                         &handler_data, &establisher, nullptr);
    }
}

/// Code addresses among the faulting thread's stack words: callers even where unwinding stops.
void StackScan(HANDLE file, DWORD64 rsp) {
    const NT_TIB* const tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
    const auto stack_low = reinterpret_cast<DWORD64>(tib->StackLimit);
    const auto stack_high = reinterpret_cast<DWORD64>(tib->StackBase);
    if (rsp < stack_low || rsp >= stack_high) {
        Line(file, "Stack scan: rsp %#llx is outside this thread's stack %#llx-%#llx",
             static_cast<unsigned long long>(rsp), static_cast<unsigned long long>(stack_low),
             static_cast<unsigned long long>(stack_high));
        return;
    }
    Line(file, "Code addresses on the stack:");
    int shown = 0;
    for (DWORD64 slot = rsp & ~DWORD64{7}; slot + 8 <= stack_high && slot < rsp + 0x4000 &&
                                           shown < 96;
         slot += 8) {
        const DWORD64 value = *reinterpret_cast<const DWORD64*>(slot);
        if (value < 0x10000 || !IsCode(value)) {
            continue;
        }
        char module[MAX_PATH + 32];
        ModuleOf(value, module, sizeof(module));
        Line(file, "  [rsp+%#llx] %#llx %s", static_cast<unsigned long long>(slot - rsp),
             static_cast<unsigned long long>(value), module);
        ++shown;
    }
}

void Modules(HANDLE file) {
    HMODULE modules[1024];
    DWORD needed{};
    if (!K32EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed)) {
        return;
    }
    Line(file, "Modules:");
    const DWORD count = (std::min)(needed / static_cast<DWORD>(sizeof(HMODULE)), DWORD{1024});
    for (DWORD index = 0; index < count; ++index) {
        MODULEINFO module_info{};
        K32GetModuleInformation(GetCurrentProcess(), modules[index], &module_info,
                                sizeof(module_info));
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(modules[index], path, MAX_PATH);
        char narrow[MAX_PATH * 3]{};
        WideCharToMultiByte(CP_UTF8, 0, path, -1, narrow, sizeof(narrow), nullptr, nullptr);
        Line(file, "  %p-%p %s", module_info.lpBaseOfDll,
             static_cast<const char*>(module_info.lpBaseOfDll) + module_info.SizeOfImage, narrow);
    }
}

/// ntdll's ring of recently unloaded modules: code an indirect call reaches after its module
/// was unloaded lies in one of these ranges.
void UnloadedModules(HANDLE file) {
    struct UnloadEvent {
        PVOID base;
        SIZE_T size;
        ULONG sequence;
        ULONG time_date_stamp;
        ULONG checksum;
        WCHAR name[32];
        ULONG version[2];
    };
    using GetTrace = VOID(NTAPI*)(PULONG*, PULONG*, PVOID*);
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto get_trace =
        ntdll ? reinterpret_cast<GetTrace>(
                    reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetUnloadEventTraceEx")))
              : nullptr;
    if (get_trace == nullptr) {
        return;
    }
    PULONG element_size{};
    PULONG element_count{};
    PVOID trace{};
    get_trace(&element_size, &element_count, &trace);
    if (element_size == nullptr || element_count == nullptr || trace == nullptr ||
        *element_size < sizeof(UnloadEvent)) {
        return;
    }
    Line(file, "Unloaded modules:");
    for (ULONG index = 0; index < *element_count; ++index) {
        const auto* event = reinterpret_cast<const UnloadEvent*>(
            static_cast<const char*>(trace) + static_cast<size_t>(index) * *element_size);
        if (event->base == nullptr) {
            continue;
        }
        char narrow[128]{};
        WideCharToMultiByte(CP_UTF8, 0, event->name, 32, narrow, sizeof(narrow) - 1, nullptr,
                            nullptr);
        Line(file, "  %p-%p %s (unload #%lu)", event->base,
             static_cast<const char*>(event->base) + event->size, narrow, event->sequence);
    }
}

void WriteDump(EXCEPTION_POINTERS* pointers, const wchar_t* path) {
    using WriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, int, const void*, const void*,
                                      const void*);
    struct ExceptionInformation {
        DWORD thread_id;
        EXCEPTION_POINTERS* exception_pointers;
        BOOL client_pointers;
    };
    const HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll");
    const auto write = dbghelp ? reinterpret_cast<WriteDumpFn>(reinterpret_cast<void*>(
                                     GetProcAddress(dbghelp, "MiniDumpWriteDump")))
                               : nullptr;
    if (write == nullptr) {
        return;
    }
    const HANDLE file =
        CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    const ExceptionInformation information{GetCurrentThreadId(), pointers, FALSE};
    const void* const exception = pointers != nullptr ? &information : nullptr;
    // Unloaded modules, thread times and start addresses and thread info. Not
    // MiniDumpWithIndirectlyReferencedMemory, which failed on Eden's address space, nor
    // MiniDumpWithFullMemoryInfo: walking the reserved placeholders stopped every thread for
    // ~15 s and still wrote nothing (r52). A plain dump (all threads' stacks) if this fails.
    constexpr int flags = 0x20 | 0x100 | 0x1000;
    if (!write(GetCurrentProcess(), GetCurrentProcessId(), file, flags, exception, nullptr,
               nullptr)) {
        SetFilePointer(file, 0, nullptr, FILE_BEGIN);
        SetEndOfFile(file);
        write(GetCurrentProcess(), GetCurrentProcessId(), file, 0, exception, nullptr, nullptr);
    }
    CloseHandle(file);
}

void WriteReport(EXCEPTION_POINTERS* pointers, const char* what) {
    if (g_written.exchange(true)) {
        return;
    }
    // Fails harmlessly when the folder exists.
    CreateDirectoryW(g_directory, nullptr);
    wchar_t text_path[MAX_PATH + 64];
    wchar_t dump_path[MAX_PATH + 64];
    std::swprintf(text_path, MAX_PATH + 64, L"%ls\\lichii-crash-%lu.txt", g_directory,
                  GetCurrentProcessId());
    std::swprintf(dump_path, MAX_PATH + 64, L"%ls\\lichii-crash-%lu.dmp", g_directory,
                  GetCurrentProcessId());
    const HANDLE file = CreateFileW(text_path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        Line(file, "Lichii crash (EDEN_NX2_CRASH_DUMP): %s, thread %lu", what,
             GetCurrentThreadId());
        if (pointers != nullptr) {
            const EXCEPTION_RECORD& record = *pointers->ExceptionRecord;
            const CONTEXT& context = *pointers->ContextRecord;
            Line(file, "Exception %#lx, flags %#lx, at %p, %lu parameters: %#llx %#llx %#llx",
                 record.ExceptionCode, record.ExceptionFlags, record.ExceptionAddress,
                 record.NumberParameters,
                 static_cast<unsigned long long>(
                     record.NumberParameters > 0 ? record.ExceptionInformation[0] : 0),
                 static_cast<unsigned long long>(
                     record.NumberParameters > 1 ? record.ExceptionInformation[1] : 0),
                 static_cast<unsigned long long>(
                     record.NumberParameters > 2 ? record.ExceptionInformation[2] : 0));
            if (record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
                record.NumberParameters > 1) {
                const ULONG_PTR kind = record.ExceptionInformation[0];
                Line(file, "Access violation: %s at %#llx",
                     kind == 0   ? "read"
                     : kind == 1 ? "write"
                     : kind == 8 ? "execute (DEP)"
                                 : "?",
                     static_cast<unsigned long long>(record.ExceptionInformation[1]));
                Region(file, "Accessed", record.ExceptionInformation[1]);
            }
            Region(file, "rip", context.Rip);
            Line(file,
                 "rip %#llx rsp %#llx rbp %#llx rax %#llx rbx %#llx rcx %#llx rdx %#llx rsi "
                 "%#llx rdi %#llx",
                 static_cast<unsigned long long>(context.Rip),
                 static_cast<unsigned long long>(context.Rsp),
                 static_cast<unsigned long long>(context.Rbp),
                 static_cast<unsigned long long>(context.Rax),
                 static_cast<unsigned long long>(context.Rbx),
                 static_cast<unsigned long long>(context.Rcx),
                 static_cast<unsigned long long>(context.Rdx),
                 static_cast<unsigned long long>(context.Rsi),
                 static_cast<unsigned long long>(context.Rdi));
            Line(file,
                 "r8 %#llx r9 %#llx r10 %#llx r11 %#llx r12 %#llx r13 %#llx r14 %#llx r15 %#llx",
                 static_cast<unsigned long long>(context.R8),
                 static_cast<unsigned long long>(context.R9),
                 static_cast<unsigned long long>(context.R10),
                 static_cast<unsigned long long>(context.R11),
                 static_cast<unsigned long long>(context.R12),
                 static_cast<unsigned long long>(context.R13),
                 static_cast<unsigned long long>(context.R14),
                 static_cast<unsigned long long>(context.R15));
            CallStack(file, context);
            StackScan(file, context.Rsp);
        } else {
            CONTEXT context{};
            RtlCaptureContext(&context);
            CallStack(file, context);
        }
        UnloadedModules(file);
        Modules(file);
        FlushFileBuffers(file);
        CloseHandle(file);
    }
    WriteDump(pointers, dump_path);
}

LONG WINAPI Filter(EXCEPTION_POINTERS* pointers) {
    // A C++ exception of the GCC runtime passes the top-level filter on its way to
    // std::terminate (MinGW's own filter lets it go on); the terminate handler reports it.
    constexpr DWORD gcc_magic = 0x20474343;
    if ((pointers->ExceptionRecord->ExceptionCode & 0x20ffffff) != gcc_magic) {
        WriteReport(pointers, "unhandled exception");
    }
    return g_previous_filter != nullptr ? g_previous_filter(pointers) : EXCEPTION_CONTINUE_SEARCH;
}

void Terminate() {
    WriteReport(nullptr, "std::terminate (an uncaught C++ exception)");
    if (g_previous_terminate != nullptr) {
        g_previous_terminate();
    }
    std::abort();
}

} // Anonymous namespace

void InstallNx2CrashDump() {
    const char* const value = std::getenv("EDEN_NX2_CRASH_DUMP");
    if (value == nullptr || value[0] == '\0' || std::strcmp(value, "0") == 0) {
        return;
    }
    if (g_directory[0] == L'\0') {
        std::filesystem::path directory;
        if (std::strcmp(value, "1") == 0) {
            // Not made here: %APPDATA%\lichii must not exist before the Eden import runs. The
            // profile's folders include it, and WriteReport makes it for a crash before them.
            directory = Common::FS::GetEdenPath(Common::FS::EdenPath::CrashDumpsDir);
        } else {
            directory = std::filesystem::path{std::u8string{reinterpret_cast<const char8_t*>(value)}};
            std::error_code error;
            std::filesystem::create_directories(directory, error);
        }
        const std::wstring text = directory.wstring();
        if (text.empty() || text.size() >= MAX_PATH) {
            return;
        }
        std::wcsncpy(g_directory, text.c_str(), MAX_PATH - 1);
        g_previous_terminate = std::set_terminate(&Terminate);
    }
    // Again after a game launched: a library may have installed its own filter. Ours calls the
    // filter it replaces.
    const LPTOP_LEVEL_EXCEPTION_FILTER previous = SetUnhandledExceptionFilter(&Filter);
    if (previous != &Filter) {
        g_previous_filter = previous;
    }
}

#else

void InstallNx2CrashDump() {}

#endif
