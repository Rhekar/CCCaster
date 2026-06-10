#include "MainUpdater.hpp"
#include "Logger.hpp"
#include "ProcessManager.hpp"

// rapidjson (vendored by cereal) is included before <windows.h> so the windows min/max macros
// don't clash with its templates.
#include <cereal/external/rapidjson/document.h>

#include <vector>
#include <unordered_set>
#include <fstream>
#include <functional>

#include <windows.h>
#include <wininet.h>

using namespace std;


// Main update archive file name
#define UPDATE_ARCHIVE_FILE "update.zip"

// GitHub repository the updater pulls releases from.
#define GITHUB_OWNER "SkyLeite"
#define GITHUB_REPO  "CCCaster"
#define GITHUB_RELEASES_URL "https://api.github.com/repos/" GITHUB_OWNER "/" GITHUB_REPO "/releases?per_page=30"

// Reported to GitHub; the API rejects requests without a User-Agent.
#define UPDATER_USER_AGENT "CCCaster-Updater"

// Timeouts (ms): the version check is small, the archive can be several MB.
#define VERSION_CHECK_TIMEOUT ( 5000 )
#define ARCHIVE_TIMEOUT       ( 30000 )

// How often the event-loop timer polls the worker thread.
#define POLL_INTERVAL_MS      ( 50 )


namespace
{

// Strip a leading 'v'/'V' from a release tag (e.g. "v4.0" -> "4.0") to get a Version code.
string stripTagPrefix ( const string& tag )
{
    if ( ! tag.empty() && ( tag[0] == 'v' || tag[0] == 'V' ) )
        return tag.substr ( 1 );
    return tag;
}

// Blocking HTTPS GET via WinINet (handles TLS + redirects). Returns true on HTTP 200, with the
// body in outBody. Mirrors the WinINet usage in lib/SentryClient.cpp.
bool httpsGet ( const string& url, const string& headers, string& outBody, int& statusCode, uint32_t timeoutMs )
{
    outBody.clear();
    statusCode = 0;

    HINTERNET hInet = InternetOpenA ( UPDATER_USER_AGENT, INTERNET_OPEN_TYPE_PRECONFIG, 0, 0, 0 );
    if ( ! hInet )
    {
        LOG ( "InternetOpen failed err=%lu", GetLastError() );
        return false;
    }

    DWORD t = timeoutMs;
    InternetSetOption ( hInet, INTERNET_OPTION_CONNECT_TIMEOUT, &t, sizeof ( t ) );
    InternetSetOption ( hInet, INTERNET_OPTION_SEND_TIMEOUT, &t, sizeof ( t ) );
    InternetSetOption ( hInet, INTERNET_OPTION_RECEIVE_TIMEOUT, &t, sizeof ( t ) );

    const DWORD flags = INTERNET_FLAG_SECURE | INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE
                        | INTERNET_FLAG_NO_UI | INTERNET_FLAG_KEEP_CONNECTION;

    HINTERNET hUrl = InternetOpenUrlA ( hInet, url.c_str(),
                                        headers.empty() ? 0 : headers.c_str(),
                                        headers.empty() ? 0 : ( DWORD ) headers.size(),
                                        flags, 0 );
    if ( ! hUrl )
    {
        LOG ( "InternetOpenUrl failed err=%lu url=%s", GetLastError(), url );
        InternetCloseHandle ( hInet );
        return false;
    }

    DWORD status = 0, len = sizeof ( status );
    HttpQueryInfoA ( hUrl, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &len, 0 );
    statusCode = ( int ) status;

    char buf[8192];
    DWORD read = 0;
    while ( InternetReadFile ( hUrl, buf, sizeof ( buf ), &read ) && read > 0 )
        outBody.append ( buf, read );

    InternetCloseHandle ( hUrl );
    InternetCloseHandle ( hInet );

    return ( statusCode == 200 );
}

// Blocking HTTPS download to a file, reporting progress via onProgress(downloaded, total).
bool httpsDownload ( const string& url, const string& filePath, uint32_t timeoutMs,
                     const function<void ( uint32_t, uint32_t )>& onProgress )
{
    HINTERNET hInet = InternetOpenA ( UPDATER_USER_AGENT, INTERNET_OPEN_TYPE_PRECONFIG, 0, 0, 0 );
    if ( ! hInet )
    {
        LOG ( "InternetOpen failed err=%lu", GetLastError() );
        return false;
    }

    DWORD t = timeoutMs;
    InternetSetOption ( hInet, INTERNET_OPTION_CONNECT_TIMEOUT, &t, sizeof ( t ) );
    InternetSetOption ( hInet, INTERNET_OPTION_SEND_TIMEOUT, &t, sizeof ( t ) );
    InternetSetOption ( hInet, INTERNET_OPTION_RECEIVE_TIMEOUT, &t, sizeof ( t ) );

    const DWORD flags = INTERNET_FLAG_SECURE | INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE
                        | INTERNET_FLAG_NO_UI | INTERNET_FLAG_KEEP_CONNECTION;

    HINTERNET hUrl = InternetOpenUrlA ( hInet, url.c_str(), 0, 0, flags, 0 );
    if ( ! hUrl )
    {
        LOG ( "InternetOpenUrl failed err=%lu url=%s", GetLastError(), url );
        InternetCloseHandle ( hInet );
        return false;
    }

    DWORD status = 0, len = sizeof ( status );
    HttpQueryInfoA ( hUrl, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &len, 0 );

    if ( status != 200 )
    {
        LOG ( "Download HTTP %lu for %s", status, url );
        InternetCloseHandle ( hUrl );
        InternetCloseHandle ( hInet );
        return false;
    }

    DWORD total = 0;
    len = sizeof ( total );
    HttpQueryInfoA ( hUrl, HTTP_QUERY_CONTENT_LENGTH | HTTP_QUERY_FLAG_NUMBER, &total, &len, 0 );

    ofstream out ( filePath.c_str(), ios::binary );
    if ( ! out )
    {
        LOG ( "Could not open for write: %s", filePath );
        InternetCloseHandle ( hUrl );
        InternetCloseHandle ( hInet );
        return false;
    }

    char buf[16384];
    DWORD read = 0;
    uint32_t done = 0;
    bool ok = true;

    for ( ;; )
    {
        if ( ! InternetReadFile ( hUrl, buf, sizeof ( buf ), &read ) )
        {
            LOG ( "InternetReadFile failed err=%lu", GetLastError() );
            ok = false;
            break;
        }

        if ( read == 0 )
            break;

        out.write ( buf, read );
        done += read;

        if ( onProgress )
            onProgress ( done, total );
    }

    out.close();

    InternetCloseHandle ( hUrl );
    InternetCloseHandle ( hInet );

    return ok;
}

} // anonymous namespace


