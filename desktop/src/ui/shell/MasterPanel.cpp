#include "ui/shell/MasterPanel.h"

#include <cmath>

#include "ui/console/ConsoleStyle.h"
#include "ui/console/MixActions.h"

namespace vcc::desktop::shell {

namespace {
constexpr double kTargets[] = {-14.0, -16.0, -23.0, -24.0};
const char* kTargetNames[] = {"Streaming", "Apple", "EBU R 128", "ATSC A/85"};
const char* kChainKinds[] = {"eq", "compressor", "limiter"};
const char* kChainNames[] = {"EQ", "Glue comp", "Limiter"};

juce::String lufs_text(double v)
{
    return v <= audio::LoudnessMeter::kSilence + 1.0 ? juce::String("-INF") : juce::String(v, 1);
}
}  // namespace

MasterPanel::MasterPanel(AppContext& context) : hw::Panel("Master"), context_(context)
{
    for (auto* c : std::initializer_list<juce::Component*>{&integrated_, &momentary_, &short_term_, &true_peak_, &deviation_,
                                                           &progress_, &target_, &reset_, &analyse_, &match_, &dither_, &song_,
                                                           &selection_, &export_})
        addAndMakeVisible(c);
    integrated_.set_caption("INTEGRATED  LUFS");
    momentary_.set_caption("M");
    short_term_.set_caption("S");
    true_peak_.set_caption("TP  dBTP");
    deviation_.set_caption("TO TARGET");
    for (auto* l : {&integrated_, &momentary_, &short_term_, &true_peak_, &deviation_})
        l->set_justification(juce::Justification::centredRight);
    for (auto* k : {&target_, &reset_, &analyse_, &match_, &dither_, &song_, &selection_, &export_}) {
        k->set_legend_size(10.0f);
        k->setWantsKeyboardFocus(false);
    }
    target_.setTooltip("The loudness the master is made for: -14 streaming, -16 Apple, -23 EBU R 128, -24 ATSC (click to change)");
    reset_.setTooltip("Start the integrated reading afresh (it also restarts each time playback starts)");
    analyse_.setTooltip("Render the whole song offline and measure it (integrated LUFS and true peak)");
    match_.setTooltip("Set the master fader so the analysed song meets the target");
    target_.onClick = [this] {
        const double now = context_.session.data().loudness_target;
        std::size_t i = 0;
        while (i < 4 && std::abs(kTargets[i] - now) > 0.01) ++i;
        context_.session.set_loudness_target(kTargets[(i + 1) % 4]);
    };
    reset_.onClick = [this] { reset_meter(); };
    analyse_.onClick = [this] { analyse(); };
    match_.onClick = [this] { match_target(); };
    // The master chain on the master bus: add the processor, else bypass it.
    for (int i = 0; i < 3; ++i) {
        auto k = std::make_unique<hw::Keycap>(kChainNames[i], false);
        k->set_led_dot(true);
        k->set_legend_size(10.0f);
        k->setWantsKeyboardFocus(false);
        k->setTooltip("Click: add or bypass the master's " + juce::String(kChainNames[i]) + "; right-click: open it");
        k->onClick = [this, i] {
            const auto id = master_id();
            if (id.empty()) return;
            if (juce::ModifierKeys::currentModifiers.isPopupMenu()) {
                if (const int at = chain_index(i); at >= 0 && context_.show_insert_window) context_.show_insert_window(id, at);
                return;
            }
            if (const int at = chain_index(i); at >= 0) {
                console::toggle_insert_bypass(context_, id, at);
                return;
            }
            // In chain order: EQ, then the glue compressor, then the limiter last.
            console::begin_gesture();
            context_.session.update_bus(id, std::string("Add master ") + kChainNames[i], [&](project::Bus& b) {
                std::size_t pos = b.inserts.size();
                for (int later = i + 1; later < 3; ++later)
                    for (std::size_t s = 0; s < b.inserts.size(); ++s)
                        if (b.inserts[s].kind == kChainKinds[later]) pos = std::min(pos, s);
                project::InsertSlot slot;
                slot.kind = kChainKinds[i];
                slot.suggestion = false;
                b.inserts.insert(b.inserts.begin() + static_cast<std::ptrdiff_t>(pos), slot);
            });
        };
        addAndMakeVisible(*k);
        chain_.push_back(std::move(k));
    }
    auto group = [this](std::vector<std::unique_ptr<hw::Keycap>>& keys, std::initializer_list<const char*> names, int radio) {
        for (const char* n : names) {
            auto k = std::make_unique<hw::Keycap>(n, true);
            k->setRadioGroupId(radio);
            k->set_legend_size(10.0f);
            k->setWantsKeyboardFocus(false);
            addAndMakeVisible(*k);
            keys.push_back(std::move(k));
        }
    };
    group(formats_, {"WAV", "FLAC", "MP3"}, 0x5101);
    group(bits_, {"16-bit", "24-bit"}, 0x5102);
    group(rates_, {"44.1k", "48k", "96k"}, 0x5103);
    formats_[0]->setToggleState(true, juce::dontSendNotification);
    formats_[2]->setEnabled(false);
    formats_[2]->setTooltip("MP3 export comes later, once the encoder is chosen");
    bits_[1]->setToggleState(true, juce::dontSendNotification);
    rates_[1]->setToggleState(true, juce::dontSendNotification);
    for (auto& k : bits_) k->onStateChange = [this] { dither_.setEnabled(bits_[0]->getToggleState()); };
    dither_.set_led_dot(true);
    dither_.setToggleState(true, juce::dontSendNotification);
    dither_.setEnabled(false);
    dither_.setTooltip("TPDF dither when exporting at 16-bit");
    song_.setRadioGroupId(0x5104);
    selection_.setRadioGroupId(0x5104);
    song_.setToggleState(true, juce::dontSendNotification);
    export_.set_solid(hw::Theme::get().led());
    export_.setComponentID("master_export");
    export_.onClick = [this] {
        if (job_) {
            job_->cancel();
            return;
        }
        const auto& path = context_.session.path();
        juce::File folder = path.empty() ? juce::File::getSpecialLocation(juce::File::userMusicDirectory).getChildFile("VC Composer 2")
                                         : juce::File(juce::String(path)).getParentDirectory().getChildFile("Bounces");
        const bool flac = formats_[1]->getToggleState();
        const auto name = juce::File::createLegalFileName(juce::String(context_.session.title())) + "_master" + (flac ? ".flac" : ".wav");
        chooser_ = std::make_unique<juce::FileChooser>("Export master", folder.getChildFile(name), flac ? "*.flac" : "*.wav");
        chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                              [this](const juce::FileChooser& fc) {
                                  const auto f = fc.getResult();
                                  if (f != juce::File()) export_to(f);
                              });
    };
    listener_ = context_.session.add_listener([this](session::Change c) {
        if (c == session::Change::Document || c == session::Change::Selection) refresh();
    });
    refresh();
    startTimerHz(15);
}

