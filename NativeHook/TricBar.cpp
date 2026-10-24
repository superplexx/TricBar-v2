// TricBar.cpp  ->  TricBar.dll
//
// DLL carregada DENTRO do explorer.exe pela infraestrutura de XAML Diagnostics
// (InitializeXamlDiagnosticsEx), a mesma tecnica usada pelo TranslucentTB e pelo
// Windhawk. No Windows 11 22H2+ o fundo da barra e um Rectangle XAML
// ("BackgroundFill") que fica POR CIMA da janela da barra, entao o
// SetWindowCompositionAttribute (blur / acrylic / cor) do TricBar.exe nao
// aparece. Aqui a gente acha esse retangulo e troca o Fill por transparente; o
// efeito aplicado pelo .exe na janela passa a ficar visivel.
//
// Sinais com o TricBar.exe (eventos nomeados):
//   TricBar_Blur_Alive   existe enquanto o app esta rodando. Se sumir (app fechou
//                        ou travou), o fundo original volta e a DLL se desarma.
//   TricBar_Blur_Active  sinalizado = efeito ligado (modo != Normal).
//                        Nao sinalizado = devolve o fundo original, mas continua pronta.
//   TricBar_Blur_Applied o app espera por ele para saber que a DLL achou a barra.
//
// Build: NativeHook\build.bat (precisa do MSVC + Windows SDK).

#define NOMINMAX
#include <windows.h>
#include <unknwn.h>
#include <ocidl.h>
#include <xamlom.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Shapes.h>

#include <algorithm>
#include <chrono>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>
#include <tlhelp32.h>

namespace wf   = winrt::Windows::Foundation;
namespace wux  = winrt::Windows::UI::Xaml;
namespace wuxm = winrt::Windows::UI::Xaml::Media;
namespace wuxs = winrt::Windows::UI::Xaml::Shapes;

// {B7C0A2E1-5D34-4F6A-9E81-3C7A1D5F2B90} (tambem definido pelo motor nativo)
static const GUID CLSID_TricBarBlur =
    { 0xb7c0a2e1, 0x5d34, 0x4f6a, { 0x9e, 0x81, 0x3c, 0x7a, 0x1d, 0x5f, 0x2b, 0x90 } };

static constexpr wchar_t kAliveEvent[]   = L"TricBar_Blur_Alive";
static constexpr wchar_t kActiveEvent[]  = L"TricBar_Blur_Active";
static constexpr wchar_t kAppliedEvent[] = L"TricBar_Blur_Applied";
static constexpr char kCompositionFunction[] = "SetWindowCompositionAttribute";
static constexpr wchar_t kTagMarker[]    = L"TricBar.Transparent";
static constexpr wchar_t kVersion[]      = L"1.1.0";

struct CompositionAttributeData
{
    int attribute;
    void* data;
    SIZE_T size;
};

using SetWindowCompositionAttributeFn = BOOL (WINAPI*)(HWND, CompositionAttributeData*);
static std::atomic<SetWindowCompositionAttributeFn> g_setWindowCompositionAttribute{ nullptr };
static std::atomic<bool> g_importHookInstalled{ false };
static std::atomic<bool> g_noImportLogged{ false };
static std::atomic<bool> g_compositionBlockLogged{ false };
static std::atomic_flag g_installingImportHook = ATOMIC_FLAG_INIT;
static std::atomic<DWORD> g_nextInstallAttempt{};
static std::mutex g_hookLock;
static std::vector<std::pair<DWORD, HHOOK>> g_taskbarHooks;
// Contagem dos botoes de apps da barra (Taskbar.TaskListButton), feita aqui dentro do Explorer.
// g_taskListGen sobe toda vez que um botao entra ou sai da arvore visual.
static std::atomic<uint32_t> g_taskListGen{ 1 };
static std::mutex g_taskListLock;
static std::unordered_set<uint64_t> g_taskListHandles;

static BOOL WINAPI SetWindowCompositionAttributeHook(HWND hwnd, CompositionAttributeData* data);
static LRESULT CALLBACK TaskbarThreadHook(int code, WPARAM wParam, LPARAM lParam);
static bool EffectActive();

// Log simples em %LOCALAPPDATA%\\TricBar\\blur.log (so grava quando algo muda).
static void Log(const wchar_t* fmt, ...)
{
    wchar_t base[MAX_PATH]{};
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    std::wstring directory = std::wstring(base) + L"\\TricBar";
    CreateDirectoryW(directory.c_str(), nullptr);
    std::wstring path = directory + L"\\blur.log";

    wchar_t line[400]{};
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(line, 380, fmt, ap);
    va_end(ap);
    wcscat(line, L"\r\n");

    char utf8[1200]{};
    int len = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, sizeof(utf8), nullptr, nullptr);
    if (len <= 1) return;

    HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(h, utf8, (DWORD)(len - 1), &w, nullptr);
    CloseHandle(h);
}

static bool ContainsRange(BYTE* base, DWORD imageSize, const void* address, size_t length)
{
    const auto begin = reinterpret_cast<uintptr_t>(base);
    const auto current = reinterpret_cast<uintptr_t>(address);
    return current >= begin && length <= imageSize && current - begin <= imageSize - length;
}

