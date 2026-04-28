#include "pch.h"
#include "hook.h"
#include "debug.h"
#include "ztl/ztl.h"
#include "wvs/util.h"
#include "wvs/wvsapp.h"
#include "wvs/wvscontext.h"
#include "wvs/inputsystem.h"
#include "wvs/actionman.h"
#include "wvs/config.h"
#include "wvs/field.h"
#include "wvs/stage.h"
#include "wvs/login.h"
#include "wvs/clientsocket.h"
#include "wvs/temporarystatview.h"
#include "wvs/ctrlwnd.h"

#include <windows.h>
#include <timeapi.h>

#pragma warning(disable : 4996)

ZALLOC_GLOBAL
ZALLOCEX(ZAllocAnonSelector, 0x00C6E67C)
ZALLOCEX(ZAllocStrSelector<char>, 0x00C6E6A8)
ZALLOCEX(ZAllocStrSelector<wchar_t>, 0x00C6E64C)
ZRECYCLABLE(ZInetAddr, 0x00C63EE4)


static auto CWvsApp__ctor = 0x009CA8A0;

void __fastcall CWvsApp__ctor_hook(CWvsApp* pThis, void* _EDX, const char* sCmdLine) {
    DEBUG_MESSAGE("CWvsApp::CWvsApp");
    *reinterpret_cast<uintptr_t*>(pThis) = 0x00BAD460; // vftable: CWvsApp::`vftable'
    CWvsApp::ms_pInstance = pThis;
    pThis->m_hWnd = nullptr;
    pThis->m_bPCOMInitialized = 0;
    pThis->m_hHook = nullptr;
    pThis->m_nOSVersion = 0;
    pThis->m_nOSMinorVersion = 0;
    pThis->m_nOSBuildNumber = 0;
    construct<ZXString<char>>(&pThis->m_sCSDVersion);
    pThis->m_b64BitInfo = 0;
    pThis->m_tUpdateTime = 0;
    pThis->m_bFirstUpdate = 1;
    construct<ZXString<char>>(&pThis->m_sCmdLine);
    pThis->m_nGameStartMode = 0;
    pThis->m_bAutoConnect = 1;
    pThis->m_bShowAdBalloon = 0;
    pThis->m_bExitByTitleEscape = 0;
    pThis->m_hrZExceptionCode = 0;
    pThis->m_hrComErrorCode = 0;
    pThis->m_tNextSecurityCheck = 0;
    pThis->m_bEnabledDX9 = true;
    construct<ZArray<uint8_t>>(&pThis->m_pBackupBuffer);
    pThis->m_dwBackupBufferSize = 0;
    pThis->m_dwClearStackLog = 0;
    pThis->m_bWindowActive = 1;

    pThis->m_pBackupBuffer.Alloc(0x1000);
    pThis->m_nGameStartMode = 2;
    pThis->m_dwMainThreadId = GetCurrentThreadId();

    OSVERSIONINFOA ovi;
    ovi.dwOSVersionInfoSize = 148;
    GetVersionExA(&ovi);
    pThis->m_bWin9x = ovi.dwPlatformId == 1;
    if (ovi.dwMajorVersion >= 6 && !pThis->m_nGameStartMode) {
        pThis->m_nGameStartMode = 2;
    }

    typedef BOOL(WINAPI * LPFN_ISWOW64PROCESS)(HANDLE, PBOOL);
    auto fnIsWow64Process = reinterpret_cast<LPFN_ISWOW64PROCESS>(GetProcAddress(GetModuleHandleA("KERNEL32"), "IsWow64Process"));
    BOOL bIs64 = 0;
    if (fnIsWow64Process) {
        fnIsWow64Process(GetCurrentProcess(), &bIs64);
    }
    if (ovi.dwMajorVersion >= 6 && !bIs64) {
        // ResetLSP();
        reinterpret_cast<void(__cdecl*)()>(0x0045ECD0)();
    }

    // CWvsApp::SetClearStackLog(this, (bIs64 << 8) + (m_nOSVersion << 12));
    reinterpret_cast<void(__thiscall*)(CWvsApp*, uint32_t)>(0x009C1960)(pThis, (bIs64 << 8) + (pThis->m_nOSVersion << 12));
    pThis->m_nOSVersion = ovi.dwMajorVersion;
    pThis->m_nOSMinorVersion = ovi.dwMinorVersion;
    pThis->m_nOSBuildNumber = ovi.dwBuildNumber;
    pThis->m_sCSDVersion = ovi.szCSDVersion;
    pThis->m_b64BitInfo = bIs64;
}


