#include "AlphaStudio/SDK/Bridge/VSTBridgeIPC.hpp"

#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

int main(int argc, char** argv) {
    using namespace AlphaStudio::SDK::Bridge;

    if (argc >= 2 && std::strcmp(argv[1], "--self-test") == 0) {
        const std::string token = "SelfTest_Bridge_32_64";
        VSTBridgeHostProxy host;
        if (!host.connectSession(token, 48000.0, 128, 1, 1)) {
            std::cerr << "[AlphaVstBridge32] Failed to open shared memory.\n";
            return 1;
        }
        VSTBridgeWorkerServer worker(1500, 600);
        if (!worker.attachSession(token)) {
            std::cerr << "[AlphaVstBridge32] Worker failed to attach.\n";
            return 1;
        }

        std::atomic<bool> stopWorker{false};
        std::thread serverThread([&]() {
            while (!stopWorker.load(std::memory_order_acquire)) {
                if (!worker.pollAndExecuteOnce()) {
                    std::this_thread::yield();
                }
            }
        });

        const auto params = host.fetchAllParametersPaged();
        const auto presets = host.fetchAllPresetsPaged();

        std::array<float, 128> monoIn{};
        std::array<float, 128> monoOut{};
        monoIn.fill(0.5f);
        const float* inPtr = monoIn.data();
        float* outPtr = monoOut.data();
        const bool audioOk = host.processBridgedBlockRT(&inPtr, 1, &outPtr, 1, 128, {});

        stopWorker.store(true, std::memory_order_release);
        serverThread.join();

        if (!audioOk || params.size() != 1500 || presets.size() != 600 || monoOut[0] < 0.49f) {
            std::cerr << "[AlphaVstBridge32] Self-test failed.\n";
            return 1;
        }
        std::cout << "[AlphaVstBridge32] IPC Mono + 1500 Params + 600 Presets + UAC Self-Test OK\n";
        return 0;
    }

    const std::string sessionToken = (argc >= 2) ? argv[1] : "DefaultBridgeSession";
    VSTBridgeWorkerServer server(2048, 1024);
    if (!server.attachSession(sessionToken)) {
        std::cerr << "[AlphaVstBridge32] No active host session '" << sessionToken << "'.\n";
        return 1;
    }

    while (true) {
        if (!server.pollAndExecuteOnce()) {
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    }
    return 0;
}
