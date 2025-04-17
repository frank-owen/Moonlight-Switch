
#ifdef __SWITCH__
#include <switch.h>
#endif

#include "InputManager.hpp"
#include "Limelight.h"
#include "Settings.hpp"
#include <borealis.hpp>
#include <streaming_view.hpp>
#include <chrono>

using namespace brls;

float fsqrt_(float f) {
    int i = *(int *)&f;
    i = (i >> 1) + 0x1fbb67ae;
    float f1 = *(float *)&i;
    return 0.5F * (f1 + f / f1);

}

MoonlightInputManager::MoonlightInputManager() {
    auto inputManager = brls::Application::getPlatform()->getInputManager();

    inputManager
        ->getMouseCusorOffsetChanged()
        ->subscribe([this](brls::Point offset) {
            if (!inputEnabled) return;

            if (offset.x != 0 || offset.y != 0) {
                float multiplier =
                        (float) Settings::instance().get_mouse_speed_multiplier() / 100.f *
                        1.5f + 0.5f;

                if (!this->inputDropped) {
                    LiSendMouseMoveEvent(short(offset.x * multiplier),
                                         short(offset.y * multiplier));
                }
            }
        });

    inputManager
        ->getMouseScrollOffsetChanged()
        ->subscribe([this](brls::Point scroll) {
            if (!inputEnabled) return;

            if (scroll.x != 0) {
                brls::Logger::info("Mouse scroll X sended: {}", scroll.x);
                LiSendHighResHScrollEvent( short(scroll.x));
            }
            if (scroll.y != 0) {
                brls::Logger::info("Mouse scroll Y sended: {}", scroll.y);
                LiSendHighResScrollEvent( short(scroll.y));
            }
        });

    inputManager
        ->getKeyboardKeyStateChanged()
        ->subscribe([this](brls::KeyState state) {
            if (!inputEnabled) return;

            int vkKey = MoonlightInputManager::glfwKeyToVKKey(state.key);
            char modifiers = state.mods;
            LiSendKeyboardEvent(vkKey,
                                state.pressed ? KEY_ACTION_DOWN : KEY_ACTION_UP,
                                modifiers);
        });

    inputManager
        ->getControllerSensorStateChanged()
        ->subscribe([this](brls::SensorEvent event) {
            if (!inputEnabled) return;
            
            switch (event.type) {
                case brls::SensorEventType::ACCEL:
                    LiSendControllerMotionEvent((uint8_t)event.controllerIndex, LI_MOTION_TYPE_ACCEL, event.data[0], event.data[1], event.data[2]);
                    break;
                case brls::SensorEventType::GYRO:
                    // Convert rad/s to deg/s
                    LiSendControllerMotionEvent((uint8_t)event.controllerIndex, LI_MOTION_TYPE_GYRO, 
                        event.data[0] * 57.2957795f, 
                        event.data[1] * 57.2957795f, 
                        event.data[2] * 57.2957795f);
                    break;
            }
        });
}

void MoonlightInputManager::reloadButtonMappingLayout() {
    KeyMappingLayout layout = (*Settings::instance().get_mapping_laouts())
        [Settings::instance().get_current_mapping_layout()];
    for (int i = 0; i < _BUTTON_MAX; i++) {
        if (layout.mapping.count(i) == 1) {
            mappingButtons[i] = (brls::ControllerButton)layout.mapping.at(i);
        } else {
            mappingButtons[i] = (brls::ControllerButton)i;
        }
    }
}

void MoonlightInputManager::updateTouchScreenPanDelta(
    brls::PanGestureStatus panStatus) {
    this->panStatus = panStatus;
}

void MoonlightInputManager::handleRumble(unsigned short controller,
                                         unsigned short lowFreqMotor,
                                         unsigned short highFreqMotor) {
    brls::Logger::debug("Rumble {} {}", lowFreqMotor, highFreqMotor);

    float rumbleMultiplier = Settings::instance().get_rumble_force();

    rumbleCache[controller].lowFreqMotor = lowFreqMotor * rumbleMultiplier;
    rumbleCache[controller].highFreqMotor = highFreqMotor * rumbleMultiplier;

    brls::Application::getPlatform()->getInputManager()->sendRumble(
        controller, 
        rumbleCache[controller].lowFreqMotor,
        rumbleCache[controller].highFreqMotor);
}

