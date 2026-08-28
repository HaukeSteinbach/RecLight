#include "PluginEditor.h"

// One height for both pages. Switching pages could resize the editor
// instead, but hosts handle a plug-in that changes its own size unevenly,
// and the jump reads as a glitch.
static constexpr int W      = 360;
static constexpr int H      = 268;
static constexpr int PAD    = 18;
static constexpr int CONT_R = W - PAD;   // right edge of the content

// Letter-spacing of the mono eyebrows, as a fraction of the font height
// (0.18em on the site).
static constexpr float TRACK = 0.18f;

// =============================================================================
//  OALook helpers
// =============================================================================
namespace OALook
{
juce::Font fat (float height)
{
    // Archivo Black can't be shipped with a plug-in binary the way the site
    // self-hosts it, so this uses the fallback the site's own stylesheet
    // names. Present on macOS and Windows alike.
    return juce::Font (juce::FontOptions ("Arial Black", height, juce::Font::plain));
}

juce::Font mono (float height)
{
    return juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                          height, juce::Font::plain));
}

float trackedWidth (const juce::String& text, const juce::Font& f, float tracking)
{
    if (text.isEmpty())
        return 0.0f;

    const float extra = f.getHeight() * tracking;
    float total = 0.0f;
    for (auto c : text)
        total += juce::GlyphArrangement::getStringWidth (f, juce::String::charToString (c)) + extra;
    return total - extra;   // no trailing gap after the last glyph
}

void drawTracked (juce::Graphics& g, const juce::String& text,
                  juce::Rectangle<int> area, const juce::Font& f,
                  float tracking, juce::Justification just)
{
    g.setFont (f);

    const float extra = f.getHeight() * tracking;
    const float total = trackedWidth (text, f, tracking);

    float x = (float) area.getX();
    if (just.testFlags (juce::Justification::horizontallyCentred))
        x += ((float) area.getWidth() - total) * 0.5f;
    else if (just.testFlags (juce::Justification::right))
        x += (float) area.getWidth() - total;

    const float baseline = (float) area.getCentreY() + f.getHeight() * 0.34f;

    for (auto c : text)
    {
        const auto s = juce::String::charToString (c);
        g.drawSingleLineText (s, juce::roundToInt (x), juce::roundToInt (baseline));
        x += juce::GlyphArrangement::getStringWidth (f, s) + extra;
    }
}
} // namespace OALook

// =============================================================================
//  SteinbachLNF
// =============================================================================
SteinbachLNF::SteinbachLNF()
{
    setColour (juce::PopupMenu::backgroundColourId,          OALook::panel);
    setColour (juce::PopupMenu::textColourId,                OALook::grey);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, OALook::accent);
    setColour (juce::PopupMenu::highlightedTextColourId,     OALook::onAcc);
}

void SteinbachLNF::drawLinearSlider (juce::Graphics& g, int x, int y, int w, int h,
                                     float sliderPos, float, float,
                                     juce::Slider::SliderStyle, juce::Slider&)
{
    const float cy = (float) y + (float) h * 0.5f;

    // Hairline track, accent up to the thumb — the same two-tone rule the
    // portal's slider uses, so both surfaces read as one control.
    g.setColour (OALook::hair);
    g.fillRect ((float) x, cy - 1.0f, (float) w, 2.0f);
    g.setColour (OALook::accent);
    g.fillRect ((float) x, cy - 1.0f, sliderPos - (float) x, 2.0f);

    // The thumb is a tick, not a knob: it picks up the rail on the right edge.
    const float tw = 6.0f, th = 22.0f;
    g.fillRect (sliderPos - tw * 0.5f, cy - th * 0.5f, tw, th);
}

juce::Font SteinbachLNF::getComboBoxFont (juce::ComboBox&)      { return OALook::mono (11.0f); }
juce::Font SteinbachLNF::getPopupMenuFont()                     { return OALook::mono (11.0f); }

