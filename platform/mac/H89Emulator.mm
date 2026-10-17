#import "H89Emulator.h"
#import "H89Emulator+Core.h"

#include "DebugControl.h"
#include "Machine.h"
#include "RomSet.h"
#include "SerialHost.h"
#include "TerminalLink.h"

#include <algorithm>
#include <memory>

@implementation H89Emulator {
    std::unique_ptr<h89::Machine> _machine;
    // Terminal-only mode: an H19 on its own and its host serial port.
    std::unique_ptr<h89::H19> _terminal;
    std::unique_ptr<h89::TerminalLink> _link;
    h89::TextSendOptions _textOptions;
    std::unique_ptr<h89::SerialHost[]> _serial;
    h89::DebugControl _debug;
    NSString* _romSummary;
}

- (nullable instancetype)initWithRomDirectory:(NSString*)directory error:(NSError**)error {
    return [self initWithRomDirectory:directory preferMTR89:NO memoryKB:64 error:error];
}

- (nullable instancetype)initWithRomDirectory:(NSString*)directory
                                  preferMTR89:(BOOL)preferMTR89
                                     memoryKB:(NSInteger)memoryKB
                                        error:(NSError**)error {
    h89::RomScan scan = h89::scanRomDirectory(directory.fileSystemRepresentation, preferMTR89);
    if (!scan.ok()) {
        if (error) {
            *error = [NSError errorWithDomain:@"H89"
                                         code:1
                                     userInfo:@{NSLocalizedDescriptionKey : @(scan.error.c_str())}];
        }
        return nil;
    }
    if ((self = [super init])) {
        scan.config.ramKB = memoryKB == 16 || memoryKB == 32 || memoryKB == 48 ? static_cast<int>(memoryKB) : 64;
        _machine = std::make_unique<h89::Machine>(std::move(scan.config));
        _romSummary = @(scan.summary.c_str());
        _serial.reset(new h89::SerialHost[h89::Machine::kSerialPorts]);
        h89::SerialHost* serial = _serial.get();
        _machine->serialOutput = [serial](int port, uint8_t b) { serial[port].output(b); };
        _machine->serialSource = [serial](int port) { return serial[port].next(); };
        [self hookTerminal];
    }
    return self;
}

- (nullable instancetype)initTerminalWithRomDirectory:(NSString*)directory error:(NSError**)error {
    h89::RomScan scan = h89::scanRomDirectory(directory.fileSystemRepresentation);
    if (!scan.config.hasTerminal()) {
        if (error) {
            NSString* message = [NSString stringWithFormat:@"The H19 ROMs (444-46 and 444-29) are not in %@.", directory];
            *error = [NSError errorWithDomain:@"H89" code:1 userInfo:@{NSLocalizedDescriptionKey : message}];
        }
        return nil;
    }
    if ((self = [super init])) {
        _terminal = std::make_unique<h89::H19>(scan.config.terminal);
        _link = std::make_unique<h89::TerminalLink>(*_terminal);
        _romSummary = [NSString stringWithFormat:@"H19 firmware and font from %@", directory.lastPathComponent];
        [self hookTerminal];
    }
    return self;
}

// The H19 in use: the H89's own, or the one on its own.
- (h89::H19*)h19 {
    return _terminal ? _terminal.get() : _machine->terminal();
}

- (void)hookTerminal {
    __weak H89Emulator* weakSelf = self;
    self.h19->onBell = [weakSelf] {
        void (^handler)(void) = weakSelf.bellHandler;
        if (handler) handler();
    };
    self.h19->onKeyClick = [weakSelf] {
        void (^handler)(void) = weakSelf.keyClickHandler;
        if (handler) handler();
    };
}

- (BOOL)terminalOnly {
    return _terminal != nullptr;
}

- (BOOL)connectTerminal:(NSString*)spec error:(NSError**)error {
    if (!_link) return NO;
    std::string message;
    h89::SerialSettings settings;
    if (h89::SerialSettings::parse(spec.UTF8String, settings, &message) && _link->open(settings, &message)) return YES;
    if (error) {
        *error = [NSError errorWithDomain:@"H89"
                                     code:5
                                 userInfo:@{NSLocalizedDescriptionKey : @(message.c_str())}];
    }
    return NO;
}

- (void)disconnectTerminal {
    if (_link) _link->close();
}

