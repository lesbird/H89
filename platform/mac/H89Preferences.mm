#import "H89Preferences.h"

NSString* const H89RomDirectoryKey = @"RomDirectory";
NSString* const H89MonitorKey = @"Monitor";
NSString* const H89MemoryKey = @"MemoryKB";
NSString* const H89BootFromH37Key = @"BootFromH37";
NSString* const H89AutoBootKey = @"AutoBoot";
NSString* const H89SpeedKey = @"Speed";
NSString* const H89PhosphorKey = @"Phosphor";
NSString* const H89CrtEffectsKey = @"CRTEffects";
NSString* const H89BellKey = @"Bell";
NSString* const H89KeyClickKey = @"KeyClick";

@implementation H89Preferences {
    __weak id<H89PreferencesDelegate> _delegate;
    NSPopUpButton* _roms;
    NSTextField* _romPath;
    NSPopUpButton* _monitor;
    NSPopUpButton* _memory;
    NSPopUpButton* _bootDevice;
    NSButton* _autoBoot;
    NSPopUpButton* _phosphor;
    NSButton* _crt;
    NSPopUpButton* _speed;
    NSPopUpButton* _bell;
    NSButton* _keyClick;
    NSTextField* _disksPath;
}

static NSUserDefaults* Defaults() {
    return NSUserDefaults.standardUserDefaults;
}

- (instancetype)initWithDelegate:(id<H89PreferencesDelegate>)delegate {
    NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 520, 480)
                                                   styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                                     backing:NSBackingStoreBuffered
                                                       defer:YES];
    window.title = @"H89 Settings";
    if ((self = [super initWithWindow:window])) {
        _delegate = delegate;
        [self buildContent];
        [window center];
    }
    return self;
}

#pragma mark - Building

- (NSPopUpButton*)popUp:(NSArray<NSString*>*)titles tags:(NSArray<NSNumber*>*)tags action:(SEL)action {
    NSPopUpButton* popUp = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    for (NSUInteger i = 0; i < titles.count; ++i) {
        [popUp addItemWithTitle:titles[i]];
        popUp.lastItem.tag = tags[i].integerValue;
    }
    popUp.target = self;
    popUp.action = action;
    return popUp;
}

- (NSButton*)checkbox:(NSString*)title action:(SEL)action {
    NSButton* box = [NSButton checkboxWithTitle:title target:self action:action];
    return box;
}

- (NSTextField*)note:(NSString*)text {
    NSTextField* label = [NSTextField wrappingLabelWithString:text];
    label.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
    label.textColor = NSColor.secondaryLabelColor;
    label.preferredMaxLayoutWidth = 340;
    return label;
}

- (NSTextField*)heading:(NSString*)text {
    NSTextField* label = [NSTextField labelWithString:text];
    label.font = [NSFont boldSystemFontOfSize:NSFont.systemFontSize];
    return label;
}

