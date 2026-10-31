#include "DllControllerManager.hpp"
#include "DllOverlayUi.hpp"
#include "DllHacks.hpp"
#include "DllAsmHacks.hpp"
#include "DllTrialManager.hpp"
#include "KeyboardState.hpp"
#include "CharacterSelect.hpp"
#include "DllFrameRate.hpp"

#include <windows.h>

using namespace std;


#define VK_TOGGLE_TRIAL_MENU ( VK_F3 )
#define VK_TOGGLE_OVERLAY ( VK_F4 )
#define VK_ENABLE_FRAMESTEP ( VK_F5 )

extern bool stopping;


void DllControllerManager::initControllers ( const ControllerMappings& mappings )
{
    Lock lock ( ControllerManager::get().mutex );

    ControllerManager::get().getKeyboard()->setMappings ( ProcessManager::fetchKeyboardConfig() );
    ControllerManager::get().setMappings ( mappings );
    ControllerManager::get().check();

    _allControllers = ControllerManager::get().getControllers();

    // Set the owner (Which enables callbacks) AFTER we have the list of all controllers
    ControllerManager::get().owner = this;

    // The first controller is keyboard which is always attached.
    // So we only set this flag for controllers beyond the first one.
    _controllerAttached = ( _allControllers.size() > 1 );
}

bool DllControllerManager::isNotMapping() const
{
    Lock lock ( ControllerManager::get().mutex );

    return  ( !_playerControllers[0] || !_playerControllers[0]->isMapping() )
            && ( !_playerControllers[1] || !_playerControllers[1]->isMapping() );
}

