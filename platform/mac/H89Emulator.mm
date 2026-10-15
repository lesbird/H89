#import "H89Emulator.h"
#import "H89Emulator+Core.h"

#include "Machine.h"
#include "RomSet.h"
#include "SerialHost.h"

#include <memory>

@implementation H89Emulator {
    std::unique_ptr<h89::Machine> _machine;
    std::unique_ptr<h89::SerialHost[]> _serial;
    NSString* _romSummary;
}

- (nullable instancetype)initWithRomDirectory:(NSString*)directory error:(NSError**)error {
    h89::RomScan scan = h89::scanRomDirectory(directory.fileSystemRepresentation);
    if (!scan.ok()) {
        if (error) {
            *error = [NSError errorWithDomain:@"H89"
                                         code:1
                                     userInfo:@{NSLocalizedDescriptionKey : @(scan.error.c_str())}];
        }
        return nil;
    }
    if ((self = [super init])) {
        _machine = std::make_unique<h89::Machine>(std::move(scan.config));
        _romSummary = @(scan.summary.c_str());
        _serial.reset(new h89::SerialHost[h89::Machine::kSerialPorts]);
        h89::SerialHost* serial = _serial.get();
        _machine->serialOutput = [serial](int port, uint8_t b) { serial[port].output(b); };
        _machine->serialSource = [serial](int port) { return serial[port].next(); };
        __weak H89Emulator* weakSelf = self;
        _machine->terminal()->onBell = [weakSelf] {
            void (^handler)(void) = weakSelf.bellHandler;
            if (handler) handler();
        };
    }
    return self;
}

- (NSString*)romSummary { return _romSummary; }
- (NSInteger)screenWidth { return h89::kScreenWidth; }
- (NSInteger)screenHeight { return h89::kScreenHeight; }
- (uint64_t)frameCount { return _machine->frameCount(); }

- (void)reset { _machine->reset(); }
- (void)runFrame {
    for (int p = 0; p < h89::Machine::kSerialPorts; ++p) _serial[p].pump(*_machine, p);
    _machine->runFrame();
}

- (BOOL)connectSerialPort:(H89SerialPort)port spec:(NSString*)spec error:(NSError**)error {
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
    _serial[port].close();
    _serial[port].pump(*_machine, static_cast<int>(port));
}

- (nullable NSString*)serialPortSpec:(H89SerialPort)port {
    return _serial[port].active() ? @(_serial[port].spec().c_str()) : nil;
}

- (NSString*)serialPortStatus:(H89SerialPort)port {
    return @(_serial[port].description().c_str());
}

- (const uint8_t*)framebuffer {
    return _machine->framebuffer().pixels.data();
}

static constexpr NSInteger kH17Drives = H89FirstH37Drive;
static_assert(kH17Drives == h89::H17::kDrives);
static constexpr NSInteger kH37Drives = h89::H37::kDrives;

- (NSInteger)driveCount {
    return kH17Drives + kH37Drives;
}

- (BOOL)driveIsH37:(NSInteger)drive {
    return drive >= kH17Drives;
}

- (BOOL)driveIsAvailable:(NSInteger)drive {
    if (drive < 0 || drive >= self.driveCount) return NO;
    return [self driveIsH37:drive] ? _machine->h37() != nullptr : _machine->h17() != nullptr;
}

- (NSString*)driveName:(NSInteger)drive {
    return [self driveIsH37:drive] ? [NSString stringWithFormat:@"DK%ld:", (long)(drive - kH17Drives)]
                                   : [NSString stringWithFormat:@"SY%ld:", (long)drive];
}

- (BOOL)bootFromH37 {
    return (_machine->config().sw501 & h89::sw501::kBootFrom170) != 0;
}

- (void)setBootFromH37:(BOOL)h37 {
    uint8_t sw = _machine->config().sw501 & ~h89::sw501::kBootFrom170;
    _machine->setSw501(h37 ? (sw | h89::sw501::kBootFrom170) : sw);
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
    std::vector<uint8_t> state = _machine->saveState();
    return [NSData dataWithBytes:state.data() length:state.size()];
}

- (BOOL)loadState:(NSData*)data
         warnings:(NSArray<NSString*>* _Nullable* _Nullable)warnings
            error:(NSError**)error {
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
    if (character < 0x80) _machine->terminal()->typeChar(static_cast<uint8_t>(character));
}

- (void)pressKey:(H89Key)key shift:(BOOL)shift {
    _machine->terminal()->pressKey(static_cast<h89::H19Key>(key), shift);
}

- (void)setBreakKeyDown:(BOOL)down {
    _machine->terminal()->setBreak(down);
}

@end
