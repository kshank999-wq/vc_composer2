// The Swift-facing bridge to the shared C++ core (vcc::mobile::Document):
// the desktop's project format, score editor and engraving. Geometry is in
// staff spaces, y down; drawing scales by points per staff space.
#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

typedef NS_ENUM(NSInteger, VCCValue) {
    VCCValueWhole = 1,
    VCCValueHalf = 2,
    VCCValueQuarter = 3,
    VCCValueEighth = 4,
    VCCValueSixteenth = 5,
    VCCValueThirtySecond = 6,
};

/// Where a touch lands on a staff: a tick on the entry grid and a pitch.
@interface VCCPlacement : NSObject
@property(nonatomic, readonly) BOOL valid;
@property(nonatomic, readonly) int64_t tick;
@property(nonatomic, readonly) NSInteger staff;
@property(nonatomic, readonly) NSInteger staffPosition;
@property(nonatomic, readonly) NSInteger midi;
@property(nonatomic, readonly, copy) NSString *pitchName;
@end

@interface VCCTrack : NSObject
@property(nonatomic, readonly, copy) NSString *identifier;
@property(nonatomic, readonly, copy) NSString *name;
@property(nonatomic, readonly, copy) NSString *family;
@property(nonatomic, readonly) NSInteger staves;
@property(nonatomic, readonly) BOOL expanded;
@end

@interface VCCPlayedNote : NSObject
@property(nonatomic, readonly) double start;
@property(nonatomic, readonly) double duration;
@property(nonatomic, readonly) NSInteger pitch;
@property(nonatomic, readonly) NSInteger velocity;
@end

@interface VCCDocument : NSObject

/// The shipped content (instruments, templates) and Bravura's metadata, from
/// the app bundle. Nil, with an error, when the content is missing.
+ (nullable instancetype)documentWithContentDirectory:(NSString *)contentDirectory
                                        fontMetadata:(NSString *)fontMetadata
                                               error:(NSError **)error NS_SWIFT_NAME(init(contentDirectory:fontMetadata:));

- (void)newEmptyWithInstrument:(NSString *)instrumentId NS_SWIFT_NAME(newEmpty(withInstrument:));
- (BOOL)newFromTemplate:(NSString *)templateId error:(NSError **)error NS_SWIFT_NAME(newFromTemplate(_:));
- (BOOL)loadFromPath:(NSString *)path error:(NSError **)error NS_SWIFT_NAME(load(fromPath:));
- (BOOL)saveToPath:(NSString *)path error:(NSError **)error NS_SWIFT_NAME(save(toPath:));

@property(nonatomic, readonly, copy) NSString *title;
@property(nonatomic, readonly, copy) NSArray<VCCTrack *> *tracks;
@property(nonatomic, readonly) NSInteger barCount;
@property(nonatomic, readonly) double tempo;
@property(nonatomic, readonly, copy) NSString *meter;
@property(nonatomic, readonly, copy) NSString *key;
@property(nonatomic, readonly) uint64_t revision;
@property(nonatomic, readonly) BOOL canUndo;
@property(nonatomic, readonly) BOOL canRedo;
@property(nonatomic, readonly, copy) NSString *undoLabel;

- (NSString *)addTrackNamed:(NSString *)name instrument:(NSString *)instrumentId NS_SWIFT_NAME(addTrack(named:instrument:));

/// Engraves a track (one continuous system); returns its bounds in staff spaces.
- (CGRect)layoutTrack:(NSString *)trackId spacesPerQuarter:(double)spacesPerQuarter NS_SWIFT_NAME(layoutTrack(_:spacesPerQuarter:));
/// Draws the track's last layout into `context` (y down), `scale` points per
/// staff space, with `selected` notes highlighted.
- (void)drawTrack:(NSString *)trackId
        inContext:(CGContextRef)context
            scale:(CGFloat)scale
         selected:(NSSet<NSNumber *> *)selected
            color:(CGColorRef)ink
        highlight:(CGColorRef)highlight NS_SWIFT_NAME(drawTrack(_:in:scale:selected:color:highlight:));
/// Draws one music-font glyph (a shadow note's head) at staff-space x, y.
- (void)drawGlyph:(uint32_t)codepoint
               atX:(double)x
                 y:(double)y
         inContext:(CGContextRef)context
             scale:(CGFloat)scale
             color:(CGColorRef)color NS_SWIFT_NAME(drawGlyph(_:atX:y:in:scale:color:));

- (VCCPlacement *)placementInTrack:(NSString *)trackId x:(double)x y:(double)y grid:(VCCValue)grid NS_SWIFT_NAME(placement(inTrack:x:y:grid:));
- (uint64_t)noteInTrack:(NSString *)trackId x:(double)x y:(double)y NS_SWIFT_NAME(note(inTrack:x:y:));
- (double)xForTick:(int64_t)tick inTrack:(NSString *)trackId NS_SWIFT_NAME(x(forTick:inTrack:));
- (double)yForStaff:(NSInteger)staff position:(NSInteger)position inTrack:(NSString *)trackId NS_SWIFT_NAME(y(forStaff:position:inTrack:));

- (BOOL)enterNoteInTrack:(NSString *)trackId at:(VCCPlacement *)at value:(VCCValue)value dots:(NSInteger)dots NS_SWIFT_NAME(enterNote(inTrack:at:value:dots:));
- (BOOL)enterNoteInTrack:(NSString *)trackId
                      at:(VCCPlacement *)at
                   value:(VCCValue)value
                    dots:(NSInteger)dots
              alteration:(NSInteger)alteration NS_SWIFT_NAME(enterNote(inTrack:at:value:dots:alteration:));
- (BOOL)enterRestInTrack:(NSString *)trackId at:(VCCPlacement *)at value:(VCCValue)value dots:(NSInteger)dots NS_SWIFT_NAME(enterRest(inTrack:at:value:dots:));
- (BOOL)removeNote:(uint64_t)note inTrack:(NSString *)trackId NS_SWIFT_NAME(removeNote(_:inTrack:));
- (BOOL)setAccidental:(NSInteger)alteration forNote:(uint64_t)note inTrack:(NSString *)trackId NS_SWIFT_NAME(setAccidental(_:forNote:inTrack:));
- (BOOL)undo;
- (BOOL)redo;

/// Every staff's notes as they play.
- (NSArray<VCCPlayedNote *> *)performance NS_SWIFT_NAME(performance());

@end

NS_ASSUME_NONNULL_END
