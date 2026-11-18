#include "vcc/project/Mackie.h"
#include "vcc_test.h"

#include <cmath>

using namespace vcc::project::mackie;

TEST_CASE("mackie: faders, touch, keys, V-Pots and the jog decode")
{
    auto f = decode(0xE2, 0x7F, 0x7F);  // strip 3 at the top
    REQUIRE(f.has_value());
    CHECK(f->kind == Event::Kind::Fader);
    CHECK_EQ(f->strip, 2);
    CHECK(std::abs(f->value - 1.0) < 1e-9);
    auto m = decode(0xE8, 0x00, 0x40);  // the master, half way
    REQUIRE(m.has_value());
    CHECK_EQ(m->strip, kMasterStrip);
    CHECK(std::abs(m->value - 8192.0 / 16383.0) < 1e-9);
    CHECK(!decode(0xE9, 0, 0).has_value());

    auto t = decode(0x90, FaderTouch1 + 4, 0x7F);
    REQUIRE(t.has_value());
    CHECK(t->kind == Event::Kind::FaderTouch);
    CHECK_EQ(t->strip, 4);
    CHECK(t->pressed);
    auto tm = decode(0x90, FaderTouchMaster, 0x00);
    REQUIRE(tm.has_value());
    CHECK_EQ(tm->strip, kMasterStrip);
    CHECK(!tm->pressed);

    auto b = decode(0x90, Mute1 + 5, 0x7F);
    REQUIRE(b.has_value());
    CHECK(b->kind == Event::Kind::Button);
    CHECK_EQ(b->button, Mute1 + 5);
    CHECK(b->pressed);
    auto off = decode(0x80, Play, 0x40);  // note off: released
    REQUIRE(off.has_value());
    CHECK(!off->pressed);
    CHECK(!decode(0x91, Play, 0x7F).has_value());  // other channels are not Mackie
    CHECK(!decode(0x90, 0x75, 0x7F).has_value());

    auto cw = decode(0xB0, 0x13, 0x03);
    REQUIRE(cw.has_value());
    CHECK(cw->kind == Event::Kind::VPot);
    CHECK_EQ(cw->strip, 3);
    CHECK_EQ(cw->delta, 3);
    auto ccw = decode(0xB0, 0x10, 0x41);
    REQUIRE(ccw.has_value());
    CHECK_EQ(ccw->delta, -1);
    auto jog = decode(0xB0, 0x3C, 0x42);
    REQUIRE(jog.has_value());
    CHECK(jog->kind == Event::Kind::Jog);
    CHECK_EQ(jog->delta, -2);
    CHECK(!decode(0xB0, 0x20, 0x01).has_value());
}

TEST_CASE("mackie: what the surface is sent")
{
    CHECK(fader(0, 1.0) == Bytes({0xE0, 0x7F, 0x7F}));
    CHECK(fader(kMasterStrip, 0.0) == Bytes({0xE8, 0x00, 0x00}));
    // A fader round trip lands where it was sent.
    const auto fb = fader(5, 0.6);
    const auto back = decode(fb[0], fb[1], fb[2]);
    REQUIRE(back.has_value());
    CHECK(std::abs(back->value - 0.6) < 1e-4);

    CHECK(led(Solo1 + 2, Led::On) == Bytes({0x90, 0x0A, 0x7F}));
    CHECK(led(Record, Led::Flash) == Bytes({0x90, 0x5F, 0x01}));
    CHECK(led(Play, Led::Off) == Bytes({0x90, 0x5E, 0x00}));

    CHECK(vpot_pan(0, 0.0) == Bytes({0xB0, 0x30, 0x06}));   // centre
    CHECK(vpot_pan(7, -1.0) == Bytes({0xB0, 0x37, 0x01}));  // hard left
    CHECK(vpot_pan(1, 1.0) == Bytes({0xB0, 0x31, 0x0B}));   // hard right

    CHECK(meter(3, 1.0) == Bytes({0xD0, 0x3C}));
    CHECK(meter(0, 0.0) == Bytes({0xD0, 0x00}));

    const auto l = lcd(2, 1, "Violins");
    CHECK(l.size() == 5u + 2u + 7u + 1u);
    CHECK(l[5] == 0x12);
    CHECK(l[6] == 56 + 14);
    CHECK(l[7] == 'V');
    CHECK(l.back() == 0xF7);
    const auto padded = lcd(0, 0, "Hn");
    CHECK(padded[9] == ' ');
    const auto odd = lcd(0, 0, "Caf\xC3\xA9");
    CHECK(odd[10] == '?');
    CHECK(lcd_line(0, "x").size() == 5u + 2u + 56u + 1u);
}

TEST_CASE("mackie: the fader law and short names")
{
    CHECK(std::abs(fader_to_db(0.75)) < 1e-9);
    CHECK(std::abs(fader_to_db(1.0) - 6.0) < 1e-9);
    CHECK(fader_to_db(0.0) <= -144.0);
    CHECK(fader_to_db(0.5) < -10.0);
    CHECK(fader_to_db(0.5) > -13.0);
    for (double db : {6.0, 3.0, 0.0, -3.0, -10.0, -24.0, -48.0})
        CHECK(std::abs(fader_to_db(db_to_fader(db)) - db) < 1e-6);
    CHECK(db_to_fader(-200.0) == 0.0);

    CHECK_EQ(short_name("Horns"), std::string("Horns"));
    CHECK(short_name("Violins I").size() <= 7u);
    CHECK(short_name("Violins I - Pizzicato").size() <= 7u);
    CHECK(short_name("Contrabassoon").size() <= 7u);
    CHECK_EQ(short_name("Celli").size(), 5u);
    CHECK(short_name("Violins I - Pizzicato").front() == 'V');
}
