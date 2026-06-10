#include "SentryClient.hpp"

#include <cereal/external/rapidjson/stringbuffer.h>
#include <cereal/external/rapidjson/writer.h>

#include <windows.h>
#include <wininet.h>
#include <dbghelp.h>

#include <string>
#include <vector>
#include <utility>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <exception>

using namespace std;


// SDK identity reported to Sentry / Glitchtip.
#define SENTRY_CLIENT_NAME      "cccaster.sentry"
#define SENTRY_CLIENT_VERSION   "1.0"

// Max breadcrumbs retained for context, and max captured stack frames per crash.
#define MAX_BREADCRUMBS         ( 30 )
#define MAX_STACK_FRAMES        ( 48 )

// WinINet timeouts (ms) so a dead endpoint never stalls a crash path for long.
#define HTTP_TIMEOUT_MS         ( 5000 )


namespace
{

// JSON is built with the rapidjson Writer that cereal already vendors (the same library the netplay
// protocol's JSON archive uses). The Writer handles all string escaping and framing for us, so the
// event payload is always well-formed regardless of what ends up in an exception/log message.
typedef rapidjson::Writer<rapidjson::StringBuffer> JsonWriter;


struct State
{
    bool enabled = false;

    // Parsed DSN.
    bool https = true;
    string host;
    INTERNET_PORT port = 443;
    string path;            // /api/<projectId>/envelope/
    string dsn;             // original DSN, echoed in the envelope header
    string authHeader;      // value of X-Sentry-Auth

    // Event metadata.
    string release;
    string environment;
    string dist;

    // Guards breadcrumbs, tags and the async queue.
    CRITICAL_SECTION cs;
    bool csReady = false;

    vector<pair<string, string>> tags;
    vector<pair<string, string>> breadcrumbs;   // (timestamp, message)

    // Async upload worker.
    HANDLE workerThread = 0;
    HANDLE wakeEvent = 0;
    vector<string> queue;           // ready-to-POST envelope bodies

    // Diagnostics sink (e.g. routed to the app's logger).
    SentryClient::LogSink logSink = 0;

    // DbgHelp symbol handler initialized (for StackWalk64 + symbol names).
    bool symReady = false;

