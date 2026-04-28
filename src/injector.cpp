#include "pch.h"
#include "hook.h"
#include "config.h"

extern "C" __declspec(dllexport) VOID DummyExport() {}

char* g_sServerAddress = nullptr;
long g_nServerPort = 0;

// Skip argv[0] in a Windows command line, honoring quoted exe paths.
static const char* SkipArgv0(const char* sCmdLine) {
    const char* p = sCmdLine;
    if (*p == '"') {
        ++p;
        while (*p && *p != '"') {
            ++p;
        }
        if (*p == '"') {
            ++p;
        }
    } else {
        while (*p && *p != ' ' && *p != '\t') {
            ++p;
        }
    }
    while (*p == ' ' || *p == '\t') {
        ++p;
    }
    return p;
}

static bool TryParse(const char* p, char (&sAddress)[16], long& nPort) {
    int matched = sscanf_s(p, "%15s %ld", sAddress, static_cast<unsigned>(sizeof(sAddress)), &nPort);
    if (matched < 1) {
        return false;
    }
    in_addr probe;
    return InetPtonA(AF_INET, sAddress, &probe) == 1;
}

void ProcessCommandLine() {
    // The bundled launcher passes lpCmdLine (no argv[0]) into
    // DetourCreateProcessWithDllExA, so GetCommandLineA() inside MapleStory.exe
    // is "<addr> <port>" with no exe path. A different launcher / manual injection
    // may put the exe path first — try the raw line, then fall back to skipping argv[0].
    const char* sCmdLine = GetCommandLineA();
    char sAddress[16] = {};
    long nPort = 0;

    if (!TryParse(sCmdLine, sAddress, nPort) && !TryParse(SkipArgv0(sCmdLine), sAddress, nPort)) {
        return;
    }
    // Process-lifetime storage — point g_sServerAddress at a static buffer instead of
    // _strdup'ing (which leaks for the life of the process). Keep the pointer-based
    // shape so consumers can still null-check g_sServerAddress to fall back to default.
    static char s_sAddressBuf[16];
    if (strcpy_s(s_sAddressBuf, sizeof(s_sAddressBuf), sAddress) != 0) {
        return;
    }
    g_sServerAddress = s_sAddressBuf;
    g_nServerPort = nPort;
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    switch (fdwReason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hinstDLL);
        ProcessCommandLine();
        AttachSystemHooks();
        break;
    case DLL_PROCESS_DETACH:
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
        break;
    }
    return TRUE;
}