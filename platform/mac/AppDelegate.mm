#import "AppDelegate.h"

#import "H89Emulator.h"
#import "H89View.h"

static NSString* const kRomDirectoryKey = @"RomDirectory";

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

    if (![self startEmulatorPrompting:NO]) {
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
}

// Loads the ROMs from the H89_ROMS environment variable or the saved folder,
// asking for a folder when those fail (or always, if `prompt`). Returns NO if
// the user cancels.
- (BOOL)startEmulatorPrompting:(BOOL)prompt {
    NSString* dir = NSProcessInfo.processInfo.environment[@"H89_ROMS"];
    if (!dir) dir = [NSUserDefaults.standardUserDefaults stringForKey:kRomDirectoryKey];

    NSError* error = nil;
    H89Emulator* emulator = nil;
    if (dir && !prompt) emulator = [[H89Emulator alloc] initWithRomDirectory:dir error:&error];

    while (!emulator) {
        NSString* message = error ? error.localizedDescription
                                  : @"Choose the folder containing your H89 and H19 ROM images.";
        dir = [self chooseRomDirectoryWithMessage:message];
        if (!dir) return NO;
        emulator = [[H89Emulator alloc] initWithRomDirectory:dir error:&error];
    }

    [NSUserDefaults.standardUserDefaults setObject:dir forKey:kRomDirectoryKey];
    __weak AppDelegate* weakSelf = self;
    emulator.bellHandler = ^{ [weakSelf ringBell]; };
    _emulator = emulator;
    _view.emulator = emulator;
    [self restoreDisks];
    return YES;
}

#pragma mark - Disks

- (void)restoreDisks {
    NSUserDefaults* defaults = NSUserDefaults.standardUserDefaults;
    for (NSInteger d = 0; d < _emulator.driveCount; ++d) {
        NSString* path = [defaults stringForKey:DiskKey(d, @"Path")];
        if (!path) continue;
        if (![_emulator insertDiskAtPath:path drive:d readOnly:[defaults boolForKey:DiskKey(d, @"ReadOnly")] error:nil])
            [defaults removeObjectForKey:DiskKey(d, @"Path")];
    }
    [self updateTitle];
}

- (void)insertDisk:(NSMenuItem*)sender {
    NSInteger drive = sender.tag;
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.message = [NSString stringWithFormat:@"Choose an H17 disk image (.h8d) for SY%ld:", (long)drive];
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

- (void)updateTitle {
    NSString* sy0 = [_emulator diskPathForDrive:0];
    _window.title = sy0 ? [NSString stringWithFormat:@"Heathkit H89 — SY0: %@", sy0.lastPathComponent]
                        : @"Heathkit H89";
}

// Rebuilds the Disks menu each time it opens so it shows what is mounted.
- (void)menuNeedsUpdate:(NSMenu*)menu {
    if (menu != _disksMenu) return;
    [menu removeAllItems];
    if (_emulator.driveCount == 0) {
        [menu addItemWithTitle:@"No H17 Controller (444-19 ROM missing)" action:nil keyEquivalent:@""].enabled = NO;
        return;
    }
    for (NSInteger d = 0; d < _emulator.driveCount; ++d) {
        NSString* path = [_emulator diskPathForDrive:d];
        NSString* status = path ? [NSString stringWithFormat:@"SY%ld:  %@%@", (long)d, path.lastPathComponent,
                                                            [_emulator diskIsReadOnlyInDrive:d] ? @" (write protected)" : @""]
                                : [NSString stringWithFormat:@"SY%ld:  empty", (long)d];
        [menu addItemWithTitle:status action:nil keyEquivalent:@""].enabled = NO;
        NSMenuItem* insert = [menu addItemWithTitle:@"Insert Disk…"
                                             action:@selector(insertDisk:)
                                      keyEquivalent:d == 0 ? @"o" : @""];
        insert.tag = d;
        insert.target = self;
        insert.indentationLevel = 1;
        NSMenuItem* eject = [menu addItemWithTitle:@"Eject"
                                            action:path ? @selector(ejectDisk:) : nil
                                     keyEquivalent:d == 0 ? @"e" : @""];
        eject.tag = d;
        eject.target = self;
        eject.enabled = path != nil;
        eject.indentationLevel = 1;
        if (d + 1 < _emulator.driveCount) [menu addItem:[NSMenuItem separatorItem]];
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
    if (now - _lastBell < 0.15) return;
    _lastBell = now;
    NSBeep();
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender {
    return YES;
}

- (void)tick:(NSTimer*)timer {
    [_emulator runFrame];
    _view.needsDisplay = YES;
}

- (void)reset:(id)sender {
    [_emulator reset];
}

- (void)chooseRomFolder:(id)sender {
    [self startEmulatorPrompting:YES];
}

- (void)choosePhosphor:(NSMenuItem*)sender {
    _view.phosphor = (H89Phosphor)sender.tag;
}

- (BOOL)validateMenuItem:(NSMenuItem*)item {
    if (item.action == @selector(choosePhosphor:)) {
        item.state = item.tag == _view.phosphor ? NSControlStateValueOn : NSControlStateValueOff;
    }
    return YES;
}

- (void)buildMenus {
    NSMenu* bar = [[NSMenu alloc] init];

    NSMenuItem* appItem = [bar addItemWithTitle:@"" action:nil keyEquivalent:@""];
    NSMenu* appMenu = [[NSMenu alloc] init];
    [appMenu addItemWithTitle:@"About H89"
                       action:@selector(orderFrontStandardAboutPanel:)
                keyEquivalent:@""];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItemWithTitle:@"Quit H89" action:@selector(terminate:) keyEquivalent:@"q"];
    appItem.submenu = appMenu;

    NSMenuItem* machineItem = [bar addItemWithTitle:@"" action:nil keyEquivalent:@""];
    NSMenu* machineMenu = [[NSMenu alloc] initWithTitle:@"Machine"];
    [machineMenu addItemWithTitle:@"Reset" action:@selector(reset:) keyEquivalent:@"r"].target = self;
    [machineMenu addItem:[NSMenuItem separatorItem]];
    [machineMenu addItemWithTitle:@"Choose ROM Folder…"
                           action:@selector(chooseRomFolder:)
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
    [viewMenu addItemWithTitle:@"Enter Full Screen"
                        action:@selector(toggleFullScreen:)
                 keyEquivalent:@"f"].keyEquivalentModifierMask =
        NSEventModifierFlagCommand | NSEventModifierFlagControl;
    viewItem.submenu = viewMenu;

    NSApp.mainMenu = bar;
}

@end
