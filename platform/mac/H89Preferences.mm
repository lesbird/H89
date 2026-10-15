#import "H89Preferences.h"
#import "H89Emulator.h"

#include "SerialPort.h"
#include "terminal/H19.h"

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
NSString* const H89TerminalOnlyKey = @"TerminalOnly";
NSString* const H89TerminalPortKey = @"TerminalPort";
NSString* const H89TerminalBaudKey = @"TerminalBaud";
NSString* const H89TerminalFormatKey = @"TerminalFormat";
NSString* const H89TerminalRtsCtsKey = @"TerminalRTSCTS";
NSString* const H89TextLineEndingKey = @"TextLineEnding";
NSString* const H89TextCharDelayKey = @"TextCharDelay";
NSString* const H89TextLineDelayKey = @"TextLineDelay";

static NSUserDefaults* Defaults() {
    return NSUserDefaults.standardUserDefaults;
}

// The H19 switches offered in Settings: (defaults key, switch, bit, default).
struct SwitchSetting {
    NSString* key;
    NSString* title;
    int sw; // 401 or 402
    uint8_t bit;
    bool fallback;
};
static const SwitchSetting kSwitches[] = {
    {@"H19FullDuplex", @"Full duplex (off: keys also show on the screen)", 401, h89::sw401::kFullDuplex, true},
    {@"H19BlockCursor", @"Block cursor", 402, h89::sw402::kBlockCursor, false},
    {@"H19WrapAtEnd", @"Wrap at the end of a line", 402, h89::sw402::kWrapAtEol, false},
    {@"H19AutoLF", @"Line feed after each received carriage return", 402, h89::sw402::kAutoLf, false},
    {@"H19AutoCR", @"Carriage return after each received line feed", 402, h89::sw402::kAutoCr, false},
    {@"H19ANSI", @"ANSI escape sequences (off: Heath)", 402, h89::sw402::kAnsiMode, false},
    {@"H19KeypadShifted", @"Keypad shifted", 402, h89::sw402::kKeypadShifted, false},
};

static bool SwitchOn(const SwitchSetting& s) {
    id v = [Defaults() objectForKey:s.key];
    return v ? [v boolValue] : s.fallback;
}

void H89TerminalSwitches(NSInteger* sw401, NSInteger* sw402) {
    // The baud rate bits matter only inside an H89, whose console runs at
    // 9600; on a host serial port the port's rate is used.
    uint8_t a = h89::sw401::kBaud9600, b = 0;
    for (const auto& s : kSwitches)
        if (SwitchOn(s)) (s.sw == 401 ? a : b) |= s.bit;
    *sw401 = a;
    *sw402 = b;
}

NSString* _Nullable H89TerminalSpec(void) {
    NSUserDefaults* d = Defaults();
    NSString* port = [d stringForKey:H89TerminalPortKey];
    if (!port.length) return nil;
    NSInteger baud = [d integerForKey:H89TerminalBaudKey];
    NSString* format = [d stringForKey:H89TerminalFormatKey] ?: @"8N1";
    return [NSString stringWithFormat:@"%@:%ld:%@%@", port, (long)(baud > 0 ? baud : 9600), format,
                                      [d boolForKey:H89TerminalRtsCtsKey] ? @":rtscts" : @""];
}

@implementation H89Preferences {
    __weak id<H89PreferencesDelegate> _delegate;
    NSTabView* _tabs;
    // Machine
    NSPopUpButton* _roms;
    NSTextField* _romPath;
    NSPopUpButton* _monitor;
    NSPopUpButton* _memory;
    NSPopUpButton* _bootDevice;
    NSButton* _autoBoot;
    NSPopUpButton* _speed;
    // Terminal
    NSPopUpButton* _mode;
    NSPopUpButton* _port;
    NSComboBox* _baud;
    NSPopUpButton* _format;
    NSButton* _rtsCts;
    NSTextField* _status;
    NSMutableArray<NSButton*>* _switches;
    NSPopUpButton* _lineEnding;
    NSTextField* _charDelay;
    NSTextField* _lineDelay;
    // Display and sound
    NSPopUpButton* _phosphor;
    NSButton* _crt;
    NSPopUpButton* _bell;
    NSButton* _keyClick;
    // Disks
    NSTextField* _disksPath;
    NSTimer* _statusTimer;
}

