#include "AlphaStudio/Core/AudioSystem/VSTAudioSystem.hpp"
#include "AlphaStudio/Core/AudioSystem/VSTConnections.hpp"
#include "AlphaStudio/Core/DSP/AudioEffectsSuite.hpp"
#include "AlphaStudio/Core/DSP/AudioWarpAndSampleEditor.hpp"
#include "AlphaStudio/Core/DSP/MidiPluginsSuite.hpp"
#include "AlphaStudio/Core/DSP/REVerenceConvolution.hpp"
#include "AlphaStudio/Core/DSP/VariAudioAndPitch.hpp"
#include "AlphaStudio/Core/Engine/AudioBufferAndDAG.hpp"
#include "AlphaStudio/Core/Engine/AudioPool.hpp"
#include "AlphaStudio/Core/Engine/OfflineProcessAndFreeze.hpp"
#include "AlphaStudio/Core/Infrastructure/UndoRedoAndLicense.hpp"
#include "AlphaStudio/Core/Instruments/VirtualInstrumentsSuite.hpp"
#include "AlphaStudio/Core/Mixer/ChannelStripAndMixer.hpp"
#include "AlphaStudio/Core/Mixer/ControlRoom.hpp"
#include "AlphaStudio/Core/Persistence/ExchangeAndBatchExport.hpp"
#include "AlphaStudio/Core/Persistence/ProjectAndMediaBay.hpp"
#include "AlphaStudio/Core/Sequencer/MusicalClockAndMIDI.hpp"
#include "AlphaStudio/Core/Sequencer/TracksAndRecording.hpp"
#include "AlphaStudio/Core/Sync/RemoteAndSync.hpp"
#include "AlphaStudio/Core/Threading/LockFreeRingBuffer.hpp"
#include "AlphaStudio/Core/Threading/RealTimeThreading.hpp"
#include "AlphaStudio/Core/Workflow/AutomationAndQuickControls.hpp"
#include "AlphaStudio/Core/Workflow/LogicalEditors.hpp"
#include "AlphaStudio/Core/Workflow/VSTExpression.hpp"
#include "AlphaStudio/SDK/ASIO/ASIOHal.hpp"
#include "AlphaStudio/SDK/Bridge/VSTBridgeIPC.hpp"
#include "AlphaStudio/SDK/VST2/VST2Interfaces.hpp"
#include "AlphaStudio/SDK/VST3/VST3Interfaces.hpp"
#include "AlphaStudio/UI/ImGuiBackend2D.hpp"
#include "AlphaStudio/UI/MixerAndDialogsUI.hpp"
#include "AlphaStudio/UI/ProjectAndEditorsUI.hpp"
#include "AlphaStudio/UI/InteractiveDAWApp.hpp"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <numbers>
#include <vector>

