#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <oleacc.h>
#include <UIAutomation.h>
#include <stdint.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <iterator>
#include <wchar.h>
#include <winternl.h>

#pragma comment(lib, "oleacc.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "UIAutomationCore.lib")

namespace
{
#define IDI_TRICBAR_ICON 101
constexpr wchar_t kWindowClass[] = L"TricBarEngineHidden";
constexpr wchar_t kMutexName[] = L"TricBar_SingleInstance";
constexpr wchar_t kAliveName[] = L"TricBar_Blur_Alive";
constexpr wchar_t kActiveName[] = L"TricBar_Blur_Active";
constexpr wchar_t kAppliedName[] = L"TricBar_Blur_Applied";
constexpr wchar_t kTaskbarAppCountMapping[] = L"Local\\TricBar_TaskbarAppCount";
constexpr wchar_t kReloadMessage[] = L"TricBar_Reload";
constexpr wchar_t kTaskbarAppButtonClass[] = L"Taskbar.TaskListButtonAutomationPeer";
constexpr int kKeepAliveTimer = 1;
constexpr int kCenterTimer = 2;
constexpr int kTaskbarAppCountTimer = 3;
constexpr int kTaskbarAppCountFallbackTimer = 4;
constexpr int kMaxTaskbars = 16;
constexpr UINT kTaskbarAppCountDebounceMs = 10;
constexpr UINT kTaskbarAppCountFallbackMs = 1000;
constexpr int kAdjustX = 28;
constexpr uint32_t kWmTray = WM_APP + 1;
constexpr UINT kMenuSettings = 1;
constexpr UINT kMenuExit = 2;
constexpr int kWcaAccentPolicy = 19;
constexpr int kAccentDisabled = 0;
constexpr int kAccentGradient = 1;
constexpr int kAccentTransparentGradient = 2;
constexpr int kAccentBlur = 3;
constexpr int kAccentAcrylic = 4;
constexpr DWORD kEventCreate = EVENT_OBJECT_CREATE;
constexpr DWORD kEventLocation = EVENT_OBJECT_LOCATIONCHANGE;
constexpr DWORD kEventName = EVENT_OBJECT_NAMECHANGE;

enum class Mode { Normal, Clear, Blur, Acrylic, LiquidGlass, Color };

struct AccentPolicy
{
    int state;
    int flags;
    uint32_t color;
    int animationId;
};
struct WindowCompositionAttributeData
{
    int attribute;
    void* data;
    SIZE_T size;
};
using SetWindowCompositionAttributeFn = BOOL(WINAPI*)(HWND, WindowCompositionAttributeData*);
using InitXamlDiagnosticsFn = HRESULT(STDMETHODCALLTYPE*)(
    LPCWSTR, UINT, LPCWSTR, LPCWSTR, GUID, LPCWSTR);
using InstallAccentHookFn = BOOL(WINAPI*)(HWND);
using UninstallAccentHooksFn = void(WINAPI*)();
struct Config
{
    Mode mode = Mode::Acrylic;
    int rgb = 0x808080;
    int opacity = 60;
    bool center = true;
};

struct OleApartment
{
    HRESULT result = OleInitialize(nullptr);
    ~OleApartment()
    {
        if (SUCCEEDED(result)) OleUninitialize();
    }
};

HWND g_window;
HWINEVENTHOOK g_eventHook;
HWINEVENTHOOK g_taskbarAppCountEventHooks[5]{};
DWORD g_taskbarAppCountExplorerPid;
HANDLE g_mutex;
HANDLE g_alive;
HANDLE g_active;
HANDLE g_applied;
HANDLE g_taskbarAppCountMapping;
volatile LONG* g_taskbarAppCountView;
HMODULE g_accentLibrary;
HICON g_trayIcon;
IUIAutomation* g_uiAutomation;
SetWindowCompositionAttributeFn g_setComposition;
InstallAccentHookFn g_installAccentHook;
UninstallAccentHooksFn g_uninstallAccentHooks;
HWND g_taskbars[kMaxTaskbars]{};
int g_taskbarCount;
HWND g_tray;
HWND g_taskList;
HWND g_taskListParent;
DWORD g_eventExplorerPid;
DWORD g_accentHookPid;
DWORD g_tapPid;
int g_tapAttempts;
int g_tapInjections;
ULONGLONG g_nextTapAttempt;
volatile LONG g_tapBusy;
bool g_exiting;
Config g_config;
FILETIME g_configStamp{};
bool g_hasConfigStamp;
UINT g_reloadMessage;
int g_windowsBuild;
bool g_uiAutomationFailureLogged;
std::wstring g_tapPath;
std::wstring g_tooltipSuffix;

std::wstring GetConfigPath()
{
    wchar_t base[MAX_PATH]{};
    DWORD n = GetEnvironmentVariableW(L"APPDATA", base, MAX_PATH);
    if (!n || n >= MAX_PATH) return {};
    std::wstring path(base);
    path += L"\\TricBar\\config.ini";
    return path;
}

std::wstring GetModulePath()
{
    wchar_t path[32768]{};
    DWORD n = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    return n && n < std::size(path) ? std::wstring(path, n) : std::wstring();
}

std::wstring GetModuleDirectory()
{
    std::wstring path = GetModulePath();
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    path.resize(slash);
    return path;
}

bool FileStamp(const std::wstring& path, FILETIME* stamp)
{
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return false;
    *stamp = data.ftLastWriteTime;
    return true;
}

void ReadConfig()
{
    const std::wstring path = GetConfigPath();
    FILETIME stamp{};
    if (path.empty() || !FileStamp(path, &stamp)) return;
    if (g_hasConfigStamp && CompareFileTime(&stamp, &g_configStamp) == 0) return;

    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;

    char text[2048]{};
    DWORD read = 0;
    const BOOL ok = ReadFile(file, text, sizeof(text) - 1, &read, nullptr);
    CloseHandle(file);
    if (!ok) return;
    if (read >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF)
    {
        memmove(text, text + 3, read - 3);
        read -= 3;
        text[read] = '\0';
    }

    Config next;
    char* context = nullptr;
    for (char* line = strtok_s(text, "\r\n", &context); line; line = strtok_s(nullptr, "\r\n", &context))
    {
        char* equals = strchr(line, '=');
        if (!equals) continue;
        *equals++ = '\0';
        if (_stricmp(line, "mode") == 0)
        {
            if (_stricmp(equals, "normal") == 0) next.mode = Mode::Normal;
            else if (_stricmp(equals, "blur") == 0) next.mode = Mode::Blur;
            else if (_stricmp(equals, "acrylic") == 0 || _stricmp(equals, "macos") == 0)
                next.mode = Mode::Acrylic;
            else if (_stricmp(equals, "lightliquidglass") == 0 || _stricmp(equals, "liquidglass") == 0 || _stricmp(equals, "mica") == 0)
                next.mode = Mode::LiquidGlass;
            else if (_stricmp(equals, "darkliquidglass") == 0)
                next.mode = Mode::LiquidGlass;
            else if (_stricmp(equals, "color") == 0) next.mode = Mode::Color;
            else next.mode = Mode::Clear;
        }
        else if (_stricmp(line, "color") == 0)
        {
            if (*equals == '#') ++equals;
            next.rgb = static_cast<int>(strtoul(equals, nullptr, 16) & 0xFFFFFF);
        }
        else if (_stricmp(line, "opacity") == 0)
        {
            next.opacity = atoi(equals);
            if (next.opacity < 0) next.opacity = 0;
            if (next.opacity > 100) next.opacity = 100;
        }
        else if (_stricmp(line, "center") == 0)
            next.center = *equals != '0';
    }
    g_config = next;
    g_configStamp = stamp;
    g_hasConfigStamp = true;
}

uint32_t ToAbgr(uint32_t alpha)
{
    const uint32_t rgb = static_cast<uint32_t>(g_config.rgb);
    return (alpha << 24) | ((rgb & 0xFF) << 16) | (rgb & 0xFF00) | ((rgb >> 16) & 0xFF);
}

void ComputeAccent(int* state, uint32_t* color)
{
    const uint32_t alpha = static_cast<uint32_t>(g_config.opacity * 255 / 100);
    if (g_windowsBuild >= 22621)
    {
        if (g_config.mode == Mode::Normal)
        {
            *state = kAccentDisabled;
            *color = 0;
        }
        else if (g_config.mode == Mode::Acrylic)
        {
            *state = kAccentAcrylic;
            *color = ToAbgr(alpha ? alpha : 1);
        }
        else
        {
            *state = kAccentTransparentGradient;
            *color = 0;
        }
        return;
    }
    switch (g_config.mode)
    {
    case Mode::Normal: *state = kAccentDisabled; *color = 0; break;
    case Mode::Blur: *state = kAccentBlur; *color = ToAbgr(alpha); break;
    case Mode::Acrylic: *state = kAccentAcrylic; *color = ToAbgr(alpha ? alpha : 1); break;
    case Mode::LiquidGlass: *state = kAccentAcrylic; *color = ToAbgr(alpha ? alpha : 1); break;
    case Mode::Color:
        *state = g_config.opacity < 100 ? kAccentTransparentGradient : kAccentGradient;
        *color = ToAbgr(alpha);
        break;
    default: *state = kAccentTransparentGradient; *color = 0; break;
    }
}

void RefreshTaskbars()
{
    g_taskbarCount = 0;
    HWND primary = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (primary) g_taskbars[g_taskbarCount++] = primary;
    HWND current = nullptr;
    while (g_taskbarCount < kMaxTaskbars &&
        (current = FindWindowExW(nullptr, current, L"Shell_SecondaryTrayWnd", nullptr)) != nullptr)
        g_taskbars[g_taskbarCount++] = current;
}

bool TaskbarsStale()
{
    if (!g_taskbarCount) return true;
    for (int i = 0; i < g_taskbarCount; ++i)
        if (!IsWindow(g_taskbars[i])) return true;
    return false;
}

bool InitializeTaskbarAppCounter()
{
    g_taskbarAppCountMapping = CreateFileMappingW(
        INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(LONG), kTaskbarAppCountMapping);
    if (!g_taskbarAppCountMapping) return false;
    g_taskbarAppCountView = static_cast<volatile LONG*>(
        MapViewOfFile(g_taskbarAppCountMapping, FILE_MAP_WRITE, 0, 0, sizeof(LONG)));
    if (!g_taskbarAppCountView)
    {
        CloseHandle(g_taskbarAppCountMapping);
        g_taskbarAppCountMapping = nullptr;
        return false;
    }
    InterlockedExchange(g_taskbarAppCountView, 0);

    return true;
}

void ReleaseTaskbarAppAutomation()
{
    if (g_uiAutomation)
    {
        g_uiAutomation->Release();
        g_uiAutomation = nullptr;
    }
    g_uiAutomationFailureLogged = false;
}

void UpdateTaskbarAppCount()
{
    if (g_config.mode != Mode::LiquidGlass) return;

    if (!g_uiAutomation)
    {
        const HRESULT result = CoCreateInstance(
            CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&g_uiAutomation));
        if (FAILED(result))
        {
            if (!g_uiAutomationFailureLogged)
            {
                OutputDebugStringW(L"TricBar: UI Automation initialization failed.\n");
                g_uiAutomationFailureLogged = true;
            }
            return;
        }
        g_uiAutomationFailureLogged = false;
    }

    HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    IUIAutomationElement* root = nullptr;
    IUIAutomationCondition* buttonCondition = nullptr;
    IUIAutomationElementArray* buttons = nullptr;
    VARIANT className{};
    VariantInit(&className);
    className.vt = VT_BSTR;
    className.bstrVal = SysAllocString(kTaskbarAppButtonClass);

    HRESULT result = className.bstrVal
        ? (taskbar
        ? g_uiAutomation->ElementFromHandle(taskbar, &root)
        : HRESULT_FROM_WIN32(ERROR_NOT_FOUND))
        : E_OUTOFMEMORY;
    if (SUCCEEDED(result))
        result = g_uiAutomation->CreatePropertyCondition(
            UIA_ClassNamePropertyId, className, &buttonCondition);
    if (SUCCEEDED(result))
        result = root->FindAll(TreeScope_Descendants, buttonCondition, &buttons);
    VariantClear(&className);
    if (FAILED(result))
    {
        if (!g_uiAutomationFailureLogged)
        {
            OutputDebugStringW(L"TricBar: UI Automation could not enumerate taskbar buttons.\n");
            g_uiAutomationFailureLogged = true;
        }
        if (buttons) buttons->Release();
        if (buttonCondition) buttonCondition->Release();
        if (root) root->Release();
        return;
    }