void DllControllerManager::updateControls ( uint16_t *localInputs )
{

    if ( stopping )
        return;

    Lock lock ( ControllerManager::get().mutex );

    bool toggleTrialMenu = false;
    bool toggleOverlay = false;

    // // Automatically show overlay when a controller is attached during chara select
    // if ( _controllerAttached )
    // {
    //     _controllerAttached = false;

    //     if ( !DllOverlayUi::isEnabled() && *CC_GAME_MODE_ADDR == CC_GAME_MODE_CHARA_SELECT )
    //         toggleOverlay = true;
    // }

    // Toggle with a keyboard hotkey
    if ( KeyboardState::isPressed ( VK_TOGGLE_TRIAL_MENU ) &&
         *CC_GAME_MODE_ADDR == CC_GAME_MODE_IN_GAME )
    {
        toggleTrialMenu = true;
    }
    if ( KeyboardState::isPressed ( VK_TOGGLE_OVERLAY ) && !chatManPtr->isTyping)
    {
        toggleOverlay = true;
    }
    if ( KeyboardState::isPressed ( VK_ENABLE_FRAMESTEP ) )
    {
        framestepEnabled = true;
    }

	if(KeyboardState::isDown(VK_LCONTROL) && KeyboardState::isPressed(VK_LSHIFT) && KeyboardState::isDown(VK_NUMPAD0)) {
		log("`ctrl + shift + numpad0 pressed, crashing intentionally");

		int* _temp = NULL;
		*_temp = 0;
	}

	if(KeyboardState::isDown(VK_MENU) && KeyboardState::isPressed(VK_LSHIFT) && KeyboardState::isDown(VK_NUMPAD1)) {
		log("`toggling fps limit");

		if(DllFrameRate::desiredFps > 100) {
			setDesiredFPS(60.0);
		} else {
			setDesiredFPS(17474.0);
		}
	}
	
	if ( !chatManPtr->isTyping 
		&& KeyboardState::isPressed ( VK_ENBABLE_CHAT ) 
		&& (!DllOverlayUi::isEnabled() && !DllOverlayUi::isToggling())
	)
    {
		// its funny, ive become so indecisive over the last year. i know what to do, but thinking removes my ability to do it
        chatManPtr->startTyping();
    }

	if(chatManPtr->isTyping) {
		chatManPtr->typeMessage();
	}

    for ( Controller *controller : _allControllers )
    {
        // Don't sticky controllers if the overlay is enabled
        if ( DllOverlayUi::isEnabled() )
            continue;

        // Sticky controllers to the first available player when anything is pressed
        if ( getInput ( controller ) )
        {
            if ( isSinglePlayer )
            {
                // Only sticky the local player in single player modes
                if ( !_playerControllers[localPlayer - 1] && controller != _playerControllers[localPlayer - 1] )
                {
                    _playerControllers[localPlayer - 1] = controller;
                }
            }
            else
            {
                // Sticky player 1 then player 2 optimistically
                if ( !_playerControllers[0] && controller != _playerControllers[1] )
                {
                    _playerControllers[0] = controller;
                }
                else if ( !_playerControllers[1] && controller != _playerControllers[0] )
                {
                    _playerControllers[1] = controller;
                }
            }
        }

        // Toggle with 3 held joystick buttons + any direction during trial mode
        if ( *CC_GAME_MODE_ADDR == CC_GAME_MODE_IN_GAME
             && numJoystickButtonsDown ( controller ) >= 3
             && !controller->getJoystickState().isNeutral()
             && isTrial )
        {
            toggleTrialMenu = true;
        }
        // Toggle with 3 held joystick buttons + any direction during chara select
        if ( *CC_GAME_MODE_ADDR == CC_GAME_MODE_CHARA_SELECT
                && numJoystickButtonsDown ( controller ) >= 3
                && !controller->getJoystickState().isNeutral() )
        {
            toggleOverlay = true;
        }
    }

    // Show message takes priority over the overlay UI
    if ( DllOverlayUi::isShowingMessage() )
    {
        DllOverlayUi::updateMessage();
    }
    else if ( toggleTrialMenu )
    {
        toggleTrialMenu = false;

        if ( ! DllOverlayUi::isEnabled() )
        {
            // Refresh the list of joysticks if we're enabling the overlay
            ControllerManager::get().refreshJoysticks();

            // Enable keyboard events, this effectively eats all keyboard inputs for the window
            KeyboardManager::get().keyboardWindow = DllHacks::windowHandle;
            KeyboardManager::get().matchedKeys.clear();
            KeyboardManager::get().ignoredKeys.clear();
            KeyboardManager::get().hook ( this, true );

            // Disable Escape to exit
            AsmHacks::enableEscapeToExit = false;
            if ( TrialManager::isRecording ) {
                TrialManager::isRecording = false;
                TrialManager::saveTrial();
            }

            // Enable overlay
            DllOverlayUi::setTrial();
            DllOverlayUi::enable();
        }
        else if ( DllOverlayUi::isTrial() )
        {
            disableTrialMenuOverlay();
        }
    }
    else if ( toggleOverlay )
    {
        toggleOverlay = false;

        if ( ! DllOverlayUi::isEnabled() )
        {
            // Refresh the list of joysticks if we're enabling the overlay
            ControllerManager::get().refreshJoysticks();

            // Enable keyboard events, this effectively eats all keyboard inputs for the window
            KeyboardManager::get().keyboardWindow = DllHacks::windowHandle;
            KeyboardManager::get().matchedKeys.clear();
            KeyboardManager::get().ignoredKeys.clear();
            KeyboardManager::get().hook ( this, true );

            // Disable Escape to exit
            AsmHacks::enableEscapeToExit = false;

            // Enable overlay
            DllOverlayUi::setMapping();
            DllOverlayUi::enable();
        }
        else if ( DllOverlayUi::isMapping() )
        {
            // Cancel all mapping if we're disabling the overlay
            if ( _playerControllers[0] )
                _playerControllers[0]->cancelMapping();
            if ( _playerControllers[1] )
                _playerControllers[1]->cancelMapping();

            _overlayPositions[0] = 0;
            _overlayPositions[1] = 0;

            // Disable keyboard events, since we use GetKeyState for regular controller inputs
            KeyboardManager::get().unhook();

            // Re-enable Escape to exit
            AsmHacks::enableEscapeToExit = true;

            // Disable overlay
			//DllOverlayUi::setNoneMode();
            DllOverlayUi::disable();
        }
    }

    // Only update player controls when the overlay is NOT enabled
    if ( !DllOverlayUi::isEnabled() )
    {
        if ( _playerControllers[localPlayer - 1] ) {
            uint16_t input = getInput ( _playerControllers[localPlayer - 1] );
            if ( localPlayer == 1 ) {
                if ( *CC_P1_FACING_FLAG_ADDR )
                    input |= COMBINE_INPUT ( 0, CC_PLAYER_FACING );
            }
            if ( localPlayer == 2) {
                if ( *CC_P2_FACING_FLAG_ADDR )
                    input |= COMBINE_INPUT ( 0, CC_PLAYER_FACING );
            }
            localInputs[0] = input;
        }

        if ( _playerControllers[remotePlayer - 1] ) {
            uint16_t input = getInput ( _playerControllers[remotePlayer - 1] );
            if ( remotePlayer == 1 ) {
                if ( *CC_P1_FACING_FLAG_ADDR )
                    input |= COMBINE_INPUT ( 0, CC_PLAYER_FACING );
            }
            if ( remotePlayer == 2) {
                if ( *CC_P2_FACING_FLAG_ADDR )
                    input |= COMBINE_INPUT ( 0, CC_PLAYER_FACING );
            }
            localInputs[1] = input;
        }
        return;
    }

    if ( DllOverlayUi::isTrial() )
    {
        handleTrialMenuOverlay();
    }
    else if ( DllOverlayUi::isMapping() )
    {
        handleMappingOverlay();
    }

	
}

void DllControllerManager::disableTrialMenuOverlay()
{
    _trialMenuSelection = 0;
    _trialSubMenuSelection = 0;
    DllOverlayUi::updateSelector ( 0 );
    DllOverlayUi::updateSelector ( 1 );
    _trialOverlayPositions[0] = 0;
    _trialOverlayPositions[1] = 0;
    _trialOverlayPositions[2] = 0;
    _trialMenuIndex = 0;

    TrialManager::inputEditorEnabled = false;

    // Disable keyboard events, since we use GetKeyState for regular controller inputs
    KeyboardManager::get().unhook();

    // Re-enable Escape to exit
    AsmHacks::enableEscapeToExit = true;

    // Disable overlay
    DllOverlayUi::disable();
}

