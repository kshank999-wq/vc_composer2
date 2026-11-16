#import "VCCDocument.h"

#import <CoreText/CoreText.h>
#import <UIKit/UIKit.h>

#include <algorithm>
#include <map>
#include <memory>
#include <string>

#include "vcc/mobile/Document.h"

namespace {

std::string str(NSString *s) { return s ? std::string(s.UTF8String) : std::string(); }
NSString *ns(const std::string &s) { return [NSString stringWithUTF8String:s.c_str()] ?: @""; }

NSError *fail(const std::string &message)
{
    return [NSError errorWithDomain:@"VCComposer"
                               code:1
                           userInfo:@{NSLocalizedDescriptionKey: ns(message.empty() ? "Something went wrong" : message)}];
}

}  // namespace

@interface VCCPlacement ()
@property(nonatomic) vcc::mobile::Placement value;
@end

@implementation VCCPlacement
- (BOOL)valid { return _value.valid; }
- (int64_t)tick { return _value.tick; }
- (NSInteger)staff { return _value.staff; }
- (NSInteger)staffPosition { return _value.staff_position; }
- (NSInteger)midi { return _value.midi; }
- (NSString *)pitchName { return ns(_value.pitch_name); }
@end

@implementation VCCTrack {
@public
    NSString *_identifier, *_name, *_family;
    NSInteger _staves;
    BOOL _expanded;
}
- (NSString *)identifier { return _identifier; }
- (NSString *)name { return _name; }
- (NSString *)family { return _family; }
- (NSInteger)staves { return _staves; }
- (BOOL)expanded { return _expanded; }
@end

@implementation VCCPlayedNote {
@public
    double _start, _duration;
    NSInteger _pitch, _velocity;
}
- (double)start { return _start; }
- (double)duration { return _duration; }
- (NSInteger)pitch { return _pitch; }
- (NSInteger)velocity { return _velocity; }
@end

@implementation VCCRecognizedItem {
@public
    vcc::mobile::Recognized _r;
}
- (BOOL)isRest { return _r.kind == vcc::mobile::Recognized::Kind::Rest; }
- (NSInteger)value { return _r.value; }
- (NSInteger)dots { return _r.dots; }
- (NSInteger)staff { return _r.staff; }
- (NSArray<NSNumber *> *)positions
{
    NSMutableArray *out = [NSMutableArray array];
    for (int p : _r.positions) [out addObject:@(p)];
    return out;
}
- (int64_t)tick { return _r.tick; }
- (BOOL)doubtful { return _r.doubtful; }
- (NSString *)why { return ns(_r.why); }
- (BOOL)hasAlternative { return _r.has_alternative; }
@end

@implementation VCCRecognition {
@public
    vcc::mobile::Recognition _rec;
}
- (NSArray<VCCRecognizedItem *> *)items
{
    NSMutableArray *out = [NSMutableArray array];
    for (const auto &r : _rec.items) {
        VCCRecognizedItem *i = [[VCCRecognizedItem alloc] init];
        i->_r = r;
        [out addObject:i];
    }
    return out;
}
- (NSString *)summary { return ns(_rec.summary); }
- (NSInteger)doubtfulCount { return _rec.doubtful_count(); }
- (NSInteger)unusedStrokeCount { return static_cast<NSInteger>(_rec.unused_strokes.size()); }
- (void)useAlternativeAtIndex:(NSInteger)index
{
    if (index < 0 || static_cast<std::size_t>(index) >= _rec.items.size()) return;
    auto &r = _rec.items[static_cast<std::size_t>(index)];
    if (!r.has_alternative || r.positions.empty()) return;
    std::swap(r.positions.front(), r.alternative_position);
}
- (void)removeItemAtIndex:(NSInteger)index
{
    if (index < 0 || static_cast<std::size_t>(index) >= _rec.items.size()) return;
    _rec.items.erase(_rec.items.begin() + index);
}
@end

@implementation VCCDocument {
    std::unique_ptr<vcc::mobile::Document> _doc;
    std::map<std::string, vcc::mobile::Drawing> _drawings;
}

