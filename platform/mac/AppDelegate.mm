#import "AppDelegate.h"

#import "H89DiskBrowser.h"
#import "H89Emulator.h"
#import "H89View.h"

static NSString* const kRomDirectoryKey = @"RomDirectory";
static NSString* const kBootFromH37Key = @"BootFromH37";
static NSString* const kSpeedKey = @"Speed";
static NSString* const kCrtEffectsKey = @"CRTEffects";

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
    NSInteger _speed;      // frames per tick; 0 = maximum
    NSInteger _lastSpeed;  // to return to from turbo
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
    NSUserDefaults* defaults = NSUserDefaults.standardUserDefaults;
    _view.crtEffects = [defaults objectForKey:kCrtEffectsKey] ? [defaults boolForKey:kCrtEffectsKey] : YES;
    _window.contentView = _view;
    [_window center];
    [_window makeKeyAndOrderFront:nil];
    [_window makeFirstResponder:_view];
    [NSApp activateIgnoringOtherApps:YES];

    _speed = [NSUserDefaults.standardUserDefaults objectForKey:kSpeedKey]
                 ? [NSUserDefaults.standardUserDefaults integerForKey:kSpeedKey]
                 : 1;
    _lastSpeed = _speed ? _speed : 1;
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
    emulator.bootFromH37 = [NSUserDefaults.standardUserDefaults boolForKey:kBootFromH37Key];
    [self restoreDisks];
    for (NSInteger p = 0; p < 3; ++p) {
        NSString* spec = [NSUserDefaults.standardUserDefaults stringForKey:SerialKey(p)];
        if (spec) [emulator connectSerialPort:(H89SerialPort)p spec:spec error:nil];
    }
    for (H89DiskBrowser* browser in _browsers) {
        browser.emulator = emulator;
        [browser refresh];
    }
    return YES;
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
    _emulator.bootFromH37 = !_emulator.bootFromH37;
    [NSUserDefaults.standardUserDefaults setBool:_emulator.bootFromH37 forKey:kBootFromH37Key];
    [self updateTitle];
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
    if (now - _lastBell < 0.15) return;
    _lastBell = now;
    NSBeep();
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
}

- (void)chooseSpeed:(NSMenuItem*)sender {
    _speed = sender.tag;
    if (_speed) _lastSpeed = _speed;
    [NSUserDefaults.standardUserDefaults setInteger:_speed forKey:kSpeedKey];
}

- (void)toggleTurbo:(id)sender {
    _speed = _speed == 0 ? _lastSpeed : 0;
    [NSUserDefaults.standardUserDefaults setInteger:_speed forKey:kSpeedKey];
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
    [defaults setBool:_emulator.bootFromH37 forKey:kBootFromH37Key];
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

- (void)chooseRomFolder:(id)sender {
    [self startEmulatorPrompting:YES];
}

- (void)toggleCrtEffects:(id)sender {
    _view.crtEffects = !_view.crtEffects;
    [NSUserDefaults.standardUserDefaults setBool:_view.crtEffects forKey:kCrtEffectsKey];
}

- (void)choosePhosphor:(NSMenuItem*)sender {
    _view.phosphor = (H89Phosphor)sender.tag;
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
    [machineMenu addItemWithTitle:@"Save State…" action:@selector(saveState:) keyEquivalent:@"s"].target = self;
    [machineMenu addItemWithTitle:@"Load State…" action:@selector(loadState:) keyEquivalent:@"l"].target = self;
    [machineMenu addItem:[NSMenuItem separatorItem]];
    // SW501 section 4: which controller the monitor's B command boots.
    [machineMenu addItemWithTitle:@"Boot from H37 (DK0:)"
                           action:@selector(toggleBootDevice:)
                    keyEquivalent:@""].target = self;
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