MasterPanel::~MasterPanel()
{
    stopTimer();
    context_.session.remove_listener(listener_);
}

std::string MasterPanel::master_id() const
{
    for (const auto& b : context_.session.data().buses)
        if (b.kind == "master") return b.id;
    return {};
}

int MasterPanel::chain_index(int i) const
{
    const auto* b = context_.session.data().find_bus(master_id());
    if (!b) return -1;
    for (std::size_t s = 0; s < b->inserts.size(); ++s)
        if (b->inserts[s].kind == kChainKinds[i]) return static_cast<int>(s);
    return -1;
}

void MasterPanel::refresh()
{
    const auto& d = context_.session.data();
    const double target = d.loudness_target;
    juce::String name = "Custom";
    for (int i = 0; i < 4; ++i)
        if (std::abs(kTargets[i] - target) < 0.01) name = kTargetNames[i];
    target_.setButtonText("Target " + juce::String(static_cast<int>(target)) + " LUFS");
    target_.set_sublegend(name);
    const auto* b = d.find_bus(master_id());
    for (int i = 0; i < 3; ++i) {
        const int at = chain_index(i);
        const bool on = b && at >= 0 && !b->inserts[static_cast<std::size_t>(at)].bypass && !b->inserts[static_cast<std::size_t>(at)].suggestion;
        chain_[static_cast<std::size_t>(i)]->set_led_colour(at >= 0 && !on ? hw::tok::meter_amber : juce::Colour());
        chain_[static_cast<std::size_t>(i)]->set_led_lit(at >= 0);
    }
    selection_.setEnabled(context_.session.selection().has_range());
    if (!selection_.isEnabled() && selection_.getToggleState()) song_.setToggleState(true, juce::sendNotification);
    match_.setEnabled(analysis_.ok && analysis_.integrated > audio::LoudnessMeter::kSilence + 1.0);
    show_readings();
}

void MasterPanel::add_blocks(const std::vector<audio::LoudnessBlock>& blocks)
{
    for (const auto& b : blocks) meter_.add(b);
    if (!blocks.empty()) show_readings();
}

