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
// The H19 on its own, as a terminal on a host serial port.
extern NSString* const H89TerminalOnlyKey;
extern NSString* const H89TerminalPortKey;   // /dev/cu.usbserial-1410
extern NSString* const H89TerminalBaudKey;   // any rate; default 9600
extern NSString* const H89TerminalFormatKey; // "8N1", "7E1", ...
extern NSString* const H89TerminalRtsCtsKey;
extern NSString* const H89TextLineEndingKey; // 0 CR, 1 CR LF, 2 LF, 3 unchanged
extern NSString* const H89TextCharDelayKey;  // ms after each character
extern NSString* const H89TextLineDelayKey;  // ms after each line

typedef NS_ENUM(NSInteger, H89Bell) { H89BellH19, H89BellSystem, H89BellOff };

// The H19's SW401 and SW402 as set in Settings.
void H89TerminalSwitches(NSInteger* sw401, NSInteger* sw402);
// The terminal's host port as a spec for -[H89Emulator connectTerminal:], or
// nil if no port is chosen.
NSString* _Nullable H89TerminalSpec(void);

@protocol H89PreferencesDelegate <NSObject>
// The ROMs, monitor, memory or mode changed: the machine has to be rebuilt.
- (void)preferencesChangedMachine;
// The terminal's serial port settings changed: reconnect.
- (void)preferencesChangedTerminalPort;
// Anything else: apply it to the running machine and view.
- (void)preferencesChanged;
// The terminal's connection, for the Terminal tab.
- (NSString*)terminalStatus;
- (NSString*)disksDirectory;
- (void)restoreOriginalDisks;
@end

// The Settings window (⌘,). Changes apply as they are made.
@interface H89Preferences : NSWindowController

- (instancetype)initWithDelegate:(id<H89PreferencesDelegate>)delegate;
// Shows the window with the current settings.
- (void)show;
// Shows it at the Terminal tab.
- (void)showTerminal;
// Re-reads the settings, after they were changed from a menu.
- (void)reload;

@end

NS_ASSUME_NONNULL_END