- (NSString*)terminalStatus {
    return _link ? @(_link->description().c_str()) : @"";
}

+ (NSArray<NSString*>*)hostSerialPorts {
    NSMutableArray<NSString*>* ports = [NSMutableArray array];
    for (const auto& p : h89::SerialPort::list()) [ports addObject:@(p.c_str())];
    return ports;
}

- (void)setTextLineEnding:(NSInteger)ending charDelay:(NSInteger)charMs lineDelay:(NSInteger)lineMs {
    using LE = h89::TextSendOptions::LineEnding;
    _textOptions.lineEnding = ending == 1 ? LE::CRLF : ending == 2 ? LE::LF : ending == 3 ? LE::AsIs : LE::CR;
    _textOptions.charDelayMs = static_cast<int>(std::clamp<NSInteger>(charMs, 0, 5000));
    _textOptions.lineDelayMs = static_cast<int>(std::clamp<NSInteger>(lineMs, 0, 10000));
}

static NSError* TransferError(const std::string& message) {
    return [NSError errorWithDomain:@"H89" code:6 userInfo:@{NSLocalizedDescriptionKey : @(message.c_str())}];
}

- (BOOL)startTransfer:(std::unique_ptr<h89::FileTransfer>)transfer
              message:(const std::string&)message
                error:(NSError**)error {
    std::string why = message;
    if (!_link) why = "File transfers need the terminal on a serial port.";
    else if (_link->transferRunning()) why = "A transfer is already running.";
    else if (transfer && _link->startTransfer(std::move(transfer))) return YES;
    if (error) *error = TransferError(why);
    return NO;
}

- (BOOL)startUpload:(H89Upload)kind paths:(NSArray<NSString*>*)paths error:(NSError**)error {
    std::vector<std::string> files;
    for (NSString* p in paths) files.emplace_back(p.fileSystemRepresentation);
    if (files.empty()) return NO;
    std::string message;
    std::unique_ptr<h89::FileTransfer> t;
    switch (kind) {
    case H89UploadText: t = h89::sendTextFile(files[0], _textOptions, &message); break;
    case H89UploadXmodem: t = h89::xmodemSend(files[0], false, &message); break;
    case H89UploadXmodem1K: t = h89::xmodemSend(files[0], true, &message); break;
    case H89UploadZmodem: t = h89::zmodemSend(files, &message); break;
    }
    return [self startTransfer:std::move(t) message:message error:error];
}

- (BOOL)startXmodemDownloadTo:(NSString*)path error:(NSError**)error {
    std::string message;
    auto t = h89::xmodemReceive(path.fileSystemRepresentation, &message);
    return [self startTransfer:std::move(t) message:message error:error];
}

- (BOOL)startZmodemDownload:(NSError**)error {
    return [self startTransfer:h89::zmodemReceive() message:"" error:error];
}

- (void)cancelTransfer {
    if (_link) _link->cancelTransfer();
}

- (void)clearTransfer {
    if (_link) _link->clearTransfer();
}

- (h89::FileTransfer*)transfer {
    return _link ? _link->transfer() : nullptr;
}

- (BOOL)transferExists {
    return self.transfer != nullptr;
}

- (BOOL)transferRunning {
    return _link && _link->transferRunning();
}

- (BOOL)transferSucceeded {
    return self.transfer && self.transfer->state() == h89::FileTransfer::State::Done;
}

- (NSString*)transferTitle {
    return self.transfer ? @(self.transfer->title().c_str()) : @"";
}

- (NSString*)transferStatus {
    return self.transfer ? @(self.transfer->status().c_str()) : @"";
}

- (double)transferFraction {
    h89::FileTransfer* t = self.transfer;
    if (!t || !t->bytesTotal()) return -1;
    return std::min(1.0, static_cast<double>(t->bytesDone()) / static_cast<double>(t->bytesTotal()));
}

- (nullable NSString*)transferOfferedName {
    h89::FileTransfer* t = self.transfer;
    return t && t->state() == h89::FileTransfer::State::NeedsSavePath ? @(t->offeredName().c_str()) : nil;
}

- (void)setTransferSavePath:(nullable NSString*)path {
    if (h89::FileTransfer* t = self.transfer) t->setSavePath(path ? path.fileSystemRepresentation : "");
}

