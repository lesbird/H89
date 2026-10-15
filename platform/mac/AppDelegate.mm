#import "AppDelegate.h"

#if H89_DEBUGGER
#import "H89DebuggerView.h"
#endif
#import "H89DiskBrowser.h"
#import "H89Emulator.h"
#import "H89View.h"

#include "AppFiles.h"

#include <string>
#include <vector>

// Set once the disks bundled with the app have been copied for the user.
static NSString* const kDisksInstalledKey = @"DisksInstalled";

static NSString* SerialKey(NSInteger port) {
    return [NSString stringWithFormat:@"Serial%ldSpec", (long)port];
}
static NSString* const kSerialNames[] = {@"AUX (D0h)", @"MODEM (D8h)", @"LP: Printer (E0h)"};

// Machine › Speed: emulated frames per host frame, or 0 for as many as fit.
static const NSInteger kSpeeds[] = {1, 2, 4, 0};

// Saved disk for drive n: "Disk<n>Path" and "Disk<n>ReadOnly".
static NSString* DiskKey(NSInteger drive, NSString* field) {
    return [NSString stringWithFormat:@"Disk%ld%@", (long)drive, field];
}

@implementation AppDelegate {
    NSWindow* _window;
    H89View* _view;
    H89Emulator* _emulator;
    NSTimer* _timer;
    NSTimeInterval _lastBell;
    NSMenu* _disksMenu;
    NSMutableArray<H89DiskBrowser*>* _browsers;
    NSMenu* _serialMenu;
#if H89_DEBUGGER
    NSWindow* _debuggerWindow;
    H89DebuggerView* _debuggerView;
#endif
    NSInteger _speed;      // frames per tick; 0 = maximum
    NSInteger _lastSpeed;  // to return to from turbo
    H89Preferences* _preferences;
    H89Bell _bell;
    BOOL _keyClick;
    NSSound* _bellSound;
    NSSound* _clickSound;
}

static NSUserDefaults* Defaults() {
    return NSUserDefaults.standardUserDefaults;
}

static NSSound* SoundFromWav(const std::vector<uint8_t>& wav) {
    return [[NSSound alloc] initWithData:[NSData dataWithBytes:wav.data() length:wav.size()]];
}

