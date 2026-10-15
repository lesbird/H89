#import <Cocoa/Cocoa.h>

NS_ASSUME_NONNULL_BEGIN

// Settings, all kept in NSUserDefaults.
extern NSString* const H89RomDirectoryKey;   // absent: the ROMs built into the app
extern NSString* const H89MonitorKey;        // 90 or 89
extern NSString* const H89MemoryKey;         // 16, 32, 48 or 64
extern NSString* const H89BootFromH37Key;
extern NSString* const H89AutoBootKey;
extern NSString* const H89SpeedKey;          // frames per tick; 0 = maximum
extern NSString* const H89PhosphorKey;       // H89Phosphor
extern NSString* const H89CrtEffectsKey;
extern NSString* const H89BellKey;           // H89Bell
extern NSString* const H89KeyClickKey;

typedef NS_ENUM(NSInteger, H89Bell) { H89BellH19, H89BellSystem, H89BellOff };

@protocol H89PreferencesDelegate <NSObject>
// The ROMs, monitor or memory changed: the machine has to be rebuilt.
- (void)preferencesChangedMachine;
// Anything else: apply it to the running machine and view.
- (void)preferencesChanged;
- (NSString*)disksDirectory;
- (void)restoreOriginalDisks;
@end

// The Settings window (⌘,). Changes apply as they are made.
@interface H89Preferences : NSWindowController

- (instancetype)initWithDelegate:(id<H89PreferencesDelegate>)delegate;
// Shows the window with the current settings.
- (void)show;
// Re-reads the settings, after they were changed from a menu.
- (void)reload;

@end

NS_ASSUME_NONNULL_END