void MasterPanel::reset_meter()
{
    meter_.reset();
    context_.engine.reset_loudness();
    context_.engine.take_loudness_blocks();
    show_readings();
}

void MasterPanel::show_readings()
{
    const double target = context_.session.data().loudness_target;
    integrated_.set_text(lufs_text(meter_.integrated()));
    momentary_.set_text(lufs_text(meter_.momentary()));
    short_term_.set_text(lufs_text(meter_.short_term()));
    const double tp = meter_.true_peak_db();
    true_peak_.set_text(lufs_text(tp) + (tp > -1.0 ? "  OVER" : ""));
    // How far from the target: the live reading, else the last analysis.
    double reading = meter_.integrated();
    if (reading <= audio::LoudnessMeter::kSilence + 1.0 && analysis_.ok) reading = analysis_.integrated;
    if (reading <= audio::LoudnessMeter::kSilence + 1.0) {
        deviation_.set_text("-");
    } else {
        const double lu = reading - target;
        deviation_.set_text((lu >= 0 ? "+" : "") + juce::String(lu, 1) + " LU");
    }
    repaint();
}

void MasterPanel::timerCallback()
{
    // Live: the master's loudness while it plays; each new play starts afresh.
    const bool playing = context_.engine.playing();
    if (playing && !was_playing_) reset_meter();
    was_playing_ = playing;
    add_blocks(context_.engine.take_loudness_blocks());
    if (job_) {
        if (!job_->step(30.0)) job_done();
        else progress_.set_text((job_is_analysis_ ? "ANALYSING  " : "EXPORTING  ") + juce::String(juce::roundToInt(job_->progress() * 100.0)) + "%");
    }
}

MasterExportRequest MasterPanel::request_for(const juce::File& file) const
{
    MasterExportRequest rq;
    rq.file = file;
    rq.format = formats_[1]->getToggleState() ? MasterExportRequest::Format::Flac : MasterExportRequest::Format::Wav;
    rq.bit_depth = bits_[0]->getToggleState() ? 16 : 24;
    rq.sample_rate = rates_[0]->getToggleState() ? 44100 : rates_[2]->getToggleState() ? 96000 : 48000;
    rq.dither = dither_.getToggleState();
    const auto& sel = context_.session.selection();
    if (selection_.getToggleState() && sel.has_range()) {
        rq.start = sel.from;
        rq.end = sel.to;
    } else {
        rq.start = 0;
        rq.end = std::max<Ticks>(context_.session.content_end(), kTicksPerWhole);
    }
    return rq;
}

void MasterPanel::start(MasterExportRequest rq, bool analysis)
{
    if (job_) return;
    context_.engine.stop();
    job_is_analysis_ = analysis;
    job_ = std::make_unique<MasterExportJob>(context_, std::move(rq));
    export_.setButtonText("Cancel");
    progress_.set_text(analysis ? "ANALYSING" : "EXPORTING");
}

void MasterPanel::analyse()
{
    auto rq = request_for({});
    rq.sample_rate = 48000;
    start(rq, true);
}

void MasterPanel::export_to(const juce::File& file) { start(request_for(file), false); }

void MasterPanel::finish_now()
{
    if (!job_) return;
    while (job_->step(1000.0)) {
    }
    job_done();
}

void MasterPanel::job_done()
{
    if (!job_) return;
    const auto r = job_->result();
    const bool analysis = job_is_analysis_;
    const auto fmt = job_->request();
    job_.reset();
    export_.setButtonText("Export master");
    if (analysis) {
        analysis_ = r;
        progress_.set_text(r.ok ? "SONG  " + lufs_text(r.integrated) + " LUFS  " + lufs_text(r.true_peak) + " dBTP" : juce::String("ANALYSIS FAILED"));
    } else {
        exported_ = r;
        progress_.set_text(r.ok ? juce::String(fmt.format == MasterExportRequest::Format::Flac ? "FLAC  " : "WAV  ") + juce::String(fmt.bit_depth) +
                                      "-BIT  " + juce::String(fmt.sample_rate / 1000.0, fmt.sample_rate % 1000 ? 1 : 0) + "K\n" +
                                      lufs_text(r.integrated) + " LUFS  " + lufs_text(r.true_peak) + " dBTP"
                                : juce::String("EXPORT FAILED"));
        if (context_.status)
            context_.status(r.ok ? "Exported " + r.message + (r.true_peak > -1.0 ? " - over -1 dBTP: a limiter on the master would catch it" : "")
                                 : "Export master: " + r.message);
    }
    refresh();
}