static size_t HookModuleImports(HMODULE module)
{
    auto base = reinterpret_cast<BYTE*>(module);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 ||
        static_cast<size_t>(dos->e_lfanew) > 0x100000)
        return 0;

    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR_MAGIC)
        return 0;

    const DWORD imageSize = nt->OptionalHeader.SizeOfImage;
    const auto& imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!imports.VirtualAddress || imports.Size < sizeof(IMAGE_IMPORT_DESCRIPTOR) ||
        !ContainsRange(base, imageSize, base + imports.VirtualAddress, imports.Size))
        return 0;

    size_t patched = 0;
    auto descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + imports.VirtualAddress);
    const size_t descriptorCount = imports.Size / sizeof(IMAGE_IMPORT_DESCRIPTOR);

    for (size_t d = 0; d < descriptorCount && descriptor->Name; ++d, ++descriptor)
    {
        if (!ContainsRange(base, imageSize, base + descriptor->Name, sizeof("user32.dll")) ||
            _stricmp(reinterpret_cast<const char*>(base + descriptor->Name), "user32.dll") != 0)
            continue;

        if (!descriptor->FirstThunk || !descriptor->OriginalFirstThunk) continue;
        auto iat = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->FirstThunk);
        auto names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->OriginalFirstThunk);

        for (; ContainsRange(base, imageSize, iat, sizeof(*iat)) &&
               ContainsRange(base, imageSize, names, sizeof(*names)) &&
               names->u1.AddressOfData;
             ++iat, ++names)
        {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto importByName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (!ContainsRange(base, imageSize, importByName,
                               sizeof(WORD) + sizeof(kCompositionFunction)) ||
                std::strcmp(reinterpret_cast<const char*>(importByName->Name), kCompositionFunction) != 0)
                continue;

            if (reinterpret_cast<void*>(iat->u1.Function) ==
                reinterpret_cast<void*>(SetWindowCompositionAttributeHook))
                continue;

            DWORD oldProtection = 0;
            if (!VirtualProtect(&iat->u1.Function, sizeof(iat->u1.Function),
                                PAGE_READWRITE, &oldProtection))
                continue;

            InterlockedExchangePointer(
                reinterpret_cast<void* volatile*>(&iat->u1.Function),
                reinterpret_cast<void*>(SetWindowCompositionAttributeHook));
            DWORD ignored = 0;
            VirtualProtect(&iat->u1.Function, sizeof(iat->u1.Function), oldProtection, &ignored);
            ++patched;
        }
    }

    return patched;
}

static size_t HookProcessImports()
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
                                                GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE) return 0;

    size_t patched = 0;
    MODULEENTRY32W module{};
    module.dwSize = sizeof(module);
    if (Module32FirstW(snapshot, &module))
    {
        do
        {
            patched += HookModuleImports(module.hModule);
        }
        while (Module32NextW(snapshot, &module));
    }
    CloseHandle(snapshot);
    return patched;
}

static bool IsTaskbarWindow(HWND hwnd)
{
    if (hwnd == FindWindowW(L"Shell_TrayWnd", nullptr)) return true;

    HWND after = nullptr;
    while ((after = FindWindowExW(nullptr, after, L"Shell_SecondaryTrayWnd", nullptr)) != nullptr)
        if (hwnd == after) return true;

    return false;
}

static BOOL WINAPI SetWindowCompositionAttributeHook(HWND hwnd, CompositionAttributeData* data)
{
    if (data && data->attribute == 19 && IsTaskbarWindow(hwnd) && EffectActive())
    {
        if (!g_compositionBlockLogged.exchange(true, std::memory_order_acq_rel))
            Log(L"[v%s] chamada de composicao do Explorer bloqueada para a taskbar", kVersion);
        return TRUE;
    }
    auto original = g_setWindowCompositionAttribute.load(std::memory_order_acquire);
    return original ? original(hwnd, data) : FALSE;
}

static LRESULT CALLBACK TaskbarThreadHook(int code, WPARAM wParam, LPARAM lParam)
{
    if (code >= 0)
    {
        auto original = g_setWindowCompositionAttribute.load(std::memory_order_acquire);
        if (!original)
        {
            HMODULE user32 = GetModuleHandleW(L"user32.dll");
            if (user32)
            {
                original = reinterpret_cast<SetWindowCompositionAttributeFn>(
                    GetProcAddress(user32, kCompositionFunction));
                if (original)
                {
                    SetWindowCompositionAttributeFn expected = nullptr;
                    g_setWindowCompositionAttribute.compare_exchange_strong(
                        expected, original, std::memory_order_release, std::memory_order_relaxed);
                }
            }
        }

        const DWORD now = GetTickCount();
        const DWORD nextTry = g_nextInstallAttempt.load(std::memory_order_relaxed);
        if (g_setWindowCompositionAttribute.load(std::memory_order_acquire) &&
            static_cast<LONG>(now - nextTry) >= 0 &&
            !g_installingImportHook.test_and_set(std::memory_order_acquire))
        {
            const size_t count = HookProcessImports();
            g_nextInstallAttempt.store(now + 5000, std::memory_order_relaxed);
            if (count != 0)
            {
                g_importHookInstalled.store(true, std::memory_order_release);
                Log(L"[v%s] hook de composicao instalado no Explorer (%zu imports)", kVersion, count);
            }
            else if (!g_importHookInstalled.load(std::memory_order_acquire) &&
                     !g_noImportLogged.exchange(true, std::memory_order_acq_rel))
            {
                Log(L"[v%s] ainda nao encontrei imports de SetWindowCompositionAttribute no Explorer", kVersion);
            }
            g_installingImportHook.clear(std::memory_order_release);
        }
    }

    return CallNextHookEx(nullptr, code, wParam, lParam);
}

extern "C" __declspec(dllexport) BOOL WINAPI TricBarInstallAccentHook(HWND taskbar)
{
    if (!taskbar || !IsWindow(taskbar)) return FALSE;

    DWORD processId = 0;
    const DWORD threadId = GetWindowThreadProcessId(taskbar, &processId);
    if (!threadId || processId == GetCurrentProcessId()) return FALSE;

    std::lock_guard<std::mutex> lock(g_hookLock);
    for (auto const& hook : g_taskbarHooks)
        if (hook.first == threadId) return TRUE;

    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&TaskbarThreadHook), &module))
        return FALSE;

    HHOOK hook = SetWindowsHookExW(WH_CALLWNDPROC, TaskbarThreadHook, module, threadId);
    if (!hook) return FALSE;
    g_taskbarHooks.emplace_back(threadId, hook);
    return TRUE;
}

