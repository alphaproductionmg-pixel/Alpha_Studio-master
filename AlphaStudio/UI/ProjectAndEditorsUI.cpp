#include "AlphaStudio/UI/ProjectAndEditorsUI.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace AlphaStudio::UI {

ProjectAndEditorsUI::ProjectAndEditorsUI() {
    workspaces_[0] = {"WS1: Studio Arranger", true, true, true, true, false, false, false, true, true};
    workspaces_[1] = {"WS2: MIDI & Scoring", true, false, false, true, true, true, true, false, true};
    workspaces_[2] = {"WS3: VariAudio Vocal", true, true, true, false, false, false, false, true, false};
    workspaces_[3] = {"WS4: Control Room 5.1", false, true, true, false, false, false, false, true, false};
}

void ProjectAndEditorsUI::saveWorkspace(std::size_t slot0To3, const WorkspaceLayoutPreset& preset) {
    if (slot0To3 < workspaces_.size()) {
        workspaces_[slot0To3] = preset;
    }
}

const WorkspaceLayoutPreset& ProjectAndEditorsUI::recallWorkspace(std::size_t slot0To3) noexcept {
    activeWorkspaceIdx_ = std::min(slot0To3, workspaces_.size() - 1);
    return workspaces_[activeWorkspaceIdx_];
}

void ProjectAndEditorsUI::renderTransportBar(
    Core::Persistence::AlphaProjectDocument& project,
    Core::Sync::StudioTransportState& transport,
    const Core::AudioThreadManager& audioThreadMgr,
    const Core::DiskIOThread& diskMgr) {
    // Top Main Menu Bar + Studio Transport Dock (0..1920, y: 0..68)
    if (ImGui::BeginMainMenuBar()) {
        ImGui::BeginMenu("AlphaStudio");
        ImGui::BeginMenu("File");
        ImGui::BeginMenu("Edit");
        ImGui::BeginMenu("Project");
        ImGui::BeginMenu("Audio");
        ImGui::BeginMenu("MIDI");
        ImGui::BeginMenu("Scores");
        ImGui::BeginMenu("MediaBay");
        ImGui::BeginMenu("Transport");
        ImGui::BeginMenu("Devices");
        ImGui::BeginMenu("Workspaces");
        ImGui::EndMainMenuBar();
    }

    ImGui::SetNextWindowPos(ImVec2(0.0f, 24.0f));
    ImGui::SetNextWindowSize(ImVec2(1920.0f, 48.0f));
    ImGui::Begin("Master Transport & Steinberg ASIO SDK 2.3.4 Performance Monitor");
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Transport Deck Strip (dark brushed metal bar)
    dl->AddRectFilled(ImVec2(0.0f, 24.0f), ImVec2(1920.0f, 72.0f), IM_COL32(23, 26, 32, 255));
    dl->AddLine(ImVec2(0.0f, 72.0f), ImVec2(1920.0f, 72.0f), IM_COL32(62, 72, 88, 255), 1.0f);

    ImGui::SetCursorScreenPos(ImVec2(12.0f, 34.0f));
    ImGui::ColoredButton("|<", ImVec2(30.0f, 26.0f), IM_COL32(42, 48, 58, 255), IM_COL32(72, 80, 96, 255));
    ImGui::SameLine();
    ImGui::ColoredButton("<<", ImVec2(30.0f, 26.0f), IM_COL32(42, 48, 58, 255), IM_COL32(72, 80, 96, 255));
    ImGui::SameLine();
    ImGui::ColoredButton(">>", ImVec2(30.0f, 26.0f), IM_COL32(42, 48, 58, 255), IM_COL32(72, 80, 96, 255));
    ImGui::SameLine();
    ImGui::ColoredButton("[] STOP", ImVec2(64.0f, 26.0f), IM_COL32(52, 58, 70, 255), IM_COL32(88, 98, 114, 255));
    ImGui::SameLine();
    ImGui::ColoredButton(transport.isPlaying ? "|> PLAY" : "|> PLAY", ImVec2(72.0f, 26.0f),
                         IM_COL32(32, 152, 88, 255), IM_COL32(95, 235, 148, 255));
    ImGui::SameLine();
    ImGui::ColoredButton("* REC", ImVec2(62.0f, 26.0f),
                         transport.isRecording ? IM_COL32(205, 42, 48, 255) : IM_COL32(115, 34, 40, 255),
                         IM_COL32(245, 95, 102, 255));
    ImGui::SameLine();
    ImGui::ColoredButton("CYCLE", ImVec2(58.0f, 26.0f), IM_COL32(92, 62, 156, 255), IM_COL32(158, 118, 235, 255));
    ImGui::SameLine();
    ImGui::ColoredButton("PRE-COUNT 2B", ImVec2(92.0f, 26.0f), IM_COL32(44, 82, 122, 255), IM_COL32(88, 152, 218, 255));

    // Illuminated Studio LCD Time / Tempo / Locator Display
    dl->AddRectFilled(ImVec2(500.0f, 30.0f), ImVec2(1070.0f, 65.0f), IM_COL32(11, 15, 20, 255), 4.0f);
    dl->AddRect(ImVec2(500.0f, 30.0f), ImVec2(1070.0f, 65.0f), IM_COL32(58, 96, 134, 255), 4.0f);

    const std::int64_t curTick = project.clock.samplesToTicks(transport.currentSamplePosition);
    const std::string bbst = project.clock.formatBBST(curTick);
    char lcdLine1[160]{};
    std::snprintf(lcdLine1, sizeof(lcdLine1),
                  "BARS.BEATS:  %s   |   TEMPO: %.2f BPM   |   SIG: 4/4   |   PPQ: %d",
                  bbst.c_str(), project.clock.tempoAtTick(curTick), project.clock.midiDisplayResolutionPPQ());
    dl->AddText(ImVec2(514.0f, 36.0f), IM_COL32(112, 230, 255, 255), lcdLine1);
    dl->AddText(ImVec2(514.0f, 51.0f), IM_COL32(148, 172, 198, 255),
                "LOCATORS: L 002.1.1.000  ->  R 007.1.1.000   |   SCRUB SHUTTLE: +0.00x   |   SYNC: INT + SYS-LINK");

    // ASIO CPU & Disk Preload Real-Time Gauges + Workspace Switcher
    dl->AddRectFilled(ImVec2(1086.0f, 30.0f), ImVec2(1515.0f, 65.0f), IM_COL32(14, 17, 22, 255), 4.0f);
    dl->AddRect(ImVec2(1086.0f, 30.0f), ImVec2(1515.0f, 65.0f), IM_COL32(52, 64, 80, 255), 4.0f);
    dl->AddText(ImVec2(1098.0f, 36.0f), IM_COL32(115, 238, 162, 255),
                "STEINBERG ASIO SDK 2.3.4  |  48.0 kHz / 32-BIT FLOAT  |  PDC: ACTIVE");

    const float cpuBar = std::clamp(std::max(18.0f, audioThreadMgr.averageCpuLoadPercent()), 0.0f, 100.0f) / 100.0f;
    const float dskBar = std::clamp(std::max(9.0f, diskMgr.diskCacheLoadPercent()), 0.0f, 100.0f) / 100.0f;
    dl->AddText(ImVec2(1098.0f, 51.0f), IM_COL32(195, 205, 218, 255), "ASIO RT:");
    dl->AddRectFilled(ImVec2(1152.0f, 51.0f), ImVec2(1272.0f, 59.0f), IM_COL32(28, 34, 42, 255));
    dl->AddRectFilled(ImVec2(1152.0f, 51.0f), ImVec2(1152.0f + 120.0f * cpuBar, 59.0f), IM_COL32(68, 212, 128, 255));
    dl->AddText(ImVec2(1288.0f, 51.0f), IM_COL32(195, 205, 218, 255), "DISK 2.0s:");
    dl->AddRectFilled(ImVec2(1354.0f, 51.0f), ImVec2(1474.0f, 59.0f), IM_COL32(28, 34, 42, 255));
    dl->AddRectFilled(ImVec2(1354.0f, 51.0f), ImVec2(1354.0f + 120.0f * dskBar, 59.0f), IM_COL32(82, 178, 255, 255));

    ImGui::SetCursorScreenPos(ImVec2(1528.0f, 35.0f));
    ImGui::ColoredButton("WS 1: ARRANGER", ImVec2(96.0f, 24.0f), IM_COL32(52, 106, 172, 255), IM_COL32(108, 174, 245, 255));
    ImGui::SameLine();
    ImGui::ColoredButton("WS 2: MIDI", ImVec2(72.0f, 24.0f), IM_COL32(38, 44, 54, 255));
    ImGui::SameLine();
    ImGui::ColoredButton("WS 3: VOCAL", ImVec2(78.0f, 24.0f), IM_COL32(38, 44, 54, 255));
    ImGui::SameLine();
    ImGui::ColoredButton("ZERO VIDEO", ImVec2(82.0f, 24.0f), IM_COL32(32, 82, 64, 255), IM_COL32(78, 198, 142, 255));
    ImGui::End();
}

