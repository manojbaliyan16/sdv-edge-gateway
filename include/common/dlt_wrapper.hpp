#pragma once

// dlt_wrapper.hpp — portable DLT logging for Mac (dev) and Pi (target).
//
// On Pi  : compile with -DUSE_DLT — real DLT daemon receives logs.
// On Mac : no-op stubs — code compiles and runs, logs go nowhere.
//
// Usage in any module:
//   #include "common/dlt_wrapper.hpp"
//   DLT_DECLARE_CONTEXT(ctx_offline_buffer);
//   DLT_REGISTER_CONTEXT(ctx_offline_buffer, "OFBF", "OfflineBuffer logs");
//   DLT_LOG(ctx_offline_buffer, DLT_LOG_ERROR, DLT_STRING("store() failed"));

#ifdef USE_DLT
    #include <dlt/dlt.h>
#else
    // ── Mac no-op stubs ───────────────────────────────────────────────────────
    typedef int DltContext;
    typedef int DltLogLevelType;

    #define DLT_LOG_OFF     0
    #define DLT_LOG_FATAL   1
    #define DLT_LOG_ERROR   2
    #define DLT_LOG_WARN    3
    #define DLT_LOG_INFO    4
    #define DLT_LOG_DEBUG   5
    #define DLT_LOG_VERBOSE 6

    #define DLT_STRING(x)               x
    #define DLT_INT(x)                  x
    #define DLT_UINT(x)                 x
    #define DLT_FLOAT64(x)              x

    #define DLT_DECLARE_CONTEXT(ctx)
    #define DLT_REGISTER_APP(id, desc)
    #define DLT_REGISTER_CONTEXT(ctx, id, desc)
    #define DLT_UNREGISTER_CONTEXT(ctx)
    #define DLT_UNREGISTER_APP()
    #define DLT_LOG(ctx, level, ...)
#endif
