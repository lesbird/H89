#import "H89DebuggerView.h"

#import "H89Emulator+Core.h"

#include "DebugControl.h"
#include "DebuggerUI.h"
#include "imgui.h"
#include "imgui_impl_opengl3.h"

#import <OpenGL/gl3.h>
#import <QuartzCore/QuartzCore.h>

#include <algorithm>
#include <string>

namespace {

// macOS virtual key codes (kVK_* in Carbon) for the keys ImGui needs.
struct KeyMapping {
    unsigned short code;
    ImGuiKey key;
};
constexpr KeyMapping kKeys[] = {
    {0x7B, ImGuiKey_LeftArrow}, {0x7C, ImGuiKey_RightArrow}, {0x7D, ImGuiKey_DownArrow},
    {0x7E, ImGuiKey_UpArrow},   {0x33, ImGuiKey_Backspace},  {0x75, ImGuiKey_Delete},
    {0x24, ImGuiKey_Enter},     {0x4C, ImGuiKey_KeypadEnter}, {0x30, ImGuiKey_Tab},
    {0x35, ImGuiKey_Escape},    {0x73, ImGuiKey_Home},       {0x77, ImGuiKey_End},
    {0x74, ImGuiKey_PageUp},    {0x79, ImGuiKey_PageDown},   {0x00, ImGuiKey_A},
    {0x08, ImGuiKey_C},         {0x09, ImGuiKey_V},          {0x07, ImGuiKey_X},
    {0x06, ImGuiKey_Z},         {0x10, ImGuiKey_Y},
};

ImGuiKey imguiKey(unsigned short code) {
    for (const KeyMapping& k : kKeys)
        if (k.code == code) return k.key;
    return ImGuiKey_None;
}

const char* clipboardText(ImGuiContext*) {
    static std::string text;
    NSString* s = [NSPasteboard.generalPasteboard stringForType:NSPasteboardTypeString];
    text = s ? s.UTF8String : "";
    return text.c_str();
}

void setClipboardText(ImGuiContext*, const char* text) {
    [NSPasteboard.generalPasteboard clearContents];
    [NSPasteboard.generalPasteboard setString:@(text) forType:NSPasteboardTypeString];
}

} // namespace

@implementation H89DebuggerView {
    ImGuiContext* _context;
    h89::DebuggerUI _ui;
    CFTimeInterval _lastFrame;
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
        [self addTrackingArea:[[NSTrackingArea alloc]
                                  initWithRect:NSZeroRect
                                       options:NSTrackingMouseMoved | NSTrackingActiveInKeyWindow |
                                               NSTrackingInVisibleRect
                                         owner:self
                                      userInfo:nil]];
    }
    return self;
}

- (void)dealloc {
    if (_context) {
        [self.openGLContext makeCurrentContext];
        ImGui::SetCurrentContext(_context);
        ImGui_ImplOpenGL3_Shutdown();
        ImGui::DestroyContext(_context);
    }
}

- (void)prepareOpenGL {
    [super prepareOpenGL];
    GLint swapInterval = 0; // the emulator's timer paces redraws
    [self.openGLContext setValues:&swapInterval forParameter:NSOpenGLContextParameterSwapInterval];
    _context = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.BackendPlatformName = "h89_osx_view";
    ImGuiPlatformIO& platform = ImGui::GetPlatformIO();
    platform.Platform_GetClipboardTextFn = clipboardText;
    platform.Platform_SetClipboardTextFn = setClipboardText;
    ImGui::StyleColorsDark();
    ImGui_ImplOpenGL3_Init("#version 150");
    _lastFrame = CACurrentMediaTime();
}

- (BOOL)acceptsFirstResponder {
    return YES;
}

- (BOOL)isFlipped {
    return YES; // ImGui's origin is the top left
}

#pragma mark - Input

- (void)useContext {
    ImGui::SetCurrentContext(_context);
}

- (void)mouseMovedTo:(NSEvent*)event {
    if (!_context) return;
    [self useContext];
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    ImGui::GetIO().AddMousePosEvent((float)p.x, (float)p.y);
}

- (void)mouseMoved:(NSEvent*)event { [self mouseMovedTo:event]; }
- (void)mouseDragged:(NSEvent*)event { [self mouseMovedTo:event]; }
- (void)rightMouseDragged:(NSEvent*)event { [self mouseMovedTo:event]; }

