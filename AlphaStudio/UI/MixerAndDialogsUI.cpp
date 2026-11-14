#include "AlphaStudio/UI/MixerAndDialogsUI.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace AlphaStudio::UI {

MixerAndDialogsUI::MixerAndDialogsUI() {
    keyBindings_ = {
        {"Transport", "Start/Stop", "Space"},
        {"Transport", "Record", "Num *"},
        {"Edit", "Undo", "Ctrl+Z"},
        {"Edit", "Redo", "Ctrl+Shift+Z"},
        {"Devices", "Mixer", "F3"},
        {"Devices", "VST Connections", "F4"},
        {"Media", "Open MediaBay", "F5"},
        {"Audio", "Open Sample Editor / VariAudio", "Enter"}
    };
    macros_.push_back(UserMacroDefinition{
        .macroName = "Prepare Vocal Stem & Normalize",
        .stepCommandNames = {"Edit: Split Range", "Audio: Bounce Selection", "Audio: Normalize -1.0 dB"}
    });
}

void MixerAndDialogsUI::renderMixerWindow(Core::VSTMixer& mixer) {
    // Lower-Right Docked VST Mixer Console (Extended View, x: 1040..1578, y: 588..1080)
    ImGui::SetNextWindowPos(ImVec2(1040.0f, 588.0f));
    ImGui::SetNextWindowSize(ImVec2(538.0f, 492.0f));
    ImGui::Begin("VST Mixer - Extended Console (3 Views + Gain-Inverse EQ)");
    ImDrawList* dl = ImGui::GetWindowDrawList();
    (void)mixer;

    // Console View Selector Pills
    ImGui::SetCursorScreenPos(ImVec2(1048.0f, 615.0f));
    ImGui::ColoredButton("VIEW 1: ALL", ImVec2(82.0f, 18.0f), IM_COL32(52, 108, 174, 255), IM_COL32(106, 172, 248, 255));
    ImGui::SameLine();
    ImGui::ColoredButton("VIEW 2: STEMS", ImVec2(88.0f, 18.0f), IM_COL32(38, 44, 54, 255));
    ImGui::SameLine();
    ImGui::ColoredButton("VIEW 3: SURROUND", ImVec2(106.0f, 18.0f), IM_COL32(38, 44, 54, 255));
    ImGui::SameLine();
    ImGui::ColoredButton("EXTENDED EQ", ImVec2(86.0f, 18.0f), IM_COL32(34, 96, 78, 255));

    struct ConsoleStripVis {
        const char* name;
        const char* insertTag;
        float faderNorm;
        float meterL;
        float meterR;
        float eqPeak;
        ImU32 stripCol;
    };
    const std::array<ConsoleStripVis, 7> strips = {{
        {"Mic In1", "HPF 80Hz", 0.78f, 0.62f, 0.60f, -0.2f, IM_COL32(198, 72, 72, 255)},
        {"LeadVox", "Comp+EQ",  0.82f, 0.84f, 0.81f,  0.6f, IM_COL32(62, 152, 232, 255)},
        {"GA Drums","VSTDyn",   0.76f, 0.88f, 0.86f,  0.4f, IM_COL32(228, 148, 54, 255)},
        {"Strings", "REVernc",  0.68f, 0.58f, 0.63f,  0.3f, IM_COL32(168, 98, 235, 255)},
        {"MonoBas", "DaTube",   0.74f, 0.76f, 0.76f, -0.4f, IM_COL32(56, 192, 142, 255)},
        {"Pad 5.1", "Mix6to2",  0.64f, 0.54f, 0.57f,  0.5f, IM_COL32(68, 182, 212, 255)},
        {"MASTER",  "UV22 HR",  0.85f, 0.91f, 0.89f,  0.2f, IM_COL32(226, 68, 78, 255)}
    }};

    float sx = 1048.0f;
    const float stripW = 72.0f;
    const float stripTop = 639.0f;
    const float stripBot = 1072.0f;

    for (std::size_t i = 0; i < strips.size(); ++i) {
        const auto& st = strips[i];
        const ImVec2 sMin(sx, stripTop);
        const ImVec2 sMax(sx + stripW, stripBot);

        dl->AddRectFilled(sMin, sMax, (i == 6) ? IM_COL32(34, 30, 36, 255) : IM_COL32(25, 29, 36, 255), 2.0f);
        dl->AddRect(sMin, sMax, IM_COL32(54, 62, 74, 255), 2.0f);

        // Extended Upper Rack: Insert Slot Pill + Mini 2D Parametric EQ Curve
        dl->AddRectFilled(ImVec2(sMin.x + 3.0f, sMin.y + 3.0f), ImVec2(sMax.x - 3.0f, sMin.y + 18.0f),
                          IM_COL32(36, 58, 86, 255), 2.0f);
        dl->AddText(ImVec2(sMin.x + 6.0f, sMin.y + 7.0f), IM_COL32(215, 235, 255, 255), st.insertTag);

        const ImVec2 eqMin(sMin.x + 4.0f, sMin.y + 22.0f);
        const ImVec2 eqMax(sMax.x - 4.0f, sMin.y + 68.0f);
        dl->AddRectFilled(eqMin, eqMax, IM_COL32(13, 16, 22, 255), 2.0f);
        const float eqMid = 0.5f * (eqMin.y + eqMax.y);
        dl->AddLine(ImVec2(eqMin.x, eqMid), ImVec2(eqMax.x, eqMid), IM_COL32(38, 46, 58, 255));
        ImVec2 prevPt(eqMin.x + 2.0f, eqMid);
        for (int k = 1; k <= 14; ++k) {
            const float t = static_cast<float>(k) / 14.0f;
            const float dy = st.eqPeak * std::sin(t * 3.14159f) * 16.0f;
            const ImVec2 pt(eqMin.x + 2.0f + t * (eqMax.x - eqMin.x - 4.0f), eqMid - dy);
            dl->AddLine(prevPt, pt, IM_COL32(84, 212, 255, 255), 1.5f);
            prevPt = pt;
        }

        // Mute / Solo / PFL Badges
        dl->AddRectFilled(ImVec2(sMin.x + 4.0f, sMin.y + 74.0f), ImVec2(sMin.x + 24.0f, sMin.y + 89.0f), IM_COL32(52, 56, 66, 255), 2.0f);
        dl->AddText(ImVec2(sMin.x + 11.0f, sMin.y + 78.0f), IM_COL32(225, 225, 230, 255), "M");
        dl->AddRectFilled(ImVec2(sMin.x + 27.0f, sMin.y + 74.0f), ImVec2(sMin.x + 47.0f, sMin.y + 89.0f), IM_COL32(52, 56, 66, 255), 2.0f);
        dl->AddText(ImVec2(sMin.x + 34.0f, sMin.y + 78.0f), IM_COL32(225, 225, 230, 255), "S");
        dl->AddRectFilled(ImVec2(sMin.x + 50.0f, sMin.y + 74.0f), ImVec2(sMax.x - 4.0f, sMin.y + 89.0f), IM_COL32(42, 78, 118, 255), 2.0f);
        dl->AddText(ImVec2(sMin.x + 55.0f, sMin.y + 78.0f), IM_COL32(198, 232, 255, 255), "L");

        // Pan Bar
        dl->AddRectFilled(ImVec2(sMin.x + 6.0f, sMin.y + 94.0f), ImVec2(sMax.x - 6.0f, sMin.y + 102.0f), IM_COL32(15, 18, 24, 255));
        dl->AddRectFilled(ImVec2(sMin.x + 28.0f, sMin.y + 94.0f), ImVec2(sMin.x + 44.0f, sMin.y + 102.0f), IM_COL32(82, 186, 255, 255));

        // Fader Track + Brushed Aluminum Fader Cap
        const float fTop = sMin.y + 114.0f;
        const float fBot = sMax.y - 30.0f;
        const float fH = fBot - fTop;
        const float fTrackX = sMin.x + 20.0f;
        dl->AddLine(ImVec2(fTrackX, fTop), ImVec2(fTrackX, fBot), IM_COL32(12, 14, 18, 255), 4.0f);
        for (int tick = 0; tick <= 8; ++tick) {
            const float ty = fTop + static_cast<float>(tick) * (fH / 8.0f);
            dl->AddLine(ImVec2(fTrackX - 9.0f, ty), ImVec2(fTrackX - 4.0f, ty), IM_COL32(92, 102, 116, 255));
        }
        const float capY = fBot - st.faderNorm * fH;
        dl->AddRectFilled(ImVec2(fTrackX - 11.0f, capY - 12.0f), ImVec2(fTrackX + 11.0f, capY + 12.0f),
                          (i == 6) ? IM_COL32(215, 62, 68, 255) : IM_COL32(196, 204, 216, 255), 2.0f);
        dl->AddLine(ImVec2(fTrackX - 9.0f, capY), ImVec2(fTrackX + 9.0f, capY), IM_COL32(20, 24, 30, 255), 2.0f);

        // Dual Stereo Segmented Pre/Post Peak LED Meter (Green -> Amber -> Red)
        const float mLeftX = sMax.x - 24.0f;
        const float mRightX = sMax.x - 13.0f;
        dl->AddRectFilled(ImVec2(mLeftX - 1.0f, fTop), ImVec2(sMax.x - 4.0f, fBot), IM_COL32(12, 15, 20, 255));

        const int totalSegs = 28;
        for (int seg = 0; seg < totalSegs; ++seg) {
            const float normSeg = static_cast<float>(seg + 1) / static_cast<float>(totalSegs);
            const float sy1 = fBot - static_cast<float>(seg) * (fH / static_cast<float>(totalSegs));
            const float sy0 = sy1 - (fH / static_cast<float>(totalSegs)) + 1.5f;
            const ImU32 activeCol = (normSeg > 0.88f) ? IM_COL32(252, 72, 72, 255)
                                  : (normSeg > 0.70f) ? IM_COL32(252, 202, 68, 255)
                                                      : IM_COL32(68, 228, 126, 255);
            dl->AddRectFilled(ImVec2(mLeftX, sy0), ImVec2(mLeftX + 7.0f, sy1),
                              (normSeg <= st.meterL) ? activeCol : IM_COL32(24, 32, 28, 255));
            dl->AddRectFilled(ImVec2(mRightX, sy0), ImVec2(mRightX + 7.0f, sy1),
                              (normSeg <= st.meterR) ? activeCol : IM_COL32(24, 32, 28, 255));
        }

        // Bottom Colored Channel Label Strip
        dl->AddRectFilled(ImVec2(sMin.x + 2.0f, sMax.y - 22.0f), ImVec2(sMax.x - 2.0f, sMax.y - 2.0f), st.stripCol, 2.0f);
        dl->AddText(ImVec2(sMin.x + 6.0f, sMax.y - 15.0f), IM_COL32(14, 16, 22, 255), st.name);

        sx += stripW + 3.0f;
    }
    ImGui::End();
}

