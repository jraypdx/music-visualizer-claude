#include "audio_devices.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

static std::string WideToUtf8(const wchar_t* wide)
{
    if (!wide) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return {};
    std::string out(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), len, nullptr, nullptr);
    return out;
}

std::vector<AudioDevice> EnumerateLoopbackDevices()
{
    std::vector<AudioDevice> devices;

    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator),
        reinterpret_cast<void**>(enumerator.GetAddressOf()));
    if (FAILED(hr)) return devices;

    // Determine the default render device ID so we can mark it
    std::wstring defaultId;
    {
        ComPtr<IMMDevice> defaultDevice;
        if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &defaultDevice))) {
            LPWSTR idStr = nullptr;
            if (SUCCEEDED(defaultDevice->GetId(&idStr))) {
                defaultId = idStr;
                CoTaskMemFree(idStr);
            }
        }
    }

    // Enumerate all active render endpoints (these support WASAPI loopback)
    ComPtr<IMMDeviceCollection> collection;
    hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(hr)) return devices;

    UINT count = 0;
    collection->GetCount(&count);

    AudioDevice defaultEntry;
    bool foundDefault = false;

    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(i, &device))) continue;

        LPWSTR idStr = nullptr;
        if (FAILED(device->GetId(&idStr))) continue;
        std::wstring id = idStr;
        CoTaskMemFree(idStr);

        ComPtr<IPropertyStore> props;
        if (FAILED(device->OpenPropertyStore(STGM_READ, &props))) continue;

        PROPVARIANT pv;
        PropVariantInit(&pv);
        std::string friendlyName;
        if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR)
            friendlyName = WideToUtf8(pv.pwszVal);
        PropVariantClear(&pv);

        if (friendlyName.empty()) friendlyName = "(Unknown Device)";

        AudioDevice dev;
        dev.id        = id;
        dev.isDefault = (id == defaultId);
        dev.name      = dev.isDefault ? friendlyName + "  [Default]" : friendlyName;

        if (dev.isDefault) {
            defaultEntry  = std::move(dev);
            foundDefault  = true;
        } else {
            devices.push_back(std::move(dev));
        }
    }

    // Insert the default device at the front so index 0 is the right starting selection
    if (foundDefault)
        devices.insert(devices.begin(), std::move(defaultEntry));

    return devices;
}
