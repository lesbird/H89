#import "H89DiskBrowser.h"

#import "H89Emulator+Core.h"

#include "media/DiskFiles.h"
#include "media/H17Disk.h"
#include "media/SoftDisk.h"

#include <memory>
#include <vector>

static NSString* const kNameColumn = @"name";
static NSString* const kSizeColumn = @"size";
static NSString* const kDateColumn = @"date";
static NSString* const kFlagsColumn = @"flags";
static NSString* const kUserColumn = @"user";

static NSString* CanonicalPath(NSString* path) {
    return path.stringByResolvingSymlinksInPath.stringByStandardizingPath;
}

@interface H89DiskBrowser () <NSWindowDelegate>
@end

@implementation H89DiskBrowser {
    NSString* _path;
    // The image, read fresh, when no drive holds it: .h8d files are H17
    // disks, anything else soft-sectored (H37).
    h89::H17Disk _ownH17;
    h89::SoftDisk _ownSoft;
    bool _readOnly;
    NSInteger _drive; // drive holding the image, or -1
    std::unique_ptr<h89::DiskFileSystem> _fs;
    std::vector<h89::DiskFile> _files;

    NSTextField* _infoLabel;
    NSTextField* _hintLabel;
    NSTableView* _table;
    NSButton* _insertButton;
    NSButton* _deleteButton;
    NSButton* _extractButton;
    NSButton* _extractAllButton;
}

- (instancetype)initWithPath:(NSString*)path emulator:(nullable H89Emulator*)emulator {
    NSWindow* window = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 600, 440)
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                            NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                    backing:NSBackingStoreBuffered
                      defer:YES];
    window.releasedWhenClosed = NO;
    window.title = [NSString stringWithFormat:@"Files on %@", path.lastPathComponent];
    window.contentMinSize = NSMakeSize(460, 260);
    if ((self = [super initWithWindow:window])) {
        _path = [path copy];
        _emulator = emulator;
        _drive = -1;
        window.delegate = self;
        [self buildContent];
        [window center];
        [self refresh];
    }
    return self;
}

- (NSString*)path { return _path; }

#pragma mark - Layout