MainUpdater::MainUpdater ( Owner *owner ) : owner ( owner )
{
    if ( ProcessManager::isWine() )
    {
        _downloadDir = ProcessManager::appDir;
        return;
    }

    char buffer[4096];
    if ( GetTempPath ( sizeof ( buffer ), buffer ) )
        _downloadDir = normalizeWindowsPath ( buffer );

    if ( _downloadDir.empty() )
        _downloadDir = ProcessManager::appDir;
}

void MainUpdater::fetch ( const Type& type )
{
    _type = type;

    {
        LOCK ( _mutex );
        _done = false;
        _success = false;
        _progDone = 0;
        _progTotal = 0;
    }

    if ( type == Type::Version )
    {
        _targetVersion.clear();
        _changelogBody.clear();
        _assetUrl.clear();
    }

    // Run the blocking WinINet work on a worker thread, then poll it from the event loop so the
    // owner callbacks (and progress bar) stay on the main thread.
    _worker.reset ( new FetchThread ( *this, type ) );
    _worker->start();

    _pollTimer.reset ( new Timer ( this ) );
    _pollTimer->start ( POLL_INTERVAL_MS );
}

void MainUpdater::timerExpired ( Timer *timer )
{
    ASSERT ( _pollTimer.get() == timer );

    bool done, success;
    uint32_t progDone, progTotal;

    {
        LOCK ( _mutex );
        done = _done;
        success = _success;
        progDone = _progDone;
        progTotal = _progTotal;
    }

    if ( ! done )
    {
        if ( owner && progTotal > 0 )
            owner->fetchProgress ( this, _type, double ( progDone ) / progTotal );

        _pollTimer->start ( POLL_INTERVAL_MS );
        return;
    }

    _pollTimer.reset();

    if ( _worker )
    {
        _worker->join();
        _worker.reset();
    }

    if ( ! owner )
        return;

    if ( success )
        owner->fetchCompleted ( this, _type );
    else
        owner->fetchFailed ( this, _type );
}