    // Chained crash handlers + dedupe/reentrancy guards.
    PVOID vehHandle = 0;
    LPTOP_LEVEL_EXCEPTION_FILTER prevFilter = 0;
    terminate_handler prevTerminate = 0;
    void ( *prevSigabrt ) ( int ) = 0;
    volatile LONG vehReported = 0;  // VEH reports a fatal first-chance fault at most once
    volatile LONG inFilter = 0;     // reentrancy guard for the unhandled filter
};

State g;


// Emit an internal diagnostic line. Routed to the registered sink (the app logger) and always to
// OutputDebugString, so a crash reporter that goes quiet can still be debugged from the logs.
void diag ( const string& msg )
{
    if ( g.logSink )
        g.logSink ( msg.c_str() );

    OutputDebugStringA ( ( "[sentry] " + msg + "\n" ).c_str() );
}


// ----- small helpers -------------------------------------------------------

string isoTimestamp()
{
    time_t t = time ( 0 );
    char buf[32] = { 0 };
    strftime ( buf, sizeof ( buf ), "%Y-%m-%dT%H:%M:%SZ", gmtime ( &t ) );
    return buf;
}

string randomEventId()
{
    static const char *hex = "0123456789abcdef";
    char id[33];
    for ( int i = 0; i < 32; ++i )
        id[i] = hex[rand() % 16];
    id[32] = 0;
    return id;
}

string hexPtr ( const void *p )
{
    char buf[24];
    snprintf ( buf, sizeof ( buf ), "0x%08x", ( unsigned ) ( uintptr_t ) p );
    return buf;
}

string hexCode ( unsigned code )
{
    char buf[16];
    snprintf ( buf, sizeof ( buf ), "0x%08x", code );
    return buf;
}

const char *levelStr ( SentryClient::Level level )
{
    switch ( level )
    {
        case SentryClient::Level::Debug:
            return "debug";
        case SentryClient::Level::Info:
            return "info";
        case SentryClient::Level::Warning:
            return "warning";
        case SentryClient::Level::Error:
            return "error";
        case SentryClient::Level::Fatal:
            return "fatal";
    }
    return "error";
}

// Crash-class exception codes worth reporting from the first-chance vectored handler. Excludes C++
// exceptions (0xE06D7363) and debugger/control codes so normal handled exceptions aren't reported.
bool isFatalCode ( DWORD code )
{
    switch ( code )
    {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_STACK_OVERFLOW:
        case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_PRIV_INSTRUCTION:
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case EXCEPTION_IN_PAGE_ERROR:
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
            return true;
        default:
            return false;
    }
}

// Write a "key": "value" string member onto the current JSON object.
void writeMember ( JsonWriter& w, const char *key, const string& value )
{
    w.String ( key );
    w.String ( value.c_str(), ( rapidjson::SizeType ) value.size() );
}

string baseName ( const string& path )
{
    const size_t i = path.find_last_of ( "\\/" );
    return ( i == string::npos ) ? path : path.substr ( i + 1 );
}

// Resolve the module owning an address; returns false if unknown.
bool moduleForAddr ( const void *addr, HMODULE& mod, string& path )
{
    mod = 0;
    if ( ! GetModuleHandleExA ( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                ( LPCSTR ) addr, &mod ) )
        return false;

    char name[MAX_PATH] = { 0 };
    if ( GetModuleFileNameA ( mod, name, sizeof ( name ) ) )
        path = name;
    return true;
}

// True if the address belongs to a module under the Windows directory (a system DLL). Used to
// ignore benign first-chance exceptions that Windows DLLs (dxdiag/setupapi/...) raise as control
// flow, so the vectored handler only reports faults in the game / our own code.
bool addrInSystemModule ( const void *addr )
{
    HMODULE mod = 0;
    string path;
    if ( ! moduleForAddr ( addr, mod, path ) || path.empty() )
        return false;

    char winDir[MAX_PATH] = { 0 };
    const UINT n = GetWindowsDirectoryA ( winDir, sizeof ( winDir ) );
    if ( n == 0 || n >= sizeof ( winDir ) )
        return false;

    return _strnicmp ( path.c_str(), winDir, n ) == 0;
}


// ----- HTTP ----------------------------------------------------------------

// Synchronous fire-and-forget POST of an already-framed envelope body.
void httpPost ( const string& body )
{
    HINTERNET hInet = InternetOpenA ( SENTRY_CLIENT_NAME "/" SENTRY_CLIENT_VERSION,
                                      INTERNET_OPEN_TYPE_PRECONFIG, 0, 0, 0 );
    if ( ! hInet )
    {
        diag ( "InternetOpen failed err=" + hexCode ( GetLastError() ) );
        return;
    }

    HINTERNET hConn = InternetConnectA ( hInet, g.host.c_str(), g.port, 0, 0, INTERNET_SERVICE_HTTP, 0, 0 );
    if ( ! hConn )
    {
        diag ( "InternetConnect failed err=" + hexCode ( GetLastError() ) );
        InternetCloseHandle ( hInet );
        return;
    }

    DWORD flags = INTERNET_FLAG_NO_UI | INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE;
    if ( g.https )
    {
        // Be lenient on certs: self-hosted Glitchtip instances often use private/relaxed certs,
        // and a crash report is best-effort anyway.
        flags |= INTERNET_FLAG_SECURE
                 | INTERNET_FLAG_IGNORE_CERT_CN_INVALID
                 | INTERNET_FLAG_IGNORE_CERT_DATE_INVALID;
    }

    HINTERNET hReq = HttpOpenRequestA ( hConn, "POST", g.path.c_str(), 0, 0, 0, flags, 0 );
    if ( ! hReq )
    {
        diag ( "HttpOpenRequest failed err=" + hexCode ( GetLastError() ) );
        InternetCloseHandle ( hConn );
        InternetCloseHandle ( hInet );
        return;
    }

    DWORD timeout = HTTP_TIMEOUT_MS;
    InternetSetOption ( hReq, INTERNET_OPTION_CONNECT_TIMEOUT, &timeout, sizeof ( timeout ) );
    InternetSetOption ( hReq, INTERNET_OPTION_SEND_TIMEOUT, &timeout, sizeof ( timeout ) );
    InternetSetOption ( hReq, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof ( timeout ) );

    const string headers = "Content-Type: application/x-sentry-envelope\r\nX-Sentry-Auth: " + g.authHeader + "\r\n";

    const BOOL ok = HttpSendRequestA ( hReq, headers.c_str(), ( DWORD ) headers.size(),
                                       ( LPVOID ) body.data(), ( DWORD ) body.size() );

    if ( ! ok )
    {
        diag ( "HttpSendRequest failed err=" + hexCode ( GetLastError() ) );
    }
    else
    {
        DWORD status = 0, len = sizeof ( status );
        HttpQueryInfoA ( hReq, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &len, 0 );
        char buf[16];
        snprintf ( buf, sizeof ( buf ), "%u", ( unsigned ) status );
        diag ( "POST " + g.host + g.path + " -> HTTP " + buf );
    }

    InternetCloseHandle ( hReq );
    InternetCloseHandle ( hConn );
    InternetCloseHandle ( hInet );
}

DWORD WINAPI workerMain ( LPVOID )
{
    for ( ;; )
    {
        WaitForSingleObject ( g.wakeEvent, INFINITE );

        for ( ;; )
        {
            string body;

            EnterCriticalSection ( &g.cs );
            if ( g.queue.empty() )
            {
                LeaveCriticalSection ( &g.cs );
                break;
            }
            body = g.queue.front();
            g.queue.erase ( g.queue.begin() );
            LeaveCriticalSection ( &g.cs );

            httpPost ( body );
        }
    }
    return 0;
}

void enqueue ( const string& body )
{
    EnterCriticalSection ( &g.cs );

    if ( ! g.workerThread )
    {
        g.wakeEvent = CreateEvent ( 0, FALSE, FALSE, 0 );
        g.workerThread = CreateThread ( 0, 0, workerMain, 0, 0, 0 );
    }

    g.queue.push_back ( body );
    LeaveCriticalSection ( &g.cs );

    if ( g.wakeEvent )
        SetEvent ( g.wakeEvent );
}


// ----- event / envelope building -------------------------------------------

// Collect the real faulting call chain. When we have the fault CONTEXT we walk it with StackWalk64
// (the actual call stack, not the exception-dispatch path that RtlCaptureStackBackTrace returns
// from inside a filter). Otherwise (terminate/SIGABRT) we fall back to the current call stack.
int captureFrames ( EXCEPTION_POINTERS *info, void *addrs[MAX_STACK_FRAMES] )
{
    int count = 0;

    if ( info && info->ContextRecord )
    {
        CONTEXT ctx = *info->ContextRecord;     // StackWalk64 mutates the context, so copy it
        STACKFRAME64 sf = { 0 };
        sf.AddrPC.Offset = ctx.Eip;
        sf.AddrPC.Mode = AddrModeFlat;
        sf.AddrFrame.Offset = ctx.Ebp;
        sf.AddrFrame.Mode = AddrModeFlat;
        sf.AddrStack.Offset = ctx.Esp;
        sf.AddrStack.Mode = AddrModeFlat;

        const HANDLE proc = GetCurrentProcess();
        const HANDLE thread = GetCurrentThread();

        while ( count < MAX_STACK_FRAMES
                && StackWalk64 ( IMAGE_FILE_MACHINE_I386, proc, thread, &sf, &ctx,
                                 0, SymFunctionTableAccess64, SymGetModuleBase64, 0 ) )
        {
            if ( sf.AddrPC.Offset == 0 )
                break;
            addrs[count++] = ( void * ) ( uintptr_t ) sf.AddrPC.Offset;
        }
    }

    if ( count == 0 )
    {
        if ( info && info->ExceptionRecord )
            addrs[count++] = info->ExceptionRecord->ExceptionAddress;
        count += CaptureStackBackTrace ( 0, MAX_STACK_FRAMES - count, &addrs[count], 0 );
    }

    return count;
}

// Write a "stacktrace" member (frames oldest-first) onto the current exception-value object. Each
// frame carries the absolute address, owning module + base (so a module-relative offset can be
// derived) and, when DbgHelp can resolve it, a symbol name. For MinGW DWARF binaries DbgHelp only
// resolves exported names; the authoritative source location comes from running addr2line on the
// unstripped binary with (instruction_addr - image_addr).
void writeStacktrace ( JsonWriter& w, EXCEPTION_POINTERS *info )
{
    void *addrs[MAX_STACK_FRAMES] = { 0 };
    const int count = captureFrames ( info, addrs );

    if ( count == 0 )
        return;

    const HANDLE proc = GetCurrentProcess();

    // Symbol buffer for SymFromAddr (struct + room for the name).
    char symBuf[sizeof ( SYMBOL_INFO ) + 256] = { 0 };
    SYMBOL_INFO *sym = ( SYMBOL_INFO * ) symBuf;
    sym->SizeOfStruct = sizeof ( SYMBOL_INFO );
    sym->MaxNameLen = 255;

    w.String ( "stacktrace" );
    w.StartObject();
    w.String ( "frames" );
    w.StartArray();

    // Sentry expects frames oldest-first, so emit in reverse of the captured (newest-first) order.
    for ( int i = count - 1; i >= 0; --i )
    {
        if ( ! addrs[i] )
            continue;

        HMODULE mod = 0;
        string path;
        const bool haveMod = moduleForAddr ( addrs[i], mod, path );

        // Function name: prefer a DbgHelp symbol, else fall back to module!+0xRVA so the frame is
        // still human-readable and the RVA is right there for addr2line.
        string function;
        DWORD64 disp = 0;
        if ( g.symReady && SymFromAddr ( proc, ( DWORD64 ) ( uintptr_t ) addrs[i], &disp, sym ) )
        {
            char d[16];
            snprintf ( d, sizeof ( d ), "+0x%llx", ( unsigned long long ) disp );
            function = string ( sym->Name ) + d;
        }
        else if ( haveMod )
        {
            const uintptr_t rva = ( uintptr_t ) addrs[i] - ( uintptr_t ) mod;
            char r[24];
            snprintf ( r, sizeof ( r ), "!0x%x", ( unsigned ) rva );
            function = baseName ( path ) + r;
        }

        w.StartObject();
        if ( ! function.empty() )
            writeMember ( w, "function", function );
        writeMember ( w, "instruction_addr", hexPtr ( addrs[i] ) );
        if ( haveMod )
        {
            writeMember ( w, "package", path );
            writeMember ( w, "image_addr", hexPtr ( ( void * ) mod ) );
        }
        w.EndObject();
    }

    w.EndArray();
    w.EndObject();
}

// Build the Sentry event payload. type empty => a message event; type non-empty => an exception
// event (optionally carrying a stacktrace from info). The generated event_id is returned via param.
string buildEvent ( SentryClient::Level level, const string& type, const string& value,
                    EXCEPTION_POINTERS *info, bool includeStack, string& eventId )
{
    eventId = randomEventId();

    rapidjson::StringBuffer sb;
    JsonWriter w ( sb );

    w.StartObject();

    writeMember ( w, "event_id", eventId );
    writeMember ( w, "timestamp", isoTimestamp() );
    w.String ( "platform" );
    w.String ( "native" );
    writeMember ( w, "level", levelStr ( level ) );
    w.String ( "logger" );
    w.String ( "cccaster" );

    w.String ( "sdk" );
    w.StartObject();
    w.String ( "name" );
    w.String ( SENTRY_CLIENT_NAME );
    w.String ( "version" );
    w.String ( SENTRY_CLIENT_VERSION );
    w.EndObject();

    if ( ! g.release.empty() )
        writeMember ( w, "release", g.release );
    if ( ! g.environment.empty() )
        writeMember ( w, "environment", g.environment );
    if ( ! g.dist.empty() )
        writeMember ( w, "dist", g.dist );

    EnterCriticalSection ( &g.cs );

    if ( ! g.tags.empty() )
    {
        w.String ( "tags" );
        w.StartObject();
        for ( const auto& t : g.tags )
            writeMember ( w, t.first.c_str(), t.second );
        w.EndObject();
    }

    if ( ! g.breadcrumbs.empty() )
    {
        w.String ( "breadcrumbs" );
        w.StartObject();
        w.String ( "values" );
        w.StartArray();
        for ( const auto& b : g.breadcrumbs )
        {
            w.StartObject();
            writeMember ( w, "timestamp", b.first );
            writeMember ( w, "message", b.second );
            w.EndObject();
        }
        w.EndArray();
        w.EndObject();
    }

    LeaveCriticalSection ( &g.cs );

    if ( type.empty() )
    {
        w.String ( "message" );
        w.StartObject();
        writeMember ( w, "formatted", value );
        w.EndObject();
    }
    else
    {
        w.String ( "exception" );
        w.StartObject();
        w.String ( "values" );
        w.StartArray();
        w.StartObject();
        writeMember ( w, "type", type );
        writeMember ( w, "value", value );
        if ( includeStack )
            writeStacktrace ( w, info );
        w.EndObject();
        w.EndArray();
        w.EndObject();
    }

    w.EndObject();

    return string ( sb.GetString(), sb.Size() );
}

// Assemble a full Sentry envelope (header line, item header line, event payload) ready to POST.
string buildEnvelope ( SentryClient::Level level, const string& type, const string& value,
                       EXCEPTION_POINTERS *info, bool includeStack )
{
    string eventId;
    const string payload = buildEvent ( level, type, value, info, includeStack, eventId );

    rapidjson::StringBuffer header;
    JsonWriter hw ( header );
    hw.StartObject();
    writeMember ( hw, "event_id", eventId );
    writeMember ( hw, "sent_at", isoTimestamp() );
    writeMember ( hw, "dsn", g.dsn );
    hw.EndObject();

    rapidjson::StringBuffer item;
    JsonWriter iw ( item );
    iw.StartObject();
    iw.String ( "type" );
    iw.String ( "event" );
    iw.String ( "length" );
    iw.Uint ( ( unsigned ) payload.size() );
    iw.EndObject();

    string envelope ( header.GetString(), header.Size() );
    envelope += "\n";
    envelope.append ( item.GetString(), item.Size() );
    envelope += "\n";
    envelope += payload;
    envelope += "\n";
    return envelope;
}

// Build a crash event and upload it. Fatal/terminating paths post synchronously (must deliver
// before the process dies); a swallowed first-chance fault posts async (process lives on, and we
// must not stall gameplay).
void captureCrash ( SentryClient::Level level, const string& type, const string& value,
                    EXCEPTION_POINTERS *info, bool async )
{
    if ( ! g.enabled )
        return;

    const string envelope = buildEnvelope ( level, type, value, info, true );

    if ( async )
        enqueue ( envelope );
    else
        httpPost ( envelope );
}


// ----- crash handlers ------------------------------------------------------

// First-chance vectored handler. Catches crash-class faults even when a downstream __except (the
// game's or D3D's) would swallow them before the unhandled filter runs. Reports at most once and
// posts asynchronously so it never blocks the game; the unhandled filter below is the reliable
// synchronous path when the fault actually terminates the process.
LONG CALLBACK vehHandler ( EXCEPTION_POINTERS *info )
{
    if ( ! g.enabled || ! info || ! info->ExceptionRecord )
        return EXCEPTION_CONTINUE_SEARCH;

    if ( ! isFatalCode ( info->ExceptionRecord->ExceptionCode ) )
        return EXCEPTION_CONTINUE_SEARCH;

    // Ignore first-chance faults inside Windows system DLLs: they're handled internally (e.g.
    // dxdiag/setupapi during device enumeration) and are not crashes in the game or our code.
    if ( addrInSystemModule ( info->ExceptionRecord->ExceptionAddress ) )
        return EXCEPTION_CONTINUE_SEARCH;

    if ( InterlockedExchange ( &g.vehReported, 1 ) )
        return EXCEPTION_CONTINUE_SEARCH;

    const string code = hexCode ( info->ExceptionRecord->ExceptionCode );
    const string value = "First-chance fatal exception " + code
                         + " at " + hexPtr ( info->ExceptionRecord->ExceptionAddress );

    diag ( "VEH caught " + value );
    captureCrash ( SentryClient::Level::Fatal, "VEH " + code, value, info, /* async */ true );

    return EXCEPTION_CONTINUE_SEARCH;
}

LONG WINAPI sehFilter ( EXCEPTION_POINTERS *info )
{
    if ( g.enabled && info && info->ExceptionRecord && ! InterlockedExchange ( &g.inFilter, 1 ) )
    {
        const string code = hexCode ( info->ExceptionRecord->ExceptionCode );
        const string value = "Unhandled SEH exception " + code
                             + " at " + hexPtr ( info->ExceptionRecord->ExceptionAddress );

        diag ( "unhandled filter caught " + value );
        captureCrash ( SentryClient::Level::Fatal, "SEH " + code, value, info, /* async */ false );
    }

    // Preserve the host's normal crash behaviour.
    return g.prevFilter ? g.prevFilter ( info ) : EXCEPTION_CONTINUE_SEARCH;
}

void terminateHandler()
{
    string what = "Unhandled C++ exception";

    try
    {
        exception_ptr e = current_exception();
        if ( e )
            rethrow_exception ( e );
    }
    catch ( const exception& ex )
    {
        what = string ( "Unhandled exception: " ) + ex.what();
    }
    catch ( ... )
    {
    }

    diag ( "terminate: " + what );
    captureCrash ( SentryClient::Level::Fatal, "terminate", what, 0, /* async */ false );

    if ( g.prevTerminate )
        g.prevTerminate();
    else
        abort();
}

void sigabrtHandler ( int sig )
{
    diag ( "SIGABRT caught" );
    captureCrash ( SentryClient::Level::Fatal, "SIGABRT", "abort() called (assertion failure)", 0,
                   /* async */ false );

    // Chain to whatever was installed before us so existing cleanup/exit still runs.
    if ( g.prevSigabrt && g.prevSigabrt != SIG_DFL && g.prevSigabrt != SIG_IGN )
    {
        g.prevSigabrt ( sig );
    }
    else
    {
        signal ( SIGABRT, SIG_DFL );
        raise ( SIGABRT );
    }
}

} // anonymous namespace