+ (instancetype)documentWithContentDirectory:(NSString *)contentDirectory
                                fontMetadata:(NSString *)fontMetadata
                                       error:(NSError **)error
{
    std::string message;
    auto doc = vcc::mobile::Document::create(str(contentDirectory), str(fontMetadata), &message);
    if (!doc) {
        if (error) *error = fail(message);
        return nil;
    }
    VCCDocument *d = [[VCCDocument alloc] init];
    d->_doc = std::move(doc);
    return d;
}

- (void)newEmptyWithInstrument:(NSString *)instrumentId
{
    _drawings.clear();
    _doc->new_empty(str(instrumentId));
}

- (BOOL)newFromTemplate:(NSString *)templateId error:(NSError **)error
{
    _drawings.clear();
    std::string message;
    if (_doc->new_from_template(str(templateId), &message)) return YES;
    if (error) *error = fail(message);
    return NO;
}

- (BOOL)loadFromPath:(NSString *)path error:(NSError **)error
{
    _drawings.clear();
    std::string message;
    if (_doc->load(str(path), &message)) return YES;
    if (error) *error = fail(message);
    return NO;
}

- (BOOL)saveToPath:(NSString *)path error:(NSError **)error
{
    std::string message;
    if (_doc->save_to(str(path), &message)) return YES;
    if (error) *error = fail(message);
    return NO;
}

- (NSString *)title { return ns(_doc->title()); }
- (NSInteger)barCount { return _doc->bar_count(); }
- (double)tempo { return _doc->tempo_bpm(); }
- (NSString *)meter { return ns(_doc->meter()); }
- (NSString *)key { return ns(_doc->key()); }
- (uint64_t)revision { return _doc->revision(); }
- (BOOL)canUndo { return _doc->can_undo(); }
- (BOOL)canRedo { return _doc->can_redo(); }
- (NSString *)undoLabel { return ns(_doc->undo_label()); }

- (NSArray<VCCTrack *> *)tracks
{
    NSMutableArray *out = [NSMutableArray array];
    for (const auto &t : _doc->tracks()) {
        VCCTrack *x = [[VCCTrack alloc] init];
        x->_identifier = ns(t.id);
        x->_name = ns(t.name);
        x->_family = ns(t.family);
        x->_staves = t.staves;
        x->_expanded = t.expanded;
        [out addObject:x];
    }
    return out;
}

- (NSString *)addTrackNamed:(NSString *)name instrument:(NSString *)instrumentId
{
    return ns(_doc->add_track(str(name), str(instrumentId)));
}

- (CGRect)layoutTrack:(NSString *)trackId spacesPerQuarter:(double)spacesPerQuarter
{
    auto &d = _drawings[str(trackId)];
    d = _doc->layout_track(str(trackId), spacesPerQuarter);
    return CGRectMake(d.left, d.top, d.right - d.left, d.bottom - d.top);
}

static CTFontRef music_font(CGFloat scale)
{
    // SMuFL: the em is four staff spaces.
    return CTFontCreateWithName(CFSTR("Bravura"), 4.0 * scale, nullptr);
}

static void draw_codepoint(CGContextRef ctx, CTFontRef font, uint32_t cp, CGFloat x, CGFloat y)
{
    UniChar chars[2];
    CFIndex count = 1;
    if (cp > 0xFFFF) {
        cp -= 0x10000;
        chars[0] = static_cast<UniChar>(0xD800 + (cp >> 10));
        chars[1] = static_cast<UniChar>(0xDC00 + (cp & 0x3FF));
        count = 2;
    } else {
        chars[0] = static_cast<UniChar>(cp);
    }
    CGGlyph glyphs[2] = {0, 0};
    if (!CTFontGetGlyphsForCharacters(font, chars, glyphs, count)) return;
    CGContextSaveGState(ctx);
    // The context is y down; glyphs are drawn y up about their baseline.
    CGContextTranslateCTM(ctx, x, y);
    CGContextScaleCTM(ctx, 1.0, -1.0);
    CGPoint origin = CGPointZero;
    CTFontDrawGlyphs(font, glyphs, &origin, 1, ctx);
    CGContextRestoreGState(ctx);
}

