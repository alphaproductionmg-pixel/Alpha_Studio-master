#include "AlphaStudio/UI/ImGuiBackend2D.hpp"

#if !defined(ALPHASTUDIO_HAS_EXTERNAL_IMGUI)

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>

void ImDrawList::AddLine(ImVec2 p1, ImVec2 p2, ImU32 col, float thickness) {
    commands_.push_back({DrawPrimitiveKind::Line, p1, p2, {}, {}, col, thickness, 0.0f, {}});
}

void ImDrawList::AddRect(ImVec2 pMin, ImVec2 pMax, ImU32 col, float rounding, int /*flags*/, float thickness) {
    commands_.push_back({DrawPrimitiveKind::RectOutline, pMin, pMax, {}, {}, col, thickness, rounding, {}});
}

void ImDrawList::AddRectFilled(ImVec2 pMin, ImVec2 pMax, ImU32 col, float rounding, int /*flags*/) {
    commands_.push_back({DrawPrimitiveKind::RectFilled, pMin, pMax, {}, {}, col, 1.0f, rounding, {}});
}

void ImDrawList::AddCircleFilled(ImVec2 center, float radius, ImU32 col, int /*numSegments*/) {
    commands_.push_back({DrawPrimitiveKind::CircleFilled, center, {}, {}, {}, col, radius, 0.0f, {}});
}

void ImDrawList::AddTriangleFilled(ImVec2 p1, ImVec2 p2, ImVec2 p3, ImU32 col) {
    commands_.push_back({DrawPrimitiveKind::TriangleFill, p1, p2, p3, {}, col, 1.0f, 0.0f, {}});
}

void ImDrawList::AddBezierCubic(ImVec2 p1, ImVec2 p2, ImVec2 p3, ImVec2 p4,
                                ImU32 col, float thickness, int /*numSegments*/) {
    commands_.push_back({DrawPrimitiveKind::BezierCubic, p1, p2, p3, p4, col, thickness, 0.0f, {}});
}

void ImDrawList::AddText(ImVec2 pos, ImU32 col, const char* textBegin) {
    commands_.push_back({DrawPrimitiveKind::TextLabel, pos, {}, {}, {}, col, 1.0f, 0.0f,
                         textBegin ? std::string(textBegin) : std::string{}});
}

namespace ImGui {

namespace {

struct WindowFrameState {
    std::string title;
    ImVec2 pos{0.0f, 0.0f};
    ImVec2 size{640.0f, 480.0f};
    ImVec2 lineStart{8.0f, 32.0f};
    ImVec2 lastItemEnd{8.0f, 32.0f};
    float lastLineHeight{18.0f};
};

struct InternalContext {
    ImGuiIO io{};
    bool prevMouseDown[5]{};
    ImVec2 prevMousePos{-100.0f, -100.0f};

    ImDrawList drawList{};
    ImDrawList fgDrawList{};

    std::uint64_t activeWidgetId{0};
    std::uint64_t hotWidgetId{0};
    std::string openMenuName;
    ImVec2 openMenuPos{0.0f, 24.0f};
    float menuPopupY{26.0f};