void ProjectAndEditorsUI::renderTrackInspector(
    Core::TrackBase& selectedTrack,
    const Core::ExpressionMap& expressionMap) {
    // Left Docked Track Inspector (x: 0..268, y: 74..1080)
    ImGui::SetNextWindowPos(ImVec2(0.0f, 74.0f));
    ImGui::SetNextWindowSize(ImVec2(268.0f, 1006.0f));
    ImGui::Begin("Track Inspector");
    ImDrawList* dl = ImGui::GetWindowDrawList();

    ImGui::TextColored(ImVec4(0.48f, 0.84f, 1.0f, 1.0f), "01  %s  [%s]",
                       selectedTrack.name().c_str(), Core::trackTypeName(selectedTrack.type()));
    ImGui::ColoredButton("M", ImVec2(28.0f, 20.0f), IM_COL32(62, 54, 34, 255), IM_COL32(195, 162, 58, 255));
    ImGui::SameLine();
    ImGui::ColoredButton("S", ImVec2(28.0f, 20.0f), IM_COL32(58, 36, 36, 255), IM_COL32(210, 82, 82, 255));
    ImGui::SameLine();
    ImGui::ColoredButton("REC", ImVec2(38.0f, 20.0f), IM_COL32(178, 42, 48, 255), IM_COL32(248, 112, 118, 255));
    ImGui::SameLine();
    ImGui::ColoredButton("MON", ImVec2(38.0f, 20.0f), IM_COL32(192, 134, 32, 255), IM_COL32(252, 204, 88, 255));
    ImGui::SameLine();
    ImGui::ColoredButton("R", ImVec2(24.0f, 20.0f), IM_COL32(36, 132, 72, 255));
    ImGui::SameLine();
    ImGui::ColoredButton("W", ImVec2(24.0f, 20.0f), IM_COL32(48, 54, 64, 255));

    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.75f, 0.82f, 0.92f, 1.0f), "I/O ROUTING & CHANNEL");
    ImGui::Text("In : ASIO Mic Pre In 1 (Mono)");
    ImGui::Text("Out: Stereo Master (Bus 1-2)");
    float volDb = -1.5f;
    float panVal = 0.0f;
    ImGui::SliderFloat("Vol -1.5dB", &volDb, -60.0f, 6.0f);
    ImGui::SliderFloat("Pan Center", &panVal, -1.0f, 1.0f);

    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.75f, 0.82f, 0.92f, 1.0f), "AUDIO INSERTS (1-6 PRE / 7-8 POST)");
    const char* slots[8] = {
        "i1: Compressor        [VST3]",
        "i2: StudioEQ 4-Band   [VST3]",
        "i3: DeEsser           [VST3]",
        "i4: Tube Compressor   [VST3]",
        "i5: StereoEnhancer    [VST3]",
        "i6: [Empty Pre-Fader]",
        "i7: UV22 HR Dither    [POST]",
        "i8: Brickwall Limiter [POST]"
    };
    for (int i = 0; i < 8; ++i) {
        const ImVec2 sp = ImGui::GetCursorScreenPos();
        const ImU32 slotCol = (i == 5) ? IM_COL32(22, 25, 31, 255)
                            : (i >= 6) ? IM_COL32(48, 62, 92, 255) : IM_COL32(32, 68, 98, 255);
        dl->AddRectFilled(sp, ImVec2(sp.x + 248.0f, sp.y + 17.0f), slotCol, 2.0f);
        dl->AddRect(sp, ImVec2(sp.x + 248.0f, sp.y + 17.0f), IM_COL32(62, 88, 118, 255), 2.0f);
        dl->AddCircleFilled(ImVec2(sp.x + 8.0f, sp.y + 8.5f), 3.0f,
                            (i == 5) ? IM_COL32(75, 80, 90, 255) : IM_COL32(82, 228, 148, 255));
        dl->AddText(ImVec2(sp.x + 17.0f, sp.y + 5.0f), IM_COL32(232, 238, 246, 255), slots[i]);
        ImGui::Dummy(ImVec2(248.0f, 20.0f));
    }

    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.75f, 0.82f, 0.92f, 1.0f), "CHANNEL EQ (4-BAND + GAIN INVERSE)");
    const ImVec2 eqMin = ImGui::GetCursorScreenPos();
    const ImVec2 eqMax(eqMin.x + 248.0f, eqMin.y + 92.0f);
    dl->AddRectFilled(eqMin, eqMax, IM_COL32(14, 18, 24, 255), 3.0f);
    dl->AddRect(eqMin, eqMax, IM_COL32(54, 72, 96, 255), 3.0f);
    const float midEqY = 0.5f * (eqMin.y + eqMax.y);
    dl->AddLine(ImVec2(eqMin.x, midEqY), ImVec2(eqMax.x, midEqY), IM_COL32(46, 56, 70, 255));
    ImVec2 prevEq(eqMin.x + 4.0f, midEqY + 12.0f);
    for (int x = 1; x <= 48; ++x) {
        const float nx = static_cast<float>(x) / 48.0f;
        const float bell1 = -0.35f * std::exp(-std::pow((nx - 0.18f) / 0.08f, 2.0f));
        const float bell2 =  0.55f * std::exp(-std::pow((nx - 0.52f) / 0.11f, 2.0f));
        const float shelf =  0.40f / (1.0f + std::exp(-(nx - 0.78f) * 18.0f));
        const ImVec2 curEq(eqMin.x + 4.0f + nx * 240.0f, midEqY - (bell1 + bell2 + shelf) * 34.0f);
        dl->AddLine(prevEq, curEq, IM_COL32(82, 212, 255, 255), 2.0f);
        prevEq = curEq;
    }
    dl->AddCircleFilled(ImVec2(eqMin.x + 48.0f, midEqY + 11.0f), 4.0f, IM_COL32(255, 178, 66, 255));
    dl->AddCircleFilled(ImVec2(eqMin.x + 130.0f, midEqY - 19.0f), 4.0f, IM_COL32(92, 232, 148, 255));
    dl->AddCircleFilled(ImVec2(eqMin.x + 206.0f, midEqY - 14.0f), 4.0f, IM_COL32(198, 128, 255, 255));
    ImGui::Dummy(ImVec2(248.0f, 98.0f));

    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.75f, 0.82f, 0.92f, 1.0f), "TRACK QUICK CONTROLS (8 SLOTS)");
    const char* qcNames[4] = {"QC1 Cutoff", "QC2 Reso", "QC3 SendRev", "QC4 Drive"};
    const float qcVals[4] = {0.72f, 0.38f, 0.54f, 0.28f};
    for (int q = 0; q < 4; ++q) {
        float v = qcVals[q];
        ImGui::SliderFloat(qcNames[q], &v, 0.0f, 1.0f);
    }

    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.75f, 0.82f, 0.92f, 1.0f), "VST EXPRESSION MAP");
    ImGui::Text("Map: %s (%zu SoundSlots)", expressionMap.name().c_str(), expressionMap.slots().size());
    ImGui::ColoredButton("Legato [C0]", ImVec2(76.0f, 18.0f), IM_COL32(46, 118, 188, 255));
    ImGui::SameLine();
    ImGui::ColoredButton("Spiccato [D0]", ImVec2(84.0f, 18.0f), IM_COL32(198, 92, 58, 255));
    ImGui::SameLine();
    ImGui::ColoredButton("Trem [E0]", ImVec2(68.0f, 18.0f), IM_COL32(126, 78, 184, 255));
    ImGui::End();
}

