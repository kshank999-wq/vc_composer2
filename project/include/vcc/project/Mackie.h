// Mackie Control (MCU) protocol: what a hardware mixer controller in Mackie
// Control mode sends, and what it is sent back. Most of the industry's
// controllers speak it - the Mackie MCU Pro and its extenders, SSL UF8 / UF1,
// Avid S1 (in MCU mode), Icon Platform M+/X+, Behringer X-Touch, PreSonus
// FaderPort 8/16, Softube Console 1 Fader and others.
//
// The surface has eight channel strips (fader, touch-sensitive; V-Pot with a
// ring of LEDs and a push; Rec, Solo, Mute and Select keys; a scribble strip
// of two 7-character lines; a level meter) and a master fader, plus
// transport, bank, cursor and jog controls. This header is the wire format
// only - no MIDI ports, no project - so it is tested on its own.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace vcc::project::mackie {

constexpr int kStrips = 8;
constexpr int kMasterStrip = 8;  ///< The master fader is "strip" 8 (pitch bend channel 9).

/// Button notes (Note On, channel 1; velocity 127 pressed, 0 released).
enum Button : std::uint8_t {
    RecArm1 = 0x00,   // ..0x07
    Solo1 = 0x08,     // ..0x0F
    Mute1 = 0x10,     // ..0x17
    Select1 = 0x18,   // ..0x1F
    VPotPush1 = 0x20, // ..0x27
    AssignTrack = 0x28,
    AssignSend = 0x29,
    AssignPan = 0x2A,
    AssignPlugin = 0x2B,
    AssignEq = 0x2C,
    AssignInstrument = 0x2D,
    BankLeft = 0x2E,
    BankRight = 0x2F,
    ChannelLeft = 0x30,
    ChannelRight = 0x31,
    Flip = 0x32,
    GlobalView = 0x33,
    NameValue = 0x34,
    SmpteBeats = 0x35,
    F1 = 0x36,  // ..F8 0x3D
    Save = 0x50,
    Undo = 0x51,
    Cancel = 0x52,
    Enter = 0x53,
    Marker = 0x54,
    Nudge = 0x55,
    Cycle = 0x56,
    Drop = 0x57,
    Replace = 0x58,
    Click = 0x59,
    SoloGlobal = 0x5A,
    Rewind = 0x5B,
    FastForward = 0x5C,
    Stop = 0x5D,
    Play = 0x5E,
    Record = 0x5F,
    CursorUp = 0x60,
    CursorDown = 0x61,
    CursorLeft = 0x62,
    CursorRight = 0x63,
    Zoom = 0x64,
    Scrub = 0x65,
    FaderTouch1 = 0x68,  // ..0x6F
    FaderTouchMaster = 0x70,
};

/// What the surface sent.
struct Event {
    enum class Kind { Fader, FaderTouch, Button, VPot, Jog };
    Kind kind = Kind::Button;
    int strip = -1;       ///< Fader, FaderTouch, VPot: 0-7, or kMasterStrip for the master fader.
    int button = -1;      ///< Button: the note (see Button).
    bool pressed = false; ///< Button, FaderTouch.
    double value = 0.0;   ///< Fader: 0..1.
    int delta = 0;        ///< VPot, Jog: steps, + clockwise.
};

/// Decodes one short MIDI message (status, data1, data2). Nullopt for
/// anything that is not Mackie Control.
std::optional<Event> decode(std::uint8_t status, std::uint8_t data1, std::uint8_t data2);

// --- What the surface is sent -----------------------------------------------------

using Bytes = std::vector<std::uint8_t>;

/// Moves a motorised fader: 0..1 (pitch bend on the strip's channel).
Bytes fader(int strip, double value);
/// A key's light: off, on or flashing.
enum class Led { Off, On, Flash };
Bytes led(int button, Led state);
/// A V-Pot's ring. Pan: a single dot from left (-1) through centre to right (+1).
Bytes vpot_pan(int strip, double pan);
/// A strip's level meter, 0..1 (channel pressure; 0x0C is clip). Send ~every 250 ms; it decays on its own.
Bytes meter(int strip, double level);
/// The scribble strips: `line` 0 (top) or 1 (bottom), `strip` 0-7, up to 7
/// characters (padded, trimmed; non-ASCII becomes '?').
Bytes lcd(int strip, int line, const std::string& text);
/// A whole line of the LCD at once (56 characters).
Bytes lcd_line(int line, const std::string& text56);

/// Fader travel <-> dB, as Mackie surfaces mark it: 0..1 maps to -inf..+6 dB
/// with unity (0 dB) at about three quarters of the travel.
double fader_to_db(double value);
double db_to_fader(double db);

/// A track name fitted to a 7-character scribble strip ("Violins I - Pizz" -> "VlnIPiz").
std::string short_name(const std::string& name, std::size_t width = 7);

}  // namespace vcc::project::mackie
