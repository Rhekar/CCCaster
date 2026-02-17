#include "Main.hpp"
#include "MainUi.hpp"
#include "Version.hpp"
#include "ConsoleUi.hpp"
#include "Exceptions.hpp"
#include "ErrorStringsExt.hpp"
#include "CharacterSelect.hpp"
#include "StringUtils.hpp"
#include "NetplayStates.hpp"

#include <algorithm>
#include <iomanip>
#include <mmsystem.h>
#include <wininet.h>

using namespace std;


// Indent position of the pinging stats (must be a string)
#define INDENT_STATS "20"

// System sound prefix
#define SYSTEM_ALERT_PREFEX "System"

// Main configuration file
#define CONFIG_FILE FOLDER TAG "config.ini"

// Main update archive file name
#define UPDATE_ARCHIVE "update.zip"

// Run macro that deinitializes controllers, runs, then reinitializes controllers
#define RUN(ADDRESS, CONFIG)                                                                            \
    do {                                                                                                \
        ControllerManager::get().deinitialize();                                                        \
        run ( ADDRESS, CONFIG );                                                                        \
        ControllerManager::get().loadMappings ( ProcessManager::appDir + FOLDER, MAPPINGS_EXT );        \
        ControllerManager::get().initialize ( this );                                                   \
    } while ( 0 )


static const string uiTitle = "CCCaster " + LocalVersion.majorMinor();

static ConsoleUi::Menu *mainMenu = 0;

static Mutex uiMutex;

static CondVar uiCondVar;

static vector<IpAddrPort> loadServers() {
    std::ifstream infile(LOBBY_LIST);
    std::string str;
    vector<IpAddrPort> servers;
    while (std::getline(infile, str)) {
        servers.push_back(str);
    }
    return servers;
}

static const vector<IpAddrPort> serverList = loadServers();

MainUi::MainUi() : _updater ( this )
{
}

void MainUi::netplay ( RunFuncPtr run )
{
    ConsoleUi::Prompt *menu = new ConsoleUi::Prompt ( ConsoleUi::Prompt::String,
            "Enter/paste <ip>:<port> to join or <port> to host:" );

    _ui->pushRight ( menu, { 1, 0 } ); // Expand width

    for ( ;; )
    {
        menu->setInitial ( ( _address.addr.empty() && !_address.empty() )
                           ? _address.str().substr ( 1 )
                           : _address.str() );

        _ui->popUntilUserInput();

        if ( menu->resultStr.empty() )
            break;

        _ui->clearBelow();

        try
        {
            _address = trimmed ( menu->resultStr );
        }
        catch ( const Exception& exc )
        {
            _ui->pushBelow ( new ConsoleUi::TextBox ( exc.user ), { 1, 0 } ); // Expand width
            continue;
        }

        if ( _address.addr.empty() )
        {
            _config.setInteger ( "lastUsedPort", _address.port );
            saveConfig();

            if ( ! gameMode ( true ) ) // Show below
                continue;

            initialConfig.mode.value = ClientMode::Host;
        }
        else
        {
            initialConfig.mode.value = ClientMode::Client;
        }

        _netplayConfig.clear();

        RUN ( _address, initialConfig );

        _ui->popNonUserInput();

        if ( ! sessionError.empty() )
        {
            _ui->pushBelow ( new ConsoleUi::TextBox ( sessionError ), { 1, 0 } ); // Expand width
            sessionError.clear();
        }
    }

    _ui->pop();
}

void MainUi::server ( RunFuncPtr run )
{
    serverMode = true;
    _ui->pushRight ( new ConsoleUi::Menu ( "Mode", { "Lobby", "Matchmaking" }, "Cancel" ) );
    _ui->top<ConsoleUi::Menu>()->setPosition ( _config.getInteger ( "lastServerMenuPosition" ) - 1 );
    try {
        for ( ;; )
        {
            int mode = _ui->popUntilUserInput ( false )->resultInt;
            LOG( "mode %d", mode );
            if ( mode < 0 || mode >= 2 )
                break;
            _config.setInteger ( "lastServerMenuPosition", mode + 1 );
            switch ( mode )
            {
            case 0:
                lobby( run );
                _lobby.reset();
                if ( ! sessionError.empty() ) {
                    _ui->pushBelow ( new ConsoleUi::TextBox ( sessionError ), { 1, 0 } ); // Expand width
                    sessionError.clear();
                }
                break;
            case 1:
                matchmaking( run );
                if ( ! sessionError.empty() ) {
                    _ui->pushBelow ( new ConsoleUi::TextBox ( sessionError ), { 1, 0 } ); // Expand width
                    sessionError.clear();
                }
                break;
            default:
                break;
            }
        }
    } catch (std::exception& e) {
        LOG("Exception");
        LOG(e.what());
    }
    _ui->pop();
    serverMode = false;
}

void MainUi::wait() {
    while ( uiCondVar.wait ( uiMutex, 2500 ) ) {
        if ( ! EventManager::get().isRunning() ) {
            break;
        }
    }
}

