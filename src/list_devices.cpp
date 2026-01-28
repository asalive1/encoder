#include <iostream>
#include <portaudio.h>

int main() {
    PaError err = Pa_Initialize();
    if (err != paNoError) {
        std::cerr << "PortAudio init error: " << Pa_GetErrorText(err) << std::endl;
        return 1;
    }

    int numDevices = Pa_GetDeviceCount();
    if (numDevices < 0) {
        std::cerr << "PortAudio error: " << Pa_GetErrorText(numDevices) << std::endl;
        Pa_Terminate();
        return 1;
    }

    std::cout << "Found " << numDevices << " audio devices:\n";
    for (int i = 0; i < numDevices; i++) {
        const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
        std::cout << "Index " << i << ": " << info->name
                  << " | Inputs=" << info->maxInputChannels
                  << " | Outputs=" << info->maxOutputChannels
                  << " | Default sample rate=" << info->defaultSampleRate
                  << std::endl;
    }

    Pa_Terminate();
    return 0;
}
