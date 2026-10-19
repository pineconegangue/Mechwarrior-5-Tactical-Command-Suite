// ScreenExport.cpp - external MFD screen exporter.
//
// PURPOSE. This plugin replaces, from the OUTSIDE, every modification we currently carry
// inside sicsix's src/hud. With this DLL loaded, src/hud/{HUD,Renderer,WidgetRenderData}
// and include/Log.h can be byte-identical to upstream and the second-screen MFD still
// works. That is the whole point: our fork's diff against upstream goes to zero, so a
// refactor upstream (the 2.60 single-DLL merge) costs us a rebuild instead of a re-merge.
//
// WHAT IT PUBLISHES. One memory-mapped file holding an ExportControlBlock whose layout is
// byte-identical to the one HUDDisplay.exe already reads, with one shared keyed-mutex
// D3D11 texture per MFD screen. HUDDisplay needs no code change beyond the MMF NAME, which
// is deliberately different here so this proof-of-concept can never collide with the
// modified HUD.dll still deployed in the live plugins folder. See kMmfName.
//
// ---------------------------------------------------------------------------------------
// HOW IT REACHES THE TEXTURES, AND WHY NO PART OF THIS NEEDS UPSTREAM'S CODE
//
// The MFD screens are the MOD's assets, and each VR_*ScreenComponent is a Blueprint
// subclass of UMG's UWidgetComponent - confirmed out of the uasset itself:
//
//     VR_ScreenComponent.uasset -> NativeParentClass = Class'/Script/UMG.WidgetComponent'
//
// so RenderTarget is an ordinary reflected UPROPERTY and the whole chain is reachable:
//
//   pawn --MechMeshComponent--> mesh --FirstPersonCockpitComponent--> cockpit
//        --ChildActor--> mechCockpit --VR_<name>ScreenComponent--> component
//        --RenderTarget--> UTextureRenderTarget2D
//        --offset 0x98--> FTextureResource --offset 0x90--> FRHITexture2D
//        --GetNativeResource() (vtable slot 7)--> ID3D11Texture2D
//
// The property names and the seven-deep target-screen fallback are copied verbatim from
// upstream's own HUD::OnNewPawn, because upstream already proved empirically which names
// real cockpits use. The last three hops are upstream's mapped structs in include/mw5/.
//
// We CANNOT share the game's own render target directly: a texture must be created with
// D3D11_RESOURCE_MISC_SHARED_* and that flag cannot be added afterwards. So, exactly as
// the in-tree version did, we create our OWN shared texture per screen and CopyResource
// into it. The keyed-mutex protocol is fixed by the frozen consumer: producer acquires key
// 0 and releases key 1; HUDDisplay acquires 1 and releases 0. Do not "tidy" those numbers.
//
// ---------------------------------------------------------------------------------------
// THREADING - READ THIS BEFORE CHANGING ANYTHING
//
// Upstream resolves the components on the GAME thread (ValidateMech, reached from its
// FRendererModule::BeginRenderingViewFamily detour) and touches the textures on the RENDER
// thread. We cannot copy that split:
//
//   * The UEVR SDK's on_pre_engine_tick is NOT DELIVERED in this setup. LidarExport
//     established this empirically - its log line never appears - which is why it drives
//     off a Blueprint UFunction detour instead. We still register for the tick and prefer
//     it if it ever starts arriving, but we must not depend on it.
//   * We will NOT detour a game-thread UFunction to get one. LidarExport already detours
//     ReceiveTick and OnCalculateHeadAim, and AddEventHook works by overwriting the
//     UFunction's native pointer - a second plugin overwriting the same slot makes
//     uninstall order load-order-dependent. Two hooks on one function is how this project
//     has broken itself before.
//   * We will NOT detour FRendererModule::BeginRenderingViewFamily the way upstream does,
//     because upstream's own HUD.dll already MinHooks that exact address.
//
// So the resolve runs from the render callback as well, and is made safe by being narrow
// and rare rather than by being on the right thread:
//
//   - It is a FIXED pointer/property chain - ten property reads - never an actor
//     enumeration. LidarExport's access violation came from GetAllActorsOfClass racing a
//     resizing actor array; nothing here enumerates anything.
//   - It only runs while unresolved, and then at most once per kResolveThrottleMs. Once the
//     six render targets are cached, steady state does ZERO reflection per frame.
//   - Every hop is null-checked, and every property is type-checked against ObjectProperty
//     before its slot is dereferenced. Two crashes in this project came from skipping that
//     check: a TWeakObjectPtr read as a raw pointer, and a float read as an object because
//     its name looked like one.
//
// The per-frame work - RenderTarget -> Resource -> Texture2DRHI -> GetNativeResource -
// happens on the render thread, which is where upstream does it too, so that half carries
// no new risk.
// ---------------------------------------------------------------------------------------

#include <PluginExtension.h>

#include <windows.h>
#include <GsPointer.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

#include <mw5/UTextureRenderTarget2D.h>
#include <mw5/FTextureResource.h>
#include <mw5/FRHITexture2D.h>

using Microsoft::WRL::ComPtr;

