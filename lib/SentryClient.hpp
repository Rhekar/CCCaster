#pragma once

#include <string>


// The DSN is baked in at build time via -DSENTRY_DSN (see the Makefile). Fall back to an empty
// string so any translation unit that includes this header still compiles if the define is absent
// (e.g. cppcheck, ad-hoc tool builds). An empty DSN leaves reporting disabled.
#ifndef SENTRY_DSN
#define SENTRY_DSN ""
#endif


// Lightweight Sentry / Glitchtip crash & error reporter.
//
// Self-contained: depends only on <windows.h>, <wininet.h> and the C++ stdlib, so it can be linked
// into the standalone, the injected hook.dll, AND the dependency-light launcher.exe. It deliberately
// does NOT use LOG/ASSERT (would recurse through the crash path) or EventManager/Thread (uses raw
// Win32 threading so launcher.exe doesn't need to pull in winpthread).
//
// All entry points are cheap no-ops until init() is given a non-empty DSN, so call sites need no
// #ifdef guards. The DSN is baked in at build time via -DSENTRY_DSN (see the Makefile); an empty
// SENTRY_DSN leaves reporting disabled with effectively zero overhead.

namespace SentryClient
{

enum class Level { Debug, Info, Warning, Error, Fatal };


// Configure the client. Empty dsn => disabled (every other call becomes a no-op).
//   dsn         : Sentry DSN, e.g. https://<publicKey>@host[:port]/<projectId>
//   release     : maps to Sentry "release" (we pass the CCCaster version code)
//   environment : maps to Sentry "environment" (we pass the build type: debug/logging/release)
//   dist        : optional build distinguisher (revision / build time)
void init ( const std::string& dsn,
            const std::string& release,
            const std::string& environment,
            const std::string& dist = "" );

bool isEnabled();

// Route internal diagnostics (init result, handler install, crash caught, upload status) to this
// sink — e.g. the app's logger so they land in the debug/dll log. Diagnostics are ALSO always
// emitted via OutputDebugString. Set this before init() to capture the init diagnostics too.
typedef void ( *LogSink ) ( const char *message );
void setLogSink ( LogSink sink );

// A tag attached to every subsequent event (e.g. "mode", "revision", "wine").
void setTag ( const std::string& key, const std::string& value );

// Append a breadcrumb to the bounded ring buffer included with future events.
void addBreadcrumb ( const std::string& message );

// Capture a plain message event. When async, the upload is handed to a background worker thread so
// the caller never blocks (use this for mid-match captures in the hook.dll).
void captureMessage ( Level level, const std::string& message, bool async = false );

// Capture an exception-style event (shows in Sentry with a distinct type + value).
void captureException ( const std::string& type, const std::string& value, bool async = false );

// Install process-wide crash handlers: a vectored exception handler (catches faults even when a
// downstream __except — e.g. the game's or D3D's — would otherwise swallow them), the SEH
// unhandled-exception filter (access violations, etc. that actually terminate the process),
// std::terminate (unhandled C++ exceptions) and SIGABRT (abort()/failed ASSERT). Previous handlers
// are chained so the host's normal crash behaviour is preserved. Reports the faulting context +
// a best-effort backtrace.
void installCrashHandler();

// Re-assert the unhandled-exception filter. For the injected hook.dll: MBAA.exe / the D3D runtime
// can install their own filter during startup and bump ours off the top. Call this once the game
// is fully loaded. Safe to call repeatedly; preserves correct chaining.
void reassertCrashHandler();

} // namespace SentryClient
