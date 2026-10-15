#import <Cocoa/Cocoa.h>

@class H89Emulator;

NS_ASSUME_NONNULL_BEGIN

// A small window following a file transfer: what it is, a progress bar, its
// status, and Cancel (Close once it's over).
@interface H89TransferPanel : NSWindowController

- (instancetype)initWithParent:(NSWindow*)parent;
@property(nonatomic, strong, nullable) H89Emulator* emulator;
// Shows it for the transfer just started.
- (void)show;
// Refreshes it from the emulator (call often while a transfer runs).
- (void)update;

@end

NS_ASSUME_NONNULL_END