- (void)buildContent {
    _roms = [self popUp:@[ @"Built into H89", @"Custom Folder…" ] tags:@[ @0, @1 ] action:@selector(romsChanged:)];
    _romPath = [self note:@""];
    _romPath.lineBreakMode = NSLineBreakByTruncatingMiddle;
    _romPath.maximumNumberOfLines = 1;
    _monitor = [self popUp:@[ @"MTR-90 (444-142)", @"MTR-89 (444-62)" ]
                      tags:@[ @90, @89 ]
                    action:@selector(machineChanged:)];
    _memory = [self popUp:@[ @"16K", @"32K", @"48K", @"64K" ]
                     tags:@[ @16, @32, @48, @64 ]
                   action:@selector(machineChanged:)];
    _bootDevice = [self popUp:@[ @"H17 hard-sectored (SY0:)", @"H37 soft-sectored (DK0:)" ]
                         tags:@[ @0, @1 ]
                       action:@selector(settingChanged:)];
    _autoBoot = [self checkbox:@"Boot from the disk at power-on and reset" action:@selector(settingChanged:)];
    _phosphor = [self popUp:@[ @"White (P4)", @"Green (P1)", @"Amber (P3)" ]
                       tags:@[ @0, @1, @2 ]
                     action:@selector(settingChanged:)];
    _crt = [self checkbox:@"CRT effects (glow, persistence, curved screen)" action:@selector(settingChanged:)];
    _speed = [self popUp:@[ @"Normal (2 MHz)", @"2×", @"4×", @"Maximum" ]
                    tags:@[ @1, @2, @4, @0 ]
                  action:@selector(settingChanged:)];
    _bell = [self popUp:@[ @"H19 speaker", @"System alert sound", @"Off" ]
                   tags:@[ @(H89BellH19), @(H89BellSystem), @(H89BellOff) ]
                 action:@selector(settingChanged:)];
    _keyClick = [self checkbox:@"Click when a key is pressed" action:@selector(settingChanged:)];

    _disksPath = [self note:@""];
    _disksPath.lineBreakMode = NSLineBreakByTruncatingMiddle;
    _disksPath.maximumNumberOfLines = 1;
    _disksPath.selectable = YES;
    NSButton* show = [NSButton buttonWithTitle:@"Show in Finder" target:self action:@selector(showDisks:)];
    NSButton* restore = [NSButton buttonWithTitle:@"Restore Originals…" target:self action:@selector(restoreDisks:)];
    NSStackView* diskButtons = [NSStackView stackViewWithViews:@[ show, restore ]];

    NSView* empty = NSGridCell.emptyContentView;
    NSGridView* grid = [NSGridView gridViewWithViews:@[
        @[ [self heading:@"Machine"], empty ],
        @[ [NSTextField labelWithString:@"ROMs:"], _roms ],
        @[ empty, _romPath ],
        @[ [NSTextField labelWithString:@"Monitor:"], _monitor ],
        @[ [NSTextField labelWithString:@"Memory:"], _memory ],
        @[ empty, [self note:@"Changing the ROMs, monitor or memory restarts the H89."] ],
        @[ [NSTextField labelWithString:@"Boot from:"], _bootDevice ],
        @[ empty, _autoBoot ],
        @[ [NSTextField labelWithString:@"Speed:"], _speed ],
        @[ [self heading:@"Display and sound"], empty ],
        @[ [NSTextField labelWithString:@"Phosphor:"], _phosphor ],
        @[ empty, _crt ],
        @[ [NSTextField labelWithString:@"Bell:"], _bell ],
        @[ empty, _keyClick ],
        @[ [self heading:@"Disks"], empty ],
        @[ [NSTextField labelWithString:@"Your disks:"], _disksPath ],
        @[ empty, diskButtons ],
        @[ empty, [self note:@"The disks that came with H89 are copied here, so changes "
                              @"never touch the originals. Restore Originals puts them back "
                              @"as they came."] ],
    ]];
    grid.translatesAutoresizingMaskIntoConstraints = NO;
    grid.rowSpacing = 8;
    grid.columnSpacing = 8;
    [grid columnAtIndex:0].xPlacement = NSGridCellPlacementTrailing;
    [grid columnAtIndex:1].width = 340;
    grid.rowAlignment = NSGridRowAlignmentFirstBaseline;
    for (NSInteger r = 0; r < grid.numberOfRows; ++r) {
        NSGridCell* first = [grid cellAtColumnIndex:0 rowIndex:r];
        NSView* v = first.contentView;
        if ([v isKindOfClass:NSTextField.class] && ((NSTextField*)v).font.fontDescriptor.symbolicTraits &
                                                        NSFontDescriptorTraitBold) {
            [grid mergeCellsInHorizontalRange:NSMakeRange(0, 2) verticalRange:NSMakeRange(r, 1)];
            first.xPlacement = NSGridCellPlacementLeading;
            [grid rowAtIndex:r].topPadding = r ? 10 : 0;
        }
    }

    NSView* content = self.window.contentView;
    [content addSubview:grid];
    [NSLayoutConstraint activateConstraints:@[
        [grid.topAnchor constraintEqualToAnchor:content.topAnchor constant:20],
        [grid.leadingAnchor constraintEqualToAnchor:content.leadingAnchor constant:20],
        [grid.trailingAnchor constraintEqualToAnchor:content.trailingAnchor constant:-20],
        [grid.bottomAnchor constraintEqualToAnchor:content.bottomAnchor constant:-20],
    ]];
}

#pragma mark - Showing

- (void)show {
    [self load];
    [self showWindow:nil];
    [self.window makeKeyAndOrderFront:nil];
}

static void SelectTag(NSPopUpButton* popUp, NSInteger tag) {
    if (![popUp selectItemWithTag:tag]) [popUp selectItemAtIndex:popUp.numberOfItems - 1];
}

