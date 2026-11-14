#include "ui/console/MixActions.h"

#include <algorithm>
#include <map>

#include "ui/console/ConsoleStyle.h"

namespace vcc::desktop::console {

namespace {
/// Menu results arrive later; the control may be gone by then.
template <typename F> std::function<void(int)> guarded(juce::Component& target, F f)
{
    return [alive = juce::Component::SafePointer<juce::Component>(&target), f](int r) {
        if (alive) f(r);
    };
}

std::string owner_name(const AppContext& context, const std::string& id)
{
    const auto& d = context.session.data();
    if (const auto* t = d.find_track(id)) return t->name;
    if (const auto* b = d.find_bus(id)) return b->name;
    return id;
}
}  // namespace

juce::String insert_name(const project::InsertSlot& s)
{
    if (s.kind == "plugin") return s.plugin.name.empty() ? juce::String(s.plugin_ref) : juce::String(s.plugin.name);
    for (const auto& [k, n] : builtin_inserts())
        if (k == s.kind) return n;
    return juce::String(s.kind).toUpperCase();
}

const std::vector<std::pair<std::string, const char*>>& builtin_inserts()
{
    static const std::vector<std::pair<std::string, const char*>> list{{"eq", "EQ"},         {"compressor", "Compressor"},
                                                                       {"gate", "Gate"},     {"limiter", "Limiter"},
                                                                       {"reverb", "Reverb"}, {"delay", "Delay"}};
    return list;
}

bool is_track(const AppContext& context, const std::string& owner_id)
{
    return context.session.data().find_track(owner_id) != nullptr;
}

const std::vector<project::InsertSlot>* inserts_of(const AppContext& context, const std::string& owner_id)
{
    const auto& d = context.session.data();
    if (const auto* t = d.find_track(owner_id)) return &t->inserts;
    if (const auto* b = d.find_bus(owner_id)) return &b->inserts;
    return nullptr;
}

void edit_owner(AppContext& context, const std::string& owner_id, const std::string& label,
                const std::function<void(double&, double&, std::vector<project::InsertSlot>&, std::vector<project::Send>&)>& change)
{
    if (is_track(context, owner_id))
        context.session.update_track_mix(owner_id, label, [&](project::Track& t) { change(t.gain_db, t.pan, t.inserts, t.sends); });
    else
        context.session.update_bus(owner_id, label, [&](project::Bus& b) { change(b.gain_db, b.pan, b.inserts, b.sends); });
}

void toggle_insert_bypass(AppContext& context, const std::string& owner_id, int index)
{
    const auto* list = inserts_of(context, owner_id);
    if (!list || index < 0 || index >= static_cast<int>(list->size())) return;
    const auto at = static_cast<std::size_t>(index);
    const bool v = !(*list)[at].bypass;
    begin_gesture();
    edit_owner(context, owner_id, (v ? "Bypass insert " : "Activate insert ") + owner_name(context, owner_id) + gesture_suffix(),
               [&](double&, double&, std::vector<project::InsertSlot>& ins, std::vector<project::Send>&) {
                   if (at < ins.size()) ins[at].bypass = v;
               });
}

void show_insert_menu(AppContext& context, const std::string& owner_id, int index, juce::Component& target)
{
    const auto* list = inserts_of(context, owner_id);
    if (!list) return;
    const bool used = index < static_cast<int>(list->size());
    const auto at = static_cast<std::size_t>(std::max(0, index));
    // Built-in processors (100+) and the scanned effect plug-ins (1000+),
    // grouped by manufacturer.
    juce::Array<juce::PluginDescription> effects;
    for (const auto& d : context.plugins.known().getTypes())
        if (!d.isInstrument) effects.add(d);
    std::sort(effects.begin(), effects.end(), [](const juce::PluginDescription& a, const juce::PluginDescription& b) {
        return a.manufacturerName == b.manufacturerName ? a.name < b.name : a.manufacturerName < b.manufacturerName;
    });
    auto fill = [&](juce::PopupMenu& menu) {
        for (std::size_t i = 0; i < builtin_inserts().size(); ++i) menu.addItem(100 + static_cast<int>(i), builtin_inserts()[i].second);
        juce::PopupMenu plugins;
        std::map<juce::String, juce::PopupMenu> by_maker;
        for (int i = 0; i < effects.size(); ++i)
            by_maker[effects[i].manufacturerName.isEmpty() ? juce::String("Other") : effects[i].manufacturerName].addItem(1000 + i,
                                                                                                                      effects[i].name);
        for (auto& [maker, sub] : by_maker) plugins.addSubMenu(maker, sub);
        if (effects.isEmpty()) plugins.addItem(-1, "No effect plug-ins scanned yet (Tools > Plug-ins)", false);
        menu.addSubMenu("Effect plug-ins", plugins);
    };
    juce::PopupMenu m, add;
    fill(add);
    if (used) {
        const auto& s = (*list)[at];
        m.addSectionHeader(insert_name(s) + (s.suggestion ? " (suggested)" : ""));
        m.addItem(5, "Open window...");
        if (s.suggestion && s.kind != "plugin") m.addItem(3, "Use the built-in " + insert_name(s));
        m.addItem(1, "Bypass", true, s.bypass);
        m.addSubMenu("Replace with", add);
        if (at > 0) m.addItem(4, "Move up");
        m.addSeparator();
        m.addItem(2, "Remove");
    } else {
        m.addSectionHeader("Add insert");
        fill(m);
    }
    auto& ctx = context;
    const std::string owner = owner_id;
    const std::string name = owner_name(context, owner_id);
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&target), guarded(target, [&ctx, owner, name, at, used, effects](int r) {
        if (r <= 0) return;
        if (r == 5) {
            if (ctx.show_insert_window) ctx.show_insert_window(owner, static_cast<int>(at));
            return;
        }
        begin_gesture();
        auto edit = [&](const std::string& what, const std::function<void(std::vector<project::InsertSlot>&)>& f) {
            edit_owner(ctx, owner, what + " " + name + gesture_suffix(),
                       [&](double&, double&, std::vector<project::InsertSlot>& ins, std::vector<project::Send>&) { f(ins); });
        };
        if (r >= 1000 && r - 1000 < effects.size()) {
            const auto ref = ctx.plugins.ref_for(effects[r - 1000]);
            edit(used ? "Replace insert" : "Add insert", [&](std::vector<project::InsertSlot>& ins) {
                project::InsertSlot slot;
                slot.kind = "plugin";
                slot.plugin_ref = ref.identifier;
                slot.plugin = ref;
                slot.suggestion = false;
                if (used && at < ins.size()) ins[at] = slot;
                else ins.push_back(slot);
            });
            return;
        }
        if (r >= 100 && r < 1000) {
            const auto& kind = builtin_inserts()[static_cast<std::size_t>(r - 100)];
            edit(used ? "Replace insert" : "Add insert", [&](std::vector<project::InsertSlot>& ins) {
                project::InsertSlot slot;
                slot.kind = kind.first;
                slot.suggestion = false;
                if (used && at < ins.size()) ins[at] = slot;
                else ins.push_back(slot);
            });
            return;
        }
        edit(r == 2 ? "Remove insert" : (r == 4 ? "Move insert" : "Insert"), [&](std::vector<project::InsertSlot>& ins) {
            if (at >= ins.size()) return;
            if (r == 1) ins[at].bypass = !ins[at].bypass;
            else if (r == 2) ins.erase(ins.begin() + static_cast<std::ptrdiff_t>(at));
            else if (r == 3) ins[at].suggestion = false;
            else if (r == 4 && at > 0) std::swap(ins[at], ins[at - 1]);
        });
    }));
}

