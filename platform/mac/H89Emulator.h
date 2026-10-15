#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

// Drives 0-2 are the H17's SY0:-SY2:, 3-6 the H37's DK0:-DK3:.
static const NSInteger H89FirstH37Drive = 3;

// H19 keys that don't produce a plain character (mirrors h89::H19Key).
typedef NS_ENUM(NSInteger, H89Key) {
    H89KeyF1, H89KeyF2, H89KeyF3, H89KeyF4, H89KeyF5,
    H89KeyErase, H89KeyBlue, H89KeyRed, H89KeyWhite,
    H89KeyKeypad0, H89KeyKeypad1, H89KeyKeypad2, H89KeyKeypad3, H89KeyKeypad4,
    H89KeyKeypad5, H89KeyKeypad6, H89KeyKeypad7, H89KeyKeypad8, H89KeyKeypad9,
    H89KeyKeypadDot, H89KeyKeypadEnter, H89KeyScroll,
};

// Objective-C face of the C++ emulation core. This header is plain
// Objective-C so it can be imported from any AppKit (or Swift) code; the C++
// Machine lives only in H89Emulator.mm.
@interface H89Emulator : NSObject

// Loads the ROMs from `directory`; returns nil and sets `error` if any
// required ROM is missing. `preferMTR89` picks the MTR-89 monitor when the
// folder has both; `memoryKB` is 16, 32, 48 or 64.
- (nullable instancetype)initWithRomDirectory:(NSString*)directory
                                  preferMTR89:(BOOL)preferMTR89
                                     memoryKB:(NSInteger)memoryKB
                                        error:(NSError**)error NS_DESIGNATED_INITIALIZER;
- (nullable instancetype)initWithRomDirectory:(NSString*)directory error:(NSError**)error;
// Just the H19, as a terminal for a real computer on a host serial port (see
// -connectTerminal:). Only the H19 ROMs (444-46, 444-29) are needed.
- (nullable instancetype)initTerminalWithRomDirectory:(NSString*)directory
                                                error:(NSError**)error NS_DESIGNATED_INITIALIZER;

// YES for a terminal on its own: no computer, no drives, no H89 serial card,
// no states and no debugger.
@property(nonatomic, readonly) BOOL terminalOnly;
// Terminal only: opens the host serial port, "DEVICE[:BAUD[:FORMAT[:rtscts]]]"
// such as "/dev/cu.usbserial-1410:115200:8N1". The line runs at that rate,
// whatever SW401 says. If the port can't be opened the terminal runs
// anyway and the port is retried every second; that also happens if it goes
// away (an adapter unplugged).
- (BOOL)connectTerminal:(NSString*)spec error:(NSError**)error;
- (void)disconnectTerminal;
// "/dev/cu.usbserial-1410 at 115200 8N1", or why it isn't connected.
@property(nonatomic, readonly) NSString* terminalStatus;
// The serial ports on this Mac (/dev/cu.*).
+ (NSArray<NSString*>*)hostSerialPorts;

// File transfers over the terminal's serial port (terminal only). One runs at
// a time; while a protocol transfer runs, the keyboard is ignored.
typedef NS_ENUM(NSInteger, H89Upload) { H89UploadText, H89UploadXmodem, H89UploadXmodem1K, H89UploadZmodem };
// Text files: line endings (0 CR, 1 CR LF, 2 LF, 3 unchanged) and pauses.
- (void)setTextLineEnding:(NSInteger)ending charDelay:(NSInteger)charMs lineDelay:(NSInteger)lineMs;
// Text and XMODEM take one file; ZMODEM takes several.
- (BOOL)startUpload:(H89Upload)kind paths:(NSArray<NSString*>*)paths error:(NSError**)error;
- (BOOL)startXmodemDownloadTo:(NSString*)path error:(NSError**)error;
// The sender names each file: transferOfferedName then asks where it goes.
- (BOOL)startZmodemDownload:(NSError**)error;
- (void)cancelTransfer;
// Forgets a finished transfer.
- (void)clearTransfer;
@property(nonatomic, readonly) BOOL transferExists;
@property(nonatomic, readonly) BOOL transferRunning;
@property(nonatomic, readonly) BOOL transferSucceeded;
@property(nonatomic, readonly) NSString* transferTitle;
@property(nonatomic, readonly) NSString* transferStatus;
// 0 to 1, or -1 when the size isn't known.
@property(nonatomic, readonly) double transferFraction;
// A ZMODEM download was offered this file: answer with -setTransferSavePath:
// (nil skips it).
@property(nonatomic, readonly, nullable) NSString* transferOfferedName;
- (void)setTransferSavePath:(nullable NSString*)path;
// The H19's SW401 (in H89 mode also its baud rate) and SW402. Changing them
// resets the terminal, as switching it off and on would.
- (void)setTerminalSW401:(NSInteger)sw401 SW402:(NSInteger)sw402;
- (instancetype)init NS_UNAVAILABLE;

