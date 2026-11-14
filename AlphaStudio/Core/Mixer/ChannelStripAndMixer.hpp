#pragma once

#include "AlphaStudio/Core/AudioSystem/VSTConnections.hpp"
#include "AlphaStudio/Core/Engine/AudioBufferAndDAG.hpp"
#include "AlphaStudio/SDK/VST3/VST3Interfaces.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace AlphaStudio::Core {

// ============================================================================
// Abstract Real-Time Audio Insert Processor Interface (Clean-Room DSP / VST3)
// ============================================================================
class IAudioInsertProcessor {
public:
    virtual ~IAudioInsertProcessor() = default;

    [[nodiscard]] virtual const char* effectName() const noexcept = 0;
    virtual void prepare(double sampleRate, std::uint32_t maxBlockFrames) = 0;
    virtual void reset() noexcept = 0;

    // Supports optional VST 3 Sidechain auxiliary audio input (sidechainIn != nullptr)
    virtual void processAudioRT(AudioBlock32& ioBuffer,
                                const AudioBlock32* sidechainIn,
                                std::uint32_t numFrames) noexcept = 0;

    [[nodiscard]] virtual std::uint32_t latencySamples() const noexcept { return 0U; }
    [[nodiscard]] virtual bool supportsSidechain() const noexcept { return false; }

    virtual void setParameterNormalized(std::uint32_t paramId, float normValue) noexcept {
        (void)paramId;
        (void)normValue;
    }
    [[nodiscard]] virtual float getParameterNormalized(std::uint32_t paramId) const noexcept {
        (void)paramId;
        return 0.0f;
    }
    [[nodiscard]] virtual std::uint32_t parameterCount() const noexcept { return 0U; }
    [[nodiscard]] virtual const char* parameterName(std::uint32_t paramId) const noexcept {
        (void)paramId;
        return "";
    }
};

// ============================================================================
// 4-Band Parametric ChannelEQ with Per-Band Gain Inverse Button
// ============================================================================
enum class ChannelEQBandType : std::uint8_t {
    ParametricPeak = 0,
    LowShelf       = 1,
    HighShelf      = 2,
    LowCutHPF      = 3,
    HighCutLPF     = 4
};

struct ChannelEQBand {
    bool enabled{false};
    bool invertGain{false}; // Button Gain Inverse flipping +G dB <-> -G dB!
    ChannelEQBandType type{ChannelEQBandType::ParametricPeak};
    float frequencyHz{1000.0f};
    float gainDb{0.0f}; // -24.0 dB .. +24.0 dB
    float qFactor{1.0f};

    // Biquad coefficients & per-channel Direct-Form II Transposed states (6ch)
    float b0{1.0f}, b1{0.0f}, b2{0.0f}, a1{0.0f}, a2{0.0f};
    std::array<float, AudioBlock32::kMaxChannels> z1{};
    std::array<float, AudioBlock32::kMaxChannels> z2{};
};

class ChannelEQ {
public:
    static constexpr std::size_t kNumBands = 4;

    ChannelEQ();

    void prepare(double sampleRate) noexcept;
    void resetState() noexcept;
    void setBypassed(bool bypassed) noexcept { bypassed_ = bypassed; }
    [[nodiscard]] bool isBypassed() const noexcept { return bypassed_; }

    void configureBand(std::size_t bandIdx, bool enabled, ChannelEQBandType type,
                       float freqHz, float gainDb, float q, bool invertGain = false) noexcept;
    void setBandInvertGain(std::size_t bandIdx, bool invert) noexcept;
    [[nodiscard]] const ChannelEQBand& band(std::size_t bandIdx) const noexcept { return bands_[bandIdx]; }
    [[nodiscard]] ChannelEQBand& band(std::size_t bandIdx) noexcept { return bands_[bandIdx]; }

