// DroidScreen IddCx Virtual Display Driver
// Driver.cpp - Main entry point and IddCx callback implementations
//
// This is a UMDF v2 Indirect Display Driver that creates a single virtual
// monitor on the system. The desktop app (DroidScreen) captures this display
// via WGC and streams it to an Android tablet over USB.

#include "Driver.h"
#include "IndirectMonitor.h"
#include "SwapChainProcessor.h"

using namespace DroidScreen;
using Microsoft::WRL::ComPtr;

// ===========================================================================
// DriverEntry
//
// Standard WDF driver entry point. Registers EvtDeviceAdd.
// ===========================================================================

extern "C" NTSTATUS DriverEntry(
    _In_ PDRIVER_OBJECT  pDriverObject,
    _In_ PUNICODE_STRING pRegistryPath)
{
    DS_LOG("DriverEntry");

    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, EvtDeviceAdd);

    NTSTATUS status = WdfDriverCreate(
        pDriverObject,
        pRegistryPath,
        WDF_NO_OBJECT_ATTRIBUTES,
        &config,
        WDF_NO_HANDLE);

    if (!NT_SUCCESS(status))
    {
        DS_ERR("WdfDriverCreate failed: 0x%08X", status);
    }

    return status;
}

// ===========================================================================
// EvtDeviceAdd
//
// Called by PnP when a new device instance is found. We set up the WDF device
// and initialize the IddCx adapter.
// ===========================================================================

extern "C" NTSTATUS EvtDeviceAdd(
    _In_ WDFDRIVER       Driver,
    _Inout_ PWDFDEVICE_INIT pDeviceInit)
{
    UNREFERENCED_PARAMETER(Driver);

    DS_LOG("EvtDeviceAdd");

    NTSTATUS status;

    // Let IddCx hook into the device init
    IddCxDeviceInitConfig initConfig;
    initConfig.EvtIddCxDeviceIoControl = nullptr; // No custom IOCTL handling

    status = IddCxDeviceInitialize(&pDeviceInit, &initConfig);
    if (!NT_SUCCESS(status))
    {
        DS_ERR("IddCxDeviceInitialize failed: 0x%08X", status);
        return status;
    }

    // Create the WDF device with our AdapterContext
    WDF_OBJECT_ATTRIBUTES deviceAttribs;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttribs, AdapterContext);

    WDFDEVICE device = nullptr;
    status = WdfDeviceCreate(&pDeviceInit, &deviceAttribs, &device);
    if (!NT_SUCCESS(status))
    {
        DS_ERR("WdfDeviceCreate failed: 0x%08X", status);
        return status;
    }

    // Store the device handle in our context
    auto* pContext = GetAdapterContext(device);
    pContext->Device = device;

    // Configure IddCx adapter callbacks
    IDD_CX_CLIENT_CONFIG iddConfig = {};
    iddConfig.Size = sizeof(iddConfig);

    iddConfig.EvtIddCxAdapterInitFinished         = EvtAdapterInitFinished;
    iddConfig.EvtIddCxAdapterCommitModes           = EvtAdapterCommitModes;
    iddConfig.EvtIddCxParseMonitorDescription      = EvtParseMonitorDescription;
    iddConfig.EvtIddCxMonitorGetDefaultDescriptionModes = EvtMonitorGetDefaultModes;
    iddConfig.EvtIddCxMonitorQueryTargetModes      = EvtMonitorQueryTargetModes;
    iddConfig.EvtIddCxMonitorAssignSwapChain       = EvtMonitorAssignSwapChain;
    iddConfig.EvtIddCxMonitorUnassignSwapChain     = EvtMonitorUnassignSwapChain;

    status = IddCxDeviceInitConfig(device, &iddConfig);
    if (!NT_SUCCESS(status))
    {
        DS_ERR("IddCxDeviceInitConfig failed: 0x%08X", status);
        return status;
    }

    // Configure power management — we need D0Entry to init the adapter
    WDF_PNPPOWER_EVENT_CALLBACKS pnpCallbacks;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpCallbacks);
    pnpCallbacks.EvtDeviceD0Entry = EvtDeviceD0Entry;
    WdfDeviceInitSetPnpPowerEventCallbacks(pDeviceInit, &pnpCallbacks);

    return status;
}

// ===========================================================================
// EvtDeviceD0Entry
//
// Called when the device enters the D0 (working) power state. We initialize
// the IddCx adapter here.
// ===========================================================================

