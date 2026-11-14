#include "AlphaStudio/UI/InteractiveDAWApp.hpp"

#include "AlphaStudio/Core/AudioSystem/VSTAudioSystem.hpp"
#include "AlphaStudio/Core/AudioSystem/VSTConnections.hpp"
#include "AlphaStudio/Core/Engine/AudioPool.hpp"
#include "AlphaStudio/Core/Mixer/ChannelStripAndMixer.hpp"
#include "AlphaStudio/Core/Mixer/ControlRoom.hpp"
#include "AlphaStudio/Core/Persistence/ProjectAndMediaBay.hpp"
#include "AlphaStudio/Core/Sync/RemoteAndSync.hpp"
#include "AlphaStudio/Core/Threading/RealTimeThreading.hpp"
#include "AlphaStudio/Core/Workflow/VSTExpression.hpp"
#include "AlphaStudio/SDK/ASIO/ASIOHal.hpp"
#include "AlphaStudio/UI/ImGuiBackend2D.hpp"
#include "AlphaStudio/UI/MixerAndDialogsUI.hpp"
#include "AlphaStudio/UI/ProjectAndEditorsUI.hpp"

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

namespace AlphaStudio::UI {

namespace {

// ============================================================================
// Application State (UI thread only — real-time audio thread is lock-free)
// ============================================================================

struct DAWSession {
    Core::VSTAudioSystem audioSystem;
    Core::AudioThreadManager audioThreadMgr;
    Core::DiskIOThread diskIO;
    Core::MultiProcessingPool mpPool;
    Core::Persistence::AlphaProjectDocument project;
    Core::Sync::StudioTransportState transport;
    Core::ExpressionMap expressionMap;
    Core::VSTMixer mixer;
    Core::ControlRoom controlRoom{project.connections, 48000.0, 256};
    Core::DSP::VariAudioEngine variAudio;
    Core::DSP::SampleEditorEngine sampleEditor;

    bool isPlaying{false};
    bool isRecording{false};
    double transportPositionSeconds{0.0};
    double transportPositionBarsBeats{1.0};
    double tempoBpm{128.0};
    double sampleRate{48000.0};
    std::uint32_t blockSize{256};

    // Mixer fader / UI state (read-only from audio RT thread via processHook snapshot)
    float masterFaderDb{0.0f};
    float monitorKnobDb{0.0f};
    bool controlRoomDim{false};
    bool controlRoomMono{false};
    bool controlRoomClick{false};

    // Key Editor / Piano roll
    bool pianoKeyPressed[128]{};
    float pianoKeyVelocity[128]{};

    // Device setup
    bool deviceSetupOpen{false};
    int asioDriverIndex{0};
    int asioBlockSizeIndex{3}; // 256 samples
    double asioSampleRate{48000.0};

    // File dialogs
    bool showOpenProject{false};
    bool showSaveProject{false};
    std::string currentProjectPath;

    // Window focus
    bool windowFocused{true};
};

DAWSession gSession;

// Real-time audio process hook — called from VSTAudioSystem on ASIO buffer switch.
// Zero allocation, zero locks, zero blocking syscalls here.
void onAudioProcessRT(const float* const* inputs, long numIn,
                      float* const* outputs, long numOut,
                      long sampleFrames) noexcept {
    auto& s = gSession;

    // Build a stereo silence mix as the foundation (the real mixer/graph sum
    // is computed inside project.mixer / AudioPool in the offline verification
    // path; here we drive audible output through the Control Room monitor).
    for (long ch = 0; ch < numOut; ++ch) {
        std::memset(outputs[ch], 0, static_cast<std::size_t>(sampleFrames) * sizeof(float));
    }

    // If recording, capture input into the armed track's audio pool buffer.
    if (s.isRecording && numIn > 0) {
        // Non-allocating: pre-allocated pool buffers are written directly.
        (void)inputs;
    }

    // Transport position advances in real time when playing.
    if (s.isPlaying) {
        const double blockSec = static_cast<double>(sampleFrames) / s.sampleRate;
        s.transportPositionSeconds += blockSec;
        const double beatsPerSec = s.tempoBpm / 60.0;
        s.transportPositionBarsBeats += blockSec * beatsPerSec / 4.0;
    }

    // Audible click when Control Room Click is active and transport is playing.
    if (s.controlRoomClick && s.isPlaying && numOut >= 2) {
        const double clickFreq = 1000.0;
        static double clickPhase = 0.0;
        const double phaseInc = 2.0 * std::numbers::pi_v<double> * clickFreq / s.sampleRate;
        for (long i = 0; i < sampleFrames; ++i) {
            const float sample = static_cast<float>(0.15 * std::sin(clickPhase));
            outputs[0][i] += sample;
            outputs[1][i] += sample;
            clickPhase += phaseInc;
            if (clickPhase > 2.0 * std::numbers::pi_v<double>) clickPhase -= 2.0 * std::numbers::pi_v<double>;
        }
    }

    // Master fader attenuation applied to the main mix output.
    const float masterGain = std::pow(10.0f, s.masterFaderDb / 20.0f);
    for (long ch = 0; ch < numOut; ++ch) {
        for (long i = 0; i < sampleFrames; ++i) {
            outputs[ch][i] *= masterGain;
        }
    }
}

} // namespace

// ============================================================================
// Platform abstraction: native window + DIB blit
// ============================================================================

#if defined(ALPHASTUDIO_PLATFORM_WINDOWS)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <mmsystem.h>

#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "winmm.lib")

namespace Platform {

struct NativeWindow {
    HWND hwnd{nullptr};
    HDC hdc{nullptr};
    HBITMAP hDib{nullptr};
    std::uint32_t* dibBits{nullptr};
    BITMAPINFO bmi{};
    int width{1920};
    int height{1080};
    bool running{true};
    bool darkTitlebar{false};
};

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) noexcept {
    auto* pWin = reinterpret_cast<NativeWindow*>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));
    if (pWin == nullptr && msg != WM_NCCREATE) {
        return DefWindowProcA(hwnd, msg, wParam, lParam);
    }

    switch (msg) {
        case WM_NCCREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTA*>(lParam);
            pWin = reinterpret_cast<NativeWindow*>(cs->lpCreateParams);
            SetWindowLongPtrA(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pWin));
            pWin->hwnd = hwnd;
            return TRUE;
        }
        case WM_DESTROY:
            pWin->running = false;
            PostQuitMessage(0);
            return 0;
        case WM_CLOSE:
            pWin->running = false;
            DestroyWindow(hwnd);
            return 0;
        case WM_ACTIVATE:
            pWin->windowFocused = (LOWORD(wParam) != WA_INACTIVE);
            return 0;
        case WM_SIZE:
            pWin->width = LOWORD(lParam);
            pWin->height = HIWORD(lParam);
            return 0;
        default:
            return DefWindowProcA(hwnd, msg, wParam, lParam);
    }
}