    int count = 0;
    int buttonCount = 0;
    buttons->get_Length(&buttonCount);
    for (int index = 0; index < buttonCount; ++index)
    {
        IUIAutomationElement* button = nullptr;
        if (FAILED(buttons->GetElement(index, &button)) || !button) continue;

        BOOL isOffscreen = TRUE;
        RECT bounds{};
        const HRESULT visibleResult = button->get_CurrentIsOffscreen(&isOffscreen);
        const HRESULT boundsResult = button->get_CurrentBoundingRectangle(&bounds);
        if (SUCCEEDED(visibleResult) && !isOffscreen &&
            SUCCEEDED(boundsResult) && bounds.right > bounds.left && bounds.bottom > bounds.top)
            ++count;

        button->Release();
    }

    InterlockedExchange(g_taskbarAppCountView, count);
    g_uiAutomationFailureLogged = false;
    buttons->Release();
    buttonCondition->Release();
    root->Release();
}

void CALLBACK OnTaskbarAppCountEvent(
    HWINEVENTHOOK, DWORD, HWND hwnd, LONG, LONG, DWORD, DWORD)
{
    if (g_exiting || !g_window) return;
    HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!taskbar || (hwnd != taskbar && !IsChild(taskbar, hwnd))) return;
    KillTimer(g_window, kTaskbarAppCountTimer);
    SetTimer(g_window, kTaskbarAppCountTimer, kTaskbarAppCountDebounceMs, nullptr);
}