static auto CWvsApp__SetUp = 0x009CAFB0;

void __fastcall CWvsApp__SetUp_hook(CWvsApp* pThis, void* _EDX) {
    DEBUG_MESSAGE("CWvsApp::SetUp");
    // CWvsApp::InitializeAuth(this);
    reinterpret_cast<void(__thiscall*)(CWvsApp*)>(0x009C3AD0)(pThis);
    srand(timeGetTime());
    // GetSEPrivilege();
    reinterpret_cast<void(__cdecl*)()>(0x0045E030)();

    // ms_aIpAddr table init: copy 0x200 bytes from .text:0x00401234 to .data:0x00C68848,
    // then overlay 10 dwords at indices 0x40..0x4A with InitSafeDll() returns. The CRC-32
    // table init that follows in the original SetUp is harmless to drop (only consumed by
    // CWvsApp::Run, which is wholesale replaced by CWvsApp__Run_hook).
    memcpy(reinterpret_cast<void*>(0x00C68848), reinterpret_cast<const void*>(0x00401234), 0x200);
    {
        auto pTable = reinterpret_cast<uint32_t*>(0x00C68848);
        auto InitSafeDll = reinterpret_cast<HINSTANCE(__cdecl*)()>(0x0045E2F0);
        for (int i = 0x40; i < 0x4A; ++i) {
            pTable[i] = reinterpret_cast<uint32_t>(InitSafeDll());
        }
    }

    DEBUG_MESSAGE("CWvsApp::SetUp - Initializing...");
    // TSingleton<CConfig>::CreateInstance();
    reinterpret_cast<void(__cdecl*)()>(0x009C2420)();
    // CWvsApp::InitializePCOM(this);
    reinterpret_cast<void(__thiscall*)(CWvsApp*)>(0x009C16C0)(pThis);
    // CWvsApp::CreateMainWindow(this);
    reinterpret_cast<void(__thiscall*)(CWvsApp*)>(0x009C74A0)(pThis);

    DEBUG_MESSAGE("CWvsApp::SetUp - Connecting to server...");
    // TSingleton<CClientSocket>::CreateInstance();
    reinterpret_cast<void(__cdecl*)()>(0x009C23A0)();
    // CWvsApp::ConnectLogin(this);
    reinterpret_cast<void(__thiscall*)(CWvsApp*)>(0x009C1B30)(pThis);
    // TSingleton<CFuncKeyMappedMan>::CreateInstance();
    reinterpret_cast<void(__cdecl*)()>(0x009C2510)();
    // TSingleton<CQuickslotKeyMappedMan>::CreateInstance();
    reinterpret_cast<void(__cdecl*)()>(0x009C27E0)();
    // TSingleton<CMacroSysMan>::CreateInstance();
    reinterpret_cast<void(__cdecl*)()>(0x009C2590)();
    // TSingleton<CBattleRecordMan>::CreateInstance();
    reinterpret_cast<void(__cdecl*)()>(0x009C2220)();
    // CWvsApp::InitializeResMan(this);
    pThis->InitializeResMan();

    DEBUG_MESSAGE("CWvsApp::SetUp - Graphic & Sound...");
    // CWvsApp::InitializeGr2D(this);
    reinterpret_cast<void(__thiscall*)(CWvsApp*)>(0x009C7670)(pThis);
    // TSingleton<CInputSystem>::CreateInstance();
    reinterpret_cast<void(__cdecl*)()>(0x009C7C30)();
    // CInputSystem::Init(CInputSystem::GetInstance(), m_hWnd, m_ahInput);
    reinterpret_cast<void(__thiscall*)(CInputSystem*, HWND, void**)>(0x00571A60)(CInputSystem::GetInstance(), pThis->m_hWnd, pThis->m_ahInput);

    ShowWindow(pThis->m_hWnd, SW_SHOW);
    UpdateWindow(pThis->m_hWnd);
    SetForegroundWindow(pThis->m_hWnd);
    get_gr()->RenderFrame();
    // CWvsApp::InitializeSound(this);
    reinterpret_cast<void(__thiscall*)(CWvsApp*)>(0x009CA170)(pThis);

    DEBUG_MESSAGE("CWvsApp::SetUp - Loading Data...");
    // CWvsApp::InitializeGameData(this);
    reinterpret_cast<void(__thiscall*)(CWvsApp*)>(0x009C8440)(pThis);
    // TSingleton<CQuestMan>::CreateInstance()->LoadDemand();
    auto pQuestMan = reinterpret_cast<void*(__cdecl*)()>(0x009C21A0)();
    if (!reinterpret_cast<int32_t(__thiscall*)(void*)>(0x006C3D60)(pQuestMan)) {
        ErrorMessage("Failed to load quest data.");
    }
    // CQuestMan::LoadPartyQuestInfo(pQuestMan);
    reinterpret_cast<int32_t(__thiscall*)(void*)>(0x006C5540)(pQuestMan);
    // CQuestMan::LoadExclusive(pQuestMan);
    reinterpret_cast<int32_t(__thiscall*)(void*)>(0x006B9670)(pQuestMan);

    DEBUG_MESSAGE("CwvsApp::SetUp - Complete!");
    // TSingleton<CMonsterBookMan>::CreateInstance()->LoadBook();
    auto pMonsterBookMan = reinterpret_cast<void*(__cdecl*)()>(0x009CA820)();
    if (!reinterpret_cast<int32_t(__thiscall*)(void*)>(0x00664C10)(pMonsterBookMan)) {
        ErrorMessage("Failed to load monster book data.");
    }
    // CWvsApp::CreateWndManager(this);
    reinterpret_cast<void(__thiscall*)(CWvsApp*)>(0x009C2060)(pThis);
    // CConfig::ApplySysOpt(TSingleton<CConfig>::GetInstance(), nullptr, 0);
    reinterpret_cast<void(__thiscall*)(CConfig*, CONFIG_SYSOPT*, int)>(0x004B2300)(CConfig::GetInstance(), nullptr, 0);
    // TSingleton<CActionMan>::CreateInstance()->Init();
    auto pActionMan = reinterpret_cast<void*(__cdecl*)()>(0x009C22A0)();
    reinterpret_cast<void(__thiscall*)(void*)>(0x0041BEB0)(pActionMan);
    // TSingleton<CAnimationDisplayer>::CreateInstance();
    reinterpret_cast<void*(__cdecl*)()>(0x009C2320)();
    // TSingleton<CMapleTVMan>::CreateInstance()->Init()
    auto pMapleTVMan = reinterpret_cast<void*(__cdecl*)()>(0x009C2680)();
    reinterpret_cast<void(__thiscall*)(void*)>(0x0060FBC0)(pMapleTVMan);
    // TSingleton<CRadioManager>::CreateInstance();
    reinterpret_cast<void*(__cdecl*)()>(0x009C2770)();

    // (CLogo*) operator new(0x48); -> (CLogin*) operator new(0x2C8);
    CStage* pStage = static_cast<CStage*>(ZAllocEx<ZAllocAnonSelector>::s_Alloc(0x2C8));
    if (pStage) {
        // CLogo::CLogo(pStage); -> CLogin::Clogin(pStage);
        reinterpret_cast<void(__thiscall*)(void*)>(0x005DB440)(pStage);
    }
    set_stage(pStage, nullptr);
}


