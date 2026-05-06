#pragma once

#ifdef _DEBUG
#define DEBUG_MESSAGE(FORMAT, ...) DebugMessage(FORMAT, __VA_ARGS__)
#else
#define DEBUG_MESSAGE(FORMAT, ...)
#endif


void DebugMessage(const char* pszFormat, ...);

void ErrorMessage(const char* pszFormat, ...);

// Spin up a stdout console so DebugMessage() lines are visible without a
// debugger / DebugView. Call once from DllMain. No-op in release.
void AttachDebugConsole();