- (void)drawTrack:(NSString *)trackId
        inContext:(CGContextRef)ctx
            scale:(CGFloat)s
         selected:(NSSet<NSNumber *> *)selected
            color:(CGColorRef)ink
        highlight:(CGColorRef)highlight
{
    const auto it = _drawings.find(str(trackId));
    if (it == _drawings.end()) return;
    const auto &d = it->second;
    auto colour = [&](uint64_t source) {
        return (source != 0 && [selected containsObject:@(source)]) ? highlight : ink;
    };
    CGContextSetLineCap(ctx, kCGLineCapButt);
    for (const auto &l : d.lines) {
        CGContextSetStrokeColorWithColor(ctx, colour(l.source));
        CGContextSetLineWidth(ctx, std::max<CGFloat>(0.5, l.thickness * s));
        CGContextMoveToPoint(ctx, l.x1 * s, l.y1 * s);
        CGContextAddLineToPoint(ctx, l.x2 * s, l.y2 * s);
        CGContextStrokePath(ctx);
    }
    for (const auto &q : d.quads) {
        CGContextSetFillColorWithColor(ctx, colour(q.source));
        CGContextMoveToPoint(ctx, q.x[0] * s, q.y[0] * s);
        for (int i = 1; i < 4; ++i) CGContextAddLineToPoint(ctx, q.x[i] * s, q.y[i] * s);
        CGContextClosePath(ctx);
        CGContextFillPath(ctx);
    }
    for (const auto &c : d.curves) {
        CGContextSetStrokeColorWithColor(ctx, colour(c.source));
        CGContextSetLineWidth(ctx, std::max<CGFloat>(0.6, c.thickness * s));
        CGContextMoveToPoint(ctx, c.x[0] * s, c.y[0] * s);
        CGContextAddCurveToPoint(ctx, c.x[1] * s, c.y[1] * s, c.x[2] * s, c.y[2] * s, c.x[3] * s, c.y[3] * s);
        CGContextStrokePath(ctx);
    }
    CTFontRef font = music_font(s);
    CTFontRef small = nullptr;
    for (const auto &g : d.glyphs) {
        if (g.codepoint == 0) continue;
        CGContextSetFillColorWithColor(ctx, colour(g.source));
        CTFontRef use = font;
        if (g.scale != 1.0) {
            if (small) CFRelease(small);
            small = CTFontCreateCopyWithAttributes(font, 4.0 * s * g.scale, nullptr, nullptr);
            use = small;
        }
        draw_codepoint(ctx, use, g.codepoint, g.x * s, g.y * s);
    }
    if (small) CFRelease(small);
    CFRelease(font);
    for (const auto &t : d.texts) {
        CTFontRef tf = CTFontCreateUIFontForLanguage(t.bold ? kCTFontUIFontEmphasizedSystem : kCTFontUIFontSystem,
                                                     t.size * s, nullptr);
        NSDictionary *attrs = @{(id)kCTFontAttributeName: (__bridge id)tf, (id)kCTForegroundColorAttributeName: (__bridge id)ink};
        NSAttributedString *a = [[NSAttributedString alloc] initWithString:ns(t.text) attributes:attrs];
        CTLineRef line = CTLineCreateWithAttributedString((__bridge CFAttributedStringRef)a);
        CGContextSaveGState(ctx);
        CGContextTranslateCTM(ctx, t.x * s, t.y * s);
        CGContextScaleCTM(ctx, 1.0, -1.0);
        CGContextSetTextPosition(ctx, 0, 0);
        CTLineDraw(line, ctx);
        CGContextRestoreGState(ctx);
        CFRelease(line);
        CFRelease(tf);
    }
}

