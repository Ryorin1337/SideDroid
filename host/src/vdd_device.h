// The Virtual Display Driver as a Plug and Play device. Changing its state takes administrator
// rights, so only the driver tool and the service do that. Anyone may ask whether it exists.
#pragma once
#include <windows.h>

namespace sd {

// In any state, disabled included.
bool vddDeviceExists();
// True when SideDroid installed the driver, which makes the device ours to turn on and off.
bool vddDriverIsOurs();

enum class VddDeviceAction { Enable, Disable, Restart };
// Returns ERROR_SUCCESS, or why not.
DWORD changeVddDevice(VddDeviceAction action);

}  // namespace sd