void DllControllerManager::handleInputEditor()
{
    int input = 0;
    for ( Controller *controller : _allControllers )
    {
        if ( ( controller->isJoystick() && isDirectionPressed ( controller, 8 ) )
                || ( controller->isKeyboard() && KeyboardState::isPressed ( VK_UP ) ) ) {
            // Up input
            input = 8;
            break;
        } else if ( ( controller->isJoystick() && isDirectionPressed ( controller, 4 ) )
                  || ( controller->isKeyboard() && KeyboardState::isPressed ( VK_LEFT ) ) ) {
            // Left input
            input = 4;
            break;
        } else if ( ( controller->isJoystick() && isDirectionPressed ( controller, 6 ) )
                || ( controller->isKeyboard() && KeyboardState::isPressed ( VK_RIGHT ) ) ) {
            // Right input
            input = 6;
            break;
        } else if ( ( controller->isJoystick() && isDirectionPressed ( controller, 2 ) )
                  || ( controller->isKeyboard() && KeyboardState::isPressed ( VK_DOWN ) ) ) {
            // Down input
            input = 2;
            break;
        } else if ( ( controller->isJoystick() && isButtonPressed ( controller, CC_BUTTON_A ) )
                    || ( controller->isKeyboard() && isButtonPressed( controller, CC_BUTTON_A ) ) ) {
            input = 9;
            break;
        } else if ( ( controller->isJoystick() && isButtonPressed ( controller, CC_BUTTON_B ) )
                || ( controller->isKeyboard() && isButtonPressed( controller, CC_BUTTON_B ) ) ) {
            input = 10;
            break;
        } else if ( ( controller->isJoystick() && isButtonPressed ( controller, CC_BUTTON_E ) )
                    || ( controller->isKeyboard() && isButtonPressed( controller, CC_BUTTON_E ) ) ) {

            input = 11;
        }
        if ( ( controller->isJoystick() && isButtonPressed ( controller, CC_BUTTON_C ) )
                || ( controller->isKeyboard() && isButtonPressed( controller, CC_BUTTON_C ) ) ) {
            if ( TrialManager::inputEditorSpeed == 100 ) {
                TrialManager::inputEditorSpeed = 1;
            } else {
                TrialManager::inputEditorSpeed *= 10 ;
            }
        } else if ( ( controller->isJoystick() && isButtonPressed ( controller, CC_BUTTON_D ) )
                || ( controller->isKeyboard() && isButtonPressed( controller, CC_BUTTON_D ) ) ) {
            input = 12;
        }
    }
    int numYCells = 13;
    if ( input == 2 ) {
        if ( TrialManager::inputEditorY > numYCells - 1 ||
             TrialManager::inputEditorSpeed > 1 ) {
            TrialManager::inputEditorPosition += TrialManager::inputEditorSpeed;
        } else {
            TrialManager::inputEditorY += 1;
        }
        //TrialManager::inputEditorY %= 14;
        if ( TrialManager::inputEditorPosition > 5900 ) {
            TrialManager::inputEditorPosition = 5900;
        }
    }
    if ( input == 8 ) {
        if ( ( TrialManager::inputEditorY == 0 &&
               TrialManager::inputEditorPosition > 0 ) ||
             TrialManager::inputEditorSpeed > 1 ) {
            TrialManager::inputEditorPosition -= TrialManager::inputEditorSpeed;
            if ( TrialManager::inputEditorPosition < 0 ) {
                TrialManager::inputEditorPosition = 0;
            }
        } else {
            TrialManager::inputEditorY += 13;
            TrialManager::inputEditorY %= 14;
        }
    }
    if ( input == 4 ) {
        TrialManager::inputEditorX += 8;
        TrialManager::inputEditorX %= 9;
    }
    if ( input == 6 ) {
        TrialManager::inputEditorX += 1;
        TrialManager::inputEditorX %= 9;
    }
    int currentRow = TrialManager::inputEditorY + TrialManager::inputEditorPosition;
    if ( input == 9 ) {
        if ( TrialManager::inputEditorX == 0 ) {
            TrialManager::inputEditorBuffer[currentRow] &= ~(1 << 2);
        }
        if ( TrialManager::inputEditorX == 2 ) {
            TrialManager::inputEditorBuffer[currentRow] &= ~(1 << 0);
        }
        if ( TrialManager::inputEditorX == 1 ) {
            TrialManager::inputEditorBuffer[currentRow] &= ~(1 << 3);
        }
        if ( TrialManager::inputEditorX == 3 ) {
            TrialManager::inputEditorBuffer[currentRow] &= ~(1 << 1);
        }
        TrialManager::inputEditorBuffer[currentRow] ^= 1 << TrialManager::inputEditorX;
    }
    if ( input == 10 ) {
        saveEdits();
        DllOverlayUi::enable();
        TrialManager::showCombo = true;
        TrialManager::inputEditorEnabled = false;
        _trialMenuSelection = 0;
        TrialManager::inputEditorX = 0;
        TrialManager::inputEditorY = 0;
    }
    if ( input == 11 ) {
        deleteTrialRow( currentRow );
    }
    if ( input == 12 ) {
        insertTrialRow( currentRow );
    }
    ControllerManager::get().savePrevStates();
}