void SteinbachLNF::drawComboBox (juce::Graphics& g, int w, int h, bool isDown,
                                 int, int, int, int, juce::ComboBox& box)
{
    g.setColour (OALook::panel);
    g.fillRect (0, 0, w, h);

    g.setColour ((isDown || box.isMouseOver()) ? OALook::accent : OALook::hair);
    g.drawRect (0, 0, w, h, 1);

    // Chevron, drawn rather than glyphed so it keeps the hard-edged geometry.
    const float cx = (float) w - 14.0f, cy = (float) h * 0.5f - 1.0f, r = 3.5f;
    juce::Path p;
    p.startNewSubPath (cx - r, cy - r * 0.6f);
    p.lineTo (cx,          cy + r * 0.6f);
    p.lineTo (cx + r,      cy - r * 0.6f);
    g.setColour (isDown ? OALook::accent : OALook::grey3);
    g.strokePath (p, juce::PathStrokeType (1.4f));
}

void SteinbachLNF::positionComboBoxText (juce::ComboBox& box, juce::Label& label)
{
    label.setBounds (11, 0, box.getWidth() - 30, box.getHeight());
    label.setFont (OALook::mono (10.0f));
    label.setColour (juce::Label::textColourId, OALook::grey);
    label.setJustificationType (juce::Justification::centredLeft);
    label.setMinimumHorizontalScale (1.0f);
}

void SteinbachLNF::drawPopupMenuBackground (juce::Graphics& g, int w, int h)
{
    g.fillAll (OALook::panel);
    g.setColour (OALook::hair);
    g.drawRect (0, 0, w, h, 1);
}

void SteinbachLNF::drawPopupMenuItem (juce::Graphics& g, const juce::Rectangle<int>& area,
                                      bool isSeparator, bool isActive, bool isHighlighted,
                                      bool /*isTicked*/, bool /*hasSubMenu*/,
                                      const juce::String& text, const juce::String&,
                                      const juce::Drawable*, const juce::Colour*)
{
    if (isSeparator)
    {
        g.setColour (OALook::hair);
        g.fillRect (area.getX() + 6, area.getCentreY(), area.getWidth() - 12, 1);
        return;
    }

    if (isHighlighted && isActive)
        g.setColour (OALook::accent), g.fillRect (area);

    g.setColour (isHighlighted && isActive ? OALook::onAcc
                                           : (isActive ? OALook::grey : OALook::grey3));
    OALook::drawTracked (g, text.toUpperCase(), area.reduced (10, 0),
                         OALook::mono (11.0f), TRACK * 0.5f);
}

void SteinbachLNF::drawButtonBackground (juce::Graphics& g, juce::Button& b,
                                         const juce::Colour&, bool over, bool)
{
    const auto r = b.getLocalBounds();
    const bool on = b.getToggleState();

    g.setColour (on ? OALook::accent : OALook::panel);
    g.fillRect (r);
    g.setColour (on ? OALook::accent : (over ? OALook::grey3 : OALook::hair));
    g.drawRect (r, 1);
}

void SteinbachLNF::drawButtonText (juce::Graphics& g, juce::TextButton& b, bool, bool)
{
    g.setColour (b.getToggleState() ? OALook::onAcc : OALook::grey2);
    OALook::drawTracked (g, b.getButtonText().toUpperCase(), b.getLocalBounds(),
                         OALook::mono (10.0f), TRACK,
                         juce::Justification::centred);
}

void SteinbachLNF::fillTextEditorBackground (juce::Graphics& g, int w, int h, juce::TextEditor&)
{
    g.setColour (OALook::panel);
    g.fillRect (0, 0, w, h);
}

void SteinbachLNF::drawTextEditorOutline (juce::Graphics& g, int w, int h, juce::TextEditor& ed)
{
    g.setColour (ed.hasKeyboardFocus (true) ? OALook::accent : OALook::hair);
    g.drawRect (0, 0, w, h, 1);
}