void MixerAndDialogsUI::renderControlRoomPanel(Core::ControlRoom& controlRoom) {
    // Far-Right Studio Control Room Dock (x: 1580..1920, y: 74..1080)
    ImGui::SetNextWindowPos(ImVec2(1580.0f, 74.0f));
    ImGui::SetNextWindowSize(ImVec2(340.0f, 1006.0f));
    ImGui::Begin("Control Room Mixer (Isolated Bus)");
    ImDrawList* dl = ImGui::GetWindowDrawList();
    (void)controlRoom;

    ImGui::ColoredButton("CONTROL ROOM ACTIVE", ImVec2(168.0f, 24.0f),
                         IM_COL32(34, 136, 86, 255), IM_COL32(95, 232, 152, 255));
    ImGui::SameLine();
    ImGui::ColoredButton("EXCL. PORTS ON", ImVec2(148.0f, 24.0f),
                         IM_COL32(42, 74, 118, 255), IM_COL32(92, 152, 228, 255));

    // Studio Talkback & Dim Buttons
    ImGui::ColoredButton("TALKBACK [MIC]", ImVec2(158.0f, 32.0f),
                         IM_COL32(186, 44, 52, 255), IM_COL32(252, 118, 124, 255));
    ImGui::SameLine();
    ImGui::ColoredButton("DIM (-20.0 dB)", ImVec2(158.0f, 32.0f),
                         IM_COL32(198, 138, 28, 255), IM_COL32(255, 212, 92, 255));

    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.55f, 0.85f, 1.0f, 1.0f), "MONITOR SPEAKER SETS & DOWNMIX");
    ImGui::Selectable("[A] Genelec 8351B Main (Stereo)   [ACTIVE]", true);
    ImGui::Selectable("[B] Yamaha NS-10M Nearfield       [STBY]", false);
    ImGui::Selectable("[C] Studio Surround 5.1 Array     [STBY]", false);
    ImGui::ColoredButton("5.1 Surround", ImVec2(102.0f, 20.0f), IM_COL32(38, 44, 56, 255));
    ImGui::SameLine();
    ImGui::ColoredButton("Downmix -> ST", ImVec2(106.0f, 20.0f), IM_COL32(52, 108, 174, 255));
    ImGui::SameLine();
    ImGui::ColoredButton("Mono Check", ImVec2(98.0f, 20.0f), IM_COL32(38, 44, 56, 255));

    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.55f, 0.85f, 1.0f, 1.0f), "STUDIO CUES & PHONES (INDEPENDENT)");
    float cue1 = 0.82f, cue2 = 0.68f, phones = 0.76f;
    ImGui::SliderFloat("Cue 1 (Vocal Booth)", &cue1, 0.0f, 1.0f);
    ImGui::SliderFloat("Cue 2 (Drummer Mix)", &cue2, 0.0f, 1.0f);
    ImGui::SliderFloat("Phones 1 (Engineer)", &phones, 0.0f, 1.0f);

    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.55f, 0.85f, 1.0f, 1.0f), "CONTROL ROOM INSERTS (6 PRE + 2 POST)");
    ImGui::Text("Pre 1 : Room Calibration EQ (Linear)");
    ImGui::Text("Pre 2 : Bass Management Crossover");
    ImGui::Text("Post 7: MultiScope Loudness Analyzer");
    ImGui::Text("Post 8: UV22 HR Monitor Dither (24b)");

    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.55f, 0.85f, 1.0f, 1.0f), "MASTER CONTROL ROOM METERS & LEVEL");

    // Large Precision Control Room Peak/RMS Meter + Big Monitor Volume Knob
    const ImVec2 crPos = ImGui::GetCursorScreenPos();
    const float mTop = crPos.y + 8.0f;
    const float mBot = 1058.0f;
    const float mH = mBot - mTop;
    dl->AddRectFilled(ImVec2(1594.0f, mTop), ImVec2(1706.0f, mBot), IM_COL32(13, 16, 21, 255), 3.0f);
    dl->AddRect(ImVec2(1594.0f, mTop), ImVec2(1706.0f, mBot), IM_COL32(52, 64, 80, 255), 3.0f);

    const float chLevels[6] = {0.86f, 0.84f, 0.72f, 0.45f, 0.64f, 0.62f};
    const char* chNames[6]  = {"L", "R", "C", "LFE", "Ls", "Rs"};
    for (int c = 0; c < 6; ++c) {
        const float bx = 1602.0f + static_cast<float>(c) * 16.5f;
        for (int s = 0; s < 36; ++s) {
            const float n = static_cast<float>(s + 1) / 36.0f;
            const float y1 = mBot - 18.0f - static_cast<float>(s) * ((mH - 26.0f) / 36.0f);
            const float y0 = y1 - ((mH - 26.0f) / 36.0f) + 1.5f;
            const ImU32 col = (n > 0.88f) ? IM_COL32(252, 72, 72, 255)
                            : (n > 0.72f) ? IM_COL32(252, 204, 68, 255)
                                          : IM_COL32(66, 228, 128, 255);
            dl->AddRectFilled(ImVec2(bx, y0), ImVec2(bx + 11.0f, y1),
                              (n <= chLevels[c]) ? col : IM_COL32(22, 30, 26, 255));
        }
        dl->AddText(ImVec2(bx + 1.0f, mBot - 12.0f), IM_COL32(196, 206, 220, 255), chNames[c]);
    }

    // Studio Big Rotary Monitor Level Knob
    const ImVec2 knobCenter(1814.0f, mTop + 115.0f);
    dl->AddCircleFilled(knobCenter, 62.0f, IM_COL32(22, 26, 33, 255));
    dl->AddCircleFilled(knobCenter, 52.0f, IM_COL32(52, 60, 74, 255));
    dl->AddCircleFilled(knobCenter, 42.0f, IM_COL32(34, 40, 50, 255));
    // Arc ticks around Big Knob
    for (int a = 0; a <= 16; ++a) {
        const float ang = (0.75f + (static_cast<float>(a) / 16.0f) * 1.5f) * 3.14159265f;
        const ImVec2 tPt(knobCenter.x + std::cos(ang) * 69.0f, knobCenter.y + std::sin(ang) * 69.0f);
        dl->AddCircleFilled(tPt, 2.2f, (a <= 12) ? IM_COL32(82, 208, 255, 255) : IM_COL32(62, 70, 82, 255));
    }
    const float ptrAng = (0.75f + 0.75f * 1.5f) * 3.14159265f;
    dl->AddLine(knobCenter, ImVec2(knobCenter.x + std::cos(ptrAng) * 38.0f,
                                   knobCenter.y + std::sin(ptrAng) * 38.0f),
                IM_COL32(110, 232, 255, 255), 3.0f);
    dl->AddText(ImVec2(1766.0f, knobCenter.y + 82.0f), IM_COL32(232, 240, 250, 255), "MONITOR: -6.0 dB");
    dl->AddText(ImVec2(1754.0f, knobCenter.y + 100.0f), IM_COL32(118, 232, 162, 255), "CLICK METRONOME: ON");
    dl->AddText(ImVec2(1748.0f, knobCenter.y + 118.0f), IM_COL32(168, 184, 204, 255), "LUFS INT: -14.2 LUFS");
    dl->AddText(ImVec2(1748.0f, knobCenter.y + 136.0f), IM_COL32(168, 184, 204, 255), "TRUE PEAK: -1.0 dBTP");
    ImGui::End();
}