- (void)buildContent {
    NSView* content = self.window.contentView;
    const CGFloat margin = 20, width = content.bounds.size.width, height = content.bounds.size.height;

    _infoLabel = [NSTextField wrappingLabelWithString:@""];
    _infoLabel.frame = NSMakeRect(margin, height - 14 - 36, width - 2 * margin, 36);
    _infoLabel.autoresizingMask = NSViewWidthSizable | NSViewMinYMargin;
    [content addSubview:_infoLabel];

    _table = [[NSTableView alloc] init];
    _table.allowsMultipleSelection = YES;
    _table.usesAlternatingRowBackgroundColors = YES;
    _table.dataSource = self;
    _table.delegate = self;
    _table.target = self;
    _table.doubleAction = @selector(extract:);
    [_table registerForDraggedTypes:@[ NSPasteboardTypeFileURL ]];
    struct {
        NSString* identifier;
        NSString* title;
        CGFloat width;
    } columns[] = {
        {kUserColumn, @"User", 40},  {kNameColumn, @"Name", 170}, {kSizeColumn, @"Size", 80},
        {kDateColumn, @"Date", 100}, {kFlagsColumn, @"Flags", 60},
    };
    for (const auto& c : columns) {
        NSTableColumn* column = [[NSTableColumn alloc] initWithIdentifier:c.identifier];
        column.title = c.title;
        column.width = c.width;
        if (c.identifier == kSizeColumn) column.headerCell.alignment = NSTextAlignmentRight;
        [_table addTableColumn:column];
    }

    NSScrollView* scroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(margin, 86, width - 2 * margin,
                                                                          height - 14 - 36 - 10 - 86)];
    scroll.documentView = _table;
    scroll.hasVerticalScroller = YES;
    scroll.borderType = NSBezelBorder;
    scroll.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [content addSubview:scroll];

    _hintLabel = [NSTextField wrappingLabelWithString:@""];
    _hintLabel.frame = NSMakeRect(margin, 48, width - 2 * margin, 32);
    _hintLabel.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
    _hintLabel.textColor = NSColor.secondaryLabelColor;
    _hintLabel.autoresizingMask = NSViewWidthSizable | NSViewMaxYMargin;
    [content addSubview:_hintLabel];

    auto button = [&](NSString* title, SEL action) {
        NSButton* b = [NSButton buttonWithTitle:title target:self action:action];
        [b sizeToFit];
        b.frame = NSMakeRect(0, 12, MAX(b.frame.size.width, 96), b.frame.size.height);
        [content addSubview:b];
        return b;
    };
    _insertButton = button(@"Insert Files…", @selector(insert:));
    _deleteButton = button(@"Delete", @selector(deleteFiles:));
    _extractAllButton = button(@"Extract All", @selector(extractAll:));
    _extractButton = button(@"Extract", @selector(extract:));
    _insertButton.toolTip = @"Copy files from your Mac onto the disk. You can also drop files on the list.";
    _extractButton.toolTip = @"Copy the selected files to a folder on your Desktop.";
    _extractAllButton.toolTip = @"Copy every file to a folder on your Desktop.";

    CGFloat x = margin - 6; // buttons have a built-in inset
    for (NSButton* b in @[ _insertButton, _deleteButton ]) {
        [b setFrameOrigin:NSMakePoint(x, 12)];
        b.autoresizingMask = NSViewMaxXMargin | NSViewMaxYMargin;
        x += b.frame.size.width;
    }
    x = width - margin + 6;
    for (NSButton* b in @[ _extractButton, _extractAllButton ]) {
        x -= b.frame.size.width;
        [b setFrameOrigin:NSMakePoint(x, 12)];
        b.autoresizingMask = NSViewMinXMargin | NSViewMaxYMargin;
    }
    _extractButton.keyEquivalent = @"\r";
}

#pragma mark - State

// Finds the image: through the drive that holds it, else read from the file.
- (BOOL)openDisk:(NSString**)error {
    _fs.reset();
    _drive = -1;
    NSString* canonical = CanonicalPath(_path);
    std::string message;
    for (NSInteger d = 0; d < _emulator.driveCount; ++d) {
        NSString* mounted = [_emulator diskPathForDrive:d];
        if (!mounted || ![CanonicalPath(mounted) isEqualToString:canonical]) continue;
        _drive = d;
        _readOnly = [_emulator diskIsReadOnlyInDrive:d];
        if (h89::SoftDisk* soft = [_emulator softDiskForDrive:d]) _fs = h89::DiskFileSystem::open(*soft, &message);
        else _fs = h89::DiskFileSystem::open(*[_emulator diskForDrive:d], &message);
        break;
    }
    if (_drive < 0) {
        _readOnly = ![NSFileManager.defaultManager isWritableFileAtPath:_path];
        bool loaded;
        if ([_path.pathExtension caseInsensitiveCompare:@"h8d"] == NSOrderedSame) {
            loaded = _ownH17.load(_path.fileSystemRepresentation, _readOnly, &message);
            if (loaded) _fs = h89::DiskFileSystem::open(_ownH17, &message);
        } else {
            loaded = _ownSoft.load(_path.fileSystemRepresentation, _readOnly, &message);
            if (loaded) _fs = h89::DiskFileSystem::open(_ownSoft, &message);
        }
    }
    if (!_fs) *error = @(message.c_str());
    return _fs != nullptr;
}

- (BOOL)canWrite {
    return _fs && !_readOnly;
}

