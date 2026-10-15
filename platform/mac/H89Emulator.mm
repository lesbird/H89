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

- (NSInteger)driveCount {
    return _machine->h17() ? h89::H17::kDrives : 0;
}

- (BOOL)insertDiskAtPath:(NSString*)path
                   drive:(NSInteger)drive
                readOnly:(BOOL)readOnly
                   error:(NSError**)error {
    std::string message = "This machine has no H17 controller (444-19 ROM missing).";
    if (_machine->h17() && drive >= 0 && drive < self.driveCount &&
        _machine->h17()->insertDisk(static_cast<int>(drive), path.fileSystemRepresentation, readOnly,
                                    &message)) {
        return YES;
    }
    if (error) {
        *error = [NSError errorWithDomain:@"H89"
                                     code:2
                                 userInfo:@{NSLocalizedDescriptionKey : @(message.c_str())}];
    }
    return NO;
}

- (void)ejectDrive:(NSInteger)drive {
    if (_machine->h17() && drive >= 0 && drive < self.driveCount)
        _machine->h17()->ejectDisk(static_cast<int>(drive));
}

- (nullable NSString*)diskPathForDrive:(NSInteger)drive {
    if (!_machine->h17() || drive < 0 || drive >= self.driveCount) return nil;
    const h89::H17Disk& disk = _machine->h17()->disk(static_cast<int>(drive));
    return disk.loaded() ? @(disk.path().c_str()) : nil;
}

- (nullable h89::H17Disk*)diskForDrive:(NSInteger)drive {
    if (!_machine->h17() || drive < 0 || drive >= self.driveCount) return nullptr;
    return &_machine->h17()->disk(static_cast<int>(drive));
}

- (BOOL)diskIsReadOnlyInDrive:(NSInteger)drive {
    if (!_machine->h17() || drive < 0 || drive >= self.driveCount) return NO;
    return _machine->h17()->disk(static_cast<int>(drive)).readOnly();
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
