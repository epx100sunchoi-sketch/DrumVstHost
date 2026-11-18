// JUCE를 거치지 않는 Windows 전용 보조 함수.

#include "Platform.h"

#ifdef _WIN32

#ifndef NOMINMAX
 #define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
 #define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <dwmapi.h>
#include <dbghelp.h>
#include <cstdio>
#include <cstring>

#pragma comment (lib, "dwmapi.lib")
#pragma comment (lib, "dbghelp.lib")

namespace
{
wchar_t crashLogPath[2048] {};
wchar_t symbolSearchPath[2048] {};
volatile LONG crashReportWritten = 0;
HMODULE appModule = nullptr;

void writeText (HANDLE file, const char* text)
{
    DWORD written = 0;
    WriteFile (file, text, (DWORD) std::strlen (text), &written, nullptr);
}

HMODULE moduleOf (DWORD64 address)
{
    HMODULE module = nullptr;
    GetModuleHandleExW (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                        reinterpret_cast<LPCWSTR> (address), &module);
    return module;
}

void moduleNameOf (HMODULE module, char* out, size_t size)
{
    char path[MAX_PATH] {};

    if (module == nullptr || GetModuleFileNameA (module, path, MAX_PATH) == 0)
    {
        strcpy_s (out, size, "?");
        return;
    }

    const char* slash = std::strrchr (path, '\\');
    strcpy_s (out, size, slash != nullptr ? slash + 1 : path);
}

// x64 스택을 RtlVirtualUnwind로 거슬러 올라간다(힙을 쓰지 않는다 — 힙이 망가졌을 수 있다).
int unwindStack (const CONTEXT& start, DWORD64* addresses, int maxFrames)
{
    CONTEXT context = start;
    int count = 0;

    while (count < maxFrames && context.Rip != 0)
    {
        addresses[count++] = context.Rip;
        DWORD64 imageBase = 0;
        auto* entry = RtlLookupFunctionEntry (context.Rip, &imageBase, nullptr);

        if (entry == nullptr)
        {
            if (context.Rsp == 0)
                break;

            context.Rip = *reinterpret_cast<DWORD64*> (context.Rsp);   // 잎 함수: 돌아갈 주소가 스택 맨 위
            context.Rsp += 8;
        }
        else
        {
            void* handlerData = nullptr;
            DWORD64 establisherFrame = 0;
            RtlVirtualUnwind (UNW_FLAG_NHANDLER, imageBase, context.Rip, entry, &context, &handlerData, &establisherFrame, nullptr);
        }
    }

    return count;
}

void writeCrashReport (EXCEPTION_POINTERS* info)
{
    if (InterlockedExchange (&crashReportWritten, 1) != 0)
        return;

    HANDLE file = CreateFileW (crashLogPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (file == INVALID_HANDLE_VALUE)
        return;

    char line[2048];
    SYSTEMTIME now {};
    GetLocalTime (&now);
    const auto* record = info->ExceptionRecord;

    sprintf_s (line, "DrumVstHost crash %04u-%02u-%02u %02u:%02u:%02u, thread %lu\r\nexception 0x%08lx at %p\r\n",
               now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, GetCurrentThreadId(),
               (unsigned long) record->ExceptionCode, record->ExceptionAddress);
    writeText (file, line);

    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2)
    {
        sprintf_s (line, "%s address 0x%llx\r\n", record->ExceptionInformation[0] == 0 ? "read" : (record->ExceptionInformation[0] == 1 ? "write" : "execute"),
                   (unsigned long long) record->ExceptionInformation[1]);
        writeText (file, line);
    }

    // 1) 함수 이름 없이 모듈+위치만(힙이 망가져도 남도록 먼저 쓴다)
    DWORD64 addresses[64] {};
    const int count = unwindStack (*info->ContextRecord, addresses, 64);
    writeText (file, "\r\nframes:\r\n");

    for (int i = 0; i < count; ++i)
    {
        const auto module = moduleOf (addresses[i]);
        char name[MAX_PATH];
        moduleNameOf (module, name, sizeof (name));
        sprintf_s (line, "  %2d  %s+0x%llx\r\n", i, name, (unsigned long long) (addresses[i] - (DWORD64) module));
        writeText (file, line);
    }

    FlushFileBuffers (file);

    // 2) 기호(.pdb)로 함수 이름과 줄 번호
    const auto process = GetCurrentProcess();
    SymSetOptions (SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_FAIL_CRITICAL_ERRORS);

    if (SymInitializeW (process, symbolSearchPath, TRUE))
    {
        writeText (file, "\r\nsymbols:\r\n");

        for (int i = 0; i < count; ++i)
        {
            const DWORD64 address = i == 0 ? addresses[i] : addresses[i] - 1;   // 돌아갈 주소는 호출 다음 명령이다
            alignas (SYMBOL_INFO) char symbolBuffer[sizeof (SYMBOL_INFO) + 512] {};
            auto* symbol = reinterpret_cast<SYMBOL_INFO*> (symbolBuffer);
            symbol->SizeOfStruct = sizeof (SYMBOL_INFO);
            symbol->MaxNameLen = 511;
            DWORD64 displacement = 0;
            const char* function = SymFromAddr (process, address, &displacement, symbol) ? symbol->Name : "?";

            IMAGEHLP_LINE64 source {};
            source.SizeOfStruct = sizeof (source);
            DWORD lineDisplacement = 0;

            if (SymGetLineFromAddr64 (process, address, &lineDisplacement, &source))
                sprintf_s (line, "  %2d  %s  (%s:%lu)\r\n", i, function, source.FileName, source.LineNumber);
            else
                sprintf_s (line, "  %2d  %s\r\n", i, function);

            writeText (file, line);
        }

        SymCleanup (process);
    }

    CloseHandle (file);
}

bool isFatalInThisApp (EXCEPTION_POINTERS* info)
{
    const auto code = info->ExceptionRecord->ExceptionCode;

    if (code == 0xC0000374)   // STATUS_HEAP_CORRUPTION: 어디서 났든 곧 종료된다
        return true;

    // 메모리 접근 오류는 이 앱 코드에서 난 것만(플러그인은 스스로 처리하기도 한다)
    return (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION || code == EXCEPTION_INT_DIVIDE_BY_ZERO
            || code == EXCEPTION_STACK_OVERFLOW)
        && moduleOf ((DWORD64) info->ExceptionRecord->ExceptionAddress) == appModule;
}

LONG CALLBACK firstChanceHandler (EXCEPTION_POINTERS* info)
{
    if (isFatalInThisApp (info))
        writeCrashReport (info);

    return EXCEPTION_CONTINUE_SEARCH;
}

LONG WINAPI lastChanceFilter (EXCEPTION_POINTERS* info)
{
    writeCrashReport (info);
    return EXCEPTION_CONTINUE_SEARCH;
}
} // namespace