- (void)refresh {
    NSString* error = nil;
    _files.clear();
    if ([self openDisk:&error]) _files = _fs->list();
    BOOL cpm = _fs && _fs->format() == h89::DiskFileSystem::Format::Cpm;

    NSMutableString* info = [NSMutableString stringWithString:_path.lastPathComponent];
    if (_drive >= 0) [info appendFormat:@" (in %@)", [_emulator driveName:_drive]];
    if (_fs) {
        [info appendFormat:@" — %s\n%zu files, ", _fs->description().c_str(), _files.size()];
        uint32_t freeBytes = _fs->freeBytes();
        if (cpm) [info appendFormat:@"%uK free", freeBytes / 1024];
        else [info appendFormat:@"%u sectors free", freeBytes / h89::H17Disk::kSectorSize];
        if (_readOnly) [info appendString:@" — write protected"];
    } else {
        [info appendFormat:@"\n%@", error];
    }
    _infoLabel.stringValue = info;

    NSString* hint = @"";
    if (_fs && _drive >= 0 && !_readOnly) {
        hint = cpm ? @"This disk is in a drive. After inserting or deleting files, warm boot CP/M (Control-C at the "
                     @"A> prompt) so it sees the change."
                   : @"This disk is in a drive. After inserting or deleting files, DISMOUNT and MOUNT it in HDOS "
                     @"(or reboot) so HDOS sees the change.";
    }
    _hintLabel.stringValue = hint;

    [_table tableColumnWithIdentifier:kUserColumn].hidden = !cpm;
    [_table tableColumnWithIdentifier:kDateColumn].hidden = cpm;
    [_table reloadData];
    [self updateButtons];
}

- (void)updateButtons {
    BOOL selected = _table.selectedRowIndexes.count > 0;
    _insertButton.enabled = self.canWrite;
    _deleteButton.enabled = self.canWrite && selected;
    _extractButton.enabled = _fs && selected;
    _extractAllButton.enabled = _fs && !_files.empty();
}

- (void)windowDidBecomeKey:(NSNotification*)notification {
    [self refresh];
}

- (std::vector<h89::DiskFile>)selectedFiles {
    std::vector<h89::DiskFile> files;
    NSIndexSet* rows = _table.selectedRowIndexes;
    for (NSUInteger row = rows.firstIndex; row != NSNotFound; row = [rows indexGreaterThanIndex:row])
        if (row < _files.size()) files.push_back(_files[row]);
    return files;
}

#pragma mark - Actions

- (void)extract:(id)sender {
    if (_table.selectedRowIndexes.count == 0) return; // double-click on empty space
    [self extractFiles:self.selectedFiles];
}

- (void)extractAll:(id)sender {
    [self extractFiles:_files];
}

// Copies files to ~/Desktop/<image name>/ (CP/M user areas other than 0 go in
// "User n" subfolders) and shows them in the Finder.
- (void)extractFiles:(std::vector<h89::DiskFile>)files {
    [self refresh];
    if (!_fs || files.empty()) return;
    NSFileManager* fm = NSFileManager.defaultManager;
    NSError* error = nil;
    NSURL* desktop = [fm URLForDirectory:NSDesktopDirectory
                                inDomain:NSUserDomainMask
                       appropriateForURL:nil
                                  create:YES
                                   error:&error];
    NSURL* folder = [desktop URLByAppendingPathComponent:_path.lastPathComponent.stringByDeletingPathExtension
                                             isDirectory:YES];
    NSMutableArray<NSURL*>* written = [NSMutableArray array];
    NSMutableArray<NSString*>* problems = [NSMutableArray array];
    for (const h89::DiskFile& f : files) {
        std::vector<uint8_t> bytes;
        std::string message;
        if (!_fs->read(f, &bytes, &message)) {
            [problems addObject:@(message.c_str())];
            continue;
        }
        NSURL* dir = f.user ? [folder URLByAppendingPathComponent:[NSString stringWithFormat:@"User %d", f.user]
                                                       isDirectory:YES]
                            : folder;
        NSString* name = [@(f.name.c_str()) stringByReplacingOccurrencesOfString:@"/" withString:@"_"];
        NSURL* url = [dir URLByAppendingPathComponent:name];
        NSData* data = [NSData dataWithBytes:bytes.data() length:bytes.size()];
        if (![fm createDirectoryAtURL:dir withIntermediateDirectories:YES attributes:nil error:&error] ||
            ![data writeToURL:url options:NSDataWritingAtomic error:&error]) {
            [problems addObject:error.localizedDescription];
            continue;
        }
        [written addObject:url];
    }
    if (written.count) [NSWorkspace.sharedWorkspace activateFileViewerSelectingURLs:written];
    [self reportProblems:problems title:@"Some files couldn't be extracted."];
}

