#include "GLRenderer.h"

#include "GL.h"

#include <cstdio>

namespace h89 {

namespace {

// Fullscreen triangle generated from gl_VertexID; no vertex buffers needed.
const char* kVertexShader = R"(#version 150 core
out vec2 vUV;
void main() {
    vec2 pos = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    vUV = vec2(pos.x, 1.0 - pos.y); // row 0 of the framebuffer is the top
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
)";

const char* kFragmentShader = R"(#version 150 core
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uScreen;
uniform vec3 uTint;
uniform vec2 uSourceSize;
uniform float uScanlines;
void main() {
    float lum = texture(uScreen, vUV).r;
    // Darken between scanlines: brightest at the centre of each source row.
    float row = fract(vUV.y * uSourceSize.y);
    float beam = 1.0 - uScanlines * pow(abs(row - 0.5) * 2.0, 2.0);
    fragColor = vec4(uTint * lum * beam, 1.0);
}
)";

unsigned compile(unsigned type, const char* src) {
    unsigned s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        std::fprintf(stderr, "shader compile failed: %s\n", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

} // namespace

GLRenderer::~GLRenderer() { shutdown(); }

bool GLRenderer::init() {
    unsigned vs = compile(GL_VERTEX_SHADER, kVertexShader);
    unsigned fs = compile(GL_FRAGMENT_SHADER, kFragmentShader);
    if (!vs || !fs) return false;

    program_ = glCreateProgram();
    glAttachShader(program_, vs);
    glAttachShader(program_, fs);
    glLinkProgram(program_);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok = 0;
    glGetProgramiv(program_, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(program_, sizeof log, nullptr, log);
        std::fprintf(stderr, "shader link failed: %s\n", log);
        return false;
    }

    locScreen_ = glGetUniformLocation(program_, "uScreen");
    locTint_ = glGetUniformLocation(program_, "uTint");
    locSourceSize_ = glGetUniformLocation(program_, "uSourceSize");
    locScanlines_ = glGetUniformLocation(program_, "uScanlines");

    glGenVertexArrays(1, &vao_); // core profile requires a bound VAO to draw

    glGenTextures(1, &texture_);
    glBindTexture(GL_TEXTURE_2D, texture_);
    // Linear filtering softens the scale-up the way a CRT spot would.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return true;
}

void GLRenderer::shutdown() {
    if (texture_) glDeleteTextures(1, &texture_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
    if (program_) glDeleteProgram(program_);
    texture_ = vao_ = program_ = 0;
}

void GLRenderer::uploadFrame(const uint8_t* pixels, int width, int height) {
    glBindTexture(GL_TEXTURE_2D, texture_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (width != texWidth_ || height != texHeight_) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, width, height, 0, GL_RED, GL_UNSIGNED_BYTE, pixels);
        texWidth_ = width;
        texHeight_ = height;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RED, GL_UNSIGNED_BYTE, pixels);
    }
}

void GLRenderer::draw(int drawableWidth, int drawableHeight) {
    glViewport(0, 0, drawableWidth, drawableHeight);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    if (!program_ || !texWidth_) return;

    // Letterbox to the CRT's 4:3 shape.
    int w = drawableWidth, h = drawableHeight;
    if (w * 3 > h * 4) w = h * 4 / 3;
    else h = w * 3 / 4;
    glViewport((drawableWidth - w) / 2, (drawableHeight - h) / 2, w, h);

    float r = 0.92f, g = 0.95f, b = 1.0f; // P4 white
    if (phosphor_ == Phosphor::Green) r = 0.25f, g = 1.0f, b = 0.35f;
    else if (phosphor_ == Phosphor::Amber) r = 1.0f, g = 0.7f, b = 0.1f;

    glUseProgram(program_);
    glUniform1i(locScreen_, 0);
    glUniform3f(locTint_, r, g, b);
    glUniform2f(locSourceSize_, float(texWidth_), float(texHeight_));
    // Scanline gaps only read correctly once each source row spans >= 2 pixels.
    glUniform1f(locScanlines_, h >= texHeight_ * 2 ? scanlines_ : 0.0f);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture_);
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

} // namespace h89
