/*
 * Mesa 3-D graphics library
 *
 * Copyright (C) 1999-2008  Brian Paul   All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */


/**
 * \mainpage Mesa GL API Module
 *
 * \section GLAPIIntroduction Introduction
 *
 * The Mesa GL API module is responsible for dispatching all the
 * gl*() functions.  All GL functions are dispatched by jumping through
 * the current dispatch table (basically a struct full of function
 * pointers.)
 *
 * A per-thread current dispatch table and per-thread current context
 * pointer are managed by this module too.
 *
 * This module is intended to be non-Mesa-specific so it can be used
 * with the X/DRI libGL also.
 */


#ifndef _GLAPI_H
#define _GLAPI_H

#include "util/macros.h"
#include "util/u_thread.h"
#include "util/detect_os.h"


#ifdef __cplusplus
extern "C" {
#endif


#ifdef _GLAPI_NO_EXPORTS
#  define _GLAPI_EXPORT
#else /* _GLAPI_NO_EXPORTS */
#  ifdef _WIN32
#    ifdef _GLAPI_DLL_EXPORTS
#      define _GLAPI_EXPORT __declspec(dllexport)
#    else
#      define _GLAPI_EXPORT __declspec(dllimport)
#    endif
#  elif defined(__GNUC__)
#    define _GLAPI_EXPORT __attribute__((visibility("default")))
#  else
#    define _GLAPI_EXPORT
#  endif
#endif /* _GLAPI_NO_EXPORTS */

#include "GL/gl.h"
#include "GL/glext.h"


struct _glapi_table;

typedef void (*_glapi_proc)(void); /* generic function pointer */

typedef void (*_glapi_nop_handler_proc)(const char *name);

typedef void (*_glapi_warning_func)(void *ctx, const char *str, ...);


/*
 * Number of extension functions which we can dynamically add at runtime.
 */
#define MAX_EXTENSION_FUNCS 300

#if DETECT_OS_WINDOWS
extern __THREAD_INITIAL_EXEC struct _glapi_table * _glapi_tls_Dispatch;
extern __THREAD_INITIAL_EXEC void * _glapi_tls_Context;
#else
_GLAPI_EXPORT extern __THREAD_INITIAL_EXEC struct _glapi_table * _glapi_tls_Dispatch;
_GLAPI_EXPORT extern __THREAD_INITIAL_EXEC void * _glapi_tls_Context;
#endif

#if DETECT_OS_WINDOWS && !defined(MAPI_MODE_UTIL) && !defined(MAPI_MODE_GLAPI)
# define GET_DISPATCH() _glapi_get_dispatch()
# define GET_CURRENT_CONTEXT(C)  struct gl_context *C = (struct gl_context *) _glapi_get_context()
#else
# define GET_DISPATCH() _glapi_tls_Dispatch
# define GET_CURRENT_CONTEXT(C)  struct gl_context *C = (struct gl_context *) _glapi_tls_Context
#endif

#ifdef INSERVER
#define SERVEXTERN _declspec(dllimport)
#else
#define SERVEXTERN _declspec(dllexport)
#endif

SERVEXTERN struct _glapi_table *_glapi_Dispatch;
SERVEXTERN void *_glapi_Context;
SERVEXTERN void
_glapi_destroy_multithread(void);


SERVEXTERN void
_glapi_check_multithread(void);


SERVEXTERN void
_glapi_set_context(void *context);


SERVEXTERN void *
_glapi_get_context(void);


SERVEXTERN void
_glapi_set_dispatch(struct _glapi_table *dispatch);


SERVEXTERN struct _glapi_table *
_glapi_get_dispatch(void);

SERVEXTERN int
_glapi_begin_dispatch_override(struct _glapi_table *override);

SERVEXTERN void
_glapi_end_dispatch_override(int layer);

struct _glapi_table *
_glapi_get_override_dispatch(int layer);

SERVEXTERN GLuint
_glapi_get_dispatch_table_size(void);


SERVEXTERN int
_glapi_add_dispatch( const char * const * function_names,
		     const char * parameter_signature );

_GLAPI_EXPORT _glapi_proc
_glapi_get_proc_address(const char *funcName);

_GLAPI_EXPORT const char *
_glapi_get_proc_name(unsigned int offset);

extern struct _glapi_table *
_glapi_create_table_from_handle(void *handle, const char *symbol_prefix);

_GLAPI_EXPORT void
_glapi_table_patch(struct _glapi_table *, const char *name, void *wrapper);


void
_glapi_set_nop_handler(_glapi_nop_handler_proc func);

/** Return pointer to new dispatch table filled with no-op functions */
struct _glapi_table *
_glapi_new_nop_table(unsigned num_entries);



#ifdef __cplusplus
}
#endif

#endif /* _GLAPI_H */
