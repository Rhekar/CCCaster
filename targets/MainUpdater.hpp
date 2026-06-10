#pragma once

#include "Enum.hpp"
#include "Thread.hpp"
#include "Timer.hpp"
#include "Version.hpp"

#include <string>


class MainUpdater
    : private Timer::Owner
{
public:

    ENUM ( Type, Version, ChangeLog, Archive );

    ENUM(Channel, Stable, Dev);

    ENUM(Temporal, Latest, Previous);

    struct Owner
    {
        virtual void fetchCompleted ( MainUpdater *updater, const Type& type ) = 0;

        virtual void fetchFailed ( MainUpdater *updater, const Type& type ) = 0;

        virtual void fetchProgress ( MainUpdater *updater, const Type& type, double progress ) = 0;
    };

    Owner *owner = 0;

    MainUpdater ( Owner *owner );

    void fetch ( const Type& type );

    bool openChangeLog() const;

    bool extractArchive() const;

    void setChannel(const Channel& channel) { _channel = channel; }
    void setTemporal(const Temporal& temporal) { _temporal = temporal; }

    std::string getChannelName() const;
    std::string getTemporalName() const;
    std::string getTargetDescName() const;

    Type getType() const { return _type; }

    const Version& getTargetVersion() const { return _targetVersion; }

private:

    // Worker thread that runs the (blocking) WinINet operation off the event loop.
    struct FetchThread : public Thread
    {
        MainUpdater& updater;
        Type type;

        FetchThread ( MainUpdater& updater, const Type& type ) : updater ( updater ), type ( type ) {}

        void run() override { updater.runFetch ( type ); }
    };

    friend struct FetchThread;

    Type _type;

    Channel _channel;

    Temporal _temporal;

    Version _targetVersion;

    // Changelog body and download URL cached from the most recent Version fetch.
    std::string _changelogBody;

    std::string _assetUrl;

    std::string _downloadDir;

    // Worker thread + the state it hands back to the polling timer (guarded by _mutex).
    ThreadPtr _worker;

    TimerPtr _pollTimer;

    mutable Mutex _mutex;

    bool _done = false;

    bool _success = false;

    uint32_t _progDone = 0, _progTotal = 0;

    // Runs on the worker thread.
    void runFetch ( const Type& type );

    bool fetchVersion();

    bool parseReleases ( const std::string& body );

    bool writeChangeLog();

    bool downloadArchive();

    // Runs on the event loop; polls the worker and dispatches owner callbacks.
    void timerExpired ( Timer *timer ) override;
};