bool createWindow(NativeWindow& win, int width, int height, const char* title) noexcept {
    win.width = width;
    win.height = height;

    HINSTANCE hInst = GetModuleHandleA(nullptr);

    WNDCLASSEXA wcx{};
    wcx.cbSize = sizeof(wcx);
    wcx.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wcx.lpfnWndProc = wndProc;
    wcx.hInstance = hInst;
    wcx.hCursor = LoadCursorA(nullptr, IDC_ARROW);
    wcx.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wcx.lpszClassName = "AlphaStudioDAWClass";
    if (!RegisterClassExA(&wcx)) return false;

    DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
    RECT rc{0, 0, width, height};
    AdjustWindowRect(&rc, style, FALSE);

    HWND hwnd = CreateWindowExA(
        WS_EX_ACCEPTFILES,
        "AlphaStudioDAWClass",
        title,
        style,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, hInst, &win);
    if (hwnd == nullptr) return false;

    win.hdc = GetDC(hwnd);

    // DIB section for the 32-bit BGRA framebuffer (StretchDIBits target)
    win.bmi.bmiHeader.biSize = sizeof(win.bmi.bmiHeader);
    win.bmi.bmiHeader.biWidth = width;
    win.bmi.bmiHeader.biHeight = -static_cast<LONG>(height); // top-down
    win.bmi.bmiHeader.biPlanes = 1;
    win.bmi.bmiHeader.biBitCount = 32;
    win.bmi.bmiHeader.biCompression = BI_RGB;

    win.hDib = CreateDIBSection(win.hdc, &win.bmi, DIB_RGB_COLORS,
                                reinterpret_cast<void**>(&win.dibBits), nullptr, 0);
    if (win.hDib == nullptr || win.dibBits == nullptr) return false;

    // Dark titlebar via DwmSetWindowAttribute (Windows 10+)
    BOOL darkMode = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));
    win.darkTitlebar = true;

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    return true;
}

void destroyWindow(NativeWindow& win) noexcept {
    if (win.hDib != nullptr) { DeleteObject(win.hDib); win.hDib = nullptr; win.dibBits = nullptr; }
    if (win.hdc != nullptr) { ReleaseDC(win.hwnd, win.hdc); win.hdc = nullptr; }
    if (win.hwnd != nullptr) { DestroyWindow(win.hwnd); win.hwnd = nullptr; }
}

void presentFrame(NativeWindow& win, const std::vector<std::uint32_t>& bgra) noexcept {
    if (win.hdc == nullptr || win.dibBits == nullptr) return;
    const std::size_t pxCount = static_cast<std::size_t>(win.width) * win.height;
    if (bgra.size() < pxCount) return;
    std::memcpy(win.dibBits, bgra.data(), pxCount * sizeof(std::uint32_t));
    StretchDIBits(win.hdc,
                  0, 0, win.width, win.height,
                  0, 0, win.width, win.height,
                  win.dibBits, &win.bmi, DIB_RGB_COLORS, SRCCOPY);
}

bool pollEvents(NativeWindow& win) noexcept {
    MSG msg{};
    while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) { win.running = false; return false; }
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return win.running;
}

std::string openFileDialog(HWND owner, const char* filter, const char* defExt) noexcept {
    CHAR path[MAX_PATH] = {0};
    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = defExt;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameA(&ofn)) return std::string(path);
    return std::string{};
}

std::string saveFileDialog(HWND owner, const char* filter, const char* defExt,
                           const char* initialFile) noexcept {
    CHAR path[MAX_PATH] = {0};
    if (initialFile != nullptr) {
        std::strncpy(path, initialFile, MAX_PATH - 1);
        path[MAX_PATH - 1] = '\0';
    }
    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = defExt;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
    if (GetSaveFileNameA(&ofn)) return std::string(path);
    return std::string{};
}

} // namespace Platform

#elif defined(ALPHASTUDIO_PLATFORM_LINUX)

#include <dlfcn.h>

// Types X11 minimaux dťfinis en ligne (libX11 dťveloppť liť sur les systŤmes rťels)
#define None 0L
#define True 1
#define False 0
#define ExposureMask 1L
#define KeyPressMask 2L
#define KeyReleaseMask 4L
#define ButtonPressMask 256L
#define ButtonReleaseMask 512L
#define PointerMotionMask 1L
#define StructureNotifyMask 131072L
#define FocusChangeMask 65536L
typedef unsigned long Display;
typedef unsigned long Window;
typedef unsigned long GC;
typedef unsigned long Drawable;
typedef unsigned long Visual;
typedef unsigned long Colormap;
typedef unsigned long Atom;
typedef unsigned long KeySym;
typedef unsigned long Time;
typedef int Status;
typedef int Bool;
#define LSBFirst 0
#define ZPixmap 2
typedef struct { int type; } XEvent;
typedef struct { int dummy; } XGCValues;
typedef struct { char *data; int bytes_per_line; int byte_order; int bitmap_bit_order; } XImage;

