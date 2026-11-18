#include "HostCode.hpp"
#include "Logger.hpp"
#include "EventManager.hpp"
#include "SocketManager.hpp"
#include "TimerManager.hpp"
#include "Exceptions.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <fstream>
#include <cstring>
#include <sstream>
#include <cctype>
#include <algorithm>

using namespace std;


namespace HostCode
{

static const string Prefix = "cccaster:";

static string joinedCodeStr, siteTokenStr;

void setSiteToken ( const string& token ) { siteTokenStr = token; }
const string& siteToken() { return siteTokenStr; }

void registerLinks ( const string& exePath )
{
    const string cmd = "\"" + exePath + "\" \"%1\"";
    HKEY key = 0;
    char cur[4096] = { 0 };
    DWORD len = sizeof ( cur ) - 1;
    if ( RegOpenKeyExA ( HKEY_CURRENT_USER, "Software\\Classes\\cccaster\\shell\\open\\command", 0, KEY_READ, &key ) == ERROR_SUCCESS )
    {
        const bool same = RegQueryValueExA ( key, 0, 0, 0, ( BYTE * ) cur, &len ) == ERROR_SUCCESS && cmd == cur;
        RegCloseKey ( key );
        if ( same )
            return;
    }
    const auto set = [] ( const char *path, const char *name, const string& value ) {
        HKEY k = 0;
        if ( RegCreateKeyExA ( HKEY_CURRENT_USER, path, 0, 0, 0, KEY_WRITE, 0, &k, 0 ) != ERROR_SUCCESS )
            return false;
        const bool ok = RegSetValueExA ( k, name, 0, REG_SZ, ( const BYTE * ) value.c_str(), value.size() + 1 ) == ERROR_SUCCESS;
        RegCloseKey ( k );
        return ok;
    };
    const bool ok = set ( "Software\\Classes\\cccaster", 0, "URL:cccaster Protocol" )
                    && set ( "Software\\Classes\\cccaster", "URL Protocol", "" )
                    && set ( "Software\\Classes\\cccaster\\shell\\open\\command", 0, cmd );
    LOG ( "cccaster: links %s", ok ? "registered to this exe" : "could not be registered" );
}

Link parseLink ( const string& text, string& value )
{
    string s = trimmed ( text );
    while ( ! s.empty() && s.back() == '/' )
        s.pop_back();
    value.clear();

    const auto after = [&] ( const char *head ) -> const char * {
        const size_t n = strlen ( head );
        return s.compare ( 0, n, head ) == 0 ? s.c_str() + n : 0;
    };

    const char *rest;
    if ( ( rest = after ( "join/" ) ) || ( rest = after ( "watch/" ) ) || ( rest = after ( "spectate/" ) ) )
    {
        value = normalize ( rest );
        if ( value.empty() || strlen ( rest ) != value.size() )
        {
            value.clear();
            return Link::None;
        }
        return s[0] == 'j' ? Link::Join : Link::Spectate;
    }
    if ( ( rest = after ( "host/" ) ) )
    {
        string tok = rest;
        if ( tok.size() != 32 || tok.find_first_not_of ( "0123456789abcdef" ) != string::npos )
            return Link::None;
        value = tok;
        return Link::Host;
    }
    return Link::None;
}

// Letters without I and O; digits without 0 and 1 (nothing that reads as another symbol)
static bool isCodeLetter ( char c ) { return c >= 'A' && c <= 'Z' && c != 'I' && c != 'O'; }
static bool isCodeDigit ( char c ) { return c >= '2' && c <= '9'; }

string normalize ( const string& text )
{
    string s = trimmed ( text );

    if ( s.size() > Prefix.size() && upperCase ( s.substr ( 0, Prefix.size() ) ) == upperCase ( Prefix ) )
        s = s.substr ( Prefix.size() );

    if ( s.size() != 4 )
        return "";

    s = upperCase ( s );

    if ( ! isCodeLetter ( s[0] ) )
        return "";

    for ( size_t i = 1; i < 4; ++i )
        if ( ! isCodeLetter ( s[i] ) && ! isCodeDigit ( s[i] ) )
            return "";

    return s;
}

string maskAddress ( const string& addrPort, const string& code )
{
    const string label = "<" + ( code.empty() ? string ( "code" ) : code ) + ">";
    const size_t i = addrPort.find_last_of ( ':' );
    const size_t b = addrPort.find ( ']' );

    // "ip:port" or "[v6]:port" keeps the port; anything else is replaced whole
    if ( i != string::npos && ( b == string::npos || b < i ) && addrPort.find ( ':' ) == i )
        return label + addrPort.substr ( i );
    if ( b != string::npos && i == b + 1 )
        return label + addrPort.substr ( i );
    return label;
}

// Read a JSON string value starting at the opening quote; returns the position after the closing quote
static size_t readJsonString ( const string& json, size_t i, string& out )
{
    out.clear();
    for ( ++i; i < json.size(); ++i )
    {
        if ( json[i] == '"' )
            return i + 1;
        if ( json[i] == '\\' && i + 1 < json.size() )
        {
            ++i;
            switch ( json[i] )
            {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'u': i += 4; out += '?'; break;
                default: out += json[i]; break;
            }
            continue;
        }
        out += json[i];
    }
    return string::npos;
}

bool parseReply ( const string& json, Reply& reply )
{
    reply = Reply();

    size_t i = json.find ( '{' );
    if ( i == string::npos )
        return false;

    for ( ++i; i < json.size(); )
    {
        while ( i < json.size() && ( isspace ( ( unsigned char ) json[i] ) || json[i] == ',' ) )
            ++i;
        if ( i >= json.size() || json[i] == '}' )
            return true;
        if ( json[i] != '"' )
            return false;

        string key, str;
        i = readJsonString ( json, i, key );
        if ( i == string::npos )
            return false;
        while ( i < json.size() && ( isspace ( ( unsigned char ) json[i] ) || json[i] == ':' ) )
            ++i;
        if ( i >= json.size() )
            return false;

        if ( json[i] == '"' )
        {
            i = readJsonString ( json, i, str );
            if ( i == string::npos )
                return false;
        }
        else
        {
            const size_t j = json.find_first_of ( ",}", i );
            if ( j == string::npos )
                return false;
            str = trimmed ( json.substr ( i, j - i ) );
            i = j;
        }

        if ( key == "code" ) reply.code = str;
        else if ( key == "secret" ) reply.secret = str;
        else if ( key == "proto" ) reply.proto = str;
        else if ( key == "addr" ) reply.addr = str;
        else if ( key == "error" ) reply.error = str;
        else if ( key == "ttl" ) reply.ttl = atoi ( str.c_str() );
        else if ( key == "retryAfterMs" ) reply.retryAfterMs = ( uint32_t ) strtoul ( str.c_str(), 0, 10 );
    }

    return false;
}

string registryUrlFrom ( const string& fileContents )
{
    stringstream ss ( fileContents );
    string line;
    while ( getline ( ss, line ) )
    {
        line = trimmed ( line );
        if ( line.empty() || line[0] == '#' )
            continue;
        // (plain http:// only: CCCaster's HTTP client has no TLS, so an https:// URL is asked over http://)
        if ( line.compare ( 0, 8, "https://" ) == 0 )
            line = "http://" + line.substr ( 8 );
        else if ( line.compare ( 0, 7, "http://" ) != 0 )
            line = "http://" + line;
        return line;
    }
    return HOST_CODE_DEFAULT_URL;
}

string registryUrl()
{
    ifstream in ( CODE_LIST );
    if ( ! in.good() )
        return HOST_CODE_DEFAULT_URL;
    stringstream ss;
    ss << in.rdbuf();
    return registryUrlFrom ( ss.str() );
}

string urlEncode ( const string& str )
{
    static const char *hex = "0123456789ABCDEF";
    string out;
    for ( unsigned char c : str )
    {
        if ( isalnum ( c ) || c == '-' || c == '_' || c == '.' || c == '~' )
            out += ( char ) c;
        else
            out += { '%', hex[c >> 4], hex[c & 15] };
    }
    return out;
}

static string withQuery ( const string& url, const string& query )
{
    return url + ( url.find ( '?' ) == string::npos ? "?" : "&" ) + query;
}

const string& joinedCode() { return joinedCodeStr; }

void clearJoinedCode() { joinedCodeStr.clear(); }


// Blocking resolve: one HttpGet on a private event loop (the same pattern MainUi uses for the update check)
namespace
{
struct Resolver : public HttpGet::Owner, public Timer::Owner
{
    bool done = false, ok = false;
    int status = -1;
    string data;
    TimerPtr guard;