// ---------------------------------------------------------------------------------------
// CROSS-BINARY CONTRACT. This struct is duplicated in HUDDisplay's Main.cpp and the two
// copies must stay byte-identical - the MMF carries no version or layout description, so a
// mismatch shows up as a garbage handle and a silently black screen, not as an error. The
// static_asserts below pin the layout so a careless field insertion fails the build here
// instead of failing to render at runtime.
// ---------------------------------------------------------------------------------------
struct ExportControlBlock
{
    uint32_t SelectedWidgetIndex; // written once at init; HUDDisplay chooses its own view
    uint32_t WidgetCount;         // how many slots are currently Active

    struct WidgetInfo
    {
        HANDLE   SharedTextureHandle;
        uint32_t Width;
        uint32_t Height;
        uint8_t  Type; // WidgetType enum value: 4..9 are the MFD screens
        bool     Active;
        uint8_t  Padding[2];
    } Widgets[10]; // 4 crosshairs + 6 screens; we only ever fill 4..9

    bool ExportEnabled;
};

static_assert(sizeof(ExportControlBlock::WidgetInfo) == 24, "WidgetInfo layout changed - HUDDisplay will misread it");
static_assert(offsetof(ExportControlBlock, Widgets) == 8, "Widgets offset changed - HUDDisplay will misread it");
static_assert(sizeof(ExportControlBlock) == 256, "ExportControlBlock size changed - HUDDisplay will misread it");

class ScreenExport final : public PluginExtension
{
    // -----------------------------------------------------------------------------------
    // The MMF name is the ONLY difference from the in-tree exporter's published contract.
    // It is suffixed so this plugin and the modified HUD.dll can both be loaded without
    // two writers landing on one control block. The POC build of HUDDisplay reads this
    // name; the shipping HUDDisplay still reads MechWarriorVR_HUDExport and is unaffected.
    // When this graduates, drop the suffix here and in HUDDisplay together.
    // -----------------------------------------------------------------------------------
    static constexpr const wchar_t *kMmfName = L"MechWarriorVR_HUDExport_POC";

    static constexpr int     kSlots     = 6; // the six MFD screens
    static constexpr uint8_t kFirstType = 4; // slot i publishes as widget type i + 4

    // How long to wait between resolve attempts while unresolved. Long enough that a
    // non-mech pawn costs nothing, short enough that stepping into a mech feels immediate.
    static constexpr ULONGLONG kResolveThrottleMs = 1000;

    // One entry per MFD screen. Names[] is an ordered fallback list terminated by nullptr;
    // the target screen needs all eight because cockpits disagree about what it is called,
    // and this exact order is upstream's (Sides before Below matters for the Warhammer).
    struct ScreenDef
    {
        uint8_t        Type;
        const char    *Label; // for logging only
        const wchar_t *Names[8];
    };

    static const ScreenDef &Def(int slot)
    {
        static const ScreenDef defs[kSlots] = {
            {4,
             "TargetScreen",
             {L"VR_TargetScreenLoadoutSidesComponent", L"VR_TargetScreenLoadoutBelowComponent",
              L"VR_TargetScreenComponent", L"VR_TargetLoadoutScreenComponent", L"VR_TargetingScreenComponent",
              L"VR_TargetScreenLoadoutComponent", L"VR_LoadoutScreenComponent", L"VR_TargetScreenSidesComponent"}},
            {5, "WeaponsScreen", {L"VR_WeaponsScreenComponent", nullptr}},
            {6, "OwnerScreen", {L"VR_OwnerScreenComponent", nullptr}},
            {7, "ObjectivesScreen", {L"VR_ObjectivesScreenComponent", nullptr}},
            {8, "MapScreen", {L"VR_MapScreenComponent", nullptr}},
            {9, "CommsScreen", {L"VR_LanceCommsScreenComponent", nullptr}},
        };
        return defs[slot];
    }

    // --- published state ---------------------------------------------------------------
    HANDLE              MmfHandle = nullptr;
    ExportControlBlock *Block     = nullptr;

    // --- resolved screens --------------------------------------------------------------
    // Written by the resolve, read by the publish. Both currently run on the render thread
    // (see the threading note at the top), but these stay atomic so that moving the resolve
    // onto a real game-thread driver later is a one-line change rather than a data race.
    std::atomic<UTextureRenderTarget2D *> Rt[kSlots] = {};

    // --- our own shared textures -------------------------------------------------------
    ComPtr<ID3D11Texture2D> Shared[kSlots];
    HANDLE                  SharedHandle[kSlots] = {};
    uint32_t                SharedW[kSlots]      = {};
    uint32_t                SharedH[kSlots]      = {};
    DXGI_FORMAT             SrcFmt[kSlots]       = {}; // the render target's own format

    // --- resolve bookkeeping -----------------------------------------------------------
    const API::UObject *ResolvedPawn    = nullptr;
    ULONGLONG           LastResolveMs   = 0;
    bool                LoggedNoCockpit = false; // log the "not ready" case once per pawn

