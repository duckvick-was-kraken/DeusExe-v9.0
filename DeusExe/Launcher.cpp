#include "stdafx.h"
#include <timeapi.h> //timeBeginPeriod/timeEndPeriod for frame-limiter Sleep granularity
#include "ConfigCacheDeusExe.h"
#include "Diagnostics.h"
#include "FileManagerDeusExe.h"
#include "Misc.h"
#include "RawInput.h"
#include "LauncherDialog.h"
#include "FixApp.h"
#include "ExecHook.h"
#include "NativeHooks.h"
#include "PluginManager.h"
#include "Launcher.h"

//Do not put before stdafx.h
#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#pragma comment(lib,"winmm.lib")

extern "C" {wchar_t GPackage[64] = L"Launch"; } //Will be set to exe name later

INT WINAPI WinMain(HINSTANCE /*hInInstance*/, HINSTANCE /*hPrevInstance*/, LPSTR /*lpCmdLine*/, INT /*nCmdShow*/)
{
    //Set up the crash handler first, so faults during start-up (including plugin loading) are reported with context.
    Misc::SetupCrashHandler();

    //Disable DEP before any DLLs load (needed for Galaxy.dll; also needs the exe linked /NXCOMPAT:NO). Logged after appInit, once GLog exists.
    const bool bDEPDisabled = Misc::SetDEP(0);

    const INITCOMMONCONTROLSEX CommonControlsInfo = { sizeof(INITCOMMONCONTROLSEX), ICC_TREEVIEW_CLASSES | ICC_LINK_CLASS };
    if (InitCommonControlsEx(&CommonControlsInfo) != TRUE)
    {
        return EXIT_FAILURE;
    }

    wcsncpy_s(GPackage, appPackage(), _TRUNCATE); //appStrcpy would overrun GPackage for a long executable name

    //Init core. The Deus Exe devices add timestamps, flush every line and log the engine's load progress and error history.
    FMallocWindows Malloc;
    FOutputDeviceFileDeusExe Log;
    FOutputDeviceErrorDeusExe Error;
    FFeedbackContextDeusExe Warn;

    //If -localdata command line option present, don't use user documents for data; can't use appCmdLine() yet.
    //Persist it in an environment variable so the mode survives an engine relaunch (which rebuilds the command line and drops our options).
    const wchar_t* const pszLocalDataEnvVar = L"DeusExeLocalData";
    const bool bLocalData = Misc::HasCommandLineSwitch(L"localdata") || GetEnvironmentVariable(pszLocalDataEnvVar, nullptr, 0) != 0;
    if(bLocalData)
    {
        SetEnvironmentVariable(pszLocalDataEnvVar, L"1"); //Inherited by the engine's self-relaunches
    }

    //A -gamename gives every name its own data directory, so even with -localdata the data no longer lives where the game is installed
    wchar_t szDataDir[MAX_PATH];
    std::unique_ptr<FFileManagerDeusExe> pFileManager(Misc::GetDataDir(szDataDir, bLocalData) ? new FFileManagerDeusExeDataDir(szDataDir) : new FFileManagerDeusExe);

    //Load plugins before appInit so they can hook the entire start-up; must outlive appInit and the launcher. PreAppInit/PostAppInit bracket appInit.
    CPluginManager Plugins(PROJECTNAME);
    Plugins.Dispatch(DeusExePluginEvent_PreAppInit);

    appInit(GPackage, GetCommandLine(), &Malloc, &Log, &Error, &Warn, pFileManager.get(), FConfigCacheDeusExe::Factory, 1);

    UBOOL bVerboseLogging = FALSE; //Only readable now: the log device is already running, so its first lines are always stock
    GConfig->GetBool(PROJECTNAME, L"VerboseLogging", bVerboseLogging);
    Misc::SetVerboseLogging(bVerboseLogging != 0);

    GLog->Logf(L"Deus Exe: version %s.", Misc::GetVersion());
    if (!bDEPDisabled)
    {
        GLog->Log(L"Failed to set process DEP flags.");
    }

    Plugins.Dispatch(DeusExePluginEvent_PostAppInit);

    pFileManager->AfterCoreInit();

    GIsStarted = 1;
    GIsServer = 1;
    GIsClient = !ParseParam(appCmdLine(), L"SERVER");
    GIsEditor = 0;
    GIsScriptable = 1;
    GLazyLoad = !GIsClient;

    {
        CLauncher Launcher(Plugins);
    }

    Plugins.Dispatch(DeusExePluginEvent_Shutdown); //While the DLLs are still loaded; the manager frees them when it goes out of scope

    //Uninit
    if (!GIsCriticalError)
    {
        appPreExit();
    }
    appExit();
    GIsStarted = 0;

    return EXIT_SUCCESS;
}