// =============================================================================
//  Editor
// =============================================================================
OnAirAudioProcessorEditor::OnAirAudioProcessorEditor (OnAirAudioProcessor& p)
    : AudioProcessorEditor (&p), audioProcessor (p)
{
    setSize (W, H);
    setResizable (false, false);
    setLookAndFeel (&laf);

    testOn.setClickingTogglesState (true);
    testOn.onClick = [this]
    {
        // Driven through the parameter, not the flag: the gesture calls are
        // what let a host record the change as automation and what make the
        // control visible to MIDI learn while it is being moved.
        if (auto* p = audioProcessor.onParam)
        {
            p->beginChangeGesture();
            p->setValueNotifyingHost (testOn.getToggleState() ? 1.0f : 0.0f);
            p->endChangeGesture();
        }
    };
    testOn.setLookAndFeel (&laf);
    addAndMakeVisible (testOn);

    brightness.setRange (OnAirAudioProcessor::kBrightnessMin,
                         OnAirAudioProcessor::kBrightnessMax, 1.0);
    brightness.setValue ((double) audioProcessor.brightness.load(),
                         juce::dontSendNotification);
    brightness.onValueChange = [this]
    {
        audioProcessor.setBrightness ((int) brightness.getValue());
    };
    brightness.setLookAndFeel (&laf);
    addAndMakeVisible (brightness);

    // IDs are 1-based in a ComboBox, the protocol values are 0-based.
    modeBox.addItem ("CLASSIC", OnAirAudioProcessor::Classic + 1);
    modeBox.addItem ("PULSE",   OnAirAudioProcessor::Pulse   + 1);
    modeBox.setSelectedId (audioProcessor.lampMode.load() + 1, juce::dontSendNotification);
    modeBox.onChange = [this] { audioProcessor.setLampMode (modeBox.getSelectedId() - 1); };
    modeBox.setLookAndFeel (&laf);
    addAndMakeVisible (modeBox);

    settingsBtn.setLookAndFeel (&laf);
    settingsBtn.onClick = [this] { setPage (true); };
    addAndMakeVisible (settingsBtn);

    // ── Settings page ────────────────────────────────────────────────
    backBtn.setLookAndFeel (&laf);
    backBtn.onClick = [this] { setPage (false); };
    addChildComponent (backBtn);

    auto initField = [this] (juce::TextEditor& ed, bool password)
    {
        ed.setLookAndFeel (&laf);
        ed.setFont (OALook::mono (12.0f));
        ed.setColour (juce::TextEditor::textColourId,       OALook::grey);
        ed.setColour (juce::TextEditor::highlightColourId,  OALook::accent);
        ed.setColour (juce::TextEditor::highlightedTextColourId, OALook::onAcc);
        ed.setColour (juce::CaretComponent::caretColourId,  OALook::accent);
        ed.setJustification (juce::Justification::centredLeft);
        ed.setIndents (10, 0);
        if (password)
            ed.setPasswordCharacter ((juce_wchar) 0x2022);
        addChildComponent (ed);
    };
    initField (ssidField, false);
    initField (passField, true);
    ssidField.setText (audioProcessor.wifiSsid, juce::dontSendNotification);

    saveBtn.setLookAndFeel (&laf);
    saveBtn.onClick = [this]
    {
        audioProcessor.wifiSsid     = ssidField.getText().trim();
        audioProcessor.wifiPassword = passField.getText();
        audioProcessor.sendProvisioning();
        resetArmed = false;
        repaint();
    };
    addChildComponent (saveBtn);

    resetBtn.setLookAndFeel (&laf);
    resetBtn.setClickingTogglesState (false);
    resetBtn.onClick = [this]
    {
        // Two-step: the first press only arms it. Wiping the device's network
        // settings sends it back to its setup access point, which is a long
        // way to come back from for a mis-click.
        if (! resetArmed)
        {
            resetArmed = true;
            resetBtn.setButtonText ("CONFIRM RESET");
        }
        else
        {
            audioProcessor.resetNetwork();
            resetArmed = false;
            resetBtn.setButtonText ("FACTORY RESET");
            passField.clear();
        }
        repaint();
    };
    addChildComponent (resetBtn);

    setPage (false);
    startTimerHz (10);
}

OnAirAudioProcessorEditor::~OnAirAudioProcessorEditor()
{
    // Every explicit setLookAndFeel has to be undone before `laf` dies.
    for (auto* c : std::initializer_list<juce::Component*> {
             &modeBox, &brightness, &testOn, &settingsBtn, &backBtn,
             &ssidField, &passField, &saveBtn, &resetBtn })
        c->setLookAndFeel (nullptr);
    setLookAndFeel (nullptr);
}