extern "C" __declspec(dllexport) void WINAPI TricBarUninstallAccentHooks()
{
    std::lock_guard<std::mutex> lock(g_hookLock);
    for (auto const& hook : g_taskbarHooks)
        UnhookWindowsHookEx(hook.second);
    g_taskbarHooks.clear();
}
// ---------------------------------------------------------------- sinais com o app

static bool AppAlive()
{
    HANDLE h = OpenEventW(SYNCHRONIZE, FALSE, kAliveEvent);
    if (!h) return false;
    CloseHandle(h);
    return true;
}

static bool EffectActive()
{
    HANDLE h = OpenEventW(SYNCHRONIZE, FALSE, kActiveEvent);
    if (!h) return false;
    bool on = WaitForSingleObject(h, 0) == WAIT_OBJECT_0;
    CloseHandle(h);
    return on;
}

static void SignalApplied()
{
    HANDLE h = OpenEventW(EVENT_MODIFY_STATE, FALSE, kAppliedEvent);
    if (!h) return;
    SetEvent(h);
    CloseHandle(h);
}

// ---------------------------------------------------------------- aplicar / restaurar

// ---------------------------------------------------------------- config (mesmo config.ini do Launcher)
//
// No Windows 11 22H2+ a opacidade da cor passada para o blur/acrylic da janela nao e
// respeitada (a cor aparece mesmo com opacidade 0). Entao a cor e a opacidade sao
// aplicadas aqui, no proprio retangulo XAML: alfa 0 = so o blur, sem tinta.

struct Cfg
{
    int  mode = 3;      // 0 normal, 1 clear, 2 blur, 3 acrylic, 4 color
    int  rgb = 0x808080; // 0xRRGGBB; neutral Windows-style gray tint
    int  opacity = 60;  // 0..100

    bool operator==(Cfg const& o) const { return mode == o.mode && rgb == o.rgb && opacity == o.opacity; }
};

static std::mutex g_cfgLock;
static Cfg g_cfg;
static FILETIME g_cfgStamp{};
static bool g_cfgHasStamp = false;

static void RefreshCfg()
{
    wchar_t appdata[MAX_PATH]{};
    DWORD n = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    std::wstring path = std::wstring(appdata) + L"\\TricBar\\config.ini";

    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) return;

    std::lock_guard<std::mutex> lock(g_cfgLock);
    if (g_cfgHasStamp && CompareFileTime(&fad.ftLastWriteTime, &g_cfgStamp) == 0) return;

    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    char buf[1024]{};
    DWORD got = 0;
    ReadFile(h, buf, sizeof(buf) - 1, &got, nullptr);
    CloseHandle(h);

    g_cfgStamp = fad.ftLastWriteTime;
    g_cfgHasStamp = true;

    Cfg c;
    std::string text(buf, got);
    if (text.size() >= 3 && (uint8_t)text[0] == 0xEF &&
        (uint8_t)text[1] == 0xBB && (uint8_t)text[2] == 0xBF)
        text.erase(0, 3);
    size_t pos = 0;
    while (pos < text.size())
    {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        std::string key = line.substr(0, eq), val = line.substr(eq + 1);
        for (auto& ch : key) ch = (char)tolower((unsigned char)ch);
        for (auto& ch : val) ch = (char)tolower((unsigned char)ch);
        if (key == "mode")
            c.mode = val == "normal" ? 0 : val == "blur" ? 2 :
                val == "acrylic" || val == "macos" ? 3 :
                val == "lightliquidglass" || val == "liquidglass" || val == "mica" ? 4 :
                val == "darkliquidglass" ? 6 :
                val == "color" ? 5 : 1;
        else if (key == "color")
        {
            if (!val.empty() && val[0] == '#') val.erase(0, 1);
            c.rgb = (int)(strtoul(val.c_str(), nullptr, 16) & 0xFFFFFF);
        }
        else if (key == "opacity")
        {
            int o = atoi(val.c_str());
            c.opacity = o < 0 ? 0 : o > 100 ? 100 : o;
        }
    }
    g_cfg = c;
    Log(L"[v%s] config lida: mode=%d cor=%06X opacidade=%d", kVersion, c.mode, c.rgb, c.opacity);
}

static Cfg GetCfg()
{
    std::lock_guard<std::mutex> lock(g_cfgLock);
    return g_cfg;
}

