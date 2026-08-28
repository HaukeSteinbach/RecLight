#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
constexpr int kControlPort = 4300;

// Settings persist across plugin delete/reinstall in every DAW.
// Stored in ~/Library/Application Support/RecLight/RecLight.settings
static juce::PropertiesFile& appSettings()
{
    static juce::ApplicationProperties ap;
    static bool initialised = false;
    if (!initialised)
    {
        juce::PropertiesFile::Options o;
        o.applicationName     = "RecLight";
        o.filenameSuffix      = "settings";
        o.osxLibrarySubFolder = "Application Support";
        ap.setStorageParameters (o);
        initialised = true;
    }
    return *ap.getUserSettings();
}

juce::String buildProvisioningMessage (const juce::String& ssid,
                                       const juce::String& password)
{
    return "CFG:1\nSSID:" + ssid + "\nPASS:" + password;
}

bool writeDatagram (juce::DatagramSocket& socket,
                    const juce::String& ip,
                    int port,
                    const juce::String& message)
{
    if (ip.trim().isEmpty() || port <= 0)
        return false;

    return socket.write (ip, port, message.toRawUTF8(), (int) message.getNumBytesAsUTF8()) > 0;
}
}

void OnAirAudioProcessor::loadFromDisk()
{
    auto& p = appSettings();
    if (p.containsKey ("ip") && p.getValue ("ip").isNotEmpty())
        targetIp = p.getValue ("ip");
    if (p.containsKey ("ssid"))   wifiSsid     = p.getValue ("ssid");
    if (p.containsKey ("pass"))   wifiPassword = p.getValue ("pass");
    if (p.containsKey ("bright"))
        brightness.store (juce::jlimit (kBrightnessMin, kBrightnessMax,
                                        p.getIntValue ("bright", kBrightnessMax)));
    if (p.containsKey ("lampMode"))
        lampMode.store (p.getIntValue ("lampMode", Classic) == Pulse ? Pulse : Classic);
}

void OnAirAudioProcessor::saveToDisk()
{
    auto& p = appSettings();
    p.setValue ("ip",     targetIp);
    p.setValue ("ssid",   wifiSsid);
    p.setValue ("pass",   wifiPassword);
    p.setValue ("bright",   brightness.load());
    p.setValue ("lampMode", lampMode.load());
    p.saveIfNeeded();
}

OnAirAudioProcessor::OnAirAudioProcessor()
    : AudioProcessor (BusesProperties()
                        .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                        .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
    // The plug-in's only automatable parameter. Hosts show it in their
    // parameter list, which is where MIDI mapping picks it up.
    addParameter (onParam = new juce::AudioParameterBool (
                      juce::ParameterID { "manualOn", 1 }, "On", false));

    socket.bindToPort (0);
    discoverySocket.bindToPort (4211); // receives ONAIR_IP broadcasts from the ESP
    targetPort = kControlPort;

    // Load saved settings -- also works for a freshly inserted plugin
    // instance with no DAW state yet.
    loadFromDisk();

    constructedAtMs = (int64_t) juce::Time::getMillisecondCounter();
    if (targetIp.isNotEmpty())
        updateSetupStatus ("Searching for RecLight\u2026");
    startTimerHz (10);
}

void OnAirAudioProcessor::sendLampState (LampState state)
{
    lastSentLampState = state;
    juce::String msg;
    switch (state)
    {
        case LampState::Recording: msg = "REC:1";  break;
        case LampState::Playing:   msg = "PLAY:1"; break;
        case LampState::Off:       msg = "REC:0";  break;
    }
    writeDatagram (socket, targetIp, targetPort, msg);
}

void OnAirAudioProcessor::setBrightness (int percent)
{
    const int v = juce::jlimit (kBrightnessMin, kBrightnessMax, percent);
    brightnessUserTouched.store (true);
    if (brightness.exchange (v) != v)
        brightnessDirty.store (true);   // the 10 Hz timer does the actual send
}

