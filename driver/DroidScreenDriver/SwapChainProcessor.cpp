// DroidScreen IddCx Virtual Display Driver
// SwapChainProcessor.cpp - Frame draining from the desktop compositor
//
// The swap chain processor runs in a dedicated thread and drains frames
// that the Windows compositor renders to our virtual display. We acquire
// each buffer and immediately release it — the actual screen capture for
// streaming is done by the DroidScreen desktop app via WGC.
//
// This "pump" is necessary because the compositor will stall if frames
// are not consumed. Even though we discard them, the driver must keep
// the pipeline moving.

#include "SwapChainProcessor.h"

using namespace DroidScreen;
using Microsoft::WRL::ComPtr;

// ===========================================================================
// Constructor — creates events and starts the processing thread
// ===========================================================================

SwapChainProcessor::SwapChainProcessor(
    IDDCX_SWAPCHAIN hSwapChain,
    ComPtr<IDXGIDevice> pDxgiDevice)
    : m_hSwapChain(hSwapChain)
    , m_pDxgiDevice(std::move(pDxgiDevice))
{
    // Create the termination event (manual-reset)
    m_hTerminateEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);

    // Get the swap chain's "new buffer available" event from IddCx
    m_hAvailableBufferEvent = IddCxSwapChainGetDirtyTracking(m_hSwapChain);

    DS_LOG("SwapChainProcessor: starting processing thread");

    // Start the worker thread
    m_Thread = std::thread([this]() { Run(); });
}

// ===========================================================================
// Destructor — signals the thread to stop and waits for it
// ===========================================================================

SwapChainProcessor::~SwapChainProcessor()
{
    DS_LOG("SwapChainProcessor: stopping processing thread");

    // Signal the thread to terminate
    if (m_hTerminateEvent)
    {
        SetEvent(m_hTerminateEvent);
    }

    // Wait for the thread to finish
    if (m_Thread.joinable())
    {
        m_Thread.join();
    }

    // Clean up the termination event (the buffer event is owned by IddCx)
    if (m_hTerminateEvent)
    {
        CloseHandle(m_hTerminateEvent);
        m_hTerminateEvent = nullptr;
    }

    DS_LOG("SwapChainProcessor: thread stopped");
}

// ===========================================================================
// Run — main loop of the processing thread
//
// Waits for either a new frame or the termination signal. When a frame
// is available, drains it immediately.
// ===========================================================================

void SwapChainProcessor::Run()
{
    DS_LOG("SwapChainProcessor::Run entered");

    // Optionally boost thread priority for real-time display work.
    // This is not strictly necessary since we just discard frames,
    // but it reduces compositor latency for the virtual display.
    DWORD taskIndex = 0;
    HANDLE hAvrt = AvSetMmThreadCharacteristicsW(L"DisplayPostProcessing", &taskIndex);

    HANDLE waitHandles[2] = { m_hTerminateEvent, m_hAvailableBufferEvent };

    while (true)
    {
        DWORD waitResult = WaitForMultipleObjects(
            2, waitHandles, FALSE, 100 /* ms timeout for periodic check */);

        if (waitResult == WAIT_OBJECT_0)
        {
            // Terminate event signaled
            DS_LOG("SwapChainProcessor: terminate signaled");
            break;
        }

        if (waitResult == WAIT_OBJECT_0 + 1 || waitResult == WAIT_TIMEOUT)
        {
            // New frame available (or timeout — try anyway to avoid stalls)
            ProcessFrame();
        }
    }

    // Revert thread characteristics
    if (hAvrt)
    {
        AvRevertMmThreadCharacteristics(hAvrt);
    }

    DS_LOG("SwapChainProcessor::Run exiting");
}

// ===========================================================================
// ProcessFrame — acquire one frame from the swap chain and release it
//
// This is the core "drain" operation. We get the surface from IddCx,
// do nothing with it, and immediately release it back. The compositor
// sees that we consumed the frame and continues rendering.
// ===========================================================================

void SwapChainProcessor::ProcessFrame()
{
    // Try to acquire a buffer from the swap chain
    IDARG_IN_RELEASEANDACQUIREBUFFER acquireArg = {};
    IDARG_OUT_RELEASEANDACQUIREBUFFER acquireOut = {};

    HRESULT hr = IddCxSwapChainReleaseAndAcquireBuffer(
        m_hSwapChain, &acquireArg, &acquireOut);

    if (hr == E_PENDING)
    {
        // No frame available right now — this is normal
        return;
    }

    if (FAILED(hr))
    {
        // If the swap chain is being torn down, this is expected.
        // Any other failure is worth logging.
        if (hr != DXGI_ERROR_ACCESS_LOST)
        {
            DS_ERR("SwapChainProcessor: ReleaseAndAcquire failed: 0x%08X", hr);
        }
        return;
    }

    // We successfully acquired a frame. The surface is in:
    //   acquireOut.MetaData.pSurface
    //
    // For DroidScreen, we intentionally do NOTHING with the surface.
    // The desktop app captures via Windows.Graphics.Capture (WGC), which
    // operates independently from the driver's swap chain.
    //
    // We just need to release the buffer on the next call to
    // IddCxSwapChainReleaseAndAcquireBuffer (it releases the previous
    // buffer when acquiring the next one), or when the swap chain is
    // unassigned.

    // The buffer will be automatically released on the next acquire call
    // or when the swap chain is destroyed. No explicit release needed
    // for a single-frame-at-a-time pattern.

    IddCxSwapChainFinishedProcessingFrame(m_hSwapChain);
}