void MoonlightInputManager::handleRumbleTriggers(uint16_t controllerNumber, 
                                                  uint16_t leftTriggerMotor, 
                                                  uint16_t rightTriggerMotor) {
    brls::Logger::debug("Rumble Trigger {} {}", leftTriggerMotor, rightTriggerMotor);

    float rumbleMultiplier = Settings::instance().get_rumble_force();

    rumbleCache[controllerNumber].leftTriggerMotor = leftTriggerMotor * rumbleMultiplier;
    rumbleCache[controllerNumber].rightTriggerMotor = rightTriggerMotor * rumbleMultiplier;

    brls::Application::getPlatform()->getInputManager()->sendRumble(
        controllerNumber, 
        rumbleCache[controllerNumber].lowFreqMotor,
        rumbleCache[controllerNumber].highFreqMotor,
        rumbleCache[controllerNumber].leftTriggerMotor,
        rumbleCache[controllerNumber].rightTriggerMotor);
}

void MoonlightInputManager::dropInput() {
    if (inputDropped)
        return;

    bool res = true;
    // Drop gamepad state
    GamepadState gamepadState;
    auto controllersCount = brls::Application::getPlatform()
                            ->getInputManager()
                            ->getControllersConnectedCount();

    for (short i = 0; i < controllersCount; i++) {
        res &= LiSendMultiControllerEvent(
                   i, controllersToMap(), gamepadState.buttonFlags,
                   gamepadState.leftTrigger, gamepadState.rightTrigger,
                   gamepadState.leftStickX, gamepadState.leftStickY,
                   gamepadState.rightStickX, gamepadState.rightStickY) == 0;
    }

    // Drop touchscreen mouse state
    LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE,BUTTON_MOUSE_LEFT);

    // Drop touchscreen state
    for (auto id: activeTouchIDs) {
        LiSendTouchEvent(LI_TOUCH_EVENT_CANCEL, id.first, 0, 0, 0, 0, 0, LI_ROT_UNKNOWN);
    }
    activeTouchIDs.clear();

    // Drop keyboard state
    for (int i = BRLS_KBD_KEY_SPACE; i < BrlsKeyboardScancode::BRLS_KBD_KEY_LAST; i++)  {
        short vkKey = MoonlightInputManager::glfwKeyToVKKey((BrlsKeyboardScancode)i);
        LiSendKeyboardEvent(vkKey, KEY_ACTION_UP, 0);
    }

    inputDropped = res;
}