void MainUi::lobby( RunFuncPtr run )
{
    _lobby.reset( new Lobby( this ) );
    ifstream infile;
    _lobby->_address = *serverList.cbegin();
    AutoManager _;

    _lobby->start();

    display ( "Connecting to server..." );
    LOCK ( uiMutex );
    wait();
    _ui->pop();
    if ( ! EventManager::get().isRunning() ) {
        sessionError = "Failed to connect";
        return;
    }
    lobbyText =_lobby->getMenu();
    lobbyIps =_lobby->getIps();
    ConsoleUi::Element* menu = new ConsoleUi::Menu ( "Lobby", { "Public Lobbies",
                                                                "Create Lobby",
                                                                "Enter Lobby Code"
                                                              },
        "Exit" );

    _ui->pushInFront ( menu );
    _ui->clearRight();
    _ui->top<ConsoleUi::Menu>()->setTimeout ( 5 );
    int numEntries;
    bool addressSelected = false;
    string lobbyBase = "Lobby";
    string lobbyDisplay = lobbyBase;
    string name = _config.getString ( "displayName" );
    for ( ;; )
    {
        if ( ! _lobby->isRunning() || ! EventManager::get().isRunning() ) {
            LOG( "Lobby stopped"  );
            sessionError = "Disconnected!";
            break;
        }
        // Update ui
        LOG("Getting lobby mutex");
        _lobby->entryMutex.lock();
        lobbyText =_lobby->getMenu();
        numEntries =_lobby->numEntries;
        lobbyIps =_lobby->getIps();
        lobbyIds =_lobby->getIds();
        _lobby->entryMutex.unlock();
        LOG("releasing lobby mutex");
        int oldPos = _ui->top<ConsoleUi::Menu>()->getPosition();
        _ui->pop();
        _ui->pushInFront ( new ConsoleUi::Menu ( lobbyDisplay,
                                                 lobbyText,
                                                 "Exit"
                                                 ) );
        _ui->top<ConsoleUi::Menu>()->setPosition ( oldPos );
        _ui->top<ConsoleUi::Menu>()->setTimeout ( 2 );
        int mode = _ui->popUntilUserInput ( false )->resultInt;
        if ( mode == MENUTIMEOUT ) {
            LOG( "menutimeout" );
            continue;
        }

        if ( _lobby->mode == MENU ) {
            if ( mode < 0 || mode >= 4 ) {
                break;
            } else if ( mode == 0 ) {
                _lobby->mode = CONCERTO_BROWSE;
                display ( "Fetching public lobbies..." );
                lobbyDisplay = "  | Room | Players";
                _lobby->fetchPublicLobby();
                wait();
                _ui->pop();
            } else if ( mode == 1 ) {
                _ui->pushInFront ( new ConsoleUi::Menu ( "Room Type",
                                                         { "Public",
                                                           "Private" },
                                                         "Exit" ),
                                   { 1, 0 }, true ); // Expand width and clear top
                int mode = _ui->popUntilUserInput ( false )->resultInt;
                _ui->pop();
                if ( mode >= 0 && mode < 2 ) {
                    display ( "Creating lobby..." );
                    string result = mode ? "Private" : "Public";
                    _lobby->create( name, result );
                    wait();
                    _ui->pop();
                    if ( _lobby->mode != CONCERTO_LOBBY ) {
                        lobbyDisplay = lobbyBase + " - Error creating lobby";
                    } else {
                        lobbyDisplay = "Room code: " + _lobby->lobbyMsg;
                    }
                }
            } else if ( mode == 2 ) {
                _ui->pushInFront ( new ConsoleUi::Prompt ( ConsoleUi::Prompt::String,
                                                           "Enter lobby code:" ),
                                   { 1, 0 }, true ); // Expand width and clear top
                _ui->popUntilUserInput();
                string result = _ui->top()->resultStr;
                _ui->pop();
                LOG( result);
                if ( _lobby->checkLobbyCode( result ) ) {
                    display ( "Joining lobby..." );
                    _lobby->join( name, result );
                    lobbyDisplay = "Room code: " + result;
                    wait();
                    _ui->pop();
                    if ( _lobby->mode != CONCERTO_LOBBY )
                        lobbyDisplay = lobbyBase + " - Invalid lobby code";
                } else {
                    lobbyDisplay = lobbyBase + " - Invalid lobby code";
                }
            } else if ( mode == 3 ) {
                _lobby->mode = DEFAULT_LOBBY;
            }
        } else if ( _lobby->mode == CONCERTO_BROWSE ) {
            if ( mode < 0 || mode >= numEntries ) {
                break;
            } else {
                display ( "Joining lobby..." );
                string code = _lobby->join( name, mode );
                lobbyDisplay = "Room code: " + code;
                wait();
                _ui->pop();
                if ( _lobby->mode != CONCERTO_LOBBY )
                    lobbyDisplay = "  | Room | Players | Error: " + _lobby->lobbyError;
            }
        } else if ( _lobby->mode == CONCERTO_LOBBY ) {
            if ( mode < 0 || mode >= numEntries ) {
                break;
            } else {
                if ( lobbyIps[ mode ] == "None" ) {
                    //Hosting
                    LOG( "Hosting" );
                    _address = "46318";
                    try {
                        _lobby->challenge( lobbyIds[mode], _address );
                    } catch (std::exception& e) {
                        LOG( e.what() );
                    }
                    wait();
                    if ( _lobby->hostSuccess ) {
                        initialConfig.mode.value = ClientMode::Host;
                        addressSelected = true;
                        LOG( "Lobby host Prerun");
                        RUN ( _address, initialConfig );
                        LOG( "Lobby host Postrun");
                        _ui->popNonUserInput();
                        LOG( "Lobby preunhost");
                        _lobby->unhost();
                        LOG( "Lobby postunhost");
                        if ( ! sessionError.empty() ) {
                            lobbyDisplay = lobbyBase + " - " + sessionError;
                            sessionError.clear();
                        }
                    }
                    try {
                        _lobby->end();
                    } catch (std::exception& e) {
                        LOG( e.what() );
                    }
                } else {
                    //Connecting
                    LOG( "Lobby Connecting" );
                    _lobby->preaccept( lobbyIds[mode] );
                    initialConfig.mode.value = ClientMode::Client;
                    _netplayConfig.clear();
                    _address = lobbyIps[ mode ];
                    addressSelected = true;
                    LOG( "Lobby join Prerun");
                    RUN ( _address, initialConfig );
                    LOG( "Lobby join Postrun");
                    _ui->popNonUserInput();
                    if ( ! sessionError.empty() ) {
                        lobbyDisplay = lobbyBase + " - " + sessionError;
                        sessionError.clear();
                    }
                }
            }
        } else if ( _lobby->mode == DEFAULT_LOBBY ) {
            if ( mode < 0 || mode >= 20 ) {
                break;
            } else if ( mode >= numEntries  ) {
                //Hosting
                LOG( "Hosting" );
                _address = "46318";
                try {
                    _lobby->host( name, _address );
                } catch (std::exception& e) {
                    LOG( e.what() );
                }
                uiCondVar.wait ( uiMutex );
                if ( _lobby->hostSuccess ) {
                    initialConfig.mode.value = ClientMode::Host;
                    addressSelected = true;
                    LOG( "Lobby host Prerun");
                    RUN ( _address, initialConfig );
                    LOG( "Lobby host Postrun");
                    _ui->popNonUserInput();
                    LOG( "Lobby preunhost");
                    _lobby->unhost();
                    LOG( "Lobby postunhost");
                    if ( ! sessionError.empty() ) {
                        lobbyDisplay = lobbyBase + " - " + sessionError;
                        sessionError.clear();
                    }
                } else {
                    lobbyDisplay = lobbyBase + " - Lobby Full";
                }
            } else {
                //Connecting
                LOG( "Connecting" );
                initialConfig.mode.value = ClientMode::Client;
                _netplayConfig.clear();
                _address = lobbyIps[ mode ];
                addressSelected = true;
                LOG( "Lobby join Prerun");
                RUN ( _address, initialConfig );
                LOG( "Lobby join Postrun");
                _ui->popNonUserInput();
                if ( ! sessionError.empty() ) {
                    lobbyDisplay = lobbyBase + " - " + sessionError;
                    sessionError.clear();
                }
            }
        }
    }
    LOG( "Lobby Stopping EM");
    EventManager::get().stop();
    LOG( "Lobby after Stopping EM");
    if ( !addressSelected ) {
        display ( "Disconnecting from server..." );
    }
    //LOCK ( uiMutex );
    LOG( "Lobby wait mutex");
    //if ( lo)
    uiCondVar.wait ( uiMutex );
    LOG( "Lobby mutex signaled");
    _ui->pop();
    LOG( "Lobby clear ui");
    if ( !addressSelected ) {
        _ui->pop();
    }
    LOG( "Lobby reset pointer");
    //_lobby.reset( new Lobby( this ) );
    //_lobby.reset();
    LOG( "Lobby exiting");
}

void MainUi::matchmaking( RunFuncPtr run )
{
    isMatchmaking = true;
    ifstream infile;
    string region = _config.getString ( "matchmakingRegion" );
    _mmm.reset( new MatchmakingManager( this, *serverList.cbegin(), region ) );
    AutoManager _ ( _mmm.get(), getConsoleWindow() );

    _mmm->start();

    display ( "Connecting to server..." );
    LOCK ( uiMutex );
    LOG( "lockConnectionMutex");
    uiCondVar.wait ( uiMutex );
    LOG( "unlockConnectionMutex");
    _ui->pop();
    if ( ! EventManager::get().isRunning() || !_mmm->isRunning() ) {
        sessionError = "Failed to connect";
        return;
    }

    display ( "Waiting for opponent..." );
    LOG( "lockWaitingMutex");
    uiCondVar.wait ( uiMutex );
    LOG( "unlockWaitingMutex");
    _ui->pop();
    if ( ! EventManager::get().isRunning() ) {
        sessionError = "Disconnected";
        return;
    }

    if ( initialConfig.mode.value == ClientMode::Client ) {
        _netplayConfig.clear();
        _mmm->ignoreKb = true;
        LOG( "MMM preRun");
        RUN ( _address, initialConfig );
        LOG( "MMM postRun");
        _ui->popNonUserInput();
    } else if ( initialConfig.mode.value == ClientMode::Host ) {
        _netplayConfig.clear();
        _address = "46318";
        _mmm->ignoreKb = true;
        LOG( "MMM preRun");
        RUN ( _address, initialConfig );
        LOG( "MMM postRun");
        _ui->popNonUserInput();
    } else {
        sessionError = "Session Closed";
    }

    LOG( "MMM stopping EM");
    EventManager::get().stop();
    LOG( "MMM done" );
    display ( "Disconnecting from server..." );
    uiCondVar.wait ( uiMutex );
    _ui->pop();
    isMatchmaking = false;
}

void MainUi::spectate ( RunFuncPtr run )
{

    _ui->pushRight ( new ConsoleUi::Menu ( "Mode", { "Netplay", "Broadcast" }, "Cancel" ) );
    
    int isBroadcast = _ui->popUntilUserInput ( true )->resultInt; 

    _ui->pop();

    saveConfig();

    ConsoleUi::Prompt *menu = new ConsoleUi::Prompt ( ConsoleUi::Prompt::String,
            "Enter/paste <ip>:<port> to spectate:" );

    _ui->pushRight ( menu, { 1, 0 } ); // Expand width

    for ( ;; )
    {
        menu->setInitial ( !_address.addr.empty() ? _address.str() : "" );

        _ui->popUntilUserInput();

        if ( menu->resultStr.empty() )
            break;

        _ui->clearBelow();

        try
        {
            _address = trimmed ( menu->resultStr );
        }
        catch ( const Exception& exc )
        {
            _ui->pushBelow ( new ConsoleUi::TextBox ( exc.user ), { 1, 0 } ); // Expand width
            continue;
        }

        if ( _address.addr.empty() )
        {
            _ui->pushBelow ( new ConsoleUi::TextBox ( ERROR_INVALID_ADDR_PORT ), { 1, 0 } ); // Expand width
            continue;
        }

        initialConfig.mode.value = ClientMode::SpectateNetplay; // is this the issue?
        if(isBroadcast) {
            initialConfig.mode.value = ClientMode::SpectateBroadcast;
        }
    
        RUN ( _address, initialConfig );
    
        _ui->popNonUserInput();

        if ( ! sessionError.empty() )
        {
            _ui->pushBelow ( new ConsoleUi::TextBox ( sessionError ), { 1, 0 } ); // Expand width
            sessionError.clear();
        }
    }

    _ui->pop();
}

