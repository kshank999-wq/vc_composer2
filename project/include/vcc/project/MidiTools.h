// MIDI tools for played-in MIDI (regions): quantize and velocity changes.
// Pure functions over notes, so the session, the recorder (input quantize)
// and the tests share one implementation. Ticks are absolute.
#pragma once

#include <vector>

#include "vcc/core/Midi.h"
#include "vcc/core/Time.h"

namespace vcc::project {

struct QuantizeSettings {
    Ticks grid = 0;          ///< Grid step; 0 = off.
    double strength = 1.0;   ///< 0..1: how far each note moves towards its grid point.
    /// 0..1: every second grid point is late by this share of a grid step
    /// (0 straight, about 0.33 a triplet feel).
    double swing = 0.0;
    bool ends = false;       ///< Also quantize where notes end (never shorter than half a step).
};

/// The grid point nearest `t` (with swing applied to every second point).
Ticks quantize_tick(Ticks t, const QuantizeSettings& q);

/// Quantizes the notes in place; returns how many moved. Afterwards no two
/// notes of one pitch and channel overlap (the earlier is shortened; an
/// exact duplicate is dropped).
int quantize_notes(std::vector<NoteEvent>& notes, const QuantizeSettings& q);

struct VelocityChange {
    double scale = 1.0;  ///< Multiplies the velocity...
    int offset = 0;      ///< ...then adds this...
    int set = -1;        ///< ...or, 1..127, sets every velocity to it.
    /// 0..1: pulls every velocity towards the notes' average by this much
    /// (0 none, 1 all the same), before scale and offset.
    double compress = 0.0;
};

/// Applies a velocity change (results kept in 1..127); returns how many changed.
int change_velocities(std::vector<NoteEvent*>& notes, const VelocityChange& change);

/// Removes same-pitch, same-channel overlaps (the earlier note is cut where
/// the later begins; an identical start drops the later one). Returns true
/// when anything changed.
bool remove_overlaps(std::vector<NoteEvent>& notes);

}  // namespace vcc::project
