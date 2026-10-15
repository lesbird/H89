#import "H89Emulator.h"

// C++ access to the core, for Objective-C++ files only.
namespace h89 {
class H17Disk;
}

@interface H89Emulator (Core)
// The disk in `drive` (loaded or not), or null if there is no such drive.
- (nullable h89::H17Disk*)diskForDrive:(NSInteger)drive;
@end