static auto CWvsApp__Run = 0x009C5F00;

void __fastcall CWvsApp__CallUpdate_hook(CWvsApp* pThis, void* _EDX, int32_t tCurTime) {
    if (pThis->m_bFirstUpdate) {
        pThis->m_tUpdateTime = tCurTime;
        pThis->m_tLastServerIPCheck = tCurTime;
        pThis->m_tLastServerIPCheck2 = tCurTime;
        pThis->m_tLastGGHookingAPICheck = tCurTime;
        pThis->m_tLastSecurityCheck = tCurTime;
        pThis->m_bFirstUpdate = 0;
    }
    while (tCurTime - pThis->m_tUpdateTime > 0) {
        auto pStage = get_stage();
        if (pStage) {
            pStage->Update();
        }
        // CWndMan::s_Update();
        reinterpret_cast<void(__cdecl*)()>(0x009B4B00)();
        pThis->m_tUpdateTime += 30;
        if (tCurTime - pThis->m_tUpdateTime > 0) {
            get_gr()->UpdateCurrentTime(pThis->m_tUpdateTime);
        }
    }
    get_gr()->UpdateCurrentTime(tCurTime);
    // CActionMan::SweepCache(TSingleton<CActionMan>::GetInstance());
    reinterpret_cast<void(__thiscall*)(CActionMan*)>(0x00415F60)(CActionMan::GetInstance());
}