void EnsureTaskbarAppCountEventHooks()
{
    HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    DWORD pid = 0;
    if (taskbar) GetWindowThreadProcessId(taskbar, &pid);
    bool hooksInstalled = false;
    for (HWINEVENTHOOK hook : g_taskbarAppCountEventHooks)
        hooksInstalled = hooksInstalled || hook != nullptr;
    if (!pid || (hooksInstalled && pid == g_taskbarAppCountExplorerPid)) return;

    for (HWINEVENTHOOK& hook : g_taskbarAppCountEventHooks)
    {
        if (hook) UnhookWinEvent(hook);
        hook = nullptr;
    }
    g_taskbarAppCountExplorerPid = pid;

    constexpr DWORD events[] =
    {
        EVENT_OBJECT_CREATE,
        EVENT_OBJECT_DESTROY,
        EVENT_OBJECT_SHOW,
        EVENT_OBJECT_HIDE,
        EVENT_OBJECT_NAMECHANGE,
        EVENT_OBJECT_REORDER,
        EVENT_OBJECT_STATECHANGE,
        EVENT_OBJECT_LOCATIONCHANGE
    };
    for (size_t index = 0; index < std::size(events); ++index)
    {
        g_taskbarAppCountEventHooks[index] = SetWinEventHook(
            events[index], events[index], nullptr, OnTaskbarAppCountEvent,
            pid, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    }
}

void RemoveTaskbarAppCountEventHooks()
{
    for (HWINEVENTHOOK& hook : g_taskbarAppCountEventHooks)
    {
        if (hook) UnhookWinEvent(hook);
        hook = nullptr;
    }
    g_taskbarAppCountExplorerPid = 0;
}

void SetAccent(HWND hwnd, int state, uint32_t color)
{
    if (!hwnd || !g_setComposition) return;
    AccentPolicy policy{ state, state == kAccentAcrylic ? 0 : 2, color, 0 };
    WindowCompositionAttributeData data{ kWcaAccentPolicy, &policy, sizeof(policy) };
    g_setComposition(hwnd, &data);
}

void ApplyAccent()
{
    if (TaskbarsStale()) RefreshTaskbars();
    int state = 0;
    uint32_t color = 0;
    ComputeAccent(&state, &color);
    for (int i = 0; i < g_taskbarCount; ++i) SetAccent(g_taskbars[i], state, color);
}

HWND FindTaskList(HWND tray)
{
    HWND rebar = FindWindowExW(tray, nullptr, L"ReBarWindow32", nullptr);
    HWND switcher = FindWindowExW(rebar, nullptr, L"MSTaskSwWClass", nullptr);
    return FindWindowExW(switcher, nullptr, L"MSTaskListWClass", nullptr);
}

bool EnsureTaskList()
{
    if (!IsWindow(g_tray) || !IsWindow(g_taskList) || !IsWindow(g_taskListParent))
    {
        g_tray = FindWindowW(L"Shell_TrayWnd", nullptr);
        g_taskList = g_tray ? FindTaskList(g_tray) : nullptr;
        g_taskListParent = g_taskList ? GetParent(g_taskList) : nullptr;
    }
    return g_taskList && g_taskListParent;
}

void CenterTaskbar()
{
    if (!EnsureTaskList()) return;
    RECT tray{}, list{};
    if (!GetWindowRect(g_tray, &tray) || !GetWindowRect(g_taskList, &list)) return;
    IAccessible* accessible = nullptr;
    if (FAILED(AccessibleObjectFromWindow(g_taskList, OBJID_CLIENT, IID_IAccessible,
        reinterpret_cast<void**>(&accessible))) || !accessible) return;

    LONG count = 0;
    if (FAILED(accessible->get_accChildCount(&count)) || count <= 0 || count > 4096)
    {
        accessible->Release();
        return;
    }
    LONG left = LONG_MAX, right = LONG_MIN;
    auto includeLocation = [&left, &right](IAccessible* target, VARIANT child)
    {
        LONG x = 0, y = 0, width = 0, height = 0;
        if (SUCCEEDED(target->accLocation(&x, &y, &width, &height, child)) && width > 0 && height > 0)
        {
            if (x < left) left = x;
            if (x + width > right) right = x + width;
        }
    };

    auto children = static_cast<VARIANT*>(HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(VARIANT) * static_cast<size_t>(count)));
    LONG obtained = 0;
    const HRESULT childrenResult = children
        ? AccessibleChildren(accessible, 0, count, children, &obtained)
        : E_OUTOFMEMORY;
    if (SUCCEEDED(childrenResult) && obtained > 0)
    {
        for (LONG index = 0; index < obtained; ++index)
        {
            if (children[index].vt == VT_I4)
                includeLocation(accessible, children[index]);
            else if (children[index].vt == VT_DISPATCH && children[index].pdispVal)
            {
                IAccessible* child = nullptr;
                if (SUCCEEDED(children[index].pdispVal->QueryInterface(
                    IID_IAccessible, reinterpret_cast<void**>(&child))) && child)
                {
                    VARIANT self{};
                    self.vt = VT_I4;
                    self.lVal = CHILDID_SELF;
                    includeLocation(child, self);
                    child->Release();
                }
            }
            VariantClear(&children[index]);
        }
        HeapFree(GetProcessHeap(), 0, children);
    }
    else
    {
        if (children)
        {
            for (LONG index = 0; index < count; ++index) VariantClear(&children[index]);
            HeapFree(GetProcessHeap(), 0, children);
        }
        for (LONG id = 1; id <= count; ++id)
        {
            VARIANT child{};
            child.vt = VT_I4;
            child.lVal = id;
            includeLocation(accessible, child);
        }
    }
    accessible->Release();
    if (left > right) return;

    const int width = right - left;
    const int offset = left - list.left;
    const int targetX = tray.left + ((tray.right - tray.left) - width) / 2 - offset + kAdjustX;
    if (abs(list.left - targetX) <= 1) return;
    POINT point{ targetX, list.top };
    ScreenToClient(g_taskListParent, &point);
    SetWindowPos(g_taskList, nullptr, point.x, point.y, 0, 0,
        SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void RestoreTaskbarPosition()
{
    if (!EnsureTaskList()) return;
    RECT list{};
    if (!GetWindowRect(g_taskList, &list)) return;
    POINT point{ list.left, list.top };
    ScreenToClient(g_taskListParent, &point);
    SetWindowPos(g_taskList, nullptr, 0, point.y, 0, 0,
        SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void CALLBACK OnWinEvent(HWINEVENTHOOK, DWORD eventType, HWND hwnd, LONG, LONG, DWORD, DWORD)
{
    if (g_exiting || g_windowsBuild >= 22000 || !g_config.center || !hwnd) return;
    if (eventType == kEventLocation)
    {
        if (hwnd != g_taskList) return;
    }
    else if (hwnd != g_taskList && hwnd != g_tray && hwnd != g_taskListParent) return;
    KillTimer(g_window, kCenterTimer);
    SetTimer(g_window, kCenterTimer, 80, nullptr);
}

void EnsureWinEventHook()
{
    if (!EnsureTaskList()) return;
    DWORD pid = 0;
    GetWindowThreadProcessId(g_tray, &pid);
    if (g_eventHook && pid == g_eventExplorerPid) return;
    if (g_eventHook) UnhookWinEvent(g_eventHook);
    g_eventExplorerPid = pid;
    g_eventHook = SetWinEventHook(kEventCreate, kEventName, nullptr, OnWinEvent,
        pid, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
}

std::wstring PrepareHookDll()
{
    std::wstring source = GetModuleDirectory() + L"\\TricBar.dll";
    FILETIME stamp{};
    if (!FileStamp(source, &stamp)) return {};
    wchar_t local[MAX_PATH]{};
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (!n || n >= MAX_PATH) return {};
    std::wstring directory(local);
    directory += L"\\TricBar";
    CreateDirectoryW(directory.c_str(), nullptr);
    ULARGE_INTEGER time{};
    time.LowPart = stamp.dwLowDateTime;
    time.HighPart = stamp.dwHighDateTime;
    wchar_t name[64]{};
    swprintf_s(name, L"\\TricBar-%016llX.dll", time.QuadPart);
    std::wstring destination = directory + name;
    if (!CopyFileW(source.c_str(), destination.c_str(), TRUE))
    {
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) return {};
    }
    return destination;
}

struct InjectionRequest
{
    DWORD pid;
    wchar_t path[MAX_PATH * 4];
};

DWORD WINAPI InjectTapThread(void* value)
{
    InjectionRequest* request = static_cast<InjectionRequest*>(value);
    HMODULE xaml = LoadLibraryW(L"Windows.UI.Xaml.dll");
    if (xaml)
    {
        auto initialize = reinterpret_cast<InitXamlDiagnosticsFn>(
            GetProcAddress(xaml, "InitializeXamlDiagnosticsEx"));
        if (initialize)
        {
            const GUID clsid = { 0xb7c0a2e1, 0x5d34, 0x4f6a, { 0x9e, 0x81, 0x3c, 0x7a, 0x1d, 0x5f, 0x2b, 0x90 } };
            initialize(L"VisualDiagConnection1", request->pid, L"", request->path, clsid, nullptr);
        }
        FreeLibrary(xaml);
    }
    HeapFree(GetProcessHeap(), 0, request);
    InterlockedExchange(&g_tapBusy, 0);
    return 0;
}

void EnsureTap()
{
    if (g_windowsBuild < 22621 || !g_alive || !g_active || !g_applied) return;
    if (g_config.mode == Mode::Normal)
    {
        ResetEvent(g_active);
        g_tooltipSuffix.clear();
        return;
    }
    SetEvent(g_active);
    if (g_tapPath.empty())
    {
        g_tooltipSuffix = L" - TricBar.dll ausente";
        return;
    }

    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    DWORD pid = 0;
    if (tray) GetWindowThreadProcessId(tray, &pid);
    if (!pid) return;
    if (pid != g_tapPid)
    {
        g_tapPid = pid;
        ResetEvent(g_applied);
        g_tapAttempts = 0;
        g_nextTapAttempt = 0;
    }
    if (WaitForSingleObject(g_applied, 0) == WAIT_OBJECT_0)
    {
        g_tooltipSuffix = L" - ativo";
        return;
    }
    if (g_tapAttempts >= 5 || g_tapInjections >= 8 || GetTickCount64() < g_nextTapAttempt)
    {
        if (g_tapAttempts >= 5 || g_tapInjections >= 8) g_tooltipSuffix = L" - falha ao aplicar";
        return;
    }
    if (InterlockedCompareExchange(&g_tapBusy, 1, 0) != 0) return;
    auto request = static_cast<InjectionRequest*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(InjectionRequest)));
    if (!request)
    {
        InterlockedExchange(&g_tapBusy, 0);
        return;
    }
    request->pid = pid;
    wcsncpy_s(request->path, g_tapPath.c_str(), _TRUNCATE);
    ++g_tapAttempts;
    ++g_tapInjections;
    g_nextTapAttempt = GetTickCount64() + 5000;
    HANDLE thread = CreateThread(nullptr, 0, InjectTapThread, request, 0, nullptr);
    if (thread) CloseHandle(thread);
    else
    {
        HeapFree(GetProcessHeap(), 0, request);
        InterlockedExchange(&g_tapBusy, 0);
    }
}

void LoadLegacyHook()
{
    if (g_windowsBuild >= 22000) return;
    const std::wstring path = GetModuleDirectory() + L"\\TricBar.dll";
    g_accentLibrary = LoadLibraryW(path.c_str());
    if (!g_accentLibrary)
    {
        g_tooltipSuffix = L" - TricBar.dll ausente";
        return;
    }
    g_installAccentHook = reinterpret_cast<InstallAccentHookFn>(
        GetProcAddress(g_accentLibrary, "TricBarInstallAccentHook"));
    g_uninstallAccentHooks = reinterpret_cast<UninstallAccentHooksFn>(
        GetProcAddress(g_accentLibrary, "TricBarUninstallAccentHooks"));
}

void EnsureLegacyHook()
{
    if (g_windowsBuild >= 22000 || !g_installAccentHook) return;
    if (TaskbarsStale()) RefreshTaskbars();
    if (!g_taskbarCount) return;
    DWORD pid = 0;
    GetWindowThreadProcessId(g_taskbars[0], &pid);
    if (!pid) return;
    if (pid != g_accentHookPid)
    {
        if (g_uninstallAccentHooks) g_uninstallAccentHooks();
        g_accentHookPid = pid;
    }
    for (int i = 0; i < g_taskbarCount; ++i) g_installAccentHook(g_taskbars[i]);
}

void ApplySettings()
{
    if (g_config.mode == Mode::LiquidGlass)
    {
        UpdateTaskbarAppCount();
        EnsureTaskbarAppCountEventHooks();
        SetTimer(g_window, kTaskbarAppCountFallbackTimer, kTaskbarAppCountFallbackMs, nullptr);
    }
    else
    {
        KillTimer(g_window, kTaskbarAppCountTimer);
        KillTimer(g_window, kTaskbarAppCountFallbackTimer);
        RemoveTaskbarAppCountEventHooks();
        ReleaseTaskbarAppAutomation();
    }
    if (g_windowsBuild >= 22621)
    {
        if (g_config.mode == Mode::Normal) ResetEvent(g_active);
        else SetEvent(g_active);
    }
    ApplyAccent();
    EnsureTap();
    if (g_windowsBuild < 22000)
    {
        EnsureLegacyHook();
        if (g_config.center)
        {
            EnsureWinEventHook();
            CenterTaskbar();
        }
        else RestoreTaskbarPosition();
    }
}

void OpenLauncher()
{
    const std::wstring path = GetModuleDirectory() + L"\\TricBarConfigurator.exe";
    ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, GetModuleDirectory().c_str(), SW_SHOWNORMAL);
}

void ShowMenu()
{
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, kMenuSettings, L"Configuracoes");
    AppendMenuW(menu, MF_STRING, kMenuExit, L"Sair");
    POINT point{};
    GetCursorPos(&point);
    SetForegroundWindow(g_window);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, point.x, point.y, 0, g_window, nullptr);
    PostMessageW(g_window, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

NOTIFYICONDATAW MakeTrayData()
{
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = g_window;
    data.uID = 1;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    data.uCallbackMessage = kWmTray;
    data.hIcon = g_trayIcon ? g_trayIcon : LoadIconW(nullptr, IDI_APPLICATION);
    swprintf_s(data.szTip, L"TricBar 1.1%s", g_tooltipSuffix.c_str());
    return data;
}

void EnsureTrayIcon()
{
    auto data = MakeTrayData();
    if (!Shell_NotifyIconW(NIM_MODIFY, &data)) Shell_NotifyIconW(NIM_ADD, &data);
}

void ExitEngine()
{
    if (g_exiting) return;
    g_exiting = true;
    KillTimer(g_window, kKeepAliveTimer);
    KillTimer(g_window, kCenterTimer);
    KillTimer(g_window, kTaskbarAppCountTimer);
    KillTimer(g_window, kTaskbarAppCountFallbackTimer);
    if (g_eventHook) UnhookWinEvent(g_eventHook);
    RemoveTaskbarAppCountEventHooks();
    ApplyAccent();
    if (g_windowsBuild < 22000) RestoreTaskbarPosition();
    if (g_active) ResetEvent(g_active);
    if (g_uninstallAccentHooks) g_uninstallAccentHooks();
    if (g_alive)
    {
        CloseHandle(g_alive);
        g_alive = nullptr;
    }
    for (int i = 0; i < g_taskbarCount; ++i) SetAccent(g_taskbars[i], kAccentDisabled, 0);
    auto data = MakeTrayData();
    Shell_NotifyIconW(NIM_DELETE, &data);
    DestroyWindow(g_window);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (g_reloadMessage && message == g_reloadMessage)
    {
        g_hasConfigStamp = false;
        ReadConfig();
        ApplySettings();
        return 0;
    }
    switch (message)
    {
    case kWmTray:
        if (lParam == WM_RBUTTONUP) ShowMenu();
        else if (lParam == WM_LBUTTONDBLCLK) OpenLauncher();
        return 0;
    case WM_COMMAND:
        if (LOWORD(wParam) == kMenuExit) ExitEngine();
        else if (LOWORD(wParam) == kMenuSettings) OpenLauncher();
        return 0;
    case WM_CLOSE:
        ExitEngine();
        return 0;
    case WM_TIMER:
        if (wParam == kCenterTimer)
        {
            KillTimer(window, kCenterTimer);
            if (g_config.center && g_windowsBuild < 22000) CenterTaskbar();
        }
        else if (wParam == kKeepAliveTimer)
        {
            ReadConfig();
            EnsureTrayIcon();
            ApplyAccent();
            EnsureTap();
            EnsureLegacyHook();
            if (g_config.mode == Mode::LiquidGlass)
                EnsureTaskbarAppCountEventHooks();
            if (g_windowsBuild < 22000 && g_config.center)
            {
                EnsureWinEventHook();
                CenterTaskbar();
            }
        }
        else if (wParam == kTaskbarAppCountTimer)
        {
            KillTimer(window, kTaskbarAppCountTimer);
            UpdateTaskbarAppCount();
        }
        else if (wParam == kTaskbarAppCountFallbackTimer)
            UpdateTaskbarAppCount();
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    OleApartment ole;
    if (FAILED(ole.result)) return static_cast<int>(ole.result);
    SetProcessDPIAware();
    RTL_OSVERSIONINFOW version{ sizeof(version) };
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    auto getVersion = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
    if (getVersion && getVersion(reinterpret_cast<PRTL_OSVERSIONINFOW>(&version)) == 0)
        g_windowsBuild = static_cast<int>(version.dwBuildNumber);
    else
        g_windowsBuild = 0;

    g_mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (!g_mutex || GetLastError() == ERROR_ALREADY_EXISTS)
    {
        if (g_mutex) CloseHandle(g_mutex);
        return 0;
    }

    g_alive = CreateEventW(nullptr, TRUE, TRUE, kAliveName);
    g_active = CreateEventW(nullptr, TRUE, FALSE, kActiveName);
    if (g_windowsBuild >= 22621)
        g_applied = CreateEventW(nullptr, TRUE, FALSE, kAppliedName);
    if (!g_alive || !g_active || (g_windowsBuild >= 22621 && !g_applied))
        return static_cast<int>(GetLastError());
    if (!InitializeTaskbarAppCounter())
        return static_cast<int>(GetLastError());

    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    g_setComposition = reinterpret_cast<SetWindowCompositionAttributeFn>(
        GetProcAddress(user32, "SetWindowCompositionAttribute"));
    g_reloadMessage = RegisterWindowMessageW(kReloadMessage);
    LoadLegacyHook();
    if (g_windowsBuild >= 22621) g_tapPath = PrepareHookDll();
    ReadConfig();

    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.style = CS_DBLCLKS;
    windowClass.lpszClassName = kWindowClass;
    if (!RegisterClassW(&windowClass)) return static_cast<int>(GetLastError());

    g_window = CreateWindowExW(0, kWindowClass, L"TricBar", 0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, instance, nullptr);
    if (!g_window) return static_cast<int>(GetLastError());

    g_trayIcon = static_cast<HICON>(LoadImageW(
        instance, MAKEINTRESOURCEW(IDI_TRICBAR_ICON), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE));
    auto trayData = MakeTrayData();
    Shell_NotifyIconW(NIM_ADD, &trayData);
    ApplySettings();
    SetTimer(g_window, kKeepAliveTimer, 1500, nullptr);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (g_eventHook) UnhookWinEvent(g_eventHook);
    RemoveTaskbarAppCountEventHooks();
    if (g_trayIcon) DestroyIcon(g_trayIcon);
    if (g_alive) CloseHandle(g_alive);
    if (g_active) CloseHandle(g_active);
    if (g_applied) CloseHandle(g_applied);
    if (g_taskbarAppCountView) UnmapViewOfFile(const_cast<LONG*>(g_taskbarAppCountView));
    if (g_taskbarAppCountMapping) CloseHandle(g_taskbarAppCountMapping);
    ReleaseTaskbarAppAutomation();
    if (g_mutex) ReleaseMutex(g_mutex), CloseHandle(g_mutex);
    if (g_accentLibrary) FreeLibrary(g_accentLibrary);
    return static_cast<int>(message.wParam);
}
