// Copyright 2017 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include "core/core_timing.h"
#include "core/hw/gpu.h"
#include "input_core/devices/keyboard.h"
#include "input_core/devices/sdl_joystick.h"
#include "input_core/input_core.h"

void InputCore::Init() {
    ParseSettings();
    tick_event = CoreTiming::RegisterEvent("InputCore::tick_event", [=](u64 u, int cycles_late) {
        InputCore::InputTickCallback(u, cycles_late);
    });
    CoreTiming::ScheduleEvent(GPU::frame_ticks, tick_event);
}

void InputCore::Shutdown() {
    CoreTiming::UnscheduleEvent(tick_event, 0);
    devices.clear();
}

Service::HID::PadState InputCore::GetPadState() {
    std::lock_guard<std::mutex> lock(pad_state_mutex);
    return pad_state;
}

void InputCore::SetPadState(const Service::HID::PadState& state) {
    std::lock_guard<std::mutex> lock(pad_state_mutex);
    pad_state.hex = state.hex;
}

std::tuple<s16, s16> InputCore::GetCirclePad() {
    return circle_pad;
}

std::shared_ptr<Keyboard> InputCore::GetKeyboard() {
    if (main_keyboard == nullptr) {
        main_keyboard = std::make_shared<Keyboard>();
    }
    return main_keyboard;
}

std::tuple<u16, u16, bool> InputCore::GetTouchState() {
    std::lock_guard<std::mutex> lock(touch_mutex);
    return std::make_tuple(touch_x, touch_y, touch_pressed);
}

void InputCore::SetTouchState(std::tuple<u16, u16, bool> value) {
    std::lock_guard<std::mutex> lock(touch_mutex);
    std::tie(touch_x, touch_y, touch_pressed) = value;
}

void InputCore::UpdateEmulatorInputs(
    std::vector<std::map<Settings::InputDeviceMapping, float>> inputs) {
    std::lock_guard<std::mutex> lock(pad_state_mutex);

    // Get circle pad raw values
    float left_x = 0, left_y = 0;
    float circle_pad_modifier = 1.0;
    auto circle_pad_modifier_mapping = Settings::values.pad_circle_modifier;

    for (auto& input_device : inputs) {
        for (auto& button_states : input_device) { // Loop through all buttons from input device
            float strength = button_states.second;
            auto emulator_inputs = key_mappings[button_states.first];
            for (auto& emulator_input : emulator_inputs) {
                if (strength == 0)
                    continue;
                if (emulator_input == Service::HID::PAD_CIRCLE_UP)
                    left_y = -strength;

                else if (emulator_input == Service::HID::PAD_CIRCLE_DOWN)
                    left_y = strength;

                else if (emulator_input == Service::HID::PAD_CIRCLE_LEFT)
                    left_x = -strength;

                else if (emulator_input == Service::HID::PAD_CIRCLE_RIGHT)
                    left_x = strength;
            }
            if (button_states.first == circle_pad_modifier_mapping)
                circle_pad_modifier = (button_states.second > input_detect_threshold)
                                          ? Settings::values.pad_circle_modifier_scale
                                          : 1.0;
        }
    }
    // Apply deadzone to raw circle pad values
    float deadzone = Settings::values.pad_circle_deadzone;
    std::tuple<float, float> left_stick = ApplyDeadzone(left_x, left_y, deadzone);

    // Set emulator circlepad values
    std::get<0>(circle_pad) =
        std::get<0>(left_stick) * KeyMap::MAX_CIRCLEPAD_POS * circle_pad_modifier;
    std::get<1>(circle_pad) =
        std::get<1>(left_stick) * KeyMap::MAX_CIRCLEPAD_POS * -1 * circle_pad_modifier;

    // Set emulator digital button values
    for (auto& input_device : inputs) {
        for (auto& button_states : input_device) { // Loop through all buttons from input device
            float strength = button_states.second;
            auto emulator_inputs = key_mappings[button_states.first];
            for (auto& emulator_input : emulator_inputs) {
                if (std::find(std::begin(KeyMap::analog_inputs), std::end(KeyMap::analog_inputs),
                              emulator_input) == std::end(KeyMap::analog_inputs)) {
                    if (abs(strength) < input_detect_threshold && // Key released
                        keys_pressed[emulator_input] == true) {
                        pad_state.hex &= ~emulator_input.hex;
                        keys_pressed[emulator_input] = false;
                    } else if (abs(strength) >= input_detect_threshold &&
                               keys_pressed[emulator_input] == false) { // Key pressed
                        pad_state.hex |= emulator_input.hex;
                        keys_pressed[emulator_input] = true;
                    }
                }
            }
        }
    }
}