- (instancetype)initWithDelegate:(id<H89PreferencesDelegate>)delegate {
    NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 560, 440)
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
    return [NSButton checkboxWithTitle:title target:self action:action];
}

- (NSTextField*)note:(NSString*)text {
    NSTextField* label = [NSTextField wrappingLabelWithString:text];
    label.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
    label.textColor = NSColor.secondaryLabelColor;
    label.preferredMaxLayoutWidth = 360;
    return label;
}

- (NSTextField*)heading:(NSString*)text {
    NSTextField* label = [NSTextField labelWithString:text];
    label.font = [NSFont boldSystemFontOfSize:NSFont.systemFontSize];
    return label;
}

- (NSTextField*)numberField {
    NSTextField* field = [NSTextField textFieldWithString:@"0"];
    NSNumberFormatter* f = [[NSNumberFormatter alloc] init];
    f.minimum = @0;
    f.maximum = @10000;
    f.allowsFloats = NO;
    field.formatter = f;
    field.alignment = NSTextAlignmentRight;
    [field.widthAnchor constraintEqualToConstant:52].active = YES;
    field.target = self;
    field.action = @selector(textChanged:);
    return field;
}

- (NSTextField*)label:(NSString*)text {
    return [NSTextField labelWithString:text];
}

// A tab holding a two-column grid of rows; a bold heading spans both columns.
- (void)addTab:(NSString*)title rows:(NSArray<NSArray<NSView*>*>*)rows {
    NSGridView* grid = [NSGridView gridViewWithViews:rows];
    grid.translatesAutoresizingMaskIntoConstraints = NO;
    grid.rowSpacing = 8;
    grid.columnSpacing = 8;
    [grid columnAtIndex:0].xPlacement = NSGridCellPlacementTrailing;
    [grid columnAtIndex:1].width = 360;
    grid.rowAlignment = NSGridRowAlignmentFirstBaseline;
    for (NSInteger r = 0; r < grid.numberOfRows; ++r) {
        NSGridCell* first = [grid cellAtColumnIndex:0 rowIndex:r];
        NSView* v = first.contentView;
        if ([v isKindOfClass:NSTextField.class] &&
            (((NSTextField*)v).font.fontDescriptor.symbolicTraits & NSFontDescriptorTraitBold)) {
            [grid mergeCellsInHorizontalRange:NSMakeRange(0, 2) verticalRange:NSMakeRange(r, 1)];
            first.xPlacement = NSGridCellPlacementLeading;
            [grid rowAtIndex:r].topPadding = r ? 10 : 0;
        }
    }
    NSView* page = [[NSView alloc] initWithFrame:NSZeroRect];
    [page addSubview:grid];
    [NSLayoutConstraint activateConstraints:@[
        [grid.topAnchor constraintEqualToAnchor:page.topAnchor constant:16],
        [grid.leadingAnchor constraintEqualToAnchor:page.leadingAnchor constant:16],
        [grid.trailingAnchor constraintEqualToAnchor:page.trailingAnchor constant:-16],
        [grid.bottomAnchor constraintLessThanOrEqualToAnchor:page.bottomAnchor constant:-16],
    ]];
    NSTabViewItem* item = [[NSTabViewItem alloc] initWithIdentifier:title];
    item.label = title;
    item.view = page;
    [_tabs addTabViewItem:item];
}