void OnAirAudioProcessor::sendBrightness()
{
    const auto msg = "BRIGHT:" + juce::String (brightness.load());

    // Control port for normal operation, config port as well while the device
    // is still on its own setup network -- during first-time setup that's the
    // only port the firmware is listening on for us.
    writeDatagram (socket, targetIp, targetPort, msg);
    writeDatagram (socket, targetIp, setupPort,  msg);
    if (targetIp != setupIp)
        writeDatagram (socket, setupIp, setupPort, msg);
}

void OnAirAudioProcessor::setLampMode (int mode)
{
    const int m = (mode == Pulse) ? Pulse : Classic;
    lampModeUserTouched.store (true);
    if (lampMode.exchange (m) != m)
        lampModeDirty.store (true);
}

void OnAirAudioProcessor::sendLampMode()
{
    const auto msg = "MODE:" + juce::String (lampMode.load());
    writeDatagram (socket, targetIp, targetPort, msg);
    writeDatagram (socket, targetIp, setupPort,  msg);
    if (targetIp != setupIp)
        writeDatagram (socket, setupIp, setupPort, msg);
}

void OnAirAudioProcessor::sendProvisioning()
{
    const auto ssid = wifiSsid.trim();

    if (ssid.isEmpty())
    {
        updateSetupStatus ("SSID missing");
        return;
    }

    const auto message = buildProvisioningMessage (ssid, wifiPassword);
    bool sent = writeDatagram (socket, setupIp, setupPort, message);

    if (targetIp != setupIp)
        sent = writeDatagram (socket, targetIp, setupPort, message) || sent;

    updateSetupStatus (sent ? "WiFi details sent, waiting for ESP confirmation"
                            : "Failed to send");
}

void OnAirAudioProcessor::resetNetwork()
{
    bool sent = false;

    sent = writeDatagram (socket, setupIp, setupPort, "RESET") || sent;
    if (targetIp != setupIp)
        sent = writeDatagram (socket, targetIp, setupPort, "RESET") || sent;

    // Clear all saved settings.
    auto& p = appSettings();
    p.removeValue ("ip");
    p.removeValue ("ssid");
    p.removeValue ("pass");
    p.saveIfNeeded();

    wifiSsid = {};
    wifiPassword = {};
    targetIp = setupIp;

    updateSetupStatus (sent ? "Network reset - ESP is restarting in setup mode"
                            : "Reset sent (no confirmation)");
}

