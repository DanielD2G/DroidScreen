// DroidScreen IddCx Virtual Display Driver
// IndirectMonitor.cpp - Monitor configuration, EDID, and display modes

#include "IndirectMonitor.h"

using namespace DroidScreen;

// ===========================================================================
// EDID Block
//
// Minimal valid 128-byte EDID for the DroidScreen virtual display.
//
// Key fields:
//   Manufacturer: "DRS" (DroidScreen)
//   Product Code: 0xD5C0
//   Serial:       0x0000D50D
//   Week/Year:    Week 1, 2024
//   EDID Version: 1.3
//   Display:      Digital input, 8-bit color depth
//   Preferred:    2560x1600 @ 60Hz
//   Name:         "DroidScreen"
//
// Manufacturer ID encoding for "DRS":
//   D=4, R=18, S=19  -> ((4 << 10) | (18 << 5) | 19) = 0x1253
//   Stored big-endian: 0x12, 0x53
// ===========================================================================

static const BYTE s_Edid[128] =
{
    // Bytes 0-7: Header
    0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00,

    // Bytes 8-9: Manufacturer ID "DRS" = 0x1253 (big-endian)
    0x12, 0x53,

    // Bytes 10-11: Product code 0xD5C0 (little-endian)
    0xC0, 0xD5,

    // Bytes 12-15: Serial number 0x0000D50D (little-endian)
    0x0D, 0xD5, 0x00, 0x00,

    // Byte 16: Manufacture week (1)
    0x01,

    // Byte 17: Manufacture year (2024 - 1990 = 34 = 0x22)
    0x22,

    // Byte 18: EDID version (1)
    0x01,

    // Byte 19: EDID revision (3)
    0x03,

    // Byte 20: Video input definition
    //   Bit 7: Digital input (1)
    //   Bits 4-6: Color bit depth = 8 bits (010)
    //   Bits 0-3: Interface = undefined (0000)
    0xA0,

    // Byte 21: Horizontal screen size (cm) - ~34cm for a 13" tablet
    0x22,

    // Byte 22: Vertical screen size (cm) - ~21cm
    0x15,

    // Byte 23: Display gamma (2.20 = (gamma * 100) - 100 = 120 = 0x78)
    0x78,

    // Byte 24: Feature support
    //   Bit 7: DPMS standby not supported
    //   Bit 6: DPMS suspend not supported
    //   Bit 5: DPMS active-off not supported
    //   Bits 3-4: RGB color
    //   Bit 2: Preferred timing in DTD1
    //   Bit 1: Standard sRGB
    //   Bit 0: Continuous frequency not supported
    0x06,

    // Bytes 25-34: Chromaticity coordinates (sRGB-ish values)
    0xEE, 0x91, 0xA3, 0x54, 0x4C, 0x99, 0x26, 0x0F, 0x50, 0x54,

    // Bytes 35-36: Established timings I & II
    0x00, 0x00,

    // Byte 37: Manufacturer reserved timing
    0x00,

    // Bytes 38-53: Standard timing descriptors (8 x 2 bytes, unused = 0x0101)
    0x01, 0x01,  // unused
    0x01, 0x01,  // unused
    0x01, 0x01,  // unused
    0x01, 0x01,  // unused
    0x01, 0x01,  // unused
    0x01, 0x01,  // unused
    0x01, 0x01,  // unused
    0x01, 0x01,  // unused

    // -----------------------------------------------------------------------
    // Detailed Timing Descriptor 1 (bytes 54-71): 2560x1600 @ 60Hz
    //
    // Pixel clock = 268.50 MHz -> 26850 (x10kHz) = 0x68D2
    //
    // Horizontal:
    //   Active: 2560, Blanking: 160
    //   Front porch: 48, Sync width: 32
    //   Total: 2720
    //
    // Vertical:
    //   Active: 1600, Blanking: 46
    //   Front porch: 3, Sync width: 6
    //   Total: 1646
    //
    // Pixel clock check: 2720 * 1646 * 60 = 268,627,200 ~ 268.50 MHz
    // -----------------------------------------------------------------------
    0xD2, 0x68,           // Pixel clock (little-endian) = 268.50 MHz

    0x00,                 // H active low 8 bits (2560 & 0xFF = 0x00)
    0xA0,                 // H blanking low 8 bits (160 & 0xFF = 0xA0)
    0x0A,                 // H active high nibble (2560 >> 8 = 0xA) | H blanking high nibble (0)
                          // = (0xA << 4) | 0x0 = 0xA0 ... wait, 160 >> 8 = 0, so = 0xA0
                          // Corrected: upper nibble = 2560>>8 = 10 = 0xA, lower = 160>>8 = 0
                          // = 0xA0

    0x40,                 // V active low 8 bits (1600 & 0xFF = 0x40)
    0x2E,                 // V blanking low 8 bits (46 & 0xFF = 0x2E)
    0x60,                 // V active high nibble (1600 >> 8 = 6) | V blanking high nibble (0)
                          // = (0x6 << 4) | 0x0 = 0x60

    0x30,                 // H front porch low 8 bits (48)
    0x20,                 // H sync width low 8 bits (32)

    0x36,                 // V front porch high nibble (3) | V sync width low nibble (6)
                          // = (0x3 << 4) | 0x6 = 0x36

    0x00,                 // Upper 2 bits of H front porch, H sync, V front porch, V sync

    0x22,                 // H image size low 8 bits (cm) = 34 = 0x22
    0x15,                 // V image size low 8 bits (cm) = 21 = 0x15
    0x00,                 // Upper nibbles of image size

    0x00,                 // H border pixels
    0x00,                 // V border pixels

    0x1E,                 // Flags: non-interlaced, no stereo, digital separate sync,
                          // H sync positive, V sync positive

    // -----------------------------------------------------------------------
    // Descriptor 2 (bytes 72-89): Monitor name
    // Tag = 0xFC
    // -----------------------------------------------------------------------
    0x00, 0x00, 0x00,     // Flag (not a timing descriptor)
    0xFC,                  // Tag: Monitor name
    0x00,                  // Flag
    // "DroidScreen\n " (13 chars, padded with 0x0A then 0x20)
    'D', 'r', 'o', 'i', 'd', 'S', 'c', 'r', 'e', 'e', 'n', 0x0A, 0x20,

    // -----------------------------------------------------------------------
    // Descriptor 3 (bytes 90-107): Monitor range limits
    // Tag = 0xFD
    // -----------------------------------------------------------------------
    0x00, 0x00, 0x00,     // Flag
    0xFD,                  // Tag: Monitor range limits
    0x00,                  // Flag
    0x38,                  // Min V rate: 56 Hz
    0x4C,                  // Max V rate: 76 Hz
    0x1E,                  // Min H rate: 30 kHz
    0xA0,                  // Max H rate: 160 kHz
    0x1B,                  // Max pixel clock: 270 MHz (27 * 10)
    0x00,                  // Extended timing type: default GTF
    0x0A,                  // Padding
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, // Padding

    // -----------------------------------------------------------------------
    // Descriptor 4 (bytes 108-125): Monitor serial number
    // Tag = 0xFF
    // -----------------------------------------------------------------------
    0x00, 0x00, 0x00,     // Flag
    0xFF,                  // Tag: Serial number string
    0x00,                  // Flag
    'D', 'S', '-', '0', '0', '0', '1', 0x0A,  // "DS-0001\n"
    0x20, 0x20, 0x20, 0x20, 0x20,              // Padding

    // Byte 126: Extension count (0, no extensions)
    0x00,

    // Byte 127: Checksum (will be computed below at compile time, placeholder)
    0x00
};