// Pincel do fundo da barra para a config atual.
//  - Clear:           cor totalmente transparente (a janela, com TRANSPARENTGRADIENT, deixa ver o papel de parede).
//  - Cor:             cor + opacidade (cor lisa, sem blur).
//  - Blur / Acrylic:  AcrylicBrush do proprio XAML, o mesmo que o Windhawk Taskbar Styler usa no fundo da barra.
//                     E o unico jeito de ter desfoque no 22H2+: um SolidColorBrush so pinta cor por cima
//                     e o accent de blur da janela nao aparece atras do retangulo.
//                     Se o Windows estiver com "efeitos de transparencia" desligados ou em economia de energia,
//                     o AcrylicBrush cai sozinho para a FallbackColor (a cor com a opacidade escolhida).
//  - Liquid Glass:    Acrylic com reflexo frio, borda luminosa e forma de capsula; requer o hook do Windows 11 22H2+.
static wuxm::Brush MakeBrush(Cfg const& c)
{
    uint8_t r = (uint8_t)((c.rgb >> 16) & 0xFF), g = (uint8_t)((c.rgb >> 8) & 0xFF), b = (uint8_t)(c.rgb & 0xFF);
    uint8_t a = c.mode >= 2 ? (uint8_t)(c.opacity * 255 / 100) : (uint8_t)0;

    if (c.mode == 4 || c.mode == 6)
    {
        try
        {
            const uint8_t glassR = static_cast<uint8_t>((r + 205) / 2);
            const uint8_t glassG = static_cast<uint8_t>((g + 225) / 2);
            const uint8_t glassB = static_cast<uint8_t>((b + 250) / 2);
            const bool darkMode = c.mode == 6;
            const uint8_t tintR = darkMode ? 24 : glassR;
            const uint8_t tintG = darkMode ? 28 : glassG;
            const uint8_t tintB = darkMode ? 40 : glassB;
            const uint8_t fallbackR = darkMode ? 32 : glassR;
            const uint8_t fallbackG = darkMode ? 36 : glassG;
            const uint8_t fallbackB = darkMode ? 48 : glassB;
            const float tintOpacity = 0.08f + (c.opacity / 100.0f) * 0.20f;

            wuxm::AcrylicBrush br;
            br.BackgroundSource(wuxm::AcrylicBackgroundSource::Backdrop);
            br.TintColor(winrt::Windows::UI::Color{ 255, tintR, tintG, tintB });
            br.TintOpacity(tintOpacity);
            br.TintLuminosityOpacity(winrt::box_value(darkMode ? 0.38 : 0.58).as<wf::IReference<double>>());
            Log(L"[v%s] pincel Liquid Glass criado: cor=%06X opacidade=%d backdrop dark=%s", kVersion, c.rgb, c.opacity, darkMode ? L"true" : L"false");
            return br;
        }
        catch (...)
        {
            Log(L"[v%s] pincel Liquid Glass falhou (0x%08X), usando cor lisa", kVersion, (unsigned)winrt::to_hresult());
            return wuxm::SolidColorBrush(winrt::Windows::UI::Color{ a, r, g, b });
        }
    }

    if (c.mode == 2 || c.mode == 3)
    {
        try
        {
            const bool blurMode = c.mode == 2;
            const float tintFactor = 0.70f;
            const uint8_t tintR = blurMode ? r : static_cast<uint8_t>(std::clamp(r * tintFactor + 18.0f, 0.0f, 255.0f));
            const uint8_t tintG = blurMode ? g : static_cast<uint8_t>(std::clamp(g * tintFactor + 18.0f, 0.0f, 255.0f));
            const uint8_t tintB = blurMode ? b : static_cast<uint8_t>(std::clamp(b * tintFactor + 16.0f, 0.0f, 255.0f));
            const float tintOpacity = blurMode
                ? static_cast<float>(c.opacity) / 100.0f
                : 0.08f + (c.opacity / 100.0f) * 0.20f;

            wuxm::AcrylicBrush br;
            br.BackgroundSource(wuxm::AcrylicBackgroundSource::Backdrop);
            br.TintColor(winrt::Windows::UI::Color{ 255, tintR, tintG, tintB });
            br.TintOpacity(tintOpacity);
            br.TintLuminosityOpacity(blurMode ? 0.0 : 0.12);
            Log(L"[v%s] AcrylicBrush criado: modo=%d cor=%06X tint=%d%% mais escuro", kVersion, c.mode, c.rgb, c.opacity);
            return br;
        }
        catch (...)
        {
            Log(L"[v%s] AcrylicBrush falhou (0x%08X), usando cor lisa", kVersion, (unsigned)winrt::to_hresult());
        }
    }
    return wuxm::SolidColorBrush(winrt::Windows::UI::Color{ a, r, g, b });
}

struct Entry
{
    wuxs::Rectangle rect{ nullptr };
    wuxm::Brush original{ nullptr };
    wuxm::Brush ours{ nullptr };
    wuxm::Brush originalStroke{ nullptr };
    Cfg applied{ -1, -1, -1 };  // config com que 'ours' foi montado
    double originalWidth{};
    double originalHeight{};
    double originalRadiusX{};
    double originalRadiusY{};
    double originalStrokeThickness{};
    wux::HorizontalAlignment originalHorizontalAlignment{};
    wux::VerticalAlignment originalVerticalAlignment{};
    wux::Thickness originalMargin{};
    wux::DispatcherTimer timer{ nullptr };
    wux::DispatcherTimer liquidGlassAnimationTimer{ nullptr };
    int64_t token{ 0 };
    ULONGLONG liquidGlassAnimationStart{};
    double liquidGlassAnimationFrom{};
    double liquidGlassWidth{ -1 };
    bool liquidGlassAnimating{ false };
    bool alive{ false };    // callback + timer registrados
    bool enabled{ false };  // fundo atualmente transparente
    bool liquidGlassApplied{ false };
    bool liquidGlassStyleConfigured{ false };
    int liquidGlassAppCount{ -1 };
    int buttonCount{ -1 };       // ultima contagem de botoes da barra
    uint32_t buttonGen{ 0 };     // g_taskListGen visto na ultima contagem
    ULONGLONG nextRecount{ 0 };  // proxima recontagem completa (varre a arvore)
    ULONGLONG nextQuick{ 0 };    // proxima reavaliacao rapida dos botoes ja conhecidos
    std::vector<winrt::weak_ref<wux::FrameworkElement>> buttons;  // botoes achados na ultima varredura
    winrt::weak_ref<wux::FrameworkElement> buttonRoot;            // raiz da arvore da barra
    bool geometry{ false };  // true so no fundo da barra principal (flyouts so recebem cor)
};