GamepadState MoonlightInputManager::getControllerState(int controllerNum,
                                                       bool specialKey) {
    brls::ControllerState rawController{};
    brls::ControllerState controller{};

    brls::Application::setSwapHalfJoyconStickToDpad(Settings::instance().swap_joycon_stick_to_dpad());
    brls::Application::getPlatform()->getInputManager()->updateControllerState(
        &rawController, controllerNum);
    controller = mapController(rawController);

    // Use axis or button if axis is not available (equals 0)
    float lzAxis = controller.axes[LEFT_Z] > 0 ? controller.axes[LEFT_Z] : (controller.buttons[brls::BUTTON_LT] ? 1.f : 0.f);
    float rzAxis = controller.axes[RIGHT_Z] > 0 ? controller.axes[RIGHT_Z] : (controller.buttons[brls::BUTTON_RT] ? 1.f : 0.f);

    // Truncate dead zones
    float leftStickDeadzone = Settings::instance().get_deadzone_stick_left();
    float rightStickDeadzone = Settings::instance().get_deadzone_stick_right();

    float leftXAxis = controller.axes[brls::LEFT_X];
    float leftYAxis = controller.axes[brls::LEFT_Y];
    float rightXAxis = controller.axes[brls::RIGHT_X];
    float rightYAxis = controller.axes[brls::RIGHT_Y];

    if (leftStickDeadzone > 0) {
        float magnitude = fsqrt_(std::powf(leftXAxis, 2) + std::powf(leftYAxis, 2));
        if (magnitude < leftStickDeadzone) {
            leftXAxis = 0;
            leftYAxis = 0;
        }
    }

    if (rightStickDeadzone > 0) {
        float magnitude = fsqrt_(std::powf(rightXAxis, 2) + std::powf(rightYAxis, 2));
        if (magnitude < rightStickDeadzone) {
            rightXAxis = 0;
            rightYAxis = 0;
        }
    }

    GamepadState gamepadState{
        .buttonFlags = 0,
        .leftTrigger = static_cast<unsigned char>(
            0xFF * (!specialKey ? lzAxis : 0)),
        .rightTrigger = static_cast<unsigned char>(
            0xFF * (!specialKey ? rzAxis : 0)),
        .leftStickX = static_cast<short>(
            0x7FFF * (!specialKey ? leftXAxis : 0)),
        .leftStickY = static_cast<short>(
            -0x7FFF * (!specialKey ? leftYAxis : 0)),
        .rightStickX = static_cast<short>(
            0x7FFF * (!specialKey ? rightXAxis : 0)),
        .rightStickY = static_cast<short>(
            -0x7FFF * (!specialKey ? rightYAxis : 0)),
    };

    brls::ControllerButton a = brls::BUTTON_A;
    brls::ControllerButton b = brls::BUTTON_B;
    brls::ControllerButton x = brls::BUTTON_X;
    brls::ControllerButton y = brls::BUTTON_Y;

#define SET_GAME_PAD_STATE(LIMELIGHT_KEY, GAMEPAD_BUTTON)                      \
    controller.buttons[GAMEPAD_BUTTON]                                         \
        ? (gamepadState.buttonFlags |= LIMELIGHT_KEY)                          \
        : (gamepadState.buttonFlags &= ~LIMELIGHT_KEY);

    SET_GAME_PAD_STATE(UP_FLAG, brls::BUTTON_UP);
    SET_GAME_PAD_STATE(DOWN_FLAG, brls::BUTTON_DOWN);
    SET_GAME_PAD_STATE(LEFT_FLAG, brls::BUTTON_LEFT);
    SET_GAME_PAD_STATE(RIGHT_FLAG, brls::BUTTON_RIGHT);

#ifdef __SWITCH__
    SET_GAME_PAD_STATE(A_FLAG, b);
    SET_GAME_PAD_STATE(B_FLAG, a);
    SET_GAME_PAD_STATE(X_FLAG, y);
    SET_GAME_PAD_STATE(Y_FLAG, x);
#else
    SET_GAME_PAD_STATE(A_FLAG, a);
    SET_GAME_PAD_STATE(B_FLAG, b);
    SET_GAME_PAD_STATE(X_FLAG, x);
    SET_GAME_PAD_STATE(Y_FLAG, y);
#endif

    SET_GAME_PAD_STATE(BACK_FLAG, brls::BUTTON_BACK);
    SET_GAME_PAD_STATE(PLAY_FLAG, brls::BUTTON_START);

    SET_GAME_PAD_STATE(LB_FLAG, brls::BUTTON_LB);
    SET_GAME_PAD_STATE(RB_FLAG, brls::BUTTON_RB);

    SET_GAME_PAD_STATE(LS_CLK_FLAG, brls::BUTTON_LSB);
    SET_GAME_PAD_STATE(RS_CLK_FLAG, brls::BUTTON_RSB);

    auto guideKeys = Settings::instance().guide_key_options().buttons;
    bool guideCombo = !guideKeys.empty();
    for (auto key : guideKeys)
        guideCombo &= controller.buttons[key];

    if (guideCombo ||
        lastGamepadStates[controllerNum].buttonFlags & SPECIAL_FLAG)
        gamepadState.buttonFlags = 0;

    bool guidePressed = guideCombo || controller.buttons[brls::BUTTON_GUIDE];
    guidePressed ? (gamepadState.buttonFlags |= SPECIAL_FLAG)
               : (gamepadState.buttonFlags &= ~SPECIAL_FLAG);

    return gamepadState;
}