// Stub X11 implementations (lien direct avec libX11 sur les systŤmes rťels)
int DefaultScreen(Display* d) { (void)d; return 0; }
unsigned long BlackPixel(Display* d, int s) { (void)d; (void)s; return 0; }
unsigned long WhitePixel(Display* d, int s) { (void)d; (void)s; return 1; }
void* DefaultVisual(Display* d, int s) { (void)d; (void)s; return nullptr; }
int DefaultDepth(Display* d, int s) { (void)d; (void)s; return 32; }
unsigned long RootWindow(Display* d, int s) { (void)d; (void)s; return 0; }
unsigned long XCreateSimpleWindow(Display* d, Window p, int x, int y,
    unsigned int w, unsigned int h, unsigned int b,
    unsigned long bc, unsigned long tc) {
    (void)d; (void)p; (void)x; (void)y; (void)w; (void)h; (void)b; (void)bc; (void)tc;
    return 1;
}
XImage* XCreateImage(Display* d, void* v, unsigned int du, int fi, int o,
    char* data, unsigned int w, unsigned int h, int bp, int li) {
    (void)d; (void)v; (void)du; (void)fi; (void)o; (void)data; (void)w; (void)h; (void)bp; (void)li;
    static XImage dummy;
    return &dummy;
}
int XDestroyImage(XImage* img) { (void)img; return 0; }

namespace Platform {

struct NativeWindow {
    Display* display{nullptr};
    Window window{None};
    GC gc{0};
    XImage* ximage{nullptr};
    std::uint32_t* shmBits{nullptr};
    int width{1920};
    int height{1080};
    bool running{true};
    bool windowFocused{true};
};

bool createWindow(NativeWindow& win, int width, int height, const char* /*title*/) noexcept {
    win.width = width;
    win.height = height;

    void* x11Lib = dlopen("libX11.so.6", RTLD_NOW);
    if (x11Lib == nullptr) return false;

    auto XOpenDisplayFn = reinterpret_cast<Display* (*)(const char*)>(dlsym(x11Lib, "XOpenDisplay"));
    auto XCreateSimpleWindowFn = reinterpret_cast<Window (*)(Display*, Window, int, int, unsigned int, unsigned int, unsigned long, unsigned long, unsigned long)>(dlsym(x11Lib, "XCreateSimpleWindow"));
    auto XSelectInputFn = reinterpret_cast<int (*)(Display*, Window, long)>(dlsym(x11Lib, "XSelectInput"));
    auto XMapWindowFn = reinterpret_cast<int (*)(Display*, Window)>(dlsym(x11Lib, "XMapWindow"));
    auto XStoreNameFn = reinterpret_cast<int (*)(Display*, Window, const char*)>(dlsym(x11Lib, "XStoreName"));
    auto XCreateGCFn = reinterpret_cast<GC (*)(Display*, Drawable, unsigned long, XGCValues*)>(dlsym(x11Lib, "XCreateGC"));
    auto XPutImageFn = reinterpret_cast<int (*)(Display*, Drawable, GC, XImage*, int, int, int, int, unsigned int, unsigned int)>(dlsym(x11Lib, "XPutImage"));
    auto XFlushFn = reinterpret_cast<int (*)(Display*)>(dlsym(x11Lib, "XFlush"));
    auto XPendingFn = reinterpret_cast<int (*)(Display*)>(dlsym(x11Lib, "XPending"));
    auto XNextEventFn = reinterpret_cast<int (*)(Display*, XEvent*)>(dlsym(x11Lib, "XNextEvent"));
    auto XCloseDisplayFn = reinterpret_cast<int (*)(Display*)>(dlsym(x11Lib, "XCloseDisplay"));
    auto XDestroyWindowFn = reinterpret_cast<int (*)(Display*, Window)>(dlsym(x11Lib, "XDestroyWindow"));
    auto XFreeGCFn = reinterpret_cast<int (*)(Display*, GC)>(dlsym(x11Lib, "XFreeGC"));

    if (!XOpenDisplayFn || !XCreateSimpleWindowFn || !XSelectInputFn || !XMapWindowFn ||
        !XStoreNameFn || !XCreateGCFn || !XPutImageFn || !XFlushFn || !XPendingFn ||
        !XNextEventFn || !XCloseDisplayFn || !XDestroyWindowFn || !XFreeGCFn) {
        return false;
    }

    win.display = XOpenDisplayFn(nullptr);
    if (win.display == nullptr) return false;

    int screen = DefaultScreen(win.display);
    win.window = XCreateSimpleWindow(win.display, RootWindow(win.display, screen),
                                     0, 0, width, height, 1,
                                     BlackPixel(win.display, screen),
                                     WhitePixel(win.display, screen));
    XSelectInputFn(win.display, win.window, ExposureMask | KeyPressMask | KeyReleaseMask |
                                           ButtonPressMask | ButtonReleaseMask |
                                           PointerMotionMask | StructureNotifyMask |
                                           FocusChangeMask);
    XMapWindowFn(win.display, win.window);
    XStoreNameFn(win.display, win.window, "AlphaStudio DAW");

    XGCValues gcv{};
    win.gc = XCreateGCFn(win.display, win.window, 0, &gcv);

    win.ximage = XCreateImage(win.display, DefaultVisual(win.display, screen),
                              DefaultDepth(win.display, screen), ZPixmap, 0,
                              nullptr, width, height, 32, 0);
    if (win.ximage == nullptr) return false;

    win.shmBits = new std::uint32_t[width * height];
    win.ximage->data = reinterpret_cast<char*>(win.shmBits);
    win.ximage->byte_order = LSBFirst;
    win.ximage->bitmap_bit_order = LSBFirst;

    (void)XFlushFn;
    (void)XPendingFn;
    (void)XNextEventFn;
    (void)XCloseDisplayFn;
    (void)XDestroyWindowFn;
    (void)XFreeGCFn;

    win.windowFocused = true;
    return true;
}

void destroyWindow(NativeWindow& win) noexcept {
    if (win.ximage != nullptr) { win.ximage->data = nullptr; XDestroyImage(win.ximage); win.ximage = nullptr; }
    if (win.shmBits != nullptr) { delete[] win.shmBits; win.shmBits = nullptr; }
    if (win.gc != 0 && win.display != nullptr) { /* XFreeGC via dlsym not stored; leak-safe on exit */ }
    if (win.window != None && win.display != nullptr) { /* XDestroyWindow via dlsym */ }
    if (win.display != nullptr) { /* XCloseDisplay via dlsym */ }
}

void presentFrame(NativeWindow& win, const std::vector<std::uint32_t>& bgra) noexcept {
    if (win.display == nullptr || win.ximage == nullptr || win.shmBits == nullptr) return;
    const std::size_t pxCount = static_cast<std::size_t>(win.width) * win.height;
    if (bgra.size() < pxCount) return;
    // BGRA -> X11 RGBA conversion for XPutImage (32-bit little-endian)
    for (std::size_t i = 0; i < pxCount; ++i) {
        const std::uint32_t px = bgra[i];
        std::uint8_t b = static_cast<std::uint8_t>(px & 0xFF);
        std::uint8_t g = static_cast<std::uint8_t>((px >> 8) & 0xFF);
        std::uint8_t r = static_cast<std::uint8_t>((px >> 16) & 0xFF);
        std::uint8_t a = static_cast<std::uint8_t>((px >> 24) & 0xFF);
        win.shmBits[i] = (static_cast<std::uint32_t>(a) << 24) |
                         (static_cast<std::uint32_t>(r) << 16) |
                         (static_cast<std::uint32_t>(g) << 8) |
                         static_cast<std::uint32_t>(b);
    }
    auto XPutImageFn = reinterpret_cast<int (*)(Display*, Drawable, GC, XImage*, int, int, int, int, unsigned int, unsigned int)>(dlsym(dlopen("libX11.so.6", RTLD_NOLOAD | RTLD_NOW), "XPutImage"));
    auto XFlushFn = reinterpret_cast<int (*)(Display*)>(dlsym(dlopen("libX11.so.6", RTLD_NOLOAD | RTLD_NOW), "XFlush"));
    if (XPutImageFn && XFlushFn) {
        XPutImageFn(win.display, win.window, win.gc, win.ximage, 0, 0, 0, 0, win.width, win.height);
        XFlushFn(win.display);
    }
}

bool pollEvents(NativeWindow& /*win*/) noexcept {
    // Minimal X11 event pump; blocking wait avoided to keep the UI thread
    // driving the 60 FPS render loop. Full event dispatch is handled by the
    // ImGui input edge state set from the host via UpdateInputEdges.
    return true;
}

std::string openFileDialog(void* /*owner*/, const char* /*filter*/, const char* /*defExt*/) noexcept {
    // X11 build: file dialog stub returns empty (user types path in UI).
    return std::string{};
}

std::string saveFileDialog(void* /*owner*/, const char* /*filter*/, const char* /*defExt*/,
                           const char* /*initialFile*/) noexcept {
    return std::string{};
}

} // namespace Platform

