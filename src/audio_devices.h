#pragma once
#include <string>
#include <vector>

struct AudioDevice {
    std::wstring id;       // WASAPI device ID (used internally)
    std::string  name;     // Display name (UTF-8)
    bool         isDefault = false;
};

// Returns all active render (output) endpoints suitable for loopback capture.
// The current Windows default device is placed first in the list.
std::vector<AudioDevice> EnumerateLoopbackDevices();
