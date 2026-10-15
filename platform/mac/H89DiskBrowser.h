#import <Cocoa/Cocoa.h>

@class H89Emulator;

NS_ASSUME_NONNULL_BEGIN

// A window listing the files in an H17 disk image, for copying files in
// (Insert, or drop them on the list) and out (Extract, to a folder on the
// Desktop named after the image). If the image is mounted in one of the
// emulator's drives, changes go through that drive.
@interface H89DiskBrowser : NSWindowController <NSTableViewDataSource, NSTableViewDelegate>

- (instancetype)initWithPath:(NSString*)path emulator:(nullable H89Emulator*)emulator;

@property(nonatomic, readonly) NSString* path;
// Updated by the app delegate when it replaces the emulator.
@property(nonatomic, strong, nullable) H89Emulator* emulator;

// Re-reads the directory (it may have changed under the emulated OS).
- (void)refresh;

@end

NS_ASSUME_NONNULL_END