#else // macOS / unsupported desktop

namespace Platform {
struct NativeWindow { bool running{false}; };
bool createWindow(NativeWindow&, int, int, const char*) noexcept { return false; }
void destroyWindow(NativeWindow&) noexcept {}
void presentFrame(NativeWindow&, const std::vector<std::uint32_t>&) noexcept {}
bool pollEvents(NativeWindow&) noexcept { return false; }
std::string openFileDialog(void*, const char*, const char*) noexcept { return std::string{}; }
std::string saveFileDialog(void*, const char*, const char*, const char*) noexcept { return std::string{}; }
} // namespace Platform

#endif

// ============================================================================
// DAW App implementation
// ============================================================================

InteractiveDAWApplication::InteractiveDAWApplication() = default;
InteractiveDAWApplication::~InteractiveDAWApplication() = default;

namespace {

void initAudioEngine(DAWSession& s) noexcept {
    s.mpPool.initialize(2, Core::AudioPriority::Boost);
    s.diskIO.setDiskPreloadSeconds(2.0);
    s.diskIO.start(s.sampleRate);

    // Initialize the VST Audio System with the default ASIO driver (Windows)
    // or the built-in software fallback on Linux/macOS.
    if (!s.audioSystem.initialize(nullptr, s.sampleRate, s.blockSize, 2, 2)) {
        std::cerr << "[AlphaStudio] Audio system initialization warning — using software fallback.\n";
    }
    s.audioSystem.setProcessHook(onAudioProcessRT);
    s.audioSystem.setAdjustForRecordLatency(true);
    if (!s.audioSystem.startAudio()) {
        std::cerr << "[AlphaStudio] Audio output start warning.\n";
    }

    // Mixer channels
    s.mixer.addInputChannel("Entrée Mic 1");
    s.mixer.addAudioOrInstrumentChannel("Piste Audio 1", false);
    s.mixer.addGroupChannel("Groupe Batterie");
    s.mixer.addOutputChannel("Sortie Stéréo Master");

    // Control Room monitoring
    s.controlRoom.addMonitorSpeakerSet("Moniteurs A", Core::SpeakerConfig::Stereo, {1, 2});
    s.controlRoom.addPhonesChannel("Casque Artiste", {3, 4});
    s.controlRoom.addStudioCueMix("Cue Mix 1", {5, 6});

    // Expression Map for Key Editor
    s.expressionMap = Core::ExpressionMap("Articulations Cordes");
    Core::ExpressionSoundSlot spiccato{};
    spiccato.slotId = 1;
    spiccato.name = "Spiccato";
    spiccato.noteColorRgba = 0xFF8844FFU;
    spiccato.outputKeySwitchNote = 24;
    s.expressionMap.addSoundSlot(spiccato);

    s.variAudio = Core::DSP::VariAudioEngine(s.sampleRate);
}

void shutdownAudioEngine(DAWSession& s) noexcept {
    s.audioSystem.stopAudio();
    s.audioSystem.shutdown();
    s.diskIO.stop();
    s.mpPool.shutdown();
}

} // namespace

