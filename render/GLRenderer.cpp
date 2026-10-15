#include "GLRenderer.h"

#include "GL.h"

#include <cstdio>
#include <initializer_list>

namespace h89 {

namespace {

// Fullscreen triangle generated from gl_VertexID; no vertex buffers needed.
// Texture row 0 is the top of the picture: on screen (uFlip = 1) that is
// the top of the viewport, in an offscreen pass it stays row 0.
const char* kVertexShader = R"(#version 150 core
uniform float uFlip;
out vec2 vUV;
void main() {
    vec2 pos = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    vUV = vec2(pos.x, mix(pos.y, 1.0 - pos.y, uFlip));
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
)";

// Phosphor persistence: what was lit fades by uDecay each frame.
const char* kPersistShader = R"(#version 150 core
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uFrame;
uniform sampler2D uPrevious;
uniform float uDecay;
void main() {
    float now = texture(uFrame, vUV).r;
    float before = texture(uPrevious, vUV).r * uDecay;
    fragColor = vec4(max(now, before), 0.0, 0.0, 1.0);
}
)";

// One direction of a 9-tap Gaussian blur, uStep texels apart.
const char* kBlurShader = R"(#version 150 core
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uSource;
uniform vec2 uStep;
void main() {
    const float w[5] = float[](0.227, 0.195, 0.122, 0.054, 0.016);
    float sum = texture(uSource, vUV).r * w[0];
    for (int i = 1; i < 5; ++i) {
        sum += texture(uSource, vUV + uStep * float(i)).r * w[i];
        sum += texture(uSource, vUV - uStep * float(i)).r * w[i];
    }
    fragColor = vec4(sum, 0.0, 0.0, 1.0);
}
)";

const char* kFinalShader = R"(#version 150 core
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uScreen;
uniform sampler2D uGlow;
uniform vec3 uTint;
uniform vec2 uSourceSize;
uniform float uScanlines;
uniform float uCurvature;
uniform float uGlowAmount;
uniform float uVignette;
void main() {
    // Barrel distortion of the tube face.
    vec2 c = vUV * 2.0 - 1.0;
    c *= 1.0 + uCurvature * vec2(c.y * c.y, c.x * c.x);
    vec2 uv = c * 0.5 + 0.5;
    // Rounded corners of the bezel opening.
    vec2 edge = max(abs(c) - (1.0 - 0.06), 0.0);
    float mask = 1.0 - smoothstep(0.0, 0.012, length(edge) - (uCurvature > 0.0 ? 0.06 : 0.0));
    if (uCurvature <= 0.0) mask = 1.0;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) mask = 0.0;

    float lum = texture(uScreen, uv).r;
    // Darken between scanlines: brightest at the centre of each source row.
    float row = fract(uv.y * uSourceSize.y);
    float beam = 1.0 - uScanlines * pow(abs(row - 0.5) * 2.0, 2.0);
    float glow = texture(uGlow, uv).r * uGlowAmount;
    float vignette = 1.0 - uVignette * dot(c, c) * 0.5;
    vec3 colour = uTint * (lum * beam + glow) * vignette;
    fragColor = vec4(colour * mask, 1.0);
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

void setTexture(unsigned program, const char* name, int unit, unsigned texture) {
    glUniform1i(glGetUniformLocation(program, name), unit);
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, texture);
}

void configureTexture() {
    // Linear filtering softens the scale-up the way a CRT spot would.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

} // namespace

GLRenderer::~GLRenderer() { shutdown(); }