- (void)buildContent {
    NSView* empty = NSGridCell.emptyContentView;
    _tabs = [[NSTabView alloc] initWithFrame:NSZeroRect];
    _tabs.translatesAutoresizingMaskIntoConstraints = NO;

    // Machine
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
    _speed = [self popUp:@[ @"Normal (2 MHz)", @"2×", @"4×", @"Maximum" ]
                    tags:@[ @1, @2, @4, @0 ]
                  action:@selector(settingChanged:)];
    [self addTab:@"Machine"
            rows:@[
                @[ [self label:@"ROMs:"], _roms ],
                @[ empty, _romPath ],
                @[ [self label:@"Monitor:"], _monitor ],
                @[ [self label:@"Memory:"], _memory ],
                @[ empty, [self note:@"Changing the ROMs, monitor or memory restarts the H89."] ],
                @[ [self label:@"Boot from:"], _bootDevice ],
                @[ empty, _autoBoot ],
                @[ [self label:@"Speed:"], _speed ],
            ]];

    // Terminal
    _mode = [self popUp:@[ @"H89 computer", @"H19 terminal on a serial port" ]
                   tags:@[ @0, @1 ]
                 action:@selector(modeChanged:)];
    _port = [self popUp:@[] tags:@[] action:@selector(portChanged:)];
    _baud = [[NSComboBox alloc] initWithFrame:NSMakeRect(0, 0, 140, 26)];
    for (size_t i = 0; i < h89::kCommonBaudRateCount; ++i) [_baud addItemWithObjectValue:@(h89::kCommonBaudRates[i])];
    _baud.numberOfVisibleItems = 14;
    _baud.target = self;
    _baud.action = @selector(portChanged:);
    NSArray* formats = @[ @"8N1", @"8N2", @"8E1", @"8O1", @"7E1", @"7O1", @"7N2" ];
    _format = [self popUp:formats tags:@[ @0, @1, @2, @3, @4, @5, @6 ] action:@selector(portChanged:)];
    _rtsCts = [self checkbox:@"RTS/CTS hardware flow control" action:@selector(portChanged:)];
    _status = [self note:@""];
    NSButton* refresh = [NSButton buttonWithTitle:@"Refresh" target:self action:@selector(refreshPorts:)];
    NSStackView* portRow = [NSStackView stackViewWithViews:@[ _port, refresh ]];
    NSMutableArray<NSArray<NSView*>*>* terminalRows = [@[
        @[ [self label:@"Run as:"], _mode ],
        @[ empty, [self note:@"As a terminal, H89 is just the H19, talking to a real computer (an H8, "
                              @"say) through a serial port such as a USB serial adapter."] ],
        @[ [self heading:@"Serial port"], empty ],
        @[ [self label:@"Port:"], portRow ],
        @[ [self label:@"Speed:"], _baud ],
        @[ empty, [self note:@"Any rate the adapter supports; type one in if it isn't listed. The H19's "
                              @"own baud rate switches don't apply."] ],
        @[ [self label:@"Format:"], _format ],
        @[ empty, _rtsCts ],
        @[ [self label:@"Status:"], _status ],
        @[ [self heading:@"H19 switches"], empty ],
    ] mutableCopy];
    _switches = [NSMutableArray array];
    for (const auto& s : kSwitches) {
        NSButton* box = [self checkbox:s.title action:@selector(switchChanged:)];
        [_switches addObject:box];
        [terminalRows addObject:@[ empty, box ]];
    }
    [terminalRows addObject:@[ empty, [self note:@"Switch changes reset the terminal."] ]];
    _lineEnding = [self popUp:@[ @"CR (Return)", @"CR LF", @"LF", @"Unchanged" ]
                         tags:@[ @0, @1, @2, @3 ]
                       action:@selector(textChanged:)];
    _charDelay = [self numberField];
    _lineDelay = [self numberField];
    [terminalRows addObjectsFromArray:@[
        @[ [self heading:@"Sending text files"], empty ],
        @[ [self label:@"Line endings:"], _lineEnding ],
        @[ [self label:@"Pause:"], [NSStackView stackViewWithViews:@[
                                       _charDelay, [self label:@"ms after each character,"], _lineDelay,
                                       [self label:@"ms after each line"] ]] ],
        @[ empty, [self note:@"Pauses give a program without flow control time to keep up, such as BASIC "
                              @"taking a typed-in listing. Text stops at a CP/M end of file (^Z)."] ],
    ]];
    [self addTab:@"Terminal" rows:terminalRows];

    // Display and sound
    _phosphor = [self popUp:@[ @"White (P4)", @"Green (P1)", @"Amber (P3)" ]
                       tags:@[ @0, @1, @2 ]
                     action:@selector(settingChanged:)];
    _crt = [self checkbox:@"CRT effects (glow, persistence, curved screen)" action:@selector(settingChanged:)];
    _bell = [self popUp:@[ @"H19 speaker", @"System alert sound", @"Off" ]
                   tags:@[ @(H89BellH19), @(H89BellSystem), @(H89BellOff) ]
                 action:@selector(settingChanged:)];
    _keyClick = [self checkbox:@"Click when a key is pressed" action:@selector(settingChanged:)];
    [self addTab:@"Display & Sound"
            rows:@[
                @[ [self label:@"Phosphor:"], _phosphor ],
                @[ empty, _crt ],
                @[ [self label:@"Bell:"], _bell ],
                @[ empty, _keyClick ],
            ]];

    // Disks
    _disksPath = [self note:@""];
    _disksPath.lineBreakMode = NSLineBreakByTruncatingMiddle;
    _disksPath.maximumNumberOfLines = 1;
    _disksPath.selectable = YES;
    NSButton* show = [NSButton buttonWithTitle:@"Show in Finder" target:self action:@selector(showDisks:)];
    NSButton* restore = [NSButton buttonWithTitle:@"Restore Originals…" target:self action:@selector(restoreDisks:)];
    [self addTab:@"Disks"
            rows:@[
                @[ [self label:@"Your disks:"], _disksPath ],
                @[ empty, [NSStackView stackViewWithViews:@[ show, restore ]] ],
                @[ empty, [self note:@"The disks that came with H89 are copied here, so changes never "
                                      @"touch the originals. Restore Originals puts them back as they came."] ],
            ]];

    NSView* content = self.window.contentView;
    [content addSubview:_tabs];
    [NSLayoutConstraint activateConstraints:@[
        [_tabs.topAnchor constraintEqualToAnchor:content.topAnchor constant:12],
        [_tabs.leadingAnchor constraintEqualToAnchor:content.leadingAnchor constant:12],
        [_tabs.trailingAnchor constraintEqualToAnchor:content.trailingAnchor constant:-12],
        [_tabs.bottomAnchor constraintEqualToAnchor:content.bottomAnchor constant:-12],
    ]];
}

