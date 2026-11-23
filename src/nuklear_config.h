/* Shared Nuklear configuration: every translation unit that touches Nuklear
 * includes this so the structs, flags and inline helpers agree. Exactly one TU
 * (main.c) defines NK_IMPLEMENTATION before including it. */
#ifndef MP_NUKLEAR_CONFIG_H
#define MP_NUKLEAR_CONFIG_H

#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_DEFAULT_FONT
#define NK_INCLUDE_STANDARD_VARARGS
#include "nuklear.h"

#endif
