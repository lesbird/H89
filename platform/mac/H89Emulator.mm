#import "H89Emulator.h"
#import "H89Emulator+Core.h"

#include "Machine.h"
#include "RomSet.h"

#include <memory>

@implementation H89Emulator {
    std::unique_ptr<h89::Machine> _machine;
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
- (void)runFrame { _machine->runFrame(); }

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
