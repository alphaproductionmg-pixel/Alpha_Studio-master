#pragma once

#if defined(ALPHASTUDIO_HAS_EXTERNAL_IMGUI)
#include <imgui.h>
#else

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// ============================================================================
// Clean-Room Interactive Dear ImGui 2D Engine & Fast 60-FPS Framebuffer
// Pure C++20 (Zero Video Subsystem, Full Mouse/Keyboard Widget Interaction)
// ============================================================================
using ImU32 = std::uint32_t;

struct ImVec2 {
    float x{0.0f};
    float y{0.0f};
    constexpr ImVec2() noexcept = default;
    constexpr ImVec2(float px, float py) noexcept : x(px), y(py) {}
};

struct ImVec4 {
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
    float w{1.0f};
    constexpr ImVec4() noexcept = default;
    constexpr ImVec4(float r, float g, float b, float a) noexcept : x(r), y(g), z(b), w(a) {}
};

constexpr ImU32 IM_COL32(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a) noexcept {
    return (static_cast<ImU32>(a) << 24U) |
           (static_cast<ImU32>(b) << 16U) |
           (static_cast<ImU32>(g) << 8U)  |
           static_cast<ImU32>(r);
}

enum class DrawPrimitiveKind : std::uint8_t {
    Line          = 0,
    RectOutline   = 1,
    RectFilled    = 2,
    CircleFilled  = 3,
    TriangleFill  = 4,
    BezierCubic   = 5,
    TextLabel     = 6
};

struct DrawCommand2D {
    DrawPrimitiveKind kind{DrawPrimitiveKind::RectFilled};
    ImVec2 p1{}, p2{}, p3{}, p4{};
    ImU32 color{0xFFFFFFFFU};
    float thicknessOrRadius{1.0f};
    float rounding{0.0f};
    std::string text;
};

class ImDrawList {
public:
    void clear() noexcept { commands_.clear(); }

    void AddLine(ImVec2 p1, ImVec2 p2, ImU32 col, float thickness = 1.0f);
    void AddRect(ImVec2 pMin, ImVec2 pMax, ImU32 col, float rounding = 0.0f, int flags = 0, float thickness = 1.0f);
    void AddRectFilled(ImVec2 pMin, ImVec2 pMax, ImU32 col, float rounding = 0.0f, int flags = 0);
    void AddCircleFilled(ImVec2 center, float radius, ImU32 col, int numSegments = 16);
    void AddTriangleFilled(ImVec2 p1, ImVec2 p2, ImVec2 p3, ImU32 col);
    void AddBezierCubic(ImVec2 p1, ImVec2 p2, ImVec2 p3, ImVec2 p4, ImU32 col, float thickness, int numSegments = 24);
    void AddText(ImVec2 pos, ImU32 col, const char* textBegin);

    [[nodiscard]] const std::vector<DrawCommand2D>& commands() const noexcept { return commands_; }
    [[nodiscard]] std::size_t commandCount() const noexcept { return commands_.size(); }

private:
    std::vector<DrawCommand2D> commands_;
};

struct ImGuiIO {
    ImVec2 DisplaySize{1920.0f, 1080.0f};
    float DeltaTime{1.0f / 60.0f};
    ImVec2 MousePos{-100.0f, -100.0f};
    ImVec2 MouseDelta{0.0f, 0.0f};
    float MouseWheel{0.0f};
    bool MouseDown[5]{};
    bool MouseClicked[5]{};
    bool MouseReleased[5]{};
    bool KeysDown[256]{};
    bool KeyPressed[256]{};
};

namespace ImGui {

void CreateContext();
void DestroyContext();
ImGuiIO& GetIO() noexcept;

// Updates per-frame click/release edge states before NewFrame()
void UpdateInputEdges(ImVec2 mousePos, bool leftDown, bool rightDown, float wheel = 0.0f) noexcept;
void NotifyKeyEvent(std::uint8_t keyCode, bool isDown) noexcept;

void NewFrame();
void Render();

void SetNextWindowPos(ImVec2 pos) noexcept;
void SetNextWindowSize(ImVec2 size) noexcept;

bool Begin(const char* name, bool* pOpen = nullptr, int flags = 0);
void End();
bool BeginChild(const char* strId, ImVec2 size = ImVec2(0, 0), bool border = false);
void EndChild();

bool BeginMainMenuBar();
void EndMainMenuBar();
bool BeginMenu(const char* label, bool enabled = true);
void EndMenu();
bool MenuItem(const char* label, const char* shortcut = nullptr, bool selected = false, bool enabled = true);

bool BeginTabBar(const char* strId);
void EndTabBar();
bool BeginTabItem(const char* label);
void EndTabItem();

void Text(const char* fmt, ...);
void TextColored(ImVec4 col, const char* fmt, ...);
void Separator();
void SameLine(float offsetFromStartX = 0.0f, float spacing = -1.0f);

bool Button(const char* label, ImVec2 size = ImVec2(0, 0));
bool ColoredButton(const char* label, ImVec2 size, ImU32 bgCol, ImU32 borderCol = 0, ImU32 textCol = 0xFFFFFFFFU);
bool Checkbox(const char* label, bool* v);
bool SliderFloat(const char* label, float* v, float vMin, float vMax, const char* format = "%.2f");
bool VSliderFloat(const char* label, ImVec2 size, float* v, float vMin, float vMax, const char* format = "%.1f");
bool Selectable(const char* label, bool selected = false);

// Interactive Hit-Testing Helpers for Custom DAW Canvas Controls (Faders, Piano Roll, Ruler, Knobs)
bool IsMouseHoveringRect(ImVec2 rMin, ImVec2 rMax) noexcept;
bool IsMouseClickedInRect(ImVec2 rMin, ImVec2 rMax, int button = 0) noexcept;
bool DragBehaviorRect(const char* idStr, ImVec2 rMin, ImVec2 rMax, bool* outHovered = nullptr) noexcept;

ImVec2 GetCursorScreenPos() noexcept;
void SetCursorScreenPos(ImVec2 pos) noexcept;
ImVec2 GetWindowPos() noexcept;
ImVec2 GetWindowSize() noexcept;
void Dummy(ImVec2 size) noexcept;
ImDrawList* GetWindowDrawList() noexcept;
ImDrawList* GetForegroundDrawList() noexcept;
std::size_t TotalSubmitted2DPrimitives() noexcept;

// Fast Real-Time 60-FPS 32-bit BGRA Pixel Rasterizer (for Win32 GDI StretchDIBits & X11 XImage)
void RenderToBGRA32Buffer(std::vector<std::uint32_t>& outBgraPixels,
                          std::uint32_t width,
                          std::uint32_t height);

// Lossless PNG Screenshot Exporter
bool SaveRenderedFrameToPNG(const std::string& outputPngPath,
                            std::uint32_t width = 1920,
                            std::uint32_t height = 1080);

} // namespace ImGui

#endif
