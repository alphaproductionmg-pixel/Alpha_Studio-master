cmake_minimum_required(VERSION 3.20)
project(AlphaStudio VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ALPHASTUDIO_ENABLE_LTO "Enable Link-Time Optimization for Release builds" OFF)
option(ALPHASTUDIO_BUILD_BRIDGE_WORKER "Build isolated 32/64-bit IPC VST Bridge worker executable" ON)
option(ALPHASTUDIO_USE_EXTERNAL_IMGUI "Link against external system/fetched Dear ImGui instead of built-in clean-room ImGui 2D layer" OFF)

# Strict exclusion of any video subsystem at the build-system level
add_compile_definitions(ALPHASTUDIO_NO_VIDEO_SUBSYSTEM=1)

# Platform and architecture detection: Windows (32/64-bit), macOS, Linux
if(WIN32)
    add_compile_definitions(ALPHASTUDIO_PLATFORM_WINDOWS=1 NOMINMAX)
    if(CMAKE_SIZEOF_VOID_P EQUAL 4)
        add_compile_definitions(ALPHASTUDIO_ARCH_X86_32=1)
        message(STATUS "AlphaStudio Target: Windows 32-bit (x86)")
    elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
        add_compile_definitions(ALPHASTUDIO_ARCH_X86_64=1)
        message(STATUS "AlphaStudio Target: Windows 64-bit (x64)")
    endif()
elseif(APPLE)
    add_compile_definitions(ALPHASTUDIO_PLATFORM_MACOS=1)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        add_compile_definitions(ALPHASTUDIO_ARCH_X86_64=1)
    endif()
    message(STATUS "AlphaStudio Target: macOS (${CMAKE_SYSTEM_PROCESSOR})")
elseif(UNIX AND NOT APPLE)
    add_compile_definitions(ALPHASTUDIO_PLATFORM_LINUX=1)
    if(CMAKE_SIZEOF_VOID_P EQUAL 4)
        add_compile_definitions(ALPHASTUDIO_ARCH_X86_32=1)
    else()
        add_compile_definitions(ALPHASTUDIO_ARCH_X86_64=1)
    endif()
    message(STATUS "AlphaStudio Target: Linux (${CMAKE_SYSTEM_PROCESSOR})")
endif()

find_package(Threads REQUIRED)

# ==============================================================================
# Target 0: SteinbergASIO234 (Integrated Official Steinberg ASIO SDK 2.3.4)
# ==============================================================================
set(STEINBERG_ASIO_SOURCES
    ThirdParty/ASIOSDK/host/ASIOConvertSamples.cpp
)
if(WIN32)
    list(APPEND STEINBERG_ASIO_SOURCES
        ThirdParty/ASIOSDK/common/asio.cpp
        ThirdParty/ASIOSDK/host/asiodrivers.cpp
        ThirdParty/ASIOSDK/host/pc/asiolist.cpp
    )
endif()