void show_output_menu(AppContext& context, const std::string& owner_id, juce::Component& target)
{
    const auto& d = context.session.data();
    const bool track = is_track(context, owner_id);
    std::string current;
    if (track) {
        if (const auto* t = d.find_track(owner_id)) current = t->output_bus_id;
    } else if (const auto* b = d.find_bus(owner_id)) {
        current = b->output;
    }
    std::vector<std::string> ids;
    juce::PopupMenu m;
    std::string master_id = "bus-master";
    for (const auto& b : d.buses)
        if (b.kind == "master") master_id = b.id;
    ids.push_back(master_id);
    m.addItem(1, "Master", true, current == master_id || current.empty() || current == "master");
    for (const char* kind : {"group", "fx"}) {
        bool first = true;
        for (const auto& b : d.buses) {
            if (b.kind != kind || b.id == owner_id || b.output == owner_id) continue;
            if (first) m.addSectionHeader(std::string(kind) == "group" ? "Groups" : "FX buses");
            first = false;
            ids.push_back(b.id);
            m.addItem(static_cast<int>(ids.size()), b.name, true, b.id == current);
        }
    }
    auto& ctx = context;
    const std::string owner = owner_id;
    const std::string name = owner_name(context, owner_id);
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&target), guarded(target, [&ctx, owner, name, ids, track](int r) {
        if (r <= 0 || r > static_cast<int>(ids.size())) return;
        const std::string dest = ids[static_cast<std::size_t>(r - 1)];
        begin_gesture();
        if (track) ctx.session.set_output_bus(owner, dest);
        else ctx.session.update_bus(owner, "Output " + name + gesture_suffix(), [&](project::Bus& b) { b.output = dest; });
    }));
}

