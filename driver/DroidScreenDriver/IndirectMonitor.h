// DroidScreen IddCx Virtual Display Driver
// IndirectMonitor.h - Monitor configuration and EDID generation

#pragma once

#include "Driver.h"

namespace DroidScreen
{

// ---------------------------------------------------------------------------
// Display mode definition
// ---------------------------------------------------------------------------

struct DisplayMode
{
    UINT Width;
    UINT Height;
    UINT RefreshRateNumerator;
    UINT RefreshRateDenominator;
};

// ---------------------------------------------------------------------------
// IndirectMonitor
//
// Manages the virtual monitor's configuration: supported display modes and
// EDID data. The driver creates one of these when the adapter is ready.
// ---------------------------------------------------------------------------

class IndirectMonitor
{
public:
    IndirectMonitor() = default;
    ~IndirectMonitor() = default;

    // Non-copyable
    IndirectMonitor(const IndirectMonitor&) = delete;
    IndirectMonitor& operator=(const IndirectMonitor&) = delete;

    // -----------------------------------------------------------------------
    // EDID
    // -----------------------------------------------------------------------

    // Returns a pointer to the 128-byte EDID block for this virtual monitor.
    static const BYTE* GetEdid();

    // Returns the size of the EDID block (always 128).
    static constexpr DWORD GetEdidSize() { return 128; }

    // -----------------------------------------------------------------------
    // Display modes
    // -----------------------------------------------------------------------

    // Returns the list of supported display modes.
    static const std::vector<DisplayMode>& GetSupportedModes();

    // Populates an array of IDDCX_TARGET_MODE from the supported modes.
    // Used by EvtMonitorQueryTargetModes.
    static void GetTargetModes(
        _Out_writes_(MaxModes) IDDCX_TARGET_MODE* pModes,
        UINT MaxModes,
        _Out_ UINT* pNumModes);

    // Populates an array of IDDCX_MONITOR_MODE for default/preferred modes.
    // Used by EvtMonitorGetDefaultModes.
    static void GetDefaultModes(
        _Out_writes_(MaxModes) IDDCX_MONITOR_MODE* pModes,
        UINT MaxModes,
        _Out_ UINT* pNumModes);

    // -----------------------------------------------------------------------
    // Monitor lifecycle
    // -----------------------------------------------------------------------

    // Creates the IddCx monitor object and signals arrival.
    // Called from EvtAdapterInitFinished.
    NTSTATUS CreateMonitor(IDDCX_ADAPTER Adapter);

    IDDCX_MONITOR GetHandle() const { return m_Monitor; }

private:
    IDDCX_MONITOR m_Monitor = nullptr;
};

} // namespace DroidScreen
