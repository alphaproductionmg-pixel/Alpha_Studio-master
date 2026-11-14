#pragma once

#include <cstdint>
#include <string>

namespace AlphaStudio::UI {

class InteractiveDAWApplication {
public:
    InteractiveDAWApplication();
    ~InteractiveDAWApplication();

    InteractiveDAWApplication(const InteractiveDAWApplication&) = delete;
    InteractiveDAWApplication& operator=(const InteractiveDAWApplication&) = delete;

    // Launches the native interactive GUI DAW window + real-time 48kHz audio engine.
    // Returns process exit code when user closes the window.
    int runDesktopApp();
};

} // namespace AlphaStudio::UI
