#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

// Drives 0-2 are the H17's SY0:-SY2:, 3-6 the H37's DK0:-DK3:.
static const NSInteger H89FirstH37Drive = 3;

// H19 keys that don't produce a plain character (mirrors h89::H19Key).
typedef NS_ENUM(NSInteger, H89Key) {
    H89KeyF1, H89KeyF2, H89KeyF3, H89KeyF4, H89KeyF5,
    H89KeyErase, H89KeyBlue, H89KeyRed, H89KeyWhite,
    H89KeyKeypad0, H89KeyKeypad1, H89KeyKeypad2, H89KeyKeypad3, H89KeyKeypad4,
    H89KeyKeypad5, H89KeyKeypad6, H89KeyKeypad7, H89KeyKeypad8, H89KeyKeypad9,
    H89KeyKeypadDot, H89KeyKeypadEnter, H89KeyScroll,
};

// Objective-C face of the C++ emulation core. This header is plain
// Objective-C so it can be imported from any AppKit (or Swift) code; the C++
// Machine lives only in H89Emulator.mm.
@interface H89Emulator : NSObject

// Loads the ROMs from `directory`; returns nil and sets `error` if any
// required ROM is missing.
- (nullable instancetype)initWithRomDirectory:(NSString*)directory
                                        error:(NSError**)error NS_DESIGNATED_INITIALIZER;
- (instancetype)init NS_UNAVAILABLE;

// Which ROM files were loaded, one per line.
@property(nonatomic, readonly) NSString* romSummary;

@property(nonatomic, readonly) NSInteger screenWidth;
@property(nonatomic, readonly) NSInteger screenHeight;
@property(nonatomic, readonly) uint64_t frameCount;

// Called on the emulation thread (currently the main thread) when the H19
// rings its bell.
@property(nonatomic, copy, nullable) void (^bellHandler)(void);

- (void)reset;
// Advances emulation by one 1/60 s frame.
- (void)runFrame;
// One luminance byte per pixel, screenWidth x screenHeight, valid until the
// next -runFrame.
- (const uint8_t*)framebuffer NS_RETURNS_INNER_POINTER;

// Floppy drives: 0-2 are the H17's SY0:-SY2: (hard-sectored .h8d images),
// 3-6 the H37's DK0:-DK3: (soft-sectored .h37 / .imd images). Writes go
// straight back to the image file unless mounted read-only.
@property(nonatomic, readonly) NSInteger driveCount;
// NO if the drive's controller isn't fitted (the H17 needs its ROM).
- (BOOL)driveIsAvailable:(NSInteger)drive;
- (BOOL)driveIsH37:(NSInteger)drive;
// "SY0:" or "DK0:".
- (NSString*)driveName:(NSInteger)drive;
// Boot from the H37 (SW501 section 4) instead of the H17; takes effect at
// the next boot.
@property(nonatomic) BOOL bootFromH37;
- (BOOL)insertDiskAtPath:(NSString*)path
                   drive:(NSInteger)drive
                readOnly:(BOOL)readOnly
                   error:(NSError**)error;
- (void)ejectDrive:(NSInteger)drive;
// Path of the disk in `drive`, or nil if empty.
- (nullable NSString*)diskPathForDrive:(NSInteger)drive;
- (BOOL)diskIsReadOnlyInDrive:(NSInteger)drive;

// Keyboard: ASCII characters (including control characters) and special keys.
- (void)typeCharacter:(unichar)character;
- (void)pressKey:(H89Key)key shift:(BOOL)shift;
- (void)setBreakKeyDown:(BOOL)down;

@end

NS_ASSUME_NONNULL_END