// ---------------------------------------------------------------------------
// Compute the EDID checksum at runtime init and patch it
// The sum of all 128 bytes must equal 0 (mod 256).
// ---------------------------------------------------------------------------

static BYTE s_EdidWithChecksum[128];
static bool s_EdidReady = false;

static void EnsureEdidChecksum()
{
    if (s_EdidReady) return;

    memcpy(s_EdidWithChecksum, s_Edid, 128);

    BYTE sum = 0;
    for (int i = 0; i < 127; i++)
    {
        sum += s_EdidWithChecksum[i];
    }
    s_EdidWithChecksum[127] = static_cast<BYTE>(256 - sum);

    s_EdidReady = true;
}

// ===========================================================================
// Supported display modes
// ===========================================================================

static const std::vector<DisplayMode> s_SupportedModes =
{
    { 2560, 1600, 60, 1 },   // Tablet native (e.g. Pixel Tablet, Galaxy Tab S)
    { 1920, 1200, 60, 1 },   // 16:10 reduced
    { 1280,  800, 60, 1 },   // 16:10 compact
    { 1920, 1080, 60, 1 },   // 16:9 Full HD
    { 3840, 2160, 60, 1 },   // 4K option
};

// ===========================================================================
// IndirectMonitor public API
// ===========================================================================