static bool IsMainTaskbarBackground(wux::DependencyObject const& start);

static void RestoreLiquidGlassStyle(std::shared_ptr<Entry> const& e)
{
    if (!e->liquidGlassApplied) return;
    try
    {
        if (e->liquidGlassAnimationTimer)
            e->liquidGlassAnimationTimer.Stop();
        e->liquidGlassAnimating = false;
        e->liquidGlassWidth = -1;
        e->rect.Width(e->originalWidth);
        e->rect.Height(e->originalHeight);
        e->rect.RadiusX(e->originalRadiusX);
        e->rect.RadiusY(e->originalRadiusY);
        e->rect.HorizontalAlignment(e->originalHorizontalAlignment);
        e->rect.VerticalAlignment(e->originalVerticalAlignment);
        e->rect.Margin(e->originalMargin);
        e->rect.Stroke(e->originalStroke);
        e->rect.StrokeThickness(e->originalStrokeThickness);
    }
    catch (...) {}
    e->liquidGlassApplied = false;
    e->liquidGlassStyleConfigured = false;
    e->liquidGlassAppCount = -1;
}

// Conta os Taskbar.TaskListButton visiveis na arvore XAML da barra (mesmo criterio que o
// UI Automation usava: botao de app, visivel e com tamanho). Sobe da propria barra ate a
// raiz e desce pelos filhos; so roda quando um botao entra/sai ou a cada 10 s.
// Diagnostico: registra (uma vez cada, no maximo 16) os nomes de classe com "TaskList" que aparecem.
static void NoteTaskListName(wchar_t const* where, std::wstring_view name)
{
    static std::mutex lock;
    static std::unordered_set<std::wstring> seen;
    std::lock_guard<std::mutex> guard(lock);
    if (seen.size() >= 16) return;
    if (seen.insert(std::wstring(name)).second)
        Log(L"[v%s] %s: %s", kVersion, where, std::wstring(name).c_str());
}

// Botao realmente na tela: visivel, com tamanho, opaco e dentro da area da barra.
// Botoes de apps fechados ficam reciclados na arvore (fora da area visivel) e nao contam.
static bool ButtonOnScreen(wux::FrameworkElement const& fe, wux::FrameworkElement const& root)
{
    try
    {
        if (fe.Visibility() != wux::Visibility::Visible) return false;
        const double w = fe.ActualWidth(), h = fe.ActualHeight();
        if (!(w > 0 && h > 0)) return false;
        if (fe.Opacity() <= 0.01) return false;
        if (!root) return true;
        const wf::Point pt = fe.TransformToVisual(root).TransformPoint(wf::Point{ 0, 0 });
        const double rw = root.ActualWidth(), rh = root.ActualHeight();
        if (rw > 0 && (pt.X + w <= 0 || pt.X >= rw)) return false;
        if (rh > 0 && (pt.Y + h <= 0 || pt.Y >= rh)) return false;
        return true;
    }
    catch (...) { return true; }
}

// Varredura completa: acha os botoes de apps na arvore XAML da barra (sobe ate a raiz e desce).
static int ScanTaskListButtons(std::shared_ptr<Entry> const& e, int previous)
{
    try
    {
        wux::DependencyObject root = e->rect;
        for (;;)
        {
            auto parent = wuxm::VisualTreeHelper::GetParent(root);
            if (!parent) break;
            root = parent;
        }
        auto rootFe = root.try_as<wux::FrameworkElement>();
        if (rootFe) e->buttonRoot = winrt::make_weak(rootFe);
        else e->buttonRoot = nullptr;
        e->buttons.clear();

        int count = 0;
        std::vector<wux::DependencyObject> stack;
        stack.push_back(root);
        while (!stack.empty())
        {
            wux::DependencyObject cur = stack.back();
            stack.pop_back();

            winrt::hstring cls = winrt::get_class_name(cur);
            std::wstring_view clsView{ cls };
            const bool taskList = clsView.find(L"TaskList") != std::wstring_view::npos;
            if (taskList) NoteTaskListName(L"classe na arvore", clsView);
            if (taskList &&
                clsView.find(L"Button") != std::wstring_view::npos &&
                clsView.find(L"Peer") == std::wstring_view::npos &&
                clsView.find(L"Panel") == std::wstring_view::npos)
            {
                if (auto fe = cur.try_as<wux::FrameworkElement>())
                {
                    e->buttons.push_back(winrt::make_weak(fe));
                    if (ButtonOnScreen(fe, rootFe)) ++count;
                }
                continue;  // nao precisa descer dentro do botao
            }

            const int children = wuxm::VisualTreeHelper::GetChildrenCount(cur);
            for (int k = 0; k < children; ++k)
                stack.push_back(wuxm::VisualTreeHelper::GetChild(cur, k));
        }
        return count;
    }
    catch (...)
    {
        return previous;
    }
}

// Reavaliacao rapida: so os botoes ja conhecidos (poucos), sem varrer a arvore.
static int QuickCountButtons(std::shared_ptr<Entry> const& e)
{
    wux::FrameworkElement rootFe{ nullptr };
    if (e->buttonRoot) rootFe = e->buttonRoot.get();
    int count = 0;
    for (auto const& weak : e->buttons)
        if (auto fe = weak.get())
            if (ButtonOnScreen(fe, rootFe)) ++count;
    return count;
}

