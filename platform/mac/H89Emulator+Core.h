#import "H89Emulator.h"

// C++ access to the core, for Objective-C++ files only.
namespace h89 {
class H17Disk;
class SoftDisk;
}

@interface H89Emulator (Core)
// The disk in `drive` (loaded or not), or null if there is no such drive.
- (nullable h89::H17Disk*)diskForDrive:(NSInteger)drive;
// The disk in an H37 drive (3-6), or null for other drives.
- (nullable h89::SoftDisk*)softDiskForDrive:(NSInteger)drive;
@end