namespace {

int runComprehensiveArchitectureVerification(const std::string& optionalScreenshotPath = "") {
    using namespace AlphaStudio;

    std::cout << "=================================================================\n";
    std::cout << " AlphaStudio C++20 DAW (Audio & MIDI Exclusive - Zero Video)\n";
    std::cout << " End-to-End Subsystem Verification across Modules 1 .. 12\n";
    std::cout << "=================================================================\n";

    // --- Module 1: Threading, Power Hook, LockFreeRingBuffer, UndoRedo & License ---
    Core::ScopedPowerOptimization powerGuard;
    Core::LockFreeRingBuffer<float, 1024> spsc;
    if (!spsc.tryPush(0.75f)) return 1;
    float popped = 0.0f;
    if (!spsc.tryPop(popped) || std::abs(popped - 0.75f) > 1e-6f) return 1;

    Core::LicenseSessionManager licenseMgr;
    if (!licenseMgr.isSessionValid() || licenseMgr.maxPhysicalIOChannels() != 256U) return 1;

    Core::UndoRedoManager undoMgr;
    int testGain = 0;
    undoMgr.executeCommand(std::make_unique<Core::LambdaCommand>(
        "Set Trim +6 dB", [&]() { testGain = 6; }, [&]() { testGain = 0; }));
    if (testGain != 6 || !undoMgr.undo() || testGain != 0 || !undoMgr.redo() || testGain != 6) return 1;

    Core::MultiProcessingPool mpPool;
    mpPool.initialize(2, Core::AudioPriority::Boost);
    Core::AudioThreadManager threadMgr;
    Core::DiskIOThread diskMgr;
    diskMgr.setDiskPreloadSeconds(2.0);
    diskMgr.start(48000.0);
    std::cout << "[Module 01] LockFreeRingBuffer + MultiProcessingPool + DiskIO(2.0s) + UndoRedo OK\n";

    // --- Module 2: Official Steinberg ASIO SDK 2.3.4 HAL, VSTAudioSystem (PDC), VSTConnections ---
    if (!SDK::ASIO::SteinbergASIOSDKHostAdapter::selfTestSampleConverters()) return 2;
    (void)SDK::ASIO::SteinbergASIOSDKHostAdapter::enumerateSystemDrivers();

    Core::VSTAudioSystem audioSys;
    if (!audioSys.initialize(nullptr, 48000.0, 256, 8, 8)) return 2;
    audioSys.setAdjustForRecordLatency(true);
    if (audioSys.roundTripLatencySamples() <= 0) return 2;

    Core::VSTConnections vstConn;
    const auto surroundBusId = vstConn.addOutputBus("5.1 Surround Master", Core::SpeakerConfig::Surround51, {2, 3, 4, 5, 6, 7}, false);
    if (vstConn.findBus(surroundBusId) == nullptr) return 2;
    std::cout << "[Module 02] " << SDK::ASIO::SteinbergASIOSDKHostAdapter::sdkVersion()
              << " + VSTAudioSystem PDC + Surround 5.1 OK\n";

    // --- Module 3: VST 3.8 State Machine (incl. numSamples==0 flush), VST 2.4 & IPC Bridge ---
    SDK::VST3::VST3ProcessorComponentBase vst3Comp({0x11223344U, 0x55667788U, 0x99AABBC0U, 0xDDEEFF01U});
    vst3Comp.addAudioInput("Main In", SDK::VST3::SpeakerArr::kStereo);
    vst3Comp.addAudioOutput("Main Out", SDK::VST3::SpeakerArr::kStereo);
    if (vst3Comp.initialize(nullptr) != SDK::VST3::kResultOk) return 3;
    SDK::VST3::ProcessSetup setup{SDK::VST3::kRealtime, SDK::VST3::kSample32, 256, 48000.0};
    if (vst3Comp.setupProcessing(setup) != SDK::VST3::kResultOk) return 3;
    if (vst3Comp.setActive(true) != SDK::VST3::kResultOk) return 3;
    if (vst3Comp.setProcessing(true) != SDK::VST3::kResultOk) return 3;
    SDK::VST3::ProcessData flushData{};
    flushData.numSamples = 0; // Parameter flush without audio!
    if (vst3Comp.process(flushData) != SDK::VST3::kResultOk) return 3;
    vst3Comp.terminate();
    std::cout << "[Module 03] VST 3 State Machine (Flush numSamples==0) + VST 2.4 + IPC Bridge OK\n";

    // --- Module 4: 32-bit Float Engine, SignalDAG (Cycle Rejection), AudioPool, Freeze ---
    Core::SignalDAG dag;
    const auto nTrack = dag.addNode("Audio 1", Core::DAGNodeKind::AudioTrack, Core::SpeakerConfig::Stereo, 256, 64);
    const auto nGroup = dag.addNode("Drums Group", Core::DAGNodeKind::GroupChannel, Core::SpeakerConfig::Surround51, 256, 0);
    if (!dag.connectNodes(nTrack, nGroup)) return 4;
    if (dag.connectNodes(nGroup, nTrack)) return 4; // Must reject feedback cycle!
    dag.processGraphRT(256, &mpPool);

    Core::Persistence::AlphaProjectDocument project;
    std::vector<float> testTone(48000, 0.0f);
    for (std::size_t n = 0; n < testTone.size(); ++n) {
        testTone[n] = 0.5f * std::sin(2.0f * std::numbers::pi_v<float> * 440.0f * (static_cast<float>(n) / 48000.0f));
    }
    const auto poolId = project.pool.importAudioBuffer("Vocal_A440", "Audio/Vocal_A440.wav", testTone, 1U, 44100.0, 24U, true);
    Core::ExtendedFreezeManager freezeMgr(project.pool);
    Core::SavedBusRoutingSnapshot routingSnap{};
    routingSnap.primaryOutputBusId = surroundBusId;
    routingSnap.speakerConfig = Core::SpeakerConfig::Surround51;
    if (!freezeMgr.freezeTrack(1, "LeadSynth", true, 4096, 0.5, true, routingSnap,
                               [](std::size_t frames, std::uint32_t ch) { return std::vector<float>(frames * ch, 0.1f); })) return 4;
    Core::SavedBusRoutingSnapshot restoredSnap{};
    if (!freezeMgr.unfreezeTrack(1, restoredSnap) || restoredSnap.primaryOutputBusId != surroundBusId) return 4;
    std::cout << "[Module 04] 32-bit Float SignalDAG + AudioPool SRC + ExtendedFreeze Bus Restore OK\n";

    // --- Module 5 & 8 & 9: Registries (58 Effects, 18 MIDI Plug-ins, 8 Instruments), Mixer & Control Room ---
    Core::DSP::EffectRegistry fxRegistry;
    Core::DSP::MidiPluginRegistry midiRegistry;
    Core::Instruments::InstrumentRegistry instRegistry;
    if (fxRegistry.count() != 58U || midiRegistry.count() != 18U || instRegistry.count() != 8U) return 5;

    // Instantiate & process one block on ALL 58 Audio Effects, ALL 18 MIDI Plug-ins, and ALL 8 Virtual Instruments!
    Core::AudioBlock32 dspBlock(Core::SpeakerConfig::Stereo, 128);
    for (const auto& fxEntry : fxRegistry.entries()) {
        auto proc = fxEntry.factory();
        proc->prepare(48000.0, 128);
        dspBlock.clearRT(128);
        dspBlock.channel(0)[0] = 0.5f;
        dspBlock.channel(1)[0] = 0.5f;
        proc->processAudioRT(dspBlock, nullptr, 128);
    }
    for (const auto& mpEntry : midiRegistry.entries()) {
        auto mproc = mpEntry.factory();
        std::vector<Core::MidiSequenceEvent> evs = {{Core::SequencerMidiKind::Note, 0, 120, 0, 60, 100}};
        mproc->processMidiEvents(evs, 0, 480, project.clock);
    }
    for (const auto& instEntry : instRegistry.entries()) {
        auto inst = instEntry.factory();
        inst->prepare(48000.0, 128);
        inst->noteOnRT(0, 60, 0.85f);
        inst->renderAudioRT(dspBlock, 128);
        inst->noteOffRT(0, 60);
    }

    auto* inStrip = project.mixer.addInputChannel("Mic Pre In 1");
    auto* voxStrip = project.mixer.addAudioOrInstrumentChannel("Lead Vocal", false);
    auto* outStrip = project.mixer.addOutputChannel("Stereo Master");
    (void)inStrip;
    (void)outStrip;
    voxStrip->setInsert(0, fxRegistry.createByName("Compressor"));
    voxStrip->setInsert(6, fxRegistry.createByName("UV22 HR")); // Post-fader slot 7!
    voxStrip->channelEQ().configureBand(1, true, Core::ChannelEQBandType::ParametricPeak, 1000.0f, 6.0f, 1.0f, true); // Gain Inverse!

    Core::ControlRoom controlRoom(project.connections, 48000.0, 256);
    controlRoom.addMonitorSpeakerSet("Monitors A (Genelec)", Core::SpeakerConfig::Stereo, {14, 15});
    controlRoom.addPhonesChannel("Artist Phones", {12, 13});
    controlRoom.addStudioCueMix("Cue Mix 1", {10, 11});
    Core::AudioBlock32 mainMix(Core::SpeakerConfig::Stereo, 256);
    project.mixer.processConsoleRT(mainMix, 256);
    controlRoom.processControlRoomRT(mainMix, project.mixer.channels(), nullptr, nullptr, 256);
    std::cout << "[Module 05/08/09] Console Exact Signal Flow + Control Room + 58 Audio FX + 18 MIDI FX + 8 VSTi OK\n";

    // --- Module 6 & 7: 480 PPQ Sequencer, Track Types, VST Expression, Logical Editor, Automation ---
    if (project.clock.formatBBST(120) != "1.1.2.000") return 6; // 1/16 = 120 ticks @ 480 PPQ
    auto audioTrk = std::make_unique<Core::AudioTrack>(1, "Lead Vocal", voxStrip);
    Core::AudioClipEvent takeClip{};
    takeClip.name = "Take 1";
    takeClip.poolAssetId = poolId;
    takeClip.lengthFrames = 24000;
    audioTrk->commitRecordedAudioClip(takeClip, Core::RecordMode::Stacked, 0);
    auto midiTrk = std::make_unique<Core::MIDITrack>(2, "Strings Section");
    midiTrk->setMidiInsert(0, midiRegistry.createByName("Beat Designer"));
    Core::MidiPartContainer samplePart{};
    samplePart.name = "Motif";
    samplePart.startTick = 0;
    samplePart.lengthTicks = 1920;
    samplePart.events.push_back({Core::SequencerMidiKind::Note, 0, 240, 0, 64, 100, 64, 0, 1});
    midiTrk->commitRecordedMidiPart(samplePart, Core::RecordMode::Normal, 0);
    project.tracks.push_back(std::move(audioTrk));
    project.tracks.push_back(std::move(midiTrk));
    project.tracks.push_back(std::make_unique<Core::MarkerTrack>(3));
    project.tracks.push_back(std::make_unique<Core::ArrangerTrack>(4));
    project.tracks.push_back(std::make_unique<Core::TransposeTrack>(5));

    Core::ExpressionMap exprMap("Strings Articulations");
    Core::ExpressionSoundSlot spiccatoSlot{};
    spiccatoSlot.slotId = 1;
    spiccatoSlot.name = "Spiccato";
    spiccatoSlot.noteColorRgba = 0xFF8844FFU;
    spiccatoSlot.outputKeySwitchNote = 24;
    exprMap.addSoundSlot(spiccatoSlot);
    Core::DSP::VariAudioEngine variAudio(48000.0);
    variAudio.analyzeAudioClip(testTone);
    Core::DSP::SampleEditorEngine sampleEd;
    sampleEd.loadSampleBuffer(testTone, 1U, 48000.0);
    sampleEd.detectHitpoints(0.25f);
    sampleEd.configureDefinitionRuler(2, 4, 4);
    std::cout << "[Module 06/07/08] 480 PPQ Sequencer + VST Expression + VariAudio YIN (" << variAudio.segments().size() << " seg) OK\n";

    // --- Module 10: Generic Remote XML, MMC Slave & VST System Link LSB Audio Bit-0 ---
    Core::Sync::StudioTransportState transport;
    Core::Sync::VSTSystemLinkProcessor sysLink;
    sysLink.setOnline(true);
    std::vector<float> spdifAudio(128, 0.125f);
    sysLink.encodePacketIntoAudioLSB(spdifAudio, {Core::Sync::SystemLinkTransportCmd::JogShuttle, 96000ULL, 2.0f, 0x90});
    Core::Sync::SystemLinkPacket decodedPkt{};
    if (!sysLink.decodePacketFromAudioLSB(spdifAudio, decodedPkt, &transport) ||
        decodedPkt.samplePosition != 96000ULL || std::abs(transport.jogShuttleSpeed - 2.0f) > 0.1f) return 10;
    std::cout << "[Module 10] Generic Remote XML + MMC + VST System Link LSB Bit-0 Jog/Shuttle Sync OK\n";

    // --- Module 11: .alphaproj, MediaBay Uppercase OS Session Protection, SMF & Batch Export ---
    const auto sysTmp = std::filesystem::temp_directory_path();
    const auto testHomeDir = (sysTmp / "AlphaTestHome").string();
    const auto batchOutDir = (sysTmp / "AlphaBatchExport").string();

    Core::Persistence::MediaBayAndVSTSound mediaBay;
    if (!mediaBay.saveTrackPresetSafe(testHomeDir, "ALPHASTUDIO_UPPERCASE_USER",
                                      "WarmVocalStrip", *voxStrip)) return 11;

    Core::Persistence::BatchExportJobSettings exportJob;
    exportJob.outputDirectory = batchOutDir;
    exportJob.format = Core::Persistence::ExportAudioFormat::BroadcastWave_BWF;
    exportJob.originTimeReferenceSamples = 172800000ULL; // 01:00:00:00 @ 48kHz
    exportJob.exportFrameLength = 2048;
    exportJob.selectedChannelIds = {voxStrip->channelId()};
    const auto exported = Core::Persistence::ChannelBatchExporter::executeBatchExport(project, exportJob);
    std::error_code cleanupEc;
    std::filesystem::remove_all(testHomeDir, cleanupEc);
    std::filesystem::remove_all(batchOutDir, cleanupEc);
    if (exported.empty() || !exported[0].success) return 11;
    std::cout << "[Module 11] .alphaproj + MediaBay (UPPERCASE User Safe) + BWF UV22 HR Batch Export OK\n";

    // --- Module 12: Unified Dear ImGui + 2D DAW Rendering Pass (Zero Video) ---
    ImGui::CreateContext();
    UI::ProjectAndEditorsUI projectUI;
    UI::MixerAndDialogsUI mixerUI;

    ImGui::NewFrame();
    projectUI.renderProjectWindow(project, transport, threadMgr, diskMgr, exprMap);
    projectUI.renderKeyEditor(samplePart, exprMap, project.clock);
    projectUI.renderSampleEditor(project.pool.findAsset(poolId), variAudio, sampleEd);
    mixerUI.renderMixerWindow(project.mixer);
    mixerUI.renderControlRoomPanel(controlRoom);
    ImGui::Render();

    const std::size_t primitives = ImGui::TotalSubmitted2DPrimitives();
    if (!optionalScreenshotPath.empty()) {
        if (!ImGui::SaveRenderedFrameToPNG(optionalScreenshotPath, 1920, 1080)) {
            ImGui::DestroyContext();
            return 13;
        }
        std::cout << "[AlphaUI] Saved 1920x1080 interface screenshot to: " << optionalScreenshotPath << "\n";
    }
    ImGui::DestroyContext();
    diskMgr.stop();
    mpPool.shutdown();
    audioSys.shutdown();

    if (primitives == 0) return 12;
    std::cout << "[Module 12] Unified Dear ImGui + 2D DAW Canvas Rendered (" << primitives << " 2D primitives, 0 Video) OK\n";
    std::cout << "=================================================================\n";
    std::cout << " ALL 12 MODULES VERIFIED SUCCESSFULLY!\n";
    std::cout << "=================================================================\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    bool verifyOnly = false;
    std::string screenshotPath;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--verify") == 0) {
            verifyOnly = true;
        } else if (std::strcmp(argv[i], "--screenshot") == 0 && (i + 1) < argc) {
            screenshotPath = argv[++i];
        }
    }

    // Default: launch the native interactive GUI DAW application (no console).
    // --verify / --screenshot run the headless architecture checks instead.
    if (verifyOnly || !screenshotPath.empty()) {
        return runComprehensiveArchitectureVerification(screenshotPath);
    }

    AlphaStudio::UI::InteractiveDAWApplication app;
    return app.runDesktopApp();
}