void DllControllerManager::deleteTrialRow( int row )
{
    for ( int i = row; i < TrialManager::maxTrialInputs - 2; ++i ) {
        TrialManager::inputEditorBuffer[i] = TrialManager::inputEditorBuffer[i+1];
    }
    TrialManager::inputEditorBuffer[TrialManager::maxTrialInputs-1] = 0;
}

void DllControllerManager::insertTrialRow( int row )
{
    for ( int i = TrialManager::maxTrialInputs - 2; i > row; --i ) {
        TrialManager::inputEditorBuffer[i] = TrialManager::inputEditorBuffer[i-1];
    }
    TrialManager::inputEditorBuffer[row] = 0;
}

void DllControllerManager::saveEdits()
{
    LOG("SaveEdits");
    vector<uint16_t>* inputs = &(TrialManager::charaTrials[TrialManager::currentTrialIndex].demoInputs);
    inputs->clear();
    int paddingCount = 0;
    for ( int i = 0; i < 5940; ++i ) {
        uint16_t bufferInput = TrialManager::inputEditorBuffer[i];
        if ( bufferInput ) {
            for ( int j = 0; j < paddingCount; ++j ) {
                inputs->push_back( 0 );
            }
            LOG(bufferInput);
            inputs->push_back( TrialManager::unconvertInputEditor( bufferInput ) );
            paddingCount = 0;
        } else {
            paddingCount += 1;
        }
    }
    TrialManager::saveTrial();
}