const BYTE* IndirectMonitor::GetEdid()
{
    EnsureEdidChecksum();
    return s_EdidWithChecksum;
}

const std::vector<DisplayMode>& IndirectMonitor::GetSupportedModes()
{
    return s_SupportedModes;
}

// ---------------------------------------------------------------------------
// Fill IDDCX_TARGET_MODE array from our supported modes
// ---------------------------------------------------------------------------

void IndirectMonitor::GetTargetModes(
    _Out_writes_(MaxModes) IDDCX_TARGET_MODE* pModes,
    UINT MaxModes,
    _Out_ UINT* pNumModes)
{
    const auto& modes = GetSupportedModes();
    UINT count = min(MaxModes, static_cast<UINT>(modes.size()));

    for (UINT i = 0; i < count; i++)
    {
        auto& mode = modes[i];

        pModes[i] = {};
        pModes[i].Size = sizeof(IDDCX_TARGET_MODE);

        DISPLAYCONFIG_VIDEO_SIGNAL_INFO& signal =
            pModes[i].TargetVideoSignalInfo.targetVideoSignalInfo;

        signal.totalSize.cx = mode.Width;
        signal.totalSize.cy = mode.Height;
        signal.activeSize.cx = mode.Width;
        signal.activeSize.cy = mode.Height;

        signal.AdditionalSignalInfo.vSyncFreqDivider = 1;
        signal.AdditionalSignalInfo.videoStandard = 255; // Other

        signal.vSyncFreq.Numerator   = mode.RefreshRateNumerator;
        signal.vSyncFreq.Denominator  = mode.RefreshRateDenominator;

        signal.hSyncFreq.Numerator   = mode.RefreshRateNumerator * mode.Height;
        signal.hSyncFreq.Denominator  = mode.RefreshRateDenominator;

        signal.scanLineOrdering =
            DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;

        // Pixel rate = width * height * refreshRate
        signal.pixelRate =
            static_cast<UINT64>(mode.Width) *
            static_cast<UINT64>(mode.Height) *
            static_cast<UINT64>(mode.RefreshRateNumerator) /
            static_cast<UINT64>(mode.RefreshRateDenominator);
    }

    *pNumModes = count;
}

// ---------------------------------------------------------------------------
// Fill IDDCX_MONITOR_MODE array from our supported modes
// ---------------------------------------------------------------------------