void MainUi::broadcast ( RunFuncPtr run )
{
    _ui->pushRight ( new ConsoleUi::Prompt ( ConsoleUi::Prompt::Integer,
                     "Enter/paste <port> to broadcast:" ), { 1, 0 } ); // Expand width

    _ui->top<ConsoleUi::Prompt>()->allowNegative = false;
    _ui->top<ConsoleUi::Prompt>()->maxDigits = 5;
    _ui->top<ConsoleUi::Prompt>()->setInitial ( _address.port ? _address.port : INT_MIN );

    for ( ;; )
    {
        ConsoleUi::Element *menu = _ui->popUntilUserInput();

        if ( menu->resultInt == INT_MIN )
            break;

        _ui->clearBelow();

        ASSERT ( menu->resultInt >= 0 );

        if ( menu->resultInt > 0xFFFF )
        {
            _ui->pushBelow ( new ConsoleUi::TextBox ( ERROR_INVALID_PORT ), { 1, 0 } ); // Expand width
            continue;
        }

        if ( ! gameMode ( true ) ) // Show below
            continue;

        _netplayConfig.clear();
        _netplayConfig.mode.value = ClientMode::Broadcast;
        _netplayConfig.mode.flags = initialConfig.mode.flags;
        _netplayConfig.delay = 0;
        _netplayConfig.hostPlayer = 1;
        _netplayConfig.broadcastPort = menu->resultInt;

        _address.port = menu->resultInt;
        _address.invalidate();

        _config.setInteger ( "lastUsedPort", _address.port );
        saveConfig();

        RUN ( "", _netplayConfig );

        _ui->popNonUserInput();

        if ( ! sessionError.empty() )
        {
            _ui->pushBelow ( new ConsoleUi::TextBox ( sessionError ), { 1, 0 } ); // Expand width
            sessionError.clear();
        }
    }

    _ui->pop();
}

void MainUi::offline ( RunFuncPtr run )
{
    if ( ! offlineGameMode() )
        return;

    const bool tournament = _netplayConfig.tournament;

    _netplayConfig.clear();
    _netplayConfig.mode.value = ClientMode::Offline;
    _netplayConfig.mode.flags = initialConfig.mode.flags;
    _netplayConfig.delay = 0;
    _netplayConfig.hostPlayer = 1; // TODO make this configurable
    _netplayConfig.tournament = tournament;
    _netplayConfig.trialAudioCue = _config.getString ( "trialAudioCueFile" );
    _netplayConfig.trialFlashColor = _config.getInteger ( "trialScreenFlashColor" );
    RUN ( "", _netplayConfig );

    _ui->popNonUserInput();
}

bool MainUi::areYouSure()
{
    _ui->pushRight ( new ConsoleUi::Menu ( "Are you sure?", { "Yes" }, "Cancel" ) );

    int ret = _ui->popUntilUserInput()->resultInt;

    _ui->pop();

    return ( ret == 0 );
}

bool MainUi::gameMode ( bool below )
{
    ConsoleUi::Menu *menu = new ConsoleUi::Menu ( "Mode", { "Versus", "Training" }, "Cancel" );

    if ( below )
        _ui->pushBelow ( menu );
    else
        _ui->pushInFront ( menu );

    _ui->popUntilUserInput ( true ); // Clear other messages since we're starting the game now

    int mode = _ui->top()->resultInt;

    _ui->pop();

    if ( mode < 0 || mode > 1 )
        return false;

    initialConfig.mode.flags = ( mode == 1 ? ClientMode::Training : 0 );
    return true;
}

bool MainUi::offlineGameMode()
{
    _ui->pushRight ( new ConsoleUi::Menu ( "Mode", { "Training", "Versus", "Versus CPU", "Tournament", "Replay", "Trial" }, "Cancel" ) );
    _ui->top<ConsoleUi::Menu>()->setPosition ( _config.getInteger ( "lastOfflineMenuPosition" ) - 1 );

    int mode = _ui->popUntilUserInput ( true )->resultInt; // Clear other messages since we're starting the game now

    _ui->pop();

    if ( mode < 0 || mode > 5 )
        return false;

    _config.setInteger ( "lastOfflineMenuPosition", mode + 1 );
    saveConfig();

    if ( mode == 0 )
        initialConfig.mode.flags = ClientMode::Training;
    else if ( mode == 1 )
        initialConfig.mode.flags = 0; // Versus
    else if ( mode == 2 )
        initialConfig.mode.flags = ClientMode::VersusCPU;
    else if ( mode == 3 )
        _netplayConfig.tournament = true;
    else if ( mode == 4 )
      initialConfig.mode.flags = ClientMode::Replay;
    else if ( mode == 5 )
      initialConfig.mode.flags = ClientMode::Training | ClientMode::Trial;
    else
        return false;

    return true;
}

void MainUi::joystickToBeDetached ( Controller *controller )
{
    LOG ( "controller=%08x; currentController=%08x", controller, _currentController );

    if ( controller != _currentController )
        return;

    _currentController = 0;
    EventManager::get().stop();
}

void MainUi::controllerKeyMapped ( Controller *controller, uint32_t key )
{
    ASSERT ( controller == _currentController );

    LOG ( "%s: controller=%08x; key=%08x", controller->getName(), controller, key );

    _mappedKey = key;
    EventManager::get().stop();
}

void MainUi::controls()
{
    for ( ;; )
    {
        ControllerManager::get().refreshJoysticks();

        vector<Controller *> controllers = ControllerManager::get().getControllers();

        vector<string> names;
        names.reserve ( controllers.size() );
        for ( Controller *c : controllers )
            names.push_back ( c->getName() );

        _ui->pushRight ( new ConsoleUi::Menu ( "Controllers", names, "Back" ) );
        int ret = _ui->popUntilUserInput ( true )->resultInt; // Clear popped since we don't care about messages
        _ui->pop();

        if ( ret < 0 || ret >= ( int ) controllers.size() )
            break;

        Controller& controller = *controllers[ret];

        int pos = ( controller.isKeyboard() ? 0 : 4 );

        for ( ;; )
        {
            loadMappings ( controller );

            vector<string> options ( gameInputBits.size() );

            for ( size_t i = 0; i < gameInputBits.size(); ++i )
            {
                const string mapping = controller.getMapping ( gameInputBits[i].second );
                options[i] = format ( "%-11s: %s", gameInputBits[i].first, mapping );
            }

            options.push_back ( "Clear all" );
            options.push_back ( "Reset to defaults" );

            if ( controller.isJoystick() )
                options.push_back ( format ( "Set joystick deadzone (%.2f)", controller.getDeadzone() ) );

            options.push_back ( format ( "Set SOCD method: %s", controller.getSocd() ) );

            // Add instructions above menu
            _ui->pushRight ( new ConsoleUi::TextBox (
                                 controller.getName() + " mappings\n"
                                 "Press Left/Right/Delete to delete a key\n"
                                 "Press Escape to cancel mapping\n" ) );

            // Add menu without title
            _ui->pushBelow ( new ConsoleUi::Menu ( options, "Back" ) );
            _ui->top<ConsoleUi::Menu>()->setPosition ( pos );
            _ui->top<ConsoleUi::Menu>()->setDelete ( 2 );

            for ( ;; )
            {
                pos = _ui->popUntilUserInput()->resultInt;

                // Cancel or exit
                if ( pos < 0 || pos > ( int ) options.size() )
                    break;

                // Modify a key
                if ( pos >= 0 && pos < ( int ) gameInputBits.size() )
                    break;

                // Last 3 options (ignore delete)
                if ( ! _ui->top()->resultStr.empty() )
                    break;
            }

            // Cancel or exit
            if ( pos < 0
                    || pos > ( int ) options.size()
                    || ( pos == ( int ) options.size() && !_ui->top()->resultStr.empty() ) )
            {
                _ui->pop();
                _ui->pop();
                break;
            }

            // Clear all
            if ( pos == ( int ) gameInputBits.size() )
            {
                _ui->pop();
                _ui->pop();

                if ( areYouSure() )
                {
                    controller.clearAllMappings();
                    saveMappings ( controller );
                }
                continue;
            }

            // Reset to defaults
            if ( pos == ( int ) gameInputBits.size() + 1 )
            {
                _ui->pop();
                _ui->pop();

                if ( areYouSure() )
                {
                    if ( controller.isKeyboard() )
                        controller.setMappings ( ProcessManager::fetchKeyboardConfig() );
                    else
                        controller.resetToDefaults();
                    saveMappings ( controller );
                }
                continue;
            }

            // Set joystick deadzone
            if ( pos == ( int ) gameInputBits.size() + 2 &&
                 controller.isJoystick() )
            {
                _ui->pop();
                _ui->pop();

                if ( ! controller.isJoystick() )
                    continue;

                _ui->pushRight ( new ConsoleUi::Prompt ( ConsoleUi::Prompt::String,
                                 "Enter a value between 0.0 and 1.0:" ) );

                _ui->top<ConsoleUi::Prompt>()->setInitial ( format ( "%.2f", controller.getDeadzone() ) );

                for ( ;; )
                {
                    _ui->popUntilUserInput();

                    if ( _ui->top()->resultStr.empty() )
                        break;

                    stringstream ss ( _ui->top()->resultStr );
                    float ret = -1.0f;

                    if ( ! ( ss >> ret ) || ret <= 0.0f || ret >= 1.0f )
                    {
                        _ui->pushBelow ( new ConsoleUi::TextBox ( "Value must be a number between 0.0 and 1.0!" ) );
                        continue;
                    }

                    controller.setDeadzone ( ret );
                    break;
                }

                saveMappings ( controller );
                _ui->pop();
                continue;
            }
            // Set SOCD
            if ( pos == ( int ) gameInputBits.size() + 2 ||
                 pos == ( int ) gameInputBits.size() + 3 )
            {
                _ui->pop();
                _ui->pop();

                if ( controller.isJoystick() &&
                     pos == ( int ) gameInputBits.size() + 2 )
                    continue;
                _ui->pushRight ( new ConsoleUi::Menu ( "SOCD settings", { "Default",
                                                                          "L/R cancel",
                                                                          "U/D cancel",
                                                                          "L/R U/D cancel",
                                                                          "U/D=U",
                                                                          "L/R cancel U/D=U" },
                        "Exit" ) );
                _ui->top<ConsoleUi::Menu>()->setPosition ( controller.getSocdInt() );
                for ( ;; )
                {
                    uint32_t ret = _ui->popUntilUserInput()->resultInt;

                    if ( ret > 5 )
                        break;

                    controller.setSocd ( ret );
                    break;
                }

                saveMappings ( controller );
                _ui->pop();
                continue;
            }

            // Delete specific mapping
            if ( _ui->top()->resultStr.empty() )
            {
                if ( pos < ( int ) gameInputBits.size() )
                    controller.clearMapping ( gameInputBits[pos].second );

                saveMappings ( controller );
                _ui->pop();
                _ui->pop();
                continue;
            }

            // Map selected key
            _ui->top<ConsoleUi::Menu>()->overlayCurrentPosition ( format ( "%-11s: ...", gameInputBits[pos].first ) );

            try
            {
                _currentController = &controller;

                LOG ( "currentController=%08x", _currentController );

                AutoManager _ ( _currentController, getConsoleWindow() );

                if ( controller.isKeyboard() )
                {
                    controller.startMapping ( this, gameInputBits[pos].second );

                    EventManager::get().start();
                }
                else
                {
                    ControllerManager::get().check(); // Flush joystick events before mapping

                    if ( _currentController )
                    {
                        controller.startMapping ( this, gameInputBits[pos].second );

                        EventManager::get().startPolling();

                        while ( EventManager::get().poll ( 1 ) )
                        {
                            ControllerManager::get().check();
                        }
                    }
                }
            }
            catch ( ... )
            {
            }

            _ui->pop();
            _ui->pop();

            if ( ! _currentController )
                break;

            saveMappings ( controller );
            _currentController = 0;

            // Continue mapping
            if ( _mappedKey && pos + 1 < ( int ) gameInputBits.size() )
            {
                ++pos;
                _ungetch ( RETURN_KEY );
            }
        }
    }
}