void show_input_menu(AppContext& context, const std::string& track_id, juce::Component& target)
{
    const auto* t = context.session.data().find_track(track_id);
    if (!t) return;
    juce::PopupMenu m;
    const int n = context.engine.num_inputs();
    m.addItem(1, "No input", true, t->input < 0);
    if (n == 0) m.addSectionHeader("No audio input device is open");
    const int count = n > 0 ? n : 2;
    juce::PopupMenu mono, stereo;
    for (int ch = 0; ch < count; ++ch) {
        juce::String label = "In " + juce::String(ch + 1);
        if (n > 0 && context.engine.input_name(ch).isNotEmpty()) label << "  (" << context.engine.input_name(ch) << ")";
        mono.addItem(100 + ch, label, true, t->input == ch && !t->input_stereo);
    }
    for (int ch = 0; ch + 1 < count; ch += 2)
        stereo.addItem(200 + ch, "In " + juce::String(ch + 1) + "-" + juce::String(ch + 2), true, t->input == ch && t->input_stereo);
    m.addSubMenu("Mono", mono);
    m.addSubMenu("Stereo", stereo);
    auto& ctx = context;
    const std::string id = track_id;
    const std::string name = t->name;
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&target), guarded(target, [&ctx, id, name](int r) {
        if (r <= 0) return;
        begin_gesture();
        ctx.session.update_track_mix(id, "Input " + name + gesture_suffix(), [&](project::Track& x) {
            if (r == 1) {
                x.input = -1;
            } else if (r >= 200) {
                x.input = r - 200;
                x.input_stereo = true;
            } else {
                x.input = r - 100;
                x.input_stereo = false;
            }
        });
    }));
}

juce::String input_text(const AppContext& context, const std::string& track_id)
{
    const auto* t = context.session.data().find_track(track_id);
    if (!t) return {};
    if (t->input < 0) return "NO INPUT";
    return "IN " + juce::String(t->input + 1) + (t->input_stereo ? "-" + juce::String(t->input + 2) : juce::String());
}

juce::String output_text(const AppContext& context, const std::string& owner_id)
{
    const auto& d = context.session.data();
    std::string out;
    if (const auto* t = d.find_track(owner_id)) out = t->output_bus_id;
    else if (const auto* b = d.find_bus(owner_id)) {
        if (b->kind == "master") return "OUT  DEVICE";
        out = b->output;
    }
    const auto* bus = d.find_bus(out);
    return "OUT  " + (bus && bus->kind != "master" ? juce::String(bus->name).toUpperCase() : juce::String("MASTER"));
}

// --- Writing a mix parameter -------------------------------------------------------------------

MixWriter::~MixWriter()
{
    for (const auto& p : touching_) context_.engine.end_touch(owner_, p);
}

void MixWriter::set_owner(std::string owner_id)
{
    if (owner_id == owner_) return;
    for (const auto& p : touching_) context_.engine.end_touch(owner_, p);
    touching_.clear();
    owner_ = std::move(owner_id);
}

void MixWriter::begin(const std::string& param)
{
    begin_gesture();
    if (owner_.empty() || !context_.engine.writes_automation(owner_, param)) return;
    context_.engine.begin_touch(owner_, param);
    touching_.insert(param);
}

void MixWriter::write(const std::string& param, double value, const std::string& label,
                      const std::function<void(double&, double&, std::vector<project::Send>&)>& apply)
{
    if (owner_.empty()) return;
    if (touching_.count(param)) {
        context_.engine.touch_value(owner_, param, value);
        return;
    }
    edit_owner(context_, owner_, label + " " + owner_name(context_, owner_) + gesture_suffix(),
               [&](double& gain, double& pan, std::vector<project::InsertSlot>&, std::vector<project::Send>& sends) {
                   apply(gain, pan, sends);
               });
}

void MixWriter::end(const std::string& param)
{
    if (touching_.erase(param) != 0) context_.engine.end_touch(owner_, param);
}

}  // namespace vcc::desktop::console