- (void)reload {
    if (self.isWindowLoaded && self.window.isVisible) [self load];
}

- (void)load {
    NSUserDefaults* d = Defaults();
    NSString* romDir = [d stringForKey:H89RomDirectoryKey];
    [_roms selectItemWithTag:romDir ? 1 : 0];
    _romPath.stringValue = romDir ? romDir.stringByAbbreviatingWithTildeInPath
                                  : @"MTR-90, MTR-89, H17 boot, H19 firmware and font";
    _romPath.toolTip = romDir;
    SelectTag(_monitor, [d integerForKey:H89MonitorKey] == 89 ? 89 : 90);
    SelectTag(_memory, [d objectForKey:H89MemoryKey] ? [d integerForKey:H89MemoryKey] : 64);
    [_bootDevice selectItemWithTag:[d boolForKey:H89BootFromH37Key] ? 1 : 0];
    _autoBoot.state = [d boolForKey:H89AutoBootKey] ? NSControlStateValueOn : NSControlStateValueOff;
    SelectTag(_speed, [d objectForKey:H89SpeedKey] ? [d integerForKey:H89SpeedKey] : 1);
    [_phosphor selectItemWithTag:[d integerForKey:H89PhosphorKey]];
    _crt.state = ![d objectForKey:H89CrtEffectsKey] || [d boolForKey:H89CrtEffectsKey];
    [_bell selectItemWithTag:[d integerForKey:H89BellKey]];
    _keyClick.state = ![d objectForKey:H89KeyClickKey] || [d boolForKey:H89KeyClickKey];
    NSString* disks = [_delegate disksDirectory];
    _disksPath.stringValue = disks.stringByAbbreviatingWithTildeInPath;
    _disksPath.toolTip = disks;
}

#pragma mark - Actions

- (void)romsChanged:(id)sender {
    NSUserDefaults* d = Defaults();
    if (_roms.selectedTag == 0) {
        if (![d stringForKey:H89RomDirectoryKey]) return;
        [d removeObjectForKey:H89RomDirectoryKey];
    } else {
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        panel.canChooseFiles = NO;
        panel.canChooseDirectories = YES;
        panel.allowsMultipleSelection = NO;
        panel.prompt = @"Use ROMs";
        panel.message = @"Choose a folder of H89 and H19 ROM images, named by part number (444-142, 444-46, …)";
        if ([panel runModal] != NSModalResponseOK) {
            [self load];
            return;
        }
        [d setObject:panel.URL.path forKey:H89RomDirectoryKey];
    }
    [_delegate preferencesChangedMachine];
    [self load]; // the delegate may have refused the folder
}

- (void)machineChanged:(id)sender {
    NSUserDefaults* d = Defaults();
    NSInteger monitor = _monitor.selectedTag, memory = _memory.selectedTag;
    if (monitor == ([d integerForKey:H89MonitorKey] == 89 ? 89 : 90) &&
        memory == ([d objectForKey:H89MemoryKey] ? [d integerForKey:H89MemoryKey] : 64))
        return;
    [d setInteger:monitor forKey:H89MonitorKey];
    [d setInteger:memory forKey:H89MemoryKey];
    [_delegate preferencesChangedMachine];
}

- (void)settingChanged:(id)sender {
    NSUserDefaults* d = Defaults();
    [d setBool:_bootDevice.selectedTag == 1 forKey:H89BootFromH37Key];
    [d setBool:_autoBoot.state == NSControlStateValueOn forKey:H89AutoBootKey];
    [d setInteger:_speed.selectedTag forKey:H89SpeedKey];
    [d setInteger:_phosphor.selectedTag forKey:H89PhosphorKey];
    [d setBool:_crt.state == NSControlStateValueOn forKey:H89CrtEffectsKey];
    [d setInteger:_bell.selectedTag forKey:H89BellKey];
    [d setBool:_keyClick.state == NSControlStateValueOn forKey:H89KeyClickKey];
    [_delegate preferencesChanged];
}

- (void)showDisks:(id)sender {
    [NSWorkspace.sharedWorkspace openURL:[NSURL fileURLWithPath:[_delegate disksDirectory] isDirectory:YES]];
}

- (void)restoreDisks:(id)sender {
    [_delegate restoreOriginalDisks];
}

@end