// The headline used to fall back to "IDLE", which only restated the absence
// of the other two states -- it told you nothing you couldn't already see.
// With nothing rolling, the thing worth knowing is whether the plug-in can
// reach the lamp at all, so the link state takes the headline instead.
void OnAirAudioProcessorEditor::setPage (bool settings)
{
    showSettings = settings;

    // Leaving the settings page disarms a half-pressed reset, so it can't be
    // completed by accident on the way back in.
    resetArmed = false;
    resetBtn.setButtonText ("FACTORY RESET");

    for (auto* c : std::initializer_list<juce::Component*> { &testOn, &brightness,
                                                             &modeBox, &settingsBtn })
        c->setVisible (! settings);

    for (auto* c : std::initializer_list<juce::Component*> { &backBtn, &ssidField,
                                                             &passField, &saveBtn, &resetBtn })
        c->setVisible (settings);

    repaint();
}

juce::String OnAirAudioProcessorEditor::stateWord() const
{
    if (audioProcessor.lampIsRecording.load() || audioProcessor.manualOn.load())
        return "REC";
    if (audioProcessor.lampIsPlaying.load())
        return "PLAY";

    if (audioProcessor.espReachable.load())          return "READY";
    return audioProcessor.getDeviceIp().isEmpty() ? "SEARCHING" : "OFFLINE";
}

// One line, three states. The old editor printed the full setup walkthrough
// here ("connect this Mac to the RecLight Setup WiFi", a numbered checklist on
// failure); that guidance now lives where setup actually happens — the device
// display and the web portal — and none of it told you anything while the
// plug-in was simply working.
// Second line. While the transport is rolling it carries the link state,
// because a lamp that isn't reachable is exactly what you'd want to know at
// that moment. Otherwise the headline already says it, and this line is left
// to the device address -- or to nothing at all.
juce::String OnAirAudioProcessorEditor::statusLine() const
{
    const auto ip = audioProcessor.getDeviceIp();
    const bool rolling = audioProcessor.lampIsRecording.load()
                      || audioProcessor.lampIsPlaying.load()
                      || audioProcessor.manualOn.load();

    if (audioProcessor.espReachable.load())
        return ip;

    if (rolling)
        return ip.isEmpty() ? "NO DEVICE" : "DEVICE OFFLINE";

    return {};
}

void OnAirAudioProcessorEditor::timerCallback()
{
    if (showSettings)
    {
        // The provisioning status is the only thing that moves on this page.
        const auto st = audioProcessor.getSetupStatus();
        if (st != lastStatus) { lastStatus = st; repaint(); }
        return;
    }

    // Follow the parameter, so automation and MIDI move the button too.
    const bool on = audioProcessor.onParam != nullptr ? audioProcessor.onParam->get()
                                                      : audioProcessor.manualOn.load();
    if (testOn.getToggleState() != on)
        testOn.setToggleState (on, juce::dontSendNotification);

    // Follow the processor when the value came from elsewhere (the device
    // reported its stored setting, or another plug-in instance changed it).
    // dontSendNotification, or this would echo straight back out as a change.
    const int devBrightness = audioProcessor.brightness.load();
    if (! brightness.isMouseButtonDown() && (int) brightness.getValue() != devBrightness)
        brightness.setValue ((double) devBrightness, juce::dontSendNotification);

    const int devMode = audioProcessor.lampMode.load() + 1;
    if (modeBox.getSelectedId() != devMode)
        modeBox.setSelectedId (devMode, juce::dontSendNotification);

    const auto st = stateWord();
    const auto ln = statusLine();
    if (st != lastState || ln != lastStatus || devBrightness != lastBrightnessShown)
    {
        lastState = st;
        lastStatus = ln;
        lastBrightnessShown = devBrightness;
        repaint();
    }
}

void OnAirAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (OALook::black);

    // ── Kicker: accent block, mono caps ──────────────────────────────
    {
        const auto f = OALook::mono (9.0f);
        const juce::String t = showSettings ? "SETTINGS" : "STEINBACH  RECLIGHT";
        const int bw = juce::roundToInt (OALook::trackedWidth (t, f, 0.20f)) + 22;
        juce::Rectangle<int> block (PAD, PAD, bw, 21);
        g.setColour (OALook::accent);
        g.fillRect (block);
        g.setColour (OALook::onAcc);
        OALook::drawTracked (g, t, block, f, 0.20f, juce::Justification::centred);
    }

    if (showSettings)
    {
        const auto lf = OALook::mono (9.0f);

        g.setColour (OALook::grey3);
        OALook::drawTracked (g, "WIFI NETWORK", { PAD, 56, 180, 12 }, lf, TRACK);
        OALook::drawTracked (g, "PASSWORD",     { PAD, 108, 180, 12 }, lf, TRACK);

        g.setColour (OALook::hair);
        g.fillRect (PAD, 200, CONT_R - PAD, 1);

        // The device's own reply, verbatim: on this page the wording is the
        // point, and it is the only place the plug-in still shows it.
        g.setColour (OALook::grey3);
        g.setFont (OALook::mono (9.5f));
        g.drawFittedText (audioProcessor.getSetupStatus(),
                          { PAD, 244, CONT_R - PAD, 16 },
                          juce::Justification::centredLeft, 1, 0.9f);
        return;
    }

    // ── Poster headline: the transport state ─────────────────────────
    {
        const auto word = stateWord();
        // Sized so the longest state word still fits the measure. Only the
        // two link-failure words are long enough to need the smaller size.
        const auto f = OALook::fat (word.length() > 5 ? 34.0f : 46.0f);
        const bool rec  = (word == "REC");
        const bool play = (word == "PLAY");
        const bool warn = (word == "OFFLINE" || word == "SEARCHING");

        juce::Rectangle<int> row (PAD, 52, CONT_R - PAD, 48);

        g.setFont (f);
        const int tw = juce::roundToInt (
            juce::GlyphArrangement::getStringWidth (f, word));

        if (rec)
        {
            // The site sets emphasis as a solid accent block behind the line,
            // never as coloured text — so REC is the block.
            g.setColour (OALook::accent);
            g.fillRect (row.getX(), row.getY(), tw + 22, row.getHeight());
            g.setColour (OALook::onAcc);
        }
        else
        {
            g.setColour (play ? OALook::grey : (warn ? OALook::grey3 : OALook::grey2));
        }

        g.drawText (word, row.getX() + (rec ? 11 : 0), row.getY(),
                    tw + 24, row.getHeight(), juce::Justification::centredLeft, false);
    }

    // ── Status line ──────────────────────────────────────────────────
    {
        const auto f = OALook::mono (9.0f);
        juce::Rectangle<int> r (PAD, 104, CONT_R - PAD, 14);
        const auto line = statusLine();
        g.setColour (audioProcessor.espReachable.load() ? OALook::grey3 : OALook::accent);
        OALook::drawTracked (g, line, r, f, TRACK);
    }

    g.setColour (OALook::hair);
    g.fillRect (PAD, 126, CONT_R - PAD, 1);

    // ── Row labels ───────────────────────────────────────────────────
    const auto lf = OALook::mono (9.0f);

    g.setColour (OALook::grey3);
    OALook::drawTracked (g, "BRIGHTNESS", { PAD, 178, 120, 12 }, lf, TRACK);

    g.setColour (OALook::accent);
    OALook::drawTracked (g, juce::String (audioProcessor.brightness.load()) + "%",
                         { CONT_R - 60, 178, 60, 12 }, lf, TRACK,
                         juce::Justification::right);

    g.setColour (OALook::grey3);
    OALook::drawTracked (g, "WHILE RECORDING", { PAD, 228, 160, 12 }, lf, TRACK);
}

void OnAirAudioProcessorEditor::resized()
{
    settingsBtn.setBounds (CONT_R - 84, PAD, 84, 21);
    backBtn    .setBounds (CONT_R - 84, PAD, 84, 21);

    ssidField.setBounds (PAD, 70,  CONT_R - PAD, 28);
    passField.setBounds (PAD, 122, CONT_R - PAD, 28);
    saveBtn  .setBounds (PAD, 160, CONT_R - PAD, 30);
    resetBtn .setBounds (PAD, 210, CONT_R - PAD, 26);

    testOn    .setBounds (PAD,        140, 78,  26);
    brightness.setBounds (PAD,        192, CONT_R - PAD, 22);
    modeBox   .setBounds (CONT_R - 116, 222, 116, 24);
}
