// DroidScreen IddCx Virtual Display Driver
// SwapChainProcessor.h - Frame draining from the desktop compositor

#pragma once

#include "Driver.h"

namespace DroidScreen
{

// ---------------------------------------------------------------------------
// SwapChainProcessor
//
// The Windows compositor renders frames to the virtual display's swap chain.
// This processor runs in a dedicated thread, acquiring and releasing buffers
// to keep the compositor happy.
//
// DroidScreen does NOT read pixel data from the driver side. The desktop
// application captures the virtual display via WGC (Windows.Graphics.Capture).
// The driver's sole job is to make the virtual display EXIST so Windows
// renders a desktop onto it.
// ---------------------------------------------------------------------------

class SwapChainProcessor
{
public:
    SwapChainProcessor(
        IDDCX_SWAPCHAIN hSwapChain,
        Microsoft::WRL::ComPtr<IDXGIDevice> pDxgiDevice);

    ~SwapChainProcessor();

    // Non-copyable
    SwapChainProcessor(const SwapChainProcessor&) = delete;
    SwapChainProcessor& operator=(const SwapChainProcessor&) = delete;

private:
    // The processing thread entry point
    void Run();

    // Process a single available frame from the swap chain
    void ProcessFrame();

    // IddCx swap chain handle
    IDDCX_SWAPCHAIN m_hSwapChain = nullptr;

    // DXGI device associated with this swap chain
    Microsoft::WRL::ComPtr<IDXGIDevice> m_pDxgiDevice;

    // Worker thread that drains the swap chain
    std::thread m_Thread;

    // Event signaled by IddCx when a new frame is available
    HANDLE m_hAvailableBufferEvent = nullptr;

    // Event signaled to terminate the processing thread
    HANDLE m_hTerminateEvent = nullptr;
};

} // namespace DroidScreen