// Timer runs on the message thread: sends UDP when state changes.
// State is updated by processBlock (audio thread).
void OnAirAudioProcessor::timerCallback()
{
    const int64_t nowMs = (int64_t) juce::Time::getMillisecondCounter();

    if (socket.waitUntilReady (false, 0) == 1)
    {
        char buf[128] = {};
        juce::String senderIp;
        int senderPort = 0;
        int len = socket.read (buf, (int) sizeof (buf) - 1, false, senderIp, senderPort);

        if (len > 0)
        {
            juce::String msg (buf, (size_t) len);
            msg = msg.trim();

            if (msg.startsWith ("CFG:OK"))
            {
                targetIp = senderIp;
                lastEspContactMs.store (nowMs);
                espReachable.store (true);
                espLostWarningShown = false;
                updateSetupStatus (msg.fromFirstOccurrenceOf ("CFG:OK", false, false).trim().isNotEmpty()
                                       ? msg.fromFirstOccurrenceOf ("CFG:OK", false, false).trim()
                                       : "ESP connected: " + senderIp);
            }
            else if (msg.startsWith ("ONAIR_IP:"))
            {
                // PING reply from the ESP (comes back on the main socket)
                auto discovered = msg.fromFirstOccurrenceOf ("ONAIR_IP:", false, false).trim();
                if (discovered.isNotEmpty())
                {
                    targetIp   = discovered;
                    lastEspContactMs.store (nowMs);
                    espReachable.store (true);
                    espLostWarningShown = false;
                    saveToDisk();
                    updateSetupStatus ("ESP found (PING): " + discovered);
                }
            }
            else if (msg.startsWith ("BRIGHT:OK"))
            {
                // Only adopt while the user hasn't touched the slider: after
                // that the plugin is the one giving orders.
                if (!brightnessUserTouched.load())
                {
                    const int v = msg.fromFirstOccurrenceOf ("BRIGHT:OK", false, false)
                                     .trim().getIntValue();
                    if (v > 0)
                    {
                        brightness.store (juce::jlimit (kBrightnessMin, kBrightnessMax, v));
                        saveToDisk();
                    }
                }
            }
            else if (msg.startsWith ("MODE:OK"))
            {
                if (!lampModeUserTouched.load())
                {
                    const int m = msg.fromFirstOccurrenceOf ("MODE:OK", false, false)
                                     .trim().getIntValue();
                    lampMode.store (m == Pulse ? Pulse : Classic);
                    saveToDisk();
                }
            }
            else if (msg.startsWith ("CFG:ERR"))
            {
                updateSetupStatus (msg.fromFirstOccurrenceOf ("CFG:ERR", false, false).trim().isNotEmpty()
                                       ? msg.fromFirstOccurrenceOf ("CFG:ERR", false, false).trim()
                                       : "ESP could not join the WiFi network");
            }
        }
    }

    // Auto-discovery: receive ONAIR_IP broadcasts from the ESP (port 4211)
    if (discoverySocket.waitUntilReady (false, 0) == 1)
    {
        char buf[64] = {};
        juce::String senderIp;
        int senderPort = 0;
        int len = discoverySocket.read (buf, (int) sizeof (buf) - 1, false, senderIp, senderPort);
        if (len > 0)
        {
            juce::String msg (buf, (size_t) len);
            if (msg.startsWith ("ONAIR_IP:"))
            {
                auto discovered = msg.fromFirstOccurrenceOf ("ONAIR_IP:", false, false).trim();

                if (discovered.isNotEmpty())
                {
                    targetIp = discovered;
                    lastEspContactMs.store (nowMs);
                    espReachable.store (true);
                    espLostWarningShown = false;
                    saveToDisk(); // persist the IP
                    updateSetupStatus ("ESP found: " + discovered);
                }
            }
        }
    }

    // ── Watchdog + active PING discovery ────────────────────────────────
    // PING to 192.168.4.1:4212 (always reachable in setup WiFi) and to the
    // saved IP. The ESP replies with ONAIR_IP on the same socket. This lets
    // the plugin find the ESP even without broadcast reception (port 4211).
    static int64_t lastPingMs = 0;
    const bool needsPing = targetIp.isEmpty() || !espReachable.load();
    if (needsPing && (nowMs - lastPingMs) > 3000)
    {
        lastPingMs = nowMs;
        writeDatagram (socket, setupIp, setupPort, "PING");      // 192.168.4.1:4212
        if (targetIp.isNotEmpty() && targetIp != setupIp)
            writeDatagram (socket, targetIp, setupPort, "PING"); // saved IP
    }

    if (targetIp.isNotEmpty())
    {
        const int64_t lastContact = lastEspContactMs.load();
        const int64_t reference   = (lastContact > 0) ? lastContact : constructedAtMs;

        if ((nowMs - reference) > kEspTimeoutMs && !espLostWarningShown)
        {
            espReachable.store (false);
            espLostWarningShown = true;
            updateSetupStatus ("RecLight not found\n"
                               "1. Device powered on & on WiFi?\n"
                               "2. Mac on the same WiFi as RecLight?");
        }
    }

    // ── Brightness sync ───────────────────────────────────────────────
    // Ask the device once per (re)discovery what it is set to, then only
    // send when the user actually changes something. The 10 Hz timer is the
    // throttle: dragging a slider can't produce more than ten packets a
    // second no matter how fast the mouse moves.
    const bool nowReachable = espReachable.load();
    if (nowReachable && !wasReachablePrev && targetIp.isNotEmpty())
    {
        if (brightnessUserTouched.load())
            brightnessDirty.store (true);   // push our value back after a device reboot
        else
            writeDatagram (socket, targetIp, setupPort, "BRIGHT?");

        if (lampModeUserTouched.load())
            lampModeDirty.store (true);
        else
            writeDatagram (socket, targetIp, setupPort, "MODE?");
    }
    wasReachablePrev = nowReachable;

    if (brightnessDirty.exchange (false))
    {
        sendBrightness();
        saveToDisk();
    }

    if (lampModeDirty.exchange (false))
    {
        sendLampMode();
        saveToDisk();
    }

    { const juce::ScopedLock l (uiLock); uiIp = targetIp; }

    // The parameter drives everything downstream, whether it was moved by the
    // UI, by automation, or by a mapped MIDI control.
    if (onParam != nullptr)
        manualOn.store (onParam->get());

    const bool wantsOn = manualOn.load();

    // ── Transport detection ───────────────────────────────────────
    // 1500ms tolerance: Ableton can briefly pause on complex sessions or buffer
    // underruns without the lamp flickering off during that pause.
    const int64_t lastBlock = lastProcessBlockMs.load();
    const bool    audioActive = (lastBlock != 0) && (nowMs - lastBlock < 1500);

    // 400ms hold on the recording flag itself: Ableton's isRecording() flag
    // briefly flickers false/true for a handful of blocks right as you stop
    // recording ("abklingen"), which used to make the lamp -- and the REC
    // pill in the plugin UI, since it read the raw flag directly -- blink
    // irregularly for a moment. Bridging that gap here fixes both.
    static constexpr int64_t kRecordingHoldMs = 400;
    const int64_t lastRecTrue = lastRecordingTrueMs.load();
    const bool isPlaying   = audioActive && lastIsPlaying.load();
    const bool isRecording = audioActive && lastRecTrue != 0 && (nowMs - lastRecTrue < kRecordingHoldMs);

    lampIsPlaying.store (isPlaying);
    lampIsRecording.store (isRecording);

    LampState currentLampState = LampState::Off;
    if (isRecording)     currentLampState = LampState::Recording;
    else if (isPlaying)  currentLampState = LampState::Playing;

    // ── Manual toggle: LED permanently on ────────────────────────────
    if (wantsOn)
    {
        if (ledMode != LedMode::ManualOn)
        {
            ledMode = LedMode::ManualOn;
            // REC, not PLAY. The manual toggle is the only way to see the
            // lamp without running a transport, and it is labelled REC in the
            // UI -- sending PLAY made it show the solid "playing" appearance
            // instead, so switching Classic/Pulse looked like it did nothing.
            sendLampState (LampState::Recording);
        }
        // Heartbeat re-sends while ManualOn to recover from ESP restart / packet loss.
        if (targetIp.isNotEmpty() && ++heartbeatTick >= 50)
        {
            heartbeatTick = 0;
            sendLampState (lastSentLampState);
        }
        return;
    }

    // Toggle just switched off -> send the current transport state
    if (ledMode == LedMode::ManualOn)
    {
        ledMode = LedMode::Normal;
        prevLampState = currentLampState;
        sendLampState (currentLampState);
        return;
    }

    if (currentLampState != prevLampState)
    {
        sendLampState (currentLampState);
        prevLampState = currentLampState;
    }

    // ── Heartbeat: resend state every 5s ──────────────────────────────
    // Restores the last state after an ESP restart or network change.
    if (targetIp.isNotEmpty() && ++heartbeatTick >= 50)
    {
        heartbeatTick = 0;
        sendLampState (lastSentLampState);
    }
}

void OnAirAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                        juce::MidiBuffer&)
{
    juce::ignoreUnused (buffer);
    lastProcessBlockMs.store (juce::Time::getMillisecondCounter());

    // isPlaying || isRecording covers every DAW:
    // - Ableton: only sets isPlaying reliably
    // - Cubase/Reaper (VST3): set isRecording correctly
    // - Both together = "transport running or recording active"
    bool isPlay = false, isRec = false;
    if (auto* ph = getPlayHead())
    {
        if (auto pos = ph->getPosition())
        {
            isPlay = pos->getIsPlaying();
            isRec  = pos->getIsRecording();
        }
    }

    // Classic Audio Unit (Logic Pro's / GarageBand's only plugin format) has no
    // host-callback for "isRecording" at all: kAudioUnitProperty_HostCallbacks
    // only ever exposes isPlaying/looping (see JUCE's AU wrapper, which never
    // calls PositionInfo::setIsRecording()). So isRec is *always* false when
    // wrapperType == wrapperType_AudioUnit, no matter what the user does in
    // Logic -- and the firmware doesn't react to plain "PLAY" messages, so the
    // lamp never lit up. Treat transport-rolling as "recording" in that case;
    // it's the closest signal AU can give us and matches what a tally light
    // is for.
    if (wrapperType == wrapperType_AudioUnit)
        isRec = isRec || isPlay;

    lastIsPlaying  .store (isPlay);
    lastIsRecording.store (isRec);
    if (isRec)
        lastRecordingTrueMs.store (juce::Time::getMillisecondCounter());
}

void OnAirAudioProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    juce::ValueTree t ("OnAirState");
    t.setProperty ("ip",       targetIp,     nullptr);
    t.setProperty ("port",     targetPort,   nullptr);
    t.setProperty ("wifiSsid", wifiSsid,     nullptr);
    t.setProperty ("wifiPass", wifiPassword, nullptr);
    t.setProperty ("setupIp",  setupIp,      nullptr);
    t.setProperty ("setupPort", setupPort,   nullptr);
    t.setProperty ("brightness", brightness.load(), nullptr);
    t.setProperty ("lampMode",   lampMode.load(),   nullptr);

    if (auto xml = t.createXml())
        copyXmlToBinary (*xml, dest);
}

void OnAirAudioProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
    {
        auto t = juce::ValueTree::fromXml (*xml);
        if (t.isValid())
        {
            if (t.hasProperty ("ip"))        targetIp     = t.getProperty ("ip").toString();
            if (t.hasProperty ("port"))      targetPort   = (int) t.getProperty ("port");
            if (t.hasProperty ("wifiSsid"))  wifiSsid     = t.getProperty ("wifiSsid").toString();
            if (t.hasProperty ("wifiPass"))  wifiPassword = t.getProperty ("wifiPass").toString();
            if (t.hasProperty ("setupIp"))   setupIp      = t.getProperty ("setupIp").toString();
            if (t.hasProperty ("setupPort")) setupPort    = (int) t.getProperty ("setupPort");
            if (t.hasProperty ("brightness"))
                brightness.store (juce::jlimit (kBrightnessMin, kBrightnessMax,
                                                (int) t.getProperty ("brightness")));
            if (t.hasProperty ("lampMode"))
                lampMode.store ((int) t.getProperty ("lampMode") == Pulse ? Pulse : Classic);
        }
    }

    // If the DAW state has no IP (freshly inserted plugin), load from disk.
    if (targetIp.isEmpty())
        loadFromDisk();
    else
        saveToDisk(); // DAW state is current -- persist to disk

    // Reset watchdog state: the plugin searches for the ESP again.
    // The IP is still saved and reused as soon as the next ONAIR_IP
    // broadcast arrives (max. 10 s).
    constructedAtMs     = (int64_t) juce::Time::getMillisecondCounter();
    lastEspContactMs.store (0);
    espReachable.store (false);
    espLostWarningShown = false;

    if (targetIp.isNotEmpty())
        updateSetupStatus ("Searching for RecLight\u2026");
}

void OnAirAudioProcessor::updateSetupStatus (juce::String newStatus)
{
    setupStatus = std::move (newStatus);
}

juce::AudioProcessorEditor* OnAirAudioProcessor::createEditor()
{
    return new OnAirAudioProcessorEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new OnAirAudioProcessor();
}