    bool hasNextPos{false};
    bool hasNextSize{false};
    ImVec2 nextPos{16.0f, 32.0f};
    ImVec2 nextSize{640.0f, 360.0f};
    float autoStackY{32.0f};
    WindowFrameState win{};
    std::vector<WindowFrameState> childStack{};
    ImVec2 cursor{16.0f, 32.0f};
    float menuBarX{14.0f};
    float tabBarX{14.0f};
    std::size_t lastFramePrimitives{0};
};

InternalContext& ctx() noexcept {
    static InternalContext gCtx;
    return gCtx;
}

std::uint64_t hashWidgetId(const std::string& scope, const char* label) noexcept {
    std::uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : scope) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    h ^= 0xFFU;
    h *= 1099511628211ULL;
    if (label != nullptr) {
        for (const char* p = label; *p != '\0'; ++p) {
            h ^= static_cast<unsigned char>(*p);
            h *= 1099511628211ULL;
        }
    }
    return (h == 0ULL) ? 1ULL : h;
}

ImU32 lightenColor(ImU32 col, int delta) noexcept {
    const int r = std::clamp(static_cast<int>(col & 0xFFU) + delta, 0, 255);
    const int g = std::clamp(static_cast<int>((col >> 8U) & 0xFFU) + delta, 0, 255);
    const int b = std::clamp(static_cast<int>((col >> 16U) & 0xFFU) + delta, 0, 255);
    const int a = static_cast<int>((col >> 24U) & 0xFFU);
    return IM_COL32(static_cast<std::uint8_t>(r),
                    static_cast<std::uint8_t>(g),
                    static_cast<std::uint8_t>(b),
                    static_cast<std::uint8_t>(a));
}

// ============================================================================
// Crisp 5x7 Studio Pixel Font (ASCII 0x20..0x7E)
// ============================================================================
constexpr std::array<std::array<std::uint8_t, 5>, 95> kFont5x7 = {{
    {0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x5F,0x00,0x00}, {0x00,0x07,0x00,0x07,0x00},
    {0x14,0x7F,0x14,0x7F,0x14}, {0x24,0x2A,0x7F,0x2A,0x12}, {0x23,0x13,0x08,0x64,0x62},
    {0x36,0x49,0x55,0x22,0x50}, {0x00,0x05,0x03,0x00,0x00}, {0x00,0x1C,0x22,0x41,0x00},
    {0x00,0x41,0x22,0x1C,0x00}, {0x14,0x08,0x3E,0x08,0x14}, {0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00}, {0x08,0x08,0x08,0x08,0x08}, {0x00,0x60,0x60,0x00,0x00},
    {0x20,0x10,0x08,0x04,0x02}, {0x3E,0x51,0x49,0x45,0x3E}, {0x00,0x42,0x7F,0x40,0x00},
    {0x42,0x61,0x51,0x49,0x46}, {0x21,0x41,0x45,0x4B,0x31}, {0x18,0x14,0x12,0x7F,0x10},
    {0x27,0x45,0x45,0x45,0x39}, {0x3C,0x4A,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1E}, {0x00,0x36,0x36,0x00,0x00},
    {0x00,0x56,0x36,0x00,0x00}, {0x08,0x14,0x22,0x41,0x00}, {0x14,0x14,0x14,0x14,0x14},
    {0x00,0x41,0x22,0x14,0x08}, {0x02,0x01,0x51,0x09,0x06}, {0x32,0x49,0x79,0x41,0x3E},
    {0x7E,0x11,0x11,0x11,0x7E}, {0x7F,0x49,0x49,0x49,0x36}, {0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x22,0x1C}, {0x7F,0x49,0x49,0x49,0x41}, {0x7F,0x09,0x09,0x09,0x01},
    {0x3E,0x41,0x49,0x49,0x7A}, {0x7F,0x08,0x08,0x08,0x7F}, {0x00,0x41,0x7F,0x41,0x00},
    {0x20,0x40,0x41,0x3F,0x01}, {0x7F,0x08,0x14,0x22,0x41}, {0x7F,0x40,0x40,0x40,0x40},
    {0x7F,0x02,0x0C,0x02,0x7F}, {0x7F,0x04,0x08,0x10,0x7F}, {0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06}, {0x3E,0x41,0x51,0x21,0x5E}, {0x7F,0x09,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31}, {0x01,0x01,0x7F,0x01,0x01}, {0x3F,0x40,0x40,0x40,0x3F},
    {0x1F,0x20,0x40,0x20,0x1F}, {0x3F,0x40,0x38,0x40,0x3F}, {0x63,0x14,0x08,0x14,0x63},
    {0x07,0x08,0x70,0x08,0x07}, {0x61,0x51,0x49,0x45,0x43}, {0x00,0x7F,0x41,0x41,0x00},
    {0x02,0x04,0x08,0x10,0x20}, {0x00,0x41,0x41,0x7F,0x00}, {0x04,0x02,0x01,0x02,0x04},
    {0x40,0x40,0x40,0x40,0x40}, {0x00,0x01,0x02,0x04,0x00}, {0x20,0x54,0x54,0x54,0x78},
    {0x7F,0x48,0x44,0x44,0x38}, {0x38,0x44,0x44,0x44,0x20}, {0x38,0x44,0x44,0x48,0x7F},
    {0x38,0x54,0x54,0x54,0x18}, {0x08,0x7E,0x09,0x01,0x02}, {0x0C,0x52,0x52,0x52,0x3E},
    {0x7F,0x08,0x04,0x04,0x78}, {0x00,0x44,0x7D,0x40,0x00}, {0x20,0x40,0x44,0x3D,0x00},
    {0x7F,0x10,0x28,0x44,0x00}, {0x00,0x41,0x7F,0x40,0x00}, {0x7C,0x04,0x18,0x04,0x78},
    {0x7C,0x08,0x04,0x04,0x78}, {0x38,0x44,0x44,0x44,0x38}, {0x7C,0x14,0x14,0x14,0x08},
    {0x08,0x14,0x14,0x18,0x7C}, {0x7C,0x08,0x04,0x04,0x08}, {0x48,0x54,0x54,0x54,0x20},
    {0x04,0x3F,0x44,0x40,0x20}, {0x3C,0x40,0x40,0x20,0x7C}, {0x1C,0x20,0x40,0x20,0x1C},
    {0x3C,0x40,0x30,0x40,0x3C}, {0x44,0x28,0x10,0x28,0x44}, {0x0C,0x50,0x50,0x50,0x3C},
    {0x44,0x64,0x54,0x4C,0x44}, {0x00,0x08,0x36,0x41,0x00}, {0x00,0x00,0x7F,0x00,0x00},
    {0x00,0x41,0x36,0x08,0x00}, {0x10,0x08,0x08,0x10,0x08}
}};

// ============================================================================
// High-Speed 60-FPS 32-bit BGRA Rasterizer (Zero-Copy Win32 DIB / X11 Ready)
// ============================================================================
class FastCanvasBGRA {
public:
    FastCanvasBGRA(std::uint32_t* buf, std::uint32_t w, std::uint32_t h, ImU32 clearCol) noexcept
        : buf_(buf), w_(w), h_(h) {
        const std::uint32_t bgra = toBGRA(clearCol);
        std::fill(buf_, buf_ + static_cast<std::size_t>(w_) * h_, bgra);
    }

    static constexpr std::uint32_t toBGRA(ImU32 rgba) noexcept {
        const std::uint32_t r = rgba & 0xFFU;
        const std::uint32_t g = (rgba >> 8U) & 0xFFU;
        const std::uint32_t b = (rgba >> 16U) & 0xFFU;
        return 0xFF000000U | (r << 16U) | (g << 8U) | b;
    }

    inline void blendPx(int x, int y, ImU32 col, std::uint32_t alphaScale256 = 256U) noexcept {
        if (static_cast<unsigned>(x) >= w_ || static_cast<unsigned>(y) >= h_) return;
        const std::uint32_t a = (((col >> 24U) & 0xFFU) * alphaScale256) >> 8U;
        if (a == 0U) return;
        std::uint32_t& dst = buf_[static_cast<std::size_t>(y) * w_ + static_cast<std::size_t>(x)];
        if (a >= 254U) {
            dst = toBGRA(col);
            return;
        }
        const std::uint32_t sr = col & 0xFFU;
        const std::uint32_t sg = (col >> 8U) & 0xFFU;
        const std::uint32_t sb = (col >> 16U) & 0xFFU;
        const std::uint32_t dr = (dst >> 16U) & 0xFFU;
        const std::uint32_t dg = (dst >> 8U) & 0xFFU;
        const std::uint32_t db = dst & 0xFFU;
        const std::uint32_t inv = 256U - a;
        const std::uint32_t nr = (dr * inv + sr * a) >> 8U;
        const std::uint32_t ng = (dg * inv + sg * a) >> 8U;
        const std::uint32_t nb = (db * inv + sb * a) >> 8U;
        dst = 0xFF000000U | (nr << 16U) | (ng << 8U) | nb;
    }

    void fillRect(ImVec2 pMin, ImVec2 pMax, ImU32 col) noexcept {
        const int x0 = std::max(0, static_cast<int>(pMin.x));
        const int y0 = std::max(0, static_cast<int>(pMin.y));
        const int x1 = std::min(static_cast<int>(w_), static_cast<int>(pMax.x + 0.5f));
        const int y1 = std::min(static_cast<int>(h_), static_cast<int>(pMax.y + 0.5f));
        if (x0 >= x1 || y0 >= y1) return;

        const std::uint32_t a = (col >> 24U) & 0xFFU;
        if (a >= 254U) {
            const std::uint32_t bgra = toBGRA(col);
            for (int y = y0; y < y1; ++y) {
                std::uint32_t* row = buf_ + static_cast<std::size_t>(y) * w_;
                std::fill(row + x0, row + x1, bgra);
            }
        } else {
            for (int y = y0; y < y1; ++y) {
                for (int x = x0; x < x1; ++x) {
                    blendPx(x, y, col);
                }
            }
        }
    }