- (void)setTerminalSW401:(NSInteger)sw401 SW402:(NSInteger)sw402 {
    h89::H19* h19 = self.h19;
    if (h19->config().sw401 == sw401 && h19->config().sw402 == sw402) return;
    h19->setSwitches(static_cast<uint8_t>(sw401), static_cast<uint8_t>(sw402));
    h19->reset();
}

- (NSString*)romSummary { return _romSummary; }
- (NSInteger)screenWidth { return h89::kScreenWidth; }
- (NSInteger)screenHeight { return h89::kScreenHeight; }
- (uint64_t)frameCount { return _terminal ? _terminal->frameCount() : _machine->frameCount(); }

- (void)reset {
    if (_terminal) _terminal->reset();
    else _machine->reset();
}

- (void)runFrame {
    if (_link) {
        _link->runFrame();
        return;
    }
    for (int p = 0; p < h89::Machine::kSerialPorts; ++p) _serial[p].pump(*_machine, p);
    _debug.runFrame(*_machine);
}

- (BOOL)debuggerPaused {
    return !_terminal && _debug.paused();
}

- (h89::Machine&)machine {
    return *_machine;
}

- (h89::DebugControl&)debugControl {
    return _debug;
}

- (BOOL)connectSerialPort:(H89SerialPort)port spec:(NSString*)spec error:(NSError**)error {
    if (_terminal) return NO;
    std::string message;
    if (_serial[port].open(spec.UTF8String, &message)) {
        _serial[port].pump(*_machine, static_cast<int>(port));
        return YES;
    }
    if (error) {
        *error = [NSError errorWithDomain:@"H89"
                                     code:4
                                 userInfo:@{NSLocalizedDescriptionKey : @(message.c_str())}];
    }
    return NO;
}

- (void)disconnectSerialPort:(H89SerialPort)port {
    if (_terminal) return;
    _serial[port].close();
    _serial[port].pump(*_machine, static_cast<int>(port));
}

- (nullable NSString*)serialPortSpec:(H89SerialPort)port {
    if (_terminal) return nil;
    return _serial[port].active() ? @(_serial[port].spec().c_str()) : nil;
}

- (NSString*)serialPortStatus:(H89SerialPort)port {
    if (_terminal) return @"not available";
    return @(_serial[port].description().c_str());
}

- (NSInteger)scrollbackOffset {
    return self.h19->viewOffset();
}

- (void)setScrollbackOffset:(NSInteger)lines {
    self.h19->setViewOffset(static_cast<int>(lines));
}

- (const uint8_t*)framebuffer {
    return self.h19->framebuffer().pixels.data();
}

static constexpr NSInteger kH17Drives = H89FirstH37Drive;
static_assert(kH17Drives == h89::H17::kDrives);
static constexpr NSInteger kH37Drives = h89::H37::kDrives;

- (NSInteger)driveCount {
    return _terminal ? 0 : kH17Drives + kH37Drives;
}

- (BOOL)driveIsH37:(NSInteger)drive {
    return drive >= kH17Drives;
}

- (BOOL)driveIsAvailable:(NSInteger)drive {
    if (_terminal || drive < 0 || drive >= self.driveCount) return NO;
    return [self driveIsH37:drive] ? _machine->h37() != nullptr : _machine->h17() != nullptr;
}

- (NSString*)driveName:(NSInteger)drive {
    return [self driveIsH37:drive] ? [NSString stringWithFormat:@"DK%ld:", (long)(drive - kH17Drives)]
                                   : [NSString stringWithFormat:@"SY%ld:", (long)drive];
}

- (BOOL)bootFromH37 {
    if (_terminal) return NO;
    return (_machine->config().sw501 & h89::sw501::kBootFrom170) != 0;
}

- (void)setBootFromH37:(BOOL)h37 {
    if (_terminal) return;
    uint8_t sw = _machine->config().sw501 & ~h89::sw501::kBootFrom170;
    _machine->setSw501(h37 ? (sw | h89::sw501::kBootFrom170) : sw);
}

- (BOOL)autoBoot {
    if (_terminal) return NO;
    return (_machine->config().sw501 & h89::sw501::kAutoBoot) != 0;
}

- (void)setAutoBoot:(BOOL)autoBoot {
    if (_terminal) return;
    uint8_t sw = _machine->config().sw501 & ~h89::sw501::kAutoBoot;
    _machine->setSw501(autoBoot ? (sw | h89::sw501::kAutoBoot) : sw);
}