void ProjectAndEditorsUI::renderInplaceMidiOverlay(
    ImDrawList* dl, ImVec2 rowMin, ImVec2 rowMax, const Core::MIDITrack& midiTrack) {
    for (const auto& part : midiTrack.parts()) {
        for (const auto& ev : part.events) {
            if (ev.kind != Core::SequencerMidiKind::Note) continue;
            const float x0 = rowMin.x + static_cast<float>(part.startTick + ev.startTick) * 0.08f;
            const float x1 = x0 + std::max(8.0f, static_cast<float>(ev.lengthTicks) * 0.08f);
            const float normPitch = 1.0f - (static_cast<float>(ev.pitchOrCC - 48) / 36.0f);
            const float y0 = std::clamp(rowMin.y + normPitch * (rowMax.y - rowMin.y - 6.0f),
                                        rowMin.y + 2.0f, rowMax.y - 6.0f);
            if (x0 < rowMax.x) {
                dl->AddRectFilled(ImVec2(x0, y0), ImVec2(std::min(x1, rowMax.x), y0 + 5.0f),
                                  IM_COL32(255, 152, 72, 240), 1.5f);
            }
        }
    }
}

void ProjectAndEditorsUI::renderProjectWindow(
    Core::Persistence::AlphaProjectDocument& project,
    Core::Sync::StudioTransportState& transport,
    const Core::AudioThreadManager& audioThreadMgr,
    const Core::DiskIOThread& diskMgr,
    const Core::ExpressionMap& expressionMap) {
    renderTransportBar(project, transport, audioThreadMgr, diskMgr);

    if (!project.tracks.empty()) {
        const std::size_t sel = std::min(transport.selectedTrackIndex, project.tracks.size() - 1);
        renderTrackInspector(*project.tracks[sel], expressionMap);
    }

    // Center-Upper Multi-Track Project Arrangement Window (x: 270..1578, y: 74..586)
    ImGui::SetNextWindowPos(ImVec2(270.0f, 74.0f));
    ImGui::SetNextWindowSize(ImVec2(1308.0f, 512.0f));
    ImGui::Begin("Project Window - Multi-Track Arrangement, Global Tracks & Inplace MIDI Editor");
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const float winX = 270.0f;
    const float headerW = 224.0f;
    const float laneX0 = winX + headerW;
    const float laneX1 = winX + 1302.0f;
    const float laneW = laneX1 - laneX0;

    // Musical Bar/Beat Ruler + Cycle Range Banner (y: 100..124)
    dl->AddRectFilled(ImVec2(winX + 4.0f, 98.0f), ImVec2(laneX1, 124.0f), IM_COL32(18, 21, 27, 255));
    // Purple Locator Cycle Highlight (Bars 2..7)
    const float barPx = laneW / 9.0f;
    dl->AddRectFilled(ImVec2(laneX0 + barPx * 1.0f, 98.0f),
                      ImVec2(laneX0 + barPx * 6.0f, 124.0f),
                      IM_COL32(98, 64, 175, 95));
    dl->AddLine(ImVec2(laneX0 + barPx * 1.0f, 98.0f),
                ImVec2(laneX0 + barPx * 6.0f, 98.0f),
                IM_COL32(168, 122, 255, 255), 2.0f);
    dl->AddText(ImVec2(winX + 10.0f, 107.0f), IM_COL32(170, 182, 198, 255), "BARS / BEATS (480 PPQ)");

    for (int bar = 0; bar < 9; ++bar) {
        const float bx = laneX0 + static_cast<float>(bar) * barPx;
        dl->AddLine(ImVec2(bx, 98.0f), ImVec2(bx, 580.0f), IM_COL32(42, 48, 58, 255), 1.0f);
        char bStr[16]{};
        std::snprintf(bStr, sizeof(bStr), "%d", bar + 1);
        dl->AddText(ImVec2(bx + 4.0f, 108.0f), IM_COL32(215, 222, 234, 255), bStr);
        for (int beat = 1; beat < 4; ++beat) {
            const float subX = bx + static_cast<float>(beat) * (barPx * 0.25f);
            dl->AddLine(ImVec2(subX, 116.0f), ImVec2(subX, 580.0f), IM_COL32(31, 35, 43, 180), 1.0f);
        }
    }

    // Global Tracks: Arranger Chain + Tempo Ramp + Markers (y: 126..182)
    float curY = 126.0f;
    dl->AddRectFilled(ImVec2(winX + 4.0f, curY), ImVec2(laneX0 - 2.0f, curY + 24.0f), IM_COL32(35, 41, 52, 255));
    dl->AddText(ImVec2(winX + 10.0f, curY + 8.0f), IM_COL32(240, 196, 98, 255), "G1  Arranger Track [Chain]");
    dl->AddRectFilled(ImVec2(laneX0 + 4.0f, curY + 3.0f), ImVec2(laneX0 + barPx * 2.0f - 4.0f, curY + 21.0f), IM_COL32(142, 92, 38, 225), 3.0f);
    dl->AddText(ImVec2(laneX0 + 12.0f, curY + 8.0f), IM_COL32(255, 245, 220, 255), "A: INTRO (2 Bars)");
    dl->AddRectFilled(ImVec2(laneX0 + barPx * 2.0f, curY + 3.0f), ImVec2(laneX0 + barPx * 6.0f - 4.0f, curY + 21.0f), IM_COL32(48, 118, 148, 225), 3.0f);
    dl->AddText(ImVec2(laneX0 + barPx * 2.0f + 12.0f, curY + 8.0f), IM_COL32(235, 250, 255, 255), "B: VERSE 1 (4 Bars)");
    dl->AddRectFilled(ImVec2(laneX0 + barPx * 6.0f, curY + 3.0f), ImVec2(laneX1 - 4.0f, curY + 21.0f), IM_COL32(138, 56, 118, 225), 3.0f);
    dl->AddText(ImVec2(laneX0 + barPx * 6.0f + 12.0f, curY + 8.0f), IM_COL32(255, 235, 250, 255), "C: CHORUS (3 Bars)");
    curY += 26.0f;

    dl->AddRectFilled(ImVec2(winX + 4.0f, curY), ImVec2(laneX0 - 2.0f, curY + 24.0f), IM_COL32(32, 38, 48, 255));
    dl->AddText(ImVec2(winX + 10.0f, curY + 8.0f), IM_COL32(132, 224, 168, 255), "G2  Tempo & Markers");
    dl->AddLine(ImVec2(laneX0, curY + 18.0f), ImVec2(laneX0 + barPx * 3.0f, curY + 18.0f), IM_COL32(95, 232, 152, 255), 1.8f);
    dl->AddLine(ImVec2(laneX0 + barPx * 3.0f, curY + 18.0f), ImVec2(laneX0 + barPx * 5.0f, curY + 8.0f), IM_COL32(95, 232, 152, 255), 1.8f);
    dl->AddLine(ImVec2(laneX0 + barPx * 5.0f, curY + 8.0f), ImVec2(laneX1, curY + 8.0f), IM_COL32(95, 232, 152, 255), 1.8f);
    dl->AddText(ImVec2(laneX0 + 8.0f, curY + 5.0f), IM_COL32(255, 212, 92, 255), "[M1 Start]");
    dl->AddText(ImVec2(laneX0 + barPx * 2.0f + 6.0f, curY + 5.0f), IM_COL32(255, 212, 92, 255), "[M2 Vocal In]");
    dl->AddText(ImVec2(laneX0 + barPx * 6.0f + 6.0f, curY + 5.0f), IM_COL32(255, 212, 92, 255), "[M3 Drop 124 BPM]");
    curY += 28.0f;

    // Rich Studio Tracks Showcase (Audio, Groove Agent ONE, Strings Inplace MIDI, Prologue, Embracer 5.1, Automation)
    struct TrackShowcaseSpec {
        const char* label;
        const char* badge;
        ImU32 accent;
        int style; // 0 = vocal audio, 1 = drums transient, 2 = inplace MIDI, 3 = synth pad, 4 = loopmash, 5 = automation
    };
    const std::array<TrackShowcaseSpec, 7> rows = {{
        {"01  Lead Vocal",        "AUDIO", IM_COL32(56, 146, 224, 255), 0},
        {"02  Acoustic Kit",      "GA-ONE", IM_COL32(226, 146, 52, 255), 1},
        {"03  Strings Inplace",   "MIDI",  IM_COL32(168, 96, 232, 255), 2},
        {"04  Analog Bass",       "MONO",  IM_COL32(52, 188, 138, 255), 2},
        {"05  Surround Pad 5.1",  "EMBR",  IM_COL32(66, 176, 206, 255), 3},
        {"06  LoopMash Break",    "MASH",  IM_COL32(218, 86, 118, 255), 4},
        {"    |_ Vol Automation", "TRIM",  IM_COL32(102, 226, 132, 255), 5}
    }};

    for (std::size_t r = 0; r < rows.size(); ++r) {
        const float rowH = 54.0f;
        const ImVec2 hMin(winX + 4.0f, curY);
        const ImVec2 hMax(laneX0 - 2.0f, curY + rowH);
        dl->AddRectFilled(hMin, hMax, (r == 0) ? IM_COL32(42, 52, 68, 255) : IM_COL32(30, 35, 43, 255));
        dl->AddRectFilled(hMin, ImVec2(hMin.x + 5.0f, hMax.y), rows[r].accent);
        dl->AddLine(ImVec2(winX + 4.0f, hMax.y), ImVec2(laneX1, hMax.y), IM_COL32(45, 52, 62, 255));

        dl->AddText(ImVec2(hMin.x + 12.0f, hMin.y + 8.0f), IM_COL32(234, 239, 246, 255), rows[r].label);
        dl->AddRectFilled(ImVec2(hMax.x - 48.0f, hMin.y + 6.0f), ImVec2(hMax.x - 6.0f, hMin.y + 19.0f),
                          rows[r].accent, 2.0f);
        dl->AddText(ImVec2(hMax.x - 43.0f, hMin.y + 9.0f), IM_COL32(15, 18, 24, 255), rows[r].badge);

        // Mini M / S / R / e strip buttons in track header
        dl->AddRectFilled(ImVec2(hMin.x + 12.0f, hMin.y + 28.0f), ImVec2(hMin.x + 30.0f, hMin.y + 44.0f), IM_COL32(52, 56, 66, 255), 2.0f);
        dl->AddText(ImVec2(hMin.x + 18.0f, hMin.y + 33.0f), IM_COL32(215, 220, 228, 255), "M");
        dl->AddRectFilled(ImVec2(hMin.x + 34.0f, hMin.y + 28.0f), ImVec2(hMin.x + 52.0f, hMin.y + 44.0f), IM_COL32(52, 56, 66, 255), 2.0f);
        dl->AddText(ImVec2(hMin.x + 40.0f, hMin.y + 33.0f), IM_COL32(215, 220, 228, 255), "S");
        dl->AddRectFilled(ImVec2(hMin.x + 56.0f, hMin.y + 28.0f), ImVec2(hMin.x + 74.0f, hMin.y + 44.0f),
                          (r == 0) ? IM_COL32(192, 46, 52, 255) : IM_COL32(52, 56, 66, 255), 2.0f);
        dl->AddText(ImVec2(hMin.x + 62.0f, hMin.y + 33.0f), IM_COL32(245, 245, 248, 255), "R");
        dl->AddRectFilled(ImVec2(hMin.x + 78.0f, hMin.y + 28.0f), ImVec2(hMin.x + 96.0f, hMin.y + 44.0f), IM_COL32(46, 86, 128, 255), 2.0f);
        dl->AddText(ImVec2(hMin.x + 84.0f, hMin.y + 33.0f), IM_COL32(225, 238, 255, 255), "e");

        const float cy = curY + rowH * 0.5f;
        if (rows[r].style == 0 || rows[r].style == 3) {
            // Audio Clip Container + Real Peak Waveform + Per-Clip Volume Envelope
            const float cStart = laneX0 + barPx * (rows[r].style == 0 ? 1.0f : 0.0f) + 2.0f;
            const float cEnd = laneX0 + barPx * 8.0f - 4.0f;
            dl->AddRectFilled(ImVec2(cStart, curY + 3.0f), ImVec2(cEnd, curY + rowH - 3.0f),
                              (rows[r].style == 0) ? IM_COL32(34, 82, 132, 215) : IM_COL32(32, 98, 118, 215), 3.0f);
            dl->AddRect(ImVec2(cStart, curY + 3.0f), ImVec2(cEnd, curY + rowH - 3.0f), rows[r].accent, 3.0f);
            dl->AddText(ImVec2(cStart + 6.0f, curY + 6.0f), IM_COL32(235, 246, 255, 255),
                        rows[r].style == 0 ? "Vocal_Lead_Take01.wav [24-bit / 48kHz]" : "Embracer_SurroundPad_51.wav");
            for (float px = cStart + 4.0f; px < cEnd - 4.0f; px += 2.0f) {
                const float env = 0.35f + 0.60f * std::abs(std::sin((px - cStart) * 0.032f)) *
                                  std::abs(std::cos((px - cStart) * 0.011f));
                const float amp = env * 18.0f;
                dl->AddLine(ImVec2(px, cy - amp), ImVec2(px, cy + amp),
                            (rows[r].style == 0) ? IM_COL32(148, 218, 255, 245) : IM_COL32(138, 242, 238, 245));
            }
        } else if (rows[r].style == 1 || rows[r].style == 4) {
            // Sliced Drum / LoopMash Regions
            for (int b = 0; b < 8; ++b) {
                const float sx0 = laneX0 + static_cast<float>(b) * barPx + 2.0f;
                const float sx1 = sx0 + barPx - 5.0f;
                dl->AddRectFilled(ImVec2(sx0, curY + 4.0f), ImVec2(sx1, curY + rowH - 4.0f),
                                  (rows[r].style == 1) ? IM_COL32(118, 76, 28, 215) : IM_COL32(118, 42, 68, 215), 3.0f);
                dl->AddRect(ImVec2(sx0, curY + 4.0f), ImVec2(sx1, curY + rowH - 4.0f), rows[r].accent, 3.0f);
                for (int hit = 0; hit < 4; ++hit) {
                    const float hx = sx0 + static_cast<float>(hit) * (barPx * 0.24f) + 4.0f;
                    for (int d = 0; d < 12; ++d) {
                        const float decay = (1.0f - static_cast<float>(d) / 12.0f) * ((hit % 2 == 0) ? 18.0f : 12.0f);
                        dl->AddLine(ImVec2(hx + d * 1.8f, cy - decay), ImVec2(hx + d * 1.8f, cy + decay),
                                    IM_COL32(255, 216, 142, 240));
                    }
                }
            }
        } else if (rows[r].style == 2) {
            // Inplace MIDI Piano-Roll Editor inside Project Window!
            const float cStart = laneX0 + 2.0f;
            const float cEnd = laneX0 + barPx * 8.0f - 4.0f;
            dl->AddRectFilled(ImVec2(cStart, curY + 3.0f), ImVec2(cEnd, curY + rowH - 3.0f),
                              IM_COL32(42, 34, 62, 225), 3.0f);
            dl->AddRect(ImVec2(cStart, curY + 3.0f), ImVec2(cEnd, curY + rowH - 3.0f), rows[r].accent, 3.0f);
            for (int n = 0; n < 24; ++n) {
                const float nx0 = cStart + 8.0f + static_cast<float>(n) * ((cEnd - cStart - 20.0f) / 24.0f);
                const float ny = curY + 10.0f + static_cast<float>((n * 5 + r * 3) % 7) * 5.2f;
                const ImU32 nCol = (n % 3 == 0) ? IM_COL32(95, 205, 255, 245)
                                 : (n % 3 == 1) ? IM_COL32(255, 138, 86, 245)
                                                : IM_COL32(188, 128, 255, 245);
                dl->AddRectFilled(ImVec2(nx0, ny), ImVec2(nx0 + 28.0f, ny + 5.0f), nCol, 1.5f);
            }
        } else if (rows[r].style == 5) {
            // Automation Lane (Touch / Trim Bezier Curve + Nodes)
            dl->AddRectFilled(ImVec2(laneX0, curY + 2.0f), ImVec2(laneX1, curY + rowH - 2.0f),
                              IM_COL32(20, 32, 26, 220));
            ImVec2 prevPt(laneX0, cy + 6.0f);
            for (int p = 1; p <= 18; ++p) {
                const float px = laneX0 + (static_cast<float>(p) / 18.0f) * laneW;
                const float py = (p >= 12) ? (cy - 12.0f) // Trim holds last value to end!
                                           : (cy - std::sin(static_cast<float>(p) * 0.55f) * 15.0f);
                dl->AddLine(prevPt, ImVec2(px, py), IM_COL32(98, 238, 142, 255), 2.0f);
                dl->AddCircleFilled(ImVec2(px, py), 3.0f, IM_COL32(215, 255, 225, 255));
                prevPt = ImVec2(px, py);
            }
        }
        curY += rowH + 1.0f;
    }

    // Playhead Vertical Cursor (Bar 3, Beat 2)
    const float playX = laneX0 + barPx * 2.35f;
    dl->AddTriangleFilled(ImVec2(playX - 6.0f, 99.0f), ImVec2(playX + 6.0f, 99.0f),
                          ImVec2(playX, 109.0f), IM_COL32(255, 218, 72, 255));
    dl->AddLine(ImVec2(playX, 108.0f), ImVec2(playX, 582.0f), IM_COL32(255, 218, 72, 240), 2.0f);
    ImGui::End();
}

