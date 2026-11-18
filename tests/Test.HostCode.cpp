#ifndef RELEASE

#include "Test.hpp"
#include "HostCode.hpp"
#include "IpAddrPort.hpp"
#include "Logger.hpp"
#include "Exceptions.hpp"

#include <gtest/gtest.h>

using namespace std;


// The one validator: a letter, then 3 letters or digits, no I / O / 0 / 1, any case; never a port or an address
TEST ( HostCode, Parse )
{
    EXPECT_EQ ( "K7QX", HostCode::normalize ( "K7QX" ) );
    EXPECT_EQ ( "K7QX", HostCode::normalize ( "k7qx" ) );
    EXPECT_EQ ( "K7QX", HostCode::normalize ( "  k7Qx\r\n" ) );
    EXPECT_EQ ( "K7QX", HostCode::normalize ( "cccaster:K7QX" ) );
    EXPECT_EQ ( "AAAA", HostCode::normalize ( "AAAA" ) );
    EXPECT_EQ ( "Z999", HostCode::normalize ( "Z999" ) );
    EXPECT_EQ ( "H2NP", HostCode::normalize ( "h2np" ) );

    // letter first: a 4-digit or 5-digit port is never a code
    EXPECT_EQ ( "", HostCode::normalize ( "4821" ) );
    EXPECT_EQ ( "", HostCode::normalize ( "46318" ) );
    EXPECT_EQ ( "", HostCode::normalize ( "7KQX" ) );

    // no I, O, 0, 1 anywhere
    EXPECT_EQ ( "", HostCode::normalize ( "IAAA" ) );
    EXPECT_EQ ( "", HostCode::normalize ( "OAAA" ) );
    EXPECT_EQ ( "", HostCode::normalize ( "K7QI" ) );
    EXPECT_EQ ( "", HostCode::normalize ( "KOQX" ) );
    EXPECT_EQ ( "", HostCode::normalize ( "K0QX" ) );
    EXPECT_EQ ( "", HostCode::normalize ( "K1QX" ) );
    EXPECT_EQ ( "", HostCode::normalize ( "kiqx" ) );

    // exactly 4 symbols
    EXPECT_EQ ( "", HostCode::normalize ( "K7Q" ) );
    EXPECT_EQ ( "", HostCode::normalize ( "K7QXA" ) );
    EXPECT_EQ ( "", HostCode::normalize ( "K7 X" ) );
    EXPECT_EQ ( "", HostCode::normalize ( "K7-X" ) );
    EXPECT_EQ ( "", HostCode::normalize ( "" ) );
    EXPECT_EQ ( "", HostCode::normalize ( "cccaster:" ) );

    // addresses and host names are never codes
    EXPECT_EQ ( "", HostCode::normalize ( "127.0.0.1:4821" ) );
    EXPECT_EQ ( "", HostCode::normalize ( "1.2.3.4" ) );
    EXPECT_EQ ( "", HostCode::normalize ( ":4821" ) );
    EXPECT_EQ ( "", HostCode::normalize ( "melty:3939" ) );

    EXPECT_TRUE ( HostCode::isCode ( "k7qx" ) );
    EXPECT_FALSE ( HostCode::isCode ( "46318" ) );
}