CLauncher::CLauncher(CPluginManager& Plugins)
{
    if (QueryPerformanceFrequency(&m_iPerfCounterFreq) == FALSE)
    {
        GError->Log(L"Failed to query performance counter.");
    }

    int iFirstRun = 0;
    GConfig->GetInt(L"FirstRun", L"FirstRun", iFirstRun);
    const bool bFirstRun = iFirstRun < ENGINE_VERSION;
    if (bFirstRun) //Select better default options
    {
        GConfig->SetString(L"Engine.Engine", L"GameRenderDevice", L"D3DDrv.D3DRenderDevice");
        GConfig->SetString(L"WinDrv.WindowsClient", L"FullscreenColorBits", L"32");
        wchar_t szTemp[1024];
        _itow_s(GetSystemMetrics(SM_CXSCREEN), szTemp, 10);
        GConfig->SetString(L"WinDrv.WindowsClient", L"FullscreenViewportX", szTemp);
        _itow_s(GetSystemMetrics(SM_CYSCREEN), szTemp, 10);
        GConfig->SetString(L"WinDrv.WindowsClient", L"FullscreenViewportY", szTemp);
    }

    //Show options dialog
    if (ParseParam(appCmdLine(), L"changevideo") || bFirstRun)
    {
        CFixApp FixApp;
        FixApp.Show(NULL);
        if (bFirstRun)
        {
            GConfig->SetInt(L"FirstRun", L"FirstRun", ENGINE_VERSION);
        }
    }

    //Show launcher dialog
    HMONITOR hMonitor = NULL;

    const auto DoLauncherDialog = [&hMonitor]
    {
        CLauncherDialog LD;
        const auto bRet = LD.Show(NULL);
        hMonitor = LD.GetChildWindowMonitor();
        return bRet;
    };

    if (!GIsClient || ParseParam(appCmdLine(), TEXT("skipdialog")) || DoLauncherDialog()) //Here the game actually starts
    {
        LoadSettings();

        static_cast<FFileManagerDeusExe*>(GFileManager)->OnGameStart();

        if (m_bUseSingleCPU)
        {
            if (SetProcessAffinityMask(GetCurrentProcess(), 0x1) == FALSE) //Force on single CPU
            {
                GLog->Log(L"Failed to set process affinity.");
            }
        }

        if (m_bRawInput) //If raw input is enabled, disable DirectInput
        {
            GConfig->SetBool(L"WinDrv.WindowsClient", L"UseDirectInput", FALSE);
        }

        if (m_bBorderlessFullscreenWindow) //In borderless mode, disable normal full screen
        {
            GConfig->SetBool(L"WinDrv.WindowsClient", L"StartupFullscreen", FALSE);
        }

        //Init windowing
        InitWindowing();

        //Create log window
        const std::unique_ptr<WLog> LogWindowPtr = std::make_unique<WLog>(static_cast<FOutputDeviceFile*>(GLog)->Filename, static_cast<FOutputDeviceFile*>(GLog)->LogAr, L"GameLog");
        GLogWindow = LogWindowPtr.get(); //Yup...
        GLogWindow->OpenWindow(!GIsClient, 0);
        GLogWindow->Log(NAME_Title, LocalizeGeneral("Start"));

        GExec = this;


        GIsGuarded = 1;
        try
        {
            //Init engine
            UClass* const pEngineClass = LoadClass<UGameEngine>(nullptr, L"ini:Engine.Engine.GameEngine", nullptr, LOAD_NoFail, nullptr);
            assert(pEngineClass);
            UEngine* const pEngine = ConstructObject<UEngine>(pEngineClass);
            assert(pEngine);
            if (!pEngine)
            {
                GError->Log(L"Engine initialization failed.");
            }

            Plugins.Dispatch(DeusExePluginEvent_PreEngineInit, pEngine);
            pEngine->Init();
            Plugins.Dispatch(DeusExePluginEvent_PostEngineInit, pEngine);

            GLogWindow->SetExec(pEngine); //If we directly set GExec, only our custom commands work
            GLogWindow->Log(NAME_Title, LocalizeGeneral("Run"));

            //Find window handle
            if (GIsClient)
            {
                if (pEngine->Client && pEngine->Client->Viewports.Num() > 0)
                {
                    m_pViewPort = pEngine->Client->Viewports(0);
                    m_hWnd = static_cast<const HWND>(m_pViewPort->GetWindow());
                }
                else
                {
                    GLog->Log(L"Unable to get viewport.");
                }
            }

            Plugins.Dispatch(DeusExePluginEvent_ViewportCreated, pEngine, m_pViewPort, m_hWnd);

            //Move window to launcher's monitor
            if (hMonitor != NULL && m_hWnd)
            {
                Misc::CenterWindowOnMonitor(m_hWnd, hMonitor);
            }

            if (m_bBorderlessFullscreenWindow)
            {
                ToggleBorderlessWindowedFullscreen();
            }

            //Initialize raw input
            if (m_bRawInput && m_hWnd)
            {
                if (!RegisterRawInput(m_hWnd))
                {
                    GError->Log(L"Raw input: Failed to register raw input device.");
                }
            }

            if (m_hWnd)
            {
                SetWindowSubclass(m_hWnd, &CLauncher::ViewportSubclassProc, 0, reinterpret_cast<DWORD_PTR>(this));
            }

            //Seed from the viewport size, which is what the main loop tracks; the client rect can differ in borderless mode
            if (GIsClient && m_bAutoFov && m_pViewPort && m_pViewPort->SizeX > 0 && m_pViewPort->SizeY > 0)
            {
                ApplyAutoFOV(static_cast<size_t>(m_pViewPort->SizeX), static_cast<size_t>(m_pViewPort->SizeY));
            }

            //Initialize native hooks
            CNativeHooks NativeHooks(PROJECTNAME);

            //Main loop
            GIsRunning = 1;
            if (!GIsRequestingExit)
            {
                Plugins.Dispatch(DeusExePluginEvent_PreMainLoop, pEngine, m_pViewPort, m_hWnd);
                MainLoop(pEngine);
            }
            GIsRunning = 0;

            GIsGuarded = 0;
        }
        catch(...)
        {
            GIsGuarded = 0;
            ReleaseCursor(); //MainLoop didn't get to do it, and the error message box needs a visible, unclipped cursor
            GError->HandleError(); //GErrorHist now holds the unwound call chain; this logs it and shows it
        }

        GLogWindow->Log(NAME_Title, LocalizeGeneral("Exit"));

        //appPreExit()/appExit() still run after this scope, so don't leave them pointing at destroyed objects
        GExec = nullptr;
        GLogWindow = nullptr;
    }

}

