#include "audio_manager.h"

#include <windows.h>
#include <string>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>

#include <vector>

#pragma comment(lib, "ole32.lib")

static std::string wide_to_utf8(const wchar_t *w)
{
    if (!w)
        return {};

    int len = WideCharToMultiByte(
        CP_UTF8, 0,
        w, -1,
        nullptr, 0,
        nullptr, nullptr);

    if (len <= 0)
        return {};

    std::string out(len - 1, '\0');
    WideCharToMultiByte(
        CP_UTF8, 0,
        w, -1,
        out.data(), len,
        nullptr, nullptr);

    return out;
}

namespace gcap::audio
{
    std::vector<device> enumerate_devices()
    {
        std::vector<device> out;

        CoInitializeEx(nullptr, COINIT_MULTITHREADED);

        IMMDeviceEnumerator *enumerator = nullptr;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                    CLSCTX_ALL, IID_PPV_ARGS(&enumerator))))
            return out;

        IMMDeviceCollection *collection = nullptr;
        if (FAILED(enumerator->EnumAudioEndpoints(
                eCapture, DEVICE_STATE_ACTIVE, &collection)))
        {
            enumerator->Release();
            return out;
        }

        UINT count = 0;
        collection->GetCount(&count);

        for (UINT i = 0; i < count; ++i)
        {
            IMMDevice *dev = nullptr;
            if (FAILED(collection->Item(i, &dev)))
                continue;

            device info;

            // ---- device id ----
            LPWSTR wid = nullptr;
            if (SUCCEEDED(dev->GetId(&wid)))
            {
                info.id = wide_to_utf8(wid);
                CoTaskMemFree(wid);
            }

            // ---- friendly name ----
            IPropertyStore *props = nullptr;
            if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props)))
            {
                PROPVARIANT v;
                PropVariantInit(&v);
                if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)))
                {
                    info.name = wide_to_utf8(v.pwszVal);
                }
                PropVariantClear(&v);
                props->Release();
            }

            // ---- audio format ----
            IAudioClient *client = nullptr;
            if (SUCCEEDED(dev->Activate(__uuidof(IAudioClient),
                                        CLSCTX_ALL, nullptr,
                                        (void **)&client)))
            {
                WAVEFORMATEX *wfx = nullptr;
                if (SUCCEEDED(client->GetMixFormat(&wfx)) && wfx)
                {
                    info.channels = wfx->nChannels;
                    info.sample_rate = wfx->nSamplesPerSec;
                    info.bits_per_sample = wfx->wBitsPerSample;

                    if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
                    {
                        auto *ext = reinterpret_cast<WAVEFORMATEXTENSIBLE *>(wfx);
                        if (ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)
                            info.is_float = true;
                    }

                    CoTaskMemFree(wfx);
                }
                client->Release();
            }

            dev->Release();
            out.push_back(info);
        }

        collection->Release();
        enumerator->Release();
        CoUninitialize();

        return out;
    }
}