    [[nodiscard]] float magnitudeDbAtFrequency(float freqHz) const noexcept;
    void processRT(AudioBlock32& buffer, std::uint32_t numFrames) noexcept;

private:
    void recalculateCoefficients(std::size_t bandIdx) noexcept;

    double sampleRate_{48000.0};
    bool bypassed_{false};
    std::array<ChannelEQBand, kNumBands> bands_{};
};

// ============================================================================
// ChannelPanner: Equal Power (-3 dB sin/cos), Linear, Dual-Pan, Surround 5.1 & Bypass
// ============================================================================
enum class PanLawMode : std::uint8_t {
    EqualPowerSinCos = 0, // -3.01 dB center Equal Power
    LinearMinus6dB   = 1,
    LinearZeroDb     = 2
};

class ChannelPanner {
public:
    void setPan(float panNormalizedMinus1ToPlus1) noexcept; // -1.0 Left .. 0.0 Center .. +1.0 Right
    [[nodiscard]] float pan() const noexcept { return pan_; }

    void setFrontRearSurround(float frontPlus1ToRearMinus1) noexcept { frontRear_ = frontPlus1ToRearMinus1; }
    [[nodiscard]] float frontRearSurround() const noexcept { return frontRear_; }
    void setLfeLevel(float linear) noexcept { lfeSend_ = linear; }

    void setEqualPowerMode(bool equalPower) noexcept {
        lawMode_ = equalPower ? PanLawMode::EqualPowerSinCos : PanLawMode::LinearMinus6dB;
    }
    [[nodiscard]] PanLawMode lawMode() const noexcept { return lawMode_; }

    void setBypassed(bool bypassed) noexcept { bypassed_ = bypassed; }
    [[nodiscard]] bool isBypassed() const noexcept { return bypassed_; }

    void processRT(AudioBlock32& buffer, std::uint32_t numFrames) const noexcept;

private:
    float pan_{0.0f};
    float frontRear_{1.0f};
    float lfeSend_{0.0f};
    PanLawMode lawMode_{PanLawMode::EqualPowerSinCos};
    bool bypassed_{false};
};

// ============================================================================
// Aux Send & Studio Cue Send Descriptors + Insert Slot with VST 3 Sidechain
// ============================================================================
struct AuxSendSlot {
    bool enabled{false};
    bool preFader{false}; // Pre-Fader tapped after Inserts 1-6 & EQ; Post-Fader tapped after Inserts 7-8
    std::uint32_t targetBusChannelId{0};
    float levelDb{0.0f};
};

struct StudioCueSendSlot {
    bool enabled{false};
    bool preFader{false};
    std::uint32_t studioCueIndex{0}; // 0..3 (Studio/Cue Mix 1..4)
    float levelDb{0.0f};
    float pan{0.0f};
};

struct AudioInsertSlot {
    bool enabled{false};
    bool bypassed{false};
    bool sidechainActive{false};
    std::uint32_t sidechainSourceChannelId{0};
    std::shared_ptr<IAudioInsertProcessor> processor;
    const AudioBlock32* externalSidechainTap{nullptr}; // Real-time bound pointer
};

// ============================================================================
// ChannelStrip: Exact & Immutable Signal Chain
//   IN -> PhaseReverse + InputGain (Trim)
//      -> Meter (Pre) & PFL
//      -> Inserts 1..6 (Pre-Fader)
//      -> ChannelEQ (with Gain Inverse)
//      -> Aux Sends (Pre) & Studio Sends (Pre)
//      -> Level (Main Fader), Mute, Solo
//      -> Inserts 7..8 (Post-Fader)
//      -> Aux Sends (Post) & Studio Sends (Post)
//      -> Meter (Post 1) [pre-pan]
//      -> Pan (Equal Power / Bypass) -> OUT
//      -> Meter (Post 2) & AFL [post-pan]
// ============================================================================
enum class MixerChannelKind : std::uint8_t {
    InputChannel      = 0, // Visible in Mixer & Project Window for processed recording!
    AudioChannel      = 1,
    InstrumentChannel = 2,
    MIDIChannel       = 3,
    GroupChannel      = 4,
    FXReturnChannel   = 5,
    OutputChannel     = 6
};