    // -----------------------------------------------------------------------------------
    // PAWN-NULL DEBOUNCE. get_local_pawn(0) returns null for a frame or two at a time while
    // the player is still sitting in the same mech. Field evidence, one session:
    //
    //     19:50:03.315  local pawn became null - dropping 000002544447E010
    //     19:50:03.367  resolve pass: 6 of 6 screens        <- 52 ms later
    //     19:52:16.312  local pawn became null - dropping 000002544447E010   <- same pawn
    //
    // The "local pawn changed" line never fired once in fifteen minutes, and only two
    // distinct render targets were ever resolved - one per mech. So the pawn is not being
    // recreated and the cached UTextureRenderTarget2D* cannot dangle across these blinks.
    //
    // Dropping everything on the first null frame is what made all six screens unpublish and
    // republish, which is the flicker the consumer was tripping over. A real exit from the
    // mech lasts far longer than this grace (one gap in the same session was four minutes),
    // so one second separates the two cases without ambiguity.
    // -----------------------------------------------------------------------------------
    static constexpr ULONGLONG kPawnNullGraceMs = 1000;
    ULONGLONG                  PawnNullSinceMs  = 0; // 0 = pawn was present last time we looked

    // A plain counter, logged on change only, so the log says whether we are exporting
    // without saying it sixty times a second.
    uint32_t LastPublished = 0xFFFFFFFFu;

    // Frames where a slot WAS RESOLVED but its render target handed us no RHI texture. Not an
    // error on its own - a UWidgetComponent does not redraw every frame - but a count that
    // climbs without pause distinguishes "never gives us a texture" from "not resolved".
    //
    // The "resolved" qualifier is the whole point of the metric and it has already been lost
    // once. When the latch came out, the unresolved case and the no-texture case were folded
    // into a single !src test, so every frame before the cockpit spawns was counted as a miss
    // - the field log read "source misses by slot: 2520 2520 2520 2520 2520 2520", which looked
    // like a catastrophe and was in fact eighty seconds of sitting in the mechbay. If you ever
    // find yourself simplifying that branch again, this counter stops meaning anything.
    uint32_t                  SourceMisses[kSlots] = {};

    // One-shot warning flags, so a persistent oddity says its piece once instead of per frame.
    bool FormatWarned[kSlots]       = {};
    bool SizeMismatchLogged[kSlots] = {};

    // Post-render callback rate, for the once-per-frame / once-per-eye question.
    uint64_t  CallbackCalls       = 0;
    ULONGLONG CallbackRateSinceMs = 0;
    uint32_t  LastCallbackRate    = 0xFFFFFFFFu;
    uint32_t                  MissLogCounter       = 0;
    uint32_t                  LastMissTotal        = 0xFFFFFFFFu;
    static constexpr uint32_t kMissLogEveryFrames  = 600; // ~10 s at 60 fps

    std::atomic<bool> GameTickAlive{false};

  public:
    inline static ScreenExport *Instance;

    ScreenExport()
    {
        Instance                  = this;
        PluginExtension::Instance = this;
        Name                      = "ScreenExport";
        Version                   = "0.1.0";
        VersionInt                = 10;
        // VersionCheckFnName / VersionPropertyName left unset on purpose, as in
        // LidarExport: that handshake is MWVR's own. The base logs one harmless
        // "not found" and continues; we do not depend on VR_Global.
    }

    ~ScreenExport() override
    {
        ReleaseTextures();
        if (Block)
        {
            Block->ExportEnabled = false;
            UnmapViewOfFile(Block);
            Block = nullptr;
        }
        if (MmfHandle)
        {
            CloseHandle(MmfHandle);
            MmfHandle = nullptr;
        }
        if (WriterMutex)
        {
            ReleaseMutex(WriterMutex);
            CloseHandle(WriterMutex);
            WriterMutex = nullptr;
        }
    }

    // -----------------------------------------------------------------------------------
    // Skip the base class's MWVR version handshake.
    //
    // PluginExtension::on_initialize calls SetupVersionCheckHook, which looks up a UFunction
    // named by VersionCheckFnName. We leave that unset on purpose - the handshake publishes a
    // plugin version into MWVR's VR_Global blueprint and nothing about this plugin depends on
    // it. The cost of leaving it to fail was two ERROR-level lines at every launch:
    //
    //     [error] Failed to find function NOT SET
    //     [error] Failed to hook into NOT SET
    //
    // which is noise in the one log we read to diagnose everything else. Overriding here
    // keeps the base class untouched and says out loud that the omission is deliberate, so
    // nobody later "fixes" it by inventing a function name that does not exist.
    // -----------------------------------------------------------------------------------
    void on_initialize() override
    {
        LogInfo("%s %s initialized (MWVR version handshake deliberately skipped)", Name, Version);
        OnInitialize();
    }

    void OnInitialize() override
    {
        LogInfo("BUILD=screenexport-quietlog (QUIET LOG 2026-10-18: periodic profiles / status lines only with GlobalSettings [debug] verbose_log = 1) (screenexport-external)-reflect-poc3");
        EnsureExportReady();
    }