static int CurrentButtonCount(std::shared_ptr<Entry> const& e)
{
    const uint32_t gen = g_taskListGen.load(std::memory_order_relaxed);
    const ULONGLONG now = GetTickCount64();
    const int before = e->buttonCount;
    if (before < 0 || gen != e->buttonGen || now >= e->nextRecount)
    {
        e->buttonGen = gen;
        e->buttonCount = ScanTaskListButtons(e, before < 0 ? 0 : before);
        // se mudou, confere de novo em 400 ms (o layout do botao novo ainda pode nao ter largura)
        e->nextRecount = now + (e->buttonCount != before ? 400 : e->buttonCount == 0 ? 1000 : 10000);
        e->nextQuick = now + 250;
    }
    else if (now >= e->nextQuick)
    {
        e->nextQuick = now + 250;
        e->buttonCount = QuickCountButtons(e);
    }
    if (e->buttonCount != before)
        Log(L"[v%s] botoes de apps na barra: %d (conhecidos: %zu)", kVersion, e->buttonCount, e->buttons.size());
    return e->buttonCount;
}

static void AnimateLiquidGlassWidth(std::shared_ptr<Entry> const& e, double targetWidth)
{
    if (e->liquidGlassWidth < 0)
    {
        e->rect.Width(targetWidth);
        e->liquidGlassWidth = targetWidth;
        return;
    }
    if (std::abs(e->liquidGlassWidth - targetWidth) < 1) return;

    const double currentWidth = e->rect.Width();
    e->liquidGlassAnimationFrom = std::isfinite(currentWidth) && currentWidth > 0
        ? currentWidth
        : e->liquidGlassWidth;
    e->liquidGlassWidth = targetWidth;
    e->liquidGlassAnimationStart = GetTickCount64();

    if (!e->liquidGlassAnimationTimer)
    {
        e->liquidGlassAnimationTimer = wux::DispatcherTimer();
        e->liquidGlassAnimationTimer.Interval(std::chrono::milliseconds(16));
        e->liquidGlassAnimationTimer.Tick(
            [weak = std::weak_ptr<Entry>(e)](wf::IInspectable const&, wf::IInspectable const&)
            {
                auto entry = weak.lock();
                if (!entry || !entry->liquidGlassAnimating) return;

                const double progress = std::min(
                    static_cast<double>(GetTickCount64() - entry->liquidGlassAnimationStart) / 65.0, 1.0);
                const double eased = 1.0 - std::pow(1.0 - progress, 3.0);
                const double width = entry->liquidGlassAnimationFrom +
                    (entry->liquidGlassWidth - entry->liquidGlassAnimationFrom) * eased;
                entry->rect.Width(width);

                if (progress >= 1.0)
                {
                    entry->rect.Width(entry->liquidGlassWidth);
                    entry->liquidGlassAnimating = false;
                    entry->liquidGlassAnimationTimer.Stop();
                }
            });
    }

    if (!e->liquidGlassAnimating)
    {
        e->liquidGlassAnimating = true;
        e->liquidGlassAnimationTimer.Start();
    }
}

static void ApplyLiquidGlassStyle(std::shared_ptr<Entry> const& e)
{
    if (!e->geometry) return;  // flyouts (preview etc.) seguem so a cor da barra
    try
    {
        const int appCount = CurrentButtonCount(e);
        if (e->liquidGlassStyleConfigured && appCount == e->liquidGlassAppCount)
            return;
        e->liquidGlassAppCount = appCount;

        if (!e->liquidGlassApplied)
            e->liquidGlassApplied = true;
        const double width = std::clamp((appCount * 32.0) + 180.0, 180.0, 800.0);
        const bool widthChanged = e->liquidGlassWidth < 0 ||
            std::abs(e->liquidGlassWidth - width) >= 1;
        AnimateLiquidGlassWidth(e, width);
        if (!e->liquidGlassStyleConfigured)
        {
            e->rect.Height(54);
            e->rect.RadiusX(27);
            e->rect.RadiusY(27);
            e->rect.HorizontalAlignment(wux::HorizontalAlignment::Center);
            e->rect.VerticalAlignment(wux::VerticalAlignment::Center);
            e->rect.Margin(wux::Thickness{ 0, 0, 0, 2 });
            e->rect.Stroke(wuxm::SolidColorBrush(winrt::Windows::UI::Color{ 160, 240, 255, 255 }));
            e->rect.StrokeThickness(1.0);
            e->rect.Opacity(1.0);
            e->liquidGlassStyleConfigured = true;
        }
        if (widthChanged)
            Log(L"[v%s] geometria Liquid Glass atualizada: largura=%.0f apps na barra=%d", kVersion, width, appCount);
    }
    catch (...)
    {
        RestoreLiquidGlassStyle(e);
        Log(L"[v%s] nao foi possivel aplicar a geometria Liquid Glass", kVersion);
    }
}

// Liga: guarda o fill atual do sistema e poe o transparente.
static void Enable(std::shared_ptr<Entry> const& e)
{
    if (!e->alive || e->enabled) return;
    try
    {
        auto cur = e->rect.Fill();
        if (cur != e->ours) e->original = cur;
        e->applied = GetCfg();
        e->ours = MakeBrush(e->applied);
        if (e->applied.mode == 4 || e->applied.mode == 6) ApplyLiquidGlassStyle(e);
        else RestoreLiquidGlassStyle(e);
        e->enabled = true;
        Log(L"[v%s] fundo da barra LIGADO: modo=%d cor=%06X opacidade=%d", kVersion, e->applied.mode, e->applied.rgb, e->applied.opacity);
        e->rect.Fill(e->ours);
    }
    catch (...) { e->enabled = false; }
}

// Desliga: devolve o fill original (mas continua observando).
static void Disable(std::shared_ptr<Entry> const& e)
{
    if (!e->alive || !e->enabled) return;
    e->enabled = false;
    RestoreLiquidGlassStyle(e);
    Log(L"[v%s] fundo original restaurado", kVersion);
    try { e->rect.Fill(e->original); } catch (...) {}
}

