// What every mix control shares - the console window's strips, the Producer
// view's channel strip and the mixer deck: the insert, output and input
// menus, insert bypass, and writing a fader, pan or send either as an edit
// (one undo step per gesture) or, while an automation pass is being written
// for that parameter, as automation.
#pragma once

#include <functional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "ui/AppContext.h"

namespace vcc::desktop::console {

juce::String insert_name(const project::InsertSlot& s);
/// The built-in processors: kind and name.
const std::vector<std::pair<std::string, const char*>>& builtin_inserts();

bool is_track(const AppContext& context, const std::string& owner_id);
/// A track's or bus's inserts, or null.
const std::vector<project::InsertSlot>* inserts_of(const AppContext& context, const std::string& owner_id);
/// Changes a track's or bus's mix settings; `label` coalesces a gesture.
void edit_owner(AppContext& context, const std::string& owner_id, const std::string& label,
                const std::function<void(double& gain_db, double& pan, std::vector<project::InsertSlot>& inserts,
                                         std::vector<project::Send>& sends)>& change);

/// The insert slot menu (add, replace, bypass, move, remove, open window).
/// `guard`: the menu's result is dropped if it has gone.
void show_insert_menu(AppContext& context, const std::string& owner_id, int index, juce::Component& target);
void toggle_insert_bypass(AppContext& context, const std::string& owner_id, int index);
/// Where a track or bus goes: the master or a group/FX bus.
void show_output_menu(AppContext& context, const std::string& owner_id, juce::Component& target);
/// A track's input: none, a mono input, or a stereo pair.
void show_input_menu(AppContext& context, const std::string& track_id, juce::Component& target);
/// "IN 1-2 › STRINGS" style names for the readouts.
juce::String input_text(const AppContext& context, const std::string& track_id);
juce::String output_text(const AppContext& context, const std::string& owner_id);

/// A gesture on a mix parameter ("volume", "pan", "send:<bus>"): an
/// automation write while the owner's mode writes it, an edit otherwise.
class MixWriter {
public:
    MixWriter(AppContext& context, std::string owner_id) : context_(context), owner_(std::move(owner_id)) {}
    ~MixWriter();
    void set_owner(std::string owner_id);
    /// A gesture starts (mouse down on the control).
    void begin(const std::string& param);
    /// A new value: written as automation when a pass is writing it, else
    /// `apply` edits the document under `label`.
    void write(const std::string& param, double value, const std::string& label,
               const std::function<void(double& gain_db, double& pan, std::vector<project::Send>& sends)>& apply);
    void end(const std::string& param);
    const std::string& owner() const noexcept { return owner_; }

private:
    AppContext& context_;
    std::string owner_;
    std::set<std::string> touching_;
};

}  // namespace vcc::desktop::console
