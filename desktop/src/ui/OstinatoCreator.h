// Ostinato Creator (Templates Supplement §10): seeded A/B/C variants of a
// repeating figure for a track, each auditionable, variable one aspect at a
// time, and sent to a track as one undo step.
#pragma once

#include <array>
#include <memory>
#include <vector>

#include "ui/AppContext.h"
#include "ui/Dialogs.h"

namespace vcc::desktop {

class OstinatoCreator final : public juce::Component {
public:
    explicit OstinatoCreator(AppContext& context);
    ~OstinatoCreator() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    class Card;

    ostinato::OstinatoOptions build_options();
    std::string preset_id() const;
    void preset_changed();
    void track_changed();
    void generate();
    void new_seed();
    void vary(int index, ostinato::Aspect aspect);
    void audition(int index);
    void stop();
    void send(int index);
    void send_with(const std::string& track_id, const ostinato::Variant& variant, const std::string& preset,
                   const juce::String& what, project::CollisionPolicy policy);
    void update_info();

    AppContext& context_;
    juce::Label track_caption_, preset_caption_, key_caption_, follow_caption_, length_caption_, start_caption_,
        meter_caption_, seed_caption_, density_caption_, swing_caption_;
    dialogs::TrackSelector target_;
    juce::ComboBox preset_, tonic_, mode_, follow_;
    juce::Slider length_, start_, density_, swing_;
    juce::Label meter_;
    juce::TextEditor seed_;
    juce::TextButton new_seed_{"New seed"}, generate_{"Generate"}, stop_{"Stop"};
    juce::ToggleButton evolve_{"Evolve every 4 bars"}, call_response_{"Call and response"};
    juce::Label info_, hint_;
    std::array<std::unique_ptr<Card>, 3> cards_;

    std::vector<std::string> preset_ids_;
    ostinato::OstinatoOptions options_;  ///< What the shown variants were generated with.
    std::vector<ostinato::Variant> variants_;
    std::array<int, 3> vary_count_{};
    juce::String follow_note_;
    int auditioning_ = -1;
};

}  // namespace vcc::desktop