- (void)drawGlyph:(uint32_t)codepoint
              atX:(double)x
                y:(double)y
        inContext:(CGContextRef)ctx
            scale:(CGFloat)s
            color:(CGColorRef)color
{
    CTFontRef font = music_font(s);
    CGContextSetFillColorWithColor(ctx, color);
    draw_codepoint(ctx, font, codepoint, x * s, y * s);
    CFRelease(font);
}

- (VCCPlacement *)placementInTrack:(NSString *)trackId x:(double)x y:(double)y grid:(VCCValue)grid
{
    VCCPlacement *p = [[VCCPlacement alloc] init];
    p.value = _doc->place(str(trackId), x, y, static_cast<vcc::mobile::Value>(grid));
    return p;
}

- (uint64_t)noteInTrack:(NSString *)trackId x:(double)x y:(double)y { return _doc->note_at(str(trackId), x, y); }
- (double)xForTick:(int64_t)tick inTrack:(NSString *)trackId { return _doc->x_for_tick(str(trackId), tick); }
- (double)yForStaff:(NSInteger)staff position:(NSInteger)position inTrack:(NSString *)trackId
{
    return _doc->y_for(str(trackId), static_cast<int>(staff), static_cast<int>(position));
}

- (BOOL)enterNoteInTrack:(NSString *)trackId at:(VCCPlacement *)at value:(VCCValue)value dots:(NSInteger)dots
{
    return _doc->enter_note(str(trackId), at.value, static_cast<vcc::mobile::Value>(value), static_cast<int>(dots));
}

- (BOOL)enterNoteInTrack:(NSString *)trackId
                      at:(VCCPlacement *)at
                   value:(VCCValue)value
                    dots:(NSInteger)dots
              alteration:(NSInteger)alteration
{
    return _doc->enter_note(str(trackId), at.value, static_cast<vcc::mobile::Value>(value), static_cast<int>(dots),
                            static_cast<int>(alteration));
}

- (BOOL)enterRestInTrack:(NSString *)trackId at:(VCCPlacement *)at value:(VCCValue)value dots:(NSInteger)dots
{
    return _doc->enter_rest(str(trackId), at.value, static_cast<vcc::mobile::Value>(value), static_cast<int>(dots));
}

- (BOOL)removeNote:(uint64_t)note inTrack:(NSString *)trackId { return _doc->remove_note(str(trackId), note); }
- (BOOL)setAccidental:(NSInteger)alteration forNote:(uint64_t)note inTrack:(NSString *)trackId
{
    return _doc->set_accidental(str(trackId), note, static_cast<int>(alteration));
}
+ (int64_t)ticksPerQuarter { return vcc::mobile::Document::ticks_per_quarter(); }

- (VCCRecognition *)recognizeInTrack:(NSString *)trackId strokes:(NSArray<NSArray<NSValue *> *> *)strokes
{
    std::vector<vcc::mobile::Stroke> ink;
    for (NSArray<NSValue *> *stroke in strokes) {
        vcc::mobile::Stroke s;
        for (NSValue *v in stroke) {
            const CGPoint p = v.CGPointValue;
            s.push_back({p.x, p.y});
        }
        if (!s.empty()) ink.push_back(std::move(s));
    }
    VCCRecognition *r = [[VCCRecognition alloc] init];
    r->_rec = _doc->recognize(str(trackId), ink);
    return r;
}

- (BOOL)commitRecognition:(VCCRecognition *)recognition inTrack:(NSString *)trackId
{
    return _doc->commit(str(trackId), recognition->_rec);
}

- (BOOL)undo { return _doc->undo(); }
- (BOOL)redo { return _doc->redo(); }

- (NSArray<VCCPlayedNote *> *)performance
{
    NSMutableArray *out = [NSMutableArray array];
    for (const auto &n : _doc->performance()) {
        VCCPlayedNote *p = [[VCCPlayedNote alloc] init];
        p->_start = n.start;
        p->_duration = n.duration;
        p->_pitch = n.pitch;
        p->_velocity = n.velocity;
        [out addObject:p];
    }
    return out;
}

@end