void MoonlightInputManager::handleControllers(bool specialKey) {
    static int lastControllerCount = 0;

    auto controllersCount = brls::Application::getPlatform()
                            ->getInputManager()
                            ->getControllersConnectedCount();

    short mappedControllersCount = controllersToMap();

    for (int i = 0; i < controllersCount; i++) {
        GamepadState gamepadState = getControllerState(i, specialKey);

        if (!gamepadState.is_equal(lastGamepadStates[i])) {
            lastGamepadStates[i] = gamepadState;

            if (lastControllerCount != controllersCount) {
                lastControllerCount = controllersCount;
                
                for (int i = 0; i < controllersCount; i++) {
                    Logger::debug("StreamingView: send features message for controller #{}", i);
                    LiSendControllerArrivalEvent(i, mappedControllersCount, LI_CTYPE_UNKNOWN, 0, LI_CCAP_RUMBLE | LI_CCAP_ACCEL | LI_CCAP_GYRO);
                }
            }

            if (LiSendMultiControllerEvent(
                    i, mappedControllersCount, gamepadState.buttonFlags,
                    gamepadState.leftTrigger, gamepadState.rightTrigger,
                    gamepadState.leftStickX, gamepadState.leftStickY,
                    gamepadState.rightStickX, gamepadState.rightStickY) != 0)
                brls::Logger::info("StreamingView: error sending input data");
        }
    }
}