    void finish() { if ( ! done ) { done = true; guard.reset(); EventManager::get().stop(); } }

    void httpResponse ( HttpGet *, int code, const string& d, uint32_t remainingBytes ) override
    {
        if ( remainingBytes )
            return;
        status = code;
        data = d;
        ok = true;
        finish();
    }
    void httpFailed ( HttpGet * ) override { finish(); }
    void httpProgress ( HttpGet *, uint32_t, uint32_t ) override {}
    void timerExpired ( Timer * ) override { finish(); }
};
}

bool resolve ( const string& codeText, IpAddrPort& address, string& error )
{
    const string code = normalize ( codeText );
    joinedCodeStr.clear();

    if ( code.empty() )
    {
        error = "Not a code";
        return false;
    }

    LOG ( "Looking up code %s", code );

    Resolver r;
    {
        TimerManager::get().initialize();
        SocketManager::get().initialize();

        shared_ptr<HttpGet> get;
        try
        {
            get.reset ( new HttpGet ( &r, withQuery ( registryUrl(), "action=resolve&code=" + urlEncode ( code ) ),
                                      HOST_CODE_HTTP_TIMEOUT ) );
            get->quiet = true; // the reply holds the host's address
            r.guard.reset ( new Timer ( &r ) );
            r.guard->start ( HOST_CODE_HTTP_TIMEOUT * 2 );
            get->start();
            if ( ! r.done )
                EventManager::get().start();
        }
        catch ( const Exception& exc )
        {
            LOG ( "Host code resolve failed: %s", exc.user );
        }
        get.reset();
        r.guard.reset();
    }

    Reply reply;
    const bool parsed = r.ok && parseReply ( r.data, reply );
    r.data.clear();   // (it held the address)

    if ( ! parsed )
    {
        error = "Can't reach the code server (codes are down; <ip>:<port> still works)";
        LOG ( "Code %s: registry unreachable (status=%d)", code, r.status );
        return false;
    }

    if ( r.status == 429 || reply.error == "throttled" )
    {
        const unsigned secs = ( reply.retryAfterMs + 999 ) / 1000;
        error = format ( "Too many code lookups, try again in %u second%s", secs, ( secs == 1 ? "" : "s" ) );
        LOG ( "Code %s: throttled", code );
        return false;
    }

    if ( r.status != 200 || reply.proto != "cc" || reply.addr.empty() )
    {
        error = format ( "No one is hosting with code %s", code );
        LOG ( "Code %s: not found (status=%d)", code, r.status );
        return false;
    }

    IpAddrPort resolved;
    try
    {
        resolved = reply.addr;
    }
    catch ( ... )
    {
        resolved.clear();
    }
    reply.addr.clear();

    if ( resolved.addr.empty() || resolved.port == 0 )
    {
        error = format ( "No one is hosting with code %s", code );
        LOG ( "Code %s: bad address in reply", code );
        return false;
    }

    // From here on the address never reaches the log or the UI
    Logger::get().redact ( resolved.addr, "<" + code + ">" );
    address = resolved;
    joinedCodeStr = code;

    LOG ( "Code %s -> %s", code, maskAddress ( address.str(), code ) );
    return true;
}

// Best-effort blocking GET for the 'end' call: the host is stopping, its event loop may already be gone
static void blockingGet ( const string& url, uint32_t timeoutMs )
{
    if ( url.compare ( 0, 7, "http://" ) != 0 )
        return;

    string rest = url.substr ( 7 );
    const size_t slash = rest.find ( '/' );
    const string hostPort = rest.substr ( 0, slash );
    const string path = ( slash == string::npos ? "/" : rest.substr ( slash ) );

    string host = hostPort, port = "80";
    const size_t colon = hostPort.find_last_of ( ':' );
    if ( colon != string::npos && hostPort.find ( ']' ) == string::npos )
    {
        host = hostPort.substr ( 0, colon );
        port = hostPort.substr ( colon + 1 );
    }

    addrinfo hints = {}, *res = 0;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if ( getaddrinfo ( host.c_str(), port.c_str(), &hints, &res ) != 0 || ! res )
        return;

    SOCKET s = socket ( res->ai_family, res->ai_socktype, res->ai_protocol );
    if ( s == INVALID_SOCKET )
    {
        freeaddrinfo ( res );
        return;
    }

    u_long nonBlocking = 1;
    ioctlsocket ( s, FIONBIO, &nonBlocking );
    connect ( s, res->ai_addr, ( int ) res->ai_addrlen );
    freeaddrinfo ( res );

    fd_set wfds;
    FD_ZERO ( &wfds );
    FD_SET ( s, &wfds );
    timeval tv = { ( long ) ( timeoutMs / 1000 ), ( long ) ( ( timeoutMs % 1000 ) * 1000 ) };

    if ( select ( 0, 0, &wfds, 0, &tv ) == 1 )
    {
        nonBlocking = 0;
        ioctlsocket ( s, FIONBIO, &nonBlocking );
        const string req = "GET " + path + " HTTP/1.1\r\nHost: " + hostPort + "\r\nConnection: close\r\n\r\n";
        send ( s, req.c_str(), ( int ) req.size(), 0 );

        // wait briefly for the reply so the request is processed before the socket closes
        fd_set rfds;
        FD_ZERO ( &rfds );
        FD_SET ( s, &rfds );
        timeval tv2 = { ( long ) ( timeoutMs / 1000 ), ( long ) ( ( timeoutMs % 1000 ) * 1000 ) };
        if ( select ( 0, &rfds, 0, 0, &tv2 ) == 1 )
        {
            char buf[256];
            recv ( s, buf, sizeof ( buf ), 0 );
        }
    }

    closesocket ( s );
}

} // namespace HostCode


