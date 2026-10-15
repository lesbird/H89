#import <Cocoa/Cocoa.h>

@class H89Emulator;

NS_ASSUME_NONNULL_BEGIN

typedef NS_ENUM(NSInteger, H89Phosphor) {
    H89PhosphorWhite,
    H89PhosphorGreen,
    H89PhosphorAmber,
};

// OpenGL view that shows the emulator's CRT.
@interface H89View : NSOpenGLView

@property(nonatomic, strong, nullable) H89Emulator* emulator;
@property(nonatomic) H89Phosphor phosphor;
// Phosphor persistence, glow and a curved screen (on by default).
@property(nonatomic) BOOL crtEffects;

- (instancetype)initWithFrame:(NSRect)frame;

@end

NS_ASSUME_NONNULL_END