void __fastcall CWvsApp__Run_hook(CWvsApp* pThis, void* _EDX, int32_t* pbTerminate) {
    HRESULT hr;
    MSG msg;
    ISMSG isMsg;
    memset(&msg, 0, sizeof(msg));
    memset(&isMsg, 0, sizeof(isMsg));
    if (CClientSocket::IsInstantiated()) {
        // CClientSocket::ManipulatePacket(TSingleton<CClientSocket>::GetInstance());
        reinterpret_cast<void(__thiscall*)(CClientSocket*)>(0x004B0220)(CClientSocket::GetInstance());
    }
    do {
        DWORD dwRet = MsgWaitForMultipleObjects(3, pThis->m_ahInput, 0, 0, 0xFF);
        if (dwRet <= 2) {
            // CInputSystem::UpdateDevice(TSingleton<CInputSystem>::GetInstance(), dwRet);
            reinterpret_cast<void(__thiscall*)(CInputSystem*, int32_t)>(0x00571710)(CInputSystem::GetInstance(), dwRet);
            do {
                // if (!CInputSystem::GetISMessage(TSingleton<CInputSystem>::GetInstance(), &isMsg))
                if (!reinterpret_cast<int32_t(__thiscall*)(CInputSystem*, ISMSG*)>(0x005708F0)(CInputSystem::GetInstance(), &isMsg)) {
                    break;
                }
                // CWvsApp::ISMsgProc(this, isMsg.message, isMsg.wParam, isMsg.lParam);
                reinterpret_cast<void(__thiscall*)(CWvsApp*, uint32_t, uint32_t, int32_t)>(0x009C1CE0)(pThis, isMsg.message, isMsg.wParam, isMsg.lParam);
            } while (!*pbTerminate);
        } else if (dwRet == 3) {
            do {
                if (!PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
                    break;
                }
                TranslateMessage(&msg);
                DispatchMessageA(&msg);
                // if (CWvsApp::ExtractComErrorCode(this, &hr))
                if (reinterpret_cast<int32_t(__thiscall*)(CWvsApp*, HRESULT*)>(0x009C0860)(pThis, &hr)) {
                    _com_issue_error(hr);
                }
                // if (CWvsApp::ExtractZExceptionCode(this, &hr))
                if (reinterpret_cast<int32_t(__thiscall*)(CWvsApp*, HRESULT*)>(0x009C0830)(pThis, &hr)) {
                    ZException exception(hr);
                    if (hr == 0x20000000) {
                        // CPatchException::CPatchException(&exception, m_nTargetVersion);
                        reinterpret_cast<void(__thiscall*)(void*, int32_t)>(0x00520FA0)(&exception, pThis->m_nTargetVersion);
                    } else if (hr >= 0x21000000 && hr <= 0x21000006) {
                        // CDisconnectException::CDisconnectException(&exception, hr);
                        reinterpret_cast<void(__thiscall*)(void*, HRESULT)>(0x00429860)(&exception, hr);
                    } else if (hr >= 0x22000000 && hr <= 0x2200000E) {
                        // CTerminateException::CTerminateException(&exception, hr);
                        reinterpret_cast<void(__thiscall*)(void*, HRESULT)>(0x00401D50)(&exception, hr);
                    }
                    throw exception;
                }
            } while (!*pbTerminate && msg.message != WM_QUIT);
        } else {
            // if (CInputSystem::GenerateAutoKeyDown(TSingleton<CInputSystem>::GetInstance(), &isMsg))
            if (reinterpret_cast<int32_t(__thiscall*)(CInputSystem*, ISMSG*)>(0x0056F990)(CInputSystem::GetInstance(), &isMsg)) {
                // CWvsApp::ISMsgProc(this, isMsg.message, isMsg.wParam, isMsg.lParam);
                reinterpret_cast<void(__thiscall*)(CWvsApp*, uint32_t, uint32_t, int32_t)>(0x009C1CE0)(pThis, isMsg.message, isMsg.wParam, isMsg.lParam);
            }
            // if (CInputSystem::GenerateAutoBtnDown(TSingleton<CInputSystem>::GetInstance(), &isMsg))
            if (reinterpret_cast<int32_t(__thiscall*)(CInputSystem*, ISMSG*)>(0x0056FAC0)(CInputSystem::GetInstance(), &isMsg)) {
                // CWvsApp::ISMsgProc(this, isMsg.message, isMsg.wParam, isMsg.lParam);
                reinterpret_cast<void(__thiscall*)(CWvsApp*, uint32_t, uint32_t, int32_t)>(0x009C1CE0)(pThis, isMsg.message, isMsg.wParam, isMsg.lParam);
            }
            int32_t tCurTime = get_gr()->nextRenderTime;
            CWvsApp__CallUpdate_hook(pThis, _EDX, tCurTime);
            // CWndMan::RedrawInvalidatedWindows();
            reinterpret_cast<void(__cdecl*)()>(0x009B2340)();
            get_gr()->RenderFrame();
            Sleep(1);
        }
    } while (!*pbTerminate && msg.message != WM_QUIT);
    if (msg.message == WM_QUIT) {
        PostQuitMessage(0);
    }
}


