// DroidScreen IddCx Virtual Display Driver
// Driver.h - Common includes, trace macros, and context structures

#pragma once

#include <windows.h>
#include <wdf.h>
#include <iddcx.h>
#include <avrt.h>
#include <dxgi1_5.h>
#include <d3d11_4.h>
#include <wrl/client.h>

#include <memory>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>

namespace DroidScreen
{

// ---------------------------------------------------------------------------
// Debug / trace helpers
// ---------------------------------------------------------------------------

#if DBG
#define DS_LOG(fmt, ...) \
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, \
               "[DroidScreen] " fmt "\n", ##__VA_ARGS__)
#define DS_ERR(fmt, ...) \
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, \
               "[DroidScreen ERROR] " fmt "\n", ##__VA_ARGS__)
#else
#define DS_LOG(fmt, ...) ((void)0)
#define DS_ERR(fmt, ...) ((void)0)
#endif

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------

class SwapChainProcessor;
class IndirectMonitor;

// ---------------------------------------------------------------------------
// Adapter context — one per IddCx adapter (i.e. one per device instance)
// ---------------------------------------------------------------------------

struct DeviceContext
{
    WDFDEVICE     Device  = nullptr;
    IDDCX_ADAPTER Adapter = nullptr;

    // The single virtual monitor we create
    std::unique_ptr<IndirectMonitor> Monitor;
};

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DeviceContext, GetDeviceContext);

// ---------------------------------------------------------------------------
// Adapter context — stored on the IDDCX_ADAPTER WDF object.
// Contains a back-pointer to the parent WDFDEVICE so we can navigate
// from adapter callbacks back to our DeviceContext.
// ---------------------------------------------------------------------------

struct AdapterContext
{
    WDFDEVICE ParentDevice = nullptr;
};

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(AdapterContext, GetAdapterContext);

// ---------------------------------------------------------------------------
// Monitor context — stored in the IddCx monitor object
// ---------------------------------------------------------------------------

struct MonitorContext
{
    IDDCX_MONITOR Monitor = nullptr;

    // Swap chain processor for draining compositor frames
    std::unique_ptr<SwapChainProcessor> Processor;
};

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(MonitorContext, GetMonitorContext);

// ---------------------------------------------------------------------------
// IddCx callback declarations
// ---------------------------------------------------------------------------

extern "C"
{

// WDF entry points
DRIVER_INITIALIZE                           DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD                   EvtDeviceAdd;
EVT_WDF_DEVICE_D0_ENTRY                     EvtDeviceD0Entry;

// IddCx adapter callbacks
EVT_IDD_CX_ADAPTER_INIT_FINISHED           EvtAdapterInitFinished;
EVT_IDD_CX_ADAPTER_COMMIT_MODES            EvtAdapterCommitModes;

// IddCx monitor callbacks
EVT_IDD_CX_PARSE_MONITOR_DESCRIPTION       EvtParseMonitorDescription;
EVT_IDD_CX_MONITOR_GET_DEFAULT_DESCRIPTION_MODES  EvtMonitorGetDefaultModes;
EVT_IDD_CX_MONITOR_QUERY_TARGET_MODES      EvtMonitorQueryTargetModes;
EVT_IDD_CX_MONITOR_ASSIGN_SWAPCHAIN        EvtMonitorAssignSwapChain;
EVT_IDD_CX_MONITOR_UNASSIGN_SWAPCHAIN      EvtMonitorUnassignSwapChain;

} // extern "C"

} // namespace DroidScreen