struct ChannelMeterSnapshot {
    float prePeakL{0.0f};
    float prePeakR{0.0f};
    float post1PrePanPeakL{0.0f};
    float post1PrePanPeakR{0.0f};
    float post2PostPanPeakL{0.0f};
    float post2PostPanPeakR{0.0f};
};

struct ChannelSettingsSnapshot {
    bool phaseReversed{false};
    float inputGainDb{0.0f};
    float faderDb{0.0f};
    bool muted{false};
    float pan{0.0f};
    bool panBypassed{false};
    ChannelEQ eqState{};
    std::array<AuxSendSlot, 8> auxSends{};
    std::array<StudioCueSendSlot, 4> studioSends{};
};

class ChannelStrip {
public:
    static constexpr std::size_t kNumAudioInserts = 8; // 0..5 Pre-Fader, 6..7 Post-Fader
    static constexpr std::size_t kNumPreFaderInserts = 6;
    static constexpr std::size_t kNumAuxSends = 8;
    static constexpr std::size_t kNumStudioCueSends = 4;
    static constexpr std::size_t kNumMidiInserts = 4;

    ChannelStrip(std::uint32_t channelId, std::string name,
                 MixerChannelKind kind, SpeakerConfig config = SpeakerConfig::Stereo);

    void prepare(double sampleRate, std::uint32_t maxBlockFrames);

    [[nodiscard]] std::uint32_t channelId() const noexcept { return channelId_; }
    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    void setName(std::string newName) { name_ = std::move(newName); }
    [[nodiscard]] MixerChannelKind kind() const noexcept { return kind_; }
    [[nodiscard]] SpeakerConfig speakerConfig() const noexcept { return config_; }
    [[nodiscard]] bool visibleInProjectWindow() const noexcept { return visibleInProject_; }
    void setVisibleInProjectWindow(bool v) noexcept { visibleInProject_ = v; }

    // Input Stage (PhaseReverse + Trim/InputGain)
    void setPhaseReverse(bool reverse) noexcept { phaseReversed_ = reverse; }
    [[nodiscard]] bool phaseReverse() const noexcept { return phaseReversed_; }
    void setInputGainDb(float db) noexcept { inputGainDb_ = db; }
    [[nodiscard]] float inputGainDb() const noexcept { return inputGainDb_; }

    // Fader, Mute, Solo, Listen (PFL/AFL)
    void setFaderDb(float db) noexcept { faderDb_ = db; }
    [[nodiscard]] float faderDb() const noexcept { return faderDb_; }
    void setMute(bool m) noexcept { muted_ = m; }
    [[nodiscard]] bool isMuted() const noexcept { return muted_; }
    void setSolo(bool s) noexcept { soloed_ = s; }
    [[nodiscard]] bool isSoloed() const noexcept { return soloed_; }
    void setSoloDefeat(bool sd) noexcept { soloDefeat_ = sd; }
    [[nodiscard]] bool isSoloDefeat() const noexcept { return soloDefeat_; }
    void setListenEnabled(bool listen) noexcept { listenEnabled_ = listen; }
    [[nodiscard]] bool isListenEnabled() const noexcept { return listenEnabled_; }

    // Inserts, EQ, Sends, Panner
    void setInsert(std::size_t slotIdx, std::shared_ptr<IAudioInsertProcessor> proc, bool enabled = true);
    bool enableSidechainOnInsert(std::size_t slotIdx, std::uint32_t sourceChannelId,
                                 const AudioBlock32* sourceTapBuffer) noexcept;
    [[nodiscard]] const AudioInsertSlot& insertSlot(std::size_t slotIdx) const noexcept { return inserts_[slotIdx]; }
    [[nodiscard]] static constexpr bool isInsertPreFader(std::size_t slotIdx) noexcept {
        return slotIdx < kNumPreFaderInserts;
    }