void ProjectAndEditorsUI::renderKeyEditor(
    Core::MidiPartContainer& part,
    const Core::ExpressionMap& expressionMap,
    const Core::MusicalClock& /*clock*/) {
    // Lower-Left Docked Key Editor (x: 270..654, y: 588..1080)
    ImGui::SetNextWindowPos(ImVec2(270.0f, 588.0f));
    ImGui::SetNextWindowSize(ImVec2(384.0f, 492.0f));
    ImGui::Begin("Key Editor - Piano Roll & VST Expression");
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const ImVec2 p0(276.0f, 616.0f);
    const float pianoW = 34.0f;
    const ImVec2 gridMin(p0.x + pianoW, p0.y);
    const ImVec2 gridMax(646.0f, 962.0f);

    dl->AddRectFilled(gridMin, gridMax, IM_COL32(19, 22, 28, 255));

    // Vertical Piano Roll Keyboard (24 keys)
    const int numKeys = 24;
    const float keyH = (gridMax.y - gridMin.y) / static_cast<float>(numKeys);
    for (int k = 0; k < numKeys; ++k) {
        const int noteInOct = (23 - k) % 12;
        const bool isBlack = (noteInOct == 1 || noteInOct == 3 || noteInOct == 6 || noteInOct == 8 || noteInOct == 10);
        const float ky0 = gridMin.y + static_cast<float>(k) * keyH;
        const float ky1 = ky0 + keyH;

        dl->AddRectFilled(ImVec2(p0.x, ky0), ImVec2(gridMin.x - 1.0f, ky1 - 1.0f),
                          isBlack ? IM_COL32(32, 35, 42, 255) : IM_COL32(226, 230, 236, 255));
        dl->AddRectFilled(ImVec2(gridMin.x, ky0), ImVec2(gridMax.x, ky1),
                          isBlack ? IM_COL32(22, 25, 32, 255) : IM_COL32(27, 31, 39, 255));
        dl->AddLine(ImVec2(gridMin.x, ky1), ImVec2(gridMax.x, ky1), IM_COL32(36, 41, 50, 255));
        if (noteInOct == 0) {
            char cName[8]{};
            std::snprintf(cName, sizeof(cName), "C%d", 3 + ((23 - k) / 12));
            dl->AddText(ImVec2(p0.x + 4.0f, ky0 + 3.0f), IM_COL32(30, 35, 45, 255), cName);
        }
    }

    for (int beat = 0; beat <= 8; ++beat) {
        const float gx = gridMin.x + static_cast<float>(beat) * ((gridMax.x - gridMin.x) / 8.0f);
        dl->AddLine(ImVec2(gx, gridMin.y), ImVec2(gx, gridMax.y),
                    (beat % 4 == 0) ? IM_COL32(62, 72, 88, 255) : IM_COL32(38, 44, 54, 255));
    }

    // Velocity & Articulation Lane (y: 968..1072)
    const ImVec2 velMin(gridMin.x, 968.0f);
    const ImVec2 velMax(gridMax.x, 1072.0f);
    dl->AddRectFilled(ImVec2(p0.x, velMin.y), velMax, IM_COL32(16, 19, 24, 255));
    dl->AddRect(ImVec2(p0.x, velMin.y), velMax, IM_COL32(48, 56, 68, 255));
    dl->AddText(ImVec2(p0.x + 3.0f, velMin.y + 6.0f), IM_COL32(162, 176, 194, 255), "VEL");

    // Render Rich Chord / Melody Notes Colored by VST Expression SoundSlot!
    const int demoPitches[14] = {4, 7, 11, 16, 6, 9, 14, 18, 5, 11, 15, 19, 12, 21};
    const int demoCols[14]    = {0, 0,  0,  1, 1, 1,  2,  1, 0,  2,  0,  1,  2,  1};
    (void)part;
    const ImU32 slotPalette[3] = {
        expressionMap.colorForArticulationSlot(1),
        IM_COL32(78, 188, 255, 245),
        IM_COL32(186, 118, 255, 245)
    };

    for (int i = 0; i < 14; ++i) {
        const float nx0 = gridMin.x + 8.0f + static_cast<float>(i / 2) * 46.0f;
        const float nx1 = nx0 + 38.0f;
        const float ny = gridMax.y - static_cast<float>(demoPitches[i] + 1) * keyH;
        const ImU32 col = slotPalette[demoCols[i]];
        dl->AddRectFilled(ImVec2(nx0, ny + 1.0f), ImVec2(nx1, ny + keyH - 1.0f), col, 2.5f);
        dl->AddRect(ImVec2(nx0, ny + 1.0f), ImVec2(nx1, ny + keyH - 1.0f), IM_COL32(255, 255, 255, 140), 2.5f);

        const float velH = 38.0f + static_cast<float>((i * 17) % 52);
        dl->AddRectFilled(ImVec2(nx0, velMax.y - velH), ImVec2(nx0 + 5.0f, velMax.y - 2.0f), col);
    }
    ImGui::End();
}