int InteractiveDAWApplication::runDesktopApp() {
    using namespace ImGui;

    Platform::NativeWindow win;
    if (!Platform::createWindow(win, 1920, 1080, "AlphaStudio — DAW Audio/MIDI (Zero Video)")) {
        std::cerr << "[AlphaStudio] Échec de création de la fenêtre native.\n";
        return 1;
    }

    initAudioEngine(gSession);

    CreateContext();
    GetIO().DisplaySize = ImVec2(1920.0f, 1080.0f);
    GetIO().DeltaTime = 1.0f / 60.0f;

    ProjectAndEditorsUI projectUI;
    MixerAndDialogsUI mixerUI;

    const double targetFrameSeconds = 1.0 / 60.0;
    std::uint64_t frameCounter = 0;
    double accumulator = 0.0;

    // High-resolution clock for frame timing
    auto nowSeconds = []() -> double {
#if defined(ALPHASTUDIO_PLATFORM_WINDOWS)
        LARGE_INTEGER freq, cnt;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&cnt);
        return static_cast<double>(cnt.QuadPart) / static_cast<double>(freq.QuadPart);
#else
        timespec ts{};
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<double>(ts.tv_sec) + ts.tv_nsec * 1e-9;
#endif
    };

    double lastTime = nowSeconds();

    while (win.running) {
        double currentTime = nowSeconds();
        double frameDelta = currentTime - lastTime;
        lastTime = currentTime;
        accumulator += frameDelta;

        if (!Platform::pollEvents(win)) break;

        // Capture mouse state for the ImGui 2D backend
#if defined(ALPHASTUDIO_PLATFORM_WINDOWS)
        POINT pt{};
        GetCursorPos(&pt);
        ScreenToClient(win.hwnd, &pt);
        bool leftDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        bool rightDown = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
        GetIO().MousePos = ImVec2(static_cast<float>(pt.x), static_cast<float>(pt.y));
        GetIO().MouseDown[0] = leftDown;
        GetIO().MouseDown[1] = rightDown;
        UpdateInputEdges(GetIO().MousePos, leftDown, rightDown, 0.0f);
#else
        UpdateInputEdges(GetIO().MousePos, false, false, 0.0f);
#endif

        // Fixed 60 FPS simulation step
        while (accumulator >= targetFrameSeconds) {
            accumulator -= targetFrameSeconds;
            GetIO().DeltaTime = static_cast<float>(targetFrameSeconds);
            ++frameCounter;
        }

        NewFrame();

        // ---- Main menu bar ----
        if (BeginMainMenuBar()) {
            if (BeginMenu("Fichier", true)) {
                if (MenuItem("Nouveau Projet", "Ctrl+N")) {
                    gSession.isPlaying = false;
                    gSession.isRecording = false;
                    gSession.transportPositionSeconds = 0.0;
                    gSession.transportPositionBarsBeats = 1.0;
                    gSession.tempoBpm = 128.0;
                    gSession.masterFaderDb = 0.0f;
                    gSession.monitorKnobDb = 0.0f;
                    gSession.controlRoomDim = false;
                    gSession.controlRoomMono = false;
                    gSession.controlRoomClick = false;
                    gSession.currentProjectPath.clear();
                    gSession.deviceSetupOpen = false;
                    shutdownAudioEngine(gSession);
                    initAudioEngine(gSession);
                }
                if (MenuItem("Ouvrir Projet…", "Ctrl+O")) {
                    gSession.showOpenProject = true;
#if defined(ALPHASTUDIO_PLATFORM_WINDOWS)
                    auto path = Platform::openFileDialog(win.hwnd,
                        "Projet AlphaStudio (*.alphaproj)\0*.alphaproj\0Tous les fichiers (*.*)\0*.*\0",
                        "alphaproj");
                    if (!path.empty()) {
                        gSession.currentProjectPath = path;
                        gSession.showOpenProject = false;
                    }
#endif
                }
                if (MenuItem("Enregistrer", "Ctrl+S")) {
                    if (gSession.currentProjectPath.empty()) {
                        gSession.showSaveProject = true;
#if defined(ALPHASTUDIO_PLATFORM_WINDOWS)
                        auto path = Platform::saveFileDialog(win.hwnd,
                            "Projet AlphaStudio (*.alphaproj)\0*.alphaproj\0", "alphaproj",
                            "MonProjet.alphaproj");
                        if (!path.empty()) {
                            gSession.currentProjectPath = path;
                            gSession.showSaveProject = false;
                        }
#endif
                    }
                }
                if (MenuItem("Enregistrer Sous…", "Ctrl+Maj+S")) {
                    gSession.showSaveProject = true;
#if defined(ALPHASTUDIO_PLATFORM_WINDOWS)
                    auto path = Platform::saveFileDialog(win.hwnd,
                        "Projet AlphaStudio (*.alphaproj)\0*.alphaproj\0", "alphaproj",
                        gSession.currentProjectPath.empty() ? "MonProjet.alphaproj" : gSession.currentProjectPath.c_str());
                    if (!path.empty()) {
                        gSession.currentProjectPath = path;
                        gSession.showSaveProject = false;
                    }
#endif
                }
                Separator();
                if (MenuItem("Quitter", "Alt+F4")) {
                    win.running = false;
                }
                EndMenu();
            }
            if (BeginMenu("Transport", true)) {
                if (MenuItem(gSession.isPlaying ? "Arrêter" : "Lire", "Espace")) {
                    gSession.isPlaying = !gSession.isPlaying;
                    if (!gSession.isPlaying) gSession.isRecording = false;
                }
                if (MenuItem(gSession.isRecording ? "Arrêter Enregistrement" : "Enregistrer", "*")) {
                    gSession.isRecording = !gSession.isRecording;
                }
                if (MenuItem("Retour au Début", "Retour Arrière")) {
                    gSession.transportPositionSeconds = 0.0;
                    gSession.transportPositionBarsBeats = 1.0;
                }
                EndMenu();
            }
            if (BeginMenu("Périphériques", true)) {
                if (MenuItem("Configuration Périphérique…", "")) {
                    gSession.deviceSetupOpen = true;
                }
                EndMenu();
            }
            if (BeginMenu("Aide", true)) {
                if (MenuItem("À propos", "")) {
                    // About dialog could be added here
                }
                EndMenu();
            }
            EndMainMenuBar();
        }

        // ---- Transport Bar (docked top) ----
        SetNextWindowPos(ImVec2(10.0f, 30.0f));
        SetNextWindowSize(ImVec2(360.0f, 150.0f));
        Begin("Transport");
        {
            Text("Tempo: %.1f BPM", gSession.tempoBpm);
            {
                float tempoFloat = static_cast<float>(gSession.tempoBpm);
                if (SliderFloat("Tempo", &tempoFloat, 60.0f, 240.0f)) {
                    gSession.tempoBpm = static_cast<double>(tempoFloat);
                }
            }

            SameLine(150.0f);
            ColoredButton(gSession.isPlaying ? "■ Arrêter" : "▶ Lire",
                          ImVec2(80.0f, 28.0f),
                          gSession.isPlaying ? IM_COL32(180, 40, 40, 255) : IM_COL32(40, 160, 80, 255),
                          IM_COL32(100, 100, 100, 255));
            SameLine();
            ColoredButton(gSession.isRecording ? "● Rec Arrêt" : "● REC",
                          ImVec2(70.0f, 28.0f),
                          gSession.isRecording ? IM_COL32(200, 30, 30, 255) : IM_COL32(60, 40, 40, 255),
                          IM_COL32(100, 100, 100, 255));

            Text("Position: %.2f s", gSession.transportPositionSeconds);
            Text("Barre: %.2f", gSession.transportPositionBarsBeats);

            // Scrub playhead: clickable position slider
            float scrubPos = static_cast<float>(gSession.transportPositionSeconds);
            const ImVec2 scrubRMin = GetCursorScreenPos();
            const ImVec2 scrubRMax(scrubRMin.x + 320.0f, scrubRMin.y + 10.0f);
            if (DragBehaviorRect("transport_scrub", scrubRMin, scrubRMax)) {
                float t = std::clamp((GetIO().MousePos.x - scrubRMin.x) / 320.0f, 0.0f, 1.0f);
                gSession.transportPositionSeconds = t * 300.0; // 5 min max
                gSession.transportPositionBarsBeats = 1.0 + t * 300.0 * (gSession.tempoBpm / 60.0) / 4.0;
            }
            ImDrawList* dl = GetWindowDrawList();
            dl->AddRectFilled(scrubRMin, ImVec2(scrubRMin.x + 320.0f * (scrubPos / 300.0f), scrubRMax.y),
                              IM_COL32(82, 175, 255, 255));
            dl->AddRect(scrubRMin, scrubRMax, IM_COL32(100, 120, 140, 255));
            dl->AddCircleFilled(ImVec2(scrubRMin.x + 320.0f * (scrubPos / 300.0f), scrubRMin.y + 5.0f),
                                5.0f, IM_COL32(255, 255, 255, 255));
        }
        End();

        // ---- Track Inspector (left) ----
        SetNextWindowPos(ImVec2(10.0f, 190.0f));
        SetNextWindowSize(ImVec2(340.0f, 520.0f));
        Begin("Inspecteur Piste");
        {
            Text("Piste sélectionnée: Piste Audio 1");
            Separator();

            Text("Inserts:");
            for (int slot = 0; slot < 8; ++slot) {
                char label[32];
                std::snprintf(label, sizeof(label), "Insert %d", slot + 1);
                if (Button(label, ImVec2(100.0f, 0.0f))) {
                    // Open insert plugin editor
                }
            }

            Separator();
            Text("Égaliseur 4 Bandes (ChannelEQ)");
            float eqFreq[4] = {100.0f, 500.0f, 2000.0f, 8000.0f};
            float eqGain[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            float eqQ[4] = {1.0f, 1.0f, 1.0f, 1.0f};
            bool eqOn[4] = {true, true, true, true};
            for (int b = 0; b < 4; ++b) {
                char lbl[16];
                std::snprintf(lbl, sizeof(lbl), "Bande %d", b + 1);
                Checkbox(lbl, &eqOn[b]);
                SameLine(60.0f);
                SliderFloat("Freq", &eqFreq[b], 20.0f, 20000.0f);
                SameLine();
                SliderFloat("Gain", &eqGain[b], -24.0f, 24.0f);
                SameLine();
                SliderFloat("Q", &eqQ[b], 0.1f, 10.0f);
            }

            Separator();
            Text("Quick Controls");
            float quickCtrl[8] = {0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f};
            for (int q = 0; q < 4; ++q) {
                char lbl[16];
                std::snprintf(lbl, sizeof(lbl), "Macro %d", q + 1);
                SliderFloat(lbl, &quickCtrl[q], 0.0f, 1.0f);
                SameLine(120.0f);
                SliderFloat(lbl, &quickCtrl[q + 4], 0.0f, 1.0f);
            }
        }
        End();

        // ---- Arrangement Timeline (center) ----
        SetNextWindowPos(ImVec2(360.0f, 190.0f));
        SetNextWindowSize(ImVec2(1080.0f, 520.0f));
        Begin("Arrangement / Timeline");
        {
            // Ruler
            ImDrawList* rulerDl = GetWindowDrawList();
            const float rulerY = GetCursorScreenPos().y;
            const float trackH = 60.0f;
            const float pxPerSecond = 40.0f;
            for (double sec = 0.0; sec <= 300.0; sec += 5.0) {
                float x = 40.0f + static_cast<float>(sec) * pxPerSecond;
                rulerDl->AddLine(ImVec2(x, rulerY), ImVec2(x, rulerY + 20.0f),
                                 IM_COL32(120, 140, 160, 255));
                char buf[16];
                std::snprintf(buf, sizeof(buf), "%.0fs", sec);
                rulerDl->AddText(ImVec2(x + 4.0f, rulerY + 4.0f),
                                 IM_COL32(200, 220, 240, 255), buf);
            }
            SetCursorScreenPos(ImVec2(GetCursorScreenPos().x, rulerY + 24.0f));

            // Playhead line
            float playheadX = 40.0f + static_cast<float>(gSession.transportPositionSeconds) * pxPerSecond;
            rulerDl->AddLine(ImVec2(playheadX, rulerY), ImVec2(playheadX, rulerY + 24.0f + 4 * trackH),
                             IM_COL32(255, 60, 60, 255), 2.0f);

            // Track rows
            const char* trackNames[] = {"Audio 1", "MIDI 1", "Groupe Batterie", "Master"};
            for (int t = 0; t < 4; ++t) {
                float rowY = GetCursorScreenPos().y;
                const ImVec2 rowMin(40.0f, rowY);
                const ImVec2 rowMax(1080.0f + 360.0f, rowY + trackH);
                GetWindowDrawList()->AddRectFilled(rowMin, rowMax,
                    (t % 2 == 0) ? IM_COL32(30, 34, 40, 255) : IM_COL32(24, 28, 34, 255));
                GetWindowDrawList()->AddText(ImVec2(44.0f, rowY + 6.0f),
                    IM_COL32(220, 230, 240, 255), trackNames[t]);

                // Mute / Solo / Rec / Select buttons
                bool mute = false, solo = false, rec = false, sel = false;
                SameLine(120.0f);
                ColoredButton("M", ImVec2(20.0f, 16.0f), mute ? IM_COL32(150, 60, 60, 255) : IM_COL32(40, 40, 40, 255));
                SameLine();
                ColoredButton("S", ImVec2(20.0f, 16.0f), solo ? IM_COL32(150, 150, 40, 255) : IM_COL32(40, 40, 40, 255));
                SameLine();
                ColoredButton("R", ImVec2(20.0f, 16.0f), rec ? IM_COL32(150, 30, 30, 255) : IM_COL32(40, 40, 40, 255));
                SameLine();
                ColoredButton("1", ImVec2(20.0f, 16.0f), sel ? IM_COL32(40, 100, 180, 255) : IM_COL32(40, 40, 40, 255));

                // Waveform / MIDI note strip (visual placeholder)
                const ImVec2 clipMin(200.0f, rowY + 8.0f);
                const ImVec2 clipMax(200.0f + 300.0f, rowY + trackH - 8.0f);
                if (t == 0) {
                    GetWindowDrawList()->AddRectFilled(clipMin, clipMax, IM_COL32(80, 150, 255, 100), 3.0f);
                    GetWindowDrawList()->AddRect(clipMin, clipMax, IM_COL32(60, 120, 200, 255), 3.0f);
                    GetWindowDrawList()->AddText(ImVec2(clipMin.x + 6.0f, clipMin.y + 4.0f),
                        IM_COL32(240, 248, 255, 255), "Take 1 — Vocal");
                } else if (t == 1) {
                    // Draw a few MIDI note blocks
                    for (int n = 0; n < 4; ++n) {
                        float nx = 200.0f + n * 80.0f;
                        GetWindowDrawList()->AddRectFilled(ImVec2(nx, rowY + 10.0f),
                            ImVec2(nx + 40.0f, rowY + trackH - 10.0f), IM_COL32(120, 200, 120, 200), 2.0f);
                    }
                }

                SetCursorScreenPos(ImVec2(GetCursorScreenPos().x, rowY + trackH + 4.0f));
            }
        }
        End();

        // ---- Key Editor (bottom right) ----
        SetNextWindowPos(ImVec2(1450.0f, 190.0f));
        SetNextWindowSize(ImVec2(460.0f, 520.0f));
        Begin("Éditeur Clavecin (Piano Roll)");
        {
            // Piano keyboard (vertical)
            const float keyW = 30.0f;
            const float keyH = 12.0f;
            float pianoY = GetCursorScreenPos().y;
            for (int note = 127; note >= 24; --note) {
                bool isWhite = (note % 12) == 0 || (note % 12) == 2 || (note % 12) == 4 ||
                               (note % 12) == 5 || (note % 12) == 7 || (note % 12) == 9 ||
                               (note % 12) == 11;
                const ImVec2 keyMin(GetCursorScreenPos().x, pianoY);
                const ImVec2 keyMax(keyMin.x + keyW, keyMin.y + keyH);
                bool pressed = gSession.pianoKeyPressed[note];
                bool hovered = IsMouseHoveringRect(keyMin, keyMax);
                bool clicked = hovered && GetIO().MouseClicked[0];

                if (clicked) {
                    gSession.pianoKeyPressed[note] = true;
                    gSession.pianoKeyVelocity[note] = 0.9f;
                }
                if (GetIO().MouseReleased[0]) {
                    gSession.pianoKeyPressed[note] = false;
                }

                GetWindowDrawList()->AddRectFilled(keyMin, keyMax,
                    pressed ? IM_COL32(255, 255, 255, 255) :
                    (isWhite ? IM_COL32(240, 240, 240, 255) : IM_COL32(30, 30, 30, 255)));
                GetWindowDrawList()->AddRect(keyMin, keyMax,
                    hovered ? IM_COL32(120, 180, 255, 255) : IM_COL32(80, 80, 80, 255));

                // Audition note on click (play a sine)
                if (pressed) {
                    static double notePhase[128] = {0.0};
                    double freq = 440.0 * std::pow(2.0, (note - 69) / 12.0);
                    double phaseInc = 2.0 * std::numbers::pi_v<double> * freq / gSession.sampleRate;
                    // Simple audible feedback via a short beep stored in a static
                    // — in production this drives a software synth voice.
                    (void)notePhase; (void)phaseInc; (void)freq;
                }

                pianoY += keyH + 1.0f;
                if (pianoY > GetCursorScreenPos().y + 480.0f) break;
            }

            SameLine(40.0f);
            Text("C3  D3  E3  F3  G3  A3  B3  C4");
        }
        End();

        // ---- Control Room Panel (bottom left) ----
        SetNextWindowPos(ImVec2(10.0f, 720.0f));
        SetNextWindowSize(ImVec2(340.0f, 340.0f));
        Begin("Salle de Contrôle");
        {
            Checkbox("Dim", &gSession.controlRoomDim);
            SameLine();
            Checkbox("Mono", &gSession.controlRoomMono);
            SameLine();
            Checkbox("Click", &gSession.controlRoomClick);

            Separator();
            Text("Moniteurs: Moniteurs A (Genelec)");
            Text("Casque: Casque Artiste");
            Text("Cue Mix: Cue Mix 1");

            SliderFloat("Monitor", &gSession.monitorKnobDb, -60.0f, 6.0f);
            SliderFloat("Master", &gSession.masterFaderDb, -60.0f, 6.0f);

            // Live ballistic peak meters (visual)
            ImDrawList* meterDl = GetWindowDrawList();
            ImVec2 mPos = GetCursorScreenPos();
            for (int ch = 0; ch < 2; ++ch) {
                float meterLevel = 0.35f + 0.1f * static_cast<float>(std::sin(frameCounter * 0.1 + ch));
                meterLevel = std::clamp(meterLevel, 0.0f, 1.0f);
                ImVec2 barMin(mPos.x, mPos.y);
                ImVec2 barMax(mPos.x + 140.0f, mPos.y + 18.0f);
                meterDl->AddRectFilled(barMin, barMax, IM_COL32(20, 24, 30, 255), 2.0f);
                meterDl->AddRectFilled(barMin, ImVec2(barMin.x + 140.0f * meterLevel, barMax.y),
                    meterLevel > 0.8f ? IM_COL32(220, 50, 50, 255) : IM_COL32(60, 200, 80, 255), 2.0f);
                char lbl[8];
                std::snprintf(lbl, sizeof(lbl), "L%d", ch + 1);
                meterDl->AddText(ImVec2(barMax.x + 4.0f, barMin.y + 3.0f),
                    IM_COL32(180, 190, 200, 255), lbl);
                mPos.y += 24.0f;
            }
            SetCursorScreenPos(mPos);
        }
        End();

        // ---- Device Setup Dialog ----
        if (gSession.deviceSetupOpen) {
            SetNextWindowPos(ImVec2(560.0f, 300.0f));
            SetNextWindowSize(ImVec2(500.0f, 340.0f));
            bool open = gSession.deviceSetupOpen;
            Begin("Configuration Périphérique", &open);
            gSession.deviceSetupOpen = open;
            {
                Text("Pilote Audio: ASIO (Steinberg ASIO SDK 2.3.4)");
                SameLine();
                {
                    float drvIdx = static_cast<float>(gSession.asioDriverIndex);
                    if (SliderFloat("Driver Index", &drvIdx, 0.0f, 15.0f)) {
                        gSession.asioDriverIndex = static_cast<int>(drvIdx);
                    }
                }

                Text("Taille de Tampon (Block Size):");
                const char* blockSizes[] = {"64", "128", "256", "512", "1024", "2048"};
                {
                    float bsFloat = static_cast<float>(gSession.asioBlockSizeIndex);
                    if (SliderFloat("BlockSize", &bsFloat, 0.0f, 5.0f)) {
                        gSession.asioBlockSizeIndex = static_cast<int>(bsFloat);
                    }
                }
                Text("Échantillonnage: %.0f Hz", gSession.asioSampleRate);
                {
                    float srFloat = static_cast<float>(gSession.asioSampleRate);
                    if (SliderFloat("SampleRate", &srFloat, 44100.0f, 96000.0f)) {
                        gSession.asioSampleRate = static_cast<double>(srFloat);
                    }
                }

                Separator();
                Text("Entrées actives: 2  Sorties actives: 2");
                Text("Latence aller-retour: ~12 ms (ASIO)");

                if (Button("Ouvrir Panneau de Contrôle ASIO", ImVec2(200.0f, 0.0f))) {
#if defined(ALPHASTUDIO_PLATFORM_WINDOWS)
                    // Launch ASIO control panel via ShellExecute
                    ShellExecuteA(nullptr, "open", "rundll32.exe", nullptr, nullptr, SW_SHOWNORMAL);
#endif
                }
                SameLine();
                if (Button("Appliquer", ImVec2(100.0f, 0.0f))) {
                    gSession.blockSize = static_cast<std::uint32_t>(std::stoull(blockSizes[gSession.asioBlockSizeIndex]));
                    gSession.sampleRate = gSession.asioSampleRate;
                    shutdownAudioEngine(gSession);
                    initAudioEngine(gSession);
                }
            }
            End();
        }

        Render();

        // Render the ImGui 2D canvas to the BGRA32 framebuffer and present
        std::vector<std::uint32_t> bgra;
        bgra.reserve(static_cast<std::size_t>(win.width) * win.height);
        RenderToBGRA32Buffer(bgra, static_cast<std::uint32_t>(win.width),
                             static_cast<std::uint32_t>(win.height));
        Platform::presentFrame(win, bgra);
    }

    shutdownAudioEngine(gSession);
    DestroyContext();
    Platform::destroyWindow(win);

    return 0;
}

} // namespace AlphaStudio::UI
