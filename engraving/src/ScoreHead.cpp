#include "vcc/engraving/ScoreHead.h"

#include "vcc/engraving/Smufl.h"

namespace vcc::engraving {

namespace {
double width_of(const std::string& text, double size) { return static_cast<double>(text.size()) * size * 0.5; }
}  // namespace

double lay_out_head(DisplayList& list, const ScoreHead& head, const std::string& title, const std::string& subtitle,
                    const std::string& composer, double title_size, double page_width, double margin, double y,
                    double copyright_baseline)
{
    auto add_text = [&](const std::string& text, double size, double x, double baseline, bool bold, bool italic) {
        TextItem t;
        t.text = text;
        t.size = size;
        t.bold = bold;
        t.italic = italic;
        t.origin = Point{x, baseline};
        t.role = ItemRole::Text;
        list.add(t);
    };
    const double small = title_size * 0.38;
    const double right_edge = page_width - margin;
    const bool any = !title.empty() || !subtitle.empty() || !composer.empty();
    if (!head.info_line.empty()) {
        add_text(head.info_line, small, margin, y + small, false, false);
        y += small * 1.4;
    }
    if (!title.empty()) {
        add_text(title, title_size, page_width * 0.5 - width_of(title, title_size) * 0.5, y + title_size, head.title_bold,
                 head.title_italic);
        y += title_size * 1.6;
    }
    if (!subtitle.empty()) {
        const double s = title_size * 0.55;
        add_text(subtitle, s, page_width * 0.5 - width_of(subtitle, s) * 0.5, y + s, false, true);
        y += s * 1.8;
    }
    if (!composer.empty() || !head.lyricist.empty()) {
        const double s = title_size * 0.45;
        if (!head.lyricist.empty()) add_text(head.lyricist, s, margin, y + s, false, false);
        if (!composer.empty()) add_text(composer, s, right_edge - width_of(composer, s), y + s, false, false);
        y += s * 1.4;
    }
    if (!head.arranger.empty()) {
        add_text(head.arranger, small, right_edge - width_of(head.arranger, small), y + small, false, true);
        y += small * 1.4;
    }
    if (!head.tempo_text.empty() || head.tempo_bpm > 0.0) {
        const double s = title_size * 0.42;
        double x = margin;
        const double baseline = y + s * 1.2;
        if (!head.tempo_text.empty()) {
            add_text(head.tempo_text, s, x, baseline, true, false);
            x += width_of(head.tempo_text, s) + s * 0.8;
        }
        if (head.tempo_bpm > 0.0) {
            GlyphItem note;
            note.glyph = Glyph::MetNoteQuarterUp;
            note.origin = Point{x, baseline};
            note.role = ItemRole::Text;
            note.bounds = Rect{x, baseline - 2.5, 1.2, 3.0};
            list.add(note);
            add_text("= " + std::to_string(static_cast<int>(head.tempo_bpm + 0.5)), s, x + 1.6, baseline, false, false);
        }
        y += s * 1.8;
    }
    if (!head.copyright.empty())
        add_text(head.copyright, small, page_width * 0.5 - width_of(head.copyright, small) * 0.5, copyright_baseline, false, true);
    return any ? y : y;
}

}  // namespace vcc::engraving