void HostCodeKeeper::start ( uint16_t port )
{
    _url = HostCode::registryUrl();
    _port = port;
    _code.clear();
    _secret.clear();
    _stopped = false;
    _down = false;
    _failures = 0;

    if ( ! _timer )
        _timer.reset ( new Timer ( this ) );

    create();
}

void HostCodeKeeper::create()
{
    LOG ( "Registering a code (port %u)", _port );
    // (a website's token goes with every create: the registry names the code once, a later create stays unnamed)
    request ( format ( "action=host&proto=cc&port=%u", _port )
              + ( HostCode::siteToken().empty() ? "" : "&tok=" + HostCode::urlEncode ( HostCode::siteToken() ) ) );
}

void HostCodeKeeper::request ( const string& query )
{
    _httpGet.reset();
    try
    {
        _httpGet.reset ( new HttpGet ( this, HostCode::withQuery ( _url, query ), HOST_CODE_HTTP_TIMEOUT ) );
        _httpGet->quiet = true; // the request and the reply hold the keep-alive secret
        _httpGet->start();
    }
    catch ( const Exception& exc )
    {
        _httpGet.reset();
        failed ( "request failed" );
    }
}

void HostCodeKeeper::retryLater ( uint32_t ms )
{
    if ( _timer )
        _timer->start ( ms );
}

