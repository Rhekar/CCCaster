#include "Lobby.hpp"
#include "ConsoleUi.hpp"
#include "TcpSocket.hpp"
#include "Exceptions.hpp"
#include "EventManager.hpp"
#include "StringUtils.hpp"

using namespace std;

vector<string> Lobby::getMenu()
{
    if ( mode == MENU ) {
        return { "Public Lobbies", "Create Lobby", "Enter Lobby Code", "Default Lobby" };
    } else if ( mode == CONCERTO_BROWSE ) {
        return publiclobbies;
    } else if ( mode == CONCERTO_LOBBY ) {
        return lobbyentries;
    } else {
        return entries;
    }
}

vector<string> Lobby::getIps(){
    if ( mode == CONCERTO_LOBBY ) {
        return lobbyips;
    } else {
        return ips;
    }
}

vector<string> Lobby::getIds(){
    return lobbyids;
}

Lobby::Lobby( ILobbyBackend::Owner* owner )
{
    this->owner = owner;
    timeout = 5000;
    numEntries = 0;
    blankEntry = "";
    for( int i = 0; i < 43; ++i ) {
        blankEntry += " ";
    }
    blankEntry += "|   |Open   ";
    vector<string> menu;
    for( int i = 0; i < 20; ++i ) {
        menu.push_back( blankEntry );
        ips.push_back( blankEntry );
    }
    entries = menu;
    connectionSuccess = false;
    newRequestSuccess = false;
    mode = MENU;
}

bool Lobby::connect( std::string url )
{
    LOG ( "Connecting to: '%s'", url );

    url = _address.str();
    try
    {
        _socket = TcpSocket::connect ( this, url, true, timeout ); // Raw socket
    }
    catch ( ... )
    {
        _socket.reset();

        LOG ( "Failed to create socket!" );

        if ( owner )
            owner->connectionFailed ( this );
        return false;
    }
    return true;

}

void Lobby::disconnect()
{
    if ( owner )
        owner->connectionFailed(this);
}
void Lobby::socketConnected ( Socket *socket )
{
    ASSERT ( _socket.get() == socket );
    const string request = format ( "LIST,none" );
    LOG ( "Sending request:\n%s", request );

    _timer.reset ( new Timer ( this ) );
    _timer->start ( 3000 );

    if ( ! _socket->send ( &request[0], request.size() ) ) {
        LOG ( "Failed to send request!" );

        if ( owner )
            owner->connectionFailed ( this );
    }

}

void Lobby::socketDisconnected ( Socket *socket )
{
    LOG ( "Socket DC" );
    ASSERT ( _socket.get() == socket );

    _socket.reset();
    _timer.reset();

    if ( owner )
        owner->connectionFailed ( this );
}