void MixerAndDialogsUI::renderProjectAssistantDialog(
    Core::Persistence::MediaBayAndVSTSound& mediaBay) {
    if (!ImGui::Begin("Project Assistant")) { ImGui::End(); return; }
    for (const auto& item : mediaBay.queryByFilter("", 0)) {
        ImGui::Selectable(item.displayName.c_str(), false);
    }
    ImGui::End();
}

void MixerAndDialogsUI::renderDeviceSetupDialog(
    Core::VSTAudioSystem& audioSystem,
    Core::AudioThreadManager& threadMgr,
    Core::DiskIOThread& diskMgr,
    Core::MultiProcessingPool& mpPool) {
    (void)audioSystem; (void)threadMgr; (void)diskMgr; (void)mpPool;
}

void MixerAndDialogsUI::renderVSTConnectionsDialog(
    Core::VSTConnections& connections,
    const Core::ControlRoom& controlRoom) {
    (void)connections; (void)controlRoom;
}

void MixerAndDialogsUI::renderProjectBrowserDialog(
    const Core::Persistence::AlphaProjectDocument& project) {
    (void)project;
}

void MixerAndDialogsUI::renderKeyCommandsAndMacrosDialog(Core::UndoRedoManager& undoRedo) {
    (void)undoRedo;
}

} // namespace AlphaStudio::UI