void MainUi::settings()
{
    static const vector<string> options =
    {
        "Alert on connect",
        "Display name",
        "Show full character names",
        "Game CPU priority",
        "Versus mode win count",
        "Check for updates on startup",
        "Max allowed network delay",
        "Default rollback",
        "Held start button in versus",
        "Automatic Replay Save",
        "Matchmaking Region",
        "Trial Input Guide Settings",
        "Experimental Settings",
        "About",
    };

    _ui->pushRight ( new ConsoleUi::Menu ( "Settings", options, "Back" ) );

    for ( ;; )
    {
        ConsoleUi::Element *menu = _ui->popUntilUserInput ( true ); // Clear popped since we don't care about messages

        if ( menu->resultInt < 0 || menu->resultInt >= ( int ) options.size() )
            break;

        switch ( menu->resultInt )
        {
            case 0:
            {
                _ui->pushInFront ( new ConsoleUi::Menu ( "Alert when connected?",
                { "Focus window", "Play a sound", "Do both", "Don't alert" }, "Cancel" ),
                { 0, 0 }, true ); // Don't expand but DO clear top

                _ui->top<ConsoleUi::Menu>()->setPosition ( ( _config.getInteger ( "alertOnConnect" ) + 3 ) % 4 );
                _ui->popUntilUserInput();

                int alertType = _ui->top()->resultInt;

                _ui->pop();

                if ( alertType < 0 || alertType > 3 )
                    break;

                _config.setInteger ( "alertOnConnect", ( alertType + 1 ) % 4 );
                saveConfig();

                if ( alertType == 0 || alertType == 3 )
                    break;

                _ui->pushInFront ( new ConsoleUi::TextBox (
                                       "Enter/paste/drag a .wav file here:\n"
                                       "(Leave blank to use SystemDefault)" ),
                { 1, 0 }, true ); // Expand width and clear top

                _ui->pushBelow ( new ConsoleUi::Prompt ( ConsoleUi::Prompt::String ), { 1, 0 } ); // Expand width

                _ui->top<ConsoleUi::Prompt>()->setInitial ( _config.getString ( "alertWavFile" ) );
                _ui->popUntilUserInput();

                if ( _ui->top()->resultInt == 0 )
                {
                    if ( _ui->top()->resultStr.empty() )
                        _config.setString ( "alertWavFile", "SystemDefault" );
                    else
                        _config.setString ( "alertWavFile", _ui->top()->resultStr );
                    saveConfig();
                }

                _ui->pop();
                _ui->pop();
                break;
            }

            case 1:
                _ui->pushInFront ( new ConsoleUi::Prompt ( ConsoleUi::Prompt::String,
                                   "Enter name to show when connecting:" ),
                { 1, 0 }, true ); // Expand width and clear top

                _ui->top<ConsoleUi::Prompt>()->setInitial ( _config.getString ( "displayName" ) );
                _ui->popUntilUserInput();

                if ( _ui->top()->resultInt == 0 )
                {
                    _config.setString ( "displayName", _ui->top()->resultStr );
                    saveConfig();
                }

                _ui->pop();
                break;

            case 2:
                _ui->pushInFront ( new ConsoleUi::Menu ( "Show full character names when spectating?",
                { "Yes", "No" }, "Cancel" ),
                { 0, 0 }, true ); // Don't expand but DO clear top

                _ui->top<ConsoleUi::Menu>()->setPosition ( ( _config.getInteger ( "fullCharacterName" ) + 1 ) % 2 );
                _ui->popUntilUserInput();

                if ( _ui->top()->resultInt >= 0 && _ui->top()->resultInt <= 1 )
                {
                    _config.setInteger ( "fullCharacterName", ( _ui->top()->resultInt + 1 ) % 2 );
                    saveConfig();
                }

                _ui->pop();
                break;

            case 3:
                _ui->pushInFront ( new ConsoleUi::Menu ( "Start game with high CPU priority?",
                { "Yes", "No" }, "Cancel" ),
                { 0, 0 }, true ); // Don't expand but DO clear top

                _ui->top<ConsoleUi::Menu>()->setPosition ( ( _config.getInteger ( "highCpuPriority" ) + 1 ) % 2 );
                _ui->popUntilUserInput();

                if ( _ui->top()->resultInt >= 0 && _ui->top()->resultInt <= 1 )
                {
                    _config.setInteger ( "highCpuPriority", ( _ui->top()->resultInt + 1 ) % 2 );
                    saveConfig();
                }

                _ui->pop();
                break;

            case 4:
                _ui->pushInFront ( new ConsoleUi::Prompt ( ConsoleUi::Prompt::Integer, "Enter versus mode win count:" ),
                { 0, 0 }, true ); // Don't expand but DO clear top

                _ui->top<ConsoleUi::Prompt>()->allowNegative = false;
                _ui->top<ConsoleUi::Prompt>()->maxDigits = 1;
                _ui->top<ConsoleUi::Prompt>()->setInitial ( _config.getInteger ( "versusWinCount" ) );

                for ( ;; )
                {
                    _ui->popUntilUserInput();

                    if ( _ui->top()->resultInt < 0 )
                        break;

                    if ( _ui->top()->resultInt == 0 )
                    {
                        _ui->pushBelow ( new ConsoleUi::TextBox ( "Win count can't be zero!" ) );
                        continue;
                    }

                    if ( _ui->top()->resultInt > 5 )
                    {
                        _ui->pushBelow ( new ConsoleUi::TextBox ( "Win count can't be greater than 5!" ) );
                        continue;
                    }

                    _config.setInteger ( "versusWinCount", _ui->top()->resultInt );
                    saveConfig();
                    break;
                }

                _ui->pop();
                break;

            case 5:
                _ui->pushInFront ( new ConsoleUi::Menu ( "Check for updates on startup?",
                { "Yes", "No" }, "Cancel" ),
                { 0, 0 }, true ); // Don't expand but DO clear top

                _ui->top<ConsoleUi::Menu>()->setPosition ( ( _config.getInteger ( "autoCheckUpdates" ) + 1 ) % 2 );
                _ui->popUntilUserInput();

                if ( _ui->top()->resultInt >= 0 && _ui->top()->resultInt <= 1 )
                {
                    _config.setInteger ( "autoCheckUpdates", ( _ui->top()->resultInt + 1 ) % 2 );
                    saveConfig();
                }

                _ui->pop();
                break;

            case 6:
                _ui->pushInFront ( new ConsoleUi::Prompt ( ConsoleUi::Prompt::Integer,
                                   "Enter max allowed network delay:" ),
                { 0, 0 }, true ); // Don't expand but DO clear top

                _ui->top<ConsoleUi::Prompt>()->allowNegative = false;
                _ui->top<ConsoleUi::Prompt>()->maxDigits = 3;
                _ui->top<ConsoleUi::Prompt>()->setInitial ( _config.getInteger ( "maxRealDelay" ) );

                for ( ;; )
                {
                    _ui->popUntilUserInput();

                    if ( _ui->top()->resultInt < 0 )
                        break;

                    if ( _ui->top()->resultInt == 0 )
                    {
                        _ui->pushBelow ( new ConsoleUi::TextBox ( "Max network delay can't be zero!" ) );
                        continue;
                    }

                    if ( _ui->top()->resultInt >= 0xFF )
                    {
                        _ui->pushBelow ( new ConsoleUi::TextBox ( ERROR_INVALID_DELAY ) );
                        continue;
                    }

                    _config.setInteger ( "maxRealDelay", _ui->top()->resultInt );
                    saveConfig();
                    break;
                }

                _ui->pop();
                break;

            case 7:
                _ui->pushInFront ( new ConsoleUi::Prompt ( ConsoleUi::Prompt::Integer, "Enter default rollback:" ),
                { 0, 0 }, true ); // Don't expand but DO clear top

                _ui->top<ConsoleUi::Prompt>()->allowNegative = false;
                _ui->top<ConsoleUi::Prompt>()->maxDigits = 2;
                _ui->top<ConsoleUi::Prompt>()->setInitial ( _config.getInteger ( "defaultRollback" ) );

                for ( ;; )
                {
                    _ui->popUntilUserInput();

                    if ( _ui->top()->resultInt < 0 )
                        break;

                    if ( _ui->top()->resultInt > MAX_ROLLBACK )
                    {
                        _ui->pushBelow ( new ConsoleUi::TextBox (
                                             format ( ERROR_INVALID_ROLLBACK, 1 + MAX_ROLLBACK ) ) );
                        continue;
                    }

                    _config.setInteger ( "defaultRollback", _ui->top()->resultInt );
                    saveConfig();
                    break;
                }

                _ui->pop();
                break;

            case 8:
            {
                _ui->pushInFront ( new ConsoleUi::TextBox (
                                       "Number of seconds needed to hold the start button\n"
                                       "in versus mode before it registers:" ),
                { 1, 0 }, true ); // Expand width and clear top

                _ui->pushBelow ( new ConsoleUi::Prompt ( ConsoleUi::Prompt::String ),
                { 1, 0 } ); // Expand width

                double value = _config.getDouble ( "heldStartDuration" );

                _ui->top<ConsoleUi::Prompt>()->setInitial ( format ( "%.1f", value ) );

                for ( ;; )
                {
                    _ui->popUntilUserInput();

                    if ( _ui->top()->resultStr.empty() )
                        break;

                    stringstream ss ( _ui->top()->resultStr );

                    if ( ! ( ss >> value ) || value < 0.0 )
                    {
                        _ui->pushBelow ( new ConsoleUi::TextBox ( "Must be a decimal number and at least 0.0!" ) );
                        continue;
                    }

                    _config.setDouble ( "heldStartDuration", value );
                    saveConfig();
                    break;
                }

                _ui->pop();
                break;
            }

            case 9:
            {
                _ui->pushInFront ( new ConsoleUi::Menu ( "Turn on automatic replay saving?",
                                                         { "Yes", "No" }, "Cancel" ),
                                   { 0, 0 }, true ); // Don't expand but DO clear top

                _ui->top<ConsoleUi::Menu>()->setPosition ( ( _config.getInteger ( "autoReplaySave" ) + 1 ) % 2 );
                _ui->popUntilUserInput();

                if ( _ui->top()->resultInt >= 0 && _ui->top()->resultInt <= 1 )
                {
                    _config.setInteger ( "autoReplaySave", ( _ui->top()->resultInt + 1 ) % 2 );
                    saveConfig();
                }

                _ui->pop();
                break;
            }

            case 10:
            {
                vector<string> regions = { "NA West", "NA East", "SA", "EU", "Middle East", "Asia", "OCE" };
                _ui->pushInFront ( new ConsoleUi::Menu ( "Set Matchmaking Region",
                                                         regions, "Cancel" ),
                                   { 0, 0 }, true ); // Don't expand but DO clear top

                string configRegion = _config.getString ( "matchmakingRegion" );
                bool found = false;
                int regionPos = 0;
                for ( string region : regions ) {
                    if ( region == configRegion ) {
                        found = true;
                        break;
                    }
                    regionPos++;
                }
                if ( !found )
                    regionPos = 0;
                _ui->top<ConsoleUi::Menu>()->setPosition ( regionPos );
                _ui->popUntilUserInput();

                if ( _ui->top()->resultInt >= 0 && _ui->top()->resultInt < regions.size() )
                {
                    _config.setString ( "matchmakingRegion", regions[ _ui->top()->resultInt ] );
                    saveConfig();
                }

                _ui->pop();
                break;
            }

            case 11:
                _ui->pushInFront ( new ConsoleUi::Menu ( "Trial Input Guide Options",
                                                         { "Trial Audio Cue", "Trial Screen Flash Color" }, "Cancel" ),
                                   { 0, 0 }, true ); // Don't expand but DO clear top
                while ( true ) {
                    _ui->popUntilUserInput();
                    int trialSetting = _ui->top()->resultInt;
                    if ( trialSetting == 0 ) {
                        _ui->pushInFront ( new ConsoleUi::TextBox (
                                                                   "Enter/paste/drag a .wav file here:\n"
                                                                   "(Leave blank to use SystemDefault)" ),
                                           { 1, 0 }, true ); // Expand width and clear top

                        _ui->pushBelow ( new ConsoleUi::Prompt ( ConsoleUi::Prompt::String ), { 1, 0 } ); // Expand width
                        _ui->top<ConsoleUi::Prompt>()->setInitial ( _config.getString ( "trialAudioCueFile" ) );
                        _ui->popUntilUserInput();
                        if ( _ui->top()->resultInt == 0 )
                        {
                            if ( _ui->top()->resultStr.empty() )
                                _config.setString ( "trialAudioCueFile", "SystemDefault" );
                            else
                                _config.setString ( "trialAudioCueFile", _ui->top()->resultStr );
                            saveConfig();
                        }
                        _ui->pop();
                        _ui->pop();
                    } else if ( trialSetting == 1 ) {
                        _ui->pushInFront ( new ConsoleUi::TextBox (
                                                                   "Enter screen flash color hex code" ),
                                           { 1, 0 }, true ); // Expand width and clear top

                        _ui->pushBelow ( new ConsoleUi::Prompt ( ConsoleUi::Prompt::String ), { 1, 0 } ); // Expand width
                        stringstream stream;
                        stream << "0x" << setfill('0') << setw(8) << hex << _config.getInteger ( "trialScreenFlashColor" );
                        string color = stream.str();
                        _ui->top<ConsoleUi::Prompt>()->setInitial ( color );
                        _ui->popUntilUserInput();
                        if ( _ui->top()->resultInt == 0 )
                        {
                            if ( _ui->top()->resultStr.empty() )
                                _config.setInteger ( "trialScreenFlashColor", 0xff0000ff );
                            else {
                                string resStr = _ui->top()->resultStr;
                                LOG( resStr );
                                uint32_t resInt = strtoul(resStr.c_str(), NULL, 16);
                                LOG( resInt );
                                _config.setInteger ( "trialScreenFlashColor", resInt );
                            }
                            saveConfig();
                        }
                        _ui->pop();
                        _ui->pop();
                    } else {
                        _ui->pop();
                        break;
                    }
                }
                break;
            case 12:
                _ui->pushInFront ( new ConsoleUi::Menu ( "Experimental Options",
                                                         { "Disable Caster Frame Limiter", "Disable Stage Animations" }, "Cancel" ),
                                   { 0, 0 }, true ); // Don't expand but DO clear top
                while ( true ) {
                    _ui->popUntilUserInput();
                    int setting = _ui->top()->resultInt;
                    if ( setting == 0 ) {
                        _ui->pushInFront ( new ConsoleUi::Menu ( "Disable Caster Frame Limiter?",
                                                                 { "Yes", "No" }, "Cancel" ),
                                           { 0, 0 }, true ); // Don't expand but DO clear top

                        _ui->top<ConsoleUi::Menu>()->setPosition ( ( _config.getInteger ( "frameLimiter" ) + 1 ) % 2 );
                        _ui->popUntilUserInput();

                        if ( _ui->top()->resultInt >= 0 && _ui->top()->resultInt <= 1 )
                            {
                                _config.setInteger ( "frameLimiter", ( _ui->top()->resultInt + 1 ) % 2 );
                                saveConfig();
                            }

                        _ui->pop();
                    } else if ( setting == 1 ) {
                        _ui->pushInFront ( new ConsoleUi::Menu ( "Disable Stage Animations?",
                                                                 { "Yes", "No" }, "Cancel" ),
                                           { 0, 0 }, true ); // Don't expand but DO clear top

                        _ui->top<ConsoleUi::Menu>()->setPosition ( ( _config.getInteger ( "stageAnimations" ) + 1 ) % 2 );
                        _ui->popUntilUserInput();

                        if ( _ui->top()->resultInt >= 0 && _ui->top()->resultInt <= 1 )
                            {
                                _config.setInteger ( "stageAnimations", ( _ui->top()->resultInt + 1 ) % 2 );
                                saveConfig();
                            }

                        _ui->pop();
                    } else {
                        _ui->pop();
                        break;
                    }
                }
                break;
            case 13:
                _ui->pushInFront ( new ConsoleUi::TextBox ( format ( "CCCaster %s%s\n\nRevision %s\n\nBuilt on %s\n\n"
                                   "Created by Madscientist\n\nPress any key to go back",
                                   LocalVersion.code,
#if defined(DEBUG)
                                   " (debug)",
#elif defined(LOGGING)
                                   " (logging)",
#else
                                   "",
#endif
                                   LocalVersion.revision, LocalVersion.buildTime ) ),
                { 0, 0 }, true ); // Don't expand but DO clear top
                system ( "@pause > nul" );
                break;

            default:
                break;
        }
    }

    _ui->pop();
}