- (void)insert:(id)sender {
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.canChooseFiles = YES;
    panel.canChooseDirectories = NO;
    panel.allowsMultipleSelection = YES;
    panel.prompt = @"Insert";
    panel.message = [NSString stringWithFormat:@"Choose files to copy onto %@", _path.lastPathComponent];
    [panel beginSheetModalForWindow:self.window
                  completionHandler:^(NSModalResponse response) {
                      if (response == NSModalResponseOK) [self insertURLs:panel.URLs];
                  }];
}

- (void)insertURLs:(NSArray<NSURL*>*)urls {
    [self refresh];
    if (!self.canWrite) return;
    NSMutableArray<NSString*>* problems = [NSMutableArray array];
    NSInteger remembered = -1; // the "do this for the rest" choice: 1 replace, 0 skip
    for (NSUInteger i = 0; i < urls.count; ++i) {
        NSURL* url = urls[i];
        NSNumber* isDirectory = nil;
        [url getResourceValue:&isDirectory forKey:NSURLIsDirectoryKey error:nil];
        if (isDirectory.boolValue) {
            [problems addObject:[NSString stringWithFormat:@"%@ is a folder.", url.lastPathComponent]];
            continue;
        }
        NSError* error = nil;
        NSData* data = [NSData dataWithContentsOfURL:url options:0 error:&error];
        if (!data) {
            [problems addObject:error.localizedDescription];
            continue;
        }
        std::string message;
        std::string name = _fs->diskName(url.lastPathComponent.UTF8String, &message);
        if (name.empty()) {
            [problems addObject:@(message.c_str())];
            continue;
        }
        bool replace = false;
        if (_fs->find(name)) {
            NSInteger choice = remembered;
            if (choice < 0) {
                NSAlert* alert = [[NSAlert alloc] init];
                alert.messageText = [NSString stringWithFormat:@"%s already exists on this disk.", name.c_str()];
                alert.informativeText = [NSString stringWithFormat:@"Replace it with %@?", url.lastPathComponent];
                [alert addButtonWithTitle:@"Replace"];
                [alert addButtonWithTitle:@"Skip"];
                [alert addButtonWithTitle:@"Stop"];
                if (i + 1 < urls.count) {
                    alert.showsSuppressionButton = YES;
                    alert.suppressionButton.title = @"Do the same for the rest";
                }
                NSModalResponse response = [alert runModal];
                if (response == NSAlertThirdButtonReturn) break;
                choice = response == NSAlertFirstButtonReturn;
                if (alert.suppressionButton.state == NSControlStateValueOn) remembered = choice;
            }
            if (!choice) continue;
            replace = true;
        }
        const uint8_t* p = static_cast<const uint8_t*>(data.bytes);
        if (!_fs->write(name, std::vector<uint8_t>(p, p + data.length), replace, &message))
            [problems addObject:@(message.c_str())];
    }
    [self refresh];
    [self reportProblems:problems title:@"Some files couldn't be inserted."];
}