- (void)button:(int)button down:(BOOL)down event:(NSEvent*)event {
    if (!_context) return;
    [self mouseMovedTo:event];
    ImGui::GetIO().AddMouseButtonEvent(button, down);
    self.needsDisplay = YES;
}

- (void)mouseDown:(NSEvent*)event { [self button:0 down:YES event:event]; }
- (void)mouseUp:(NSEvent*)event { [self button:0 down:NO event:event]; }
- (void)rightMouseDown:(NSEvent*)event { [self button:1 down:YES event:event]; }
- (void)rightMouseUp:(NSEvent*)event { [self button:1 down:NO event:event]; }

- (void)scrollWheel:(NSEvent*)event {
    if (!_context) return;
    [self useContext];
    double dx = event.scrollingDeltaX, dy = event.scrollingDeltaY;
    if (event.hasPreciseScrollingDeltas) dx *= 0.1, dy *= 0.1;
    ImGui::GetIO().AddMouseWheelEvent((float)dx, (float)dy);
}

- (void)updateModifiers:(NSEventModifierFlags)flags {
    ImGuiIO& io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiMod_Ctrl, (flags & NSEventModifierFlagControl) != 0);
    io.AddKeyEvent(ImGuiMod_Shift, (flags & NSEventModifierFlagShift) != 0);
    io.AddKeyEvent(ImGuiMod_Alt, (flags & NSEventModifierFlagOption) != 0);
    io.AddKeyEvent(ImGuiMod_Super, (flags & NSEventModifierFlagCommand) != 0);
}

- (void)flagsChanged:(NSEvent*)event {
    if (!_context) return;
    [self useContext];
    [self updateModifiers:event.modifierFlags];
}

- (void)key:(NSEvent*)event down:(BOOL)down {
    if (!_context) return;
    [self useContext];
    ImGuiIO& io = ImGui::GetIO();
    [self updateModifiers:event.modifierFlags];
    ImGuiKey key = imguiKey(event.keyCode);
    if (key != ImGuiKey_None) io.AddKeyEvent(key, down);
    if (down && !(event.modifierFlags & (NSEventModifierFlagCommand | NSEventModifierFlagControl))) {
        for (NSUInteger i = 0; i < event.characters.length; ++i) {
            unichar c = [event.characters characterAtIndex:i];
            if (c >= 0x20 && c != 0x7F && (c < 0xF700 || c > 0xF8FF)) io.AddInputCharacter(c);
        }
    }
    self.needsDisplay = YES;
}

- (void)keyDown:(NSEvent*)event { [self key:event down:YES]; }
- (void)keyUp:(NSEvent*)event { [self key:event down:NO]; }

// Let the app menus have Command shortcuts the debugger doesn't use.
- (BOOL)performKeyEquivalent:(NSEvent*)event {
    if (self.window.firstResponder != self) return NO;
    ImGuiKey key = imguiKey(event.keyCode);
    if ((event.modifierFlags & NSEventModifierFlagCommand) && key >= ImGuiKey_A && key <= ImGuiKey_Z &&
        ImGui::GetCurrentContext() == _context && ImGui::GetIO().WantTextInput) {
        [self key:event down:YES];
        [self key:event down:NO];
        return YES;
    }
    return NO;
}

#pragma mark - Drawing

- (void)drawRect:(NSRect)dirtyRect {
    [self.openGLContext makeCurrentContext];
    if (!_context || !self.emulator) return;
    [self useContext];
    ImGuiIO& io = ImGui::GetIO();
    NSRect backing = [self convertRectToBacking:self.bounds];
    io.DisplaySize = ImVec2((float)self.bounds.size.width, (float)self.bounds.size.height);
    float scale = self.bounds.size.width > 0 ? (float)(backing.size.width / self.bounds.size.width) : 1.0f;
    io.DisplayFramebufferScale = ImVec2(scale, scale);
    CFTimeInterval now = CACurrentMediaTime();
    io.DeltaTime = (float)std::max(now - _lastFrame, 1.0 / 1000);
    _lastFrame = now;

    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();
    _ui.draw(self.emulator.machine, self.emulator.debugControl, true);
    ImGui::Render();
    glViewport(0, 0, (GLsizei)backing.size.width, (GLsizei)backing.size.height);
    glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    [self.openGLContext flushBuffer];
}

@end