void MainUi::results()
{
    static const vector<string> options =
    {
      "Show",
      "Next",
      "Back",
    };

    _ui->pushRight ( new ConsoleUi::Menu ( "Results", options, "Quit" ) );

    stringstream buf;
    ifstream infile;
    string line;
    vector<string> stvec;
    bool showResults = false;
    int index = 0;
    int maxIndex = 0;

    infile.open ( "results.csv" );
    while( getline ( infile, line ) ) {
        stvec.push_back ( line );
        maxIndex += 1;
    }
    for ( ;; )
    {
        ConsoleUi::Element *menu = _ui->popUntilUserInput ( false ); // Clear popped since we don't care about messages

        if ( menu->resultInt < 0 || menu->resultInt >= ( int ) options.size() )
            break;

        switch ( menu->resultInt )
        {
            case 0:
                showResults = true;
                buf.str( "" );
                for ( int i = index; i < index + 10; ++ i) {
                    if ( i < maxIndex )
                        buf << stvec[i] << endl;
                }
                _ui->pushBelow ( new ConsoleUi::TextBox ( buf.str() ) ,
                             { 1, 1 } ); // Don't expand but DO clear top
                //system ( "@pause > nul" );
                break;
            case 1:
                if ( !showResults || index > maxIndex - 10 )
                    break;
                //_ui->pop();
                buf.str( "" );
                if ( index < maxIndex - 10)
                    index += 10;
                for ( int i = index; i < index + 10; ++ i) {
                    if ( i < maxIndex )
                        buf << stvec[i] << endl;
                }
                _ui->pushBelow ( new ConsoleUi::TextBox ( buf.str() ) ,
                                 { 1, 1 } ); // Don't expand but DO clear top
                break;
            case 2:
                if ( !showResults || index < 10 )
                    break;
                //_ui->pop();
                buf.str( "" );
                index -= 10;
                for ( int i = index; i < index + 10; ++ i) {
                    buf << stvec[i] << endl;
                }
                _ui->pushBelow ( new ConsoleUi::TextBox ( buf.str() ) ,
                                 { 1, 1 } ); // Don't expand but DO clear top
                break;

            default:
                break;
        }
    }

    _ui->pop();
}