- (void)deleteFiles:(id)sender {
    std::vector<h89::DiskFile> files = self.selectedFiles;
    if (files.empty()) return;
    NSAlert* alert = [[NSAlert alloc] init];
    alert.messageText = files.size() == 1
                            ? [NSString stringWithFormat:@"Delete %s from this disk?", files[0].name.c_str()]
                            : [NSString stringWithFormat:@"Delete %zu files from this disk?", files.size()];
    alert.informativeText = @"This can't be undone.";
    [alert addButtonWithTitle:@"Delete"].hasDestructiveAction = YES;
    [alert addButtonWithTitle:@"Cancel"];
    [alert beginSheetModalForWindow:self.window
                  completionHandler:^(NSModalResponse response) {
                      if (response != NSAlertFirstButtonReturn) return;
                      [self refresh];
                      if (!self.canWrite) return;
                      NSMutableArray<NSString*>* problems = [NSMutableArray array];
                      for (const h89::DiskFile& f : files) {
                          std::string message;
                          if (!self->_fs->remove(f, &message)) [problems addObject:@(message.c_str())];
                      }
                      [self refresh];
                      [self reportProblems:problems title:@"Some files couldn't be deleted."];
                  }];
}

- (void)reportProblems:(NSArray<NSString*>*)problems title:(NSString*)title {
    if (problems.count == 0) return;
    NSAlert* alert = [[NSAlert alloc] init];
    alert.messageText = problems.count == 1 ? problems[0] : title;
    if (problems.count > 1) alert.informativeText = [problems componentsJoinedByString:@"\n"];
    [alert beginSheetModalForWindow:self.window completionHandler:nil];
}

#pragma mark - Table

- (NSInteger)numberOfRowsInTableView:(NSTableView*)tableView {
    return static_cast<NSInteger>(_files.size());
}

- (NSView*)tableView:(NSTableView*)tableView viewForTableColumn:(NSTableColumn*)column row:(NSInteger)row {
    NSTextField* cell = [tableView makeViewWithIdentifier:column.identifier owner:self];
    if (!cell) {
        cell = [NSTextField labelWithString:@""];
        cell.identifier = column.identifier;
        cell.lineBreakMode = NSLineBreakByTruncatingTail;
        if ([column.identifier isEqualToString:kSizeColumn] || [column.identifier isEqualToString:kUserColumn]) {
            cell.alignment = NSTextAlignmentRight;
            cell.font = [NSFont monospacedDigitSystemFontOfSize:NSFont.systemFontSize weight:NSFontWeightRegular];
        }
    }
    const h89::DiskFile& f = _files[static_cast<size_t>(row)];
    NSString* identifier = column.identifier;
    if ([identifier isEqualToString:kNameColumn]) cell.stringValue = @(f.name.c_str());
    else if ([identifier isEqualToString:kSizeColumn])
        cell.stringValue = [NSNumberFormatter localizedStringFromNumber:@(f.size)
                                                            numberStyle:NSNumberFormatterDecimalStyle];
    else if ([identifier isEqualToString:kDateColumn]) cell.stringValue = @(f.date.c_str());
    else if ([identifier isEqualToString:kFlagsColumn]) cell.stringValue = @(f.flags.c_str());
    else cell.stringValue = [NSString stringWithFormat:@"%d", f.user];
    return cell;
}

- (void)tableViewSelectionDidChange:(NSNotification*)notification {
    [self updateButtons];
}

- (NSDragOperation)tableView:(NSTableView*)tableView
                validateDrop:(id<NSDraggingInfo>)info
                 proposedRow:(NSInteger)row
       proposedDropOperation:(NSTableViewDropOperation)operation {
    if (!self.canWrite) return NSDragOperationNone;
    [tableView setDropRow:-1 dropOperation:NSTableViewDropOn]; // the whole list
    return NSDragOperationCopy;
}

- (BOOL)tableView:(NSTableView*)tableView
       acceptDrop:(id<NSDraggingInfo>)info
              row:(NSInteger)row
    dropOperation:(NSTableViewDropOperation)operation {
    NSArray<NSURL*>* urls = [info.draggingPasteboard readObjectsForClasses:@[ NSURL.class ]
                                                                   options:@{NSPasteboardURLReadingFileURLsOnlyKey : @YES}];
    if (urls.count == 0) return NO;
    // Let the drag finish before any replace prompts.
    dispatch_async(dispatch_get_main_queue(), ^{ [self insertURLs:urls]; });
    return YES;
}

@end