void MasterPanel::match_target()
{
    if (!analysis_.ok || analysis_.integrated <= audio::LoudnessMeter::kSilence + 1.0) {
        if (context_.status) context_.status("Analyse the song first");
        return;
    }
    const auto id = master_id();
    const auto* b = context_.session.data().find_bus(id);
    if (!b) return;
    const double delta = context_.session.data().loudness_target - analysis_.integrated;
    const double gain = juce::jlimit(-24.0, 12.0, b->gain_db + delta);
    context_.session.update_bus(id, "Match loudness target", [gain](project::Bus& bus) { bus.gain_db = gain; });
    // The analysis now describes the old level; what it would read after.
    analysis_.integrated += gain - b->gain_db;
    analysis_.true_peak += gain - b->gain_db;
    if (context_.status)
        context_.status("Master fader " + console::db_text(gain) + " to meet " + juce::String(static_cast<int>(context_.session.data().loudness_target)) +
                        " LUFS" + (analysis_.true_peak > -1.0 ? " - peaks now go over -1 dBTP: add the limiter" : ""));
    refresh();
}

void MasterPanel::resized()
{
    auto c = content();
    integrated_.setBounds(c.removeFromTop(44));
    c.removeFromTop(4);
    auto row = c.removeFromTop(30);
    const int third = (row.getWidth() - 8) / 3;
    momentary_.setBounds(row.removeFromLeft(third));
    row.removeFromLeft(4);
    short_term_.setBounds(row.removeFromLeft(third));
    row.removeFromLeft(4);
    true_peak_.setBounds(row);
    c.removeFromTop(6);
    row = c.removeFromTop(34);
    target_.setBounds(row.removeFromLeft(row.getWidth() * 55 / 100));
    row.removeFromLeft(6);
    deviation_.setBounds(row);
    c.removeFromTop(6);
    row = c.removeFromTop(24);
    reset_.setBounds(row.removeFromLeft(third));
    row.removeFromLeft(4);
    analyse_.setBounds(row.removeFromLeft(third));
    row.removeFromLeft(4);
    match_.setBounds(row);
    c.removeFromTop(18);
    row = c.removeFromTop(24);
    for (std::size_t i = 0; i < chain_.size(); ++i) {
        chain_[i]->setBounds(row.removeFromLeft(i + 1 < chain_.size() ? third : row.getWidth()));
        row.removeFromLeft(4);
    }
    c.removeFromTop(18);
    row = c.removeFromTop(24);
    for (std::size_t i = 0; i < formats_.size(); ++i) {
        formats_[i]->setBounds(row.removeFromLeft(i + 1 < formats_.size() ? third : row.getWidth()));
        row.removeFromLeft(4);
    }
    c.removeFromTop(4);
    row = c.removeFromTop(24);
    const int fifth = (row.getWidth() - 16) / 5;
    for (auto& k : bits_) {
        k->setBounds(row.removeFromLeft(fifth));
        row.removeFromLeft(4);
    }
    for (std::size_t i = 0; i < rates_.size(); ++i) {
        rates_[i]->setBounds(row.removeFromLeft(i + 1 < rates_.size() ? fifth : row.getWidth()));
        row.removeFromLeft(4);
    }
    c.removeFromTop(4);
    row = c.removeFromTop(24);
    dither_.setBounds(row.removeFromLeft(third));
    row.removeFromLeft(4);
    song_.setBounds(row.removeFromLeft(third));
    row.removeFromLeft(4);
    selection_.setBounds(row);
    c.removeFromTop(6);
    export_.setBounds(c.removeFromTop(30));
    c.removeFromTop(6);
    progress_.setBounds(c.removeFromTop(34));
}

void MasterPanel::paintOverChildren(juce::Graphics& g)
{
    auto label = [&](const juce::String& text, const juce::Component& below) {
        hw::paint::silk(g, text, {static_cast<float>(below.getX()), static_cast<float>(below.getY() - 15), 200.0f, 12.0f}, 9.5f);
    };
    label("Master chain", *chain_.front());
    label("Export", *formats_.front());
    // Over -1 dBTP: the true-peak LCD gets a red edge.
    if (meter_.true_peak_db() > -1.0) {
        g.setColour(hw::tok::rec);
        g.drawRoundedRectangle(true_peak_.getBounds().toFloat().reduced(0.5f), 4.0f, 1.5f);
    }
}

}  // namespace vcc::desktop::shell