extern "C" NTSTATUS EvtDeviceD0Entry(
    _In_ WDFDEVICE              Device,
    _In_ WDF_POWER_DEVICE_STATE PreviousState)
{
    UNREFERENCED_PARAMETER(PreviousState);

    DS_LOG("EvtDeviceD0Entry");

    auto* pContext = GetAdapterContext(Device);

    // Set up the adapter attributes
    IDDCX_ADAPTER_CAPS caps = {};
    caps.Size = sizeof(caps);
    caps.MaxMonitorsSupported = 1;
    caps.EndPointDiagnostics.Size = sizeof(caps.EndPointDiagnostics);
    caps.EndPointDiagnostics.GammaSupport = IDDCX_FEATURE_IMPLEMENTATION_NONE;
    caps.EndPointDiagnostics.TransmissionType = IDDCX_TRANSMISSION_TYPE_OTHER;
    caps.EndPointDiagnostics.pEndPointFriendlyName = L"DroidScreen Virtual Display";
    caps.EndPointDiagnostics.pEndPointManufacturerName = L"DroidScreen";
    caps.EndPointDiagnostics.pEndPointModelName = L"DroidScreen Virtual Display";

    // Initialize the adapter asynchronously
    IDARG_IN_ADAPTER_INIT adapterInit = {};
    adapterInit.WdfDevice = Device;
    adapterInit.pCaps = &caps;
    adapterInit.ObjectAttributes = WDF_NO_OBJECT_ATTRIBUTES;

    IDARG_OUT_ADAPTER_INIT adapterInitOut = {};
    NTSTATUS status = IddCxAdapterInitAsync(&adapterInit, &adapterInitOut);

    if (NT_SUCCESS(status))
    {
        pContext->Adapter = adapterInitOut.AdapterObject;
        DS_LOG("IddCxAdapterInitAsync succeeded, adapter=%p", pContext->Adapter);
    }
    else
    {
        DS_ERR("IddCxAdapterInitAsync failed: 0x%08X", status);
    }

    return status;
}

// ===========================================================================
// EvtAdapterInitFinished
//
// Called when adapter initialization is complete. Now we create our single
// virtual monitor.
// ===========================================================================

extern "C" NTSTATUS EvtAdapterInitFinished(
    _In_ IDDCX_ADAPTER Adapter,
    _In_ const IDARG_IN_ADAPTER_INIT_FINISHED* pInArgs)
{
    DS_LOG("EvtAdapterInitFinished, status=0x%08X", pInArgs->AdapterInitStatus);

    if (!NT_SUCCESS(pInArgs->AdapterInitStatus))
    {
        DS_ERR("Adapter init failed with status 0x%08X", pInArgs->AdapterInitStatus);
        return pInArgs->AdapterInitStatus;
    }

    // Get our device from the adapter's parent
    WDFDEVICE device = WdfObjectGetTypedContext<AdapterContext>(
        IddCxAdapterGetWdfDevice(Adapter))->Device;
    auto* pContext = GetAdapterContext(device);

    // Create the IndirectMonitor helper
    pContext->Monitor = std::make_unique<IndirectMonitor>();

    // Create the IddCx monitor and signal its arrival
    NTSTATUS status = pContext->Monitor->CreateMonitor(Adapter);
    if (!NT_SUCCESS(status))
    {
        DS_ERR("CreateMonitor failed: 0x%08X", status);
    }

    return status;
}

// ===========================================================================
// EvtAdapterCommitModes
//
// Called when the display mode is being committed. For an indirect display
// we just accept whatever Windows chose.
// ===========================================================================

extern "C" NTSTATUS EvtAdapterCommitModes(
    _In_ IDDCX_ADAPTER Adapter,
    _In_ const IDARG_IN_COMMITMODES* pInArgs)
{
    UNREFERENCED_PARAMETER(Adapter);
    UNREFERENCED_PARAMETER(pInArgs);

    DS_LOG("EvtAdapterCommitModes");
    return STATUS_SUCCESS;
}

// ===========================================================================
// EvtParseMonitorDescription
//
// Called by IddCx to parse our EDID. We tell it the monitor supports the
// modes we defined and return them.
// ===========================================================================

extern "C" NTSTATUS EvtParseMonitorDescription(
    _In_ const IDARG_IN_PARSEMONITORDESCRIPTION* pInArgs,
    _Out_ IDARG_OUT_PARSEMONITORDESCRIPTION* pOutArgs)
{
    DS_LOG("EvtParseMonitorDescription, DataSize=%u", pInArgs->MonitorDescriptionSize);

    // We only support EDID descriptions
    if (pInArgs->MonitorDescriptionType != IDDCX_MONITOR_DESCRIPTION_TYPE_EDID)
    {
        return STATUS_INVALID_PARAMETER;
    }

    // Report how many modes we support
    const auto& modes = IndirectMonitor::GetSupportedModes();
    pOutArgs->MonitorModeBufferOutputCount = static_cast<UINT>(modes.size());

    if (pInArgs->MonitorModeBufferInputCount == 0)
    {
        // Caller is just querying the count
        return STATUS_BUFFER_TOO_SMALL;
    }

    if (pInArgs->MonitorModeBufferInputCount < static_cast<UINT>(modes.size()))
    {
        return STATUS_BUFFER_TOO_SMALL;
    }

    // Fill in the monitor modes from EDID
    IndirectMonitor::GetDefaultModes(
        pInArgs->pMonitorModes,
        pInArgs->MonitorModeBufferInputCount,
        &pOutArgs->MonitorModeBufferOutputCount);

    // The first mode is the preferred/native mode
    pOutArgs->PreferredMonitorModeIdx = 0;

    return STATUS_SUCCESS;
}