// Links: join/<code>, watch/<code> (spectate/<code> too), host/<32 hex token>; anything else is the stock path
TEST ( HostCode, Links )
{
    string v;
    EXPECT_EQ ( HostCode::Link::Join, HostCode::parseLink ( "join/K7QX", v ) ); EXPECT_EQ ( "K7QX", v );
    EXPECT_EQ ( HostCode::Link::Join, HostCode::parseLink ( "join/k7qx/", v ) ); EXPECT_EQ ( "K7QX", v );
    EXPECT_EQ ( HostCode::Link::Spectate, HostCode::parseLink ( "watch/H2NP", v ) ); EXPECT_EQ ( "H2NP", v );
    EXPECT_EQ ( HostCode::Link::Spectate, HostCode::parseLink ( "spectate/H2NP/", v ) ); EXPECT_EQ ( "H2NP", v );
    const string tok = "0123456789abcdef0123456789abcdef";
    EXPECT_EQ ( HostCode::Link::Host, HostCode::parseLink ( "host/" + tok, v ) ); EXPECT_EQ ( tok, v );
    EXPECT_EQ ( HostCode::Link::Host, HostCode::parseLink ( "host/" + tok + "/", v ) );
    EXPECT_EQ ( HostCode::Link::None, HostCode::parseLink ( "host/" + tok.substr ( 1 ), v ) );
    EXPECT_EQ ( HostCode::Link::None, HostCode::parseLink ( "host/0123456789ABCDEF0123456789abcdef", v ) );
    EXPECT_EQ ( HostCode::Link::None, HostCode::parseLink ( "join/4821", v ) ); EXPECT_EQ ( "", v );
    EXPECT_EQ ( HostCode::Link::None, HostCode::parseLink ( "join/K7QI", v ) );
    EXPECT_EQ ( HostCode::Link::None, HostCode::parseLink ( "join/ K7QX", v ) );
    EXPECT_EQ ( HostCode::Link::Join, HostCode::parseLink ( "join/K7QX \r\n", v ) ); // (the whole link is trimmed)
    EXPECT_EQ ( HostCode::Link::None, HostCode::parseLink ( "join/1.2.3.4:46318", v ) );
    EXPECT_EQ ( HostCode::Link::None, HostCode::parseLink ( "watch/abc123", v ) );
    EXPECT_EQ ( HostCode::Link::None, HostCode::parseLink ( "K7QX", v ) );
    EXPECT_EQ ( HostCode::Link::None, HostCode::parseLink ( "46318", v ) );
    EXPECT_EQ ( HostCode::Link::None, HostCode::parseLink ( "1.2.3.4:46318", v ) );
}

// Addresses are masked, ports kept
TEST ( HostCode, Mask )
{
    EXPECT_EQ ( "<K7QX>:46318", HostCode::maskAddress ( "203.0.113.7:46318", "K7QX" ) );
    EXPECT_EQ ( "<K7QX>:7000", HostCode::maskAddress ( "[2001:db8::5]:7000", "K7QX" ) );
    EXPECT_EQ ( "<K7QX>", HostCode::maskAddress ( "2001:db8::5", "K7QX" ) );
    EXPECT_EQ ( "<K7QX>", HostCode::maskAddress ( "203.0.113.7", "K7QX" ) );
    EXPECT_EQ ( "<code>:1", HostCode::maskAddress ( "a:1", "" ) );
}

// The log never carries a redacted address
TEST ( HostCode, LogRedaction )
{
    Logger logger;
    EXPECT_EQ ( "to 203.0.113.7:46318", logger.redacted ( "to 203.0.113.7:46318" ) );
    logger.redact ( "203.0.113.7", "<K7QX>" );
    logger.redact ( "203.0.113.7", "<other>" ); // the first registration wins
    logger.redact ( "", "x" );                  // ignored
    EXPECT_EQ ( "to <K7QX>:46318, again <K7QX>", logger.redacted ( "to 203.0.113.7:46318, again 203.0.113.7" ) );
    EXPECT_EQ ( "nothing here", logger.redacted ( "nothing here" ) );
}

