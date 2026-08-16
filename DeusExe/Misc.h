#pragma once

class UObject;
class UStruct;

namespace Misc
{
    bool SetDEP(const DWORD dwFlags);

    bool GetUserDocsDir(wchar_t(&pszBuf)[MAX_PATH]);

    bool GetGameSystemDir(wchar_t(&pszBuf)[MAX_PATH]);

    const wchar_t* GetGameName();

    bool HasCommandLineSwitch(const wchar_t* const pszSwitch);

    bool GetDataDir(wchar_t(&pszBuf)[MAX_PATH], const bool bLocalData);

    const wchar_t* GetVersion();

    float GetDefaultFOV();

    float CalcFOV(const size_t iResX, const size_t iResY);

    void CenterWindowOnMonitor(const HWND hWnd, const HMONITOR hMonitor);

    enum class BorderlessFullscreenMode { NONE, CURRENT_MONITOR, ALL_MONITORS };
    void SetBorderlessFullscreen(const HWND hWnd, const BorderlessFullscreenMode Mode);

    void SetScriptTrace(const UObject* const pObject, const UStruct* const pFunction);

    void SetVerboseLogging(const bool bEnabled); //!< Gates the diagnostics that cost time every frame: the live (timestamped, flushed) log and the script trace

    bool IsVerboseLogging();

    void SetCrashPhase(const wchar_t* const pszPhase); //!< Engine stage (slow task/status update) a crash would be attributed to

    void SetCrashMap(const wchar_t* const pszMap);

    void RecordFileOpen(const wchar_t* const pszPath); //!< Feeds the ring of recently read files a crash report lists; file opens never reach the log

    const wchar_t* GetCrashContext(wchar_t* const pszBuffer, const size_t count); //!< Map, engine stage and script context; safe to call mid-crash

    const wchar_t* FormatScriptContext(wchar_t* const pszBuffer, const size_t count, const UObject* const pObject, const UStruct* const pFunction);

    void LogCrashDetail(const EXCEPTION_POINTERS* const pExceptionInfo); //!< Faulting address, registers, call stack and the objects behind them; pass null to use the recorded fault

    void SetupCrashHandler();
};
