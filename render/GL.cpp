#include "GL.h"

#if !defined(__APPLE__)

#define H89_GL_DEFINE(ret, name, args) PFN_##name name = nullptr;
H89_GL_FUNCTIONS(H89_GL_DEFINE)
#undef H89_GL_DEFINE

namespace gl {

bool loadFunctions(void* (*getProc)(const char*)) {
    bool ok = true;
#define H89_GL_LOAD(ret, name, args)                                                     \
    name = reinterpret_cast<PFN_##name>(getProc(#name));                                 \
    ok = ok && name != nullptr;
    H89_GL_FUNCTIONS(H89_GL_LOAD)
#undef H89_GL_LOAD
    return ok;
}

} // namespace gl

#endif