    void strokeRect(ImVec2 pMin, ImVec2 pMax, ImU32 col, float thickness) noexcept {
        const float t = std::max(1.0f, thickness);
        fillRect(pMin, ImVec2(pMax.x, pMin.y + t), col);
        fillRect(ImVec2(pMin.x, pMax.y - t), pMax, col);
        fillRect(ImVec2(pMin.x, pMin.y + t), ImVec2(pMin.x + t, pMax.y - t), col);
        fillRect(ImVec2(pMax.x - t, pMin.y + t), ImVec2(pMax.x, pMax.y - t), col);
    }

    void drawLine(ImVec2 p1, ImVec2 p2, ImU32 col, float thickness) noexcept {
        const float dx = p2.x - p1.x;
        const float dy = p2.y - p1.y;
        const float steps = std::max(std::abs(dx), std::abs(dy));
        if (steps < 0.5f) {
            blendPx(static_cast<int>(p1.x), static_cast<int>(p1.y), col);
            return;
        }
        if (std::abs(dx) < 0.5f && thickness <= 1.5f) {
            const int x = static_cast<int>(p1.x);
            const int y0 = std::max(0, static_cast<int>(std::min(p1.y, p2.y)));
            const int y1 = std::min(static_cast<int>(h_) - 1, static_cast<int>(std::max(p1.y, p2.y)));
            for (int y = y0; y <= y1; ++y) blendPx(x, y, col);
            return;
        }
        if (std::abs(dy) < 0.5f && thickness <= 1.5f) {
            const int y = static_cast<int>(p1.y);
            const int x0 = std::max(0, static_cast<int>(std::min(p1.x, p2.x)));
            const int x1 = std::min(static_cast<int>(w_) - 1, static_cast<int>(std::max(p1.x, p2.x)));
            for (int x = x0; x <= x1; ++x) blendPx(x, y, col);
            return;
        }
        const int nSteps = static_cast<int>(std::ceil(steps));
        const int rad = (thickness >= 2.2f) ? 1 : 0;
        for (int i = 0; i <= nSteps; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(nSteps);
            const int cx = static_cast<int>(p1.x + dx * t + 0.5f);
            const int cy = static_cast<int>(p1.y + dy * t + 0.5f);
            for (int oy = -rad; oy <= rad; ++oy) {
                for (int ox = -rad; ox <= rad; ++ox) {
                    blendPx(cx + ox, cy + oy, col);
                }
            }
        }
    }

    void fillCircle(ImVec2 c, float radius, ImU32 col) noexcept {
        const int x0 = std::max(0, static_cast<int>(c.x - radius - 1.0f));
        const int y0 = std::max(0, static_cast<int>(c.y - radius - 1.0f));
        const int x1 = std::min(static_cast<int>(w_) - 1, static_cast<int>(c.x + radius + 1.0f));
        const int y1 = std::min(static_cast<int>(h_) - 1, static_cast<int>(c.y + radius + 1.0f));
        const float r2 = radius * radius;
        for (int y = y0; y <= y1; ++y) {
            const float dy = (static_cast<float>(y) + 0.5f) - c.y;
            for (int x = x0; x <= x1; ++x) {
                const float dx = (static_cast<float>(x) + 0.5f) - c.x;
                if (dx * dx + dy * dy <= r2) {
                    blendPx(x, y, col);
                }
            }
        }
    }

    void fillTriangle(ImVec2 p1, ImVec2 p2, ImVec2 p3, ImU32 col) noexcept {
        const int x0 = std::max(0, static_cast<int>(std::min({p1.x, p2.x, p3.x})));
        const int y0 = std::max(0, static_cast<int>(std::min({p1.y, p2.y, p3.y})));
        const int x1 = std::min(static_cast<int>(w_) - 1, static_cast<int>(std::ceil(std::max({p1.x, p2.x, p3.x}))));
        const int y1 = std::min(static_cast<int>(h_) - 1, static_cast<int>(std::ceil(std::max({p1.y, p2.y, p3.y}))));
        auto edge = [](ImVec2 a, ImVec2 b, ImVec2 p) noexcept {
            return (p.x - a.x) * (b.y - a.y) - (p.y - a.y) * (b.x - a.x);
        };
        const float area = edge(p1, p2, p3);
        if (std::abs(area) < 0.001f) return;
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                const ImVec2 p(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f);
                if (edge(p2, p3, p) / area >= -0.02f &&
                    edge(p3, p1, p) / area >= -0.02f &&
                    edge(p1, p2, p) / area >= -0.02f) {
                    blendPx(x, y, col);
                }
            }
        }
    }

    void drawText(ImVec2 pos, ImU32 col, const std::string& text) noexcept {
        int penX = static_cast<int>(pos.x + 0.5f);
        const int penY = static_cast<int>(pos.y + 0.5f);
        for (unsigned char ch : text) {
            if (ch == '\n') continue;
            const std::size_t idx = (ch >= 32U && ch <= 126U) ? (ch - 32U) : ('?' - 32U);
            const auto& glyph = kFont5x7[idx];
            for (int gx = 0; gx < 5; ++gx) {
                const std::uint8_t colBits = glyph[static_cast<std::size_t>(gx)];
                if (colBits == 0U) continue;
                for (int gy = 0; gy < 7; ++gy) {
                    if ((colBits & (1U << gy)) != 0U) {
                        blendPx(penX + gx + 1, penY + gy + 1, IM_COL32(0, 0, 0, 140));
                        blendPx(penX + gx, penY + gy, col);
                    }
                }
            }
            penX += 6;
        }
    }

    void executeDrawList(const ImDrawList& list, float scaleX, float scaleY) noexcept {
        auto sc = [scaleX, scaleY](ImVec2 v) noexcept {
            return ImVec2(v.x * scaleX, v.y * scaleY);
        };
        const float scaleAvg = 0.5f * (scaleX + scaleY);
        for (const auto& cmd : list.commands()) {
            switch (cmd.kind) {
                case DrawPrimitiveKind::Line:
                    drawLine(sc(cmd.p1), sc(cmd.p2), cmd.color, cmd.thicknessOrRadius * scaleAvg);
                    break;
                case DrawPrimitiveKind::RectOutline:
                    strokeRect(sc(cmd.p1), sc(cmd.p2), cmd.color, cmd.thicknessOrRadius);
                    break;
                case DrawPrimitiveKind::RectFilled:
                    fillRect(sc(cmd.p1), sc(cmd.p2), cmd.color);
                    break;
                case DrawPrimitiveKind::CircleFilled:
                    fillCircle(sc(cmd.p1), cmd.thicknessOrRadius * scaleAvg, cmd.color);
                    break;
                case DrawPrimitiveKind::TriangleFill:
                    fillTriangle(sc(cmd.p1), sc(cmd.p2), sc(cmd.p3), cmd.color);
                    break;
                case DrawPrimitiveKind::BezierCubic: {
                    ImVec2 prev = sc(cmd.p1);
                    for (int s = 1; s <= 20; ++s) {
                        const float t = static_cast<float>(s) / 20.0f;
                        const float u = 1.0f - t;
                        const ImVec2 rawPt(
                            u*u*u*cmd.p1.x + 3*u*u*t*cmd.p2.x + 3*u*t*t*cmd.p3.x + t*t*t*cmd.p4.x,
                            u*u*u*cmd.p1.y + 3*u*u*t*cmd.p2.y + 3*u*t*t*cmd.p3.y + t*t*t*cmd.p4.y);
                        const ImVec2 pt = sc(rawPt);
                        drawLine(prev, pt, cmd.color, cmd.thicknessOrRadius * scaleAvg);
                        prev = pt;
                    }
                    break;
                }
                case DrawPrimitiveKind::TextLabel:
                    drawText(sc(cmd.p1), cmd.color, cmd.text);
                    break;
            }
        }
    }