// The code is gone (or never came): tell the owner, then keep asking for a new one in the background
void HostCodeKeeper::lose()
{
    const bool hadCode = ! _code.empty();
    const bool wasDown = _down;

    _code.clear();
    _secret.clear();
    _down = true;

    if ( owner && ( hadCode || ! wasDown ) )
        owner->hostCodeLost ( this );
}

void HostCodeKeeper::failed ( const char *why )
{
    if ( _stopped )
        return;

    ++_failures;
    const uint32_t backoff = ( uint32_t ) min<uint64_t> ( ( uint64_t ) HOST_CODE_RETRY_MS << min<uint32_t> ( _failures - 1, 8 ),
                                                          HOST_CODE_MAX_RETRY_MS );

    if ( ! _code.empty() )
    {
        // A keep-alive failed. While the code can still be alive on the registry, keep it and retry soon; once the
        // code's lifetime has passed without a keep-alive, the registry has dropped it: don't show a dead code.
        const uint64_t since = TimerManager::get().getNow ( true ) - _lastOk;
        if ( since + HOST_CODE_RETRY_MS < _ttlMs )
        {
            LOG ( "Code keep-alive failed (%s), retrying", why );
            retryLater ( HOST_CODE_RETRY_MS );
            return;
        }
        LOG ( "Code keep-alive failed (%s) for longer than the code lives; getting a new code", why );
        lose();
        retryLater ( HOST_CODE_RETRY_MS );
        return;
    }

    LOG ( "No code (%s), retrying in %u ms", why, backoff );
    lose();
    retryLater ( backoff );
}