// ===========================================================================
// EvtMonitorGetDefaultModes
//
// Called to get the default display modes when EDID is not available.
// We always provide EDID, but implement this as a fallback.
// ===========================================================================

extern "C" NTSTATUS EvtMonitorGetDefaultModes(
    _In_ IDDCX_MONITOR Monitor,
    _In_ const IDARG_IN_GETDEFAULTDESCRIPTIONMODES* pInArgs,
    _Out_ IDARG_OUT_GETDEFAULTDESCRIPTIONMODES* pOutArgs)
{
    UNREFERENCED_PARAMETER(Monitor);

    DS_LOG("EvtMonitorGetDefaultModes");

    const auto& modes = IndirectMonitor::GetSupportedModes();
    pOutArgs->DefaultMonitorModeBufferOutputCount = static_cast<UINT>(modes.size());

    if (pInArgs->DefaultMonitorModeBufferInputCount == 0)
    {
        return STATUS_BUFFER_TOO_SMALL;
    }

    if (pInArgs->DefaultMonitorModeBufferInputCount < static_cast<UINT>(modes.size()))
    {
        return STATUS_BUFFER_TOO_SMALL;
    }

    IndirectMonitor::GetDefaultModes(
        pInArgs->pDefaultMonitorModes,
        pInArgs->DefaultMonitorModeBufferInputCount,
        &pOutArgs->DefaultMonitorModeBufferOutputCount);

    pOutArgs->PreferredMonitorModeIdx = 0;

    return STATUS_SUCCESS;
}

// ===========================================================================
// EvtMonitorQueryTargetModes
//
// Reports the target (connector) modes that the display supports.
// ===========================================================================

extern "C" NTSTATUS EvtMonitorQueryTargetModes(
    _In_ IDDCX_MONITOR Monitor,
    _In_ const IDARG_IN_QUERYTARGETMODES* pInArgs,
    _Out_ IDARG_OUT_QUERYTARGETMODES* pOutArgs)
{
    UNREFERENCED_PARAMETER(Monitor);

    DS_LOG("EvtMonitorQueryTargetModes");

    const auto& modes = IndirectMonitor::GetSupportedModes();
    pOutArgs->TargetModeBufferOutputCount = static_cast<UINT>(modes.size());

    if (pInArgs->TargetModeBufferInputCount == 0)
    {
        return STATUS_BUFFER_TOO_SMALL;
    }

    if (pInArgs->TargetModeBufferInputCount < static_cast<UINT>(modes.size()))
    {
        return STATUS_BUFFER_TOO_SMALL;
    }

    IndirectMonitor::GetTargetModes(
        pInArgs->pTargetModes,
        pInArgs->TargetModeBufferInputCount,
        &pOutArgs->TargetModeBufferOutputCount);

    return STATUS_SUCCESS;
}

// ===========================================================================
// EvtMonitorAssignSwapChain
//
// Called when the compositor assigns a swap chain to our monitor. We create
// a SwapChainProcessor to drain frames.
// ===========================================================================

extern "C" NTSTATUS EvtMonitorAssignSwapChain(
    _In_ IDDCX_MONITOR Monitor,
    _In_ const IDARG_IN_SETSWAPCHAIN* pInArgs)
{
    DS_LOG("EvtMonitorAssignSwapChain");

    auto* pMonCtx = GetMonitorContext(Monitor);

    // Create a DXGI device from the rendering device provided by IddCx
    ComPtr<IDXGIDevice> dxgiDevice;
    HRESULT hr = pInArgs->pRenderDevice->QueryInterface(
        IID_PPV_ARGS(&dxgiDevice));

    if (FAILED(hr))
    {
        DS_ERR("QueryInterface for IDXGIDevice failed: 0x%08X", hr);
        return STATUS_UNSUCCESSFUL;
    }

    // Create the swap chain processor - it starts its thread immediately
    pMonCtx->Processor = std::make_unique<SwapChainProcessor>(
        pInArgs->hSwapChain, dxgiDevice);

    DS_LOG("SwapChainProcessor created");
    return STATUS_SUCCESS;
}

// ===========================================================================
// EvtMonitorUnassignSwapChain
//
// Called when the compositor removes the swap chain. Destroy the processor.
// ===========================================================================

extern "C" NTSTATUS EvtMonitorUnassignSwapChain(
    _In_ IDDCX_MONITOR Monitor)
{
    DS_LOG("EvtMonitorUnassignSwapChain");

    auto* pMonCtx = GetMonitorContext(Monitor);

    // Destroying the processor stops its thread and cleans up
    pMonCtx->Processor.reset();

    DS_LOG("SwapChainProcessor destroyed");
    return STATUS_SUCCESS;
}