void CLauncher::ApplyAutoFOV(const size_t iSizeX, const size_t iSizeY)
{
    assert(m_iSizeX != iSizeX || m_iSizeY != iSizeY);
    assert(m_pViewPort);
    const float fFOV = Misc::CalcFOV(iSizeX, iSizeY);
    wchar_t szCmd[32];
    _snwprintf_s(szCmd, _TRUNCATE, L"fov %6.3f", fFOV);

    m_pViewPort->Exec(szCmd);
    m_iSizeX = iSizeX;
    m_iSizeY = iSizeY;
}

void CLauncher::MainLoop(UEngine* const pEngine)
{
    assert(pEngine);

    if(m_iPerfCounterFreq.QuadPart <= 0) //QueryPerformanceFrequency failed in the constructor; can't pace frames
    {
        GLog->Log(L"Main loop aborted: no usable performance counter.");
        return;
    }

    LARGE_INTEGER iOldTime;
    if(!QueryPerformanceCounter(&iOldTime)) //Initial time
    {
        return;
    }

    timeBeginPeriod(1); //Give Sleep() ~1ms granularity so the frame limiter can yield the CPU without overshooting

    LARGE_INTEGER iSecondStart = iOldTime;
    int iTickCount = 0;
    CLevelWatcher LevelWatcher;

    while (GIsRunning && !GIsRequestingExit)
    {
        LARGE_INTEGER iTime;
        QueryPerformanceCounter(&iTime);
        const float fDeltaTime = (iTime.QuadPart - iOldTime.QuadPart) / static_cast<float>(m_iPerfCounterFreq.QuadPart);
        iOldTime = iTime;

        //Tick (and render) every iteration with the real elapsed time, like the stock UnEngineWin.h loop; the frame rate is capped by sleeping at the bottom of the loop.
        pEngine->Tick(fDeltaTime);
        if(GWindowManager)
        {
            GWindowManager->Tick(fDeltaTime);
        }

        LevelWatcher.Update(pEngine); //Map transitions happen inside Tick(), so this reports them right after

        iTickCount++;
        const float fSinceSecondStart = (iTime.QuadPart - iSecondStart.QuadPart) / static_cast<float>(m_iPerfCounterFreq.QuadPart);
        if(fSinceSecondStart > 1.0f)
        {
            pEngine->CurrentTickRate = static_cast<float>(iTickCount) / fSinceSecondStart;
            iSecondStart = iTime;
            iTickCount = 0;
        }

        //Re-fetch the viewport each frame: a video-mode change can swap the viewport object.
        if(pEngine->Client && pEngine->Client->Viewports.Num() > 0)
        {
            m_pViewPort = pEngine->Client->Viewports(0);
        }
        else
        {
            m_pViewPort = nullptr;
        }

        //WinDrv recreates the viewport window on a video-mode change, so re-attach (focus tracking, raw input, subclass) when the handle changes.
        if(m_pViewPort)
        {
            const HWND hViewportWindow = static_cast<HWND>(m_pViewPort->GetWindow());
            if(hViewportWindow != NULL && hViewportWindow != m_hWnd)
            {
                if(m_hWnd != NULL)
                {
                    RemoveWindowSubclass(m_hWnd, &CLauncher::ViewportSubclassProc, 0);
                }
                m_hWnd = hViewportWindow;
                if(m_bRawInput)
                {
                    RegisterRawInput(m_hWnd);
                }
                SetWindowSubclass(m_hWnd, &CLauncher::ViewportSubclassProc, 0, reinterpret_cast<DWORD_PTR>(this));
            }
        }

        //GetCursorPos fails while another desktop is active (UAC prompt, locked workstation), leaving the point unset
        POINT CursorPos = {};
        const bool bHaveCursorPos = GetCursorPos(&CursorPos) != FALSE;
        const bool bMouseOverWindow = bHaveCursorPos && WindowFromPoint(CursorPos) == m_hWnd;
        const HWND hForeground = GetForegroundWindow();
        const bool bHasFocus = m_hWnd != NULL && hForeground == m_hWnd;

        //A renderer can leave the game stuck minimized after alt+tabbing back into fullscreen. Restore when our process
        //owns the foreground but the viewport is still iconic, so we don't fight an intentional alt+tab away.
        if(m_hWnd != NULL && IsIconic(m_hWnd))
        {
            DWORD dwForegroundPid = 0;
            GetWindowThreadProcessId(hForeground, &dwForegroundPid);
            if(dwForegroundPid == GetCurrentProcessId())
            {
                ShowWindow(m_hWnd, SW_RESTORE);
            }
        }

        RECT rClientArea;
        if(m_pViewPort && m_hWnd != NULL && GetClientRect(m_hWnd, &rClientArea)) //Fails once the window is gone, leaving the rect unset
        {
            std::array<POINT, 2> ClientPoints = { { {rClientArea.left, rClientArea.top}, {rClientArea.right, rClientArea.bottom} } };
            MapWindowPoints(m_hWnd, NULL, ClientPoints.data(), ClientPoints.size());
            const RECT rClientScreen = { ClientPoints[0].x, ClientPoints[0].y, ClientPoints[1].x, ClientPoints[1].y };

            //Actor, and its root window, are briefly absent during level transitions and viewport re-creation
            const APlayerPawnExt* const pPlayer = static_cast<APlayerPawnExt*>(m_pViewPort->Actor);
            XRootWindow* const pRoot = pPlayer ? static_cast<XRootWindow*>(pPlayer->rootWindow) : nullptr;

            //PeekMessage() doesn't get WM_SIZE
            //Default/desired FOV check is so we don't change FOV while zoomed in
            if (m_bAutoFov && pPlayer && pPlayer->DesiredFOV == pPlayer->DefaultFOV)
            {
                const size_t iSizeX = static_cast<size_t>(m_pViewPort->SizeX);
                const size_t iSizeY = static_cast<size_t>(m_pViewPort->SizeY);

                //Handle auto FOV
                if(m_iSizeX != iSizeX  || m_iSizeY != iSizeY)
                {
                    ApplyAutoFOV(iSizeX, iSizeY);
                }
            }
            
            //pEngine->Client->Viewports(0)->SetMouseCapture()'s cursor centering doesn't work with raw input.
            //Why doesn't it work? Because we block WM_MOUSEMOVE messages, which the game apparently uses to center the cursor.
            //SetCursorPos() still works, though, which I'd assume the game uses; ClipCursor() didn't exist until Win2000.
            //Also, if you force the game to turn off mouse centering, the camera doesn't work; does it use the WM_MOUSEMOVE messages generated by SetCursorPos() to actually move the camera?

            //Issue: using raw input, in full-screen mode you can move the cursor around while controlling the camera, if you then open the menu and slightly move the mouse
            //The game's cursor will snap to the Windows mouse cursor position.
            //Theory as to why: SetMouseCapture() without clipping resets the mouse position to previous (looking at headers / UT X driver code).
            //In full-screen mode this is not done when going to the menu (observed in Windows Input mode, cursor keeps being centered).
            //Because the game uses relative messages for menu mouse input (MouseDelta(), not MousePosition()) this doesn't matter.

            //Other observed behavior in Windows Input mode, running windowed: mouse is clipped to window dimensions + centered in menu mode (like in camera mode)
            //Until alt+tab or mission start, at which point it's not clipped and window can be resized

            //Forcing mouse to be centered in menu mode makes it feel weird, doesn't match Windows mouse cursor movements

            /* Tests
            1. Does menu cursor track Windows cursor nicely
            2. Can cursor immediately leave window when menu first pops up (who cares)
            3. Does resize cursor pop up on window edges
            4. When alt+tabbing while not in a menu, make sure mouse isn't clipped to game window area
            5. Both windowed and full screen: when having controlled the camera and then entering a menu, the mouse should either be centered or in the position where it last was.
               When touching the mouse, it should not teleport due to having been moved in camera mode.
            5a. Still happens in raw input + windowed mode when entering menu without having first moved mouse, acceptable.
            6. When alt+tabbing and not in a menu, make sure camera isn't controlled by mouse movements until the window is clicked
            7. Make sure Windows mouse cursor is not visible (other than during testing)
            8. Make sure preferences window is usable (no hidden cursor) and that it doesn't pop up a phantom cursor in menu mode
            9. When looking around with preferences window on top, cursor doesn't appear
            10. In fullscreen mode, when rapidly clicking, window isn't minimized
            11. In two-monitor fullscreen make sure mouse can't move outside of monitor
            */

            const bool bInMenu = pRoot && pRoot->IsMouseGrabbed() != 0;

            //Confine the OS cursor to the game's client area while we own the mouse.
            if (m_bRawInput && bHasFocus)
            {
                if (m_pViewPort->IsFullscreen() && bInMenu && !m_bPrevInMenu) //Fixes that in fullscreen mode, windows mouse cursor pos isn't matched to DX menu cursor
                {
                    float fX, fY;
                    pRoot->GetRootCursorPos(&fX, &fY);
                    POINT p{static_cast<int>(fX), static_cast<int>(fY)};
                    ClientToScreen(m_hWnd, &p);
                    SetCursorPos(p.x, p.y);
                }

                if (m_pViewPort->IsFullscreen())
                {
                    //Exclusive fullscreen: confine to the whole monitor (the client-rect mapping is unreliable here).
                    MONITORINFO MonitorInfo = { sizeof(MonitorInfo) };
                    if (GetMonitorInfo(MonitorFromWindow(m_hWnd, MONITOR_DEFAULTTONEAREST), &MonitorInfo))
                    {
                        ClipCursor(&MonitorInfo.rcMonitor);
                    }
                    else
                    {
                        ClipCursor(&rClientScreen);
                    }
                    m_bCursorClipped = true;
                }
                //Borderless also covers the whole monitor, so confine in menu mode too. A plain window only confines
                //during camera control, so the user can still reach the border/title bar while a menu is open.
                else if (m_bInBorderlessFullscreenWindow || !bInMenu)
                {
                    ClipCursor(&rClientScreen);
                    m_bCursorClipped = true;
                }
                else
                {
                    ClipCursor(NULL);
                    m_bCursorClipped = false;
                }
            }
            else if (m_bCursorClipped) //Don't leave the cursor confined behind whichever window took focus
            {
                ClipCursor(NULL);
                m_bCursorClipped = false;
            }
            m_bPrevInMenu = bInMenu;

            const bool bMouseInClientRect = bHaveCursorPos && PtInRect(&rClientScreen, CursorPos)!=0; //This makes sure resize cursor isn't hidden
            const bool bCaptured = GetCapture() == m_hWnd;
            //Only hide the cursor while the game owns focus, else it vanishes over the game area behind a focused tool window.
            //Want to show cursor when over preferences window when we don't have focus, but not when it's under the window if we do
            SetCursorHidden(bHasFocus && bMouseInClientRect && (bMouseOverWindow || bCaptured));
        }

        MSG Msg;
        while (PeekMessage(&Msg, NULL, 0, 0, PM_REMOVE))
        {
            bool bSkipMessage = false;

            switch (Msg.message)
            {
            case WM_QUIT:
                GIsRequestingExit = 1;
                break;

            case WM_MOUSEMOVE:
                if (m_pViewPort && m_bRawInput)
                {
                    if (bMouseOverWindow) //Because preferences window defers mousemove calls to us, somehow
                    {
                        //Use WM_MOUSEMOVE to control menu cursor
                        const int iXPos = GET_X_LPARAM(Msg.lParam);
                        const int iYPos = GET_Y_LPARAM(Msg.lParam);
                        pEngine->MousePosition(m_pViewPort, 0, static_cast<float>(iXPos), static_cast<float>(iYPos));
                    }
                    bSkipMessage = true;
                }
                break;

            case WM_LBUTTONDOWN:
            case WM_RBUTTONDOWN:
            case WM_MBUTTONDOWN:
                //After alt+tab
                if (Msg.hwnd == m_hWnd && !bHasFocus)
                {
                    bSkipMessage = true;
                }
                break;

            case WM_KEYDOWN:
            case WM_SYSKEYDOWN:
                //Only for the game window: alt+enter in the log window shouldn't restyle the viewport
                if (m_bBorderlessFullscreenWindow && Msg.hwnd == m_hWnd && Msg.wParam == VK_RETURN && (HIWORD(Msg.lParam) & KF_ALTDOWN)) //User hits alt+enter
                {
                    ToggleBorderlessWindowedFullscreen();
                    bSkipMessage = true;
                }
                break;


            case WM_INPUT:
            {
                //Use raw input to control camera
                if (m_pViewPort && bHasFocus)
                {
                    RAWINPUT raw;
                    UINT rawSize = sizeof(raw);
                    //On failure this returns (UINT)-1 and leaves 'raw' untouched, so the deltas below would be stack garbage
                    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(Msg.lParam), RID_INPUT, &raw, &rawSize, sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1) || raw.header.dwType != RIM_TYPEMOUSE)
                    {
                        break;
                    }

                    const float fDeltaX = static_cast<float>(raw.data.mouse.lLastX);
                    const float fDeltaY = static_cast<float>(raw.data.mouse.lLastY);
                    if(fDeltaX != 0.0f)
                    {
                        pEngine->InputEvent(m_pViewPort, EInputKey::IK_MouseX, EInputAction::IST_Axis, fDeltaX);
                    }
                    if(fDeltaY != 0.0f)
                    {
                        pEngine->InputEvent(m_pViewPort, EInputKey::IK_MouseY, EInputAction::IST_Axis, -fDeltaY);
                    }

                    if (raw.data.mouse.ulButtons & RI_MOUSE_BUTTON_4_UP)
                    {
                        pEngine->InputEvent(m_pViewPort, EInputKey::IK_Unknown05, EInputAction::IST_Release);
                    }
                    else if (raw.data.mouse.ulButtons & RI_MOUSE_BUTTON_4_DOWN)
                    {
                        pEngine->InputEvent(m_pViewPort, EInputKey::IK_Unknown05, EInputAction::IST_Press);
                    }

                    if (raw.data.mouse.ulButtons & RI_MOUSE_BUTTON_5_UP)
                    {
                        pEngine->InputEvent(m_pViewPort, EInputKey::IK_Unknown06, EInputAction::IST_Release);
                    }
                    else if (raw.data.mouse.ulButtons & RI_MOUSE_BUTTON_5_DOWN)
                    {
                        pEngine->InputEvent(m_pViewPort, EInputKey::IK_Unknown06, EInputAction::IST_Press);
                    }

                    bSkipMessage = true;
                }
            }
                break;
            }

            if(!bSkipMessage)
            {
                TranslateMessage(&Msg);
                DispatchMessage(&Msg);
            }
        }

        if(m_hWnd != NULL && !IsWindow(m_hWnd)) //Force window close handling
        {
            ReleaseCursor();
            m_pViewPort = nullptr;
            m_hWnd = NULL;
        }

        //When the EditActor window closes, re-enter the fullscreen we dropped for it: that forces WinDrv's full input
        //re-init, the only thing that reliably restores keyboard/mouse routing after it stole focus mid-switch.
        if(m_hToolWindow != NULL && !IsWindow(m_hToolWindow))
        {
            m_hToolWindow = NULL;
            if(m_hWnd != NULL)
            {
                SetForegroundWindow(m_hWnd);
                if(m_bRestoreFullscreenOnToolClose && m_pViewPort && !m_pViewPort->IsFullscreen())
                {
                    m_pViewPort->Exec(TEXT("ToggleFullscreen"));
                }
            }
            m_bRestoreFullscreenOnToolClose = false;
        }

        //Cap the frame rate by sleeping away the rest of the frame's period. Cap = min(user FPSLimit, engine
        //GetMaxTickRate()); 0 = unlimited. Sleep(1) while >~6ms remain (timeBeginPeriod gives ~1ms granularity), then
        //spin for the final approach. Paced from the frame start (iTime) so the next delta lands on one period, no drift.
        if(!GIsRequestingExit)
        {
            const float fEngineMaxTickRate = pEngine->GetMaxTickRate();
            float fMaxFPS = m_fFPSLimit; //Already clamped to >= 0 in LoadSettings
            if(fEngineMaxTickRate > 0.0f && (fMaxFPS == 0.0f || fEngineMaxTickRate < fMaxFPS))
            {
                fMaxFPS = fEngineMaxTickRate;
            }
            if(fMaxFPS > 0.0f)
            {
                const LONGLONG iPeriodTicks = static_cast<LONGLONG>(m_iPerfCounterFreq.QuadPart / fMaxFPS);
                const LONGLONG iSpinTicks = m_iPerfCounterFreq.QuadPart * 6 / 1000; //~6ms worth of counter ticks
                for(;;)
                {
                    LARGE_INTEGER iNow;
                    QueryPerformanceCounter(&iNow);
                    const LONGLONG iRemaining = iPeriodTicks - (iNow.QuadPart - iTime.QuadPart);
                    if(iRemaining <= 0)
                    {
                        break;
                    }
                    if(iRemaining > iSpinTicks) //More than ~6ms to go: yield instead of busy-spinning
                    {
                        Sleep(1);
                    }
                    else
                    {
                        YieldProcessor(); //Hint to the CPU that this is a spin-wait, so the core isn't hammered
                    }
                }
            }
        }
    }

    timeEndPeriod(1);
    ReleaseCursor();
}

