#pragma once

#include "AlphaStudio/Core/DSP/AudioWarpAndSampleEditor.hpp"
#include "AlphaStudio/Core/DSP/VariAudioAndPitch.hpp"
#include "AlphaStudio/Core/Persistence/ProjectAndMediaBay.hpp"
#include "AlphaStudio/Core/Sync/RemoteAndSync.hpp"
#include "AlphaStudio/Core/Threading/RealTimeThreading.hpp"
#include "AlphaStudio/Core/Workflow/VSTExpression.hpp"
#include "AlphaStudio/UI/ImGuiBackend2D.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace AlphaStudio::UI {

// ============================================================================
// WorkspaceLayoutPreset: Virtual Desktops (Workspaces) for AlphaStudio UI
// Strictly Zero Video Windows!
// ============================================================================
struct WorkspaceLayoutPreset {
    std::string name{"Tracking & Arrangement"};
    bool showProjectWindow{true};
    bool showMixerWindow{true};
    bool showControlRoom{true};
    bool showKeyEditor{false};
    bool showDrumEditor{false};
    bool showListEditor{false};
    bool showScoreEditor{false};
    bool showSampleEditor{false};
    bool showInplaceEditor{true};
};

// ============================================================================
// ProjectWindow & Dedicated Audio/MIDI Editors:
// - ProjectWindow (Transport with Pre-Count & High-Precision Scrubbing,
//   TrackInspector, Track List, Multi-track 2D Timeline, Arranger Panel,
//   Global Tracks [Tempo, Signature, Marker, Transpose], Workspaces)
// - KeyEditor (Piano Roll + CC Lanes + VST Expression SoundSlot Note Colors)
// - DrumEditor (Diamond Drum Hits + DrumMap List)
// - ListEditor (Chronological MIDI Event Table + Value Bars)
// - BasicScoreEditor (2D Staff, Clef, Notes, Barlines, MusicXML Link)
// - InplaceEditor (Inline Piano Roll inside ProjectWindow Track Lanes)
// - SampleEditor (2D Waveform + VariAudio Segments + Hitpoints & Definition Ruler)
// ============================================================================
class ProjectAndEditorsUI {
public:
    ProjectAndEditorsUI();

    void saveWorkspace(std::size_t slot0To3, const WorkspaceLayoutPreset& preset);
    const WorkspaceLayoutPreset& recallWorkspace(std::size_t slot0To3) noexcept;
    [[nodiscard]] const WorkspaceLayoutPreset& activeWorkspace() const noexcept {
        return workspaces_[activeWorkspaceIdx_];
    }

    void renderProjectWindow(Core::Persistence::AlphaProjectDocument& project,
                             Core::Sync::StudioTransportState& transport,
                             const Core::AudioThreadManager& audioThreadMgr,
                             const Core::DiskIOThread& diskMgr,
                             const Core::ExpressionMap& expressionMap);

    void renderKeyEditor(Core::MidiPartContainer& part,
                         const Core::ExpressionMap& expressionMap,
                         const Core::MusicalClock& clock);

    void renderDrumEditor(Core::MidiPartContainer& part,
                          const Core::MusicalClock& clock);

    void renderListEditor(Core::MidiPartContainer& part,
                          const Core::MusicalClock& clock);

    void renderBasicScoreEditor(const Core::MidiPartContainer& part,
                                const Core::MusicalClock& clock);

    void renderSampleEditor(const Core::AudioPoolAsset* asset,
                            const Core::DSP::VariAudioEngine& variAudio,
                            const Core::DSP::SampleEditorEngine& sampleEditor);

private:
    void renderTransportBar(Core::Persistence::AlphaProjectDocument& project,
                            Core::Sync::StudioTransportState& transport,
                            const Core::AudioThreadManager& audioThreadMgr,
                            const Core::DiskIOThread& diskMgr);
    void renderTrackInspector(Core::TrackBase& selectedTrack,
                              const Core::ExpressionMap& expressionMap);
    void renderInplaceMidiOverlay(ImDrawList* dl, ImVec2 rowMin, ImVec2 rowMax,
                                  const Core::MIDITrack& midiTrack);

    std::array<WorkspaceLayoutPreset, 4> workspaces_{};
    std::size_t activeWorkspaceIdx_{0};
    bool showGlobalTracks_{true};
    bool showArrangerPanel_{true};
};

} // namespace AlphaStudio::UI
