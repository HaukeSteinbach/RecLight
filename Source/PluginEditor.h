// Copyright (c) 2026 Hauke Steinbach. All rights reserved.
// Published for inspection only, not as open source: no reuse, no derivative
// works, and no use as machine-learning training data. See LICENSE.

#pragma once
#include <JuceHeader.h>
#include "PluginProcessor.h"

// =============================================================================
//  Palette — the studio site's design system (haukesteinbach.de,
//  assets/css/steinbach.css): pure black ground, headline grey rather than
//  white, one flat accent used as a solid block, hairline rules, hard edges.
// =============================================================================
namespace OALook
{
    static const juce::Colour black   { 0xFF000000u };
    static const juce::Colour panel   { 0xFF0B0B0Bu };  // a hair off the ground
    static const juce::Colour hair    { 0xFF232323u };
    static const juce::Colour grey    { 0xFFD6D6D6u };  // deliberately not white
    static const juce::Colour grey2   { 0xFF8C8C8Cu };
    static const juce::Colour grey3   { 0xFF4A4A4Au };
    static const juce::Colour accent  { 0xFFE94560u };
    static const juce::Colour onAcc   { 0xFF000000u };  // reads on the accent
    static const juce::Colour ok      { 0xFF7BE38Bu };

    // Archivo Black isn't installable from a plug-in, so the site's own
    // declared fallback is used instead — the same substitution the web
    // portal makes.
    juce::Font fat  (float height);
    juce::Font mono (float height);

    // Draws text with letter-spacing, which JUCE's Font has no notion of.
    // The mono eyebrows are set at 0.18em on the site and read as a different
    // typeface entirely without it, so it is not a detail worth skipping.
    void drawTracked (juce::Graphics&, const juce::String&, juce::Rectangle<int>,
                      const juce::Font&, float tracking,
                      juce::Justification = juce::Justification::centredLeft);

    float trackedWidth (const juce::String&, const juce::Font&, float tracking);
}

// =============================================================================
//  SteinbachLNF — every control is drawn here rather than tinted. JUCE's
//  stock slider and combo box carry rounded corners, gradients and a drop
//  shadow; recolouring those still reads as a JUCE plug-in, not as the site.
// =============================================================================
class SteinbachLNF : public juce::LookAndFeel_V4
{
public:
    SteinbachLNF();

    void drawLinearSlider (juce::Graphics&, int x, int y, int w, int h,
                           float sliderPos, float minPos, float maxPos,
                           juce::Slider::SliderStyle, juce::Slider&) override;

    void drawComboBox (juce::Graphics&, int w, int h, bool isDown,
                       int buttonX, int buttonY, int buttonW, int buttonH,
                       juce::ComboBox&) override;
    juce::Font getComboBoxFont (juce::ComboBox&) override;
    void positionComboBoxText (juce::ComboBox&, juce::Label&) override;

    void drawPopupMenuBackground (juce::Graphics&, int w, int h) override;
    void drawPopupMenuItem (juce::Graphics&, const juce::Rectangle<int>&,
                            bool isSeparator, bool isActive, bool isHighlighted,
                            bool isTicked, bool hasSubMenu,
                            const juce::String& text,
                            const juce::String& shortcutKeyText,
                            const juce::Drawable*, const juce::Colour*) override;
    juce::Font getPopupMenuFont() override;

    void drawButtonBackground (juce::Graphics&, juce::Button&,
                               const juce::Colour&, bool over, bool down) override;
    void drawButtonText (juce::Graphics&, juce::TextButton&,
                         bool over, bool down) override;

    void fillTextEditorBackground (juce::Graphics&, int w, int h, juce::TextEditor&) override;
    void drawTextEditorOutline   (juce::Graphics&, int w, int h, juce::TextEditor&) override;
};

// =============================================================================
//  OnAirAudioProcessorEditor
// =============================================================================
class OnAirAudioProcessorEditor : public juce::AudioProcessorEditor,
                                   private juce::Timer
{
public:
    explicit OnAirAudioProcessorEditor (OnAirAudioProcessor&);
    ~OnAirAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    OnAirAudioProcessor& audioProcessor;

    SteinbachLNF laf;   // declared before the child components that use it

    // ── Main page ────────────────────────────────────────────────────
    juce::TextButton testOn { "ON" };
    juce::Slider     brightness { juce::Slider::LinearHorizontal,
                                  juce::Slider::NoTextBox };
    juce::ComboBox   modeBox;
    juce::TextButton settingsBtn { "SETTINGS" };

    // ── Settings page ────────────────────────────────────────────────
    // Shown in place of the main page rather than in a separate window: a
    // plug-in editor that spawns its own window gets lost behind the DAW.
    bool             showSettings { false };
    juce::TextButton backBtn  { "BACK" };
    juce::TextEditor ssidField, passField;
    juce::TextButton saveBtn  { "SEND TO DEVICE" };
    juce::TextButton resetBtn { "FACTORY RESET" };

    // The reset wipes the device's network settings, so it asks once: the
    // first click arms it, the second carries it out.
    bool resetArmed { false };

    void setPage (bool settings);

    // Repaint only when something visible actually changed: the timer runs at
    // 10 Hz and a full repaint of the poster headline every tick is wasted
    // work in every session that isn't recording.
    juce::String lastState, lastStatus;
    int          lastBrightnessShown { -1 };

    juce::String stateWord() const;
    juce::String statusLine() const;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OnAirAudioProcessorEditor)
};
