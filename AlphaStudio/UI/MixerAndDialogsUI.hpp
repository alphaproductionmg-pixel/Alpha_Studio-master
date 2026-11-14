#pragma once

#include "AlphaStudio/Core/AudioSystem/VSTAudioSystem.hpp"
#include "AlphaStudio/Core/AudioSystem/VSTConnections.hpp"
#include "AlphaStudio/Core/Infrastructure/UndoRedoAndLicense.hpp"
#include "AlphaStudio/Core/Mixer/ChannelStripAndMixer.hpp"
#include "AlphaStudio/Core/Mixer/ControlRoom.hpp"
#include "AlphaStudio/Core/Persistence/ProjectAndMediaBay.hpp"
#include "AlphaStudio/Core/Threading/RealTimeThreading.hpp"
#include "AlphaStudio/UI/ImGuiBackend2D.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace AlphaStudio::UI {

// ============================================================================
// MixerWindow, ControlRoomPanel & Studio System Dialogs:
// - MixerWindow (3 Memorizable Views + ExtendedMixerView + Live 2D EQ Curve +
//   Pre/Post1/Post2 Meters + PFL/AFL + Sidechain Badges + Copy/Paste)
// - ControlRoomPanel (Monitors, Phones, 4 Cue/Studio Mixes, 6 Pre + 2 Post
//   Inserts, Downmix Presets, Talkback, Dim, ExclusiveDevicePortsForMonitorChannels)
// - Dialogs: ProjectAssistant, DeviceSetup, VSTConnections, ProjectBrowser,
//   KeyCommands & Macros
// ============================================================================
struct KeyCommandBinding {
    std::string commandCategory;
    std::string commandName;
    std::string keyShortcut;
};

struct UserMacroDefinition {
    std::string macroName;
    std::vector<std::string> stepCommandNames;
};

class MixerAndDialogsUI {
public:
    MixerAndDialogsUI();

    void renderMixerWindow(Core::VSTMixer& mixer);
    void renderControlRoomPanel(Core::ControlRoom& controlRoom);

    void renderProjectAssistantDialog(Core::Persistence::MediaBayAndVSTSound& mediaBay);
    void renderDeviceSetupDialog(Core::VSTAudioSystem& audioSystem,
                                 Core::AudioThreadManager& threadMgr,
                                 Core::DiskIOThread& diskMgr,
                                 Core::MultiProcessingPool& mpPool);
    void renderVSTConnectionsDialog(Core::VSTConnections& connections,
                                    const Core::ControlRoom& controlRoom);
    void renderProjectBrowserDialog(const Core::Persistence::AlphaProjectDocument& project);
    void renderKeyCommandsAndMacrosDialog(Core::UndoRedoManager& undoRedo);

    [[nodiscard]] const std::vector<KeyCommandBinding>& keyCommands() const noexcept { return keyBindings_; }
    [[nodiscard]] const std::vector<UserMacroDefinition>& macros() const noexcept { return macros_; }

private:
    std::vector<KeyCommandBinding> keyBindings_;
    std::vector<UserMacroDefinition> macros_;
};

} // namespace AlphaStudio::UI