void Lobby::socketRead ( Socket *socket, const char *bytes, size_t len, const IpAddrPort& address )
{
    LOG ( "Socket Read" );
    ASSERT ( _socket.get() == socket );
    const string data ( bytes, len );

    //_timer.reset();

    LOG( data );

    vector<string> rawdata = split( data, "\x1f" );
    string reqType = rawdata[0];
    if ( reqType == "LIST" ) {
        int entryCount = stoi( rawdata[1] );
        if ( entryCount > 0 ) {
            vector<string> lines = { rawdata.begin()+2, rawdata.end() };
            vector<string> names;
            vector<string> ip;
            for ( string line : lines ) {
                vector<string> nameipaddr = split( line, "\x1e" );
                names.push_back(nameipaddr[0]);
                ip.push_back(nameipaddr[1]);
            }
            entryMutex.lock();
            numEntries = stoi( rawdata[1] );
            LOG( lines[0] );
            LOG( numEntries );
            for ( int i = 0; i < numEntries; ++i ){
                entries[i] = names[i];
                ips[i] = ip[i];
            }
            for ( int i = numEntries; i < 20; ++i ) {
                entries[i] = blankEntry;
            }
            entryMutex.unlock();
        } else {
            entryMutex.lock();
            numEntries = 0;
            for ( int i = 0; i < 20; ++i ) {
                entries[i] = blankEntry;
            }
            entryMutex.unlock();
        }

    } else if  ( reqType == "HOST" ) {
        hostSuccess = rawdata[1] == "True";
    } else if ( reqType == "CLIST" ) {
        int entryCount = stoi( rawdata[1] );
        if ( entryCount > 0 ) {
            entryMutex.lock();
            numEntries = stoi( rawdata[1] );
            publiclobbies.clear();
            roomcodes.clear();
            for( uint32_t i = 2; i < numEntries + 2; ++i ) {
                vector<string> codeplayer = split( rawdata[i], "\x1e" );
                string code = codeplayer[0];
                roomcodes.push_back( code );
                string numPlayers = codeplayer[1];
                string output = code + "   " + numPlayers;
                publiclobbies.push_back( output );
            }
            entryMutex.unlock();
        } else {
            entryMutex.lock();
            numEntries = 0;
            entryMutex.unlock();
        }
    } else if ( reqType == "CLOBBY" || reqType == "CCREATE" ) {
        mode = CONCERTO_LOBBY;
        int entryCount = stoi( rawdata[1] );
        if ( entryCount > 0 ) {
            entryMutex.lock();
            numEntries = stoi( rawdata[1] );
            LOG(rawdata[1]);
            lobbyentries.clear();
            lobbyips.clear();
            lobbyids.clear();
            for( uint32_t i = 2; i < numEntries + 2; ++i ) {
                vector<string> players = split( rawdata[i], "\x1e" );
                string name = players[0];
                LOG(name);
                string ip = players[1];
                string id = players[2];
                lobbyentries.push_back( name );
                lobbyips.push_back( ip );
                lobbyids.push_back( id );
            }
            entryMutex.unlock();
        } else {
            entryMutex.lock();
            numEntries = 0;
            lobbyentries.clear();
            entryMutex.unlock();
        }
        if ( reqType == "CCREATE" ) {
            lobbyMsg = rawdata.back();
            LOG( lobbyMsg );
        }
    } else if ( reqType == "CCHAL" ) {
        hostSuccess = true;
    } else if ( reqType == "CJOINF" ) {
        lobbyError = rawdata[1];
        owner->unlock( this );
    } else if ( reqType == "CERROR" ) {
        lobbyError = rawdata[1];
        owner->unlock( this );
    }

    if ( owner && !connectionSuccess ) {
        connectionSuccess = true;
        LOG("D");
        owner->unlock( this );
    }
    if ( owner && !newRequestSuccess ) {
        if ( ( mode == CONCERTO_BROWSE && reqType == "CLIST" ) ||
             ( mode == CONCERTO_LOBBY && reqType == "CLOBBY" ) ) {
            newRequestSuccess = true;
            LOG("D");
            owner->unlock( this );
        }
    }
    if ( owner && !hostResponse ) {
        hostResponse = true;
        owner->unlock( this );
    }
    LOG("endscoketre");
}

// Timer callback
void Lobby::timerExpired ( Timer *timer )
{
    LOG( "Timer Expired Callback" );
    ASSERT ( _timer.get() == timer );

    string request;
    if ( mode == DEFAULT_LOBBY ) {
        request = format ( "LIST,none" );
    } else if ( mode == CONCERTO_BROWSE ) {
        request = format ( "CLIST,none" );
    } else if ( mode == CONCERTO_LOBBY ) {
        request = format( "CLOBBY,none");
    } else {
        return;
    }
    LOG ( "Sending request:\n%s", request );

    _timer.reset ( new Timer ( this ) );
    _timer->start ( 3000 );

    if ( ! _socket->send ( &request[0], request.size() ) ) {
        LOG ( "Failed to send request!" );

        if ( owner )
            owner->connectionFailed ( this );
    }
}