void MainUi::setMaxRealDelay ( uint8_t delay )
{
    _config.setInteger ( "maxRealDelay", delay );
    saveConfig();
}

void MainUi::setDefaultRollback ( uint8_t rollback )
{
    _config.setInteger ( "defaultRollback", rollback );
    saveConfig();
}

void MainUi::initialize()
{
    _ui.reset ( new ConsoleUi ( uiTitle + LocalVersion.suffix(), ProcessManager::isWine() ) );

    // Configurable settings (defaults)
    _config.setInteger ( "alertOnConnect", 3 );
    _config.setString ( "alertWavFile", "SystemDefault" );
    _config.setString ( "trialAudioCueFile", "SystemDefault" );
    _config.setString ( "displayName", ProcessManager::fetchGameUserName() );
    _config.setInteger ( "fullCharacterName", 0 );
    _config.setInteger ( "highCpuPriority", 1 );
    _config.setInteger ( "versusWinCount", 2 );
    _config.setInteger ( "maxRealDelay", 254 );
    _config.setInteger ( "defaultRollback", 4 );
    _config.setInteger ( "autoCheckUpdates", 1 );
    _config.setInteger ( "autoReplaySave", 1 );
    _config.setInteger ( "frameLimiter", 0 );
    _config.setInteger ( "stageAnimations", 1 );
    _config.setString ( "matchmakingRegion", "NA West" );
    _config.setDouble ( "heldStartDuration", 1.5 );
    _config.setInteger ( "updateChannel", static_cast<int>(MainUpdater::Channel::Stable ) );
    _config.setInteger ( "trialScreenFlashColor", 0xff0000ff );

    // Cached UI state (defaults)
    _config.setInteger ( "lastUsedPort", -1 );
    _config.setInteger ( "lastMainMenuPosition", -1 );
    _config.setInteger ( "lastServerMenuPosition", -1 );
    _config.setInteger ( "lastOfflineMenuPosition", -1 );

    // Load and save main config (this creates the config file on the first time)
    loadConfig();
    saveConfig();

    // Reset the initial config
    initialConfig.clear();
    initialConfig.localName = _config.getString ( "displayName" );
    initialConfig.winCount = _config.getInteger ( "versusWinCount" );
    initialConfig.trialAudioCue = _config.getString ( "trialAudioCueFile" );
    initialConfig.trialFlashColor = _config.getInteger ( "trialScreenFlashColor" );

    // Initialize controllers
    ControllerManager::get().initialize ( this );
    ControllerManager::get().windowHandle = getConsoleWindow();
    ControllerManager::get().check();

    // Setup default mappings
    ControllerManager::get().getKeyboard()->setMappings ( ProcessManager::fetchKeyboardConfig() );
    for ( Controller *controller : ControllerManager::get().getJoysticks() )
        controller->resetToDefaults();

    // Load then save all controller mappings
    ControllerManager::get().loadMappings ( ProcessManager::appDir + FOLDER, MAPPINGS_EXT );
    ControllerManager::get().saveMappings ( ProcessManager::appDir + FOLDER, MAPPINGS_EXT );

    // Initialize updater
    _updater.setTemporal(MainUpdater::Temporal::Latest);
    _updater.setChannel(static_cast<MainUpdater::Channel::Enum>(_config.getInteger("updateChannel")));
}

void MainUi::saveConfig()
{
    const string file = ProcessManager::appDir + CONFIG_FILE;

    LOG ( "Saving: %s", file );

    if ( _config.save ( file ) )
        return;

    const string msg = format ( "Failed to save: %s", file );

    LOG ( "%s", msg );

    if ( sessionError.find ( msg ) == string::npos )
        sessionError += format ( "\n%s", msg );
}

void MainUi::loadConfig()
{
    const string file = ProcessManager::appDir + CONFIG_FILE;

    LOG ( "Loading: %s", file );

    if ( _config.load ( file ) )
        return;

    LOG ( "Failed to load: %s", file );
}

void MainUi::saveMappings ( const Controller& controller )
{
    const string file = ProcessManager::appDir + FOLDER + controller.getName() + MAPPINGS_EXT;

    LOG ( "Saving: %s", file );

    if ( controller.saveMappings ( file ) )
        return;

    const string msg = format ( "Failed to save: %s", file );

    LOG ( "%s", msg );

    if ( sessionError.find ( msg ) == string::npos )
        sessionError += format ( "\n%s", msg );
}

void MainUi::loadMappings ( Controller& controller )
{
    const string file = ProcessManager::appDir + FOLDER + controller.getName() + MAPPINGS_EXT;

    LOG ( "Loading: %s", file );

    if ( controller.loadMappings ( file ) )
        return;

    LOG ( "Failed to load: %s", file );
}

static bool isOnline()
{
    DWORD state;
    InternetGetConnectedState ( &state, 0 );
    return ( state & ( INTERNET_CONNECTION_LAN | INTERNET_CONNECTION_MODEM | INTERNET_CONNECTION_PROXY ) );
}

void MainUi::main ( RunFuncPtr run )
{
    if ( _config.getInteger ( "autoCheckUpdates" ) )
        sessionMessage = getUpdate ( true );

    ASSERT ( _ui.get() != 0 );

    _ui->clearAll();

    int mainSelection = _config.getInteger ( "lastMainMenuPosition" ) - 1;

    for ( ;; )
    {
        const vector<string> options =
        {
            "Netplay",
            "Spectate",
            "Broadcast",
            "Offline",
            "Server",
            "Controls",
            "Settings",
            "Update",
            "Results",
        };

        _ui->pushRight ( new ConsoleUi::Menu ( uiTitle, options, "Quit" ) );

        mainMenu = _ui->top<ConsoleUi::Menu>();
        mainMenu->setEscape ( false );
        mainMenu->setPosition ( mainSelection );

        ConsoleUi::clearScreen();

        string message;

        if ( !sessionError.empty() && !sessionMessage.empty() )
            message = sessionError + "\n" + sessionMessage;
        else if ( ! sessionError.empty() )
            message = sessionError;
        else if ( ! sessionMessage.empty() )
            message = sessionMessage;

        sessionError.clear();
        sessionMessage.clear();

        _ui->pushRight ( new ConsoleUi::TextBox ( message ), { 1, 0 } ); // Expand width

        // Update cached UI state
        initialConfig.localName = _config.getString ( "displayName" );
        initialConfig.winCount = _config.getInteger ( "versusWinCount" );

        // Reset flags
        initialConfig.mode.flags = 0;
        _netplayConfig.clear();

        if ( _address.empty() && _config.getInteger ( "lastUsedPort" ) > 0 )
        {
            _address.port = _config.getInteger ( "lastUsedPort" );
            _address.invalidate();
        }

        mainSelection = _ui->popUntilUserInput()->resultInt;

        if ( mainSelection < 0 || mainSelection >= ( int ) options.size() )
        {
            mainMenu = 0;
            _ui->pop();
            break;
        }

        _ui->clearRight();

        if ( mainSelection >= 0 && mainSelection <= 4 )
        {
            _config.setInteger ( "lastMainMenuPosition", mainSelection + 1 );
            saveConfig();
        }

        switch ( mainSelection )
        {
            case 0:
                netplay ( run );
                break;

            case 1:
                spectate ( run );
                break;

            case 2:
                broadcast ( run );
                break;

            case 3:
                offline ( run );
                break;

            case 4:
                server( run );
                break;

            case 5:
                controls();
                break;

            case 6:
                settings();
                break;

            case 7:
                update();
                break;

            case 8:
                results();
                break;

            default:
                break;
        }

        mainMenu = 0;
        _ui->pop();
    }

    ControllerManager::get().deinitialize();
}