void MoonlightInputManager::handleInput(bool ignoreTouch) {
    inputDropped = false;
    static brls::ControllerState rawController;
    static brls::ControllerState controller;
    static brls::RawMouseState mouse;

    brls::Application::getPlatform()
            ->getInputManager()
            ->updateUnifiedControllerState(&rawController);
    brls::Application::getPlatform()->getInputManager()->updateMouseStates(
            &mouse);
    controller = mapController(rawController);

    std::vector<brls::RawTouchState> states;
    brls::Application::getPlatform()->getInputManager()->updateTouchStates(&states);

    //Do not use gamepad for mouse controll assist if touchscreen mode enabled
    bool specialKey = !ignoreTouch && !Settings::instance().touchscreen_mouse_mode() && states.size() == 1;

    handleControllers(specialKey);

    float stickScrolling =
            specialKey
            ? (controller.axes[brls::LEFT_Y] + controller.axes[brls::RIGHT_Y])
            : 0;

    static MouseStateS lastMouseState;

    MouseStateS mouseState;
    if (!Settings::instance().touchscreen_mouse_mode()) {
        mouseState = {
                .scroll_y = stickScrolling,
                .l_pressed = (specialKey && controller.buttons[brls::BUTTON_RT]) || mouse.leftButton,
                .m_pressed = mouse.middleButton,
                .r_pressed = (specialKey && controller.buttons[brls::BUTTON_LT]) || mouse.rightButton
        };
    } else {
        mouseState = {
                .scroll_y = 0,
                .l_pressed = mouse.leftButton,
                .m_pressed = mouse.middleButton,
                .r_pressed = mouse.rightButton
        };
    }

    if (Settings::instance().swap_mouse_scroll())
        mouseState.scroll_y *= -1;

    if (mouseState.l_pressed != lastMouseState.l_pressed) {
        lastMouseState.l_pressed = mouseState.l_pressed;
        auto lb = Settings::instance().swap_mouse_keys() ? BUTTON_MOUSE_RIGHT
                                                         : BUTTON_MOUSE_LEFT;
        LiSendMouseButtonEvent(mouseState.l_pressed ? BUTTON_ACTION_PRESS
                                                    : BUTTON_ACTION_RELEASE,
                               lb);
        if (!mouseState.l_pressed)
            Logger::debug("Release key lmb");
    }

    if (mouseState.m_pressed != lastMouseState.m_pressed) {
        lastMouseState.m_pressed = mouseState.m_pressed;
        LiSendMouseButtonEvent(mouseState.m_pressed ? BUTTON_ACTION_PRESS
                                                    : BUTTON_ACTION_RELEASE,
                               BUTTON_MOUSE_MIDDLE);
    }

    if (mouseState.r_pressed != lastMouseState.r_pressed) {
        lastMouseState.r_pressed = mouseState.r_pressed;
        auto rb = Settings::instance().swap_mouse_keys() ? BUTTON_MOUSE_LEFT
                                                         : BUTTON_MOUSE_RIGHT;
        LiSendMouseButtonEvent(mouseState.r_pressed ? BUTTON_ACTION_PRESS
                                                    : BUTTON_ACTION_RELEASE,
                               rb);
    }

    std::chrono::high_resolution_clock::time_point timeNow =
            std::chrono::high_resolution_clock::now();
    static std::chrono::high_resolution_clock::time_point timeStamp = timeNow;

    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            timeNow - timeStamp)
            .count();
    if (mouseState.scroll_y != 0 &&
        (float) duration > 550 - std::fabs(mouseState.scroll_y) * 500) {
        timeStamp = timeNow;
        brls::Logger::info("Scroll sended: {}", mouseState.scroll_y);
        lastMouseState.scroll_y = mouseState.scroll_y;
        LiSendScrollEvent(mouseState.scroll_y > 0 ? 1 : -1);
    }

    if (!Settings::instance().touchscreen_mouse_mode()) {
        // Do not process touch events, useful if onscreen keyboard is presented
        if (ignoreTouch) { return; }

        if (panStatus.has_value()) {
            float multiplier =
                    Settings::instance().get_mouse_speed_multiplier() / 100.f * 1.5f +
                    0.5f;
            LiSendMouseMoveEvent(-panStatus->delta.x * multiplier,
                                 -panStatus->delta.y * multiplier);
            panStatus.reset();
        }
    } else {
        uint8_t eventType;

        auto touches = brls::Application::currentTouchState;
        for (int i = 0; i < touches.size(); i++) {
            auto touch = touches[i];
            if (touch.view && touch.view->hasParent() && dynamic_cast<StreamingView*>(touch.view->getParent()) == nullptr) return;

            switch (touch.phase) {
                case TouchPhase::START:
                    eventType = LI_TOUCH_EVENT_DOWN;
                    break;
                case TouchPhase::STAY:
                    eventType = LI_TOUCH_EVENT_MOVE;
                    break;
                case TouchPhase::END:
                    eventType = LI_TOUCH_EVENT_UP;
                    break;
                case TouchPhase::NONE:
                    eventType = LI_TOUCH_EVENT_CANCEL;
                    break;
            }

            if (touch.phase != TouchPhase::NONE) {
                activeTouchIDs.insert(std::pair(touch.fingerId, true));
            } else {
                activeTouchIDs.erase(touch.fingerId);
            }

            if (LiSendTouchEvent(eventType, touch.fingerId, touch.position.x / (float) Application::contentWidth,
                                 touch.position.y / (float) Application::contentHeight, 0, 0, 0, LI_ROT_UNKNOWN) ==
                LI_ERR_UNSUPPORTED && i == 0) {
                // Fallback to move cursor and click if touch unsupported
                if (touch.phase != TouchPhase::NONE) {
                    LiSendMousePositionEvent(touch.position.x, touch.position.y, Application::contentWidth,
                                             Application::contentHeight);
                }
                if (touch.phase == TouchPhase::START) {
                    LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_MOUSE_LEFT);
                }
                if (touch.phase == TouchPhase::END) {
                    LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_MOUSE_LEFT);
                }
            }
        }
    }
}

short MoonlightInputManager::controllersToMap() {
    switch (brls::Application::getPlatform()
                ->getInputManager()
                ->getControllersConnectedCount()) {
    case 0:
        return 0x0;
    case 1:
        return 0x1;
    case 2:
        return 0x3;
    case 3:
        return 0x7;
    default:
        return 0xF;
    }
}

brls::ControllerState
MoonlightInputManager::mapController(brls::ControllerState controller) {
    brls::ControllerState result{};

    std::fill(result.buttons, result.buttons + sizeof(result.buttons), false);

    for (int i = 0; i < _AXES_MAX; i++)
        result.axes[i] = controller.axes[i];

    for (int i = 0; i < _BUTTON_MAX; i++) {
        result.buttons[mappingButtons[i]] |= controller.buttons[i];
    }

    return result;
}

void MoonlightInputManager::leftMouseClick() {
    LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_MOUSE_LEFT);
    LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_MOUSE_LEFT);
}

void MoonlightInputManager::rightMouseClick() {
    LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_MOUSE_RIGHT);
    LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_MOUSE_RIGHT);
}