void Lobby::fetchPublicLobby()
{
    LOG( "Fetch public lobbies" );
    const string request = format ( "CLIST,none" );
    if ( ! _socket->send ( &request[0], request.size() ) ) {
        LOG ( "Failed to send request!" );

        if ( owner )
            owner->connectionFailed ( this );
    }
    newRequestSuccess = false;
}

void Lobby::run()
{
    LOG( "Lobby run" );
    connect( "ASDF" );
    EventManager::get().start();
    LOG( "Lobby stop" );
    stop();
}

void Lobby::stop()
{
    LOG( "stop" );
    _socket.reset();
    _timer.reset();
    owner->unlock( this );
    entryMutex.unlock();
}

void Lobby::host( string name, IpAddrPort port ) {
    hostSuccess = false;
    string portStr = port.str();
    hostResponse = false;
    const string request = format ( "HOST," + name + "|" + portStr );
    _timer.reset ( new Timer ( this ) );
    _timer->start ( 3000 );
    LOG( request );
    if ( ! _socket->send ( &request[0], request.size() ) ) {
        LOG ( "Failed to send request!" );

        if ( owner )
            owner->connectionFailed ( this );
    }
}

void Lobby::challenge( string target, IpAddrPort port ) {
    hostSuccess = false;
    string portStr = port.str();
    hostResponse = false;
    const string request = format ( "CCHAL," + target + "|" + portStr );
    _timer.reset ( new Timer ( this ) );
    _timer->start ( 3000 );
    LOG( request );
    if ( ! _socket->send ( &request[0], request.size() ) ) {
        LOG ( "Failed to send request!" );

        if ( owner )
            owner->connectionFailed ( this );
    }
}

void Lobby::end() {
    const string request = format ( "CEND," );
    LOG( request );
    if ( ! _socket->send ( &request[0], request.size() ) ) {
        LOG ( "Failed to send request!" );

        if ( owner )
            owner->connectionFailed ( this );
    }
}

void Lobby::unhost() {
    const string request = format ( "UNHOST,none" );
    if ( ! _socket->send ( &request[0], request.size() ) ) {
        LOG ( "Failed to send request!" );

        if ( owner )
            owner->connectionFailed ( this );
    }
}

string Lobby::join( string name, int selection ) {
    string code = roomcodes[selection];
    join( name, code );
    return code;
}

void Lobby::join( string name, string code ) {
    const string request = format ( "CJOIN," + name + "|" + code );
    _timer.reset ( new Timer ( this ) );
    _timer->start ( 3000 );
    LOG( request );
    if ( ! _socket->send ( &request[0], request.size() ) ) {
        LOG ( "Failed to send request!" );

        if ( owner )
            owner->connectionFailed ( this );
    }

    newRequestSuccess = false;
}

void Lobby::preaccept( string code ) {
    const string request = format ( "CPREACCEPT," + code );
    _timer.reset ( new Timer ( this ) );
    _timer->start ( 3000 );
    LOG( request );
    if ( ! _socket->send ( &request[0], request.size() ) ) {
        LOG ( "Failed to send request!" );

        if ( owner )
            owner->connectionFailed ( this );
    }
}

void Lobby::accept() {
    const string request = format ( "CACCEPT," );
    LOG( request );
    if ( ! _socket->send ( &request[0], request.size() ) ) {
        LOG ( "Failed to send request!" );

        if ( owner )
            owner->connectionFailed ( this );
    }
}

void Lobby::create( string name, string type ) {
    const string request = format ( "CCREATE," + name + "|" + type );
    _timer.reset ( new Timer ( this ) );
    _timer->start ( 3000 );
    LOG( request );
    if ( ! _socket->send ( &request[0], request.size() ) ) {
        LOG ( "Failed to send request!" );

        if ( owner )
            owner->connectionFailed ( this );
    }
    newRequestSuccess = false;
}

bool Lobby::checkLobbyCode( string code ) {
    if ( code.size() < 2 || code.size() > 8 ) {
        return false;
    }
    for ( uint32_t i; i < code.size(); ++i ) {
        if ( !isalnum( code[i] ) ) {
            return false;
        }
    }
    return true;
}