- (void)applicationDidFinishLaunching:(NSNotification*)notification {
    [self buildMenus];

    NSRect frame = NSMakeRect(0, 0, 960, 720);
    _window = [[NSWindow alloc]
        initWithContentRect:frame
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                            NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    _window.title = @"Heathkit H89";
    _window.contentMinSize = NSMakeSize(320, 240);

    _view = [[H89View alloc] initWithFrame:frame];
    _window.contentView = _view;
    [_window center];
    [_window makeKeyAndOrderFront:nil];
    [_window makeFirstResponder:_view];
    [NSApp activateIgnoringOtherApps:YES];

    _bellSound = SoundFromWav(h89::makeBellSound());
    _clickSound = SoundFromWav(h89::makeKeyClickSound());
    [self installDisks];
    if (![self startEmulator]) {
        [NSApp terminate:nil];
        return;
    }

    // Emulation runs on the main thread for now; it moves to its own thread
    // once the machine needs tighter host-clock throttling.
    _timer = [NSTimer scheduledTimerWithTimeInterval:1.0 / 60.0
                                              target:self
                                            selector:@selector(tick:)
                                            userInfo:nil
                                             repeats:YES];
    [[NSRunLoop currentRunLoop] addTimer:_timer forMode:NSRunLoopCommonModes];
#if H89_DEBUGGER
    // "open H89.app --args -H89ShowDebugger YES" starts with the debugger open.
    if ([NSUserDefaults.standardUserDefaults boolForKey:@"H89ShowDebugger"]) [self showDebugger:nil];
#endif
}

- (nullable NSString*)builtInRomDirectory {
    NSString* dir = [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"roms"];
    return [NSFileManager.defaultManager fileExistsAtPath:dir] ? dir : nil;
}

- (H89Emulator*)emulatorWithRoms:(NSString*)dir error:(NSError**)error {
    NSUserDefaults* d = Defaults();
    return [[H89Emulator alloc] initWithRomDirectory:dir
                                         preferMTR89:[d integerForKey:H89MonitorKey] == 89
                                            memoryKB:[d objectForKey:H89MemoryKey] ? [d integerForKey:H89MemoryKey] : 64
                                               error:error];
}

// (Re)builds the machine from the settings: the ROMs come from the H89_ROMS
// environment variable, the chosen folder, or the ones built into the app,
// in that order. If none load, asks for a folder; returns NO if the user
// cancels.
- (BOOL)startEmulator {
    NSUserDefaults* d = Defaults();
    NSString* builtIn = [self builtInRomDirectory];
    NSString* dir = NSProcessInfo.processInfo.environment[@"H89_ROMS"];
    if (!dir) dir = [d stringForKey:H89RomDirectoryKey];
    if (!dir) dir = builtIn;

    NSError* error = nil;
    H89Emulator* emulator = dir ? [self emulatorWithRoms:dir error:&error] : nil;
    if (!emulator && builtIn && ![dir isEqualToString:builtIn]) {
        NSAlert* alert = [[NSAlert alloc] init];
        alert.messageText = @"Using the ROMs built into H89";
        alert.informativeText = error.localizedDescription;
        [alert runModal];
        [d removeObjectForKey:H89RomDirectoryKey];
        emulator = [self emulatorWithRoms:builtIn error:&error];
    }
    while (!emulator) {
        NSString* message = error ? error.localizedDescription
                                  : @"Choose the folder containing your H89 and H19 ROM images.";
        dir = [self chooseRomDirectoryWithMessage:message];
        if (!dir) return NO;
        emulator = [self emulatorWithRoms:dir error:&error];
        if (emulator) [d setObject:dir forKey:H89RomDirectoryKey];
    }

    __weak AppDelegate* weakSelf = self;
    emulator.bellHandler = ^{ [weakSelf ringBell]; };
    emulator.keyClickHandler = ^{ [weakSelf keyClick]; };
    _emulator = emulator;
    _view.emulator = emulator;
#if H89_DEBUGGER
    _debuggerView.emulator = emulator;
#endif
    for (H89DiskBrowser* browser in _browsers) browser.emulator = emulator;
    [self preferencesChanged];
    [self restoreDisks];
    for (H89DiskBrowser* browser in _browsers) [browser refresh];
    // Last, once the old machine is gone, so a listening port is free again.
    for (NSInteger p = 0; p < 3; ++p) {
        NSString* spec = [d stringForKey:SerialKey(p)];
        if (spec) [emulator connectSerialPort:(H89SerialPort)p spec:spec error:nil];
    }
    return YES;
}

#pragma mark - Settings

- (void)showPreferences:(id)sender {
    if (!_preferences) _preferences = [[H89Preferences alloc] initWithDelegate:self];
    [_preferences show];
}

- (void)preferencesChangedMachine {
    [self startEmulator];
}

// Applies every setting except the machine's make-up.
- (void)preferencesChanged {
    NSUserDefaults* d = Defaults();
    _speed = [d objectForKey:H89SpeedKey] ? [d integerForKey:H89SpeedKey] : 1;
    if (_speed) _lastSpeed = _speed;
    else if (!_lastSpeed) _lastSpeed = 1;
    NSInteger phosphor = [d integerForKey:H89PhosphorKey];
    _view.phosphor = phosphor >= H89PhosphorWhite && phosphor <= H89PhosphorAmber ? (H89Phosphor)phosphor
                                                                                   : H89PhosphorWhite;
    _view.crtEffects = ![d objectForKey:H89CrtEffectsKey] || [d boolForKey:H89CrtEffectsKey];
    _bell = (H89Bell)[d integerForKey:H89BellKey];
    _keyClick = ![d objectForKey:H89KeyClickKey] || [d boolForKey:H89KeyClickKey];
    _emulator.bootFromH37 = [d boolForKey:H89BootFromH37Key];
    _emulator.autoBoot = [d boolForKey:H89AutoBootKey];
    [self updateTitle];
    [_preferences reload];
}

#pragma mark - Bundled disks

- (NSString*)disksDirectory {
    return @(h89::userDisksDir().c_str());
}

- (nullable NSString*)bundledDisksDirectory {
    NSString* dir = [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"disks"];
    return [NSFileManager.defaultManager fileExistsAtPath:dir] ? dir : nil;
}

// The first time H89 runs, copies the disks that come with it to the user's
// disks folder and, if no disks are mounted yet, puts them in the drives:
// HDOS and CP/M for the H17 in SY0: and SY1:, and for the H37 in DK0: and DK1:.
- (void)installDisks {
    NSUserDefaults* d = Defaults();
    NSString* bundled = [self bundledDisksDirectory];
    if (!bundled || [d boolForKey:kDisksInstalledKey]) return;
    NSString* dir = [self disksDirectory];
    std::string error;
    h89::copyBundledFiles(bundled.fileSystemRepresentation, dir.fileSystemRepresentation, false, &error);
    if (!error.empty()) NSLog(@"H89: %s", error.c_str());
    [d setBool:YES forKey:kDisksInstalledKey];

    for (NSInteger drive = 0; drive < H89FirstH37Drive + 4; ++drive)
        if ([d stringForKey:DiskKey(drive, @"Path")]) return;
    for (const auto& [drive, path] : h89::starterDisks(dir.fileSystemRepresentation)) {
        [d setObject:@(path.c_str()) forKey:DiskKey(drive, @"Path")];
        [d setBool:NO forKey:DiskKey(drive, @"ReadOnly")];
    }
}

- (void)showDisksFolder:(id)sender {
    NSString* dir = [self disksDirectory];
    [NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
    [NSWorkspace.sharedWorkspace openURL:[NSURL fileURLWithPath:dir isDirectory:YES]];
}

// Replaces the user's copies of the bundled disks with fresh ones, taking
// them out of the drives while they are copied.
- (void)restoreOriginalDisks {
    NSString* bundled = [self bundledDisksDirectory];
    if (!bundled) return;
    NSAlert* alert = [[NSAlert alloc] init];
    alert.messageText = @"Restore the original disks?";
    alert.informativeText = @"Your copies of the disks that came with H89 are replaced by fresh ones, losing any "
                            @"changes made to them, including files copied onto them. Other disks in the folder "
                            @"are not touched.";
    [alert addButtonWithTitle:@"Restore"];
    [alert addButtonWithTitle:@"Cancel"];
    if ([alert runModal] != NSAlertFirstButtonReturn) return;

    NSString* dir = [self disksDirectory];
    NSArray<NSString*>* originals = [NSFileManager.defaultManager contentsOfDirectoryAtPath:bundled error:nil];
    NSMutableDictionary<NSNumber*, NSNumber*>* remount = [NSMutableDictionary dictionary];
    for (NSInteger drive = 0; drive < _emulator.driveCount; ++drive) {
        NSString* path = [_emulator diskPathForDrive:drive];
        if ([path.stringByDeletingLastPathComponent isEqualToString:dir] &&
            [originals containsObject:path.lastPathComponent]) {
            remount[@(drive)] = @([_emulator diskIsReadOnlyInDrive:drive]);
            [_emulator ejectDrive:drive];
        }
    }
    std::string error;
    h89::copyBundledFiles(bundled.fileSystemRepresentation, dir.fileSystemRepresentation, true, &error);
    for (NSNumber* drive in remount) {
        NSString* path = [Defaults() stringForKey:DiskKey(drive.integerValue, @"Path")];
        [_emulator insertDiskAtPath:path drive:drive.integerValue readOnly:remount[drive].boolValue error:nil];
    }
    for (H89DiskBrowser* browser in _browsers) [browser refresh];
    [self updateTitle];
    if (!error.empty()) {
        NSAlert* failed = [[NSAlert alloc] init];
        failed.messageText = @"Some disks could not be restored.";
        failed.informativeText = @(error.c_str());
        [failed runModal];
    }
}

- (void)restoreOriginalDisks:(id)sender {
    [self restoreOriginalDisks];
}

// Disk images opened from the Finder go in the first empty drive that takes
// them (or the first such drive, if all are full).
- (void)application:(NSApplication*)application openURLs:(NSArray<NSURL*>*)urls {
    if (!_emulator) return;
    for (NSURL* url in urls) {
        BOOL h17 = [url.pathExtension caseInsensitiveCompare:@"h8d"] == NSOrderedSame;
        NSInteger first = h17 ? 0 : H89FirstH37Drive, last = h17 ? H89FirstH37Drive : _emulator.driveCount;
        NSInteger drive = first;
        for (NSInteger d = first; d < last; ++d) {
            if ([_emulator driveIsAvailable:d] && ![_emulator diskPathForDrive:d]) {
                drive = d;
                break;
            }
        }
        NSError* error = nil;
        if (![_emulator insertDiskAtPath:url.path drive:drive readOnly:NO error:&error]) {
            [[NSAlert alertWithError:error] runModal];
            continue;
        }
        [Defaults() setObject:url.path forKey:DiskKey(drive, @"Path")];
        [Defaults() setBool:NO forKey:DiskKey(drive, @"ReadOnly")];
    }
    [self updateTitle];
}

#pragma mark - Disks

- (void)restoreDisks {
    NSUserDefaults* defaults = NSUserDefaults.standardUserDefaults;
    for (NSInteger d = 0; d < _emulator.driveCount; ++d) {
        NSString* path = [defaults stringForKey:DiskKey(d, @"Path")];
        if (!path || ![_emulator driveIsAvailable:d]) continue;
        if (![_emulator insertDiskAtPath:path drive:d readOnly:[defaults boolForKey:DiskKey(d, @"ReadOnly")] error:nil])
            [defaults removeObjectForKey:DiskKey(d, @"Path")];
    }
    [self updateTitle];
}

- (void)insertDisk:(NSMenuItem*)sender {
    NSInteger drive = sender.tag;
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    // Set explicitly: AppKit can hand back a panel still configured by the
    // ROM-folder chooser (folders only), which greys out every disk image.
    panel.canChooseFiles = YES;
    panel.canChooseDirectories = NO;
    panel.allowsMultipleSelection = NO;
    panel.prompt = @"Insert";
    panel.message = [_emulator driveIsH37:drive]
                        ? [NSString stringWithFormat:@"Choose an H37 disk image (.h37 or .imd) for %@",
                                                     [_emulator driveName:drive]]
                        : [NSString stringWithFormat:@"Choose an H17 disk image (.h8d) for %@", [_emulator driveName:drive]];
    NSButton* readOnly = [NSButton checkboxWithTitle:@"Write protected" target:nil action:nil];
    [readOnly sizeToFit];
    panel.accessoryView = readOnly;
    panel.accessoryViewDisclosed = YES;
    if ([panel runModal] != NSModalResponseOK) return;

    NSString* path = panel.URL.path;
    BOOL ro = readOnly.state == NSControlStateValueOn;
    NSError* error = nil;
    if (![_emulator insertDiskAtPath:path drive:drive readOnly:ro error:&error]) {
        [[NSAlert alertWithError:error] runModal];
        return;
    }
    NSUserDefaults* defaults = NSUserDefaults.standardUserDefaults;
    [defaults setObject:path forKey:DiskKey(drive, @"Path")];
    [defaults setBool:ro forKey:DiskKey(drive, @"ReadOnly")];
    [self updateTitle];
}

- (void)ejectDisk:(NSMenuItem*)sender {
    [_emulator ejectDrive:sender.tag];
    [NSUserDefaults.standardUserDefaults removeObjectForKey:DiskKey(sender.tag, @"Path")];
    [self updateTitle];
}

// Shows the files in a disk image, reusing its window if one is open.
- (void)browseDiskAtPath:(NSString*)path {
    if (!_browsers) _browsers = [NSMutableArray array];
    for (H89DiskBrowser* browser in [_browsers copy]) {
        if (!browser.window.isVisible) {
            [_browsers removeObject:browser];
        } else if ([browser.path isEqualToString:path]) {
            [browser refresh];
            [browser showWindow:nil];
            return;
        }
    }
    H89DiskBrowser* browser = [[H89DiskBrowser alloc] initWithPath:path emulator:_emulator];
    [_browsers addObject:browser];
    [browser showWindow:nil];
}

- (void)showDriveFiles:(NSMenuItem*)sender {
    NSString* path = [_emulator diskPathForDrive:sender.tag];
    if (path) [self browseDiskAtPath:path];
}

- (void)browseDiskImage:(id)sender {
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.canChooseFiles = YES;
    panel.canChooseDirectories = NO;
    panel.allowsMultipleSelection = NO;
    panel.prompt = @"Show Files";
    panel.message = @"Choose an H17 disk image (.h8d) to copy files to or from";
    if ([panel runModal] != NSModalResponseOK) return;
    [self browseDiskAtPath:panel.URL.path];
}

// Shows the boot drive's disk in the title.
- (void)updateTitle {
    NSInteger boot = _emulator.bootFromH37 ? H89FirstH37Drive : 0;
    NSString* path = [_emulator diskPathForDrive:boot];
    _window.title = path ? [NSString stringWithFormat:@"Heathkit H89 — %@ %@", [_emulator driveName:boot],
                                                      path.lastPathComponent]
                         : @"Heathkit H89";
}

- (void)toggleBootDevice:(id)sender {
    [Defaults() setBool:!_emulator.bootFromH37 forKey:H89BootFromH37Key];
    [self preferencesChanged];
}

// Rebuilds the Disks menu each time it opens so it shows what is mounted:
// a submenu per drive, grouped by controller.
- (void)menuNeedsUpdate:(NSMenu*)menu {
    if (menu == _serialMenu) {
        [self rebuildSerialMenu:menu];
        return;
    }
    if (menu != _disksMenu) return;
    [menu removeAllItems];
    [menu addItemWithTitle:@"Browse Disk Image…" action:@selector(browseDiskImage:) keyEquivalent:@"O"].target = self;
    [menu addItemWithTitle:@"Show Disks Folder" action:@selector(showDisksFolder:) keyEquivalent:@""].target = self;
    if ([self bundledDisksDirectory]) {
        [menu addItemWithTitle:@"Restore Original Disks…" action:@selector(restoreOriginalDisks:)
                 keyEquivalent:@""].target = self;
    }
    for (NSInteger d = 0; d < _emulator.driveCount; ++d) {
        BOOL h37 = [_emulator driveIsH37:d];
        if (d == 0 || d == H89FirstH37Drive) {
            [menu addItem:[NSMenuItem separatorItem]];
            NSString* title = h37 ? @"H37 (soft-sectored)" : @"H17 (hard-sectored)";
            if (![_emulator driveIsAvailable:d])
                title = h37 ? @"No H37 controller" : @"No H17 controller (444-19 ROM missing)";
            [menu addItemWithTitle:title action:nil keyEquivalent:@""].enabled = NO;
        }
        if (![_emulator driveIsAvailable:d]) continue;

        NSString* path = [_emulator diskPathForDrive:d];
        NSString* title = path ? [NSString stringWithFormat:@"%@  %@%@", [_emulator driveName:d], path.lastPathComponent,
                                                           [_emulator diskIsReadOnlyInDrive:d] ? @" (write protected)" : @""]
                               : [NSString stringWithFormat:@"%@  empty", [_emulator driveName:d]];
        NSMenuItem* item = [menu addItemWithTitle:title action:nil keyEquivalent:@""];
        NSMenu* sub = [[NSMenu alloc] initWithTitle:title];
        sub.autoenablesItems = NO;
        NSMenuItem* insert = [sub addItemWithTitle:@"Insert Disk…"
                                            action:@selector(insertDisk:)
                                     keyEquivalent:d == 0 ? @"o" : @""];
        insert.tag = d;
        insert.target = self;
        NSMenuItem* files = [sub addItemWithTitle:@"Show Files…" action:@selector(showDriveFiles:) keyEquivalent:@""];
        files.tag = d;
        files.target = self;
        files.enabled = path != nil;
        NSMenuItem* eject = [sub addItemWithTitle:@"Eject"
                                           action:@selector(ejectDisk:)
                                    keyEquivalent:d == 0 ? @"e" : @""];
        eject.tag = d;
        eject.target = self;
        eject.enabled = path != nil;
        item.submenu = sub;
    }
}

- (nullable NSString*)chooseRomDirectoryWithMessage:(NSString*)message {
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.canChooseFiles = NO;
    panel.canChooseDirectories = YES;
    panel.allowsMultipleSelection = NO;
    panel.message = message;
    panel.prompt = @"Use ROMs";
    if ([panel runModal] != NSModalResponseOK) return nil;
    return panel.URL.path;
}

- (void)ringBell {
    // The firmware can ring in bursts (e.g. a full key buffer); one beep per
    // burst is plenty.
    NSTimeInterval now = NSDate.timeIntervalSinceReferenceDate;
    if (_bell == H89BellOff || now - _lastBell < 0.15) return;
    _lastBell = now;
    if (_bell == H89BellSystem) {
        NSBeep();
    } else {
        [_bellSound stop];
        [_bellSound play];
    }
}

- (void)keyClick {
    if (!_keyClick || _speed != 1) return; // clicks would pile up when running fast
    [_clickSound stop];
    [_clickSound play];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender {
    return YES;
}

- (void)tick:(NSTimer*)timer {
    if (_speed == 0) {
        // Maximum: as many frames as fit in most of a 60 Hz tick.
        CFAbsoluteTime start = CFAbsoluteTimeGetCurrent();
        do [_emulator runFrame];
        while (CFAbsoluteTimeGetCurrent() - start < 0.012);
    } else {
        for (NSInteger i = 0; i < _speed; ++i) [_emulator runFrame];
    }
    _view.needsDisplay = YES;
#if H89_DEBUGGER
    if (_emulator.debuggerPaused && !_debuggerWindow.isVisible) [self showDebugger:nil]; // e.g. a breakpoint
    if (_debuggerWindow.isVisible) _debuggerView.needsDisplay = YES;
#endif
}

#if H89_DEBUGGER
- (void)showDebugger:(id)sender {
    if (!_debuggerWindow) {
        NSRect frame = NSMakeRect(0, 0, 780, 700);
        _debuggerWindow = [[NSWindow alloc]
            initWithContentRect:frame
                      styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                        backing:NSBackingStoreBuffered
                          defer:NO];
        _debuggerWindow.title = @"H89 Debugger";
        _debuggerWindow.releasedWhenClosed = NO;
        _debuggerWindow.contentMinSize = NSMakeSize(560, 420);
        _debuggerView = [[H89DebuggerView alloc] initWithFrame:frame];
        _debuggerView.emulator = _emulator;
        _debuggerWindow.contentView = _debuggerView;
        [_debuggerWindow cascadeTopLeftFromPoint:NSMakePoint(NSMaxX(_window.frame) - 120, NSMaxY(_window.frame) - 40)];
    }
    [_debuggerWindow makeKeyAndOrderFront:nil];
}
#endif

- (void)chooseSpeed:(NSMenuItem*)sender {
    [Defaults() setInteger:sender.tag forKey:H89SpeedKey];
    [self preferencesChanged];
}

- (void)toggleTurbo:(id)sender {
    [Defaults() setInteger:_speed == 0 ? _lastSpeed : 0 forKey:H89SpeedKey];
    [self preferencesChanged];
}

#pragma mark - Serial ports

- (void)connectSerial:(NSInteger)port spec:(NSString*)spec {
    NSError* error = nil;
    if (![_emulator connectSerialPort:(H89SerialPort)port spec:spec error:&error]) {
        [[NSAlert alertWithError:error] runModal];
        return;
    }
    [NSUserDefaults.standardUserDefaults setObject:spec forKey:SerialKey(port)];
}

// Asks for one line of text; nil if cancelled.
- (nullable NSString*)askFor:(NSString*)question detail:(NSString*)detail initial:(NSString*)initial {
    NSAlert* alert = [[NSAlert alloc] init];
    alert.messageText = question;
    alert.informativeText = detail;
    [alert addButtonWithTitle:@"OK"];
    [alert addButtonWithTitle:@"Cancel"];
    NSTextField* field = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 240, 24)];
    field.stringValue = initial;
    alert.accessoryView = field;
    alert.window.initialFirstResponder = field;
    if ([alert runModal] != NSAlertFirstButtonReturn) return nil;
    NSString* text = [field.stringValue stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
    return text.length ? text : nil;
}

- (void)serialToFile:(NSMenuItem*)sender {
    NSSavePanel* panel = [NSSavePanel savePanel];
    panel.nameFieldStringValue = sender.tag == H89SerialPortPrinter ? @"H89 Printer.txt" : @"H89 Serial.txt";
    panel.message = @"Output is added to the end of this file.";
    if ([panel runModal] != NSModalResponseOK) return;
    [self connectSerial:sender.tag spec:[@"file:" stringByAppendingString:panel.URL.path]];
}

- (void)serialListen:(NSMenuItem*)sender {
    NSString* port = [self askFor:@"Listen on which TCP port?"
                           detail:@"Connect to it from this Mac, for example with: telnet localhost 8089"
                          initial:@"8089"];
    if (port) [self connectSerial:sender.tag spec:[@"listen:" stringByAppendingString:port]];
}

- (void)serialConnect:(NSMenuItem*)sender {
    NSString* target = [self askFor:@"Connect to which host and port?" detail:@"For example: localhost:2323"
                            initial:@""];
    if (target) [self connectSerial:sender.tag spec:[@"connect:" stringByAppendingString:target]];
}

- (void)serialDisconnect:(NSMenuItem*)sender {
    [_emulator disconnectSerialPort:(H89SerialPort)sender.tag];
    [NSUserDefaults.standardUserDefaults removeObjectForKey:SerialKey(sender.tag)];
}

// Rebuilt when opened, to show each port's connection.
- (void)rebuildSerialMenu:(NSMenu*)menu {
    [menu removeAllItems];
    for (NSInteger p = 0; p < 3; ++p) {
        if (p) [menu addItem:[NSMenuItem separatorItem]];
        NSString* title = [NSString stringWithFormat:@"%@ — %@", kSerialNames[p],
                                                     [_emulator serialPortStatus:(H89SerialPort)p]];
        [menu addItemWithTitle:title action:nil keyEquivalent:@""].enabled = NO;
        struct {
            NSString* title;
            SEL action;
        } items[] = {
            {@"Send Output to File…", @selector(serialToFile:)},
            {@"Listen on TCP Port…", @selector(serialListen:)},
            {@"Connect to TCP Host…", @selector(serialConnect:)},
            {@"Disconnect", @selector(serialDisconnect:)},
        };
        for (const auto& i : items) {
            NSMenuItem* item = [menu addItemWithTitle:i.title action:i.action keyEquivalent:@""];
            item.tag = p;
            item.target = self;
            item.indentationLevel = 1;
            if (i.action == @selector(serialDisconnect:)) item.enabled = [_emulator serialPortSpec:(H89SerialPort)p] != nil;
        }
    }
}

#pragma mark - Save states

- (void)saveState:(id)sender {
    NSData* state = _emulator.saveState; // the moment the command was chosen
    NSSavePanel* panel = [NSSavePanel savePanel];
    panel.nameFieldStringValue = @"H89.h89state";
    panel.message = @"Save the machine's state (memory, CPU, terminal and which disks are in the drives)";
    if ([panel runModal] != NSModalResponseOK) return;
    NSError* error = nil;
    if (![state writeToURL:panel.URL options:NSDataWritingAtomic error:&error]) [[NSAlert alertWithError:error] runModal];
}

- (void)loadState:(id)sender {
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.canChooseFiles = YES;
    panel.canChooseDirectories = NO;
    panel.allowsMultipleSelection = NO;
    panel.message = @"Choose a saved machine state";
    if ([panel runModal] != NSModalResponseOK) return;
    NSError* error = nil;
    NSData* data = [NSData dataWithContentsOfURL:panel.URL options:0 error:&error];
    NSArray<NSString*>* warnings = nil;
    if (!data || ![_emulator loadState:data warnings:&warnings error:&error]) {
        [[NSAlert alertWithError:error] runModal];
        return;
    }
    // The state brings its own disks and boot switch: remember them as if
    // chosen from the menus.
    NSUserDefaults* defaults = NSUserDefaults.standardUserDefaults;
    for (NSInteger d = 0; d < _emulator.driveCount; ++d) {
        NSString* path = [_emulator diskPathForDrive:d];
        if (path) {
            [defaults setObject:path forKey:DiskKey(d, @"Path")];
            [defaults setBool:[_emulator diskIsReadOnlyInDrive:d] forKey:DiskKey(d, @"ReadOnly")];
        } else {
            [defaults removeObjectForKey:DiskKey(d, @"Path")];
        }
    }
    [defaults setBool:_emulator.bootFromH37 forKey:H89BootFromH37Key];
    [self updateTitle];
    for (H89DiskBrowser* browser in _browsers) [browser refresh];
    _view.needsDisplay = YES;
    if (warnings.count) {
        NSAlert* alert = [[NSAlert alloc] init];
        alert.messageText = @"The state was loaded, but check its disks.";
        alert.informativeText = [warnings componentsJoinedByString:@"\n\n"];
        [alert runModal];
    }
}

- (void)reset:(id)sender {
    [_emulator reset];
}

- (void)toggleCrtEffects:(id)sender {
    [Defaults() setBool:!_view.crtEffects forKey:H89CrtEffectsKey];
    [self preferencesChanged];
}

- (void)choosePhosphor:(NSMenuItem*)sender {
    [Defaults() setInteger:sender.tag forKey:H89PhosphorKey];
    [self preferencesChanged];
}

- (void)showAbout:(id)sender {
    NSString* credits = [NSString stringWithFormat:@"A Heathkit H89 (Zenith Z-89) emulator: the CPU board, H19 "
                                                   @"terminal, H17 and H37 floppy controllers.\n\n%@",
                                                   _emulator.romSummary ?: @""];
    NSMutableParagraphStyle* style = [[NSMutableParagraphStyle alloc] init];
    style.alignment = NSTextAlignmentCenter;
    NSAttributedString* text = [[NSAttributedString alloc]
        initWithString:credits
            attributes:@{
                NSFontAttributeName : [NSFont systemFontOfSize:NSFont.smallSystemFontSize],
                NSForegroundColorAttributeName : NSColor.labelColor,
                NSParagraphStyleAttributeName : style,
            }];
    [NSApp orderFrontStandardAboutPanelWithOptions:@{NSAboutPanelOptionCredits : text}];
}

- (BOOL)validateMenuItem:(NSMenuItem*)item {
    if (item.action == @selector(chooseSpeed:)) {
        item.state = item.tag == _speed ? NSControlStateValueOn : NSControlStateValueOff;
    }
    if (item.action == @selector(toggleTurbo:)) {
        item.state = _speed == 0 ? NSControlStateValueOn : NSControlStateValueOff;
    }
    if (item.action == @selector(toggleBootDevice:)) {
        item.state = _emulator.bootFromH37 ? NSControlStateValueOn : NSControlStateValueOff;
    }
    if (item.action == @selector(toggleCrtEffects:)) {
        item.state = _view.crtEffects ? NSControlStateValueOn : NSControlStateValueOff;
    }
    if (item.action == @selector(choosePhosphor:)) {
        item.state = item.tag == _view.phosphor ? NSControlStateValueOn : NSControlStateValueOff;
    }
    return YES;
}

- (void)buildMenus {
    NSMenu* bar = [[NSMenu alloc] init];

    NSMenuItem* appItem = [bar addItemWithTitle:@"" action:nil keyEquivalent:@""];
    NSMenu* appMenu = [[NSMenu alloc] init];
    [appMenu addItemWithTitle:@"About H89" action:@selector(showAbout:) keyEquivalent:@""].target = self;
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItemWithTitle:@"Settings…" action:@selector(showPreferences:) keyEquivalent:@","].target = self;
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItemWithTitle:@"Hide H89" action:@selector(hide:) keyEquivalent:@"h"];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItemWithTitle:@"Quit H89" action:@selector(terminate:) keyEquivalent:@"q"];
    appItem.submenu = appMenu;

    NSMenuItem* machineItem = [bar addItemWithTitle:@"" action:nil keyEquivalent:@""];
    NSMenu* machineMenu = [[NSMenu alloc] initWithTitle:@"Machine"];
    [machineMenu addItemWithTitle:@"Reset" action:@selector(reset:) keyEquivalent:@"r"].target = self;
    [machineMenu addItem:[NSMenuItem separatorItem]];
    NSMenuItem* speedItem = [machineMenu addItemWithTitle:@"Speed" action:nil keyEquivalent:@""];
    NSMenu* speedMenu = [[NSMenu alloc] initWithTitle:@"Speed"];
    NSArray* speedNames = @[ @"Normal (2 MHz)", @"2×", @"4×", @"Maximum" ];
    for (NSUInteger i = 0; i < speedNames.count; ++i) {
        NSMenuItem* item = [speedMenu addItemWithTitle:speedNames[i] action:@selector(chooseSpeed:) keyEquivalent:@""];
        item.tag = kSpeeds[i];
        item.target = self;
    }
    speedItem.submenu = speedMenu;
    [machineMenu addItemWithTitle:@"Turbo" action:@selector(toggleTurbo:) keyEquivalent:@"t"].target = self;
    [machineMenu addItem:[NSMenuItem separatorItem]];
    NSMenuItem* serialItem = [machineMenu addItemWithTitle:@"Serial Ports" action:nil keyEquivalent:@""];
    _serialMenu = [[NSMenu alloc] initWithTitle:@"Serial Ports"];
    _serialMenu.delegate = self;
    _serialMenu.autoenablesItems = NO;
    serialItem.submenu = _serialMenu;
    [machineMenu addItem:[NSMenuItem separatorItem]];