void CLauncher::SetCursorHidden(const bool bHide)
{
    //ShowCursor keeps a display counter, so only step it on an actual change: it stays at -1 (hidden) or 0 (shown)
    const int iTarget = bHide ? -1 : 0;
    constexpr int iMaxSteps = 32;
    int iCount = ShowCursor(bHide ? FALSE : TRUE);
    for(int i = 0; iCount != iTarget && i < iMaxSteps; i++)
    {
        iCount = ShowCursor(iCount < iTarget ? TRUE : FALSE);
    }
}

void CLauncher::ReleaseCursor()
{
    if(m_bCursorClipped)
    {
        ClipCursor(NULL);
        m_bCursorClipped = false;
    }
    SetCursorHidden(false);
}

void CLauncher::LoadSettings()
{
    assert(GConfig);
    int iFPSLimit = static_cast<int>(m_fFPSLimit);
    GConfig->GetInt(PROJECTNAME, L"FPSLimit", iFPSLimit);
    if(iFPSLimit < 0) //A negative/malformed ini value would otherwise disable the limit entirely
    {
        iFPSLimit = 0;
    }
    m_fFPSLimit = static_cast<float>(iFPSLimit);

    GConfig->GetBool(PROJECTNAME, L"RawInput", m_bRawInput);
    GConfig->GetBool(PROJECTNAME, L"UseAutoFOV", m_bAutoFov);
    GConfig->GetBool(PROJECTNAME, L"BorderlessFullscreenWindow", m_bBorderlessFullscreenWindow);
    GConfig->GetBool(PROJECTNAME, L"BorderlessFullscreenWindowAllMonitors", m_bBorderlessFullscreenWindowUseAllMonitors);
    GConfig->GetBool(PROJECTNAME, L"UseSingleCPU", m_bUseSingleCPU);
}