void HostCodeKeeper::httpResponse ( HttpGet *httpGet, int status, const string& data, uint32_t remainingBytes )
{
    if ( remainingBytes || _stopped )
        return;

    // Parse before dropping the HttpGet: data belongs to it
    HostCode::Reply reply;
    const bool parsed = HostCode::parseReply ( data, reply );

    _httpGet.reset();

    if ( status == 200 && parsed && HostCode::isCode ( reply.code ) )
    {
        const string code = HostCode::normalize ( reply.code );
        const bool isNew = ( code != _code );

        if ( ! reply.secret.empty() )
            _secret = reply.secret;
        if ( reply.ttl > 0 )
            _ttlMs = ( uint32_t ) reply.ttl * 1000;

        _code = code;
        _down = false;
        _failures = 0;
        _lastOk = TimerManager::get().getNow ( true );

        retryLater ( min<uint32_t> ( HOST_CODE_KEEPALIVE_MS, _ttlMs / 3 ) );

        if ( isNew )
        {
            LOG ( "Code: %s", _code );
            if ( owner )
                owner->hostCodeReady ( this, _code );
        }
        return;
    }

    LOG ( "Code registry answered %d '%s'", status, reply.error );

    if ( ! _code.empty() && status == 403 )
    {
        // The registry forgot our code (it restarted, or our keep-alives were lost for too long): get a new one,
        // and never show the dead code meanwhile
        LOG ( "Code %s is gone from the registry; getting a new code", _code );
        lose();
        _failures = 0;
        retryLater ( HOST_CODE_RETRY_MS );
        return;
    }

    failed ( parsed ? "refused" : "bad reply" );
}

void HostCodeKeeper::httpFailed ( HttpGet *httpGet )
{
    _httpGet.reset();
    failed ( "unreachable" );
}

void HostCodeKeeper::timerExpired ( Timer *timer )
{
    if ( _stopped )
        return;

    if ( _code.empty() )
    {
        create();
        return;
    }

    request ( format ( "action=host&proto=cc&code=%s&secret=%s&st=%s",
                       HostCode::urlEncode ( _code ), HostCode::urlEncode ( _secret ), _inMatch ? "m" : "w" ) );
}

void HostCodeKeeper::stop()
{
    if ( _stopped )
        return;
    _stopped = true;

    _timer.reset();
    _httpGet.reset();

    if ( ! _code.empty() && ! _secret.empty() )
    {
        LOG ( "Ending code %s", _code );
        HostCode::blockingGet ( HostCode::withQuery ( _url, "action=end&code=" + HostCode::urlEncode ( _code )
                                + "&secret=" + HostCode::urlEncode ( _secret ) ), 500 );
    }

    _code.clear();
    _secret.clear();
    _down = false;
}
