#pragma once

/**
Basically copied from UnEngineWin.h and cleaned up a little
*/

class FExecHook : public FExec, public FNotifyHook
{
public:
    FExecHook() = default;
    ~FExecHook();
private:
    WConfigProperties* m_pPreferences = nullptr; //Cleaned up by engine
    WObjectProperties* m_pEditActor = nullptr; //Reused across EditActor commands; cleaned up by engine

//From FExec
protected:
    HWND m_hToolWindow = NULL; //EditActor property window; CLauncher watches it to restore input when it closes
    bool m_bRestoreFullscreenOnToolClose = false; //We dropped exclusive fullscreen to show the tool window; return to it on close
    UBOOL Exec( const TCHAR* Cmd, FOutputDevice& Ar ) override;

//From FNotifyHook
private:
    void NotifyDestroy( void* Src ) override;
};