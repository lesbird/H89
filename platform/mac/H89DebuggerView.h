#import <Cocoa/Cocoa.h>

@class H89Emulator;

NS_ASSUME_NONNULL_BEGIN

// Hosts the Dear ImGui debugger in its own window. Input goes to ImGui only
// from this view, so the emulator window keeps its keyboard.
@interface H89DebuggerView : NSOpenGLView

@property(nonatomic, strong, nullable) H89Emulator* emulator;

- (instancetype)initWithFrame:(NSRect)frame;

@end

NS_ASSUME_NONNULL_END