void DllControllerManager::handleTrialMenuOverlay()
{
    array<string, 3> text;
    text[0] = "Press Up/Down to choose entry\n";
    text[0] += "Press Right to select\n";
    text[0] += "Press Left to cancel\n";
    text[0] += "\n";

    enum Options {
        TrialSelect = 1,
        Demo,
        RecordDemo,
        EditDemo,
        InputGuide,
        Interface,
        GuideOptions,
        Exit
    };

    array<vector<string>, 2> options;
    options[0].push_back( "Trial Select\n" );
    options[0].push_back( "Demo\n" );
    options[0].push_back( "Record Demo\n" );
    options[0].push_back( "Edit Demo\n" );
    options[0].push_back( "Input Guide\n" );
    options[0].push_back( "Interface Size\n" );
    options[0].push_back( "Input Guide Options\n" );
    options[0].push_back( "Exit\n" );

    if ( _trialMenuSelection == Options::TrialSelect ) {
        if ( TrialManager::charaTrials.size() > 0 ) {
            if ( TrialManager::charaTrials.size() > 10 ) {
                if ( _trialScrollSelect == 0 ) {
                    text[2] = "Trials\n\n";
                } else {
                    text[2] = "Trials\n...\n";
                }
                for ( int i = 0; i < 10; ++i ) {
                    Trial t = TrialManager::charaTrials[ _trialScrollSelect + i ];
                    text[2] += t.name;
                    text[2] += "\n";
                    options[1].push_back( t.name );
                }
                if ( _trialScrollSelect < TrialManager::charaTrials.size() - 10 ) {
                    text[2] += "...\n";
                } else {
                    text[2] += "\n";
                }
            } else {
                text[2] = "Trials\n\n";
                for ( Trial t : TrialManager::charaTrials ) {
                    text[2] += t.name;
                    text[2] += "\n";
                    options[1].push_back( t.name );
                }
            }
        } else {
            text[2] = "No trials found\n\n";
            text[2] += "Press Left to return";
            options[1].push_back( "Press Left to return\n" );
        }
    } else if ( _trialMenuSelection == Options::Demo ) {
        text[2] = "No demo exists for this trial\n\n";
        text[2] += "Press Left to return";
        options[1].push_back( "Press Left to return\n" );
    } else if ( _trialMenuSelection == Options::RecordDemo ) {
        if ( TrialManager::charaTrials[TrialManager::currentTrialIndex].demoInputs.empty() ) {
            text[2] = "No trial currently selected\n\n";
            text[2] += "Press Left to return";
            options[1].push_back( "Press Left to return\n" );
        } else {
            text[2] = "A demo already exists for this trial. Overwrite?\n\n";
            text[2] += "Yes\n";
            text[2] += "No\n";
            options[1].push_back( "Yes" );
            options[1].push_back( "No" );
        }
    } else if ( _trialMenuSelection == Options::InputGuide ) {
        options[1].push_back( "Press Left to return\n" );
    } else if ( _trialMenuSelection == Options::Interface ) {
        text[2] = "Select interface size\n\n";
        text[2] += "Large\n";
        text[2] += "Medium\n";
        text[2] += "Small\n";
        options[1].push_back( "Large" );
        options[1].push_back( "Medium" );
        options[1].push_back( "Small" );
    } else if ( _trialMenuSelection == Options::GuideOptions ) {
        text[2] = "Input guide options\n\n";
        string audioText = "Play audio cue: ";
        audioText += ( TrialManager::playAudioCue ? "Enabled" : "Disabled" );
        string flashText = "Flash screen: ";
        flashText += ( TrialManager::playScreenFlash ? "Enabled" : "Disabled" );
        text[2] += audioText + "\n";
        text[2] += flashText + "\n";
        options[1].push_back( audioText );
        options[1].push_back( flashText );
    } else if ( _trialMenuSelection == Options::EditDemo ) {
        //DllOverlayUi::disable();
        text[0] = text[1] = text[2] = "";
        TrialManager::showCombo = false;
        _trialMenuIndex = 0;
        TrialManager::inputEditorEnabled = true;
        DllOverlayUi::updateText ( text );
        handleInputEditor();
        return;
    }

    vector<string> demooptions;
    vector<string> recordoptions;

    for( string option : options[0] ) {
        text[0] += option;
    }

    int headerOffset = 4;
    int subheaderOffset = 2;

    // Get first input from any controller
    uint8_t input = 0;
    for ( Controller *controller : _allControllers )
    {
        if ( ( controller->isJoystick() && isDirectionPressed ( controller, 8 ) )
                || ( controller->isKeyboard() && KeyboardState::isPressed ( VK_UP ) ) ) {
            // Up input
            input = 8;
            break;
        } else if ( ( controller->isJoystick() && isDirectionPressed ( controller, 4 ) )
                  || ( controller->isKeyboard() && KeyboardState::isPressed ( VK_LEFT ) ) ) {
            // Left input
            LOG("Left");
            input = 4;
            break;
        } else if ( ( controller->isJoystick() && isDirectionPressed ( controller, 6 ) )
                || ( controller->isKeyboard() && KeyboardState::isPressed ( VK_RIGHT ) ) ) {
            // Right input
            LOG("Right");
            input = 6;
            break;
        } else if ( ( controller->isJoystick() && isDirectionPressed ( controller, 2 ) )
                  || ( controller->isKeyboard() && KeyboardState::isPressed ( VK_DOWN ) ) ) {
            // Down input
            LOG("Down");
            input = 2;
            break;
        }
    }

    if ( input == 2 ) {
        if ( _trialMenuSelection == Options::TrialSelect ) {
            if ( _trialOverlayPositions[1] == 9 ) {
                if ( _trialScrollSelect < TrialManager::charaTrials.size() - 10 ) {
                    _trialScrollSelect += 1;
                }
            } else {
                _trialOverlayPositions[_trialMenuIndex] = ( _trialOverlayPositions[_trialMenuIndex] + 1 ) % options[_trialMenuIndex].size();
            }
        } else {
            _trialOverlayPositions[_trialMenuIndex] = ( _trialOverlayPositions[_trialMenuIndex] + 1 ) % options[_trialMenuIndex].size();
        }
    } else if ( input == 8 ) {
        if ( _trialMenuSelection == Options::TrialSelect ) {
            if ( _trialOverlayPositions[1] == 0 ) {
                if ( _trialScrollSelect > 0 ) {
                    _trialScrollSelect -= 1;
                }
            } else {
                _trialOverlayPositions[_trialMenuIndex] = ( _trialOverlayPositions[_trialMenuIndex] + options[_trialMenuIndex].size() - 1 ) % options[_trialMenuIndex].size();
            }
        } else {
            _trialOverlayPositions[_trialMenuIndex] = ( _trialOverlayPositions[_trialMenuIndex] + options[_trialMenuIndex].size() - 1 ) % options[_trialMenuIndex].size();
        }
    }

    if ( input == 4 ) {
        LOG("four");
        if ( _trialMenuIndex != 0 ) {
            _trialMenuSelection = 0;
            _trialMenuIndex = 0;
            _trialSubMenuSelection = 0;
            _trialOverlayPositions[1] = 0;
        }
    } else if ( input == 6 ) {
        LOG("six");
        if ( _trialMenuIndex == 0 ) {
            _trialMenuSelection = _trialOverlayPositions[0] + 1;
            _trialMenuIndex = 1;
        } else if ( _trialMenuIndex == 1 ) {
            _trialSubMenuSelection = _trialOverlayPositions[1] + 1;
        }
    }

    if ( _trialMenuSelection == Options::TrialSelect ) {
        if ( _trialSubMenuSelection ) {
            int adjustedPosition = _trialOverlayPositions[1] + _trialScrollSelect;
            LOG("trial selected: %s", TrialManager::charaTrials[adjustedPosition].name);
            TrialManager::currentTrialIndex = adjustedPosition;
            disableTrialMenuOverlay();
            return;
        }
    }
    if ( _trialMenuSelection == Options::Demo ) {
        if ( TrialManager::charaTrials.size() > 0 ) {
            if ( TrialManager::charaTrials[TrialManager::currentTrialIndex].demoInputs.size() > 0 ) {
                TrialManager::playDemo = true;
                disableTrialMenuOverlay();
            }
        }
    }
    if ( _trialMenuSelection == Options::RecordDemo ) {
        if ( TrialManager::charaTrials.size() > 0 ) {
            if ( TrialManager::charaTrials[TrialManager::currentTrialIndex].demoInputs.empty() ||
                 _trialSubMenuSelection == 1 ) {
                TrialManager::isRecording = true;
                LOG( "Saving starting pos %d %d", *CC_P1_X_POSITION_ADDR, *CC_P2_X_POSITION_ADDR);
                TrialManager::charaTrials[TrialManager::currentTrialIndex].startingPositions[0] = *CC_P1_X_POSITION_ADDR;
                TrialManager::charaTrials[TrialManager::currentTrialIndex].startingPositions[1] = *CC_P2_X_POSITION_ADDR;
                TrialManager::charaTrials[TrialManager::currentTrialIndex].demoInputs.clear();
                TrialManager::charaTrials[TrialManager::currentTrialIndex].demoInputsFormatted.clear();
                disableTrialMenuOverlay();
            } else if ( _trialSubMenuSelection == 2 ) {
                _trialMenuSelection = 0;
                _trialMenuIndex = 0;
                _trialSubMenuSelection = 0;
                _trialOverlayPositions[1] = 0;
            }
        }
    } else if ( _trialMenuSelection == Options::EditDemo ) {
        text[0] = text[1] = text[2] = "";
        int n = TrialManager::charaTrials[TrialManager::currentTrialIndex].demoInputs.size();
        for ( int i = 0; i < n; ++i ) {
            uint16_t trialInput = TrialManager::charaTrials[TrialManager::currentTrialIndex].demoInputs[i];
            TrialManager::inputEditorBuffer[i] = TrialManager::convertInputEditor( trialInput );
        }
    } else if ( _trialMenuSelection == Options::InputGuide ) {
        TrialManager::inputGuideEnabled = !TrialManager::inputGuideEnabled;
        disableTrialMenuOverlay();
        return;
    } else if ( _trialMenuSelection == Options::Interface ) {
        if ( _trialSubMenuSelection ) {
            TrialManager::trialScale = _trialSubMenuSelection - 1;
            disableTrialMenuOverlay();
            return;
        }
    } else if ( _trialMenuSelection == Options::GuideOptions ) {
        if ( _trialSubMenuSelection ) {
            if ( _trialSubMenuSelection == 1) {
                TrialManager::playAudioCue = !TrialManager::playAudioCue;
            }
            if ( _trialSubMenuSelection == 2) {
                TrialManager::playScreenFlash = !TrialManager::playScreenFlash;
            }
            _trialSubMenuSelection = 0;
        }
    } else if ( _trialMenuSelection == options[0].size() ) {
        LOG("exiting trial menu");
        disableTrialMenuOverlay();
        return;
    }

    DllOverlayUi::updateText ( text );
    DllOverlayUi::updateSelector ( 0, headerOffset + _trialOverlayPositions[0], options[0][_trialOverlayPositions[0]] );
    if ( _trialMenuSelection && options[1].size() > 0 ) {
        DllOverlayUi::updateSelector ( 1, subheaderOffset + _trialOverlayPositions[1], options[1][_trialOverlayPositions[1]] );
    } else {
        DllOverlayUi::updateSelector ( 1 );
    }
    ControllerManager::get().savePrevStates();
}