#pragma mark - Showing

- (void)show {
    [self load];
    [self showWindow:nil];
    [self.window makeKeyAndOrderFront:nil];
    // The connection can change on its own (an adapter plugged in or out).
    if (!_statusTimer) {
        __weak H89Preferences* weakSelf = self;
        _statusTimer = [NSTimer scheduledTimerWithTimeInterval:1.0
                                                       repeats:YES
                                                         block:^(NSTimer*) { [weakSelf updateStatus]; }];
    }
}

- (void)showTerminal {
    [self show];
    [_tabs selectTabViewItemWithIdentifier:@"Terminal"];
}

static void SelectTag(NSPopUpButton* popUp, NSInteger tag) {
    if (![popUp selectItemWithTag:tag]) [popUp selectItemAtIndex:popUp.numberOfItems - 1];
}

- (void)reload {
    if (self.isWindowLoaded && self.window.isVisible) [self load];
}

- (void)updateStatus {
    if (!self.window.isVisible) {
        [_statusTimer invalidate];
        _statusTimer = nil;
        return;
    }
    _status.stringValue = [Defaults() boolForKey:H89TerminalOnlyKey] ? [_delegate terminalStatus]
                                                                     : @"Not in use (running as an H89)";
}

- (void)fillPorts {
    NSString* chosen = [Defaults() stringForKey:H89TerminalPortKey];
    [_port removeAllItems];
    NSArray<NSString*>* ports = H89Emulator.hostSerialPorts;
    for (NSString* p in ports) [_port addItemWithTitle:p];
    if (chosen.length && ![ports containsObject:chosen]) [_port addItemWithTitle:chosen]; // unplugged for now
    if (!_port.numberOfItems) [_port addItemWithTitle:@"No serial ports found"];
    if (chosen.length) [_port selectItemWithTitle:chosen];
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

    [_mode selectItemWithTag:[d boolForKey:H89TerminalOnlyKey] ? 1 : 0];
    [self fillPorts];
    NSInteger baud = [d integerForKey:H89TerminalBaudKey];
    _baud.stringValue = [NSString stringWithFormat:@"%ld", (long)(baud > 0 ? baud : 9600)];
    [_format selectItemWithTitle:[d stringForKey:H89TerminalFormatKey] ?: @"8N1"];
    _rtsCts.state = [d boolForKey:H89TerminalRtsCtsKey] ? NSControlStateValueOn : NSControlStateValueOff;
    for (NSUInteger i = 0; i < _switches.count; ++i)
        _switches[i].state = SwitchOn(kSwitches[i]) ? NSControlStateValueOn : NSControlStateValueOff;
    [_lineEnding selectItemWithTag:[d integerForKey:H89TextLineEndingKey]];
    _charDelay.integerValue = [d integerForKey:H89TextCharDelayKey];
    _lineDelay.integerValue = [d integerForKey:H89TextLineDelayKey];
    [self updateStatus];

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

- (void)modeChanged:(id)sender {
    BOOL terminal = _mode.selectedTag == 1;
    if (terminal == [Defaults() boolForKey:H89TerminalOnlyKey]) return;
    [Defaults() setBool:terminal forKey:H89TerminalOnlyKey];
    [_delegate preferencesChangedMachine];
    [self load];
}

- (void)refreshPorts:(id)sender {
    [self fillPorts];
}

- (void)portChanged:(id)sender {
    NSUserDefaults* d = Defaults();
    NSString* port = _port.titleOfSelectedItem;
    if ([port hasPrefix:@"/"]) [d setObject:port forKey:H89TerminalPortKey];
    NSInteger baud = _baud.integerValue;
    if (baud >= 50) [d setInteger:baud forKey:H89TerminalBaudKey];
    else _baud.stringValue = [NSString stringWithFormat:@"%ld", (long)MAX([d integerForKey:H89TerminalBaudKey], 9600)];
    [d setObject:_format.titleOfSelectedItem forKey:H89TerminalFormatKey];
    [d setBool:_rtsCts.state == NSControlStateValueOn forKey:H89TerminalRtsCtsKey];
    [_delegate preferencesChangedTerminalPort];
    [self updateStatus];
}

- (void)textChanged:(id)sender {
    NSUserDefaults* d = Defaults();
    [d setInteger:_lineEnding.selectedTag forKey:H89TextLineEndingKey];
    [d setInteger:MAX(_charDelay.integerValue, 0) forKey:H89TextCharDelayKey];
    [d setInteger:MAX(_lineDelay.integerValue, 0) forKey:H89TextLineDelayKey];
    [_delegate preferencesChanged];
}

- (void)switchChanged:(id)sender {
    for (NSUInteger i = 0; i < _switches.count; ++i)
        [Defaults() setBool:_switches[i].state == NSControlStateValueOn forKey:kSwitches[i].key];
    [_delegate preferencesChanged];
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