private:
    std::uint32_t* buf_{nullptr};
    std::uint32_t w_{1920};
    std::uint32_t h_{1080};
};

// Pure C++20 PNG Encoder
std::uint32_t crc32Png(const std::uint8_t* data, std::size_t len) noexcept {
    std::uint32_t crc = 0xFFFFFFFFU;
    for (std::size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) {
            crc = (crc >> 1U) ^ ((crc & 1U) ? 0xEDB88320U : 0U);
        }
    }
    return ~crc;
}

void appendBE32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>((v >> 24U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((v >> 16U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((v >> 8U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>(v & 0xFFU));
}

void appendPngChunk(std::vector<std::uint8_t>& out, const char type[4],
                    const std::uint8_t* payload, std::size_t payloadLen) {
    appendBE32(out, static_cast<std::uint32_t>(payloadLen));
    const std::size_t typeOffset = out.size();
    out.insert(out.end(), reinterpret_cast<const std::uint8_t*>(type),
               reinterpret_cast<const std::uint8_t*>(type) + 4);
    if (payloadLen > 0) {
        out.insert(out.end(), payload, payload + payloadLen);
    }
    appendBE32(out, crc32Png(out.data() + typeOffset, 4U + payloadLen));
}

} // namespace

void CreateContext() {
    ctx().drawList.clear();
    ctx().fgDrawList.clear();
}

void DestroyContext() {
    ctx().drawList.clear();
    ctx().fgDrawList.clear();
}

ImGuiIO& GetIO() noexcept {
    return ctx().io;
}

void UpdateInputEdges(ImVec2 mousePos, bool leftDown, bool rightDown, float wheel) noexcept {
    auto& c = ctx();
    c.io.MouseDelta = ImVec2(mousePos.x - c.prevMousePos.x, mousePos.y - c.prevMousePos.y);
    c.prevMousePos = mousePos;
    c.io.MousePos = mousePos;
    c.io.MouseWheel = wheel;

    const bool cur[2] = {leftDown, rightDown};
    for (int b = 0; b < 2; ++b) {
        c.io.MouseClicked[b] = cur[b] && !c.prevMouseDown[b];
        c.io.MouseReleased[b] = !cur[b] && c.prevMouseDown[b];
        c.io.MouseDown[b] = cur[b];
        c.prevMouseDown[b] = cur[b];
    }
    if (c.io.MouseReleased[0]) {
        c.activeWidgetId = 0;
    }
}

void NotifyKeyEvent(std::uint8_t keyCode, bool isDown) noexcept {
    auto& c = ctx();
    if (isDown && !c.io.KeysDown[keyCode]) {
        c.io.KeyPressed[keyCode] = true;
    }
    c.io.KeysDown[keyCode] = isDown;
}

void NewFrame() {
    auto& c = ctx();
    c.drawList.clear();
    c.fgDrawList.clear();
    c.hotWidgetId = 0;
    c.autoStackY = 28.0f;
    c.hasNextPos = false;
    c.hasNextSize = false;
    c.menuBarX = 14.0f;
    c.childStack.clear();
}

void Render() {
    auto& c = ctx();
    c.lastFramePrimitives = c.drawList.commandCount() + c.fgDrawList.commandCount();
    std::fill(std::begin(c.io.KeyPressed), std::end(c.io.KeyPressed), false);
    c.io.MouseWheel = 0.0f;
}

bool IsMouseHoveringRect(ImVec2 rMin, ImVec2 rMax) noexcept {
    const ImVec2 m = ctx().io.MousePos;
    return (m.x >= rMin.x && m.x <= rMax.x && m.y >= rMin.y && m.y <= rMax.y);
}

bool IsMouseClickedInRect(ImVec2 rMin, ImVec2 rMax, int button) noexcept {
    return ctx().io.MouseClicked[button] && IsMouseHoveringRect(rMin, rMax);
}

bool DragBehaviorRect(const char* idStr, ImVec2 rMin, ImVec2 rMax, bool* outHovered) noexcept {
    auto& c = ctx();
    const std::uint64_t id = hashWidgetId(c.win.title, idStr);
    const bool hovered = IsMouseHoveringRect(rMin, rMax);
    if (outHovered != nullptr) *outHovered = hovered;
    if (hovered && c.io.MouseClicked[0] && c.activeWidgetId == 0) {
        c.activeWidgetId = id;
    }
    return (c.activeWidgetId == id && c.io.MouseDown[0]);
}

void SetNextWindowPos(ImVec2 pos) noexcept {
    ctx().nextPos = pos;
    ctx().hasNextPos = true;
}

void SetNextWindowSize(ImVec2 size) noexcept {
    ctx().nextSize = size;
    ctx().hasNextSize = true;
}

bool Begin(const char* name, bool* pOpen, int /*flags*/) {
    if (pOpen != nullptr && !(*pOpen)) {
        ctx().hasNextPos = false;
        ctx().hasNextSize = false;
        return false;
    }

    const ImVec2 wPos = ctx().hasNextPos ? ctx().nextPos : ImVec2(12.0f, ctx().autoStackY);
    const ImVec2 wSize = ctx().hasNextSize ? ctx().nextSize : ImVec2(640.0f, 320.0f);
    ctx().hasNextPos = false;
    ctx().hasNextSize = false;

    ctx().win.title = name ? name : "Win";
    ctx().win.pos = wPos;
    ctx().win.size = wSize;

    ctx().drawList.AddRectFilled(wPos, ImVec2(wPos.x + wSize.x, wPos.y + wSize.y),
                                 IM_COL32(28, 31, 37, 255));
    ctx().drawList.AddRectFilled(wPos, ImVec2(wPos.x + wSize.x, wPos.y + 22.0f),
                                 IM_COL32(41, 47, 57, 255));
    ctx().drawList.AddLine(ImVec2(wPos.x, wPos.y + 22.0f),
                           ImVec2(wPos.x + wSize.x, wPos.y + 22.0f),
                           IM_COL32(72, 138, 210, 220), 1.0f);
    ctx().drawList.AddRect(wPos, ImVec2(wPos.x + wSize.x, wPos.y + wSize.y),
                           IM_COL32(58, 65, 76, 255), 0.0f, 0, 1.0f);
    ctx().drawList.AddCircleFilled(ImVec2(wPos.x + 10.0f, wPos.y + 11.0f), 3.2f,
                                   IM_COL32(82, 175, 255, 255));
    ctx().drawList.AddText(ImVec2(wPos.x + 20.0f, wPos.y + 7.0f),
                           IM_COL32(236, 241, 248, 255), name);

    ctx().cursor = ImVec2(wPos.x + 8.0f, wPos.y + 28.0f);
    ctx().win.lineStart = ctx().cursor;
    ctx().win.lastItemEnd = ctx().cursor;
    ctx().tabBarX = ctx().cursor.x;
    return true;
}

void End() {
    ctx().autoStackY = std::max(ctx().autoStackY, ctx().win.pos.y + ctx().win.size.y + 6.0f);
}

bool BeginChild(const char* strId, ImVec2 size, bool border) {
    ctx().childStack.push_back(ctx().win);
    const ImVec2 cPos = ctx().cursor;
    const ImVec2 cSize(size.x > 0.0f ? size.x : (ctx().win.size.x - 16.0f),
                       size.y > 0.0f ? size.y : 120.0f);
    ctx().drawList.AddRectFilled(cPos, ImVec2(cPos.x + cSize.x, cPos.y + cSize.y),
                                 IM_COL32(22, 25, 31, 255), 3.0f);
    if (border) {
        ctx().drawList.AddRect(cPos, ImVec2(cPos.x + cSize.x, cPos.y + cSize.y),
                               IM_COL32(54, 62, 74, 255), 3.0f, 0, 1.0f);
    }
    ctx().win.title += std::string("/") + (strId ? strId : "child");
    ctx().win.pos = cPos;
    ctx().win.size = cSize;
    ctx().cursor = ImVec2(cPos.x + 6.0f, cPos.y + 6.0f);
    ctx().win.lineStart = ctx().cursor;
    return true;
}

void EndChild() {
    const ImVec2 childPos = ctx().win.pos;
    const ImVec2 childSize = ctx().win.size;
    if (!ctx().childStack.empty()) {
        ctx().win = ctx().childStack.back();
        ctx().childStack.pop_back();
    }
    ctx().cursor = ImVec2(ctx().win.pos.x + 8.0f, childPos.y + childSize.y + 6.0f);
    ctx().win.lineStart = ctx().cursor;
    ctx().win.lastItemEnd = ImVec2(childPos.x + childSize.x, childPos.y);
    ctx().win.lastLineHeight = childSize.y;
}

bool BeginMainMenuBar() {
    ctx().drawList.AddRectFilled(ImVec2(0.0f, 0.0f), ImVec2(ctx().io.DisplaySize.x, 24.0f),
                                 IM_COL32(19, 22, 27, 255));
    ctx().drawList.AddLine(ImVec2(0.0f, 24.0f), ImVec2(ctx().io.DisplaySize.x, 24.0f),
                           IM_COL32(52, 60, 72, 255), 1.0f);
    ctx().menuBarX = 12.0f;
    return true;
}

void EndMainMenuBar() {
    // Close open menu if user clicked outside menu bar and popup area
    if (!ctx().openMenuName.empty() && ctx().io.MouseClicked[0] && ctx().io.MousePos.y > 24.0f) {
        const ImVec2 pMin = ctx().openMenuPos;
        const ImVec2 pMax(pMin.x + 230.0f, ctx().menuPopupY + 6.0f);
        if (!IsMouseHoveringRect(pMin, pMax)) {
            ctx().openMenuName.clear();
        }
    }
}

bool BeginMenu(const char* label, bool enabled) {
    if (!enabled || label == nullptr) return false;
    const float w = static_cast<float>(std::strlen(label)) * 6.0f + 16.0f;
    const ImVec2 rMin(ctx().menuBarX - 4.0f, 1.0f);
    const ImVec2 rMax(ctx().menuBarX + w - 4.0f, 23.0f);
    const bool hovered = IsMouseHoveringRect(rMin, rMax);
    const bool isOpen = (ctx().openMenuName == label);

    if (hovered || isOpen) {
        ctx().drawList.AddRectFilled(rMin, rMax, isOpen ? IM_COL32(54, 104, 168, 255) : IM_COL32(38, 48, 62, 255));
    }
    if (hovered && ctx().io.MouseClicked[0]) {
        ctx().openMenuName = isOpen ? "" : std::string(label);
        ctx().openMenuPos = ImVec2(rMin.x, 24.0f);
    } else if (hovered && !ctx().openMenuName.empty() && !isOpen) {
        ctx().openMenuName = label;
        ctx().openMenuPos = ImVec2(rMin.x, 24.0f);
    }

    ctx().drawList.AddText(ImVec2(ctx().menuBarX, 8.0f), IM_COL32(228, 234, 242, 255), label);
    ctx().menuBarX += w;

    if (ctx().openMenuName == label) {
        ctx().menuPopupY = 28.0f;
        ctx().fgDrawList.AddRectFilled(ctx().openMenuPos,
                                       ImVec2(ctx().openMenuPos.x + 230.0f, ctx().openMenuPos.y + 168.0f),
                                       IM_COL32(26, 30, 38, 250));
        ctx().fgDrawList.AddRect(ctx().openMenuPos,
                                 ImVec2(ctx().openMenuPos.x + 230.0f, ctx().openMenuPos.y + 168.0f),
                                 IM_COL32(82, 148, 224, 255));
        return true;
    }
    return false;
}

void EndMenu() {}

bool MenuItem(const char* label, const char* shortcut, bool selected, bool enabled) {
    if (label == nullptr) return false;
    if (!ctx().openMenuName.empty()) {
        const ImVec2 rMin(ctx().openMenuPos.x + 2.0f, ctx().menuPopupY);
        const ImVec2 rMax(ctx().openMenuPos.x + 228.0f, ctx().menuPopupY + 18.0f);
        const bool hovered = enabled && IsMouseHoveringRect(rMin, rMax);
        if (hovered || selected) {
            ctx().fgDrawList.AddRectFilled(rMin, rMax, IM_COL32(52, 108, 176, 255));
        }
        ctx().fgDrawList.AddText(ImVec2(rMin.x + 8.0f, rMin.y + 5.0f),
                                 enabled ? IM_COL32(238, 242, 248, 255) : IM_COL32(120, 126, 136, 255), label);
        if (shortcut != nullptr) {
            const float sw = static_cast<float>(std::strlen(shortcut)) * 6.0f;
            ctx().fgDrawList.AddText(ImVec2(rMax.x - sw - 8.0f, rMin.y + 5.0f),
                                     IM_COL32(158, 188, 224, 255), shortcut);
        }
        ctx().menuPopupY += 19.0f;
        if (hovered && ctx().io.MouseClicked[0]) {
            ctx().openMenuName.clear();
            return true;
        }
        return false;
    }
    return false;
}

bool BeginTabBar(const char* /*strId*/) {
    ctx().tabBarX = ctx().cursor.x;
    return true;
}
void EndTabBar() {
    ctx().cursor.x = ctx().win.pos.x + 8.0f;
    ctx().cursor.y += 22.0f;
}
bool BeginTabItem(const char* label) {
    const float w = static_cast<float>(std::strlen(label)) * 6.0f + 16.0f;
    const ImVec2 rMin(ctx().tabBarX, ctx().cursor.y);
    const ImVec2 rMax(ctx().tabBarX + w, ctx().cursor.y + 18.0f);
    const bool hovered = IsMouseHoveringRect(rMin, rMax);
    ctx().drawList.AddRectFilled(rMin, rMax, hovered ? IM_COL32(66, 112, 168, 255) : IM_COL32(50, 88, 136, 255), 3.0f);
    ctx().drawList.AddText(ImVec2(ctx().tabBarX + 8.0f, ctx().cursor.y + 5.0f),
                           IM_COL32(245, 248, 255, 255), label);
    ctx().tabBarX += w + 4.0f;
    return true;
}
void EndTabItem() {}

void Text(const char* fmt, ...) {
    char buf[512]{};
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    ctx().drawList.AddText(ctx().cursor, IM_COL32(225, 230, 238, 255), buf);
    ctx().win.lastItemEnd = ImVec2(ctx().cursor.x + static_cast<float>(std::strlen(buf)) * 6.0f, ctx().cursor.y);
    ctx().win.lastLineHeight = 13.0f;
    ctx().cursor.x = ctx().win.pos.x + 8.0f;
    ctx().cursor.y += 13.0f;
}

void TextColored(ImVec4 col, const char* fmt, ...) {
    char buf[512]{};
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    const ImU32 c32 = IM_COL32(
        static_cast<std::uint8_t>(std::clamp(col.x, 0.0f, 1.0f) * 255.0f),
        static_cast<std::uint8_t>(std::clamp(col.y, 0.0f, 1.0f) * 255.0f),
        static_cast<std::uint8_t>(std::clamp(col.z, 0.0f, 1.0f) * 255.0f),
        static_cast<std::uint8_t>(std::clamp(col.w, 0.0f, 1.0f) * 255.0f));
    ctx().drawList.AddText(ctx().cursor, c32, buf);
    ctx().win.lastItemEnd = ImVec2(ctx().cursor.x + static_cast<float>(std::strlen(buf)) * 6.0f, ctx().cursor.y);
    ctx().win.lastLineHeight = 13.0f;
    ctx().cursor.x = ctx().win.pos.x + 8.0f;
    ctx().cursor.y += 13.0f;
}

void Separator() {
    ctx().drawList.AddLine(ImVec2(ctx().win.pos.x + 6.0f, ctx().cursor.y + 2.0f),
                           ImVec2(ctx().win.pos.x + ctx().win.size.x - 6.0f, ctx().cursor.y + 2.0f),
                           IM_COL32(55, 62, 74, 255));
    ctx().cursor.x = ctx().win.pos.x + 8.0f;
    ctx().cursor.y += 6.0f;
}

void SameLine(float offsetFromStartX, float spacing) {
    const float gap = (spacing >= 0.0f) ? spacing : 6.0f;
    if (offsetFromStartX > 0.0f) {
        ctx().cursor.x = ctx().win.pos.x + offsetFromStartX;
    } else {
        ctx().cursor.x = ctx().win.lastItemEnd.x + gap;
    }
    ctx().cursor.y = ctx().win.lastItemEnd.y;
}

bool ColoredButton(const char* label, ImVec2 size, ImU32 bgCol, ImU32 borderCol, ImU32 textCol) {
    const float textW = static_cast<float>(std::strlen(label)) * 6.0f;
    const float w = (size.x > 0.0f) ? size.x : (textW + 14.0f);
    const float h = (size.y > 0.0f) ? size.y : 19.0f;
    const ImVec2 rMin = ctx().cursor;
    const ImVec2 rMax(rMin.x + w, rMin.y + h);
    const bool hovered = ctx().openMenuName.empty() && IsMouseHoveringRect(rMin, rMax);
    const bool clicked = hovered && ctx().io.MouseClicked[0];

    const ImU32 drawBg = hovered ? lightenColor(bgCol, ctx().io.MouseDown[0] ? -18 : 26) : bgCol;
    ctx().drawList.AddRectFilled(rMin, rMax, drawBg, 3.0f);
    if (borderCol != 0U || hovered) {
        ctx().drawList.AddRect(rMin, rMax, hovered ? IM_COL32(140, 205, 255, 255) : borderCol, 3.0f);
    }
    const float tx = rMin.x + std::max(3.0f, 0.5f * (w - textW));
    const float ty = rMin.y + std::max(2.0f, 0.5f * (h - 7.0f));
    ctx().drawList.AddText(ImVec2(tx, ty), textCol, label);

    ctx().win.lastItemEnd = ImVec2(rMax.x, rMin.y);
    ctx().win.lastLineHeight = h + 4.0f;
    ctx().cursor.x = ctx().win.pos.x + 8.0f;
    ctx().cursor.y += h + 4.0f;
    return clicked;
}

bool Button(const char* label, ImVec2 size) {
    return ColoredButton(label, size, IM_COL32(48, 76, 114, 255), IM_COL32(78, 116, 168, 255),
                         IM_COL32(242, 246, 252, 255));
}

bool Checkbox(const char* label, bool* v) {
    const float totalW = 17.0f + static_cast<float>(std::strlen(label)) * 6.0f;
    const ImVec2 rMin = ctx().cursor;
    const ImVec2 rMax(rMin.x + totalW, rMin.y + 13.0f);
    const bool hovered = ctx().openMenuName.empty() && IsMouseHoveringRect(rMin, rMax);
    bool changed = false;
    if (hovered && ctx().io.MouseClicked[0] && v != nullptr) {
        *v = !(*v);
        changed = true;
    }

    const bool checked = (v != nullptr && *v);
    ctx().drawList.AddRectFilled(rMin, ImVec2(rMin.x + 12.0f, rMin.y + 12.0f),
                                 checked ? IM_COL32(38, 128, 86, 255) : IM_COL32(20, 23, 28, 255), 2.0f);
    ctx().drawList.AddRect(rMin, ImVec2(rMin.x + 12.0f, rMin.y + 12.0f),
                           hovered ? IM_COL32(150, 210, 255, 255) : IM_COL32(110, 122, 138, 255), 2.0f);
    if (checked) {
        ctx().drawList.AddRectFilled(ImVec2(rMin.x + 3.0f, rMin.y + 3.0f),
                                     ImVec2(rMin.x + 9.0f, rMin.y + 9.0f),
                                     IM_COL32(110, 245, 165, 255));
    }
    ctx().drawList.AddText(ImVec2(rMin.x + 17.0f, rMin.y + 3.0f),
                           IM_COL32(220, 225, 234, 255), label);

    ctx().win.lastItemEnd = ImVec2(rMax.x, rMin.y);
    ctx().win.lastLineHeight = 16.0f;
    ctx().cursor.x = ctx().win.pos.x + 8.0f;
    ctx().cursor.y += 16.0f;
    return changed;
}

bool SliderFloat(const char* label, float* v, float vMin, float vMax, const char* /*format*/) {
    const float barW = 110.0f;
    const ImVec2 rMin = ctx().cursor;
    const ImVec2 rMax(rMin.x + barW, rMin.y + 13.0f);
    bool hovered = false;
    bool changed = false;

    if (ctx().openMenuName.empty() && DragBehaviorRect(label, rMin, rMax, &hovered) && v != nullptr) {
        const float newT = std::clamp((ctx().io.MousePos.x - rMin.x) / barW, 0.0f, 1.0f);
        const float newVal = vMin + newT * (vMax - vMin);
        if (std::abs(newVal - *v) > 1e-5f) {
            *v = newVal;
            changed = true;
        }
    }

    const float val = (v != nullptr) ? *v : vMin;
    const float t = std::clamp((vMax > vMin) ? ((val - vMin) / (vMax - vMin)) : 0.0f, 0.0f, 1.0f);
    ctx().drawList.AddRectFilled(rMin, rMax, IM_COL32(18, 21, 26, 255), 2.0f);
    ctx().drawList.AddRectFilled(rMin, ImVec2(rMin.x + barW * t, rMax.y),
                                 hovered ? IM_COL32(92, 176, 255, 255) : IM_COL32(68, 146, 226, 255), 2.0f);
    ctx().drawList.AddRect(rMin, rMax, hovered ? IM_COL32(128, 192, 255, 255) : IM_COL32(64, 74, 88, 255), 2.0f);
    ctx().drawList.AddText(ImVec2(rMax.x + 6.0f, rMin.y + 3.0f),
                           IM_COL32(210, 216, 226, 255), label);

    const float totalW = barW + 6.0f + static_cast<float>(std::strlen(label)) * 6.0f;
    ctx().win.lastItemEnd = ImVec2(rMin.x + totalW, rMin.y);
    ctx().cursor.x = ctx().win.pos.x + 8.0f;
    ctx().cursor.y += 16.0f;
    return changed;
}

bool VSliderFloat(const char* label, ImVec2 size, float* v, float vMin, float vMax, const char* /*format*/) {
    const ImVec2 rMin = ctx().cursor;
    const ImVec2 rMax(rMin.x + size.x, rMin.y + size.y);
    bool hovered = false;
    bool changed = false;

    if (ctx().openMenuName.empty() && DragBehaviorRect(label, rMin, rMax, &hovered) && v != nullptr) {
        const float newT = std::clamp(1.0f - ((ctx().io.MousePos.y - rMin.y) / std::max(1.0f, size.y)), 0.0f, 1.0f);
        const float newVal = vMin + newT * (vMax - vMin);
        if (std::abs(newVal - *v) > 1e-5f) {
            *v = newVal;
            changed = true;
        }
    }

    const float val = (v != nullptr) ? *v : vMin;
    const float t = std::clamp((vMax > vMin) ? ((val - vMin) / (vMax - vMin)) : 0.0f, 0.0f, 1.0f);
    ctx().drawList.AddRectFilled(rMin, rMax, IM_COL32(20, 23, 28, 255), 2.0f);
    const float knobY = rMin.y + size.y * (1.0f - t);
    ctx().drawList.AddRectFilled(ImVec2(rMin.x - 1.0f, knobY - 5.0f),
                                 ImVec2(rMax.x + 1.0f, knobY + 5.0f),
                                 hovered ? IM_COL32(240, 246, 255, 255) : IM_COL32(205, 212, 224, 255), 2.0f);
    ctx().drawList.AddText(ImVec2(rMin.x, rMax.y + 3.0f), IM_COL32(190, 196, 206, 255), label);

    ctx().win.lastItemEnd = ImVec2(rMax.x, rMin.y);
    ctx().cursor.x = ctx().win.pos.x + 8.0f;
    ctx().cursor.y += size.y + 15.0f;
    return changed;
}

bool Selectable(const char* label, bool selected) {
    const float w = ctx().win.size.x - 16.0f;
    const ImVec2 rMin = ctx().cursor;
    const ImVec2 rMax(rMin.x + w, rMin.y + 14.0f);
    const bool hovered = ctx().openMenuName.empty() && IsMouseHoveringRect(rMin, rMax);
    const bool clicked = hovered && ctx().io.MouseClicked[0];

    if (selected || hovered) {
        ctx().drawList.AddRectFilled(rMin, rMax,
                                     selected ? IM_COL32(52, 102, 165, 220) : IM_COL32(38, 56, 82, 200), 2.0f);
    }
    ctx().drawList.AddText(ImVec2(rMin.x + 4.0f, rMin.y + 4.0f),
                           selected ? IM_COL32(250, 252, 255, 255) : IM_COL32(205, 212, 222, 255), label);
    ctx().cursor.x = ctx().win.pos.x + 8.0f;
    ctx().cursor.y += 15.0f;
    return clicked;
}

ImVec2 GetCursorScreenPos() noexcept { return ctx().cursor; }
void SetCursorScreenPos(ImVec2 pos) noexcept { ctx().cursor = pos; }
ImVec2 GetWindowPos() noexcept { return ctx().win.pos; }
ImVec2 GetWindowSize() noexcept { return ctx().win.size; }

void Dummy(ImVec2 size) noexcept {
    ctx().win.lastItemEnd = ImVec2(ctx().cursor.x + size.x, ctx().cursor.y);
    ctx().cursor.x = ctx().win.pos.x + 8.0f;
    ctx().cursor.y += size.y;
}

ImDrawList* GetWindowDrawList() noexcept { return &ctx().drawList; }
ImDrawList* GetForegroundDrawList() noexcept { return &ctx().fgDrawList; }
std::size_t TotalSubmitted2DPrimitives() noexcept { return ctx().lastFramePrimitives; }

void RenderToBGRA32Buffer(std::vector<std::uint32_t>& outBgraPixels,
                          std::uint32_t width,
                          std::uint32_t height) {
    if (width == 0 || height == 0) return;
    outBgraPixels.resize(static_cast<std::size_t>(width) * height);
    FastCanvasBGRA canvas(outBgraPixels.data(), width, height, IM_COL32(15, 17, 21, 255));
    const float sx = static_cast<float>(width) / std::max(1.0f, ctx().io.DisplaySize.x);
    const float sy = static_cast<float>(height) / std::max(1.0f, ctx().io.DisplaySize.y);
    canvas.executeDrawList(ctx().drawList, sx, sy);
    canvas.executeDrawList(ctx().fgDrawList, sx, sy);
}

bool SaveRenderedFrameToPNG(const std::string& outputPngPath,
                            std::uint32_t width,
                            std::uint32_t height) {
    std::vector<std::uint32_t> bgra;
    RenderToBGRA32Buffer(bgra, width, height);

    const std::size_t rowBytes = 1U + static_cast<std::size_t>(width) * 4U;
    std::vector<std::uint8_t> rawScanlines(rowBytes * height, 0);
    for (std::uint32_t y = 0; y < height; ++y) {
        rawScanlines[y * rowBytes] = 0;
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::uint32_t px = bgra[static_cast<std::size_t>(y) * width + x];
            const std::size_t dst = y * rowBytes + 1U + static_cast<std::size_t>(x) * 4U;
            rawScanlines[dst + 0] = static_cast<std::uint8_t>((px >> 16U) & 0xFFU);
            rawScanlines[dst + 1] = static_cast<std::uint8_t>((px >> 8U) & 0xFFU);
            rawScanlines[dst + 2] = static_cast<std::uint8_t>(px & 0xFFU);
            rawScanlines[dst + 3] = 255;
        }
    }

    std::vector<std::uint8_t> zlibData;
    zlibData.reserve(rawScanlines.size() + (rawScanlines.size() / 65535U + 2U) * 8U);
    zlibData.push_back(0x78);
    zlibData.push_back(0x01);

    std::uint32_t s1 = 1U, s2 = 0U;
    for (std::uint8_t b : rawScanlines) {
        s1 = (s1 + b) % 65521U;
        s2 = (s2 + s1) % 65521U;
    }

    std::size_t pos = 0;
    while (pos < rawScanlines.size()) {
        const std::size_t chunk = std::min<std::size_t>(65535U, rawScanlines.size() - pos);
        zlibData.push_back((pos + chunk == rawScanlines.size()) ? 0x01U : 0x00U);
        const auto len16 = static_cast<std::uint16_t>(chunk);
        const auto nlen16 = static_cast<std::uint16_t>(~len16);
        zlibData.push_back(static_cast<std::uint8_t>(len16 & 0xFFU));
        zlibData.push_back(static_cast<std::uint8_t>((len16 >> 8U) & 0xFFU));
        zlibData.push_back(static_cast<std::uint8_t>(nlen16 & 0xFFU));
        zlibData.push_back(static_cast<std::uint8_t>((nlen16 >> 8U) & 0xFFU));
        zlibData.insert(zlibData.end(), rawScanlines.begin() + static_cast<std::ptrdiff_t>(pos),
                        rawScanlines.begin() + static_cast<std::ptrdiff_t>(pos + chunk));
        pos += chunk;
    }
    appendBE32(zlibData, (s2 << 16U) | s1);

    std::vector<std::uint8_t> png;
    const std::array<std::uint8_t, 8> sig = {137, 80, 78, 71, 13, 10, 26, 10};
    png.insert(png.end(), sig.begin(), sig.end());
    std::vector<std::uint8_t> ihdr;
    appendBE32(ihdr, width);
    appendBE32(ihdr, height);
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});
    appendPngChunk(png, "IHDR", ihdr.data(), ihdr.size());
    appendPngChunk(png, "IDAT", zlibData.data(), zlibData.size());
    appendPngChunk(png, "IEND", nullptr, 0);

    std::ofstream out(outputPngPath, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;
    out.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    return out.good();
}

} // namespace ImGui

#endif