    // -----------------------------------------------------------------------------------
    // WRITER OWNERSHIP IS A MUTEX, NOT THE EXISTENCE OF THE SECTION. READ THIS BEFORE
    // "SIMPLIFYING" IT.
    //
    // The first version refused to publish when CreateFileMapping reported
    // ERROR_ALREADY_EXISTS, on the theory that an existing section meant another exporter
    // owned it. That is wrong, and it broke the plugin in the single most ordinary way this
    // thing gets used. A named section stays alive while ANY process holds a handle,
    // INCLUDING READERS, and HUDDisplay holds its handle for its entire lifetime. Field log:
    //
    //     23:47:52  HUDDisplay opens the section left over from the previous game session
    //     23:52:25  [error] MMF already exists - another exporter owns it. Standing down.
    //     00:14:08  the last HUDDisplay exits, the stale section finally dies
    //     00:14:14  "Game not connected yet" - nothing has published for the whole session
    //
    // So: leave the MFD windows open and restart the game, which is how anyone would use
    // this, and the exporter silently disables itself. Worse, it only tried ONCE, so it
    // stayed dead even after the obstruction cleared.
    //
    // A named mutex tracks what we actually care about - a live WRITER. The OS releases it
    // when the owning process dies, so a crashed or exited game frees it automatically, and
    // readers never touch it. Only if that mutex already exists is there genuinely another
    // exporter running.
    //
    // Deliberately no field is added to ExportControlBlock to carry a writer id: that struct
    // is a byte-identical cross-binary contract with HUDDisplay and is not to be extended for
    // our own bookkeeping.
    // -----------------------------------------------------------------------------------
    static constexpr const wchar_t *kWriterMutexName = L"Local\\MW5_ScreenExport_POC_Writer";
    static constexpr ULONGLONG      kSetupRetryMs    = 2000;

    HANDLE    WriterMutex        = nullptr;
    ULONGLONG LastSetupAttemptMs = 0;
    bool      BusyLogged         = false;

    // Called at init and again from the render callback until it succeeds, because a setup
    // that can fail must never be a one-shot: the obstruction it hits is usually temporary.
    bool EnsureExportReady()
    {
        if (Block)
            return true;

        const ULONGLONG now = GetTickCount64();
        if (LastSetupAttemptMs != 0 && now - LastSetupAttemptMs < kSetupRetryMs)
            return false;
        LastSetupAttemptMs = now;

        // 1) Claim writer ownership.
        if (!WriterMutex)
        {
            WriterMutex = CreateMutexW(nullptr, TRUE, kWriterMutexName);
            if (!WriterMutex)
            {
                LogError("CreateMutex failed (%lu) - will retry", GetLastError());
                return false;
            }
            if (GetLastError() == ERROR_ALREADY_EXISTS)
            {
                if (!BusyLogged)
                {
                    BusyLogged = true;
                    LogError("another ScreenExport writer is live - waiting for it to exit "
                             "(retrying every %llu ms; this is NOT triggered by HUDDisplay)",
                             kSetupRetryMs);
                }
                CloseHandle(WriterMutex);
                WriterMutex = nullptr;
                return false;
            }
            BusyLogged = false;
        }

        // 2) Create or adopt the section. Adopting one a reader kept alive is normal and
        //    expected; its contents are stale by definition, which the zero below handles.
        MmfHandle = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                       static_cast<DWORD>(sizeof(ExportControlBlock)), kMmfName);
        if (!MmfHandle)
        {
            LogError("CreateFileMapping failed (%lu) - will retry", GetLastError());
            return false;
        }
        const bool adopted = (GetLastError() == ERROR_ALREADY_EXISTS);

        Block = static_cast<ExportControlBlock *>(
            MapViewOfFile(MmfHandle, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(ExportControlBlock)));
        if (!Block)
        {
            LogError("MapViewOfFile failed (%lu) - will retry", GetLastError());
            CloseHandle(MmfHandle);
            MmfHandle = nullptr;
            return false;
        }

        // Wipe it unconditionally. An adopted block still holds the previous game process's
        // shared-texture handles, and every one of them is dead - a consumer that trusted
        // them would open garbage or nothing. Zeroing is what makes adoption safe.
        ZeroMemory(Block, sizeof(ExportControlBlock));
        Block->SelectedWidgetIndex = 0;
        Block->WidgetCount         = 0;
        Block->ExportEnabled       = true;