void MainUi::display ( const string& message, bool replace )
{
    if ( replace && ( _ui->empty() || !_ui->top()->requiresUser || _ui->top() != mainMenu ) )
        _ui->pushInFront ( new ConsoleUi::TextBox ( message ), { 1, 0 }, true ); // Expand width and clear top
    else if ( _ui->top() == 0 || _ui->top()->expandWidth() )
        _ui->pushBelow ( new ConsoleUi::TextBox ( message ), { 1, 0 } ); // Expand width
    else
        _ui->pushRight ( new ConsoleUi::TextBox ( message ), { 1, 0 } ); // Expand width
}

void MainUi::alertUser()
{
    int alert = _config.getInteger ( "alertOnConnect" );

    if ( alert & 1 )
        SetForegroundWindow ( ( HWND ) getConsoleWindow() );

    if ( alert & 2 )
    {
        const string buffer = _config.getString ( "alertWavFile" );

        if ( buffer.find ( SYSTEM_ALERT_PREFEX ) == 0 )
            PlaySound ( TEXT ( buffer.c_str() ), 0, SND_ALIAS | SND_ASYNC );
        else
            PlaySound ( TEXT ( buffer.c_str() ), 0, SND_FILENAME | SND_ASYNC | SND_NODEFAULT );
    }
}

bool MainUi::configure ( const PingStats& pingStats )
{
    alertUser();

    bool ret = false;

    ASSERT ( _ui.get() != 0 );

    const int delay = computeDelay ( pingStats.latency.getMean() );
    const int worst = computeDelay ( pingStats.latency.getWorst() );
    const int variance = computeDelay ( pingStats.latency.getVariance() );

    int rollback = clamped ( delay + worst + variance, 0, _config.getInteger ( "defaultRollback" ) );

    _netplayConfig.delay = worst + 1;

    // TODO maybe implement this as a slider or something

    _ui->pushBelow ( new ConsoleUi::Prompt ( ConsoleUi::Prompt::Integer, "Enter max frames of rollback:" ) );

    _ui->top<ConsoleUi::Prompt>()->allowNegative = false;
    _ui->top<ConsoleUi::Prompt>()->maxDigits = 2;
    _ui->top<ConsoleUi::Prompt>()->setInitial ( rollback );

    for ( ;; )
    {
        ConsoleUi::Element *menu = _ui->popUntilUserInput();

        if ( menu->resultInt < 0 )
        {
            _ui->pop();
            break;
        }

        if ( menu->resultInt > MAX_ROLLBACK )
        {
            _ui->pushRight ( new ConsoleUi::TextBox ( format ( ERROR_INVALID_ROLLBACK, 1 + MAX_ROLLBACK ) ) );
            continue;
        }

        _ui->clearTop();

        rollback = _netplayConfig.rollback = menu->resultInt;

        _ui->pushInFront ( new ConsoleUi::Prompt ( ConsoleUi::Prompt::Integer, "Enter input delay:" ) );

        _ui->top<ConsoleUi::Prompt>()->allowNegative = false;
        _ui->top<ConsoleUi::Prompt>()->maxDigits = 3;
        _ui->top<ConsoleUi::Prompt>()->setInitial ( clamped ( delay - rollback, 0, delay ) );

        for ( ;; )
        {
            menu = _ui->popUntilUserInput();

            if ( menu->resultInt < 0 )
                break;

            if ( menu->resultInt >= 0xFF )
            {
                _ui->pushRight ( new ConsoleUi::TextBox ( ERROR_INVALID_DELAY ) );
                continue;
            }

            if ( rollback )
                _netplayConfig.rollbackDelay = menu->resultInt;
            else
                _netplayConfig.delay = menu->resultInt;

            _netplayConfig.winCount = _config.getInteger ( "versusWinCount" );
#ifdef RELEASE
            _netplayConfig.hostPlayer = 1 + ( rand() % 2 );
#else
            _netplayConfig.hostPlayer = 1; // Host is always player 1 for easier debugging
#endif

            ret = true;
            _ui->pop();
            break;
        }

        _ui->pop();
        if ( ret )
            break;
    }

    if ( ret )
    {
        _ui->pushBelow ( new ConsoleUi::TextBox ( format ( "Using %u delay%s",
                         ( _netplayConfig.rollback ? _netplayConfig.rollbackDelay : _netplayConfig.delay ),
                         ( _netplayConfig.rollback ? format ( ", %u rollback", _netplayConfig.rollback ) : "" ) ) ),
        { 1, 0 } ); // Expand width
    }

    return ret;
}

bool MainUi::connected ( const InitialConfig& initialConfig, const PingStats& pingStats )
{
    ASSERT ( _ui.get() != 0 );

    const string connectString = ( initialConfig.mode.isHost()
                                   ? initialConfig.remoteName + " connected"
                                   : "Connected to " + initialConfig.remoteName );

    const string modeString = ( initialConfig.mode.isTraining()
                                ? "Training mode"
                                : format ( "Versus mode, each game is %u rounds", initialConfig.winCount ) );

    _ui->pushInFront ( new ConsoleUi::TextBox (
                           connectString
                           + "\n\n" + modeString
                           + "\n\n" + formatStats ( pingStats ) ), { 1, 0 }, true ); // Expand width and clear top

    if ( configure ( pingStats ) )
    {
        display ( string ( "Waiting for " ) + ( initialConfig.mode.isHost() ? "client" : "host" ) + "...", false );
        return true;
    }

    return false;
}

void MainUi::spectate ( const SpectateConfig& spectateConfig )
{
    alertUser();

    ASSERT ( _ui.get() != 0 );

    string text;

    if ( spectateConfig.mode.isBroadcast() )
    {
        initialConfig.mode.value = ClientMode::SpectateBroadcast;
        text = format ( "Spectating a %s mode broadcast (0 delay)\n\n",
                        ( spectateConfig.mode.isTraining() ? "training" : "versus" ) );

    }
    else
    {
        initialConfig.mode.value = ClientMode::SpectateNetplay;
        text = format ( "Spectating %s mode (%u delay%s)\n\n",
                        ( spectateConfig.mode.isTraining() ? "training" : "versus" ), spectateConfig.delay,
                        ( spectateConfig.rollback ? format ( ", %u rollback", spectateConfig.rollback ) : "" ) );
    }

    CharaNameFunc charaNameFunc = ( _config.getInteger ( "fullCharacterName" ) ? getFullCharaName : getShortCharaName );

    if ( spectateConfig.initial.netplayState <= NetplayState::CharaSelect )
    {
        if ( spectateConfig.mode.isBroadcast() )
            text += "Currently selecting characters...";
        else
            text += format ( "%s vs %s", spectateConfig.names[0], spectateConfig.names[1] );
    }
    else
    {
        text += spectateConfig.formatPlayer ( 1, charaNameFunc )
                + " vs " + spectateConfig.formatPlayer ( 2, charaNameFunc );
    }

    text += "\n\n(Tap the spacebar to toggle fast-forward)";

    _ui->pushInFront ( new ConsoleUi::TextBox ( text ), { 1, 0 }, true ); // Expand width and clear top
}

bool MainUi::confirm ( const string& question )
{
    bool ret = false;

    ASSERT ( _ui.get() != 0 );

    _ui->pushBelow ( new ConsoleUi::Menu ( question, { "Yes" }, "No" ) );

    ret = ( _ui->popUntilUserInput()->resultInt == 0 );

    _ui->pop();

    return ret;
}

void *MainUi::getConsoleWindow()
{
    return ConsoleUi::getConsoleWindow();
}

string MainUi::formatStats ( const PingStats& pingStats )
{
    return format (
               "%-" INDENT_STATS "s Ping: %.2f ms"
#ifndef NDEBUG
               "\n%-" INDENT_STATS "s Worst: %.2f ms"
               "\n%-" INDENT_STATS "s StdErr: %.2f ms"
               "\n%-" INDENT_STATS "s StdDev: %.2f ms"
               "\n%-" INDENT_STATS "s Packet Loss: %d%%"
#endif
               , format ( "Network delay: %d", computeDelay ( pingStats.latency.getMean() ) )
               , pingStats.latency.getMean()
#ifndef NDEBUG
               , "", pingStats.latency.getWorst()
               , "", pingStats.latency.getStdErr()
               , "", pingStats.latency.getStdDev()
               , "", pingStats.packetLoss
#endif
           );
}