static auto CClientSocket__Connect = 0x004B0340;

void __fastcall CClientSocket__Connect_hook(CClientSocket* pThis, void* _EDX, CClientSocket::CONNECTCONTEXT* ctx) {
    DEBUG_MESSAGE("CClientSocket::Connect");
    pThis->m_ctxConnect.lAddr.RemoveAll();
    pThis->m_ctxConnect.lAddr.AddTail(ctx->lAddr);
    pThis->m_ctxConnect.posList = ctx->posList;
    pThis->m_ctxConnect.bLogin = ctx->bLogin;
    pThis->m_ctxConnect.posList = pThis->m_ctxConnect.lAddr.GetHeadPosition();
    auto next = ZList<ZInetAddr>::GetNext(pThis->m_ctxConnect.posList);

    DEBUG_MESSAGE("CClientSocket::Connect (addr)");
    // CClientSocket::ClearSendReceiveCtx(this);
    reinterpret_cast<void(__thiscall*)(CClientSocket*)>(0x004AE1A0)(pThis);
    // ZSocketBase::CloseSocket(&m_sock);
    reinterpret_cast<void(__thiscall*)(ZSocketBase*)>(0x004ACF30)(&pThis->m_sock);
    // ZSocketBase::Socket(&m_sock, 1, 2, 0);
    reinterpret_cast<void(__thiscall*)(ZSocketBase*, int, int, int)>(0x004ACF50)(&pThis->m_sock, 1, 2, 0);
    // CClientSocket::SetTimeout(this);
    reinterpret_cast<void(__thiscall*)(CClientSocket*)>(0x004ACBA0)(pThis);
    if (WSAAsyncSelect(pThis->m_sock, pThis->m_hWnd, 0x401, 0x33) == -1 ||
            connect(pThis->m_sock, next, 16) != -1 ||
            WSAGetLastError() != WSAEWOULDBLOCK) {
        // CClientSocket::OnConnect(this, 0);
        reinterpret_cast<void(__thiscall*)(CClientSocket*, int)>(0x004AEF10)(pThis, 0);
    }
}

static auto CClientSocket__OnAliveReq = 0x004AFC90;

void __fastcall CClientSocket__OnAliveReq_hook(CClientSocket* pThis, void* _EDX, CInPacket& iPacket) {
    COutPacket oPacket(25); // CP_AliveAck
    pThis->SendPacket(oPacket);
}


static auto CLogin__SendCheckPasswordPacket = 0x005DB9D0;