#if H89_DEBUGGER
    [machineMenu addItemWithTitle:@"Debugger" action:@selector(showDebugger:) keyEquivalent:@"d"].target = self;
    [machineMenu addItem:[NSMenuItem separatorItem]];
#endif
    [machineMenu addItemWithTitle:@"Save State…" action:@selector(saveState:) keyEquivalent:@"s"].target = self;
    [machineMenu addItemWithTitle:@"Load State…" action:@selector(loadState:) keyEquivalent:@"l"].target = self;
    [machineMenu addItem:[NSMenuItem separatorItem]];
    // SW501 section 4: which controller the monitor's B command boots.
    [machineMenu addItemWithTitle:@"Boot from H37 (DK0:)"
                           action:@selector(toggleBootDevice:)
                    keyEquivalent:@""].target = self;
    machineItem.submenu = machineMenu;

    NSMenuItem* disksItem = [bar addItemWithTitle:@"" action:nil keyEquivalent:@""];
    _disksMenu = [[NSMenu alloc] initWithTitle:@"Disks"];
    _disksMenu.delegate = self;
    _disksMenu.autoenablesItems = NO;
    disksItem.submenu = _disksMenu;

    NSMenuItem* viewItem = [bar addItemWithTitle:@"" action:nil keyEquivalent:@""];
    NSMenu* viewMenu = [[NSMenu alloc] initWithTitle:@"View"];
    NSArray* names = @[ @"White Phosphor", @"Green Phosphor", @"Amber Phosphor" ];
    for (NSInteger i = 0; i < (NSInteger)names.count; ++i) {
        NSMenuItem* item = [viewMenu addItemWithTitle:names[i]
                                               action:@selector(choosePhosphor:)
                                        keyEquivalent:[NSString stringWithFormat:@"%ld", (long)i + 1]];
        item.tag = i;
        item.target = self;
    }
    [viewMenu addItem:[NSMenuItem separatorItem]];
    [viewMenu addItemWithTitle:@"CRT Effects" action:@selector(toggleCrtEffects:) keyEquivalent:@""].target = self;
    [viewMenu addItem:[NSMenuItem separatorItem]];
    [viewMenu addItemWithTitle:@"Enter Full Screen"
                        action:@selector(toggleFullScreen:)
                 keyEquivalent:@"f"].keyEquivalentModifierMask =
        NSEventModifierFlagCommand | NSEventModifierFlagControl;
    viewItem.submenu = viewMenu;

    NSApp.mainMenu = bar;
}

@end
