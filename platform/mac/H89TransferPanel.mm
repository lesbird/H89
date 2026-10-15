#import "H89TransferPanel.h"

#import "H89Emulator.h"

@implementation H89TransferPanel {
    __weak NSWindow* _parent;
    NSTextField* _title;
    NSProgressIndicator* _progress;
    NSTextField* _status;
    NSButton* _button;
}

- (instancetype)initWithParent:(NSWindow*)parent {
    NSPanel* panel = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, 420, 130)
                                                styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskUtilityWindow
                                                  backing:NSBackingStoreBuffered
                                                    defer:YES];
    panel.title = @"File Transfer";
    panel.hidesOnDeactivate = NO;
    panel.becomesKeyOnlyIfNeeded = YES; // typing stays with the terminal
    if ((self = [super initWithWindow:panel])) {
        _parent = parent;
        _title = [NSTextField labelWithString:@""];
        _title.font = [NSFont boldSystemFontOfSize:NSFont.systemFontSize];
        _title.lineBreakMode = NSLineBreakByTruncatingMiddle;
        _progress = [[NSProgressIndicator alloc] init];
        _progress.style = NSProgressIndicatorStyleBar;
        _progress.minValue = 0;
        _progress.maxValue = 1;
        _status = [NSTextField labelWithString:@""];
        _status.textColor = NSColor.secondaryLabelColor;
        _status.lineBreakMode = NSLineBreakByTruncatingTail;
        _button = [NSButton buttonWithTitle:@"Cancel" target:self action:@selector(buttonPressed:)];
        _button.keyEquivalent = @"\e";

        NSStackView* stack = [NSStackView stackViewWithViews:@[ _title, _progress, _status, _button ]];
        stack.orientation = NSUserInterfaceLayoutOrientationVertical;
        stack.alignment = NSLayoutAttributeLeading;
        stack.spacing = 8;
        stack.translatesAutoresizingMaskIntoConstraints = NO;
        NSView* content = panel.contentView;
        [content addSubview:stack];
        [NSLayoutConstraint activateConstraints:@[
            [stack.topAnchor constraintEqualToAnchor:content.topAnchor constant:16],
            [stack.leadingAnchor constraintEqualToAnchor:content.leadingAnchor constant:16],
            [stack.trailingAnchor constraintEqualToAnchor:content.trailingAnchor constant:-16],
            [stack.bottomAnchor constraintLessThanOrEqualToAnchor:content.bottomAnchor constant:-16],
            [_progress.widthAnchor constraintEqualToAnchor:stack.widthAnchor],
            [_title.widthAnchor constraintEqualToAnchor:stack.widthAnchor],
            [_status.widthAnchor constraintEqualToAnchor:stack.widthAnchor],
        ]];
        [stack setCustomSpacing:12 afterView:_status];
    }
    return self;
}

- (void)show {
    NSWindow* parent = _parent;
    if (!self.window.isVisible && parent) {
        NSRect p = parent.frame;
        [self.window setFrameTopLeftPoint:NSMakePoint(NSMidX(p) - 210, NSMaxY(p) - 60)];
    }
    [self update];
    [self.window orderFront:nil];
}

- (void)update {
    H89Emulator* emu = self.emulator;
    if (!self.window.isVisible || !emu.transferExists) return;
    _title.stringValue = emu.transferTitle;
    _status.stringValue = emu.transferStatus;
    double f = emu.transferFraction;
    _progress.indeterminate = f < 0 && emu.transferRunning;
    if (_progress.indeterminate) [_progress startAnimation:nil];
    else [_progress stopAnimation:nil];
    _progress.doubleValue = emu.transferRunning ? MAX(f, 0) : (emu.transferSucceeded ? 1 : _progress.doubleValue);
    _button.title = emu.transferRunning ? @"Cancel" : @"Close";
}

- (void)buttonPressed:(id)sender {
    H89Emulator* emu = self.emulator;
    if (emu.transferRunning) {
        [emu cancelTransfer];
        [self update];
        return;
    }
    [emu clearTransfer];
    [self.window orderOut:nil];
}

@end