unsigned GLRenderer::link(const char* fragmentSource) {
    unsigned vs = compile(GL_VERTEX_SHADER, kVertexShader);
    unsigned fs = compile(GL_FRAGMENT_SHADER, fragmentSource);
    if (!vs || !fs) return 0;
    unsigned program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(program, sizeof log, nullptr, log);
        std::fprintf(stderr, "shader link failed: %s\n", log);
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

bool GLRenderer::init() {
    finalProgram_ = link(kFinalShader);
    persistProgram_ = link(kPersistShader);
    blurProgram_ = link(kBlurShader);
    if (!finalProgram_ || !persistProgram_ || !blurProgram_) return false;

    glGenVertexArrays(1, &vao_); // core profile requires a bound VAO to draw
    glGenTextures(1, &frame_);
    glBindTexture(GL_TEXTURE_2D, frame_);
    configureTexture();
    return true;
}

void GLRenderer::shutdown() {
    for (Target* t : {&persist_[0], &persist_[1], &glow_[0], &glow_[1]}) deleteTarget(*t);
    if (frame_) glDeleteTextures(1, &frame_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
    for (unsigned* p : {&finalProgram_, &persistProgram_, &blurProgram_}) {
        if (*p) glDeleteProgram(*p);
        *p = 0;
    }
    frame_ = vao_ = 0;
    texWidth_ = texHeight_ = 0;
}

bool GLRenderer::makeTarget(Target& t, int width, int height) {
    deleteTarget(t);
    glGenTextures(1, &t.texture);
    glBindTexture(GL_TEXTURE_2D, t.texture);
    configureTexture();
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, width, height, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
    glGenFramebuffers(1, &t.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.texture, 0);
    bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    return ok;
}

void GLRenderer::deleteTarget(Target& t) {
    if (t.fbo) glDeleteFramebuffers(1, &t.fbo);
    if (t.texture) glDeleteTextures(1, &t.texture);
    t = Target{};
}

void GLRenderer::runPass(unsigned program, const Target& target) {
    glBindFramebuffer(GL_FRAMEBUFFER, target.fbo);
    glViewport(0, 0, texWidth_, texHeight_);
    glUniform1f(glGetUniformLocation(program, "uFlip"), 0.0f);
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

void GLRenderer::uploadFrame(const uint8_t* pixels, int width, int height) {
    GLint screenFbo = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &screenFbo);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, frame_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (width != texWidth_ || height != texHeight_) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, width, height, 0, GL_RED, GL_UNSIGNED_BYTE, pixels);
        texWidth_ = width;
        texHeight_ = height;
        bool ok = true;
        for (Target* t : {&persist_[0], &persist_[1], &glow_[0], &glow_[1]}) ok = makeTarget(*t, width, height) && ok;
        if (!ok) {
            std::fprintf(stderr, "CRT effects unavailable: offscreen framebuffers incomplete\n");
            effects_ = false;
        }
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RED, GL_UNSIGNED_BYTE, pixels);
    }

    if (effects_) {
        // How much of the last frame's light remains 1/60 s later.
        float decay = phosphor_ == Phosphor::White ? 0.2f : phosphor_ == Phosphor::Green ? 0.45f : 0.55f;
        int next = current_ ^ 1;
        glUseProgram(persistProgram_);
        setTexture(persistProgram_, "uFrame", 0, frame_);
        setTexture(persistProgram_, "uPrevious", 1, persist_[current_].texture);
        glUniform1f(glGetUniformLocation(persistProgram_, "uDecay"), decay);
        runPass(persistProgram_, persist_[next]);
        current_ = next;

        glUseProgram(blurProgram_);
        setTexture(blurProgram_, "uSource", 0, persist_[current_].texture);
        glUniform2f(glGetUniformLocation(blurProgram_, "uStep"), 1.5f / width, 0.0f);
        runPass(blurProgram_, glow_[0]);
        setTexture(blurProgram_, "uSource", 0, glow_[0].texture);
        glUniform2f(glGetUniformLocation(blurProgram_, "uStep"), 0.0f, 1.5f / height);
        runPass(blurProgram_, glow_[1]);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(screenFbo));
}

void GLRenderer::draw(int drawableWidth, int drawableHeight) {
    glViewport(0, 0, drawableWidth, drawableHeight);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    if (!finalProgram_ || !texWidth_) return;

    // Letterbox to the CRT's 4:3 shape.
    int w = drawableWidth, h = drawableHeight;
    if (w * 3 > h * 4) w = h * 4 / 3;
    else h = w * 3 / 4;
    glViewport((drawableWidth - w) / 2, (drawableHeight - h) / 2, w, h);

    float r = 0.92f, g = 0.95f, b = 1.0f; // P4 white
    if (phosphor_ == Phosphor::Green) r = 0.25f, g = 1.0f, b = 0.35f;
    else if (phosphor_ == Phosphor::Amber) r = 1.0f, g = 0.7f, b = 0.1f;

    unsigned p = finalProgram_;
    glUseProgram(p);
    glUniform1f(glGetUniformLocation(p, "uFlip"), 1.0f);
    setTexture(p, "uScreen", 0, effects_ ? persist_[current_].texture : frame_);
    setTexture(p, "uGlow", 1, effects_ ? glow_[1].texture : frame_);
    glUniform3f(glGetUniformLocation(p, "uTint"), r, g, b);
    glUniform2f(glGetUniformLocation(p, "uSourceSize"), float(texWidth_), float(texHeight_));
    // Scanline gaps only read correctly once each source row spans >= 2 pixels.
    glUniform1f(glGetUniformLocation(p, "uScanlines"), h >= texHeight_ * 2 ? scanlines_ : 0.0f);
    glUniform1f(glGetUniformLocation(p, "uCurvature"), effects_ ? 0.045f : 0.0f);
    glUniform1f(glGetUniformLocation(p, "uGlowAmount"), effects_ ? 0.45f : 0.0f);
    glUniform1f(glGetUniformLocation(p, "uVignette"), effects_ ? 0.35f : 0.0f);
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glActiveTexture(GL_TEXTURE0);
}

} // namespace h89