int32_t __fastcall CLogin__SendCheckPasswordPacket_hook(CLogin* pThis, void* _EDX, char* sID, char* sPasswd) {
    // m_bRequestSent is set on first send but the client never clears it on a failed
    // login response, so a wrong-password retry would silently no-op. Drop the guard.
    pThis->m_WorldItem.RemoveAll();
    pThis->m_aBalloon.RemoveAll();

    CSystemInfo si;
    // CSystemInfo::Init(&si);
    reinterpret_cast<void(__thiscall*)(CSystemInfo*)>(0x00A1F1B0)(&si);

    COutPacket oPacket(1); // CP_CheckPassword
    oPacket.EncodeStr(sID);
    oPacket.EncodeStr(sPasswd);
    oPacket.EncodeBuffer(si.MachineId, 16);
    oPacket.Encode4(0); // CSystemInfo::GetGameRoomClient(&v15)
    oPacket.Encode1(CWvsApp::GetInstance()->m_nGameStartMode);
    oPacket.Encode1(0);
    oPacket.Encode1(0);
    oPacket.Encode4(0); // CConfig::GetPartnerCode(TSingleton<CConfig>::ms_pInstance._m_pStr)
    CClientSocket::GetInstance()->SendPacket(oPacket);
    return 0;
}


static auto CWvsContext__OnEnterField = 0x009DBEC0;

void __fastcall CWvsContext__OnEnterField_hook(CWvsContext* pThis, void* _EDX) {
    // CWvsContext::UI_CloseRevive(this);
    reinterpret_cast<void(__thiscall*)(CWvsContext*)>(0x009CCCD0)(pThis);
    // CTemporaryStatView::Show(&m_temporaryStatView);
    reinterpret_cast<void(__thiscall*)(CTemporaryStatView*)>(0x0075C6A0)(&pThis->m_temporaryStatView);
    pThis->m_bKillMobFromEnterField = 0;

    // CUIRaiseManager::RestoreWindows() — restore minimized/raised quest UI windows
    // through ZRef<CUIRaiseManager> at this+0x3EA8.
    auto pUIRaiseManager = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(pThis) + 0x3EA8);
    reinterpret_cast<void(__thiscall*)(void*)>(0x0083C5B0)(pUIRaiseManager);

    // Spawn party HP UI when joining a party that doesn't yet have one.
    if (reinterpret_cast<int32_t(__thiscall*)(CWvsContext*)>(0x0050F430)(pThis) // GetPartyID()
            && !reinterpret_cast<int32_t(__cdecl*)()>(0x008B7130)()              // !TSingleton<CUIPartyHP>::IsInstantiated()
            && reinterpret_cast<int32_t(__thiscall*)(CConfig*)>(0x004B3110)(CConfig::GetInstance())) { // CConfig::GetShowPartyHP()
        // TSingleton<CUIPartyHP>::CreateInstance()
        reinterpret_cast<void(__cdecl*)()>(0x008D6460)();
    }

    // Apply pending party-search remocon state captured before the field change.
    if (pThis->m_bPartyAdvertise_Apply) {
        pThis->m_bPartyAdvertise_Apply = 0;
        if (pThis->m_nPartyAdvertise_Mode == 1) {
            // CWvsContext::ShowPartySearch_Remocon_Searching
            reinterpret_cast<void(__thiscall*)(CWvsContext*)>(0x009DA9A0)(pThis);
            // CWvsContext::SendPartyWanted(a, b, c, d)
            reinterpret_cast<void(__thiscall*)(CWvsContext*, uint32_t, uint32_t, uint32_t, uint32_t)>(0x00A10100)(
                pThis,
                pThis->m_anPartyAdvertise[0],
                pThis->m_anPartyAdvertise[1],
                pThis->m_anPartyAdvertise[2],
                pThis->m_anPartyAdvertise[3]);
        } else if (pThis->m_nPartyAdvertise_Mode == 2) {
            // CWvsContext::ShowPartySearch_Remocon_Holding
            reinterpret_cast<void(__thiscall*)(CWvsContext*)>(0x009DAA70)(pThis);
        }
    } else {
        // CWvsContext::StopPartySearch
        reinterpret_cast<void(__thiscall*)(CWvsContext*)>(0x009D6B80)(pThis);
    }

    // Restore radio mute level / re-show radio UI when entering a field with the radio playing.
    if (reinterpret_cast<int32_t(__cdecl*)()>(0x004B2220)()) { // TSingleton<CRadioManager>::IsInstantiated()
        auto pRadio = reinterpret_cast<void*(__cdecl*)()>(0x004B2210)(); // TSingleton<CRadioManager>::GetInstance()
        if (reinterpret_cast<int32_t(__thiscall*)(void*)>(0x004B2000)(pRadio)) { // pRadio->IsPlaying()
            if (pThis->m_bRadio_Restore) {
                // pRadio->Mute(volume)
                reinterpret_cast<void(__thiscall*)(void*, int32_t)>(0x006C8840)(pRadio, pThis->m_nRadio_Volume);
            }
            // pRadio->ShowUI(1)
            reinterpret_cast<void(__thiscall*)(void*, int32_t)>(0x006CB870)(pRadio, 1);
            pThis->m_bRadio_Restore = 0;
        }
    }

    // Wild-hunter mount handshake: clear m_nMountKind once we land on a map matching
    // the requested kind (1 = flying, 2 = swimming).
    auto pBasicStat = reinterpret_cast<void*(__thiscall*)(CWvsContext*)>(0x004701C0)(pThis);
    int32_t nJob = reinterpret_cast<int32_t(__thiscall*)(void*)>(0x0047D870)(pBasicStat);
    if (reinterpret_cast<int32_t(__cdecl*)(int32_t)>(0x004B7CE0)(nJob)) { // is_wildhunter_job
        if (auto pField = get_field()) {
            if ((pThis->m_nMountKind == 1 && reinterpret_cast<int32_t(__thiscall*)(CField*)>(0x008DE100)(pField)) ||
                (pThis->m_nMountKind == 2 && reinterpret_cast<int32_t(__thiscall*)(CField*)>(0x0063A040)(pField))) {
                pThis->m_nMountKind = 0;
            }
        }
    }

    // CConfig::SaveSessionInfo_FieldID(GetCurFieldID())
    int32_t nFieldID = reinterpret_cast<int32_t(__thiscall*)(CWvsContext*)>(0x009DB0A0)(pThis);
    reinterpret_cast<void(__thiscall*)(CConfig*, int32_t)>(0x004B2680)(CConfig::GetInstance(), nFieldID);
}