    [[nodiscard]] ChannelEQ& channelEQ() noexcept { return eq_; }
    [[nodiscard]] const ChannelEQ& channelEQ() const noexcept { return eq_; }
    [[nodiscard]] ChannelPanner& panner() noexcept { return panner_; }
    [[nodiscard]] const ChannelPanner& panner() const noexcept { return panner_; }

    void configureAuxSend(std::size_t idx, bool enabled, bool preFader,
                          std::uint32_t targetBusId, float levelDb) noexcept;
    [[nodiscard]] const AuxSendSlot& auxSend(std::size_t idx) const noexcept { return auxSends_[idx]; }

    void configureStudioSend(std::size_t cueIdx, bool enabled, bool preFader,
                             float levelDb, float pan = 0.0f) noexcept;
    [[nodiscard]] const StudioCueSendSlot& studioSend(std::size_t cueIdx) const noexcept { return studioSends_[cueIdx]; }

    // Copy / Paste Settings Support
    [[nodiscard]] ChannelSettingsSnapshot captureSettings() const noexcept;
    void applySettings(const ChannelSettingsSnapshot& snap) noexcept;

    // Real-Time Exact Signal Chain Execution
    void processStripRT(AudioBlock32& mainIo,
                        std::uint32_t numFrames,
                        bool anyOtherChannelSoloed,
                        bool bypassInsertsAndEQ = false) noexcept;

    [[nodiscard]] const AudioBlock32& pflTapBuffer() const noexcept { return pflTapBuffer_; }
    [[nodiscard]] const AudioBlock32& aflTapBuffer() const noexcept { return aflTapBuffer_; }
    [[nodiscard]] const AudioBlock32& auxSendTapBuffer(std::size_t idx) const noexcept { return auxTapBuffers_[idx]; }
    [[nodiscard]] const AudioBlock32& studioCueTapBuffer(std::size_t idx) const noexcept { return studioCueTapBuffers_[idx]; }
    [[nodiscard]] ChannelMeterSnapshot meterSnapshot() const noexcept;

private:
    std::uint32_t channelId_{0};
    std::string name_;
    MixerChannelKind kind_{MixerChannelKind::AudioChannel};
    SpeakerConfig config_{SpeakerConfig::Stereo};
    double sampleRate_{48000.0};
    std::uint32_t maxBlockFrames_{4096};
    bool visibleInProject_{true}; // InputChannel visible in both Console and Project window

    bool phaseReversed_{false};
    float inputGainDb_{0.0f};
    float faderDb_{0.0f};
    bool muted_{false};
    bool soloed_{false};
    bool soloDefeat_{false};
    bool listenEnabled_{false};

    std::array<AudioInsertSlot, kNumAudioInserts> inserts_{};
    ChannelEQ eq_;
    std::array<AuxSendSlot, kNumAuxSends> auxSends_{};
    std::array<StudioCueSendSlot, kNumStudioCueSends> studioSends_{};
    ChannelPanner panner_;

    // Pre-allocated tap buffers (Zero allocation in RT)
    AudioBlock32 pflTapBuffer_;
    AudioBlock32 aflTapBuffer_;
    std::array<AudioBlock32, kNumAuxSends> auxTapBuffers_{};
    std::array<AudioBlock32, kNumStudioCueSends> studioCueTapBuffers_{};

    std::atomic<float> prePeakL_{0.0f};
    std::atomic<float> prePeakR_{0.0f};
    std::atomic<float> post1PeakL_{0.0f};
    std::atomic<float> post1PeakR_{0.0f};
    std::atomic<float> post2PeakL_{0.0f};
    std::atomic<float> post2PeakR_{0.0f};
};