void CLauncher::ToggleBorderlessWindowedFullscreen()
{
    if(m_hWnd == NULL) //No viewport window (dedicated server, or it's already gone): nothing to restyle, and the mode flag must not flip out of sync
    {
        return;
    }
    Misc::SetBorderlessFullscreen(m_hWnd, m_bInBorderlessFullscreenWindow ? Misc::BorderlessFullscreenMode::NONE : m_bBorderlessFullscreenWindowUseAllMonitors ? Misc::BorderlessFullscreenMode::ALL_MONITORS : Misc::BorderlessFullscreenMode::CURRENT_MONITOR);
    m_bInBorderlessFullscreenWindow = !m_bInBorderlessFullscreenWindow;
}

//Subclass proc for the WinDrv viewport window. Runs ahead of WinDrv's own proc, which we chain to via DefSubclassProc.
LRESULT CALLBACK CLauncher::ViewportSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR /*dwRefData*/)
{
    switch (uMsg)
    {
    case WM_MOUSEACTIVATE:
        //Do not fire when regaining focus
        if (LOWORD(lParam) == HTCLIENT)
        {
            return MA_ACTIVATEANDEAT;
        }
        break;

    case WM_ACTIVATE:
        if (LOWORD(wParam) != WA_INACTIVE && IsIconic(hWnd))
        {
            ShowWindow(hWnd, SW_RESTORE);
        }
        break;

    case WM_NCDESTROY:
        RemoveWindowSubclass(hWnd, &CLauncher::ViewportSubclassProc, uIdSubclass);
        break;
    }

    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

UBOOL CLauncher::Exec(const TCHAR * Cmd, FOutputDevice & Ar)
{
    if (ParseCommand(&Cmd, TEXT("ToggleFullScreen")))
    {
        assert(m_pViewPort);
        if (m_bBorderlessFullscreenWindow) //In borderless mode, prevent switch to 'real' fullscreen
        {
            ToggleBorderlessWindowedFullscreen();

            return TRUE;
        }

        return FALSE;
    }
    else if (ParseCommand(&Cmd, TEXT("SetRes")))
    {
        if (m_bInBorderlessFullscreenWindow) //Block resolution changes in borderless fullscreen mode
        {
            return TRUE;
        }
        return FALSE;
    }
    else
    {
        return FExecHook::Exec(Cmd, Ar);
    }
}
