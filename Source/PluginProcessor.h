#pragma once
#include <JuceHeader.h>
#include <atomic>
#include <memory>

class OnAirAudioProcessor : public juce::AudioProcessor,
                            private juce::Timer
{
public:
    enum class LampState { Off, Playing, Recording };

    OnAirAudioProcessor();
    ~OnAirAudioProcessor() override { stopTimer(); sendLampState (LampState::Off); }

    void prepareToPlay (double, int) override {}
    void releaseResources() override {}

    bool isBusesLayoutSupported (const BusesLayout&) const override { return true; }

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "OnAir Recording Light"; }

    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    // Operating network of the ESP - found automatically via ONAIR_IP broadcast.
    // Manual entry only needed as a fallback.
    juce::String targetIp;
    int          targetPort { 4300 };

    // First-time setup: connect the Mac to the ESP's "RecLight Setup" WiFi,
    // then send the SSID/password here.
    juce::String wifiSsid;
    juce::String wifiPassword;
    juce::String setupIp   { "192.168.4.1" };
    int          setupPort { 4212 };

    std::atomic<bool> lastIsPlaying   { false };
    std::atomic<bool> lastIsRecording { false };
    std::atomic<int64_t> lastProcessBlockMs { 0 }; // 0 = never called yet
    // Mirror of the `onParam` below, kept for the audio/timer paths that only
    // need a plain flag. The parameter is the source of truth.
    std::atomic<bool> manualOn { false };

    // The ON toggle, exposed as a real plug-in parameter so the DAW knows it
    // exists: that is what makes it MIDI-mappable and automatable. A plain
    // member variable is invisible to the host -- there is nothing for
    // Ableton's MIDI map mode to latch onto.
    juce::AudioParameterBool* onParam { nullptr };

    // ── Lamp brightness ──────────────────────────────────────────────
    // Percent, mirrored on the device (which persists it in NVS and applies
    // it via PWM). The minimum is 5, not 0: a brightness control that can
    // switch the tally light off entirely just makes a working device look
    // broken.
    static constexpr int kBrightnessMin =   5;
    static constexpr int kBrightnessMax = 100;

    std::atomic<int>  brightness      { kBrightnessMax };
    std::atomic<bool> brightnessDirty { false };  // pending send to the device

    // True once the user has moved the slider in this session. Until then the
    // device -- which is the side that actually persists the value in NVS,
    // and which the setup portal writes to as well -- wins, so a freshly
    // opened plugin shows what the lamp is really doing instead of pushing a
    // stale saved value over it.
    std::atomic<bool> brightnessUserTouched { false };

    // ── Recording lamp mode ──────────────────────────────────────────
    // Classic: the lamp blinks once a second while recording.
    // Pulse:   its brightness eases up and down, never reaching off -- a
    //          tally light has to read as "recording" at any instant.
    // Mirrored on the device exactly like the brightness above, including
    // who wins on startup.
    enum LampMode { Classic = 0, Pulse = 1 };

    std::atomic<int>  lampMode            { Classic };
    std::atomic<bool> lampModeDirty       { false };
    std::atomic<bool> lampModeUserTouched { false };

    void setLampMode (int mode);

    void setBrightness (int percent);

    // Debounced state actually driving the lamp (set once per timer tick,
    // 10 Hz) -- the UI reads these instead of the raw playhead flags above so
    // the "REC" pill never gets stuck on and always matches what the ESP
    // lamp is doing (see timerCallback()).
    std::atomic<bool> lampIsRecording { false };
    std::atomic<bool> lampIsPlaying   { false };

    // Last time processBlock() observed isRecording==true (audio thread).
    // Used to hold the Recording state briefly through the tiny gaps some
    // DAWs (Ableton) leave in the recording flag right as recording stops,
    // which otherwise made the lamp/UI flicker rapidly during that moment.
    std::atomic<int64_t> lastRecordingTrueMs { 0 };

    // ESP reachability (updated via ONAIR_IP broadcast every 10s)
    std::atomic<int64_t> lastEspContactMs { 0 }; // ms timestamp of last contact
    std::atomic<bool>    espReachable      { false };

    void sendLampState (LampState state);
    void sendBrightness();
    void sendLampMode();
    void sendProvisioning();
    void resetNetwork();
    const juce::String& getSetupStatus() const noexcept { return setupStatus; }

    // targetIp is written from the timer thread and read from the message
    // thread by the editor. Rather than lock every write site, the timer
    // publishes one copy per tick that the UI can read safely.
    juce::String getDeviceIp() const { const juce::ScopedLock l (uiLock); return uiIp; }
    bool isEspUnreachableWarning() const noexcept { return espLostWarningShown; }

private:
    void timerCallback() override;

    void updateSetupStatus (juce::String newStatus);
    void loadFromDisk();   // Loads saved settings from the PropertiesFile
    void saveToDisk();     // Persists current settings

    enum class LedMode { Normal, ManualOn };
    LedMode ledMode      { LedMode::Normal };
    LampState prevLampState { LampState::Off };
    juce::String setupStatus { "Setup: connect this Mac to the 'RecLight Setup' WiFi" };

    juce::DatagramSocket socket          { true };
    juce::DatagramSocket discoverySocket { true }; // listens on port 4211 for ONAIR_IP broadcasts

    int64_t constructedAtMs     { 0 };    // set in the ctor and in setStateInformation
    bool    espLostWarningShown { false }; // only accessed from the timer thread
    bool    wasReachablePrev    { false }; // ditto -- rising edge of espReachable

    juce::CriticalSection uiLock;
    juce::String          uiIp;   // guarded by uiLock, see getDeviceIp()
    static constexpr int64_t kEspTimeoutMs = 35000LL; // 3.5x the broadcast interval (10s)

    LampState lastSentLampState { LampState::Off }; // last state sent to the ESP
    int  heartbeatTick  { 0 };     // counter for the 5s heartbeat


    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OnAirAudioProcessor)
};
