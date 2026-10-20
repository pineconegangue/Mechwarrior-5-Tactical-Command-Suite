// HUDDisplay / Main.cpp
//
// Second-screen MFD for MW5. Two kinds of view live here:
//
//   * The MWVR HUD widgets, mirrored out of the game as shared D3D11 textures through
//     the MechWarriorVR_HUDExport memory-mapped file; the game's own HUD screens, maps and
//     zoom camera from the VanillaHud / LidarExport plugins.
//   * The LIDAR tactical scope (see LidarView.h), a Direct2D port of the standalone
//     LidarDisplay.exe. That app and its source are untouched; this hosts a copy.
//
// Both draw into the one swap chain this file owns. The D3D11 device carries
// BGRA support so Direct2D can target the same back buffer, which is also what the
// MFD bezel labels are drawn with.
//
// The app no longer requires the game: it opens immediately, and both memory-mapped
// files are retried in the background so screens can be laid out before MW5 launches.

// windows.h first: d2d1_1.h and the DXGI headers expect the base Win32 types to be
// declared already.
#include <windows.h>
#include <windowsx.h>

#include <d2d1.h>
#include <d2d1_1.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dwrite.h>
#include <dxgi.h>
#include <dxgi1_5.h>
#include <mmsystem.h>
#include <wrl/client.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "LidarView.h"
#include "TopoView.h"
#include "Topo3DView.h"
#include "VanillaHudShared.h"
#include "VoiceIntentShared.h" // 2026-10-19: V-LNC - VoiceCommand's orders in words, open questions, who talks
#include "LanceStatusShared.h" // 2026-10-12: TacOps "lock this enemy" - who detects what (read only)
#include "GlobalSettings.h" // 2026-10-07f: the lance switches live in GlobalSettings.ini (all windows)
#include "HudProfiles.h"   // 2026-10-20: edit mode SAVE AS - a new profile (config + shortcut)

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "winmm.lib")

// Note: <windows.h> #defines DrawText -> DrawTextW, and it is included before d2d1.h
// (which d2d1_1.h requires), so ID2D1RenderTarget's DrawText is declared as DrawTextW
// here. Rather than #undef the macro and lose the binding, all text in this file goes
// through CreateTextLayout + DrawTextLayout, same as the LIDAR renderer does.

using Microsoft::WRL::ComPtr;

// Must match Renderer.h exactly
struct ExportControlBlock
{
    uint32_t SelectedWidgetIndex;
    uint32_t WidgetCount;

    struct WidgetInfo
    {
        HANDLE SharedTextureHandle;
        uint32_t Width;
        uint32_t Height;
        uint8_t Type;
        bool Active;
        uint8_t Padding[2];
    } Widgets[10];

    bool ExportEnabled;
};

const char *GetWidgetTypeName(uint8_t type)
{
    const char *names[] = {"Torso Crosshair", "Arms Crosshair", "Arms Target Crosshair", "Head Crosshair",
                           "Target Screen",   "Weapons Screen", "Owner Screen",          "Objectives Screen",
                           "Map Screen",      "Comms Screen"};
    return type < 10 ? names[type] : "Unknown";
}

// ---- MFD bezel labels -----------------------------------------------------
// Each label is one bezel button: what it says, where it sits, what it does and which
// key fires it. Positions are normalised to the client area so a label stays on its
// bezel button when the window is resized. Everything here is persisted.

enum ViewAction : uint8_t
{
    ACT_None = 0,      // an unassigned bezel button - drawn as an empty outline
    ACT_Widget,        // Param = widget type (4..9)
    ACT_Lidar,         // the LIDAR scope
    ACT_ScreenCapture, // the CAP page: the plugin's zoom camera
    ACT_LidarZoomIn,   // shorten the LIDAR scope range one step
    ACT_LidarZoomOut,  // lengthen it one step
    // APPEND ONLY BELOW THIS LINE. The config stores Action as an integer index, so
    // inserting anywhere above renumbers every label in every existing .cfg file.
    ACT_CaptureZoomIn,  // zoom camera: one step in
    ACT_CaptureZoomOut, // zoom camera: one step out
    ACT_Topo,           // the topographic map, drawn from MW5_TerrainExport
    ACT_Topo3D,         // the 3D tactical page: terrain plus the full LIDAR picture
    ACT_CamRecentre,    // snap the 3D camera back to heading-up chase
    ACT_CamLeft,        // orbit / pan, 3D page only
    ACT_CamRight,
    ACT_CamUp,
    ACT_CamDown,
    ACT_CamFwd,
    ACT_CamBack,
    // RETIRED (label mode, select next / previous): the 3D page's labels no longer have modes.
    // The numbers stay reserved because configs store Action as a number.
    ACT_RetiredLabelMode, // reserved numbers - load as ACT_None (ParseLabelLine)
    ACT_RetiredSelNext,
    ACT_RetiredSelPrev,
    ACT_VanillaWidget,  // Param = VanillaScreen: a screen built from the game's own HUD (VanillaHud.dll)
    ACT_ZoomCamIR,      // toggle IR on the CAP page's zoom camera (VanillaHud.dll)
    ACT_TacOverlay,     // the TacOps page over the game, toggled (VanillaHud.dll draws it)
    ACT_TacOverlayHold, // ... shown while the key is held
    ACT_EiVision,       // v22: Enhanced Imaging full screen over the game (toggle; any window, one renders it)
    ACT_Compass,        // 2026-10-12: the heading compass on this window (strip, rings, clock, LIDAR ring) on / off
    ACT_LancePage,      // 2026-10-19: V-LNC - the three lancemates, TacOps plates at page size (+ ammo, current order)
    ACT_COUNT
};

// Everything a bezel button can be set to, in the order the edit menu steps through.
struct ActionChoice
{
    ViewAction Action;
    uint8_t Param;
};

static const ActionChoice kActionChoices[] = {
    {ACT_None, 0},          {ACT_Widget, 4},        {ACT_Widget, 5},       {ACT_Widget, 6},
    {ACT_Widget, 7},        {ACT_Widget, 8},        {ACT_Widget, 9},       {ACT_Lidar, 0},
    {ACT_ScreenCapture, 0}, {ACT_LidarZoomIn, 0},   {ACT_LidarZoomOut, 0},
    {ACT_Topo, 0},          {ACT_Topo3D, 0},        {ACT_CamRecentre, 0},
    {ACT_CamLeft, 0},       {ACT_CamRight, 0},      {ACT_CamUp, 0},
    {ACT_CamDown, 0},       {ACT_CamFwd, 0},        {ACT_CamBack, 0},
    // The vanilla HUD screens, in VanillaScreen order.
    {ACT_VanillaWidget, VSC_Target},     {ACT_VanillaWidget, VSC_Weapons}, {ACT_VanillaWidget, VSC_Owner},
    {ACT_VanillaWidget, VSC_Objectives}, {ACT_VanillaWidget, VSC_Map},     {ACT_VanillaWidget, VSC_Comms},
    {ACT_VanillaWidget, VSC_Tactical},   {ACT_VanillaWidget, VSC_Targeting},
    {ACT_ZoomCamIR, 0},
    {ACT_VanillaWidget, VSC_TacOps},
    {ACT_VanillaWidget, VSC_Enhanced}, // v21: Enhanced Imaging
    {ACT_TacOverlay, 0},     {ACT_TacOverlayHold, 0},
    {ACT_EiVision, 0},
    {ACT_Compass, 0},
    {ACT_LancePage, 0}, // V-LNC
};
static const int kActionChoiceCount = (int)(sizeof(kActionChoices) / sizeof(kActionChoices[0]));

struct LabelSlot
{
    std::wstring Text;
    float NX = 0.5f; // normalised centre, 0..1 of client width
    float NY = 0.5f; // normalised centre, 0..1 of client height
    ViewAction Action = ACT_None;
    uint8_t Param = 0;      // widget type for ACT_Widget
    UINT Key = 0;           // virtual-key code, 0 = unbound
    UINT Mods = 0;          // MOD_CONTROL | MOD_ALT | MOD_SHIFT
    bool CustomText = false; // true once typed in edit mode - stops the abbreviation
                             // table overwriting it on load

    // Cached DirectWrite layout. Building one is the expensive part of drawing text, and
    // the legend changes only when edited - rebuilding all twenty every frame was pure
    // waste. Invalidated by clearing LayoutFor whenever Text changes.
    ComPtr<IDWriteTextLayout> Layout;
    std::wstring LayoutFor;
};

// Legends are written to a '|'-separated config field and narrowed to char on save, so
// keep typed text to printable ASCII and out of the separator's way.
static constexpr size_t kMaxLabelText = 8;

static bool IsAllowedLabelChar(wchar_t c)
{
    return c >= 32 && c < 127 && c != L'|';
}

// The config file is narrow text, legends are wide. Because IsAllowedLabelChar restricts
// typed legends to printable ASCII, both conversions are lossless - but the iterator-pair
// std::string(wide.begin(), wide.end()) form makes MSVC (rightly) warn about an implicit
// wchar_t -> char narrowing, so spell it out instead. A character that somehow slipped
// through becomes '?' rather than a silently truncated byte.
static std::string NarrowLabel(const std::wstring &w)
{
    std::string out;
    out.reserve(w.size());
    for (wchar_t c : w)
        out.push_back(IsAllowedLabelChar(c) ? static_cast<char>(c) : '?');
    return out;
}

static std::wstring WidenLabel(const std::string &s)
{
    std::wstring out;
    out.reserve(s.size());
    for (unsigned char c : s)
        out.push_back(static_cast<wchar_t>(c));
    return out;
}

// What the bezel shows: short all-caps legends, the way an F/A-18 DDI labels its
// pushbuttons. Change these here and every label follows.
static const wchar_t *ActionAbbrev(ViewAction a, uint8_t param)
{
    switch (a)
    {
    case ACT_Widget:
        switch (param)
        {
        case 4:
            return L"TGT";
        case 5:
            return L"WPN";
        case 6:
            return L"OWN";
        case 7:
            return L"OBJ";
        case 8:
            return L"MAP";
        case 9:
            return L"COMM";
        default:
            return L"WDGT";
        }
    case ACT_Lidar:
        return L"LIDR";
    case ACT_ScreenCapture:
        return L"CAP";
    case ACT_LidarZoomIn:
        return L"ZM+";
    case ACT_LidarZoomOut:
        return L"ZM-";
    case ACT_CaptureZoomIn:
        return L"CZ+";
    case ACT_CaptureZoomOut:
        return L"CZ-";
    case ACT_Topo:
        return L"TOPO";
    case ACT_Topo3D:
        return L"3D";
    case ACT_CamRecentre:
        return L"CTR";
    case ACT_CamLeft:
        return L"<";
    case ACT_CamRight:
        return L">";
    case ACT_CamUp:
        return L"^";
    case ACT_CamDown:
        return L"v";
    case ACT_CamFwd:
        return L"FWD";
    case ACT_CamBack:
        return L"BACK";
    case ACT_VanillaWidget:
        return VanillaScreenAbbrev(param);
    case ACT_ZoomCamIR:
        return L"IR";
    case ACT_TacOverlay:
        return L"OVL";
    case ACT_TacOverlayHold:
        return L"OVL*";
    case ACT_EiVision:
        return L"E-VIS";
    case ACT_Compass:
        return L"CMPS";
    case ACT_LancePage:
        return L"V-LNC";
    default:
        return L""; // unassigned - drawn as an empty outline
    }
}

// Spelled out, for the edit panel only.
static const wchar_t *ActionFullName(ViewAction a, uint8_t param)
{
    switch (a)
    {
    case ACT_Widget:
        switch (param)
        {
        case 4:
            return L"Target Screen";
        case 5:
            return L"Weapons Screen";
        case 6:
            return L"Owner Screen";
        case 7:
            return L"Objectives Screen";
        case 8:
            return L"Map Screen";
        case 9:
            return L"Comms Screen";
        default:
            return L"Widget";
        }
    case ACT_Lidar:
        return L"LIDAR scope";
    case ACT_ScreenCapture:
        return L"Zoom camera (CAP)";
    case ACT_LidarZoomIn:
        return L"Zoom in (active page)";
    case ACT_LidarZoomOut:
        return L"Zoom out (active page)";
    case ACT_CaptureZoomIn:
        return L"Zoom camera: zoom in";
    case ACT_CaptureZoomOut:
        return L"Zoom camera: zoom out";
    case ACT_Topo:
        return L"Topographic map";
    case ACT_Topo3D:
        return L"3D tactical map";
    case ACT_CamRecentre:
        return L"Camera recentre";
    case ACT_CamLeft:
        return L"Camera orbit left";
    case ACT_CamRight:
        return L"Camera orbit right";
    case ACT_CamUp:
        return L"Camera pitch up";
    case ACT_CamDown:
        return L"Camera pitch down";
    case ACT_CamFwd:
        return L"Camera pan forward";
    case ACT_CamBack:
        return L"Camera pan back";
    case ACT_VanillaWidget:
        return VanillaScreenName(param);
    case ACT_ZoomCamIR:
        return L"Zoom camera IR";
    case ACT_TacOverlay:
        return L"HUD over the game (toggle)";
    case ACT_TacOverlayHold:
        return L"HUD over the game (hold)";
    case ACT_EiVision:
        return L"Enhanced Imaging vision mode (toggle)";
    case ACT_Compass:
        return L"Heading compass on this window (toggle)";
    case ACT_LancePage:
        return L"Lance page (V-LNC): the three lancemates";
    default:
        return L"(unassigned)";
    }
}

// Human-readable binding, e.g. "Ctrl+Num1". Unbound reads "-".
static std::wstring KeyName(UINT vk, UINT mods)
{
    if (vk == 0)
        return L"-";

    std::wstring s;
    if (mods & MOD_CONTROL)
        s += L"Ctrl+";
    if (mods & MOD_ALT)
        s += L"Alt+";
    if (mods & MOD_SHIFT)
        s += L"Shift+";

    wchar_t buf[24];
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9)
        swprintf_s(buf, L"Num%u", vk - VK_NUMPAD0);
    else if (vk >= VK_F1 && vk <= VK_F24)
        swprintf_s(buf, L"F%u", vk - VK_F1 + 1);
    else if (vk == VK_DECIMAL)
        swprintf_s(buf, L"Num.");
    else if (vk == VK_DIVIDE)
        swprintf_s(buf, L"Num/");
    else if (vk == VK_MULTIPLY)
        swprintf_s(buf, L"Num*");
    else if (vk == VK_SUBTRACT)
        swprintf_s(buf, L"Num-");
    else if (vk == VK_ADD)
        swprintf_s(buf, L"Num+");
    else if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z'))
        swprintf_s(buf, L"%c", (wchar_t)vk);
    else
        swprintf_s(buf, L"VK%02X", vk);

    return s + buf;
}

// UI colours (0xRRGGBB). The bezel is DDI phosphor green: legend text only, no boxes
// and no fills, so it reads as painted-on cockpit lettering rather than a toolbar.
namespace UiCol
{
constexpr UINT32 LabelText = 0x2FD46B;   // unlit legend
constexpr UINT32 ActiveText = 0x9CFFC0;  // the view currently on screen
constexpr UINT32 EmptyEdge = 0x1E6B3C;   // outline of an unassigned button
constexpr UINT32 Panel = 0x0A140E;
constexpr UINT32 PanelEdge = 0x1E6B3C;
constexpr UINT32 PanelText = 0x9CFFC0;
constexpr UINT32 PanelDim = 0x3F7A55;
constexpr UINT32 CheckOn = 0x2FD46B;
constexpr UINT32 Warn = 0xFFB020;
constexpr UINT32 Alarm = 0xFF2A1A;      // missile warning - the only red on the panel
} // namespace UiCol

// Every page is inset so the bezel legends have clear
// space around the edge of the window. 10% per side = 80% of the client area, which is
// what a 64 px legend box needs to clear the content in the left/right columns.
static constexpr float kContentInset = 0.10f;

// Legend box, in pixels. Fixed size so legends stay readable as the window scales.
static constexpr float kLabelBoxW = 64.0f;
static constexpr float kLabelBoxH = 26.0f;
static constexpr float kLabelFontSize = 18.0f;

// Bumped whenever the default bezel layout changes shape. A config written by an older
// version keeps its window geometry and LIDAR settings but has its labels re-seeded,
// rather than leaving the user with a stale half-bezel.
static constexpr int kConfigVersion = 5;

// A config older than this had a different bezel *shape*, so its labels must be re-seeded.
// Anything newer only needs its bindings migrated, which preserves whatever positions the
// user has dragged their labels into.
static constexpr int kConfigReseedBelow = 3;

// Reserved: this is the only way into edit mode, so it must never become a bindable
// hotkey. Binding it would make WM_HOTKEY swallow the keystroke before the toggle sees
// it, locking the user out with no route back except hand-editing the config.
static constexpr UINT kEditModeKey = VK_ESCAPE; // was Numpad . (now free to bind); Esc no longer quits

class HUDDisplay
{
  private:
    HWND hwnd = nullptr;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain> swapChain;
    ComPtr<ID3D11Texture2D> sharedTextures[10];
    ComPtr<ID3D11ShaderResourceView> sharedSRVs[10];
    ComPtr<ID3D11Texture2D> cachedTextures[10]; // Local copies to prevent flicker
    ComPtr<ID3D11ShaderResourceView> cachedSRVs[10];
    ComPtr<ID3D11RenderTargetView> backBufferRTV;
    ComPtr<ID3D11VertexShader> vertexShader;
    ComPtr<ID3D11PixelShader> pixelShader;
    ComPtr<ID3D11SamplerState> sampler;

    // Direct2D, sharing this file's swap chain. d2dTarget is the back buffer wrapped as
    // a D2D bitmap; it is destroyed and rebuilt around every resize.
    ComPtr<ID2D1Factory1> d2dFactory;
    ComPtr<ID2D1Device> d2dDevice;
    ComPtr<ID2D1DeviceContext> d2dCtx;
    ComPtr<ID2D1Bitmap1> d2dTarget;
    ComPtr<IDWriteFactory> dwrite;
    ComPtr<IDWriteTextFormat> labelFmt;
    ComPtr<IDWriteTextFormat> panelFmt;
    ComPtr<IDWriteTextFormat> menuFmt, menuFmtC; // the options menu: no wrapping, "..." when too long; centred
    ComPtr<ID2D1SolidColorBrush> uiBrush;
    bool d2dAvailable = false;

    // LIDAR scope (ported copy of LidarDisplay.exe's renderer).
    LidarView lidar;
    bool isLidarMode = false;

    // Topographic map. Same shape as the scope - it owns its own MMF, does its own
    // BeginDraw/EndDraw, and shares this file's D2D stack - so it costs nothing while another
    // view is up beyond a once-a-second reconnect attempt.
    TopoView topo;
    bool isTopoMode = false;

    // The 3D tactical page. Terrain through D3D11 with a depth buffer, contacts through D2D on
    // top; it takes its LIDAR data from LidarView rather than opening a second connection, so
    // the ping reveal, the fades and the control arbitration all stay in one place.
    Topo3DView topo3d;
    bool isTopo3DMode = false;
    bool isLanceMode = false; // V-LNC (2026-10-19): the lance page
    float topoSpanM = 2560.0f; // 5 tiles across, which is what fits a 600 px MFD window legibly
    // Heading-up by default: the LIDAR scope is heading-up, and this page is meant to end up
    // underneath it. Set topo_north_up=1 in the config for a conventional north-up map.
    bool topoNorthUp = false;
    // THE HEADING COMPASS (2026-10-12): this window's CMPS bezel button (kept in the .cfg); what is drawn comes
    // from GlobalSettings.ini [hud] compass_* (gs::CompassOpts, every window).
    bool compassOn = true;
    gs::CompassOpts compassOpts;
    int topoCellM = TopoView::kDefaultCellM;
    // The 3D page carries its own, finer by default: in perspective the terrain's shape comes
    // from its silhouette, and 64 m point-sampling of 16 m data aliases ridges flat and turns
    // lone samples into spikes. See Topo3DView.h.
    int topo3dCellM = Topo3DView::kDefaultCellM;
    double topoNextLogMs = 0.0;

    ExportControlBlock controlBlock = {};
    HANDLE mmfHandle = nullptr;
    HANDLE instanceMutex = nullptr; // detects a second window on the same profile
    void *mmfBuffer = nullptr;
    bool running = true;
    uint32_t currentWidget = 4;       // index into the export table, not a type

    // What the PILOT asked to see, as a widget type. Distinct from currentWidget, which is a
    // table index that only means anything while the exporter is publishing. Selection must
    // not depend on the game: pressing a bezel button always changes the view, and the screen
    // fills in by itself if and when data for it appears.
    uint8_t selectedWidgetType = 4;   // Target Screen, matching the startup default
    // A screen chosen before the game connects. currentWidget is a table INDEX; the two
    // only coincide because HUD.dll happens to place each widget at the index matching its
    // type today, which is not a contract. Resolved properly once the export appears.
    uint8_t pendingWidgetType = 0xFF; // none: the app starts on the game's own Target screen (isVanillaMode)
    HANDLE lastKnownHandles[10] = {}; // Track handles to detect changes

    // ---- the vanilla HUD screens (VanillaHud.dll) ----------------------------------------
    // A second, independent export: the game's own HUD elements rather than MWVR's screens.
    // Its own block, its own textures, its own mode flag - nothing above is shared with it, so
    // the MWVR screens behave exactly as before whether or not the vanilla plugin is loaded.
    bool isVanillaMode = true; // start on the game's own Target screen
    uint8_t vanillaScreen = VSC_Target;
    float podZoom = 1.0f;        // the targeting screen's magnification
    // v21 ENHANCED IMAGING: V-POD's clone. At 1x its depth is the GAME's own (VS_EiDepth, no second
    // scene render); zoomed it takes V-POD's capture (VS_Depth). Which one this frame uses:
    int  vPodSlot = VS_Depth;
    bool IsPilotScreen() const { return vanillaScreen == VSC_Targeting || vanillaScreen == VSC_Enhanced; }
    float camZoom = 1.0f;        // the CAP page's zoom camera magnification
    bool  zoomCamIR = false;     // ...and whether it shows the IR image
    double vDiagNextMs = 0.0;
    HANDLE vMmfHandle = nullptr;
    VanillaExportBlock *vBlock = nullptr;
    ULONGLONG vLastAttempt = 0;
    ComPtr<ID3D11Texture2D> vShared[VHUD_MAX_SLOTS];
    ComPtr<ID3D11ShaderResourceView> vSharedSRV[VHUD_MAX_SLOTS];
    ComPtr<ID3D11Texture2D> vCached[VHUD_MAX_SLOTS];
    ComPtr<ID3D11ShaderResourceView> vCachedSRV[VHUD_MAX_SLOTS];
    uint64_t vLastHandle[VHUD_MAX_SLOTS] = {};
    // Per source: the part of the image that was actually drawn (alpha > 0), refreshed about once a
    // second from a small CPU readback. The plugin gives every element a canvas much larger than
    // its box so nothing it paints outside that box is cut off; this crops the result back down.
    struct VTrim
    {
        uint32_t X0 = 0, Y0 = 0, X1 = 0, Y1 = 0, W = 0, H = 0;
        double NextMs = 0.0;
        bool   Empty  = true; // nothing drawn at the last look (the dialogue shows only while it draws)
        // Gauges: the last kTrimHist one-second boxes; the trim is their union (see UpdateTrim).
        uint32_t Hist[10][4] = {};
        int      HistN = 0, HistPos = 0;
        // trimasync1: a copy issued to the staging texture, read back on a later frame.
        bool     Pending = false;
        uint32_t PendW = 0, PendH = 0;
        double   PendMs = 0.0;
    } vTrim[VHUD_MAX_SLOTS];
    ComPtr<ID3D11Texture2D> vStage[VHUD_MAX_SLOTS];
    ComPtr<ID3D11PixelShader> psXform;  // samples a sub-rectangle of the texture
    ComPtr<ID3D11PixelShader> psDepthEdge; // the Targeting screen's mesh outlines, from scene depth
    ComPtr<ID3D11PixelShader> psZoomIR;    // the zoom camera's IR image
    ComPtr<ID3D11Buffer> xformCB;
    ComPtr<ID3D11Buffer> unitsCB; // the outline pass's unit boxes (b1)
    static constexpr int kMaxUnitBoxes = 32;
    // The depth image this frame and the camera pose it was rendered from, taken together under
    // the texture's keyed mutex (VanillaExportBlock::DepthCam).
    VanillaExportBlock::Camera vDepthCam{};
    VanillaExportBlock::Lock   vDepthLock{};
    uint32_t                   vDepthBeamCount = 0;
    VanillaExportBlock::Beam   vDepthBeams[VanillaExportBlock::kMaxBeams] = {};
    ID3D11ShaderResourceView  *vDepthSrv = nullptr;
    uint32_t                   vDepthW = 0, vDepthH = 0;
    ComPtr<ID3D11BlendState> alphaBlend;
    ULONGLONG lastMmfAttempt = 0;     // background reconnect throttle

    // Which instance this is. Several HUDDisplay windows run at once, one per screen, and
    // the profile name is what gives each its own config file and window title. It used to
    // be a ctrl/alt/shift word that ALSO forced a modifier onto every hotkey; bindings
    // carry their own modifiers now, so the name is purely an identity.
    std::string profileName = "default";

    // Window position/size config
    struct WindowConfig
    {
        int x = 100;
        int y = 100;
        int width = 777;
        int height = 809;
    } windowConfig;

    // Bezel labels + edit mode.
    std::vector<LabelSlot> labels;
    bool editMode = false;
    int selectedSlot = -1;
    bool capturingKey = false;
    bool editingText = false;
    // 2026-10-20 (user): SAVE AS - typing a new profile's name (editingText is on too, so the keyboard is ours).
    bool namingProfile = false;
    std::wstring savedAsName;   // the last profile saved as - OPEN IT starts it
    ULONGLONG savedAsMs = 0;
    std::wstring textBuffer;
    // Tracks what we've actually told Windows, so registration can be made idempotent
    // instead of relying on every Begin/End pair to balance by hand.
    bool hotkeysRegistered = false;
    int dragSlot = -1;
    float dragOffsetX = 0.0f;
    float dragOffsetY = 0.0f;
    bool dragMoved = false;
    std::wstring editStatus;

    // LIDAR settings, owned here and pushed into the view. Ported out of the scope's own
    // options menu, which the integrated copy no longer has.
    uint32_t lidarLayers = 0;
    uint32_t lidarRangeM = 1000;
    // Interpolate projectile positions between the plugin's ~30 Hz publishes. On by
    // default; it is the difference between rounds hopping ten pixels at a time and
    // travelling. Toggleable because it is a per-round cost on the render thread.
    bool lidarProjSmooth = true;
    // Incoming-missile warning volume, 0-100. The alarm ON/OFF is a LIDAR layer bit, not a
    // separate flag, so it shows up in the same checkbox list as everything else it
    // depends on.
    int lidarAlarmVol = 70;

    bool presentOccluded = false;
    double lastTargetLossLog = 0.0;
    // One-second rolling stats. A single sampled slow frame could not distinguish
    // "60 fps with one hitch a second" from "a steady 10 fps" - and those want completely
    // different fixes, so the log has to say which it is.
    int statFrames = 0;
    int statSlow = 0;
    double statWorst = 0.0;
    double statWorstView = 0.0;
    double statWorstUi = 0.0;
    double statWorstPresent = 0.0;
    double statWindowStart = 0.0;
    double lastVramLog = 0.0;
    // No vsync: with MW5 in Independent Flip, Present(1, 0) on this window was measured blocking
    // 48-88 ms while our own drawing cost 0 ms. The loop paces itself (targetFrameMs) instead.
    double targetFrameMs = 1000.0 / 60.0; // replaced by the panel's real rate at startup
    bool timerPeriodSet = false;

    // The legacy DISCARD swap chain. The flip model was tried and reverted: it did not reduce the
    // Present stalls (a small windowed chain is composited by DWM regardless) and it broke the
    // Direct2D target, whose bitmap binds to one surface that flip rotates away. It was removed
    // with the new menu, along with its config switch.
    static constexpr UINT swapChainFlags = 0;
    // Per-frame phase timings, so a slow frame reports WHERE the time went rather than
    // just that it happened. Measured inside Render(); the watchdog's own logging sits
    // outside all three and so can never be counted as a stall it caused.
    double phaseView = 0.0;    // drawing the LIDAR scope / widget texture / capture
    double phaseUi = 0.0;      // the Direct2D bezel + edit panel pass
    double phasePresent = 0.0; // Present() - i.e. waiting on the GPU or the compositor

    // High-resolution clock. GetTickCount64 only moves every ~15.6 ms, which is far too
    // coarse to measure or pace a 16.7 ms frame.
    static double NowMs()
    {
        static const double freq = [] {
            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            return (double)f.QuadPart / 1000.0;
        }();
        LARGE_INTEGER c;
        QueryPerformanceCounter(&c);
        return (double)c.QuadPart / freq;
    }

    // The CAP page: the plugin's zoom camera. (The desktop-region copy it used to fall back
    // on - Desktop Duplication, GDI and the region picker - was removed with the new menu.)
    bool isScreenCaptureMode = false;

    // "Reset bezel" is destructive, so it arms on the first click and fires on the second.
    bool resetArmed = false;
    ULONGLONG resetArmedAt = 0;

    // Helper function to find widget index by type
    int FindWidgetByType(uint8_t targetType)
    {
        if (!mmfBuffer)
            return -1;

        ExportControlBlock *control = static_cast<ExportControlBlock *>(mmfBuffer);

        for (int i = 0; i < 10; i++)
        {
            if (control->Widgets[i].Active && control->Widgets[i].Type == targetType)
            {
                return i;
            }
        }

        return -1; // Not found
    }

    // HUDDisplay is a WIN32_EXECUTABLE, so it has no console and every line written to
    // std::cout was discarded. That left the second screen a black box in exactly the
    // situations worth diagnosing - a crash, a stuck alarm, a control-ownership fight
    // between two instances - and cost several missions of guesswork. Mirror to a file
    // beside the exe, named per profile so two instances never interleave.
    void Log(const std::string &msg)
    {
        std::cout << msg << std::endl;

        static std::ofstream logFile(ExeDir() + "HUDDisplay_" + profileName + ".log",
                                     std::ios::trunc); // a fresh log each launch
        if (!logFile)
            return;
        SYSTEMTIME t;
        GetLocalTime(&t);
        char stamp[32];
        sprintf_s(stamp, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
        logFile << stamp << msg << std::endl;
    }

    // Once a second, and only when something changed, record what the alarm subsystem is
    // actually doing. "proj" is the bit that matters most: without it there is no array,
    // and without the array there is no per-round binding no matter what else is right.
    void LogAlarmDiag()
    {
        const LidarView::Diag d = lidar.GetDiag();
        char line[256];
        sprintf_s(line,
                  "alarm: conn=%d own=%d scope=%d proj=%d locked=%d bound=%d any=%d snd=%d",
                  d.Connected, d.OwnsControl, d.HoldsScope, d.ProjRequested, d.Locked,
                  d.BoundRounds, d.BoundAny, d.Sounding);
        static std::string last;
        if (line == last)
            return;
        last = line;
        Log(line);
    }

    std::string ExeDir()
    {
        char path[MAX_PATH];
        GetModuleFileNameA(nullptr, path, MAX_PATH);
        std::string exePath(path);
        size_t lastSlash = exePath.find_last_of("\\/");
        return exePath.substr(0, lastSlash + 1);
    }

    // One unified config per instance, beside the exe, named for the profile.
    std::string GetConfigPath()
    {
        return ExeDir() + "MFDDisplay_" + profileName + ".cfg";
    }

    // Configs written before profiles existed were named for the ctrl/alt/shift word. The
    // default profile adopts the old no-argument file so an existing setup survives the
    // change without being renamed by hand.
    std::string GetPreProfileConfigPath()
    {
        return ExeDir() + "MFDDisplay_None.cfg";
    }

    // Older still: the window-geometry-only .ini from before the unified config.
    std::string GetLegacyConfigPath()
    {
        return ExeDir() + "HUDDisplay_" + (profileName == "default" ? "None" : profileName) + ".ini";
    }

    // ---- Bezel label defaults ---------------------------------------------
    // Seeded to a 5-per-side DDI bezel: five along the bottom edge, five along the top.
    // Everything is draggable, so this is only a starting point - drag them onto the
    // real button positions once and the layout persists.
    void SeedDefaultLabels()
    {
        labels.clear();

        auto add = [&](ViewAction a, uint8_t param, float nx, float ny, UINT key) {
            LabelSlot s;
            s.Action = a;
            s.Param = param;
            s.Text = ActionAbbrev(a, param);
            s.NX = nx;
            s.NY = ny;
            s.Key = key;
            s.Mods = 0; // bare keys; add modifiers per label in edit mode if you need them
            labels.push_back(s);
        };

        // Twenty buttons, five to a side, matching an F/A-18 DDI bezel. Unassigned ones
        // are real slots too - they draw as an empty outline and can be given an action
        // and a key in edit mode. Drag any of them onto the real hardware positions.
        const float bottomY = 0.950f, topY = 0.050f;
        const float leftX = 0.050f, rightX = 0.950f;
        const float hCols[5] = {0.18f, 0.34f, 0.50f, 0.66f, 0.82f};
        const float vRows[5] = {0.22f, 0.36f, 0.50f, 0.64f, 0.78f};

        // Bottom edge: the five main screens.
        // The game's own HUD screens (VanillaHud). The MWVR screens stay in the menu.
        add(ACT_VanillaWidget, VSC_Target, hCols[0], bottomY, VK_NUMPAD1);
        add(ACT_VanillaWidget, VSC_Weapons, hCols[1], bottomY, VK_NUMPAD2);
        add(ACT_VanillaWidget, VSC_Owner, hCols[2], bottomY, VK_NUMPAD3);
        add(ACT_VanillaWidget, VSC_Objectives, hCols[3], bottomY, VK_NUMPAD4);
        add(ACT_VanillaWidget, VSC_Map, hCols[4], bottomY, VK_NUMPAD5);

        // Top edge: comms, the LIDAR scope, the zoom camera, and the two zoom steps.
        add(ACT_VanillaWidget, VSC_Comms, hCols[0], topY, VK_NUMPAD6);
        add(ACT_Lidar, 0, hCols[1], topY, VK_NUMPAD0);
        add(ACT_ScreenCapture, 0, hCols[2], topY, VK_NUMPAD9);
        add(ACT_LidarZoomIn, 0, hCols[3], topY, VK_ADD);      // numpad +
        add(ACT_LidarZoomOut, 0, hCols[4], topY, VK_SUBTRACT); // numpad -

        // Left edge: the topographic map, then spares. Numpad 7 and 8 are the only numpad
        // keys this layout leaves free, so TOPO takes 7.
        add(ACT_Topo, 0, leftX, vRows[0], VK_NUMPAD7);
        add(ACT_Topo3D, 0, leftX, vRows[1], VK_NUMPAD8);
        add(ACT_CamRecentre, 0, leftX, vRows[2], VK_MULTIPLY); // numpad *
        add(ACT_None, 0, leftX, vRows[3], 0);
        add(ACT_LancePage, 0, leftX, vRows[4], 0); // V-LNC (2026-10-19)
        for (int i = 0; i < 5; i++)
            add(ACT_None, 0, rightX, vRows[i], 0);
    }

    void LoadConfig()
    {
        lidarLayers = LidarView::DefaultLayers();

        // Own config first. Failing that, the default profile adopts the pre-profile
        // MFDDisplay_None.cfg, so an existing single-window setup carries over untouched.
        // A named profile with no config starts from defaults.
        std::string path = GetConfigPath();
        if (!std::ifstream(path).is_open() && profileName == "default")
        {
            const std::string old = GetPreProfileConfigPath();
            if (std::ifstream(old).is_open())
            {
                Log("Adopting pre-profile config " + old + " for the default profile");
                path = old;
            }
        }

        std::ifstream file(path);
        if (!file.is_open())
        {
            Log("No config found - seeding defaults");
            SeedDefaultLabels();
            LoadLegacyWindowConfig();
            return;
        }

        labels.clear();
        int fileVersion = 1;

        std::string line;
        while (std::getline(file, line))
        {
            if (line.empty() || line[0] == '#')
                continue;

            size_t eq = line.find('=');
            if (eq == std::string::npos)
                continue;

            std::string key = line.substr(0, eq);
            std::string value = line.substr(eq + 1);

            try
            {
                if (key == "version")
                    fileVersion = std::stoi(value);
                else if (key == "x")
                    windowConfig.x = std::stoi(value);
                else if (key == "y")
                    windowConfig.y = std::stoi(value);
                else if (key == "width")
                    windowConfig.width = std::stoi(value);
                else if (key == "height")
                    windowConfig.height = std::stoi(value);
                else if (key == "lidar_layers")
                    lidarLayers = (uint32_t)std::stoul(value);
                else if (key == "lidar_projms")
                    ; // retired: the projectile rate is fixed at 30 Hz. Parsed and ignored
                      // so an older config file loads cleanly instead of tripping on it.
                else if (key == "lidar_range")
                    lidarRangeM = (uint32_t)std::stoul(value);
                else if (key == "lidar_projsmooth")
                    lidarProjSmooth = std::stoi(value) != 0;
                else if (key == "lidar_alarmvol")
                    lidarAlarmVol = std::stoi(value);
                else if (key == "topo_span_m")
                    topoSpanM = std::stof(value);
                else if (key == "topo_north_up")
                    topoNorthUp = std::stoi(value) != 0;
                else if (key == "compass_on")
                    compassOn = std::stoi(value) != 0;
                else if (key == "topo_cell_m")
                    topoCellM = std::stoi(value);
                else if (key == "topo3d_cell_m")
                    topo3dCellM = std::stoi(value);
                else if (key == "borderless")
                    borderless = std::stoi(value) != 0;
                else if (key == "insane_allies")
                    allyMode = (std::max)(0, (std::min)(2, std::stoi(value)));
                else if (key == "mute_game_voices")
                    muteGameVoices = std::stoi(value) != 0;
                else if (key == "borderless_key")
                {
                    const int k = std::stoi(value);
                    if (k > 0 && k < 256 && (UINT)k != kEditModeKey)
                        borderlessKey = (UINT)k;
                }
                // Checked: a bad value (NaN from a damaged file) would otherwise never compare equal
                // and keep the options menu "unsaved" for ever. -10 = centred; else a fraction.
                else if (key == "ov_x" || key == "ov_y")
                {
                    float v = std::stof(value);
                    if (!std::isfinite(v) || (v != -10.0f && (v < -1.0f || v > 1.0f)))
                        v = -10.0f;
                    (key == "ov_x" ? ovPosX : ovPosY) = v;
                }
                else if (key == "ov_scale")
                {
                    const float v = std::stof(value);
                    ovScale = std::isfinite(v) ? (std::min)((std::max)(v, 0.2f), 1.0f) : 0.9f;
                }
                else if (key == "ov_opacity")
                {
                    const float v = std::stof(value);
                    ovOpacity = std::isfinite(v) ? (std::min)((std::max)(v, 0.3f), 1.0f) : 0.7f;
                }
                else if (key.rfind("label", 0) == 0)
                    ParseLabelLine(value);
            }
            catch (...)
            {
                Log("Skipping malformed config line: " + line);
            }
        }

        if (fileVersion < kConfigReseedBelow)
        {
            Log("Config predates the current bezel layout - re-seeding labels (window and "
                "LIDAR settings kept)");
            SeedDefaultLabels();
        }
        else if (labels.empty())
        {
            Log("Config had no labels - seeding defaults");
            SeedDefaultLabels();
        }
        else if (fileVersion < kConfigVersion)
        {
            MigrateBindings(fileVersion);
        }

        // The missile alarm arrived in v5 as a new, previously unused layer bit, so every
        // older config has it clear and the feature would silently never fire. Turn it on
        // once, on upgrade, rather than leaving the user to discover a checkbox they have
        // no reason to suspect exists.
        if (fileVersion < 5)
        {
            lidarLayers |= LidarView::AlarmLayerMask();
            Log("Config predates the missile alarm - enabling it (GlobalSettings > HUD > ALARM turns it off)");
        }

        // 2026-10-07f: the lance switches moved into GlobalSettings.ini - the first profile read brings its own over.
        gs::PublishPath(); // 2026-10-07h: tells LanceControl (in the game) where GlobalSettings.ini is
        if (!gs::Has(L"lance", L"insane_allies"))
            gs::SetInt(L"lance", L"insane_allies", allyMode);
        if (!gs::Has(L"lance", L"mute_game_voices"))
            gs::SetInt(L"lance", L"mute_game_voices", muteGameVoices ? 1 : 0);
        // 2026-10-13: the missile alarm moved into GlobalSettings.ini - the first profile read brings its own over.
        if (!gs::Has(L"hud", L"alarm_on"))
            gs::SetInt(L"hud", L"alarm_on", (lidarLayers & LidarView::AlarmLayerMask()) ? 1 : 0);
        if (!gs::Has(L"hud", L"alarm_vol"))
            gs::SetInt(L"hud", L"alarm_vol", (lidarAlarmVol >= 0 && lidarAlarmVol <= 100) ? lidarAlarmVol : 70);
        // 2026-10-13: the LIDAR layers and smoothing moved there too - the first profile read brings its own over
        for (int i = 0; i < 11; ++i)
            if (gs::kLidarLayerKeys[i] && !gs::Has(L"hud", gs::kLidarLayerKeys[i]))
                gs::SetInt(L"hud", gs::kLidarLayerKeys[i], (lidarLayers >> i) & 1u);
        if (!gs::Has(L"hud", L"lidar_smooth"))
            gs::SetInt(L"hud", L"lidar_smooth", lidarProjSmooth ? 1 : 0);
        if (lidarAlarmVol < 0 || lidarAlarmVol > 100)
            lidarAlarmVol = 70;
        if (lidarRangeM < LidarView::kMinRangeM || lidarRangeM > LidarView::kMaxRangeFarM) // 5 km mode (2026-10-15)
            lidarRangeM = 1000;
        // 0 is the FIT sentinel and is legal; anything else outside the range is not. The >=
        // test also catches NaN, which would otherwise sail through a naive range check.
        if (topoSpanM != TopoView::kFitSpanSentinel &&
            (!(topoSpanM >= TopoView::kMinSpanM) || topoSpanM > TopoView::kMaxSpanM))
            topoSpanM = 2560.0f;

        Log("Loaded config: " + std::to_string(labels.size()) + " labels, window " +
            std::to_string(windowConfig.x) + "," + std::to_string(windowConfig.y) + " " +
            std::to_string(windowConfig.width) + "x" + std::to_string(windowConfig.height));
    }

    // Move bindings that changed between config versions, leaving positions, legends and
    // every other binding exactly as the user left them.
    void MigrateBindings(int fileVersion)
    {
        if (fileVersion < 4)
        {
            // v4: LIDAR moved back to Numpad 0, and zoom moved onto the numpad +/- keys.
            for (LabelSlot &s : labels)
            {
                if (s.Action == ACT_Lidar)
                    s.Key = VK_NUMPAD0;
                else if (s.Action == ACT_LidarZoomIn)
                    s.Key = VK_ADD;
                else if (s.Action == ACT_LidarZoomOut)
                    s.Key = VK_SUBTRACT;
            }
            Log("Migrated LIDAR and zoom bindings to config version 4 (positions kept)");
        }
    }

    // labelN=text|nx|ny|action|param|key|mods|customtext
    void ParseLabelLine(const std::string &value)
    {
        std::vector<std::string> f;
        std::stringstream ss(value);
        std::string tok;
        while (std::getline(ss, tok, '|'))
            f.push_back(tok);

        if (f.size() < 7)
            return;

        LabelSlot s;
        s.Text = WidenLabel(f[0]);
        s.NX = std::stof(f[1]);
        s.NY = std::stof(f[2]);
        int a = std::stoi(f[3]);
        s.Action = (a > ACT_None && a < ACT_COUNT) ? (ViewAction)a : ACT_None;
        // Retired actions load as unassigned buttons (their numbers stay reserved: configs store
        // actions by number, and removing them would repoint every later binding).
        if (s.Action == ACT_RetiredLabelMode || s.Action == ACT_RetiredSelNext || s.Action == ACT_RetiredSelPrev)
            s.Action = ACT_None;
        s.Param = (uint8_t)std::stoi(f[4]);
        s.Key = (UINT)std::stoul(f[5]);
        s.Mods = (UINT)std::stoul(f[6]);
        s.CustomText = (f.size() >= 8) && std::stoi(f[7]) != 0;

        // Clamp so a corrupt file can't strand a label off-screen.
        if (s.NX < 0.0f || s.NX > 1.0f)
            s.NX = 0.5f;
        if (s.NY < 0.0f || s.NY > 1.0f)
            s.NY = 0.5f;

        // A legend typed in edit mode is kept verbatim. Otherwise the abbreviation table
        // is authoritative, so editing it updates existing configs too.
        if (!s.CustomText)
            s.Text = ActionAbbrev(s.Action, s.Param);

        labels.push_back(s);
    }

    // One-time migration: keep the window where the user last put it.
    void LoadLegacyWindowConfig()
    {
        std::ifstream file(GetLegacyConfigPath());
        if (!file.is_open())
            return;

        std::string line;
        while (std::getline(file, line))
        {
            size_t eq = line.find('=');
            if (line.empty() || line[0] == '#' || eq == std::string::npos)
                continue;

            std::string key = line.substr(0, eq), value = line.substr(eq + 1);
            try
            {
                // The legacy .ini only ever held window geometry - no version, no labels.
                if (key == "x")
                    windowConfig.x = std::stoi(value);
                else if (key == "y")
                    windowConfig.y = std::stoi(value);
                else if (key == "width")
                    windowConfig.width = std::stoi(value);
                else if (key == "height")
                    windowConfig.height = std::stoi(value);
            }
            catch (...)
            {
            }
        }
        Log("Migrated window geometry from " + GetLegacyConfigPath());
    }

    // Everything that changes a setting calls this. While the options menu is open it does
    // nothing: changes wait for the menu's SAVE (or are reverted / discarded), which calls
    // WriteConfig directly.
    void SaveConfig()
    {
        if (editMode)
            return;
        WriteConfig();
    }

    void WriteConfig()
    {
        if (hwnd && IsWindow(hwnd))
        {
            RECT rect;
            if (GetWindowRect(hwnd, &rect))
            {
                // Validate values are reasonable (not garbage)
                const int w = rect.right - rect.left, h = rect.bottom - rect.top;
                if (rect.left > -10000 && rect.left < 10000 && rect.top > -10000 && rect.top < 10000 && w >= 100 &&
                    w <= 5000 && h >= 100 && h <= 5000)
                {
                    windowConfig.x = rect.left;
                    windowConfig.y = rect.top;
                    windowConfig.width = w;
                    windowConfig.height = h;
                }
                else
                {
                    Log("Window rect has invalid values, keeping previous");
                }
            }
        }

        std::ofstream file(GetConfigPath());
        if (!file.is_open())
        {
            Log("Failed to save config");
            return;
        }

        file << "# MW5 MFD Display configuration (profile: " << profileName << ")\n";
        file << "version=" << kConfigVersion << "\n";
        file << "x=" << windowConfig.x << "\n";
        file << "y=" << windowConfig.y << "\n";
        file << "width=" << windowConfig.width << "\n";
        file << "height=" << windowConfig.height << "\n";
        file << "lidar_layers=" << lidarLayers << "\n";
        file << "lidar_range=" << lidarRangeM << "\n";
        file << "topo_span_m=" << topoSpanM << "\n";
        file << "topo_north_up=" << (topoNorthUp ? 1 : 0) << "\n";
        file << "compass_on=" << (compassOn ? 1 : 0) << "\n";
        file << "topo_cell_m=" << topoCellM << "\n";
        file << "topo3d_cell_m=" << topo3dCellM << "\n";
        file << "lidar_projsmooth=" << (lidarProjSmooth ? 1 : 0) << "\n";
        file << "lidar_alarmvol=" << lidarAlarmVol << "\n";
        file << "borderless=" << (borderless ? 1 : 0) << "\n";
        file << "borderless_key=" << borderlessKey << "\n";
        file << "ov_x=" << ovPosX << "\n";
        file << "ov_y=" << ovPosY << "\n";
        file << "ov_scale=" << ovScale << "\n";
        file << "ov_opacity=" << ovOpacity << "\n";
        file << "# labelN=text|nx|ny|action|param|key|mods|customtext\n";

        for (size_t i = 0; i < labels.size(); i++)
        {
            const LabelSlot &s = labels[i];
            const std::string text = NarrowLabel(s.Text);
            file << "label" << i << "=" << text << "|" << s.NX << "|" << s.NY << "|" << (int)s.Action << "|"
                 << (int)s.Param << "|" << s.Key << "|" << s.Mods << "|" << (s.CustomText ? 1 : 0) << "\n";
        }
    }

    // ---- Hotkeys ----------------------------------------------------------
    // One RegisterHotKey per bound label, id = label index + 1. Re-registered whenever a
    // binding changes; failures are surfaced in the edit panel rather than swallowed,
    // since the usual cause is another application already owning the combination.
    // Single source of truth: hotkeys are live exactly when no transient mode needs the
    // raw keyboard. Every state change calls this instead of pairing its own
    // Register/Unregister, which is what previously let them be left down (or registered
    // twice, whose failures were then reported as another application's fault).
    void SyncHotkeys()
    {
        const bool want = !capturingKey && !editingText;
        if (want == hotkeysRegistered)
            return;

        if (want)
            RegisterAllHotkeys();
        else
            UnregisterAllHotkeys();

        hotkeysRegistered = want;
    }

    void ReleaseHotkeysNow()
    {
        if (hotkeysRegistered)
        {
            UnregisterAllHotkeys();
            hotkeysRegistered = false;
        }
    }

    // Bindings changed while hotkeys are live: drop them and take them again so a freed
    // combination is genuinely released back to the system.
    void ReapplyHotkeys()
    {
        if (hotkeysRegistered)
        {
            UnregisterAllHotkeys();
            hotkeysRegistered = false;
        }
        SyncHotkeys();
    }

    void RegisterAllHotkeys()
    {
        if (!hwnd)
            return;

        std::wstring failed;
        for (size_t i = 0; i < labels.size(); i++)
        {
            if (labels[i].Key == 0)
                continue;

            if (!RegisterHotKey(hwnd, (int)i + 1, MOD_NOREPEAT | labels[i].Mods, labels[i].Key))
            {
                if (!failed.empty())
                    failed += L", ";
                failed += KeyName(labels[i].Key, labels[i].Mods);
            }
        }

        if (!failed.empty())
        {
            editStatus = L"In use by another app: " + failed;
            Log("RegisterHotKey failed for: " + NarrowLabel(failed));
        }
        else
        {
            editStatus.clear(); // otherwise a stale warning outlives its cause
        }
    }

    void UnregisterAllHotkeys()
    {
        if (!hwnd)
            return;
        for (size_t i = 0; i < labels.size(); i++)
            UnregisterHotKey(hwnd, (int)i + 1);
    }

    bool InitWindow()
    {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(WNDCLASSEX);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = WindowProc;
        wc.hInstance = GetModuleHandle(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = L"HUDDisplayClass";

        if (!RegisterClassExW(&wc))
        {
            Log("Failed to register window class");
            return false;
        }

        // Title carries the profile so several windows are tellable apart on the taskbar.
        std::wstring title = L"MW5 MFD Display (" + WidenLabel(profileName) + L")";

        // Use saved position/size from config
        hwnd = CreateWindowExW(WS_EX_TOPMOST, L"HUDDisplayClass", title.c_str(), WindowStyleFor(borderless), windowConfig.x,
                               windowConfig.y, windowConfig.width, windowConfig.height, nullptr, nullptr,
                               GetModuleHandle(nullptr), this);

        if (!hwnd)
        {
            Log("Failed to create window");
            return false;
        }

        SyncHotkeys();
        Log("Registered global hotkeys from config (" + std::to_string(labels.size()) + " labels)");

        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);

        Log("Window created successfully");
        return true;
    }

    bool InitD3D()
    {
        auto attempt = [&](UINT deviceFlags) -> HRESULT {
            swapChain.Reset();
            device.Reset();
            context.Reset();

            DXGI_SWAP_CHAIN_DESC scd = {};
            scd.BufferCount = 1;
            scd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            scd.OutputWindow = hwnd;
            scd.SampleDesc.Count = 1;
            scd.Windowed = TRUE;
            scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
            scd.Flags = swapChainFlags;

            D3D_FEATURE_LEVEL featureLevel;
            return D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, deviceFlags, nullptr, 0,
                                                 D3D11_SDK_VERSION, &scd, &swapChain, &device, &featureLevel,
                                                 &context);
        };

        // BGRA support is what lets Direct2D share this swap chain. Every driver of the
        // last decade has it; the final fallback exists so ancient hardware still gets the
        // widget views, just without LIDAR or bezel labels.
        HRESULT hr = E_FAIL;
        hr = attempt(D3D11_CREATE_DEVICE_BGRA_SUPPORT);
        if (FAILED(hr))
        {
            Log("BGRA device creation failed - retrying without Direct2D support");
            hr = attempt(0);
        }

        if (FAILED(hr))
        {
            Log("Failed to create D3D11 device");
            return false;
        }

        ComPtr<ID3D11Texture2D> backBuffer;
        swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
        device->CreateRenderTargetView(backBuffer.Get(), nullptr, &backBufferRTV);

        // A frame-latency clamp of 1 was tried here to shorten stalls. Against a starved
        // compositor it does the opposite - it makes Present wait for the previous frame
        // to retire before accepting the next - so the queue is left at the default depth.

        Log("D3D11 initialized (legacy chain, no vsync, 1 buffer)");

        ReportAdapterPlacement();
        AdoptMonitorRefreshRate();
        LogVideoMemory("at startup");
        return true;
    }

    // VRAM telemetry. Two separate MW5 out-of-memory crashes have followed an increase in
    // this process's graphics memory use (a third swap-chain buffer, then a second
    // instance), on an 8 GB card already running UEVR stereo. Budget is what the OS is
    // currently willing to give THIS process - it shrinks as the system comes under
    // pressure, so watching it fall is the signal.
    void LogVideoMemory(const char *when)
    {
        ComPtr<IDXGIDevice> dxgiDevice;
        ComPtr<IDXGIAdapter> adapter;
        if (FAILED(device.As(&dxgiDevice)) || FAILED(dxgiDevice->GetAdapter(&adapter)))
            return;

        ComPtr<IDXGIAdapter3> adapter3;
        if (FAILED(adapter.As(&adapter3)))
            return;

        DXGI_QUERY_VIDEO_MEMORY_INFO info = {};
        if (FAILED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
            return;

        const auto mb = [](UINT64 bytes) { return std::to_string(bytes / (1024ull * 1024ull)); };
        Log(std::string("VRAM ") + when + ": this process " + mb(info.CurrentUsage) + " MB, budget " +
            mb(info.Budget) + " MB" + (info.CurrentUsage > info.Budget ? "  *** OVER BUDGET ***" : ""));
    }

    // Pace to the refresh rate of the display this window is on. A hardcoded 60 is simply
    // wrong on a panel running anything else, and produces a slow beat against it.
    void AdoptMonitorRefreshRate()
    {
        MONITORINFOEXW mi = {};
        mi.cbSize = sizeof(mi);
        const HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        if (!mon || !GetMonitorInfoW(mon, &mi))
            return;

        DEVMODEW dm = {};
        dm.dmSize = sizeof(dm);
        if (!EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm))
            return;

        // 0 and 1 are the documented "default/unspecified" values, not real rates.
        if (dm.dmDisplayFrequency <= 1)
            return;

        targetFrameMs = 1000.0 / (double)dm.dmDisplayFrequency;
        Log("Pacing to " + std::to_string(dm.dmDisplayFrequency) + " Hz (" +
            std::to_string((int)(targetFrameMs * 100) / 100.0).substr(0, 5) + " ms/frame)");
    }

    // Diagnostic for the case that would explain trivial GPU work with huge Present times:
    // the window living on a monitor driven by a DIFFERENT adapter than our device. Every
    // present then needs a cross-adapter copy that DWM mediates, which no swap chain flag
    // can avoid. The only fix is to move the monitor's cable to the other GPU.
    void ReportAdapterPlacement()
    {
        ComPtr<IDXGIDevice> dxgiDevice;
        ComPtr<IDXGIAdapter> adapter;
        if (FAILED(device.As(&dxgiDevice)) || FAILED(dxgiDevice->GetAdapter(&adapter)))
            return;

        DXGI_ADAPTER_DESC ad = {};
        if (SUCCEEDED(adapter->GetDesc(&ad)))
        {
            std::wstring name(ad.Description);
            Log("Render adapter: " + NarrowLabel(name));
        }

        const HMONITOR windowMonitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        bool onSameAdapter = false;

        for (UINT i = 0;; i++)
        {
            ComPtr<IDXGIOutput> output;
            if (adapter->EnumOutputs(i, &output) == DXGI_ERROR_NOT_FOUND)
                break;
            if (!output)
                continue;

            DXGI_OUTPUT_DESC od = {};
            if (SUCCEEDED(output->GetDesc(&od)) && od.Monitor == windowMonitor)
            {
                onSameAdapter = true;
                std::wstring dev(od.DeviceName);
                Log("Window is on " + NarrowLabel(dev) + " (same adapter - good)");
                break;
            }
        }

        if (!onSameAdapter)
        {
            Log("WARNING: this window's monitor is NOT driven by the render adapter.");
            Log("         Every Present needs a cross-adapter copy through the compositor,");
            Log("         which is slow and irregular no matter what the swap chain does.");
            Log("         Move this monitor's cable to the same GPU as the game's display.");
        }
    }

    // ---- Direct2D on the same swap chain ----------------------------------
    // D2D 1.1: ID2D1Device built from this file's DXGI device, so the device context
    // draws straight into the D3D11 back buffer. (D2D 1.0's CreateDxgiSurfaceRenderTarget
    // cannot target a D3D11 surface - it needs a D3D10.1 device - so the 1.1 path is the
    // one that works here.) ID2D1DeviceContext derives from ID2D1RenderTarget, so the
    // LIDAR code that was written against an ID2D1HwndRenderTarget needs no changes.
    bool InitD2D()
    {
        D2D1_FACTORY_OPTIONS opts = {};
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &opts,
                                     reinterpret_cast<void **>(d2dFactory.GetAddressOf()))))
        {
            Log("Direct2D factory unavailable - LIDAR view and bezel labels disabled");
            return false;
        }

        ComPtr<IDXGIDevice> dxgiDevice;
        if (FAILED(device.As(&dxgiDevice)))
        {
            Log("No DXGI device (BGRA fallback in effect) - LIDAR view and bezel labels disabled");
            return false;
        }

        if (FAILED(d2dFactory->CreateDevice(dxgiDevice.Get(), &d2dDevice)) ||
            FAILED(d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2dCtx)))
        {
            Log("Failed to create Direct2D device context");
            return false;
        }

        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                       reinterpret_cast<IUnknown **>(dwrite.GetAddressOf()))))
        {
            Log("Failed to create DirectWrite factory");
            return false;
        }

        const wchar_t *family = PickDisplayFont();
        {
            std::wstring msg(family);
            Log("Bezel font: " + NarrowLabel(msg));
        }

        dwrite->CreateTextFormat(family, nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                                 DWRITE_FONT_STRETCH_NORMAL, kLabelFontSize, L"en-us", &labelFmt);
        if (labelFmt)
        {
            labelFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            labelFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }

        dwrite->CreateTextFormat(family, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                 DWRITE_FONT_STRETCH_NORMAL, 14.0f, L"en-us", &panelFmt);
        for (int c = 0; c < 2; ++c)
        {
            ComPtr<IDWriteTextFormat> &f = c ? menuFmtC : menuFmt;
            if (FAILED(dwrite->CreateTextFormat(family, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                                DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"en-us", &f)) ||
                !f)
                continue;
            f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            if (c)
                f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            ComPtr<IDWriteInlineObject> dots;
            DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            if (SUCCEEDED(dwrite->CreateEllipsisTrimmingSign(f.Get(), &dots)))
                f->SetTrimming(&trim, dots.Get());
        }

        if (!CreateD2DTarget())
            return false;

        d2dAvailable = true;

        // The LIDAR scope shares this D2D stack.
        if (lidar.InitResources(d2dFactory.Get(), dwrite.Get()))
        {
            lidar.SetLayers(lidarLayers);
            lidar.SetRangeM(lidarRangeM);
            lidar.SetProjectileSmoothing(lidarProjSmooth);
            ApplyAlarmOpts(); // GlobalSettings decides the alarm (2026-10-13)
            lidar.SetContentInset(kContentInset);
            lidar.SetRenderTarget(d2dCtx.Get());
            Log("LIDAR view initialized");
        }

        // The scope draws the terrain wireframe inside its disc, through the same TopoView the
        // TOPO page uses - one renderer, two callers, so the two cannot drift apart. Wired
        // unconditionally; the scope's "Terrain" layer toggle decides whether it appears, and a
        // TopoView with no producer simply draws nothing.
        lidar.SetTerrainUnderlay(&topo);

        // So does the topographic map.
        if (topo.InitResources(d2dFactory.Get(), dwrite.Get()))
        {
            topo.SetContentInset(kContentInset);
            if (topoSpanM == TopoView::kFitSpanSentinel)
            {
                topo.SetFitMap(true);
            }
            else
            {
                topo.SetSpanM(topoSpanM);
                topoSpanM = topo.GetSpanM(); // whatever it clamped to is the truth, not the ask
            }
            topo.SetNorthUp(topoNorthUp);
            topo.SetCellM(topoCellM);
            topoCellM = topo.GetCellM(); // snapped to a multiple of the data spacing
            topo.SetRenderTarget(d2dCtx.Get());
            Log("TOPO view initialized (span " + std::to_string((int)topoSpanM) + " m, " +
                std::to_string(topoCellM) + " m cells, " + (topoNorthUp ? "north-up" : "heading-up") + ")");
        }
        else
        {
            Log("TOPO view failed to initialize");
        }

        // The 3D page. It needs the D3D11 device as well as the D2D stack, because the terrain
        // mesh is depth-buffered geometry and the contacts standing on it are not.
        if (topo3d.InitResources(device.Get(), context.Get(), d2dFactory.Get(), dwrite.Get()))
        {
            topo3d.SetContentInset(kContentInset);
            topo3d.SetCellM(topo3dCellM);
            topo3d.SetRangeM(lidarRangeM);
            RECT rc3;
            GetClientRect(hwnd, &rc3);
            topo3d.SetTargets(backBufferRTV.Get(), d2dCtx.Get(), (unsigned)(rc3.right - rc3.left),
                              (unsigned)(rc3.bottom - rc3.top));
            Log("3D view initialized (" + std::to_string(lidarRangeM) + " m, " + std::to_string(topo3dCellM) +
                " m cells)");
        }
        else
        {
            Log("3D view unavailable - shader or depth buffer creation failed");
        }

        Log("Direct2D initialized");
        return true;
    }

    // Military MFD symbology is a squarish DIN-style face - no such font ships with
    // Windows, so walk a preference list and take the closest one that is actually
    // installed. Bahnschrift IS DIN 1451 and ships with Windows 10 1709+, which makes it
    // the nearest honest match to an F/A-18 DDI or Apache MPD legend. The rest are
    // fallbacks, ending at Consolas, which is always present.
    const wchar_t *PickDisplayFont()
    {
        static const wchar_t *kCandidates[] = {
            L"Bahnschrift SemiCondensed", L"Bahnschrift", L"OCR A Extended",
            L"Eurostile",                 L"Lucida Console", L"Consolas",
        };

        ComPtr<IDWriteFontCollection> fonts;
        if (dwrite && SUCCEEDED(dwrite->GetSystemFontCollection(&fonts, FALSE)) && fonts)
        {
            for (const wchar_t *name : kCandidates)
            {
                UINT32 index = 0;
                BOOL exists = FALSE;
                if (SUCCEEDED(fonts->FindFamilyName(name, &index, &exists)) && exists)
                    return name;
            }
        }
        return L"Consolas";
    }

    // Wrap the current back buffer as the device context's target.
    bool CreateD2DTarget()
    {
        if (!d2dCtx || !swapChain)
            return false;

        ComPtr<IDXGISurface> surface;
        if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&surface))))
            return false;

        D2D1_BITMAP_PROPERTIES1 props =
            D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));

        if (FAILED(d2dCtx->CreateBitmapFromDxgiSurface(surface.Get(), &props, &d2dTarget)))
        {
            Log("Failed to wrap back buffer for Direct2D");
            return false;
        }

        d2dCtx->SetTarget(d2dTarget.Get());
        uiBrush.Reset();
        d2dCtx->CreateSolidColorBrush(D2D1::ColorF(UiCol::LabelText), &uiBrush);
        return true;
    }

    // ResizeBuffers fails while any reference to a back buffer is outstanding, so every
    // D2D reference has to go before it runs - not just backBufferRTV.
    void ReleaseD2DTarget()
    {
        if (d2dCtx)
        {
            d2dCtx->SetTarget(nullptr);
            // SetTarget alone can leave a queued reference to the surface, which is enough
            // to make ResizeBuffers fail. Flush forces the context to let go.
            d2dCtx->Flush();
        }
        lidar.SetRenderTarget(nullptr);
        topo.SetRenderTarget(nullptr);
        topo3d.SetTargets(nullptr, nullptr, 0, 0);
        uiBrush.Reset();
        d2dTarget.Reset();
    }

    bool OpenMMF()
    {
        // POC BRANCH ONLY. The shipping build reads "MechWarriorVR_HUDExport", published by
        // the export code we patched into HUD.dll. This branch reads the _POC block that
        // ScreenExport.dll publishes from outside the mod's source, so both pipelines can be
        // loaded at once and compared side by side without two writers on one block. When
        // the external route graduates, drop the suffix here and in ScreenExport::kMmfName
        // together - they are one contract and must change in the same commit.
        mmfHandle = OpenFileMapping(FILE_MAP_ALL_ACCESS, FALSE, TEXT("MechWarriorVR_HUDExport_POC"));
        if (!mmfHandle)
            return false;

        mmfBuffer = MapViewOfFile(mmfHandle, FILE_MAP_ALL_ACCESS, 0, 0, 0);
        if (!mmfBuffer)
        {
            Log("Failed to map view of file");
            CloseHandle(mmfHandle);
            mmfHandle = nullptr;
            return false;
        }

        memcpy(&controlBlock, mmfBuffer, sizeof(ExportControlBlock));

        ExportControlBlock *control = static_cast<ExportControlBlock *>(mmfBuffer);
        control->SelectedWidgetIndex = currentWidget;

        Log("MMF opened. WidgetCount=" + std::to_string(controlBlock.WidgetCount));

        // Honour a screen picked while the game was still starting.
        if (pendingWidgetType != 0xFF)
        {
            const uint8_t want = pendingWidgetType;
            pendingWidgetType = 0xFF;
            SelectWidget(want);
        }
        return true;
    }

    // Non-blocking reconnect, ~1 Hz. The app no longer waits on the game at startup, so
    // this is the only place the HUD export is acquired.
    void PollMMF()
    {
        if (mmfBuffer)
            return;

        const ULONGLONG now = GetTickCount64();
        if (now - lastMmfAttempt < 1000)
            return;
        lastMmfAttempt = now;

        OpenMMF();
    }

    bool OpenSharedTexture(int widgetIndex)
    {
        if (widgetIndex >= 10 || !mmfBuffer)
            return false;

        // Read latest control block
        memcpy(&controlBlock, mmfBuffer, sizeof(ExportControlBlock));

        auto &info = controlBlock.Widgets[widgetIndex];
        if (!info.Active)
            return false;

        if (!info.SharedTextureHandle)
            return false;

        // Check if handle changed (mission restart, etc.) - if so, close old texture
        if (lastKnownHandles[widgetIndex] != info.SharedTextureHandle && lastKnownHandles[widgetIndex] != nullptr)
        {
            Log("Widget " + std::to_string(widgetIndex) + " handle changed - reopening");
            sharedSRVs[widgetIndex].Reset();
            sharedTextures[widgetIndex].Reset();
        }

        // If we already have this exact texture open, we're done
        if (sharedTextures[widgetIndex] && lastKnownHandles[widgetIndex] == info.SharedTextureHandle)
        {
            return true;
        }

        // Close old texture if we're reopening
        if (sharedTextures[widgetIndex])
        {
            sharedSRVs[widgetIndex].Reset();
            sharedTextures[widgetIndex].Reset();
            // Also reset cached texture so it gets recreated with new dimensions
            cachedSRVs[widgetIndex].Reset();
            cachedTextures[widgetIndex].Reset();
        }

        ComPtr<ID3D11Texture2D> texture;
        HRESULT hr = device->OpenSharedResource(info.SharedTextureHandle, __uuidof(ID3D11Texture2D), (void **)&texture);

        if (FAILED(hr))
        {
            Log("Failed to open shared texture " + std::to_string(widgetIndex) + " (" + GetWidgetTypeName(info.Type) +
                "): HRESULT=0x" + std::to_string(hr));
            lastKnownHandles[widgetIndex] = nullptr;
            return false;
        }

        sharedTextures[widgetIndex] = texture;
        hr = device->CreateShaderResourceView(texture.Get(), nullptr, &sharedSRVs[widgetIndex]);

        if (FAILED(hr))
        {
            Log("Failed to create SRV for widget " + std::to_string(widgetIndex));
            sharedTextures[widgetIndex].Reset();
            lastKnownHandles[widgetIndex] = nullptr;
            return false;
        }

        lastKnownHandles[widgetIndex] = info.SharedTextureHandle;

        Log("Opened texture " + std::to_string(widgetIndex) + ": " + GetWidgetTypeName(info.Type) + " (" +
            std::to_string(info.Width) + "x" + std::to_string(info.Height) + ")");
        return true;
    }

    bool InitShaders()
    {
        const char *vsCode = R"(
            struct VSOut {
                float4 pos : SV_POSITION;
                float2 uv : TEXCOORD0;
            };

            VSOut main(uint id : SV_VertexID) {
                VSOut output;
                output.uv = float2((id << 1) & 2, id & 2);
                output.pos = float4(output.uv * float2(2, -2) + float2(-1, 1), 0, 1);
                return output;
            }
        )";

        const char *psCode = R"(
            Texture2D tex : register(t0);
            SamplerState samp : register(s0);

            float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
                return tex.Sample(samp, uv);
            }
        )";

        // Separate error blobs: reusing one leaks the first message when both fail.
        ComPtr<ID3DBlob> vsBlob, psBlob, vsError, psError;

        HRESULT hr =
            D3DCompile(vsCode, strlen(vsCode), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, &vsBlob, &vsError);
        if (FAILED(hr) || !vsBlob)
        {
            Log(std::string("Vertex shader failed to compile: ") +
                (vsError ? (const char *)vsError->GetBufferPointer() : "no compiler output"));
            return false;
        }

        hr = D3DCompile(psCode, strlen(psCode), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &psBlob, &psError);
        if (FAILED(hr) || !psBlob)
        {
            Log(std::string("Pixel shader failed to compile: ") +
                (psError ? (const char *)psError->GetBufferPointer() : "no compiler output"));
            return false;
        }

        if (FAILED(device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr,
                                              &vertexShader)) ||
            FAILED(device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr,
                                             &pixelShader)))
        {
            Log("Failed to create shaders");
            return false;
        }

        // The vanilla screens' extra passes: a sub-rectangle sampler (crops and zoom), and the
        // targeting screen's look - monochrome phosphor, edge-lit, scanlined.
        const char *psXformCode = R"(
            Texture2D tex : register(t0);
            SamplerState samp : register(s0);
            cbuffer XF : register(b0) { float4 uvXform; float4 params; };
            float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
                return tex.Sample(samp, uv * uvXform.xy + uvXform.zw);
            }
        )";
        // Outlines of the game's own meshes from its linear scene depth (cm): a line wherever depth
        // jumps relative to the nearer side (silhouettes, including the horizon), plus a fainter
        // line where the surface folds (creases). Green, fading with range, nothing else.
        // Outlines from depth. The texture is a PAIR (VanillaHudShared v8): scene depth on the left,
        // units-only depth on the right, same pose, same frame. A pixel shows a VISIBLE unit where
        // the unit depth is present and matches the scene depth (anything nearer means something is
        // in front of it). Only the outline of those pixels, and lines inside them, are drawn
        // bright - hidden parts of a unit get nothing. Everything else is the world: at most 60%,
        // down to 22% close by, so near foliage recedes. Read with Load, not a filtered sample:
        // a blend of two depths at an edge is a depth that does not exist.
        const char *psDepthEdgeCode = R"(
            Texture2D<float> tex : register(t0);
            cbuffer XF : register(b0) { float4 uvXform; float4 params; };
            float S(int2 p, int2 wh) { p = clamp(p, int2(0, 0), wh - 1); return tex.Load(int3(p, 0)); }
            float U(int2 p, int2 wh) { p = clamp(p, int2(0, 0), wh - 1); return tex.Load(int3(p.x + wh.x, p.y, 0)); }
            bool Vis(int2 p, int2 wh) {
                float u = U(p, wh);
                if (u <= 1.0 || u > 5000000.0) return false;
                return u <= S(p, wh) * 1.01 + 30.0;
            }
            float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
                uint fw, fh;
                tex.GetDimensions(fw, fh);
                const int2 wh = int2(fw / 2, fh);
                float2 t = uv * uvXform.xy + uvXform.zw;
                int2 p = int2(t * float2(wh));

                float c = S(p, wh);
                float l = S(p - int2(1, 0), wh), r = S(p + int2(1, 0), wh);
                float u = S(p - int2(0, 1), wh), b = S(p + int2(0, 1), wh);
                float nearD = max(min(min(c, l), min(min(r, u), b)), 50.0);
                float jump = max(max(abs(c - l), abs(c - r)), max(abs(c - u), abs(c - b))) / nearD;
                float crease = (abs(l + r - 2 * c) + abs(u + b - 2 * c)) / max(c, 50.0);
                float fold = smoothstep(0.015, 0.045, crease);

                if (params.w > 0.5) {
                    bool v  = Vis(p, wh);
                    bool vl = Vis(p - int2(1, 0), wh), vr = Vis(p + int2(1, 0), wh);
                    bool vu = Vis(p - int2(0, 1), wh), vb = Vis(p + int2(0, 1), wh);
                    bool edgeIn  = v && !(vl && vr && vu && vb);   // the unit's own rim
                    bool edgeOut = !v && (vl || vr || vu || vb);   // and the pixel beside it: 2 px, reads at range
                    float a = (edgeIn || edgeOut) ? 1.0 : (v ? smoothstep(0.02, 0.07, jump) * 0.85 : 0.0);
                    if (a > 0.01) return float4(0.62, 1.0, 0.72, a);
                }
                float sil = smoothstep(0.04, 0.12, jump);
                float fade = lerp(1.0, 0.35, saturate(nearD / 150000.0));
                float nearDim = lerp(0.22, 0.6, saturate((nearD - 3000.0) / 37000.0));
                return float4(0.33, 1.0, 0.54, saturate(sil + fold * 0.33) * fade * nearDim);
            }
        )";
        {
            ComPtr<ID3DBlob> b, e;
            if (SUCCEEDED(D3DCompile(psXformCode, strlen(psXformCode), nullptr, nullptr, nullptr, "main", "ps_5_0", 0,
                                     0, &b, &e)) && b)
                device->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &psXform);
            else
                Log(std::string("Crop shader failed: ") + (e ? (const char *)e->GetBufferPointer() : "?"));
            b.Reset();
            e.Reset();
            if (SUCCEEDED(D3DCompile(psDepthEdgeCode, strlen(psDepthEdgeCode), nullptr, nullptr, nullptr, "main", "ps_5_0", 0,
                                     0, &b, &e)) && b)
                device->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &psDepthEdge);
            else
                Log(std::string("Outline shader failed: ") + (e ? (const char *)e->GetBufferPointer() : "?"));
            b.Reset();
            e.Reset();
            // IR, the reverse of the usual thermal palette: black is cold, white is hot. The zoom
            // camera sends scene colour (HDR) with scene depth in alpha, and a units-only depth
            // image; a unit that is VISIBLE (its depth matches the scene's) is hot, everything
            // else is cold scenery shaded dimly by its own brightness so terrain still reads.
            const char *psZoomIRCode = R"(
                Texture2D<float4> col   : register(t0);
                Texture2D<float>  units : register(t1);
                cbuffer XF : register(b0) { float4 uvXform; float4 params; };
                float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
                    uint cw, ch;
                    col.GetDimensions(cw, ch);
                    float2 t = uv * uvXform.xy + uvXform.zw;
                    int2 p = clamp(int2(t * float2(cw, ch)), int2(0, 0), int2(cw - 1, ch - 1));
                    float4 c = col.Load(int3(p, 0));
                    float u = units.Load(int3(p, 0));
                    float lum = dot(max(c.rgb, 0.0), float3(0.299, 0.587, 0.114));
                    float tl = lum / (1.0 + lum);
                    bool hot = u > 1.0 && u < 5000000.0 && u <= c.a * 1.01 + 30.0;
                    // Light sources - lamps, fires, muzzle flashes, glowing vents - are hot too:
                    // anything bright in HDR rises toward white instead of being dimmed.
                    float glow = smoothstep(0.5, 0.85, tl);
                    float cold = 0.04 + 0.26 * tl + 0.70 * glow;
                    float v = hot ? max(0.82 + 0.18 * saturate(tl * 3.0), cold) : cold;
                    return float4(v, v, v, 1.0);
                }
            )";
            if (SUCCEEDED(D3DCompile(psZoomIRCode, strlen(psZoomIRCode), nullptr, nullptr, nullptr, "main", "ps_5_0", 0,
                                     0, &b, &e)) && b)
                device->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &psZoomIR);
            else
                Log(std::string("IR shader failed: ") + (e ? (const char *)e->GetBufferPointer() : "?"));

            D3D11_BUFFER_DESC cb = {};
            cb.ByteWidth = 32;
            cb.Usage = D3D11_USAGE_DYNAMIC;
            cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            device->CreateBuffer(&cb, nullptr, &xformCB);
            cb.ByteWidth = 16 + 16 * kMaxUnitBoxes;
            device->CreateBuffer(&cb, nullptr, &unitsCB);

            D3D11_BLEND_DESC bd = {};
            bd.RenderTarget[0].BlendEnable = TRUE;
            bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
            bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
            bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
            bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
            bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
            bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
            bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            device->CreateBlendState(&bd, &alphaBlend);
        }

        D3D11_SAMPLER_DESC sampDesc = {};
        sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        device->CreateSamplerState(&sampDesc, &sampler);

        Log("Shaders created");
        return true;
    }

    // ---- View switching ---------------------------------------------------

    void LeaveLidar()
    {
        if (!isLidarMode)
            return;
        isLidarMode = false;
        lidar.ReleaseControl(); // stop the plugin scanning projectiles nobody is watching
    }

    // The topo page holds no game-side resource - no control channel, no projectile request,
    // nothing the plugin pays for - so leaving it is just a flag. It keeps polling for its MMF
    // in the background exactly like the scope does.
    void LeaveTopo() { isTopoMode = false; }
    void LeaveTopo3D() { isTopo3DMode = false; }
    // The 3D page is drawn for its own view and for the vanilla Tactical screen, which lays the
    // game's compass and torso indicator over it. Camera buttons work on both.
    bool Show3D() const
    {
        return isTopo3DMode || (isVanillaMode && (vanillaScreen == VSC_Tactical || vanillaScreen == VSC_Targeting ||
                                                  vanillaScreen == VSC_Enhanced ||
                                                  vanillaScreen == VSC_TacOps));
    }

    void SelectTopo3D()
    {
        EndPageInteractions(); // a drag or menu on the old page must not carry over
        isLanceMode = false;   // V-LNC
        if (!d2dAvailable)
        {
            Log("3D view unavailable (Direct2D not initialized)");
            return;
        }
        LeaveLidar();
        LeaveTopo();
        isScreenCaptureMode = false;
        isVanillaMode = false;
        isTopo3DMode = true;
        pendingWidgetType = 0xFF;
        topo3d.SetRangeM(lidarRangeM);
        Log("Selected: 3D tactical map (" + std::to_string(lidarRangeM) + " m)");
        topoNextLogMs = 0.0;
    }

    void SelectTopo()
    {
        EndPageInteractions(); // a drag or menu on the old page must not carry over
        isLanceMode = false;   // V-LNC
        if (!d2dAvailable)
        {
            Log("TOPO view unavailable (Direct2D not initialized)");
            return;
        }
        LeaveLidar();
        LeaveTopo3D();
        isScreenCaptureMode = false;
        isVanillaMode = false;
        isTopoMode = true;
        pendingWidgetType = 0xFF; // don't get pulled back to a widget when the game connects
        if (!topo.IsConnected())
        {
            Log("Selected: topographic map (MW5_TerrainExport not up yet - it will fill in)");
            return;
        }
        // Deliberately NOT the whole status here: nothing has rendered yet, so the draw and bake
        // figures are all zero. That is exactly the trap the v1 diag fell into - it logged
        // "0 tiles, 0 segments, bake 0.000 ms" and looked like a failure when it was just early.
        Log("Selected: topographic map (span " + std::to_string((int)topoSpanM) + " m)");
        topoNextLogMs = 0.0; // sample properly on the next frame that actually draws
    }

    void SelectLidar()
    {
        EndPageInteractions(); // a drag or menu on the old page must not carry over
        isLanceMode = false;   // V-LNC
        if (!d2dAvailable)
        {
            Log("LIDAR view unavailable (Direct2D not initialized)");
            return;
        }
        LeaveTopo();
        LeaveTopo3D();
        isScreenCaptureMode = false;
        isVanillaMode = false;
        isLidarMode = true;
        pendingWidgetType = 0xFF; // don't get pulled back to a widget when the game connects
        // selectedWidgetType is deliberately NOT cleared. IsSlotActive already suppresses the
        // screen legends while the scope or the capture view is up, so clearing it would add
        // nothing - and it would lose which screen to re-light when you come back off them.
        Log("Selected: LIDAR scope");
    }

    // -----------------------------------------------------------------------------------
    // Selecting a screen ALWAYS succeeds. It is a view change, not a request to the game.
    //
    // This used to refuse in three places - no MMF, no slot of that type, slot not Active -
    // and a refusal meant the button did nothing at all. That is the wrong model: whether a
    // mech has published its Weapons screen yet is not a reason to deny the pilot the Weapons
    // page. Worse, the refusals were indistinguishable from a dead binding, which is how an
    // exporter that had quietly stood down looked exactly like a broken keyboard.
    //
    // Now the view always moves and the screen fills itself in. RenderWidget re-reads the
    // control block every frame, shows black while the slot is inactive, and opens the shared
    // texture the moment it goes live - so a screen selected before the mech exists simply
    // appears when the mech does, with no latch, no retry timer and no extra state.
    // -----------------------------------------------------------------------------------
    void SelectWidget(uint8_t widgetType)
    {
        EndPageInteractions(); // a drag or menu on the old page must not carry over
        isLanceMode = false;   // V-LNC
        LeaveLidar();
        LeaveTopo();
        LeaveTopo3D();
        isScreenCaptureMode = false;
        isVanillaMode = false;
        selectedWidgetType = widgetType;

        // The table index for a type. Both exporters lay the screens out at index == type
        // (slots 4..9), so that is the slot to watch even before anything is published there;
        // FindWidgetByType is still preferred when the exporter is live, in case that ever
        // stops holding. Bounds-checked either way - currentWidget indexes a 10-slot array.
        int widgetIndex = -1;
        if (mmfBuffer)
        {
            memcpy(&controlBlock, mmfBuffer, sizeof(ExportControlBlock));
            widgetIndex = FindWidgetByType(widgetType);
        }
        if (widgetIndex < 0)
            widgetIndex = (widgetType < 10) ? (int)widgetType : 4;

        currentWidget = (uint32_t)widgetIndex;

        // Still honoured by OpenMMF for a screen picked before the game connects.
        pendingWidgetType = widgetType;

        if (!mmfBuffer)
        {
            Log("Selected: " + std::string(GetWidgetTypeName(widgetType)) + " (game not connected - black until it is)");
            return;
        }

        ExportControlBlock *control = static_cast<ExportControlBlock *>(mmfBuffer);
        control->SelectedWidgetIndex = (uint32_t)widgetIndex;

        auto &info = controlBlock.Widgets[widgetIndex];
        if (!info.Active || !info.SharedTextureHandle)
        {
            Log("Selected: " + std::string(GetWidgetTypeName(widgetType)) + " (index " + std::to_string(widgetIndex) +
                ", not published yet - black until it is)");
            return;
        }

        Log("Selected: " + std::string(GetWidgetTypeName(info.Type)) + " (index " + std::to_string(widgetIndex) + ", " +
            std::to_string(info.Width) + "x" + std::to_string(info.Height) + ")");

        if (!sharedTextures[widgetIndex] || !sharedSRVs[widgetIndex])
        {
            OpenSharedTexture(widgetIndex);
        }
    }

    // Sample the TOPO page's own numbers while it is up. The interval is long enough not to
    // clutter the log and short enough to catch the filling phase, which is the only time any
    // of this is in motion.
    void LogTopoStatus()
    {
        // Also sampled while the SCOPE is up, because terrain is drawn there now and its cost
        // belongs in the same log rather than only being visible on the page it came from.
        if (!(isTopoMode || isLidarMode || Show3D()) || !topo.IsConnected())
            return;
        const double now = NowMs();
        if (now < topoNextLogMs)
            return;
        topoNextLogMs = now + 5000.0;
        if (Show3D())
        {
            const Topo3DView::Diag d = topo3d.GetDiag();
            Log("3D " + std::string(d.Free ? "FREE" : "CHASE") + " | " + std::to_string(d.RangeM) + " m | " +
                std::to_string(d.TilesDrawn) + " tiles / " + std::to_string(d.Lines) + " lines | " +
                std::to_string(d.Contacts) + " contacts, " + std::to_string(d.Projectiles) + " rounds (" +
                std::to_string(d.Trails) + " smoking, " + std::to_string(d.Flashing) + " inbound) | " +
                std::to_string(d.Silhouettes) + " posed | " +
                std::to_string(d.MechInfo) + " known, " + std::to_string(d.Labels) +
                " labelled (" + std::to_string(d.DollArt) + " art, " +
                std::to_string(d.InArc) + " doll shown, lock " + std::to_string(d.LockedId) + ") | " +
                std::to_string(d.MechContacts) + "M/" + std::to_string(d.VehicleContacts) +
                "V, cull " + std::to_string(d.CulledFar) + " far " +
                std::to_string(d.CulledOffScreen) + " off, " + std::to_string(d.NoRecord) +
                " unmatched, " + std::to_string(d.ArtMisses) + " no-art" +
                (d.MissingArt[0] ? (" (" + std::string(d.MissingArt) + ")") : std::string()) +
                (d.FocusWeapons >= 0 ? (" | focus " + std::string(d.FocusName) + " w" +
                                        std::to_string(d.FocusWeapons)) : std::string()) +
                " | yaw " +
                std::to_string((int)d.YawDeg) + " pitch " + std::to_string((int)d.PitchDeg) + " | build " +
                std::to_string(d.BuildMs) + " ms");
        }
        else if (isLidarMode)
            Log("LIDAR terrain underlay: " + std::to_string(lidar.TerrainSegments()) + " segs | " + TopoStatus());
        else
            Log("TOPO " + TopoStatus());
    }

    void ToggleScreenCapture()
    {
        EndPageInteractions(); // a drag or menu on the old page must not carry over
        isLanceMode = false;   // V-LNC
        LeaveLidar();
        LeaveTopo();
        LeaveTopo3D();
        pendingWidgetType = 0xFF;
        isVanillaMode = false;
        isScreenCaptureMode = !isScreenCaptureMode;

        Log(isScreenCaptureMode ? "Switched to the zoom camera (CAP)" : "Zoom camera (CAP) off - back to the MWVR screen");
    }

    void DoAction(const LabelSlot &s)
    {
        switch (s.Action)
        {
        case ACT_Widget:
            SelectWidget(s.Param);
            break;
        case ACT_VanillaWidget:
            SelectVanilla(s.Param);
            break;
        case ACT_ZoomCamIR:
            zoomCamIR = !zoomCamIR;
            Log(std::string("Zoom camera IR ") + (zoomCamIR ? "on" : "off"));
            break;
        case ACT_TacOverlay:
        case ACT_TacOverlayHold:
            OvToggle(s.Action == ACT_TacOverlayHold, s.Key);
            break;
        case ACT_EiVision:
            EiVisionToggle();
            break;
        case ACT_LancePage:
            SelectLance();
            break;
        case ACT_Compass:
            compassOn = !compassOn;
            Log(std::string("Heading compass ") + (compassOn ? "on" : "off"));
            SaveConfig();
            break;
        case ACT_Lidar:
            SelectLidar();
            break;
        case ACT_ScreenCapture:
            ToggleScreenCapture();
            break;
        case ACT_LidarZoomIn:
            ZoomActivePage(true);
            break;
        case ACT_LidarZoomOut:
            ZoomActivePage(false);
            break;
        case ACT_CaptureZoomIn:
            StepCamZoom(true);
            break;
        case ACT_CaptureZoomOut:
            StepCamZoom(false);
            break;
        case ACT_Topo:
            SelectTopo();
            break;
        case ACT_Topo3D:
            SelectTopo3D();
            break;
        // Camera controls act only on the 3D page. On any other page they do nothing rather
        // than something surprising - a camera button that silently re-ranged the scope would
        // be worse than a dead press.
        case ACT_CamRecentre:
            if (IsTacOps()) { topo3d.TacRecentre(); Log("TacOps camera recentred on the mech"); }
            else if (Show3D()) { topo3d.Recentre(); Log("3D camera recentred"); }
            break;
        case ACT_CamLeft:
            if (Show3D()) topo3d.Orbit(-10.0f, 0.0f);
            break;
        case ACT_CamRight:
            if (Show3D()) topo3d.Orbit(10.0f, 0.0f);
            break;
        case ACT_CamUp:
            if (Show3D()) topo3d.Orbit(0.0f, 5.0f);
            break;
        case ACT_CamDown:
            if (Show3D()) topo3d.Orbit(0.0f, -5.0f);
            break;
        case ACT_CamFwd:
            if (Show3D()) topo3d.Pan(Topo3DView::kPanStepM, 0.0f);
            break;
        case ACT_CamBack:
            if (Show3D()) topo3d.Pan(-Topo3DView::kPanStepM, 0.0f);
            break;
        default:
            break; // ACT_None - an unassigned button does nothing
        }
    }

    // Is this label the view currently on screen?
    bool IsSlotActive(const LabelSlot &s) const
    {
        switch (s.Action)
        {
        case ACT_Lidar:
            return isLidarMode;
        case ACT_ScreenCapture:
            return isScreenCaptureMode;
        case ACT_Topo:
            return isTopoMode;
        case ACT_Topo3D:
            return isTopo3DMode;
        case ACT_CamRecentre:
        case ACT_CamLeft:
        case ACT_CamRight:
        case ACT_CamUp:
        case ACT_CamDown:
        case ACT_CamFwd:
        case ACT_CamBack:
            return Show3D() && topo3d.IsFree();
        case ACT_VanillaWidget:
            return isVanillaMode && vanillaScreen == s.Param;
        case ACT_ZoomCamIR:
            return zoomCamIR;
        case ACT_TacOverlay:
        case ACT_TacOverlayHold:
            return ovOn;
        case ACT_EiVision:
            return vBlock && vBlock->EiVisionOn != 0;
        case ACT_Compass:
            return compassOn;
        case ACT_LancePage:
            return isLanceMode;
        case ACT_Widget:
            if (isLidarMode || isScreenCaptureMode || isTopoMode || isTopo3DMode || isVanillaMode || isLanceMode)
                return false;
            // The pilot's choice, not the published table. Reading Type out of the block would
            // leave the button unlit whenever the slot is unpublished - a freshly cleared
            // block reads Type 0 for every slot - which is exactly when the lit button matters
            // most, because the screen is black and the legend is the only confirmation.
            return selectedWidgetType == s.Param;
        default:
            return false;
        }
    }

    // ---- Rendering --------------------------------------------------------

    void RenderWidget()
    {
        // Refresh control block to get latest data
        if (mmfBuffer)
        {
            memcpy(&controlBlock, mmfBuffer, sizeof(ExportControlBlock));
        }

        const float clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        context->ClearRenderTargetView(backBufferRTV.Get(), clearColor);

        if (!mmfBuffer)
            return; // game not connected - black behind the bezel

        auto &info = controlBlock.Widgets[currentWidget];

        // If widget is not active, clear cached resources and show black screen
        if (!info.Active)
        {
            sharedTextures[currentWidget].Reset();
            sharedSRVs[currentWidget].Reset();
            cachedTextures[currentWidget].Reset();
            cachedSRVs[currentWidget].Reset();
            lastKnownHandles[currentWidget] = nullptr;
            return;
        }

        bool needsReopen = !sharedTextures[currentWidget] || !sharedSRVs[currentWidget] ||
                           (info.SharedTextureHandle && lastKnownHandles[currentWidget] != info.SharedTextureHandle);

        if (needsReopen)
        {
            OpenSharedTexture(currentWidget);
            if (!sharedTextures[currentWidget])
                return;
        }

        ComPtr<IDXGIKeyedMutex> mutex;

        if (SUCCEEDED(sharedTextures[currentWidget].As(&mutex)))
        {
            // Try to acquire with longer timeout (33ms = ~2 frames at 60fps)
            HRESULT hr = mutex->AcquireSync(1, 33);

            if (hr == S_OK) // bug sweep 2026-10-17: WAIT_TIMEOUT / WAIT_ABANDONED are SUCCESS codes - not the lock
            {
                // Successfully got the mutex - copy to our local cache

                // Get shared texture descriptor
                D3D11_TEXTURE2D_DESC sharedDesc;
                sharedTextures[currentWidget]->GetDesc(&sharedDesc);

                // Check if cached texture exists and matches size
                bool needsRecreate = !cachedTextures[currentWidget];
                if (cachedTextures[currentWidget])
                {
                    D3D11_TEXTURE2D_DESC cachedDesc;
                    cachedTextures[currentWidget]->GetDesc(&cachedDesc);

                    // Recreate if size changed
                    if (cachedDesc.Width != sharedDesc.Width || cachedDesc.Height != sharedDesc.Height)
                    {
                        Log("Cached texture size mismatch for widget " + std::to_string(currentWidget) +
                            ", recreating");
                        cachedTextures[currentWidget].Reset();
                        cachedSRVs[currentWidget].Reset();
                        needsRecreate = true;
                    }
                }

                if (needsRecreate)
                {
                    // Create cached texture matching shared texture size
                    D3D11_TEXTURE2D_DESC desc = sharedDesc;
                    desc.MiscFlags = 0; // Remove shared flag for local copy

                    if (SUCCEEDED(device->CreateTexture2D(&desc, nullptr, cachedTextures[currentWidget].GetAddressOf())))
                    {
                        device->CreateShaderResourceView(cachedTextures[currentWidget].Get(), nullptr,
                                                         cachedSRVs[currentWidget].GetAddressOf());
                        Log("Created cached texture for widget " + std::to_string(currentWidget));
                    }
                }

                if (cachedTextures[currentWidget])
                {
                    context->CopyResource(cachedTextures[currentWidget].Get(), sharedTextures[currentWidget].Get());
                }

                mutex->ReleaseSync(0);
            }
        }

        // Render from cache (whether we just updated it or using old frame)
        if (cachedSRVs[currentWidget])
            DrawFullscreenTexture(cachedSRVs[currentWidget].Get());
    }

    // ---- Vanilla HUD screens --------------------------------------------------------------
    //
    // VanillaHud.dll takes the whole vanilla HUD off the main screen and publishes it as texture
    // SOURCES (one per element, plus the leftover HUD as a full-view overlay, plus the game image
    // on request). The SCREENS below are compositions of those sources, laid out inside the same
    // inset every other view uses, so nothing ever sits under the bezel legends.

    struct VRect
    {
        float x, y, w, h;
    };

    void SelectVanilla(uint8_t screen)
    {
        EndPageInteractions(); // a drag or menu on the old page must not carry over
        isLanceMode = false;   // V-LNC
        if (screen >= VSC_COUNT)
        {
            Log("That vanilla screen no longer exists - reassign the button (edit menu)");
            return;
        }
        LeaveLidar();
        LeaveTopo();
        LeaveTopo3D();
        isScreenCaptureMode = false;
        pendingWidgetType = 0xFF;
        isVanillaMode = true;
        vanillaScreen = screen;
        vDiagNextMs = 0.0;
        if (screen == VSC_Tactical || screen == VSC_TacOps)
            topo3d.SetRangeM(lidarRangeM);
        char name[96];
        WideCharToMultiByte(CP_UTF8, 0, VanillaScreenName(screen), -1, name, sizeof(name), nullptr, nullptr);
        Log(std::string("Selected: ") + name + (vBlock ? "" : " (VanillaHud plugin not connected yet)"));
    }

    // ~1 Hz until the plugin's block exists. Read/write: the targeting screen writes its request
    // for the game image into it.
    void PollVanillaMMF()
    {
        if (vBlock)
        {
            if (vBlock->Version == VHUD_VERSION)
                return;
            // The plugin was rebuilt under us with another layout; let go and look again.
            UnmapViewOfFile(vBlock);
            vBlock = nullptr;
            CloseHandle(vMmfHandle);
            vMmfHandle = nullptr;
            for (int i = 0; i < VHUD_MAX_SLOTS; ++i)
                ResetVanillaSlot(i);
        }
        const ULONGLONG now = GetTickCount64();
        if (now - vLastAttempt < 1000)
            return;
        vLastAttempt = now;
        vMmfHandle = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, VHUD_MMF_NAME);
        if (!vMmfHandle)
            return;
        vBlock = static_cast<VanillaExportBlock *>(
            MapViewOfFile(vMmfHandle, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(VanillaExportBlock)));
        if (!vBlock || vBlock->Version != VHUD_VERSION)
        {
            if (vBlock)
            {
                Log("VanillaHud block is version " + std::to_string(vBlock->Version) + ", this build reads " +
                    std::to_string(VHUD_VERSION) + " - rebuild the plugin and HUDDisplay together");
                UnmapViewOfFile(vBlock);
                vBlock = nullptr;
            }
            CloseHandle(vMmfHandle);
            vMmfHandle = nullptr;
            return;
        }
        Log("VanillaHud block opened (v" + std::to_string(VHUD_VERSION) + ")");
    }

    void ResetVanillaSlot(int i)
    {
        vStage[i].Reset();
        vTrim[i] = VTrim{};
        vShared[i].Reset();
        vSharedSRV[i].Reset();
        vCached[i].Reset();
        vCachedSRV[i].Reset();
        vLastHandle[i] = 0;
    }

    // The latest image of one source, copied into a local cache under the keyed mutex so a frame
    // where the game holds the texture still shows the last good one. Null when there is none.
    ID3D11ShaderResourceView *VanillaSrv(int i, uint32_t &w, uint32_t &h)
    {
        w = h = 0;
        if (!vBlock || i < 0 || i >= VHUD_MAX_SLOTS)
            return nullptr;
        const VanillaExportBlock::Slot info = vBlock->Slots[i];
        if (!info.Active || !info.SharedHandle)
        {
            ResetVanillaSlot(i);
            return nullptr;
        }
        if (!vShared[i] || vLastHandle[i] != info.SharedHandle)
        {
            ResetVanillaSlot(i);
            ComPtr<ID3D11Texture2D> tex;
            if (FAILED(device->OpenSharedResource(reinterpret_cast<HANDLE>(info.SharedHandle), __uuidof(ID3D11Texture2D),
                                                  (void **)tex.GetAddressOf())))
                return nullptr;
            if (FAILED(device->CreateShaderResourceView(tex.Get(), nullptr, vSharedSRV[i].GetAddressOf())))
                return nullptr;
            vShared[i] = tex;
            vLastHandle[i] = info.SharedHandle;
        }
        ComPtr<IDXGIKeyedMutex> mutex;
        if (SUCCEEDED(vShared[i].As(&mutex)) && mutex->AcquireSync(1, 8) == S_OK) // bug sweep 2026-10-17: a timeout is no lock
        {
            D3D11_TEXTURE2D_DESC sd;
            vShared[i]->GetDesc(&sd);
            if (vCached[i])
            {
                D3D11_TEXTURE2D_DESC cd;
                vCached[i]->GetDesc(&cd);
                if (cd.Width != sd.Width || cd.Height != sd.Height || cd.Format != sd.Format)
                {
                    vCached[i].Reset();
                    vCachedSRV[i].Reset();
                }
            }
            if (!vCached[i])
            {
                D3D11_TEXTURE2D_DESC d = sd;
                d.MiscFlags = 0;
                if (SUCCEEDED(device->CreateTexture2D(&d, nullptr, vCached[i].GetAddressOf())))
                    device->CreateShaderResourceView(vCached[i].Get(), nullptr, vCachedSRV[i].GetAddressOf());
            }
            if (vCached[i])
            {
                context->CopyResource(vCached[i].Get(), vShared[i].Get());
                if (i == VS_Depth)
                {
                    vDepthCam  = vBlock->DepthCam;  // the pose this image was rendered from
                    vDepthLock = vBlock->DepthLock; // and the locked target in that same frame
                    vDepthBeamCount = vBlock->BeamCount <= (uint32_t)VanillaExportBlock::kMaxBeams ? vBlock->BeamCount : 0;
                    memcpy(vDepthBeams, (const void *)vBlock->Beams, sizeof(vDepthBeams[0]) * vDepthBeamCount);
                }
                else if (i == VS_EiDepth) // v21: Enhanced Imaging's frame - same fields, its own copy in the block
                {
                    vDepthCam  = vBlock->EiCam;
                    vDepthLock = vBlock->EiLock;
                    vDepthBeamCount = vBlock->EiBeamCount <= (uint32_t)VanillaExportBlock::kMaxBeams ? vBlock->EiBeamCount : 0;
                    memcpy(vDepthBeams, (const void *)vBlock->EiBeams, sizeof(vDepthBeams[0]) * vDepthBeamCount);
                }
            }
            mutex->ReleaseSync(0);
        }
        if (!vCachedSRV[i])
            return nullptr;
        w = info.Width;
        h = info.Height;
        return vCachedSRV[i].Get();
    }

    // The drawable area: the window minus the bezel margin every view keeps clear.
    VRect ContentRect() const
    {
        if (renderOverW > 0.0f) // v22: the vision image - all of it, no bezel
            return {0.0f, 0.0f, renderOverW, renderOverH};
        RECT rc;
        GetClientRect(hwnd, &rc);
        const float cw = (float)(rc.right - rc.left), ch = (float)(rc.bottom - rc.top);
        return {cw * kContentInset, ch * kContentInset, cw * (1.0f - 2.0f * kContentInset),
                ch * (1.0f - 2.0f * kContentInset)};
    }

    // Largest rectangle of the texture's aspect that fits in the cell, centred.
    static VRect FitRect(uint32_t tw, uint32_t th, const VRect &cell)
    {
        if (!tw || !th)
            return cell;
        const float k = ((cell.w / (float)tw) < (cell.h / (float)th)) ? (cell.w / (float)tw) : (cell.h / (float)th);
        const float w = (float)tw * k, h = (float)th * k;
        return {cell.x + (cell.w - w) * 0.5f, cell.y + (cell.h - h) * 0.5f, w, h};
    }

    // Draw a texture into a rectangle of the window: optionally a sub-rectangle of it (uvXform =
    // scale.xy, offset.xy), through a chosen pixel shader, blended or opaque.
    void DrawSrv(ID3D11ShaderResourceView *srv, const VRect &r, ID3D11PixelShader *ps, const float uvXform[4],
                 bool blend, const float params[4] = nullptr)
    {
        if (!srv || r.w < 1.0f || r.h < 1.0f || !ps || !xformCB)
            return;
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(context->Map(xformCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
        {
            float cb[8] = {uvXform[0], uvXform[1], uvXform[2], uvXform[3], 0, 0, 0, 0};
            if (params)
                memcpy(cb + 4, params, 4 * sizeof(float));
            memcpy(m.pData, cb, sizeof(cb));
            context->Unmap(xformCB.Get(), 0);
        }
        context->OMSetRenderTargets(1, backBufferRTV.GetAddressOf(), nullptr);
        const float bf[4] = {0, 0, 0, 0};
        context->OMSetBlendState(blend ? alphaBlend.Get() : nullptr, bf, 0xFFFFFFFF);
        D3D11_VIEWPORT vp = {r.x, r.y, r.w, r.h, 0.0f, 1.0f};
        context->RSSetViewports(1, &vp);
        context->VSSetShader(vertexShader.Get(), nullptr, 0);
        context->PSSetShader(ps, nullptr, 0);
        context->PSSetConstantBuffers(0, 1, xformCB.GetAddressOf());
        context->PSSetShaderResources(0, 1, &srv);
        context->PSSetSamplers(0, 1, sampler.GetAddressOf());
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->Draw(3, 0);
        context->OMSetBlendState(nullptr, bf, 0xFFFFFFFF);
    }

    // Find the drawn part of a source's image: the bounding box of its non-transparent pixels.
    void UpdateTrim(int i, uint32_t w, uint32_t h)
    {
        VTrim &t = vTrim[i];
        const double now = NowMs();
        // NON-BLOCKING READBACK (trimasync1). Copying the panel to a CPU texture and mapping it
        // straight away made the CPU wait for the GPU to finish everything queued before it -
        // the 41 ms 'view' frames on the HUD pages. Now the copy is issued on one frame and read
        // with DO_NOT_WAIT on later frames until the GPU has it; the crop simply updates a frame
        // or two later. A copy the GPU never delivers is dropped after 2 s and re-issued.
        D3D11_TEXTURE2D_DESC d;
        D3D11_MAPPED_SUBRESOURCE m;
        if (!t.Pending)
        {
            if (t.W == w && t.H == h && now < t.NextMs)
                return;
            t.NextMs = now + 1000.0;
            if (!vCached[i])
                return;
            vCached[i]->GetDesc(&d);
            if (d.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB && d.Format != DXGI_FORMAT_B8G8R8A8_UNORM)
                return;
            if (vStage[i])
            {
                D3D11_TEXTURE2D_DESC sd;
                vStage[i]->GetDesc(&sd);
                if (sd.Width != d.Width || sd.Height != d.Height || sd.Format != d.Format)
                    vStage[i].Reset();
            }
            if (!vStage[i])
            {
                D3D11_TEXTURE2D_DESC sd = d;
                sd.Usage = D3D11_USAGE_STAGING;
                sd.BindFlags = 0;
                sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                sd.MiscFlags = 0;
                if (FAILED(device->CreateTexture2D(&sd, nullptr, vStage[i].GetAddressOf())))
                    return;
            }
            context->CopyResource(vStage[i].Get(), vCached[i].Get());
            t.Pending = true;
            t.PendW = w;
            t.PendH = h;
            t.PendMs = now;
            return;
        }
        if (!vStage[i])
        {
            t.Pending = false;
            return;
        }
        const HRESULT mhr = context->Map(vStage[i].Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
        if (mhr == DXGI_ERROR_WAS_STILL_DRAWING && now - t.PendMs < 2000.0)
            return;
        t.Pending = false;
        if (FAILED(mhr))
            return;
        vStage[i]->GetDesc(&d);
        w = t.PendW; // the size the copy was taken at
        h = t.PendH;
        uint32_t x0 = d.Width, y0 = d.Height, x1 = 0, y1 = 0;
        for (uint32_t y = 0; y < d.Height; ++y)
        {
            const uint8_t *row = static_cast<const uint8_t *>(m.pData) + (size_t)y * m.RowPitch;
            for (uint32_t x = 0; x < d.Width; ++x)
                if (row[x * 4 + 3] > 8)
                {
                    if (x < x0) x0 = x;
                    if (x > x1) x1 = x;
                    if (y < y0) y0 = y;
                    if (y > y1) y1 = y;
                }
        }
        context->Unmap(vStage[i].Get(), 0);
        if (x1 < x0 || y1 < y0)
        {
            // Nothing drawn this second (e.g. no target): keep the last box rather than jump.
            if (t.W != w || t.H != h)
                t = VTrim{0, 0, w, h, w, h, t.NextMs};
            t.Empty = true;
            return;
        }
        t.Empty = false;
        const uint32_t pad = 3;
        uint32_t nx0 = x0 > pad ? x0 - pad : 0;
        uint32_t ny0 = y0 > pad ? y0 - pad : 0;
        uint32_t nx1 = (x1 + 1 + pad < w) ? x1 + 1 + pad : w;
        uint32_t ny1 = (y1 + 1 + pad < h) ? y1 + 1 + pad : h;
        // GAUGES: THE UNION OF THE LAST TEN SECONDS' BOXES. A bar's drawn extent changes with its
        // value, so a box trimmed to this second's pixels rescaled the whole gauge as the throttle
        // moved. That used to be solved by only ever GROWING the box - but one bad frame (the
        // canvas before the widget first paints is solid) latched it at the whole canvas for good,
        // and the log showed exactly that: throttle and heat "trimmed" to 224x361, jump jets to
        // 102x358, their full canvases, so the real gauges drew at a fraction of the room. Now a
        // box that fills the canvas is ignored outright (a gauge's canvas is 1.6x its own box plus
        // a margin, so it never legitimately does), and older boxes age out.
        // The OWNER PANEL too (ownunion1): its doll blinks parts out when hit, and a box taken
        // from one such second cropped to the two parts still lit and blew them up to fill the
        // page. Its canvas is 2.4x its width, so the 'fills the canvas' rule never fires on it.
        const bool grow = (i == VS_Heat || i == VS_Throttle || i == VS_JumpJets || i == VS_Owner);
        if (grow)
        {
            const bool full = (nx1 - nx0) * 100u >= w * 95u && (ny1 - ny0) * 100u >= h * 95u;
            if (t.W != w || t.H != h)
                t.HistN = t.HistPos = 0;
            if (full)
            {
                if (t.HistN == 0)
                    t = VTrim{0, 0, w, h, w, h, t.NextMs}; // nothing better yet
                t.Empty = false;
                return;
            }
            uint32_t *e = t.Hist[t.HistPos];
            e[0] = nx0; e[1] = ny0; e[2] = nx1; e[3] = ny1;
            t.HistPos = (t.HistPos + 1) % 10;
            if (t.HistN < 10)
                ++t.HistN;
            for (int k = 0; k < t.HistN; ++k)
            {
                const uint32_t *b = t.Hist[k];
                nx0 = b[0] < nx0 ? b[0] : nx0;
                ny0 = b[1] < ny0 ? b[1] : ny0;
                nx1 = b[2] > nx1 ? b[2] : nx1;
                ny1 = b[3] > ny1 ? b[3] : ny1;
            }
        }
        t.X0 = nx0;
        t.Y0 = ny0;
        t.X1 = nx1;
        t.Y1 = ny1;
        t.W = w;
        t.H = h;
    }

    // One source, trimmed to what it drew, aspect-fitted into a cell, blended (the elements are
    // drawn on transparency).
    void DrawSource(int src, const VRect &cell)
    {
        // VHUD v20: a panel the player left on the game's HUD (GlobalSettings, HUD tab).
        if (vBlock && src >= 0 && src < VHUD_MAX_SLOTS && vBlock->Slots[src].HostState == VHS_InGame)
        {
            if (d2dCtx)
            {
                d2dCtx->BeginDraw();
                DrawPanelText(cell.x + cell.w * 0.5f - 70.0f, cell.y + cell.h * 0.5f - 11.0f, 160.0f, L"SHOWN IN GAME",
                              0x55FF8A, 0.6f);
                d2dCtx->EndDraw();
            }
            return;
        }
        uint32_t w, h;
        ID3D11ShaderResourceView *srv = VanillaSrv(src, w, h);
        if (!srv)
            return;
        UpdateTrim(src, w, h);
        const VTrim &t = vTrim[src];
        float xf[4] = {1, 1, 0, 0};
        uint32_t cw = w, ch = h;
        if (t.W == w && t.H == h && t.X1 > t.X0 && t.Y1 > t.Y0)
        {
            cw = t.X1 - t.X0;
            ch = t.Y1 - t.Y0;
            xf[0] = (float)cw / (float)w;
            xf[1] = (float)ch / (float)h;
            xf[2] = (float)t.X0 / (float)w;
            xf[3] = (float)t.Y0 / (float)h;
        }
        DrawSrv(srv, FitRect(cw, ch, cell), psXform.Get(), xf, true);
    }

    // Gauges side by side at ONE shared scale - the largest at which every one of them fits its
    // column - bottom-aligned, so a narrow bar is not blown up to a different size from a wide one.
    void DrawGaugeRow(const int *src, int n, const VRect &area, float gap)
    {
        if (n <= 0 || n > 8)
            return;
        const float colW = (area.w - gap * (float)(n - 1)) / (float)n;
        ID3D11ShaderResourceView *srv[8] = {};
        float xf[8][4];
        uint32_t cw[8] = {}, ch[8] = {};
        float scale = 1e9f;
        for (int i = 0; i < n; ++i)
        {
            uint32_t w, h;
            srv[i] = VanillaSrv(src[i], w, h);
            if (!srv[i])
                continue;
            UpdateTrim(src[i], w, h);
            const VTrim &t = vTrim[src[i]];
            xf[i][0] = 1; xf[i][1] = 1; xf[i][2] = 0; xf[i][3] = 0;
            cw[i] = w;
            ch[i] = h;
            if (t.W == w && t.H == h && t.X1 > t.X0 && t.Y1 > t.Y0)
            {
                cw[i] = t.X1 - t.X0;
                ch[i] = t.Y1 - t.Y0;
                xf[i][0] = (float)cw[i] / (float)w;
                xf[i][1] = (float)ch[i] / (float)h;
                xf[i][2] = (float)t.X0 / (float)w;
                xf[i][3] = (float)t.Y0 / (float)h;
            }
        }
        // ONE SCALE FOR THE ROW, SET BY THE ROOM IT ACTUALLY NEEDS. Each gauge used to get an equal
        // third of the width and the row took the smallest fit, so one wide gauge (or one whose
        // trimmed box is wide) shrank all three to a sliver - about a tenth of the paperdoll's
        // height. Now the gauges sit side by side at their own widths: the row is as tall as the
        // area allows unless their combined width runs out first.
        (void)colW;
        float sumW = 0.0f, maxH = 0.0f;
        int live = 0;
        for (int i = 0; i < n; ++i)
            if (srv[i])
            {
                sumW += (float)cw[i];
                maxH = (float)ch[i] > maxH ? (float)ch[i] : maxH;
                ++live;
            }
        if (!live || sumW < 1.0f || maxH < 1.0f)
            return;
        const float roomW = area.w - gap * (float)(live - 1);
        scale = (roomW / sumW < area.h / maxH) ? roomW / sumW : area.h / maxH;

        // What each gauge trimmed to, logged when it changes - if the row is still small, this
        // says which gauge's box is the wide one.
        static uint32_t lastSig = 0;
        uint32_t sig = 2166136261u;
        for (int i = 0; i < n; ++i)
            sig = (sig ^ (cw[i] * 131u + ch[i])) * 16777619u;
        if (sig != lastSig)
        {
            lastSig = sig;
            std::string msg = "Gauge row: area " + std::to_string((int)area.w) + "x" + std::to_string((int)area.h) +
                              ", scale " + std::to_string(scale) + " |";
            for (int i = 0; i < n; ++i)
            {
                char nm[64];
                WideCharToMultiByte(CP_UTF8, 0, VanillaSourceName(src[i]), -1, nm, sizeof(nm), nullptr, nullptr);
                msg += std::string(" ") + nm + " " + std::to_string(cw[i]) + "x" + std::to_string(ch[i]) + " |";
            }
            Log(msg);
        }

        // TOP-ALIGNED, and lifted a little off the bottom of the area. Each gauge's box starts at
        // its bar's top edge, while what hangs below the bar differs (the thermometer icon is
        // taller than the throttle's), so lining the images up by their bottoms left the heat bar
        // standing a few pixels proud. Lining up the tops lines up the bars.
        const float rowW = sumW * scale + gap * (float)(live - 1);
        const float rowH = maxH * scale;
        const float lift = area.h * 0.07f;
        float top = area.y + area.h - rowH - lift;
        if (top < area.y)
            top = area.y;
        float x = area.x + (area.w - rowW) * 0.5f;
        for (int i = 0; i < n; ++i)
        {
            if (!srv[i])
                continue;
            const float dw = (float)cw[i] * scale, dh = (float)ch[i] * scale;
            DrawSrv(srv[i], {x, top, dw, dh}, psXform.Get(), xf[i], true);
            x += dw + gap;
        }
    }

    // A source that is live and drew something at the last trim.
    bool SourceShowing(int src)
    {
        uint32_t w, h;
        if (!VanillaSrv(src, w, h))
            return false;
        UpdateTrim(src, w, h);
        return !vTrim[src].Empty;
    }

    void RenderVanilla()
    {
        const float clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        context->ClearRenderTargetView(backBufferRTV.Get(), clearColor);
        PollVanillaMMF();
        if (!vBlock)
            return;
        LogVanillaDiag();

        const VRect R = ContentRect();
        const float g = (R.w < R.h ? R.w : R.h) * 0.015f; // gutter between cells

        switch (vanillaScreen)
        {
        case VSC_Target:
            DrawSource(VS_Target, R);
            break;
        case VSC_Weapons:
            DrawSource(VS_Weapons, R);
            break;
        case VSC_Map:
            DrawSource(VS_Map, R);
            break;
        case VSC_Comms:
            DrawSource(VS_Comms, R);
            break;
        case VSC_Owner:
        {
            // Top half: the paperdoll (front and rear) with the three gauges beside it on the
            // right. Bottom half: the weapon list across the full width.
            const float topH = R.h * 0.50f;
            const float dollW = R.w * 0.70f;
            DrawSource(VS_Owner, {R.x, R.y, dollW - g, topH - g});
            const float gx = R.x + dollW, gw = R.w - dollW;
            const int gauges[3] = {VS_Throttle, VS_JumpJets, VS_Heat}; // throttle, jump jets, heat
            DrawGaugeRow(gauges, 3, {gx, R.y, gw, topH - g}, g);
            DrawSource(VS_Weapons, {R.x, R.y + topH, R.w, R.h - topH});
            break;
        }
        case VSC_Objectives:
        {
            // The primary objective as a banner, the list under it, alerts along the bottom. While a
            // transmission is up (its box or subtitles draw anything) the list gives up room for it.
            const bool box = SourceShowing(VS_Dialogue), sub = SourceShowing(VS_Subtitles);
            const bool dlg = box || sub;
            const float top = R.h * 0.16f, mid = R.h * (dlg ? 0.38f : 0.58f);
            const float alerts = dlg ? R.h * 0.14f : R.h - top - mid;
            DrawSource(VS_PrimaryObjective, {R.x, R.y, R.w, top - g});
            DrawSource(VS_Objectives, {R.x, R.y + top, R.w, mid - g});
            DrawSource(VS_Alerts, {R.x, R.y + top + mid, R.w, alerts - g});
            if (dlg)
            {
                const float y0 = R.y + top + mid + alerts, h = R.y + R.h - y0;
                const float bh = (box && sub) ? h * 0.65f : h;
                if (box)
                    DrawSource(VS_Dialogue, {R.x, y0, R.w, bh - (sub ? g : 0.0f)});
                if (sub)
                    DrawSource(VS_Subtitles, {R.x, box ? y0 + bh : y0, R.w, box ? h - bh : h});
            }
            break;
        }
        default:
            break; // VSC_Tactical is drawn by the 3D branch of Render()
        }
    }

    // =====================================================================================
    // TACOPS - the command page (VSC_TacOps)
    //
    // The 3D map exactly as the 3D page draws it (chase camera, torso slew), with the lance on it.
    // The lance comes from VanillaHud (TacMembers: who stands in which order slot, and where);
    // orders go back through its command ring and are posted by the plugin exactly as the
    // game's F1-F4 keys post them. Everything else - selection, routes, formations, menus - is
    // this app's own, and the window never takes focus from the game while this page is up
    // (WM_MOUSEACTIVATE), so the mouse works here while the keyboard and stick stay in the game.
    //
    //   left click / drag     select lancemates (ring or plate); Shift or Ctrl adds
    //   right click           a menu: on the ground - move / move and hold / add waypoint /
    //                         form on me / cancel route; on an enemy - attack / focus fire /
    //                         move to engage
    //   middle drag           pan the map
    //   bottom buttons        ALL, FORM, HOLD, NEAREST, FIRE
    //   formation panel       how a group move is laid out, and how far apart
    // =====================================================================================
    static constexpr int   kTacMax      = VanillaExportBlock::kTacMaxMembers;
    static constexpr float kTacArriveCm = 3000.0f; // a waypoint is reached within 30 m
    static constexpr float kTacEngageCm = 25000.0f; // "move to engage" stops 250 m short
    enum TacOrderState { TOS_None = 0, TOS_Move, TOS_Attack, TOS_Follow, TOS_Hold };
    enum TacButton { TB_All = 0, TB_Form, TB_Hold, TB_Nearest, TB_Fire, TB_COUNT };
    enum TacWpAct { TWA_None = 0, TWA_Hold, TWA_Attack };
    enum TacFormation { TF_Line = 0, TF_Column, TF_Wedge, TF_Box, TF_COUNT };
    // A function, not a static constexpr array: this target builds as C++14, where indexing
    // such an array at run time needs an out-of-class definition.
    static float TacSpacingM(int i)
    {
        static const float k[3] = {20.0f, 35.0f, 60.0f};
        return k[(i >= 0 && i < 3) ? i : 1];
    }

    struct TacLocal
    {
        bool  Selected = false;
        int   WpCount  = 0;
        float Wp[8][3] = {};
        int   WpAct[8] = {};    // TacWpAct, done on arriving at that waypoint
        int   WpTarget[8] = {}; // contact id for TWA_Attack
        uint32_t WpSeq = 0;     // the command that sent the current leg, to match the plugin's echo
        uint32_t WpEchoed = 0;  // the WpSeq the plugin has been seen echoing (your own route only)
        bool  IsPlayer = false;
        int   MoveMode = 0;     // this route's legs: 0 plain, 1 fire on the move, 2 fall back (face the enemy), 3 retired (was advance / fall back to me), 4 hold position (indirect fire only)
        int   Order    = TOS_None;
        // Fire settings (TCO_FireSettings): what was last sent, shown until the plugin's own
        // report (TacMember::Fire*) has caught up.
        uint8_t FsDisc = 0, FsMask = kFcAll, FsHeat = 0, FsPlugin = 0;
        double  FsSentMs = 0.0;
        bool    FsPending = false; // sent, and the plugin's report does not show it yet
    };
    TacLocal tacLocal[kTacMax + 1];              // by order slot
    VanillaExportBlock::TacMember tacMembers[kTacMax] = {};
    int      tacMemberCount = 0;
    Topo3DView::TacOpsView tacView{};
    bool     tacDragging = false, tacPanning = false, tacDragArmed = false;
    // Dragging your torso arrow's tip: the torso follows (a TCO_TorsoAngle about 5 times a second).
    bool     tacTorsoDrag = false;
    float    tacTorsoX = 0, tacTorsoY = 0;   // the cursor, window pixels
    double   tacTorsoNextMs = 0.0;
    bool     tacTorsoDirty = false;           // moved since the last order
    float    tacDX0 = 0, tacDY0 = 0, tacDX1 = 0, tacDY1 = 0;
    float    tacPanX = 0, tacPanY = 0;
    UINT     tacMouseFlags = 0; // wParam of the mouse message being handled: MK_SHIFT, MK_CONTROL
    std::wstring tacStatus;
    double   tacStatusMs = 0.0;
    D2D1_RECT_F tacBtn[TB_COUNT] = {};
    int      tacFormation = TF_Line;
    int      tacSpacing   = 1;
    D2D1_RECT_F tacFormBtn[TF_COUNT] = {}, tacSpaceBtn = {};

    // The right-click menu.
    enum TacMenuItem { TMI_Move = 0, TMI_MoveHold, TMI_Waypoint, TMI_Form, TMI_Cancel, TMI_Attack, TMI_Focus, TMI_Engage,
                       TMI_MoveFire, TMI_Fallback, TMI_IndirectCover, // TMI_AdvanceFb retired 2026-10-13 (user)
                       // The player's own torso - no selection needed.
                       TMI_TorsoAngle, TMI_TorsoPoint, TMI_TorsoTarget, TMI_TorsoCentre, TMI_TorsoRelease,
                       // Fire settings, from right-clicking a lancemate (16 items: the menu's maximum).
                       TMI_FsFree, TMI_FsReturn, TMI_FsHold, TMI_FsNormal, TMI_FsCool, TMI_FsHot, TMI_FsAlpha,
                       TMI_FsCat0, TMI_FsCat1, TMI_FsCat2, TMI_FsCat3, TMI_FsCat4, TMI_FsCat5, TMI_FsCat6, TMI_FsCat7,
                       TMI_FsReset,
                       // NAV POINTS (2026-10-07k): mark a nav point of your own here / remove yours under the click
                       TMI_NavMark, TMI_NavUnmark,
                       // 2026-10-12: on an enemy - your lock on it (only when the lance detects it), and the called-shot
                       // submenu; TMI_Cs0 + part = TCO_CalledShot's part (0 cancel, 1 legs ... 11 head)
                       TMI_Lock, TMI_CalledShotMenu, TMI_Cs0, TMI_CsLast = TMI_Cs0 + 11,
                       // 2026-10-20 (lancecontrol-75): flank it (each selected lancemate, the nearer side) / a pincer on it
                       // (the selected lancemates: your left, your right, behind it)
                       TMI_Flank, TMI_Pincer };
    static bool TacFlItem(int i) { return i == TMI_Flank || i == TMI_Pincer; }
    static bool TacCsItem(int i) { return i >= TMI_Cs0 && i <= TMI_CsLast; }
    static bool TacTorsoItem(int i) { return i >= TMI_TorsoAngle && i <= TMI_TorsoRelease; }
    static bool TacFsItem(int i) { return i >= TMI_FsFree && i <= TMI_FsReset; }
    int   tacFsSlots[8] = {};   // the lancemates the open fire-settings menu applies to
    int   tacFsCount = 0;
    bool  tacMenuOpen = false;
    int   tacMenuItems[16] = {};
    int   tacMenuCount = 0;
    float tacMenuX = 0, tacMenuY = 0;
    float tacMenuLoc[3] = {};
    int   tacMenuTarget = 0;
    int   tacMenuNav = -1;      // NAV POINTS: the letter of your own nav point under the right click, -1 none
    float tacMenuNavLoc[3] = {};
    D2D1_RECT_F tacMenuRect[16] = {};
    bool  tacMenuDisabled[16] = {}; // greyed out: drawn, not selectable (2026-10-12: lock on an undetected enemy)
    uint32_t tacMenuActorLo = 0, tacMenuActorHi = 0; // the enemy under the right click, from the lance status feed
    int   tacMenuSpotter = 0;   // a lancemate that sees it (the patch-through's "from"), else 0

    // 2026-10-12: THE LANCE STATUS FEED (MW5_LanceStatus, LanceControl), read only - which enemies the lance detects
    // (a lancemate's line of sight, a lancemate's or your sensors). Opened on demand.
    HANDLE lsMmf = nullptr;
    const LanceStatusBlock *lsBlock = nullptr;
    ULONGLONG lsLastAttempt = 0;
    bool LsFeedOpen()
    {
        if (lsBlock && lsBlock->Version == LANCE_VERSION)
            return true;
        if (lsBlock)
        {
            UnmapViewOfFile(lsBlock);
            lsBlock = nullptr;
            CloseHandle(lsMmf);
            lsMmf = nullptr;
        }
        const ULONGLONG now = GetTickCount64();
        if (now - lsLastAttempt < 1000)
            return false;
        lsLastAttempt = now;
        lsMmf = OpenFileMappingW(FILE_MAP_READ, FALSE, LANCE_MMF_NAME);
        if (!lsMmf)
            return false;
        lsBlock = static_cast<const LanceStatusBlock *>(MapViewOfFile(lsMmf, FILE_MAP_READ, 0, 0, sizeof(LanceStatusBlock)));
        if (!lsBlock || lsBlock->Version != LANCE_VERSION)
        {
            if (lsBlock)
                UnmapViewOfFile(lsBlock);
            lsBlock = nullptr;
            CloseHandle(lsMmf);
            lsMmf = nullptr;
            return false;
        }
        return true;
    }
    // =====================================================================================
    // V-LNC - THE LANCE PAGE (2026-10-19, user): the three lancemates as TacOps plates at page size,
    // plus their ammo and what each one is doing now. Orders count whoever gave them - TacOps, voice
    // (VoiceCommand) or the plugin itself: the lance status feed reports the unit's state (Mode bits,
    // target) and every order it was given (LSE_OrderReceived), not who sent it.
    // =====================================================================================
    void SelectLance()
    {
        EndPageInteractions(); // a drag or menu on the old page must not carry over
        if (!d2dAvailable)
        {
            Log("Lance page unavailable (Direct2D not initialized)");
            return;
        }
        LeaveLidar();
        LeaveTopo();
        LeaveTopo3D();
        isScreenCaptureMode = false;
        isVanillaMode = false;
        isLanceMode = true;
        pendingWidgetType = 0xFF;
        Log("Selected: lance page (V-LNC)");
    }

    Topo3DView::LancePageView lancePage{};
    LsSlot     lsSlotsCopy[kLsMaxSlots] = {};
    LsAmmoList lsAmmoCopy[kLsMaxSlots] = {};
    LsPilot    lsPilotsCopy[kLsMaxSlots] = {};
    uint32_t   lsSlotCopyCount = 0;
    uint32_t   lsEvSeen = 0; // events read so far (EventHead)
    struct LsOrderSeen
    {
        float    Value = -1.0f; // TacOrder; -1 none seen
        uint32_t TimeMs = 0;
        uint32_t ActorLo = 0, ActorHi = 0; // the unit it was for (a new mission / mech forgets it)
        uint32_t SinceMs = 0; // when this unit was first seen in the slot: older voice orders are not its own
    };
    LsOrderSeen lsLastOrder[kLsMaxSlots + 1];

    // MW5_VoiceIntent (VoiceCommand, 2026-10-19): opened on demand, read under its seqlock.
    HANDLE viMmf = nullptr;
    const VoiceIntentBlock *viBlock = nullptr;
    ULONGLONG viLastAttempt = 0;
    VoiceIntentBlock viCopy{};
    bool ViRead()
    {
        if (viBlock && viBlock->Version != VOICE_INTENT_VERSION)
        {
            UnmapViewOfFile(viBlock);
            viBlock = nullptr;
            CloseHandle(viMmf);
            viMmf = nullptr;
        }
        if (!viBlock)
        {
            const ULONGLONG now = GetTickCount64();
            if (now - viLastAttempt < 1000)
                return false;
            viLastAttempt = now;
            viMmf = OpenFileMappingW(FILE_MAP_READ, FALSE, VOICE_INTENT_MMF_NAME);
            if (!viMmf)
                return false;
            viBlock = static_cast<const VoiceIntentBlock *>(MapViewOfFile(viMmf, FILE_MAP_READ, 0, 0, sizeof(VoiceIntentBlock)));
            if (!viBlock || viBlock->Version != VOICE_INTENT_VERSION)
            {
                if (viBlock)
                    UnmapViewOfFile(viBlock);
                viBlock = nullptr;
                CloseHandle(viMmf);
                viMmf = nullptr;
                return false;
            }
        }
        if (GetTickCount64() - viBlock->TickMs > 3000)
            return false; // VoiceCommand closed (the mapping can outlive it while we hold it)
        for (int attempt = 0; attempt < 4; ++attempt)
        {
            const uint32_t s1 = viBlock->Seq;
            if (s1 & 1u)
                continue;
            MemoryBarrier();
            std::memcpy(&viCopy, (const void *)viBlock, sizeof(viCopy));
            MemoryBarrier();
            if (viBlock->Seq == s1)
                return true;
        }
        return false;
    }

    // Slots / ammo / pilots under the feed's StatusSeq.
    bool LsReadSlots()
    {
        lsSlotCopyCount = 0;
        if (!LsFeedOpen() || GetTickCount64() - lsBlock->TickMs > 3000)
            return false;
        for (int attempt = 0; attempt < 4; ++attempt)
        {
            const uint32_t s1 = lsBlock->StatusSeq;
            if (s1 & 1u)
                continue;
            MemoryBarrier();
            std::memcpy(lsSlotsCopy, (const void *)lsBlock->Slots, sizeof(lsSlotsCopy));
            std::memcpy(lsAmmoCopy, (const void *)lsBlock->Ammo, sizeof(lsAmmoCopy));
            std::memcpy(lsPilotsCopy, (const void *)lsBlock->Pilots, sizeof(lsPilotsCopy));
            const uint32_t n = lsBlock->SlotCount;
            MemoryBarrier();
            if (lsBlock->StatusSeq == s1)
            {
                lsSlotCopyCount = n < (uint32_t)kLsMaxSlots ? n : (uint32_t)kLsMaxSlots;
                return true;
            }
        }
        return false;
    }

    // The last order each slot was given, from the event ring.
    void LsReadOrders()
    {
        if (!lsBlock)
            return;
        const uint32_t head = lsBlock->EventHead;
        if (head < lsEvSeen)
            lsEvSeen = 0; // the plugin started again
        uint32_t from = head > (uint32_t)kLsEventRing ? head - (uint32_t)kLsEventRing : 0;
        if (lsEvSeen > from)
            from = lsEvSeen;
        for (uint32_t n = from; n < head; ++n)
        {
            LsEvent e;
            std::memcpy(&e, (const void *)&lsBlock->Events[n % kLsEventRing], sizeof(e));
            if (e.Seq != n + 1)
                continue; // overwritten or still being written
            if (e.Type == LSE_OrderReceived && e.Slot >= 1 && e.Slot <= kLsMaxSlots)
            {
                lsLastOrder[e.Slot].Value = e.Value;
                lsLastOrder[e.Slot].TimeMs = e.TimeMs;
            }
        }
        lsEvSeen = head;
    }

    // "Ammo.Ballistic.AutoCannon.Standard.AC10" -> "AC10", "Ammo.Missile.LRM.Clan" -> "LRM-C".
    static void AmmoShortName(const char *tag, wchar_t *out, size_t n)
    {
        std::vector<std::string> parts;
        std::string cur;
        for (const char *p = tag; *p; ++p)
        {
            if (*p == '.')
            {
                if (!cur.empty())
                    parts.push_back(cur);
                cur.clear();
            }
            else
                cur += *p;
        }
        if (!cur.empty())
            parts.push_back(cur);
        std::string name = parts.empty() ? std::string("?") : parts.back();
        bool clan = false;
        while (parts.size() >= 2 && (name == "Clan" || name == "IS" || name == "Standard" || name == "Normal" ||
                                     name == "InnerSphere" || name == "Basic"))
        {
            clan = clan || name == "Clan";
            parts.pop_back();
            name = parts.back();
        }
        if (clan)
            name += "-C";
        swprintf_s(out, n, L"%hs", name.c_str());
    }

    // 2026-10-19 ONE ORDER WORDING for V-LNC (Long) and the TacOps plates (Short): what the lancemate is actually
    // doing, from the lance status feed (Mode bits), its last order event (TacOps and voice alike) and its move
    // point. "{T}" = its target's name (filled in by the renderer). Moved here from BuildLancePage unchanged, except
    // the nav point: NavName already says "NAV ALPHA" - the old " - NAV %ls" read "NAV NAV ALPHA".
    struct OrderWording
    {
        wchar_t  Long[96];
        wchar_t  Short[48];
        uint32_t Col;
        bool     PointMatters, NamesTarget;
        float    DistPt; // m to its move point, -1 = none
    };
    void OrderWords(const VanillaExportBlock::TacMember &m, const LsSlot &sl, const LsOrderSeen &lo, OrderWording &w)
    {
        w = OrderWording{};
        w.DistPt = -1.0f;
        // Where it is going (the last move the plugin sent it, snapped onto walkable ground).
        if (m.LastMoveSeq)
        {
            const float dx = (m.LastMove[0] - m.Loc[0]) * 0.01f, dy = (m.LastMove[1] - m.Loc[1]) * 0.01f;
            w.DistPt = std::sqrt(dx * dx + dy * dy);
        }
        const bool hasT = sl.TargetLo || sl.TargetHi;
        const uint16_t md = sl.Mode;
        const int ord = lo.Value >= 0.0f ? (int)(lo.Value + 0.5f) : -1;
        // (Advance / fall back to me was retired 2026-10-13 - lancecontrol-54 - so it is not shown.)
        const bool movingOrder = ord == VanillaExportBlock::TCO_Move || ord == VanillaExportBlock::TCO_MoveFire ||
                                 ord == VanillaExportBlock::TCO_HoldHere || ord == VanillaExportBlock::TCO_Fallback ||
                                 ord == VanillaExportBlock::TCO_IndirectCover;
        const bool farAway = w.DistPt > 25.0f;
        auto set = [&](const wchar_t *lng, const wchar_t *sht, uint32_t col, bool pm, bool nt) {
            swprintf_s(w.Long, L"%ls", lng);
            swprintf_s(w.Short, L"%ls", sht);
            w.Col = col;
            w.PointMatters = pm;
            w.NamesTarget = nt;
        };
        if (md & LSM_Flank) // 2026-10-20 (lancecontrol-75): swinging round its attack order's unit, firing
            set((md & LSM_Pincer) ? L"PINCER - FLANKING {T}" : L"FLANKING {T}", (md & LSM_Pincer) ? L"PINCER {T}" : L"FLANKING {T}",
                0xFF7A5A, false, true);
        else if (md & LSM_IndirectHold)
            set(farAway ? L"MOVING - INDIRECT FIRE, THEN HOLD" : L"HOLD POSITION - INDIRECT FIRE ONLY",
                farAway ? L"TO HOLD - INDIRECT" : L"HOLD - INDIRECT", 0xFFC04D, true, false);
        else if (md & LSM_Hold) // lancecontrol-10: a hold at a point walks there fire-on-the-move (FomOn), then holds
            set(farAway ? L"MOVING - FIRE ON THE MOVE, THEN HOLD" : L"HOLDING THERE",
                farAway ? L"TO HOLD - FIRING" : L"HOLDING", 0xFFC04D, true, false);
        else if (md & LSM_FallBack)
            set(L"FALLING BACK - FACING THE ENEMY", L"FALLING BACK", 0xFFC04D, true, false);
        else if (md & LSM_AttackOrder)
            set(L"ATTACKING {T}", L"ATTACKING {T}", 0xFF7A5A, false, true);
        else if ((md & LSM_MoveFire) && (farAway || w.DistPt < 0.0f))
            // 2026-10-19 (user): the plugin keeps the fire-on-the-move flag after arrival - within 25 m of its point it
            // falls through to ENGAGING {T} / ARRIVED below.
            set(L"MOVING - FIRE ON THE MOVE", L"MOVING - FIRING", 0x3FB8FF, true, false);
        else if (movingOrder && farAway)
        {
            // A plain move is the BEELINE (TCO_Move: straight there, no fighting on the way);
            // "move" / "advance" by voice and TacOps' fire-on-the-move send TCO_MoveFire (above).
            if (ord == VanillaExportBlock::TCO_MoveFire)
                set(L"MOVING - FIRE ON THE MOVE", L"MOVING - FIRING", 0x3FB8FF, true, false);
            else
                set(L"BEELINE", L"BEELINE", 0x3FB8FF, true, false);
        }
        else if (ord == VanillaExportBlock::TCO_Follow)
            set(L"FOLLOWING YOU", L"FOLLOWING YOU", 0x5DFFA0, false, false);
        else if (hasT) // no order of ours running: the game's own AI picked this fight
            set(L"ENGAGING {T} (OWN CHOICE)", L"ENGAGING {T}", 0xFF7A5A, false, true);
        else
            set(movingOrder ? L"ARRIVED - NO ENEMY" : L"NO ORDER - NO ENEMY", movingOrder ? L"ARRIVED" : L"NO ORDER",
                0xD8E2E8, false, false);

        // Its point is a nav point (TacOps or voice alike): name it.
        if (w.PointMatters && m.LastMoveSeq)
            for (int k = 0; k < navCount; ++k)
            {
                const float nx = (navRaw[k].Loc[0] - m.LastMove[0]) * 0.01f, ny = (navRaw[k].Loc[1] - m.LastMove[1]) * 0.01f;
                if (nx * nx + ny * ny < 60.0f * 60.0f)
                {
                    wchar_t nn[24];
                    NavName(navRaw[k].Letter, nn, 24); // "NAV ALPHA"
                    size_t used = wcslen(w.Long);
                    swprintf_s(w.Long + used, 96 - used, L" - %ls", nn);
                    used = wcslen(w.Short);
                    swprintf_s(w.Short + used, 48 - used, L" - %ls", nn);
                    break;
                }
            }
    }

    void BuildLancePage()
    {
        PollVanillaMMF();
        // false: no TacWantMs - LanceControl's 4-a-second lance table is plenty for this page, and asking
        // would make it publish every game tick (the TacOps page's rate) - zero extra game-thread cost.
        TacReadMembers(false);
        const bool feed = LsReadSlots();
        if (feed)
            LsReadOrders();
        const bool voice = ViRead();
        lancePage.Count = 0;
        const uint32_t nowMs = (uint32_t)GetTickCount64();
        for (int i = 0; i < tacMemberCount && lancePage.Count < 3; ++i)
        {
            const auto &m = tacMembers[i];
            if (m.IsPlayer)
                continue;
            Topo3DView::LanceCard &c = lancePage.Cards[lancePage.Count++];
            c = Topo3DView::LanceCard{};
            c.Slot = m.Slot;
            c.Dead = m.Dead;
            c.ActorLo = m.ActorLo;
            c.ActorHi = m.ActorHi;
            c.FsDisc = m.FireDisc;
            c.FsMask = m.FireMask;
            c.FsHeat = m.FireHeat;
            c.FsPlugin = m.FirePlugin;
            c.Heat = -1.0f;
            c.OrderCol = 0xD8E2E8;
            if (m.Slot < 1 || m.Slot > kLsMaxSlots)
                continue;
            LsOrderSeen &lo = lsLastOrder[m.Slot];
            if (lo.ActorLo != m.ActorLo || lo.ActorHi != m.ActorHi)
            {
                lo = LsOrderSeen{}; // a new unit in this slot: its predecessor's orders are not its own
                lo.ActorLo = m.ActorLo;
                lo.ActorHi = m.ActorHi;
                lo.SinceMs = nowMs;
            }
            // VoiceCommand: its question, its voice, and the order it gave in words (if still the latest).
            std::wstring told;
            uint32_t toldMs = 0;
            if (voice && m.Slot < kViSlots)
            {
                const ViSlot &vs = viCopy.Slots[m.Slot];
                c.HaveVoice = 1;
                c.Speaking = vs.Speaking;
                c.Queued = vs.Queued;
                for (int k = 0; k < kViWave && k < 144; ++k)
                {
                    c.WaveLo[k] = vs.WaveLo[(vs.WaveHead + k) % kViWave]; // oldest first
                    c.WaveHi[k] = vs.WaveHi[(vs.WaveHead + k) % kViWave];
                }
                if (vs.Pending && vs.PendingText[0])
                {
                    char pt[97];
                    std::memcpy(pt, vs.PendingText, 96);
                    pt[96] = 0;
                    MultiByteToWideChar(CP_UTF8, 0, pt, -1, c.Pending, 96);
                    c.Pending[95] = 0;
                }
                // The voice order is the current one unless a later order came from elsewhere (TacOps):
                // the plugin's own record of the slot's last order is then more than 2 s newer.
                const bool fresh = vs.IntentMs && (int32_t)(vs.IntentMs - lo.SinceMs) >= 0;
                const bool superseded = lo.Value >= 0.0f && (int32_t)(lo.TimeMs - vs.IntentMs) > 2000;
                if (fresh && !superseded && vs.Intent[0])
                {
                    char it[97];
                    std::memcpy(it, vs.Intent, 96);
                    it[96] = 0;
                    wchar_t w[96];
                    MultiByteToWideChar(CP_UTF8, 0, it, -1, w, 96);
                    w[95] = 0;
                    told = w;
                    toldMs = vs.IntentMs;
                }
            }
            const LsSlot *sl = nullptr;
            int si = -1;
            for (uint32_t k = 0; k < lsSlotCopyCount; ++k)
                if (lsSlotsCopy[k].Valid && lsSlotsCopy[k].Slot == m.Slot)
                {
                    sl = &lsSlotsCopy[k];
                    si = (int)k;
                    break;
                }
            if (feed)
                for (int k = 0; k < kLsMaxSlots; ++k)
                    if (lsPilotsCopy[k].Valid && lsPilotsCopy[k].Slot == m.Slot && lsPilotsCopy[k].Name[0])
                    {
                        char nm[37];
                        std::memcpy(nm, lsPilotsCopy[k].Name, 36);
                        nm[36] = 0;
                        MultiByteToWideChar(CP_UTF8, 0, nm, -1, c.Name, 40);
                        c.Name[39] = 0;
                        break;
                    }
            if (!sl)
            {
                swprintf_s(c.Order, L"NO STATUS FEED");
                c.OrderCol = 0x8FB8C9;
                continue;
            }
            c.HaveFeed = 1;
            c.Heat = sl->Heat;
            c.TargetLo = sl->TargetLo;
            c.TargetHi = sl->TargetHi;
            if (!sl->Alive)
                c.Dead = 1;
            const LsAmmoList &al = lsAmmoCopy[si];
            for (int k = 0; k < al.Count && k < kLsAmmoTypes && c.AmmoCount < 12; ++k)
            {
                char tg[57];
                std::memcpy(tg, al.Bins[k].Tag, 56);
                tg[56] = 0;
                Topo3DView::LanceAmmo &am = c.Ammo[c.AmmoCount++];
                AmmoShortName(tg, am.Name, 24);
                am.Count = al.Bins[k].Count;
                am.Max = al.Bins[k].Max;
            }

            // What it is doing - the same wording the TacOps plates use (OrderWords).
            OrderWording ow;
            OrderWords(m, *sl, lo, ow);
            swprintf_s(c.Order, L"%ls", ow.Long);
            c.OrderCol = ow.Col;
            const bool hasT = sl->TargetLo || sl->TargetHi;
            const bool pointMatters = ow.PointMatters, namesTarget = ow.NamesTarget;
            const float dPt = ow.DistPt;

            // The detail line: the point, the target, how long ago the order came.
            std::wstring d;
            auto add = [&](const std::wstring &x) { d += (d.empty() ? L"" : L"  \x00B7  ") + x; };
            wchar_t b[96];
            if (!told.empty())
                add(L"told: " + told);
            if (pointMatters && dPt >= 0.0f)
            {
                if (dPt > 25.0f)
                    swprintf_s(b, L"%.0f m to its point", dPt);
                else
                    swprintf_s(b, L"at its point");
                add(b);
            }
            if (hasT)
            {
                const wchar_t *sight = sl->TargetVisible ? L"in sight" : L"not in sight";
                if (sl->TargetDistM >= 0.0f)
                    swprintf_s(b, namesTarget ? L"%.0f m, %ls" : L"engaging {T} %.0f m, %ls", sl->TargetDistM, sight);
                else
                    swprintf_s(b, namesTarget ? L"%ls" : L"engaging {T}, %ls", sight);
                add(b);
            }
            if ((lo.Value >= 0.0f && lo.TimeMs) || toldMs)
            {
                const uint32_t age = (nowMs - (toldMs ? toldMs : lo.TimeMs)) / 1000u;
                if (age < 120u)
                    swprintf_s(b, L"ordered %u s ago", age);
                else
                    swprintf_s(b, L"ordered %u min ago", age / 60u);
                add(b);
            }
            if (sl->SeenBy > 0)
            {
                swprintf_s(b, L"seen by %u", (unsigned)sl->SeenBy);
                add(b);
            }
            swprintf_s(c.Order2, L"%ls", d.c_str());
        }
    }

    // The live enemy nearest loc (world cm, within 40 m - LanceControl's own reach): 1 = detected, 0 = not,
    // -1 = no feed / not found. lo / hi = its address (for the exact-unit lock), spotter = a lancemate seeing it.
    int TacDetected(const float loc[3], uint32_t &lo, uint32_t &hi, int &spotter)
    {
        lo = hi = 0;
        spotter = 0;
        if (!LsFeedOpen() || GetTickCount64() - lsBlock->TickMs > 3000)
            return -1;
        LsUnit u[kLsMaxUnits];
        uint32_t n = 0;
        bool got = false;
        for (int tries = 0; tries < 8 && !got; ++tries)
        {
            const uint32_t a = lsBlock->UnitSeq;
            if (a & 1)
                continue;
            MemoryBarrier();
            n = lsBlock->UnitCount;
            if (n > (uint32_t)kLsMaxUnits)
                n = kLsMaxUnits;
            std::memcpy(u, (const void *)lsBlock->Units, sizeof(LsUnit) * n);
            MemoryBarrier();
            got = lsBlock->UnitSeq == a;
        }
        if (!got)
            return -1;
        int best = -1;
        float bestD = 4000.0f * 4000.0f;
        for (uint32_t i = 0; i < n; ++i)
        {
            if (u[i].Dead)
                continue;
            const float dx = u[i].Loc[0] - loc[0], dy = u[i].Loc[1] - loc[1], dz = u[i].Loc[2] - loc[2];
            const float d = dx * dx + dy * dy + dz * dz;
            if (d < bestD)
            {
                bestD = d;
                best = (int)i;
            }
        }
        if (best < 0)
            return -1;
        lo = u[best].ActorLo;
        hi = u[best].ActorHi;
        for (int k = 0; k < 8; ++k)
            if (u[best].SeenBy & (1u << k))
            {
                spotter = k + 1;
                break;
            }
        return (u[best].SeenBy || (u[best].Flags & (LSU_OnLanceRadar | LSU_OnPlayerRadar))) ? 1 : 0;
    }

    // THE EXACT UNIT OF A MAP CONTACT (2026-10-16, lancecontrol-61): attacks and called shots from the map name the unit
    // itself - Loc = its address, Loc[2] = kTacLocActor - not a point LanceControl resolves to the nearest pawn within
    // 40 m (a turret beside a mech could win). A mech: its LidarExport record's address; anything else: the hostile in
    // the lance status feed nearest the contact's own position. Neither known: the point, as before.
    const float *TacTargetLoc(int contactId, const float at[3], float buf[3])
    {
        uint32_t lo = 0, hi = 0;
        int spotter = 0;
        if (!contactId || !topo3d.TacContactActor(contactId, lo, hi))
            TacDetected(at, lo, hi, spotter);
        if (!lo && !hi)
            return at;
        std::memcpy(&buf[0], &lo, 4);
        std::memcpy(&buf[1], &hi, 4);
        buf[2] = VanillaExportBlock::kTacLocActor;
        return buf;
    }

    // RADAR MODE (2026-10-12, gs::RadarOpts): Vanilla shows only the hostiles the game's own detection has - the
    // same test as the lock gate above (a lancemate's line of sight, a lancemate's or your radar incl. team relay).
    // A copy of LanceControl's Units[] is taken at most every 200 ms; the LIDAR scene and the 3D skeletons ask
    // RadarKeep per hostile. FAILS OPEN: no feed, a stale feed (LanceControl not running) or no unit within 40 m
    // of the contact (a list that is full, a unit the feed does not carry) -> shown, as in Full LIDAR.
    gs::RadarOpts radarOpts;
    bool lidarFar = false; // the 5 km radar mode is applied to the scope (2026-10-15)
    bool lidarFarApplied = false; // bug sweep 19: the first frame applies it too (clamps a saved >1000 m range)
    // ALARM (2026-10-13, user): the missile alarm's on/off and volume live in GlobalSettings.ini [hud] alarm_on /
    // alarm_vol for every window; the LIDAR layer bit and lidarAlarmVol just follow it (ApplyAlarmOpts).
    gs::AlarmOpts alarmOpts;
    gs::LidarOpts lidarOpts; // 2026-10-13: the LIDAR layers + smoothing too (GlobalSettings > HUD > LIDAR)
    // 2026-10-17 (user): V-POD's and CAP's centre crosshairs can be switched off (GlobalSettings > HUD > VISION,
    // [hud] vpod_crosshair / cap_crosshair, default on). Read with the alarm options (start + whenever the ini changes).
    bool vpodCross = true, capCross = true;
    void ApplyAlarmOpts()
    {
        vpodCross = gs::Int(L"hud", L"vpod_crosshair", 1) != 0;
        capCross = gs::Int(L"hud", L"cap_crosshair", 1) != 0;
        alarmOpts.Load();
        lidarOpts.Load();
        lidarLayers = lidarOpts.Layers;
        if (alarmOpts.On)
            lidarLayers |= LidarView::AlarmLayerMask();
        else
            lidarLayers &= ~LidarView::AlarmLayerMask();
        lidarProjSmooth = lidarOpts.Smooth;
        lidar.SetProjectileSmoothing(lidarProjSmooth);
        lidarAlarmVol = alarmOpts.Vol;
        lidar.SetLayers(lidarLayers);
        lidar.SetAlarmVolume(lidarAlarmVol);
        lidar.SetAlarmSound(alarmOpts.Sound);
    }
    int radarModeLogged = -1;
    LsUnit radarUnits[kLsMaxUnits];
    uint32_t radarUnitCount = 0;
    bool radarFeedOk = false;
    ULONGLONG radarSnapMs = 0;
    // test log (2026-10-12): hostile contacts the filter kept / hid in the last frame drawn, every 10 s in Vanilla
    int radarKeptF = 0, radarHidF = 0, radarKeptLast = 0, radarHidLast = 0;
    ULONGLONG radarLogMs = 0;
    void RadarRefresh()
    {
        if (radarOpts.Mode != radarModeLogged)
        {
            radarModeLogged = radarOpts.Mode;
            Log(std::string("Radar mode: ") + (radarOpts.Mode == 2 ? "Vanilla, 5 km (the game's detection, far ranges)"
                                                : radarOpts.Mode == 1 ? "Vanilla (the game's detection)" : "Full LIDAR"));
        }
        if (!radarOpts.Vanilla())
            return;
        const ULONGLONG now = GetTickCount64();
        if (now - radarLogMs >= 10000)
        {
            radarLogMs = now;
            int det = 0;
            for (uint32_t i = 0; i < radarUnitCount; ++i)
                det += !radarUnits[i].Dead && (radarUnits[i].SeenBy || (radarUnits[i].Flags & (LSU_OnLanceRadar | LSU_OnPlayerRadar)));
            Log("radar (vanilla): feed " + std::string(radarFeedOk ? "ok" : "MISSING - showing all") + ", " +
                std::to_string(radarUnitCount) + " hostiles in the feed, " + std::to_string(det) + " detected; last frame: " +
                std::to_string(radarKeptLast) + " contact checks kept, " + std::to_string(radarHidLast) + " hidden");
        }
        if (now - radarSnapMs < 200)
            return;
        radarSnapMs = now;
        radarFeedOk = false;
        if (!LsFeedOpen() || now - lsBlock->TickMs > 3000)
            return;
        for (int tries = 0; tries < 8; ++tries)
        {
            const uint32_t a = lsBlock->UnitSeq;
            if (a & 1)
                continue;
            MemoryBarrier();
            uint32_t n = lsBlock->UnitCount;
            if (n > (uint32_t)kLsMaxUnits)
                n = kLsMaxUnits;
            std::memcpy(radarUnits, (const void *)lsBlock->Units, sizeof(LsUnit) * n);
            MemoryBarrier();
            if (lsBlock->UnitSeq == a)
            {
                radarUnitCount = n;
                radarFeedOk = true;
                return;
            }
        }
    }
    static bool RadarKeep(void *ctx, float x, float y, float z)
    {
        HUDDisplay *self = static_cast<HUDDisplay *>(ctx);
        const bool keep = self->RadarDecide(x, y, z);
        ++(keep ? self->radarKeptF : self->radarHidF);
        return keep;
    }
    bool RadarDecide(float x, float y, float z) const
    {
        const HUDDisplay *self = this;
        if (!self->radarFeedOk)
            return true;
        int best = -1;
        float bestD = 4000.0f * 4000.0f;
        for (uint32_t i = 0; i < self->radarUnitCount; ++i)
        {
            const LsUnit &u = self->radarUnits[i];
            if (u.Dead)
                continue;
            const float dx = u.Loc[0] - x, dy = u.Loc[1] - y, dz = u.Loc[2] - z;
            const float d = dx * dx + dy * dy + dz * dz;
            if (d < bestD)
            {
                bestD = d;
                best = (int)i;
            }
        }
        if (best < 0)
            return true;
        const LsUnit &u = self->radarUnits[best];
        return u.SeenBy || (u.Flags & (LSU_OnLanceRadar | LSU_OnPlayerRadar));
    }
    static const wchar_t *TacCsText(int part)
    {
        static const wchar_t *k[] = {L"Cancel called shot", L"Called shot: legs", L"Called shot: left leg",
                                     L"Called shot: right leg", L"Called shot: arms", L"Called shot: left arm",
                                     L"Called shot: right arm", L"Called shot: side torsos", L"Called shot: left torso",
                                     L"Called shot: right torso", L"Called shot: centre torso", L"Called shot: head"};
        return (part >= 0 && part <= 11) ? k[part] : L"";
    }

    // NAV POINTS (2026-10-07k, VHUD v19): LanceControl's list, read every frame, handed to the LIDAR scope and the
    // 3D map (the 3D page and TacOps).
    VanillaExportBlock::NavPoint navRaw[VanillaExportBlock::kNavMax] = {};
    int navCount = 0;

    static void NavName(int letter, wchar_t *out, size_t cap)
    {
        std::wstring n = std::wstring(L"NAV ") + VhudNavName(letter);
        for (auto &ch : n)
            ch = (wchar_t)towupper(ch);
        wcsncpy_s(out, cap, n.c_str(), _TRUNCATE);
    }

    void NavPoll()
    {
        PollVanillaMMF();
        int n = 0;
        if (vBlock)
            for (int attempt = 0; attempt < 3; ++attempt)
            {
                const uint32_t s1 = vBlock->NavSeq;
                if (s1 & 1u)
                    continue;
                MemoryBarrier();
                n = (int)(std::min)(vBlock->NavCount, (uint32_t)VanillaExportBlock::kNavMax);
                std::memcpy(navRaw, (const void *)vBlock->Navs, sizeof(VanillaExportBlock::NavPoint) * (size_t)n);
                MemoryBarrier();
                if (vBlock->NavSeq == s1)
                    break;
                n = 0;
            }
        navCount = n;
        Topo3DView::NavMark m[VanillaExportBlock::kNavMax];
        LidarView::NavPoint l[VanillaExportBlock::kNavMax];
        for (int i = 0; i < n; ++i)
        {
            std::memcpy(m[i].Loc, navRaw[i].Loc, sizeof(m[i].Loc));
            m[i].Mine = navRaw[i].Mine;
            NavName(navRaw[i].Letter, m[i].Name, 16);
            l[i].X = navRaw[i].Loc[0];
            l[i].Y = navRaw[i].Loc[1];
            l[i].Mine = navRaw[i].Mine;
            NavName(navRaw[i].Letter, l[i].Name, 16);
        }
        topo3d.SetNavPoints(m, n);
        lidar.SetNavPoints(l, n);
        // DROPSHIPS (2026-10-08, TERRAIN v14): the 3D map reads them itself; the LIDAR scope gets them from it
        Topo3DView::DropshipMark dm[8];
        LidarView::Dropship ld[8];
        const int dn = topo3d.GetDropships(dm, 8);
        for (int i = 0; i < dn; ++i)
            ld[i] = LidarView::Dropship{dm[i].X, dm[i].Y, dm[i].Yaw, dm[i].EX, dm[i].EY, dm[i].Kind, dm[i].Side};
        lidar.SetDropships(ld, dn);
    }

    bool IsTacOps() const { return isVanillaMode && vanillaScreen == VSC_TacOps; }

    // =====================================================================================
    // BORDERLESS. F11 (while this window has focus) drops the title bar and frame, and puts
    // them back. The picture stays exactly where it is: the window is re-sized around the same
    // client area. Kept in the config (borderless=, and borderless_key= to use another key).
    // =====================================================================================
    bool borderless = false;
    // Insane allies: the VanillaHud plugin replaces the lancemates' AI firing and fires every ready
    // weapon (kVhudOptInsaneAllies, time-stamped into the plugin's block every frame while on; it
    // lapses 2 s after no instance writes it).
    int  allyMode = 0; // [lance] insane_allies: 0 vanilla, 1 insane, 2 impossibly strong
    // Mute game lancemate voices (2026-10-07, lancecontrol-17): LanceControl stops the game's own
    // lancemate voice lines (mission dialogue untouched) unless an instance with this OFF writes
    // kVhudOptGameVoices - so it is muted by default, and with no HUDDisplay running.
    bool muteGameVoices = true;
    // 2026-10-07f: both are GlobalSettings.ini's [lance] now (shared by every window, VoiceCommand and
    // GlobalSettings.exe): read when the file changes, written the moment a box is ticked, and passed to
    // the plugin every frame - every relay reads the same file, so windows can no longer disagree.
    gs::Watch     gsWatch{gs::Path()};
    gs::LanceOpts lanceOpts;
    UINT borderlessKey = VK_F11;

    static DWORD WindowStyleFor(bool noBorder) { return noBorder ? WS_POPUP : WS_OVERLAPPEDWINDOW; }

    void ToggleBorderless()
    {
        SetBorderless(!borderless);
        SaveConfig();
        Log(std::string("Borderless ") + (borderless ? "on" : "off"));
    }

    void SetBorderless(bool on)
    {
        RECT cr;
        if (!GetClientRect(hwnd, &cr))
            return;
        POINT tl{cr.left, cr.top};
        ClientToScreen(hwnd, &tl);
        borderless = on;
        const DWORD style = WindowStyleFor(borderless);
        RECT r{tl.x, tl.y, tl.x + (cr.right - cr.left), tl.y + (cr.bottom - cr.top)};
        AdjustWindowRectEx(&r, style, FALSE, (DWORD)GetWindowLongPtr(hwnd, GWL_EXSTYLE));
        SetWindowLongPtr(hwnd, GWL_STYLE, (LONG_PTR)(style | WS_VISIBLE));
        SetWindowPos(hwnd, HWND_TOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }

    // =====================================================================================
    // THE TACOPS OVERLAY (VHUD v16). A bezel key puts this window's image over the game (flat
    // screen) so TacOps is worked without leaving it. This app copies each finished frame into a
    // shared texture (OvCopy) and keeps OvWantMs fresh; VanillaHud draws it over the game at 70%,
    // zeroes the throttle, puts the game in UI-only input with its cursor showing, and reports
    // the cursor over the image. OvFrame turns those reports into the same mouse calls the
    // window's own messages make, so selection, drags, menus and bezel labels all work as they
    // do here. Toggle, or hold (closes when the key is let go).
    // =====================================================================================
    bool     ovOn = false, ovHold = false, ovHasPos = false, ovInside = false, ovLDownIn = false;
    UINT     ovHoldVk = 0;
    float    ovX = 0.0f, ovY = 0.0f;
    uint32_t ovSeq = 0, ovButtons = 0;
    int      ovWheel = 0;                      // wheel notches turned over the overlay, not yet used
    ULONGLONG ovReportMs = 0;                  // the plugin's last cursor report (it sends one per drawn frame)
    ComPtr<ID3D11Texture2D> ovTex;
    HANDLE   ovHandle = nullptr;
    UINT     ovW = 0, ovH = 0;
    // Where the overlay sits, how big and how solid: set by dragging it about, kept in the config.
    float    ovPosX = -10.0f, ovPosY = -10.0f; // top-left, fractions of the game; -10 = centred
    float    ovScale = 0.9f;                   // its height, fraction of the game's
    float    ovOpacity = 0.7f;
    int      ovDrag = 0;                       // 0 none, 1 moving (bezel strip), 2 resizing (corner grip)
    float    ovDragGx0 = 0, ovDragGy0 = 0, ovDragPosX0 = 0, ovDragPosY0 = 0, ovDragScale0 = 0, ovDragAspect = 1;
    static constexpr float kOvGrip = 30.0f;     // the corner handles, client pixels

    // The overlay's own controls, in this window's client pixels: they sit in the bezel corners,
    // where no label is. 0 none, 1 resize grip (bottom right), 2 opacity -, 3 opacity +, 4 reset.
    int OvHandleAt(float x, float y) const
    {
        RECT rc;
        GetClientRect(hwnd, &rc);
        const float w = (float)(rc.right - rc.left), h = (float)(rc.bottom - rc.top);
        if (x >= w - kOvGrip && y >= h - kOvGrip)
            return 1;
        if (y >= h - kOvGrip && x >= 2.0f && x < 2.0f + kOvGrip)
            return 2;
        if (y >= h - kOvGrip && x >= 4.0f + kOvGrip && x < 4.0f + 2.0f * kOvGrip)
            return 3;
        if (y <= kOvGrip && x >= 2.0f && x < 2.0f + 1.6f * kOvGrip)
            return 4;
        if (y <= kOvGrip && x >= w - 2.0f - 2.2f * kOvGrip && x <= w - 2.0f)
            return 5; // MOVE: the same as dragging the bezel strip, but impossible to miss
        return 0;
    }

    bool OvOverLabel(float x, float y) const
    {
        RECT rc;
        GetClientRect(hwnd, &rc);
        const float w = (float)(rc.right - rc.left), h = (float)(rc.bottom - rc.top);
        for (const auto &l : labels)
            if (PointIn(SlotRect(l, w, h), x, y))
                return true;
        return false;
    }

    void DrawOvHandles(float w, float h)
    {
        if (!ovOn || !uiBrush)
            return;
        // Resize grip: three diagonal strokes in the bottom-right corner.
        SetUiColor(0x3FE6CD, 0.9f);
        for (int i = 1; i <= 3; ++i)
        {
            const float o = 8.0f * (float)i;
            d2dCtx->DrawLine(D2D1::Point2F(w - 4.0f, h - 4.0f - o), D2D1::Point2F(w - 4.0f - o, h - 4.0f), uiBrush.Get(), 2.0f);
        }
        auto box = [&](float x0, float y0, float bw, const wchar_t *t) {
            const D2D1_RECT_F r = D2D1::RectF(x0, y0, x0 + bw, y0 + kOvGrip - 4.0f);
            SetUiColor(0x05080A, 0.85f);
            d2dCtx->FillRectangle(r, uiBrush.Get());
            SetUiColor(0x3FE6CD, 0.9f);
            d2dCtx->DrawRectangle(r, uiBrush.Get(), 1.0f);
            DrawPanelText(x0 + 5.0f, y0 + 2.0f, bw - 6.0f, t, 0x3FE6CD, 0.9f);
        };
        box(2.0f, h - kOvGrip + 2.0f, kOvGrip - 2.0f, L"-");
        box(4.0f + kOvGrip, h - kOvGrip + 2.0f, kOvGrip - 2.0f, L"+");
        wchar_t pct[16];
        swprintf_s(pct, L"%d%%", (int)(ovOpacity * 100.0f + 0.5f));
        DrawPanelText(8.0f + 2.0f * kOvGrip, h - kOvGrip + 4.0f, 60.0f, pct, 0x3FE6CD, 0.8f);
        box(2.0f, 2.0f, 1.6f * kOvGrip, L"RST");
        box(w - 2.0f - 2.2f * kOvGrip, 2.0f, 2.2f * kOvGrip, L"MOVE");
    }

    // The cursor over the game, drawn on whatever page is up (the game's own may not show).
    void DrawOvCursor()
    {
        if (!ovOn || !ovHasPos || !ovInside || !uiBrush)
            return;
        SetUiColor(0x000000, 0.8f);
        d2dCtx->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(ovX, ovY), 7.0f, 7.0f), uiBrush.Get(), 3.5f);
        SetUiColor(0xFFFFFF, 1.0f);
        d2dCtx->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(ovX, ovY), 7.0f, 7.0f), uiBrush.Get(), 1.5f);
        d2dCtx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(ovX, ovY), 1.5f, 1.5f), uiBrush.Get());
    }

    // The mouse wheel while the overlay is up. The game has the foreground, so the wheel goes to
    // it; raw input with INPUTSINK hands this window a copy without taking anything away from
    // the game. Registered only while the overlay is open, so the rest of the time this window
    // is not woken by every mouse move.
    void OvWheelSink(bool on)
    {
        RAWINPUTDEVICE rid{};
        rid.usUsagePage = 0x01; // generic desktop
        rid.usUsage = 0x02;     // mouse
        rid.dwFlags = on ? RIDEV_INPUTSINK : RIDEV_REMOVE;
        rid.hwndTarget = on ? hwnd : nullptr;
        if (!RegisterRawInputDevices(&rid, 1, sizeof(rid)) && on)
            Log("HUD overlay: no mouse wheel (raw input registration failed)");
    }

    void OnRawInput(HRAWINPUT h)
    {
        if (!ovOn)
            return;
        RAWINPUT ri{};
        UINT sz = sizeof(ri);
        if (GetRawInputData(h, RID_INPUT, &ri, &sz, sizeof(RAWINPUTHEADER)) == (UINT)-1)
            return;
        if (ri.header.dwType != RIM_TYPEMOUSE || !(ri.data.mouse.usButtonFlags & RI_MOUSE_WHEEL))
            return;
        // Only while the overlay is really being drawn over the game, and not when this window is
        // in front (its own WM_MOUSEWHEEL handles that - counting both would zoom twice).
        if (GetForegroundWindow() == hwnd || GetTickCount64() - ovReportMs > 100)
            return;
        const short d = (short)ri.data.mouse.usButtonData;
        if (ovInside)
            ovWheel += d > 0 ? 1 : (d < 0 ? -1 : 0);
    }

    // =====================================================================================
    // MOUSE CAMERA on the 3D and Tactical pages (TacOps has its own): left drag orbits, middle
    // drag pans, right click recentres. The wheel zooms every page that has a zoom.
    // =====================================================================================
    bool  camOrbiting = false, camPanning = false;
    float camMX = 0.0f, camMY = 0.0f;
    float camDownX = 0.0f, camDownY = 0.0f; // where the left button went down: no drag = a click

    // A click on a map page: the contact under it gets its paperdoll (one at a time); clicking it
    // again, or the empty ground, hides it. True when a contact was clicked.
    bool ContactClick(float x, float y)
    {
        const int id = topo3d.PickContact(x, y, 18.0f);
        if (!id)
        {
            topo3d.SetShownContact(0);
            return false;
        }
        topo3d.SetShownContact(topo3d.ShownContact() == id ? 0 : id);
        return true;
    }
    int   winWheelAcc = 0;

    bool MouseCam() const
    {
        return Show3D() && !IsTacOps() && !(isVanillaMode && IsPilotScreen());
    }

    void CamMouseEnd()
    {
        const bool had = camOrbiting || camPanning;
        camOrbiting = camPanning = false;
        if (had && GetCapture() == hwnd)
            ReleaseCapture();
    }

    void CamMouseMove(float x, float y)
    {
        const float dx = x - camMX, dy = y - camMY;
        camMX = x;
        camMY = y;
        if (!MouseCam())
        {
            CamMouseEnd();
            return;
        }
        if (camOrbiting)
            topo3d.Orbit(-dx * 0.3f, -dy * 0.2f);
        else if (camPanning)
        {
            // About the range across the page's height: the ground keeps roughly under the cursor.
            const VRect R = ContentRect();
            const float k = (float)lidarRangeM * 2.0f / (std::max)(R.h, 1.0f);
            topo3d.Pan(dy * k, -dx * k);
        }
    }

    // Polled like the TacOps drag: a click here does not activate this window, and a background
    // window's capture only sees the moves made over it.
    void CamPollDrag()
    {
        POINT p;
        if (ovOn || !(camOrbiting || camPanning) || !GetCursorPos(&p) || !ScreenToClient(hwnd, &p))
            return;
        if ((float)p.x != camMX || (float)p.y != camMY)
            CamMouseMove((float)p.x, (float)p.y);
    }

    void OnRightDown(float x, float y)
    {
        if (editMode)
            return;
        if (IsTacOps())
            TacRightDown(x, y);
        else if (MouseCam())
        {
            topo3d.Recentre();
            Log("3D camera recentred (right click)");
        }
    }

    void OnMiddleDown(float x, float y)
    {
        if (editMode)
            return;
        if (IsTacOps())
            TacMiddleDown(x, y);
        else if (MouseCam())
        {
            camPanning = true;
            camOrbiting = false;
            camMX = x;
            camMY = y;
            if (!ovOn)
                SetCapture(hwnd);
        }
    }

    void OnMiddleUp()
    {
        TacMiddleUp();
        if (camPanning)
            CamMouseEnd();
    }

    void OvToggle(bool hold, UINT vk)
    {
        if (ovOn)
        {
            OvClose("key");
            return;
        }
        PollVanillaMMF();
        if (!vBlock)
        {
            Log("TacOps overlay: VanillaHud plugin not connected");
            return;
        }
        // Whatever page this window is on goes over the game; the bezel labels change it from there.
        ovOn = true;
        OvWheelSink(true);
        // Hold only when a key started it and is still down (a click on the label toggles).
        ovHold = hold && vk && (GetAsyncKeyState((int)vk) & 0x8000);
        ovHoldVk = vk;
        ovSeq = vBlock->OvMouseSeq;
        ovButtons = 0;
        ovHasPos = ovInside = ovLDownIn = false;
        ovDrag = 0;
        ovWheel = 0;
        Log(std::string("HUD overlay: open") + (ovHold ? " (while held)" : ""));
    }

    void OvClose(const char *why)
    {
        if (!ovOn)
            return;
        ovOn = false;
        OvWheelSink(false);
        EndDrags(true);       // a label being dragged in the options menu
        EndPageInteractions();
        if (vBlock)
        {
            vBlock->OvWantMs = 0;
            vBlock->OvHandle = 0;
        }
        ovTex.Reset();
        ovHandle = nullptr;
        ovW = ovH = 0;
        Log(std::string("HUD overlay: closed (") + why + ")");
    }

    // Every frame: keep the plugin's request fresh and play its cursor reports into the page.
    void OvFrame()
    {
        if (!ovOn)
            return;
        PollVanillaMMF();
        if (!vBlock)
        {
            OvClose("plugin gone");
            return;
        }
        if (ovHold && ovHoldVk && !(GetAsyncKeyState((int)ovHoldVk) & 0x8000))
        {
            OvClose("key let go");
            return;
        }
        // (No Esc-to-close: the key state is Windows-wide, so Esc pressed in any program closed it.)
        vBlock->OvOpacity = ovOpacity;
        vBlock->OvScale = ovScale;
        vBlock->OvPosX = ovPosX;
        vBlock->OvPosY = ovPosY;
        vBlock->OvWantMs = GetTickCount64();

        if (!vBlock->OvShown)
        {
            ovWheel = 0;
            return;
        }
        // Wheel notches over the image (WM_INPUT gathers them; the plugin does not see the wheel).
        while (ovWheel != 0)
        {
            if (ovInside && !editMode)
                ZoomActivePage(ovWheel > 0, true);
            ovWheel += ovWheel > 0 ? -1 : 1;
        }
        const uint32_t seq = vBlock->OvMouseSeq;
        if (seq == ovSeq)
            return;
        ovSeq = seq;
        ovReportMs = GetTickCount64();
        MemoryBarrier();
        const float u = vBlock->OvMouseU, v = vBlock->OvMouseV;
        const uint32_t b = vBlock->OvButtons;
        // The cursor in game fractions, through the rect this report was made against.
        const float rX = vBlock->OvRectX, rY = vBlock->OvRectY, rW = vBlock->OvRectW, rH = vBlock->OvRectH;
        const float gx = rX + u * rW, gy = rY + v * rH;
        RECT rc;
        GetClientRect(hwnd, &rc);
        const float x = u * (float)(rc.right - rc.left), y = v * (float)(rc.bottom - rc.top);
        const bool inside = u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f;
        const bool moved = !ovHasPos || std::fabs(x - ovX) > 0.5f || std::fabs(y - ovY) > 0.5f;
        ovX = x;
        ovY = y;
        ovHasPos = true;
        ovInside = inside;
        tacMouseFlags = 0; // modifiers are read live (TacAdditive)
        const uint32_t down = b & ~ovButtons, up = ovButtons & ~b;
        ovButtons = b;

        // Moving / resizing the overlay itself.
        if (ovDrag)
        {
            if (ovDrag == 1)
            {
                ovPosX = (std::min)((std::max)(ovDragPosX0 + (gx - ovDragGx0), -rW * 0.9f), 1.0f - rW * 0.1f);
                ovPosY = (std::min)((std::max)(ovDragPosY0 + (gy - ovDragGy0), -rH * 0.9f), 1.0f - rH * 0.1f);
            }
            else
            {
                // Bottom-right grip, top-left fixed: whichever way it was pulled further, in
                // height terms (width converted through the image's aspect on the game screen).
                const float dy = gy - ovDragGy0, dxAsH = (gx - ovDragGx0) * ovDragAspect;
                ovScale = (std::min)((std::max)(ovDragScale0 + (std::fabs(dxAsH) > std::fabs(dy) ? dxAsH : dy), 0.2f), 1.0f);
            }
            if (up & 1u)
            {
                ovDrag = 0;
                SaveConfig();
                Log("TacOps overlay: now at (" + std::to_string(ovPosX) + ", " + std::to_string(ovPosY) + "), height " +
                    std::to_string(ovScale));
            }
            return;
        }
        if ((down & 1u) && inside)
        {
            const int hnd = OvHandleAt(x, y);
            const VRect C = ContentRect();
            const bool bezel = !(x >= C.x && x <= C.x + C.w && y >= C.y && y <= C.y + C.h);
            if (hnd == 2 || hnd == 3)
            {
                ovOpacity = (std::min)((std::max)(ovOpacity + (hnd == 3 ? 0.1f : -0.1f), 0.3f), 1.0f);
                SaveConfig();
                return;
            }
            if (hnd == 4)
            {
                ovPosX = ovPosY = -10.0f;
                ovScale = 0.9f;
                ovOpacity = 0.7f;
                SaveConfig();
                Log("TacOps overlay: position, size and opacity reset");
                return;
            }
            if (hnd == 1 || hnd == 5 || (bezel && !OvOverLabel(x, y)))
            {
                ovDrag = hnd == 1 ? 2 : 1;
                Log(std::string("TacOps overlay: ") + (ovDrag == 2 ? "resizing" : "moving") +
                    (hnd == 5 ? " (MOVE handle)" : (hnd == 1 ? "" : " (bezel strip)")));
                ovDragGx0 = gx;
                ovDragGy0 = gy;
                ovDragPosX0 = rX; // from where it is actually drawn, centred or not
                ovDragPosY0 = rY;
                ovDragScale0 = rH;
                ovDragAspect = rW > 0.0f ? rH / rW : 1.0f;
                return;
            }
        }
        if (moved)
            OnMouseMove(x, y);
        if ((down & 1u) && inside)
        {
            ovLDownIn = true;
            OnLeftDown(x, y);
            if (!ovOn)
                return; // that click closed the overlay (its own bezel label)
        }
        if ((up & 1u) && ovLDownIn)
        {
            ovLDownIn = false;
            OnLeftUp();
        }
        if ((down & 2u) && inside)
            OnRightDown(x, y);
        if ((down & 4u) && inside)
            OnMiddleDown(x, y);
        if (up & 4u)
            OnMiddleUp();
    }

    // The finished frame into the shared texture, under its keyed mutex (this side 0 -> 1).
    void OvCopy()
    {
        if (!ovOn || !vBlock || !swapChain || !device || !context)
            return;
        ComPtr<ID3D11Texture2D> bb;
        if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)bb.GetAddressOf())))
            return;
        D3D11_TEXTURE2D_DESC bd{};
        bb->GetDesc(&bd);
        if (!ovTex || ovW != bd.Width || ovH != bd.Height)
        {
            vBlock->OvHandle = 0; // the plugin lets go of the old one before it is replaced
            ovTex.Reset();
            ovHandle = nullptr;
            D3D11_TEXTURE2D_DESC d{};
            d.Width = bd.Width;
            d.Height = bd.Height;
            d.MipLevels = 1;
            d.ArraySize = 1;
            d.Format = bd.Format;
            d.SampleDesc.Count = 1;
            d.Usage = D3D11_USAGE_DEFAULT;
            d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            d.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
            ComPtr<IDXGIResource> r;
            if (FAILED(device->CreateTexture2D(&d, nullptr, ovTex.GetAddressOf())) || FAILED(ovTex.As(&r)) ||
                FAILED(r->GetSharedHandle(&ovHandle)) || !ovHandle)
            {
                ovTex.Reset();
                ovHandle = nullptr;
                Log("TacOps overlay: could not create the shared image");
                OvClose("no shared image");
                return;
            }
            ovW = bd.Width;
            ovH = bd.Height;
            vBlock->OvW = ovW;
            vBlock->OvH = ovH;
            MemoryBarrier();
            vBlock->OvHandle = (uint64_t)(uintptr_t)ovHandle;
            Log("TacOps overlay: shared image " + std::to_string(ovW) + "x" + std::to_string(ovH));
        }
        ComPtr<IDXGIKeyedMutex> km;
        if (FAILED(ovTex.As(&km)) || km->AcquireSync(0, 0) != S_OK)
            return; // the plugin still has the last one; skip this frame
        context->CopyResource(ovTex.Get(), bb.Get());
        km->ReleaseSync(1);
    }

    void TacSay(const std::wstring &s)
    {
        tacStatus   = s;
        tacStatusMs = NowMs();
    }

    const VanillaExportBlock::TacMember *TacMember(int slot) const
    {
        for (int i = 0; i < tacMemberCount; ++i)
            if (tacMembers[i].Slot == slot)
                return &tacMembers[i];
        return nullptr;
    }

    // One order into VanillaHud's ring. This app is the ring's only writer.
    uint32_t TacSend(uint8_t order, int slot, const float *loc, uint8_t pad0 = 0, uint8_t pad1 = 0) // pads: TCO_CalledShot
    {
        if (!vBlock || slot < 1 || slot > kTacMax)
            return 0;
        // Claimed with a compare-exchange: another writer (the voice module) may be sending too.
        uint32_t n = 0;
        for (;;)
        {
            n = vBlock->TacCmdHead;
            if ((uint32_t)InterlockedCompareExchange(reinterpret_cast<volatile LONG *>(&vBlock->TacCmdHead), (LONG)(n + 1), (LONG)n) == n)
                break;
        }
        VanillaExportBlock::TacCmd &c = vBlock->TacCmds[n % VanillaExportBlock::kTacCmdRing];
        c.Seq   = 0;
        MemoryBarrier();
        c.Order = order;
        c.Slot  = (uint8_t)slot;
        c.Pad[0] = pad0;
        c.Pad[1] = pad1;
        c.Loc[0] = loc ? loc[0] : 0.0f;
        c.Loc[1] = loc ? loc[1] : 0.0f;
        c.Loc[2] = loc ? loc[2] : 0.0f;
        MemoryBarrier();
        c.Seq = n + 1;
        MemoryBarrier();
        return n + 1;
    }

    // One leg of a route. The player's own mech walks under the plugin's autopilot; everybody
    // else gets the game's move order.
    void TacSendMove(int slot, const float *loc)
    {
        TacLocal &L = tacLocal[slot];
        uint8_t order = VanillaExportBlock::TCO_Move;
        if (L.IsPlayer)
            order = VanillaExportBlock::TCO_SelfMove;
        else if (L.MoveMode == 1)
            order = VanillaExportBlock::TCO_MoveFire;
        else if (L.MoveMode == 2)
            order = VanillaExportBlock::TCO_Fallback;
        else if (L.MoveMode == 4)
            order = VanillaExportBlock::TCO_IndirectCover;
        L.WpSeq = TacSend(order, slot, loc);
    }

    void TacStop(int slot)
    {
        TacLocal &L = tacLocal[slot];
        L.WpCount = 0;
        L.WpSeq = 0;
        if (L.IsPlayer)
            TacSend(VanillaExportBlock::TCO_SelfStop, slot, nullptr);
    }

    // The order a lancemate is given on arriving at a waypoint that carries one.
    void TacArrive(int slot, int act, int target, const float *here)
    {
        TacLocal &L = tacLocal[slot];
        if (act == TWA_Hold)
        {
            // HOLD HERE: the plugin finishes the walk to the point, then switches on the AI's own
            // "Wait" so it stands there, and keeps it supplied with enemies to shoot.
            L.Order = TOS_Hold;
            if (!L.IsPlayer)
                TacSend(VanillaExportBlock::TCO_HoldHere, slot, here);
        }
        else if (act == TWA_Attack && !L.IsPlayer)
        {
            float loc[3], ex[3];
            if (topo3d.TacContactLoc(target, loc))
            {
                L.Order = TOS_Attack;
                TacSend(VanillaExportBlock::TCO_Attack, slot, TacTargetLoc(target, loc, ex)); // the exact unit (lc61)
            }
        }
    }

    // Every frame the page is up: ask for the lance, read it, walk the routes, build the view.
    // bug sweep 18 (2026-10-18): the lance read on its own, for the 3D page's compass too (LanceControl publishes the
    // lance 4 times a second anyway; `want` asks for every frame - the TacOps page only).
    void TacReadMembers(bool want)
    {
        tacMemberCount = 0;
        if (vBlock)
        {
            if (want)
                vBlock->TacWantMs = GetTickCount64();
            for (int attempt = 0; attempt < 3; ++attempt)
            {
                const uint32_t s1 = vBlock->TacSeq;
                if (s1 & 1u)
                    continue;
                MemoryBarrier();
                VanillaExportBlock::TacMember tmp[kTacMax];
                std::memcpy(tmp, (const void *)vBlock->TacMembers, sizeof(tmp));
                const uint32_t cnt = vBlock->TacCount;
                MemoryBarrier();
                if (vBlock->TacSeq != s1)
                    continue;
                for (uint32_t i = 0; i < cnt && i < (uint32_t)kTacMax; ++i)
                    if (tmp[i].Valid && tmp[i].Slot >= 1 && tmp[i].Slot <= kTacMax)
                        tacMembers[tacMemberCount++] = tmp[i];
                break;
            }
        }
    }

    void TacPrepare()
    {
        PollVanillaMMF();
        TacReadMembers(true);

        // Routes: a lancemate that has reached its waypoint does what that waypoint says, then
        // is sent on to the next one.
        for (int s = 1; s <= kTacMax; ++s)
        {
            TacLocal &L = tacLocal[s];
            const auto *m = TacMember(s);
            if (!m)
            {
                L.Selected = false;
                continue;
            }
            L.IsPlayer = m->IsPlayer != 0;
            if (L.WpCount <= 0)
                continue;
            // The plugin drops a move's echo when it replaces the move itself: your autopilot letting
            // go early (you took the controls, it got stuck, it was cancelled), or a lancemate's
            // "advance, fall back to me" turning into the fall back. The rest of the route goes too -
            // reaching the next point would otherwise send a leg nobody asked for any more.
            if (L.WpSeq)
            {
                if (m->LastMoveSeq == L.WpSeq)
                    L.WpEchoed = L.WpSeq;
                else if (L.WpEchoed == L.WpSeq)
                {
                    L.WpCount = 0;
                    L.WpSeq = 0;
                    if (L.Order == TOS_Move)
                        L.Order = TOS_None;
                    if (L.IsPlayer)
                    {
                        TacSay(L"Autopilot off - you have control");
                        Log("TacOps: your route dropped - the autopilot let go");
                    }
                    else
                    {
                        TacSay(L"Contact - falling back to you");
                        Log("TacOps: slot " + std::to_string(s) + " route dropped - the plugin replaced the move");
                    }
                    continue;
                }
            }
            // AGAINST THE POINT THE PLUGIN SENT, not the one clicked. It snaps a move onto
            // walkable ground, which can be tens of metres away - measuring against the click
            // meant a lancemate could stand on its destination and never be counted as arrived,
            // so the rest of its route, and "move and hold", never happened.
            const float *goal = (L.WpSeq && m->LastMoveSeq == L.WpSeq) ? m->LastMove : L.Wp[0];
            const float dx = m->Loc[0] - goal[0], dy = m->Loc[1] - goal[1];
            if (dx * dx + dy * dy > kTacArriveCm * kTacArriveCm)
                continue;
            const int act = L.WpAct[0], target = L.WpTarget[0];
            float reached[3]; // copied: the shift below overwrites Wp[0], which `goal` may point at
            std::memcpy(reached, goal, sizeof(reached));
            for (int k = 1; k < L.WpCount; ++k)
            {
                std::memcpy(L.Wp[k - 1], L.Wp[k], sizeof(L.Wp[k]));
                L.WpAct[k - 1] = L.WpAct[k];
                L.WpTarget[k - 1] = L.WpTarget[k];
            }
            --L.WpCount;
            L.WpSeq = 0;
            if (L.WpCount > 0)
                TacSendMove(s, L.Wp[0]);
            else
            {
                if (L.Order == TOS_Move)
                    L.Order = TOS_None;
                TacArrive(s, act, target, reached);
            }
        }

        for (int i = 0; i < tacMemberCount; ++i)
        {
            const auto &m = tacMembers[i];
            TacLocal &F = tacLocal[m.Slot];
            F.FsPlugin = m.FirePlugin;
            // What was sent is shown until the plugin reports exactly that (or 5 s pass).
            if (F.FsPending && m.FireDisc == F.FsDisc && m.FireMask == F.FsMask && m.FireHeat == F.FsHeat)
                F.FsPending = false;
            if (F.FsPending && NowMs() - F.FsSentMs > 5000.0)
                F.FsPending = false;
            if (!F.FsPending)
            {
                F.FsDisc = m.FireDisc;
                F.FsMask = m.FireMask;
                F.FsHeat = m.FireHeat;
            }
        }
        tacView.MateCount = 0;
        const bool voice = ViRead(); // 2026-10-19: the voice trace on each lancemate's plate
        const bool feed = LsReadSlots(); // 2026-10-19: what each lancemate is actually doing (OrderWords)
        if (feed)
            LsReadOrders();
        const uint32_t nowMsOw = (uint32_t)GetTickCount64();
        for (int i = 0; i < tacMemberCount && tacView.MateCount < 8; ++i)
        {
            const auto &m = tacMembers[i];
            const TacLocal &L = tacLocal[m.Slot];
            Topo3DView::TacMate &t = tacView.Mates[tacView.MateCount++];
            t.Slot = m.Slot;
            t.Selected = L.Selected ? 1 : 0;
            t.Dead = m.Dead;
            t.IsPlayer = m.IsPlayer;
            std::memcpy(t.Loc, m.Loc, sizeof(t.Loc));
            t.Yaw = m.Yaw;
            t.WpCount = L.WpCount;
            std::memcpy(t.Wp, L.Wp, sizeof(t.Wp));
            t.Order = L.Order;
            t.ActorLo = m.ActorLo;
            t.ActorHi = m.ActorHi;
            t.FsDisc = L.FsDisc;
            t.FsMask = L.FsMask;
            t.FsHeat = L.FsHeat;
            t.FsPlugin = L.FsPlugin;
            t.HaveVoice = 0;
            t.Speaking = 0;
            t.OrderText[0] = 0;
            t.OrderCol = 0;
            t.TargetLo = t.TargetHi = 0;
            if (feed && !m.IsPlayer && m.Slot >= 1 && m.Slot <= kLsMaxSlots)
                for (uint32_t k = 0; k < lsSlotCopyCount; ++k)
                    if (lsSlotsCopy[k].Valid && lsSlotsCopy[k].Slot == m.Slot)
                    {
                        LsOrderSeen &lo = lsLastOrder[m.Slot];
                        if (lo.ActorLo != m.ActorLo || lo.ActorHi != m.ActorHi)
                        {
                            lo = LsOrderSeen{}; // a new unit in this slot (as V-LNC does)
                            lo.ActorLo = m.ActorLo;
                            lo.ActorHi = m.ActorHi;
                            lo.SinceMs = nowMsOw;
                        }
                        OrderWording ow;
                        OrderWords(m, lsSlotsCopy[k], lo, ow);
                        swprintf_s(t.OrderText, L"%ls", ow.Short);
                        t.OrderCol = ow.Col;
                        t.TargetLo = lsSlotsCopy[k].TargetLo;
                        t.TargetHi = lsSlotsCopy[k].TargetHi;
                        break;
                    }
            if (voice && !m.IsPlayer && m.Slot < kViSlots)
            {
                const ViSlot &vs = viCopy.Slots[m.Slot];
                t.HaveVoice = 1;
                t.Speaking = vs.Speaking;
                for (int k = 0; k < kViWave && k < 144; ++k)
                {
                    t.WaveLo[k] = vs.WaveLo[(vs.WaveHead + k) % kViWave]; // oldest first
                    t.WaveHi[k] = vs.WaveHi[(vs.WaveHead + k) % kViWave];
                }
            }
        }
        // The player's plate shows the game's own-mech paperdoll (the V-OWN element), drawn over it
        // after the map - see DrawTacOpsOverlays.
        tacView.OwnImage = (vBlock && vBlock->Slots[VS_Owner].Active && vBlock->Slots[VS_Owner].SharedHandle) ? 1 : 0;
        topo3d.SetTacOps(&tacView);
        TacPollDrag();
    }

    int TacSelectedCount() const
    {
        int n = 0;
        for (int i = 0; i < tacView.MateCount; ++i)
            n += (tacLocal[tacView.Mates[i].Slot].Selected && !tacView.Mates[i].Dead) ? 1 : 0;
        return n;
    }

    // The lancemate ring (or plate) under (x, y), by slot; 0 for none.
    int TacMateAt(float x, float y) const
    {
        int best = 0;
        float bestD = 18.0f * 18.0f;
        for (int i = 0; i < tacView.MateCount; ++i)
        {
            float sx, sy;
            if (!topo3d.TacProject(tacView.Mates[i].Loc, sx, sy))
                continue;
            const float d = (sx - x) * (sx - x) + (sy - y) * (sy - y);
            if (d < bestD)
            {
                bestD = d;
                best = tacView.Mates[i].Slot;
            }
        }
        if (!best)
            best = topo3d.TacPanelAt(x, y);
        return best;
    }

    // From the mouse message itself: the window does not have the keyboard, so GetKeyState
    // would be reading a keyboard this thread never sees.
    bool TacAdditive() const
    {
        return (tacMouseFlags & (MK_SHIFT | MK_CONTROL)) != 0 || (GetAsyncKeyState(VK_SHIFT) & 0x8000) ||
               (GetAsyncKeyState(VK_CONTROL) & 0x8000);
    }

    // THE DRAG, POLLED. The log showed no box select ever completing: this window is never the
    // active one on this page, and the mouse moves of a drag did not reach it. So while the
    // button is down the cursor and the button are read directly each frame - both are
    // system-wide state and do not care which window has focus.
    void TacPollDrag()
    {
        if (tacTorsoDrag)
        {
            // Polled for the same reason as the box drag below; over the game, OvFrame's reports
            // arrive through TacMouseMove instead.
            POINT tp;
            if (!ovOn && GetCursorPos(&tp) && ScreenToClient(hwnd, &tp) &&
                ((float)tp.x != tacTorsoX || (float)tp.y != tacTorsoY))
            {
                tacTorsoX = (float)tp.x;
                tacTorsoY = (float)tp.y;
                tacTorsoDirty = true;
            }
            TacTorsoSend(false);
            return;
        }
        if (!tacDragArmed)
            return;
        POINT p;
        if (ovOn)
        {
            // Over the game: the cursor comes from the plugin (OvFrame keeps ovX / ovY).
            if (ovHasPos)
            {
                tacDX1 = ovX;
                tacDY1 = ovY;
                if (!tacDragging &&
                    ((tacDX1 - tacDX0) * (tacDX1 - tacDX0) + (tacDY1 - tacDY0) * (tacDY1 - tacDY0)) > 36.0f)
                    tacDragging = true;
            }
        }
        else if (GetCursorPos(&p) && ScreenToClient(hwnd, &p))
        {
            tacDX1 = (float)p.x;
            tacDY1 = (float)p.y;
            if (!tacDragging &&
                ((tacDX1 - tacDX0) * (tacDX1 - tacDX0) + (tacDY1 - tacDY0) * (tacDY1 - tacDY0)) > 36.0f)
                tacDragging = true;
        }
        // The button state is NOT polled. With it polled, HUDDisplay_up.log showed every
        // release as a "click" - most likely GetAsyncKeyState read the button as up while the
        // game held the foreground, ending the drag on the next frame. WM_LBUTTONUP does arrive
        // (every click logs it), so it alone ends the drag.
    }

    bool TacLeftDown(float x, float y)
    {
        if (tacMenuOpen)
        {
            for (int i = 0; i < tacMenuCount; ++i)
                if (PointIn(tacMenuRect[i], x, y))
                {
                    if (tacMenuDisabled[i])
                        return true; // greyed out: not selectable, the menu stays up
                    if (tacMenuItems[i] == TMI_CalledShotMenu)
                    {
                        // 2026-10-12: the same menu, now the parts (same enemy, same place)
                        static const int kOrder[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 0};
                        tacMenuCount = 0;
                        for (int p : kOrder)
                        {
                            tacMenuDisabled[tacMenuCount] = false;
                            tacMenuItems[tacMenuCount++] = TMI_Cs0 + p;
                        }
                        return true;
                    }
                    tacMenuOpen = false;
                    if (TacFsItem(tacMenuItems[i]))
                    {
                        TacFsPick(tacMenuItems[i]);
                        tacMenuOpen = true; // toggles: the menu stays up
                        return true;
                    }
                    TacMenuPick(tacMenuItems[i]);
                    return true;
                }
            tacMenuOpen = false; // a click anywhere else closes it
            return true;
        }
        if (TacTorsoHandleAt(x, y))
        {
            tacTorsoDrag = true;
            tacTorsoDirty = false;
            tacTorsoX = x;
            tacTorsoY = y;
            tacTorsoNextMs = 0.0;
            if (!ovOn)
                SetCapture(hwnd);
            return true;
        }
        for (int b = 0; b < TB_COUNT; ++b)
            if (PointIn(tacBtn[b], x, y))
            {
                TacButtonPress(b);
                return true;
            }
        for (int f = 0; f < TF_COUNT; ++f)
            if (PointIn(tacFormBtn[f], x, y))
            {
                tacFormation = f;
                static const wchar_t *kName[TF_COUNT] = {L"Line abreast", L"Column", L"Wedge", L"Box"};
                TacSay(std::wstring(L"Formation: ") + kName[f]);
                return true;
            }
        if (PointIn(tacSpaceBtn, x, y))
        {
            tacSpacing = (tacSpacing + 1) % 3;
            TacSay(L"Spacing " + std::to_wstring((int)TacSpacingM(tacSpacing)) + L" m");
            return true;
        }
        tacDragArmed = true;
        tacDragging  = false;
        tacDX0 = tacDX1 = x;
        tacDY0 = tacDY1 = y;
        if (!ovOn) // over the game the mouse is the game window's: nothing to capture here
            SetCapture(hwnd);
        return true;
    }

    void TacMouseMove(float x, float y)
    {
        if (tacTorsoDrag)
        {
            if (x != tacTorsoX || y != tacTorsoY)
                tacTorsoDirty = true;
            tacTorsoX = x;
            tacTorsoY = y;
            TacTorsoSend(false);
            return;
        }
        if (tacPanning)
        {
            float a[3], b[3];
            if (topo3d.TacPickGround(tacPanX, tacPanY, a) && topo3d.TacPickGround(x, y, b))
                topo3d.TacPanWorld((a[0] - b[0]) * 0.01f, (a[1] - b[1]) * 0.01f);
            tacPanX = x;
            tacPanY = y;
            return;
        }
        if (!tacDragArmed)
            return;
        tacDX1 = x;
        tacDY1 = y;
        if (!tacDragging && ((x - tacDX0) * (x - tacDX0) + (y - tacDY0) * (y - tacDY0)) > 36.0f)
            tacDragging = true;
    }

    void TacLeftUp()
    {
        if (!tacDragArmed)
            return;
        tacDragArmed = false;
        // THE BUG ALL ALONG: ReleaseCapture sends WM_CAPTURECHANGED straight back into this
        // window, and that handler clears the drag (TacEndDrags) - so by the next line every drag,
        // however far it had moved (the log: "moved 1072 px"), read as a click. Decide first,
        // release after.
        const bool wasDrag = tacDragging || ((tacDX1 - tacDX0) * (tacDX1 - tacDX0) +
                                             (tacDY1 - tacDY0) * (tacDY1 - tacDY0)) > 36.0f;
        tacDragging = wasDrag;
        if (GetCapture() == hwnd)
            ReleaseCapture();
        tacDragging = wasDrag;
        const bool add = TacAdditive();
        {
            char b[128];
            std::snprintf(b, sizeof(b), "TacOps: left up, %s (moved %.0f px)%s", tacDragging ? "drag" : "click",
                          std::sqrt((tacDX1 - tacDX0) * (tacDX1 - tacDX0) + (tacDY1 - tacDY0) * (tacDY1 - tacDY0)),
                          add ? " (adding)" : "");
            Log(b);
        }
        if (!tacDragging)
        {
            const int slot = TacMateAt(tacDX0, tacDY0);
            // Not one of yours: a contact's paperdoll, shown or hidden - and the selection is
            // left as it was, so inspecting an enemy does not lose the lancemates picked.
            if (!slot && ContactClick(tacDX0, tacDY0))
                return;
            if (!add)
                for (auto &L : tacLocal)
                    L.Selected = false;
            if (slot)
                tacLocal[slot].Selected = add ? !tacLocal[slot].Selected : true;
            return;
        }
        tacDragging = false;
        // A ring counts if any of it is inside the box, not only its centre.
        const float pad = 12.0f;
        const float x0 = (std::min)(tacDX0, tacDX1) - pad, x1 = (std::max)(tacDX0, tacDX1) + pad;
        const float y0 = (std::min)(tacDY0, tacDY1) - pad, y1 = (std::max)(tacDY0, tacDY1) + pad;
        if (!add)
            for (auto &L : tacLocal)
                L.Selected = false;
        int n = 0;
        for (int i = 0; i < tacView.MateCount; ++i)
        {
            float sx, sy;
            if (tacView.Mates[i].Dead || !topo3d.TacProject(tacView.Mates[i].Loc, sx, sy))
                continue;
            if (sx >= x0 && sx <= x1 && sy >= y0 && sy <= y1)
            {
                tacLocal[tacView.Mates[i].Slot].Selected = true;
                ++n;
            }
        }
        Log("TacOps: box select " + std::to_string(n) + " of " + std::to_string(tacView.MateCount));
    }

    void TacRightDown(float x, float y)
    {
        tacMenuOpen = false;
        tacMenuCount = 0;
        for (bool &d : tacMenuDisabled)
            d = false;
        tacMenuX = x;
        tacMenuY = y;
        float hit[3];
        // A lancemate (ring or side plate): its fire settings - and every selected lancemate's, if
        // it is one of the selection.
        if (const int ms = TacMateAt(x, y))
            if (!tacLocal[ms].IsPlayer && TacMateLoc(ms))
            {
                tacFsCount = 0;
                if (tacLocal[ms].Selected)
                    tacFsCount = TacSelection(tacFsSlots, 8);
                bool in = false;
                for (int i = 0; i < tacFsCount; ++i)
                    in |= tacFsSlots[i] == ms;
                if (!in)
                {
                    tacFsCount = 0;
                    tacFsSlots[tacFsCount++] = ms;
                }
                // (a selection including you: your own guns are yours)
                int k = 0;
                for (int i = 0; i < tacFsCount; ++i)
                    if (!tacLocal[tacFsSlots[i]].IsPlayer)
                        tacFsSlots[k++] = tacFsSlots[i];
                tacFsCount = k;
                tacMenuTarget = 0;
                for (int it = TMI_FsFree; it <= TMI_FsReset; ++it)
                    tacMenuItems[tacMenuCount++] = it;
                tacMenuOpen = true;
                return;
            }
        if (const int id = topo3d.TacHostileAt(x, y, 16.0f, hit))
        {
            tacMenuTarget = id;
            std::memcpy(tacMenuLoc, hit, sizeof(hit));
            tacMenuItems[tacMenuCount++] = TMI_Attack;
            tacMenuItems[tacMenuCount++] = TMI_Focus;
            tacMenuItems[tacMenuCount++] = TMI_Engage;
            // 2026-10-12: your lock - greyed out unless the lance detects it - and the called-shot submenu
            tacMenuDisabled[tacMenuCount] = TacDetected(tacMenuLoc, tacMenuActorLo, tacMenuActorHi, tacMenuSpotter) != 1;
            tacMenuItems[tacMenuCount++] = TMI_Lock;
            tacMenuItems[tacMenuCount++] = TMI_CalledShotMenu;
            tacMenuItems[tacMenuCount++] = TMI_Flank;  // 2026-10-20 (lancecontrol-75)
            tacMenuItems[tacMenuCount++] = TMI_Pincer;
            tacMenuItems[tacMenuCount++] = TMI_TorsoTarget;
            tacMenuItems[tacMenuCount++] = TMI_TorsoRelease;
        }
        else if (topo3d.TacPickGround(x, y, tacMenuLoc))
        {
            tacMenuTarget = 0;
            tacMenuItems[tacMenuCount++] = TMI_Move;
            tacMenuItems[tacMenuCount++] = TMI_MoveFire;
            tacMenuItems[tacMenuCount++] = TMI_Fallback;
            tacMenuItems[tacMenuCount++] = TMI_IndirectCover;
            tacMenuItems[tacMenuCount++] = TMI_MoveHold;
            tacMenuItems[tacMenuCount++] = TMI_Waypoint;
            tacMenuItems[tacMenuCount++] = TMI_Form;
            tacMenuItems[tacMenuCount++] = TMI_Cancel;
            tacMenuItems[tacMenuCount++] = TMI_TorsoAngle;
            tacMenuItems[tacMenuCount++] = TMI_TorsoPoint;
            tacMenuItems[tacMenuCount++] = TMI_TorsoCentre;
            tacMenuItems[tacMenuCount++] = TMI_TorsoRelease;
            // NAV POINTS (2026-10-07k): mark one here; and remove yours, if the click is on one
            tacMenuItems[tacMenuCount++] = TMI_NavMark;
            tacMenuNav = -1;
            float best = 18.0f * 18.0f;
            for (int i = 0; i < navCount; ++i)
            {
                float sx, sy;
                if (!navRaw[i].Mine || !topo3d.TacProject(navRaw[i].Loc, sx, sy))
                    continue;
                const float d = (sx - x) * (sx - x) + (sy - y) * (sy - y);
                if (d < best)
                {
                    best = d;
                    tacMenuNav = navRaw[i].Letter;
                    std::memcpy(tacMenuNavLoc, navRaw[i].Loc, sizeof(tacMenuNavLoc));
                }
            }
            if (tacMenuNav >= 0 && tacMenuCount < 16)
                tacMenuItems[tacMenuCount++] = TMI_NavUnmark;
        }
        else
        {
            TacSay(L"No ground there");
            return;
        }
        tacMenuOpen = true;
    }

    // The selected, living lancemates' slots, in slot order.
    int TacSelection(int *slots, int max) const
    {
        int n = 0;
        for (int s = 1; s <= kTacMax && n < max; ++s)
        {
            if (!tacLocal[s].Selected)
                continue;
            for (int i = 0; i < tacView.MateCount; ++i)
                if (tacView.Mates[i].Slot == s && !tacView.Mates[i].Dead)
                {
                    slots[n++] = s;
                    break;
                }
        }
        return n;
    }

    const float *TacMateLoc(int slot) const
    {
        for (int i = 0; i < tacView.MateCount; ++i)
            if (tacView.Mates[i].Slot == slot)
                return tacView.Mates[i].Loc;
        return nullptr;
    }

    // Your own order slot (the torso orders ignore it, but it must be valid): yours if known.
    int TacPlayerSlot() const
    {
        int slot = 1;
        for (int i = 0; i < tacView.MateCount; ++i)
            if (tacView.Mates[i].IsPlayer)
                slot = tacView.Mates[i].Slot;
        return slot;
    }

    // THE TORSO HANDLE. A press within reach of the arrow's tip picks it up; while held, the
    // torso is ordered at the ground under the cursor (TCO_TorsoAngle: held relative to the legs
    // afterwards), at most every 200 ms, and once more on release.
    bool TacTorsoHandleAt(float x, float y) const
    {
        float ox, oy, tx, ty;
        if (!topo3d.TacTorsoArrow(ox, oy, tx, ty))
            return false;
        return (x - tx) * (x - tx) + (y - ty) * (y - ty) <= 14.0f * 14.0f;
    }

    void TacTorsoSend(bool final)
    {
        const double now = NowMs();
        if (!tacTorsoDirty || (!final && now < tacTorsoNextMs))
            return;
        float cm[3];
        if (!topo3d.TacPickGround(tacTorsoX, tacTorsoY, cm))
            return; // over the sky: nothing to point at, keep the last order
        TacSend(VanillaExportBlock::TCO_TorsoAngle, TacPlayerSlot(), cm);
        tacTorsoDirty = false;
        tacTorsoNextMs = now + 200.0;
    }

    void TacTorsoEnd()
    {
        if (!tacTorsoDrag)
            return;
        TacTorsoSend(true);
        tacTorsoDrag = false;
        if (GetCapture() == hwnd)
            ReleaseCapture();
        TacSay(L"Torso: holding that angle");
    }

    // The fire-settings menu: the first lancemate's settings are the menu's state; a pick changes
    // that one thing and sends every lancemate in the menu the result.
    void TacFsPick(int item)
    {
        if (!tacFsCount)
            return;
        // Only the thing picked changes, on each lancemate's own settings (a category toggle follows
        // the menu's first lancemate: on for all if it was off there, off for all if it was on).
        const TacLocal &ref = tacLocal[tacFsSlots[0]];
        const int cat = (item >= TMI_FsCat0 && item <= TMI_FsCat7) ? item - TMI_FsCat0 : -1;
        const bool catOn = cat >= 0 && !((ref.FsMask >> cat) & 1);
        bool def = true;
        for (int i = 0; i < tacFsCount; ++i)
        {
            TacLocal &L = tacLocal[tacFsSlots[i]];
            uint8_t disc = L.FsDisc, mask = L.FsMask, heat = L.FsHeat;
            if (item >= TMI_FsFree && item <= TMI_FsHold)
                disc = (uint8_t)(item - TMI_FsFree);
            else if (item >= TMI_FsNormal && item <= TMI_FsAlpha)
                heat = (uint8_t)(item - TMI_FsNormal);
            else if (cat >= 0)
                mask = catOn ? (uint8_t)(mask | (1u << cat)) : (uint8_t)(mask & ~(1u << cat));
            else
            {
                disc = 0;
                mask = kFcAll;
                heat = 0;
            }
            L.FsDisc = disc;
            L.FsMask = mask;
            L.FsHeat = heat;
            L.FsSentMs = NowMs();
            L.FsPending = true;
            def = def && disc == 0 && mask == kFcAll && heat == 0;
            const float v[3] = {(float)disc, (float)mask, (float)heat};
            TacSend(VanillaExportBlock::TCO_FireSettings, tacFsSlots[i], v);
        }
        TacSay(def ? L"Fire settings: defaults - the game fires" : L"Fire settings sent");
    }

    // The fire-settings item's text, with its tick for the menu's first lancemate.
    std::wstring TacFsText(int item) const
    {
        static const wchar_t *kT[] = {L"Weapons free", L"Return fire", L"Hold fire", L"Heat: normal (85%)", L"Heat: cool (60%)",
                                      L"Heat: hot (95%)", L"Heat: alpha (no limit)", L"Lasers", L"PPCs", L"Ballistics",
                                      L"Lock-on missiles", L"Dumbfire missiles", L"MG / flamers", L"TAG", L"NARC",
                                      L"Reset to defaults"};
        const int k = item - TMI_FsFree;
        if (k < 0 || k > 15)
            return L"";
        if (item == TMI_FsReset || !tacFsCount)
            return kT[k];
        const TacLocal &L = tacLocal[tacFsSlots[0]];
        bool on = false;
        if (item <= TMI_FsHold)
            on = L.FsDisc == (uint8_t)(item - TMI_FsFree);
        else if (item <= TMI_FsAlpha)
            on = L.FsHeat == (uint8_t)(item - TMI_FsNormal);
        else
            on = (L.FsMask >> (item - TMI_FsCat0)) & 1;
        return std::wstring(item >= TMI_FsCat0 ? (on ? L"[x] " : L"[ ] ") : (on ? L"(o) " : L"( ) ")) + kT[k];
    }

    void TacMenuPick(int item)
    {
        if (item == TMI_NavMark || item == TMI_NavUnmark) // NAV POINTS (2026-10-07k): no lancemate needed
        {
            const bool mark = item == TMI_NavMark;
            TacSend(mark ? VanillaExportBlock::TCO_NavMark : VanillaExportBlock::TCO_NavUnmark, TacPlayerSlot(),
                    mark ? tacMenuLoc : tacMenuNavLoc);
            if (mark)
                TacSay(L"Nav point marked");
            else
                TacSay(std::wstring(L"Nav ") + VhudNavName(tacMenuNav) + L" removed");
            return;
        }
        if (TacTorsoItem(item))
        {
            const int slot = TacPlayerSlot();
            static const uint8_t kOrd[] = {VanillaExportBlock::TCO_TorsoAngle, VanillaExportBlock::TCO_TorsoPoint,
                                           VanillaExportBlock::TCO_TorsoTarget, VanillaExportBlock::TCO_TorsoCentre,
                                           VanillaExportBlock::TCO_TorsoRelease};
            static const wchar_t *kSaid[] = {L"Torso: holding that angle", L"Torso: on that point",
                                             L"Torso: tracking the target", L"Torso: centring", L"Torso: released"};
            const int k = item - TMI_TorsoAngle;
            TacSend(kOrd[k], slot, tacMenuLoc);
            TacSay(kSaid[k]);
            return;
        }
        if (item == TMI_Lock) // 2026-10-12: your lock, the exact unit; LanceControl checks again that the lance detects it
        {
            if (!tacMenuActorLo && !tacMenuActorHi)
            {
                TacSay(L"Lock: that unit is not in the lance's list");
                return;
            }
            float loc[3];
            std::memcpy(&loc[0], &tacMenuActorLo, 4);
            std::memcpy(&loc[1], &tacMenuActorHi, 4);
            loc[2] = VanillaExportBlock::kTacLocActor;
            TacSend(VanillaExportBlock::TCO_LockTarget, tacMenuSpotter ? tacMenuSpotter : TacPlayerSlot(), loc);
            TacSay(L"Patching it through - target locked");
            return;
        }
        int slots[8];
        int n = TacSelection(slots, 8);
        if (TacCsItem(item)) // 2026-10-12: a called shot (and the attack) on that unit, for each selected lancemate
        {
            const int part = item - TMI_Cs0;
            int sent = 0;
            float ex[3];
            const float *tl = TacTargetLoc(tacMenuTarget, tacMenuLoc, ex); // the exact unit (lc61)
            for (int i = 0; i < n; ++i)
            {
                if (tacLocal[slots[i]].IsPlayer)
                    continue; // your own guns are yours
                if (part)
                {
                    tacLocal[slots[i]].WpCount = 0;
                    tacLocal[slots[i]].Order = TOS_Attack;
                }
                TacSend(VanillaExportBlock::TCO_CalledShot, slots[i], tl, (uint8_t)part, 0);
                ++sent;
            }
            TacSay(sent ? std::wstring(TacCsText(part)) : std::wstring(L"Select a lancemate first"));
            return;
        }
        if (TacFlItem(item)) // 2026-10-20 (lancecontrol-75): FLANK / PINCER on that unit, each selected lancemate
        {
            float ex[3];
            const float *tl = TacTargetLoc(tacMenuTarget, tacMenuLoc, ex); // the exact unit (lc61)
            int mates[8], m = 0;
            for (int i = 0; i < n; ++i)
                if (!tacLocal[slots[i]].IsPlayer) // your own mech is yours to drive
                    mates[m++] = slots[i];
            if (!m || (item == TMI_Pincer && m < 2))
            {
                TacSay(!m ? L"Select a lancemate first" : L"A pincer needs two lancemates selected");
                return;
            }
            static const uint8_t kPin[3] = {1, 2, 5}; // your left, your right, behind it (then left / right again)
            for (int i = 0; i < m; ++i)
            {
                tacLocal[mates[i]].WpCount = 0;
                tacLocal[mates[i]].Order = TOS_Attack;
                const uint8_t side = item == TMI_Pincer ? (uint8_t)(i < 3 ? kPin[i] : 1 + (i % 2)) | 8 : 0;
                TacSend(VanillaExportBlock::TCO_Flank, mates[i], tl, side, 0);
            }
            TacSay(item == TMI_Pincer ? L"Pincer - left, right" + std::wstring(m > 2 ? L", rear" : L"") : std::wstring(L"Flank it"));
            return;
        }
        if (item == TMI_Focus)
        {
            // The whole lance, whatever is selected.
            n = 0;
            for (int i = 0; i < tacView.MateCount && n < 8; ++i)
                if (!tacView.Mates[i].Dead)
                    slots[n++] = tacView.Mates[i].Slot;
        }
        if (!n)
        {
            TacSay(L"Select a lancemate first");
            return;
        }
        float tgtBuf[3];
        const float *tgtLoc = (item == TMI_Attack || item == TMI_Focus || item == TMI_Engage)
                                  ? TacTargetLoc(tacMenuTarget, tacMenuLoc, tgtBuf) // the exact unit (lc61)
                                  : tacMenuLoc;
        switch (item)
        {
        case TMI_Move:
            TacMoveTo(tacMenuLoc, false, TWA_None);
            break;
        case TMI_MoveFire:
            TacMoveTo(tacMenuLoc, false, TWA_None, 1);
            break;
        case TMI_Fallback:
            TacMoveTo(tacMenuLoc, false, TWA_None, 2);
            break;
        case TMI_IndirectCover:
            TacMoveTo(tacMenuLoc, false, TWA_None, 4);
            break;
        case TMI_MoveHold:
            TacMoveTo(tacMenuLoc, false, TWA_Hold);
            break;
        case TMI_Waypoint:
            TacMoveTo(tacMenuLoc, true, TWA_None);
            break;
        case TMI_Form:
            for (int i = 0; i < n; ++i)
            {
                if (tacLocal[slots[i]].IsPlayer)
                    continue; // you cannot form on yourself
                TacStop(slots[i]);
                tacLocal[slots[i]].Order = TOS_Follow;
                TacSend(VanillaExportBlock::TCO_Follow, slots[i], nullptr);
            }
            TacSay(L"Form on me");
            break;
        case TMI_Cancel:
            for (int i = 0; i < n; ++i)
            {
                TacStop(slots[i]);
                tacLocal[slots[i]].Order = TOS_None;
            }
            TacSay(L"Route cancelled");
            break;
        case TMI_Attack:
        case TMI_Focus:
            for (int i = 0; i < n; ++i)
            {
                if (tacLocal[slots[i]].IsPlayer)
                    continue; // your own guns are yours
                tacLocal[slots[i]].WpCount = 0;
                tacLocal[slots[i]].Order = TOS_Attack;
                TacSend(VanillaExportBlock::TCO_Attack, slots[i], tgtLoc);
            }
            TacSay(item == TMI_Focus ? L"Focus fire - whole lance" : L"Attack");
            break;
        case TMI_Engage:
            // Each to a point short of the target on its own line of approach, then attack.
            for (int i = 0; i < n; ++i)
            {
                const float *from = TacMateLoc(slots[i]);
                if (!from)
                    continue;
                float dx = tacMenuLoc[0] - from[0], dy = tacMenuLoc[1] - from[1];
                const float len = std::sqrt(dx * dx + dy * dy);
                TacLocal &L = tacLocal[slots[i]];
                L.WpCount = 0;
                if (len <= kTacEngageCm + kTacArriveCm)
                {
                    if (L.IsPlayer)
                        continue;
                    L.Order = TOS_Attack;
                    TacSend(VanillaExportBlock::TCO_Attack, slots[i], tgtLoc);
                    continue;
                }
                dx /= len;
                dy /= len;
                float p[3] = {tacMenuLoc[0] - dx * kTacEngageCm, tacMenuLoc[1] - dy * kTacEngageCm, tacMenuLoc[2]};
                topo3d.TacGroundAt(p[0], p[1], p[2]);
                std::memcpy(L.Wp[0], p, sizeof(p));
                L.WpAct[0] = L.IsPlayer ? TWA_None : TWA_Attack;
                L.WpTarget[0] = tacMenuTarget;
                L.WpCount = 1;
                L.Order = TOS_Move;
                TacSendMove(slots[i], p);
            }
            TacSay(L"Move to engage");
            break;
        }
    }

    // Where member i of n stands in the formation, in the frame of the move: `fwd` along the
    // direction of travel, `side` across it, metres. The destination is the formation's front.
    void TacFormationSlot(int i, int n, float s, float &fwd, float &side) const
    {
        switch (tacFormation)
        {
        case TF_Column:
            fwd = -(float)i * s;
            side = 0.0f;
            return;
        case TF_Wedge: {
            // The point at the destination, the rest paired back on either side.
            const int row = (i + 1) / 2;
            fwd = -(float)row * s * 0.8f;
            side = (i == 0) ? 0.0f : ((i & 1) ? 1.0f : -1.0f) * (float)row * s;
            return;
        }
        case TF_Box: {
            const int cols = (int)std::ceil(std::sqrt((float)n));
            const int rows = (n + cols - 1) / cols;
            const int r = i / cols, c = i % cols;
            fwd = (0.5f * (float)(rows - 1) - (float)r) * s;
            side = ((float)c - 0.5f * (float)(cols - 1)) * s;
            return;
        }
        default: // line abreast
            fwd = 0.0f;
            side = ((float)i - 0.5f * (float)(n - 1)) * s;
            return;
        }
    }

    // A move for the selection, laid out in the chosen formation. `act` is done on arrival.
    // moveMode: 0 plain, 1 fire on the move, 2 fall back facing the enemy.
    void TacMoveTo(const float *pt, bool append, int act, int moveMode = 0)
    {
        int slots[8];
        const int n = TacSelection(slots, 8);
        if (!n)
            return;
        float cx = 0, cy = 0;
        for (int i = 0; i < n; ++i)
        {
            const TacLocal &L = tacLocal[slots[i]];
            const float *here = TacMateLoc(slots[i]);
            // From where it will be when this leg starts: its last waypoint when appending.
            const float *from = (append && L.WpCount > 0) ? L.Wp[L.WpCount - 1] : here;
            cx += from ? from[0] : pt[0];
            cy += from ? from[1] : pt[1];
        }
        cx /= (float)n;
        cy /= (float)n;
        float dx = pt[0] - cx, dy = pt[1] - cy;
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len > 1.0f) { dx /= len; dy /= len; } else { dx = 1.0f; dy = 0.0f; }
        const float px = -dy, py = dx; // across the direction of travel
        const float sp = TacSpacingM(tacSpacing) * 100.0f;
        for (int i = 0; i < n; ++i)
        {
            float fwd, side;
            TacFormationSlot(i, n, sp, fwd, side);
            float p[3] = {pt[0] + dx * fwd + px * side, pt[1] + dy * fwd + py * side, pt[2]};
            // Its own ground, not the click's: an offset point on a slope or past an edge is at
            // another height, and the plugin looks for walkable ground near THIS height first.
            topo3d.TacGroundAt(p[0], p[1], p[2]);
            TacLocal &L = tacLocal[slots[i]];
            if (!append)
            {
                L.WpCount = 0;
                L.MoveMode = moveMode; // a new route decides; waypoints added to it keep its mode
            }
            if (L.WpCount >= 8)
                continue;
            std::memcpy(L.Wp[L.WpCount], p, sizeof(p));
            L.WpAct[L.WpCount] = act;
            L.WpTarget[L.WpCount] = 0;
            ++L.WpCount;
            L.Order = TOS_Move;
            if (L.WpCount == 1)
                TacSendMove(slots[i], p);
        }
        TacSay(append ? L"Waypoint added"
                      : (act == TWA_Hold ? L"Beeline and hold"
                                         : (moveMode == 1 ? L"Move - fire on the move"
                                                          : (moveMode == 2 ? L"Fall back - facing the enemy"
                                                                           : (moveMode == 4 ? L"Hold position - indirect fire only"
                                                                                            : L"Beeline")))));
    }

    void TacButtonPress(int b)
    {
        if (b == TB_All)
        {
            for (int i = 0; i < tacView.MateCount; ++i)
                tacLocal[tacView.Mates[i].Slot].Selected = !tacView.Mates[i].Dead;
            TacSay(L"Whole lance selected");
            return;
        }
        if (!TacSelectedCount())
        {
            TacSay(L"Select a lancemate first");
            return;
        }
        int orders = 0;
        for (int i = 0; i < tacView.MateCount; ++i)
        {
            const auto &m = tacView.Mates[i];
            TacLocal &L = tacLocal[m.Slot];
            if (!L.Selected || m.Dead)
                continue;
            switch (b)
            {
            case TB_Form:
                if (L.IsPlayer)
                    break;
                TacStop(m.Slot);
                L.Order = TOS_Follow;
                TacSend(VanillaExportBlock::TCO_Follow, m.Slot, nullptr);
                ++orders;
                break;
            case TB_Hold:
                TacStop(m.Slot); // the player's autopilot lets go here
                L.Order = TOS_Hold;
                if (!L.IsPlayer)
                    TacSend(VanillaExportBlock::TCO_Move, m.Slot, m.Loc);
                ++orders;
                break;
            case TB_Nearest: {
                if (L.IsPlayer)
                    break;
                float hit[3], ex[3];
                if (const int hid = topo3d.TacNearestHostile(m.Loc, 1000.0f, hit))
                {
                    L.WpCount = 0;
                    L.Order = TOS_Attack;
                    TacSend(VanillaExportBlock::TCO_Attack, m.Slot, TacTargetLoc(hid, hit, ex)); // the exact unit (lc61)
                    ++orders;
                }
                break;
            }
            case TB_Fire:
                if (L.IsPlayer)
                    break;
                TacSend(VanillaExportBlock::TCO_HoldFire, m.Slot, nullptr);
                ++orders;
                break;
            }
        }
        static const wchar_t *kSaid[TB_COUNT] = {L"", L"Form on me", L"Hold position", L"Attack nearest",
                                                 L"Hold fire / weapons free"};
        TacSay(orders ? kSaid[b] : L"No known enemy within 1000 m");
    }

    void TacMiddleDown(float x, float y)
    {
        tacMenuOpen = false;
        tacPanning = true;
        tacPanX = x;
        tacPanY = y;
        if (!ovOn)
            SetCapture(hwnd);
    }

    void TacMiddleUp()
    {
        const bool was = tacPanning;
        tacPanning = false;
        if (was && GetCapture() == hwnd) // only its own capture, not another drag's
            ReleaseCapture();
    }

    // Everything the mouse can be in the middle of on a page: TacOps drags (box, pan, torso
    // handle), its right-click menu, and the 3D camera's orbit / pan. Ended - not completed:
    // no order is sent - when the page changes, the options menu opens or the overlay closes.
    void EndPageInteractions()
    {
        TacEndDrags();
        tacMenuOpen = false;
        CamMouseEnd();
        if (GetCapture() == hwnd && dragSlot < 0)
            ReleaseCapture();
    }

    void TacEndDrags()
    {
        tacTorsoDrag = false;
        tacPanning = tacDragging = tacDragArmed = false;
    }

    void TacButtonBox(const D2D1_RECT_F &r, const wchar_t *text, bool lit)
    {
        SetUiColor(lit ? 0x163A33 : 0x05080A, 0.88f);
        d2dCtx->FillRectangle(r, uiBrush.Get());
        SetUiColor(lit ? 0xFFFFFF : 0x3FE6CD, 0.9f);
        d2dCtx->DrawRectangle(r, uiBrush.Get(), lit ? 1.8f : 1.2f);
        DrawPanelText(r.left + 6.0f, r.top + 3.0f, r.right - r.left - 8.0f, text, lit ? 0xFFFFFF : 0x3FE6CD, 1.0f);
    }

    // Over the page: the buttons, the formation panel, the drag box, the menu and the status.
    // =====================================================================================
    // THE HEADING COMPASS (2026-10-12, user's design). For heading orders by voice. On the 3D and TacOps maps:
    //   - a heading strip along the top (in place of the game's captured compass), centred on your cockpit heading
    //     (body + torso twist, so it turns with the torso slew, redrawn every frame), +-50 degrees, with green dots
    //     for lancemates and red for enemies under it (dimmed at the edge when off the strip);
    //   - a compass ring on the ground at 1 km (N in amber, every 30 degrees labelled) and range rings at 250 /
    //     500 m, laid on the terrain like the lancemate rings;
    //   - clock positions 12 / 3 / 6 / 9 at 380 m, from where the torso faces ("move to my 3 o'clock").
    // Bearings are the voice orders' own: X north, Y east, degrees from north towards east. Each part has a
    // GlobalSettings switch (gs::CompassOpts); the CMPS bezel button (compassOn) hides the lot on this window.
    // Called inside an open BeginDraw.
    // =====================================================================================
    void DrawCompass(bool tacops)
    {
        if (!compassOn || !d2dCtx)
            return;
        const VRect R = ContentRect();
        const float right = tacops ? topo3d.TacPanelLeft() - 10.0f : R.x + R.w;
        const float hdg = topo3d.CompassHeadingDeg();
        const float k = 0.01745329252f;
        auto wrap = [](float d) { d = std::fmod(d, 360.0f); if (d > 180.0f) d -= 360.0f; if (d < -180.0f) d += 360.0f; return d; };
        auto deg3 = [](float d) {
            wchar_t b[8];
            swprintf_s(b, L"%03d", ((int)std::lround(d) % 360 + 360) % 360);
            return std::wstring(b);
        };
        float me[3];
        const bool haveMe = topo3d.CompassPlayerCm(me);
        auto at = [&](float bearing, float rM, float out[3]) {
            out[0] = me[0] + std::cos(bearing * k) * rM * 100.0f;
            out[1] = me[1] + std::sin(bearing * k) * rM * 100.0f;
            out[2] = me[2];
        };
        if (haveMe && (compassOpts.TacRings || compassOpts.TacClock))
        {
            d2dCtx->PushAxisAlignedClip(D2D1::RectF(R.x, R.y, right, R.y + R.h), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            if (compassOpts.TacRings)
            {
                static const float kRings[3] = {250.0f, 500.0f, 1000.0f};
                static const wchar_t *kRingName[3] = {L"250 m", L"500 m", L"1 km"};
                for (int ri = 0; ri < 3; ++ri)
                {
                    const bool outer = ri == 2;
                    SetUiColor(0x3FE6CD, outer ? 0.7f : 0.3f);
                    float px = 0.0f, py = 0.0f;
                    bool prev = false;
                    for (int i = 0; i <= 72; ++i)
                    {
                        float p[3], sx, sy;
                        at((float)i * 5.0f, kRings[ri], p);
                        const bool ok = topo3d.TacProject(p, sx, sy);
                        if (ok && prev)
                            d2dCtx->DrawLine(D2D1::Point2F(px, py), D2D1::Point2F(sx, sy), uiBrush.Get(), outer ? 1.4f : 1.0f);
                        px = sx;
                        py = sy;
                        prev = ok;
                    }
                    float p[3], sx, sy;
                    at(hdg + 4.0f, kRings[ri], p);
                    if (topo3d.TacProject(p, sx, sy))
                        DrawPanelText(sx + 4.0f, sy - 20.0f, 60.0f, kRingName[ri], 0x3FE6CD, 0.6f);
                }
                for (int d = 0; d < 360; d += 5)
                {
                    const float len = (d % 30 == 0) ? 70.0f : ((d % 10 == 0) ? 40.0f : 20.0f);
                    float p0[3], p1[3], x0, y0, x1, y1;
                    at((float)d, 1000.0f, p0);
                    at((float)d, 1000.0f - len, p1);
                    if (!topo3d.TacProject(p0, x0, y0) || !topo3d.TacProject(p1, x1, y1))
                        continue;
                    SetUiColor(d == 0 ? 0xFFC04D : 0x3FE6CD, d == 0 ? 1.0f : 0.85f);
                    d2dCtx->DrawLine(D2D1::Point2F(x0, y0), D2D1::Point2F(x1, y1), uiBrush.Get(), d == 0 ? 3.0f : 1.4f);
                    if (d % 30 != 0)
                        continue;
                    float pl[3], lx, ly;
                    at((float)d, 1080.0f, pl);
                    if (!topo3d.TacProject(pl, lx, ly))
                        continue;
                    const bool card = d % 90 == 0;
                    const std::wstring lab = card ? std::wstring(d == 0 ? L"N" : (d == 90 ? L"E" : (d == 180 ? L"S" : L"W"))) : deg3((float)d);
                    DrawPanelText(lx - (card ? 5.0f : 13.0f), ly - 11.0f, 44.0f, lab.c_str(),
                                  d == 0 ? 0xFFC04D : (card ? 0x9CFFC0 : 0x3FE6CD), 0.95f);
                }
            }
            if (compassOpts.TacClock)
            {
                static const wchar_t *kClock[4] = {L"12", L"3", L"6", L"9"};
                for (int q = 0; q < 4; ++q)
                {
                    float p[3], sx, sy;
                    at(hdg + 90.0f * (float)q, 380.0f, p);
                    if (topo3d.TacProject(p, sx, sy))
                        DrawPanelText(sx - (q == 0 ? 7.0f : 4.0f), sy - 11.0f, 30.0f, kClock[q], 0x9CFFC0, 0.55f);
                }
            }
            d2dCtx->PopAxisAlignedClip();
        }
        if (!compassOpts.TacStrip)
            return;
        // The strip: +-50 degrees about the heading.
        const float tw = (std::min)(520.0f, (right - R.x) * 0.8f), th = 30.0f;
        const float tcx = (R.x + right) * 0.5f, ttop = R.y + 8.0f;
        const float ppd = (tw * 0.5f) / 50.0f;
        const D2D1_RECT_F tape = D2D1::RectF(tcx - tw * 0.5f, ttop, tcx + tw * 0.5f, ttop + th);
        SetUiColor(0x05080A, 0.85f);
        d2dCtx->FillRectangle(tape, uiBrush.Get());
        SetUiColor(0x3FE6CD, 0.5f);
        d2dCtx->DrawLine(D2D1::Point2F(tape.left, tape.bottom), D2D1::Point2F(tape.right, tape.bottom), uiBrush.Get(), 1.0f);
        for (int d = (int)std::ceil((hdg - 50.0f) / 5.0f) * 5; d <= (int)std::floor(hdg + 50.0f); d += 5)
        {
            const float x = tcx + ((float)d - hdg) * ppd;
            const bool major = ((d % 10) + 10) % 10 == 0;
            SetUiColor(0x3FE6CD, 1.0f);
            d2dCtx->DrawLine(D2D1::Point2F(x, tape.bottom), D2D1::Point2F(x, tape.bottom - (major ? 11.0f : 6.0f)), uiBrush.Get(), 1.2f);
            if (!major || std::fabs(x - tcx) < 34.0f)
                continue;
            const int dd = ((d % 360) + 360) % 360;
            const bool card = dd % 90 == 0;
            const std::wstring lab = card ? std::wstring(dd == 0 ? L"N" : (dd == 90 ? L"E" : (dd == 180 ? L"S" : L"W"))) : deg3((float)dd);
            DrawPanelText(x - (card ? 5.0f : 13.0f), ttop - 1.0f, 44.0f, lab.c_str(), dd == 0 ? 0xFFC04D : (card ? 0x9CFFC0 : 0x3FE6CD), 0.95f);
        }
        const D2D1_RECT_F box = D2D1::RectF(tcx - 28.0f, ttop - 4.0f, tcx + 28.0f, ttop + 24.0f);
        SetUiColor(0x070D0B, 1.0f);
        d2dCtx->FillRectangle(box, uiBrush.Get());
        SetUiColor(0xFFFFFF, 1.0f);
        d2dCtx->DrawRectangle(box, uiBrush.Get(), 1.5f);
        DrawPanelText(tcx - 15.0f, ttop - 1.0f, 50.0f, deg3(hdg).c_str(), 0xFFFFFF, 1.0f);
        if (!compassOpts.Dots || !haveMe)
            return;
        auto dot = [&](const float p[3], UINT32 col, float y) {
            const float dx = p[0] - me[0], dy = p[1] - me[1];
            if (dx * dx + dy * dy < 100.0f * 100.0f)
                return;
            const float w = wrap(std::atan2(dy, dx) / k - hdg);
            const bool on = std::fabs(w) <= 50.0f;
            const float x = on ? tcx + w * ppd : (w < 0.0f ? tape.left + 4.0f : tape.right - 4.0f);
            SetUiColor(col, on ? 1.0f : 0.4f);
            d2dCtx->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, y), 3.5f, 3.5f), uiBrush.Get());
        };
        // bug sweep 18 (2026-10-18): the lancemates from the lance read itself - tacView is only rebuilt while the TacOps
        // page is up, so the 3D page's dots stood where the mates were when TacOps was last shown (or were missing).
        if (!tacops)
            TacReadMembers(false);
        for (int i = 0; i < tacMemberCount; ++i)
            if (!tacMembers[i].IsPlayer && !tacMembers[i].Dead)
                dot(tacMembers[i].Loc, 0x5DFFA0, tape.bottom + 6.0f);
        float hostile[64][3];
        const int nh = topo3d.CompassHostilesCm(hostile, 64);
        for (int i = 0; i < nh; ++i)
            dot(hostile[i], 0xFF5A4D, tape.bottom + 15.0f);
    }

    void DrawTacOpsOverlays()
    {
        const VRect R = ContentRect();
        const float right = topo3d.TacPanelLeft() - 10.0f;
        const float bw = 96.0f, bh = 26.0f, gap = 8.0f;
        const float by = R.y + R.h - bh - 8.0f;
        (void)gap;
        d2dCtx->BeginDraw();
        // The bottom-left buttons (ALL, FORM, HOLD, NEAREST, FIRE) are no longer drawn, at the
        // user's request. Their rects stay empty so a click there falls through to the map;
        // TacButtonPress is kept for when any of them moves into the right-click menu.
        for (int b = 0; b < TB_COUNT; ++b)
            tacBtn[b] = D2D1::RectF(0, 0, 0, 0);

        // The formation panel, under the lancemate plates.
        {
            const float px0 = topo3d.TacPanelLeft(), pw = Topo3DView::kTacPanelW;
            const int plates = tacView.MateCount;
            float py0 = R.y + Topo3DView::kTacPanelTop +
                        (Topo3DView::kTacPanelH + Topo3DView::kTacPanelGap) * (float)plates + 6.0f;
            const float ph = 26.0f + 2.0f * (bh + 6.0f) + bh + 10.0f;
            if (py0 + ph > R.y + R.h - 4.0f)
                py0 = R.y + R.h - 4.0f - ph;
            SetUiColor(0x05080A, 0.85f);
            d2dCtx->FillRectangle(D2D1::RectF(px0 - 3.0f, py0 - 3.0f, px0 + pw + 3.0f, py0 + ph), uiBrush.Get());
            SetUiColor(0x3FE6CD, 0.5f);
            d2dCtx->DrawRectangle(D2D1::RectF(px0 - 3.0f, py0 - 3.0f, px0 + pw + 3.0f, py0 + ph), uiBrush.Get(), 1.0f);
            DrawPanelText(px0 + 2.0f, py0, pw, L"FORMATION", 0x3FE6CD, 0.95f);
            static const wchar_t *kForm[TF_COUNT] = {L"LINE", L"COLUMN", L"WEDGE", L"BOX"};
            const float fw = (pw - 6.0f) * 0.5f;
            for (int f = 0; f < TF_COUNT; ++f)
            {
                const float fx = px0 + (float)(f & 1) * (fw + 6.0f);
                const float fy = py0 + 24.0f + (float)(f >> 1) * (bh + 6.0f);
                tacFormBtn[f] = D2D1::RectF(fx, fy, fx + fw, fy + bh);
                TacButtonBox(tacFormBtn[f], kForm[f], tacFormation == f);
            }
            const float sy = py0 + 24.0f + 2.0f * (bh + 6.0f);
            tacSpaceBtn = D2D1::RectF(px0, sy, px0 + pw, sy + bh);
            const std::wstring sp = L"SPACING " + std::to_wstring((int)TacSpacingM(tacSpacing)) + L" m";
            TacButtonBox(tacSpaceBtn, sp.c_str(), false);
        }

        DrawCompass(true); // 2026-10-12: the heading compass, under the drag box and the menu

        if (tacDragging)
        {
            const D2D1_RECT_F r = D2D1::RectF((std::min)(tacDX0, tacDX1), (std::min)(tacDY0, tacDY1),
                                              (std::max)(tacDX0, tacDX1), (std::max)(tacDY0, tacDY1));
            SetUiColor(0x5DFFA0, 0.12f);
            d2dCtx->FillRectangle(r, uiBrush.Get());
            SetUiColor(0x5DFFA0, 0.9f);
            d2dCtx->DrawRectangle(r, uiBrush.Get(), 1.0f);
        }

        if (tacMenuOpen)
        {
            static const wchar_t *kItem[] = {L"Beeline here", L"Beeline here and hold", L"Add waypoint", L"Form on me",
                                             L"Cancel route", L"Attack", L"Focus fire (whole lance)", L"Move to engage",
                                             L"Move (fire on the move)", L"Fall back here (face enemy)",
                                             L"Hold position (indirect fire only)",
                                             L"Torso: hold this angle", L"Torso: aim at this point",
                                             L"Torso: track this target", L"Torso: centre", L"Torso: release"};
            const float mw = 250.0f, ih = 24.0f; // wide enough for "Hold position (indirect fire only)"
            float mx = tacMenuX + 4.0f, my = tacMenuY + 4.0f;
            if (mx + mw > R.x + R.w)
                mx = tacMenuX - mw - 4.0f;
            if (my + ih * (float)tacMenuCount > R.y + R.h)
                my = tacMenuY - ih * (float)tacMenuCount - 4.0f;
            for (int i = 0; i < tacMenuCount; ++i)
            {
                tacMenuRect[i] = D2D1::RectF(mx, my + ih * (float)i, mx + mw, my + ih * (float)(i + 1));
                SetUiColor(0x05080A, 0.93f);
                d2dCtx->FillRectangle(tacMenuRect[i], uiBrush.Get());
                const bool cs = TacCsItem(tacMenuItems[i]) || tacMenuItems[i] == TMI_Lock || tacMenuItems[i] == TMI_CalledShotMenu ||
                                TacFlItem(tacMenuItems[i]);
                const bool enemy = (tacMenuItems[i] >= TMI_Attack && tacMenuItems[i] <= TMI_Engage) || cs;
                const bool off = tacMenuDisabled[i]; // greyed out (2026-10-12)
                const bool torso = TacTorsoItem(tacMenuItems[i]); // yours, not the lance's: its own colour
                const bool fire = TacFsItem(tacMenuItems[i]);     // fire settings: amber
                const bool nav = tacMenuItems[i] == TMI_NavMark || tacMenuItems[i] == TMI_NavUnmark; // nav points: violet
                SetUiColor(off ? 0x606060 : enemy ? 0xFF5A4D : (torso ? 0x7DFF6A : (fire ? 0xFFC04D : (nav ? 0xE07BFF : 0x3FE6CD))), 0.8f);
                d2dCtx->DrawRectangle(tacMenuRect[i], uiBrush.Get(), 1.0f);
                const std::wstring ft = fire ? TacFsText(tacMenuItems[i])
                                        : nav ? (tacMenuItems[i] == TMI_NavMark ? std::wstring(L"Mark a nav point here")
                                                                               : std::wstring(L"Remove Nav ") + VhudNavName(tacMenuNav))
                                        : tacMenuItems[i] == TMI_Lock ? std::wstring(off ? L"Lock (not detected)" : L"Lock this enemy")
                                        : tacMenuItems[i] == TMI_CalledShotMenu ? std::wstring(L"Called shot >")
                                        : tacMenuItems[i] == TMI_Flank ? std::wstring(L"Flank it")
                                        : tacMenuItems[i] == TMI_Pincer ? std::wstring(L"Pincer it (2+ selected)")
                                        : TacCsItem(tacMenuItems[i]) ? std::wstring(TacCsText(tacMenuItems[i] - TMI_Cs0))
                                              : std::wstring();
                DrawPanelText(mx + 8.0f, tacMenuRect[i].top + 2.0f, mw - 12.0f, (fire || nav || cs) ? ft.c_str() : kItem[tacMenuItems[i]],
                              off ? 0x707070 : enemy ? 0xFF8A80 : (torso ? 0x7DFF6A : (fire ? 0xFFC04D : (nav ? 0xE07BFF : 0x3FE6CD))), 1.0f);
            }
            // Where the menu's order will land.
            SetUiColor(0xFFFFFF, 0.9f);
            d2dCtx->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(tacMenuX, tacMenuY), 6.0f, 6.0f), uiBrush.Get(), 1.5f);
        }

        std::wstring line;
        if (!vBlock)
            line = L"VanillaHud plugin not connected";
        else if (!tacMemberCount)
            line = L"No lance - waiting for the plugin (in a mission?)";
        else if (!tacStatus.empty() && NowMs() - tacStatusMs < 3000.0)
            line = tacStatus;
        // No standing "N selected" hint any more (user's request): the line is only there when
        // something needs saying - plugin missing, no lance, or a 3 s order/feedback message.
        if (!line.empty())
            DrawPanelText(R.x + 8.0f, by + bh - 22.0f, right - R.x - 16.0f, line.c_str(), 0x3FE6CD, 0.95f);
        // The torso being dragged: a line from the mech to the cursor, where it is being turned to.
        if (tacTorsoDrag)
        {
            float ox, oy, tx, ty;
            if (topo3d.TacTorsoArrow(ox, oy, tx, ty))
            {
                SetUiColor(0x3FE6CD, 0.8f);
                d2dCtx->DrawLine(D2D1::Point2F(ox, oy), D2D1::Point2F(tacTorsoX, tacTorsoY), uiBrush.Get(), 1.5f);
                d2dCtx->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(tacTorsoX, tacTorsoY), 7.0f, 7.0f), uiBrush.Get(), 1.5f);
            }
        }
        d2dCtx->EndDraw();

        // Your own plate: the game's own-mech paperdoll, the same element the V-OWN page shows.
        // Nothing extra is read in the game for this - the element is already being exported.
        float ol, ot, orr, ob;
        if (topo3d.TacOwnPlate(ol, ot, orr, ob))
            DrawSource(VS_Owner, {ol, ot, orr - ol, ob - ot});
    }

    // Over the 3D page: the compass centred along the top, torso twist in the bottom-right corner
    // (the 3D page's own cue panel owns the bottom-left).
    void DrawTacticalOverlays()
    {
        PollVanillaMMF();
        if (!vBlock)
            return;
        LogVanillaDiag();
        const VRect R = ContentRect();
        // 2026-10-12: our heading strip replaces the game's captured compass when it is on.
        if (!(compassOn && compassOpts.TacStrip))
            DrawSource(VS_Compass, {R.x + R.w * 0.22f, R.y + R.h * 0.01f, R.w * 0.56f, R.h * 0.12f});
        if (compassOn && d2dCtx)
        {
            d2dCtx->BeginDraw();
            DrawCompass(false);
            d2dCtx->EndDraw();
        }
        DrawSource(VS_Torso, {R.x + R.w * 0.79f, R.y + R.h * 0.76f, R.w * 0.20f, R.h * 0.23f});
    }

    // ---- the Targeting screen ------------------------------------------------------------
    // Drawn, not captured: HUDDisplay's own 3D map from the pilot's camera (true scale, green,
    // ground and rounds only), the game's real meshes as outlines from the plugin's depth capture,
    // and the rest of the HUD - crosshair, lock ring, hit arrows, prompts, world markers - laid on
    // top at the same crop. Zoom narrows the field of view about the centre.

    // =====================================================================================
    // ENHANCED IMAGING VISION MODE (VHUD v22). Enhanced Imaging full screen over the game, in place of
    // the view (flat screen): toggled from any window (ACT_EiVision - bezel button / its hotkey) or by
    // VoiceCommand ("activate / deactivate enhanced imaging") through vBlock->EiVisionOn. ONE window
    // renders it - the holder of Local\MW5_HUDDisplay_EiVision - into a shared image at the game's
    // size, whatever page that window itself shows; CameraCapture draws it over the game, opaque. 1x
    // only for now (the game camera's own zoom is a later step).
    // =====================================================================================
    HANDLE eiVisMutex = nullptr;
    bool   eiVisOwner = false;
    double eiVisMutexMs = 0.0;
    ComPtr<ID3D11Texture2D>        eiVisTex;
    ComPtr<ID3D11RenderTargetView> eiVisRtv;
    ComPtr<ID2D1Bitmap1>           eiVisBmp;
    HANDLE eiVisHandle = nullptr;
    UINT   eiVisW = 0, eiVisH = 0;
    bool   eiVisLoggedOn = false;
    // While rendering the vision image, ContentRect is the whole image (no bezel inset).
    float  renderOverW = 0.0f, renderOverH = 0.0f;

    void EiVisionToggle()
    {
        PollVanillaMMF();
        if (!vBlock)
        {
            Log("Enhanced Imaging vision mode: the plugin is not connected");
            return;
        }
        vBlock->EiVisionOn = vBlock->EiVisionOn ? 0u : 1u;
        Log(std::string("Enhanced Imaging vision mode ") + (vBlock->EiVisionOn ? "ON" : "off"));
    }

    bool EiVisOwnerCheck()
    {
        if (eiVisOwner)
            return true;
        const double now = NowMs();
        if (now - eiVisMutexMs < 1000.0)
            return false;
        eiVisMutexMs = now;
        if (!eiVisMutex)
            eiVisMutex = CreateMutexA(nullptr, FALSE, "Local\\MW5_HUDDisplay_EiVision");
        if (!eiVisMutex)
            return false;
        const DWORD r = WaitForSingleObject(eiVisMutex, 0);
        if (r == WAIT_OBJECT_0 || r == WAIT_ABANDONED)
        {
            eiVisOwner = true;
            Log("Enhanced Imaging vision mode: this window renders it");
        }
        return eiVisOwner;
    }

    void EiVisRelease()
    {
        if (vBlock && eiVisHandle && vBlock->EiVisHandle == (uint64_t)(uintptr_t)eiVisHandle) // ours only: the plugin may own it now
            vBlock->EiVisHandle = 0;
        eiVisBmp.Reset();
        eiVisRtv.Reset();
        eiVisTex.Reset();
        eiVisHandle = nullptr;
        eiVisW = eiVisH = 0;
    }

    // After the window's own frame: the vision image, when it is on and this window renders it.
    void EiVisionFrame()
    {
        // v23: EiVisWant (CameraCapture: on, or the closing wave still running), and only while EnhancedImaging.dll
        // is not rendering it itself (its heartbeat, EiVisPluginSec, within 3 s).
        const uint32_t nowSec = (uint32_t)(GetTickCount64() / 1000ull);
        const bool pluginRenders = vBlock && vBlock->EiVisPluginSec && nowSec - vBlock->EiVisPluginSec <= 3;
        const bool want = vBlock && vBlock->EiVisWant && !pluginRenders;
        if (!want || !d2dAvailable || !d2dCtx || !EiVisOwnerCheck())
        {
            if (eiVisTex && !want)
            {
                EiVisRelease();
                eiVisLoggedOn = false;
            }
            return;
        }
        UINT W = vBlock->GameW, H = vBlock->GameH;
        if (W < 64 || H < 64 || W > 8192 || H > 8192)
        {
            W = 1920;
            H = 1080;
        }
        if (!eiVisTex || eiVisW != W || eiVisH != H)
        {
            EiVisRelease();
            D3D11_TEXTURE2D_DESC d{};
            d.Width = W;
            d.Height = H;
            d.MipLevels = 1;
            d.ArraySize = 1;
            d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            d.SampleDesc.Count = 1;
            d.Usage = D3D11_USAGE_DEFAULT;
            d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            d.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
            ComPtr<IDXGIResource> r;
            ComPtr<IDXGISurface> surf;
            const D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
                D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            if (FAILED(device->CreateTexture2D(&d, nullptr, eiVisTex.GetAddressOf())) ||
                FAILED(device->CreateRenderTargetView(eiVisTex.Get(), nullptr, eiVisRtv.GetAddressOf())) ||
                FAILED(eiVisTex.As(&r)) || FAILED(r->GetSharedHandle(&eiVisHandle)) || !eiVisHandle ||
                FAILED(eiVisTex.As(&surf)) || FAILED(d2dCtx->CreateBitmapFromDxgiSurface(surf.Get(), &bp, eiVisBmp.GetAddressOf())))
            {
                Log("Enhanced Imaging vision mode: could not make the shared image");
                EiVisRelease();
                vBlock->EiVisionOn = 0;
                return;
            }
            eiVisW = W;
            eiVisH = H;
            vBlock->EiVisW = W;
            vBlock->EiVisH = H;
            MemoryBarrier();
            vBlock->EiVisHandle = (uint64_t)(uintptr_t)eiVisHandle;
            Log("Enhanced Imaging vision mode: rendering at " + std::to_string(W) + "x" + std::to_string(H));
        }
        ComPtr<IDXGIKeyedMutex> km;
        // Not taken by the plugin yet (game paused / a menu up): take our own image back and
        // overwrite it - waiting for key 0 deadlocked the vision mode after Escape.
        if (FAILED(eiVisTex.As(&km)))
            return;
        if (km->AcquireSync(0, 0) != S_OK) // taken back only after 100 ms untaken (pause / menus)
        {
            const ULONGLONG handed = vBlock->EiVisFrameMs;
            if ((handed && GetTickCount64() - handed < 100) || km->AcquireSync(1, 0) != S_OK)
                return;
        }

        // Point everything at the vision image, render Enhanced Imaging at 1x, point it all back.
        ComPtr<ID3D11RenderTargetView> keepRtv = backBufferRTV;
        ComPtr<ID2D1Image> keepTarget;
        d2dCtx->GetTarget(keepTarget.GetAddressOf());
        const uint8_t keepScreen = vanillaScreen;
        const bool keepVanilla = isVanillaMode;
        const float keepZoom = podZoom;
        const int keepSlot = vPodSlot;

        backBufferRTV = eiVisRtv;
        d2dCtx->SetTarget(eiVisBmp.Get());
        renderOverW = (float)W;
        renderOverH = (float)H;
        vanillaScreen = VSC_Enhanced;
        isVanillaMode = true;
        podZoom = 1.0f;
        topo3d.SaveView(); // bug sweep 21: the window's camera / TacOps state / tile cache, put back below
        topo3d.SetContentInset(0.0f);

        const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        context->ClearRenderTargetView(eiVisRtv.Get(), black);
        lidar.NotifyRendered();
        LidarScene scene;
        lidar.BuildScene(scene, true); // pilot view: every round, whatever the scope's layers
        topo3d.SetTargets(eiVisRtv.Get(), d2dCtx.Get(), W, H);
        topo3d.SetTacOps(nullptr);
        if (PrepareTargeting())
        {
            topo3d.Render(scene);
            DrawTargetingOverlays();
            topo3d.RenderPilotBeams();
            topo3d.SetPilotMissileLock(MlAcquiring(), MlProgress(), MlLocked());
            topo3d.RenderPilotLock(scene);
        }
        topo3d.SetPilotCamera(false, nullptr, nullptr, 0.0f);

        // Back to the window.
        topo3d.SetContentInset(kContentInset);
        backBufferRTV = keepRtv;
        d2dCtx->SetTarget(keepTarget.Get());
        renderOverW = renderOverH = 0.0f;
        vanillaScreen = keepScreen;
        isVanillaMode = keepVanilla;
        podZoom = keepZoom;
        vPodSlot = keepSlot;
        RECT rc;
        GetClientRect(hwnd, &rc);
        topo3d.SetTargets(backBufferRTV.Get(), d2dCtx.Get(), (unsigned)(rc.right - rc.left), (unsigned)(rc.bottom - rc.top));
        topo3d.RestoreView(); // bug sweep 21

        km->ReleaseSync(1);
        vBlock->EiVisFrameMs = GetTickCount64();
        if (!eiVisLoggedOn)
        {
            eiVisLoggedOn = true;
            Log("Enhanced Imaging vision mode: first frame handed to the game");
        }
    }

    // The crop of the game's full view this screen shows: the screen's aspect. (Zoom is optical
    // now - it narrows the capture's and the 3D page's field of view - so it no longer crops.)
    void TargetingCrop(const VRect &R, float xf[4])
    {
        uint32_t w = 0, h = 0;
        if (vBlock)
        {
            const bool ov = vBlock->Slots[VS_Overlay].Active != 0;
            const auto &sl = vBlock->Slots[ov ? VS_Overlay : vPodSlot];
            w = ov ? sl.Width : sl.Width / 2; // the depth texture is a side-by-side pair
            h = sl.Height;
        }
        const float as = (w && h) ? (float)w / (float)h : 16.0f / 9.0f, ar = R.w / R.h;
        float cw = 1.0f, ch = 1.0f;
        if (ar < as)
            cw = ar / as;
        else
            ch = as / ar;
        xf[0] = cw;
        xf[1] = ch;
        xf[2] = 0.5f - cw * 0.5f;
        xf[3] = 0.5f - ch * 0.5f;
    }

    // Before the 3D page renders: point it through the game's camera, narrowed to the crop.
    // Returns false (and the page draws its normal chase view) until a camera has arrived.
    bool PrepareTargeting()
    {
        PollVanillaMMF();
        if (!vBlock)
            return false;
        // v21: Enhanced Imaging at 1x asks for the game-depth image instead of V-POD's capture.
        const bool eiGame = vanillaScreen == VSC_Enhanced && podZoom <= 1.01f;
        vPodSlot = eiGame ? VS_EiDepth : VS_Depth;
        if (eiGame)
            vBlock->EiWantMs = GetTickCount64();
        else
        {
            // v23: Enhanced Imaging zoomed borrows V-POD's capture at the FULL viewport; V-POD keeps half.
            vBlock->TargetingFullRes = (vanillaScreen == VSC_Enhanced) ? 1u : 0u;
            vBlock->TargetingWantMs = GetTickCount64(); // keeps the depth capture and camera running
            vBlock->TargetingZoom   = podZoom;          // the capture narrows its field of view by this
        }
        VanillaExportBlock::Camera cam{};
        bool got = false;
        for (int i = 0; i < 4 && !got; ++i)
        {
            const uint32_t s1 = vBlock->Cam.Seq;
            memcpy(&cam, (const void *)&vBlock->Cam, sizeof(cam));
            const uint32_t s2 = vBlock->Cam.Seq;
            got = (s1 == s2) && !(s1 & 1u) && cam.Valid;
        }
        if (!got)
            return false;
        // The depth image first, with the pose it was rendered from: the terrain is drawn from
        // THAT pose, so grid and outlines match at any zoom (the image trails the live camera by
        // a couple of frames, and at 20x a couple of frames of torso turn is most of the view).
        vDepthSrv = VanillaSrv(vPodSlot, vDepthW, vDepthH);
        float xf[4];
        TargetingCrop(ContentRect(), xf);
        float hfCrop;
        if (vDepthSrv && vDepthCam.Valid)
        {
            const float hf = vDepthCam.FovDeg * 0.01745329252f; // zoom already applied by the plugin
            hfCrop = 2.0f * atanf(tanf(hf * 0.5f) * xf[0]) * 57.29577951f;
            topo3d.SetPilotCamera(true, vDepthCam.Loc, vDepthCam.Rot, hfCrop);
            topo3d.SetPilotLiveLock(vDepthLock.Valid != 0, vDepthLock.Origin, vDepthLock.Extent);
            topo3d.SetPilotBeams((int)vDepthBeamCount, &vDepthBeams[0].From[0]);
        }
        else
        {
            const float hf = cam.FovDeg * 0.01745329252f;
            hfCrop = 2.0f * atanf(tanf(hf * 0.5f) * xf[0] / podZoom) * 57.29577951f;
            topo3d.SetPilotCamera(true, cam.Loc, cam.Rot, hfCrop);
            topo3d.SetPilotLiveLock(false, nullptr, nullptr);
            topo3d.SetPilotBeams(0, nullptr);
        }
        return true;
    }

    void DrawTargetingOverlays()
    {
        if (!vBlock)
            return;
        LogVanillaDiag();
        const VRect R = ContentRect();
        float xf[4];
        TargetingCrop(R, xf);

        uint32_t w, h;
        ID3D11ShaderResourceView *depth = vDepthSrv; // the one PrepareTargeting drew the terrain for
        vDepthSrv = nullptr;
        if (depth && psDepthEdge && vDepthW && vDepthH)
        {
            // params.w: the right half holds a units image (VanillaHudShared v8).
            const float params[4] = {0.0f, 1.0f / (float)vDepthW, 1.0f / (float)vDepthH, vDepthCam.UnitsValid};
            DrawSrv(depth, R, psDepthEdge.Get(), xf, true, params);
        }
        // The HUD overlay and markers are the GAME's view, at the game's field of view. Zoomed, they
        // are laid on unscaled - the crosshair stays centred and its own size - and the world
        // markers, which would sit at their unzoomed places, are left out.
        // 2026-10-08 (VHUD v20): the game's HUD layer and world markers are no longer taken -
        // they stay on the game's own screen - so V-POD draws its own crosshair instead.
        (void)w; (void)h;
        if (d2dCtx && uiBrush && vpodCross)
        {
            d2dCtx->BeginDraw();
            DrawCrosshairLines(R.x + R.w * 0.5f, R.y + R.h * 0.5f, 0x55FF8A);
            d2dCtx->EndDraw();
        }
    }

    // A centred cross with a gap, drawn twice (a dark halo, then the colour). Inside BeginDraw.
    void DrawCrosshairLines(float cx, float cy, UINT32 col)
    {
        const float gap = 6.0f, arm = 18.0f;
        SetUiColor(0x000000, 0.6f);
        for (int pass = 0; pass < 2; ++pass)
        {
            const float wdt = pass ? 1.5f : 3.5f;
            if (pass)
                SetUiColor(col, 0.95f);
            d2dCtx->DrawLine(D2D1::Point2F(cx - gap - arm, cy), D2D1::Point2F(cx - gap, cy), uiBrush.Get(), wdt);
            d2dCtx->DrawLine(D2D1::Point2F(cx + gap, cy), D2D1::Point2F(cx + gap + arm, cy), uiBrush.Get(), wdt);
            d2dCtx->DrawLine(D2D1::Point2F(cx, cy - gap - arm), D2D1::Point2F(cx, cy - gap), uiBrush.Get(), wdt);
            d2dCtx->DrawLine(D2D1::Point2F(cx, cy + gap), D2D1::Point2F(cx, cy + gap + arm), uiBrush.Get(), wdt);
        }
    }

    // ==========================================================================================
    // THE MISSILE LOCK (2026-10-08, VHUD v20). CameraCapture publishes the player's LockOnComponent
    // every frame: state (3 = locked), the lock percent, whether it is on the current target, and
    // that target's bounds. V-POD and CAP turn it into a closing square on the lock bracket and a
    // solid bracket when locked. (Our lock tones were scrapped 2026-10-08c.)
    // ==========================================================================================
    VanillaExportBlock::MissileLockInfo mlNow{};
    bool   mlValid = false;

    bool ReadMissileLock(VanillaExportBlock::MissileLockInfo &m)
    {
        if (!vBlock)
            return false;
        for (int i = 0; i < 4; ++i)
        {
            const uint32_t s1 = vBlock->MissileLock.Seq;
            memcpy(&m, (const void *)&vBlock->MissileLock, sizeof(m));
            const uint32_t s2 = vBlock->MissileLock.Seq;
            if (s1 == s2 && !(s1 & 1u))
                return true;
        }
        return false;
    }
    bool MlLocked() const { return mlValid && mlNow.OnTarget && mlNow.State == kVhudLockLocked; }
    bool MlAcquiring() const
    {
        return mlValid && mlNow.OnTarget && mlNow.State != 0 && mlNow.State != kVhudLockLocked;
    }
    // The game keeps no lock percent on the component (cameracapture-5 found CurrentLockPercent missing -
    // it is only a delegate parameter), so until the plugin finds the timer the closing square runs on
    // time: from the moment acquiring starts, over kMlAssumedLockS; losing (state 2) opens it again.
    static constexpr double kMlAssumedLockS = 2.0;
    double mlPhaseMs = 0.0, mlPhaseFrom = 0.0;
    uint32_t mlPhaseState = 0;
    float  mlProg = 0.0f;
    float MlProgress() const { return mlNow.Percent > 0.001f ? mlNow.Percent : mlProg; }
    void MlTrackProgress()
    {
        const double now = NowMs();
        const uint32_t st = mlValid ? mlNow.State : 0;
        if (st != mlPhaseState)
        {
            mlPhaseState = st;
            mlPhaseMs = now;
            mlPhaseFrom = mlProg;
        }
        const double t = (now - mlPhaseMs) / (kMlAssumedLockS * 1000.0);
        if (st == 1)
            mlProg = (float)(std::min)(0.97, mlPhaseFrom + t);
        else if (st == 2)
            mlProg = (float)(std::max)(0.0, mlPhaseFrom - t);
        else
            mlProg = (st == kVhudLockLocked) ? 1.0f : 0.0f;
    }

    // Every frame, any page: the lock state.
    void MissileLockTick()
    {
        mlValid = ReadMissileLock(mlNow);
        MlTrackProgress();
        // (2026-10-08c: our lock tone was scrapped - the lock is shown, not sounded)
    }

    // Every 5 s while a vanilla screen is up: which sources exist and what the plugin did with them.
    void LogVanillaDiag()
    {
        const double now = NowMs();
        if (now < vDiagNextMs || !vBlock)
            return;
        vDiagNextMs = now + 5000.0;
        std::string s = "Vanilla sources:";
        static const char *st[] = {"idle", "hosted", "absent", "FAILED", "in game"};
        for (int i = 0; i < VS_COUNT; ++i)
        {
            const auto &sl = vBlock->Slots[i];
            char n[48];
            WideCharToMultiByte(CP_UTF8, 0, VanillaSourceName(i), -1, n, sizeof(n), nullptr, nullptr);
            s += std::string(" | ") + n + " " + (sl.Active ? std::to_string(sl.Width) + "x" + std::to_string(sl.Height)
                                                           : std::string("-"));
            if (i < VS_Depth)
                s += std::string(" ") + (sl.HostState < 5 ? st[sl.HostState] : "?");
        }
        Log(s);
    }

    // Like DrawFullscreenTexture, but keeps the element's aspect ratio.
    // THE CAP PAGE. With VanillaHud running it is a real camera: a second full-colour game
    // camera at its own magnification (1-20x, the page's zoom buttons), re-rendered sharp at every
    // step, with an IR mode. Without the plugin there is nothing to show.
    void RenderScreenCapture()
    {
        const float clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        context->ClearRenderTargetView(backBufferRTV.Get(), clearColor);

        PollVanillaMMF();
        if (vBlock)
        {
            vBlock->ZoomCamWantMs = GetTickCount64(); // keeps the zoom camera running
            vBlock->ZoomCamZoom   = camZoom;
            vBlock->ZoomCamIR     = zoomCamIR ? 1u : 0u;
            uint32_t w = 0, h = 0;
            if (ID3D11ShaderResourceView *cam = VanillaSrv(VS_ZoomCam, w, h))
            {
                RenderZoomCam(cam, w, h);
                ZoomLockOverlay(w, h);
            }
            else if (d2dCtx)
            {
                const VRect R = ContentRect();
                d2dCtx->BeginDraw();
                DrawPanelText(R.x + R.w * 0.5f - 80.0f, R.y + R.h * 0.5f - 11.0f, 200.0f, L"ZOOM CAMERA STARTING", 0x55FF8A, 0.7f);
                d2dCtx->EndDraw();
            }
            return;
        }

        if (d2dCtx)
        {
            const VRect R = ContentRect();
            d2dCtx->BeginDraw();
            DrawPanelText(R.x + R.w * 0.5f - 110.0f, R.y + R.h * 0.5f - 11.0f, 260.0f, L"NO CAMERA (plugin not running)", 0x55FF8A, 0.7f);
            d2dCtx->EndDraw();
        }
    }


    void StepCamZoom(bool in)
    {
        static const float k[] = {1.0f, 2.0f, 3.0f, 4.0f, 6.0f, 8.0f, 12.0f, 16.0f, 20.0f};
        const int n = (int)(sizeof(k) / sizeof(k[0]));
        int at = 0;
        for (int i = 0; i < n; ++i)
            if (camZoom >= k[i] - 0.01f)
                at = i;
        at = in ? (at < n - 1 ? at + 1 : n - 1) : (at > 0 ? at - 1 : 0);
        camZoom = k[at];
        Log("Zoom camera " + std::to_string((int)camZoom) + "x");
    }

    // CAP (2026-10-08): the same lock bracket, doll-and-weapons plate and missile lock V-POD
    // shows, laid over the zoom camera from the pose it was pointed with (VHUD v20 ZoomCam). The
    // 3D page works out the view only (SetPilotBare) - the camera image is the page.
    void ZoomLockOverlay(uint32_t w, uint32_t h)
    {
        if (!vBlock || !d2dAvailable || !d2dTarget)
            return;
        VanillaExportBlock::Camera z{};
        bool got = false;
        for (int i = 0; i < 4 && !got; ++i)
        {
            const uint32_t s1 = vBlock->ZoomCam.Seq;
            memcpy(&z, (const void *)&vBlock->ZoomCam, sizeof(z));
            const uint32_t s2 = vBlock->ZoomCam.Seq;
            got = (s1 == s2) && !(s1 & 1u) && z.Valid;
        }
        if (!got)
            return;
        const VRect R = ContentRect();
        const float as = (w && h) ? (float)w / (float)h : 16.0f / 9.0f, ar = R.w / R.h;
        const float cropX = (ar < as) ? ar / as : 1.0f; // RenderZoomCam's crop, horizontally
        const float hfCrop = 2.0f * atanf(tanf(z.FovDeg * 0.5f * 0.01745329252f) * cropX) * 57.29577951f;
        lidar.NotifyRendered();
        LidarScene scene;
        lidar.BuildScene(scene, true); // pilot view (CAP zoom): every round
        RECT rc;
        GetClientRect(hwnd, &rc);
        topo3d.SetTargets(backBufferRTV.Get(), d2dCtx.Get(), (unsigned)(rc.right - rc.left), (unsigned)(rc.bottom - rc.top));
        topo3d.SetTacOps(nullptr);
        topo3d.SetPilotCamera(true, z.Loc, z.Rot, hfCrop);
        const auto &b = mlNow.Bounds;
        topo3d.SetPilotLiveLock(mlValid && b.Valid != 0, b.Origin, b.Extent);
        topo3d.SetPilotBeams(0, nullptr);
        topo3d.SetPilotMissileLock(MlAcquiring(), MlProgress(), MlLocked());
        topo3d.SetPilotBare(true);
        topo3d.Render(scene);
        topo3d.SetPilotBare(false);
        topo3d.RenderPilotLock(scene);
        topo3d.SetPilotCamera(false, nullptr, nullptr, 0.0f);
    }

    void RenderZoomCam(ID3D11ShaderResourceView *cam, uint32_t w, uint32_t h)
    {
        const VRect R = ContentRect();
        // The camera image cropped to this screen's aspect, about its centre.
        const float as = (w && h) ? (float)w / (float)h : 16.0f / 9.0f, ar = R.w / R.h;
        float xf[4] = {1.0f, 1.0f, 0.0f, 0.0f};
        if (ar < as)
            xf[0] = ar / as;
        else
            xf[1] = as / ar;
        xf[2] = 0.5f - xf[0] * 0.5f;
        xf[3] = 0.5f - xf[1] * 0.5f;

        // IR only once the plugin has actually switched over (a float image with depth in alpha).
        bool irImage = false;
        if (vCached[VS_ZoomCam])
        {
            D3D11_TEXTURE2D_DESC d;
            vCached[VS_ZoomCam]->GetDesc(&d);
            irImage = d.Format == DXGI_FORMAT_R32G32B32A32_FLOAT;
        }
        uint32_t uw = 0, uh = 0;
        ID3D11ShaderResourceView *units = irImage ? VanillaSrv(VS_ZoomUnits, uw, uh) : nullptr;
        if (irImage && units && psZoomIR)
        {
            context->PSSetShaderResources(1, 1, &units);
            DrawSrv(cam, R, psZoomIR.Get(), xf, false);
            ID3D11ShaderResourceView *none = nullptr;
            context->PSSetShaderResources(1, 1, &none);
        }
        else if (!irImage)
            DrawSrv(cam, R, psXform.Get(), xf, false);

        // Crosshair on the centre (the camera's own axis), and the magnification.
        if (!d2dCtx || !uiBrush)
            return;
        const UINT32 col = zoomCamIR ? 0xFFFFFF : 0x55FF8A;
        d2dCtx->BeginDraw();
        if (capCross) // 2026-10-17: GlobalSettings > HUD > VISION (the magnification tag stays)
            DrawCrosshairLines(R.x + R.w * 0.5f, R.y + R.h * 0.5f, col);
        wchar_t tag[32];
        swprintf_s(tag, L"%dx%s", (int)camZoom, zoomCamIR ? L"  IR" : L"");
        DrawPanelText(R.x + 8.0f, R.y + 6.0f, 120.0f, tag, col, 0.9f);
        d2dCtx->EndDraw();
    }

    void DrawFullscreenTexture(ID3D11ShaderResourceView *srv)
    {
        context->OMSetRenderTargets(1, backBufferRTV.GetAddressOf(), nullptr);

        RECT rect;
        GetClientRect(hwnd, &rect);
        const float cw = (float)(rect.right - rect.left);
        const float ch = (float)(rect.bottom - rect.top);

        // Inset viewport: the texture is drawn into the middle 80% of the window so the
        // bezel legends around the edge stay clear of it. The surround is already black
        // from the ClearRenderTargetView the caller did.
        D3D11_VIEWPORT vp = {};
        vp.TopLeftX = cw * kContentInset;
        vp.TopLeftY = ch * kContentInset;
        vp.Width = cw * (1.0f - 2.0f * kContentInset);
        vp.Height = ch * (1.0f - 2.0f * kContentInset);
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;
        context->RSSetViewports(1, &vp);

        context->VSSetShader(vertexShader.Get(), nullptr, 0);
        context->PSSetShader(pixelShader.Get(), nullptr, 0);
        context->PSSetShaderResources(0, 1, &srv);
        context->PSSetSamplers(0, 1, sampler.GetAddressOf());
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->Draw(3, 0);
    }

    void SetUiColor(UINT32 rgb, float a = 1.0f)
    {
        if (uiBrush)
            uiBrush->SetColor(D2D1::ColorF(rgb, a));
    }

    void DrawPanelText(float x, float y, float w, const wchar_t *s, UINT32 color, float a = 1.0f)
    {
        if (!panelFmt || !dwrite || !d2dCtx)
            return;
        SetUiColor(color, a);
        ComPtr<IDWriteTextLayout> layout;
        if (SUCCEEDED(dwrite->CreateTextLayout(s, (UINT32)wcslen(s), panelFmt.Get(), w, 22.0f, &layout)))
            d2dCtx->DrawTextLayout(D2D1::Point2F(x, y), layout.Get(), uiBrush.Get());
    }

    // Label geometry: fixed pixel box centred on the normalised anchor, so labels keep a
    // constant size while the window scales around them.
    D2D1_RECT_F SlotRect(const LabelSlot &s, float w, float h) const
    {
        float left = s.NX * w - kLabelBoxW * 0.5f;
        float top = s.NY * h - kLabelBoxH * 0.5f;

        // Positions are normalised but the box is fixed pixels, so a narrow window could
        // otherwise push an edge-column legend off screen. Shift it back in rather than
        // shrinking it - the legend stays readable and the drag position is untouched.
        if (left < 2.0f)
            left = 2.0f;
        if (left + kLabelBoxW > w - 2.0f)
            left = w - 2.0f - kLabelBoxW;
        if (top < 2.0f)
            top = 2.0f;
        if (top + kLabelBoxH > h - 2.0f)
            top = h - 2.0f - kLabelBoxH;

        return D2D1::RectF(left, top, left + kLabelBoxW, top + kLabelBoxH);
    }

    static bool PointIn(const D2D1_RECT_F &r, float x, float y)
    {
        // An empty rect is how a control that isn't currently drawn is marked. Without
        // this, a click at exactly client (0,0) would hit every one of them.
        if (r.right <= r.left || r.bottom <= r.top)
            return false;
        return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
    }

    // Missile warning cue. Drawn over WHATEVER view is showing, because the alarm itself is
    // view-independent - a warning you can only see on the scope page is no warning at all.
    // Centred in the bottom-right quadrant (0.75w, 0.75h), which puts it inside the content
    // area rather than the bezel margin, so it does overlay the view. That is the point: it
    // should be impossible to miss, and it only exists while a salvo is actually tracking.
    //
    // Blink is derived from the wall clock, not a frame counter: this app's frame rate is
    // neither stable nor tied to the monitor, and a counter-driven blink would change speed
    // whenever the render loop hitched.
    void DrawMissileWarning(float w, float h)
    {
        if (!labelFmt || !uiBrush || !dwrite || !lidar.IsAlarmSounding())
            return;

        const double phase = fmod(NowMs(), 640.0) / 640.0; // ~1.6 Hz, steady in real time
        const bool bright = phase < 0.55;

        static const wchar_t kMsg[] = L"INCOMING MISSILE";
        const UINT32 msgLen = (UINT32)(sizeof(kMsg) / sizeof(kMsg[0]) - 1);

        // Sized and stroked like a bezel legend (same height, same 1.5 px outline), so it
        // reads as part of the panel rather than something pasted on top of it. No fill:
        // the whole HUD is outline-and-glyph on black, and a solid block would be the only
        // filled shape on screen.
        const float bw = 210.0f, bh = kLabelBoxH + 2.0f;
        // Centre of the bottom-right quadrant, then clamped so a small window can never
        // push the box off the edge.
        float bx = w * 0.75f - bw * 0.5f;
        float by = h * 0.75f - bh * 0.5f;
        if (bx < 2.0f) bx = 2.0f;
        if (by < 2.0f) by = 2.0f;
        if (bx + bw > w - 2.0f) bx = w - bw - 2.0f;
        if (by + bh > h - 2.0f) by = h - bh - 2.0f;
        const D2D1_RECT_F box = D2D1::RectF(bx, by, bx + bw, by + bh);

        // Outline and glyphs pulse together on one alpha, so it reads as the whole legend
        // brightening rather than a box with something flashing inside it.
        const float a = bright ? 1.0f : 0.3f;

        SetUiColor(UiCol::Alarm, a);
        d2dCtx->DrawRectangle(box, uiBrush.Get(), 1.5f);

        IDWriteTextLayout *layout = nullptr;
        if (SUCCEEDED(dwrite->CreateTextLayout(kMsg, msgLen, labelFmt.Get(), bw, bh, &layout)) && layout)
        {
            SetUiColor(UiCol::Alarm, a);
            d2dCtx->DrawTextLayout(D2D1::Point2F(bx, by), layout, uiBrush.Get());
            layout->Release();
        }
    }

    void DrawLabels(float w, float h)
    {
        if (!labelFmt || !uiBrush || !dwrite)
            return;

        for (size_t i = 0; i < labels.size(); i++)
        {
            LabelSlot &m = labels[i];
            const D2D1_RECT_F r = SlotRect(m, w, h);
            const bool active = IsSlotActive(m);
            const bool selected = editMode && (int)i == selectedSlot;

            // While a legend is being typed, show the live buffer on the button itself -
            // the panel alone gave no feedback on the thing actually being labelled.
            const bool liveEdit = editingText && !namingProfile && (int)i == selectedSlot;
            const std::wstring &shown = liveEdit ? textBuffer : m.Text;

            // A button with no legend is an empty outline: enough to see it and click it,
            // nothing to read. This is about the TEXT being empty, not about the button
            // being unassigned - an unassigned button can still carry a legend, and a
            // legend typed onto one used to be swallowed here and never drawn.
            if (shown.empty())
            {
                SetUiColor(selected ? UiCol::Warn : UiCol::EmptyEdge, selected ? 0.95f : 0.55f);
                d2dCtx->DrawRectangle(r, uiBrush.Get(), 1.0f);
                continue;
            }

            // Legend text alone: no fill, no border. The active view reads brighter, which
            // is how a DDI shows the selected page.
            if (selected)
            {
                SetUiColor(UiCol::Warn, 0.95f);
                d2dCtx->DrawRectangle(r, uiBrush.Get(), 1.0f);
            }

            // labelFmt is centred both ways, so a layout the size of the box lands the
            // text in the middle of it. Built once per legend, not once per frame - and
            // keyed on the shown string, so live editing rebuilds only while typing.
            if (!m.Layout || m.LayoutFor != shown)
            {
                m.Layout.Reset();
                if (SUCCEEDED(dwrite->CreateTextLayout(shown.c_str(), (UINT32)shown.size(), labelFmt.Get(),
                                                       kLabelBoxW, kLabelBoxH, &m.Layout)))
                {
                    m.LayoutFor = shown;
                }
            }

            SetUiColor(active ? UiCol::ActiveText : UiCol::LabelText, 1.0f);
            if (m.Layout)
                d2dCtx->DrawTextLayout(D2D1::Point2F(r.left, r.top), m.Layout.Get(), uiBrush.Get());
        }
    }

    // =====================================================================================
    // THE OPTIONS MENU (Esc). Drawn over the page area - the middle 80% of the window that every
    // page uses - so it never covers a bezel label: labels stay clickable while it is open, and a
    // click on one selects it. Laid out on a fixed kMenuW x kMenuH canvas, scaled to fit.
    //
    // Changes are live but are not written to the config until SAVE. REVERT goes back to the last
    // save (or to how things were when the menu opened), UNDO steps back one change at a time;
    // both can be undone themselves. Esc with unsaved changes asks first: Esc again discards them.
    // Anything that would normally save on its own while the menu is open (zoom keys, the
    // overlay's handles, F11) waits for SAVE too - see SaveConfig.
    // =====================================================================================
    static constexpr float kMenuW = 660.0f, kMenuH = 728.0f;

    struct MenuLabel
    {
        std::wstring Text;
        float NX = 0, NY = 0;
        int Action = 0;
        uint8_t Param = 0;
        UINT Key = 0, Mods = 0;
        bool Custom = false;
        bool operator==(const MenuLabel &o) const
        {
            return Text == o.Text && NX == o.NX && NY == o.NY && Action == o.Action && Param == o.Param &&
                   Key == o.Key && Mods == o.Mods && Custom == o.Custom;
        }
    };
    struct MenuState
    {
        std::vector<MenuLabel> L;
        uint32_t Layers = 0, RangeM = 0;
        bool Smooth = false, NorthUp = false, Borderless = false, InsaneAllies = false, MuteVoices = true;
        int Vol = 0;
        float SpanM = 0, OvOpacity = 0, OvScale = 0, OvPosX = 0, OvPosY = 0;
        bool operator==(const MenuState &o) const
        {
            return L == o.L && Layers == o.Layers && RangeM == o.RangeM && Smooth == o.Smooth && NorthUp == o.NorthUp &&
                   Borderless == o.Borderless && Vol == o.Vol && SpanM == o.SpanM && OvOpacity == o.OvOpacity &&
                   OvScale == o.OvScale && OvPosX == o.OvPosX && OvPosY == o.OvPosY;
        }
    };
    MenuState menuSaved, menuLast;       // at the last save / after the last recorded change
    std::vector<MenuState> menuUndo;     // each entry: how things were before one change
    bool      menuExitArmed = false;
    ULONGLONG menuExitAt = 0;
    std::wstring menuNote;               // a short confirmation ("Saved"), shown for a few seconds
    ULONGLONG menuNoteMs = 0;
    float     menuMouseX = -1.0f, menuMouseY = -1.0f; // the cursor, window pixels (hover)
    float     menuSc = 1.0f, menuOx = 0.0f, menuOy = 0.0f; // canvas -> window
    int       menuHoverChoice = -1;
    enum MenuHitId
    {
        MH_None = 0, MH_Label, MH_Page, MH_Text, MH_Rebind, MH_ClearKey, MH_Borderless, MH_OvOpM, MH_OvOpP,
        MH_OvSizeM, MH_OvSizeP, MH_OvReset, MH_NorthUp, MH_SpanM, MH_SpanP, MH_Layer, MH_Smooth, MH_RangeM,
        MH_RangeP, MH_VolM, MH_VolP, MH_Save, MH_Revert, MH_Undo, MH_ResetBezel, MH_InsaneAllies, MH_MuteVoices,
        MH_SaveAs, MH_OpenNew // 2026-10-20: a new profile from these settings, and starting it
    };
    struct MenuHit
    {
        D2D1_RECT_F R; // window pixels
        int Id, Arg;
    };
    std::vector<MenuHit> menuHits;

    MenuState MenuCapture() const
    {
        MenuState m;
        m.L.reserve(labels.size());
        for (const auto &s : labels)
        {
            MenuLabel l;
            l.Text = s.Text;
            l.NX = s.NX;
            l.NY = s.NY;
            l.Action = (int)s.Action;
            l.Param = s.Param;
            l.Key = s.Key;
            l.Mods = s.Mods;
            l.Custom = s.CustomText;
            m.L.push_back(l);
        }
        m.Layers = lidarLayers;
        m.RangeM = lidarRangeM;
        m.Smooth = lidarProjSmooth;
        m.Vol = lidarAlarmVol;
        m.NorthUp = topoNorthUp;
        m.SpanM = topoSpanM;
        m.Borderless = borderless;
        m.OvOpacity = ovOpacity;
        m.OvScale = ovScale;
        m.OvPosX = ovPosX;
        m.OvPosY = ovPosY;
        return m;
    }

    void MenuApply(const MenuState &m)
    {
        ReleaseHotkeysNow(); // before the list changes size: ids are 1..labels.size()
        labels.resize(m.L.size());
        for (size_t i = 0; i < m.L.size(); ++i)
        {
            LabelSlot &d = labels[i];
            const MenuLabel &l = m.L[i];
            d.Text = l.Text;
            d.NX = l.NX;
            d.NY = l.NY;
            d.Action = (ViewAction)l.Action;
            d.Param = l.Param;
            d.Key = l.Key;
            d.Mods = l.Mods;
            d.CustomText = l.Custom;
            d.Layout.Reset();
            d.LayoutFor.clear();
        }
        if (selectedSlot >= (int)labels.size())
            selectedSlot = -1;
        ReapplyHotkeys();
        lidarLayers = m.Layers;
        lidar.SetLayers(lidarLayers);
        lidarProjSmooth = m.Smooth;
        lidar.SetProjectileSmoothing(lidarProjSmooth);
        lidarRangeM = m.RangeM;
        lidar.SetRangeM(lidarRangeM);
        topo3d.SetRangeM(lidarRangeM);
        ApplyAlarmOpts(); // the alarm is GlobalSettings', not the profile's (2026-10-13)
        topoNorthUp = m.NorthUp;
        topo.SetNorthUp(topoNorthUp);
        topoSpanM = m.SpanM;
        if (topoSpanM == TopoView::kFitSpanSentinel)
            topo.SetFitMap(true);
        else
        {
            topo.SetFitMap(false);
            topo.SetSpanM(topoSpanM);
        }
        if (borderless != m.Borderless)
            SetBorderless(m.Borderless);
        ovOpacity = m.OvOpacity;
        ovScale = m.OvScale;
        ovPosX = m.OvPosX;
        ovPosY = m.OvPosY;
    }

    // Records a change once it is finished: not mid-drag, mid-typing or while a key is awaited,
    // so one label dragged across the bezel is one step of undo, not a hundred.
    void MenuTrack()
    {
        if (dragSlot >= 0 || editingText || capturingKey)
            return;
        MenuState c = MenuCapture();
        if (c == menuLast)
            return;
        menuUndo.push_back(menuLast);
        if (menuUndo.size() > 200)
            menuUndo.erase(menuUndo.begin());
        menuLast = c;
        menuExitArmed = false;
    }

    bool MenuDirty() const { return !(menuLast == menuSaved); }

    void MenuSay(const wchar_t *s)
    {
        menuNote = s;
        menuNoteMs = GetTickCount64();
    }

    void MenuSave()
    {
        MenuTrack();
        WriteConfig();
        menuSaved = menuLast;
        menuExitArmed = false;
        MenuSay(L"Saved");
        Log("Options saved");
    }

    void MenuRevert()
    {
        MenuTrack();
        if (!MenuDirty())
        {
            MenuSay(L"Nothing to revert");
            return;
        }
        menuUndo.push_back(menuLast); // a revert can be undone too
        MenuApply(menuSaved);
        menuLast = menuSaved;
        MenuSay(L"Reverted to the last save");
        Log("Options reverted to the last save");
    }

    void MenuUndo()
    {
        MenuTrack();
        if (menuUndo.empty())
        {
            MenuSay(L"Nothing to undo");
            return;
        }
        const MenuState s = menuUndo.back();
        menuUndo.pop_back();
        MenuApply(s);
        menuLast = s;
        MenuSay(L"Undone");
    }

    // The window is closing with the menu open: unsaved menu changes are dropped, like Esc twice.
    void MenuDiscardForExit()
    {
        if (!editMode)
            return;
        CancelTransientModes();
        MenuTrack();
        if (MenuDirty())
        {
            MenuApply(menuSaved);
            Log("Options: unsaved changes discarded on exit");
        }
        editMode = false;
    }

    void SetSlotAction(ViewAction a, uint8_t p)
    {
        if (selectedSlot < 0 || selectedSlot >= (int)labels.size())
            return;
        LabelSlot &s = labels[selectedSlot];
        s.Action = a;
        s.Param = p;
        // A hand-typed legend survives a view change - it was chosen deliberately. Table-derived
        // legends follow the new view.
        if (!s.CustomText)
            s.Text = ActionAbbrev(s.Action, s.Param);
        SaveConfig();
    }

    // ---- drawing on the canvas ----------------------------------------------------------
    D2D1_RECT_F MenuToWin(float x, float y, float w, float h) const
    {
        return D2D1::RectF(menuOx + x * menuSc, menuOy + y * menuSc, menuOx + (x + w) * menuSc,
                           menuOy + (y + h) * menuSc);
    }

    bool MenuHover(float x, float y, float w, float h) const
    {
        return PointIn(MenuToWin(x, y, w, h), menuMouseX, menuMouseY);
    }

    void MenuText(float x, float y, float w, float h, const wchar_t *s, UINT32 color, float a = 1.0f,
                  bool centred = false)
    {
        IDWriteTextFormat *f = centred ? menuFmtC.Get() : menuFmt.Get();
        if (!f || !s)
            return;
        SetUiColor(color, a);
        ComPtr<IDWriteTextLayout> layout;
        if (SUCCEEDED(dwrite->CreateTextLayout(s, (UINT32)wcslen(s), f, w, h, &layout)))
            d2dCtx->DrawTextLayout(D2D1::Point2F(x, y), layout.Get(), uiBrush.Get());
    }

    void MenuHeader(float x, float y, float w, const wchar_t *s)
    {
        MenuText(x, y, w, 18.0f, s, UiCol::PanelDim);
        SetUiColor(UiCol::PanelEdge, 0.8f);
        d2dCtx->DrawLine(D2D1::Point2F(x, y + 19.0f), D2D1::Point2F(x + w, y + 19.0f), uiBrush.Get(), 1.0f);
    }

    // A button. Lit = filled (the current choice); disabled = dim and not clickable.
    void MenuButton(float x, float y, float w, float h, const wchar_t *s, int id, int arg, bool lit = false,
                    bool enabled = true, UINT32 edge = UiCol::PanelEdge)
    {
        const D2D1_RECT_F r = D2D1::RectF(x, y, x + w, y + h);
        const bool hov = enabled && MenuHover(x, y, w, h);
        if (lit || hov)
        {
            SetUiColor(lit ? UiCol::CheckOn : UiCol::PanelText, lit ? (hov ? 0.40f : 0.28f) : 0.14f);
            d2dCtx->FillRectangle(r, uiBrush.Get());
        }
        SetUiColor(hov ? UiCol::PanelText : edge, enabled ? 1.0f : 0.4f);
        d2dCtx->DrawRectangle(r, uiBrush.Get(), hov ? 1.6f : 1.2f);
        MenuText(x + 2.0f, y, w - 4.0f, h, s, enabled ? UiCol::PanelText : UiCol::PanelDim, enabled ? 1.0f : 0.6f,
                 true);
        if (enabled && id)
            menuHits.push_back({MenuToWin(x, y, w, h), id, arg});
    }

    void MenuCheck(float x, float y, float w, const wchar_t *s, bool on, int id, int arg = 0)
    {
        const bool hov = MenuHover(x, y, w, 20.0f);
        if (hov)
        {
            SetUiColor(UiCol::PanelText, 0.10f);
            d2dCtx->FillRectangle(D2D1::RectF(x - 2.0f, y, x + w, y + 20.0f), uiBrush.Get());
        }
        const D2D1_RECT_F box = D2D1::RectF(x, y + 3.0f, x + 14.0f, y + 17.0f);
        SetUiColor(hov ? UiCol::PanelText : UiCol::PanelEdge);
        d2dCtx->DrawRectangle(box, uiBrush.Get(), 1.4f);
        if (on)
        {
            SetUiColor(UiCol::CheckOn);
            d2dCtx->FillRectangle(D2D1::RectF(box.left + 3, box.top + 3, box.right - 3, box.bottom - 3), uiBrush.Get());
        }
        MenuText(x + 22.0f, y, w - 24.0f, 20.0f, s, UiCol::PanelText);
        menuHits.push_back({MenuToWin(x - 2.0f, y, w + 2.0f, 20.0f), id, arg});
    }

    // name ....... [-] value [+]
    void MenuStepper(float x, float y, const wchar_t *name, const wchar_t *value, int idMinus, int idPlus)
    {
        MenuText(x, y, 118.0f, 22.0f, name, UiCol::PanelText);
        MenuButton(x + 120.0f, y, 26.0f, 22.0f, L"-", idMinus, 0);
        MenuText(x + 150.0f, y, 84.0f, 22.0f, value, UiCol::PanelText, 1.0f, true);
        MenuButton(x + 238.0f, y, 26.0f, 22.0f, L"+", idPlus, 0);
    }

    // The page picker: every choice, grouped. Indices into kActionChoices.
    struct MenuGroup
    {
        const wchar_t *Name;
        ActionChoice Items[12];
        int Count;
    };

    void DrawEditPanel()
    {
        menuHits.clear();
        menuHoverChoice = -1;
        if (!editMode || !menuFmt || !menuFmtC || !uiBrush)
            return;
        MenuTrack();

        // The page area, dimmed and framed; the canvas is scaled to fit and centred in it.
        const VRect R = ContentRect();
        SetUiColor(UiCol::Panel, 0.96f);
        d2dCtx->FillRectangle(D2D1::RectF(R.x, R.y, R.x + R.w, R.y + R.h), uiBrush.Get());
        SetUiColor(UiCol::PanelEdge, 0.9f);
        d2dCtx->DrawRectangle(D2D1::RectF(R.x, R.y, R.x + R.w, R.y + R.h), uiBrush.Get(), 1.0f);
        menuSc = (std::min)(R.w / kMenuW, R.h / kMenuH);
        menuSc = (std::min)((std::max)(menuSc, 0.3f), 1.8f);
        menuOx = R.x + (R.w - kMenuW * menuSc) * 0.5f;
        menuOy = R.y + (R.h - kMenuH * menuSc) * 0.5f;
        D2D1_MATRIX_3X2_F old;
        d2dCtx->GetTransform(&old);
        d2dCtx->SetTransform(D2D1::Matrix3x2F::Scale(menuSc, menuSc) * D2D1::Matrix3x2F::Translation(menuOx, menuOy));

        const bool dirty = MenuDirty();
        // The title row: OPTIONS, the two lance switches, the save state - sized for 13 px text, no "..." (2026-10-07d).
        MenuText(10.0f, 4.0f, 66.0f, 22.0f, L"OPTIONS", UiCol::Warn);
        {
            static const wchar_t *kAlly[3] = {L"Allies: vanilla", L"Allies: insane", L"Allies: impossibly strong"};
            MenuButton(80.0f, 5.0f, 182.0f, 22.0f, kAlly[allyMode], MH_InsaneAllies, 0, allyMode != 0); // click: the next mode
        }
        MenuCheck(268.0f, 5.0f, 202.0f, L"Mute game lancemate voices", muteGameVoices, MH_MuteVoices);
        MenuText(476.0f, 4.0f, 178.0f, 22.0f, dirty ? L"UNSAVED CHANGES" : L"all changes saved",
                 dirty ? UiCol::Warn : UiCol::PanelDim, 1.0f);

        MenuDrawLabelList(10.0f, 32.0f);
        MenuDrawSelected(312.0f, 32.0f);
        MenuDrawDisplay(10.0f, 430.0f);
        MenuDrawLidar(312.0f, 430.0f);

        // Status: the latest of - a warning from the editor, the exit question, a confirmation.
        const ULONGLONG now = GetTickCount64();
        if (menuExitArmed && now - menuExitAt > 5000)
            menuExitArmed = false;
        std::wstring status;
        UINT32 sc = UiCol::Warn;
        if (namingProfile)
            status = L"New profile name: " + textBuffer + L"_   Enter / SAVE AS saves it, Esc cancels" +
                     (ovOn ? L" (Alt+Tab here to type)" : L"");
        else if (menuExitArmed)
            status = L"Unsaved changes - press Esc again to discard them, or click SAVE";
        else if (ovOn && (editingText || capturingKey))
            status = L"Typing needs this window focused: Alt+Tab to it (the game has the keyboard)";
        else if (!editStatus.empty())
            status = editStatus;
        else if (!menuNote.empty() && now - menuNoteMs < 3000)
        {
            status = menuNote;
            sc = UiCol::PanelText;
        }
        if (!status.empty())
            MenuText(10.0f, 644.0f, 640.0f, 20.0f, status.c_str(), sc);

        MenuButton(10.0f, 668.0f, 86.0f, 26.0f, L"SAVE", MH_Save, 0, dirty, true, dirty ? UiCol::Warn : UiCol::PanelEdge);
        MenuButton(104.0f, 668.0f, 86.0f, 26.0f, L"REVERT", MH_Revert, 0, false, dirty);
        wchar_t ub[32];
        swprintf_s(ub, L"UNDO (%d)", (int)menuUndo.size());
        MenuButton(198.0f, 668.0f, 96.0f, 26.0f, ub, MH_Undo, 0, false, !menuUndo.empty());
        // 2026-10-20 (user): these settings as a NEW profile - its own config file and a "HUDDisplay - <name>" shortcut
        MenuButton(302.0f, 668.0f, 110.0f, 26.0f, namingProfile ? L"SAVE IT" : L"SAVE AS...", MH_SaveAs, 0, namingProfile, true,
                   namingProfile ? UiCol::Warn : UiCol::PanelEdge);
        if (!savedAsName.empty() && now - savedAsMs < 30000)
            MenuButton(420.0f, 668.0f, 102.0f, 26.0f, L"OPEN IT", MH_OpenNew, 0, false, !hudprof::Running(savedAsName));
        const bool armed = resetArmed && (now - resetArmedAt) < 4000;
        if (!armed)
            resetArmed = false;
        MenuButton(530.0f, 668.0f, 120.0f, 26.0f, armed ? L"CONFIRM RESET" : L"RESET BEZEL", MH_ResetBezel, 0, false,
                   true, armed ? UiCol::Warn : UiCol::PanelEdge);

        wchar_t hint[160];
        swprintf_s(hint, L"Esc close  |  %ls borderless  |  click a label to select, drag to move  |  wheel zoom",
                   KeyName(borderlessKey, 0).c_str());
        MenuText(10.0f, 702.0f, 640.0f, 20.0f, hint, UiCol::PanelDim);

        d2dCtx->SetTransform(old);
    }

    void MenuDrawLabelList(float x, float y)
    {
        MenuHeader(x, y, 290.0f, L"LABELS");
        MenuText(x + 2.0f, y + 22.0f, 22.0f, 18.0f, L"#", UiCol::PanelDim);
        MenuText(x + 24.0f, y + 22.0f, 70.0f, 18.0f, L"LEGEND", UiCol::PanelDim);
        MenuText(x + 96.0f, y + 22.0f, 96.0f, 18.0f, L"PAGE", UiCol::PanelDim);
        MenuText(x + 194.0f, y + 22.0f, 96.0f, 18.0f, L"KEY", UiCol::PanelDim);
        const int n = (int)labels.size();
        const float top = y + 42.0f;
        const float rowH = n > 0 ? (std::min)(17.0f, 344.0f / (float)n) : 17.0f;
        for (int i = 0; i < n; ++i)
        {
            const LabelSlot &s = labels[i];
            const float ry = top + rowH * (float)i;
            const bool sel = i == selectedSlot, hov = MenuHover(x, ry, 290.0f, rowH);
            if (sel || hov)
            {
                SetUiColor(sel ? UiCol::Warn : UiCol::PanelText, sel ? 0.20f : 0.10f);
                d2dCtx->FillRectangle(D2D1::RectF(x, ry, x + 290.0f, ry + rowH), uiBrush.Get());
            }
            wchar_t num[8];
            swprintf_s(num, L"%d", i + 1);
            const UINT32 c = sel ? UiCol::Warn : UiCol::PanelText;
            MenuText(x + 2.0f, ry, 22.0f, rowH, num, UiCol::PanelDim);
            MenuText(x + 24.0f, ry, 70.0f, rowH, s.Text.empty() ? L"-" : s.Text.c_str(), c);
            MenuText(x + 96.0f, ry, 96.0f, rowH, ActionFullName(s.Action, s.Param), c, 0.85f);
            MenuText(x + 194.0f, ry, 96.0f, rowH, KeyName(s.Key, s.Mods).c_str(), c, 0.85f);
            menuHits.push_back({MenuToWin(x, ry, 290.0f, rowH), MH_Label, i});
        }
    }

    void MenuDrawSelected(float x, float y)
    {
        const float w = 338.0f;
        const bool has = selectedSlot >= 0 && selectedSlot < (int)labels.size();
        wchar_t head[48];
        if (has)
            swprintf_s(head, L"SELECTED LABEL  #%d", selectedSlot + 1);
        MenuHeader(x, y, w, has ? head : L"SELECTED LABEL");
        if (!has)
        {
            MenuText(x, y + 26.0f, w, 20.0f, L"Click a label on the bezel, or a row in the list", UiCol::PanelDim);
            return;
        }
        LabelSlot &s = labels[selectedSlot];
        float ry = y + 26.0f;
        MenuText(x, ry, 64.0f, 22.0f, L"Legend", UiCol::PanelDim);
        const bool legendEdit = editingText && !namingProfile;
        const std::wstring shown = legendEdit ? (textBuffer + L"_") : (s.Text.empty() ? L"(blank)" : s.Text);
        MenuText(x + 66.0f, ry, 170.0f, 22.0f, shown.c_str(), legendEdit ? UiCol::Warn : UiCol::PanelText);
        MenuButton(x + w - 90.0f, ry, 90.0f, 22.0f, legendEdit ? L"DONE" : L"EDIT", MH_Text, 0, legendEdit);
        ry += 26.0f;
        MenuText(x, ry, 64.0f, 22.0f, L"Key", UiCol::PanelDim);
        MenuText(x + 66.0f, ry, 150.0f, 22.0f, capturingKey ? L"press a key..." : KeyName(s.Key, s.Mods).c_str(),
                 capturingKey ? UiCol::Warn : UiCol::PanelText);
        MenuButton(x + w - 166.0f, ry, 90.0f, 22.0f, capturingKey ? L"CANCEL" : L"REBIND", MH_Rebind, 0, capturingKey);
        MenuButton(x + w - 70.0f, ry, 70.0f, 22.0f, L"CLEAR", MH_ClearKey, 0, false, s.Key != 0);
        ry += 26.0f;
        MenuText(x, ry, 64.0f, 22.0f, L"Page", UiCol::PanelDim);
        MenuText(x + 66.0f, ry, w - 66.0f, 22.0f, ActionFullName(s.Action, s.Param), UiCol::PanelText);
        ry += 24.0f;
        const float hintY = ry; // filled in after the buttons, once the hovered one is known
        ry += 22.0f;

        static const MenuGroup kGroups[] = {
            {L"GAME HUD",
             {{ACT_VanillaWidget, VSC_Target}, {ACT_VanillaWidget, VSC_Weapons}, {ACT_VanillaWidget, VSC_Owner},
              {ACT_VanillaWidget, VSC_Objectives}, {ACT_VanillaWidget, VSC_Map}, {ACT_VanillaWidget, VSC_Comms},
              {ACT_VanillaWidget, VSC_Tactical}, {ACT_VanillaWidget, VSC_Targeting}, {ACT_VanillaWidget, VSC_TacOps},
              {ACT_VanillaWidget, VSC_Enhanced}},
             10},
            {L"MAPS, CAMERA & MWVR SCREENS",
             {{ACT_Lidar, 0}, {ACT_Topo, 0}, {ACT_Topo3D, 0}, {ACT_ScreenCapture, 0}, {ACT_Widget, 4}, {ACT_Widget, 5},
              {ACT_Widget, 6}, {ACT_Widget, 7}, {ACT_Widget, 8}, {ACT_Widget, 9}},
             10},
            {L"CONTROLS",
             {{ACT_LidarZoomIn, 0}, {ACT_LidarZoomOut, 0}, {ACT_ZoomCamIR, 0}, {ACT_CamRecentre, 0}, {ACT_CamLeft, 0},
              {ACT_CamRight, 0}, {ACT_CamUp, 0}, {ACT_CamDown, 0}, {ACT_CamFwd, 0}, {ACT_CamBack, 0}},
             10},
            {L"OTHER", {{ACT_TacOverlay, 0}, {ACT_TacOverlayHold, 0}, {ACT_EiVision, 0}, {ACT_Compass, 0}, {ACT_LancePage, 0}, {ACT_None, 0}}, 6},
        };
        const float bw = 64.0f, bh = 20.0f, gap = 4.5f;
        for (const auto &g : kGroups)
        {
            MenuText(x, ry, w, 15.0f, g.Name, UiCol::PanelDim, 0.9f);
            ry += 15.0f;
            for (int i = 0; i < g.Count; ++i)
            {
                const int col = i % 5, row = i / 5;
                const float bx = x + (bw + gap) * (float)col, by = ry + (bh + 3.0f) * (float)row;
                const ActionChoice &c = g.Items[i];
                int idx = -1;
                for (int k = 0; k < kActionChoiceCount; ++k)
                    if (kActionChoices[k].Action == c.Action && kActionChoices[k].Param == c.Param)
                        idx = k;
                if (idx < 0)
                    continue;
                if (MenuHover(bx, by, bw, bh))
                    menuHoverChoice = idx;
                const wchar_t *t = ActionAbbrev(c.Action, c.Param);
                MenuButton(bx, by, bw, bh, (t && *t) ? t : L"NONE", MH_Page, idx,
                           s.Action == c.Action && s.Param == c.Param);
            }
            ry += (bh + 3.0f) * (float)((g.Count + 4) / 5) + 3.0f;
        }
        if (menuHoverChoice >= 0)
            MenuText(x + 66.0f, hintY, w - 66.0f, 20.0f,
                     ActionFullName(kActionChoices[menuHoverChoice].Action, kActionChoices[menuHoverChoice].Param),
                     UiCol::Warn, 0.9f);
        else
            MenuText(x + 66.0f, hintY, w - 66.0f, 20.0f, L"(pick a page below)", UiCol::PanelDim, 0.8f);
    }

    void MenuDrawDisplay(float x, float y)
    {
        const float w = 290.0f;
        MenuHeader(x, y, w, L"DISPLAY");
        float ry = y + 24.0f;
        wchar_t b[64];
        MenuCheck(x, ry, 190.0f, L"Borderless window", borderless, MH_Borderless);
        swprintf_s(b, L"key %ls", KeyName(borderlessKey, 0).c_str());
        MenuText(x + 196.0f, ry, 94.0f, 20.0f, b, UiCol::PanelDim);
        ry += 26.0f;
        swprintf_s(b, L"%d%%", (int)(ovOpacity * 100.0f + 0.5f));
        MenuStepper(x, ry, L"Overlay opacity", b, MH_OvOpM, MH_OvOpP);
        ry += 26.0f;
        swprintf_s(b, L"%d%%", (int)(ovScale * 100.0f + 0.5f));
        MenuStepper(x, ry, L"Overlay size", b, MH_OvSizeM, MH_OvSizeP);
        ry += 26.0f;
        const bool centred = ovPosX < -5.0f;
        MenuText(x, ry, 190.0f, 22.0f, centred ? L"Overlay position: centred" : L"Overlay position: moved",
                 UiCol::PanelText);
        MenuButton(x + 200.0f, ry, 64.0f, 22.0f, L"CENTRE", MH_OvReset, 0, false, !centred);
        ry += 36.0f;

        MenuHeader(x, ry, w, L"2D MAP");
        ry += 24.0f;
        MenuCheck(x, ry, 190.0f, L"North up", topoNorthUp, MH_NorthUp);
        ry += 26.0f;
        if (topoSpanM == TopoView::kFitSpanSentinel)
            swprintf_s(b, L"whole map");
        else
            swprintf_s(b, L"%d m", (int)topoSpanM);
        MenuStepper(x, ry, L"Map span", b, MH_SpanM, MH_SpanP);
    }

    void MenuDrawLidar(float x, float y)
    {
        const float w = 338.0f;
        MenuHeader(x, y, w, L"LIDAR");
        float ry = y + 24.0f;
        // 2026-10-13 (user): the layers, smoothing and alarm are set in GlobalSettings > HUD > LIDAR / ALARM for every
        // window; only the range (this window's zoom, also ZM+ / ZM-) is set here.
        MenuText(x, ry, w, 20.0f, L"Layers, smoothing, alarm: GlobalSettings > HUD", UiCol::PanelDim);
        ry += 24.0f;
        wchar_t b[32];
        swprintf_s(b, L"%u m", lidarRangeM);
        MenuStepper(x, ry, L"Range", b, MH_RangeM, MH_RangeP);
        // (Alarm volume moved to GlobalSettings > HUD > ALARM, 2026-10-13)
    }

    bool MenuClick(float mx, float my)
    {
        const MenuHit *h = nullptr;
        for (const auto &m : menuHits)
            if (PointIn(m.R, mx, my))
                h = &m; // the last one drawn is on top
        if (!h)
            return false;
        const int id = h->Id, arg = h->Arg;
        // Anything but the text / key buttons themselves ends a pending edit first.
        if (id != MH_Text && id != MH_SaveAs && editingText)
            EndTextEdit(true); // (a profile name is cancelled by this - only Enter / SAVE IT saves it)
        if (id != MH_Rebind && capturingKey)
            EndKeyCapture(false, 0);
        if (id != MH_ResetBezel)
            resetArmed = false;
        editStatus.clear();
        switch (id)
        {
        case MH_Label:
            selectedSlot = arg;
            break;
        case MH_Page:
            if (arg >= 0 && arg < kActionChoiceCount)
                SetSlotAction(kActionChoices[arg].Action, kActionChoices[arg].Param);
            break;
        case MH_Text:
            if (editingText)
                EndTextEdit(true);
            else
                BeginTextEdit();
            break;
        case MH_Rebind:
            if (capturingKey)
                EndKeyCapture(false, 0);
            else
                BeginKeyCapture();
            break;
        case MH_ClearKey:
            ClearBinding();
            break;
        case MH_Borderless:
            ToggleBorderless();
            break;
        case MH_InsaneAllies:
            allyMode = (allyMode + 1) % 3;
            lanceOpts.AllyMode = allyMode;
            lanceOpts.InsaneAllies = allyMode != 0;
            gs::SetInt(L"lance", L"insane_allies", allyMode); // every window, at once (GlobalSettings.ini)
            Log(std::string("Allies: ") + (allyMode == 0 ? "vanilla" : allyMode == 1 ? "insane" : "impossibly strong"));
            break;
        case MH_MuteVoices:
            muteGameVoices = !muteGameVoices;
            lanceOpts.MuteGameVoices = muteGameVoices;
            gs::SetInt(L"lance", L"mute_game_voices", muteGameVoices ? 1 : 0); // every window, at once
            Log(std::string("Game lancemate voices ") + (muteGameVoices ? "muted" : "playing"));
            break;
        case MH_OvOpM:
        case MH_OvOpP:
            ovOpacity = (std::min)((std::max)(ovOpacity + (id == MH_OvOpP ? 0.1f : -0.1f), 0.3f), 1.0f);
            ovOpacity = std::round(ovOpacity * 10.0f) / 10.0f;
            break;
        case MH_OvSizeM:
        case MH_OvSizeP:
            ovScale = (std::min)((std::max)(ovScale + (id == MH_OvSizeP ? 0.05f : -0.05f), 0.2f), 1.0f);
            ovScale = std::round(ovScale * 20.0f) / 20.0f;
            break;
        case MH_OvReset:
            ovPosX = ovPosY = -10.0f;
            break;
        case MH_NorthUp:
            topoNorthUp = !topoNorthUp;
            topo.SetNorthUp(topoNorthUp);
            break;
        case MH_SpanM:
        case MH_SpanP:
            topo.AdjustSpan(id == MH_SpanM); // a smaller span is zooming in
            topoSpanM = topo.GetFitMap() ? TopoView::kFitSpanSentinel : topo.GetSpanM();
            break;
        case MH_Layer:
            ToggleLidarLayer(arg);
            break;
        case MH_Smooth:
            lidarProjSmooth = !lidarProjSmooth;
            lidar.SetProjectileSmoothing(lidarProjSmooth);
            break;
        case MH_RangeM:
            AdjustLidarRange(-(int)LidarView::kRangeStepM);
            break;
        case MH_RangeP:
            AdjustLidarRange((int)LidarView::kRangeStepM);
            break;
        case MH_VolM:
            AdjustAlarmVolume(-10);
            break;
        case MH_VolP:
            AdjustAlarmVolume(10);
            break;
        case MH_Save:
            MenuSave();
            break;
        case MH_SaveAs:
            if (namingProfile)
                EndProfileName(true);
            else
                BeginProfileName();
            break;
        case MH_OpenNew:
            if (!savedAsName.empty() && !hudprof::Running(savedAsName))
            {
                if (hudprof::Launch(hudprof::ExeDir(), savedAsName, hwnd))
                    MenuSay((L"Starting profile " + savedAsName).c_str());
                else
                    MenuSay(L"Could not start HUDDisplay.exe");
            }
            break;
        case MH_Revert:
            MenuRevert();
            break;
        case MH_Undo:
            MenuUndo();
            break;
        case MH_ResetBezel:
            ResetBezel();
            break;
        default:
            break;
        }
        return true;
    }

    void Render()
    {
        OvFrame();
        CamPollDrag();
        PollMMF();
        lidar.Poll();
        topo.Poll();
        topo3d.Poll();
        NavPoll(); // NAV POINTS (2026-10-07k)
        LogTopoStatus();

        const double tStart = NowMs();

        // The LIDAR scope draws itself through Direct2D; the widget and capture views
        // draw through D3D11. Both land in the same back buffer, and the bezel goes on
        // top of whichever ran.
        if (isLanceMode && d2dAvailable && d2dTarget)
        {
            // V-LNC: the lance page, drawn by the 3D view's plate code without the 3D view.
            RECT rcl;
            GetClientRect(hwnd, &rcl);
            topo3d.SetTargets(backBufferRTV.Get(), d2dCtx.Get(), (unsigned)(rcl.right - rcl.left),
                              (unsigned)(rcl.bottom - rcl.top));
            BuildLancePage();
            topo3d.RenderLancePage(lancePage);
        }
        else if (isLidarMode && d2dAvailable && d2dTarget)
        {
            // Idempotent: only does work if the scope dropped its target after a
            // D2DERR_RECREATE_TARGET, which it cannot recover from on its own.
            lidar.SetRenderTarget(d2dCtx.Get());
            lidar.Render();
        }
        else if (Show3D() && d2dAvailable && d2dTarget)
        {
            // Tell the scope its data is on screen even though it is not drawing, or the control
            // channel stops asking the plugin for the projectile scan a second from now and the
            // rounds quietly stop arriving.
            lidar.NotifyRendered();
            LidarScene scene;
            lidar.BuildScene(scene, isVanillaMode && IsPilotScreen()); // V-POD / E-IMG: every round
            RECT rc3;
            GetClientRect(hwnd, &rc3);
            topo3d.SetTargets(backBufferRTV.Get(), d2dCtx.Get(), (unsigned)(rc3.right - rc3.left),
                              (unsigned)(rc3.bottom - rc3.top));
            const bool targeting = isVanillaMode && IsPilotScreen(); // V-POD and Enhanced Imaging
            const bool tacops = IsTacOps();
            if (tacops)
                TacPrepare();
            else
                topo3d.SetTacOps(nullptr);
            if (targeting && !PrepareTargeting())
            {
                // No game camera yet (menus, loading, the plugin not running): say so, rather
                // than falling back to the chase view, which is a different page altogether.
                const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
                context->ClearRenderTargetView(backBufferRTV.Get(), black);
                topo3d.SetPilotCamera(false, nullptr, nullptr, 0.0f);
                const VRect R = ContentRect();
                d2dCtx->BeginDraw();
                DrawPanelText(R.x + R.w * 0.5f - 60.0f, R.y + R.h * 0.5f - 11.0f, 160.0f, L"NO CAMERA", 0x55FF8A, 0.8f);
                d2dCtx->EndDraw();
            }
            else
            {
                if (!targeting)
                    topo3d.SetPilotCamera(false, nullptr, nullptr, 0.0f);
                topo3d.Render(scene);
                if (targeting)
                {
                    DrawTargetingOverlays();
                    topo3d.RenderPilotBeams();     // lasers, over the outlines
                    topo3d.SetPilotMissileLock(MlAcquiring(), MlProgress(), MlLocked());
                    topo3d.RenderPilotLock(scene); // lock bracket + plate, over everything else
                }
                else if (tacops)
                    DrawTacOpsOverlays();
                else if (isVanillaMode)
                    DrawTacticalOverlays();
                else if (compassOn && d2dCtx)
                {
                    // 2026-10-12 (game test): the heading compass on the 3D page under MWVR too - it was only drawn
                    // with the vanilla HUD's tactical overlays.
                    d2dCtx->BeginDraw();
                    DrawCompass(false);
                    d2dCtx->EndDraw();
                }
            }
        }
        else if (isTopoMode && d2dAvailable && d2dTarget)
        {
            // Same idempotent re-arm as the scope, for the same reason: the page drops its
            // target on D2DERR_RECREATE_TARGET and cannot hand itself a new one.
            topo.SetRenderTarget(d2dCtx.Get());
            topo.Render();
        }
        else if (isScreenCaptureMode)
        {
            RenderScreenCapture();
        }
        else if (isVanillaMode)
        {
            RenderVanilla();
        }
        else
        {
            RenderWidget();
        }

        const double tView = NowMs();

        if (d2dAvailable && d2dCtx && d2dTarget)
        {
            RECT rc;
            GetClientRect(hwnd, &rc);
            const float w = (float)(rc.right - rc.left), h = (float)(rc.bottom - rc.top);

            LogAlarmDiag();

            d2dCtx->BeginDraw();
            DrawLabels(w, h);
            DrawOvHandles(w, h);
            DrawMissileWarning(w, h);
            DrawEditPanel();
            DrawOvCursor(); // last, over everything
            const HRESULT hr = d2dCtx->EndDraw();

            if (hr == (HRESULT)D2DERR_RECREATE_TARGET)
            {
                // Driver reset, TDR, RDP reconnect or a GPU switch. Rebuild the bitmap
                // that wraps the back buffer. Without this the bezel and the scope simply
                // stopped drawing for the rest of the session, and the scope's own
                // recovery never fired because it waits for the host to hand it a target.
                if (NowMs() - lastTargetLossLog > 1000.0)
                {
                    lastTargetLossLog = NowMs();
                    Log("Direct2D target lost - rebuilding");
                }
                ReleaseD2DTarget();
                if (CreateD2DTarget())
                {
                    lidar.SetRenderTarget(d2dCtx.Get());
                    topo.SetRenderTarget(d2dCtx.Get());
                    RECT rcr;
                    GetClientRect(hwnd, &rcr);
                    topo3d.SetTargets(backBufferRTV.Get(), d2dCtx.Get(), (unsigned)(rcr.right - rcr.left),
                                      (unsigned)(rcr.bottom - rcr.top));
                }
                else
                {
                    // The D2D *device* is gone, not just the target. Recovering that means
                    // rebuilding the D3D device and every shared texture with it, so stop
                    // here rather than pretend: the widget views keep working.
                    d2dAvailable = false;
                    isLidarMode = false;
                    isTopoMode = false;
                    isTopo3DMode = false;
                    // The scope will never render again, so nothing else would ever turn a
                    // sounding alarm off.
                                lidar.ReleaseControl();
                    lidar.SetRenderTarget(nullptr);
                    topo.SetRenderTarget(nullptr);
                    topo3d.SetTargets(nullptr, nullptr, 0, 0);
                    Log("Direct2D device lost - bezel and LIDAR disabled; restart to restore them");
                }
            }
        }

        // Present(0, 0): no vsync (see the note on the swap chain). The loop paces itself in
        // Run() against the panel's refresh rate, with timeBeginPeriod(1) for an accurate Sleep.
        const double tUi = NowMs();

        OvCopy(); // the finished frame, for the game overlay - before Present hands the buffer away
        const HRESULT pr = swapChain->Present(0, 0);

        // Occluded or minimised: Present returns immediately, so Run() sleeps instead of
        // spinning a core doing nothing.
        presentOccluded = (pr == (HRESULT)DXGI_STATUS_OCCLUDED);

        const double tEnd = NowMs();
        phaseView = tView - tStart;
        phaseUi = tUi - tView;
        phasePresent = tEnd - tUi;
    }

    // ---- Edit-mode input --------------------------------------------------

    // Esc. Opening takes a snapshot for REVERT and starts a fresh undo history. Closing with
    // unsaved changes asks first (the status line says so); Esc again within 5 s discards them.
    void ToggleEditMode()
    {
        CancelTransientModes();
        EndPageInteractions();
        if (!editMode)
        {
            editMode = true;
            menuSaved = menuLast = MenuCapture();
            menuUndo.clear();
            menuExitArmed = false;
            menuNote.clear();
            editStatus.clear();
            Log("Options open");
            return;
        }
        MenuTrack();
        if (MenuDirty())
        {
            if (!menuExitArmed || GetTickCount64() - menuExitAt > 5000)
            {
                menuExitArmed = true;
                menuExitAt = GetTickCount64();
                return;
            }
            MenuApply(menuSaved);
            Log("Options closed - unsaved changes discarded");
        }
        else
            Log("Options closed");
        editMode = false;
        menuExitArmed = false;
        resetArmed = false;
        editStatus.clear();
        WriteConfig(); // window position and anything saved outside the menu's own settings
    }

    // -----------------------------------------------------------------------------------
    // ONE ZOOM CONTROL, ACTING ON WHATEVER PAGE IS UP.
    //
    // These two buttons started life as LIDAR-only range steps, and the enum names still say
    // so - they are load-bearing, because the config stores Action as an integer index and
    // renaming or renumbering them would silently repoint every existing binding. The
    // BEHAVIOUR is what changed: on a real MFD a control acts on the page in front of you,
    // not on one particular page regardless of what you are looking at.
    //
    // So: TOPO span, zoom camera, targeting zoom, or LIDAR / map range. A screen with nothing
    // to zoom leaves the bezel buttons on the LIDAR range - better than a dead press - but the
    // mouse wheel does nothing there (fromWheel), so scrolling over a HUD screen cannot quietly
    // change the range.
    //
    // ACT_CaptureZoomIn/Out (the zoom camera's own steps) are kept working for configs that
    // bind them, but no longer offered in the menu: this covers the CAP page too.
    // -----------------------------------------------------------------------------------
    // One place that knows how to say what the page is doing, so the select log, the zoom log
    // and the periodic sample cannot drift apart and describe it three different ways.
    std::string TopoStatus() const
    {
        const TopoView::Diag d = topo.GetDiag();
        if (!d.Connected)
            return "not connected to MW5_TerrainExport";

        // Built as a named local rather than a temporary's c_str() inside the call. That would
        // have been legal - the temporary survives to the end of the full expression - but it is
        // one refactor away from a dangling pointer, and this costs nothing.
        const std::string spanText = d.FitMap ? std::string("FIT") : ("span " + std::to_string((int)d.SpanM) + " m");

        char buf[320];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                    "gen %u | map %u tiles%s (%d x %d, %u claimed, %u off-map) | "
                    "geom %d baked, %d queued | drawing %d tiles / %u segs at %d m cell | "
                    "%s | bake %.1f ms | elev %.0f-%.0f m | %s hdg %03d",
                    d.Generation, d.TilesComplete, d.Exploring ? " exploring" : " done",
                    d.ExtentTilesX, d.ExtentTilesY, d.TilesClaimed, d.TilesOffMap, d.Baked,
                    d.DirtyQueued, d.DrawnTiles, d.DrawnSegments, d.CellM,
                    spanText.c_str(),
                    d.LastBakeMs, d.MinElevM, d.MaxElevM, d.NorthUp ? "north-up" : "heading-up",
                    ((int)d.HeadingDeg % 360 + 360) % 360);
        return std::string(buf);
    }

    // Pages that have a zoom of their own. The mouse wheel does nothing anywhere else.
    bool PageHasZoom() const
    {
        return isTopoMode || isScreenCaptureMode || isLidarMode || Show3D();
    }

    void ZoomActivePage(bool in, bool fromWheel = false)
    {
        if (fromWheel && !PageHasZoom())
            return;
        if (isTopoMode)
        {
            topo.AdjustSpan(in);
            topoSpanM = topo.GetFitMap() ? TopoView::kFitSpanSentinel : topo.GetSpanM();
            SaveConfig();
            Log(std::string("TOPO ") + (topo.GetFitMap() ? "FIT (whole map)" : "span " + std::to_string((int)topoSpanM) + " m") +
                " - " + TopoStatus());
            return;
        }
        if (isScreenCaptureMode)
        {
            StepCamZoom(in);
            return;
        }
        if (isVanillaMode && IsPilotScreen())
        {
            // Optical: the plugin narrows the depth capture's field of view and the 3D page narrows
            // its own, so every step is a fresh full-resolution render, not an enlarged crop.
            static const float k[] = {1.0f, 2.0f, 3.0f, 4.0f, 6.0f, 8.0f, 12.0f, 16.0f, 20.0f};
            const int n = (int)(sizeof(k) / sizeof(k[0]));
            int at = 0;
            for (int i = 0; i < n; ++i)
                if (podZoom >= k[i] - 0.01f)
                    at = i;
            at = in ? (at < n - 1 ? at + 1 : n - 1) : (at > 0 ? at - 1 : 0);
            podZoom = k[at];
            Log("Targeting zoom " + std::to_string((int)podZoom) + "x");
            return;
        }
        // The 3D page shares the scope's range on purpose: one notion of "how far can I see",
        // so the two pages can be compared button-to-button without re-reading a second number.
        AdjustLidarRange(in ? -(int)LidarView::kRangeStepM : (int)LidarView::kRangeStepM);
    }

    // Zoom steps in 100 m increments. Works from a bezel button or the edit panel, and
    // whether or not the scope is the view currently on screen.
    void AdjustLidarRange(int deltaM)
    {
        // 5 km radar mode (2026-10-15): 500 m steps past 1 km, up to 5 km
        int v = (int)lidarRangeM;
        if (deltaM > 0)
            v += v >= 1000 ? (int)LidarView::kRangeStepFarM : deltaM;
        else
            v -= v > 1000 ? (int)LidarView::kRangeStepFarM : -deltaM;
        if (v < (int)LidarView::kMinRangeM)
            v = (int)LidarView::kMinRangeM;
        const int top = (int)(lidarFar ? LidarView::kMaxRangeFarM : LidarView::kMaxRangeM);
        if (v > top)
            v = top;
        if ((uint32_t)v == lidarRangeM)
            return;
        lidarRangeM = (uint32_t)v;
        lidar.SetRangeM(lidarRangeM);
        topo3d.SetRangeM(lidarRangeM);
        SaveConfig();
        Log("LIDAR range " + std::to_string(lidarRangeM) + " m");
    }

    void ClearBinding()
    {
        if (selectedSlot < 0 || selectedSlot >= (int)labels.size())
            return;
        if (labels[selectedSlot].Key == 0)
            return;

        labels[selectedSlot].Key = 0;
        labels[selectedSlot].Mods = 0;
        SaveConfig();
        ReapplyHotkeys();
    }

    // Two-step because it throws away every dragged position, legend and binding.
    void ResetBezel()
    {
        if (!resetArmed || (GetTickCount64() - resetArmedAt) >= 4000)
        {
            resetArmed = true;
            resetArmedAt = GetTickCount64();
            return;
        }

        resetArmed = false;
        ReleaseHotkeysNow(); // before the list changes size: ids are 1..labels.size()
        SeedDefaultLabels();
        selectedSlot = -1;
        ReapplyHotkeys();
        SaveConfig();
        Log("Bezel reset to defaults");
    }

    // Volume goes straight to the open audio device, so this is safe to hammer - it
    // re-synthesises nothing and frees nothing, which is what made the previous
    // implementation fragile to adjust under fire.
    void AdjustAlarmVolume(int delta)
    {
        int v = lidarAlarmVol + delta;
        if (v < 0)
            v = 0;
        if (v > 100)
            v = 100;
        lidarAlarmVol = v;
        lidar.SetAlarmVolume(lidarAlarmVol);
        SaveConfig();
    }

    void ToggleLidarLayer(int i)
    {
        (void)i;
        return; // GlobalSettings owns the layers now (2026-10-13)
        lidarLayers ^= (1u << i);
        lidar.SetLayers(lidarLayers);
        SaveConfig();
    }

    // Typing a legend needs the keyboard to itself: our own global hotkeys would eat the
    // numpad and anything else that happens to be bound, so they come down for the
    // duration and go back up on commit or cancel - the same trick key capture uses.
    void BeginTextEdit()
    {
        if (selectedSlot < 0 || selectedSlot >= (int)labels.size())
            return;
        if (capturingKey)
            EndKeyCapture(false, 0); // the two modes both want the raw keyboard
        editingText = true;
        textBuffer = labels[selectedSlot].Text;
        editStatus.clear();
        SyncHotkeys();
    }

    // ---- SAVE AS (2026-10-20, user): these settings as a new profile ----
    void BeginProfileName()
    {
        if (capturingKey)
            EndKeyCapture(false, 0);
        if (editingText)
            EndTextEdit(true);
        namingProfile = true;
        editingText = true; // the keyboard is ours (hotkeys down) while the name is typed
        textBuffer.clear();
        editStatus.clear();
        SyncHotkeys();
    }

    void EndProfileName(bool commit)
    {
        const std::wstring name = hudprof::Clean(textBuffer);
        namingProfile = false;
        editingText = false;
        textBuffer.clear();
        SyncHotkeys();
        if (!commit)
        {
            MenuSay(L"Save as cancelled");
            return;
        }
        if (name.empty())
        {
            MenuSay(L"Type a name first - letters, digits, - and _");
            return;
        }
        const std::wstring dir = hudprof::ExeDir();
        if (hudprof::SameName(name, WidenLabel(profileName)) || hudprof::Exists(hudprof::CfgPath(dir, name)))
        {
            MenuSay((L"A profile called " + name + L" already exists - pick another name").c_str());
            return;
        }
        // The current settings (as they stand in the menu, saved or not) go into the new profile's file;
        // this window stays on its own profile, which is left as it was.
        MenuTrack();
        const std::string keep = profileName;
        profileName = std::string(name.begin(), name.end()); // ASCII after Clean
        WriteConfig();
        profileName = keep;
        if (!hudprof::Exists(hudprof::CfgPath(dir, name)))
        {
            MenuSay(L"Could not write the new profile's config file");
            return;
        }
        const bool lnk = hudprof::MakeShortcut(dir, name);
        savedAsName = name;
        savedAsMs = GetTickCount64();
        MenuSay((L"Saved as profile " + name + (lnk ? L" + shortcut \"HUDDisplay - " + name + L"\" - OPEN IT starts it"
                                                    : std::wstring(L" (the shortcut could not be made) - OPEN IT starts it")))
                    .c_str());
        Log("Saved as new profile '" + std::string(name.begin(), name.end()) + "'" + (lnk ? " + shortcut" : " (no shortcut)"));
    }

    void EndTextEdit(bool commit)
    {
        if (namingProfile) // any other way out of the name box cancels it
        {
            EndProfileName(false);
            return;
        }
        if (commit && selectedSlot >= 0 && selectedSlot < (int)labels.size())
        {
            labels[selectedSlot].Text = textBuffer;
            // Mark it custom so ActionAbbrev stops overwriting it on load. Typing the
            // table's own abbreviation back in hands the label to the table again.
            labels[selectedSlot].CustomText =
                (textBuffer != ActionAbbrev(labels[selectedSlot].Action, labels[selectedSlot].Param));
            SaveConfig();
        }

        editingText = false;
        textBuffer.clear();
        SyncHotkeys();
    }

    void TextEditChar(wchar_t c)
    {
        if (namingProfile)
        {
            if (c == L'\b')
            {
                if (!textBuffer.empty())
                    textBuffer.pop_back();
            }
            else if (c == L' ' && textBuffer.size() < hudprof::kMaxName)
                textBuffer.push_back(L'_');
            else if (hudprof::NameChar(c) && textBuffer.size() < hudprof::kMaxName)
                textBuffer.push_back(c);
            return;
        }
        if (c == L'\b')
        {
            if (!textBuffer.empty())
                textBuffer.pop_back();
            return;
        }
        if (IsAllowedLabelChar(c) && textBuffer.size() < kMaxLabelText)
            textBuffer.push_back(c);
    }

    // Rebinding needs the raw keystroke, but our own global hotkeys would swallow it -
    // so they come down for the duration of the capture and go back up afterwards.
    void BeginKeyCapture()
    {
        if (selectedSlot < 0)
            return;
        if (editingText)
            EndTextEdit(true);
        capturingKey = true;
        editStatus.clear();
        SyncHotkeys();
        Log("Press a key to bind (Esc cancels)");
    }

    void EndKeyCapture(bool commit, UINT vk)
    {
        if (commit && selectedSlot >= 0 && selectedSlot < (int)labels.size())
        {
            UINT mods = 0;
            if (GetKeyState(VK_CONTROL) & 0x8000)
                mods |= MOD_CONTROL;
            if (GetKeyState(VK_MENU) & 0x8000)
                mods |= MOD_ALT;
            if (GetKeyState(VK_SHIFT) & 0x8000)
                mods |= MOD_SHIFT;

            if (vk == kEditModeKey && mods == 0)
            {
                // Binding this would make WM_HOTKEY swallow it and lock edit mode away.
                editStatus = L"Esc is reserved for edit mode";
                capturingKey = false;
                SyncHotkeys();
                return;
            }
            if (vk == borderlessKey && mods == 0)
            {
                // The same for the borderless toggle: a hotkey would swallow it.
                editStatus = L"That key is reserved for borderless";
                capturingKey = false;
                SyncHotkeys();
                return;
            }

            // A combination can only be registered once, so a duplicate would silently
            // leave one of the two labels dead. Take it from the previous owner and say
            // so, rather than refusing and making the user hunt for the clash.
            for (size_t i = 0; i < labels.size(); i++)
            {
                if ((int)i == selectedSlot || labels[i].Key != vk || labels[i].Mods != mods)
                    continue;

                labels[i].Key = 0;
                labels[i].Mods = 0;
                editStatus = KeyName(vk, mods) + L" taken from " +
                             (labels[i].Text.empty() ? L"an unassigned button" : labels[i].Text.c_str());
            }

            labels[selectedSlot].Key = vk;
            labels[selectedSlot].Mods = mods;
            SaveConfig();
        }

        capturingKey = false;
        SyncHotkeys();
    }

    // One place that ends any drag. releaseCapture is false when Windows has already
    // taken the capture away from us (WM_CAPTURECHANGED), where calling ReleaseCapture
    // would be wrong.
    void EndDrags(bool releaseCapture)
    {
        if (releaseCapture && dragSlot >= 0 && GetCapture() == hwnd)
            ReleaseCapture();
        dragSlot = -1;
        dragMoved = false;
    }

    // Back out of every transient mode. Used by the edit-mode toggle and by losing focus,
    // both of which previously left captures and key grabs stranded.
    void CancelTransientModes()
    {
        if (editingText)
            EndTextEdit(true); // typed text is committed rather than thrown away
        if (capturingKey)
            EndKeyCapture(false, 0);
        EndDrags(true);
        SyncHotkeys();
    }

    void OnLeftDown(float mx, float my)
    {
        if (!editMode)
        {
            // Outside edit mode a label is just a button.
            RECT rc;
            GetClientRect(hwnd, &rc);
            const float w = (float)(rc.right - rc.left), h = (float)(rc.bottom - rc.top);
            for (size_t i = 0; i < labels.size(); i++)
            {
                if (PointIn(SlotRect(labels[i], w, h), mx, my))
                {
                    DoAction(labels[i]);
                    return;
                }
            }
            if (IsTacOps())
                TacLeftDown(mx, my);
            else if (MouseCam())
            {
                camOrbiting = true;
                camPanning = false;
                camMX = camDownX = mx;
                camMY = camDownY = my;
                if (!ovOn)
                    SetCapture(hwnd);
            }
            return;
        }

        // The menu's own controls first.
        if (MenuClick(mx, my))
            return;
        // Any other click ends whatever transient mode is open rather than leaving it armed.
        // Typed text is committed; an armed rebind is cancelled, because the next keystroke
        // would otherwise land on whichever label this click selects.
        resetArmed = false;
        if (editingText)
            EndTextEdit(true);
        if (capturingKey)
            EndKeyCapture(false, 0);

        // Otherwise: pick up a label to drag.
        RECT rc;
        GetClientRect(hwnd, &rc);
        const float w = (float)(rc.right - rc.left), h = (float)(rc.bottom - rc.top);
        for (size_t i = 0; i < labels.size(); i++)
        {
            const D2D1_RECT_F r = SlotRect(labels[i], w, h);
            if (PointIn(r, mx, my))
            {
                selectedSlot = (int)i;
                dragSlot = (int)i;
                dragMoved = false;
                dragOffsetX = mx - labels[i].NX * w;
                dragOffsetY = my - labels[i].NY * h;
                if (!ovOn) // over the game the mouse is the game window's
                    SetCapture(hwnd);
                return;
            }
        }
    }

    void OnMouseMove(float mx, float my)
    {
        menuMouseX = mx; // the options menu's hover highlight
        menuMouseY = my;
        if (!editMode && IsTacOps())
        {
            TacMouseMove(mx, my);
            return;
        }
        if (!editMode && (camOrbiting || camPanning))
        {
            CamMouseMove(mx, my);
            return;
        }
        if (!editMode)
            return; // nothing is draggable outside edit mode

        if (dragSlot < 0 || dragSlot >= (int)labels.size())
            return;

        RECT rc;
        GetClientRect(hwnd, &rc);
        const float w = (float)(rc.right - rc.left), h = (float)(rc.bottom - rc.top);
        if (w <= 0.0f || h <= 0.0f)
            return;

        float nx = (mx - dragOffsetX) / w;
        float ny = (my - dragOffsetY) / h;
        if (nx < 0.02f)
            nx = 0.02f;
        if (nx > 0.98f)
            nx = 0.98f;
        if (ny < 0.02f)
            ny = 0.02f;
        if (ny > 0.98f)
            ny = 0.98f;

        // Kept on the bezel: the page area is the options menu's (and every page's), and a label
        // dropped there would be hidden under it. Pushed out to the nearest edge's strip.
        const float in0 = kContentInset, in1 = 1.0f - kContentInset;
        if (nx > in0 && nx < in1 && ny > in0 && ny < in1)
        {
            const float dl = nx - in0, dr = in1 - nx, dt = ny - in0, db = in1 - ny;
            const float m = (std::min)((std::min)(dl, dr), (std::min)(dt, db));
            if (m == dl)
                nx = in0 * 0.5f;
            else if (m == dr)
                nx = 1.0f - in0 * 0.5f;
            else if (m == dt)
                ny = in0 * 0.5f;
            else
                ny = 1.0f - in0 * 0.5f;
        }
        labels[dragSlot].NX = nx;
        labels[dragSlot].NY = ny;
        dragMoved = true;
    }

    void OnLeftUp()
    {
        if (!editMode && tacTorsoDrag)
        {
            TacTorsoEnd();
            return;
        }
        if (!editMode && tacDragArmed)
        {
            TacLeftUp();
            return;
        }
        if (camOrbiting)
        {
            // Barely moved: a click, not an orbit - show or hide the paperdoll under it.
            const bool click = (camMX - camDownX) * (camMX - camDownX) + (camMY - camDownY) * (camMY - camDownY) < 25.0f;
            CamMouseEnd();
            if (click && MouseCam())
                ContactClick(camDownX, camDownY);
            return;
        }
        const bool moved = dragMoved;
        const bool wasSlot = dragSlot >= 0;

        EndDrags(true);

        if (wasSlot && moved)
            SaveConfig();
    }

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        HUDDisplay *display = nullptr;

        if (msg == WM_CREATE)
        {
            CREATESTRUCT *cs = (CREATESTRUCT *)lParam;
            display = (HUDDisplay *)cs->lpCreateParams;
            SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)display);
        }
        else
        {
            display = (HUDDisplay *)GetWindowLongPtr(hwnd, GWLP_USERDATA);
        }

        if (display)
        {
            switch (msg)
            {
            case WM_CLOSE:
                // Save window position/size and the bezel layout before closing. Options changed
                // in the menu and not saved are dropped first, as Esc twice would.
                display->MenuDiscardForExit();
                display->WriteConfig();
                break;
            case WM_DESTROY:
                display->running = false;
                PostQuitMessage(0);
                return 0;
            case WM_SIZE:
                if (display->swapChain && display->device && wParam != SIZE_MINIMIZED)
                {
                    // ResizeBuffers refuses to run while ANY reference to a back buffer
                    // is outstanding. Releasing the ComPtrs is not enough: the D3D11
                    // immediate context holds its own reference to whatever is bound, and
                    // DrawFullscreenTexture binds the RTV (and an SRV) every frame and
                    // never unbinds. Missing this made the resize fail silently - the swap
                    // chain kept its old size while the bezel was drawn and hit-tested
                    // against the new client rect, so clicks stopped matching the picture.
                    display->ReleaseD2DTarget();
                    display->backBufferRTV.Reset();

                    ID3D11ShaderResourceView *noSRV[1] = {nullptr};
                    display->context->OMSetRenderTargets(0, nullptr, nullptr);
                    display->context->PSSetShaderResources(0, 1, noSRV);
                    display->context->Flush();

                    // Same flags as creation.
                    HRESULT hr =
                        display->swapChain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, display->swapChainFlags);
                    if (FAILED(hr))
                        display->Log("ResizeBuffers failed - display may be stretched until the next resize");

                    ComPtr<ID3D11Texture2D> backBuffer;
                    if (SUCCEEDED(display->swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&backBuffer)))
                        display->device->CreateRenderTargetView(backBuffer.Get(), nullptr, &display->backBufferRTV);

                    if (display->d2dAvailable)
                    {
                        // Only hand the scope a context that actually has a target.
                        const bool ok = display->CreateD2DTarget();
                        display->lidar.SetRenderTarget(ok ? display->d2dCtx.Get() : nullptr);
                        display->topo.SetRenderTarget(ok ? display->d2dCtx.Get() : nullptr);
                        display->topo3d.SetTargets(ok ? display->backBufferRTV.Get() : nullptr,
                                                   ok ? display->d2dCtx.Get() : nullptr,
                                                   (unsigned)LOWORD(lParam), (unsigned)HIWORD(lParam));
                    }
                }
                return 0;
            case WM_HOTKEY: {
                const int idx = (int)wParam - 1;
                if (idx >= 0 && idx < (int)display->labels.size())
                    display->DoAction(display->labels[idx]);
                return 0;
            }
            case WM_KILLFOCUS:
                // Key capture and legend editing hold the hotkeys down. Alt+Tab away with
                // one armed and they would stay down for the rest of the session, which
                // silently kills the whole point of the app.
                display->CancelTransientModes();
                return 0;
            case WM_CAPTURECHANGED:
                // Capture was taken from us (Alt+Tab, a system dialog). No WM_LBUTTONUP is
                // coming, so end the drag here or it stays latched to the cursor.
                display->EndDrags(false);
                // Only when someone ELSE took it: our own ReleaseCapture (lParam null) lands here
                // too, in the middle of finishing the drag.
                if (lParam && (HWND)lParam != hwnd)
                {
                    display->TacEndDrags();
                    display->CamMouseEnd();
                }
                return 0;
            case WM_MOUSEACTIVATE:
                // TacOps and the 3D pages are worked with the mouse while the game keeps the keyboard
                // and stick: a click on the PAGE must not bring this window to the front. A click on
                // the bezel strip around it does, so Esc / F11 are reachable even when borderless.
                if ((display->IsTacOps() || display->MouseCam()) && !display->editMode && LOWORD(lParam) == HTCLIENT)
                {
                    POINT p;
                    const VRect C = display->ContentRect();
                    if (GetCursorPos(&p) && ScreenToClient(hwnd, &p) && (float)p.x >= C.x && (float)p.x <= C.x + C.w &&
                        (float)p.y >= C.y && (float)p.y <= C.y + C.h)
                        return MA_NOACTIVATE;
                }
                break;
            case WM_RBUTTONDOWN:
                display->tacMouseFlags = (UINT)wParam;
                display->OnRightDown((float)GET_X_LPARAM(lParam), (float)GET_Y_LPARAM(lParam));
                return 0;
            case WM_MBUTTONDOWN:
                display->OnMiddleDown((float)GET_X_LPARAM(lParam), (float)GET_Y_LPARAM(lParam));
                return 0;
            case WM_MBUTTONUP:
                display->OnMiddleUp();
                return 0;
            case WM_MOUSEWHEEL: {
                // Over this window on its own screen. (Over the game the overlay's raw input does it.)
                POINT p{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                RECT rc;
                if (display->editMode || !ScreenToClient(hwnd, &p) || !GetClientRect(hwnd, &rc) || !PtInRect(&rc, p))
                    return 0;
                display->winWheelAcc += GET_WHEEL_DELTA_WPARAM(wParam);
                while (display->winWheelAcc >= WHEEL_DELTA || display->winWheelAcc <= -WHEEL_DELTA)
                {
                    const bool in = display->winWheelAcc > 0;
                    display->winWheelAcc += in ? -WHEEL_DELTA : WHEEL_DELTA;
                    display->ZoomActivePage(in, true);
                }
                return 0;
            }
            case WM_INPUT:
                display->OnRawInput((HRAWINPUT)lParam);
                break; // DefWindowProc must still see WM_INPUT
            case WM_LBUTTONDOWN:
                display->tacMouseFlags = (UINT)wParam;
                display->OnLeftDown((float)GET_X_LPARAM(lParam), (float)GET_Y_LPARAM(lParam));
                return 0;
            case WM_MOUSEMOVE:
                display->tacMouseFlags = (UINT)wParam;
                display->OnMouseMove((float)GET_X_LPARAM(lParam), (float)GET_Y_LPARAM(lParam));
                return 0;
            case WM_LBUTTONUP:
                display->tacMouseFlags = (UINT)wParam;
                display->OnLeftUp();
                return 0;
            case WM_CHAR:
                // Printable characters and backspace only reach here while a legend is
                // being typed; everything else ignores WM_CHAR.
                if (display->editingText)
                    display->TextEditChar((wchar_t)wParam);
                return 0;
            case WM_KEYDOWN:
                // Legend editing owns the keyboard while it is open, so Esc cannot
                // toggle edit mode out from under it and hotkeys stay unregistered.
                if (display->editingText)
                {
                    if (wParam == VK_ESCAPE)
                        display->EndTextEdit(false);
                    else if (wParam == VK_RETURN)
                    {
                        if (display->namingProfile)
                            display->EndProfileName(true);
                        else
                            display->EndTextEdit(true);
                    }
                    return 0;
                }

                // Key capture owns the keyboard while it is armed.
                if (display->capturingKey)
                {
                    if (wParam == VK_ESCAPE)
                        display->EndKeyCapture(false, 0);
                    else if (wParam != VK_CONTROL && wParam != VK_MENU && wParam != VK_SHIFT)
                        display->EndKeyCapture(true, (UINT)wParam);
                    return 0;
                }

                if (wParam == display->borderlessKey)
                {
                    display->ToggleBorderless();
                    return 0;
                }
                switch (wParam)
                {
                case kEditModeKey:
                    // Esc opens and closes the options (edit mode). It never quits the app: close
                    // the window for that.
                    display->ToggleEditMode();
                    break;
                }
                return 0;
            }
        }

        return DefWindowProc(hwnd, msg, wParam, lParam);
    }

  public:
    // Profile name = this instance's identity. Sanitised because it becomes a filename:
    // anything outside letters, digits, '-' and '_' is dropped, and an empty or absent name
    // falls back to "default".
    void SetProfile(const std::string &name)
    {
        std::string clean;
        for (char c : name)
        {
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_')
                clean.push_back(c);
            if (clean.size() >= 24)
                break;
        }
        profileName = clean.empty() ? "default" : clean;
    }

    bool Initialize()
    {
        AllocConsole();
        FILE *fp;
        freopen_s(&fp, "CONOUT$", "w", stdout);

        Log("=== MW5 MFD Display Starting ===");
        Log("Profile: " + profileName + "  (config: " + GetConfigPath() + ")");

        // Sleep() defaults to ~15.6 ms granularity, which is useless for pacing a 16.7 ms
        // frame. This makes it millisecond-accurate for the life of the process.
        timerPeriodSet = (timeBeginPeriod(1) == TIMERR_NOERROR);
        Log("Esc opens the options menu (labels, keys, display and LIDAR settings); F11 by default toggles borderless.");

        // Two windows on one profile share a config file: last writer wins, and their
        // hotkeys collide. Warn rather than refuse - the second window still works, it
        // just shouldn't be used to edit the layout.
        instanceMutex = CreateMutexA(nullptr, FALSE, ("Local\\MW5_MFDDisplay_" + profileName).c_str());
        if (instanceMutex && GetLastError() == ERROR_ALREADY_EXISTS)
        {
            Log("WARNING: another window is already running profile '" + profileName + "'.");
            Log("         They share one config file - whichever closes last wins.");
            Log("         Use profile=<name> to give this window its own.");
        }

        // Config first: it carries the window geometry and the bezel bindings that
        // InitWindow needs in order to register hotkeys.
        LoadConfig();

        if (!InitWindow())
            return false;
        if (!InitD3D())
            return false;
        if (!InitShaders())
            return false;

        if (!InitD2D())
            Log("Continuing without Direct2D - widget views only");

        // The game is optional. Both memory-mapped files are polled from the render loop,
        // so screens can be laid out before MW5 is launched.
        Log("Ready. Waiting for the game in the background.");
        return true;
    }

    void Run()
    {
        Log("Entering render loop");

        MSG msg = {};
        while (running)
        {
            // Process all pending messages without blocking
            while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
            {
                // WM_QUIT never reaches WindowProc, so the loop has to notice it here.
                if (msg.message == WM_QUIT)
                {
                    running = false;
                    break;
                }
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }

            const double frameStart = NowMs();

            PollVanillaMMF();
            MissileLockTick(); // 2026-10-08: the lock state for V-POD / CAP, and the lock tones
            // The lance switches shown in the Esc menu follow GlobalSettings.ini (changed elsewhere too).
            if (gsWatch.Changed(GetTickCount64()))
            {
                lanceOpts.Load();
                allyMode = lanceOpts.AllyMode;
                muteGameVoices = lanceOpts.MuteGameVoices;
                compassOpts.Load();
                radarOpts.Load();
                ApplyAlarmOpts(); // 2026-10-13
            }
            RadarRefresh(); // 2026-10-12: radar mode - Vanilla's copy of the game's detection flags
            lidar.SetContactFilter(radarOpts.Vanilla() ? &HUDDisplay::RadarKeep : nullptr, this);
            topo3d.SetHostileFilter(radarOpts.Vanilla() ? &HUDDisplay::RadarKeep : nullptr, this);
            // bug sweep 19 (2026-10-18): also on the first frame - a range saved in the 5 km mode (say 3000 m) was kept at
            // startup with the mode off (lidarFar false on both sides), so the 3D page drew 3000 m and ZM- jumped to 1000.
            if (!lidarFarApplied || radarOpts.Far() != lidarFar) // THE 5 KM RADAR MODE (2026-10-15): the scope's band + the plugin's far ranges
            {
                lidarFarApplied = true;
                lidarFar = radarOpts.Far();
                lidar.SetFar(lidarFar);
                const uint32_t top = lidarFar ? LidarView::kMaxRangeFarM : LidarView::kMaxRangeM;
                if (lidarRangeM > top)
                    lidarRangeM = top; // not saved: back in the 5 km mode the step buttons go out again
                lidar.SetRangeM(lidarRangeM);
                topo3d.SetRangeM(lidarRangeM);
            }
            lidar.SetCompass(compassOn && compassOpts.LidarRing, compassOn && compassOpts.LidarClock,
                             compassOn && compassOpts.Dots);
            // (2026-10-07h: nothing is sent to the plugin any more - LanceControl reads GlobalSettings.ini itself.)

            radarKeptF = radarHidF = 0;
            Render();
            radarKeptLast = radarKeptF;
            radarHidLast = radarHidF;
            EiVisionFrame(); // v22: Enhanced Imaging over the game, when on and this window renders it

            if (presentOccluded)
            {
                // Minimised or fully covered: Present returns instantly, so without this
                // the loop would spin a core doing nothing.
                Sleep(50);
            }
            else
            {
                // Present no longer paces the loop, so pace it here. timeBeginPeriod(1)
                // makes this accurate - without it Sleep's granularity is ~15.6 ms and
                // the cap would overshoot by a whole frame.
                const double elapsed = NowMs() - frameStart;
                if (elapsed < targetFrameMs)
                {
                    const int rest = (int)(targetFrameMs - elapsed);
                    if (rest > 0)
                        Sleep((DWORD)rest);
                }
            }

            // Watchdog. The breakdown is the point: "present" dominating means we are
            // waiting on the GPU or the compositor, and no amount of CPU-side tuning will
            // help. "view" or "ui" dominating means the work is ours to cut.
            const double frameMs = phaseView + phaseUi + phasePresent;
            const double now = NowMs();

            if (statWindowStart == 0.0)
                statWindowStart = now;

            statFrames++;
            if (frameMs > 40.0)
                statSlow++;
            if (frameMs > statWorst)
            {
                statWorst = frameMs;
                statWorstView = phaseView;
                statWorstUi = phaseUi;
                statWorstPresent = phasePresent;
            }

            if (now - statWindowStart >= 1000.0)
            {
                if (statSlow > 0)
                {
                    char buf[200];
                    sprintf_s(buf, "%d fps | %d slow | worst %d ms (view %d | ui %d | present %d)", statFrames,
                              statSlow, (int)statWorst, (int)statWorstView, (int)statWorstUi,
                              (int)statWorstPresent);
                    Log(buf);
                }
                statWindowStart = now;
                statFrames = 0;
                statSlow = 0;
                statWorst = 0.0;

                if (now - lastVramLog > 10000.0)
                {
                    lastVramLog = now;
                    LogVideoMemory("now");
                }
            }
        }

        Log("Exiting");

        if (timerPeriodSet)
        {
            timeEndPeriod(1);
            timerPeriodSet = false;
        }

        lidar.ReleaseControl();
        lidar.Shutdown();
        topo.Shutdown();
        topo3d.Shutdown();

        UnregisterAllHotkeys();

        // Cleanup
        if (vBlock)
        {
            UnmapViewOfFile(vBlock);
            vBlock = nullptr;
        }
        if (vMmfHandle)
        {
            CloseHandle(vMmfHandle);
            vMmfHandle = nullptr;
        }
        if (mmfBuffer)
        {
            UnmapViewOfFile(mmfBuffer);
            mmfBuffer = nullptr;
        }
        if (mmfHandle)
        {
            CloseHandle(mmfHandle);
            mmfHandle = nullptr;
        }
        if (instanceMutex)
        {
            CloseHandle(instanceMutex);
            instanceMutex = nullptr;
        }
    }
};

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR lpCmdLine, int)
{
    HUDDisplay display;

    // Command line:  [profile=<name>]
    //
    // profile=<name> selects this instance's config file and window title; omit it for
    // "default".
    //
    // Split into whitespace tokens and walk them, rather than searching the raw string for
    // substrings. Substring matching is what made the old parser read a profile called
    // "altimeter" as the Alt instance.
    std::vector<std::string> args;
    {
        std::istringstream iss{std::string(lpCmdLine)};
        std::string tok;
        while (iss >> tok)
            args.push_back(tok);
    }

    auto lowered = [](std::string t) {
        for (char &c : t)
            c = (char)tolower((unsigned char)c);
        return t;
    };

    std::string profile = "default";

    for (size_t i = 0; i < args.size(); i++)
    {
        const std::string key = lowered(args[i]);

        if (key.rfind("profile=", 0) == 0)
        {
            profile = args[i].substr(8); // original case is kept
        }
    }

    display.SetProfile(profile);

    if (display.Initialize())
    {
        display.Run();
    }

    return 0;
}
