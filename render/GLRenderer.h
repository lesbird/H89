#pragma once

#include <cstdint>

namespace h89 {

// Draws the emulated CRT into the current OpenGL 3.2+ core context.
// The caller owns the context and must make it current around every call.
//
// With CRT effects on, each uploaded frame is blended with a decaying copy
// of the previous ones (phosphor persistence), blurred into a glow, and the
// picture is drawn on curved glass with scanlines and a vignette. Off, it is
// a plain tinted scale-up with scanlines.
class GLRenderer {
public:
    enum class Phosphor { White, Green, Amber };

    GLRenderer() = default;
    ~GLRenderer();
    GLRenderer(const GLRenderer&) = delete;
    GLRenderer& operator=(const GLRenderer&) = delete;

    bool init(); // compiles shaders and creates the textures
    void shutdown();

    // Uploads one luminance byte per pixel (once per emulated frame).
    void uploadFrame(const uint8_t* pixels, int width, int height);
    // Draws the screen letterboxed to 4:3 into a drawable of this size, in
    // device pixels (i.e. Retina backing size on macOS).
    void draw(int drawableWidth, int drawableHeight);

    void setPhosphor(Phosphor p) { phosphor_ = p; }
    void setScanlines(float strength) { scanlines_ = strength; }
    void setEffects(bool on) { effects_ = on; }
    bool effects() const { return effects_; }

private:
    struct Target { // a texture with a framebuffer that renders into it
        unsigned texture = 0, fbo = 0;
    };
    bool makeTarget(Target& t, int width, int height);
    void deleteTarget(Target& t);
    // Renders `program` over the whole of `target` (its own size).
    void runPass(unsigned program, const Target& target);
    unsigned link(const char* fragmentSource);

    unsigned finalProgram_ = 0, persistProgram_ = 0, blurProgram_ = 0;
    unsigned vao_ = 0;
    unsigned frame_ = 0; // the latest uploaded frame
    Target persist_[2];  // ping-pong phosphor persistence
    Target glow_[2];     // horizontal, then vertical blur
    int current_ = 0;    // which persist_ holds the latest image
    int texWidth_ = 0, texHeight_ = 0;
    Phosphor phosphor_ = Phosphor::White;
    float scanlines_ = 0.35f;
    bool effects_ = true;
};

} // namespace h89
