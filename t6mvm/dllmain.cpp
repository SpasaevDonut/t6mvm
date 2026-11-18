#include "pch.h"
#include <StdInclude.h>
#include "CustomDvars.h"
#include "UIBase.h"
#include "TheaterBase.h"
#include "WeaponAnimations.h"
#include "Settings.h"
#include "Trace.h"

void SetDemoDirectoryDvar()
{
    if (!Settings::Settings::GetDemosDirectory().empty())
        return;
    if (T6SDK::CrossVersion::GetGameVersion() == T6SDK::CrossVersion::GameVersion::V43)
    {
        std::string redactedPath = std::string(T6SDK::Dvars::GetString(*T6SDK::Dvars::DvarList::fs_homepath)) + "\\Plugins\\OpenNetStorage\\Cloud\\FILM_MP\\";
        Settings::Settings::SetDemosDirectory(redactedPath);
        T6SDK::ConsoleLog::LogTagged(T6SDK::ConsoleLog::C_INFO, false, "T6MVM", "Demos directory was restored from redacted mp game path.");
    }
    else if (T6SDK::CrossVersion::GetGameVersion() == T6SDK::CrossVersion::GameVersion::V41)
    {
        std::string redactedPath = std::string(T6SDK::Dvars::GetString(*T6SDK::Dvars::DvarList::fs_homepath)) + "\\Plugins\\OpenNetStorage\\Cloud\\FILM_ZM\\";
        Settings::Settings::SetDemosDirectory(redactedPath);
        T6SDK::ConsoleLog::LogTagged(T6SDK::ConsoleLog::C_INFO, false, "T6MVM", "Demos directory was restored from redacted zm game path.");
    }
}

void OnGameLoaded()
{
    T6MVM_Trace::Write("OnGameLoaded start");
    CustomDvars::Init();
    T6MVM_Trace::Write("CustomDvars::Init done");
    UIBase::Init();
    T6MVM_Trace::Write("UIBase::Init done");
    TheaterBase::Init();
    T6MVM_Trace::Write("TheaterBase::Init done");
    if (T6SDK::Typedefs::UI_OpenToastPopup)
        T6SDK::InternalFunctions::UI_OpenToastPopup("^5T6MVM loaded!", "Have fun editing!", 10000);
    T6MVM_Trace::Write("toast done");
    Streams::InitStreams();
    T6MVM_Trace::Write("Streams::InitStreams done");
    T6SDK::MAIN::InitializeDevConsole();
    T6MVM_Trace::Write("InitializeDevConsole done");
    WeaponAnimations::Init();
    T6MVM_Trace::Write("WeaponAnimations::Init done");
    //Read settings
    Settings::Settings::Load();
    T6MVM_Trace::Write("Settings::Load done");
    SetDemoDirectoryDvar();
    T6MVM_Trace::Write("SetDemoDirectoryDvar done");
    Common::LoadAllDemos();
    T6MVM_Trace::Write("LoadAllDemos done");
}

void Init()
{
    T6SDK::MAIN::DEBUG = true;
    T6SDK::MAIN::PreInit();
    T6SDK::Events::RegisterListener(T6SDK::EventType::OnGameLoaded, (uintptr_t)&OnGameLoaded);
}
HANDLE hThread;
inline static bool initialized = false;

DWORD WINAPI mainThread(LPVOID param)
{
    T6MVM_Trace::Write("worker start");
    Sleep(5000);
    T6MVM_Trace::Write("worker awake");
    T6MVM_Trace::Write("T6SDK PreInit begin");
    Init();
    initialized = true;
    T6MVM_Trace::Write("T6SDK PreInit end");
    SuspendThread(hThread);
    CloseHandle(hThread);
    return 0;
}

// Redacted's plugin loader rejects (and unloads) a plugin that does not export
// this whole entry-point set. PreInit starts T6MVM's worker thread; these
// entry points keep the module resident and satisfy the loader.
extern "C" __declspec(dllexport) int PreInit()
{
    T6MVM_Trace::Write("PreInit export");
    if (!hThread)
        hThread = CreateThread(nullptr, 0, &mainThread, nullptr, 0, nullptr);
    return 1;
}

extern "C" __declspec(dllexport) int PostInit()
{
    return 1;
}

extern "C" __declspec(dllexport) int Uninit()
{
    return 1;
}

extern "C" __declspec(dllexport) const char* AuthorInfo()
{
    return "T6MVM";
}

extern "C" __declspec(dllexport) const char* ExtendedInfo()
{
    return "T6MVM";
}

extern "C" __declspec(dllexport) int OfficialMod()
{
    return 0;
}

extern "C" __declspec(dllexport) int DependencyCount()
{
    return 0;
}

extern "C" __declspec(dllexport) const char* GetDependency(int index)
{
    return nullptr;
}

extern "C" __declspec(dllexport) int SendMessageB()
{
    return 1;
}

static HMODULE selfModule = nullptr;
static PVOID exceptionHandler = nullptr;

static LONG CALLBACK TraceException(PEXCEPTION_POINTERS exceptionInfo)
{
    EXCEPTION_RECORD* record = exceptionInfo->ExceptionRecord;
    if (record->ExceptionCode == 0xC0000005 || record->ExceptionCode == 0xC0000409 || record->ExceptionCode == 0xC00000FD)
    {
        uintptr_t address = (uintptr_t)record->ExceptionAddress;
        uintptr_t self = (uintptr_t)selfModule;
        uintptr_t game = (uintptr_t)GetModuleHandleA(nullptr);
        T6MVM_Trace::Write("VEH code=%08X addr=%p self=%p selfOff=%p game=%p gameOff=%p",
            record->ExceptionCode, record->ExceptionAddress, selfModule,
            (void*)(address >= self ? address - self : 0), (void*)game,
            (void*)(address >= game ? address - game : 0));
        if (record->NumberParameters > 1)
            T6MVM_Trace::Write("VEH access=%p address=%p", (void*)record->ExceptionInformation[0], (void*)record->ExceptionInformation[1]);
        T6MVM_Trace::Write("VEH eip=%p esp=%p ebp=%p", (void*)exceptionInfo->ContextRecord->Eip, (void*)exceptionInfo->ContextRecord->Esp, (void*)exceptionInfo->ContextRecord->Ebp);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD  ul_reason_for_call, LPVOID lpReserved)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        selfModule = hModule;
        T6MVM_Trace::Write("DllMain attach");
        exceptionHandler = AddVectoredExceptionHandler(1, TraceException);
        break;
    case DLL_PROCESS_DETACH:
        T6MVM_Trace::Write("DllMain detach");
        if (exceptionHandler)
            RemoveVectoredExceptionHandler(exceptionHandler);
        if (initialized)
            T6SDK::MAIN::DeInitialize();
        break;
    }
    return TRUE;
}