- (BOOL)insertDiskAtPath:(NSString*)path
                   drive:(NSInteger)drive
                readOnly:(BOOL)readOnly
                   error:(NSError**)error {
    std::string message = [self driveIsH37:drive] ? "This machine has no H37 controller."
                                                    : "This machine has no H17 controller (444-19 ROM missing).";
    if ([self driveIsAvailable:drive]) {
        bool ok = [self driveIsH37:drive]
                      ? _machine->h37()->insertDisk(static_cast<int>(drive - kH17Drives), path.fileSystemRepresentation,
                                                    readOnly, &message)
                      : _machine->h17()->insertDisk(static_cast<int>(drive), path.fileSystemRepresentation, readOnly,
                                                    &message);
        if (ok) return YES;
    }
    if (error) {
        *error = [NSError errorWithDomain:@"H89"
                                     code:2
                                 userInfo:@{NSLocalizedDescriptionKey : @(message.c_str())}];
    }
    return NO;
}

- (void)ejectDrive:(NSInteger)drive {
    if (![self driveIsAvailable:drive]) return;
    if ([self driveIsH37:drive]) _machine->h37()->ejectDisk(static_cast<int>(drive - kH17Drives));
    else _machine->h17()->ejectDisk(static_cast<int>(drive));
}

- (nullable NSString*)diskPathForDrive:(NSInteger)drive {
    if (h89::H17Disk* disk = [self diskForDrive:drive]) return disk->loaded() ? @(disk->path().c_str()) : nil;
    if (h89::SoftDisk* disk = [self softDiskForDrive:drive]) return disk->loaded() ? @(disk->path().c_str()) : nil;
    return nil;
}

- (nullable h89::H17Disk*)diskForDrive:(NSInteger)drive {
    if ([self driveIsH37:drive] || ![self driveIsAvailable:drive]) return nullptr;
    return &_machine->h17()->disk(static_cast<int>(drive));
}

- (nullable h89::SoftDisk*)softDiskForDrive:(NSInteger)drive {
    if (![self driveIsH37:drive] || ![self driveIsAvailable:drive]) return nullptr;
    return &_machine->h37()->disk(static_cast<int>(drive - kH17Drives));
}

- (BOOL)diskIsReadOnlyInDrive:(NSInteger)drive {
    if (h89::H17Disk* disk = [self diskForDrive:drive]) return disk->readOnly();
    if (h89::SoftDisk* disk = [self softDiskForDrive:drive]) return disk->readOnly();
    return NO;
}

- (NSData*)saveState {
    if (_terminal) return [NSData data];
    std::vector<uint8_t> state = _machine->saveState();
    return [NSData dataWithBytes:state.data() length:state.size()];
}

- (BOOL)loadState:(NSData*)data
         warnings:(NSArray<NSString*>* _Nullable* _Nullable)warnings
            error:(NSError**)error {
    if (_terminal) {
        if (error) {
            *error = [NSError errorWithDomain:@"H89"
                                         code:3
                                     userInfo:@{NSLocalizedDescriptionKey : @"States can't be loaded in terminal mode."}];
        }
        return NO;
    }
    const uint8_t* p = static_cast<const uint8_t*>(data.bytes);
    std::string message;
    std::vector<std::string> notes;
    if (!_machine->loadState(std::vector<uint8_t>(p, p + data.length), &message, &notes)) {
        if (error) {
            *error = [NSError errorWithDomain:@"H89"
                                         code:3
                                     userInfo:@{NSLocalizedDescriptionKey : @(message.c_str())}];
        }
        return NO;
    }
    if (warnings) {
        NSMutableArray<NSString*>* list = [NSMutableArray array];
        for (const auto& n : notes) [list addObject:@(n.c_str())];
        *warnings = list;
    }
    return YES;
}

- (void)typeCharacter:(unichar)character {
    if (character < 0x80) self.h19->typeChar(static_cast<uint8_t>(character));
}

- (void)pressKey:(H89Key)key shift:(BOOL)shift {
    self.h19->pressKey(static_cast<h89::H19Key>(key), shift);
}

- (void)setBreakKeyDown:(BOOL)down {
    self.h19->setBreak(down);
}

@end