// ----- public API ----------------------------------------------------------

namespace SentryClient
{

void setLogSink ( LogSink sink )
{
    g.logSink = sink;
}

void init ( const string& dsn, const string& release, const string& environment, const string& dist )
{
    if ( ! g.csReady )
    {
        InitializeCriticalSection ( &g.cs );
        g.csReady = true;
    }

    g.enabled = false;

    if ( dsn.empty() )
    {
        diag ( "disabled: no DSN configured (build with -DSENTRY_DSN / SENTRY_DSN=...)" );
        return;
    }

    // Parse: <scheme>://<publicKey>@<host>[:<port>]/<projectId>
    const size_t schemeEnd = dsn.find ( "://" );
    if ( schemeEnd == string::npos )
    {
        diag ( "disabled: malformed DSN (no scheme)" );
        return;
    }

    g.https = ( dsn.compare ( 0, schemeEnd, "https" ) == 0 );

    const string rest = dsn.substr ( schemeEnd + 3 );

    const size_t at = rest.find ( '@' );
    if ( at == string::npos )
    {
        diag ( "disabled: malformed DSN (no '@')" );
        return;
    }

    const string publicKey = rest.substr ( 0, at );
    string hostPart = rest.substr ( at + 1 );

    const size_t slash = hostPart.find ( '/' );
    if ( slash == string::npos )
    {
        diag ( "disabled: malformed DSN (no project id)" );
        return;
    }

    string projectId = hostPart.substr ( slash + 1 );
    const string hostPort = hostPart.substr ( 0, slash );

    // projectId may have a trailing slash or query; keep just the leading number/segment.
    const size_t projEnd = projectId.find_first_of ( "/?" );
    if ( projEnd != string::npos )
        projectId = projectId.substr ( 0, projEnd );

    if ( publicKey.empty() || hostPort.empty() || projectId.empty() )
    {
        diag ( "disabled: malformed DSN (empty key/host/project)" );
        return;
    }

    const size_t colon = hostPort.find ( ':' );
    if ( colon != string::npos )
    {
        g.host = hostPort.substr ( 0, colon );
        g.port = ( INTERNET_PORT ) atoi ( hostPort.substr ( colon + 1 ).c_str() );
    }
    else
    {
        g.host = hostPort;
        g.port = ( g.https ? 443 : 80 );
    }

    g.path = "/api/" + projectId + "/envelope/";
    g.dsn = dsn;
    g.authHeader = "Sentry sentry_version=7, sentry_key=" + publicKey
                   + ", sentry_client=" SENTRY_CLIENT_NAME "/" SENTRY_CLIENT_VERSION;

    g.release = release;
    g.environment = environment;
    g.dist = dist;

    g.enabled = true;

    char portBuf[8];
    snprintf ( portBuf, sizeof ( portBuf ), "%u", ( unsigned ) g.port );
    diag ( "enabled: " + string ( g.https ? "https" : "http" ) + " host=" + g.host + ":" + portBuf
           + " path=" + g.path + " release=" + g.release + " env=" + g.environment );
}

bool isEnabled()
{
    return g.enabled;
}

void setTag ( const string& key, const string& value )
{
    if ( ! g.enabled )
        return;

    EnterCriticalSection ( &g.cs );
    for ( auto& t : g.tags )
    {
        if ( t.first == key )
        {
            t.second = value;
            LeaveCriticalSection ( &g.cs );
            return;
        }
    }
    g.tags.push_back ( { key, value } );
    LeaveCriticalSection ( &g.cs );
}

void addBreadcrumb ( const string& message )
{
    if ( ! g.enabled )
        return;

    EnterCriticalSection ( &g.cs );
    g.breadcrumbs.push_back ( { isoTimestamp(), message } );
    if ( g.breadcrumbs.size() > MAX_BREADCRUMBS )
        g.breadcrumbs.erase ( g.breadcrumbs.begin() );
    LeaveCriticalSection ( &g.cs );
}

void captureMessage ( Level level, const string& message, bool async )
{
    if ( ! g.enabled )
        return;

    diag ( "captureMessage: " + message );

    const string envelope = buildEnvelope ( level, "", message, 0, false );

    if ( async )
        enqueue ( envelope );
    else
        httpPost ( envelope );
}

void captureException ( const string& type, const string& value, bool async )
{
    if ( ! g.enabled )
        return;

    diag ( "captureException: " + type + ": " + value );

    const string envelope = buildEnvelope ( Level::Error, type, value, 0, false );

    if ( async )
        enqueue ( envelope );
    else
        httpPost ( envelope );
}

void installCrashHandler()
{
    if ( ! g.enabled )
        return;

    // DbgHelp: used to walk the faulting stack and resolve symbol names at crash time.
    if ( ! g.symReady )
    {
        SymSetOptions ( SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_LOAD_LINES );
        if ( SymInitialize ( GetCurrentProcess(), 0, TRUE ) )
            g.symReady = true;
    }

    g.vehHandle = AddVectoredExceptionHandler ( 1, vehHandler );
    g.prevFilter = SetUnhandledExceptionFilter ( sehFilter );
    g.prevTerminate = set_terminate ( terminateHandler );
    g.prevSigabrt = signal ( SIGABRT, sigabrtHandler );

    diag ( "crash handlers installed (veh + unhandled filter + terminate + SIGABRT)" );
}

void reassertCrashHandler()
{
    if ( ! g.enabled )
        return;

    // Reclaim the top-level filter if something replaced it; only update the chain when the current
    // top isn't already ours (else we'd chain to ourselves and recurse).
    LPTOP_LEVEL_EXCEPTION_FILTER cur = SetUnhandledExceptionFilter ( sehFilter );
    if ( cur != sehFilter )
    {
        g.prevFilter = cur;
        diag ( "reasserted unhandled filter (it had been replaced)" );
    }
}

} // namespace SentryClient
