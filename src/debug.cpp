#include "pch.h"
#include "debug.h"


void DebugMessage(const char* pszFormat, ...) {
    char pszDest[1024];
    size_t cbDest = 1024 * sizeof(char);
    va_list argList;
    va_start(argList, pszFormat);
    StringCbVPrintfA(pszDest, cbDest, pszFormat, argList);
    OutputDebugStringA(pszDest);
#ifdef _DEBUG
    // Mirror to the AttachDebugConsole() console so messages are visible
    // without a debugger / DebugView. fputs is a no-op when stdout was
    // never redirected (release builds, or debug without AllocConsole),
    // so this is harmless if the console didn't get attached.
    fputs(pszDest, stdout);
    fputc('\n', stdout);
    fflush(stdout);
#endif
    va_end(argList);
}

void ErrorMessage(const char* pszFormat, ...) {
    char pszDest[1024];
    size_t cbDest = 1024 * sizeof(char);
    va_list argList;
    va_start(argList, pszFormat);
    StringCbVPrintfA(pszDest, cbDest, pszFormat, argList);
    MessageBox(nullptr, pszDest, "Error", MB_ICONERROR);
    va_end(argList);
}

void AttachDebugConsole() {
#ifdef _DEBUG
    if (!AllocConsole()) {
        // Already attached (e.g. parent console) — still safe to redirect.
    }
    SetConsoleTitleA("Kinoko debug");
    FILE* fIgnore = nullptr;
    freopen_s(&fIgnore, "CONOUT$", "w", stdout);
    freopen_s(&fIgnore, "CONOUT$", "w", stderr);
#endif
}