void MainUpdater::runFetch ( const Type& type )
{
    bool ok = false;

    switch ( type.value )
    {
        case Type::Version:
            ok = fetchVersion();
            break;

        case Type::ChangeLog:
            ok = writeChangeLog();
            break;

        case Type::Archive:
            ok = downloadArchive();
            break;

        default:
            break;
    }

    LOCK ( _mutex );
    _success = ok;
    _done = true;
}

bool MainUpdater::fetchVersion()
{
    static const string headers =
        "Accept: application/vnd.github+json\r\n"
        "X-GitHub-Api-Version: 2022-11-28\r\n"
        "Accept-Encoding: identity\r\n";

    string body;
    int status = 0;

    if ( ! httpsGet ( GITHUB_RELEASES_URL, headers, body, status, VERSION_CHECK_TIMEOUT ) )
    {
        LOG ( "Failed to fetch GitHub releases (status=%d)", status );
        return false;
    }

    return parseReleases ( body );
}

bool MainUpdater::parseReleases ( const string& body )
{
    // This (cereal-vendored) rapidjson predates the modern API: Parse needs explicit flags and
    // arrays are walked with Begin()/End() rather than GetArray().
    rapidjson::Document doc;
    doc.Parse<0> ( body.c_str() );

    if ( doc.HasParseError() || ! doc.IsArray() )
    {
        LOG ( "Could not parse releases JSON" );
        return false;
    }

    // Pick the Nth release matching the channel (newest-first): Latest=0, Previous=1.
    const unsigned wantIndex = ( _temporal == Temporal::Previous ) ? 1 : 0;
    unsigned matched = 0;

    for ( auto it = doc.Begin(); it != doc.End(); ++it )
    {
        rapidjson::Value& rel = *it;

        if ( ! rel.IsObject() )
            continue;

        // This rapidjson exposes IsTrue()/IsFalse() rather than IsBool()/GetBool(); IsTrue() is
        // exactly the "present and true" test we want.
        const bool draft = rel.HasMember ( "draft" ) && rel["draft"].IsTrue();
        if ( draft )
            continue;

        const bool prerelease = rel.HasMember ( "prerelease" ) && rel["prerelease"].IsTrue();

        // Stable channel skips prereleases; Dev channel keeps them.
        if ( _channel == Channel::Stable && prerelease )
            continue;

        if ( matched++ != wantIndex )
            continue;

        if ( ! rel.HasMember ( "tag_name" ) || ! rel["tag_name"].IsString() )
        {
            LOG ( "Matched release has no tag_name" );
            return false;
        }

        const string code = stripTagPrefix ( rel["tag_name"].GetString() );

        _targetVersion = Version ( code );

        _changelogBody.clear();
        if ( rel.HasMember ( "body" ) && rel["body"].IsString() )
            _changelogBody = rel["body"].GetString();

        // Choose the release asset: prefer cccaster.v<code>.zip, else the first .zip.
        _assetUrl.clear();
        const string preferred = format ( "cccaster.v%s.zip", code );

        if ( rel.HasMember ( "assets" ) && rel["assets"].IsArray() )
        {
            rapidjson::Value& assets = rel["assets"];

            for ( auto a = assets.Begin(); a != assets.End(); ++a )
            {
                rapidjson::Value& asset = *a;

                if ( ! asset.IsObject()
                     || ! asset.HasMember ( "name" ) || ! asset["name"].IsString()
                     || ! asset.HasMember ( "browser_download_url" ) || ! asset["browser_download_url"].IsString() )
                    continue;

                const string name = asset["name"].GetString();
                const string url = asset["browser_download_url"].GetString();

                if ( name == preferred )
                {
                    _assetUrl = url;
                    break;
                }

                if ( _assetUrl.empty() && name.size() >= 4 && name.compare ( name.size() - 4, 4, ".zip" ) == 0 )
                    _assetUrl = url;
            }
        }

        LOG ( "Resolved %s: code=%s asset=%s", getTargetDescName(), code, _assetUrl );

        return ( ! _targetVersion.major().empty() && ! _targetVersion.minor().empty() );
    }

    LOG ( "No matching release for %s", getTargetDescName() );
    return false;
}