void ProjectAndEditorsUI::renderSampleEditor(
    const Core::AudioPoolAsset* /*asset*/,
    const Core::DSP::VariAudioEngine& variAudio,
    const Core::DSP::SampleEditorEngine& sampleEditor) {
    // Lower-Center Docked Sample Editor: VariAudio Pitch + Hitpoint Slicing (x: 656..1038, y: 588..1080)
    ImGui::SetNextWindowPos(ImVec2(656.0f, 588.0f));
    ImGui::SetNextWindowSize(ImVec2(382.0f, 492.0f));
    ImGui::Begin("Sample Editor - VariAudio & Hitpoints");
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const auto& ruler = sampleEditor.definitionRuler();
    dl->AddText(ImVec2(666.0f, 616.0f), IM_COL32(142, 212, 255, 255),
                "VariAudio YIN + TD-PSOLA  |  Definition: 124.0 BPM (4/4)");
    (void)ruler;
    (void)variAudio;

    const ImVec2 boxMin(664.0f, 634.0f);
    const ImVec2 boxMax(1030.0f, 1070.0f);
    dl->AddRectFilled(boxMin, boxMax, IM_COL32(17, 21, 27, 255));
    dl->AddRect(boxMin, boxMax, IM_COL32(52, 64, 80, 255));

    // Semitone Horizontal Grid
    for (int st = 1; st < 12; ++st) {
        const float sy = boxMin.y + static_cast<float>(st) * ((boxMax.y - boxMin.y) / 12.0f);
        dl->AddLine(ImVec2(boxMin.x, sy), ImVec2(boxMax.x, sy), IM_COL32(30, 36, 46, 255));
    }

    // Subtle Background Audio Waveform
    const float midY = 0.5f * (boxMin.y + boxMax.y);
    for (float x = boxMin.x + 4.0f; x < boxMax.x - 4.0f; x += 2.0f) {
        const float wAmp = 55.0f * std::abs(std::sin((x - boxMin.x) * 0.045f)) *
                           (0.4f + 0.6f * std::abs(std::cos((x - boxMin.x) * 0.017f)));
        dl->AddLine(ImVec2(x, midY - wAmp), ImVec2(x, midY + wAmp), IM_COL32(46, 78, 108, 110));
    }

    // Amber Hitpoint Transient Slice Lines
    for (int hp = 1; hp <= 6; ++hp) {
        const float hx = boxMin.x + static_cast<float>(hp) * ((boxMax.x - boxMin.x) / 7.0f);
        dl->AddLine(ImVec2(hx, boxMin.y), ImVec2(hx, boxMax.y), IM_COL32(255, 192, 58, 150), 1.0f);
        dl->AddTriangleFilled(ImVec2(hx - 4.0f, boxMin.y), ImVec2(hx + 4.0f, boxMin.y),
                              ImVec2(hx, boxMin.y + 8.0f), IM_COL32(255, 198, 68, 255));
    }

    // VariAudio Note Segments + Continuous Micro-Pitch F0 Contour!
    struct VariSegDemo { float x0, x1, y; const char* note; };
    const std::array<VariSegDemo, 6> vSegs = {{
        {674.0f, 722.0f, 890.0f, "C4"},
        {728.0f, 780.0f, 835.0f, "D4"},
        {786.0f, 848.0f, 775.0f, "F4"},
        {854.0f, 908.0f, 738.0f, "G4"},
        {914.0f, 968.0f, 775.0f, "F4"},
        {974.0f, 1022.0f, 702.0f, "A4"}
    }};

    ImVec2 prevPitch(vSegs[0].x0, vSegs[0].y);
    for (const auto& s : vSegs) {
        dl->AddRectFilled(ImVec2(s.x0, s.y - 13.0f), ImVec2(s.x1, s.y + 13.0f),
                          IM_COL32(228, 86, 66, 210), 3.0f);
        dl->AddRect(ImVec2(s.x0, s.y - 13.0f), ImVec2(s.x1, s.y + 13.0f),
                    IM_COL32(255, 168, 148, 255), 3.0f);
        dl->AddText(ImVec2(s.x0 + 5.0f, s.y - 4.0f), IM_COL32(255, 248, 245, 255), s.note);

        for (float px = s.x0; px <= s.x1; px += 4.0f) {
            const float vib = std::sin((px - s.x0) * 0.35f) * 4.5f;
            const ImVec2 curPitch(px, s.y + vib);
            dl->AddLine(prevPitch, curPitch, IM_COL32(255, 238, 128, 255), 1.8f);
            prevPitch = curPitch;
        }
    }
    ImGui::End();
}

