#include "Logger.hpp"
#include "Algorithms.hpp"
#include "TimerManager.hpp"

using namespace std;


void Logger::redact ( const string& text, const string& replacement )
{
    if ( text.empty() )
        return;
    for ( const auto& r : _redactions )
        if ( r.first == text )
            return;
    _redactions.push_back ( { text, replacement } );
}

string Logger::redacted ( const string& message ) const
{
    string out = message;
    for ( const auto& r : _redactions )
    {
        for ( size_t i = out.find ( r.first ); i != string::npos; i = out.find ( r.first, i + r.second.size() ) )
            out.replace ( i, r.first.size(), r.second );
    }
    return out;
}


#ifdef DISABLE_LOGGING

void Logger::initialize ( const string& filePath, uint32_t _options ) {}
void Logger::deinitialize() {}
void Logger::flush() {}
void Logger::log ( const char *srcFile, int srcLine, const char *srcFunc, const char *logMessage ) {}

#else

void Logger::initialize ( const string& filePath, uint32_t _options )
{
#ifdef LOGGER_MUTEXED
    LOCK ( _mutex );
#endif

    bool same = _initialized && ( _filePath == filePath );

    this->_options = _options;

    if ( filePath.empty() )
    {
        _filePath.clear();
        _fd = stdout;
    }
    else
    {
        if ( _options & PID_IN_FILENAME )
        {
            const size_t i = filePath.find_last_of ( '.' );
            const string tmp = filePath.substr ( 0, i ) + format ( "_%08d", _getpid() ) + filePath.substr ( i );
            same = ( _filePath == tmp );
            _filePath = tmp;
        }
        else
        {
            _filePath = filePath;
        }

        // Reopen the file if the path changed
        if ( _fd && !same )
        {
            fflush ( _fd );
            fclose ( _fd );
        }

        // Append to the file if the path is the same
        _fd = fopen ( _filePath.c_str(), same ? "a" : "w" );
    }

    if ( ! _initialized )
        _logId = generateRandomId();

    _initialized = true;
}

void Logger::deinitialize()
{
    if ( ! _initialized )
        return;

#ifdef LOGGER_MUTEXED
    LOCK ( _mutex );
#endif

    if ( _fd && _fd != stdout )
        fclose ( _fd );

    sessionId.clear();
    _filePath.clear();
    _logId.clear();
    _options = 0;
    _fd = 0;
    _initialized = false;
}

void Logger::flush()
{
#ifdef LOGGER_MUTEXED
    LOCK ( _mutex );
#endif

    fflush ( _fd );
}

void Logger::log ( const char *srcFile, int srcLine, const char *srcFunc, const char *logMessage )
{
    if ( ! _fd )
        return;

#ifdef LOGGER_MUTEXED
    LOCK ( _mutex );
#endif

    bool hasPrefix = false;

    if ( _options & ( LOG_GM_TIME | LOG_LOCAL_TIME ) )
    {
        time_t t;
        time ( &t );

        tm *ts;
        if ( _options & LOG_GM_TIME )
            ts = gmtime ( &t );
        else
            ts = localtime ( &t );

        strftime ( _buffer, sizeof ( _buffer ), "%H:%M:%S", ts );

        const uint64_t now = TimerManager::get().getNow ( true );

        fprintf ( _fd, "%s.%03u:", _buffer, ( uint32_t ) ( now % 1000 ) );
        hasPrefix = true;
    }

    if ( _options & LOG_FILE_LINE )
    {
        fprintf ( _fd, "%s:%3d:", srcFile, srcLine );
        hasPrefix = true;
    }

    if ( _options & LOG_FUNC_NAME )
    {
        string shortFunc ( srcFunc );
        shortFunc = shortFunc.substr ( 0, shortFunc.find ( '(' ) );
        fprintf ( _fd, "%s:", shortFunc.c_str() );
        hasPrefix = true;
    }

    if ( _redactions.empty() )
        fprintf ( _fd, ( hasPrefix ? " %s\n" : "%s\n" ), logMessage );
    else
        fprintf ( _fd, ( hasPrefix ? " %s\n" : "%s\n" ), redacted ( logMessage ).c_str() );
    fflush ( _fd );
}

#endif // DISABLE_LOGGING

Logger& Logger::get()
{
    static Logger instance;
    return instance;
}