// Which ROM files were loaded, one per line.
@property(nonatomic, readonly) NSString* romSummary;

@property(nonatomic, readonly) NSInteger screenWidth;
@property(nonatomic, readonly) NSInteger screenHeight;
@property(nonatomic, readonly) uint64_t frameCount;

// Called on the emulation thread (currently the main thread) when the H19
// rings its bell.
@property(nonatomic, copy, nullable) void (^bellHandler)(void);
// Called likewise when the H19 clicks for a key press.
@property(nonatomic, copy, nullable) void (^keyClickHandler)(void);

- (void)reset;
// Advances emulation by one 1/60 s frame, unless the debugger has paused it.
- (void)runFrame;
// True while stopped in the debugger (paused, stepping, or at a breakpoint).
@property(nonatomic, readonly) BOOL debuggerPaused;
// One luminance byte per pixel, screenWidth x screenHeight, valid until the
// next -runFrame.
- (const uint8_t*)framebuffer NS_RETURNS_INNER_POINTER;

// Floppy drives: 0-2 are the H17's SY0:-SY2: (hard-sectored .h8d images),
// 3-6 the H37's DK0:-DK3: (soft-sectored .h37 / .imd images). Writes go
// straight back to the image file unless mounted read-only.
@property(nonatomic, readonly) NSInteger driveCount;
// NO if the drive's controller isn't fitted (the H17 needs its ROM).
- (BOOL)driveIsAvailable:(NSInteger)drive;
- (BOOL)driveIsH37:(NSInteger)drive;
// "SY0:" or "DK0:".
- (NSString*)driveName:(NSInteger)drive;
// Boot from the H37 (SW501 section 4) instead of the H17; takes effect at
// the next boot.
@property(nonatomic) BOOL bootFromH37;
// MTR-90 boots the primary device at power-on and reset, without waiting
// for the B command (SW501 section 8).
@property(nonatomic) BOOL autoBoot;
- (BOOL)insertDiskAtPath:(NSString*)path
                   drive:(NSInteger)drive
                readOnly:(BOOL)readOnly
                   error:(NSError**)error;
- (void)ejectDrive:(NSInteger)drive;
// Path of the disk in `drive`, or nil if empty.
- (nullable NSString*)diskPathForDrive:(NSInteger)drive;
- (BOOL)diskIsReadOnlyInDrive:(NSInteger)drive;

// The serial card's ports: 0 AUX (D0h), 1 MODEM (D8h), 2 LP printer (E0h).
// A port connects to "file:PATH" (output appended), "listen:PORT" (one TCP
// client on this Mac) or "connect:HOST:PORT".
typedef NS_ENUM(NSInteger, H89SerialPort) { H89SerialPortAux, H89SerialPortModem, H89SerialPortPrinter };
- (BOOL)connectSerialPort:(H89SerialPort)port spec:(NSString*)spec error:(NSError**)error;
- (void)disconnectSerialPort:(H89SerialPort)port;
// The spec it was connected with, or nil.
- (nullable NSString*)serialPortSpec:(H89SerialPort)port;
// "listening on port 8089", "client connected on port 8089", a file path...
- (NSString*)serialPortStatus:(H89SerialPort)port;

// Snapshot of the whole machine, including which disks are mounted (by
// path). Loading fails, leaving the machine as it was, if the state came
// from different ROMs; `warnings` lists disks that are missing or changed.
- (NSData*)saveState;
- (BOOL)loadState:(NSData*)data
         warnings:(NSArray<NSString*>* _Nullable* _Nullable)warnings
            error:(NSError**)error;

// Keyboard: ASCII characters (including control characters) and special keys.
- (void)typeCharacter:(unichar)character;
- (void)pressKey:(H89Key)key shift:(BOOL)shift;
- (void)setBreakKeyDown:(BOOL)down;

@end

NS_ASSUME_NONNULL_END