static auto CInputSystem__DetectJoystick = 0x00571740;

void __fastcall CInputSystem__DetectJoystick_hook(CInputSystem* pThis, void* _EDX) {
    // noop
}


static auto CCtrlComboBox__AddItem = 0x004DE640;

void __fastcall CCtrlComboBox__AddItem_hook(CCtrlComboBox* pThis, void* _EDX, char* sItemName, uint32_t dwParam) {
    pThis->AddItem("To Spouse", 0x6);   // ID_CHAT_TARGET_COUPLE
    pThis->AddItem("Whisper", 0x7);     // ID_CHAT_TARGET_WHISPER
    pThis->AddItem(sItemName, dwParam); // overwritten call for ID_CHAT_TARGET_ALL
}


void AttachClientBypass() {
    ATTACH_HOOK(CWvsApp__ctor, CWvsApp__ctor_hook);
    ATTACH_HOOK(CWvsApp__SetUp, CWvsApp__SetUp_hook);
    ATTACH_HOOK(CWvsApp__Run, CWvsApp__Run_hook);
    ATTACH_HOOK(CClientSocket__Connect, CClientSocket__Connect_hook);
    ATTACH_HOOK(CClientSocket__OnAliveReq, CClientSocket__OnAliveReq_hook);
    ATTACH_HOOK(CLogin__SendCheckPasswordPacket, CLogin__SendCheckPasswordPacket_hook);
    ATTACH_HOOK(CWvsContext__OnEnterField, CWvsContext__OnEnterField_hook);
    ATTACH_HOOK(CInputSystem__DetectJoystick, CInputSystem__DetectJoystick_hook);

    // CUIStatusBar::MakeCtrlEdit - add missing combo box items : "To Spouse", "Whisper"
    PatchCall(0x00870E82, reinterpret_cast<uintptr_t>(&CCtrlComboBox__AddItem_hook));

    PatchRetZero(0x004AB900); // DR_check
    PatchRetZero(0x0045EBD0); // Hidedll
    PatchRetZero(0x009BF6C0); // SendHSLog
    PatchRetZero(0x009BF370); // CeTracer::Run
    PatchRetZero(0x009BF390); // ShowStartUpWndModal
    PatchRetZero(0x00429000); // ShowAdBalloon
    PatchRetZero(0x009CC220); // CWvsApp::EnableWinKey
}