void DllControllerManager::handleMappingOverlay()
{
    // Check all controllers
    for ( Controller *controller : _allControllers )
    {
        if ( ( controller->isJoystick() && isDirectionPressed ( controller, 6 ) )
                || ( controller->isKeyboard() && KeyboardState::isPressed ( VK_RIGHT ) ) )
        {
            // Move controller right
            if ( controller == _playerControllers[0] )
            {
                if ( ! ( controller->isKeyboard() && controller->isMapping()
                         && _overlayPositions[0] >= 1 && _overlayPositions[0] <= 4 ) )
                {
                    _playerControllers[0]->cancelMapping();
                    _playerControllers[0] = 0;
                }
            }
            else if ( isSinglePlayer && localPlayer == 1 )
            {
                // Only one controller (player 1)
                continue;
            }
            else if ( ! _playerControllers[1] )
            {
                _playerControllers[1] = controller;
                _overlayPositions[1] = 0;
            }
        }
        else if ( ( controller->isJoystick() && isDirectionPressed ( controller, 4 ) )
                  || ( controller->isKeyboard() && KeyboardState::isPressed ( VK_LEFT ) ) )
        {
            // Move controller left
            if ( controller == _playerControllers[1] )
            {
                if ( ! ( controller->isKeyboard() && controller->isMapping()
                         && _overlayPositions[1] >= 1 && _overlayPositions[1] <= 4 ) )
                {
                    _playerControllers[1]->cancelMapping();
                    _playerControllers[1] = 0;
                }
            }
            else if ( isSinglePlayer && localPlayer == 2 )
            {
                // Only one controller (player 2)
                continue;
            }
            else if ( ! _playerControllers[0] )
            {
                _playerControllers[0] = controller;
                _overlayPositions[0] = 0;
            }
        }
    }

    array<string, 3> text;

    // Display all controllers
    text[1] = "Controllers\n";
    for ( const Controller *controller : _allControllers )
        if ( controller != _playerControllers[0] && controller != _playerControllers[1] )
            text[1] += "\n" + controller->getName();

    const size_t controllersHeight = 3 + _allControllers.size();

    // Update player controllers
    for ( uint8_t i = 0; i < 2; ++i )
    {
        string& playerText = text [ i ? 2 : 0 ];

        // Hide / disable other player's overlay in netplay
        if ( isSinglePlayer && localPlayer != i + 1 )
        {
            playerText.clear();
            DllOverlayUi::updateSelector ( i );
            continue;
        }

        // Show placeholder when player has no controller assigned
        if ( ! _playerControllers[i] )
        {
            playerText = ( i == 0 ? "Press Left on P1 controller" : "Press Right on P2 controller" );
            DllOverlayUi::updateSelector ( i );
            continue;
        }

        // Generate mapping options starting with controller name
        size_t headerHeight = 0;
        vector<string> options;
        options.push_back ( _playerControllers[i]->getName() );

        if ( _playerControllers[i]->isKeyboard() )
        {
            headerHeight = max ( 3u, controllersHeight );

            // Instructions for mapping keyboard controls
            playerText = "Press Enter to set a direction key\n";
            playerText += format ( "Press %s to delete a key\n", ( i == 0 ? "Left" : "Right" ) );
            playerText += string ( headerHeight - 3, '\n' );

            // Add directions to keyboard mapping options
            for ( size_t j = 0; j < 4; ++j )
            {
                const string mapping = _playerControllers[i]->getMapping ( gameInputBits[j].second, "..." );
                options.push_back ( gameInputBits[j].first + " : " + mapping );
            }
        }
        else
        {
            headerHeight = max ( 2u, controllersHeight );

            // Instructions for mapping joystick buttons
            playerText = format ( "Press %s to delete a key\n", ( i == 0 ? "Left" : "Right" ) );
            playerText += string ( headerHeight - 2, '\n' );
        }

        // Add buttons to mapping options
        for ( size_t j = 4; j < gameInputBits.size(); ++j )
        {
            const string mapping = _playerControllers[i]->getMapping ( gameInputBits[j].second );
            options.push_back ( gameInputBits[j].first + " : " + mapping );
        }

        // Finally add done option
        options.push_back ( _playerControllers[i]->isKeyboard() ? "Done (press Enter)" : "Done (press any button)" );

        // Disable overlay if both players are done
        if ( _overlayPositions[i] + 1 == options.size()
                && ( ( _playerControllers[i]->isJoystick() && _playerControllers[i]->getJoystickState().buttons )
                     || ( _playerControllers[i]->isKeyboard() && KeyboardState::isPressed ( VK_RETURN ) ) ) )
        {
            _overlayPositions[i] = 0;

            if ( ( !_playerControllers[0] || !_overlayPositions[0] )
                    && ( !_playerControllers[1] || !_overlayPositions[1] ) )
            {
                _overlayPositions[0] = 0;
                _overlayPositions[1] = 0;
                DllOverlayUi::disable();
                KeyboardManager::get().unhook();
                AsmHacks::enableEscapeToExit = true;
                return;
            }
        }

        // Update overlay text with all the options
        for ( const string& option : options )
            playerText += "\n" + option;

        // Filter keyboard overlay controls when mapping directions
        if ( _playerControllers[i]->isKeyboard() && _playerControllers[i]->isMapping()
                && _overlayPositions[i] >= 1 && _overlayPositions[i] <= 4 )
        {
            DllOverlayUi::updateSelector ( i, headerHeight + _overlayPositions[i], options[_overlayPositions[i]] );
            continue;
        }

        bool deleteMapping = false, mapDirections = false, changedPosition = false;

        if ( ( i == 0
                && ( ( _playerControllers[i]->isJoystick() && isDirectionPressed ( _playerControllers[i], 4 ) )
                     || ( _playerControllers[i]->isKeyboard() && KeyboardState::isPressed ( VK_LEFT ) ) ) )
                || ( i == 1
                     && ( ( _playerControllers[i]->isJoystick() && isDirectionPressed ( _playerControllers[i], 6 ) )
                          || ( _playerControllers[i]->isKeyboard() && KeyboardState::isPressed ( VK_RIGHT ) ) ) ) )
        {
            // Delete selected mapping
            deleteMapping = true;
        }
        else if ( _playerControllers[i]->isKeyboard()
                  && ( KeyboardState::isReleased ( VK_RETURN )      // Use Return key released to prevent the
                       || KeyboardState::isPressed ( VK_DELETE ) )  // same key event from being mapped immediately.
                  && _overlayPositions[i] >= 1 && _overlayPositions[i] <= 4 )
        {
            // Press enter / delete to modify direction keys
            if ( KeyboardState::isReleased ( VK_RETURN ) )
                mapDirections = true;
            else
                deleteMapping = true;
        }
        else if ( ( _playerControllers[i]->isJoystick() && isDirectionPressed ( _playerControllers[i], 2 ) )
                  || ( _playerControllers[i]->isKeyboard() && KeyboardState::isPressedOrHeld ( VK_DOWN ) ) )
        {
            // Move selector down
            _overlayPositions[i] = ( _overlayPositions[i] + 1 ) % options.size();
            changedPosition = true;
        }
        else if ( ( _playerControllers[i]->isJoystick() && isDirectionPressed ( _playerControllers[i], 8 ) )
                  || ( _playerControllers[i]->isKeyboard() && KeyboardState::isPressedOrHeld ( VK_UP ) ) )
        {
            // Move selector up
            _overlayPositions[i] = ( _overlayPositions[i] + options.size() - 1 ) % options.size();
            changedPosition = true;
        }

        if ( deleteMapping || mapDirections || changedPosition || _finishedMapping[i] )
        {
            if ( _overlayPositions[i] >= 1 && _overlayPositions[i] < gameInputBits.size() + 1 )
            {
                // Convert selector position to game input bit position
                const size_t pos = _overlayPositions[i] - 1 + ( _playerControllers[i]->isKeyboard() ? 0 : 4 );

                if ( deleteMapping && pos < gameInputBits.size() )
                {
                    // Delete mapping
                    _playerControllers[i]->clearMapping ( gameInputBits[pos].second );
                    saveMappings ( _playerControllers[i] );
                }
                else if ( pos >= 4 && pos < gameInputBits.size() )
                {
                    // Map a button only
                    _playerControllers[i]->startMapping ( this, gameInputBits[pos].second,
                                                          MAP_CONTINUOUSLY | MAP_PRESERVE_DIRS );
                }
                else if ( ( mapDirections || _finishedMapping[i] ) && pos < 4 )
                {
                    ASSERT ( _playerControllers[i]->isKeyboard() == true );

                    // Map a keyboard direction
                    _playerControllers[i]->startMapping ( this, gameInputBits[pos].second );
                }
                else
                {
                    // In all other situations cancel the current mapping
                    _playerControllers[i]->cancelMapping();
                }
            }
            else
            {
                // In all other situations cancel the current mapping
                _playerControllers[i]->cancelMapping();
            }

            _finishedMapping[i] = false;
        }

        if ( _overlayPositions[i] == 0 )
        {
            playerText = string ( "Press Up or Down to set keys" )
                         + string ( controllersHeight, '\n' )
                         + _playerControllers[i]->getName();
            DllOverlayUi::updateSelector ( i, controllersHeight, _playerControllers[i]->getName() );
        }
        else
        {
            DllOverlayUi::updateSelector ( i, headerHeight + _overlayPositions[i], options[_overlayPositions[i]] );
        }
    }

    if(displayIPs) {
        text[1] += "\n";
        text[1] += "\nPublic: " + remoteIP + ":" + port;
        for(const auto& s : localIP) {
            text[1] += "\nPrivate: " + s + ":" + port;
        }
    }

    DllOverlayUi::updateText ( text );

    // Enable Escape to exit if neither controller is being mapped
    AsmHacks::enableEscapeToExit =
        ( !_playerControllers[0] || _overlayPositions[0] == 0 )
        && ( !_playerControllers[1] || _overlayPositions[1] == 0 );

    ControllerManager::get().savePrevStates();
}

