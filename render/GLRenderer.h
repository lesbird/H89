#pragma once

#include <cstdint>

namespace h89 {

// Draws the emulated CRT into the current OpenGL 3.2+ core context.
// The caller owns the context and must make it current around every call.
class GLRenderer {
public:
    enum class Phosphor { White, Green, Amber };

    GLRenderer() = default;
    ~GLRenderer();
    GLRenderer(const GLRenderer&) = delete;
    GLRenderer& operator=(const GLRenderer&) = delete;

    bool init(); // compiles shaders and creates the screen texture
    void shutdown();

    // Uploads one luminance byte per pixel.
    void uploadFrame(const uint8_t* pixels, int width, int height);
    // Draws the screen letterboxed to 4:3 into a drawable of this size, in
    // device pixels (i.e. Retina backing size on macOS).
    void draw(int drawableWidth, int drawableHeight);

    void setPhosphor(Phosphor p) { phosphor_ = p; }
    void setScanlines(float strength) { scanlines_ = strength; }

private:
    unsigned program_ = 0;
    unsigned vao_ = 0;
    unsigned texture_ = 0;
    int texWidth_ = 0, texHeight_ = 0;
    int locScreen_ = -1, locTint_ = -1, locSourceSize_ = -1, locScanlines_ = -1;
    Phosphor phosphor_ = Phosphor::White;
    float scanlines_ = 0.35f;
};

} // namespace h89