void IndirectMonitor::GetDefaultModes(
    _Out_writes_(MaxModes) IDDCX_MONITOR_MODE* pModes,
    UINT MaxModes,
    _Out_ UINT* pNumModes)
{
    const auto& modes = GetSupportedModes();
    UINT count = min(MaxModes, static_cast<UINT>(modes.size()));

    for (UINT i = 0; i < count; i++)
    {
        auto& mode = modes[i];

        pModes[i] = {};
        pModes[i].Size = sizeof(IDDCX_MONITOR_MODE);
        pModes[i].Origin = IDDCX_MONITOR_MODE_ORIGIN_DRIVER;

        DISPLAYCONFIG_VIDEO_SIGNAL_INFO& signal =
            pModes[i].MonitorVideoSignalInfo;

        signal.totalSize.cx  = mode.Width;
        signal.totalSize.cy  = mode.Height;
        signal.activeSize.cx = mode.Width;
        signal.activeSize.cy = mode.Height;

        signal.AdditionalSignalInfo.vSyncFreqDivider = 1;
        signal.AdditionalSignalInfo.videoStandard = 255;

        signal.vSyncFreq.Numerator   = mode.RefreshRateNumerator;
        signal.vSyncFreq.Denominator  = mode.RefreshRateDenominator;

        signal.hSyncFreq.Numerator   = mode.RefreshRateNumerator * mode.Height;
        signal.hSyncFreq.Denominator  = mode.RefreshRateDenominator;

        signal.scanLineOrdering =
            DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;

        signal.pixelRate =
            static_cast<UINT64>(mode.Width) *
            static_cast<UINT64>(mode.Height) *
            static_cast<UINT64>(mode.RefreshRateNumerator) /
            static_cast<UINT64>(mode.RefreshRateDenominator);
    }

    *pNumModes = count;
}

// ===========================================================================
// CreateMonitor
//
// Creates the IddCx monitor object and signals its arrival to Windows.
// After this call, the virtual display appears in Display Settings.
// ===========================================================================

NTSTATUS IndirectMonitor::CreateMonitor(IDDCX_ADAPTER Adapter)
{
    DS_LOG("IndirectMonitor::CreateMonitor");

    // Prepare monitor info with our EDID
    IDDCX_MONITOR_INFO monitorInfo = {};
    monitorInfo.Size = sizeof(monitorInfo);
    monitorInfo.MonitorType = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_OTHER;
    monitorInfo.ConnectorIndex = 0;

    monitorInfo.MonitorDescription.Size = sizeof(monitorInfo.MonitorDescription);
    monitorInfo.MonitorDescription.Type = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;
    monitorInfo.MonitorDescription.DataSize = GetEdidSize();
    monitorInfo.MonitorDescription.pData = const_cast<BYTE*>(GetEdid());

    // Configure the monitor context
    WDF_OBJECT_ATTRIBUTES monitorAttribs;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&monitorAttribs, MonitorContext);

    // Create the monitor
    IDARG_IN_MONITORCREATE monitorCreate = {};
    monitorCreate.ObjectAttributes = &monitorAttribs;
    monitorCreate.pMonitorInfo = &monitorInfo;

    IDARG_OUT_MONITORCREATE monitorCreateOut = {};
    NTSTATUS status = IddCxMonitorCreate(Adapter, &monitorCreate, &monitorCreateOut);

    if (!NT_SUCCESS(status))
    {
        DS_ERR("IddCxMonitorCreate failed: 0x%08X", status);
        return status;
    }

    m_Monitor = monitorCreateOut.MonitorObject;
    DS_LOG("IddCxMonitorCreate succeeded, monitor=%p", m_Monitor);

    // Initialize the monitor context
    auto* pMonCtx = GetMonitorContext(m_Monitor);
    pMonCtx->Monitor = m_Monitor;

    // Signal monitor arrival — this makes Windows see the display
    IDARG_OUT_MONITORARRIVAL arrivalOut = {};
    status = IddCxMonitorArrival(m_Monitor, &arrivalOut);

    if (!NT_SUCCESS(status))
    {
        DS_ERR("IddCxMonitorArrival failed: 0x%08X", status);
    }
    else
    {
        DS_LOG("IddCxMonitorArrival succeeded — virtual display is now active");
    }

    return status;
}