        LogInfo("export block ready (%zu bytes, %s)", sizeof(ExportControlBlock),
                adopted ? "adopted a section a reader was holding open, contents cleared" : "created fresh");
        return true;
    }

    // Registered in case this setup ever delivers it; harmless if it never fires. Doing the
    // resolve here instead of on the render thread is strictly better, so prefer it.
    void on_pre_engine_tick(API::UGameEngine *, float) override
    {
        if (!Block)
            return;
        if (!GameTickAlive.exchange(true))
            LogInfo("on_pre_engine_tick is being delivered - resolving on the GAME thread");
        ResolveIfNeeded();
    }

    void on_device_reset() override
    {
        // The device is gone, and every shared texture and handle with it. Drop them and
        // clear the published handles so HUDDisplay reopens instead of using a dead handle.
        ReleaseTextures();
        if (Block)
        {
            for (int i = 0; i < kSlots; ++i)
            {
                Block->Widgets[kFirstType + i].Active              = false;
                Block->Widgets[kFirstType + i].SharedTextureHandle = nullptr;
            }
        }
        LastPublished = 0xFFFFFFFFu;
    }

    void on_post_render_vr_framework_dx11(ID3D11DeviceContext *ctx, ID3D11Texture2D *vrTexture,
                                          ID3D11RenderTargetView *) override
    {
        LogVrRenderTargetOnce(vrTexture);

        if (!ctx)
            return;
        if (!Block && !EnsureExportReady())
            return;

        // Only resolve here while no game-thread driver is delivering ticks.
        if (!GameTickAlive.load(std::memory_order_relaxed))
            ResolveIfNeeded();

        ComPtr<ID3D11Device> dev;
        ctx->GetDevice(dev.GetAddressOf());
        if (!dev)
            return;

        uint32_t active = 0;

        for (int i = 0; i < kSlots; ++i)
        {
            auto &info = Block->Widgets[kFirstType + i];
            info.Type  = static_cast<uint8_t>(kFirstType + i);

            // -------------------------------------------------------------------------------
            // Active means exactly what it meant in the in-tree exporter: we have a source
            // texture for this screen THIS FRAME. No latching.
            //
            // An earlier version of this loop latched Active on until the screens were
            // unresolved, to stop the consumer sampling false frames. That was the wrong fix
            // at the wrong layer. The flicker was never transient RHI nulls - the field log
            // settles it:
            //
            //     source misses by slot: 3 3 3 3 2 3     <- and then it stops climbing, ever
            //
            // Three misses per slot during warmup, none afterwards. Every dropout came from
            // the blinking pawn clearing Rt, which the debounce in ResolveIfNeeded now
            // absorbs. With the cause fixed at the cause, Active can go back to being honest:
            // a screen that genuinely dies stops claiming to be available, instead of serving
            // a stale image forever.
            // -------------------------------------------------------------------------------
            const bool resolved  = Rt[i].load(std::memory_order_acquire) != nullptr;
            ID3D11Texture2D *src = resolved ? SourceTexture(i) : nullptr;

            if (!src)
            {
                if (info.Active)
                    LogInfo("slot %d (%s) unpublished - no source texture", kFirstType + i, Def(i).Label);
                else if (resolved)
                    ++SourceMisses[i]; // see the note on SourceMisses: RESOLVED slots only
                info.Active              = false;
                info.SharedTextureHandle = nullptr;
                continue;
            }

            // Dimensions come from the SOURCE texture's desc, not the render target UObject's
            // SizeX/SizeY. CopyResource validates against the desc, so the desc is the only
            // authority; the two agreed on every screen in the field logs, and MismatchLogged
            // below says so out loud if they ever stop agreeing.
            D3D11_TEXTURE2D_DESC sd{};
            src->GetDesc(&sd);
            CheckDeclaredSize(i, sd);

            if (!EnsureShared(dev.Get(), i, sd))
            {
                info.Active              = false;
                info.SharedTextureHandle = nullptr;
                continue;
            }

            // Handle first, then Active. The in-tree version set Active before creating the
            // texture, which left a window where Active was true and the handle was stale or
            // null - and that is the one combination HUDDisplay rejects outright. Same
            // observable behaviour when everything works, no bad intermediate state when it
            // does not.
            info.Width               = SharedW[i];
            info.Height              = SharedH[i];
            info.SharedTextureHandle = SharedHandle[i];

            if (!info.Active)
            {
                info.Active = true;
                LogInfo("slot %d (%s) published: %ux%u", kFirstType + i, Def(i).Label, SharedW[i], SharedH[i]);
            }

            CopyInto(ctx, i, src); // a failure here costs this frame and nothing else
            ++active;
        }

        // DIAGNOSTIC ONLY, and the reason is worth stating: the in-tree exporter guarded its
        // export with `eye == 0  // Only export once per frame`, because its hook fired once
        // per eye. We do not know whether this callback does the same, and guessing would mean
        // inventing an alternation guard on a hunch. So count invocations and report the rate:
        // if it comes out at twice the in-game frame rate, every copy and every mutex
        // acquire/release below is being done twice a frame and a guard is warranted.
        ++CallbackCalls;
        if (CallbackRateSinceMs == 0)
            CallbackRateSinceMs = GetTickCount64();
        else if (const ULONGLONG elapsed = GetTickCount64() - CallbackRateSinceMs; elapsed >= 5000)
        {
            const uint32_t perSec = static_cast<uint32_t>((CallbackCalls * 1000ULL) / elapsed);
            if (perSec != LastCallbackRate)
            {
                LastCallbackRate = perSec;
                if (gsp::VerboseLog()) LogInfo("post-render callback rate: ~%u/s (compare against in-game fps: equal "
                        "means once per frame, double means once per eye)",
                        perSec);
            }
            CallbackCalls       = 0;
            CallbackRateSinceMs = GetTickCount64();
        }

        Block->WidgetCount = active;

        if (active != LastPublished)
        {
            LastPublished = active;
            LogInfo("publishing %u of %d MFD screens", active, kSlots);
        }

        // Transient source misses are expected at low rates (the widget only redraws when it
        // needs to). A slot whose count climbs continuously means its render target is never
        // giving us an RHI texture, which is a different problem from not resolving - so log
        // the totals rarely rather than never.
        if (++MissLogCounter >= kMissLogEveryFrames)
        {
            MissLogCounter = 0;
            uint32_t total = 0;
            for (int i = 0; i < kSlots; ++i)
                total += SourceMisses[i];
            if (total != LastMissTotal)
            {
                LastMissTotal = total;
                LogInfo("source misses by slot: %u %u %u %u %u %u", SourceMisses[0], SourceMisses[1], SourceMisses[2],
                        SourceMisses[3], SourceMisses[4], SourceMisses[5]);
            }
        }
    }

  private:
    // -----------------------------------------------------------------------------------
    // DIAGNOSTIC ONLY. Describe the VR framework's render target once, then never again.
    //
    // UEVR hands us this texture every frame and we have always ignored it. It is the game's
    // own rendered image at RENDER resolution, which would be a far better source for the
    // second screen's region capture than cropping the desktop - zooming would magnify real
    // pixels instead of interpolated ones.
    //
    // Whether that is viable turns entirely on this desc, which is why the first step is to
    // read it and do nothing else:
    //
    //   * samples == 1 and an 8-bit typed format -> a crop is an inert CopySubresourceRegion.
    //     Copies bind nothing, so they cannot disturb the game's pipeline state, which is why
    //     this plugin needs none of the StateBackup machinery upstream's Renderer carries.
    //   * samples > 1 -> CopyResource is illegal and CopySubresourceRegion cannot crop MSAA
    //     at all; it would need a ResolveSubresource first.
    //   * a float HDR format -> it cannot be published as B8G8R8A8_UNORM_SRGB without a
    //     conversion, and a conversion means a DRAW on the game's immediate context. That is
    //     the point where this stops being an inert observer and we would have to save and
    //     restore pipeline state. Do not cross that line casually.
    //
    // We do not AddRef it and we do not keep the pointer past this call.
    // -----------------------------------------------------------------------------------
    bool VrRtLogged = false;

    void LogVrRenderTargetOnce(ID3D11Texture2D *tex)
    {
        if (VrRtLogged || !tex)
            return;
        VrRtLogged = true;

        D3D11_TEXTURE2D_DESC d{};
        tex->GetDesc(&d);
        LogInfo("VR framework render target: %ux%u fmt %d samples %u mips %u array %u usage %d "
                "bind 0x%X misc 0x%X - DIAGNOSTIC ONLY, nothing is read from or copied out of it",
                d.Width, d.Height, (int)d.Format, d.SampleDesc.Count, d.MipLevels, d.ArraySize, (int)d.Usage,
                d.BindFlags, d.MiscFlags);
    }

    // -----------------------------------------------------------------------------------
    // Read an ObjectProperty and return what it points at.
    //
    // The type check is not defensive padding, it is the lesson from two crashes. A
    // property whose NAME reads like an object can be a float (AV at 0x3f800010 - the bit
    // pattern of 1.0f) and an object-ish property can be a TWeakObjectPtr, whose slot is a
    // {index, serial} pair, not a pointer (AV at 0x10000000f - serial 1, index 15). Only a
    // plain ObjectProperty may be dereferenced as a pointer.
    // -----------------------------------------------------------------------------------
    static API::UObject *ObjProp(const API::UObject *obj, const wchar_t *name)
    {
        if (!obj || !name)
            return nullptr;

        const auto *cls = obj->get_class();
        if (!cls)
            return nullptr;

        const auto *prop = cls->find_property(name);
        if (!prop)
            return nullptr;

        const auto *pclass = prop->get_class();
        if (!pclass)
            return nullptr;
        if (pclass->get_name() != L"ObjectProperty")
            return nullptr;

        API::UObject *const *slot = obj->get_property_data<API::UObject *>(name);
        return slot ? *slot : nullptr;
    }

    void ClearResolved()
    {
        for (int i = 0; i < kSlots; ++i)
            Rt[i].store(nullptr, std::memory_order_release);
        ResolvedPawn    = nullptr;
        LoggedNoCockpit = false;
    }

    // -----------------------------------------------------------------------------------
    // Walk pawn -> cockpit -> six screen components -> six render targets. Cheap exit when
    // already resolved for the current pawn; throttled when not, so a pawn that never has
    // a cockpit (dropship, mission end, menus) costs one walk per second and no logging.
    // -----------------------------------------------------------------------------------
    void ResolveIfNeeded()
    {
        const auto *pawn = API::get()->get_local_pawn(0);
        const ULONGLONG nowMs = GetTickCount64();

        if (!pawn)
        {
            if (!ResolvedPawn)
                return; // nothing held; nothing to drop

            // Start (or continue) the grace period rather than dropping immediately.
            if (PawnNullSinceMs == 0)
            {
                PawnNullSinceMs = nowMs;
                return;
            }
            if (nowMs - PawnNullSinceMs < kPawnNullGraceMs)
                return; // still inside the blink - keep the screens exactly as they are

            LogInfo("local pawn null for %llu ms - dropping %p", nowMs - PawnNullSinceMs,
                    (const void *)ResolvedPawn);
            ClearResolved();
            PawnNullSinceMs = 0;
            return;
        }

        // Pawn is back (or was never away). Cancel any grace period in progress.
        if (PawnNullSinceMs != 0)
        {
            LogInfo("local pawn returned after %llu ms - screens kept", nowMs - PawnNullSinceMs);
            PawnNullSinceMs = 0;
        }

        if (pawn == ResolvedPawn)
            return;

        // A genuinely different pawn. This never fired in the field logs, which is what made
        // the debounce above safe - keep the line, because if it ever DOES fire while screens
        // are held, the cached render targets belonged to the old cockpit.
        if (ResolvedPawn)
            LogInfo("local pawn changed: %p -> %p", (const void *)ResolvedPawn, (const void *)pawn);

        if (LastResolveMs != 0 && nowMs - LastResolveMs < kResolveThrottleMs)
            return;
        LastResolveMs = nowMs;

        // A different pawn than whatever we had: everything cached is stale.
        if (ResolvedPawn)
            ClearResolved();

        const auto *mesh = ObjProp(pawn, L"MechMeshComponent");
        if (!mesh)
            return; // not a mech; silent, this is the common case out of combat

        const auto *cockpit     = ObjProp(mesh, L"FirstPersonCockpitComponent");
        const auto *mechCockpit = cockpit ? ObjProp(cockpit, L"ChildActor") : nullptr;
        if (!mechCockpit)
        {
            // In a mech but the cockpit child actor has not spawned yet - upstream hits
            // this too and simply retries. Log once so a permanent failure is visible.
            if (!LoggedNoCockpit)
            {
                LoggedNoCockpit = true;
                LogInfo("mech pawn found but cockpit child actor not ready - retrying");
            }
            return;
        }

        int found = 0;
        for (int i = 0; i < kSlots; ++i)
        {
            const ScreenDef &def = Def(i);

            API::UObject *comp = nullptr;
            for (const wchar_t *const *n = def.Names; *n && !comp; ++n)
                comp = ObjProp(mechCockpit, *n);

            if (!comp)
                continue; // this mech genuinely lacks this screen

            auto *rt = reinterpret_cast<UTextureRenderTarget2D *>(ObjProp(comp, L"RenderTarget"));
            if (!rt)
                continue; // component exists but its render target is not created yet

            Rt[i].store(rt, std::memory_order_release);
            ++found;
            LogInfo("resolved %s -> RenderTarget %p (%dx%d)", def.Label, (void *)rt, rt->SizeX, rt->SizeY);
        }

        if (found == 0)
            return; // keep ResolvedPawn null so we try again next tick

        // Latch only once every screen resolved, so a half-spawned cockpit does not freeze
        // us with empty slots for the rest of the mission. A mech that genuinely lacks a
        // screen therefore keeps retrying once a second, which is cheap and self-healing.
        if (found == kSlots)
            ResolvedPawn = pawn;

        LogInfo("resolve pass: %d of %d screens (%s)", found, kSlots,
                found == kSlots ? "complete, latched" : "partial, will retry");
    }

    // Walk the render-target chain to this frame's D3D11 texture. Every hop is checked;
    // any of them can be null during a resize or a level transition.
    ID3D11Texture2D *SourceTexture(int slot)
    {
        auto *rt = Rt[slot].load(std::memory_order_acquire);
        if (!rt)
            return nullptr;
        auto *res = rt->Resource;
        if (!res)
            return nullptr;
        auto *rhi = res->Texture2DRHI;
        if (!rhi)
            return nullptr;
        return rhi->GetNativeResource();
    }

    // -----------------------------------------------------------------------------------
    // Warn once per slot if the render target's declared SizeX/SizeY disagrees with the
    // texture's actual desc. The in-tree exporter published SizeX/SizeY; we publish the desc,
    // because CopyResource validates against the desc and nothing else. Every screen in the
    // field logs agreed, so the two choices were equivalent - this line is here so that if
    // they ever stop agreeing we find out by reading the log rather than by wondering why a
    // screen is cropped.
    // -----------------------------------------------------------------------------------
    void CheckDeclaredSize(int slot, const D3D11_TEXTURE2D_DESC &sd)
    {
        if (SizeMismatchLogged[slot])
            return;
        auto *rt = Rt[slot].load(std::memory_order_acquire);
        if (!rt)
            return;
        if (static_cast<uint32_t>(rt->SizeX) == sd.Width && static_cast<uint32_t>(rt->SizeY) == sd.Height)
            return;

        SizeMismatchLogged[slot] = true;
        LogWarn("%s declares %dx%d but its texture is %ux%u - publishing the texture's size, "
                "which is what CopyResource validates against",
                Def(slot).Label, rt->SizeX, rt->SizeY, sd.Width, sd.Height);
    }

    // Create (or recreate) our shared texture for this slot so it matches the source's size.
    // Recreating publishes a new handle, which HUDDisplay already detects and reopens - it
    // compares against lastKnownHandles for exactly this case. Expect one recreate per screen
    // shortly after a cockpit spawns: the mod creates its render targets at full size and then
    // resizes them to 0.75x (900x430 -> 675x322, 600x525 -> 450x393 in the field logs).
    bool EnsureShared(ID3D11Device *dev, int slot, const D3D11_TEXTURE2D_DESC &src)
    {
        // Compare against the SOURCE format too, so a source that changes format still
        // triggers a recreate even though we always publish the same one.
        if (Shared[slot] && SharedW[slot] == src.Width && SharedH[slot] == src.Height &&
            SrcFmt[slot] == src.Format)
            return true;

        // Resetting the texture closes its shared handle; do not close it by hand.
        Shared[slot].Reset();
        SharedHandle[slot] = nullptr;

        // ---------------------------------------------------------------------------------
        // THE PUBLISHED FORMAT IS A CONSTANT, AND THAT IS DELIBERATE.
        //
        // The MFD render targets are DXGI_FORMAT_B8G8R8A8_TYPELESS (fmt 90). A typeless
        // texture carries no interpretation, so an SRV over it CANNOT be created from a null
        // descriptor - and HUDDisplay calls CreateShaderResourceView(texture, nullptr, ...).
        // Mirroring the source format produced "Failed to create SRV for widget 8" at frame
        // rate: handle opened, texture real, view unbuildable.
        //
        // This is exactly why the in-tree exporter hardcoded this constant, and it ran for
        // months without a single texture problem. Mirroring the source was a correctness
        // improvement for the COPY and a regression for the CONSUMER; a mapping table was
        // then speculative generality over a proven constant. So: back to the constant.
        //
        // CopyResource permits differing formats within one typeless group, so
        // B8G8R8A8_TYPELESS copies into B8G8R8A8_UNORM_SRGB legally. Anything OUTSIDE that
        // group would be copied wrongly or not at all, so say so rather than publishing
        // something that cannot work.
        // ---------------------------------------------------------------------------------
        constexpr DXGI_FORMAT kExportFormat = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;

        if (src.Format != DXGI_FORMAT_B8G8R8A8_TYPELESS && src.Format != DXGI_FORMAT_B8G8R8A8_UNORM &&
            src.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB && !FormatWarned[slot])
        {
            FormatWarned[slot] = true;
            LogError("%s render target is format %d, outside the B8G8R8A8 group this exporter "
                     "publishes (%d). The copy will be wrong or will fail - a mapping is needed.",
                     Def(slot).Label, (int)src.Format, (int)kExportFormat);
        }

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width            = src.Width;
        desc.Height           = src.Height;
        desc.MipLevels        = 1;
        desc.ArraySize        = 1;
        desc.Format           = kExportFormat;
        desc.SampleDesc.Count = 1;
        desc.Usage            = D3D11_USAGE_DEFAULT;
        desc.BindFlags        = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        desc.MiscFlags        = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

        ComPtr<ID3D11Texture2D> tex;
        if (FAILED(dev->CreateTexture2D(&desc, nullptr, tex.GetAddressOf())))
        {
            LogError("CreateTexture2D failed for %s (%ux%u fmt %d)", Def(slot).Label, src.Width, src.Height,
                     (int)src.Format);
            return false;
        }

        ComPtr<IDXGIResource> dxgi;
        if (FAILED(tex.As(&dxgi)))
            return false;

        HANDLE h = nullptr;
        if (FAILED(dxgi->GetSharedHandle(&h)) || !h)
        {
            LogError("GetSharedHandle failed for %s", Def(slot).Label);
            return false;
        }

        Shared[slot]       = tex;
        SharedHandle[slot] = h;
        SharedW[slot]      = src.Width;
        SharedH[slot]      = src.Height;
        SrcFmt[slot]       = src.Format;

        LogInfo("shared texture for %s: %ux%u src fmt %d -> published fmt %d handle %p", Def(slot).Label, src.Width,
                src.Height, (int)src.Format, (int)kExportFormat, h);
        return true;
    }

    // Keyed-mutex protocol is fixed by the frozen consumer: we acquire 0 and release 1,
    // HUDDisplay acquires 1 and releases 0. AcquireSync is called with a zero timeout so a
    // consumer that is mid-read costs us this frame and never a stalled render thread.
    bool CopyInto(ID3D11DeviceContext *ctx, int slot, ID3D11Texture2D *src)
    {
        ComPtr<IDXGIKeyedMutex> mutex;
        if (FAILED(Shared[slot].As(&mutex)))
            return false;

        const HRESULT hr = mutex->AcquireSync(0, 0);
        if (hr != S_OK)
            return false; // WAIT_TIMEOUT or worse - skip this frame, keep the slot alive

        ctx->CopyResource(Shared[slot].Get(), src);
        mutex->ReleaseSync(1);
        return true;
    }

    void ReleaseTextures()
    {
        for (int i = 0; i < kSlots; ++i)
        {
            Shared[i].Reset();
            SharedHandle[i] = nullptr;
            SharedW[i]      = 0;
            SharedH[i]      = 0;
            SrcFmt[i]       = DXGI_FORMAT_UNKNOWN;
        }
    }
};

// ReSharper disable once CppInconsistentNaming
std::unique_ptr<ScreenExport> g_plugin{new ScreenExport()}; // NOLINT(misc-use-internal-linkage)