bool InputCore::CheckIfMappingExists(const std::set<Settings::InputDeviceMapping>& unique_mapping,
                                     Settings::InputDeviceMapping mapping_to_check) {
    return std::any_of(
        unique_mapping.begin(), unique_mapping.end(),
        [mapping_to_check](const auto& mapping) { return mapping == mapping_to_check; });
}

std::set<Settings::InputDeviceMapping> InputCore::GatherUniqueMappings() {
    std::set<Settings::InputDeviceMapping> unique_mappings;

    for (const auto& mapping : Settings::values.input_mappings) {
        if (!CheckIfMappingExists(unique_mappings, mapping)) {
            unique_mappings.insert(mapping);
        }
    }
    if (!CheckIfMappingExists(unique_mappings, Settings::values.pad_circle_modifier)) {
        unique_mappings.insert(Settings::values.pad_circle_modifier);
    }
    return unique_mappings;
}

void InputCore::BuildKeyMapping() {
    key_mappings.clear();
    for (size_t i = 0; i < Settings::values.input_mappings.size(); i++) {
        auto key = Settings::values.input_mappings[i];
        auto val = KeyMap::mapping_targets[i];
        key_mappings.emplace(key, std::vector<Service::HID::PadState>());
        key_mappings[key].push_back(val);
    }
}

void InputCore::GenerateUniqueDevices() {
    auto uniqueMappings = GatherUniqueMappings();
    devices.clear();
    std::shared_ptr<InputDeviceInterface> input;
    for (const auto& mapping : uniqueMappings) {
        switch (mapping.framework) {
        case Settings::DeviceFramework::SDL: {
            if (mapping.device == Settings::Device::Keyboard) {
                main_keyboard = std::make_shared<Keyboard>();
                input = main_keyboard;
                break;
            } else if (mapping.device == Settings::Device::Gamepad) {
                input = std::make_shared<SDLJoystick>();
                break;
            }
        }
        }
        devices.push_back(input);
        input->InitDevice(mapping.number);
    }
}

void InputCore::ParseSettings() {
    GenerateUniqueDevices();
    BuildKeyMapping();
}

void InputCore::ReloadSettings() {
    if (devices.empty())
        return;
    std::lock_guard<std::mutex> lock(pad_state_mutex);
    devices.clear();
    ParseSettings();
}

std::vector<std::shared_ptr<InputDeviceInterface>> InputCore::GetAllDevices() {
    auto all_devices = SDLJoystick::GetAllDevices();
    auto keyboard = InputCore::GetKeyboard();
    all_devices.push_back(keyboard);

    return all_devices;
}

Settings::InputDeviceMapping InputCore::DetectInput(int max_time,
                                                    std::function<void(void)> update_gui) {
    auto devices = GetAllDevices();
    for (auto& device : devices) {
        device->Clear();
    }

    // Get current state of inputs. Keep tracking of buttons that are already pressed when starting
    // detection
    std::map<Settings::InputDeviceMapping, bool> current_input_states;
    for (auto& device : devices) {
        auto current_state = device->ProcessInput();
        for (const auto& i : current_state) {
            current_input_states.emplace(i.first, i.second > (input_detect_threshold));
        }
    }

    Settings::InputDeviceMapping input_device;
    auto start = std::chrono::high_resolution_clock::now();
    while (input_device.key.empty()) {
        update_gui();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::high_resolution_clock::now() - start)
                            .count();
        if (duration >= max_time) {
            break;
        }
        for (auto& device : devices) {
            auto inputs = device->ProcessInput();
            for (const auto& i : inputs) {
                if (i.second > input_detect_threshold && current_input_states[i.first] == false)
                    return i.first;
                else if (i.second < input_detect_threshold && current_input_states[i.first] == true)
                    current_input_states[i.first] = false;
            }
        }
    };
    return input_device;
}

void InputCore::InputTickCallback(u64, int cycles_late) {
    std::vector<std::map<Settings::InputDeviceMapping, float>> inputs;
    for (auto& device : devices) {
        inputs.push_back(device->ProcessInput());
    }
    UpdateEmulatorInputs(inputs);

    Service::HID::Update();

    // Reschedule recurrent event
    CoreTiming::ScheduleEvent(GPU::frame_ticks - cycles_late, tick_event);
}

std::tuple<float, float> InputCore::ApplyDeadzone(float x, float y, float dead_zone) {
    float magnitude = std::sqrt((x * x) + (y * y));
    if (magnitude < dead_zone) {
        x = 0;
        y = 0;
    } else {
        float normalized_x = x / magnitude;
        float normalized_y = y / magnitude;
        x = normalized_x * ((magnitude - dead_zone) / (1 - dead_zone));
        y = normalized_y * ((magnitude - dead_zone) / (1 - dead_zone));
    }
    return std::tuple<float, float>(x, y);
}