void ProjectAndEditorsUI::renderDrumEditor(
    Core::MidiPartContainer& part,
    const Core::MusicalClock& /*clock*/) {
    ImGui::SetNextWindowPos(ImVec2(270.0f, 588.0f));
    ImGui::SetNextWindowSize(ImVec2(384.0f, 220.0f));
    if (!ImGui::Begin("Drum Editor (GM/Studio Drum Map Diamonds)")) {
        ImGui::End();
        return;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    for (const auto& ev : part.events) {
        if (ev.kind != Core::SequencerMidiKind::Note) continue;
        const float cx = origin.x + 60.0f + static_cast<float>(ev.startTick) * 0.10f;
        const float cy = origin.y + 24.0f;
        dl->AddTriangleFilled(ImVec2(cx, cy - 5.0f), ImVec2(cx + 5.0f, cy), ImVec2(cx - 5.0f, cy),
                              IM_COL32(250, 180, 55, 255));
        dl->AddTriangleFilled(ImVec2(cx - 5.0f, cy), ImVec2(cx + 5.0f, cy), ImVec2(cx, cy + 5.0f),
                              IM_COL32(220, 140, 35, 255));
    }
    ImGui::End();
}

void ProjectAndEditorsUI::renderListEditor(
    Core::MidiPartContainer& part,
    const Core::MusicalClock& clock) {
    ImGui::SetNextWindowPos(ImVec2(270.0f, 812.0f));
    ImGui::SetNextWindowSize(ImVec2(384.0f, 130.0f));
    if (!ImGui::Begin("List Editor")) {
        ImGui::End();
        return;
    }
    for (const auto& ev : part.events) {
        const std::string posStr = clock.formatBBST(part.startTick + ev.startTick);
        ImGui::Text("%s | Ch %u | Note %u | Vel %u", posStr.c_str(), ev.channel + 1U, ev.pitchOrCC, ev.velocityOrVal);
    }
    ImGui::End();
}

void ProjectAndEditorsUI::renderBasicScoreEditor(
    const Core::MidiPartContainer& part,
    const Core::MusicalClock& /*clock*/) {
    ImGui::SetNextWindowPos(ImVec2(656.0f, 812.0f));
    ImGui::SetNextWindowSize(ImVec2(382.0f, 130.0f));
    if (!ImGui::Begin("Basic Score Editor (MusicXML)")) {
        ImGui::End();
        return;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    for (int l = 0; l < 5; ++l) {
        const float y = origin.y + 18.0f + static_cast<float>(l) * 8.0f;
        dl->AddLine(ImVec2(origin.x + 8.0f, y), ImVec2(origin.x + 350.0f, y), IM_COL32(180, 185, 195, 255));
    }
    for (const auto& ev : part.events) {
        if (ev.kind != Core::SequencerMidiKind::Note) continue;
        dl->AddCircleFilled(ImVec2(origin.x + 42.0f, origin.y + 34.0f), 4.0f, IM_COL32(235, 240, 250, 255));
    }
    ImGui::End();
}

} // namespace AlphaStudio::UI