// ============================================================================
// VSTMixer: Full Studio Mixing Console
// Capacities: 256 Physical I/O, 256 GroupChannel, 8 FXSend, 64 FXReturn,
//             64 VSTi Rack Slots, 3 Memorizable Views + ExtendedMixerView,
//             and Grouped Channel Copy/Paste.
// ============================================================================
struct MixerViewConfiguration {
    std::string viewName{"All Channels"};
    bool showInputChannels{true};
    bool showAudioChannels{true};
    bool showInstrumentChannels{true};
    bool showMIDIChannels{true};
    bool showGroupChannels{true};
    bool showFXReturns{true};
    bool showOutputChannels{true};
    bool narrowStrips{false};
    bool extendedUpperView{true}; // ExtendedMixerView: Inserts, EQ, Aux & Studio Sends
};

class VSTMixer {
public:
    static constexpr std::size_t kMaxPhysicalIO    = 256;
    static constexpr std::size_t kMaxGroupChannels = 256;
    static constexpr std::size_t kMaxFXReturns     = 64;
    static constexpr std::size_t kMaxVSTiSlots     = 64;
    static constexpr std::size_t kNumSavedViews    = 3;

    VSTMixer(double sampleRate = 48000.0, std::uint32_t maxBlockFrames = 4096);

    ChannelStrip* addInputChannel(const std::string& name, SpeakerConfig cfg = SpeakerConfig::Stereo);
    ChannelStrip* addAudioOrInstrumentChannel(const std::string& name, bool isInstrument,
                                              SpeakerConfig cfg = SpeakerConfig::Stereo);
    ChannelStrip* addGroupChannel(const std::string& name, SpeakerConfig cfg = SpeakerConfig::Stereo);
    ChannelStrip* addFXReturnChannel(const std::string& name, SpeakerConfig cfg = SpeakerConfig::Stereo);
    ChannelStrip* addOutputChannel(const std::string& name, SpeakerConfig cfg = SpeakerConfig::Stereo);

    [[nodiscard]] ChannelStrip* findChannel(std::uint32_t channelId) noexcept;
    [[nodiscard]] const ChannelStrip* findChannel(std::uint32_t channelId) const noexcept;
    [[nodiscard]] const std::vector<std::unique_ptr<ChannelStrip>>& channels() const noexcept { return channels_; }

    // VST 3 Sidechain Wiring between any source strip and target strip insert
    bool routeSidechain(std::uint32_t sourceChannelId,
                        std::uint32_t targetChannelId,
                        std::size_t targetInsertSlotIdx) noexcept;

    // 3 Memorizable Console Views + ExtendedMixerView
    void saveMixerView(std::size_t slot0To2, const MixerViewConfiguration& cfg);
    [[nodiscard]] const MixerViewConfiguration& recallMixerView(std::size_t slot0To2) noexcept;
    [[nodiscard]] std::size_t activeViewIndex() const noexcept { return activeViewIdx_; }
    void setExtendedMixerView(bool enabled) noexcept {
        savedViews_[activeViewIdx_].extendedUpperView = enabled;
    }

    // Grouped Copy/Paste of Channel Settings
    void copyChannelSettings(std::uint32_t sourceChannelId);
    std::size_t pasteSettingsToChannelGroup(std::span<const std::uint32_t> targetChannelIds);

    // Real-time console summing pass
    void processConsoleRT(AudioBlock32& mainMixOut, std::uint32_t numFrames) noexcept;

private:
    double sampleRate_{48000.0};
    std::uint32_t maxBlockFrames_{4096};
    std::uint32_t nextChannelId_{1};
    std::size_t numInputsCount_{0};
    std::size_t numGroupsCount_{0};
    std::size_t numFXReturnsCount_{0};
    std::size_t numOutputsCount_{0};

    std::vector<std::unique_ptr<ChannelStrip>> channels_;
    std::array<MixerViewConfiguration, kNumSavedViews> savedViews_{};
    std::size_t activeViewIdx_{0};
    bool hasClipboard_{false};
    ChannelSettingsSnapshot clipboard_{};
};

} // namespace AlphaStudio::Core
