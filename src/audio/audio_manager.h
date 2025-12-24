#pragma once

#include <string>
#include <vector>

namespace gcap::audio
{
    struct device
    {
        std::string id;
        std::string name;
        int channels = 0;
        int sample_rate = 0;
        int bits_per_sample = 0;
        bool is_float = false;
    };

    std::vector<device> enumerate_devices();
}