bool MainUpdater::writeChangeLog()
{
    if ( _changelogBody.empty() )
    {
        LOG ( "No changelog body cached" );
        return false;
    }

    const string path = _downloadDir + CHANGELOG;

    ofstream out ( path.c_str(), ios::binary );
    if ( ! out )
    {
        LOG ( "Could not write changelog: %s", path );
        return false;
    }

    out.write ( _changelogBody.data(), _changelogBody.size() );
    out.close();

    return true;
}

bool MainUpdater::downloadArchive()
{
    if ( _targetVersion.empty() || _assetUrl.empty() )
    {
        std::string name = getTargetDescName();
        name[0] = std::toupper ( name[0] );
        LOG ( name + " has no downloadable archive" );
        return false;
    }

    return httpsDownload ( _assetUrl, _downloadDir + UPDATE_ARCHIVE_FILE, ARCHIVE_TIMEOUT,
                           [this] ( uint32_t done, uint32_t total )
    {
        LOCK ( _mutex );
        _progDone = done;
        _progTotal = total;
    } );
}

bool MainUpdater::openChangeLog() const
{
    unordered_set<string> folders = { _downloadDir, ProcessManager::appDir };

    for ( const string& folder : folders )
    {
        const DWORD val = GetFileAttributes ( ( folder + CHANGELOG ).c_str() );

        if ( val != INVALID_FILE_ATTRIBUTES )
        {
            if ( ProcessManager::isWine() )
                system ( ( "notepad " + folder + CHANGELOG ).c_str() );
            else
                system ( ( "\"start \"Viewing change log\" notepad " + folder + CHANGELOG + "\"" ).c_str() );
            return true;
        }

        LOG ( "Missing: %s", folder + CHANGELOG );
    }

    LOG ( "Could not open any change logs" );

    return false;
}

bool MainUpdater::extractArchive() const
{
    DWORD val = GetFileAttributes ( ( _downloadDir + UPDATE_ARCHIVE_FILE ).c_str() );

    if ( val == INVALID_FILE_ATTRIBUTES )
    {
        LOG ( "Missing: %s", _downloadDir + UPDATE_ARCHIVE_FILE );
        return false;
    }

    if ( _targetVersion.empty() )
    {
        std::string name = getTargetDescName();
        name[0] = std::toupper(name[0]);
        LOG(name + " version is unknown");
        return false;
    }

    const string srcUpdater = ProcessManager::appDir + FOLDER + UPDATER;
    string tmpUpdater = _downloadDir + UPDATER;

    if ( srcUpdater != tmpUpdater && ! CopyFile ( srcUpdater.c_str(), tmpUpdater.c_str(), FALSE ) )
        tmpUpdater = srcUpdater;

    const string binary = format ( "cccaster.v%s.%s.exe", _targetVersion.major(), _targetVersion.minor() );

    const string command = format ( "\"" + tmpUpdater + "\" %d %s %s %s",
                                    GetCurrentProcessId(),
                                    binary,
                                    _downloadDir + UPDATE_ARCHIVE_FILE,
                                    ProcessManager::appDir );

    LOG ( "Binary: %s", binary );

    LOG ( "Command: %s", command );

    system ( ( "\"start \"Updating...\" " + command + "\"" ).c_str() );

    exit ( 0 );

    return true;
}

std::string MainUpdater::getChannelName() const
{
    switch (_channel.value) {
        case Channel::Dev: return "dev";
        case Channel::Stable: return "stable";
        default: return "unknown-channel";
    }
}

std::string MainUpdater::getTemporalName() const
{
    switch (_temporal.value) {
        case Temporal::Latest: return "latest";
        case Temporal::Previous: return "previous";
        default: return "unknown-temporal";
    }
}

std::string MainUpdater::getTargetDescName() const
{
    return getTemporalName() + " " + getChannelName();
}
