#pragma once

// Host codes: a short code instead of an IP address.
//
// A host registers its port with a small HTTP registry and gets a code (K7QX) that lives only while it keeps hosting.
// Anyone types the code into the join or spectate box, or opens a cccaster:join/K7QX link; the registry answers the
// host's address and the normal connect path takes over unchanged (SmartSocket, tunnel fallback, old peers).
// Pasting an IP address still works exactly as before.
//
// Registry (scripts/hostcode_registry/codes_server.mjs, open source; anyone can run one):
//   GET <url>?action=host&proto=cc&port=P             -> {"code","secret","ttl"}   (the address is the request's source)
//   GET <url>?action=host&proto=cc&code=C&secret=S    -> {"code","ttl"}            keep-alive; 403 once the code is gone
//   GET <url>?action=end&code=C&secret=S              -> {"ok":true}
//   GET <url>?action=resolve&code=C                   -> {"proto":"cc","addr":"ip:port"}   one code at a time, rate-limited
// There is no request that lists codes or addresses. The registry URL is HOST_CODE_DEFAULT_URL unless CODE_LIST
// (code_list.txt, beside lobby_list.txt) names another one.
//
// The resolved address is never shown or logged: the UI shows the code and the log redacts it (Logger::redact).

#include "HttpGet.hpp"
#include "Timer.hpp"
#include "IpAddrPort.hpp"

#include <string>
#include <memory>


#ifndef CODE_LIST
#define CODE_LIST               "code_list.txt"
#endif

#define HOST_CODE_DEFAULT_URL   "http://watch.meltyblood.org/hc"
#define HOST_CODE_KEEPALIVE_MS  ( 20000 )
#define HOST_CODE_HTTP_TIMEOUT  ( 3000 )
#define HOST_CODE_RETRY_MS      ( 5000 )    // first retry after a failure; never ask for a code faster than this
#define HOST_CODE_MAX_RETRY_MS  ( 30000 )   // retries back off to this


namespace HostCode
{

// The ONE code-format validator: returns the normalized (upper-case) code, or "" if the text is not a code.
// A code is 4 characters: a letter, then 3 letters or digits, without I, O, 0 or 1 (K7QX). Case-insensitive.
// A code always starts with a letter, so it is never mistaken for a port, and has no ':' or '.', so never for an
// address. An optional "cccaster:" prefix is allowed.
std::string normalize ( const std::string& text );

inline bool isCode ( const std::string& text ) { return ! normalize ( text ).empty(); }

// "1.2.3.4:46318" -> "<K7QX>:46318" for logs (the port is not secret, the address is)
std::string maskAddress ( const std::string& addrPort, const std::string& code );

// A parsed registry reply (flat JSON object of strings and numbers)
struct Reply
{
    std::string code, secret, proto, addr, error;
    int ttl = 0;
    uint32_t retryAfterMs = 0;
};

// Parse a registry reply; false if it is not a JSON object
bool parseReply ( const std::string& json, Reply& reply );

// The registry URL from the contents of code_list.txt (first line that is not blank or '#'), or the default
std::string registryUrlFrom ( const std::string& fileContents );

// The registry URL in use (reads code_list.txt from the working directory, like lobby_list.txt)
std::string registryUrl();

// Percent-encode a query value
std::string urlEncode ( const std::string& str );

// Resolve a code to an address, blocking (runs its own event loop; call it before a session starts, never inside one).
// On success sets address, registers the address with Logger::redact and remembers the code (joinedCode).
bool resolve ( const std::string& code, IpAddrPort& address, std::string& error );

// Links (the part after "cccaster:"; a trailing '/' that browsers add is ignored):
//   join/K7QX -> Join; watch/K7QX or spectate/K7QX -> Spectate (value = the code);
//   host/<32 hex token> -> Host (value = the token, sent with the code so a website can show it).
// Anything else -> None (the stock "cccaster:<args>" path).
enum class Link { None, Join, Spectate, Host };
Link parseLink ( const std::string& text, std::string& value );

// A website's one-time token sent with this run's first code (cccaster:host/<token>)
void setSiteToken ( const std::string& token );
const std::string& siteToken();

// Register "cccaster:" links for this user (HKCU\Software\Classes\cccaster, no admin) to this exe; the caller checks
// config.ini linkHandler (1 by default, 0 turns it off). Silent; only writes when the registered command differs.
void registerLinks ( const std::string& exePath );

// The code the current session was joined with, or "" if it was joined by address
const std::string& joinedCode();
void clearJoinedCode();

} // namespace HostCode


// Keeps a host's code alive while hosting. Lives in the host's MainApp (its event loop drives the requests).
// If the registry can't be reached it keeps retrying in the background (5 s, backing off to 30 s); if the registry
// forgets the code (a restart, or keep-alives lost for longer than the code's lifetime) it gets a new one.
class HostCodeKeeper
    : private HttpGet::Owner
    , private Timer::Owner
{
public:

    struct Owner
    {
        // A code was assigned, or replaced by a new one
        virtual void hostCodeReady ( HostCodeKeeper *keeper, const std::string& code ) = 0;

        // There is no code right now (the registry is down or forgot it); the keeper is retrying
        virtual void hostCodeLost ( HostCodeKeeper *keeper ) = 0;
    };

    Owner *owner = 0;

    HostCodeKeeper ( Owner *owner ) : owner ( owner ) {}

    ~HostCodeKeeper() { stop(); }

    // Register the hosting port and keep the code alive
    void start ( uint16_t port );

    // A match started (the code then spectates): sent with the keep-alives
    void setInMatch ( bool inMatch ) { _inMatch = inMatch; }

    // End the code now (best effort, short blocking request), stop the keep-alive
    void stop();

    bool isStarted() const { return ! _stopped; }

    // The current code, or "" while there is none
    const std::string& code() const { return _code; }

    // True after a failed request while there is no code (the keeper is retrying)
    bool isDown() const { return _down; }

private:

    std::string _url, _code, _secret;

    uint16_t _port = 0;

    uint64_t _lastOk = 0;

    uint32_t _ttlMs = 60000;

    uint32_t _failures = 0;

    bool _stopped = true;

    bool _down = false;

    bool _inMatch = false;

    std::shared_ptr<HttpGet> _httpGet;

    TimerPtr _timer;

    void create();

    void request ( const std::string& query );

    void failed ( const char *why );

    void lose();

    void retryLater ( uint32_t ms );

    void httpResponse ( HttpGet *httpGet, int code, const std::string& data, uint32_t remainingBytes ) override;

    void httpFailed ( HttpGet *httpGet ) override;

    void httpProgress ( HttpGet *httpGet, uint32_t receivedBytes, uint32_t totalBytes ) override {}

    void timerExpired ( Timer *timer ) override;
};