void DllControllerManager::keyboardEvent ( uint32_t vkCode, uint32_t scanCode, bool isExtended, bool isDown )
{
    Lock lock ( ControllerManager::get().mutex );

    for ( uint8_t i = 0; i < 2; ++i )
    {
        if ( _playerControllers[i] && _playerControllers[i]->isKeyboard()
                && _overlayPositions[i] >= 1 && _overlayPositions[i] < gameInputBits.size() + 1 )
        {
            // Convert selector position to game input bit position
            const size_t pos = _overlayPositions[i] - 1;

            // Ignore specific control keys when mapping keyboard buttons
            if ( pos >= 4 && pos < gameInputBits.size()
                    && ( vkCode == VK_TOGGLE_OVERLAY
                         || vkCode == VK_ESCAPE
                         || vkCode == VK_UP
                         || vkCode == VK_DOWN
                         || vkCode == VK_LEFT
                         || vkCode == VK_RIGHT ) )
            {
                return;
            }

            _playerControllers[i]->keyboardEvent ( vkCode, scanCode, isExtended, isDown );
            return;
        }
    }
}

void DllControllerManager::joystickAttached ( Controller *controller )
{
    // This is a callback from within ControllerManager, so we don't need to lock the main mutex

    _allControllers.push_back ( controller );

    _controllerAttached = true;
}