// Desarma de vez (app fechou).
static void Teardown(std::shared_ptr<Entry> const& e)
{
    if (!e->alive) return;
    Disable(e);
    e->alive = false;

    try
    {
        e->rect.UnregisterPropertyChangedCallback(wuxs::Shape::FillProperty(), e->token);
        e->rect.Tag(nullptr);
    }
    catch (...) {}

    if (e->timer)
    {
        auto t = e->timer;
        e->timer = nullptr;
        try { t.Stop(); } catch (...) {}
    }
}

static void ApplyOnUiThread(wuxs::Rectangle const& rect)
{
    auto e = std::make_shared<Entry>();
    e->rect = rect;
    e->original = rect.Fill();
    e->originalStroke = rect.Stroke();
    e->originalWidth = rect.Width();
    e->originalHeight = rect.Height();
    e->originalRadiusX = rect.RadiusX();
    e->originalRadiusY = rect.RadiusY();
    e->originalHorizontalAlignment = rect.HorizontalAlignment();
    e->originalVerticalAlignment = rect.VerticalAlignment();
    e->originalMargin = rect.Margin();
    e->originalStrokeThickness = rect.StrokeThickness();
    try { e->geometry = IsMainTaskbarBackground(rect); }
    catch (...) { e->geometry = false; }  // se falhar, o fundo ainda recebe cor
    RefreshCfg();
    e->applied = GetCfg();
    e->ours = MakeBrush(e->applied);
    e->alive = true;

    rect.Tag(winrt::box_value(winrt::hstring{ kTagMarker }));

    // Se o sistema trocar o Fill (ex.: ao abrir o preview), reaplica o nosso.
    try
    {
        e->token = rect.RegisterPropertyChangedCallback(wuxs::Shape::FillProperty(),
            [weak = std::weak_ptr<Entry>(e)](wux::DependencyObject const&, wux::DependencyProperty const&)
            {
                auto entry = weak.lock();
                if (!entry || !entry->alive || !entry->enabled || !entry->ours) return;
                try
                {
                    auto cur = entry->rect.Fill();
                    if (cur != entry->ours)
                    {
                        entry->original = cur;
                        entry->rect.Fill(entry->ours);
                    }
                }
                catch (...) {}
            });
    }
    catch (...) {}

    if (EffectActive()) Enable(e);

    e->timer = wux::DispatcherTimer();
    e->timer.Interval(std::chrono::milliseconds(100));
    e->timer.Tick([e](wf::IInspectable const&, wf::IInspectable const&)
    {
        if (!AppAlive()) { Teardown(e); return; }
        RefreshCfg();
        if (EffectActive())
        {
            Enable(e);
            if (e->enabled)
            {
                Cfg now = GetCfg();
                if (!(now == e->applied))
                {
                    if (now.mode == 4 || now.mode == 6) ApplyLiquidGlassStyle(e);
                    else RestoreLiquidGlassStyle(e);
                    e->applied = now;
                    e->ours = MakeBrush(now);
                    e->rect.Fill(e->ours);
                    Log(L"[v%s] fundo atualizado: modo=%d cor=%06X opacidade=%d", kVersion, now.mode, now.rgb, now.opacity);
                }
                if (e->applied.mode == 4 || e->applied.mode == 6)
                    ApplyLiquidGlassStyle(e);
            }
        }
        else Disable(e);
    });
    e->timer.Start();
}

// Todo Rectangle "BackgroundFill"/"BackgroundStroke" da barra recebe cor/transparencia
// (inclusive os flyouts: preview de janelas, jump lists etc., que seguem o estilo da barra).
static bool IsTaskbarElement(wux::DependencyObject const&)
{
    return true;  // o VisualTreeWatcher ja filtra pelo nome do retangulo
}

// Mas a GEOMETRIA do Liquid Glass (capsula, altura, borda) so vale para o fundo da barra
// principal. Nos flyouts ela fazia a capsula aparecer dentro do preview. Entao: sobe a
// arvore, recusa se achar um ancestral de flyout e exige o TaskbarFrame/TaskbarBackground.
static bool IsMainTaskbarBackground(wux::DependencyObject const& start)
{
    static constexpr std::wstring_view kExcluded[] =
    {
        L"Thumbnail", L"Flyout", L"Popup", L"ToolTip", L"ContextMenu",
        L"MenuFlyout", L"JumpView", L"Preview", L"Overflow", L"Hover"
    };

    bool inMainTaskbar = false;
    for (wux::DependencyObject cur = start; cur; cur = wuxm::VisualTreeHelper::GetParent(cur))
    {
        winrt::hstring name = winrt::get_class_name(cur);
        std::wstring_view sv{ name };

        for (auto const& word : kExcluded)
            if (sv.find(word) != std::wstring_view::npos)
            {
                Log(L"[v%s] fundo de flyout (so cor, sem geometria): %s", kVersion, name.c_str());
                return false;
            }

        if (sv.find(L"Taskbar.TaskbarFrame") != std::wstring_view::npos ||
            sv.find(L"Taskbar.TaskbarBackground") != std::wstring_view::npos)
            inMainTaskbar = true;

        if (auto fe = cur.try_as<wux::FrameworkElement>())
        {
            winrt::hstring elementNameStr = fe.Name();
            std::wstring_view elementName{ elementNameStr };
            for (auto const& word : kExcluded)
                if (elementName.find(word) != std::wstring_view::npos)
                {
                    Log(L"[v%s] fundo de flyout (so cor, sem geometria): %s", kVersion, elementNameStr.c_str());
                    return false;
                }
        }
    }
    return inMainTaskbar;
}