// Registry replies
TEST ( HostCode, Reply )
{
    HostCode::Reply r;

    ASSERT_TRUE ( HostCode::parseReply ( "{\"code\":\"K7QX\",\"secret\":\"0f9a\",\"ttl\":60}", r ) );
    EXPECT_EQ ( "K7QX", r.code );
    EXPECT_EQ ( "0f9a", r.secret );
    EXPECT_EQ ( 60, r.ttl );

    ASSERT_TRUE ( HostCode::parseReply ( "{ \"proto\" : \"cc\", \"addr\" : \"203.0.113.7:46318\" }", r ) );
    EXPECT_EQ ( "cc", r.proto );
    EXPECT_EQ ( "203.0.113.7:46318", r.addr );
    EXPECT_EQ ( "", r.code );

    ASSERT_TRUE ( HostCode::parseReply ( "{\"proto\":\"pc\",\"room\":\"r-9f2c\"}", r ) );
    EXPECT_EQ ( "pc", r.proto );
    EXPECT_EQ ( "", r.addr );

    ASSERT_TRUE ( HostCode::parseReply ( "{\"error\":\"throttled\",\"retryAfterMs\":4000}", r ) );
    EXPECT_EQ ( "throttled", r.error );
    EXPECT_EQ ( 4000u, r.retryAfterMs );

    ASSERT_TRUE ( HostCode::parseReply ( "{\"error\":\"say \\\"no\\\"\",\"ok\":true}", r ) );
    EXPECT_EQ ( "say \"no\"", r.error );

    ASSERT_TRUE ( HostCode::parseReply ( "{}", r ) );
    EXPECT_FALSE ( HostCode::parseReply ( "", r ) );
    EXPECT_FALSE ( HostCode::parseReply ( "<html>502 Bad Gateway</html>", r ) );
    EXPECT_FALSE ( HostCode::parseReply ( "{\"code\":\"K7", r ) );
    EXPECT_FALSE ( HostCode::parseReply ( "{code:1}", r ) );
}

// Registry URL: the constant, or code_list.txt's first real line
TEST ( HostCode, RegistryUrl )
{
    EXPECT_EQ ( HOST_CODE_DEFAULT_URL, HostCode::registryUrlFrom ( "" ) );
    EXPECT_EQ ( HOST_CODE_DEFAULT_URL, HostCode::registryUrlFrom ( "# comment\n\n" ) );
    EXPECT_EQ ( "http://127.0.0.1:7720/hc", HostCode::registryUrlFrom ( "# local\r\n127.0.0.1:7720/hc\r\n" ) );
    EXPECT_EQ ( "http://example.org/hc", HostCode::registryUrlFrom ( "http://example.org/hc\nhttp://second/hc\n" ) );
    EXPECT_EQ ( "http://example.org/hc", HostCode::registryUrlFrom ( "https://example.org/hc\n" ) );
    EXPECT_EQ ( "K7QX", HostCode::urlEncode ( "K7QX" ) );
    EXPECT_EQ ( "a%20b%26c%3D", HostCode::urlEncode ( "a b&c=" ) );
}

// The join box: digits = a port to host, ':' = an address to join, a code = a code. Typed addresses and ports parse
// exactly as before.
TEST ( HostCode, OldInputStillWorks )
{
    IpAddrPort a ( "203.0.113.7:46318" );
    EXPECT_EQ ( "203.0.113.7", a.addr );
    EXPECT_EQ ( 46318, a.port );
    EXPECT_FALSE ( HostCode::isCode ( "203.0.113.7:46318" ) );

    IpAddrPort h ( "46318" );
    EXPECT_TRUE ( h.addr.empty() );
    EXPECT_EQ ( 46318, h.port );

    IpAddrPort h4 ( "4821" );
    EXPECT_TRUE ( h4.addr.empty() );
    EXPECT_EQ ( 4821, h4.port );
    EXPECT_FALSE ( HostCode::isCode ( "4821" ) );

    IpAddrPort n ( "melty.example.org:3939" );
    EXPECT_EQ ( "melty.example.org", n.addr );
    EXPECT_FALSE ( HostCode::isCode ( "melty.example.org:3939" ) );

    EXPECT_THROW ( IpAddrPort ( "203.0.113.7:" ), Exception );

    HostCode::clearJoinedCode();
    EXPECT_EQ ( "", HostCode::joinedCode() );
}

#endif // NOT RELEASE