void DllControllerManager::joystickToBeDetached ( Controller *controller )
{
    // This is a callback from within ControllerManager, so we don't need to lock the main mutex

    if ( _playerControllers[0] == controller )
    {
        _playerControllers[0] = 0;
        _overlayPositions[0] = 0;
    }

    if ( _playerControllers[1] == controller )
    {
        _playerControllers[1] = 0;
        _overlayPositions[1] = 0;
    }

    const auto it = find ( _allControllers.begin(), _allControllers.end(), controller );

    ASSERT ( it != _allControllers.end() );

    _allControllers.erase ( it );
}

void DllControllerManager::controllerKeyMapped ( Controller *controller, uint32_t key )
{
    // This is a callback from within ControllerManager, so we don't need to lock the main mutex

    LOG ( "%s: controller=%08x; key=%08x", controller->getName(), controller, key );

    if ( key )
    {
        saveMappings ( controller );

        for ( uint8_t i = 0; i < 2; ++i )
        {
            if ( controller == _playerControllers[i]
                    && _overlayPositions[i] >= 1 && _overlayPositions[i] < gameInputBits.size() + 1  )
            {
                // Convert selector position to game input bit position
                const size_t pos = _overlayPositions[i] - 1 + ( _playerControllers[i]->isKeyboard() ? 0 : 4 );

                // Continue mapping
                if ( pos + 1 < gameInputBits.size() )
                    ++_overlayPositions[i];

                _finishedMapping[i] = true;
                return;
            }
        }
    }
}
