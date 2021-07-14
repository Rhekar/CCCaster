#pragma once

#include "KeyboardManager.hpp"
#include "MouseManager.hpp"
#include "ControllerManager.hpp"
#include "Controller.hpp"
#include "DllControllerUtils.hpp"

#include <vector>
#include <array>


class DllControllerManager
    : private KeyboardManager::Owner
    , private ControllerManager::Owner
    , private Controller::Owner
    , protected DllControllerUtils
{
public:

    // Local vs remote player numbers
    uint8_t localPlayer = 1, remotePlayer = 2;

    // Single player setting
    bool isSinglePlayer = false;

    // In trial mode
    bool isTrial = false;

    // Initialize all controllers with the given mappings
    void initControllers ( const ControllerMappings& mappings );

    // True only if both controllers are not mapping
    bool isNotMapping() const;

    // Update local controls and overlay UI inputs
    void updateControls ( uint16_t *localInputs );

    // KeyboardManager callback
    void keyboardEvent ( uint32_t vkCode, uint32_t scanCode, bool isExtended, bool isDown ) override;

    // ControllerManager callbacks
    void joystickAttached ( Controller *controller ) override;
    void joystickToBeDetached ( Controller *controller ) override;

    // Controller callback
    void controllerKeyMapped ( Controller *controller, uint32_t key ) override;

    // To be implemented
    virtual void saveMappings ( const Controller *controller ) const = 0;

    bool framestepEnabled;

private:

    std::vector<Controller *> _allControllers;

    std::array<Controller *, 2> _playerControllers = {{ 0, 0 }};

    std::array<size_t, 2> _overlayPositions = {{ 0, 0 }};

    std::array<bool, 2> _finishedMapping = {{ false, false }};

    bool _controllerAttached = false;

    void handleTrialMenuOverlay();
    void handleMappingOverlay();

};
