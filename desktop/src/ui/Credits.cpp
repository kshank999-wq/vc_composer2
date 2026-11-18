#include "ui/Credits.h"

#include "ui/Dialogs.h"
#include "ui/Theme.h"

namespace vcc::desktop {

namespace {

const char* const kMit =
    "Permission is hereby granted, free of charge, to any person obtaining a copy of this software and "
    "associated documentation files (the \"Software\"), to deal in the Software without restriction, including "
    "without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell "
    "copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the "
    "following conditions:\n\n"
    "The above copyright notice and this permission notice shall be included in all copies or substantial "
    "portions of the Software.\n\n"
    "THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT "
    "LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO "
    "EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER "
    "IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR "
    "THE USE OR OTHER DEALINGS IN THE SOFTWARE.";

juce::TabbedComponent*& tabs_slot()
{
    static juce::TabbedComponent* t = nullptr;
    return t;
}

juce::Component::SafePointer<dialogs::ToolWindow>& window_slot()
{
    static juce::Component::SafePointer<dialogs::ToolWindow> w;
    return w;
}

/// One category: its entries as read-only text.
class CategoryPage final : public juce::Component {
public:
    explicit CategoryPage(const CreditCategory& category)
    {
        text_.setMultiLine(true, true);
        text_.setReadOnly(true);
        text_.setCaretVisible(false);
        text_.setScrollbarsShown(true);
        text_.setFont(juce::FontOptions(14.0f));
        text_.setColour(juce::TextEditor::backgroundColourId, colours::panel);
        text_.setColour(juce::TextEditor::textColourId, colours::text);
        text_.setColour(juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
        text_.setTitle("Credits: " + category.name);
        text_.setText(credits_text(category), false);
        addAndMakeVisible(text_);
    }
    void resized() override { text_.setBounds(getLocalBounds().reduced(8)); }

private:
    juce::TextEditor text_;
};

class CreditsPanel final : public juce::Component {
public:
    CreditsPanel()
    {
        setComponentID("credits_panel");
        intro_.setText("VC Composer is built with the work of the people below. Thank you.",
                       juce::dontSendNotification);
        intro_.setColour(juce::Label::textColourId, colours::text);
        addAndMakeVisible(intro_);
        for (const auto& c : credits()) tabs_.addTab(c.name, colours::panel, new CategoryPage(c), true);
        addAndMakeVisible(tabs_);
        tabs_slot() = &tabs_;
        setSize(640, 560);
    }
    ~CreditsPanel() override
    {
        if (tabs_slot() == &tabs_) tabs_slot() = nullptr;
    }
    void paint(juce::Graphics& g) override { g.fillAll(colours::background); }
    void resized() override
    {
        auto r = getLocalBounds().reduced(10);
        intro_.setBounds(r.removeFromTop(26));
        r.removeFromTop(6);
        tabs_.setBounds(r);
    }

private:
    juce::Label intro_;
    juce::TabbedComponent tabs_{juce::TabbedButtonBar::TabsAtTop};
};

}  // namespace

std::vector<CreditCategory> credits()
{
    std::vector<CreditCategory> out;

    out.push_back({"Sounds",
                   {{"Salamander Grand Piano (V3)", "Alexander Holm",
                     "Creative Commons Attribution 3.0 (creativecommons.org/licenses/by/3.0)",
                     "The built-in piano: a Yamaha C5 grand. Recorded and published by Alexander Holm; SFZ+FLAC "
                     "edition and retuning by the FreePats project (freepats.zenvoid.org).\n"
                     "Changed for VC Composer: four of its sixteen velocity layers, notes shortened with a fade, "
                     "compressed (Ogg Vorbis) into a SoundFont. The original is not endorsed by its author in "
                     "this form."},
                    {"MuseScore General (0.2)", "S. Christian Collins",
                     "MIT licence",
                     "The built-in General MIDI orchestra and drum kits.\n\n"
                     "FluidR3 (original version) by Frank Wen, Copyright (c) 2000-02.\n"
                     "Mono conversion (FluidR3Mono) by Michael Cowgill, Copyright (c) 2014-17.\n"
                     "Adaptation for MuseScore_General.sf2 by S. Christian Collins, Copyright (c) 2018-19.\n"
                     "Temple Blocks instrument provided by Ethan Winer, Copyright (c) 2002.\n"
                     "Drumline Cymbals provided by Michael Schorsch, Copyright (c) 2016.\n"
                     "Mono version: Copyright (c) 2014-16 Michael Cowgill. Copyright (c) 2000-2002, 2008 Frank Wen.\n"
                     "Fluid was built with the help of Suren M. Seron, Scott Hanan, Steve Aupperle, Chris Gillman, "
                     "Alex Taubr, Chris Prola, Andrew Klenk, Winfried Hubbe, Dylan, Tim, Gort, Uros Katic and "
                     "Ethan Winer.\n\n" +
                         juce::String(kMit)}}});

    out.push_back({"Software",
                   {{"JUCE", "Raw Material Software Limited", "Commercial JUCE 8 licence",
                     "The application framework: windows, audio and MIDI devices, plug-in hosting."},
                    {"TinySoundFont", "Bernhard Schelling", "MIT licence",
                     "Plays the built-in sounds (SoundFont 2 and 3). Copyright (C) 2017-2025 Bernhard Schelling. "
                     "Based on SFZero, Copyright (C) 2012 Steve Folta. VC Composer keeps its samples 16-bit.\n\n" +
                         juce::String(kMit)},
                    {"stb_vorbis", "Sean Barrett", "Public domain (or MIT)",
                     "Decodes the compressed (Ogg Vorbis) built-in sounds."},
                    {"FFmpeg", "The FFmpeg developers", "GNU LGPL 2.1 or later",
                     "Reads and writes picture and audio files. Its libraries ship as separate files beside the "
                     "program and may be replaced with compatible builds. Source: ffmpeg.org/download.html; on "
                     "request VC Software provides the corresponding source for three years."},
                    {"LAME", "The LAME project", "GNU LGPL",
                     "MP3 export, when the LAME library is present; loaded at run time, never linked."},
                    {"VST 3 SDK", "Steinberg Media Technologies GmbH", "VST 3 SDK licence (as distributed with JUCE)",
                     "Hosts VST 3 instruments and effects."}}});

    out.push_back({"Fonts",
                   {{"Bravura", "Steinberg Media Technologies GmbH", "SIL Open Font License 1.1",
                     "The music font (SMuFL): notes, clefs, dynamics and every other symbol on the staff."},
                    {"Inter", "The Inter Project Authors (Rasmus Andersson)", "SIL Open Font License 1.1", "Interface text."},
                    {"Barlow Condensed", "The Barlow Project Authors (Jeremy Tribby)", "SIL Open Font License 1.1", "Headings and the mixer's labels."},
                    {"Share Tech Mono", "Carrois Type Design, Ralph du Carrois", "SIL Open Font License 1.1", "Counters and time code."}}});

    out.push_back({"Trademarks",
                   {{"ASIO", "Steinberg Media Technologies GmbH", "",
                     "ASIO is a trademark and software of Steinberg Media Technologies GmbH, registered in Europe "
                     "and other countries. (Windows, where ASIO support is included.)"},
                    {"VST", "Steinberg Media Technologies GmbH", "",
                     "VST is a trademark of Steinberg Media Technologies GmbH, registered in Europe and other "
                     "countries."},
                    {"Mackie Control", "LOUD Audio, LLC", "",
                     "Mackie Control is a trademark of LOUD Audio, LLC. VC Composer speaks the Mackie Control "
                     "protocol; it is not made or endorsed by LOUD Audio."},
                    {"Other names", "", "",
                     "Other product and company names mentioned in VC Composer are trademarks of their owners and "
                     "are used only to say what works with it."}}});
    return out;
}

juce::String credits_text(const CreditCategory& category)
{
    juce::String s;
    for (const auto& e : category.entries) {
        s << e.title << "\n";
        if (e.by.isNotEmpty()) s << "By " << e.by << "\n";
        if (e.licence.isNotEmpty()) s << "Licence: " << e.licence << "\n";
        s << "\n" << e.text << "\n\n------------------------------------------------------------\n\n";
    }
    return s.trimEnd().trimCharactersAtEnd("-").trimEnd();
}

void show_credits_window()
{
    dialogs::show_tool(window_slot(), "Credits", 640, 560, [] { return new CreditsPanel(); });
}

juce::TabbedComponent* credits_tabs() { return tabs_slot(); }

}  // namespace vcc::desktop