static void HandleRectangle(wuxs::Rectangle const& rect)
{
    if (!IsTaskbarElement(rect)) return;

    auto tag = winrt::unbox_value_or<winrt::hstring>(rect.Tag(), winrt::hstring{});
    if (tag == kTagMarker)
    {
        SignalApplied();
        return;
    }

    try
    {
        auto fill = rect.Fill();
        if (fill && fill != nullptr)
        {
            auto brushType = winrt::get_class_name(fill);
            std::wstring_view brushName{ brushType };
            if (brushName.find(L"AcrylicBrush") != std::wstring_view::npos ||
                brushName.find(L"SolidColorBrush") != std::wstring_view::npos ||
                brushName.find(L"ImageBrush") != std::wstring_view::npos)
            {
                // Preserve the existing system fill; we only replace the taskbar background when needed.
            }
        }
    }
    catch (...) {}

    auto disp = rect.Dispatcher();
    if (disp && !disp.HasThreadAccess())
    {
        disp.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [rect]
        {
            try { ApplyOnUiThread(rect); SignalApplied(); } catch (...) {}
        });
        return;
    }

    ApplyOnUiThread(rect);
    SignalApplied();
}

// ---------------------------------------------------------------- observador da arvore visual

struct VisualTreeWatcher : winrt::implements<VisualTreeWatcher, IVisualTreeServiceCallback2, winrt::non_agile>
{
    explicit VisualTreeWatcher(winrt::com_ptr<IXamlDiagnostics> diag) : m_diag(std::move(diag)) {}

    HRESULT STDMETHODCALLTYPE OnVisualTreeChange(
        ParentChildRelation, VisualElement element, VisualMutationType mutationType) noexcept override
    {
        try
        {
            // Acompanha a entrada/saida de botoes de apps da barra (Taskbar.TaskListButton).
            if (mutationType == Add)
            {
                if (element.Type && std::wcsstr(element.Type, L"TaskList"))
                {
                    NoteTaskListName(L"tipo adicionado", element.Type);
                    {
                        std::lock_guard<std::mutex> lock(g_taskListLock);
                        g_taskListHandles.insert(static_cast<uint64_t>(element.Handle));
                    }
                    g_taskListGen.fetch_add(1, std::memory_order_relaxed);
                }
            }
            else
            {
                bool removed = false;
                {
                    std::lock_guard<std::mutex> lock(g_taskListLock);
                    removed = g_taskListHandles.erase(static_cast<uint64_t>(element.Handle)) != 0;
                }
                if (removed) g_taskListGen.fetch_add(1, std::memory_order_relaxed);
            }

            if (mutationType == Add && element.Type && element.Name &&
                std::wcscmp(element.Type, L"Windows.UI.Xaml.Shapes.Rectangle") == 0 &&
                (std::wcscmp(element.Name, L"BackgroundFill") == 0 ||
                 std::wcscmp(element.Name, L"BackgroundStroke") == 0))
            {
                wf::IInspectable obj{ nullptr };
                winrt::check_hresult(m_diag->GetIInspectableFromHandle(
                    element.Handle, reinterpret_cast<::IInspectable**>(winrt::put_abi(obj))));

                if (auto rect = obj.try_as<wuxs::Rectangle>())
                    HandleRectangle(rect);
            }
        }
        catch (...) {}
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnElementStateChanged(InstanceHandle, VisualElementState, LPCWSTR) noexcept override
    {
        return S_OK;
    }

private:
    winrt::com_ptr<IXamlDiagnostics> m_diag;
};

// ---------------------------------------------------------------- site (ponto de entrada do TAP)

struct TapSite : winrt::implements<TapSite, IObjectWithSite>
{
    HRESULT STDMETHODCALLTYPE SetSite(IUnknown* site) noexcept override
    {
        try
        {
            m_site.copy_from(site);
            if (!site) return S_OK;
            Log(L"[v%s] DLL carregada no Explorer", kVersion);

            auto diag = m_site.as<IXamlDiagnostics>();
            auto svc = diag.as<IVisualTreeService3>();
            auto watcher = winrt::make_self<VisualTreeWatcher>(diag);

            // Chamar AdviseVisualTreeChange na mesma thread pode travar o XAML do Explorer
            // (visto no Windhawk Taskbar Styler), entao vai numa thread separada.
            std::thread([svc, watcher]
            {
                CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                svc->AdviseVisualTreeChange(static_cast<IVisualTreeServiceCallback*>(watcher.get()));
            }).detach();
        }
        catch (...)
        {
            return winrt::to_hresult();
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetSite(REFIID riid, void** ppv) noexcept override
    {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (!m_site) return E_FAIL;
        return m_site->QueryInterface(riid, ppv);
    }

private:
    winrt::com_ptr<IUnknown> m_site;
};

struct TapFactory : winrt::implements<TapFactory, IClassFactory>
{
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID riid, void** ppv) noexcept override
    {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        return winrt::make_self<TapSite>()->QueryInterface(riid, ppv);
    }

    HRESULT STDMETHODCALLTYPE LockServer(BOOL) noexcept override { return S_OK; }
};

// ---------------------------------------------------------------- exports da DLL

extern "C" HRESULT __stdcall DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (!IsEqualGUID(clsid, CLSID_TricBarBlur)) return CLASS_E_CLASSNOTAVAILABLE;
    return winrt::make_self<TapFactory>()->QueryInterface(riid, ppv);
}

extern "C" HRESULT __stdcall DllCanUnloadNow() { return S_FALSE; }

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(module);
        // Fixa a DLL na memoria do Explorer: threads/timers nossos continuam rodando
        // depois do restore, entao descarregar a DLL seria crash garantido.
        HMODULE pinned = nullptr;
        GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCWSTR>(&DllMain), &pinned);
    }
    return TRUE;
}
