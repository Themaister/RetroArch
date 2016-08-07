#ifndef _SHADERS_COMMON
#define _SHADERS_COMMON

#if defined(HAVE_OPENGLES)
#define CG(src)   "" #src
#define GLSL(src) "precision mediump float;\n" #src
#define GLSL_330(src)   "#version 330 es\nprecision mediump float;\n"   #src
#define GLSL_HIGHP(src) "#ifdef GL_FRAGMENT_PRECISION_HIGH\nprecision highp float;\n#else\nprecision mediump float;\n#endif\n" #src
#else
#define CG(src)   "" #src
#define GLSL(src) "" #src
#define GLSL_HIGHP(src) "" #src
#define GLSL_300(src)   "#version 300 es\n"   #src
#define GLSL_330(src)   "#version 330 core\n"   #src
#endif

#endif
