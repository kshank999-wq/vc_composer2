#include "vcc/interchange/Transposition.h"

#include "MusicXmlCommon.h"

namespace vcc::interchange {

notation::KeySignature written_key(const notation::KeySignature& concert, int semitones)
{
    if (semitones == 0) return concert;
    notation::KeySignature k = concert;
    k.fifths = static_cast<std::int8_t>(detail::normalise_fifths(
        concert.fifths + detail::fifths_delta(-detail::diatonic_for_semitones(semitones), -semitones)));
    return k;
}

notation::Score display_score(const notation::Score& score, bool concert)
{
    // One key per score: the transposition of the first transposing staff
    // decides it (a part's staves transpose alike; a piano's do not at all).
    int semitones = 0;
    for (const auto& st : score.staves())
        if (st.transposition_semitones != 0) {
            semitones = st.transposition_semitones;
            break;
        }
    if (semitones == 0) return score;

    notation::Score out = score;
    if (!concert) {
        for (const auto& change : score.key_signature_changes())
            out.set_key_signature(change.tick, written_key(change.key, semitones));
        return out;
    }
    for (std::size_t s = 0; s < score.staff_count(); ++s) {
        const int t = score.staff(s).transposition_semitones;
        if (t == 0) continue;
        for (const auto& n : score.notes(s)) {
            if (n.is_rest) continue;
            notation::ScoreNote moved = n;
            moved.pitch = detail::transpose_pitch(n.pitch, detail::diatonic_for_semitones(t), t,
                                                  score.key_signature_at(n.start_tick));
            out.modify_note(n.id, moved);
        }
        notation::Staff staff = out.staff(s);
        staff.transposition_semitones = 0;
        out.set_staff(s, staff);
    }
    return out;
}

}  // namespace vcc::interchange