short MoonlightInputManager::glfwKeyToVKKey(BrlsKeyboardScancode key) {
    if (BRLS_KBD_KEY_F1 <= key && key <= BRLS_KBD_KEY_F12)
        return key - BRLS_KBD_KEY_F1 + 0x70;

    if (BRLS_KBD_KEY_KP_0 <= key && key <= BRLS_KBD_KEY_KP_9)
        return key - BRLS_KBD_KEY_KP_0 + 0x60;

    switch (key) {
    case BRLS_KBD_KEY_BACKSPACE:
        return 0x08;
    case BRLS_KBD_KEY_SEMICOLON:
        return 0xBA;
    case BRLS_KBD_KEY_EQUAL:
        return 0xBB;
    case BRLS_KBD_KEY_COMMA:
        return 0xBC;
    case BRLS_KBD_KEY_MINUS:
        return 0xBD;
    case BRLS_KBD_KEY_PERIOD:
        return 0xBE;
    case BRLS_KBD_KEY_WORLD_1: // OEM_102 (> <)
        return 0xE2;
    case BRLS_KBD_KEY_SLASH:
        return 0xBF;
    case BRLS_KBD_KEY_GRAVE_ACCENT:
        return 0xC0;
    case BRLS_KBD_KEY_LEFT_BRACKET:
        return 0xDB;
    case BRLS_KBD_KEY_BACKSLASH:
        return 0xDC;
    case BRLS_KBD_KEY_RIGHT_BRACKET:
        return 0xDD;
    case BRLS_KBD_KEY_APOSTROPHE:
        return 0xDE;
    // case FIND_PROPER_NAME: // OEM_8 (§ !)
    //     return 0xDF;
    case BRLS_KBD_KEY_TAB:
        return 0x09;
    case BRLS_KBD_KEY_CAPS_LOCK:
        return 0x14;
    case BRLS_KBD_KEY_LEFT_SHIFT:
        return 0xA0;
    case BRLS_KBD_KEY_RIGHT_SHIFT:
        return 0xA1;
    case BRLS_KBD_KEY_LEFT_CONTROL:
        return 0xA2;
    case BRLS_KBD_KEY_RIGHT_CONTROL:
        return 0xA3;
    case BRLS_KBD_KEY_LEFT_ALT:
        return 0xA4;
    case BRLS_KBD_KEY_RIGHT_ALT:
        return 0xA5;
    case BRLS_KBD_KEY_ENTER:
        return 0x0D;
    case BRLS_KBD_KEY_LEFT_SUPER:
        return 0x5B;
    case BRLS_KBD_KEY_RIGHT_SUPER:
        return 0x5C;
    case BRLS_KBD_KEY_ESCAPE:
        return 0x1B;
    case BRLS_KBD_KEY_KP_ADD:
        return 0x6B;
    case BRLS_KBD_KEY_KP_DECIMAL:
        return 0x6E;
    case BRLS_KBD_KEY_KP_DIVIDE:
        return 0x6F;
    case BRLS_KBD_KEY_KP_MULTIPLY:
        return 0x6A;
    case BRLS_KBD_KEY_KP_ENTER:
        return 0x0D;
    case BRLS_KBD_KEY_NUM_LOCK:
        return 0x90;
    case BRLS_KBD_KEY_SCROLL_LOCK:
        return 0x91;
    case BRLS_KBD_KEY_PAGE_UP:
        return 0x21;
    case BRLS_KBD_KEY_PAGE_DOWN:
        return 0x22;
    case BRLS_KBD_KEY_END:
        return 0x23;
    case BRLS_KBD_KEY_HOME:
        return 0x24;
    case BRLS_KBD_KEY_LEFT:
        return 0x25;
    case BRLS_KBD_KEY_UP:
        return 0x26;
    case BRLS_KBD_KEY_RIGHT:
        return 0x27;
    case BRLS_KBD_KEY_DOWN:
        return 0x28;
    case BRLS_KBD_KEY_PRINT_SCREEN:
        return 0x2C;
    case BRLS_KBD_KEY_INSERT:
        return 0x2D;
    case BRLS_KBD_KEY_DELETE:
        return 0x2E;

    default:
        return key;
    }
}