string MainUi::getUpdate ( bool isStartup )
{
    if ( ! isOnline() )
    {
        return "No Internet connection";
    }

    AutoManager _;

    fetch ( MainUpdater::Type::Version );

    if ( _updater.getTargetVersion().empty() )
    {
        return "Cannot fetch info for " + _updater.getTargetDescName() + " version";
    }

    if ( LocalVersion.isSimilar ( _updater.getTargetVersion(), ( _config.getInteger("updateChannel") - 1 ) ? 3 : 2 ) )
    {
        _upToDate = true;
        if ( ! isStartup )
            return "You already have the " + _updater.getTargetDescName() + " version";
        return "";
    }

    for ( ;; )
    {
        std::string name = _updater.getTargetDescName();
        name[0] = std::toupper(name[0]);
        if (!isStartup) _ui->pop();
        string revision = _updater.getTargetVersion().revision;
        string revisionString = revision == "" ? "" : "-" + revision;
        _ui->pushBelow ( new ConsoleUi::TextBox ( format (
                             "%s" + name + " version is %s%s",
                             /* sessionMessage.empty() ? */ "" /* : ( sessionMessage + "\n" ) */,
                             _updater.getTargetVersion(),
                             revision ) ) );

        sessionMessage.clear();

        _ui->pushBelow ( new ConsoleUi::Menu ( "Update?", { "Yes", "View changes" }, "No" ) );

        const int ret = _ui->popUntilUserInput()->resultInt;

        _ui->pop();
        if (isStartup) _ui->pop();

        if ( ret == 0 )         // Yes
        {
            fetch ( MainUpdater::Type::Archive );
            return "";
        }
        else if ( ret == 1 )    // View changes
        {
            fetch ( MainUpdater::Type::ChangeLog );
            continue;
        }
        else                    // No
        {
            return "";
        }
    }
}

void MainUi::update()
{
    static const vector<string> options =
    {
        "Update to latest version",
        "Download previous version",
        "Change update type",
        "View changes",
    };

    _ui->pushRight(new ConsoleUi::Menu("Update", options, "Back"));
    std::string msg = "";
    for (;;) {
        if (!msg.empty()) _ui->pushBelow(new ConsoleUi::TextBox(msg));
        ConsoleUi::Element *menu = _ui->popUntilUserInput(false); // Don't clear popped elements yet

        _ui->clearRight();
        _ui->clearBelow(true);

        if (menu->resultInt < 0 || menu->resultInt >= (int)options.size())
            break;

        switch (menu->resultInt) {
        case 0:
            _ui->pushBelow(new ConsoleUi::TextBox("Checking for updates..."));
            msg = getUpdate();
            sessionMessage = msg;
            _ui->pop();
            break;
        case 1:
            _ui->pushBelow(new ConsoleUi::TextBox("Checking for updates..."));
            _updater.setTemporal(MainUpdater::Temporal::Previous);
            msg = getUpdate();
            sessionMessage = msg;
            _ui->pop();
            _updater.setTemporal(MainUpdater::Temporal::Latest);
            break;
        case 2:
            _ui->pushInFront(new ConsoleUi::Menu("Select update type",
                                                 {"Stable", "Dev"}, "Cancel"),
                             {0, 0}, true); // Don't expand but DO clear top

            int oldChannel;
            oldChannel = _config.getInteger("updateChannel");
            int p;
            switch (static_cast<MainUpdater::Channel::Enum>(oldChannel)) {
            case MainUpdater::Channel::Stable:
                p = 0;
                break;
            case MainUpdater::Channel::Dev:
                p = 1;
                break;
            default:
                p = 0;
                break;
            }
            _ui->top<ConsoleUi::Menu>()->setPosition(p);
            _ui->popUntilUserInput(false);

            if (_ui->top()->resultInt >= 0 && _ui->top()->resultInt <= 1) {
                int c = -1;
                switch (_ui->top()->resultInt) {
                case 0:
                    _updater.setChannel(MainUpdater::Channel::Stable);
                    c = MainUpdater::Channel::Stable;
                    break;
                case 1:
                    _updater.setChannel(MainUpdater::Channel::Dev);
                    c = MainUpdater::Channel::Dev;
                    break;
                default: break;
                }
                _config.setInteger("updateChannel", c);
                saveConfig();
                if (_config.getInteger("updateChannel") != oldChannel) {
                    _ui->pushBelow(new ConsoleUi::TextBox("Checking for updates..."));
                    msg = getUpdate();
                    sessionMessage = msg;
                    _ui->pop();
                } else msg = "";
            }

            _ui->pop();
            break;
        case 3:
            openChangeLog();
            break;
        default:
            break;
        }
    }
    _ui->pop();
}

void MainUi::openChangeLog()
{
    const DWORD val = GetFileAttributes ( ( ProcessManager::appDir + FOLDER CHANGELOG ).c_str() );

    if ( val == INVALID_FILE_ATTRIBUTES )
    {
        sessionMessage = "Missing: " FOLDER CHANGELOG;
        LOG ( "%s", sessionMessage );
        return;
    }

    const string file = ProcessManager::appDir + FOLDER + CHANGELOG;

    if ( ProcessManager::isWine() )
        system ( ( "notepad " + file ).c_str() );
    else
        system ( ( "\"start \"Viewing change log\" notepad " + file + "\"" ).c_str() );
}

void MainUi::sendConnected() {
    if ( _lobby ) {
        _lobby->accept();
    }
}

void MainUi::fetch ( const MainUpdater::Type& type )
{
    if ( type != MainUpdater::Type::Version )
        _ui->pushBelow ( new ConsoleUi::ProgressBar ( "Downloading...", 20 ) );

    _updater.fetch ( type );

    EventManager::get().start();

    if ( type != MainUpdater::Type::Version )
        _ui->pop();
}

void MainUi::fetchCompleted ( MainUpdater *updater, const MainUpdater::Type& type )
{
    ASSERT ( updater == &_updater );

    EventManager::get().stop();

    switch ( type.value )
    {
        case MainUpdater::Type::Version:
        default:
            break;

        case MainUpdater::Type::ChangeLog:
            _updater.openChangeLog();
            break;

        case MainUpdater::Type::Archive:
            // TODO see if there's a better way to do this
            if ( ProcessManager::isWine() )
            {
                _ui->pushBelow ( new ConsoleUi::TextBox (
                                     "Please extract update.zip to update.\n"
                                     "Press any key to exit." ) );
                system ( "@pause > nul" );
                exit ( 0 );
                return;
            }

            _updater.extractArchive();
            break;
    }
}

void MainUi::fetchFailed ( MainUpdater *updater, const MainUpdater::Type& type )
{
    ASSERT ( updater == &_updater );

    EventManager::get().stop();

    switch ( type.value )
    {
        case MainUpdater::Type::Version:
            sessionMessage = "Cannot fetch info for " + _updater.getTargetDescName() + " version";
            break;

        case MainUpdater::Type::ChangeLog:
            sessionMessage = "Cannot fetch latest change log";
            break;

        case MainUpdater::Type::Archive:
            sessionMessage = "Cannot download " + _updater.getTargetDescName() + " version";
            break;

        default:
            break;
    }
}

void MainUi::fetchProgress ( MainUpdater *updater, const MainUpdater::Type& type, double progress )
{
    if ( type == MainUpdater::Type::Version )
        return;

    const ConsoleUi::ProgressBar *bar = _ui->top<ConsoleUi::ProgressBar>();

    bar->update ( bar->length * progress );
}

void MainUi::connectionFailed ( Lobby *lobby )
{
    LOG( "mainUI Lobby Connection Failed" );
    EventManager::get().stop();
    if ( !_lobby->connectionSuccess )
        unlock( lobby );
}

void MainUi::unlock ( Lobby *lobby )
{
    LOG( "mainUI lobby unlock" );
    LOCK ( uiMutex );
    uiCondVar.signal();
}

void MainUi::connectionFailed ( MatchmakingManager *mmm )
{
    LOG( "mainUI mmm Connection Failed" );
    EventManager::get().stop();
    LOCK ( uiMutex );
    if ( !mmm->connectionSuccess ) {
        uiCondVar.signal();
    }
    if ( !mmm->matchSuccess ) {
        uiCondVar.signal();
    }
}

void MainUi::unlock ( MatchmakingManager *mmm )
{
    LOG( "mainUI mmm unlock" );
    LOCK ( uiMutex );
    uiCondVar.signal();
}


void MainUi::setAddr ( MatchmakingManager *mmm, string addr )
{
    _address = addr;
}


void MainUi::setMode ( MatchmakingManager *mmm, string mode )
{
    if ( mode == "Host" ) {
        initialConfig.mode.value = ClientMode::Host;
    } else if ( mode == "Client" ) {
        initialConfig.mode.value = ClientMode::Client;
    } else {
        initialConfig.mode.value = ClientMode::Offline;
    }
    LOG(initialConfig.mode.value);
}

void MainUi::hostReady() {
    if ( _mmm && _mmm->isRunning() && isMatchmaking ) {
        //Mutex host = _mmm->hostMutex;
        //LOCK( host );
        //_mmm->hostCondVar.signal();
        _mmm->sendHostReady();
    }
}