add_library(SteinbergASIO234 STATIC ${STEINBERG_ASIO_SOURCES})
target_include_directories(SteinbergASIO234 SYSTEM PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/ASIOSDK/common
    ${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/ASIOSDK/host
    ${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/ASIOSDK/host/pc
)
if(MSVC)
    target_compile_definitions(SteinbergASIO234 PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(SteinbergASIO234 PRIVATE /W0 /EHsc)
else()
    target_compile_options(SteinbergASIO234 PRIVATE -w)
endif()

# Strict compiler flags for AlphaStudio's own C++20 targets
set(ALPHASTUDIO_OWN_COMPILE_OPTIONS "")
if(MSVC)
    list(APPEND ALPHASTUDIO_OWN_COMPILE_OPTIONS /W4 /permissive- /EHsc)
else()
    list(APPEND ALPHASTUDIO_OWN_COMPILE_OPTIONS -Wall -Wextra -Wpedantic -Wno-unused-parameter)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|i686")
        list(APPEND ALPHASTUDIO_OWN_COMPILE_OPTIONS -msse2 -mfpmath=sse)
    endif()
endif()

# ==============================================================================
# Target 1: AlphaSDK (Official Steinberg ASIO 2.3.4 HAL, VST 3.0-3.8, VST 2.4, Bridge)
# ==============================================================================
set(ALPHASDK_SOURCES
    SDK/ASIO/ASIOHal.cpp
    SDK/VST3/VST3Interfaces.cpp
    SDK/VST2/VST2Interfaces.cpp
    SDK/Bridge/VSTBridgeIPC.cpp
)

add_library(AlphaSDK STATIC ${ALPHASDK_SOURCES})
target_compile_definitions(AlphaSDK PUBLIC ALPHASTUDIO_HAS_STEINBERG_ASIO_SDK=1)
target_compile_options(AlphaSDK PRIVATE ${ALPHASTUDIO_OWN_COMPILE_OPTIONS})
target_include_directories(AlphaSDK PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(AlphaSDK PUBLIC SteinbergASIO234 Threads::Threads)

if(WIN32)
    target_link_libraries(AlphaSDK PUBLIC advapi32 user32 winmm ole32 avrt ksuser)
elseif(APPLE)
    find_library(COREAUDIO_LIB CoreAudio)
    find_library(COREMIDI_LIB CoreMIDI)
    find_library(IOKIT_LIB IOKit)
    find_library(COREFOUNDATION_LIB CoreFoundation)
    if(COREAUDIO_LIB)
        target_link_libraries(AlphaSDK PUBLIC ${COREAUDIO_LIB} ${COREMIDI_LIB} ${IOKIT_LIB} ${COREFOUNDATION_LIB})
    endif()
elseif(UNIX AND NOT APPLE)
    target_link_libraries(AlphaSDK PUBLIC rt dl)
endif()

# ==============================================================================
# Target 2: AlphaCore (Real-Time Audio/MIDI/DSP Engine, Mixer, Sequencer, I/O)
# ==============================================================================
set(ALPHACORE_SOURCES
    Core/Threading/RealTimeThreading.cpp
    Core/Infrastructure/UndoRedoAndLicense.cpp
    Core/AudioSystem/VSTAudioSystem.cpp
    Core/AudioSystem/VSTConnections.cpp
    Core/Engine/AudioBufferAndDAG.cpp
    Core/Engine/AudioPool.cpp
    Core/Engine/OfflineProcessAndFreeze.cpp
    Core/Mixer/ChannelStripAndMixer.cpp
    Core/Mixer/ControlRoom.cpp
    Core/Sequencer/MusicalClockAndMIDI.cpp
    Core/Sequencer/TracksAndRecording.cpp
    Core/Workflow/VSTExpression.cpp
    Core/Workflow/LogicalEditors.cpp
    Core/Workflow/AutomationAndQuickControls.cpp
    Core/DSP/VariAudioAndPitch.cpp
    Core/DSP/AudioWarpAndSampleEditor.cpp
    Core/DSP/REVerenceConvolution.cpp
    Core/DSP/AudioEffectsSuite.cpp
    Core/DSP/MidiPluginsSuite.cpp
    Core/Instruments/VirtualInstrumentsSuite.cpp
    Core/Sync/RemoteAndSync.cpp
    Core/Persistence/ProjectAndMediaBay.cpp
    Core/Persistence/ExchangeAndBatchExport.cpp
)

add_library(AlphaCore STATIC ${ALPHACORE_SOURCES})
target_compile_options(AlphaCore PRIVATE ${ALPHASTUDIO_OWN_COMPILE_OPTIONS})
target_include_directories(AlphaCore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(AlphaCore PUBLIC AlphaSDK Threads::Threads)

# ==============================================================================
# Target 3: AlphaUI (Modular Dear ImGui + 2D DAW Rendering, Zero Video)
# ==============================================================================
set(ALPHAUI_SOURCES
    UI/ImGuiBackend2D.cpp
    UI/ProjectAndEditorsUI.cpp
    UI/MixerAndDialogsUI.cpp
    UI/InteractiveDAWApp.cpp
)

if(WIN32)
    target_link_libraries(AlphaUI PUBLIC gdi32 comdlg32 user32 dwmapi winmm)
endif()

add_library(AlphaUI STATIC ${ALPHAUI_SOURCES})
target_compile_options(AlphaUI PRIVATE ${ALPHASTUDIO_OWN_COMPILE_OPTIONS})
target_include_directories(AlphaUI PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(AlphaUI PUBLIC AlphaCore AlphaSDK)

if(ALPHASTUDIO_USE_EXTERNAL_IMGUI)
    target_compile_definitions(AlphaUI PUBLIC ALPHASTUDIO_HAS_EXTERNAL_IMGUI=1)
endif()

# ==============================================================================
# Target 4: AlphaVstBridge32 (Isolated IPC Host Process for 32/64-bit VSTs)
# ==============================================================================
if(ALPHASTUDIO_BUILD_BRIDGE_WORKER)
    add_executable(AlphaVstBridge32 SDK/Bridge/BridgeWorkerMain.cpp)
    target_compile_options(AlphaVstBridge32 PRIVATE ${ALPHASTUDIO_OWN_COMPILE_OPTIONS})
    target_link_libraries(AlphaVstBridge32 PRIVATE AlphaSDK Threads::Threads)
endif()

# ==============================================================================
# Target 5: AlphaStudio Main Executable (native GUI DAW, no console window)
# ==============================================================================
if(WIN32)
    add_executable(AlphaStudio WIN32 main.cpp)
else()
    add_executable(AlphaStudio main.cpp)
endif()
target_compile_options(AlphaStudio PRIVATE ${ALPHASTUDIO_OWN_COMPILE_OPTIONS})
target_link_libraries(AlphaStudio PRIVATE AlphaUI AlphaCore AlphaSDK Threads::Threads)

if(WIN32 AND MSVC)
    target_link_options(AlphaStudio PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)
endif()

if(ALPHASTUDIO_ENABLE_LTO AND CMAKE_BUILD_TYPE STREQUAL "Release")
    include(CheckIPOSupported)
    check_ipo_supported(RESULT LTO_SUPPORTED OUTPUT LTO_ERROR)
    if(LTO_SUPPORTED)
        set_property(TARGET AlphaSDK AlphaCore AlphaUI AlphaStudio PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
endif()
