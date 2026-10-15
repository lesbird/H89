#import "H89View.h"

#import "H89Emulator.h"

#include "GLRenderer.h"

namespace {

// Virtual key codes of the numeric keypad (kVK_ANSI_Keypad* in Carbon).
struct KeypadMapping {
    unsigned short keyCode;
    H89Key key;
};
constexpr KeypadMapping kKeypad[] = {
    {0x52, H89KeyKeypad0}, {0x53, H89KeyKeypad1}, {0x54, H89KeyKeypad2},
    {0x55, H89KeyKeypad3}, {0x56, H89KeyKeypad4}, {0x57, H89KeyKeypad5},
    {0x58, H89KeyKeypad6}, {0x59, H89KeyKeypad7}, {0x5B, H89KeyKeypad8},
    {0x5C, H89KeyKeypad9}, {0x41, H89KeyKeypadDot}, {0x4C, H89KeyKeypadEnter},
};

constexpr unichar kBreakKey = NSF10FunctionKey;

} // namespace

@implementation H89View {
    h89::GLRenderer _renderer;
    BOOL _rendererReady;
}

- (instancetype)initWithFrame:(NSRect)frame {
    NSOpenGLPixelFormatAttribute attrs[] = {
        NSOpenGLPFAOpenGLProfile, NSOpenGLProfileVersion3_2Core,
        NSOpenGLPFAColorSize, 24,
        NSOpenGLPFAAlphaSize, 8,
        NSOpenGLPFADoubleBuffer,
        NSOpenGLPFAAccelerated,
        0,
    };
    NSOpenGLPixelFormat* format = [[NSOpenGLPixelFormat alloc] initWithAttributes:attrs];
    if ((self = [super initWithFrame:frame pixelFormat:format])) {
        self.wantsBestResolutionOpenGLSurface = YES;
    }
    return self;
}

- (void)prepareOpenGL {
    [super prepareOpenGL];
    GLint swapInterval = 1;
    [self.openGLContext setValues:&swapInterval forParameter:NSOpenGLContextParameterSwapInterval];
    _rendererReady = _renderer.init();
    if (!_rendererReady) NSLog(@"H89: OpenGL renderer failed to initialise");
}

- (void)setPhosphor:(H89Phosphor)phosphor {
    _phosphor = phosphor;
    switch (phosphor) {
    case H89PhosphorWhite: _renderer.setPhosphor(h89::GLRenderer::Phosphor::White); break;
    case H89PhosphorGreen: _renderer.setPhosphor(h89::GLRenderer::Phosphor::Green); break;
    case H89PhosphorAmber: _renderer.setPhosphor(h89::GLRenderer::Phosphor::Amber); break;
    }
    self.needsDisplay = YES;
}

- (void)drawRect:(NSRect)dirtyRect {
    [self.openGLContext makeCurrentContext];
    if (_rendererReady && self.emulator) {
        _renderer.uploadFrame(self.emulator.framebuffer, (int)self.emulator.screenWidth,
                              (int)self.emulator.screenHeight);
    }
    NSRect backing = [self convertRectToBacking:self.bounds];
    _renderer.draw((int)backing.size.width, (int)backing.size.height);
    [self.openGLContext flushBuffer];
}

- (void)dealloc {
    [self.openGLContext makeCurrentContext];
    _renderer.shutdown();
}

- (BOOL)acceptsFirstResponder {
    return YES;
}

#pragma mark - Keyboard

- (void)keyDown:(NSEvent*)event {
    H89Emulator* emu = self.emulator;
    if (!emu) return;
    BOOL shift = (event.modifierFlags & NSEventModifierFlagShift) != 0;

    for (const auto& m : kKeypad) {
        if (event.keyCode == m.keyCode) {
            [emu pressKey:m.key shift:shift];
            return;
        }
    }

    NSString* bare = event.charactersIgnoringModifiers;
    unichar base = bare.length ? [bare characterAtIndex:0] : 0;
    switch (base) {
    // The H19 has no arrow keys: the shifted keypad sends cursor motion.
    case NSUpArrowFunctionKey: [emu pressKey:H89KeyKeypad8 shift:YES]; return;
    case NSDownArrowFunctionKey: [emu pressKey:H89KeyKeypad2 shift:YES]; return;
    case NSLeftArrowFunctionKey: [emu pressKey:H89KeyKeypad4 shift:YES]; return;
    case NSRightArrowFunctionKey: [emu pressKey:H89KeyKeypad6 shift:YES]; return;
    case NSHomeFunctionKey: [emu pressKey:H89KeyKeypad5 shift:YES]; return;
    case NSF1FunctionKey: [emu pressKey:H89KeyF1 shift:shift]; return;
    case NSF2FunctionKey: [emu pressKey:H89KeyF2 shift:shift]; return;
    case NSF3FunctionKey: [emu pressKey:H89KeyF3 shift:shift]; return;
    case NSF4FunctionKey: [emu pressKey:H89KeyF4 shift:shift]; return;
    case NSF5FunctionKey: [emu pressKey:H89KeyF5 shift:shift]; return;
    case NSF6FunctionKey: [emu pressKey:H89KeyErase shift:shift]; return;
    case NSF7FunctionKey: [emu pressKey:H89KeyBlue shift:shift]; return;
    case NSF8FunctionKey: [emu pressKey:H89KeyRed shift:shift]; return;
    case NSF9FunctionKey: [emu pressKey:H89KeyWhite shift:shift]; return;
    case kBreakKey: [emu setBreakKeyDown:YES]; return;
    case NSPageDownFunctionKey: [emu pressKey:H89KeyScroll shift:shift]; return;
    case NSDeleteFunctionKey: [emu typeCharacter:0x7F]; return; // forward delete = DELETE
    case 0x7F: [emu typeCharacter:0x08]; return;                 // Mac delete = BACK SPACE
    default: break;
    }

    // Everything else: the character AppKit produced, with Shift and Control
    // already applied (Control-C arrives as 03h).
    NSString* chars = event.characters;
    for (NSUInteger i = 0; i < chars.length; ++i) {
        unichar c = [chars characterAtIndex:i];
        if (c < 0x80) [emu typeCharacter:c];
    }
}

- (void)keyUp:(NSEvent*)event {
    NSString* bare = event.charactersIgnoringModifiers;
    if (bare.length && [bare characterAtIndex:0] == kBreakKey) [self.emulator setBreakKeyDown:NO];
}

@end