void pumpMessagesFor (int milliseconds)
{
    const auto end = GetTickCount64() + (ULONGLONG) juce::jmax (0, milliseconds);

    while (GetTickCount64() < end)
    {
        MSG message;

        while (PeekMessageW (&message, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage (&message);
            DispatchMessageW (&message);
        }

        Sleep (5);
    }
}

void installCrashReporter (const juce::File& logFile)
{
    logFile.getFullPathName().copyToUTF16 (reinterpret_cast<juce::CharPointer_UTF16::CharType*> (crashLogPath), sizeof (crashLogPath) - 2);
    juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory().getFullPathName()
        .copyToUTF16 (reinterpret_cast<juce::CharPointer_UTF16::CharType*> (symbolSearchPath), sizeof (symbolSearchPath) - 2);

    appModule = GetModuleHandleW (nullptr);
    AddVectoredExceptionHandler (1, firstChanceHandler);
    SetUnhandledExceptionFilter (lastChanceFilter);
}

void setDarkTitleBar (void* nativeWindowHandle)
{
    if (nativeWindowHandle == nullptr)
        return;

    const BOOL dark = TRUE;
    const DWORD useImmersiveDarkMode = 20;   // DWMWA_USE_IMMERSIVE_DARK_MODE
    DwmSetWindowAttribute ((HWND) nativeWindowHandle, useImmersiveDarkMode, &dark, sizeof (dark));
}

void setProcessEnvironmentVariable (const juce::String& name, const juce::String& value)
{
    SetEnvironmentVariableW (name.toWideCharPointer(), value.toWideCharPointer());
}

#else

void setDarkTitleBar (void*) {}
void setProcessEnvironmentVariable (const juce::String&, const juce::String&) {}
void installCrashReporter (const juce::File&) {}
void pumpMessagesFor (int) {}

#endif
