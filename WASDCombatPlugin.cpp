#include <Debug.h>
#include <kenshi/GameWorld.h>
#include <kenshi/SaveManager.h>
#include <kenshi/PlayerInterface.h>
#include <kenshi/InputHandler.h>
#include <kenshi/Character.h>
#include <kenshi/CharBody.h>
#include <kenshi/Tasker.h>
#include <kenshi/CharMovement.h>
#include <kenshi/util/UtilityT.h>       // traceNoActors — detached OTS camera collision
#include <kenshi/CharStats.h>
#include <kenshi/CombatClass.h>
#include <kenshi/CombatTechniqueData.h>  // manual dodge: isDodge technique filter
#include <kenshi/CameraClass.h>
#include <kenshi/Globals.h>
#include <kenshi/OptionsHolder.h>
#include <kenshi/Enums.h>
#include <kenshi/gui/ForgottenGUI.h>
#include <kenshi/gui/MainBarGUI.h>
#include <kenshi/gui/OrdersPanel.h>
#include <kenshi/gui/OptionsWindow.h>
#include <kenshi/gui/DatapanelGUI.h>
#include <kenshi/gui/DataPanelLine.h>
#include <ogre/OgreSceneNode.h>
#include <ogre/OgreSceneManager.h>
#include <ogre/OgreEntity.h>
#include <ogre/OgreSubEntity.h>         // FP head hidden-parts mask
#include <ogre/OgreMaterial.h>          // FP head hidden-parts mask
#include <ogre/OgreTechnique.h>         // FP head hidden-parts mask
#include <ogre/OgrePass.h>              // FP head hidden-parts mask
#include <ogre/OgreGpuProgramParams.h>  // FP head hidden-parts mask
#include <ogre/OgreOldSkeletonInstance.h>   // first-person head-bone tracking
#include <ogre/OgreOldBone.h>               // first-person head-bone tracking
#include <kenshi/Appearance.h>              // AppearanceBase (head/hair hide, skeleton)
#include <kenshi/Town.h>                    // TownBase::withinBordersRange (slave camp bounds)
#include <kenshi/Building/Building.h>      // Building::hasInterior (FP floor cutaway gate)
#include <core/Functions.h>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>                          // first-person 1kHz raw mouse-look (KenshiFP method)
#include <mmsystem.h>                        // timeBeginPeriod (1ms Sleep granularity for the poll thread)
#pragma comment(lib, "winmm.lib")
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <limits.h>
#include <mygui/MyGUI.h>
#include <mygui/MyGUI_TextBox.h>       // "Manual Attack" HUD label
#include <mygui/MyGUI_ImageBox.h>      // lock-on dot + weapon glints (GUI-space markers)
#include <mygui/MyGUI_EditBox.h>        // vanilla speech text = an EditBox in a bubble layout (the bars step over it)

// Minimal forward declaration for ManagementScreen (world map / faction / tech /
// squad window).  The full KenshiLib header <kenshi/gui/ManagementScreen.h> pulls
// in a decompiled ReorderableList template that fails to compile, so we declare
// only the two methods we call — they resolve by mangled name from KenshiLib.lib
// (same mechanism as SaveManager::getSingleton()).  Signatures MUST match the
// header exactly (public, non-const getVisible) or the linker won't find them.
class ManagementScreen
{
public:
    static ManagementScreen* getSingleton();   // RVA 0x2967F0
    bool getVisible();                          // RVA 0x48B3F0
};

// -----------------------------------------------------------------------
// Locomotion tuning config — adjust values here without changing logic.
// -----------------------------------------------------------------------
struct LocoConfig
{
    float     wasdAccelerationMultiplier;  // pre-charges currentMotion for faster ramp-up (1.0 = vanilla)
    float     wasdDecelerationMultiplier;  // force-zeros currentMotion on key release (1.0 = halt only)
    ULONGLONG wasdInputGraceMs;            // hold previous motion for N ms on key release before stopping
    float     wasdTurnResponsiveness;      // extra limit boost applied on significant direction change
    bool      normalizeDiagonalMovement;   // normalize W+A diagonal to cardinal speed (true = same speed)
    ULONGLONG wasdNudgeTapWindowMs;        // max hold duration (ms) to treat as a nudge tap (100–150 ms)
};

static const LocoConfig g_loco = {
    /* wasdAccelerationMultiplier */ 1.25f,
    /* wasdDecelerationMultiplier */ 1.35f,
    /* wasdInputGraceMs           */ 25,
    /* wasdTurnResponsiveness     */ 1.25f,
    /* normalizeDiagonalMovement  */ true,
    /* wasdNudgeTapWindowMs       */ 125,
};

// -----------------------------------------------------------------------
// WASD release-stop config — governs behavior when all WASD keys are released.
// -----------------------------------------------------------------------
struct ReleaseStopConfig
{
    bool      wasdStopOnRelease;                 // master gate — false disables the whole sequence
    float     wasdReleaseDecelerationMultiplier; // >1 forces currentMotion to zero after halt()
    ULONGLONG wasdReleaseGraceMs;                // ms to hold motion after release (0 = instant stop)
    bool      wasdAnchorSnapOnRelease;           // snap free-move anchor to current pos on release
    bool      wasdZeroVelocityOnRelease;         // zero injected velocity fields on release
};

static const ReleaseStopConfig g_release = {
    /* wasdStopOnRelease                 */ true,
    /* wasdReleaseDecelerationMultiplier */ 999.0f,
    /* wasdReleaseGraceMs               */ 0,
    /* wasdAnchorSnapOnRelease          */ true,
    /* wasdZeroVelocityOnRelease        */ true,
};

// -----------------------------------------------------------------------
// DC camera config — vertical focus offset for close-zoom chest framing.
// -----------------------------------------------------------------------
struct CameraConfig
{
    bool  dcCameraCloseZoomChestOffset; // gate — false disables the offset entirely
    float dcCameraFocusOffsetY;         // world-unit Y raise at max zoom-in (taper to 0 at medium/far)
};

static const CameraConfig g_dcCam = {
    /* dcCameraCloseZoomChestOffset */ true,
    /* dcCameraFocusOffsetY         */ 4.5f,   // navel -> upper chest ~45 cm (user req 2026-06-12)
};

// -----------------------------------------------------------------------
// Logging config — set debugLogging = true for in-game diagnostics.
// In a release build all three should remain false.
// -----------------------------------------------------------------------
struct LogConfig
{
    bool debugLogging;              // master gate — false silences all debug/verbose logs
    bool verboseMovementLogs;       // movement_injection_allowed, free_camera_input_suppressed_dc
    bool verboseCommittedActionLogs;// committed_action_true / committed_action_false detail
    bool debugVerbose;              // per-frame spam: committed_action_true, enemy_targeting_player,
                                    //   movement_injection_allowed
};

static const LogConfig g_log = {
    /* debugLogging              */ false,
    /* verboseMovementLogs       */ false,
    /* verboseCommittedActionLogs*/ false,
    /* debugVerbose              */ false,
};
// v1.5: the per-event combat / movement diagnostics that drove the 1.5 tuning
// (every hit, swing, dodge, block, stagger, aim change, release stop, load
// flag ...) stay in the build behind ONE runtime switch so a player report can
// be diagnosed by flipping a setting - [Settings] VerboseLog (tab:
// TROUBLESHOOTING > "Detailed log for bug reports").  Off by default.
static bool s_verboseLog = false;
#define VerbLog(x) do { if (s_verboseLog) DebugLog(x); } while (0)

// -----------------------------------------------------------------------
// Performance profiling — per-second accumulation, emits dc_perf once/sec.
// All accumulators store raw QPC ticks; converted to ms at emit time.
// -----------------------------------------------------------------------
static bool     s_profInited          = false;
static LONGLONG s_profFreq            = 1;    // QPC ticks per second (cached)
static LONGLONG s_prof_mainLoop       = 0;
static LONGLONG s_prof_charMove       = 0;
static LONGLONG s_prof_playerControl  = 0;
static LONGLONG s_prof_committedAct   = 0;
static LONGLONG s_prof_threatScan     = 0;
static LONGLONG s_prof_cameraLock     = 0;
static LONGLONG s_prof_wasdInject     = 0;
static LONGLONG s_prof_combatTarget   = 0;
static ULONGLONG s_prof_windowStart   = 0;
static int      s_nearbyEnemyCount    = 0;    // all enemies in scan, updated in step 8
static int      s_chaseFlapsCount     = 0;    // combat_entered/exited transitions per perf window
static int      s_pathfindingEnemyCount = 0;  // enemies in TARGET_PATHFINDING* (path-recalc proxy)

static inline LONGLONG qpcNow()
{
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    return li.QuadPart;
}

// Lightweight RAII scope timer — accumulates QPC ticks into a named counter.
struct ScopeTimer
{
    LONGLONG  _start;
    LONGLONG& _accum;
    ScopeTimer(LONGLONG& a) : _start(qpcNow()), _accum(a) {}
    ~ScopeTimer() { _accum += qpcNow() - _start; }
};

// -----------------------------------------------------------------------
// Control modes
// -----------------------------------------------------------------------
enum ControlMode { MODE_VANILLA = 0, MODE_FREE_MOVE = 1 };

static volatile ControlMode s_mode   = MODE_VANILLA;
static volatile bool        s_wHeld  = false;
static volatile bool        s_aHeld  = false;
static volatile bool        s_sHeld  = false;
static volatile bool        s_dHeld  = false;
static volatile bool        s_xPressed = false;
// SPEEDSYNC-TWEAK: set when vanilla cycle_run_speed fires while DC is active;
// mainLoop re-syncs the anchor's desired speed to the (already-cycled) panel.
static volatile bool        s_syncSpeedToAnchor = false;
// RMB press edge — set by the poll thread, consumed on the main thread.
// The earliest reliable player-click signal: pure input level, cannot be
// blocked by any game-side dispatch path.  Used only to release the
// post-WASD hold; the click itself is never touched.
static volatile bool        s_rmbPressedEdge = false;
static bool                 s_rmbPrev        = false;  // poll thread only
// LMB double-click detection (poll thread).  In DC mode a SINGLE left click must
// only SELECT a squad member (vanilla), never switch which character WASD drives;
// only a DOUBLE click reassigns control (user req 2026-06-20 — restores pre-1.1.0
// behaviour, needed for Sentient Sands compatibility).  s_lmbDoubleClickMs is the
// timestamp of the most recent detected double-click; the main-thread retarget
// consumes it within a short window.
static bool                 s_lmbPrev          = false;     // poll thread only
static ULONGLONG            s_lastLmbDownMs    = 0;         // poll thread only
static volatile ULONGLONG   s_lmbDoubleClickMs = 0;        // set by poll, consumed by main
// CTRL toggle (user req 2026-06-20; upgraded 2026-09-02): in DC mode CTRL is a
// press-on/press-off TOGGLE.  The poll thread flips s_camRotateToggle on each
// CTRL press while in DC; cameraUpdate_hook forces InputHandler::rotate (the
// flag CameraClass::update reads) so the mouse rotates the view continuously,
// AND (v1.4.0) drives the vanilla orbit camera into an OVER-THE-SHOULDER pose
// — short zoom, pivot raised/pushed toward the shoulder — so CTRL now reads as
// a third-person action-camera toggle.  The vanilla camera still owns rotation,
// collision and floor clamps; we only steer where it orbits.  Reset whenever DC
// turns off or any UI opens (cursor freed for menus, unchanged).
static volatile bool        s_camRotateToggle  = false;
static bool                 s_ctrlPrevPoll     = false;     // poll thread only
// XHAIR-TWEAK (user req 2026-09-04): draw our own fixed white "+" crosshair
// and HIDE the game pointer while FP/OTS is engaged, instead of parking the
// visible OS cursor at center.  The look math (cursor warp / rotate) is
// unchanged — it just runs behind a hidden pointer, so toggling CTRL/P shows
// the crosshair and hides the cursor with no visible sweep across the screen.
// Two WhiteSkin bars on the "Top" MyGUI layer; created lazily on the UI
// thread; hidden (pointer restored) whenever a UI suspends the mode.
static MyGUI::Widget* s_xhH = nullptr;      // horizontal bar
static MyGUI::Widget* s_xhV = nullptr;      // vertical bar
static bool           s_xhTried = false;    // one creation attempt (skip if it throws)
static bool           s_xhPointerHidden = false;
static bool           s_xhWasWant = false;   // want last frame (return-to-center edge)
// XHAIR-INDICATOR (user req 2026-09-11): because the pick position is pinned to
// the crosshair, the game's own hover logic already selects the contextual
// cursor (attk/talk/door/lockpick/...) there — we just hide it.  While a
// look-mode is active and the pointer is contextual, UNHIDE the real cursor
// icon (it renders at the pinned centre) and hide the plain "+".
// HOW THE NAME IS TRACKED: PointerManager::eventChangeMousePointer fires ONLY
// for widget-driven pointer changes (notifyChangeMouseFocus) — Kenshi's world
// hover calls setPointer() directly, which never fires it (verified in-game
// 2026-09-11: event subscribed OK, zero callbacks while vanilla icons visibly
// changed).  So we hook the MyGUIEngine_x64.dll exports themselves —
// record-and-forward only, never alter the call.
static std::string    s_xhPtrName("arrow");
static void dcNotePointerName(const std::string& _name) { s_xhPtrName = _name; }
static bool dcPointerIsContextual()
{
    // Kenshi pointer set (data/gui/pointer/kenshi_pointers.xml): everything
    // except the plain arrow / hidden states signals an interaction.
    return !s_xhPtrName.empty()
        && s_xhPtrName != "arrow"
        && s_xhPtrName != "invisible";
}
// Hook trio: public setPointer(name) = world hover; private setPointer(name,
// owner) = widget path (also reached from inside the public one when the DLL
// doesn't inline it — duplicate notes are harmless); resetToDefaultPointer =
// the "back to arrow" transition (Kenshi's default pointer IS "arrow").
typedef void(__fastcall* XhPtrSetFn)(void* self, const std::string& name);
typedef void(__fastcall* XhPtrSetWFn)(void* self, const std::string& name, void* widget);
typedef void(__fastcall* XhPtrResetFn)(void* self);
static XhPtrSetFn   s_xhSetPtrOrig  = nullptr;
static XhPtrSetWFn  s_xhSetPtrWOrig = nullptr;
static XhPtrResetFn s_xhResetOrig   = nullptr;
static void __fastcall xhSetPointer_hook(void* self, const std::string& name)
{
    s_xhSetPtrOrig(self, name);
    dcNotePointerName(name);
}
static void __fastcall xhSetPointerW_hook(void* self, const std::string& name, void* widget)
{
    s_xhSetPtrWOrig(self, name, widget);
    dcNotePointerName(name);
}
static void __fastcall xhResetPointer_hook(void* self)
{
    s_xhResetOrig(self);
    dcNotePointerName("arrow");
}
static ULONGLONG s_invFaceRetrackUntilMs = 0; // post-face-cam follow WATCHDOG
static ULONGLONG s_invFaceRetrackLastMs  = 0; // window: the re-track at inventory
                                         // close gets stomped by the vanilla close
                                         // handling and even one deferred call
                                         // didn't stick (field 2026-09-02: |HARA|
                                         // follow point frozen mid-air while the
                                         // order-driven anchor walked away).  For
                                         // a few seconds after exit, MEASURE the
                                         // orbit-pivot-to-anchor distance and
                                         // re-track only while it's visibly broken.
static float s_otsCamDistApplied = -1.0f; // OtsDistance base tracker: re-seed the
                                          // session zoom when the slider changes.

// =====================================================================
//  DEDICATED DETACHED OTS CAMERA (2026-09-06 rebuild).  OTS now owns its
//  own camera exactly like FP owns s_fpNode — it does NOT ride the engine's
//  key->rotate.  That means WE own the cursor end-to-end (hide + center, no
//  engine cursor-sharing → the intermittent "stick" is impossible).  Cost:
//  a detached camera has no built-in collision, so otsCamDrive raycasts
//  (UtilityT::traceNoActors) to keep it from clipping walls/floors — M2.
//  Look reuses the FP raw-mouse infrastructure (only one mode is ever
//  active, so sharing the DirectInput accumulator is safe).
// =====================================================================
static bool             s_otsCamActive     = false;   // detached OTS engaged
static Ogre::SceneNode* s_otsNode          = nullptr; // our camera's own node
static float            s_otsYaw           = 0.0f;    // our look yaw (own, not engine)
static float            s_otsPitch         = 0.15f;   // our look pitch (+down)
static Ogre::Vector3    s_otsSavedCamPos;
static Ogre::Quaternion s_otsSavedCamOri;
static float            s_otsSavedFovR     = 0.0f;    // saved FOV (radians)
static float            s_otsSavedNearClip = 0.0f;
static bool             s_otsCamLocalsSaved = false;
static Ogre::Vector3    s_otsRootSm(0.0f, 0.0f, 0.0f);   // smoothed orbit root (ragdoll follow)
static bool             s_otsRootSmValid    = false;
static int              s_otsRootBlendFrames = 0;        // frames of easing after a ragdoll ends
static bool             s_otsHadAutoTrack  = false;
static bool             s_otsCursorCaptured = false;  // own mouse-look baseline
static float            s_otsZoomCur       = 20.0f;   // live zoom (base + wheel)
static float            s_otsCollDistSm    = 20.0f;   // collision-clamped distance (eased out, snaps in)
// OTS rig coherence (v1.4.1): while the detached OTS camera owns the view the
// vanilla rig is parked (stopFollowing at enter) — but the AUDIO LISTENER and
// zone/foliage streaming key off that rig, so area sounds kept playing from
// wherever it was left (user reports 2026-09; F/SelectControl "fixed" it by
// re-tracking).  Mirror FP's throttled teleport (fpDriveFrame), anchored to
// the character ROOT.
static Ogre::Vector3    s_otsLastStreamPos = Ogre::Vector3::ZERO;
static bool             s_otsLastStreamValid = false;
static const float      OTS_PITCH_LIMIT    = 1.30f;   // ~74 deg up/down clamp
static float            s_otsFovDeg        = 60.0f;   // OTS field of view (3rd-person)
static float            s_otsShoulderSign  = 1.0f;    // +1 = right shoulder, -1 = left (SwapShoulder key)
static void exitOtsCam(bool restoreCamera);           // fwd decl (enterFirstPerson uses it)
// [Settings] OTS tunables (INI + settings-tab sliders; live).
static float s_otsCamDist   = 20.0f;  // OtsDistance — orbit zoom while engaged
static float s_otsCamHeight = 15.0f;  // OtsHeight — pivot height above the feet;
                                      // ~15 ≈ shoulder line, height-scaled per
                                      // character (user-tuned default 2026-09-06;
                                      // slider range -5..20)
static float s_otsNearClipMin = 0.15f; // OtsNearClip — near plane once the camera
                                      // is pulled all the way in (wall hug /
                                      // full wheel zoom); blends back to the
                                      // game's own near clip with distance so
                                      // the close cam stops slicing the
                                      // character open (user 2026-09-11).
static float s_otsCamSide   = 4.0f;   // OtsSide — shoulder offset (+ = right, - =
                                      // left; flipped live by the SwapShoulder key)
// Set by cameraUpdate_hook (player camera) when ANY UI wants the cursor.  The poll
// thread reads it to NOT flip the crosshair toggle on CTRL presses made INSIDE a
// menu (Kenshi uses CTRL+click constantly in trade/inventory) — otherwise those
// presses corrupt the crosshair toggle/home (field 2026-06-21, merchant trade).
static volatile bool        s_camRotateUiOpen  = false;
// Poll thread: set on first WASD key press; consumed and cleared on full WASD release.
static volatile ULONGLONG   s_wasdTapStartMs = 0;
// SelectControl ("G" by default since v1.5, INI-configurable) press edge.  While DC is active,
// this hands WASD control to the currently selected (portrait-highlighted) character
// — an alternative to the double-click-portrait switch, so a plain portrait
// single-click is not the only way to change who WASD drives (user req 2026-06-28).
// Set by handleSelectPress on the down-edge (poll/native path); consumed in the main
// loop.  Vanilla F still re-centers the camera; the retarget centers on the new anchor.
static volatile bool        s_fSelectEdge    = false;
// Authoritative DC intent — set only by V-key; cleared only by V-key or hard shutdown.
static volatile bool        s_userWantsDC         = false;
// Hard-shutdown guard — set true the moment LOADGAME teardown is detected.
// All hooks check this and return immediately (calling orig only) while it is set.
// Cleared only when all six stabilization conditions are confirmed.
static volatile bool        s_dcShutdownInProgress = false;
// Per-hook-type blocked-log flags — each fires once per load event, reset in clearAllState.
static bool s_hookBlockLoggedMain    = false;
static bool s_hookBlockLoggedCharMov = false;
static bool s_hookBlockLoggedPCtrl   = false;
static bool s_hookBlockLoggedRemJob  = false;
static bool s_hookBlockLoggedAddJob  = false;
// Throttle for dc_loadgame_waiting_for_safe_reacquire — once per second.
static ULONGLONG s_shutdownWaitLogTick = 0;

// NPC loot UI suspension — blocks all V-Mode input and WASD injection while looting.
// s_mode is NOT changed; the suspend is a transparent pause that restores automatically.
static volatile bool   s_lootUiSuspendActive    = false;
static bool            s_lootUiWasPrevOpen      = false;
static ULONGLONG       s_lootSuspendStartTick   = 0;
// TRADE/LOOT classification for the inventory face-cam.  Set when
// showTradeWindow fires (a foreign party — shop/loot/corpse), cleared when ALL
// inventory windows close.  Edge-latched on purpose: the gui trade HAND fields
// (inventoryWindowTrader/NPC/tradeA/tradeB) stay STALE after a trade closes, so
// reading them directly mis-classifies the next OWN inventory as a trade.  Own
// inventory = (live window count >= 1) AND !s_tradeWindowActive — this also
// covers a character with a BACKPACK, whose own inventory opens as TWO windows
// (field 2026-06-17: cnt=2 char=1 with all trade fields 0) and used to be
// wrongly rejected by the old getNumOpenInventoryWindows()==1 test.
static bool            s_tradeWindowActive      = false;
static const ULONGLONG LOOT_SUSPEND_DEBOUNCE_MS = 250;

// Inventory "move-through" mode (user opt-in 2026-06-28 via InventoryFaceCam=false):
// instead of suspending DC while an inventory window is open, keep DC movement +
// camera lock LIVE and keep the game running so the player can WASD around while
// looting/trading.  Walking out of range lets the game close the window naturally.
// Dialogue still pauses (handled separately, never overridden).
// s_invMoveThroughActive is latched on the open edge so the close edge knows which
// path (suspend vs move-through) was taken; s_invMoveThroughForcedRun records that
// we forced the game to keep running so we only touch pause when we caused it.
static bool            s_invMoveThroughActive      = false;
static bool            s_invMoveThroughForcedRun   = false;
// Manual pause during move-through (user req 2026-08-05): Kenshi auto-pauses at
// inventory OPEN and again when the shown inventory SWITCHES to another squad
// member; only those auto-pauses are defeated (grace window after each edge).
// A pause appearing outside the grace is the PLAYER pausing — latched and
// respected (incl. across inventory switches) until they unpause themselves.
// s_invMoveThroughShownChar is IDENTITY ONLY for switch-edge detection — never
// dereferenced (the character behind a closing window can be torn down).
static bool            s_invMoveThroughPlayerPaused = false;
static bool            s_invPausedBeforeOpen        = false;  // pause state sampled while NO
                                                              // inventory is open — the open-edge
                                                              // pre-existing-pause guard (2026-08-31)
static Character*      s_invMoveThroughShownChar    = nullptr;
static ULONGLONG       s_invMoveThroughEdgeTick     = 0;
// Covers one slow UI-transition frame (mainLoop spikes to ~500 ms there) so the
// auto-pause is still caught; a player pause faster than this after opening/
// switching is eaten once — pressing pause again sticks.
static const ULONGLONG INV_MT_AUTOPAUSE_GRACE_MS   = 600;
// Merchant trade does NOT auto-close on distance in vanilla (you normally can't
// walk while trading).  Corpse/own-inventory windows DO close natively, so this
// explicit close is scoped to the trader window only.  Latched so closeTradeWindow
// fires once per session.
static bool            s_invTradeCloseRequested    = false;
// "Walk away" is measured as distance MOVED from where the controlled character
// stood when the trade opened — NOT absolute distance to the merchant, which can
// already be large at open (e.g. trading across a bar counter) and would slam the
// window shut instantly.  s_invTradeAnchorStart is captured on the first trade
// frame (zero-initialized; guarded by s_invTradeStartValid).
static bool            s_invTradeStartValid        = false;
static Ogre::Vector3   s_invTradeAnchorStart;       // zero-init (static storage)
static const float     INV_TRADE_AUTOCLOSE_DIST    = 6.0f;   // units walked from open spot
static const float     INV_TRADE_AUTOCLOSE_DIST_SQ = INV_TRADE_AUTOCLOSE_DIST * INV_TRADE_AUTOCLOSE_DIST;

// Per-frame snapshot of volatile input state — set once at the top of mainLoop_hook,
// consumed by all per-character hooks that fire during s_mainLoopOrig.
// Eliminates per-character volatile reads (memory fence overhead) for hooks that
// fire 100+ times per frame in large enemy encounters.
static ControlMode s_frameMode        = MODE_VANILLA;
static bool        s_frameWasdHeld    = false;
static bool        s_frameLootSuspend = false;

// Melee/combat awareness range.  No separate middle zone.
static const float     ATTACK_RANGE    = 500.0f;
static const ULONGLONG SCAN_INTERVAL_MS = 5000; // squad-threat scan interval

static const char* modeName(ControlMode m)
{
    return m == MODE_FREE_MOVE ? "FREE_MOVE" : "VANILLA";
}

static void setMode(ControlMode next)
{
    ControlMode prev = s_mode;
    if (next == prev) return;
    s_mode = next;
    char buf[128];
    sprintf_s(buf, sizeof(buf), "[WASDCombat] MODE: %s -> %s", modeName(prev), modeName(next));
    DebugLog(buf);
}

// =======================================================================
// Keybind configuration (v1.6) — user-configurable, international-keyboard
// friendly.  Bindings are ROLE-based: the poll thread reads the virtual-key
// code assigned to each role from s_bindVk and acts on the role, never on a
// hardcoded key.  Loaded from WASDCombatPlugin.ini (next to the plugin DLL)
// at startup; a commented default file is created if missing; any invalid
// entry falls back to its default.  Nothing downstream of the s_*Held flags
// is touched — movement, hold, point-click, camera, combat, and XP logic
// are unchanged.
// =======================================================================
enum KeyRole { KR_FORWARD = 0, KR_LEFT, KR_BACK, KR_RIGHT, KR_TOGGLE, KR_SPEED, KR_SELECT, KR_FP, KR_SNEAK, KR_OTS_SHOULDER, KR_BLOCK, KR_DODGE, KR_ATTACK, KR_MANUAL, KR_COUNT };
static int s_bindVk[KR_COUNT] = { 'W', 'A', 'S', 'D', 'V', 'X', 'G', 'P', 'C', 'H', 'F', 'Q', 'E', 'Z' };  // defaults (v1.5: control G, block F)
static const char* const KR_INI_KEY[KR_COUNT] =
    { "MoveForward", "MoveLeft", "MoveBackward", "MoveRight", "ToggleDC", "SpeedCycle", "SelectControl", "FirstPerson", "SneakToggle", "SwapShoulder", "Block", "Dodge", "Attack", "ManualAttack" };
static const char* const KR_DEFAULT[KR_COUNT] =
    { "W", "A", "S", "D", "V", "X", "G", "P", "C", "H", "F", "Q", "E", "Z" };
static char    s_bindCfgStr[KR_COUNT][32];  // resolved strings for the summary log
static int     s_uiBindVk[KR_COUNT] = { 'W', 'A', 'S', 'D', 'V', 'X', 'G', 'P', 'C', 'H', 'F', 'Q', 'E', 'Z' };
                                            // options-tab dropdown mirrors of s_bindVk:
                                            // the UI edits these; dcOptionsWatch commits
                                            // changes to s_bindVk + INI.  Seeded from the
                                            // resolved binds at the end of loadKeybinds.
static HMODULE s_thisModule = nullptr;      // captured in DllMain for the INI path

// [Settings] feature toggles (separate INI section from [Keybinds]).
// InventoryFaceCam: when true (default) the camera swings to face the character
// while their own inventory is open.  Users who loot/disarm mid-fight disliked
// the cam grabbing focus, so it is both INI-toggleable AND auto-suppressed while
// the character is in combat (user req 2026-06-20).
static bool    s_settingInventoryFaceCam = true;
// WASD speed cap (user req 2026-07-25).  The injected setDirectMovement ran at an
// uncapped ~99 move-limit, so shackled / injured / encumbered characters moved at
// full speed (Rebirth leg-shackle escape) and enemies could not catch a WASD-moving
// player.  When on, cap the WASD move-limit at the character's real max run speed
// (CharStats::getMaxRunSpeed — injury/encumbrance-aware) with an extra hard clamp
// while chained/shackled.  WasdSpeedMult scales it (1.0 = exactly legit).  Cap off
// (WasdSpeedCap=false) restores the legacy uncapped behaviour.
static bool    s_settingWasdSpeedCap     = true;

// [Settings] EnemyPursuit (v1.3.2) — feed the anchor's REAL WASD velocity to the
// enemy combat AI so enemies sprint-chase and can attack a moving player (fixes
// the "enemies stall in combat stance, kite anything" exploit — Workshop report
// 2026-08-22).  false = pre-1.3.2 behaviour (AI sees the anchor as stationary).
// Full mechanism doc at the AI MOTION FEED block above charMovUpdate_hook.
static bool    s_settingEnemyPursuit     = true;

// =======================================================================
// MANUAL COMBAT - Build 2: hold-to-block = VANILLA block mode; timed press =
// PERFECT block (FocusCombat branch, 2026-09-18, after the build-1 field test).
//
//  HOLD the Block key  -> the controlled character gets Kenshi's own standing
//      order M_SET_ORDER_DEFENSIVE_COMBAT (the orders-panel BLOCK button) for
//      exactly as long as the key is down.  Blocking is then 100% vanilla:
//      skill-based success, native pose, sparks, sound, XP, and the HUD button
//      lights.  Released -> the order is restored to what it was before.
//  PRESS the Block key as an attack comes in -> for PerfectBlockWindowMs after
//      the press edge, melee hits from the front arc are answered as a block
//      regardless of skill.  Mechanism (disassembly of 1.0.65, 2026-09-18):
//      Character::hitByMeleeAttack asks CombatClass::_iHitYouAreYouHit for a
//      verdict and, on HIT_SWORD, itself runs the WHOLE native block branch -
//      _blockHit, the "Parry"/"HtH_Impacts" sound, the weapon spark at
//      "Bip01 Prop2", block XP.  So we hook only _iHitYouAreYouHit and return
//      HIT_SWORD inside the perfect window; vanilla does everything else.
//      Then a PerfectBlockRewardMs window in which the next press is free.
//      Holding past the window falls back to the skill block above.
//  Never written: the combat state machine (2026-07-29 changeState crash).
//  Vanilla's toggle_block command is suppressed while DC is on so the same
//  key doesn't also flip the panel's block MODE underneath the hold.
// =======================================================================
static bool              s_settingManualBlock  = true;   // [Settings] ManualBlock
static bool              s_settingAiBlocks     = true;   // [Settings] AiBlocksInManual: with Z on the AI still blocks on its own (G adds the hold + perfect block)
static float             s_perfectWindowMs     = 1000.0f;// [Settings] PerfectBlockWindowMs (user 2026-09-18: 1 s)
static float             s_perfectRewardMs     = 1200.0f;// [Settings] PerfectBlockRewardMs (replaced PerfectBlockCooldownMs in v1.5)
static volatile bool     s_blockHeld           = false;  // poll thread -> main thread
static volatile ULONGLONG s_blockPressMs       = 0;      // poll thread: last press EDGE tick
static bool              s_blockHeldPrev       = false;  // main thread edge tracking
static bool              s_blockOrderApplied   = false;  // we turned DEFENSIVE_COMBAT on
static Character*        s_blockOrderChar      = nullptr;// identity only (who we applied it to)
static ULONGLONG         s_perfectCooldownUntilMs = 0;   // next perfect window allowed after
static ULONGLONG         s_perfectUsedPressMs  = 0;      // press edge already spent on a perfect block
static ULONGLONG         s_perfectLastHitMs    = 0;      // last perfect hit: a combo's follow-up is covered for PERFECT_FOLLOWUP_MS after it
static const ULONGLONG   PERFECT_FOLLOWUP_MS   = 600;    // log 2026-09-29: "downward combo-18" strikes twice, 360 ms apart
// Perfect-block REWARD (user 2026-10-08): a perfect block wipes the block
// cooldowns for PerfectBlockRewardMs - the next Block press is accepted at once,
// even right after letting go - and your weapon glows blue while it lasts.
// The first fresh press spends it.  Length = PerfectBlockRewardMs.
static ULONGLONG         s_perfectRewardUntilMs = 0;
static MyGUI::ImageBox*  s_rewardGlow          = nullptr;
static bool              s_rewardGlowTried     = false;
static ULONGLONG         s_blockInjectedPressMs = 0;     // G press already turned into a block pose
// Timed block (user 2026-09-18: no unlimited hold).  A press opens a block for
// at most BlockHoldMs (the vanilla order + pose), then it drops even if the key
// stays down, and BlockCooldownMs must pass before the next press counts.
static float             s_blockHoldMs         = 1200.0f;// [Settings] BlockHoldMs
static float             s_blockCooldownMs     = 800.0f; // [Settings] BlockCooldownMs
static ULONGLONG         s_blockAcceptedPressMs = 0;     // press that passed the cooldown gate
static ULONGLONG         s_blockSeenPressMs    = 0;      // main thread: last press edge processed
static ULONGLONG         s_blockCooldownUntilMs = 0;
// Input forgiveness (user 2026-09-21): an accidental repeat must never restart
// a move.  Extra presses of a combat key inside InputRepeatGuardMs of the last
// accepted press are ignored; a Block re-press that soon after letting go
// RESUMES the same block (no cooldown, hold budget + perfect window still run
// from the first press, pose kept); a swing / dodge that just ended is followed
// by a short recovery in which new presses are ignored (follow-through plays).
static float             s_inputRepeatGuardMs  = 250.0f; // [Settings] InputRepeatGuardMs
static ULONGLONG         s_blockHoldStartMs    = 0;      // origin of the hold budget + perfect window (survives a resume)
static ULONGLONG         s_blockReleaseMs      = 0;      // tick of the last release (resume window)

// --- Build 11: MANUAL DODGE = an ACTION on the key ---------------------------
// Disassembly of 1.0.65 (see _research_RE):
//  * CombatClass::changeState(state, minTime) is three writes: stateTimer,
//    nextMove, combatState.  (The 2026-07-29 "changeState crash" was a call
//    at a STALE header RVA - real 2B2A60, header 2B25F0 - not the writes.)
//  * The BLOCK state (blockState) is self-sufficient: given currentTechnique
//    and a blocking target it starts the technique animation itself (its own
//    play call, every frame until progress passes the technique's end) and
//    hands the machine back to DECISION when the dodge is done.
//  * Dodge techniques come from CharStats::chooseBlock; vanilla only rolls
//    Dodge for the UNARMED category and by chance.  For the PICK we present
//    the character as unarmed and force the dodge chance to 100 so a dodge is
//    always chosen (still tier-gated by skill); SUCCESS is a separate, honest
//    roll of calculateDodgeChance (Dodge +DodgeSkillBonus, the flat +20 the
//    BLOCK toggle gives melee defence) vs the attacker's melee attack.
// Press (in combat, off cooldown) -> commit window (WASD buffers, the AI runs
// and re-engages combat if you were moving) -> on the first idle combat frame:
// pick, roll, inject (the four changeState writes + technique + target).  The
// animation always plays; a failed roll means the verdict hook lets the hit
// through (technique hidden from the verdict for that hit).  No i-frames from
// us: a successful dodge is protected only by the technique's own window.
static volatile bool     s_dodgeHeld           = false;
static volatile ULONGLONG s_dodgePressMs       = 0;      // poll thread: press EDGE tick
static float             s_dodgeWindowMs       = 600.0f; // [Settings] DodgeWindowMs (press must be this close to the attack)
static float             s_dodgeCooldownMs     = 300.0f; // [Settings] DodgeCooldownMs (the in-flight lock is the real gate)
static float             s_dodgeSkillBonus     = 20.0f;  // [Settings] DodgeSkillBonus (block mode's melee-defence bonus is +20)
static ULONGLONG         s_dodgeCooldownUntilMs = 0;
static float             s_dodgeRecoveryMs     = 400.0f; // [Settings] DodgeRecoveryMs (after a dodge ends)
static ULONGLONG         s_dodgeRecoveryUntilMs = 0;
static ULONGLONG         s_dodgeAcceptedPressMs = 0;     // last press that passed the gates (repeat guard)
// Stagger policy (user 2026-09-21, final): a stagger plays through.  NO input
// counts while staggered - E, Q and G presses are DROPPED (never buffered) and
// anything pending dies when it starts; inputs count again the frame it ends.
// (Build 42-49 let one Q through on a long stagger; removed by user request.)
static ULONGLONG         s_staggerStartMs      = 0;      // rising edge of the current stagger (0 = not staggered)
static ULONGLONG         s_staggerRawSinceMs   = 0;      // raw staggered signal edge (debounced into s_staggerStartMs)
static const ULONGLONG   STAGGER_DEBOUNCE_MS   = 100;    // log 2026-09-21: 31-47 ms stumble flickers ate a Dodge press
static ULONGLONG         s_dodgeInjectedPressMs = 0;     // press already turned into a dodge
static GameWorld*        s_gwFrame             = nullptr;// refreshed every mainLoop frame (foe scan)

// --- Build 17: MANUAL ATTACK (press) - same shape as the dodge -----------------
// Disassembly (1.0.65): the AI starts a swing by moving to STARTUP with
// nextMove = CHOP_WEAPON; CombatClassAI::startupState then re-asserts the
// attack target, and calls getStateClass(nextMove)->initialise() =
// AttackState::_NV_initialise -> initialiseAttack(target): nearest enemy in
// the attack zone, victim notified (attackingYou), CharStats::chooseAttack
// (skill/weapon-gated, native), the technique clip started (same play call as
// the dodge), targets in the attack zone computed - then combatState =
// CHOP_WEAPON and the attack state runs the impact checks and damage.  If the
// target is out of reach it steps in (combatMovementOffensive) and retries.
// So an E press only needs: a foe as the current target + the three
// changeState(STARTUP) writes with nextMove = CHOP_WEAPON.  Everything else -
// technique choice, animation, hit detection, XP, sounds - is vanilla's.
static volatile bool     s_attackHeld          = false;
static volatile ULONGLONG s_attackPressMs      = 0;
static float             s_attackWindowMs      = 400.0f; // [Settings] AttackWindowMs (press must find the AI within this)
static ULONGLONG         s_attackCommitUntilMs = 0;      // press live: WASD buffers, AI runs
static ULONGLONG         s_attackSeenPressMs   = 0;
static ULONGLONG         s_attackInjectedPressMs = 0;
static ULONGLONG         s_attackAcceptedPressMs = 0;    // press that passed the gates (ignored presses never inject)
static bool              s_attackActive        = false;  // our swing in flight (STARTUP -> CHOP_WEAPON)
static ULONGLONG         s_attackActiveSinceMs = 0;      // bounded: a swing that never starts is released
static bool              s_attackSawChop       = false;  // the swing actually started (CHOP_WEAPON seen)
static float             s_attackRecoveryMs    = 400.0f; // [Settings] AttackRecoveryMs (after a swing ends)
static ULONGLONG         s_attackRecoveryUntilMs = 0;
static ULONGLONG         s_attackRetryPressMs  = 0;      // press being retried after a fizzled start
static ULONGLONG         s_attackAfterDodgeMs  = 0;      // E pressed during our dodge: fires when the dodge ends (build 49; 43 such presses in the build-48 log)
static int               s_attackRetries       = 0;      // bounded (a start that keeps failing is given up)
static float             s_attackApproachMs    = 1500.0f;// [Settings] AttackApproachMs: out of reach -> keep closing in this long
static ULONGLONG         s_attackReapproachLogMs = 0;

// --- MANUAL ATTACK mode (Z toggle) -----------------------------------------------------
// While ON, in DC: the combat AI's OWN swings are denied at the game's attack
// initialiser (and any stray transition into the attack state becomes the
// armed ready stance), AND its own block/dodge choice is denied at the
// technique chooser - so the character takes no combat action by itself
// (stagger reactions stay vanilla); G / Q / E are the only responses and the
// character can be hit if the player does nothing.  Targeting stays vanilla
// (the AI's current target).  A small fixed "Manual Attack" label shows while ON.
static volatile ULONGLONG s_manualPressMs      = 0;
static ULONGLONG         s_manualSeenPressMs   = 0;
static bool              s_manualAttackOn      = false;
static ULONGLONG         s_manualSuppressLogMs = 0;
static ULONGLONG         s_manualPinLogMs      = 0;
static int               s_anchorPrevState     = -1;     // DIAG: the anchor's combat state last frame (which state started a refused swing?)
static MyGUI::TextBox*   s_manualHud           = nullptr;
static bool              s_manualHudTried      = false;

// --- AIM FOCUS (user 2026-09-21; replaces the frozen lock-on) ----------------------
// While manual combat (Z) is on, the enemy you POINT AT is the combat AI's
// target, re-evaluated live: the enemy under the mouse cursor when the cursor
// is free (vanilla camera, or a UI open), else the enemy nearest the view
// centre (FP / OTS crosshair) inside the aim cone.  The camera is NEVER moved
// for it (the old lock eased the OTS yaw onto the target and fought the
// mouse).  A brief hold keeps the focus through a cursor slip.  The same pick
// is the foe for E / Q / G.  s_lockTarget = the current aim focus (identity
// only; validated against the update list before every use).
static Character*        s_lockTarget          = nullptr;
static float             s_aimConeDeg          = 35.0f;  // [Settings] AimConeDeg (crosshair pick)
static float             s_aimRange            = 40.0f;  // units
static float             s_aimHoverPx          = 70.0f;  // [Settings] AimHoverPx (cursor pick radius)
static ULONGLONG         s_aimEvalMs           = 0;      // 20 Hz evaluation throttle
static Character*        s_aimLogged           = nullptr;// edge log
// RIGHT-CLICK focus (user 2026-09-22): with manual combat on and the character
// in combat, a right-click on / toward an enemy at ANY distance makes them the
// focus instantly (the hover/crosshair pick only reaches AimRange).  A clicked
// focus persists until the target is gone, the next click, or the cursor is
// pointed straight at a different enemy; the crosshair cone does not override it.
static volatile ULONGLONG s_rmbAimPressMs      = 0;      // poll thread: RMB press edge
static ULONGLONG         s_rmbAimSeenMs        = 0;
static bool              s_aimClicked          = false;  // the current focus came from a click
// Markers are GUI-space (Kenshi's DX11 renderer needs shader materials for raw
// 3D geometry).  ONE shared 64x64 soft-glow texture for every marker: the DX11
// backend pads texture rows, so the old 24 px dot texture (96-byte rows) was
// written with the wrong pitch and showed as a garbled white square (builds
// 32-45) while the 64 px ring (256-byte rows) drew fine.  The ring itself is
// gone (user 2026-09-21).
static const int         GLOW_TEX_PX           = 64;
static bool              s_glowTexTried        = false;
static bool              s_glowTexOk           = false;
static bool              s_settingAimName      = true;   // [Settings] AimNameTag
// Aim-focus marker (user 2026-09-22): the enemy's NAME above their head, in the
// game's own small text skin with a shadow - like the vanilla name tag - instead
// of the glowing dot (the glow texture stays for the weapon glints).
static MyGUI::TextBox*   s_nameTag             = nullptr;
static bool              s_nameTagTried        = false;
static Character*        s_nameTagFor          = nullptr;// identity: whose name the caption holds
// Health bars under the name (user 2026-09-29): BLOOD, HEAD, CHEST, STOMACH as
// thin strips (dark background + coloured fill), read live from the target's
// MedicalSystem (blood / getMaxBlood, part flesh / _maxHealth).  Same show/hide
// as the name tag.
static const int         HB_COUNT              = 4;      // 0 blood, 1 head, 2 chest, 3 stomach
static MyGUI::Widget*    s_hbBg[HB_COUNT]      = {};
static MyGUI::Widget*    s_hbFill[HB_COUNT]    = {};
static bool              s_hbTried             = false;
static bool              s_hbNamesLogged       = false;  // DIAG: the body-part names seen, once
static const int         HB_W = 64, HB_H = 3, HB_GAP = 7;   // 3 px bars on a 10 px pitch (room for the labels)
static MyGUI::TextBox*   s_hbLabel[HB_COUNT]   = {};     // tiny captions left of each bar (user 2026-09-29)
static const int         HB_LW = 46, HB_LH = 12;         // label box (game's smallest text skin, ~9 px)
static const int         HB_RAISE = 42;                   // whole name+bars group lifted so the last bars clear the head
// Vanilla SPEECH TEXT (user 2026-09-30): while the tagged enemy has a speech
// bubble up, the name+bars group steps ABOVE it by the bubble's real on-screen
// height and drops back when it is gone.  Build 66: bubbles are found by
// scanning MyGUI's root widgets for a visible EditBox with text near the head
// (MyGUI owns their lifetime).  Build 64 tracked DialogueSpeechBubble objects
// through ctor/dtor hooks, and a bubble deleted through a path the hook did not
// see left a dangling pointer read every frame - an access violation inside
// the game's main loop that another plugin's handler swallowed, so the rest of
// the game update was skipped each frame = "toggling Z pauses the game".
// WEAPON GLINT (user concept 2026-09-21): an enemy's weapon glints while their
// swing is coming at you - the visual cue for Block / Dodge / a counter.
// Signal = Character::attackingYou, the game's own "I am swinging at you"
// notification sent to the victim at swing start (the AI blocks off the same
// signal).  The glint ramps from swing start to GlintLeadMs, holds bright, and
// flashes out at impact (the block verdict) or when the swing ends.
static bool              s_settingGlint        = true;   // [Settings] WeaponGlint
static float             s_glintLeadMs         = 700.0f; // [Settings] GlintLeadMs (ramp to full brightness; log 2026-09-21: swing start -> impact median 844 ms)
static float             s_glintPx             = 22.0f;  // [Settings] GlintPx (on-screen size at full)
static const int         GLINT_MAX             = 4;      // attackers tracked at once
struct GlintSlot { Character* who; ULONGLONG startMs; ULONGLONG chopMs; ULONGLONG impactMs; int tier; };
// Glint COLOUR = the defence that works (user 2026-10-01, "honest arc rule"):
//   tier 0 WHITE  = the hit lands inside the front arc -> a Block tap stops it
//   tier 1 ORANGE = outside the arc (side / rear) -> the perfect block is
//                   refused by design; Dodge (direction-free) or the skill block
//   tier 2 RED    = heavy tier (heavy weapon, or impact power >= GlintHeavyPower)
//                   -> dodge preferred even from the front
// Re-evaluated every frame with the SAME test the block verdict applies, so
// turning to face the attacker flips orange -> white live.
static bool              s_glintColourTiers    = false;  // [Settings] GlintColourTiers (OFF for the v1.5 release, user 2026-10-03: all glints white; colours = next update)
static bool              s_glintHeavyTier      = true;   // [Settings] GlintHeavyTier (heavy = the technique's skill list says SKILL_HEAVY; log 2026-10-01: the attacker's currentWeaponType read 0 for everyone and ImpactPoint power is a flat 1)
static GlintSlot         s_glint[GLINT_MAX]    = {};     // who = identity only, validated each frame
static MyGUI::ImageBox*  s_glintBox[GLINT_MAX] = {};
static bool              s_glintTried          = false;
static CombatTechniqueData* s_dodgeActiveTech  = nullptr; // the technique we injected (identity only)
static bool              s_dodgeRollOk         = false;  // honest roll for the active dodge
// Mod-owned COPIES of the game's dodge techniques (user design 2026-09-18):
// deep-copied once from the vanilla pool, never freed, never seen by the
// chooser/AI - Q hands one straight to the block state.  Vanilla stays vanilla.
struct DcDodgeCopy { CombatTechniqueData* tech; bool stumbleOnly; float weight; };
static DcDodgeCopy       s_dcDodge[8];
static int               s_dcDodgeCount        = 0;
static CombatTechniqueData* s_dcBlock[8];                  // mod-owned copies of the BLOCK techniques
static int               s_dcBlockCount        = 0;
static bool              s_dcDodgeBuilt        = false;
// The technique-animation START call (what initialiseBlock does right after
// choosing a dodge).  Not exported by KenshiLib (no AnimationClass header), so
// it is resolved at its 1.0.65 address and BYTE-VERIFIED before the first use;
// on any mismatch the dodge is disabled with a log line instead of a call.
// Signature (from the disassembly): rcx = AnimationClass*, rdx = technique,
// xmm2 = speed (blockSpeed * animSpeedMultiplier), r9 = std::string* suffix.
typedef void (*DcTechPlayFn)(void* animClass, CombatTechniqueData* tech, float speed, std::string* suffix);
static DcTechPlayFn      s_dodgePlayFn         = nullptr;
static bool              s_dodgePlayChecked    = false;
static const uintptr_t   DC_DODGE_PLAY_RVA     = 0x5B6D70;   // Kenshi 1.0.65 (Steam/GOG RE_Kenshi exe)
static ULONGLONG         s_dodgeCommitUntilMs  = 0;      // live press = COMMITTED action: WASD buffers, the AI runs
static ULONGLONG         s_dodgeSeenPressMs    = 0;      // main thread: last press edge processed by manualBlockTick

static float   s_settingWasdSpeedMult    = 1.0f;
// Combat-mode FLICKERS (isInCombatMode blips false between swings / when the
// target is momentarily not engaged).  A single false frame at inventory-open
// used to latch the face-cam on (then the 16-frame close-debounce kept it up
// through the fight).  So "in combat" is treated as a HARD exit (no debounce) and
// held for a short grace after the last in-combat frame to smooth the flicker and
// cover looting/disarming right as a fight ends.
static ULONGLONG s_lastInCombatMs = 0;
static const ULONGLONG FACECAM_COMBAT_GRACE_MS = 1500;

// =======================================================================
// Inventory face-cam — a temporary DETACHED Ogre camera (s_fpNode) that swings
// around to face the selected character's front while their OWN inventory is
// open, so worn gear is visible.  The gameplay over-the-shoulder ("OTS") toggle
// this plumbing was originally built for was SCRAPPED (Kenshi's interior floor
// render is welded to the top-down RTS camera, mutually exclusive with a
// detached view); only the inventory face-cam survives.  Runs in
// cameraUpdate_hook (works under the inventory pause).  Kenshi scale ≈ 10cm/unit.
// =======================================================================
static bool             s_fpActive          = false;
// Inventory face-cam exit debounce.  The camera update hook is called TWICE per
// frame and the two calls DISAGREE on the own-inventory window count (one sees
// 1, the other does not) — so a naive enter/exit toggled detach/re-attach EVERY
// frame (field 2026-06-16/17: 4349 dc_cam_entered/exited pairs while one
// inventory was open).  That left s_fpActive true only half the frames, which
// silently broke the altitude-hold (scroll still zoomed name-tags) and the
// s_fpActive-gated point-click suppression (clicks landing on an "exit" frame
// walked the character).  Fix: require the own-inventory signal to be ABSENT for
// several consecutive calls before exiting, so a single dissenting per-frame
// call can no longer drop the face-cam.
static int              s_invFaceCloseStreak = 0;
static const int        INV_FACE_CLOSE_DEBOUNCE = 16;
// World name-tag (the floating squad/character labels above heads) hide while
// the inventory face-cam is up.  Scrolling/WASD still nudged those labels and
// camera-input gating couldn't stop it cleanly, so instead we just hide them
// for the duration (user request 2026-06-17) and restore the player's setting
// (`options->showNames`) on close.  s_savedShowNames is read BEFORE hiding so a
// showNames() that also writes the option can't poison the restore.
static bool             s_namesHidden       = false;
static bool             s_savedShowNames    = true;
static bool             s_fpCursorCaptured  = false;
static float            s_fpSensitivity     = 1.0f;
static float            s_fpYaw             = 0.0f;   // radians; fwd=(-sin,0,-cos)
static float            s_fpPitch           = 0.0f;
static float            s_otsDistCur        = 14.0f;  // face-cam camera distance
static bool             s_otsInvFaceActive  = false;  // own-inventory face-cam engaged
static Character*       s_otsInvFaceChar    = nullptr; // who it is aimed at (identity-compared only)
// Shoulder view saved on the FIRST inventory open, restored on close so the
// player returns to exactly the over-the-shoulder framing they had before.
static float            s_otsSavedYaw       = 0.0f;
static float            s_otsSavedPitch     = 0.0f;
static float            s_otsSavedDist      = 14.0f;
static float            s_fpFovDeg          = 65.0f;
static float            s_fpNearClip        = 0.2f;
// Crosshair/cursor horizontal offset as a fraction of client width (+ = right
// of center) so the crosshair clears the character body for selection clicks
// (user req 2026-06-13).  The cursor is pinned to this point and mouse-look
// deltas are measured from it.
static float            s_otsCrosshairOffsetX = 0.10f;  // 0.12 -> 0.10 (closer to the character, user req 2026-06-16)
static float            s_fpSavedNearClip   = 0.0f;
static Ogre::Radian     s_fpSavedFov;
static bool             s_fpCamLocalsSaved  = false;
static Ogre::Vector3    s_fpSavedCamPos     = Ogre::Vector3::ZERO;
static Ogre::Quaternion s_fpSavedCamOri;
static Ogre::SceneNode* s_fpNode            = nullptr;
static bool             s_fpHadAutoTrack    = false;
// Set when a load/teardown interrupts OTS: the detached Ogre camera persists
// across a save-load, so it must be re-attached to the rig AFTER the world is
// valid again (doing it mid-load crashed; not doing it left the camera stuck
// orphaned at the OTS position — field 2026-06-13).
static bool             s_otsRestorePending = false;
static float            s_otsSavedAltitude  = 0.0f;  // face-cam: held to block scroll-zoom

// =======================================================================
// First-person camera (v1.8-fp, ported into the live line 2026-07-22).  A
// SECOND drive mode for the SAME detached-camera machinery as the inventory
// face-cam above — both detach the Ogre camera onto s_fpNode and save/restore
// the camera locals (s_fpSavedCamPos/Ori/Fov/NearClip, s_fpHadAutoTrack,
// s_fpCamLocalsSaved) the same way, and both drive in cameraUpdate_hook.  They
// differ ONLY in what triggers them and where the camera is placed each frame:
//   * s_fpActive          = the INVENTORY FACE-CAM owns the detached view.
//   * s_firstPersonActive = FIRST-PERSON owns it (eye at the head bone, look out).
// The two are mutually exclusive (never both true — the camera has one node).
// Opening the inventory while first-person is active SUSPENDS first-person for
// the face-cam and AUTO-RETURNS to first-person when the inventory closes
// (s_fpSuspendedForInv).  Toggled by the FirstPerson key (default P) in DC.
// =======================================================================
static bool  s_firstPersonActive   = false;  // detached cam is in first-person drive mode
// s_userWantsFP — PERSISTENT first-person INTENT (mirrors s_userWantsDC).  Survives
// survivable loads (chunk streaming / micro-loads / save-load into gameplay) so FP is
// re-entered after the scene rebuilds; cleared only on P-off, DC-off (V), new game, or
// hard teardown.  Fixes "FP disables when loading between chunks" (user 2026-07-29).
static bool  s_userWantsFP         = false;
static volatile bool s_fpToggleRequested = false;  // P-key edge -> consumed on game thread
static bool  s_fpSuspendedForInv   = false;  // FP paused while the inventory face-cam runs
static float s_fpSensitivityFP     = 1.0f;   // [FirstPerson] Sensitivity (mouse-look scale)
static float s_fpEyeHeight         = 16.5f;  // [FirstPerson] EyeHeight (fallback neck model)
static float s_fpFwdOffset         = 1.0f;   // [FirstPerson] ForwardOffset (v1.4.0 tuned default)
static float s_fpFovDegFP          = 75.0f;  // [FirstPerson] FOV (wide first-person view)
static float s_fpNearClipFP        = 1.0f;   // [FirstPerson] NearClip (1.0 since build 71: 0.1 gave 50x coarser depth than vanilla's 5.0/30000 -> distant grass z-fought and shimmered on every pan; 1.0 = 5x, user-confirmed sweet spot vs body clipping)
static float s_fpNeckLimitRad      = 1.745f; // [FirstPerson] NeckLimit (deg -> rad; 100 deg — user-pictured extents 2026-08-23)
static float s_uiNeckLimitDeg      = 100.0f; // options-tab slider mirror of NeckLimit: the UI edits
                                             // DEGREES; dcOptionsWatch converts to s_fpNeckLimitRad
                                             // on change, loadFirstPersonConfig syncs it back.
static float s_fpSideLookFwd       = 1.0f;   // [FirstPerson] SideLookForward — extra eye reach at side-look so the shoulder clears the lens
static bool  s_fpHideHair          = true;   // [FirstPerson] HideHair
static bool  s_fpHideHead          = false;  // [FirstPerson] HideHead — LEGACY bone shrink.  Retired
                                             // as the default hide in v1.4.0 (HeadPixelHide), then
                                             // RE-EXPOSED in v1.4.1 (INI + settings row): the pixel
                                             // clip assumes the vanilla UV atlas, so custom races
                                             // with their own layouts need the bone shrink instead
                                             // (user reports 2026-09).  Also the auto-fallback when
                                             // another mod's character.hlsl wins the filename race.
static bool  s_fpHideInjuryOverlay = true;   // [FirstPerson] HideInjuryOverlay — suppress hand2head/handtowounds clips in FP
// (HideGear — a whole-attachment hide in FP — was tried in v1.3.2 and REVERTED
// 2026-08-26: it silently overrode the HideHair setting, which stopped doing
// anything.  HideHair/shaveHead is the sole hair control again.
// REINSTATED + REVERTED AGAIN 2026-08-31 during the zero-offset camera test:
// user verdict "didn't work" — it did not fix the in-camera hat/head-artifact
// view.  Do not retry without new evidence of what the artifact actually is.)
static bool  s_fpHairHidden        = false;  // restore tracking
static bool  s_fpHeadBoneHidden    = false;  // restore tracking
// FP head hidden-parts mask (2026-08-31, user-proposed): Kenshi's OWN
// per-pixel body-part hide — skin.hlsl line 134: "oWet.z = (partData.y &
// hiddenMask) ..." — the mechanism full-face helmets use to erase heads.
// partData is GENERATED at body build (it is NOT in the mesh files: vdecl
// scan shows no such channel), so this path is independent of every mesh/
// bone mechanism that failed (spatially re-rigged mesh STILL spun,
// 2026-08-31).  We OR our bits into the body material's "hiddenMask"
// vertex-program constant; restore = AppearanceHuman::updateHiddenParts()
// re-derives the truth from equipment.  Bits INI-tunable live (HeadMaskBits).
// FP head PIXEL hide (2026-08-31, the shader-override endgame): the 65535
// mask test proved (a) the parameter write path reaches pixels (torso+feet
// erased) and (b) HEAD pixels carry NO part bits — unmaskable by the part
// system.  So the mod ships its own character.hlsl (resource override —
// filename race proven won for meshes) adding `uniform float dcHideHead` +
// `clip(texCoord.y + 1.0 - dcHideHead)`: head-region UVs live below y=0, so
// 1.0 discards exactly the head-region pixels — including runtime-generated
// face geometry that ignores bones/rigging/part bits.  Default 0 = vanilla
// (GpuProgramParameters buffers zero-init), so only the FP anchor is touched.
static bool s_fpHeadPixelHide    = true;  // [FirstPerson] HeadPixelHide
static bool s_fpHeadPixelApplied = false; // restore tracking
// Pixel-hide auto-fallback (v1.4.1): if the dcHideHead uniform is missing from
// every body subentity, another mod's character.hlsl won the resource filename
// race (field 2026-09: Character Highlight ships its own copy — save-load crash
// thread) and the clip can never fire this session.  Engage the legacy bone
// shrink instead so the head still hides.  Cleared on FP exit and teardown.
static bool s_fpPixelFallbackOn  = false;
// FP headgear hide (2026-08-31, post-pixel-clip): hats + beards are separate
// entities with their own materials — the character.hlsl clip can't reach
// them.  Hats + scalp piece = the appearance-attachment registry
// (setAttachmentsVisible, the v6 mechanism); beards = the entity walk (they
// are OUTSIDE the registry — 2026-08 saga).  BOTH re-asserted per frame,
// because the engine re-derives facial-attachment visibility every frame
// (proven by the beard 1 Hz logs) — the same fight the injury-overlay
// suppression wins with per-frame post-orig placement.  Independent of
// HideHead (the user's config runs the pixel clip with HideHead=0).
static bool s_fpHideHeadgear = true;  // [FirstPerson] HideHeadgear
// Per-bone saved scale for the head shrink (doc at fpSetHeadBoneHidden).
// Kenshi sizes characters partly through bone scales — originals must be
// restored EXACTLY, never assumed 1.0.
static Ogre::Vector3 s_fpHideBoneSavedScale[2];
static bool          s_fpHideBoneSaved[2] = { false, false };
// HideHead beard companion (2026-08-31): mesh NAMES (never pointers — Rule 5)
// of beard entities WE hid, so restore only re-shows what we touched — a beard
// vanilla hid (helmet hide-beard flag) must survive an FP round-trip hidden.
static const int FP_BEARD_MAX = 8;   // beard + hat + modded hair etc.
static char s_fpHiddenBeards[FP_BEARD_MAX][96];
static int  s_fpHiddenBeardCount = 0;
// Anchor's head-bone height above the root, measured live in fpGetHeadWorld —
// scales EyeDrop so custom-height characters keep the same RELATIVE eye level
// (user req 2026-08-24).  16.0 = the standard-height reference EyeDrop=1.5 was
// tuned on.
static float s_fpHeadAboveRoot = 16.0f;
static float s_fpLeanFwd           = 0.0f;   // smoothed forward lean (fallback model only)
static Ogre::Vector3 s_fpHeadSmooth      = Ogre::Vector3::ZERO;  // smoothed head-bone eye
static bool          s_fpHeadSmoothValid = false;
static bool          s_fpBoneLogged      = false;  // one-shot head-bone tracking log
static float         s_fpBoneEyeUp       = 1.0f;   // eye height above the resolved mount bone
static float         s_fpEyeUpAdjust     = 0.0f;   // [FirstPerson] EyeUpAdjust — INI nudge added
                                                   // to the bone-mount eye height; raise to keep
                                                   // the chest/shoulders below frame when moving
static float         s_fpMoveLeanUp      = 0.0f;   // [FirstPerson] MoveLeanUp — extra eye height
                                                   // scaled by move speed (0..1); counters the
                                                   // forward jog/sprint lean that swings the body up
static float         s_fpMoveLeanFwd     = 0.0f;   // [FirstPerson] MoveLeanForward — extra eye
                                                   // forward scaled by move speed; pushes past the
                                                   // leaning torso/arms at jog/sprint speed
static float         s_fpMoveNearClip    = 0.0f;   // [FirstPerson] MoveNearClip — near-clip distance
                                                   // blended in by move speed; slices the arm/torso
                                                   // that sweeps into the lens at jog/sprint. 0 = off
static float         s_fpMoveLeanSmooth  = 0.0f;   // runtime: smoothed 0..1 speed factor
static float         s_fpJogForward      = 2.0f;   // [FirstPerson] JogForward — eye pushed forward along
                                                   // body facing while actively JOGging (closes the gap
                                                   // the forward jog lean opens; 0 = off)
static float         s_fpRunForward      = 4.0f;   // [FirstPerson] RunForward — same, while RUN/sprinting
static float         s_fpGaitFwdSmooth   = 0.0f;   // runtime: smoothed gait forward offset (units)
static float         s_fpActionClrSmooth = 0.0f;   // runtime: smoothed committed-action forward clearance (units)
static ULONGLONG     s_lastRigTeleportMs = 0;      // DIAG: last CameraClass::teleport we issued (rig coherence)
static float         s_fpStreamDist      = 2.0f;   // [FirstPerson] StreamUpdateDist — min WORLD UNITS (~10 cm each,
                                                   // so 2.0 = ~20 cm) the character ROOT must move before we
                                                   // re-teleport the game rig for zone/foliage
                                                   // streaming. Per-frame teleport (=0) re-pages grass every
                                                   // frame => flicker while moving; throttling to ~2m streams
                                                   // smoothly. Big value (e.g. 999) ~= never re-stream (test).
static Ogre::Vector3 s_fpLastStreamPos   = Ogre::Vector3::ZERO;  // runtime: last rig-teleport position
static bool          s_fpLastStreamValid = false;                // runtime: has s_fpLastStreamPos been set
static float         s_fpGrassRangeMult  = 1.0f;   // [FirstPerson] GrassRangeMult — while FP is active, multiply
                                                   // options->grassRange/foliageRange so grass loads in a wider
                                                   // ring. Kenshi grass range is tuned for the high top-down cam;
                                                   // at ground level the short ring pops at its edge as you turn/
                                                   // move. 1.0 = off (vanilla). Restored exactly on FP exit.
                                                   // NOTE: raising range does NOT fix the distant re-scatter —
                                                   // it just renders more grass that still re-scatters (user
                                                   // 2026-07-29). Reverted to 1..8 clamp / live 1.0.
static float         s_fpSavedGrassRange   = 0.0f;  // runtime: options->grassRange   saved on FP enter
static float         s_fpSavedFoliageRange = 0.0f;  // runtime: options->foliageRange saved on FP enter
static bool          s_fpOptRangeSaved     = false; // runtime: are the two saved values valid
static float         s_fpFollowTurn        = 0.08f; // [FirstPerson] FollowTurn — while the character moves on ITS
                                                    // OWN (point-click / combat / heal approach, NOT WASD), lerp the
                                                    // view yaw toward the movement heading so the camera follows the
                                                    // character like WASD does. 0 = off (view stays on mouse-look).
                                                    // YIELDS to the mouse: only eases in after FollowDelayMs of no
                                                    // mouse-look, so you can freely look around while moving.
static float         s_fpFollowDelayMs     = 400.0f;// [FirstPerson] FollowDelayMs — how long the mouse must be still
                                                    // before the FollowTurn auto-recentre kicks in. Higher = look
                                                    // around longer before the camera drifts back onto the path.
static ULONGLONG     s_fpLastMouseMoveMs   = 0;     // runtime: last tick the player actively moved the look
static int           s_fpFoliageCenterMode = 1;     // [FirstPerson] FoliageCenterFix — reconcile the camera CENTER
                                                    // node (Kenshi's grass/foliage streaming anchor) with the eye
                                                    // WHILE MOVING, via _setDerivedPosition + forced recompute
                                                    // (KenshiFP technique).  1 = on, 0 = off (legacy under-foot pop).
                                                    // At idle the center is left vanilla so panning can't re-scatter.
static float         s_fpLookSmooth        = 0.4f;  // [FirstPerson] LookSmooth — 0..0.9: smooths the RENDERED view
                                                    // orientation so each frame's rotation delta is smaller, which
                                                    // shrinks the 1-frame render-thread grass re-facing mismatch =
                                                    // less side-to-side foliage flicker. Costs a little mouse-look
                                                    // snappiness (camera "glide"). 0 = off/raw (default).
static float         s_fpYawSm             = 0.0f;  // runtime: render-smoothed yaw (== s_fpYaw when LookSmooth=0)
static float         s_fpPitchSm           = 0.0f;  // runtime: render-smoothed pitch
static bool          s_fpSmValid           = false; // runtime: smoothed view initialised this FP session

// --- KenshiFP-style camera port (2026-07-30) ------------------------------
// The reference mod (github.com/linguine2552/KenshiFP) welds the eye to the head
// bone's TRUE world position (via the game's own Character::getBoneWorldPosition)
// and reads look input from a 1kHz DirectInput device, instead of our synthetic
// "root + rotated model-offset" eye + cursor-warp look.  Ours reconstructed the
// head position by rotating the whole ~2m bone offset by the VIEW yaw, which
// swung the eye on an arc during pure rotation (the grass-repage/camera-swing we
// fought for weeks) and never tracked the real animated head.  These toggles let
// us A/B the new path against the old one.
static bool  s_fpTrueBoneEye  = true;   // [FirstPerson] TrueBoneEye — 1 = weld the eye to the head bone's
                                        // real world position (getBoneWorldPosition); 0 = old synthetic path.
static float s_fpEyeDrop      = -0.2f;  // [FirstPerson] EyeDrop — drop below the head-bone origin to eye level (-0.2 = v1.4.0 tuned default; the old 0.06 near-clip parking is obsolete — the head is pixel-hidden)
                                        // (units of the mount bone height; the head bone sits above the eyes).
static bool  s_fpRawMouse     = true;   // [FirstPerson] RawMouse — 1 = 1kHz DirectInput look deltas (framerate-
                                        // independent, smooth); 0 = cursor-warp deltas (old, fps-dependent).
static float s_fpMoveForward  = 0.0f;   // [FirstPerson] MoveForward — speed-scaled forward LEAD so the eye leads
                                        // faster movement instead of lagging the leaning head (KenshiFP).  0 = off.
static float s_fpMoveSpeedRef = 30.0f;  // [FirstPerson] MoveSpeedRef — ground speed (feet-delta/sec) that maps to
                                        // full run = lead 1.0.  Tune so a full sprint gives ~the MoveForward push.
// runtime: feet-delta ground-speed tracker that drives the forward lead (KenshiFP
// keys the lead off ON-SCREEN speed, not the noisy currentMotion magnitude — the
// same signal our own notes found unreliable, 2026-07-24).
static bool      s_fpHaveLastFeet   = false;
static float     s_fpLastFeetX      = 0.0f;
static float     s_fpLastFeetZ      = 0.0f;
static float     s_fpLastFeetY      = 0.0f;   // vertical feet sample (stair-climb detector)
static float     s_fpMoveSpeed      = 0.0f;   // low-passed ground speed (units/sec)
static float     s_fpMoveFwdSmooth  = 0.0f;   // smoothed forward-lead offset (units)
static float     s_fpClimbSpeedSmooth = 0.0f; // low-passed vertical speed (units/sec, + = ascending)
static ULONGLONG s_fpFeetTickMs     = 0;      // last feet-speed sample tick

// Ascent-aware forward pullback (user 2026-07-31): on stairs the horizontal
// ForwardOffset drives the eye INTO the rising steps.  While the character climbs
// (positive vertical speed) scale the forward push down toward StairForwardMinScale
// and lift the eye by StairEyeLift, so the camera stops jamming into the steps —
// full offset is restored on flat ground so the body still never clips.
// REBALANCED v1.4.1 (field 2026-09: "seeing inside my armor going uphill"):
// the old defaults (Reduce 0.25 / MinScale 0.30 / Lift 0.30) saturated at only
// 4 u/s of climb — even WALKING a mild hill pinned the pullback at max, which
// collapsed the eye back inside the arched-forward torso for the whole climb
// while the 0.3u lift did nothing visible.  New defaults: full effect needs a
// real stair-grade climb (~10 u/s), over half the forward reach survives at
// max, and the lift is big enough to clear the uphill body lean.
static float s_fpStairForwardReduce   = 0.10f; // [FirstPerson] StairForwardReduce — climb-speed sensitivity: higher
                                               // reaches full pullback at a gentler climb.  0 = feature off.
static float s_fpStairForwardMinScale = 0.55f; // [FirstPerson] StairForwardMinScale — forward-offset fraction kept at
                                               // full climb (0.55 = eye pulled back to 55% of ForwardOffset; 1 = no pullback).
static float s_fpStairEyeLift         = 1.20f; // [FirstPerson] StairEyeLift — extra eye height (units) at full climb,

// Enemy body-clip clearance (v1.3.0 combat polish).  When a live hostile stands
// inside EnemyClearRadius of the anchor, the forward eye offsets (ForwardOffset +
// gait/action pushes) collapse toward EnemyClearMinScale — the same mechanism as
// the stair pullback — so the eye retreats to the (hidden) skull instead of
// poking into the aggressor's model, and the near plane pulls in (EnemyNearClip)
// so whatever still overlaps slices as thin a cross-section as possible.
// Nearest-hostile distance is sampled per frame in mainLoop (game thread, where
// getCharacterUpdateList lives) and consumed here; -1 = no hostile in range.
static float s_fpEnemyClearRadius   = 12.0f;  // [FirstPerson] EnemyClearRadius (units; 0 = off)
static float s_fpEnemyClearMinScale = 0.15f;  // [FirstPerson] EnemyClearMinScale — fwd fraction kept at contact
static float s_fpEnemyNearClip      = 0.10f;  // [FirstPerson] EnemyNearClip — near plane at full overlap (0 = off)
// Build 72 (user 2026-10-01: "knocked down I want to see my body, not through
// it").  While DOWN / ragdolled / getting up the horizon does not matter, so the
// near plane drops to DownNearClip and the chest renders as a solid wall; it
// eases back to NearClip on standing.  BodySolidInside renders the player's own
// mesh double-sided in first person so the inside of the body is opaque.
static float s_fpDownNearClip       = 0.05f;  // [FirstPerson] DownNearClip
static float s_fpDownNearSmooth     = 0.0f;   // runtime 0..1 blend toward the down near plane
static bool  s_fpBodySolidInside    = true;   // [FirstPerson] BodySolidInside
static const int FP_BODY_SUB_MAX    = 48;
static Ogre::CullingMode       s_fpBodyCullSaved[FP_BODY_SUB_MAX];
static Ogre::ManualCullingMode s_fpBodyManualSaved[FP_BODY_SUB_MAX];
static int   s_fpBodyCullCount      = 0;      // subentities whose cull modes were changed (0 = none applied)
static float s_fpEnemyClearSmooth   = 1.0f;   // runtime: smoothed 0..1 offset scale (1 = no hostile near)
static float s_fpEnemyNearestDist   = -1.0f;  // runtime: nearest live hostile distance (set in mainLoop)

// Interior floor pre-reveal — TRIED AND REVERTED (2026-08-01): re-showing the
// storeys above the anchor (Building::setFloorVisibility after the game's
// updateFloorVisibility pass) worked mechanically, but Kenshi's upper-floor
// meshes are authored for the top-down cutaway — one-sided faces with no
// underside geometry — so from eye level below they render as floating planks,
// hollow shells and sky holes (user screenshots, shinobi tower).  The vanilla
// reveal-on-approach stays; do not re-attempt without a mesh-level solution.

// Below-floor reveal (user 2026-08-01: black voids through stairwell gaps when
// looking DOWN in FP, flickering on fast pans).  In FP restrictPosition — the
// vanilla cutaway refresh — is skipped, and the substitute per-frame
// updateFloorVisibility(characters) pass reveals storeys by where SQUAD MEMBERS
// stand, not what the camera should see: solo character on floor 2 → floors 0-1
// shell geometry hidden → black voids (furniture/NPCs are separate objects, so
// they still rendered, floating in the dark).  Fix: after that pass, force the
// cutaway to "everything up to my floor" via setFloorsVisibility — one
// consistent answer per frame, which also stops the two-writer flicker.
static bool s_fpFloorRevealBelow = true;   // [FirstPerson] FloorRevealBelow (live-retunable via P)

// Sneak toggle (Shift+C while first-person, user req 2026-08-01).  Poll-thread
// edge consumed on the game thread — the OrdersPanel sneak-button path (or
// setStealthMode fallback) must run where the anchor is valid.
static volatile bool s_sneakToggleRequested = false;
                                               // to look over the steps.  0 = off.

static const ULONGLONG FP_STRAFE_GRACE_MS  = 350;   // WASD-recency window that keeps the camera authoritative
                                                    // (body refaces to view, camera never snapped) through the
                                                    // release/coast of a strafe/backpedal — kills the jolt where
                                                    // the neck-limit used to snap the view to the body direction.
static float         s_fpActionClearFwd    = 2.0f;  // [FirstPerson] ActionClearForward — extra forward eye offset
                                                    // during committed actions (attack / block / heal / revive /
                                                    // get-up) to push the camera clear of the swinging/kneeling
                                                    // body that otherwise clips through the lens. 0 = off.
static float         s_fpBodyYaw         = 0.0f;   // runtime: smoothed VISIBLE body yaw (model only —
                                                   // the eye mounts on the view yaw, so turning the
                                                   // body never moves the camera). Lerps toward view.
static const float   FP_BODY_TURN        = 0.20f;  // per-frame body-yaw lerp toward the view
static const float FP_HEAD_LEN     = 1.8f;   // neck-pivot to eye (fallback model)
static const float FP_BONE_SMOOTH  = 0.45f;  // per-frame height lerp (damps stride bob)

// -----------------------------------------------------------------------
// Native Controls-menu keybinds (v1.7.3) — TOGGLE + SPEED ONLY.
//
// HARD CONSTRAINT learned in the field (2026-06-10): Kenshi's InputHandler
// allows exactly ONE command per physical key.  Vanilla camera panning
// owns W/S/A/D as alternate binds, so registering DC movement commands on
// those keys STOLE them from the camera (camera dead in vanilla mode) and
// the theft was then persisted into controls.cfg.  The hybrid design needs
// the same key to pan the camera in vanilla mode and move the character in
// DC mode — Kenshi's native system cannot express that.  Therefore:
//   - Movement keys (W/A/S/D roles) are ALWAYS handled by our own VK
//     polling (INI-configurable, v1.6 system); playerControl_hook already
//     context-switches the camera keys during DC.  NEVER register
//     dc_move_* commands in the InputHandler.
//   - Toggle (V) and Speed (X) sit on keys vanilla leaves unbound — they
//     are registered natively (KEP pattern) and rebindable in the
//     Controls menu.  Presses arrive via key->events in processKeys.
// Evidence the game persists plugin commands when bound: toggle_devtools
// =F12 (KEP) lives in controls.cfg; our dc_ lines were missing because
// vanilla camera lines re-stole W/A/S/D at loadConfig, leaving dc_move_*
// unbound at save time.
// -----------------------------------------------------------------------
static volatile bool s_nativeCommandsRegistered = false; // set by registerNativeCommands
static void watchNativeBindChanges();                 // defined with the keybind hooks below
static void registerNativeCommands(InputHandler* self);  // ditto

struct VkName { const char* name; int vk; };
static const VkName s_vkNames[] =
{
    { "VK_SPACE",   VK_SPACE   }, { "VK_TAB",      VK_TAB      },
    { "VK_RETURN",  VK_RETURN  }, { "VK_BACK",     VK_BACK     },
    { "VK_SHIFT",   VK_SHIFT   }, { "VK_LSHIFT",   VK_LSHIFT   }, { "VK_RSHIFT",   VK_RSHIFT   },
    { "VK_CONTROL", VK_CONTROL }, { "VK_LCONTROL", VK_LCONTROL }, { "VK_RCONTROL", VK_RCONTROL },
    { "VK_MENU",    VK_MENU    }, { "VK_LMENU",    VK_LMENU    }, { "VK_RMENU",    VK_RMENU    },
    { "VK_CAPITAL", VK_CAPITAL },
    { "VK_UP",      VK_UP      }, { "VK_DOWN",     VK_DOWN     },
    { "VK_LEFT",    VK_LEFT    }, { "VK_RIGHT",    VK_RIGHT    },
    { "VK_HOME",    VK_HOME    }, { "VK_END",      VK_END      },
    { "VK_PRIOR",   VK_PRIOR   }, { "VK_NEXT",     VK_NEXT     },
    { "VK_INSERT",  VK_INSERT  }, { "VK_DELETE",   VK_DELETE   },
    { "VK_NUMPAD0", VK_NUMPAD0 }, { "VK_NUMPAD1",  VK_NUMPAD1  }, { "VK_NUMPAD2", VK_NUMPAD2 },
    { "VK_NUMPAD3", VK_NUMPAD3 }, { "VK_NUMPAD4",  VK_NUMPAD4  }, { "VK_NUMPAD5", VK_NUMPAD5 },
    { "VK_NUMPAD6", VK_NUMPAD6 }, { "VK_NUMPAD7",  VK_NUMPAD7  }, { "VK_NUMPAD8", VK_NUMPAD8 },
    { "VK_NUMPAD9", VK_NUMPAD9 },
    { "VK_MULTIPLY", VK_MULTIPLY }, { "VK_ADD",     VK_ADD     },
    { "VK_SUBTRACT", VK_SUBTRACT }, { "VK_DECIMAL", VK_DECIMAL }, { "VK_DIVIDE", VK_DIVIDE },
    { "VK_OEM_1", VK_OEM_1 }, { "VK_OEM_2", VK_OEM_2 }, { "VK_OEM_3", VK_OEM_3 },
    { "VK_OEM_4", VK_OEM_4 }, { "VK_OEM_5", VK_OEM_5 }, { "VK_OEM_6", VK_OEM_6 },
    { "VK_OEM_7", VK_OEM_7 }, { "VK_OEM_8", VK_OEM_8 }, { "VK_OEM_102", VK_OEM_102 },
    { "VK_OEM_PLUS",  VK_OEM_PLUS  }, { "VK_OEM_COMMA",  VK_OEM_COMMA  },
    { "VK_OEM_MINUS", VK_OEM_MINUS }, { "VK_OEM_PERIOD", VK_OEM_PERIOD },
};
static const int NUM_VK_NAMES = sizeof(s_vkNames) / sizeof(s_vkNames[0]);

// parseKeyName — accepts named keys (table above), VK_A..VK_Z / VK_0..VK_9,
// bare single characters, VK_F1..VK_F24 / F1..F24, hex (0x56), or decimal
// virtual-key codes.  Returns -1 when unrecognised.
static int parseKeyName(const char* raw)
{
    char s[32];
    int  n = 0;
    for (const char* p = raw; *p && n < 31; ++p)
    {
        if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') continue;
        s[n++] = (char)toupper((unsigned char)*p);
    }
    s[n] = '\0';
    if (n == 0) return -1;

    // Named-key table — both plain ("SPACE", "NUMPAD5", "OEM_3") and
    // VK_-prefixed ("VK_SPACE") forms are accepted.
    char prefixed[36];
    sprintf_s(prefixed, sizeof(prefixed), "VK_%s", s);
    for (int i = 0; i < NUM_VK_NAMES; ++i)
        if (strcmp(s, s_vkNames[i].name) == 0
            || strcmp(prefixed, s_vkNames[i].name) == 0)
            return s_vkNames[i].vk;

    const char* f = s;
    if (strncmp(f, "VK_", 3) == 0) f += 3;

    if (f[0] == 'F' && f[1] >= '0' && f[1] <= '9')
    {
        int fn = atoi(f + 1);
        if (fn >= 1 && fn <= 24) return VK_F1 + fn - 1;
    }
    if (strlen(f) == 1 &&
        ((f[0] >= 'A' && f[0] <= 'Z') || (f[0] >= '0' && f[0] <= '9')))
        return f[0];
    if (s[0] == '0' && s[1] == 'X')
    {
        long v = strtol(s, nullptr, 16);
        if (v > 0 && v < 256) return (int)v;
    }
    {
        char* end = nullptr;
        long  v   = strtol(s, &end, 10);
        if (end != s && *end == '\0' && v > 0 && v < 256) return (int)v;
    }
    return -1;
}

// parseBool — accepts true/false, 1/0, yes/no, on/off (case-insensitive).
// Returns the supplied default for anything unrecognised so a typo can't
// silently flip a feature off.
static bool parseBool(const char* raw, bool dflt)
{
    char s[16];
    int  n = 0;
    for (const char* p = raw; *p && n < 15; ++p)
    {
        if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') continue;
        s[n++] = (char)tolower((unsigned char)*p);
    }
    s[n] = '\0';
    if (!strcmp(s, "true") || !strcmp(s, "1") || !strcmp(s, "yes") || !strcmp(s, "on"))
        return true;
    if (!strcmp(s, "false") || !strcmp(s, "0") || !strcmp(s, "no") || !strcmp(s, "off"))
        return false;
    return dflt;
}

static void getConfigPath(char* out, size_t cap)
{
    out[0] = '\0';
    if (s_thisModule &&
        GetModuleFileNameA(s_thisModule, out, (DWORD)cap) > 0)
    {
        char* slash = strrchr(out, '\\');
        if (slash) { *(slash + 1) = '\0'; }
        else       { out[0] = '\0'; }
    }
    strcat_s(out, cap, "WASDCombatPlugin.ini");
}

static void writeDefaultConfig(const char* path)
{
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, nullptr,
                           CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    // Player-facing settings only (user req 2026-08-01).  Every advanced camera
    // tunable still loads with a safe default when absent, and can be added to
    // [FirstPerson] by name for fine-tuning — the loader reads far more keys
    // than this template ships.
    static const char tmpl[] =
        "[Keybinds]\r\n"
        "; Valid names: letters, digits, F1..F24, SPACE, TAB, SHIFT, CONTROL,\r\n"
        "; arrow keys, NUMPAD0..9, OEM_1..8.  Invalid entries use the default.\r\n"
        "ToggleDC        = V\r\n"
        "MoveForward     = W\r\n"
        "MoveBackward    = S\r\n"
        "MoveLeft        = A\r\n"
        "MoveRight       = D\r\n"
        "SelectControl   = G\r\n"
        "FirstPerson     = P\r\n"
        "; Sneak is the chord SHIFT + this key (first-person and third-person views).\r\n"
        "SneakToggle     = C\r\n"
        "; Swap the CTRL third-person camera to the other shoulder.\r\n"
        "SwapShoulder    = H\r\n"
        "; Manual combat: hold to guard, tap as a hit lands for a perfect block.\r\n"
        "Block           = F\r\n"
        "; Press to dodge an incoming attack while Direct Control is on.\r\n"
        "Dodge           = Q\r\n"
        "; Manual attack (toggle): your character stops attacking on its own;\r\n"
        "; every swing is the Attack key.  A \"Manual Attack\" label shows while on.\r\n"
        "Attack          = E\r\n"
        "ManualAttack    = Z\r\n"
        "; While manual combat is on, your character fights whoever you point at:\r\n"
        "; the enemy under the mouse cursor, or nearest the crosshair in first person\r\n"
        "; and the CTRL third-person view.  The camera is never moved for it.\r\n"
        "; Key layout version (leave as is): 2 = v1.5 layout, Block F / control G.\r\n"
        "KeyLayout       = 2\r\n"
        "\r\n"
        "[Settings]\r\n"
        "; true = camera faces your character when their inventory opens.\r\n"
        "; false = keep moving with WASD while looting/trading.\r\n"
        "InventoryFaceCam = true\r\n"
        "; Cap WASD speed at the character's real max speed (injuries,\r\n"
        "; encumbrance, shackles).  Mult scales it (1.0 = exactly vanilla).\r\n"
        "WasdSpeedCap     = true\r\n"
        "WasdSpeedMult    = 1.0\r\n"
        "; Enemies see your real movement, chase at full speed and can attack\r\n"
        "; you while you move (false = old behaviour: kiting never gets you hit).\r\n"
        "EnemyPursuit     = true\r\n"
        "; Manual combat: Block and Dodge keys on/off.  Hold Block = skill block;\r\n"
        "; tap it as an attack lands = perfect block (window / cooldown in ms).\r\n"
        "ManualBlock      = true\r\n"
        "; With manual combat (Z) on, your character still blocks on their own; your\r\n"
        "; Block key adds the timed hold and the perfect block. false = fully manual defence.\r\n"
        "AiBlocksInManual = true\r\n"
        "PerfectBlockWindowMs   = 1000\r\n"
        "; After a perfect block: the next press is free (no cooldown) for this long.\r\n"
        "PerfectBlockRewardMs   = 1200\r\n"
        "; Block is timed: a press blocks for up to BlockHoldMs, then a cooldown.\r\n"
        "BlockHoldMs            = 1200\r\n"
        "BlockCooldownMs        = 800\r\n"
        "; Dodge: press within this many ms of the incoming attack; skill-rolled\r\n"
        "; like vanilla with +DodgeSkillBonus dodge skill (block mode gives +20).\r\n"
        "DodgeWindowMs          = 600\r\n"
        "DodgeCooldownMs        = 300\r\n"
        "DodgeSkillBonus        = 20\r\n"
        "; Attack: the press must reach the combat AI within this many ms.\r\n"
        "AttackWindowMs         = 400\r\n"
        "; Attack out of reach: keep stepping in for up to this many ms, then swing.\r\n"
        "AttackApproachMs       = 1500\r\n"
        "; Input forgiveness: extra presses of the same combat key within this\r\n"
        "; many ms are ignored (a double-tap is one move). Letting go of Block and\r\n"
        "; pressing it again this soon resumes the same block (no cooldown).\r\n"
        "InputRepeatGuardMs     = 250\r\n"
        "; After a swing / dodge ends, new presses are ignored for this long so\r\n"
        "; the follow-through plays out instead of restarting the move.\r\n"
        "AttackRecoveryMs       = 400\r\n"
        "DodgeRecoveryMs        = 400\r\n"
        "; Directional aim: how far off the view centre (degrees) an enemy still\r\n"
        "; counts as aimed at.\r\n"
        "AimConeDeg             = 35\r\n"
        "; Aim focus with a free cursor: an enemy this many pixels from the cursor counts as pointed at.\r\n"
        "AimHoverPx             = 70\r\n"
        "; Aim focus marker: the name of the enemy you are pointing at, above their head.\r\n"
        "AimNameTag             = true\r\n"
        "; Weapon glint: an enemy's weapon glints while their swing is coming at you\r\n"
        "; (brightens over GlintLeadMs, flashes at impact). Size in px.\r\n"
        "WeaponGlint            = true\r\n"
        "GlintLeadMs            = 700\r\n"
        "GlintPx                = 22\r\n"
        "; Detailed log for bug reports (writes every combat/movement event).\r\n"
        "VerboseLog             = false\r\n"
        "; CTRL third-person camera: distance / height / shoulder offset.\r\n"
        "OtsDistance      = 20\r\n"
        "OtsHeight        = 15.0\r\n"
        "OtsSide          = 4.0\r\n"
        "; Near plane when a wall pushes the camera in close (lower keeps\r\n"
        "; your character's mesh intact at point-blank camera range).\r\n"
        "OtsNearClip      = 0.15\r\n"
        "\r\n"
        "[FirstPerson]\r\n"
        "; First-person view (press P while Direct Control is on).\r\n"
        "Sensitivity      = 1.0\r\n"
        "; Field of view in degrees (50-110).\r\n"
        "FOV              = 75\r\n"
        "; Hide your own head in first person (per-pixel - clean).\r\n"
        "HeadPixelHide    = 1\r\n"
        "; Legacy head hide (shrinks the head bone instead).  Turn this on -\r\n"
        "; and HeadPixelHide off - for custom races that render wrong with\r\n"
        "; the per-pixel hide (missing part maps, non-standard heads).\r\n"
        "HideHead         = 0\r\n"
        "; Hide your own worn hats/helmets/masks and beard.\r\n"
        "HideHeadgear     = 1\r\n"
        "; Hide your own hair (covers modded hairstyles too).\r\n"
        "HideHair         = 1\r\n"
        "; Suppress the hand-to-head / hand-to-wound injury poses while in\r\n"
        "; first person (they plant a hand across the camera).\r\n"
        "HideInjuryOverlay = 1\r\n"
        "; Render the storeys below you inside multi-floor buildings\r\n"
        "; (0 = vanilla reveal-on-approach).\r\n"
        "FloorRevealBelow = 1\r\n"
        "; Near clipping plane.  Lower shows more of your own body but makes\r\n"
        "; distant grass shimmer when you turn; 1.0 is the balance.\r\n"
        "NearClip         = 1.0\r\n"
        "; Near plane while knocked down / ragdolled (your body stays solid).\r\n"
        "DownNearClip     = 0.05\r\n"
        "; Draw your own body double-sided so it is solid from the inside.\r\n"
        "BodySolidInside  = 1\r\n";
    DWORD written = 0;
    WriteFile(h, tmpl, (DWORD)(sizeof(tmpl) - 1), &written, nullptr);
    CloseHandle(h);
}

// loadFirstPersonConfig — read + clamp the [FirstPerson] tunables from the INI.
// Split out of loadKeybinds so the P-toggle can re-read it live: edit the INI,
// toggle FP off then on, and the new camera values apply with NO game relaunch
// (called again at the top of enterFirstPerson).  Missing keys/section fall
// back to shipped defaults, so existing INIs keep working untouched.
static void loadFirstPersonConfig(const char* path)
{
    char fb[32];
    GetPrivateProfileStringA("FirstPerson", "Sensitivity", "1.0", fb, sizeof(fb), path);
    float sens = (float)atof(fb);
    if (sens < 0.1f) sens = 0.1f;  if (sens > 5.0f) sens = 5.0f;
    s_fpSensitivityFP = sens;
    GetPrivateProfileStringA("FirstPerson", "FOV", "75", fb, sizeof(fb), path);
    float fov = (float)atof(fb);
    if (fov < 50.0f) fov = 50.0f;  if (fov > 110.0f) fov = 110.0f;
    s_fpFovDegFP = fov;
    GetPrivateProfileStringA("FirstPerson", "EyeHeight", "16.5", fb, sizeof(fb), path);
    float eye = (float)atof(fb);
    if (eye < 1.0f) eye = 1.0f;    if (eye > 40.0f) eye = 40.0f;
    s_fpEyeHeight = eye;
    GetPrivateProfileStringA("FirstPerson", "ForwardOffset", "1.0", fb, sizeof(fb), path);
    float fwd = (float)atof(fb);
    if (fwd < 0.0f) fwd = 0.0f;    if (fwd > 10.0f) fwd = 10.0f;
    s_fpFwdOffset = fwd;
    GetPrivateProfileStringA("FirstPerson", "EyeUpAdjust", "0.0", fb, sizeof(fb), path);
    float eyeUp = (float)atof(fb);
    if (eyeUp < -6.0f) eyeUp = -6.0f;  if (eyeUp > 6.0f) eyeUp = 6.0f;
    s_fpEyeUpAdjust = eyeUp;
    GetPrivateProfileStringA("FirstPerson", "MoveLeanUp", "0.0", fb, sizeof(fb), path);
    float mlu = (float)atof(fb);
    if (mlu < 0.0f) mlu = 0.0f;    if (mlu > 12.0f) mlu = 12.0f;
    s_fpMoveLeanUp = mlu;
    GetPrivateProfileStringA("FirstPerson", "MoveLeanForward", "0.0", fb, sizeof(fb), path);
    float mlf = (float)atof(fb);
    if (mlf < 0.0f) mlf = 0.0f;    if (mlf > 12.0f) mlf = 12.0f;
    s_fpMoveLeanFwd = mlf;
    GetPrivateProfileStringA("FirstPerson", "MoveNearClip", "0.0", fb, sizeof(fb), path);
    float mnc = (float)atof(fb);
    if (mnc < 0.0f) mnc = 0.0f;    if (mnc > 10.0f) mnc = 10.0f;
    s_fpMoveNearClip = mnc;
    GetPrivateProfileStringA("FirstPerson", "JogForward", "2.0", fb, sizeof(fb), path);
    float jf = (float)atof(fb);
    if (jf < 0.0f) jf = 0.0f;    if (jf > 20.0f) jf = 20.0f;
    s_fpJogForward = jf;
    GetPrivateProfileStringA("FirstPerson", "RunForward", "4.0", fb, sizeof(fb), path);
    float rf = (float)atof(fb);
    if (rf < 0.0f) rf = 0.0f;    if (rf > 20.0f) rf = 20.0f;
    s_fpRunForward = rf;
    GetPrivateProfileStringA("FirstPerson", "StreamUpdateDist", "2.0", fb, sizeof(fb), path);
    float sud = (float)atof(fb);
    if (sud < 0.0f) sud = 0.0f;    if (sud > 1000.0f) sud = 1000.0f;
    s_fpStreamDist = sud;
    GetPrivateProfileStringA("FirstPerson", "GrassRangeMult", "1.0", fb, sizeof(fb), path);
    float grm = (float)atof(fb);
    if (grm < 1.0f) grm = 1.0f;    if (grm > 8.0f) grm = 8.0f;
    s_fpGrassRangeMult = grm;
    GetPrivateProfileStringA("FirstPerson", "FollowTurn", "0.08", fb, sizeof(fb), path);
    float ft = (float)atof(fb);
    if (ft < 0.0f) ft = 0.0f;    if (ft > 1.0f) ft = 1.0f;
    s_fpFollowTurn = ft;
    GetPrivateProfileStringA("FirstPerson", "FollowDelayMs", "400", fb, sizeof(fb), path);
    float fd = (float)atof(fb);
    if (fd < 0.0f) fd = 0.0f;    if (fd > 5000.0f) fd = 5000.0f;
    s_fpFollowDelayMs = fd;
    GetPrivateProfileStringA("FirstPerson", "LookSmooth", "0.4", fb, sizeof(fb), path);
    float ls = (float)atof(fb);
    if (ls < 0.0f) ls = 0.0f;    if (ls > 0.9f) ls = 0.9f;
    s_fpLookSmooth = ls;
    s_fpFoliageCenterMode = GetPrivateProfileIntA("FirstPerson", "FoliageCenterFix", 1, path);
    if (s_fpFoliageCenterMode < 0) s_fpFoliageCenterMode = 0;
    if (s_fpFoliageCenterMode > 2) s_fpFoliageCenterMode = 2;
    GetPrivateProfileStringA("FirstPerson", "ActionClearForward", "2.0", fb, sizeof(fb), path);
    float acf = (float)atof(fb);
    if (acf < 0.0f) acf = 0.0f;    if (acf > 8.0f) acf = 8.0f;
    s_fpActionClearFwd = acf;
    GetPrivateProfileStringA("FirstPerson", "SideLookForward", "1.0", fb, sizeof(fb), path);
    float slf = (float)atof(fb);
    if (slf < 0.0f) slf = 0.0f;    if (slf > 6.0f) slf = 6.0f;
    s_fpSideLookFwd = slf;
    GetPrivateProfileStringA("FirstPerson", "NeckLimit", "100", fb, sizeof(fb), path);
    float nl = (float)atof(fb);
    if (nl < 30.0f) nl = 30.0f;    if (nl > 170.0f) nl = 170.0f;
    s_fpNeckLimitRad = nl * 3.14159265f / 180.0f;
    s_uiNeckLimitDeg = nl;   // keep the options-tab slider (degrees) in step
    GetPrivateProfileStringA("FirstPerson", "NearClip", "1.0", fb, sizeof(fb), path);
    float nc = (float)atof(fb);
    if (nc < 0.05f) nc = 0.05f;    if (nc > 5.0f) nc = 5.0f;
    s_fpNearClipFP = nc;
    s_fpHideHair = GetPrivateProfileIntA("FirstPerson", "HideHair", 1, path) != 0;
    s_fpHideHead = GetPrivateProfileIntA("FirstPerson", "HideHead", 0, path) != 0;
    s_fpHeadPixelHide =
        GetPrivateProfileIntA("FirstPerson", "HeadPixelHide", 1, path) != 0;
    s_fpHideHeadgear =
        GetPrivateProfileIntA("FirstPerson", "HideHeadgear", 1, path) != 0;
    s_fpHideInjuryOverlay =
        GetPrivateProfileIntA("FirstPerson", "HideInjuryOverlay", 1, path) != 0;
    // KenshiFP-style camera port (2026-07-30).
    s_fpTrueBoneEye = GetPrivateProfileIntA("FirstPerson", "TrueBoneEye", 1, path) != 0;
    s_fpRawMouse    = GetPrivateProfileIntA("FirstPerson", "RawMouse",    1, path) != 0;
    GetPrivateProfileStringA("FirstPerson", "EyeDrop", "-0.2", fb, sizeof(fb), path);
    float ed = (float)atof(fb);
    if (ed < -4.0f) ed = -4.0f;    if (ed > 8.0f) ed = 8.0f;
    s_fpEyeDrop = ed;
    GetPrivateProfileStringA("FirstPerson", "MoveForward", "0.0", fb, sizeof(fb), path);
    float mf = (float)atof(fb);
    if (mf < 0.0f) mf = 0.0f;      if (mf > 20.0f) mf = 20.0f;
    s_fpMoveForward = mf;
    GetPrivateProfileStringA("FirstPerson", "MoveSpeedRef", "30.0", fb, sizeof(fb), path);
    float msr = (float)atof(fb);
    if (msr < 1.0f) msr = 1.0f;    if (msr > 2000.0f) msr = 2000.0f;
    s_fpMoveSpeedRef = msr;
    // Ascent-aware stair pullback tunables.
    GetPrivateProfileStringA("FirstPerson", "StairForwardReduce", "0.10", fb, sizeof(fb), path);
    float sfr = (float)atof(fb);
    if (sfr < 0.0f) sfr = 0.0f;    if (sfr > 5.0f) sfr = 5.0f;
    s_fpStairForwardReduce = sfr;
    GetPrivateProfileStringA("FirstPerson", "StairForwardMinScale", "0.55", fb, sizeof(fb), path);
    float sms = (float)atof(fb);
    if (sms < 0.0f) sms = 0.0f;    if (sms > 1.0f) sms = 1.0f;
    s_fpStairForwardMinScale = sms;
    GetPrivateProfileStringA("FirstPerson", "StairEyeLift", "1.20", fb, sizeof(fb), path);
    float sel = (float)atof(fb);
    if (sel < 0.0f) sel = 0.0f;    if (sel > 4.0f) sel = 4.0f;
    s_fpStairEyeLift = sel;
    // Enemy body-clip clearance tunables.
    GetPrivateProfileStringA("FirstPerson", "EnemyClearRadius", "12.0", fb, sizeof(fb), path);
    float ecr = (float)atof(fb);
    if (ecr < 0.0f) ecr = 0.0f;    if (ecr > 60.0f) ecr = 60.0f;
    s_fpEnemyClearRadius = ecr;
    GetPrivateProfileStringA("FirstPerson", "EnemyClearMinScale", "0.15", fb, sizeof(fb), path);
    float ecm = (float)atof(fb);
    if (ecm < 0.0f) ecm = 0.0f;    if (ecm > 0.95f) ecm = 0.95f;
    s_fpEnemyClearMinScale = ecm;
    GetPrivateProfileStringA("FirstPerson", "EnemyNearClip", "0.10", fb, sizeof(fb), path);
    float enc = (float)atof(fb);
    if (enc < 0.0f) enc = 0.0f;    if (enc > 5.0f) enc = 5.0f;
    s_fpEnemyNearClip = enc;
    GetPrivateProfileStringA("FirstPerson", "DownNearClip", "0.05", fb, sizeof(fb), path);
    float dnc = (float)atof(fb);
    if (dnc < 0.02f) dnc = 0.02f;  if (dnc > 2.0f) dnc = 2.0f;
    s_fpDownNearClip = dnc;
    s_fpBodySolidInside = GetPrivateProfileIntA("FirstPerson", "BodySolidInside", 1, path) != 0;
    // Below-floor reveal (0 = legacy character-based reveal only).
    s_fpFloorRevealBelow = GetPrivateProfileIntA("FirstPerson", "FloorRevealBelow", 1, path) != 0;
}

static void loadKeybinds()
{
    char path[MAX_PATH];
    getConfigPath(path, sizeof(path));

    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
    {
        writeDefaultConfig(path);
        char cbuf[MAX_PATH + 64];
        sprintf_s(cbuf, sizeof(cbuf),
            "[WASDCombat] dc_keybinds_default_config_created path=%s", path);
        DebugLog(cbuf);
    }

    for (int r = 0; r < KR_COUNT; ++r)
    {
        char buf[32] = "";
        GetPrivateProfileStringA("Keybinds", KR_INI_KEY[r], KR_DEFAULT[r],
                                 buf, sizeof(buf), path);
        int vk = parseKeyName(buf);
        if (vk <= 0)
        {
            char ibuf[128];
            sprintf_s(ibuf, sizeof(ibuf),
                "[WASDCombat] dc_keybind_invalid name=%s fallback=%s",
                buf, KR_DEFAULT[r]);
            DebugLog(ibuf);
            strcpy_s(buf, sizeof(buf), KR_DEFAULT[r]);
            vk = parseKeyName(buf);
        }
        s_bindVk[r] = vk;
        strcpy_s(s_bindCfgStr[r], sizeof(s_bindCfgStr[r]), buf);
        s_uiBindVk[r] = vk;   // keep the options-tab dropdown mirror in step
    }

    // v1.5 key layout (user 2026-10-08): Block moved to F (easier in a fight),
    // control-switch moved to G.  Configs written before the change still say
    // SelectControl=F (and Block=G, or no Block at all = would now default to F
    // and collide) - swap them ONCE, then stamp KeyLayout=2 so a player who
    // later binds F/G the old way on purpose is never swapped back.
    if (GetPrivateProfileIntA("Keybinds", "KeyLayout", 1, path) < 2)
    {
        bool oldPair = s_bindVk[KR_SELECT] == 'F'
                    && (s_bindVk[KR_BLOCK] == 'G' || s_bindVk[KR_BLOCK] == 'F');
        bool gFree = true;
        for (int r = 0; r < KR_COUNT; ++r)
            if (r != KR_SELECT && r != KR_BLOCK && s_bindVk[r] == 'G') gFree = false;
        if (oldPair && gFree)
        {
            s_bindVk[KR_SELECT] = 'G'; s_uiBindVk[KR_SELECT] = 'G'; strcpy_s(s_bindCfgStr[KR_SELECT], sizeof(s_bindCfgStr[KR_SELECT]), "G");
            s_bindVk[KR_BLOCK]  = 'F'; s_uiBindVk[KR_BLOCK]  = 'F'; strcpy_s(s_bindCfgStr[KR_BLOCK],  sizeof(s_bindCfgStr[KR_BLOCK]),  "F");
            WritePrivateProfileStringA("Keybinds", "SelectControl", "G", path);
            WritePrivateProfileStringA("Keybinds", "Block",         "F", path);
            DebugLog("[WASDCombat] dc_keybinds_migrated_v15 select=G block=F");
        }
        WritePrivateProfileStringA("Keybinds", "KeyLayout", "2", path);
    }

    for (int i = 0; i < KR_COUNT; ++i)
        for (int j = i + 1; j < KR_COUNT; ++j)
            if (s_bindVk[i] == s_bindVk[j])
            {
                char dbuf[128];
                sprintf_s(dbuf, sizeof(dbuf),
                    "[WASDCombat] dc_keybind_duplicate %s and %s share key 0x%02X",
                    KR_INI_KEY[i], KR_INI_KEY[j], s_bindVk[i]);
                DebugLog(dbuf);
            }

    // [Settings] feature toggles.  Missing key/section => API returns the
    // supplied default ("true"), so existing users who never had this section
    // keep the shipped default ON without touching their file.
    {
        char sbuf[16] = "";
        GetPrivateProfileStringA("Settings", "InventoryFaceCam", "true",
                                 sbuf, sizeof(sbuf), path);
        s_settingInventoryFaceCam = parseBool(sbuf, true);

        GetPrivateProfileStringA("Settings", "WasdSpeedCap", "true",
                                 sbuf, sizeof(sbuf), path);
        s_settingWasdSpeedCap = parseBool(sbuf, true);

        GetPrivateProfileStringA("Settings", "EnemyPursuit", "true",
                                 sbuf, sizeof(sbuf), path);
        s_settingEnemyPursuit = parseBool(sbuf, true);

        GetPrivateProfileStringA("Settings", "ManualBlock", "true",
                                 sbuf, sizeof(sbuf), path);
        s_settingManualBlock = parseBool(sbuf, true);
        GetPrivateProfileStringA("Settings", "AiBlocksInManual", "true", sbuf, sizeof(sbuf), path);
        s_settingAiBlocks = parseBool(sbuf, true);
        GetPrivateProfileStringA("Settings", "VerboseLog", "false", sbuf, sizeof(sbuf), path);
        s_verboseLog = parseBool(sbuf, false);
        {
            char pbuf[16] = "";
            GetPrivateProfileStringA("Settings", "PerfectBlockWindowMs", "1000", pbuf, sizeof(pbuf), path);
            float w = (float)atof(pbuf);
            if (w < 100.0f) w = 100.0f;   if (w > 3000.0f) w = 3000.0f;
            s_perfectWindowMs = w;
            GetPrivateProfileStringA("Settings", "PerfectBlockRewardMs", "1200", pbuf, sizeof(pbuf), path);
            float c = (float)atof(pbuf);
            if (c < 0.0f) c = 0.0f;       if (c > 5000.0f) c = 5000.0f;
            s_perfectRewardMs = c;
            GetPrivateProfileStringA("Settings", "BlockHoldMs", "1200", pbuf, sizeof(pbuf), path);
            float bh = (float)atof(pbuf);
            if (bh < 200.0f) bh = 200.0f;  if (bh > 5000.0f) bh = 5000.0f;
            s_blockHoldMs = bh;
            GetPrivateProfileStringA("Settings", "BlockCooldownMs", "800", pbuf, sizeof(pbuf), path);
            float bc = (float)atof(pbuf);
            if (bc < 0.0f) bc = 0.0f;      if (bc > 10000.0f) bc = 10000.0f;
            s_blockCooldownMs = bc;
            GetPrivateProfileStringA("Settings", "DodgeWindowMs", "600", pbuf, sizeof(pbuf), path);
            float dw = (float)atof(pbuf);
            if (dw < 100.0f) dw = 100.0f;  if (dw > 3000.0f) dw = 3000.0f;
            s_dodgeWindowMs = dw;
            GetPrivateProfileStringA("Settings", "DodgeCooldownMs", "300", pbuf, sizeof(pbuf), path);
            float dc = (float)atof(pbuf);
            if (dc < 0.0f) dc = 0.0f;      if (dc > 10000.0f) dc = 10000.0f;
            s_dodgeCooldownMs = dc;
            GetPrivateProfileStringA("Settings", "AimNameTag", "true", pbuf, sizeof(pbuf), path);
            s_settingAimName = parseBool(pbuf, true);
            GetPrivateProfileStringA("Settings", "WeaponGlint", "true", pbuf, sizeof(pbuf), path);
            s_settingGlint = parseBool(pbuf, true);
            GetPrivateProfileStringA("Settings", "GlintLeadMs", "700", pbuf, sizeof(pbuf), path);
            float gl = (float)atof(pbuf);
            if (gl < 50.0f) gl = 50.0f;    if (gl > 2000.0f) gl = 2000.0f;
            s_glintLeadMs = gl;
            GetPrivateProfileStringA("Settings", "GlintPx", "22", pbuf, sizeof(pbuf), path);
            float gp = (float)atof(pbuf);
            if (gp < 6.0f) gp = 6.0f;      if (gp > 96.0f) gp = 96.0f;
            s_glintPx = gp;
            GetPrivateProfileStringA("Settings", "GlintColourTiers", "false", pbuf, sizeof(pbuf), path);
            s_glintColourTiers = parseBool(pbuf, false);
            GetPrivateProfileStringA("Settings", "GlintHeavyTier", "true", pbuf, sizeof(pbuf), path);
            s_glintHeavyTier = parseBool(pbuf, true);
            GetPrivateProfileStringA("Settings", "AimConeDeg", "35", pbuf, sizeof(pbuf), path);
            float ac = (float)atof(pbuf);
            if (ac < 5.0f) ac = 5.0f;      if (ac > 90.0f) ac = 90.0f;
            s_aimConeDeg = ac;
            GetPrivateProfileStringA("Settings", "AimHoverPx", "70", pbuf, sizeof(pbuf), path);
            float ahp = (float)atof(pbuf);
            if (ahp < 10.0f) ahp = 10.0f;  if (ahp > 400.0f) ahp = 400.0f;
            s_aimHoverPx = ahp;
            GetPrivateProfileStringA("Settings", "AttackApproachMs", "1500", pbuf, sizeof(pbuf), path);
            float ap = (float)atof(pbuf);
            if (ap < 0.0f) ap = 0.0f;      if (ap > 5000.0f) ap = 5000.0f;
            s_attackApproachMs = ap;
            GetPrivateProfileStringA("Settings", "AttackWindowMs", "400", pbuf, sizeof(pbuf), path);
            float aw = (float)atof(pbuf);
            if (aw < 100.0f) aw = 100.0f;  if (aw > 2000.0f) aw = 2000.0f;
            s_attackWindowMs = aw;
            GetPrivateProfileStringA("Settings", "InputRepeatGuardMs", "250", pbuf, sizeof(pbuf), path);
            float rg = (float)atof(pbuf);
            if (rg < 0.0f) rg = 0.0f;      if (rg > 1000.0f) rg = 1000.0f;
            s_inputRepeatGuardMs = rg;
            GetPrivateProfileStringA("Settings", "AttackRecoveryMs", "400", pbuf, sizeof(pbuf), path);
            float ar = (float)atof(pbuf);
            if (ar < 0.0f) ar = 0.0f;      if (ar > 2000.0f) ar = 2000.0f;
            s_attackRecoveryMs = ar;
            GetPrivateProfileStringA("Settings", "DodgeRecoveryMs", "400", pbuf, sizeof(pbuf), path);
            float dr = (float)atof(pbuf);
            if (dr < 0.0f) dr = 0.0f;      if (dr > 2000.0f) dr = 2000.0f;
            s_dodgeRecoveryMs = dr;
            GetPrivateProfileStringA("Settings", "DodgeSkillBonus", "20", pbuf, sizeof(pbuf), path);
            float db = (float)atof(pbuf);
            if (db < 0.0f) db = 0.0f;      if (db > 100.0f) db = 100.0f;
            s_dodgeSkillBonus = db;
        }

        char mbuf[16] = "";
        GetPrivateProfileStringA("Settings", "WasdSpeedMult", "1.0",
                                 mbuf, sizeof(mbuf), path);
        float wsm = (float)atof(mbuf);
        if (wsm < 0.1f) wsm = 0.1f;   if (wsm > 5.0f) wsm = 5.0f;
        s_settingWasdSpeedMult = wsm;

        // OTS camera pose (CTRL toggle).
        GetPrivateProfileStringA("Settings", "OtsDistance", "20", mbuf, sizeof(mbuf), path);
        float od = (float)atof(mbuf);
        if (od < 5.0f) od = 5.0f;      if (od > 60.0f) od = 60.0f;
        s_otsCamDist = od;
        GetPrivateProfileStringA("Settings", "OtsHeight", "15.0", mbuf, sizeof(mbuf), path);
        float oh = (float)atof(mbuf);
        if (oh < -5.0f) oh = -5.0f;    if (oh > 20.0f) oh = 20.0f;
        s_otsCamHeight = oh;
        GetPrivateProfileStringA("Settings", "OtsSide", "4.0", mbuf, sizeof(mbuf), path);
        float os = (float)atof(mbuf);
        if (os < -8.0f) os = -8.0f;    if (os > 8.0f) os = 8.0f;
        s_otsCamSide = os;
        GetPrivateProfileStringA("Settings", "OtsNearClip", "0.15", mbuf, sizeof(mbuf), path);
        float onc = (float)atof(mbuf);
        if (onc < 0.05f) onc = 0.05f;  if (onc > 2.0f) onc = 2.0f;
        s_otsNearClipMin = onc;
    }

    // [FirstPerson] tunables — read via the shared helper (also called on every
    // P-enter for live re-tuning).  Missing keys/section fall back to shipped
    // defaults, so existing users keep the defaults without touching their INI.
    loadFirstPersonConfig(path);

    char lbuf[420];
    sprintf_s(lbuf, sizeof(lbuf),
        "[WASDCombat] dc_keybinds_loaded toggle=%s(0x%02X) forward=%s(0x%02X)"
        " back=%s(0x%02X) left=%s(0x%02X) right=%s(0x%02X) speed=%s(0x%02X)"
        " select=%s(0x%02X) inventoryFaceCam=%d",
        s_bindCfgStr[KR_TOGGLE],  s_bindVk[KR_TOGGLE],
        s_bindCfgStr[KR_FORWARD], s_bindVk[KR_FORWARD],
        s_bindCfgStr[KR_BACK],    s_bindVk[KR_BACK],
        s_bindCfgStr[KR_LEFT],    s_bindVk[KR_LEFT],
        s_bindCfgStr[KR_RIGHT],   s_bindVk[KR_RIGHT],
        s_bindCfgStr[KR_SPEED],   s_bindVk[KR_SPEED],
        s_bindCfgStr[KR_SELECT],  s_bindVk[KR_SELECT],
        s_settingInventoryFaceCam ? 1 : 0);
    DebugLog(lbuf);

    char fpbuf[240];
    sprintf_s(fpbuf, sizeof(fpbuf),
        "[WASDCombat] dc_firstperson_cfg key=%s(0x%02X) fov=%.0f sens=%.2f"
        " neckLimitDeg=%.0f hideHead=%d hideHair=%d sneak=SHIFT+%s(0x%02X)"
        " enemyClearR=%.1f",
        s_bindCfgStr[KR_FP], s_bindVk[KR_FP], s_fpFovDegFP, s_fpSensitivityFP,
        s_fpNeckLimitRad * 180.0f / 3.14159265f,
        s_fpHideHead ? 1 : 0, s_fpHideHair ? 1 : 0,
        s_bindCfgStr[KR_SNEAK], s_bindVk[KR_SNEAK],
        s_fpEnemyClearRadius);
    DebugLog(fpbuf);
}

// =======================================================================
//  NATIVE SETTINGS TAB — a "Direct Control" page inside the game's own
//  options window (v1.5).  Blueprint: KEP's ConfigManager.  The tab is a
//  MyGUI TabItem inserted into OptionsWindow::tabs; its content is a
//  DatapanelGUI built by the game's own factory (ForgottenGUI::
//  createDatapanel) with checkbox/slider rows bound DIRECTLY to the mod's
//  setting statics — moving a slider edits the live value.
//
//  Persistence: dcOptionsWatch (called every mainLoop frame; touches only
//  our statics, no GUI calls) detects changes and immediately writes the
//  INI key, so the P-enter INI reload (loadFirstPersonConfig) always
//  reads back exactly what the UI set — the two config paths can never
//  disagree.
//
//  Rule 5: NO widget pointers are stored.  The game owns every widget; if
//  the options window is ever torn down and rebuilt, OptionsWindow::create
//  runs again and optionsCreate_hook re-injects from scratch (duplicate
//  guard: tab looked up by name each time).
// =======================================================================

struct DcOptRow
{
    const char* section;  // INI section this row persists to
    const char* key;      // INI key; nullptr = section-header text row
    bool*       bp;       // checkbox binding (exclusive with fp)
    float*      fp;       // slider binding
    float       mn, mx;   // slider range (widget-enforced; loader clamps match)
    int         prec;     // slider decimals shown
    float       def;      // shipped default (the "Reset to defaults" value;
                          // bool rows use 0/1) — user's dialed-in preference
    const char* label;    // row text — must be UNIQUE (DatapanelGUI keys its
                          // content map by this string)
    const char* tip;      // tooltip, nullptr = none
};

static DcOptRow s_dcOptRows[] =
{
    { NULL, NULL, NULL, NULL, 0, 0, 0, 0, "MOVEMENT", NULL },
    { "Settings", "WasdSpeedMult", NULL, &s_settingWasdSpeedMult, 0.1f, 3.0f, 2, 1.0f,
      "Movement speed multiplier",
      "Scales how fast your character moves with the movement keys. 1.0 = the game's normal speed." },
    { "Settings", "WasdSpeedCap", &s_settingWasdSpeedCap, NULL, 0, 0, 0, 1,
      "Cap speed at the character's real max",
      "On: injuries, heavy loads and shackles slow you down exactly as they would in normal play." },
    { "Settings", "EnemyPursuit", &s_settingEnemyPursuit, NULL, 0, 0, 0, 1,
      "Enemies chase and hit a moving player",
      "On: enemies run you down and attack a moving character. Off: the old behaviour, where enemies stall against a moving player." },
    { "Settings", "InventoryFaceCam", &s_settingInventoryFaceCam, NULL, 0, 0, 0, 1,
      "Camera faces character in inventory",
      "Off: the camera stays put and you can keep moving with the movement keys while looting or trading." },

    { NULL, NULL, NULL, NULL, 0, 0, 0, 0, "MANUAL COMBAT - ATTACK", NULL },
    { "Settings", "AttackApproachMs", NULL, &s_attackApproachMs, 0.0f, 5000.0f, 0, 1500.0f,
      "Step-in time when out of reach (ms)",
      "Press Attack on an enemy that is too far away and your character walks in for up to this long, then swings. 0 = swing at the air instead." },
    { "Settings", "AttackWindowMs", NULL, &s_attackWindowMs, 100.0f, 2000.0f, 0, 400.0f,
      "Attack press kept for (ms)",
      "If your character cannot swing the instant you press (still turning, just stopped moving), the press is kept for this long." },
    { "Settings", "AttackRecoveryMs", NULL, &s_attackRecoveryMs, 0.0f, 2000.0f, 0, 400.0f,
      "Pause after a swing (ms)",
      "After a swing ends, Attack presses are ignored for this long so the swing finishes naturally instead of being cut short." },

    { NULL, NULL, NULL, NULL, 0, 0, 0, 0, "MANUAL COMBAT - BLOCK", NULL },
    { "Settings", "ManualBlock", &s_settingManualBlock, NULL, 0, 0, 0, 1,
      "Enable the Block and Dodge keys",
      "Off: Block and Dodge do nothing and your character defends on their own as in normal play." },
    { "Settings", "AiBlocksInManual", &s_settingAiBlocks, NULL, 0, 0, 0, 1,
      "Character still blocks on their own",
      "On: your character blocks automatically as usual, and your Block key adds the timed guard and the perfect block on top. Off: only your own Block and Dodge presses stop hits." },
    { "Settings", "BlockHoldMs", NULL, &s_blockHoldMs, 200.0f, 5000.0f, 0, 1200.0f,
      "Max time a held Block guards (ms)",
      "Holding Block guards for at most this long, then the guard drops even if you keep holding. Stops endless turtling." },
    { "Settings", "BlockCooldownMs", NULL, &s_blockCooldownMs, 0.0f, 10000.0f, 0, 800.0f,
      "Block cooldown (ms)",
      "Counted from the moment a guard ends. Letting go and pressing again right away keeps the same guard instead of starting this wait." },
    { "Settings", "PerfectBlockWindowMs", NULL, &s_perfectWindowMs, 100.0f, 3000.0f, 0, 1000.0f,
      "Perfect block window after tap (ms)",
      "Tap Block and any hit from the front that lands within this time is stopped no matter your skill. Follow-up strikes of the same attack are covered too." },
    { "Settings", "PerfectBlockRewardMs", NULL, &s_perfectRewardMs, 0.0f, 5000.0f, 0, 1200.0f,
      "Perfect block reward window (ms)",
      "After a perfect block your weapon glows blue: for this long the next Block press counts at once, with no cooldown, so perfect blocks can be chained." },

    { NULL, NULL, NULL, NULL, 0, 0, 0, 0, "MANUAL COMBAT - DODGE", NULL },
    { "Settings", "DodgeWindowMs", NULL, &s_dodgeWindowMs, 100.0f, 3000.0f, 0, 600.0f,
      "Dodge press kept for (ms)",
      "Press Dodge up to this long before an attack arrives and the dodge still fires when it does. Whether it succeeds is a Dodge skill roll, as in normal play." },
    { "Settings", "DodgeSkillBonus", NULL, &s_dodgeSkillBonus, 0.0f, 100.0f, 0, 20.0f,
      "Dodge skill bonus for a timed press",
      "Added to your Dodge skill for the roll when you press at the right time. 20 matches the bonus the game's own Block mode gives to defence." },
    { "Settings", "DodgeCooldownMs", NULL, &s_dodgeCooldownMs, 0.0f, 10000.0f, 0, 300.0f,
      "Dodge cooldown (ms)",
      "A new dodge is never possible until the current one finishes; this adds extra time on top." },
    { "Settings", "DodgeRecoveryMs", NULL, &s_dodgeRecoveryMs, 0.0f, 2000.0f, 0, 400.0f,
      "Pause after a dodge (ms)",
      "After a dodge ends, Dodge presses are ignored for this long so the dodge finishes naturally." },

    { NULL, NULL, NULL, NULL, 0, 0, 0, 0, "MANUAL COMBAT - TARGETING & FEEL", NULL },
    { "Settings", "AimConeDeg", NULL, &s_aimConeDeg, 5.0f, 90.0f, 0, 35.0f,
      "Crosshair target cone (degrees)",
      "In first person and the third-person camera, the enemy nearest the crosshair within this cone is the one your character fights." },
    { "Settings", "AimHoverPx", NULL, &s_aimHoverPx, 10.0f, 400.0f, 0, 70.0f,
      "Cursor target radius (pixels)",
      "In the normal camera, an enemy this close to the mouse cursor on screen becomes the one your character fights. Right-click an enemy to pick one at any distance." },
    { "Settings", "InputRepeatGuardMs", NULL, &s_inputRepeatGuardMs, 0.0f, 1000.0f, 0, 250.0f,
      "Ignore double-presses within (ms)",
      "A second press of the same combat key this soon after the first is ignored, so a nervous double-tap never restarts a move." },

    { NULL, NULL, NULL, NULL, 0, 0, 0, 0, "MANUAL COMBAT - ON-SCREEN CUES", NULL },
    { "Settings", "AimNameTag", &s_settingAimName, NULL, 0, 0, 0, 1,
      "Show target's name and health",
      "The enemy your character will fight gets their name and thin blood / head / chest / stomach bars above their head." },
    { "Settings", "WeaponGlint", &s_settingGlint, NULL, 0, 0, 0, 1,
      "Glint on weapons swinging at you",
      "An enemy's weapon lights up while their attack is coming at you, brightest just before it lands - your cue to block, dodge or counter." },
    { "Settings", "GlintLeadMs", NULL, &s_glintLeadMs, 50.0f, 2000.0f, 0, 700.0f,
      "Glint peaks after swing start (ms)",
      "The glint reaches full brightness this long after the enemy starts their swing. Most swings land around 850 ms, so 700 peaks just before the hit." },
    { "Settings", "GlintPx", NULL, &s_glintPx, 6.0f, 96.0f, 0, 22.0f,
      "Glint size (pixels)",
      NULL },
    { "Settings", "GlintColourTiers", &s_glintColourTiers, NULL, 0, 0, 0, 0,
      "Colour glints by defence (experimental)",
      "Off: every glint is white. On: white = a Block tap stops it (front arc), orange = from the side or behind, dodge it, red = heavy-weapon strike, dodge preferred." },

    { NULL, NULL, NULL, NULL, 0, 0, 0, 0, "THIRD PERSON CAMERA (CTRL)", NULL },
    { "Settings", "OtsDistance", NULL, &s_otsCamDist, 5.0f, 60.0f, 0, 20.0f,
      "Distance behind your character",
      "How far back the shoulder camera sits. The mouse wheel zooms while it is active." },
    { "Settings", "OtsHeight", NULL, &s_otsCamHeight, -5.0f, 20.0f, 1, 15.0f,
      "Camera focus height",
      "Height of the point the camera looks at. Higher looks over the shoulder; lower looks at the back." },
    { "Settings", "OtsSide", NULL, &s_otsCamSide, -8.0f, 8.0f, 1, 4.0f,
      "Shoulder offset",
      "Positive = over the right shoulder, negative = over the left. The swap-shoulder key flips it in play." },
    { "Settings", "OtsNearClip", NULL, &s_otsNearClipMin, 0.05f, 2.0f, 2, 0.15f,
      "Close-up clipping distance",
      "Used when a wall pushes the camera in close. Lower keeps your character's body intact at point-blank range; higher sees through nearby walls sooner." },

    { NULL, NULL, NULL, NULL, 0, 0, 0, 0, "FIRST PERSON", NULL },
    { "FirstPerson", "Sensitivity", NULL, &s_fpSensitivityFP, 0.1f, 5.0f, 2, 1.0f,
      "Mouse look sensitivity",
      "How fast the view turns with the mouse." },
    { "FirstPerson", "FOV", NULL, &s_fpFovDegFP, 50.0f, 110.0f, 0, 75.0f,
      "Field of view (degrees)",
      "Wider shows more of your surroundings. Applies instantly." },
    { "FirstPerson", "NeckLimit", NULL, &s_uiNeckLimitDeg, 30.0f, 170.0f, 0, 100.0f,
      "Free-look range (degrees)",
      "How far you can look to each side of your body before the body turns to follow the view." },
    { "FirstPerson", "LookSmooth", NULL, &s_fpLookSmooth, 0.0f, 0.9f, 2, 0.4f,
      "Look smoothing",
      "Softens mouse look. 0 = raw and snappy, higher = smoother but slightly delayed." },
    { "FirstPerson", "HeadPixelHide", &s_fpHeadPixelHide, NULL, 0, 0, 0, 1,
      "Hide your own head",
      "Erases your head so it never blocks the view. Leave on unless a modded race looks wrong." },
    { "FirstPerson", "HideHead", &s_fpHideHead, NULL, 0, 0, 0, 0,
      "Hide head (legacy shrink)",
      "Only for modded races that render wrong with the normal hide. Takes effect the next time you enter first person." },
    { "FirstPerson", "HideHeadgear", &s_fpHideHeadgear, NULL, 0, 0, 0, 1,
      "Hide hats and beard",
      "Headgear and beards would otherwise sit in front of the lens." },
    { "FirstPerson", "HideHair", &s_fpHideHair, NULL, 0, 0, 0, 1,
      "Hide hair", NULL },
    { "FirstPerson", "HideInjuryOverlay", &s_fpHideInjuryOverlay, NULL, 0, 0, 0, 1,
      "Hide hand-to-wound poses",
      "Stops the hand-on-head and hand-on-wound animations covering the view when hurt." },
    { "FirstPerson", "FloorRevealBelow", &s_fpFloorRevealBelow, NULL, 0, 0, 0, 1,
      "Show floors below you in buildings",
      "Inside buildings, keeps the storeys below you drawn so stairs and balconies do not open onto nothing." },

    { NULL, NULL, NULL, NULL, 0, 0, 0, 0, "FIRST PERSON - EYE POSITION (advanced)", NULL },
    { "FirstPerson", "EyeDrop", NULL, &s_fpEyeDrop, -4.0f, 8.0f, 2, -0.2f,
      "Eye drop below head bone",
      "Fine-tunes eye level. Negative raises the eye slightly." },
    { "FirstPerson", "EyeUpAdjust", NULL, &s_fpEyeUpAdjust, -6.0f, 6.0f, 2, 0.0f,
      "Extra eye raise",
      "Added on top of the drop above, for races with unusual heads." },
    { "FirstPerson", "ForwardOffset", NULL, &s_fpFwdOffset, 0.0f, 10.0f, 2, 1.0f,
      "Eye forward of the head",
      "Keeps the view out of the inside of the skull. Raise if you see your own face." },
    { "FirstPerson", "JogForward", NULL, &s_fpJogForward, 0.0f, 20.0f, 1, 2.0f,
      "Forward push while jogging",
      "The jog animation leans the body into the view; this pushes the eye ahead of it." },
    { "FirstPerson", "RunForward", NULL, &s_fpRunForward, 0.0f, 20.0f, 1, 4.0f,
      "Forward push while running",
      "Same as above for the run animation." },
    { "FirstPerson", "SideLookForward", NULL, &s_fpSideLookFwd, 0.0f, 6.0f, 1, 1.0f,
      "Forward push when looking sideways",
      "Keeps the shoulder out of view when you look to the side." },
    { "FirstPerson", "ActionClearForward", NULL, &s_fpActionClearFwd, 0.0f, 8.0f, 1, 2.0f,
      "Forward push during attacks",
      "Clears the arms and weapon from the view while swinging." },
    { "FirstPerson", "NearClip", NULL, &s_fpNearClipFP, 0.05f, 2.0f, 2, 1.0f,
      "Clipping distance",
      "Anything closer than this to the eye is not drawn. Lower shows more of your own body but makes distant grass shimmer when you turn; higher is the reverse. 1.0 is the balance." },
    { "FirstPerson", "DownNearClip", NULL, &s_fpDownNearClip, 0.02f, 2.0f, 2, 0.05f,
      "Clipping distance while down",
      "Used while knocked down, ragdolled or getting up, when the horizon does not matter: low enough that your own body stays solid in front of the lens." },
    { "FirstPerson", "BodySolidInside", &s_fpBodySolidInside, NULL, 0, 0, 0, 1,
      "Body is solid from the inside",
      "Draws your own body double-sided in first person, so a head pressed into the chest sees cloth and skin instead of a hollow shell." },

    { NULL, NULL, NULL, NULL, 0, 0, 0, 0, "TROUBLESHOOTING", NULL },
    { "Settings", "VerboseLog", &s_verboseLog, NULL, 0, 0, 0, 0,
      "Detailed log for bug reports",
      "Writes every combat and movement event to RE_Kenshi_log.txt. Leave off normally; turn on when asked for a log, reproduce the problem, then send the file." },
};
static const int DC_OPT_ROW_COUNT = sizeof(s_dcOptRows) / sizeof(s_dcOptRows[0]);

// INI-only hotkeys exposed as key-picker DROPDOWNS (int* binding, same watch
// model as the sliders).  These CANNOT use the native press-a-key rows:
// Kenshi's command system is one-command-per-key and vanilla camera panning
// owns W/S/A/D (registering them stole the camera — June hard lesson).
// Toggle + Speed Cycle are NOT here: they are real native commands and get
// genuine KeyConfig rows in the tab.
struct DcBindRow { int role; const char* label; const char* tip; };   // role < 0 = section header
static const DcBindRow s_dcBindRows[] =
{
    { -1,         "MOVEMENT KEYS",       NULL },
    { KR_FORWARD, "Move forward",        "Walk, jog or run in the direction the camera faces." },
    { KR_LEFT,    "Move left",           NULL },
    { KR_BACK,    "Move backward",       NULL },
    { KR_RIGHT,   "Move right",          NULL },
    { KR_SELECT,  "Control hovered character", "Press with the mouse over one of your characters to control them instead." },
    { -1,         "CAMERA KEYS",         NULL },
    { KR_FP,      "First person (toggle)", "Puts the camera in your character's eyes. Press again to leave. The CTRL key toggles the third-person shoulder camera." },
    { KR_OTS_SHOULDER, "Swap camera shoulder", "Moves the third-person camera to the other shoulder." },
    { KR_SNEAK,   "Sneak (SHIFT + key)", "Hold SHIFT and press this key to toggle sneaking in first person or the third-person camera." },
    { -1,         "MANUAL COMBAT KEYS",  NULL },
    { KR_MANUAL,  "Manual combat (toggle)", "On: your character only swings when you press Attack and fights whoever you point at. Off: normal AI combat." },
    { KR_ATTACK,  "Attack (press)",      "Manual combat only. One press = one swing at the enemy you are pointing at. Out of reach, your character steps in first." },
    { KR_BLOCK,   "Block (hold / tap)",  "Manual combat only. Hold to guard with your Block skill. Tap just as a hit lands for a perfect block that always stops it." },
    { KR_DODGE,   "Dodge (press)",       "Manual combat only. Press as an attack comes in to dodge it. Success depends on your Dodge skill plus a timing bonus." },
};
static const int DC_BIND_ROW_COUNT = sizeof(s_dcBindRows) / sizeof(s_dcBindRows[0]);

// Key choices offered by the dropdowns.  Values are Windows VKs (the poll
// thread reads GetAsyncKeyState).  INI spelling comes from dcVkToIniName —
// always parseKeyName-compatible by construction.
struct DcKeyChoice { const char* label; int vk; };
static const DcKeyChoice s_dcKeyChoices[] =
{
    { "Space", VK_SPACE }, { "Tab", VK_TAB }, { "Enter", VK_RETURN },
    { "Backspace", VK_BACK }, { "Caps Lock", VK_CAPITAL },
    { "Up", VK_UP }, { "Down", VK_DOWN }, { "Left", VK_LEFT }, { "Right", VK_RIGHT },
    { "Home", VK_HOME }, { "End", VK_END }, { "Page Up", VK_PRIOR }, { "Page Down", VK_NEXT },
    { "Insert", VK_INSERT }, { "Delete", VK_DELETE },
    { "Numpad 0", VK_NUMPAD0 }, { "Numpad 1", VK_NUMPAD1 }, { "Numpad 2", VK_NUMPAD2 },
    { "Numpad 3", VK_NUMPAD3 }, { "Numpad 4", VK_NUMPAD4 }, { "Numpad 5", VK_NUMPAD5 },
    { "Numpad 6", VK_NUMPAD6 }, { "Numpad 7", VK_NUMPAD7 }, { "Numpad 8", VK_NUMPAD8 },
    { "Numpad 9", VK_NUMPAD9 },
    { "; :", VK_OEM_1 }, { "/ ?", VK_OEM_2 }, { "` ~", VK_OEM_3 },
    { "[ {", VK_OEM_4 }, { "\\ |", VK_OEM_5 }, { "] }", VK_OEM_6 }, { "' \"", VK_OEM_7 },
    { ", <", VK_OEM_COMMA }, { ". >", VK_OEM_PERIOD },
    { "- _", VK_OEM_MINUS }, { "= +", VK_OEM_PLUS },
};
static const int DC_KEY_CHOICE_COUNT = sizeof(s_dcKeyChoices) / sizeof(s_dcKeyChoices[0]);

// VK -> the spelling our INI parser (parseKeyName) reads back: bare char for
// A-Z/0-9, F1..F24, VK_ name from s_vkNames, decimal as the safety net.
static void dcVkToIniName(int vk, char* out, size_t cap)
{
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9'))
    {
        out[0] = (char)vk; out[1] = '\0';
        return;
    }
    if (vk >= VK_F1 && vk <= VK_F24)
    {
        sprintf_s(out, cap, "F%d", vk - VK_F1 + 1);
        return;
    }
    for (int i = 0; i < NUM_VK_NAMES; i++)
        if (s_vkNames[i].vk == vk)
        {
            strcpy_s(out, cap, s_vkNames[i].name);
            return;
        }
    sprintf_s(out, cap, "%d", vk);
}

static bool  s_dcOptSeeded = false;
static float s_dcOptLast[DC_OPT_ROW_COUNT];

// "Reset to defaults" button plumbing (user req 2026-09-04): the injected
// panel + its category, remembered so the button callback can refresh the
// visible widgets after resetting the bound statics.  Re-stored on every
// injection (window rebuild) — the callback only fires while the window is
// open, so the pointer is always current (Rule 5).
static DatapanelGUI* s_dcOptPanel = nullptr;
static int           s_dcOptCat   = 0;

// Reset every bound setting to its shipped default (DcOptRow::def = the user's
// dialed-in preference), then refresh the widgets.  dcOptionsWatch picks up
// the changed statics next frame → persists each to the INI + live-applies
// FOV/NearClip, exactly as if the player had moved every control by hand.
static void dcOnDefaultsPressed(MyGUI::Widget* /*sender*/)
{
    for (int i = 0; i < DC_OPT_ROW_COUNT; i++)
    {
        const DcOptRow& r = s_dcOptRows[i];
        if (!r.key) continue;
        if (r.bp)      *r.bp = (r.def != 0.0f);
        else if (r.fp) *r.fp = r.def;
    }
    // NeckLimit slider edits degrees; keep the radian value it drives in step
    // immediately (dcOptionsWatch also does this, but not until next frame).
    s_fpNeckLimitRad = s_uiNeckLimitDeg * 3.14159265f / 180.0f;

    if (s_dcOptPanel)
        for (int i = 0; i < DC_OPT_ROW_COUNT; i++)
        {
            const DcOptRow& r = s_dcOptRows[i];
            if (!r.key) continue;
            DataPanelLine* ln = s_dcOptPanel->getLine(r.label, s_dcOptCat);
            if (ln) ln->refresh();   // pull the reset value into the widget
        }
    DebugLog("[WASDCombat] dc_opt_reset_to_defaults");
}

// Change-watch: runs every mainLoop frame.  Reads only our statics — never
// touches a widget (the rows write the statics through their bound
// pointers on the UI thread; we only observe the values).
static void dcOptionsWatch()
{
    if (!s_dcOptSeeded)
    {
        // First call is after plugin config load: snapshot, write nothing.
        for (int i = 0; i < DC_OPT_ROW_COUNT; i++)
        {
            const DcOptRow& r = s_dcOptRows[i];
            s_dcOptLast[i] = r.bp ? (*r.bp ? 1.0f : 0.0f)
                           : r.fp ? *r.fp : 0.0f;
        }
        s_dcOptSeeded = true;
        return;
    }

    for (int i = 0; i < DC_OPT_ROW_COUNT; i++)
    {
        const DcOptRow& r = s_dcOptRows[i];
        if (!r.key) continue;
        float cur = r.bp ? (*r.bp ? 1.0f : 0.0f) : *r.fp;
        if (cur == s_dcOptLast[i]) continue;
        s_dcOptLast[i] = cur;

        // Unit conversion: the NeckLimit slider edits degrees.
        if (r.fp == &s_uiNeckLimitDeg)
            s_fpNeckLimitRad = cur * 3.14159265f / 180.0f;

        // Live re-apply: FOV/NearClip are only pushed to the camera at FP
        // enter — re-push so slider drags show instantly in first person.
        if ((r.fp == &s_fpFovDegFP || r.fp == &s_fpNearClipFP)
            && s_firstPersonActive && ou && ou->player
            && ou->player->camera && ou->player->camera->camera)
        {
            Ogre::Camera* ocOpt = ou->player->camera->camera;
            if (r.fp == &s_fpFovDegFP)
                ocOpt->setFOVy(Ogre::Radian(Ogre::Degree(s_fpFovDegFP)));
            else
                ocOpt->setNearClipDistance(s_fpNearClipFP);
        }

        // Persist immediately.  [Settings] is parsed with parseBool
        // (true/false); [FirstPerson] with GetPrivateProfileIntA/atof
        // (numeric).  Floats always write 3 decimals so the value
        // round-trips through the P-enter INI reload bit-stable.
        char path[MAX_PATH];
        getConfigPath(path, sizeof(path));
        char val[32];
        if (r.bp)
        {
            bool settingsSection = (strcmp(r.section, "Settings") == 0);
            strcpy_s(val, sizeof(val),
                     *r.bp ? (settingsSection ? "true" : "1")
                           : (settingsSection ? "false" : "0"));
        }
        else
        {
            sprintf_s(val, sizeof(val), "%.3f", cur);
        }
        WritePrivateProfileStringA(r.section, r.key, val, path);

        char obuf[160];
        sprintf_s(obuf, sizeof(obuf), "[WASDCombat] dc_opt_changed %s.%s=%s",
                  r.section, r.key, val);
        VerbLog(obuf);
    }

    // Keybind dropdowns: commit mirror -> live bind + INI.  s_bindVk is read
    // by the 1 kHz poll thread; an aligned int store is safe to publish.
    for (int i = 0; i < DC_BIND_ROW_COUNT; i++)
    {
        int role = s_dcBindRows[i].role;
        if (role < 0) continue;                 // build 75: section header rows (build 68) have no key - indexing [-1] clobbered s_otsCamSide
        int vk   = s_uiBindVk[role];
        if (vk == s_bindVk[role] || vk <= 0)
            continue;
        s_bindVk[role] = vk;

        char name[32];
        dcVkToIniName(vk, name, sizeof(name));
        strcpy_s(s_bindCfgStr[role], sizeof(s_bindCfgStr[role]), name);

        char path[MAX_PATH];
        getConfigPath(path, sizeof(path));
        WritePrivateProfileStringA("Keybinds", KR_INI_KEY[role], name, path);

        char bbuf[160];
        sprintf_s(bbuf, sizeof(bbuf),
                  "[WASDCombat] dc_opt_bind_changed %s=%s (0x%02X)",
                  KR_INI_KEY[role], name, vk);
        DebugLog(bbuf);

        // Same-key overlap is tolerated (matches the INI loader's behaviour)
        // but worth a log line so a "my key stopped working" report is
        // self-diagnosing.
        for (int j = 0; j < KR_COUNT; j++)
            if (j != role && s_bindVk[j] == vk)
            {
                char wbuf[128];
                sprintf_s(wbuf, sizeof(wbuf),
                          "[WASDCombat] dc_opt_bind_duplicate %s and %s share 0x%02X",
                          KR_INI_KEY[role], KR_INI_KEY[j], vk);
                DebugLog(wbuf);
            }
    }
}

// Build the tab.  Called from optionsCreate_hook (UI thread, right after
// the game builds the options window).  Known vanilla tab categories:
// General 0x1, Gameplay 0x22, Graphics 0x17, Audio 0x18, Controls 0x19,
// Mods 0x0 — we take 0x31 and collision-check against whatever exists.
static void dcInjectSettingsTab(OptionsWindow* self)
{
    if (!gui || !self->tabs)
        return;
    if (self->tabs->findItemWith(MyGUI::UString("Direct Control")) != NULL)
        return;   // already present (re-entry guard)

    size_t tabCount = self->tabs->getItemCount();
    int maxCat = 0;
    for (size_t i = 0; i < tabCount; i++)
    {
        DatapanelGUI** p = self->tabs->getItemDataAt<DatapanelGUI*>(i, false);
        // Sanity clamp: with RE_Kenshi/Mod-Hub/KEP tabs present, one tab's
        // item data type-checks as DatapanelGUI* but reads ASCII garbage as
        // its category (field build A landed on cat=0x454D44).  Real
        // categories are tiny (vanilla max 0x22, KEP 0x30) — ignore junk.
        if (p && *p != NULL
            && (*p)->currentCategory > maxCat && (*p)->currentCategory < 0x1000)
            maxCat = (*p)->currentCategory;
    }
    int cat = 0x31;
    if (cat <= maxCat) cat = maxCat + 1;

    // Index 5 = right after Controls, before Mods (KEP's slot choice).
    size_t index = (tabCount < 5) ? tabCount : 5;
    MyGUI::TabItem* tab =
        self->tabs->insertItemAt(index, MyGUI::UString("Direct Control"));
    if (!tab)
    {
        DebugLog("[WASDCombat] dc_opt_tab_insert_failed");
        return;
    }
    DatapanelGUI* panel = gui->createDatapanel("dc_options", tab, true);
    if (!panel)
    {
        DebugLog("[WASDCombat] dc_opt_panel_create_failed");
        return;
    }
    panel->changeCategory(cat);
    panel->setLineSpacing(25.0f);

    int built = 0;

    // ---- KEYBINDS (first section — no leading addSpace, it renders as a
    // large empty gap; Controls-rows field finding 2026-06) ----
    panel->setLineText("DIRECT CONTROL KEYS", "DIRECT CONTROL KEYS", cat, false,
                       MyGUI::Align(MyGUI::Align::Left));
    // Native press-a-key rows (moved here from the Controls tab per user req
    // 2026-09-02).  The game's capture flow (clickButton -> OptionsWindow::
    // setKey) is panel-agnostic; rebinds persist via the existing saveOptions
    // hook.  dc_toggle is the mod's own command; cycle_run_speed is VANILLA's
    // — editing it here rebinds the exact command the vanilla Controls tab
    // uses, so the two rows stay in sync for free (SPEEDSYNC-TWEAK).
    panel->addCustomLine(new DataPanelLine_KeyConfig(
        "dc_toggle",        "Direct Control on / off", cat));
    panel->addCustomLine(new DataPanelLine_KeyConfig(
        "cycle_run_speed",  "Cycle walk / jog / run", cat));
    built += 2;
    // INI-polled hotkeys: key-picker dropdowns (see s_dcBindRows).
    for (int i = 0; i < DC_BIND_ROW_COUNT; i++)
    {
        const DcBindRow& b = s_dcBindRows[i];
        if (b.role < 0)
        {
            panel->addSpace(cat, 0.35f);
            panel->setLineText(b.label, b.label, cat, false, MyGUI::Align(MyGUI::Align::Left));
            built++;
            continue;
        }
        DataPanelLine_DropBox* db = panel->setLineDropBox(
            b.label, cat, &s_uiBindVk[b.role], false, 0.7f);
        if (!db) continue;
        char one[2] = { 0, 0 };
        for (char c = 'A'; c <= 'Z'; c++)
        {
            one[0] = c;
            db->addAValue(std::string(one), (int)c);
        }
        for (char d = '0'; d <= '9'; d++)
        {
            one[0] = d;
            db->addAValue(std::string(one), (int)d);
        }
        for (int f = 0; f < 12; f++)
        {
            char fbuf[8];
            sprintf_s(fbuf, sizeof(fbuf), "F%d", f + 1);
            db->addAValue(std::string(fbuf), VK_F1 + f);
        }
        for (int k = 0; k < DC_KEY_CHOICE_COUNT; k++)
            db->addAValue(std::string(s_dcKeyChoices[k].label),
                          s_dcKeyChoices[k].vk);
        db->refresh();   // select the currently-bound key
        if (b.tip)
            db->setToolTip(std::string(b.tip), self->tooltip);
        built++;
    }

    bool firstHeader = false;   // the key groups were emitted above — every
                                // settings header gets its addSpace
    for (int i = 0; i < DC_OPT_ROW_COUNT; i++)
    {
        const DcOptRow& r = s_dcOptRows[i];
        if (!r.key)
        {
            // Section header.  No addSpace before the FIRST one — a leading
            // space rendered as a large empty gap (Controls-rows field
            // finding, 2026-06).
            if (!firstHeader)
                panel->addSpace(cat, 0.35f);
            firstHeader = false;
            panel->setLineText(r.label, r.label, cat, false,
                               MyGUI::Align(MyGUI::Align::Left));
            continue;
        }
        DataPanelLine* line = NULL;
        if (r.bp)
        {
            line = panel->setLineCheckbox(r.label, r.bp, cat);
        }
        else if (r.fp)
        {
            DataPanelLine_SliderEditable* sl = panel->setLineSliderEditable(
                r.label, cat, true, r.mn, r.mx, r.fp);
            if (sl) sl->setPrecision(r.prec);
            line = sl;
        }
        if (line)
        {
            if (r.tip)
                line->setToolTip(std::string(r.tip), self->tooltip);
            built++;
        }
    }

    // "Reset to defaults" button (user req 2026-09-04) — one click restores
    // every setting on this tab to the shipped defaults, for players who
    // over-tweak the camera.  Remember the panel/cat so the callback can
    // refresh the widgets.
    s_dcOptPanel = panel;
    s_dcOptCat   = cat;
    panel->addSpace(cat, 0.35f);
    DataPanelLine_Button* resetBtn =
        panel->setLineButton("Reset all settings to defaults", "Reset", cat);
    if (resetBtn && resetBtn->button)
    {
        resetBtn->button->eventMouseButtonClick += MyGUI::newDelegate(dcOnDefaultsPressed);
        built++;
    }

    // The game's tab-switch logic owns page visibility; a fresh page starts
    // hidden.  Registering the panel as the tab's item data is what lets
    // the switch logic find it (same contract as the vanilla tabs).
    tab->setVisible(false);
    self->tabs->setItemData(tab, panel);

    char tbuf[128];
    sprintf_s(tbuf, sizeof(tbuf),
              "[WASDCombat] dc_opt_tab_injected idx=%u cat=0x%X rows=%d",
              (unsigned)index, (unsigned)cat, built);
    DebugLog(tbuf);
}

// -----------------------------------------------------------------------
// Press handlers — shared by the poll thread (fallback path) and the
// native processKeys event path.  Loot-suspend gating lives here so both
// paths behave identically.
// -----------------------------------------------------------------------
static void handleTogglePress()
{
    if (s_lootUiSuspendActive)
        return;
    if (s_mode == MODE_FREE_MOVE || s_userWantsDC)
    {
        s_userWantsDC = false;
        s_userWantsFP = false;   // FP requires DC; leaving DC clears the FP intent too
        setMode(MODE_VANILLA);
        DebugLog("[WASDCombat] dc_user_intent_off_manual");
    }
    else
    {
        s_userWantsDC = true;
        setMode(MODE_FREE_MOVE);
        DebugLog("[WASDCombat] dc_user_intent_on");
    }
}

static void handleSpeedPress()
{
    if (s_lootUiSuspendActive)
        return;
    if (s_mode == MODE_FREE_MOVE)
        s_xPressed = true;
}

static void handleSelectPress()
{
    if (s_lootUiSuspendActive)
        return;
    // Only meaningful in DC; the main loop consumes the edge and switches the
    // WASD anchor to the selected/highlighted character.
    if (s_mode == MODE_FREE_MOVE)
        s_fSelectEdge = true;
}

static void handleFirstPersonPress()
{
    if (s_lootUiSuspendActive)
        return;
    // Only meaningful in DC; the main loop consumes the edge on the game thread
    // and enters/exits first-person (camera calls must run there, not here).
    if (s_mode == MODE_FREE_MOVE)
        s_fpToggleRequested = true;
}

static void handleSneakPress()
{
    if (s_lootUiSuspendActive)
        return;
    // Shift+C CHORD, first-person only (user req 2026-08-01): the sneak key alone
    // does nothing, so a bare C press can never collide with vanilla or other
    // mod uses of the key.  Final gating (live anchor, FP still active) happens
    // on the game thread where the edge is consumed.
    if (s_mode != MODE_FREE_MOVE || !(s_firstPersonActive || s_otsCamActive))   // FP or the CTRL third-person view (user req 2026-09-18)
        return;
    if (!(GetAsyncKeyState(VK_SHIFT) & 0x8000))
        return;
    s_sneakToggleRequested = true;
}

// SwapShoulder (OTS): flip the over-the-shoulder camera to the other side.
// Just flips the sign applied to OtsSide; takes effect next OTS frame.
static void handleOtsShoulderPress()
{
    s_otsShoulderSign = -s_otsShoulderSign;
    DebugLog(s_otsShoulderSign > 0.0f ? "[WASDCombat] dc_ots_shoulder_right"
                                      : "[WASDCombat] dc_ots_shoulder_left");
}

static void onPress(int role)
{
    if (role == KR_TOGGLE)       handleTogglePress();
    else if (role == KR_SPEED)   handleSpeedPress();
    else if (role == KR_SELECT)  handleSelectPress();
    else if (role == KR_FP)      handleFirstPersonPress();
    else if (role == KR_SNEAK)   handleSneakPress();
    else if (role == KR_OTS_SHOULDER) handleOtsShoulderPress();
}

// -----------------------------------------------------------------------
// Polling thread
// -----------------------------------------------------------------------
struct PollKey { int role; bool prev; };
static PollKey s_keys[] =
{
    { KR_FORWARD, false }, { KR_LEFT,   false },
    { KR_BACK,    false }, { KR_RIGHT,  false },
    { KR_TOGGLE,  false }, { KR_SPEED,  false },
    { KR_SELECT,  false }, { KR_FP,     false },
    { KR_SNEAK,   false }, { KR_OTS_SHOULDER, false },
    { KR_BLOCK,   false }, { KR_DODGE,  false }, { KR_ATTACK, false },
    { KR_MANUAL,  false },
};
static const int NUM_KEYS = 14;

static bool isKenshiForeground()
{
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

static DWORD WINAPI PollThread(LPVOID)
{
    while (true)
    {
        Sleep(50);
        if (!isKenshiForeground()) continue;
        // Movement roles are ALWAYS VK-polled (INI-configurable) — the
        // native keybind system is one-command-per-key and vanilla camera
        // owns W/S/A/D, so DC movement cannot live there.  Toggle/speed
        // polling stands down once their native commands are registered
        // (presses then arrive via the game's processKeys events).
        for (int i = 0; i < NUM_KEYS; ++i)
        {
            int role = s_keys[i].role;
            if (s_nativeCommandsRegistered
                && (role == KR_TOGGLE || role == KR_SPEED))
                continue;
            bool down = (GetAsyncKeyState(s_bindVk[role]) & 0x8000) != 0;
            if (down == s_keys[i].prev) continue;
            s_keys[i].prev = down;
            bool wasWasd = s_wHeld || s_aHeld || s_sHeld || s_dHeld;
            switch (role) {
                case KR_FORWARD: s_wHeld = down; break;
                case KR_LEFT:    s_aHeld = down; break;
                case KR_BACK:    s_sHeld = down; break;
                case KR_RIGHT:   s_dHeld = down; break;
                case KR_BLOCK:                                 // manual block (hold + timed press)
                    s_blockHeld = down;
                    if (down) s_blockPressMs = GetTickCount64();
                    break;
                case KR_DODGE:                                 // manual dodge (press)
                    s_dodgeHeld = down;
                    if (down) s_dodgePressMs = GetTickCount64();
                    break;
                case KR_ATTACK:                                // manual attack (press)
                    s_attackHeld = down;
                    if (down) s_attackPressMs = GetTickCount64();
                    break;
                case KR_MANUAL: if (down) s_manualPressMs = GetTickCount64(); break;  // manual-attack toggle edge
            }
            if (!wasWasd && (s_wHeld || s_aHeld || s_sHeld || s_dHeld))
                s_wasdTapStartMs = GetTickCount64();
            if (down) onPress(role);
        }

        // RMB press edge — earliest player-click signal (see global note).
        {
            bool rmbDown = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
            if (rmbDown && !s_rmbPrev)
            {
                s_rmbPressedEdge = true;
                s_rmbAimPressMs  = GetTickCount64();   // right-click focus (manual combat)
            }
            s_rmbPrev = rmbDown;
        }

        // LMB double-click edge — two left-press edges within the OS double-click
        // time gate the DC control-switch (see s_lmbDoubleClickMs).  The 50ms poll
        // reliably separates the two down-edges of a normal double-click (~200-
        // 400ms apart); a single click sets only s_lastLmbDownMs and never the
        // double-click timestamp.
        {
            bool lmbDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
            if (lmbDown && !s_lmbPrev)
            {
                ULONGLONG nowLB = GetTickCount64();
                if (s_lastLmbDownMs > 0
                    && (nowLB - s_lastLmbDownMs) <= (ULONGLONG)GetDoubleClickTime())
                    s_lmbDoubleClickMs = nowLB;   // double-click
                else
                    s_lmbDoubleClickMs = 0;       // fresh single — clear any stale double
                s_lastLmbDownMs = nowLB;
            }
            s_lmbPrev = lmbDown;
        }

        // CTRL press edge — toggles camera-rotate mode while DC is active (the
        // apply lives in cameraUpdate_hook).  Only flips in DC so the toggle
        // state never desyncs from any out-of-DC CTRL use.
        {
            bool ctrlDown = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            // Do NOT toggle while a UI is open — CTRL is used for menu actions
            // (CTRL+click to move stacks in trade/inventory); flipping the crosshair
            // toggle there corrupts it (field 2026-06-21).
            if (ctrlDown && !s_ctrlPrevPoll && s_mode == MODE_FREE_MOVE && !s_camRotateUiOpen)
                s_camRotateToggle = !s_camRotateToggle;
            s_ctrlPrevPoll = ctrlDown;
        }
    }
}

// -----------------------------------------------------------------------
// HUD — disabled for reload stability
// -----------------------------------------------------------------------
struct HudWidget { MyGUI::TextBox* label; bool shown; const char* tag;
    HudWidget() : label(nullptr), shown(false), tag("") {} };
static HudWidget s_vHud;
static bool      s_hudReady = false;
static void hudUpdate() {}

// -----------------------------------------------------------------------
// World state — main thread only
// -----------------------------------------------------------------------
static Character*    s_selectedCharacter = nullptr;
static CharMovement* s_selectedMovement  = nullptr;
static Character*    s_freeMoveAnchor    = nullptr;
static bool          s_wasdWasActive     = false;
static bool          s_combatWASDLogged  = false;
static bool          s_retreatLogged     = false;

static bool          s_squadThreat         = false;
static bool          s_consciousAllyThreat = false;

// Cached movement pointer — used by charMovUpdate_hook without dereferencing
// s_freeMoveAnchor (which may be freed during a LOADGAME transition).
static CharMovement* s_anchorMovement = nullptr;

static volatile bool s_loadGuardActive      = false;
static int           s_stabilizationCountdown = 0;
static bool          s_postLoadReacquire   = false;

// DC pointer-loss state — entered when any load signal fires while s_userWantsDC is true.
// Injection pauses; mode stays FREE_MOVE; reacquire loop runs until pointers are valid.
// Times out to hard shutdown only if pointers stay invalid beyond the squad-loss threshold.
static bool           s_dcPtrLossActive      = false;
static ULONGLONG      s_reacquireMs          = 0;      // build 65: tick of the last post-load reacquire (stale-anchor re-poll window)
static bool           s_streamLogged         = false;  // build 67: one log per chunk stream

// Build 67: a REAL load (save / import / new game) vs a chunk STREAM.  Both raise
// isLoadingFromASaveGame (log 2026-09-30: every chunk crossing did, 0.4-8 s, and
// the character object survived all of them - same pointer before and after -
// while true loads took ~16 s and rebuilt it).  A real load sets the SaveManager
// signal before teardown (the same mechanism the NEWGAME detection uses) and/or
// drops the player object; a stream does neither.  During a stream the mod keeps
// control: WASD keeps driving and the OTS / FP camera keeps following, in all
// three views.  The caller additionally checks the anchor is still in the live
// update list (dcInUpdateList) - if it ever vanishes mid-stream, the real-load
// path takes over exactly as before.
static bool dcInUpdateList(GameWorld* gw, Character* c);   // fwd decl (defined with mainLoop_hook)
static bool      s_realLoadLatched = false;   // build 74: SaveManager signal seen (LOAD/NEW/IMPORT) - held until the load flag drops
static ULONGLONG s_loadFlagLogMs   = 0;
static bool      s_loadFlagPrev    = false;   // build 75: load flag last frame (falling-edge latch release)
static ULONGLONG s_latchSetMs      = 0;
static bool dcRealLoad()
{
    if (!ou) return true;
    if (!ou->isLoadingFromASaveGame()) return false;
    if (!ou->player) return true;
    if (s_realLoadLatched) return true;
    SaveManager* sm = SaveManager::getSingleton();
    int sig = sm ? sm->signal : 0;
    if (sig == SaveManager::LOADGAME || sig == SaveManager::NEWGAME || sig == SaveManager::IMPORTGAME) return true;
    return false;
}
static ULONGLONG      s_dcPtrLossStartedAt   = 0;  // when pointer loss began (for duration logging)
static ULONGLONG      s_dcPtrLossLastLogTick = 0;  // throttle for periodic duration log
static MoveSpeed      s_dcPreservedSpeedMode  = WALK;
static bool          s_enemyTargetingLogged = false;
static ULONGLONG     s_lastScanTick        = 0;

static ControlMode   s_fmTrackedMode = MODE_VANILLA;

// CombatClass state tracking
static bool           s_wasPrevInCombat  = false;
static ULONGLONG      s_wasdReleasedTick = 0;

// Combat engagement tracking
static Character*     s_prevAttackTarget  = nullptr;
static bool           s_prevTargetInRange = false;

// Protected animation state tracking (knockdown / get-up / stagger)
static bool           s_wasProtectedState = false;

// Instant-stop: set when WASD movement is applied, cleared on release.
static bool           s_wasdMovementApplied = false;

// Attack commitment — protects CHOP_WEAPON from early interruption

// Healing job active — set/cleared by addJob/removeJob hooks.
// True while any medical job is running on the anchor; DC suspends
// movement injection so the animation is not interrupted.
static bool           s_healingJobActive = false;
// Pending: a medical job arrived while WASD was held.
// Promoted to s_healingJobActive in pre-AI once WASD is released.
static bool           s_healingJobPending = false;

// WASD retreat state — tracks active retreat for suppression diagnostics
static bool          s_wasdRetreatActive      = false;
static bool          s_retreatCleanLogged     = false;
static ULONGLONG     s_retreatActiveStartTick  = 0;
static ULONGLONG     s_lastCombatEnterExitTick = 0;
static bool          s_combatFlickerLogged    = false;

// Post-WASD grace period — suppress combat re-entry after WASD release
static const ULONGLONG POST_WASD_GRACE_MS          = 2000;
static const float     POST_WASD_REENGAGEMENT_RANGE = 200.0f;
static bool            s_postWasdGraceActive        = false;
static ULONGLONG       s_postWasdGraceStart          = 0;
static bool            s_combatReentryAllowed        = false;

// CombatClass::_NV_go suppression — set when go() was skipped this frame
static bool            s_retreatLockGoSuppressed     = false;
// Sticky: once go() is suppressed during a WASD hold, stays true until release
static bool            s_retreatLockEverActive       = false;

// DC OWNERSHIP-HANDOFF COMBAT MODEL (user req 2026-06-21): the controlled
// character's combat AI (CombatClass::_NV_go) runs FULLY AUTONOMOUSLY whenever
// WASD is NOT held, and is SUSPENDED the moment WASD is held (movement owns the
// character) — see combatGo_hook.  No per-frame tug-of-war = no stutter/slide.

// Downed/crippled movement tracking — set each frame applyDownedMovement fires
static bool            s_wasdDownedMovementActive    = false;

// SLAVE OBEDIENCE (v1.4.1) — order-driven WASD while the anchor is an obeying
// slave (see isObeyingSlave / applySlaveMovement).
static Ogre::Vector3   s_slaveLastDir                = Ogre::Vector3::ZERO;
static Ogre::Vector3   s_slaveLastDest               = Ogre::Vector3::ZERO;
static ULONGLONG       s_slaveLastIssueMs            = 0;
static bool            s_slaveOrderActive            = false;   // a WASD move order is live
static bool            s_slaveModeLogged             = false;   // edge log per WASD press-run
static ULONGLONG       s_lastModOrderMs              = 0;       // DIAG: last playerMoveOrderDefault WE issued
static int             s_lastSlaveState              = -1;      // DIAG: isSlave() edge tracking

// Play-dead exit — once per WASD press; reset when all keys released
static bool            s_playDeadExitDone            = false;

// Combat WASD grace ("absolute movement priority", user req 2026-06-20): while
// in combat, the AI must not react/re-orient until the player has fully released
// WASD for ~half a second.  Rolling between keys (W->A->S->D) or a brief pause
// otherwise instant-stops, letting the combat AI grab the frame to square up to
// the attacker = the "movement pauses / character distracted in combat" reports.
// For this long after the last real key, keep driving the LAST direction so the
// AI never gets a re-orient frame.  Combat-only so out-of-combat stops stay crisp.
// Used by BOTH the charMovUpdate locomotion bridge AND the combatGo_hook ownership
// handoff: for this long after the last real key, movement still owns the character
// (AI stays suspended) so a key-roll doesn't hand control back mid-roll.  250ms
// covers key-rolls while letting the AI resume combat promptly after a real stop
// (in the ownership-handoff model a long grace would delay autonomous combat).
// 140 -> 500 (absolute-priority era) -> 250 (ownership-handoff model).  Tunable.
static const ULONGLONG COMBAT_WASD_BRIDGE_MS         = 250;

// Medical job suppression — set once when any medical job is blocked during a WASD hold,
// cleared on WASD release.  Prevents repeated per-frame log spam.
static bool            s_medicalJobSuppressedThisHold = false;

// Multi-enemy tracking
static int             s_retreatBlockedAttackerCount  = 0;
static int             s_retreatTargetsProcessed      = 0;
static int             s_retreatTargetsCachedSkipped  = 0;
static int             s_lastKnownEnemyCount          = 0;

// Performance: throttle step-7 job removal to once per 250 ms.
static const ULONGLONG JOB_REMOVAL_INTERVAL_MS = 250;
static ULONGLONG       s_jobRemovalLastTick     = 0;

// movement_injection_allowed throttle — emit at most once per second.
static ULONGLONG       s_movInjLogTick         = 0;
// Athletics XP bridge throttle — ticks at 1 s intervals; 0 = timer not yet armed.
static ULONGLONG       s_athleticsXpLastTick   = 0;

// AI MOTION FEED runtime state (v1.3.2) — see the doc block above
// charMovUpdate_hook.  Declared here so clearAllState can reset them (flags only —
// clearAllState must never touch CharMovement, its pointers may be stale).
static bool          s_aiFeedWrote   = false;  // fields hold fed values (restore due)
static ULONGLONG     s_aiFeedLogTick = 0;
// Mode publish (2026-08-22 disassembly finding): CombatMovementController::
// chasingModeCheck (real 0x2AE0A0, called per-frame from combatMovementUpdate)
// only engages chase-and-strike vs a target whose CharMovement movementMode
// (+0x378) == MOVE_NORMAL, officiallyStopped (+0x8) == 0 and currentSpeed > 9.0
// sustained 1.5 s (timer at controller+0x4C resets on any failed gate).  The DC
// anchor is MOVE_DIRECTION by construction → the gate fails EVERY frame → enemies
// ring-hold and never attack a moving anchor.  Publish MOVE_NORMAL (+ not-stopped)
// post-update alongside the velocity feed; restore pre-update.
static bool          s_aiFeedModePublished = false;
static bool          s_aiFeedSavedStopped  = false;
// v2 (2026-08-22 late): CharMovement::update runs on engine SUBSTEPS — multiple
// calls with odd dt per render frame (the 3x-speed outliers, and why the v1
// per-call publish got wiped: a substep restored engine state and its re-publish
// was skipped by the dt/discontinuity gates → the AI mostly saw MOVE_DIRECTION).
// v2 computes velocity ONCE per render frame in mainLoop (frame-to-frame pos
// delta — substep-immune) and publishes unconditionally on every anchor update
// call AND once at end-of-mainLoop (the frame's last word).
static float         s_aiPubSpd = 0.0f;
static Ogre::Vector3 s_aiPubVel(0.0f, 0.0f, 0.0f);
static Ogre::Vector3 s_aiPubLastPos(0.0f, 0.0f, 0.0f);
static bool          s_aiPubValid = false;
// True only when the inject path ACTUALLY drove the anchor this frame — the
// end-of-mainLoop publish keys on this, NOT on s_wasdMovementApplied: that flag
// legitimately stays set through a committed-action release (stagger/parry skips
// the instant stop), and publishing "moving, MOVE_NORMAL, not stopped" onto a
// released character kept it WALKING (field 2026-08-23: sticky WASD after
// release while being beaten).
static bool          s_aiPubDroveThisFrame = false;
// Locomotion feel: direction tracking for turn detection and grace window.
static Ogre::Vector3   s_prevWasdDir           = Ogre::Vector3::ZERO;
static ULONGLONG       s_wasdLastHeldMs        = 0;
// Camera lock: saved freecam state from before DC was activated; restored on DC exit.
static bool            s_savedFreeCameraMode       = false;
// Camera-lock suspension states — DC stays active but tracking is paused.
static bool            s_cameraLockInvSuspend      = false;  // suspended during inventory UI
static bool            s_cameraLockTurretSuspend   = false;  // suspended during turret/mounted use
static bool            s_menuSuspendActive         = false;  // suspended while ou->isPaused() (escape/options/save/load menus)

// -----------------------------------------------------------------------
// Hybrid post-WASD hold (v1.5) — Direct Control is a hybrid play style:
// vanilla point-click movement and orders work normally in V-mode; WASD
// overrides them while pressed; after WASD release the character HOLDS
// where WASD left them until the player gives a new point-click, presses
// WASD again, or toggles V off.  The hold is "WASD parked the character
// here", never "DC owns all locomotion".
//   s_wasdHoldActive: set once at the WASD release edge; cleared by a real
//     player click (playerMove dispatcher, RVA 0x7F95F0), the next WASD
//     press, V transitions, anchor switch, and clearAllState.
//   s_holdPos/s_holdPosValid: X/Z position clamp anchor — motion zeroing
//     alone is too early for indoor routing systems that write position
//     later in the frame, so while holding, the position is restored both
//     post-orig in charMovUpdate and at the end of mainLoop.  Y stays free
//     for gravity/ramp settling.  Invalidated whenever the hold is not
//     enforcing (stale anchor would teleport-snap).
//   Door suppression: door-type addJob/addOrder on the anchor are swallowed
//     ONLY while the hold is active — the window where no player intent
//     exists and stale indoor door tasks used to fire (auto-open bug).
//     Vanilla door behavior everywhere else.  MOVE_CUS_ORDERED is never
//     suppressed (DC's own disengage orders use it).
//   DO NOT hook CharBody::setCurrentAction (KenshiLib error 8 → crash).
// -----------------------------------------------------------------------
static bool           s_wasdHoldActive          = false;
// Active player point-click/order — set in the real click dispatcher
// (playerMove_hook), cleared at the WASD press edge (WASD wins), the WASD
// release edge (the release anchor-snap cancels the order anyway), V
// transitions, anchor switch, and clearAllState.  The hold may only
// enforce when this is false: a live player click always outranks the
// hold, regardless of which was set first.
static bool           s_playerPointClickActive  = false;
static Ogre::Vector3  s_holdPos                 = Ogre::Vector3::ZERO;
static bool           s_holdPosValid            = false;
static bool           s_idleHoldEngaged         = false;  // log edge tracking
static ULONGLONG      s_authGateLogTick         = 0;      // gate log heartbeat
static bool           s_authGateLastAllow       = true;   // gate log change detect
static char           s_authGateLastReason[24]  = "";
static ULONGLONG      s_doorSuppressLogTick     = 0;      // door-suppress log throttle
static ULONGLONG      s_addOrderDiagLogTick     = 0;      // addorder-during-hold diag throttle
// DC camera focus offset: saved objectCurrentlyFollowingOffset.y from DC entry; restored on DC exit.
static float           s_savedCamFollowOffY        = 0.0f;

// Performance: session cache — each attacker blocked exactly once per WASD hold.
// No TTL: valid for the entire retreat lock; cleared on WASD release.
// 256 entries covers large guard squads without repeated miss-scans.
static const int  RETREAT_CACHE_SIZE       = 256;
static Character* s_retreatSessionCache[RETREAT_CACHE_SIZE];
static int        s_retreatSessionCacheCount = 0;

// -----------------------------------------------------------------------
// clearAllState
// -----------------------------------------------------------------------
static void otsRestoreNames();   // fwd decl — defined with the OTS camera code
static void clearAllState()
{
    s_mode              = MODE_VANILLA;
    s_wHeld             = false;
    s_aHeld             = false;
    s_sHeld             = false;
    s_dHeld             = false;
    s_camRotateToggle   = false;
    s_lmbDoubleClickMs  = 0;
    s_fSelectEdge       = false;
    s_selectedCharacter = nullptr;
    s_selectedMovement  = nullptr;
    s_freeMoveAnchor    = nullptr;
    s_anchorMovement    = nullptr;
    s_wasdWasActive     = false;
    s_combatWASDLogged  = false;
    s_retreatLogged     = false;
    s_squadThreat       = false;
    s_consciousAllyThreat = false;
    s_enemyTargetingLogged = false;
    s_lastScanTick      = 0;
    s_fmTrackedMode     = MODE_VANILLA;
    s_xPressed          = false;
    s_syncSpeedToAnchor = false;   // SPEEDSYNC-TWEAK
    s_stabilizationCountdown = 0;
    s_wasPrevInCombat   = false;
    s_wasdReleasedTick  = 0;
    s_prevAttackTarget  = nullptr;
    s_prevTargetInRange = false;
    s_wasProtectedState = false;
    s_wasdMovementApplied    = false;
    s_healingJobActive       = false;
    s_wasdRetreatActive      = false;
    s_retreatCleanLogged     = false;
    s_retreatActiveStartTick  = 0;
    s_lastCombatEnterExitTick = 0;
    s_combatFlickerLogged    = false;
    s_postWasdGraceActive      = false;
    s_postWasdGraceStart       = 0;
    s_combatReentryAllowed     = false;
    s_retreatLockGoSuppressed     = false;
    s_retreatLockEverActive       = false;
    s_blockHeld                   = false;   // manual block: drop everything on teardown
    s_blockHeldPrev               = false;   // (never dereference s_blockOrderChar here -
    s_blockOrderApplied           = false;   //  the world may be gone; the order is save
    s_blockOrderChar              = nullptr; //  state the player can also clear by button)
    s_perfectCooldownUntilMs      = 0;
    s_perfectRewardUntilMs        = 0;
    s_perfectUsedPressMs          = 0;
    s_perfectLastHitMs            = 0;
    s_blockInjectedPressMs        = 0;
    s_blockAcceptedPressMs        = 0;
    s_blockSeenPressMs            = 0;
    s_blockCooldownUntilMs        = 0;
    s_blockHoldStartMs            = 0;
    s_blockReleaseMs              = 0;
    s_dodgeHeld                   = false;
    s_dodgeCooldownUntilMs        = 0;
    s_dodgeRecoveryUntilMs        = 0;
    s_dodgeAcceptedPressMs        = 0;
    s_staggerStartMs              = 0;
    s_staggerRawSinceMs           = 0;
    s_dodgeInjectedPressMs        = 0;
    s_dodgeActiveTech             = nullptr;
    s_dodgeRollOk                 = false;
    s_dodgeCommitUntilMs          = 0;
    s_dodgeSeenPressMs            = 0;
    s_attackHeld                  = false;
    s_attackCommitUntilMs         = 0;
    s_attackSeenPressMs           = 0;
    s_attackInjectedPressMs       = 0;
    s_attackAcceptedPressMs       = 0;
    s_attackActive                = false;
    s_attackSawChop               = false;
    s_attackRecoveryUntilMs       = 0;
    s_attackRetryPressMs          = 0;
    s_attackAfterDodgeMs          = 0;
    s_attackRetries               = 0;
    s_manualAttackOn              = false;   // runtime state: re-toggle after a load
    s_manualSeenPressMs           = 0;
    s_lockTarget                  = nullptr;
    s_aimLogged                   = nullptr;
    s_aimClicked                  = false;
    s_rmbAimSeenMs                = 0;
    for (int gi = 0; gi < GLINT_MAX; ++gi) s_glint[gi].who = nullptr;   // widgets stay; hidden by the update
    s_wasdDownedMovementActive    = false;
    s_slaveOrderActive            = false;
    s_slaveModeLogged             = false;
    s_playDeadExitDone            = false;
    // Inventory face-cam: load/teardown — the scene (and our detached node) is
    // gone, so make NO camera calls here; just drop the runtime flags/pointers.
    s_fpActive                    = false;
    s_fpNode                      = nullptr;
    s_fpCursorCaptured            = false;
    s_fpCamLocalsSaved            = false;
    s_fpHadAutoTrack              = false;
    // First-person: scene (and our detached node/skeleton) is gone — drop the
    // runtime flags/pointers only, make NO camera/skeleton/GUI calls here.
    // Restoring the grass/foliage draw-range IS safe (just writes the options
    // globals) and must happen or a teardown that bypassed exitFirstPerson would
    // leave the range permanently boosted.
    if (s_fpOptRangeSaved && options)
    {
        options->grassRange   = s_fpSavedGrassRange;
        options->foliageRange = s_fpSavedFoliageRange;
        s_fpOptRangeSaved     = false;
    }
    s_firstPersonActive           = false;
    s_fpToggleRequested           = false;
    s_sneakToggleRequested        = false;
    s_fpEnemyClearSmooth          = 1.0f;
    s_fpEnemyNearestDist          = -1.0f;
    s_fpSuspendedForInv           = false;
    s_fpHeadBoneHidden            = false;
    for (int hbI = 0; hbI < 2; ++hbI)        // flags only — skeleton may be gone
        s_fpHideBoneSaved[hbI]    = false;
    s_fpHairHidden                = false;
    s_fpPixelFallbackOn           = false;    // re-probe the shader when FP resumes
    s_fpHiddenBeardCount          = 0;        // names only — entities may be gone
    s_fpHeadSmoothValid           = false;
    otsRestoreNames();            // re-show name-tags if a teardown left them hidden
    s_otsRestorePending           = false;
    s_invFaceCloseStreak          = INV_FACE_CLOSE_DEBOUNCE;
    s_otsInvFaceActive            = false;
    s_otsInvFaceChar              = nullptr;
    s_otsSavedYaw                 = 0.0f;
    s_otsSavedPitch               = 0.0f;
    s_otsSavedDist                = 14.0f;
    s_medicalJobSuppressedThisHold = false;
    s_retreatBlockedAttackerCount  = 0;
    s_retreatTargetsProcessed      = 0;
    s_retreatTargetsCachedSkipped  = 0;
    s_lastKnownEnemyCount          = 0;
    s_jobRemovalLastTick           = 0;
    s_retreatSessionCacheCount     = 0;
    s_lootUiSuspendActive          = false;
    s_lootUiWasPrevOpen            = false;
    s_tradeWindowActive            = false;
    s_lootSuspendStartTick         = 0;
    s_invMoveThroughActive         = false;
    s_invMoveThroughForcedRun      = false;
    s_invMoveThroughPlayerPaused   = false;
    s_invPausedBeforeOpen          = false;
    s_invMoveThroughShownChar      = nullptr;
    s_invMoveThroughEdgeTick       = 0;
    s_invTradeCloseRequested       = false;
    s_invTradeStartValid           = false;
    s_movInjLogTick                = 0;
    s_athleticsXpLastTick          = 0;
    s_aiFeedWrote                  = false;   // flags only — never touch CharMovement here
    s_aiFeedLogTick                = 0;
    s_aiFeedModePublished          = false;
    s_aiFeedSavedStopped           = false;
    s_aiPubValid                   = false;
    s_aiPubSpd                     = 0.0f;
    s_aiPubVel                     = Ogre::Vector3::ZERO;
    s_aiPubLastPos                 = Ogre::Vector3::ZERO;
    s_aiPubDroveThisFrame          = false;
    s_prevWasdDir                  = Ogre::Vector3::ZERO;
    s_wasdLastHeldMs               = 0;
    s_savedFreeCameraMode          = false;
    s_prof_mainLoop                = 0;
    s_prof_charMove                = 0;
    s_prof_playerControl           = 0;
    s_prof_committedAct            = 0;
    s_prof_threatScan              = 0;
    s_prof_cameraLock              = 0;
    s_prof_wasdInject              = 0;
    s_prof_combatTarget            = 0;
    s_nearbyEnemyCount             = 0;
    s_chaseFlapsCount              = 0;
    s_pathfindingEnemyCount        = 0;
    s_cameraLockInvSuspend         = false;
    s_cameraLockTurretSuspend      = false;
    s_menuSuspendActive            = false;
    s_wasdHoldActive          = false;
    s_playerPointClickActive  = false;
    s_holdPos                 = Ogre::Vector3::ZERO;
    s_holdPosValid            = false;
    s_idleHoldEngaged         = false;
    s_authGateLogTick         = 0;
    s_authGateLastAllow       = true;
    s_authGateLastReason[0]   = '\0';
    s_doorSuppressLogTick     = 0;
    s_addOrderDiagLogTick     = 0;
    s_savedCamFollowOffY           = 0.0f;
    s_wasdTapStartMs               = 0;
    s_userWantsDC           = false;
    s_userWantsFP           = false;   // hard teardown clears FP intent (survivable loads preserve it)
    s_dcPtrLossActive       = false;
    s_dcPtrLossStartedAt    = 0;
    s_dcPtrLossLastLogTick  = 0;
    s_hookBlockLoggedMain    = false;
    s_hookBlockLoggedCharMov = false;
    s_hookBlockLoggedPCtrl   = false;
    s_hookBlockLoggedRemJob  = false;
    s_hookBlockLoggedAddJob  = false;
    s_shutdownWaitLogTick    = 0;
    s_healingJobPending      = false;
}

// -----------------------------------------------------------------------
// computeWASDDirection — camera-relative direction helper.
// -----------------------------------------------------------------------
static Ogre::Vector3 s_lastCamFwd = Ogre::Vector3::ZERO;   // build 63: last known camera basis (chunk loads)
static bool computeWASDDirection(bool bW, bool bA, bool bS, bool bD, Ogre::Vector3& outDir)
{
    if (!ou) return false;
    bool haveCam = ou->player && ou->player->camera;
    Ogre::Vector3 camFwd;
    if (s_firstPersonActive)
    {
        // First-person: the game camera controller is detached and stale — the
        // real view heading lives in s_fpYaw (drives mouse-look + the rendered
        // camera orientation).  Basis must match it so W follows the gaze.
        camFwd = Ogre::Vector3(-sinf(s_fpYaw), 0.0f, -cosf(s_fpYaw));
    }
    else if (s_otsCamActive)
    {
        // Detached OTS: same story — the vanilla camera's facing is stale, our
        // view heading is s_otsYaw.  Match it so WASD is camera-relative to the
        // OTS view exactly like the normal DC camera.
        camFwd = Ogre::Vector3(-sinf(s_otsYaw), 0.0f, -cosf(s_otsYaw));
    }
    else if (haveCam)
    {
        camFwd = ou->player->camera->getFacingDirection();
        s_lastCamFwd = camFwd;
    }
    else
    {
        camFwd = s_lastCamFwd;                                  // player object away (chunk load): keep the last basis
    }
    camFwd.y = 0.0f;
    float cflen = camFwd.length();
    if (cflen < 0.001f) return false;
    camFwd /= cflen;
    Ogre::Vector3 camRight(-camFwd.z, 0.0f, camFwd.x);
    Ogre::Vector3 move = Ogre::Vector3::ZERO;
    if (bW) move += camFwd;
    if (bS) move -= camFwd;
    if (bD) move += camRight;
    if (bA) move -= camRight;
    float mlen = move.length();
    if (mlen < 0.001f) return false;
    outDir = g_loco.normalizeDiagonalMovement ? (move / mlen) : move;
    return true;
}

// -----------------------------------------------------------------------
// wasdMoveLimit — the move-limit passed to setDirectMovement for WASD.
//
// Legacy behaviour forced ~99 (uncapped), which let shackled/injured/encumbered
// characters run at full speed and outrun enemies.  With the cap on (default), the
// limit is the character's REAL max run speed (CharStats::getMaxRunSpeed, which the
// game computes from stats/injuries/encumbrance) scaled by WasdSpeedMult, with an
// extra hard clamp while chained/shackled so the Rebirth shackle escape is a shuffle.
// The turn-responsiveness boost still applies briefly on direction changes for snappy
// turns.  Cap off (WasdSpeedCap=false) → the old uncapped limit.
static float wasdMoveLimit(bool turning)
{
    const float turnBoost = turning ? g_loco.wasdTurnResponsiveness : 1.0f;
    if (!s_settingWasdSpeedCap || !s_freeMoveAnchor)
        return 99.0f * g_loco.wasdAccelerationMultiplier * turnBoost;

    float legit = 99.0f;
    CharStats* st = s_freeMoveAnchor->getStats();
    if (st)
    {
        float m = st->getMaxRunSpeed();      // injury / encumbrance aware
        if (m > 0.1f) legit = m;
    }
    // Shackles hard-limit real movement via a separate mechanism the direct-move
    // bypasses; when chained, clamp to a slow shuffle so the player can't sprint off.
    if (s_freeMoveAnchor->isChainedMode())
    {
        float shuffle = legit * 0.35f;
        if (shuffle > 6.0f) shuffle = 6.0f;   // absolute shuffle ceiling
        legit = shuffle;
    }
    // Sneaking (Shift+C in first-person, or the vanilla sneak button): vanilla
    // movement is capped at the stealth-skill speed; mirror it so WASD-sneak is
    // exactly as fast as the game's own sneak movement, no faster.
    if (st && s_freeMoveAnchor->isStealthMode())
    {
        float sneakMax = st->calculateMaxStealthSpeed();
        if (sneakMax > 0.1f && sneakMax < legit) legit = sneakMax;
    }
    legit *= s_settingWasdSpeedMult;

    // Per-second diagnostic so the real values can be read from the RE_Kenshi log.
    if (g_log.debugVerbose)
    {
        static ULONGLONG s_spdLogTick = 0;
        ULONGLONG t = GetTickCount64();
        if (t - s_spdLogTick >= 1000)
        {
            s_spdLogTick = t;
            char b[160];
            sprintf_s(b, sizeof(b),
                "[WASDCombat] wasd_speed maxRun=%.1f chained=%d mult=%.2f limit=%.1f",
                st ? st->getMaxRunSpeed() : -1.0f,
                s_freeMoveAnchor->isChainedMode() ? 1 : 0,
                s_settingWasdSpeedMult, legit * turnBoost);
            DebugLog(b);
        }
    }
    return legit * turnBoost;
}

// applyPlayerMovement — halt() + setDirectMovement in camera-relative WASD.
// -----------------------------------------------------------------------
static bool applyPlayerMovement(bool bW, bool bA, bool bS, bool bD)
{
    CharMovement* mv = s_freeMoveAnchor ? s_freeMoveAnchor->movement : nullptr;
    if (!s_freeMoveAnchor || !mv) return false;
    if (!ou || !ou->player || !ou->player->camera) return false;

    Ogre::Vector3 move;
    if (!computeWASDDirection(bW, bA, bS, bD, move)) return false;

    // Turn responsiveness: boost move limit on significant direction change.
    bool prevHasDir = (s_prevWasdDir.squaredLength() > 0.0001f);
    bool turning    = prevHasDir && (move.dotProduct(s_prevWasdDir) < 0.9f);
    float limit     = wasdMoveLimit(turning);

    mv->halt();
    mv->setDesiredSpeed(mv->speedOrders);
    mv->setDirectMovement(move, limit);
    s_prevWasdDir = move;
    return true;
}

// -----------------------------------------------------------------------
// isProtectedAnimationState — returns true when V-Mode must not interfere.
// -----------------------------------------------------------------------
static bool isProtectedAnimationState(Character* ch)
{
    if (!ch) return false;
    ProneState prone = ch->getProneState();
    if (prone == PS_KO || prone == PS_PLAYING_DEAD) return true;
    if (ch->isDown())              return true;
    if (ch->isCurrentlyGettingUp) return true;
    CombatClass* cc = ch->getCombatClass();
    if (cc && cc->getCombatState() == STUMBLE) return true;
    return false;
}

// -----------------------------------------------------------------------
// isCommittedAction — returns true when the character is executing a vanilla
// action that DC must not interrupt.
//
// Narrowed scope (current pass): only used at instant_stop and
// combat_state_restored_after_wasd_release.  Not used at movement injection
// sites until movement logs confirm it is no longer over-broad.
//
// Committed action state set:
//   STARTUP_STATE    attack windup
//   CHOP_WEAPON      active swing
//   DECISION         attack recovery
//   BLOCK            active block
//   REACTION_BLOCK   parry
//   HESITATE         hesitation between attack cycles
//   STUMBLE          stagger (also caught by isProtectedAnimationState)
//   isDown / PS_KO / PS_PLAYING_DEAD / isCurrentlyGettingUp
//                    knockdown, unconscious, playing dead, get-up
//   s_healingJobActive  medical action in progress
// -----------------------------------------------------------------------
static bool isCommittedAction(Character* ch)
{
    ScopeTimer _tCA(s_prof_committedAct);
    if (!ch) return false;

#define _LOG_COMMITTED(reason) \
    if (g_log.debugVerbose) DebugLog("[WASDCombat] committed_action_true reason=" reason)

    ProneState prone = ch->getProneState();
    if (prone == PS_KO)
        { _LOG_COMMITTED("PS_KO");              return true; }
    if (prone == PS_PLAYING_DEAD)
        { _LOG_COMMITTED("PS_PLAYING_DEAD");    return true; }
    if (ch->isDown())
        { _LOG_COMMITTED("isDown");             return true; }
    if (ch->isCurrentlyGettingUp)
        { _LOG_COMMITTED("isCurrentlyGettingUp"); return true; }
    if (s_healingJobActive)
        { _LOG_COMMITTED("HEALING_JOB");        return true; }

    CombatClass* cc = ch->getCombatClass();
    // Gate the combat-STATE checks on combatModeActive (v1.8.4 lesson): a combat
    // state left STALE after a fight (e.g. recovering from a knockdown — the state
    // machine can sit in DECISION/STUMBLE with combatModeActive already false) must
    // NOT count as a committed action, or it blocks the release-stop and WASD
    // movement is delayed after you get up (field 2026-06-21).  The physical states
    // above (KO/down/getting-up/healing) stay ungated — they are real regardless.
    if (cc && cc->combatModeActive)
    {
        swordStateEnum st = cc->getCombatState();
        if (st == STUMBLE)
            { _LOG_COMMITTED("STUMBLE");        return true; }
        if (st == STARTUP_STATE)
            { _LOG_COMMITTED("STARTUP_STATE");  return true; }
        if (st == CHOP_WEAPON)
            { _LOG_COMMITTED("CHOP_WEAPON");    return true; }
        if (st == DECISION)
            { _LOG_COMMITTED("DECISION");       return true; }
        if (st == BLOCK)
            { _LOG_COMMITTED("BLOCK");          return true; }
        if (st == REACTION_BLOCK)
            { _LOG_COMMITTED("REACTION_BLOCK"); return true; }
        if (st == HESITATE)
            { _LOG_COMMITTED("HESITATE");       return true; }
    }

#undef _LOG_COMMITTED
    // Gated (v1.4.1): this fired UNCONDITIONALLY every frame in idle DC mode —
    // ~70 lines/sec of RE_Kenshi_log.txt bloat in every player session.
    if (g_log.debugLogging && g_log.verboseCommittedActionLogs)
        DebugLog("[WASDCombat] committed_action_false");
    return false;
}

// -----------------------------------------------------------------------
// isCommittedCombatClip — the character is mid-play in a committed one-shot combat
// CLIP that must finish before WASD movement takes over: their own attack swing
// (windup STARTUP_STATE / strike CHOP_WEAPON), a stagger from being hit (STUMBLE), or
// a parry (REACTION_BLOCK).  Kenshi exposes no way to abort an animation clip, so
// cutting one with movement looks broken / stutters (field 2026-06-22: stutter when
// retreating + after being hit and stumbled).  While this is true: the buffer in
// charMovUpdate HOLDS movement (no inject, no state touched) AND combatGo_hook lets
// go() RUN so the clip advances and finishes — exactly one system drives the body, no
// fighting.  The instant the clip ends, movement resumes (plain walk).  DECISION /
// BLOCK / CIRCLE / WAIT / HESITATE are NOT included — they persist or re-trigger
// attacks, so the player must be able to move/retreat through them.  Gated on
// combatModeActive (stale post-combat states must not count — v1.8.4 lesson).
// -----------------------------------------------------------------------
static bool manualDodgeCommitLive();   // fwd decl (manual combat, below)
static bool manualAttackCommitLive();  // fwd decl (manual combat, below)
static void manualAimTick();           // fwd decl (manual combat, below)
static bool isCommittedCombatClip(Character* ch)
{
    if (!ch) return false;
    if (ch == s_freeMoveAnchor && (manualDodgeCommitLive() || manualAttackCommitLive())) return true;   // manual action in flight
    CombatClass* cc = ch->getCombatClass();
    if (!cc || !cc->combatModeActive) return false;
    swordStateEnum st = cc->getCombatState();
    return st == STARTUP_STATE || st == CHOP_WEAPON
        || st == STUMBLE       || st == REACTION_BLOCK;
}

// -----------------------------------------------------------------------
// isAnchoredToFurniture — the character is physically using a UseableStuff object
// (chair / throne / bed / crafting+research machine).  The low-level CharBody action
// for using one is OPERATE_MACHINERY (key 87) when you put them there directly, OR
// PRETEND_TO_OPERATE_MACHINERY (key 221) when they idled onto it via a toggled JOB
// (field diag 2026-06-22 — BOTH must be detected, else a job-sat character rotates in
// place after a squad-switch).  In this state the body is locked to the furniture
// node, so injecting setDirectMovement only ROTATES the model; when detected with WASD
// held we issue a real move order to detach them (see charMovUpdate).  Calls are
// header-declared (KenshiLib-linked) + null-checked.  SIT_AROUND/SIT_ON_THRONE/
// USE_BED*/REST are higher-level AI goals that never surface as the current action;
// kept as harmless belt-and-suspenders.
// -----------------------------------------------------------------------
static bool isSeatedTaskType(TaskType t)
{
    return t == OPERATE_MACHINERY
        || t == PRETEND_TO_OPERATE_MACHINERY   // job-driven idle-at-station (jobs toggled ON)
        || t == SIT_AROUND || t == SIT_ON_THRONE
        || t == USE_BED    || t == USE_BED_ORDER
        || t == REST;
}

static bool isAnchoredToFurniture(Character* ch)
{
    if (!ch) return false;
    if (ch->inSomething == IN_BED) return true;        // sleeping / lying in a bed
    CharBody* body = ch->getBody();
    if (body)
    {
        Tasker* action = body->getCurrentAction();
        if (action && isSeatedTaskType(action->key()))
            return true;
    }
    return false;
}

// -----------------------------------------------------------------------
// isUsingStationaryTurret — true when character is manning a turret/crossbow.
// When WASD is not held, V-Mode must not suppress aiming input.
// When WASD is held, turret use is cancelled (player takes movement authority).
// -----------------------------------------------------------------------
static bool isUsingStationaryTurret(Character* ch)
{
    if (!ch) return false;
    // isUsingTurret is a hand (reference to the turret building); truthy when valid.
    return (bool)(ch->isUsingTurret);
}

// -----------------------------------------------------------------------
// Retreat session cache — each enemy processed once per WASD hold, then
// immediately skipped on every subsequent call with zero overhead.
// Cleared on WASD release via s_retreatSessionCacheCount = 0.
// -----------------------------------------------------------------------
static bool retreatSessionCacheContains(Character* ch)
{
    for (int i = 0; i < s_retreatSessionCacheCount; ++i)
        if (s_retreatSessionCache[i] == ch) return true;
    return false;
}

static void retreatSessionCacheAdd(Character* ch)
{
    if (!ch) return;
    for (int i = 0; i < s_retreatSessionCacheCount; ++i)
        if (s_retreatSessionCache[i] == ch) return;
    if (s_retreatSessionCacheCount < RETREAT_CACHE_SIZE)
        s_retreatSessionCache[s_retreatSessionCacheCount++] = ch;
    // If full: silently drop — best-effort optimization
}

// -----------------------------------------------------------------------
// isDownedButMovable — true when character is downed/crippled/playing-dead but
// vanilla point-click movement still works (crawl/limp).  Hard blocks (truly
// unconscious, getting-up animation, stumble) return false.
// -----------------------------------------------------------------------
static bool isDownedButMovable(Character* ch)
{
    if (!ch) return false;
    ProneState prone = ch->getProneState();
    // PS_KO is the authoritative hard block — truly knocked out, cannot crawl.
    // Do NOT use isUnconcious() here: it returns true for PS_PLAYING_DEAD and
    // crippled characters in Kenshi even though point-click crawl still works.
    if (prone == PS_KO) return false;
    if (ch->isCurrentlyGettingUp) return false;
    // Playing-dead and crippled can crawl/limp via the point-click path.
    if (prone == PS_PLAYING_DEAD || prone == PS_CRIPPLED) return true;
    // Down but not KO and not getting up — conscious downed state.
    if (ch->isDown() && !ch->isUnconcious()) return true;
    return false;
}

// stableIndoors — isInsideBuildingLoadedInterior with 400 ms hysteresis.
// Stairwell/roof transitions (e.g. stormhouse interior -> roof) flicker the
// raw flag between floor layers; without debounce the crawl flaps between
// order mode and direct mode, each cancelling the other (field finding,
// 2026-06-12: "struggles to move smoothly through the layers").  Anchor-only
// state — DC controls one character at a time.
static bool      s_indoorEffective  = false;
static bool      s_indoorPendingVal = false;
static ULONGLONG s_indoorPendingMs  = 0;

static bool stableIndoors(CharMovement* mv)
{
    bool raw = mv->isInsideBuildingLoadedInterior();
    if (raw == s_indoorEffective)
    {
        s_indoorPendingMs = 0;
        return s_indoorEffective;
    }
    ULONGLONG now = GetTickCount64();
    if (s_indoorPendingMs == 0 || raw != s_indoorPendingVal)
    {
        s_indoorPendingVal = raw;
        s_indoorPendingMs  = now;
        return s_indoorEffective;
    }
    if (now - s_indoorPendingMs >= 400)
    {
        s_indoorEffective = raw;
        s_indoorPendingMs = 0;
        DebugLog(raw ? "[WASDCombat] dc_downed_zone_now_indoors"
                     : "[WASDCombat] dc_downed_zone_now_outdoors");
    }
    return s_indoorEffective;
}

// downedOrderDriven — ALWAYS false since v1.7.17: downed movement is
// direct-injected everywhere, identical to standing WASD.
// History of the crawl saga, so nobody resurrects the order mode:
//   - Orders indoors path-walk the interior network regardless of dest
//     ("directional keys are meaningless indoors", v1.7.13).
//   - Orders outdoors pathfind a blind 10 m dest; on rooftops/elevated
//     ground that dest lands off the structure and the pathfinder routes
//     back DOWN ("bounce off the roof layer", v1.7.16).
//   - Direct injection was proven downed-capable in v1.7.14 ("downed
//     movement feels great indoors") — the original downed stutter that
//     motivated orders was the combat-steering conflict + order-fighters,
//     both fixed independently (v1.7.6 flip, v1.7.11 gates).
// Order machinery (applyDownedMovement, stableIndoors, the step-5 order
// branch, the flag-gated stops) is retained dormant for rollback.
static bool downedOrderDriven(Character* ch)
{
    (void)ch;
    (void)&stableIndoors;   // keep dormant order machinery referenced
    return false;
}

// applyDownedMovement — issue point-click-equivalent order for downed/crippled
// characters.  Uses playerMoveOrderDefault (pathfind/crawl path) rather than
// setDirectMovement, which is only valid for standing locomotion.
static Ogre::Vector3 s_downedLastDir     = Ogre::Vector3::ZERO;
static Ogre::Vector3 s_downedLastDest    = Ogre::Vector3::ZERO;
static ULONGLONG     s_downedLastIssueMs = 0;
static Ogre::Vector3 s_crawlSamplePos    = Ogre::Vector3::ZERO;
static ULONGLONG     s_crawlSampleMs     = 0;

static void applyDownedMovement(bool bW, bool bA, bool bS, bool bD)
{
    if (!s_freeMoveAnchor || !s_freeMoveAnchor->movement) return;
    Ogre::Vector3 dir;
    if (!computeWASDDirection(bW, bA, bS, bD, dir)) return;
    float dlen = dir.length();
    if (dlen < 0.001f) return;
    dir /= dlen;   // pathfind dest needs direction only, never diagonal scaling

    Ogre::Vector3 posNow = s_freeMoveAnchor->movement->pos;
    ULONGLONG    nowDI   = GetTickCount64();

    // Diagnostic: once per second while crawling, compare actual motion
    // against the intended camera-relative direction.
    if (!s_wasdDownedMovementActive)
    {
        s_crawlSampleMs  = nowDI;
        s_crawlSamplePos = posNow;
    }
    else if (nowDI - s_crawlSampleMs >= 1000)
    {
        Ogre::Vector3 dmoved = posNow - s_crawlSamplePos;
        char buf[192];
        sprintf_s(buf, sizeof(buf),
            "[WASDCombat] dc_crawl_actual moved=(%.2f,%.2f,%.2f) want=(%.2f,%.2f,%.2f)",
            dmoved.x, dmoved.y, dmoved.z, dir.x, dir.y, dir.z);
        DebugLog(buf);
        s_crawlSampleMs  = nowDI;
        s_crawlSamplePos = posNow;
    }

    // Outdoors only — indoor downed movement is direct-injected (see
    // downedOrderDriven); v1.7.13's indoor short-hop attempt proved the
    // interior router path-walks ANY order regardless of distance.
    const float hopLen     = 100.0f;   // point-click range, 10 m
    const float approachAt = 60.0f;

    // ONE persistent order, like a point-click (per-frame re-issue
    // restarts pathfinding before it produces motion — the never-starts
    // stutter).  This only works because NOTHING else is allowed to fight
    // the order while a downed character holds keys: the press-edge
    // disengage and the post-AI standing injection are both downed-gated
    // (v1.7.11) — they were the hidden order-killers that made the
    // throttled crawl die after a few steps.  Re-issue on first press,
    // direction change, a 1.5 s refresh, or approach of the last dest.
    if (s_wasdDownedMovementActive
        && dir.dotProduct(s_downedLastDir) > 0.95f
        && nowDI - s_downedLastIssueMs < 1500
        && (s_downedLastDest - posNow).length() > approachAt)
        return;
    s_downedLastDir     = dir;
    s_downedLastIssueMs = nowDI;

    // Dest at point-click range.  v1.7.11 used 50 m, which is far enough
    // off the local navmesh that the path's first leg could head in the
    // wrong direction — the user-visible "directions are wrong".  Short
    // hops behave like the nearby point-clicks that are known good.
    Ogre::Vector3 dest = posNow + dir * hopLen;
    s_downedLastDest = dest;
    s_freeMoveAnchor->playerMoveOrderDefault(nullptr, nullptr, dest);
    {
        char buf[224];
        sprintf_s(buf, sizeof(buf),
            "[WASDCombat] dc_crawl_issue keys=%d%d%d%d dir=(%.2f,%.2f,%.2f) pos=(%.1f,%.1f,%.1f) dest=(%.1f,%.1f,%.1f)",
            bW?1:0, bA?1:0, bS?1:0, bD?1:0, dir.x, dir.y, dir.z,
            posNow.x, posNow.y, posNow.z, dest.x, dest.y, dest.z);
        DebugLog(buf);
    }
}

// =======================================================================
// OTS action camera — core implementation (ported from OTS_Project_Shelved).
// =======================================================================
static const float FP_RAD_PER_PIXEL = 0.0030f;
static const float FP_PITCH_LIMIT   = 1.45f;   // ~83 degrees, radians

static bool isKenshiForegroundMain()
{
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

// --- First-person raw mouse-look via DirectInput (KenshiFP method) ----------
// A second, NON-EXCLUSIVE BACKGROUND DirectInput mouse device reads the same
// high-rate relative stream the game does — Kenshi keeps its own input (we steal
// no registration, so right-click etc. still work).  A dedicated ~1kHz thread
// owns the reads and accumulates counts; each frame consumes the total.  This
// decouples look feel from framerate: the old GetCursorPos/SetCursorPos warp was
// sampled at the frame rate and felt sluggish-then-teleporty at high fps.  The
// To stay independent of the DirectX import libs (dxguid/dinput8 are not reliably
// on the v100 toolset's lib path), we resolve DirectInput8Create at runtime and
// define the GUIDs + mouse data format ourselves — exactly KenshiFP's approach.
typedef HRESULT (WINAPI *DI8Create_t)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
static const GUID DIFP_GUID_SysMouse =
    { 0x6F1D2B60, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
static const GUID DIFP_IID_IDirectInput8A =
    { 0xBF798030, 0x483A, 0x4DA2, { 0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00 } };
// DIMOUSESTATE2 axes at offsets 0/4/8 (lX/lY/lZ).  NULL pguid = "any object of
// this type" → the device's relative X/Y/Z map onto these slots.
static DIOBJECTDATAFORMAT DIFP_odf[] = {
    { NULL, 0, DIDFT_AXIS | DIDFT_ANYINSTANCE, 0 },
    { NULL, 4, DIDFT_AXIS | DIDFT_ANYINSTANCE, 0 },
    { NULL, 8, DIDFT_AXIS | DIDFT_ANYINSTANCE, 0 },
};
static const DIDATAFORMAT DIFP_df = {
    sizeof(DIDATAFORMAT), sizeof(DIOBJECTDATAFORMAT), DIDF_RELAXIS,
    sizeof(DIMOUSESTATE2), 3, DIFP_odf
};
static IDirectInputDevice8A* s_diMouse      = nullptr;
static bool                  s_diReady      = false;
static volatile LONG         s_diAccX       = 0;
static volatile LONG         s_diAccY       = 0;
static HANDLE                s_diThread     = nullptr;
static volatile LONG         s_diThreadRun  = 0;

static void fpEnsureDInput()
{
    static int tried = 0;
    if (s_diReady || tried >= 600) return;   // retry through early frames, then give up
    tried++;
    HWND w = FindWindowA("OgreD3D11Wnd", nullptr);
    if (!w) w = FindWindowA("OgreD3D9Wnd", nullptr);
    if (!w) w = GetForegroundWindow();
    if (!w) return;
    if (!s_diMouse)
    {
        HMODULE dll = LoadLibraryA("dinput8.dll");
        DI8Create_t create = dll
            ? (DI8Create_t)GetProcAddress(dll, "DirectInput8Create") : nullptr;
        IDirectInput8A* di = nullptr;
        if (!create || FAILED(create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION,
                                     DIFP_IID_IDirectInput8A, (void**)&di, nullptr)) || !di)
        { tried = 600; return; }
        if (FAILED(di->CreateDevice(DIFP_GUID_SysMouse, &s_diMouse, nullptr)) || !s_diMouse)
        { di->Release(); tried = 600; return; }
        di->Release();
        s_diMouse->SetDataFormat(&DIFP_df);
    }
    s_diMouse->SetCooperativeLevel(w, DISCL_BACKGROUND | DISCL_NONEXCLUSIVE);
    if (SUCCEEDED(s_diMouse->Acquire()))
    {
        s_diReady = true;
        VerbLog("[WASDCombat] dc_fp_dinput_acquired");
    }
}

static DWORD WINAPI fpDInputPollThread(void*)
{
    timeBeginPeriod(1);   // 1ms Sleep granularity for this loop
    while (InterlockedCompareExchange(&s_diThreadRun, 1, 1))
    {
        if (s_diReady && s_diMouse)
        {
            DIMOUSESTATE2 st;
            if (SUCCEEDED(s_diMouse->GetDeviceState(sizeof(st), &st)))
            {
                if (st.lX) InterlockedAdd(&s_diAccX, st.lX);
                if (st.lY) InterlockedAdd(&s_diAccY, st.lY);
            }
            else s_diMouse->Acquire();   // lost (alt-tab): re-acquire, skip this poll
        }
        Sleep(1);
    }
    return 0;
}

static void fpStartDInputThread()
{
    if (s_diThread) return;
    InterlockedExchange(&s_diThreadRun, 1);
    s_diThread = CreateThread(nullptr, 0, fpDInputPollThread, nullptr, 0, nullptr);
}

// Consume (and zero) the accumulated relative deltas since the last call.
static void fpTakeMouseAccum(float* dx, float* dy)
{
    *dx = (float)InterlockedExchange(&s_diAccX, 0);
    *dy = (float)InterlockedExchange(&s_diAccY, 0);
}

// otsRestoreCameraToRig — re-attach the Ogre camera to the game's rig and
// restore its transform/FOV/near-clip/auto-tracking, destroying our detached
// node.  ONLY for the inventory face-cam now (the gameplay OTS is scrapped).
// World-map arrow (user 2026-10-08: "the arrow faces the wrong direction in
// first person").  The map's camera marker follows the GAME camera's heading
// (CameraClass::yaw), which our detached FP / OTS views never touch, so it
// froze at the heading you entered with.  Mirror the view yaw into it every
// frame (same convention: forward = (-sin yaw, 0, -cos yaw), as the face-cam
// wake already relies on) and put the original back when the rig takes over.
static bool  s_rigYawSaved = false;
static float s_rigYawSave  = 0.0f;
static void dcSyncRigYaw(CameraClass* cam, float yaw)
{
    if (!cam) return;
    if (!s_rigYawSaved) { s_rigYawSave = cam->yaw; s_rigYawSaved = true; }
    yaw = fmodf(yaw, 6.2831853f);
    if (yaw >  3.1415927f) yaw -= 6.2831853f;
    if (yaw < -3.1415927f) yaw += 6.2831853f;
    cam->yaw = yaw;
}

// World-map arrow, take 2 (test 2026-10-08: third person right, FIRST PERSON
// INVERTED after the yaw sync).  The yaw is not what flips it: in FP the rig's
// center is welded to the character's ROOT while the camera sits at the eye,
// AHEAD of it, so a facing derived from camera -> center points backwards (in
// OTS the camera is behind the root, so the same math points forward).  In FP
// the player camera's getFacingDirection now returns the real view direction.
// s_mapFacingCalls counts FP calls so the log proves the map asks for it.
static volatile LONG s_mapFacingCalls = 0;
static Ogre::Vector3* (*s_getFacingOrig)(const CameraClass* self, Ogre::Vector3* ret);
static Ogre::Vector3* getFacing_hook(const CameraClass* self, Ogre::Vector3* ret)
{
    if (s_firstPersonActive && ou && ou->player && self == ou->player->camera)
    {
        float cp = cosf(s_fpPitchSm);
        *ret = Ogre::Vector3(-sinf(s_fpYawSm) * cp, sinf(s_fpPitchSm), -cosf(s_fpYawSm) * cp);   // q = yaw(Y) * pitch(X) applied to -Z
        InterlockedIncrement(&s_mapFacingCalls);
        return ret;
    }
    return s_getFacingOrig(self, ret);
}

static void otsRestoreCameraToRig(CameraClass* cam)
{
    if (!cam) return;
    if (s_rigYawSaved) { cam->yaw = s_rigYawSave; s_rigYawSaved = false; }
    Ogre::Camera* oc = cam->camera;
    if (oc)
    {
        oc->detachFromParent();
        Ogre::SceneNode* rigNode = cam->getCameraNode();
        if (rigNode)
            rigNode->attachObject(oc);
        if (s_fpCamLocalsSaved)
        {
            oc->setPosition(s_fpSavedCamPos);
            oc->setOrientation(s_fpSavedCamOri);
            oc->setFOVy(s_fpSavedFov);
            if (s_fpSavedNearClip > 0.0f)
                oc->setNearClipDistance(s_fpSavedNearClip);
        }
        if (s_fpHadAutoTrack)
            oc->setAutoTracking(true, cam->getCenterNode());
        if (s_fpNode)
        {
            oc->getSceneManager()->destroySceneNode(s_fpNode);
            s_fpNode = nullptr;
        }
    }
    s_fpNode           = nullptr;
    s_fpCamLocalsSaved = false;
    s_fpHadAutoTrack   = false;
}

// Restore the floating name-tags to the player's setting if we hid them.  Safe
// to call from any teardown path; only touches gui when names were actually
// hidden and gui is valid.
static void otsRestoreNames()
{
    if (!s_namesHidden) return;
    if (gui) gui->showNames(s_savedShowNames);
    s_namesHidden = false;
    DebugLog("[WASDCombat] dc_names_restored");
}

// exitOTS — re-attach the camera to the rig and re-track the anchor.
static void exitOTS(bool restoreCamera)
{
    if (!s_fpActive) return;
    s_fpActive = false;
    if (restoreCamera && ou && ou->player)
    {
        otsRestoreCameraToRig(ou->player->camera);
        if (s_freeMoveAnchor)
            ou->player->startTrackCharacter(s_freeMoveAnchor);
    }
    otsRestoreNames();
    s_fpNode = nullptr;
    s_invFaceRetrackUntilMs = GetTickCount64() + 4000;  // arm follow watchdog
    DebugLog("[WASDCombat] dc_cam_exited");
}

// enterOTS — detach the Ogre camera onto our own root node so we can aim it
// freely (the attached RTS camera can only look top-down, which is why the
// inventory face-cam can't face the character without detaching).  Used ONLY
// for the inventory face-cam, while the sim is paused and the character is
// standing — none of the walking/floor problems that scrapped the gameplay OTS.
static void enterOTS()
{
    if (s_fpActive) return;
    if (!ou || !ou->player || !ou->player->camera
        || !s_freeMoveAnchor || !s_freeMoveAnchor->movement) return;
    CameraClass* cam = ou->player->camera;
    Ogre::Camera* oc = cam->camera;
    if (!oc) return;

    s_otsSavedAltitude = cam->altitude;   // held constant during the face-cam
    cam->stopFollowing();
    s_fpHadAutoTrack = (oc->getAutoTrackTarget() != nullptr);
    oc->setAutoTracking(false);
    s_fpSavedCamPos    = oc->getPosition();
    s_fpSavedCamOri    = oc->getOrientation();
    s_fpSavedFov       = oc->getFOVy();
    s_fpSavedNearClip  = oc->getNearClipDistance();
    {   // DIAG (build 70): vanilla depth planes vs ours - far/near ratio = depth precision
        char cb[160];
        sprintf_s(cb, sizeof(cb), "[WASDCombat] dc_cam_planes vanilla_near=%.3f far=%.1f ours_near=%.3f fov=%.0f",
                  s_fpSavedNearClip, oc->getFarClipDistance(), s_fpNearClipFP, s_fpFovDegFP);
        VerbLog(cb);
    }
    s_fpCamLocalsSaved = true;
    oc->setFOVy(Ogre::Radian(Ogre::Degree(s_fpFovDeg)));
    oc->setNearClipDistance(s_fpNearClip);
    oc->detachFromParent();
    s_fpNode = oc->getSceneManager()->getRootSceneNode()->createChildSceneNode();
    s_fpNode->attachObject(oc);
    oc->setPosition(Ogre::Vector3::ZERO);
    oc->setOrientation(Ogre::Quaternion::IDENTITY);
    // Hide the floating name-tags for the duration of the face-cam.
    if (gui && !s_namesHidden)
    {
        s_savedShowNames = options ? options->showNames : true;
        gui->showNames(false);
        s_namesHidden = true;
        DebugLog("[WASDCombat] dc_names_hidden");
    }
    s_fpActive = true;
    DebugLog("[WASDCombat] dc_cam_entered");
}

// Own-inventory = at least one inventory window open (LIVE count, never stale)
// AND no trade/loot session (edge-latched flag, never stale).  True for a plain
// inventory (1 window) AND a backpack character (2 windows); false for any
// shop/loot/corpse trade.  See s_tradeWindowActive for the staleness rationale.
static bool isOwnInventoryOpen()
{
    return gui && !s_tradeWindowActive
        && !gui->isCharacterEditorMode()   // editor owns the camera; don't fight it
        && gui->getNumOpenInventoryWindows() >= 1;
}

// Inventory move-through eligibility (see s_invMoveThroughActive).  Opt-in via
// InventoryFaceCam=false: DC stays live (movement + camera lock, game running)
// while ANY inventory window is open, EXCEPT during dialogue (which keeps its
// vanilla pause).  Gated on an active DC anchor so we never touch a non-DC frame.
static bool invMoveThroughEligible()
{
    return s_mode == MODE_FREE_MOVE
        && s_freeMoveAnchor
        && !s_settingInventoryFaceCam            // opt-in
        && gui && gui->isAnyInventoryWindowOpen()
        && !gui->inDialogue()                    // dialogue still pauses everything
        && !gui->isCharacterEditorMode();
}

// =======================================================================
// First-person camera — functions (ported from the FPS prototype 2026-07-22).
// Shares the detached-camera machinery (s_fpNode, saved cam locals) with the
// inventory face-cam; s_firstPersonActive is the drive-mode selector.
// =======================================================================

// fpSetHeadBoneHidden — modern-FPS head hide, v1.3.2 FINAL FORM (user decision
// 2026-08-24): SHRINK TINY AT THE NATURAL POSITION — nothing else.
//
// Saga summary: the male head is BODY-MESH skin with blended head/neck vertex
// weights (the attachment inventory proved no head entity exists; the female
// only hides "perfectly" because her modded replacement body — vanillaAppend06,
// Nude Mod HD — has clean head-only rigging).  Every attempt to RELOCATE the
// collapse (neck tuck, -500 drop, bone-group zero) moved the remnant somewhere
// VISIBLE (torso/collar) or stretched shared-weight vertices into artifacts.
// The original v1.3.0 shrink worked precisely because the tiny head stays at
// its NATURAL position — at the FP camera itself, inside the near-clip plane,
// where it cannot render.  So: 0.001 scale, manual control, bone position and
// orientation UNTOUCHED, and the TRUE eye mounts the head bone as originally
// tuned (its derived position is unaffected by scale).
//
// Bone scales partly size Kenshi characters, so the ORIGINAL scale is saved and
// restored exactly — never assumed 1.0.
static const int FP_HIDE_BONE_COUNT = 2;
static const char* const FP_HIDE_BONES[FP_HIDE_BONE_COUNT] =
    { "Bip01 Head", "Head" };
static void fpSetHeadBoneHidden(bool hide)
{
    if (hide == s_fpHeadBoneHidden) return;
    if (!s_freeMoveAnchor) { s_fpHeadBoneHidden = false; return; }
    AppearanceBase* ap = s_freeMoveAnchor->getAppearance();
    Ogre::OldSkeletonInstance* sk = ap ? ap->getSkeleton() : nullptr;
    if (!sk) { s_fpHeadBoneHidden = false; return; }
    bool any = false;
    for (int i = 0; i < FP_HIDE_BONE_COUNT; ++i)
    {
        if (!sk->hasBone(FP_HIDE_BONES[i])) continue;
        Ogre::OldBone* hb = sk->getBone(FP_HIDE_BONES[i]);
        if (!hb) continue;
        if (hide)
        {
            if (!s_fpHideBoneSaved[i])
            {
                s_fpHideBoneSavedScale[i] = hb->getScale();
                s_fpHideBoneSaved[i]      = true;
            }
            hb->setManuallyControlled(true);
            hb->setScale(Ogre::Vector3(0.001f, 0.001f, 0.001f));
            any = true;
        }
        else if (s_fpHideBoneSaved[i])
        {
            hb->setScale(s_fpHideBoneSavedScale[i]);
            hb->setManuallyControlled(false);
            s_fpHideBoneSaved[i] = false;
        }
    }
    s_fpHeadBoneHidden = hide && any;
    DebugLog(hide ? "[WASDCombat] dc_fp_head_hidden"
                  : "[WASDCombat] dc_fp_head_restored");
}

// (THE MALE HEAD REMNANT — final verdict after the full 2026-08-23→27 hunt:
// male bodies render from RUNTIME-GENERATED morph meshes ("human_male.mesh_
// morph_N", Kenshi's physique system) that carry their OWN vertex data — they
// ignore the source file's bone-assignment chunks (purifying ALL THREE
// same-named mesh candidates simultaneously changed nothing), partially ignore
// bone scale, and are not separate scene entities (exhaustive scene searches at
// head AND chest anchors found nothing).  The only remaining approach would be
// live GPU vertex-buffer editing of the generated mesh — out of scope.
// SHIPPED MITIGATION: the head-bone shrink + EyeDrop 0.06 default parks the
// lens so the remnant near-clips away.  Clean-rigged FEMALE replacer bodies
// (Nude Mod HD) vanish completely — recommend in README.)

// fpSetBeardHidden — HideHead companion (2026-08-31): HideHead hides the
// ENTIRE head.  Beards are separate attached entities with their OWN skeletons
// (2026-08 hunt): they inherit neither the head-bone shrink nor shaveHead
// (HideHair = scalp piece only), so a bearded male kept a full-size beard
// floating at the hidden head.  Walk the scene's Entity list (the v8/v17
// export-verified iterator), keep objects riding the anchor's character node
// (bone-attached entities report the char node as their parent scene node),
// skip the body, match by MESH filename ("beard", case-insensitive —
// beard##/Beard##.mesh on disk).  Only VISIBLE matches are hidden, recorded by
// name; restore re-shows recorded names only (see s_fpHiddenBeards).  No
// entity pointers are ever cached (appearance rebuilds recreate entities); the
// 1 Hz mainLoop re-assert heals rebuild resurrections.  logInventory=true also
// names EVERY non-body entity on the char node — if a beard still renders, the
// log carries the real mesh name (v10's silent-miss lesson: silence must be
// impossible).
static void fpSetBeardHidden(bool hide, bool logInventory)
{
    if (!s_freeMoveAnchor) return;
    if (!ou || !ou->player || !ou->player->camera) return;
    Ogre::Camera* ocB = ou->player->camera->camera;
    if (!ocB) return;
    Ogre::SceneManager* smB = ocB->getSceneManager();
    AppearanceBase* apB = s_freeMoveAnchor->getAppearance();
    Ogre::Entity* bodyB = apB ? apB->getBody() : nullptr;
    if (!smB || !bodyB) return;
    Ogre::SceneNode* charNodeB = bodyB->getParentSceneNode();
    if (!charNodeB) return;
    int othersB = 0;
    Ogre::SceneManager::MovableObjectIterator itB =
        smB->getMovableObjectIterator("Entity");
    while (itB.hasMoreElements())
    {
        Ogre::MovableObject* moB = itB.getNext();
        if (!moB || moB == (Ogre::MovableObject*)bodyB) continue;
        if (moB->getParentSceneNode() != charNodeB) continue;
        Ogre::Entity* entB = static_cast<Ogre::Entity*>(moB);
        const Ogre::MeshPtr& meshB = entB->getMesh();
        if (meshB.isNull()) continue;
        const Ogre::String& mnB = meshB->getName();
        char lowB[96];
        size_t nB = mnB.size() < sizeof(lowB) - 1 ? mnB.size() : sizeof(lowB) - 1;
        for (size_t iB = 0; iB < nB; ++iB)
            lowB[iB] = (char)tolower((unsigned char)mnB[iB]);
        lowB[nB] = '\0';
        // Headwear match (2026-08-31 v2): token lists replace the attachment
        // registry, which over-reached into modded CLOTHING in the field.
        // Gear tokens ride HideHeadgear; hair meshes (incl. modded hairstyles
        // like MaleHaircutsSamuraiBun — outside shaveHead AND the registry)
        // ride HideHair.  Any miss shows up by name in the charnode_ent recon.
        // "head" (not "headband") catches headwrap/headband/headgear —
        // field miss 2026-09-01: farmers_headwrap rendered.  NO bare "wrap"
        // token: it would eat the hand-wraps underwear mod.
        static const char* const FP_GEAR_TOKENS[] =
            { "beard", "hat", "helmet", "hood", "mask", "cap",
              "turban", "head", "veil", "goggle" };
        bool matchGearB = false;
        for (int tkB = 0; tkB < 10 && !matchGearB; ++tkB)
            if (strstr(lowB, FP_GEAR_TOKENS[tkB])) matchGearB = true;
        bool matchHairB = strstr(lowB, "hair") != nullptr;
        bool isBeardB = (matchGearB && s_fpHideHeadgear)
                     || (matchHairB && s_fpHideHair);
        ++othersB;
        if (logInventory)
        {
            char invB[192];
            sprintf_s(invB, sizeof(invB),
                "[WASDCombat] dc_fp_charnode_ent mesh=%s vis=%d beard=%d",
                mnB.c_str(), moB->getVisible() ? 1 : 0, isBeardB ? 1 : 0);
            VerbLog(invB);
        }
        if (!isBeardB) continue;
        if (hide)
        {
            if (!moB->getVisible()) continue;   // vanilla-hidden (helmet): not ours
            moB->setVisible(false);
            bool knownB = false;
            for (int k = 0; k < s_fpHiddenBeardCount; ++k)
                if (strcmp(s_fpHiddenBeards[k], lowB) == 0) { knownB = true; break; }
            if (!knownB && s_fpHiddenBeardCount < FP_BEARD_MAX)
                strcpy_s(s_fpHiddenBeards[s_fpHiddenBeardCount++], lowB);
            // Throttled: the per-frame re-assert finds facial attachments
            // re-shown EVERY frame — unthrottled this logged 70 lines/sec
            // (3088-line flood, field log 2026-08-31).
            static ULONGLONG s_fpHwLogTick = 0;
            ULONGLONG nowHw = GetTickCount64();
            if (logInventory || nowHw - s_fpHwLogTick >= 5000)
            {
                s_fpHwLogTick = nowHw;
                char bBuf[160];
                sprintf_s(bBuf, sizeof(bBuf),
                    "[WASDCombat] dc_fp_headwear_hidden mesh=%s", mnB.c_str());
                VerbLog(bBuf);
            }
        }
        else
        {
            for (int k = 0; k < s_fpHiddenBeardCount; ++k)
                if (strcmp(s_fpHiddenBeards[k], lowB) == 0)
                {
                    moB->setVisible(true);
                    char bBuf[160];
                    sprintf_s(bBuf, sizeof(bBuf),
                        "[WASDCombat] dc_fp_beard_restored mesh=%s", mnB.c_str());
                    DebugLog(bBuf);
                    break;
                }
        }
    }
    if (!hide) s_fpHiddenBeardCount = 0;
    if (logInventory && othersB == 0)
        VerbLog("[WASDCombat] dc_fp_charnode_ent_none");
}


// (setAttachmentsVisible-based gear hiding was tried a THIRD time 2026-08-31
// and removed the same night: on the user's modded setup the attachment
// registry contains modded CLOTHING, which vanished with the hats.  Hats,
// beards and modded hairstyles are all charnode ENTITIES — the token walk in
// fpSetBeardHidden covers every one of them precisely, by mesh name.)

// fpSetHeadMask — set/restore the head-pixel hide uniform ("dcHideHead") on the
// anchor's body material (per-character material instance; every subentity).
// (The legacy "hiddenMask" part-bit tool was removed 2026-09-21: the head has
// no part bits, proven 2026-08-31.)
// ABI: every call export-verified 2026-08-31; buffer writes go through the
// CONST inline getters + const_cast because _markDirty (used by the non-const
// getters) is NOT exported — the value lands in the per-draw upload buffer,
// and the 1 Hz re-assert covers any refresh gap.
static void fpSetHeadMask(bool apply)
{
    if (!s_freeMoveAnchor) return;
    AppearanceBase* apM = s_freeMoveAnchor->getAppearance();
    Ogre::Entity* bodyM = apM ? apM->getBody() : nullptr;
    if (!bodyM) return;
    size_t nSub = bodyM->getNumSubEntities();
    if (!apply)
    {
        // Head-pixel uniform back to 0 on every subentity we may have touched.
        if (s_fpHeadPixelApplied)
        {
            s_fpHeadPixelApplied = false;
            for (size_t si = 0; si < nSub; ++si)
            {
                Ogre::SubEntity* seR = bodyM->getSubEntity(si);
                if (!seR) continue;
                const Ogre::MaterialPtr& matR = seR->getMaterial();
                if (matR.isNull()) continue;
                Ogre::Technique* tqR = matR->getTechnique((unsigned short)0);
                Ogre::Pass* psR = tqR ? tqR->getPass((unsigned short)0) : nullptr;
                if (!psR || !psR->hasFragmentProgram()) continue;
                Ogre::GpuProgramParametersSharedPtr fpR = psR->getFragmentProgramParameters();
                if (fpR.isNull()) continue;
                const Ogre::GpuConstantDefinition* dR =
                    fpR->_findNamedConstantDefinition("dcHideHead", false);
                if (!dR) continue;
                const Ogre::GpuProgramParameters* cR = fpR.getPointer();
                *const_cast<float*>(cR->getFloatPointer(dR->physicalIndex)) = 0.0f;
            }
            VerbLog("[WASDCombat] dc_fp_headpixel_restored");
        }
        // Build 72: body cull modes back to what they were.
        if (s_fpBodyCullCount > 0)
        {
            for (size_t si = 0; si < nSub && (int)si < s_fpBodyCullCount; ++si)
            {
                Ogre::SubEntity* seC = bodyM->getSubEntity(si);
                if (!seC) continue;
                const Ogre::MaterialPtr& matC = seC->getMaterial();
                if (matC.isNull()) continue;
                Ogre::Technique* tqC = matC->getTechnique((unsigned short)0);
                Ogre::Pass* psC = tqC ? tqC->getPass((unsigned short)0) : nullptr;
                if (!psC) continue;
                psC->setCullingMode(s_fpBodyCullSaved[si]);
                psC->setManualCullingMode(s_fpBodyManualSaved[si]);
            }
            s_fpBodyCullCount = 0;
            DebugLog("[WASDCombat] dc_fp_body_solid_inside restored");
        }
        return;
    }
    bool pixelUniformFound = false;   // verdict input: the uniform EXISTS somewhere
    for (size_t si = 0; si < nSub; ++si)
    {
        Ogre::SubEntity* seM = bodyM->getSubEntity(si);
        if (!seM) continue;
        const Ogre::MaterialPtr& matM = seM->getMaterial();
        if (matM.isNull()) continue;
        Ogre::Technique* tqM = matM->getTechnique((unsigned short)0);
        Ogre::Pass* psM = tqM ? tqM->getPass((unsigned short)0) : nullptr;
        if (!psM) continue;
        // Build 72: double-sided body while in first person (saved per subentity, restored on exit).
        if (s_fpBodySolidInside && (int)si < FP_BODY_SUB_MAX && s_fpBodyCullCount <= (int)si)
        {
            s_fpBodyCullSaved[si]   = psM->getCullingMode();
            s_fpBodyManualSaved[si] = psM->getManualCullingMode();
            psM->setCullingMode(Ogre::CULL_NONE);
            psM->setManualCullingMode(Ogre::MANUAL_CULL_NONE);
            s_fpBodyCullCount = (int)si + 1;
            if (si == 0) DebugLog("[WASDCombat] dc_fp_body_solid_inside applied");
        }
        // (b) head-pixel hide via the shader-override uniform (the real fix).
        if (s_fpHeadPixelHide && psM->hasFragmentProgram())
        {
            Ogre::GpuProgramParametersSharedPtr fppM = psM->getFragmentProgramParameters();
            const Ogre::GpuConstantDefinition* dP = fppM.isNull() ? nullptr :
                fppM->_findNamedConstantDefinition("dcHideHead", false);
            if (dP)
            {
                pixelUniformFound = true;
                const Ogre::GpuProgramParameters* cP = fppM.getPointer();
                float* vP = const_cast<float*>(cP->getFloatPointer(dP->physicalIndex));
                if (*vP < 0.5f)
                {
                    *vP = 1.0f;
                    s_fpHeadPixelApplied = true;
                    char pbuf[96];
                    sprintf_s(pbuf, sizeof(pbuf),
                        "[WASDCombat] dc_fp_headpixel_applied sub=%u", (unsigned)si);
                    VerbLog(pbuf);
                }
                else if (!s_fpHeadPixelApplied)
                {
                    // Already 1.0 (a stream pseudo-load / switch dropped our flag
                    // without a restore — user 2026-09-14 "face gone" after
                    // FP/OTS/DC toggling, fixed by a reload).  ADOPT it: we own
                    // the hide while FP is active, so the exit restores it.
                    s_fpHeadPixelApplied = true;
                    char pbuf2[96];
                    sprintf_s(pbuf2, sizeof(pbuf2),
                        "[WASDCombat] dc_fp_headpixel_adopted sub=%u", (unsigned)si);
                    VerbLog(pbuf2);
                }
            }
            else
            {
                // Override shader not live (cache/race) — say so once per FP.
                static ULONGLONG s_hpMissTick = 0;
                ULONGLONG nowHp = GetTickCount64();
                if (nowHp - s_hpMissTick >= 10000)
                {
                    s_hpMissTick = nowHp;
                    DebugLog("[WASDCombat] dc_fp_headpixel_uniform_missing");
                }
            }
        }
    }
    // Verdict AFTER the subentity walk (subentities carry different materials —
    // one hit is enough, so the fallback must not fire from a single miss): the
    // uniform was found nowhere, so another mod's character.hlsl owns the slot
    // for this whole session.  Bone-shrink the head instead.  Skipped when the
    // user already runs HideHead; idempotent (the setter no-ops when hidden).
    // (was `!s_fpHeadPixelApplied` — a uniform already at 1.0 counted as
    //  "missing" and shrank the head ON TOP of the pixel hide; then the exit
    //  restored neither cleanly = the missing face.)
    if (s_fpHeadPixelHide && !pixelUniformFound && !s_fpHideHead)
    {
        if (!s_fpPixelFallbackOn)
        {
            s_fpPixelFallbackOn = true;
            DebugLog("[WASDCombat] dc_fp_headpixel_fallback_shrink");
        }
        fpSetHeadBoneHidden(true);
    }
}

// dcBodyRoot — where the anchor's BODY actually is, for camera pivots.
// CharMovement::pos is the LOGIC position: the engine parks it while the body
// is a ragdoll (KO, thrown — physics moves the mesh) and while the character is
// CARRIED (the carrier walks off with the mesh; pos stays at the pickup spot).
// Both OTS and FP orbited/sat at that parked spot (user 2026-09-13).  Carried:
// the game's own getBoneWorldPosition composes the true rendered transform, so
// the pelvis/spine bone is where the body is; ragdoll: the physics root.
// Standing/animated: plain pos (unchanged behaviour).
static Ogre::Vector3 dcBodyRoot(Character* ch)
{
    if (!ch || !ch->movement) return Ogre::Vector3::ZERO;
    Ogre::Vector3 pos = ch->movement->pos;
    if (ch->isBeingCarried())
    {
        AppearanceBase* ap = ch->getAppearance();
        Ogre::OldSkeletonInstance* sk = ap ? ap->getSkeleton() : nullptr;
        if (sk)
        {
            static const char* const BONES[] = { "Bip01 Pelvis", "Bip01 Spine", "Bip01", "Bip01 Head" };
            for (int i = 0; i < 4; ++i)
            {
                if (!sk->hasBone(BONES[i])) continue;
                Ogre::Vector3 b = ch->getBoneWorldPosition(std::string(BONES[i]));
                bool finite = (b.x == b.x) && (b.y == b.y) && (b.z == b.z)
                           && fabsf(b.x) < 1.0e7f && fabsf(b.y) < 1.0e7f && fabsf(b.z) < 1.0e7f;
                if (finite)
                {
                    static ULONGLONG s_carryLogTick = 0;
                    ULONGLONG nowC = GetTickCount64();
                    if (nowC - s_carryLogTick >= 2000)
                    {
                        s_carryLogTick = nowC;
                        char cb[192];
                        sprintf_s(cb, sizeof(cb),
                            "[WASDCombat] dc_cam_carried_root bone=%s body=(%.1f,%.1f,%.1f) pos=(%.1f,%.1f,%.1f)",
                            BONES[i], b.x, b.y, b.z, pos.x, pos.y, pos.z);
                        DebugLog(cb);
                    }
                    return b;
                }
                break;
            }
        }
    }
    if (ch->isRagdoll()) return ch->getRagdollPhysicsRootPos();
    return pos;
}

// fpGetHeadWorld — world-space position of the anchor's animated head/neck bone.
// CRITICAL: the skeleton MUST come from AppearanceBase::getSkeleton() (the game's
// own getter).  Entity::getSkeleton() on the body entity returns a different
// runtime type whose virtual calls CRASH (the v1.8.9 P-toggle crash).  The
// bone's derived position is clean MODEL space relative to the character root;
// the entity node's transform is stale/wrong-space, so world head = logic-space
// root + bone offset rotated by the character's facing yaw.
static bool fpGetHeadWorld(Ogre::Vector3& out)
{
    if (!s_freeMoveAnchor || !s_freeMoveAnchor->movement) return false;
    AppearanceBase* ap = s_freeMoveAnchor->getAppearance();
    Ogre::Entity* ent = ap ? ap->getBody() : nullptr;
    if (!ent) return false;
    Ogre::OldSkeletonInstance* sk = ap->getSkeleton();
    if (!sk) return false;

    CharMovement* mvB   = s_freeMoveAnchor->movement;
    Ogre::Vector3 root  = dcBodyRoot(s_freeMoveAnchor);   // body, not the parked logic pos
    bool bodyMode = s_freeMoveAnchor->isRagdoll() || s_freeMoveAnchor->isBeingCarried();
    (void)mvB;

    // Resolve an EXISTING mount bone.  hasBone (safe on the AppearanceBase
    // skeleton — the only skeleton whose virtual calls don't crash, per the
    // v1.8.9 lesson) both validates the skeleton and picks a name that exists,
    // so the getBoneWorldPosition call below can't hit a missing bone.
    //  * TRUE-world path: prefer the HEAD bone (KenshiFP mounts there); its world
    //    origin is unaffected by the scale-hide, and eye level is a fixed drop
    //    below it.
    //  * legacy synthetic path: base-of-neck first — it keeps animating when the
    //    head is scale-hidden, and the eye sits higher above a neck mount.
    static const char* const BONE_TRUE[]  = { "Bip01 Head", "Bip01 Neck", "Head", "Bip01 Neck1" };
    static const char* const BONE_SYNTH[] = { "Bip01 Neck", "Bip01 Head", "Bip01 Neck1", "Head" };
    static const float        BONE_SYNTH_UP[] = { 2.4f, 1.0f, 2.4f, 1.0f };
    const char* const* names = s_fpTrueBoneEye ? BONE_TRUE : BONE_SYNTH;
    const char* used = nullptr;
    int usedIdx = -1;
    for (int i = 0; i < 4 && !used; ++i)
        if (sk->hasBone(names[i])) { used = names[i]; usedIdx = i; }
    if (!used) return false;

    if (s_fpTrueBoneEye)
    {
        // KenshiFP method: the game's own Character::getBoneWorldPosition composes
        // the full skeleton + entity transform and returns the head bone's TRUE
        // world position — tracking every animation (bob, run-lean, turn) with NO
        // synthetic reconstruction and NO dependence on the view yaw, so a pure
        // pan no longer swings the eye on an arc (the old root+rotate(offset,yaw)
        // formula did).  Same coordinate space as mvB->pos (both game-world), which
        // is the space our root-child s_fpNode consumes.
        Ogre::Vector3 head = s_freeMoveAnchor->getBoneWorldPosition(std::string(used));
        float ddx = head.x - root.x, ddy = head.y - root.y, ddz = head.z - root.z;
        bool plausible = (ddx*ddx + ddy*ddy + ddz*ddz) < 30.0f * 30.0f
                      && (bodyMode || head.y > root.y - 1.0f);   // head above the feet
                                                                 // (not when slung over a shoulder)
        if (plausible)
        {
            out              = head;
            s_fpHeadAboveRoot = ddy;   // live height sample for the EyeDrop scale
            // Neck mount (head hidden): add the neck→head rise in WORLD space.
            // The first attempt routed it through s_fpBoneEyeUp, which the eye
            // compose rotates by the VIEW orientation — pitching down tilted the
            // rise forward and sank the camera into the chest (user screenshots
            // 2026-08-23, "center" view full of body mesh).
            if (strstr(used, "Neck") != nullptr) out.y += 2.4f;
            s_fpBoneEyeUp = 0.0f;   // eye level handled by EyeDrop in the true-eye path
            if (!s_fpBoneLogged)
            {
                s_fpBoneLogged = true;
                char bbuf[256];
                sprintf_s(bbuf, sizeof(bbuf),
                    "[WASDCombat] dc_fp_truebone bone=%s world=(%.1f,%.1f,%.1f)"
                    " root=(%.1f,%.1f,%.1f)",
                    used, head.x, head.y, head.z, root.x, root.y, root.z);
                VerbLog(bbuf);
            }
            return true;
        }
        // implausible (skeleton mid-load / odd rig) -> fall through to synthetic
    }

    // Legacy synthetic path: root + model-space bone offset rotated by the VIEW yaw.
    Ogre::OldBone* b = sk->getBone(used);
    if (!b) return false;
    s_fpBoneEyeUp = (strstr(used, "Neck") != nullptr) ? 2.4f
                  : (usedIdx >= 0 && !s_fpTrueBoneEye ? BONE_SYNTH_UP[usedIdx] : 1.0f);
    Ogre::Vector3 boneModel = b->_getDerivedPosition();
    float mountYaw = s_fpYawSm;   // render-smoothed view yaw (== s_fpYaw when LookSmooth=0)
    Ogre::Quaternion qBody(Ogre::Radian(mountYaw), Ogre::Vector3::UNIT_Y);
    out = root + qBody * boneModel;

    float ddx = out.x - root.x, ddy = out.y - root.y, ddz = out.z - root.z;
    bool plausible = (ddx*ddx + ddy*ddy + ddz*ddz) < 30.0f * 30.0f;
    if (!s_fpBoneLogged)
    {
        s_fpBoneLogged = true;
        char bbuf[256];
        sprintf_s(bbuf, sizeof(bbuf),
            "[WASDCombat] dc_fp_headbone_tracking bone=%s plausible=%d"
            " boneModel=(%.1f,%.1f,%.1f) mountYaw=%.2f world=(%.1f,%.1f,%.1f)",
            used, (int)plausible,
            boneModel.x, boneModel.y, boneModel.z,
            mountYaw, out.x, out.y, out.z);
        DebugLog(bbuf);
    }
    return plausible;
}

// UNIFY (2026-09-04): ONE source of truth for "does a UI/menu currently need
// the free cursor?" — used by FP (fpDriveFrame), CTRL-OTS (cameraUpdate_hook)
// and the shared crosshair, so all three suspend/free the cursor IDENTICALLY.
// Previously each path re-derived this from a slightly different set of terms,
// which is why fixing one mode kept breaking the other.  Superset of every
// prior condition; broader = safer (never leaves the cursor locked in a menu).
static bool dcUiWantsCursor()
{
    // Pause FREES the cursor (user model 2026-09-06): pausing shows/releases the
    // cursor from centre so you can click around while paused; unpausing returns
    // it to centre + hides + locks (the rising-edge recentre in dcUpdateCrosshair).
    // If a UI is still open when you unpause, the other conditions below keep the
    // cursor free until it closes.
    if (ou && ou->isPaused())                             return true; // SPACE + ESC/menu screens
    if (s_lootUiSuspendActive)                            return true; // loot / trade
    if (ou && ou->player && ou->player->contextMenu.isVisible()) return true; // RMB interaction menu
    ManagementScreen* m = ManagementScreen::getSingleton();
    if (m && m->getVisible())                             return true; // map / factions / tech / squad
    if (gui && (gui->isStatsWindowOpen() || gui->inDialogue() || gui->isPaused())) return true; // UIs
    // Inventory MOVE-THROUGH (InventoryFaceCam=false, user 2026-10-08): the
    // view keeps looking while you walk with the inventory open - hold ALT
    // for the cursor to click items.  Every other inventory still frees it.
    if (gui && gui->isAnyInventoryWindowOpen() && !s_invMoveThroughActive) return true;
    // ALT held in first / third person = free cursor (HUD, squad bar, items).
    if ((s_firstPersonActive || s_otsCamActive) && (GetAsyncKeyState(VK_MENU) & 0x8000)) return true;
    return false;
}
// (fpShowCrosshair — the old sand-coloured FP-only "+" — REMOVED in the 2026-09-04
//  unification: both modes now use the single white crosshair in dcUpdateCrosshair.)

// While the RMB hold-menu is open in first-person, tint the hovered option
// yellow-green and the rest parchment.  ContextMenuGUI::optionsList is at 0xF8
// (the class is forward-declared, so the member is read by documented offset).
static const MyGUI::Colour FP_MENU_ACCENT(0.72f, 0.86f, 0.38f, 1.0f);  // yellow-green
static const MyGUI::Colour FP_MENU_NORMAL(0.78f, 0.75f, 0.66f, 1.0f);  // parchment
static void fpTintContextMenu()
{
    if (!ou || !ou->player) return;
    ContextMenu& cm = ou->player->contextMenu;
    if (!cm.isVisible()) return;

    MyGUI::Widget* focus = MyGUI::InputManager::getInstance().getMouseFocusWidget();
    ContextMenuGUI* menus[2] = { cm.menuGUI, cm.menuGUI2 };
    for (int m = 0; m < 2; ++m)
    {
        if (!menus[m]) continue;
        MyGUI::Widget* list =
            *(MyGUI::Widget**)((char*)menus[m] + 0xF8);  // ContextMenuGUI::optionsList
        if (!list) continue;
        size_t n = list->getChildCount();
        for (size_t i = 0; i < n; ++i)
        {
            MyGUI::Widget* c = list->getChildAt(i);
            if (!c) continue;
            MyGUI::TextBox* tb = c->castType<MyGUI::TextBox>(false);
            if (!tb) continue;
            bool hovered = (focus == c);
            if (!hovered && focus)
                for (MyGUI::Widget* p = focus->getParent(); p; p = p->getParent())
                    if (p == c) { hovered = true; break; }
            tb->setTextColour(hovered ? FP_MENU_ACCENT : FP_MENU_NORMAL);
        }
    }
}

// exitFirstPerson — leave first-person: restore head/hair/crosshair, then hand
// the camera back to the game's rig via the shared otsRestoreCameraToRig helper.
static void exitFirstPerson(bool restoreCamera)
{
    if (!s_firstPersonActive) return;
    s_firstPersonActive = false;
    s_fpCursorCaptured  = false;
    if (!restoreCamera) s_rigYawSaved = false;   // camera not ours to restore (load): drop the map-arrow save
    // Restore the exact grass/foliage draw-range the boost overrode on enter.
    if (s_fpOptRangeSaved && options)
    {
        options->grassRange   = s_fpSavedGrassRange;
        options->foliageRange = s_fpSavedFoliageRange;
        s_fpOptRangeSaved     = false;
        DebugLog("[WASDCombat] dc_fp_grass_range_restored");
    }
    fpSetHeadBoneHidden(false);
    fpSetBeardHidden(false, false);
    fpSetHeadMask(false);
    s_fpPixelFallbackOn = false;   // re-probe the shader next FP enter
    if (s_fpHairHidden)
    {
        s_fpHairHidden = false;
        if (s_freeMoveAnchor)
        {
            AppearanceBase* ap = s_freeMoveAnchor->getAppearance();
            if (ap) { ap->shaveHead(false); VerbLog("[WASDCombat] dc_fp_hair_restored"); }
        }
    }
    if (restoreCamera && ou && ou->player)
    {
        otsRestoreCameraToRig(ou->player->camera);
        if (s_freeMoveAnchor)
            ou->player->startTrackCharacter(s_freeMoveAnchor);
    }
    DebugLog("[WASDCombat] dc_fp_exited");
}

// enterFirstPerson — detach the camera onto our root node and open the view at
// the anchor's eye.  Guards against the inventory face-cam already owning the
// camera (s_fpActive).  Mirrors enterOTS but sets FP FOV/near-clip and hides
// the head/hair.  Camera calls MUST run on the game thread (consumed in mainLoop).
static void enterFirstPerson()
{
    if (s_firstPersonActive || s_fpActive) return;
    if (!ou || !ou->player || !ou->player->camera
        || !s_freeMoveAnchor || !s_freeMoveAnchor->movement) return;
    // FP and the detached OTS are mutually exclusive — both detach the same
    // camera.  If OTS is up (e.g. P pressed while CTRL-OTS active), tear it
    // down first so the camera is back on the rig before FP re-detaches it.
    if (s_otsCamActive) exitOtsCam(true);

    // Live re-tune: re-read [FirstPerson] from the INI on every entry so the
    // player can edit FOV / ForwardOffset / EyeUpAdjust / NearClip, toggle P
    // off then on, and see the new camera immediately — no game relaunch.
    {
        char cfgPath[MAX_PATH];
        getConfigPath(cfgPath, sizeof(cfgPath));
        loadFirstPersonConfig(cfgPath);
    }

    CameraClass* cam = ou->player->camera;
    Ogre::Camera* oc = cam->camera;
    if (!oc) return;

    cam->stopFollowing();   // zoom left untouched — restored view = pre-FP view
    s_fpHadAutoTrack = (oc->getAutoTrackTarget() != nullptr);
    oc->setAutoTracking(false);
    s_fpSavedCamPos    = oc->getPosition();
    s_fpSavedCamOri    = oc->getOrientation();
    s_fpSavedFov       = oc->getFOVy();
    s_fpSavedNearClip  = oc->getNearClipDistance();
    {   // DIAG (build 70): vanilla depth planes vs ours - far/near ratio = depth precision
        char cb[160];
        sprintf_s(cb, sizeof(cb), "[WASDCombat] dc_cam_planes vanilla_near=%.3f far=%.1f ours_near=%.3f fov=%.0f",
                  s_fpSavedNearClip, oc->getFarClipDistance(), s_fpNearClipFP, s_fpFovDegFP);
        VerbLog(cb);
    }
    s_fpCamLocalsSaved = true;
    oc->setFOVy(Ogre::Radian(Ogre::Degree(s_fpFovDegFP)));
    oc->setNearClipDistance(s_fpNearClipFP);
    oc->detachFromParent();
    s_fpNode = oc->getSceneManager()->getRootSceneNode()->createChildSceneNode();
    s_fpNode->attachObject(oc);
    oc->setPosition(Ogre::Vector3::ZERO);
    oc->setOrientation(Ogre::Quaternion::IDENTITY);

    // Open the view centered on the character's current facing.
    Ogre::Vector3 d = s_freeMoveAnchor->movement->direction;
    d.y = 0.0f;
    float dlen = d.length();
    if (dlen > 0.001f)
    {
        d /= dlen;
        s_fpYaw = atan2f(-d.x, -d.z);   // forward = (-sin yaw, 0, -cos yaw)
    }
    s_fpPitch = 0.0f;

    if (s_fpHideHair)
    {
        AppearanceBase* ap = s_freeMoveAnchor->getAppearance();
        if (ap) { ap->shaveHead(true); s_fpHairHidden = true; VerbLog("[WASDCombat] dc_fp_hair_hidden"); }
    }
    // Grass/foliage draw-range boost: Kenshi builds grass to a range tuned for the
    // high top-down camera, so at ground level grass only exists in a short ring
    // that pops at its edge as you turn/move.  While FP owns the view, widen the
    // range so grass is already present before it rotates into frame.  Save the
    // exact originals once (guarded) and restore on exit.  Mult 1.0 = vanilla.
    if (options && s_fpGrassRangeMult > 1.0f && !s_fpOptRangeSaved)
    {
        s_fpSavedGrassRange   = options->grassRange;
        s_fpSavedFoliageRange = options->foliageRange;
        s_fpOptRangeSaved     = true;
        options->grassRange   *= s_fpGrassRangeMult;
        options->foliageRange *= s_fpGrassRangeMult;
        DebugLog("[WASDCombat] dc_fp_grass_range_boosted");
    }
    s_fpLeanFwd         = 0.0f;
    s_fpMoveLeanSmooth  = 0.0f;
    s_fpGaitFwdSmooth   = 0.0f;
    s_fpActionClrSmooth = 0.0f;
    s_fpHaveLastFeet    = false;   // true-bone-eye feet-delta speed tracker
    s_fpMoveSpeed       = 0.0f;
    s_fpMoveFwdSmooth   = 0.0f;
    s_fpFeetTickMs      = 0;
    s_fpSmValid         = false;    // re-seed the render-smoothed view on first frame
    s_fpEnemyClearSmooth = 1.0f;    // enemy-clearance pullback starts released
    s_fpEnemyNearestDist = -1.0f;
    s_fpLastStreamValid = false;    // force a streaming teleport on the first FP frame
    s_fpBodyYaw         = s_fpYaw;   // body starts aligned with the opening view
    s_fpHeadSmoothValid = false;
    s_fpBoneLogged      = false;
    s_firstPersonActive = true;
    s_fpCursorCaptured  = false;   // first capture pass establishes the center
    if (s_fpHideHead)
        fpSetHeadBoneHidden(true);
    if (s_fpHideHeadgear || s_fpHideHair)
        fpSetBeardHidden(true, true);
    // Pixel hide is deliberately INDEPENDENT of HideHead (2026-08-31): with
    // both on they overlap harmlessly; HideHead=0 + HeadPixelHide=1 is the
    // decisive A/B — the clip acting alone on a full-size, unshrunk head.
    if (s_fpHideHead || s_fpHeadPixelHide)
        fpSetHeadMask(true);
    DebugLog("[WASDCombat] dc_fp_entered");
}

// fpDriveFrame — the per-frame first-person drive, called from cameraUpdate_hook
// AFTER the game's camera update.  Mouse-look (cursor recentered on the viewport
// center), neck-limit (turn the body when the view exceeds its facing while
// idle), then place the detached node at the head-bone eye looking along the view.
static void fpDriveFrame(CameraClass* thisptr, bool uiOpen)
{
    // Auto-exit: first-person only exists inside DC with a live anchor + no loot UI.
    if (s_mode != MODE_FREE_MOVE || !s_freeMoveAnchor
        || !s_freeMoveAnchor->movement || s_lootUiSuspendActive)
    {
        exitFirstPerson(true);
        return;
    }

    // Crosshair + pointer are owned by the shared dcUpdateCrosshair (called
    // from cameraUpdate_hook for BOTH modes) — nothing to draw here anymore.

    // Mouse-look — paused only while the cursor is genuinely needed: a menu
    // (uiOpen already folds in contextMenu.isVisible), or the interaction
    // context-menu itself.  It is NOT paused for a bare RMB HOLD (field
    // 2026-09-04: holding RMB to MOVE in FP must keep the crosshair centered;
    // the cursor frees only once the interaction menu actually appears, then
    // re-centers on close — user's "move now, but menu still frees").
    bool ctxVisible = ou->player->contextMenu.isVisible();
    if (ctxVisible)
        fpTintContextMenu();
    // uiOpen already folds in dcUiWantsCursor() (menu / loot / contextMenu /
    // pause / etc.), so it is the single suspend signal — no bare RMB hold.
    bool captureOk = !uiOpen && isKenshiForegroundMain();
    if (captureOk)
    {
        // RawMouse: ensure the 1kHz DirectInput look device is running.  Its poll
        // thread accumulates hardware deltas off the frame loop; we consume them
        // below.  Falls back to cursor-warp until the device is acquired.
        bool useRaw = s_fpRawMouse;
        if (useRaw) { fpStartDInputThread(); fpEnsureDInput(); }

        HWND fg = GetForegroundWindow();
        RECT rc;
        if (fg && GetClientRect(fg, &rc))
        {
            POINT center;
            center.x = (rc.left + rc.right) / 2;
            center.y = (rc.top + rc.bottom) / 2;
            ClientToScreen(fg, &center);

            float dx = 0.0f, dy = 0.0f;
            bool  haveDelta = false;
            if (useRaw && s_diReady)
            {
                // Framerate-independent 1kHz DirectInput deltas (raw hardware).
                fpTakeMouseAccum(&dx, &dy);
                haveDelta = s_fpCursorCaptured;    // skip the baseline frame
                SetCursorPos(center.x, center.y);  // keep the crosshair/click point centered
                s_fpCursorCaptured = true;
            }
            else
            {
                // Cursor-warp fallback: RawMouse off, or DI not yet acquired.
                POINT cur;
                if (GetCursorPos(&cur))
                {
                    if (s_fpCursorCaptured)
                    {
                        dx = (float)(cur.x - center.x);
                        dy = (float)(cur.y - center.y);
                        haveDelta = true;
                    }
                    SetCursorPos(center.x, center.y);
                    s_fpCursorCaptured = true;
                }
            }

            if (haveDelta && (dx != 0.0f || dy != 0.0f))
            {
                s_fpYaw   -= dx * FP_RAD_PER_PIXEL * s_fpSensitivityFP;
                s_fpPitch -= dy * FP_RAD_PER_PIXEL * s_fpSensitivityFP;
                if (s_fpPitch >  FP_PITCH_LIMIT) s_fpPitch =  FP_PITCH_LIMIT;
                if (s_fpPitch < -FP_PITCH_LIMIT) s_fpPitch = -FP_PITCH_LIMIT;
                // Mark active mouse-look (deadzone to ignore 1px jitter) so the
                // point-click FollowTurn yields — it only recentres after the mouse
                // has been still for FollowDelayMs.
                if (dx > 1.0f || dx < -1.0f || dy > 1.0f || dy < -1.0f)
                    s_fpLastMouseMoveMs = GetTickCount64();
            }
        }
    }
    else
    {
        s_fpCursorCaptured = false;  // re-baseline when capture resumes
        // Drain deltas accumulated while a UI owns the cursor so the view doesn't
        // jump when capture resumes.
        if (s_fpRawMouse) { float jx, jy; fpTakeMouseAccum(&jx, &jy); }
    }

    CharMovement* mvFP = s_freeMoveAnchor->movement;

    // "Actually moving" — true for WASD, point-click, AND autonomous combat/heal
    // approach movement.  currentlyMoving/currentSpeed are set by the game
    // regardless of what issued the move, so the camera can follow + compensate in
    // all of those cases, not just WASD.
    bool fpMoving = mvFP->currentlyMoving || mvFP->currentSpeed > 0.25f;

    // Body facing.
    //  * CAMERA-PRIORITY (WASD, plus a short grace after release while still
    //    moving): FORCE the body to face the view direction, so movement is strafe-
    //    relative — W walks forward, S backpedals, A/D sidestep — and the body
    //    never rotates away from the camera.  The camera yaw is NEVER touched here,
    //    so it cannot jolt.  The grace is the fix for the backpedal twitch: on the
    //    release/coast frame of an S-walk the character is briefly still facing 180°
    //    from the view, and the old idle neck-limit would SNAP the camera onto that
    //    body direction — a violent jolt.  Holding camera-priority through the coast
    //    keeps refacing the body to the view instead, so by the time we fall to idle
    //    the body is already aligned and nothing snaps.  Motion is unaffected:
    //    setDirectMovement got the world WASD vector directly, so direction here is
    //    facing only, and this hook writes late enough to survive to the frame.
    //  * point-click / autonomous move: camera follows the heading (yields to mouse).
    //  * Idle: neck-limit — turn the body toward the view if you look too far.
    bool strafeGrace = (GetTickCount64() - s_wasdLastHeldMs) < FP_STRAFE_GRACE_MS;
    if (s_frameWasdHeld || (fpMoving && strafeGrace))
    {
        // BODY FACES TRAVEL (user re-design 2026-08-24; replaces the old
        // body-faces-VIEW coupling).  While WASD drives, the body turns toward
        // the actual MOVEMENT heading; the camera looks anywhere freely.  Two
        // wins: (1) the enemy AI reads the anchor's FACING — the old view
        // coupling meant fleeing while looking backward pointed the body AT the
        // pursuers, which Kenshi's combat AI treats as a squared-up combatant
        // (ring-hold + jitter at range instead of run-down-and-attack; field
        // repro 2026-08-24).  Facing the travel direction reads as a genuine
        // fleeing back, and pursuit/attacks behave identically no matter where
        // the player looks.  (2) No more moonwalk: the walk cycle faces the way
        // the body moves.  WASD INPUT stays camera-relative (mapped elsewhere);
        // this is facing only.  On release, s_prevWasdDir zeroes and targetYaw
        // holds the last body yaw through the strafe grace — no snap.
        float targetYaw = s_fpBodyYaw;
        if (s_prevWasdDir.squaredLength() > 0.0001f)
            targetYaw = atan2f(-s_prevWasdDir.x, -s_prevWasdDir.z);
        float dyaw = targetYaw - s_fpBodyYaw;
        while (dyaw >  3.14159265f) dyaw -= 6.28318531f;
        while (dyaw < -3.14159265f) dyaw += 6.28318531f;
        s_fpBodyYaw += dyaw * FP_BODY_TURN;
        while (s_fpBodyYaw >  3.14159265f) s_fpBodyYaw -= 6.28318531f;
        while (s_fpBodyYaw < -3.14159265f) s_fpBodyYaw += 6.28318531f;
        mvFP->direction =
            Ogre::Vector3(-sinf(s_fpBodyYaw), 0.0f, -cosf(s_fpBodyYaw));
    }
    else if (fpMoving && s_fpFollowTurn > 0.0f
             && (GetTickCount64() - s_fpLastMouseMoveMs) > (ULONGLONG)s_fpFollowDelayMs)
    {
        // Point-click / combat / heal-approach movement: the CHARACTER leads (walks
        // its own path), so make the CAMERA follow — gently lerp the view yaw toward
        // the body's movement heading so you look where you're going, the mirror of
        // what WASD does.  We do NOT write mvFP->direction here (the game's pathing
        // owns it); we only turn the view.  CRUCIAL: this only runs after the mouse
        // has been STILL for FollowDelayMs — so while you are actively looking around
        // the follow stays out of the way (no fighting your mouse), then eases the
        // view back onto the path once you let go (field 2026-07-25).
        Ogre::Vector3 hd = mvFP->direction;
        hd.y = 0.0f;
        float hlen = hd.length();
        if (hlen > 0.001f)
        {
            hd /= hlen;
            float headYaw = atan2f(-hd.x, -hd.z);
            float d = headYaw - s_fpYaw;
            while (d >  3.14159265f) d -= 6.28318531f;
            while (d < -3.14159265f) d += 6.28318531f;
            s_fpYaw += d * s_fpFollowTurn;
            while (s_fpYaw >  3.14159265f) s_fpYaw -= 6.28318531f;
            while (s_fpYaw < -3.14159265f) s_fpYaw += 6.28318531f;
            s_fpBodyYaw = headYaw;   // keep body-yaw synced so a later stop won't snap
        }
    }
    else
    {
        Ogre::Vector3 bd = mvFP->direction;
        bd.y = 0.0f;
        float blen = bd.length();
        if (blen > 0.001f)
        {
            bd /= blen;
            float bodyYaw = atan2f(-bd.x, -bd.z);
            float delta   = s_fpYaw - bodyYaw;
            while (delta >  3.14159265f) delta -= 6.28318531f;
            while (delta < -3.14159265f) delta += 6.28318531f;
            if (delta > s_fpNeckLimitRad || delta < -s_fpNeckLimitRad)
            {
                mvFP->direction =
                    Ogre::Vector3(-sinf(s_fpYaw), 0.0f, -cosf(s_fpYaw));
                s_fpYaw = bodyYaw
                        + (delta > 0.0f ?  s_fpNeckLimitRad
                                        : -s_fpNeckLimitRad);
                bodyYaw = s_fpYaw;
            }
            // Keep the smoothed body yaw synced to the resting facing so the next
            // movement turn lerps from where the body actually is (no initial jump).
            s_fpBodyYaw = bodyYaw;
        }
    }

    // LookSmooth: derive a render-smoothed view from the authoritative s_fpYaw/
    // s_fpPitch (which all the control logic above wrote).  Only the VISUAL — the
    // orientation quaternion, the head-bone eye mount, and the forward vectors below
    // — uses the smoothed values; body-facing/motion already used the raw target, so
    // the character still turns crisply.  Smaller per-frame rotation delta shrinks
    // the render-thread grass re-facing mismatch → less side-to-side foliage flicker.
    // LookSmooth=0 → the smoothed value equals the raw value exactly (no change).
    if (!s_fpSmValid) { s_fpYawSm = s_fpYaw; s_fpPitchSm = s_fpPitch; s_fpSmValid = true; }
    {
        float a = 1.0f - s_fpLookSmooth;   // 1.0 = snap (off), <1 = glide
        float dyawS = s_fpYaw - s_fpYawSm;
        while (dyawS >  3.14159265f) dyawS -= 6.28318531f;
        while (dyawS < -3.14159265f) dyawS += 6.28318531f;
        s_fpYawSm += dyawS * a;
        while (s_fpYawSm >  3.14159265f) s_fpYawSm -= 6.28318531f;
        while (s_fpYawSm < -3.14159265f) s_fpYawSm += 6.28318531f;
        s_fpPitchSm += (s_fpPitch - s_fpPitchSm) * a;
    }

    Ogre::Quaternion q =
        Ogre::Quaternion(Ogre::Radian(s_fpYawSm),   Ogre::Vector3::UNIT_Y) *
        Ogre::Quaternion(Ogre::Radian(s_fpPitchSm), Ogre::Vector3::UNIT_X);

    // Enemy body-clip clearance: nearest live hostile distance was sampled this
    // frame in mainLoop (-1 = none in range).  Map it to a 0..1 offset scale —
    // 1 at/beyond EnemyClearRadius, EnemyClearMinScale at contact — and smooth
    // it so the eye eases back rather than snapping.  Both eye paths multiply
    // their forward offsets by it, so an aggressor pressing into the lens pulls
    // the eye back to the (hidden) skull instead of poking inside their model.
    {
        float enemyScale = 1.0f;
        if (s_fpEnemyClearRadius > 0.0f && s_fpEnemyNearestDist >= 0.0f
            && s_fpEnemyNearestDist < s_fpEnemyClearRadius)
        {
            float t = s_fpEnemyNearestDist / s_fpEnemyClearRadius;
            enemyScale = s_fpEnemyClearMinScale
                       + t * (1.0f - s_fpEnemyClearMinScale);
        }
        s_fpEnemyClearSmooth += (enemyScale - s_fpEnemyClearSmooth) * 0.15f;
    }

    // Eye position — primary: the ANIMATED head bone, so the camera rides the
    // neck through every pose.  Smoothed on height to damp stride bob; hard-
    // attached horizontally so a sprinting model can't outrun the camera.
    Ogre::Vector3 eye;
    Ogre::Vector3 headWorld;
    if (fpGetHeadWorld(headWorld))
    {
      if (s_fpTrueBoneEye)
      {
        // KenshiFP weld: eye = the head bone's REAL world position, Y taken RAW
        // (welded to head height — no bob smoothing/lag), plus a HORIZONTAL forward
        // push (along yaw only, so looking down does not sink the eye into the
        // chest) so the eye sits at the face rather than inside the skull.  Because
        // headWorld comes from getBoneWorldPosition and is view-independent, a pure
        // pan no longer moves the eye — the arc-swing (and the grass re-page it
        // fed) is gone.
        // Feet-delta ground speed (framerate-independent, low-passed) drives the
        // forward LEAD so the eye leads faster movement instead of trailing the
        // leaning head.  Keyed off ON-SCREEN speed, not the noisy currentMotion
        // magnitude our notes found unreliable (2026-07-24).
        {
            ULONGLONG nowF = GetTickCount64();
            float dt = (s_fpFeetTickMs > 0) ? (float)(nowF - s_fpFeetTickMs) * 0.001f : 0.0f;
            s_fpFeetTickMs = nowF;
            if (dt > 0.001f && dt < 0.25f && s_fpHaveLastFeet)
            {
                float dfx = mvFP->pos.x - s_fpLastFeetX;
                float dfz = mvFP->pos.z - s_fpLastFeetZ;
                float inst = sqrtf(dfx*dfx + dfz*dfz) / dt;
                if (inst > 400.0f) inst = 400.0f;   // reject teleport/paging jumps
                s_fpMoveSpeed += (inst - s_fpMoveSpeed) * 0.20f;
                // Vertical speed for the stair-climb pullback (+ = ascending).
                float vy = (mvFP->pos.y - s_fpLastFeetY) / dt;
                if (vy >  60.0f) vy =  60.0f;        // reject teleport/paging jumps
                else if (vy < -60.0f) vy = -60.0f;
                s_fpClimbSpeedSmooth += (vy - s_fpClimbSpeedSmooth) * 0.20f;
            }
            s_fpLastFeetX = mvFP->pos.x; s_fpLastFeetZ = mvFP->pos.z;
            s_fpLastFeetY = mvFP->pos.y; s_fpHaveLastFeet = true;
        }
        float lead = 0.0f;
        if (s_fpMoveForward > 0.0f && s_fpMoveSpeedRef > 1.0f)
        {
            float gait = s_fpMoveSpeed / s_fpMoveSpeedRef;   // 0 idle .. ~1 run
            if (gait < 0.0f) gait = 0.0f; else if (gait > 1.25f) gait = 1.25f;
            lead = s_fpMoveForward * gait;
        }
        s_fpMoveFwdSmooth += (lead - s_fpMoveFwdSmooth) * 0.15f;

        // Ascent-aware forward pullback: turn the low-passed climb speed into a
        // 0..1 ramp, shrink the forward push toward StairForwardMinScale, and lift
        // the eye by StairEyeLift so the camera clears the rising steps instead of
        // jamming into them.  Inert on flat ground (ascent01 == 0 -> scale 1, lift 0),
        // so open-ground framing and body-clip protection are unchanged.
        float ascent01 = (s_fpClimbSpeedSmooth > 0.0f)
                       ? s_fpClimbSpeedSmooth * s_fpStairForwardReduce : 0.0f;
        if (ascent01 > 1.0f) ascent01 = 1.0f;
        float ascentScale = 1.0f - ascent01 * (1.0f - s_fpStairForwardMinScale);
        float stairLift   = s_fpStairEyeLift * ascent01;

        // Keep the shared 0..1 speed factor alive in this path too — it drives
        // the MoveNearClip blend below.  It previously only updated in the legacy
        // synthetic-eye branch, which left MoveNearClip dead under TrueBoneEye
        // (the default) — found in the 2026-08-01 anti-clip audit.
        {
            float leanTarget = (s_fpMoveSpeedRef > 1.0f)
                             ? s_fpMoveSpeed / s_fpMoveSpeedRef : 0.0f;
            if (leanTarget > 1.0f) leanTarget = 1.0f;
            s_fpMoveLeanSmooth += (leanTarget - s_fpMoveLeanSmooth) * 0.12f;
        }

        float fwdScale = ascentScale * s_fpEnemyClearSmooth;
        if (g_log.debugLogging)
        {
            static ULONGLONG t = 0; ULONGLONG n = GetTickCount64();
            if (n - t >= 500) { t = n; char b[112];
                sprintf_s(b, sizeof(b),
                    "[WASDCombat] dc_fp_stair climb=%.2f scale=%.2f lift=%.2f",
                    s_fpClimbSpeedSmooth, ascentScale, stairLift);
                DebugLog(b); }
        }

        eye    = headWorld;
        // Height-proportional eye level (user req 2026-08-24): the eye BASE
        // (head bone) already tracks the character's custom height; scale the
        // fixed EyeDrop by the measured head height vs the standard-height
        // reference so short/tall characters keep the same RELATIVE eye
        // placement.  EyeUpAdjust stays absolute (user trim).
        float dropScale = s_fpHeadAboveRoot / 16.0f;
        if (dropScale < 0.5f) dropScale = 0.5f;
        if (dropScale > 2.0f) dropScale = 2.0f;
        eye.y += -(s_fpEyeDrop * dropScale) + s_fpEyeUpAdjust + stairLift;
        Ogre::Vector3 fwd(-sinf(s_fpYawSm), 0.0f, -cosf(s_fpYawSm));
        eye += fwd * ((s_fpFwdOffset + s_fpMoveFwdSmooth) * fwdScale);

        // Optional additive clearances (default 0 = inert) kept from our tuning:
        // gait-forward while jog/sprinting, and committed-action clearance.  They
        // only engage if the user opts in via the INI; both push along the view.
        {
            float gaitTarget = 0.0f;
            if (fpMoving)
            {
                MoveSpeed g = mvFP->speedOrders;
                if (g == JOG) gaitTarget = s_fpJogForward;
                else if (g == RUN || g == GROUPED) gaitTarget = s_fpRunForward;
            }
            s_fpGaitFwdSmooth += (gaitTarget - s_fpGaitFwdSmooth) * 0.10f;
            if (s_fpGaitFwdSmooth > 0.001f) eye += fwd * (s_fpGaitFwdSmooth * fwdScale);
        }
        {
            bool actionNow = s_fpActionClearFwd > 0.0f && isCommittedAction(s_freeMoveAnchor);
            float clrTarget = actionNow ? s_fpActionClearFwd : 0.0f;
            s_fpActionClrSmooth += (clrTarget - s_fpActionClrSmooth) * 0.15f;
            if (s_fpActionClrSmooth > 0.001f) eye += fwd * (s_fpActionClrSmooth * fwdScale);
        }
      }
      else
      {
        if (!s_fpHeadSmoothValid)
        {
            s_fpHeadSmooth      = headWorld;
            s_fpHeadSmoothValid = true;
        }
        else
        {
            s_fpHeadSmooth.x  = headWorld.x;
            s_fpHeadSmooth.z  = headWorld.z;
            s_fpHeadSmooth.y += (headWorld.y - s_fpHeadSmooth.y) * FP_BONE_SMOOTH;
        }
        // Speed-scaled lean compensation: at jog/sprint the model pitches
        // forward and swings the arms/chest up into view.  Ramp a smoothed 0..1
        // speed factor and add extra eye height + forward reach so the camera
        // rises above and past the leaning torso.  Zero when standing/walking,
        // so the natural upright view is untouched.  Magnitudes are INI-tuned.
        {
            float spd = mvFP->currentMotion.length();
            float leanTarget = spd * 0.04f;          // ~1.0 by jog speed
            if (leanTarget > 1.0f) leanTarget = 1.0f;
            s_fpMoveLeanSmooth += (leanTarget - s_fpMoveLeanSmooth) * 0.12f;
        }
        float leanUp  = s_fpMoveLeanUp  * s_fpMoveLeanSmooth;
        float leanFwd = s_fpMoveLeanFwd * s_fpMoveLeanSmooth;
        // Side-look clearance (user req 2026-08-23): looking toward the neck
        // limit swings the shoulder into frame (the eye pivot sits between the
        // shoulders).  Add forward reach proportional to how far sideways the
        // view is from the body facing — zero looking straight ahead, full at
        // 90°+ — so the lens clears the shoulder at the extremes.
        float sideFwd = 0.0f;
        if (s_fpSideLookFwd > 0.0f)
        {
            float relYaw = s_fpYawSm - s_fpBodyYaw;
            while (relYaw >  3.14159265f) relYaw -= 6.28318531f;
            while (relYaw < -3.14159265f) relYaw += 6.28318531f;
            float sideAmt = fabsf(sinf(relYaw));
            sideFwd = s_fpSideLookFwd * sideAmt;
        }
        eye = s_fpHeadSmooth
            + q * Ogre::Vector3(0.0f, s_fpBoneEyeUp + s_fpEyeUpAdjust + leanUp,
                                -((s_fpFwdOffset + leanFwd + sideFwd) * s_fpEnemyClearSmooth));

        // One-frame look-ahead — ONLY along the view forward.  The bone pose read
        // this frame is the previous frame's animation result; at sprint speed that
        // leaves the camera a stride behind, so feed forward velocity ahead by the
        // frame time.  Projected onto the view forward (positive only) so strafing
        // (A/D) and backpedalling (S) never shove the eye sideways or backward —
        // that lateral/back offset was the strafe "twitch".
        {
            static ULONGLONG s_fpLastTickMs = 0;
            ULONGLONG nowFF = GetTickCount64();
            float dt = (s_fpLastTickMs > 0)
                     ? (float)(nowFF - s_fpLastTickMs) * 0.001f : 0.0f;
            s_fpLastTickMs = nowFF;
            if (dt > 0.05f) dt = 0.05f;
            Ogre::Vector3 vel = mvFP->currentMotion;
            vel.y = 0.0f;
            Ogre::Vector3 viewFwd(-sinf(s_fpYawSm), 0.0f, -cosf(s_fpYawSm));
            float fwdComp = vel.dotProduct(viewFwd);
            if (fwdComp > 0.0f)
                eye += viewFwd * (fwdComp * dt);
        }

        // Gait-based forward compensation (the reliable jog/sprint fix).  Keyed
        // off the discrete gait tier (speedOrders) — NOT the noisy currentMotion
        // magnitude — and gated on ACTUAL movement (fpMoving) so it also fires for
        // point-click / combat / heal-approach running, not just WASD (that's when
        // the body was clipping through the lens).  While jogging/running the model
        // leans forward and opens a gap; push the eye forward ALONG THE VIEW — during
        // autonomous movement the view now follows the heading (FollowTurn), so the
        // push lands along the direction of travel.  WALK => 0 (walking untouched).
        {
            float gaitTarget = 0.0f;
            if (fpMoving)
            {
                MoveSpeed gait = mvFP->speedOrders;
                if (gait == JOG)      gaitTarget = s_fpJogForward;
                // RUN = solo sprint; GROUPED = squad-follow speed (group-sprint):
                // treat both as sprint so group movement gets the same fix.
                else if (gait == RUN || gait == GROUPED) gaitTarget = s_fpRunForward;
            }
            s_fpGaitFwdSmooth += (gaitTarget - s_fpGaitFwdSmooth) * 0.10f;
            if (s_fpGaitFwdSmooth > 0.001f)
            {
                Ogre::Vector3 fwdDir(-sinf(s_fpYawSm), 0.0f, -cosf(s_fpYawSm));
                eye += fwdDir * (s_fpGaitFwdSmooth * s_fpEnemyClearSmooth);
            }
        }

        // Committed-action body clearance: during attack swings, blocks, heals,
        // revives and get-ups the arms/torso/head swing hard toward the head bone
        // and clip through the lens even while standing (so the gait push above,
        // which needs movement, can't help).  When such an action is active, push
        // the eye forward along the view to sit clear of the swinging body.  Smoothed
        // so it eases in/out.  0 (default) = off — set ActionClearForward to enable.
        {
            bool actionNow = s_fpActionClearFwd > 0.0f
                          && isCommittedAction(s_freeMoveAnchor);
            float clrTarget = actionNow ? s_fpActionClearFwd : 0.0f;
            s_fpActionClrSmooth += (clrTarget - s_fpActionClrSmooth) * 0.15f;
            if (s_fpActionClrSmooth > 0.001f)
            {
                Ogre::Vector3 fwdDir(-sinf(s_fpYawSm), 0.0f, -cosf(s_fpYawSm));
                eye += fwdDir * (s_fpActionClrSmooth * s_fpEnemyClearSmooth);
            }
        }
      }   // end legacy synthetic-eye branch
    }
    else
    {
        // Fallback: root-relative neck model + speed lean compensation.
        float speed   = mvFP->currentMotion.length();
        float target  = speed * 0.04f;
        if (target > 2.5f) target = 2.5f;
        s_fpLeanFwd  += (target - s_fpLeanFwd) * 0.15f;

        Ogre::Vector3 neck = mvFP->pos;
        neck.y += (s_fpEyeHeight - FP_HEAD_LEN);
        eye = neck
            + q * Ogre::Vector3(0.0f, FP_HEAD_LEN,
                                -(s_fpFwdOffset + s_fpLeanFwd));
    }

    if (s_fpNode)
    {
        s_fpNode->setPosition(eye);
        s_fpNode->setOrientation(q);
    }

    // Near-clip management, applied every frame from the base value:
    //  * MoveNearClip — while moving fast, push the near plane OUT to slice away
    //    the arm/torso that the jog/sprint animation sweeps into the lens (blended
    //    by the 0..1 speed factor; snaps back at a stand so the close-up chest
    //    view is untouched).  Off when <= the base near-clip.
    //  * EnemyNearClip — while a hostile overlaps the lens, pull the near plane
    //    IN toward this value so whatever body part still crosses the plane
    //    slices the thinnest possible cross-section instead of opening a big
    //    see-through hole in the aggressor.  Wins over MoveNearClip (takes the
    //    minimum) because an enemy in your face matters more than your own arms.
    if (thisptr->camera)
    {
        float nc = s_fpNearClipFP;
        if (s_fpMoveNearClip > s_fpNearClipFP)
            nc += (s_fpMoveNearClip - s_fpNearClipFP) * s_fpMoveLeanSmooth;
        if (s_fpEnemyNearClip > 0.0f && s_fpEnemyNearClip < nc
            && s_fpEnemyClearMinScale < 1.0f)
        {
            float overlap01 = (1.0f - s_fpEnemyClearSmooth)
                            / (1.0f - s_fpEnemyClearMinScale);
            if (overlap01 < 0.0f) overlap01 = 0.0f;
            if (overlap01 > 1.0f) overlap01 = 1.0f;
            nc += (s_fpEnemyNearClip - nc) * overlap01;
        }
        // Down / ragdoll / getting up: ease toward DownNearClip (and back).
        {
            ProneState prD = s_freeMoveAnchor->getProneState();
            bool downNow = s_freeMoveAnchor->isRagdoll() || s_freeMoveAnchor->isDown()
                        || s_freeMoveAnchor->isCurrentlyGettingUp
                        || prD == PS_KO || prD == PS_PLAYING_DEAD;
            float tgt = downNow ? 1.0f : 0.0f;
            s_fpDownNearSmooth += (tgt - s_fpDownNearSmooth) * (downNow ? 0.35f : 0.12f);   // in fast, out gentle
            if (s_fpDownNearSmooth > 0.001f && s_fpDownNearClip < nc)
                nc += (s_fpDownNearClip - nc) * s_fpDownNearSmooth;
        }
        thisptr->camera->setNearClipDistance(nc);
    }

    // Keep the game-side rig loosely coherent (audio listener, zone/foliage
    // streaming).  teleport() is a JUMP: calling it every frame while the eye
    // moves makes the streamer re-page grass continuously → the foliage flickers
    // in/out while moving (field 2026-07-25).  Throttle it: only re-teleport once
    // the eye has moved StreamUpdateDist metres from the last streamed point, so
    // streaming stays coherent (a couple of metres of lag is invisible to zone
    // paging) without the per-frame thrash.  StreamUpdateDist=0 restores the old
    // every-frame behaviour for A/B testing.
    //   Anchor = the character ROOT (mvFP->pos), NOT the eye (fix 2026-09-18).
    // The eye sits ForwardOffset (+ gait / action clearance) AHEAD of the head
    // along the VIEW direction, so a pure left/right turn sweeps it on a 1-7 unit
    // arc; measuring that arc against StreamUpdateDist fired teleport() every
    // ~16-40 degrees of yaw while moving = the FP-only turn flicker.  The root
    // is yaw-invariant — exactly what otsCamDrive anchors to (OTS never flickered).
    Ogre::Vector3 streamRoot = mvFP->pos;
    bool doStream = !s_fpLastStreamValid || s_fpStreamDist <= 0.0f
                  || streamRoot.squaredDistance(s_fpLastStreamPos)
                     >= s_fpStreamDist * s_fpStreamDist;
    if (doStream)
    {
        thisptr->teleport(streamRoot);
        s_lastRigTeleportMs = GetTickCount64();   // DIAG
        thisptr->targetPositionY = eye.y;
        thisptr->speedY          = 0.0f;
        s_fpLastStreamPos   = streamRoot;
        s_fpLastStreamValid = true;
    }

    // THE grass-flicker fix (2026-07-25): Kenshi's foliage pager streams grass around
    // the camera's CENTER node, not the eye.  In FP the center node lagged at the
    // character's feet while we rendered from the head, so grass paged around the
    // wrong point and popped under foot.  Reconcile them each frame (after teleport,
    // which can reset the center) so the streaming anchor tracks the character.
    //   Anchor to the character ROOT (mvFP->pos), NOT the eye: the eye swings in a
    // small circle when you PAN (it sits ForwardOffset ahead of the head pivot), so
    // anchoring to it moved the streaming center during pure rotation and PagedGeometry
    // RE-SCATTERED the distant grass — the "scatter variation changes as I pan" that
    // read as flicker at speed (field 2026-07-25).  The root position is stable during
    // rotation (only moves when the character actually walks), so panning no longer
    // re-seeds the grass, while streaming still follows the character as they move.
    // Grass-paging fix — technique from linguine2552/KenshiFP (thanks!).  Kenshi's
    // foliage pager keys off the camera CENTER node.  THREE things matter, and our
    // old every-frame local setPosition got all three wrong:
    //  1. Snap ONLY WHILE MOVING.  At idle we leave the center vanilla/untouched —
    //     a stationary center does not move when you pan, so looking around while
    //     standing no longer re-scatters the grass (our old snap to the rotation-
    //     swinging eye was exactly what re-seeded it every frame you turned).
    //  2. Set the WORLD position via _setDerivedPosition (not local setPosition):
    //     the eye lives under our own node, so hand the pager the true world point.
    //  3. FORCE the derived-position recompute (_getDerivedPositionUpdated) so the
    //     SAME frame's paging pass reads the fresh center — a plain set leaves the
    //     derived value the pager reads stale, which was the residual flicker.
    // (Our camera is detached onto s_fpNode, not a child of center, so unlike
    //  KenshiFP we don't have to re-seat a child camera after moving the center.)
    // 2026-09-18: the weld point is the ROOT (streamRoot), not s_fpNode (the eye).
    // The eye swings on the view-yaw arc described above, so welding the center
    // to it re-scattered the grass on every moving turn — the very thing the
    // paragraph above says not to do.  Root = yaw-invariant, matches OTS.
    // 2026-09-20 FIX (FP + WASD save-load loop): _setDerivedPosition takes a
    // RENDER-space (scene-world) point, and streamRoot is GAME space; the two
    // differ by the root scene node's shift (see focusProject).  The old weld
    // point, s_fpNode->_getDerivedPositionUpdated(), already carried that shift;
    // the raw game-space root parked the streaming center a whole shift away
    // while moving, and the game answered by flagging a save-load every frame
    // (field 2026-09-20: WASD in FP = endless "quickload" loop).  Add the shift.
    if (s_fpFoliageCenterMode && thisptr->center && fpMoving && s_fpNode)
    {
        Ogre::Vector3 rootShift = Ogre::Vector3::ZERO;
        if (Ogre::Node* rootNode = s_fpNode->getParent())
            rootShift = rootNode->_getDerivedPosition();
        thisptr->center->_setDerivedPosition(streamRoot + rootShift);
        thisptr->center->_getDerivedPositionUpdated();
    }
}

// XHAIR-TWEAK: show/hide the fixed crosshair + game pointer.  `want` = FP or
// OTS engaged and no UI suspending it.  Runs on the UI thread (called from
// cameraUpdate_hook post-orig).  Lazily creates the two WhiteSkin bars; if
// creation throws (bad skin/layer on some setup) it degrades to leaving the
// normal cursor — never crashes, never hides the pointer without a crosshair.
// "Manual Attack" HUD label: a small fixed text box (no fade) shown while the
// mode is on.  Kenshi's own small standard text skin; top centre, out of the
// way of the orders panel and the portraits.
static void manualAttackHudUpdate()
{
    bool want = s_manualAttackOn && s_mode == MODE_FREE_MOVE && !s_loadGuardActive;
    if (want && !s_manualHud && !s_manualHudTried)
    {
        s_manualHudTried = true;
        try
        {
            MyGUI::Gui* g = MyGUI::Gui::getInstancePtr();
            MyGUI::IntSize v = MyGUI::RenderManager::getInstance().getViewSize();
            const int W = 120, H = 18;
            if (g)
                s_manualHud = g->createWidget<MyGUI::TextBox>("Kenshi_TextboxStandardText_Small",
                                  v.width / 2 - W / 2, 34, W, H, MyGUI::Align::Default, "Top", "dc_manual_attack_hud");
            if (s_manualHud)
            {
                s_manualHud->setCaption("Manual Attack");
                s_manualHud->setTextAlign(MyGUI::Align::Center);
                s_manualHud->setTextColour(MyGUI::Colour::White);
                s_manualHud->setNeedMouseFocus(false);
                s_manualHud->setVisible(false);
                DebugLog("[WASDCombat] manual_attack_hud_created");
            }
        }
        catch (...) { s_manualHud = nullptr; DebugLog("[WASDCombat] manual_attack_hud_create_failed"); }
    }
    if (!s_manualHud) return;
    if (want)
    {
        MyGUI::IntSize v = MyGUI::RenderManager::getInstance().getViewSize();   // follows resolution changes
        s_manualHud->setPosition(v.width / 2 - 60, 34);
        if (!s_manualHud->getVisible()) s_manualHud->setVisible(true);
    }
    else if (s_manualHud->getVisible()) s_manualHud->setVisible(false);
}

static bool focusTargetLoaded(Character* t);   // fwd decl (manual combat, below)
static bool focusTargetValid(Character* t);    // fwd decl (manual combat, below)

// Whose name + health bars to show: the aim focus, else (manual combat, in a
// fight) whoever the character's AI is fighting - user 2026-10-08: the bars
// appeared only while pointing at an enemy.
// Build 80 (test 2026-10-08: no bars until hovered): the build-79 version gated
// on isInCombatMode + currentTarget only, and neither is reliably set while the
// manual-combat AI is held back - use the same chain the Attack key resolves a
// foe with (AI target, biggest threat, attack target), no combat-flag gate,
// within twice the aim range.
static Character* s_barsFoeLogged = nullptr;
static Character* focusDisplayTarget()
{
    if (s_lockTarget && focusTargetLoaded(s_lockTarget)) return s_lockTarget;
    Character* found = nullptr;
    if (s_manualAttackOn && s_freeMoveAnchor && s_freeMoveAnchor->movement)
    {
        CombatClass* cc = s_freeMoveAnchor->getCombatClass();
        if (cc)
        {
            Character* cand[3] = { cc->currentTarget, cc->getBiggestThreat(cc->threats, 0.0f),
                                   cc->_getAttackTarget().getCharacter() };
            const float lim = s_aimRange * 2.0f;
            for (int i = 0; i < 3 && !found; ++i)
            {
                Character* t = cand[i];
                if (!t || !focusTargetValid(t) || !t->movement) continue;
                float dx = t->movement->pos.x - s_freeMoveAnchor->movement->pos.x;
                float dz = t->movement->pos.z - s_freeMoveAnchor->movement->pos.z;
                if (dx * dx + dz * dz <= lim * lim) found = t;
            }
        }
    }
    if (found != s_barsFoeLogged)
    {
        s_barsFoeLogged = found;
        VerbLog(found ? "[WASDCombat] bars_auto_foe set" : "[WASDCombat] bars_auto_foe none");
    }
    return found;
}


// Game-space point -> screen pixel.  Game -> render space via the root node's
// shift; projection with the camera's own matrices and an explicit w-divide.
// pullToCam nudges the point toward the camera so a marker floats off the body.
static bool focusProject(Ogre::Camera* cam, Ogre::Vector3 gamePos, float pullToCam, int& sx, int& sy)
{
    Ogre::Vector3 rootShift = Ogre::Vector3::ZERO;
    if (cam->getSceneManager() && cam->getSceneManager()->getRootSceneNode())
        rootShift = cam->getSceneManager()->getRootSceneNode()->_getDerivedPosition();
    Ogre::Vector3 pt = gamePos + rootShift;
    Ogre::Vector3 toCam = cam->getDerivedPosition() - pt;
    if (pullToCam > 0.0f && toCam.squaredLength() > 1e-4f) pt += toCam.normalisedCopy() * pullToCam;
    Ogre::Matrix4 vp = cam->getProjectionMatrix() * cam->getViewMatrix(true);
    Ogre::Vector4 c4 = vp * Ogre::Vector4(pt.x, pt.y, pt.z, 1.0f);
    if (c4.w <= 0.001f) return false;
    MyGUI::IntSize v = MyGUI::RenderManager::getInstance().getViewSize();
    sx = (int)(((c4.x / c4.w) * 0.5f + 0.5f) * v.width);
    sy = (int)((0.5f - (c4.y / c4.w) * 0.5f) * v.height);
    return true;
}

// A bone's game-space position, or a fallback height above the feet.
static bool focusBonePos(Character* t, const char* bone, float fallbackUp, Ogre::Vector3& out)
{
    AppearanceBase* ap = t->getAppearance();
    Ogre::OldSkeletonInstance* sk = ap ? ap->getSkeleton() : nullptr;
    if (sk && sk->hasBone(bone)) { out = t->getBoneWorldPosition(std::string(bone)); return true; }
    if (!t->movement) return false;
    out = t->movement->pos + Ogre::Vector3(0.0f, fallbackUp, 0.0f);
    return true;
}

// The glowing dot: a soft radial white glow, gently pulsing.
// The shared 64x64 soft white glow (solid core, squared falloff halo).
static bool focusGlowTexCreate()
{
    if (s_glowTexTried) return s_glowTexOk;
    s_glowTexTried = true;
    try
    {
        MyGUI::ITexture* tex = MyGUI::RenderManager::getInstance().createTexture("dc_glow_tex");
        if (!tex) return false;
        tex->createManual(GLOW_TEX_PX, GLOW_TEX_PX, MyGUI::TextureUsage::Static | MyGUI::TextureUsage::Write, MyGUI::PixelFormat::R8G8B8A8);
        unsigned char* px = (unsigned char*)tex->lock(MyGUI::TextureUsage::Write);
        if (!px) { tex->unlock(); return false; }
        const float c = GLOW_TEX_PX * 0.5f - 0.5f, R = GLOW_TEX_PX * 0.5f;
        for (int y = 0; y < GLOW_TEX_PX; ++y)
        for (int x = 0; x < GLOW_TEX_PX; ++x)
        {
            float r = sqrtf((x - c) * (x - c) + (y - c) * (y - c)) / R;   // 0 centre .. 1 edge
            float a;
            if (r < 0.22f)      a = 1.0f;                                   // solid core
            else if (r < 1.0f)  { float t = (1.0f - r) / 0.78f; a = t * t * t; }   // soft halo
            else                a = 0.0f;
            unsigned char* o = px + ((size_t)y * GLOW_TEX_PX + x) * 4;
            o[0] = 255; o[1] = 255; o[2] = 255; o[3] = (unsigned char)(a * 255.0f + 0.5f);
        }
        tex->unlock();
        s_glowTexOk = true;
        DebugLog("[WASDCombat] glow_tex_created");
        return true;
    }
    catch (...) { DebugLog("[WASDCombat] glow_tex_create_failed"); return false; }
}
static MyGUI::ImageBox* focusGlowBox(const char* name)
{
    MyGUI::Gui* g = MyGUI::Gui::getInstancePtr();
    if (!g || !focusGlowTexCreate()) return nullptr;
    MyGUI::ImageBox* b = g->createWidget<MyGUI::ImageBox>("ImageBox", 0, 0, GLOW_TEX_PX, GLOW_TEX_PX,
                                                          MyGUI::Align::Default, "Top", name);
    if (!b) return nullptr;
    b->setImageTexture("dc_glow_tex");
    b->setNeedMouseFocus(false);
    b->setVisible(false);
    return b;
}
static bool focusHealthBarsCreate()
{
    if (s_hbTried) return s_hbFill[0] != nullptr;
    s_hbTried = true;
    try
    {
        MyGUI::Gui* g = MyGUI::Gui::getInstancePtr();
        if (!g) return false;
        static const MyGUI::Colour FILL[HB_COUNT] = {
            MyGUI::Colour(0.85f, 0.12f, 0.12f),   // blood   - red
            MyGUI::Colour(0.95f, 0.95f, 0.95f),   // head    - white
            MyGUI::Colour(0.40f, 0.85f, 0.40f),   // chest   - green
            MyGUI::Colour(0.40f, 0.85f, 0.40f) }; // stomach - green
        static const char* const LABEL[HB_COUNT] = { "blood", "head", "chest", "stomach" };
        for (int i = 0; i < HB_COUNT; ++i)
        {
            char nb[32], nf[32];
            sprintf_s(nb, sizeof(nb), "dc_hb_bg_%d", i); sprintf_s(nf, sizeof(nf), "dc_hb_fill_%d", i);
            s_hbBg[i]   = g->createWidget<MyGUI::Widget>("WhiteSkin", 0, 0, HB_W, HB_H, MyGUI::Align::Default, "Top", nb);
            s_hbFill[i] = g->createWidget<MyGUI::Widget>("WhiteSkin", 0, 0, HB_W, HB_H, MyGUI::Align::Default, "Top", nf);
            if (!s_hbBg[i] || !s_hbFill[i]) { DebugLog("[WASDCombat] aim_health_bars_create_failed"); return false; }
            s_hbBg[i]->setColour(MyGUI::Colour(0.05f, 0.05f, 0.05f)); s_hbBg[i]->setAlpha(0.6f);
            s_hbBg[i]->setNeedMouseFocus(false); s_hbBg[i]->setVisible(false);
            s_hbFill[i]->setColour(FILL[i]); s_hbFill[i]->setAlpha(0.95f);
            s_hbFill[i]->setNeedMouseFocus(false); s_hbFill[i]->setVisible(false);
            char nl[32]; sprintf_s(nl, sizeof(nl), "dc_hb_label_%d", i);
            s_hbLabel[i] = g->createWidget<MyGUI::TextBox>("Kenshi_TextboxStandardText_Small", 0, 0, HB_LW, HB_LH,
                                                             MyGUI::Align::Default, "Top", nl);
            if (!s_hbLabel[i]) { DebugLog("[WASDCombat] aim_health_bars_create_failed"); return false; }
            s_hbLabel[i]->setCaption(LABEL[i]);
            s_hbLabel[i]->setTextAlign(MyGUI::Align::Right | MyGUI::Align::VCenter);
            s_hbLabel[i]->setTextColour(MyGUI::Colour(0.85f, 0.85f, 0.85f));
            s_hbLabel[i]->setTextShadow(true);
            s_hbLabel[i]->setTextShadowColour(MyGUI::Colour::Black);
            try { s_hbLabel[i]->setFontHeight(9); } catch (...) {}
            s_hbLabel[i]->setNeedMouseFocus(false); s_hbLabel[i]->setVisible(false);
        }
        DebugLog("[WASDCombat] aim_health_bars_created");
        return true;
    }
    catch (...) { DebugLog("[WASDCombat] aim_health_bars_create_failed"); return false; }
}
// 0..1 fill levels for blood / head / chest / stomach (negative health = empty).
static void focusHealthLevels(Character* t, float out[HB_COUNT])
{
    for (int i = 0; i < HB_COUNT; ++i) out[i] = 0.0f;
    MedicalSystem* med = &t->medical;
    float maxB = med->getMaxBlood();
    if (maxB > 0.0f) out[0] = med->blood / maxB;
    std::string names;
    for (auto it = med->status.begin(); it != med->status.end(); ++it)
    {
        GameData* gd = it->first;
        const MedicalSystem::HealthPartStatus& hp = it->second;
        if (!gd) continue;
        std::string nm = gd->name;
        for (size_t k = 0; k < nm.size(); ++k) nm[k] = (char)tolower((unsigned char)nm[k]);
        if (!s_hbNamesLogged) { names += nm; names += ' '; }
        int slot = -1;
        if (nm.find("head") != std::string::npos)         slot = 1;
        else if (nm.find("chest") != std::string::npos)   slot = 2;
        else if (nm.find("stomach") != std::string::npos) slot = 3;
        if (slot < 0) continue;
        float mx = hp._maxHealth;
        out[slot] = mx > 0.0f ? hp.flesh / mx : 0.0f;
    }
    if (!s_hbNamesLogged)
    {
        s_hbNamesLogged = true;
        char b[256];
        sprintf_s(b, sizeof(b), "[WASDCombat] aim_health_parts names=%s", names.c_str());
        VerbLog(b);
    }
    for (int i = 0; i < HB_COUNT; ++i) { if (out[i] < 0.0f) out[i] = 0.0f; if (out[i] > 1.0f) out[i] = 1.0f; }
}
static bool focusNameTagCreate()
{
    if (s_nameTag || s_nameTagTried) return s_nameTag != nullptr;
    s_nameTagTried = true;
    try
    {
        MyGUI::Gui* g = MyGUI::Gui::getInstancePtr();
        if (!g) return false;
        s_nameTag = g->createWidget<MyGUI::TextBox>("Kenshi_TextboxStandardText_Small",
                        0, 0, 220, 18, MyGUI::Align::Default, "Top", "dc_aim_name_tag");
        if (!s_nameTag) return false;
        s_nameTag->setTextAlign(MyGUI::Align::Center);
        s_nameTag->setTextColour(MyGUI::Colour(0.92f, 0.22f, 0.22f));   // enemy name in red (user 2026-09-29)
        s_nameTag->setTextShadow(true);
        s_nameTag->setTextShadowColour(MyGUI::Colour::Black);
        s_nameTag->setNeedMouseFocus(false);
        s_nameTag->setVisible(false);
        DebugLog("[WASDCombat] aim_name_tag_created");
        return true;
    }
    catch (...) { s_nameTag = nullptr; DebugLog("[WASDCombat] aim_name_tag_create_failed"); return false; }
}
static bool focusGlintCreate()
{
    if (s_glintTried) return s_glintBox[0] != nullptr;
    s_glintTried = true;
    try
    {
        for (int i = 0; i < GLINT_MAX; ++i)
        {
            char nm[32]; sprintf_s(nm, sizeof(nm), "dc_glint_%d", i);
            s_glintBox[i] = focusGlowBox(nm);
            if (!s_glintBox[i]) { for (int j = 0; j < i; ++j) { s_glintBox[j]->setVisible(false); } DebugLog("[WASDCombat] glint_create_failed"); return false; }
        }
    }
    catch (...) { DebugLog("[WASDCombat] glint_create_failed"); return false; }
    DebugLog("[WASDCombat] glint_created");
    return true;
}

// Character::attackingYou - the victim is told at swing start.  Track the
// attacker (identity only) when WE are the victim.
static void (*s_attackingYouOrig)(Character* self, Character* attacker, bool so, bool doAwarenessCheck);
static void attackingYou_hook(Character* self, Character* attacker, bool so, bool doAwarenessCheck)
{
    s_attackingYouOrig(self, attacker, so, doAwarenessCheck);
    if (!s_settingGlint || !self || self != s_freeMoveAnchor || !attacker || attacker == self
        || s_mode != MODE_FREE_MOVE || s_dcShutdownInProgress || s_loadGuardActive) return;
    ULONGLONG now = GetTickCount64();
    int slot = -1;
    for (int i = 0; i < GLINT_MAX; ++i) if (s_glint[i].who == attacker) { slot = i; break; }
    if (slot < 0) for (int i = 0; i < GLINT_MAX; ++i) if (!s_glint[i].who) { slot = i; break; }
    if (slot < 0) { slot = 0; for (int i = 1; i < GLINT_MAX; ++i) if (s_glint[i].startMs < s_glint[slot].startMs) slot = i; }
    s_glint[slot].who = attacker; s_glint[slot].startMs = now; s_glint[slot].chopMs = 0; s_glint[slot].impactMs = 0; s_glint[slot].tier = 0;
}
// Which defence works against this attacker's current swing (see GlintSlot).
static int glintTier(Character* me, Character* who, float* powerOut, int* catOut)
{
    if (powerOut) *powerOut = 0.0f;
    if (catOut)   *catOut   = -1;
    if (!me || !who || !me->movement || !who->movement) return 0;
    CombatClass* fc = who->getCombatClass();
    CombatTechniqueData* ft = fc ? fc->currentTechnique : nullptr;
    bool rearCut = false; float power = 0.0f;
    bool heavyTech = ft && (int)SKILL_HEAVY < 0x16 && ft->skillTypes[(int)SKILL_HEAVY];   // the technique belongs to the heavy-weapon skill
    if (ft && !ft->isBlock && ft->numImpactPoints() > 0)
    {
        int n = ft->numImpactPoints();
        int sec = fc->currentComboSection; if (sec < 0) sec = 0; if (sec >= n) sec = n - 1;
        CombatTechniqueData::ImpactPoint* ip = ft->impactPoint(sec);
        if (ip)
        {
            CutDirection d = me->convertCutDirection(ip->direction, who);   // the direction the verdict will see
            rearCut = (d == CUT_REAR_DOWNWARD || d == CUT_REAR_LEFT || d == CUT_REAR_RIGHT);
            power = ip->power;
        }
    }
    Ogre::Vector3 f  = me->movement->direction;  f.y  = 0.0f;
    Ogre::Vector3 to = who->movement->pos - me->movement->pos; to.y = 0.0f;
    bool front = !rearCut;
    if (front && f.squaredLength() > 1e-6f && to.squaredLength() > 1e-6f)
        front = f.normalisedCopy().dotProduct(to.normalisedCopy()) > 0.34f;   // the verdict's ~140 deg arc
    int cat = -1;
    CharStats* ws = who->getStats();
    if (ws) cat = (int)ws->currentWeaponType;
    if (powerOut) *powerOut = power;
    if (catOut)   *catOut   = cat;
    if (!front) return 1;
    if (s_glintHeavyTier && (heavyTech || cat == (int)SKILL_HEAVY)) return 2;
    return 0;
}
// Is any tracked swing still coming at us?  (Used by the idle pin so the AI's
// own block logic is never suppressed while a hit is on the way.)
static bool glintIncoming()
{
    ULONGLONG now = GetTickCount64();
    for (int i = 0; i < GLINT_MAX; ++i)
        if (s_glint[i].who && s_glint[i].impactMs == 0 && now - s_glint[i].startMs < 1500) return true;
    return false;
}
// The attacker whose swing is coming at us right now (earliest live swing), or
// nullptr.  Build 61: the G block POSE is picked for this enemy rather than the
// aim focus, so a hit from behind gets the rear block technique (before, a
// front-facing pose injected for the aimed enemy could sit in the BLOCK state
// while the real swing came from behind, and vanilla could not choose a rear
// block until that clip ended).
static Character* glintIncomingAttacker()
{
    ULONGLONG now = GetTickCount64();
    Character* best = nullptr; ULONGLONG bestStart = 0;
    for (int i = 0; i < GLINT_MAX; ++i)
    {
        const GlintSlot& g = s_glint[i];
        if (!g.who || g.impactMs != 0 || now - g.startMs >= 1500) continue;
        if (!focusTargetLoaded(g.who) || g.who->isDead() || g.who->isUnconcious()) continue;
        if (!best || g.startMs < bestStart) { best = g.who; bestStart = g.startMs; }
    }
    return best;
}
// First visible EditBox with text under a root widget (depth-limited).
static MyGUI::EditBox* dcFindSpeechEdit(MyGUI::Widget* w, int depth)
{
    if (!w || depth > 3 || !w->getVisible()) return nullptr;
    MyGUI::EditBox* e = w->castType<MyGUI::EditBox>(false);
    if (e && !e->getCaption().empty()) return e;
    size_t n = w->getChildCount();
    for (size_t i = 0; i < n; ++i)
    {
        MyGUI::EditBox* r = dcFindSpeechEdit(w->getChildAt(i), depth + 1);
        if (r) return r;
    }
    return nullptr;
}
// The top edge (screen y) of a visible speech bubble sitting over the enemy
// whose head projects to (sx, sy); -1 when there is none.
static int bubbleTopNear(int sx, int sy)
{
    int best = -1;
    MyGUI::Gui* g = MyGUI::Gui::getInstancePtr();
    if (!g) return -1;
    try
    {
        MyGUI::EnumeratorWidgetPtr en = g->getEnumerator();
        while (en.next())
        {
            MyGUI::Widget* root = en.current();
            if (!root || !root->getVisible() || root->getAlpha() <= 0.01f) continue;
            MyGUI::IntCoord c = root->getAbsoluteCoord();
            if (c.width <= 0 || c.height <= 0 || c.height > 160 || c.width > 520) continue;   // bubbles are small
            int cx = c.left + c.width / 2;
            if (abs(cx - sx) > 160) continue;                       // not over this head
            if (c.top < sy - 260 || c.top > sy + 80) continue;      // not near it vertically
            if (!dcFindSpeechEdit(root, 0)) continue;               // no text = not a bubble
            if (best < 0 || c.top < best) best = c.top;
        }
    }
    catch (...) { return -1; }
    return best;
}
// The hit landed (or was blocked): flash the glint out.  Logged for tuning
// GlintLeadMs (swing start -> impact, per attacker technique).
static void glintImpact(Character* who)
{
    if (!who) return;
    ULONGLONG now = GetTickCount64();
    for (int i = 0; i < GLINT_MAX; ++i)
        if (s_glint[i].who == who)
        {
            bool first = s_glint[i].impactMs == 0;
            if (first) s_glint[i].impactMs = now;
            float pw = 0.0f; int cat = -1;
            glintTier(s_freeMoveAnchor, who, &pw, &cat);
            char b[160];
            unsigned skills = 0;
            {
                CombatClass* ac = who->getCombatClass();
                CombatTechniqueData* at = ac ? ac->currentTechnique : nullptr;
                if (at) for (int k = 0; k < 0x16; ++k) if (at->skillTypes[k]) skills |= (1u << k);
            }
            sprintf_s(b, sizeof(b), "[WASDCombat] glint_impact ms=%llu chop_ms=%llu followup=%d tier=%d power=%.0f cat=%d skills=0x%X",
                      now - s_glint[i].startMs, s_glint[i].chopMs ? now - s_glint[i].chopMs : 0ULL, first ? 0 : 1,
                      s_glint[i].tier, pw, cat, skills);
            VerbLog(b);
        }
}

// Per frame (camera hook, post-orig): the aim focus's name above their head
// (any view) and a glint on every weapon swinging at us.
static void focusMarkersUpdate(CameraClass* thisptr)
{
    bool live = s_mode == MODE_FREE_MOVE && !s_loadGuardActive && !s_dcShutdownInProgress
             && thisptr && thisptr->camera && s_freeMoveAnchor;
    Ogre::Camera* cam = live ? thisptr->camera : nullptr;
    ULONGLONG now = GetTickCount64();
    int sx, sy;

    // aim focus (or the AI's current foe) name tag
    bool tagOn = false;
    Character* tagTgt = live ? focusDisplayTarget() : nullptr;
    if (live && s_settingAimName && tagTgt
        && (s_nameTag || focusNameTagCreate()))
    {
        Ogre::Vector3 head;
        if (focusBonePos(tagTgt, "Bip01 Head", 17.0f, head)
            && focusProject(cam, head + Ogre::Vector3(0.0f, 3.0f, 0.0f), 0.0f, sx, sy))
        {
            if (s_nameTagFor != tagTgt)
            {
                s_nameTagFor = tagTgt;
                try { s_nameTag->setCaption(tagTgt->getName()); }
                catch (...) { s_nameTag->setCaption("?"); }
            }
            const int W = 220, H = 18;
            int headY = sy;
            sy -= HB_RAISE;                                       // lift the whole group
            {
                // vanilla speech text up over this enemy? step the group above it
                int bubbleTop = bubbleTopNear(sx, headY);
                int groupBottom = sy + 1 + HB_COUNT * (HB_H + HB_GAP);
                if (bubbleTop >= 0 && groupBottom + 4 > bubbleTop)
                    sy -= (groupBottom + 4 - bubbleTop);
            }
            s_nameTag->setCoord(sx - W / 2, sy - H, W, H);
            tagOn = true;
            // health bars, stacked just under the name
            if (s_hbFill[0] || focusHealthBarsCreate())
            {
                float lv[HB_COUNT];
                focusHealthLevels(tagTgt, lv);
                for (int i = 0; i < HB_COUNT; ++i)
                {
                    int by = sy + 1 + i * (HB_H + HB_GAP);
                    int fw = (int)(HB_W * lv[i] + 0.5f);
                    s_hbBg[i]->setCoord(sx - HB_W / 2, by, HB_W, HB_H);
                    s_hbFill[i]->setCoord(sx - HB_W / 2, by, fw < 1 ? 1 : fw, HB_H);
                    s_hbFill[i]->setVisible(fw >= 1);
                    if (!s_hbBg[i]->getVisible()) s_hbBg[i]->setVisible(true);
                    if (s_hbLabel[i])
                    {
                        s_hbLabel[i]->setCoord(sx - HB_W / 2 - 4 - HB_LW, by + HB_H / 2 - HB_LH / 2, HB_LW, HB_LH);
                        if (!s_hbLabel[i]->getVisible()) s_hbLabel[i]->setVisible(true);
                    }
                }
            }
        }
    }
    if (s_nameTag && s_nameTag->getVisible() != tagOn) s_nameTag->setVisible(tagOn);
    if (!tagOn)
    {
        s_nameTagFor = nullptr;
        for (int i = 0; i < HB_COUNT; ++i)
        {
            if (s_hbBg[i]    && s_hbBg[i]->getVisible())    s_hbBg[i]->setVisible(false);
            if (s_hbFill[i]  && s_hbFill[i]->getVisible())  s_hbFill[i]->setVisible(false);
            if (s_hbLabel[i] && s_hbLabel[i]->getVisible()) s_hbLabel[i]->setVisible(false);
        }
    }

    // weapon glints
    for (int i = 0; i < GLINT_MAX; ++i)
    {
        GlintSlot& g = s_glint[i];
        bool on = false;
        if (live && s_settingGlint && g.who)
        {
            bool drop = false;
            if (!focusTargetLoaded(g.who) || g.who->isDead() || g.who->isUnconcious()) drop = true;
            else
            {
                CombatClass* cc = g.who->getCombatClass();
                swordStateEnum st = cc ? cc->getCombatState() : COMBAT_FINISHED;
                bool swinging = cc && cc->combatModeActive && (st == STARTUP_STATE || st == CHOP_WEAPON);
                if (st == CHOP_WEAPON && g.chopMs == 0) g.chopMs = now;
                if (g.impactMs == 0 && !swinging && now - g.startMs > 150) g.impactMs = now;   // swing over, no hit on us: fade
                if (g.impactMs != 0 && now - g.impactMs > 180) drop = true;
                if (now - g.startMs > 4000) drop = true;                                         // bounded
            }
            if (drop) g.who = nullptr;
            else if (s_glintBox[i] || focusGlintCreate())
            {
                Ogre::Vector3 wp;
                if (focusBonePos(g.who, "Bip01 Prop2", 12.0f, wp) && focusProject(cam, wp, 1.0f, sx, sy))
                {
                    float k;                                                            // 0..1 intensity
                    if (g.impactMs != 0) k = 1.0f - (float)(now - g.impactMs) / 180.0f; // flash out
                    else
                    {
                        float lead = s_glintLeadMs > 1.0f ? s_glintLeadMs : 1.0f;
                        k = 0.30f + 0.70f * (float)(now - g.startMs) / lead;            // ramp up, hold
                        if (k > 1.0f) k = 1.0f;
                    }
                    if (k < 0.0f) k = 0.0f;
                    float shimmer = 0.85f + 0.15f * sinf((float)(now % 160) / 160.0f * 6.2831853f);
                    if (g.impactMs == 0) g.tier = s_glintColourTiers ? glintTier(s_freeMoveAnchor, g.who, nullptr, nullptr) : 0;   // live until the hit (white unless colour tiers are on)
                    static const MyGUI::Colour TIER_COL[3] = {
                        MyGUI::Colour(1.00f, 1.00f, 1.00f),     // block  - white
                        MyGUI::Colour(1.00f, 0.58f, 0.12f),     // dodge  - orange
                        MyGUI::Colour(0.95f, 0.18f, 0.18f) };   // heavy  - red
                    s_glintBox[i]->setColour(TIER_COL[g.tier < 0 ? 0 : (g.tier > 2 ? 2 : g.tier)]);
                    int px = (int)(s_glintPx * (0.6f + 0.7f * k));
                    if (px < 4) px = 4;
                    s_glintBox[i]->setCoord(sx - px / 2, sy - px / 2, px, px);
                    s_glintBox[i]->setAlpha(k * shimmer);
                    on = true;
                }
            }
        }
        if (s_glintBox[i] && s_glintBox[i]->getVisible() != on) s_glintBox[i]->setVisible(on);
    }

    // Perfect-block reward: YOUR weapon glows blue while the next Block press
    // is free (user 2026-10-08).  Same soft glow as the enemy glint; pulses,
    // fades over the last third.  Off-screen weapon (first person, arm out of
    // view) -> the glow sits on the crosshair instead so the cue is never lost.
    {
        bool on = false;
        if (live && s_manualAttackOn && now < s_perfectRewardUntilMs)
        {
            if (!s_rewardGlow && !s_rewardGlowTried)
            {
                s_rewardGlowTried = true;
                try { s_rewardGlow = focusGlowBox("dc_perfect_reward_glow"); } catch (...) { s_rewardGlow = nullptr; }
            }
            if (s_rewardGlow)
            {
                MyGUI::IntSize v = MyGUI::RenderManager::getInstance().getViewSize();
                Ogre::Vector3 wp;
                bool placed = focusBonePos(s_freeMoveAnchor, "Bip01 Prop2", 12.0f, wp)
                           && focusProject(cam, wp, 1.0f, sx, sy)
                           && sx >= 0 && sy >= 0 && sx < v.width && sy < v.height;
                if (!placed && (s_firstPersonActive || s_otsCamActive))
                { sx = v.width / 2; sy = v.height / 2; placed = true; }
                if (placed)
                {
                    float left = (float)(s_perfectRewardUntilMs - now) / (s_perfectRewardMs > 1.0f ? s_perfectRewardMs : 1.0f);   // 1 -> 0
                    float k = left > 0.33f ? 1.0f : left / 0.33f;
                    float pulse = 0.75f + 0.25f * sinf((float)(now % 300) / 300.0f * 6.2831853f);
                    int px = (int)(s_glintPx * 1.3f);
                    if (px < 8) px = 8;
                    s_rewardGlow->setColour(MyGUI::Colour(0.30f, 0.70f, 1.00f));   // bright blue
                    s_rewardGlow->setCoord(sx - px / 2, sy - px / 2, px, px);
                    s_rewardGlow->setAlpha(k * pulse);
                    on = true;
                }
            }
        }
        if (s_rewardGlow && s_rewardGlow->getVisible() != on) s_rewardGlow->setVisible(on);
    }
}

static void dcUpdateCrosshair(bool want)
{
    MyGUI::Gui* g = MyGUI::Gui::getInstancePtr();
    if (!g) return;
    if (want && !s_xhH && !s_xhTried)
    {
        s_xhTried = true;
        try
        {
            MyGUI::IntSize v = MyGUI::RenderManager::getInstance().getViewSize();
            int cx = v.width / 2, cy = v.height / 2;
            const int L = 11, T = 1;   // arm length / thickness (px)
            s_xhH = g->createWidget<MyGUI::Widget>("WhiteSkin",
                cx - L / 2, cy - T / 2, L, T, MyGUI::Align::Center, "Top", "dc_crosshair_h");
            s_xhV = g->createWidget<MyGUI::Widget>("WhiteSkin",
                cx - T / 2, cy - L / 2, T, L, MyGUI::Align::Center, "Top", "dc_crosshair_v");
            if (s_xhH) { s_xhH->setColour(MyGUI::Colour::White); s_xhH->setNeedMouseFocus(false); s_xhH->setVisible(false); }
            if (s_xhV) { s_xhV->setColour(MyGUI::Colour::White); s_xhV->setNeedMouseFocus(false); s_xhV->setVisible(false); }
            DebugLog("[WASDCombat] dc_crosshair_created");
        }
        catch (...) { s_xhH = nullptr; s_xhV = nullptr;
            DebugLog("[WASDCombat] dc_crosshair_create_failed"); }
    }
    bool have = (s_xhH && s_xhV);
    if (!have) return;   // no crosshair → leave the normal cursor alone

    // XHAIR-INDICATOR: while a look-mode is live and the game picked a
    // contextual cursor under the crosshair, the real icon IS the indicator —
    // it renders at the pinned centre, so the "+" yields to it.
    bool iconWanted = want && dcPointerIsContextual();
    s_xhH->setVisible(want && !iconWanted);
    s_xhV->setVisible(want && !iconWanted);

    // XHAIR-TWEAK: the crosshair IS the cursor while a look-mode is active.
    // (1) RETURN-TO-CENTER on the rising edge (want false->true) — the moment a
    //     pause/UI clears and the mode takes back over, snap the cursor (OS +
    //     MyGUI) to screen centre so it re-locks under the "+".  This is the
    //     "unpausing returns the cursor to the centre" step; because want only
    //     rises once EVERY suspending UI has closed, a UI still open at unpause
    //     correctly keeps the cursor free until it too closes (user 2026-09-06).
    // (2) HOLD it there while active — pin MyGUI's tracked pick position to
    //     centre every frame so clicks always act under the "+".
    if (want && !s_xhWasWant)
    {
        MyGUI::InputManager* im = MyGUI::InputManager::getInstancePtr();
        MyGUI::IntSize v = MyGUI::RenderManager::getInstance().getViewSize();
        if (im) im->injectMouseMove(v.width / 2, v.height / 2, 0);
        HWND fg = GetForegroundWindow();
        RECT rc;
        if (fg && GetClientRect(fg, &rc))
        {
            POINT c; c.x = (rc.left + rc.right) / 2; c.y = (rc.top + rc.bottom) / 2;
            ClientToScreen(fg, &c);
            SetCursorPos(c.x, c.y);
        }
    }
    else if (want)
    {
        MyGUI::InputManager* im = MyGUI::InputManager::getInstancePtr();
        if (im)
        {
            MyGUI::IntSize v = MyGUI::RenderManager::getInstance().getViewSize();
            im->injectMouseMove(v.width / 2, v.height / 2, 0);
        }
    }
    s_xhWasWant = want;

    MyGUI::PointerManager* pm = MyGUI::PointerManager::getInstancePtr();
    if (pm)
    {
        if (want)
        {
            // XHAIR-INDICATOR: plain arrow stays hidden behind the "+";
            // contextual pointer shows through as the interaction indicator.
            pm->setVisible(iconWanted);
            s_xhPointerHidden = true;   // we own pointer visibility while active
        }
        else if (s_xhPointerHidden) { pm->setVisible(true); s_xhPointerHidden = false; }
    }
}

// enterOtsCam — detach the Ogre camera onto our own node so the OTS orbit is
// fully ours (mirrors enterFirstPerson).  No engine rotate, no cursor sharing.
static void enterOtsCam()
{
    if (s_otsCamActive || s_fpActive || s_firstPersonActive) return;
    if (!ou || !ou->player || !ou->player->camera
        || !s_freeMoveAnchor || !s_freeMoveAnchor->movement) return;
    CameraClass* cam = ou->player->camera;
    Ogre::Camera* oc = cam->camera;
    if (!oc) return;
    cam->stopFollowing();
    // Capture the rig camera's VANILLA locals ONLY when none are held (v1.4.1
    // upside-down-exit root cause, log 2026-09-12): every zone crossing briefly
    // raises isLoadingFromASaveGame, the camera hook's scene-freeing branch
    // drops s_otsCamActive WITHOUT touching the camera (scene may be dying),
    // and the next frame re-enters here — with the camera STILL parented on
    // our old orbit node.  Capturing then saved local pos ZERO / orientation
    // IDENTITY / the OTS FOV / the blended near clip as "vanilla", and the
    // exit put exactly that onto the rig = camera at the pivot, no auto-track,
    // world upside-down until restart.  The saved values are plain numbers
    // (never pointers), so holding them across a stream — or a real load —
    // is always safe; they're released only by exitOtsCam's restore.
    if (!s_otsCamLocalsSaved)
    {
        s_otsHadAutoTrack   = (oc->getAutoTrackTarget() != nullptr);
        s_otsSavedCamPos    = oc->getPosition();
        s_otsSavedCamOri    = oc->getOrientation();
        s_otsSavedFovR      = oc->getFOVy().valueRadians();
        s_otsSavedNearClip  = oc->getNearClipDistance();
        {
            char cb[128];
            sprintf_s(cb, sizeof(cb), "[WASDCombat] dc_cam_planes_ots vanilla_near=%.3f far=%.1f", s_otsSavedNearClip, oc->getFarClipDistance());
            VerbLog(cb);
        }
        s_otsCamLocalsSaved = true;
    }
    else DebugLog("[WASDCombat] dc_otscam_reentry_keeping_saved_locals");
    oc->setAutoTracking(false);
    oc->setFOVy(Ogre::Radian(Ogre::Degree(s_otsFovDeg)));
    oc->detachFromParent();
    s_otsNode = oc->getSceneManager()->getRootSceneNode()->createChildSceneNode();
    s_otsNode->attachObject(oc);
    oc->setPosition(Ogre::Vector3::ZERO);
    oc->setOrientation(Ogre::Quaternion::IDENTITY);
    // Seed the look yaw from the character's current facing so the view opens
    // behind them (not snapped to some arbitrary heading).
    Ogre::Vector3 d = s_freeMoveAnchor->movement->direction;
    d.y = 0.0f; float dl = d.length();
    if (dl > 0.001f) { d /= dl; s_otsYaw = atan2f(-d.x, -d.z); }
    s_otsPitch = -0.25f;               // negative pitch = look DOWN (over-shoulder)
    s_otsZoomCur = s_otsCamDist;        // base zoom from the slider
    s_otsCollDistSm = s_otsCamDist;     // collision distance starts at full
    s_otsCamDistApplied = s_otsCamDist;
    s_otsCursorCaptured = false;
    s_otsLastStreamValid = false;   // rig coherence: teleport on the first drive frame
    s_otsRootSmValid     = false;   // orbit root re-seeds on the first drive frame
    s_otsRootBlendFrames = 0;
    s_otsCamActive = true;
    DebugLog("[WASDCombat] dc_otscam_entered");
}

// exitOtsCam — re-attach the camera to the rig and restore saved locals.
static void exitOtsCam(bool restoreCamera)
{
    if (!s_otsCamActive) return;
    s_otsCamActive = false;
    if (!restoreCamera) s_rigYawSaved = false;   // camera not ours to restore (load): drop the map-arrow save
    if (restoreCamera && ou && ou->player && ou->player->camera
        && ou->player->camera->camera)
    {
        CameraClass* cam = ou->player->camera;
        Ogre::Camera* oc = cam->camera;
        if (s_rigYawSaved) { cam->yaw = s_rigYawSave; s_rigYawSaved = false; }   // map-arrow sync off
        // Snap the vanilla rig HOME to the anchor BEFORE re-attaching (v1.4.1):
        // after cross-zone OTS travel the parked rig could be a whole zone
        // away, and re-attaching there forced a huge follow snap whose look-at
        // could pass through the vertical and leave the camera ROLLED — the
        // vanilla orbit only ever steers yaw/pitch, so the view stayed upside-
        // down until a game restart (user reports 2026-09).  With the rig
        // teleported to the anchor first, the exit geometry is always local.
        if (s_freeMoveAnchor && s_freeMoveAnchor->movement)
        {
            Ogre::Vector3 home = s_freeMoveAnchor->movement->pos;
            cam->teleport(home);
            cam->targetPositionY = home.y;
            cam->speedY          = 0.0f;
        }
        oc->detachFromParent();
        Ogre::SceneNode* rig = cam->getCameraNode();
        if (rig) rig->attachObject(oc);
        if (s_otsCamLocalsSaved)
        {
            oc->setPosition(s_otsSavedCamPos);
            // Restore the saved local orientation with any ROLL stripped:
            // rebuild it from its look direction as pure yaw*pitch (the only
            // orientations the vanilla orbit can recover from).  Belt-and-
            // braces for the upside-down exit — a rolled quaternion must
            // never reach the rig.
            Ogre::Vector3 svD = s_otsSavedCamOri * Ogre::Vector3::NEGATIVE_UNIT_Z;
            float svL = svD.length();
            if (svL > 0.001f)
            {
                svD /= svL;
                float svPy = svD.y;
                if (svPy >  0.999f) svPy =  0.999f;
                if (svPy < -0.999f) svPy = -0.999f;
                float svPitch = asinf(svPy);
                float svYaw   = (fabsf(svD.x) + fabsf(svD.z) > 0.0001f)
                              ? atan2f(-svD.x, -svD.z) : 0.0f;
                oc->setOrientation(
                    Ogre::Quaternion(Ogre::Radian(svYaw),   Ogre::Vector3::UNIT_Y)
                  * Ogre::Quaternion(Ogre::Radian(svPitch), Ogre::Vector3::UNIT_X));
            }
            else oc->setOrientation(s_otsSavedCamOri);
            oc->setFOVy(Ogre::Radian(s_otsSavedFovR));
            if (s_otsSavedNearClip > 0.0f) oc->setNearClipDistance(s_otsSavedNearClip);
        }
        if (s_otsHadAutoTrack) oc->setAutoTracking(true, cam->getCenterNode());
        if (s_otsNode) { oc->getSceneManager()->destroySceneNode(s_otsNode); s_otsNode = nullptr; }
        if (s_freeMoveAnchor) ou->player->startTrackCharacter(s_freeMoveAnchor);
    }
    s_otsNode = nullptr;
    s_otsCamLocalsSaved = false;
    DebugLog("[WASDCombat] dc_otscam_exited");
}

// otsHeightScale — per-character height compensation for the OTS pivot.  The
// OtsHeight slider is authored against a STANDARD-height character; multiply it
// by (this character's head-above-root / 16.0 reference) so the shoulder pivot —
// and therefore the centered crosshair — sits at the same relative body level on
// tall/short custom characters WITHOUT the player re-tuning per character.  Same
// crash-safe skeleton access + 16.0 reference as the FP EyeDrop dropScale; NO
// side effects on FP state.  Returns 1.0 (no change) if it can't measure.
static float otsHeightScale()
{
    if (!s_freeMoveAnchor || !s_freeMoveAnchor->movement) return 1.0f;
    AppearanceBase* ap = s_freeMoveAnchor->getAppearance();
    Ogre::Entity* ent = ap ? ap->getBody() : nullptr;
    if (!ent) return 1.0f;
    Ogre::OldSkeletonInstance* sk = ap->getSkeleton();
    if (!sk) return 1.0f;
    static const char* const HEAD[] = { "Bip01 Head", "Bip01 Neck", "Head", "Bip01 Neck1" };
    const char* used = nullptr;
    for (int i = 0; i < 4 && !used; ++i)
        if (sk->hasBone(HEAD[i])) used = HEAD[i];
    if (!used) return 1.0f;
    Ogre::Vector3 head = s_freeMoveAnchor->getBoneWorldPosition(std::string(used));
    float above = head.y - s_freeMoveAnchor->movement->pos.y;
    if (above < 4.0f || above > 40.0f) return 1.0f;   // implausible → no scale
    float s = above / 16.0f;                          // 16.0 = standard-height ref (== FP)
    if (s < 0.5f) s = 0.5f;
    if (s > 2.0f) s = 2.0f;
    return s;
}

// otsCamDrive — per-frame detached OTS orbit (called post-orig, player cam).
// Owns mouse-look (reuse FP raw-mouse) + positions the camera behind the
// shoulder.  M1: no collision yet (M2 adds the traceNoActors pull-in).
static void otsCamDrive(CameraClass* thisptr, bool uiOpen, int wheelPre)
{
    if (!s_otsCamActive) return;
    if (s_mode != MODE_FREE_MOVE || !s_freeMoveAnchor || !s_freeMoveAnchor->movement)
        { exitOtsCam(true); return; }
    Ogre::Camera* oc = thisptr->camera;
    if (!oc || !s_otsNode) return;

    // --- mouse-look (we own it; suspended while a UI needs the cursor) ---
    if (!uiOpen && isKenshiForegroundMain())
    {
        bool useRaw = s_fpRawMouse;
        if (useRaw) { fpStartDInputThread(); fpEnsureDInput(); }
        HWND fg = GetForegroundWindow(); RECT rc;
        if (fg && GetClientRect(fg, &rc))
        {
            POINT center; center.x = (rc.left + rc.right) / 2;
            center.y = (rc.top + rc.bottom) / 2; ClientToScreen(fg, &center);
            float dx = 0.0f, dy = 0.0f; bool have = false;
            if (useRaw && s_diReady)
            { fpTakeMouseAccum(&dx, &dy); have = s_otsCursorCaptured;
              SetCursorPos(center.x, center.y); s_otsCursorCaptured = true; }
            else { POINT cur; if (GetCursorPos(&cur))
                   { if (s_otsCursorCaptured) { dx = (float)(cur.x - center.x);
                       dy = (float)(cur.y - center.y); have = true; }
                     SetCursorPos(center.x, center.y); s_otsCursorCaptured = true; } }
            if (have && (dx != 0.0f || dy != 0.0f))
            {
                s_otsYaw   -= dx * FP_RAD_PER_PIXEL * s_fpSensitivityFP;
                s_otsPitch -= dy * FP_RAD_PER_PIXEL * s_fpSensitivityFP;
                if (s_otsPitch >  OTS_PITCH_LIMIT) s_otsPitch =  OTS_PITCH_LIMIT;
                if (s_otsPitch < -OTS_PITCH_LIMIT) s_otsPitch = -OTS_PITCH_LIMIT;
            }
        }
    }
    else s_otsCursorCaptured = false;

    // --- zoom: base from slider, wheel steps the session zoom (we own it) ---
    if (s_otsCamDistApplied != s_otsCamDist)
    { s_otsZoomCur = s_otsCamDist; s_otsCamDistApplied = s_otsCamDist; }
    if (wheelPre != 0)
    {
        s_otsZoomCur -= (float)wheelPre / 120.0f * 3.0f;
        if (s_otsZoomCur < 3.0f)  s_otsZoomCur = 3.0f;
        if (s_otsZoomCur > 60.0f) s_otsZoomCur = 60.0f;
    }

    // --- orbit geometry ---
    // Derive the view basis FROM the orientation quaternion so the camera's
    // look direction and its position offset can never disagree (the earlier
    // hand-rolled trig had the shoulder-right sign flipped and the pitch
    // inverted → camera ended up down by the character's foot).  q = yaw*pitch
    // exactly like FP; fwd is where the lens looks; the shoulder axis is the
    // HORIZONTAL (yaw-only) right so height/side stay level.
    Ogre::Quaternion qYaw = Ogre::Quaternion(Ogre::Radian(s_otsYaw), Ogre::Vector3::UNIT_Y);
    Ogre::Quaternion q    = qYaw * Ogre::Quaternion(Ogre::Radian(s_otsPitch), Ogre::Vector3::UNIT_X);
    Ogre::Vector3 fwd     = q * Ogre::Vector3::NEGATIVE_UNIT_Z;   // view direction
    Ogre::Vector3 rightG  = qYaw * Ogre::Vector3::UNIT_X;         // horizontal shoulder axis
    Ogre::Vector3 root = s_freeMoveAnchor->movement->pos;
    // DOWNED / KO / RAGDOLL follow (user 2026-09-13): while the body is a
    // ragdoll the engine leaves CharMovement::pos parked where the character
    // last STOOD, so the orbit sat over an empty patch of ground while the
    // body lay (or flew) somewhere else.  Orbit the physics root instead, and
    // ease across both hand-offs (ragdoll on/off can each pop the source).
    // A jump > 20 u (zone teleport, load) snaps rather than eases.
    bool ragdoll = s_freeMoveAnchor->isRagdoll() || s_freeMoveAnchor->isBeingCarried();
    if (ragdoll) root = dcBodyRoot(s_freeMoveAnchor);   // ragdoll root / carried body bone
    if (!s_otsRootSmValid || root.squaredDistance(s_otsRootSm) > 400.0f)
    {
        s_otsRootSm      = root;
        s_otsRootSmValid = true;
    }
    else if (ragdoll || s_otsRootBlendFrames > 0)
    {
        s_otsRootSm += (root - s_otsRootSm) * 0.25f;
        if (!ragdoll) --s_otsRootBlendFrames;
    }
    else s_otsRootSm = root;
    if (ragdoll) s_otsRootBlendFrames = 30;
    root = s_otsRootSm;
    // Height compensates for custom character height so the crosshair lands at
    // the same relative body level without re-tuning per character (user req
    // 2026-09-06).  Side is left unscaled — shoulder offset reads well as an
    // absolute across heights and keeps the framing the player tuned.
    float hScale = otsHeightScale();
    Ogre::Vector3 pivot = root
                        + Ogre::Vector3(0.0f, s_otsCamHeight * hScale, 0.0f)
                        + rightG * (s_otsCamSide * s_otsShoulderSign);

    // --- M2 collision: SPHERE-trace from the pivot back toward where the
    // camera wants to sit; if static world geometry blocks it, pull in to just
    // short of the surface so the lens never clips through a wall/floor.  A
    // SPHERE (radius) not a thin ray = less "sensitive" (keeps a margin, small
    // gaps don't snap it); staticOnly=true ignores dynamic props/items so only
    // real walls/terrain block; traceNoActors semantics still ignore characters.
    // OTS_CAM_MIN keeps the camera OUTSIDE the character (was 1.0 → tucked
    // inside the body).  Snap IN quickly, ease OUT so leaving a wall isn't a
    // jerk but also isn't sluggish.
    // START the trace PAST the character's own body/gear (the weapon on the
    // back sat 0.3u behind the pivot and pinned the camera to min every frame —
    // log-proven).  Only geometry beyond OTS_CAM_START (real walls) tucks the
    // camera; the character's own stuff is skipped.  OTS_CAM_MIN keeps the lens
    // outside the body when a real wall does pull it in.
    const float OTS_CAM_RADIUS = 0.4f;
    const float OTS_CAM_START   = 2.5f;     // skip the character's own body/gear
    const float OTS_CAM_MIN     = 4.5f;     // closest the lens may sit to the pivot
    Ogre::Vector3 origin = pivot - fwd * OTS_CAM_START;
    Ogre::Vector3 sweep  = -fwd * (s_otsZoomCur - OTS_CAM_START);
    physHit hit(true, false, 0.0f);
    if (s_otsZoomCur > OTS_CAM_START)
        UtilityT::sphereTrace(hit, origin, sweep, OTS_CAM_RADIUS, 0xFFFFFFFF, true);
    float targetDist = s_otsZoomCur;
    if (hit.hit)
    {
        targetDist = OTS_CAM_START + hit.distance;      // total from the pivot
        if (targetDist < OTS_CAM_MIN) targetDist = OTS_CAM_MIN;
    }
    if (targetDist < s_otsCollDistSm) s_otsCollDistSm += (targetDist - s_otsCollDistSm) * 0.5f;   // in quick
    else                             s_otsCollDistSm += (targetDist - s_otsCollDistSm) * 0.30f;   // out steady
    Ogre::Vector3 camPos = pivot - fwd * s_otsCollDistSm;

    // Near-clip vs pull-in (user 2026-09-11, wall-hug clipping): at close
    // range the vanilla near plane slices the character open (hair/shoulder
    // shells).  Blend the near plane from the game's own value down to
    // OtsNearClip as the camera closes from OTS_NEAR_BLEND_START to
    // OTS_CAM_MIN.  Driven by the SMOOTHED distance so it tracks wall pull-in
    // AND manual wheel zoom with no pops; exitOtsCam already restores the
    // saved vanilla near clip.
    const float OTS_NEAR_BLEND_START = 10.0f;
    if (s_otsSavedNearClip > 0.0f)
    {
        float t = (s_otsCollDistSm - OTS_CAM_MIN)
                / (OTS_NEAR_BLEND_START - OTS_CAM_MIN);
        if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
        float tight = s_otsNearClipMin;
        if (tight > s_otsSavedNearClip) tight = s_otsSavedNearClip;
        oc->setNearClipDistance(tight + (s_otsSavedNearClip - tight) * t);
    }

    s_otsNode->setPosition(camPos);
    s_otsNode->setOrientation(q);   // q computed above (view basis)

    // Rig coherence — audio listener + zone/foliage streaming (v1.4.1).  Same
    // throttled teleport as FP (see fpDriveFrame): every-frame jumps make the
    // streamer re-page grass continuously, so only re-anchor once the character
    // has moved StreamUpdateDist metres.  Anchored to the character ROOT (the
    // orbit target) — stable under pure camera rotation, and it is where the
    // player's "ears" should sit in third person.
    bool otsStream = !s_otsLastStreamValid || s_fpStreamDist <= 0.0f
                   || root.squaredDistance(s_otsLastStreamPos)
                      >= s_fpStreamDist * s_fpStreamDist;
    if (otsStream)
    {
        thisptr->teleport(root);
        s_lastRigTeleportMs = GetTickCount64();   // DIAG
        thisptr->targetPositionY = root.y;
        thisptr->speedY          = 0.0f;
        s_otsLastStreamPos   = root;
        s_otsLastStreamValid = true;
    }
}

// dcDriveFloorReveal — storey visibility while OUR detached camera owns the
// view (FP and OTS).  The game derives the displayed floor from the camera's
// TRACKED character inside restrictPosition; both detached modes stop
// tracking, so without this the floor you climb to never reveals (FP: user
// 2026-07-31 thieves tower; OTS: user 2026-09-14 "fails to render the next
// floor above when walking up the stairs").  Player camera only, once per
// frame, called from cameraUpdate_hook before the mode's drive.
static void dcDriveFloorReveal()
{
    if (!ou || !ou->player) return;
    // Interior floor reveal (user 2026-07-31: shinobi thieves-tower floors did
    // NOT load in FP, unlike plain DC; HUD stayed "Floor 0" on an upper storey).
    // The game normally derives the displayed floor from the camera's TRACKED
    // character inside restrictPosition and reveals it — but FP detaches the
    // camera, stops tracking, and runs the update with controlEnabled=false, so
    // restrictPosition never runs and the player's current floor stays 0.  Drive
    // it ourselves: sync the player's current floor to the controlled character's
    // floor, then refresh storey visibility — once per frame, player camera only.
    if (s_freeMoveAnchor && s_freeMoveAnchor->movement)
    {
        int anchorFloor = s_freeMoveAnchor->movement->getCurrentFloor();
        if (anchorFloor != ou->player->getCurrentFloor())
        {
            ou->player->setCurrentFloor(anchorFloor);
            if (g_log.debugLogging)
            {
                char fbuf[80];
                sprintf_s(fbuf, sizeof(fbuf),
                    "[WASDCombat] dc_fp_floor_sync floor=%d", anchorFloor);
                DebugLog(fbuf);
            }
        }
    }
    ou->player->updateFloorVisibility(ou->player->getAllPlayerCharacters());
    // Below-floor reveal (user 2026-08-01): the character-based pass above
    // hides storeys no squad member stands on, so looking DOWN from an upper
    // floor showed black voids where the lower shells should be (and fast
    // pans flickered them as the vanilla update disagreed frame-to-frame).
    // Force the cutaway to "everything up to the anchor's floor" — the same
    // reveal restrictPosition would compute — AFTER that pass so this is the
    // frame's final word.  INI FloorRevealBelow=0 restores the old behaviour
    // (re-read on every P-enter, so it's revertible without a rebuild).
    //
    // Outside-building reveal (user 2026-08-05): vanilla only cuts a building
    // away while the tracked character is INSIDE one, but neither pass here
    // had that gate — with the whole squad outdoors the squad-based pass hid
    // every storey no one stood on and the anchor-floor clamp pinned the rest
    // to floor 0, so owned multi-floor buildings looked cut open from the
    // outside.  When the anchor is not registered in any building, the
    // frame's final word is "no cutaway" instead (identity/null check only —
    // the Building* is never dereferenced or stored).
    if (s_freeMoveAnchor && s_freeMoveAnchor->movement)
    {
        // Outdoor STRUCTURES (user 2026-09-13: standing on a swamp walkboard
        // in FP left distant owned buildings cut to floor 0 until stepping
        // off): a walkboard/platform registers the character in a Building
        // too, but it has no interior — vanilla never cuts away for those.
        // Only a building WITH an interior counts as "inside".
        // 2nd pass (user 2026-09-13): the swamp ROUND platform passed
        // hasInterior yet is just as roofless, so the gate is now the
        // engine's own "inside a LOADED interior" (isInsideBuildingLoadedInterior,
        // 400 ms debounced by stableIndoors so doorways don't flicker) — an
        // outdoor deck never loads an interior.  hasInterior stays as a
        // cheap pre-filter.  Flags logged on change to settle any other case.
        CharMovement* mvIn = s_freeMoveAnchor->movement;
        Building* bIn = mvIn->building.getBuilding();
        bool hasInt  = bIn && bIn->hasInterior();
        bool insideL = hasInt && stableIndoors(mvIn);
        {
            static int s_lastGate = -1;
            int gate = (bIn ? 1 : 0) | (hasInt ? 2 : 0) | (insideL ? 4 : 0)
                     | (mvIn->isIndoors() ? 8 : 0);
            if (gate != s_lastGate)
            {
                s_lastGate = gate;
                char gb[160];
                sprintf_s(gb, sizeof(gb),
                    "[WASDCombat] dc_fp_cutaway_gate building=%d hasInterior=%d loadedInt=%d indoors=%d floors=%d",
                    bIn ? 1 : 0, hasInt ? 1 : 0, insideL ? 1 : 0, mvIn->isIndoors() ? 1 : 0,
                    bIn ? bIn->getNumFloors() : 0);
                VerbLog(gb);
            }
        }
        if (!insideL)
            ou->player->resetFloorsVisibility();
        else if (s_fpFloorRevealBelow)
            ou->player->setFloorsVisibility(
                s_freeMoveAnchor->movement->getCurrentFloor());
    }
}

// CameraClass::update hook — runs AFTER the game's camera update, BEFORE
// render: this is the only point whose writes survive to the frame.
static void (*s_cameraUpdateOrig)(CameraClass* thisptr, bool controlEnabled);
static void cameraUpdate_hook(CameraClass* thisptr, bool controlEnabled)
{
    // OTS owns the camera: clear the vanilla MMB-rotate state before the
    // game's update sees it, so MMB does nothing while OTS is active.
    if (s_fpActive || s_firstPersonActive)
        thisptr->isRotating = false;
    // Manual combat keys vs vanilla keyboard camera-rotate (Q = rotLeft, E =
    // rotRight are direction flags this update reads): while DC is on, a
    // combat key bound to Q/E must not also spin the camera.  Only the flag
    // for a key we actually use is cleared; MMB/CTRL rotate is untouched, and
    // the vanilla keys return the moment DC is off.
    if (s_mode == MODE_FREE_MOVE && key)
    {
        if (s_bindVk[KR_DODGE] == 'Q' || s_bindVk[KR_BLOCK] == 'Q' || s_bindVk[KR_ATTACK] == 'Q') key->rotLeft  = false;
        if (s_bindVk[KR_DODGE] == 'E' || s_bindVk[KR_BLOCK] == 'E' || s_bindVk[KR_ATTACK] == 'E') key->rotRight = false;
    }

    // (2026-09-06) The old engine-rotate OTS force (key->rotate = true) was
    // removed — OTS now drives its OWN detached camera and never uses the
    // vanilla rotate.  Nothing to force here anymore.

    // While the inventory face-cam owns the camera, force controlEnabled=false
    // into the vanilla update — this is Kenshi's OWN gate for "ignore camera
    // input" (it passes false when a UI has focus), so the wheel-zoom and
    // WASD/edge pan never run.  Those were still moving/scaling the world
    // name-tags because the vanilla update lays the tags out from the (just-
    // moved) camera DURING orig, before our post-orig altitude restore could
    // undo it (field 2026-06-17: scrolling/WASD still moved the squad names).
    // We drive the detached camera ourselves via s_fpNode, so we need nothing
    // from orig's input handling here.
    bool ctlEnabled = (s_fpActive || s_firstPersonActive) ? false : controlEnabled;

    // OTS-TWEAK v2: this frame's wheel delta — must be read BEFORE orig
    // (CameraClass::update consumes and zeroes mWheel in its own zoom).
    int otsWheelPre = key ? key->mWheel : 0;

    s_cameraUpdateOrig(thisptr, ctlEnabled);
    manualAttackHudUpdate();    // "Manual Attack" label (self-gated)
    focusMarkersUpdate(thisptr);   // lock dot + weapon glints (self-gated)

    // SCENE-FREEING — the Ogre scene is actively being torn down: only `!ou`
    // (world gone) and a save-load in progress qualify.  Touching the camera
    // then reads freed memory (v1.8.0 crash).  Stop driving + mark restore
    // PENDING; keep s_fpNode + saved locals.  Deliberately NOT gated on
    // s_dcShutdownInProgress / s_loadGuardActive: those are mod wait-states
    // that STAY true through char-creation (no player to stabilize on), which
    // would strand the camera detached and hide the new-game character preview
    // (field 2026-06-13).  Once the scene is stable they're safe to ignore.
    bool sceneFreeing = !ou || dcRealLoad();   // build 67: a chunk stream keeps OTS / FP driving
    if (!sceneFreeing && ou->isLoadingFromASaveGame()
        && !(s_freeMoveAnchor && s_gwFrame && dcInUpdateList(s_gwFrame, s_freeMoveAnchor)))
        sceneFreeing = true;                    // build 74: flag up and the anchor is not provably alive -> treat as a real load
    if (sceneFreeing)
    {
        s_otsCamDistApplied = -1.0f;
        // Detached OTS: the scene/camera is being torn down — drop our state
        // WITHOUT touching the (freed) camera or node (mirrors the FP handling
        // just below).  s_otsNode dies with the scene; recreated on next enter.
        s_otsCamActive      = false;
        s_otsNode           = nullptr;   // (an old node survives a mere zone
                                         // stream as an empty orphan — harmless)
        // s_otsCamLocalsSaved is deliberately KEPT: the saved vanilla locals are
        // plain values and are the only correct thing to restore on the exit
        // after a zone-crossing re-entry (see enterOtsCam).
        s_otsCursorCaptured = false;
        s_invFaceRetrackUntilMs = 0;
        dcUpdateCrosshair(false);    // XHAIR-TWEAK: never strand a hidden pointer
        if (s_fpActive || s_firstPersonActive)
        {
            s_fpActive          = false;
            s_firstPersonActive = false;
            s_fpCursorCaptured  = false;
            s_fpHeadBoneHidden  = false;  // skeleton torn down — restore flags moot
            s_fpBodyCullCount   = 0;      // (materials gone with the scene)
            s_fpDownNearSmooth  = 0.0f;
            for (int hbT = 0; hbT < 2; ++hbT)
                s_fpHideBoneSaved[hbT] = false;
            s_fpHairHidden      = false;
            s_otsRestorePending = true;
            if (s_fpOptRangeSaved && options)   // never leave the range boosted
            {
                options->grassRange   = s_fpSavedGrassRange;
                options->foliageRange = s_fpSavedFoliageRange;
                s_fpOptRangeSaved     = false;
            }
        }
        return;
    }

    // Scene is stable (gameplay OR char-creation/menu).  thisptr is a valid
    // camera even when ou->player is null, so restore via thisptr.
    // (1) Deferred restore from a load/shutdown that hit while OTS was active.
    if (s_otsRestorePending)
    {
        s_otsRestorePending = false;
        s_fpSuspendedForInv = false;   // a load cancels any pending FP auto-return
        otsRestoreCameraToRig(thisptr);
        if (ou && ou->player && s_freeMoveAnchor)
            ou->player->startTrackCharacter(s_freeMoveAnchor);
        otsRestoreNames();   // scene is stable again — safe to re-show names
        DebugLog("[WASDCombat] dc_cam_restored_after_load");
        return;
    }

    // ONLY the player camera drives the inventory face-cam.  Kenshi calls this
    // hook for OTHER cameras too (the inventory portrait render cameras) — if
    // those touched the detached camera it flickered detach/re-attach EVERY
    // frame (field 2026-06-16: dc_cam_entered/exited toggling).  Non-player
    // cameras just return after orig and never touch s_fpActive/the node.
    if (!ou || !ou->player || thisptr != ou->player->camera)
        return;

    // ANY open UI fully DISABLES the CTRL camera-rotate toggle (user req 2026-06-21
    // — simpler + robust): the moment a menu (dialogue, trade, loot, inventory,
    // pause, anything) is up, turn the toggle OFF and actively release the cursor.
    // The player re-presses CTRL after closing the menu.  `controlEnabled` (the
    // game's own "a UI has focus" flag) is the universal signal; the gui checks are
    // belt-and-suspenders.  s_camRotateUiOpen also gates the poll thread so CTRL+
    // click inside a menu can't re-toggle it.
    // The world MAP and the character STATS window do NOT flip controlEnabled and
    // are not inventory/dialogue/pause — so in FP the cursor stayed pinned to
    // center inside them (field 2026-07-25).  Add them explicitly: the map lives
    // in ManagementScreen (also covers faction/tech/squad tabs), stats via the gui.
    // UNIFY (2026-09-04): single suspend signal shared with FP + the crosshair
    // (dcUiWantsCursor covers map/stats/dialogue/pause/inventory/loot/context-
    // menu); controlEnabled adds the game's own "UI has focus" catch-all.
    // (move-through: the open inventory drops controlEnabled, but the view
    //  must keep looking - dcUiWantsCursor still catches every real UI)
    bool uiOpenNow = (!controlEnabled && !s_invMoveThroughActive) || dcUiWantsCursor();
    // OTS engagement + cursor are handled by the DEDICATED DETACHED CAMERA
    // now (enterOtsCam / otsCamDrive / exitOtsCam, below) — the old
    // engine-rotate (key->rotate) path with its warp / lastMousePos / SendInput
    // flush was removed in the 2026-09-06 rebuild so OTS owns the cursor like
    // FP does.  s_camRotateUiOpen still gates the poll-thread CTRL toggle.
    s_camRotateUiOpen = uiOpenNow;

    // XHAIR-TWEAK (2026-09-06, user goal "just keep the cursor hidden"): drive
    // crosshair + pointer visibility off a STABLE condition — a look-mode is
    // TOGGLED (FP active or CTRL-OTS on) AND no real UI wants the cursor.  It
    // deliberately does NOT key off s_otsRotateEngaged: that flag blips off for
    // a frame during the engine's rotate re-engage (after a UI closes), which
    // let the real pointer flash back on at its last spot = the intermittent
    // "cursor stuck" the user saw.  Tied to the toggle instead, the pointer
    // stays hidden the whole time you're in the mode (except genuine UIs), so
    // any residual engine cursor blip is never visible.  Look-drive still uses
    // its own suspend (wantRotate / fpDriveFrame) independently.
    dcUpdateCrosshair((s_firstPersonActive || s_camRotateToggle) && !dcUiWantsCursor());

    // OTS: drive the DEDICATED DETACHED CAMERA (2026-09-06 rebuild) — its own
    // camera node, own mouse-look, own cursor (like FP), no engine rotate.
    // Engage on the CTRL toggle; otsCamDrive orbits behind the shoulder; exit
    // re-attaches + restores the rig camera.
    {
        bool wantOts = (s_mode == MODE_FREE_MOVE) && s_camRotateToggle
                    && !s_fpActive && !s_firstPersonActive
                    && !s_dcShutdownInProgress;
        if (wantOts)
        {
            if (!s_otsCamActive) enterOtsCam();
            if (s_otsCamActive)
            {
                dcDriveFloorReveal();   // detached camera: drive storey reveal (as FP)
                otsCamDrive(thisptr, uiOpenNow, otsWheelPre);
                if (s_otsCamActive) dcSyncRigYaw(thisptr, s_otsYaw);   // world-map arrow
            }
        }
        else if (s_otsCamActive)
        {
            exitOtsCam(true);
        }
    }

    // Post-face-cam follow watchdog (see s_invFaceRetrackUntilMs): while the
    // window is open, if the camera's orbit pivot has drifted well away from
    // the anchor (follow frozen), do a full stopFollowing+startTrackCharacter
    // reset.  0.5 s throttle keeps the track sound from spamming; the window
    // closes itself in 4 s or the moment tracking is healthy.
    if (s_invFaceRetrackUntilMs > 0 && !s_fpActive)
    {
        ULONGLONG nowRt = GetTickCount64();
        if (nowRt >= s_invFaceRetrackUntilMs
            || s_mode != MODE_FREE_MOVE || !s_freeMoveAnchor)
        {
            s_invFaceRetrackUntilMs = 0;
        }
        else if (s_freeMoveAnchor->movement && thisptr->camera
              && nowRt - s_invFaceRetrackLastMs >= 500)
        {
            s_invFaceRetrackLastMs = nowRt;
            // Probe verdict (2026-09-03): after the face-cam exit the FOLLOW
            // is healthy (pivot tracks the anchor, follow hand correct,
            // camera on the live rig node) but the camera node's PLACEMENT
            // math is asleep — vanilla recomputes it only on input, which is
            // why any keypress "fixed" it.  Detect the real symptom (camera
            // node standing still while the anchor moves) and WAKE the
            // camera with manuallySetOrientationAndZoom — the immediate-
            // apply path the inventory face-cam itself uses — at the
            // camera's own current yaw/pitch/zoom (view direction unchanged).
            static float s_ifwCamX, s_ifwCamY, s_ifwCamZ;
            static float s_ifwAncX, s_ifwAncZ;
            static bool  s_ifwHavePrev = false;
            Ogre::SceneNode* camNodeRt = thisptr->camera->getParentSceneNode();
            Ogre::Vector3 cnRt = camNodeRt ? camNodeRt->_getDerivedPosition()
                                           : Ogre::Vector3(0, 0, 0);
            Ogre::Vector3 apRt = s_freeMoveAnchor->movement->getPosition();
            if (s_ifwHavePrev)
            {
                float cdx = cnRt.x - s_ifwCamX, cdy = cnRt.y - s_ifwCamY,
                      cdz = cnRt.z - s_ifwCamZ;
                float adx = apRt.x - s_ifwAncX, adz = apRt.z - s_ifwAncZ;
                bool camStill   = (cdx*cdx + cdy*cdy + cdz*cdz) < 0.01f;
                bool anchorMoved = (adx*adx + adz*adz) > 1.0f;
                if (camStill && anchorMoved)
                {
                    thisptr->stopFollowing();
                    ou->player->startTrackCharacter(s_freeMoveAnchor);
                    Ogre::Quaternion qRt =
                        Ogre::Quaternion(Ogre::Radian(thisptr->yaw),   Ogre::Vector3::UNIT_Y) *
                        Ogre::Quaternion(Ogre::Radian(thisptr->pitch), Ogre::Vector3::UNIT_X);
                    thisptr->manuallySetOrientationAndZoom(qRt, thisptr->altitude);
                    DebugLog("[WASDCombat] dc_cam_follow_woken");
                }
            }
            s_ifwCamX = cnRt.x; s_ifwCamY = cnRt.y; s_ifwCamZ = cnRt.z;
            s_ifwAncX = apRt.x; s_ifwAncZ = apRt.z;
            s_ifwHavePrev = true;
        }
    }

    // Inventory face-cam trigger: DETACH the player camera when the player's OWN
    // inventory opens (exactly one window), re-attach when it closes.  The
    // detach is what lets the camera face the character — the attached RTS
    // camera can only look top-down.  Safe here (sim paused, character standing)
    // unlike the scrapped gameplay OTS.
    {
        // Only TRUE teardown (shutdown / save-load) exits immediately.  Every
        // other eligibility signal — mode, anchor/movement validity AND the
        // own-inventory window count — is treated as FLAPPY and debounced: those
        // all blip for a frame or two when you switch squad members with the
        // inventory open (the window briefly closes+reopens / the anchor's
        // movement ptr churns), and a single-frame dropout used to slam the
        // streak to max → instant detach (field 2026-06-17: a ~0.1s burst of
        // enter/exit + name hide/restore on every squad switch).  Debouncing
        // the soft signals keeps the camera attached across the swap; it then
        // simply re-frames the newly selected character.
        bool hardStop  = s_dcShutdownInProgress || s_loadGuardActive;
        // Face-cam engages only when enabled in the INI AND the character is NOT
        // in combat — opening inventory mid-fight to loot/disarm an enemy must
        // leave the camera where it is (user req 2026-06-20).  The point-click
        // suppression during inventory is gated separately (s_lootUiSuspendActive),
        // so disabling the face-cam here does NOT let world-clicks move the char.
        // In-combat (with a short grace) is a HARD exit, NOT part of softOK — so a
        // single flicker-false frame can't latch the face-cam through the debounce.
        ULONGLONG nowFC      = GetTickCount64();
        bool inCombatNow     = s_freeMoveAnchor
                            && s_freeMoveAnchor->isInCombatMode(true, true);
        if (inCombatNow) s_lastInCombatMs = nowFC;
        bool inCombatRecent  = inCombatNow
                            || (s_lastInCombatMs > 0
                                && nowFC - s_lastInCombatMs < FACECAM_COMBAT_GRACE_MS);

        bool softOK    = s_settingInventoryFaceCam
                      && s_mode == MODE_FREE_MOVE
                      && s_freeMoveAnchor && s_freeMoveAnchor->movement
                      && isOwnInventoryOpen();

        bool faceCamWanted;
        if (hardStop || inCombatRecent)
        {
            faceCamWanted        = false;
            s_invFaceCloseStreak = INV_FACE_CLOSE_DEBOUNCE;
        }
        else if (softOK)
        {
            faceCamWanted        = true;
            s_invFaceCloseStreak = 0;
        }
        else
        {
            if (s_invFaceCloseStreak < INV_FACE_CLOSE_DEBOUNCE)
                s_invFaceCloseStreak++;
            // Keep the face-cam alive across a brief dropout (squad switch, or a
            // single dissenting per-frame camera call); only let it drop once
            // the close signal has persisted for the full debounce.
            faceCamWanted = s_fpActive && s_invFaceCloseStreak < INV_FACE_CLOSE_DEBOUNCE;
        }

        // First-person owns the detached camera via s_firstPersonActive (NOT
        // s_fpActive).  If the inventory face-cam now wants the camera, SUSPEND
        // first-person (hand the camera back to the rig) and remember to auto-
        // return when the inventory closes.  If nothing wants the face-cam,
        // first-person simply keeps the camera and the enter/exit below is a
        // no-op (s_fpActive stays false while FP owns the view).
        if (s_firstPersonActive && faceCamWanted)
        {
            exitFirstPerson(true);        // clears s_firstPersonActive, reattaches cam
            s_fpSuspendedForInv = true;
        }
        // Detached OTS owns the camera too — hand it back to the rig BEFORE the
        // face-cam detaches, so the face-cam saves the real rig camera state
        // (not OTS's zeroed detached pose, which corrupted the restore).  OTS
        // auto-re-engages via wantOts once the inventory closes.
        if (s_otsCamActive && faceCamWanted)
            exitOtsCam(true);

        if (faceCamWanted && !s_fpActive && !s_firstPersonActive)
            enterOTS();
        else if (!faceCamWanted && s_fpActive)
        {
            exitOTS(true);
            if (s_fpSuspendedForInv)
            {
                s_fpSuspendedForInv = false;
                enterFirstPerson();       // inventory closed — auto-return to FP
            }
            return;
        }
    }

    // Robust FP auto-return (belt-and-suspenders).  The primary path above
    // re-enters first-person when the OTS face-cam closes, but a debounce edge or
    // squad-switch churn can clear s_fpActive on a frame where that branch does
    // not fire, stranding the player in top-down DC with the pending return lost.
    // Whenever FP was suspended for the inventory and the inventory is now closed
    // (nothing else owns the camera), re-enter first-person.
    if (s_fpSuspendedForInv && !s_fpActive && !s_firstPersonActive
        && s_mode == MODE_FREE_MOVE && !s_lootUiSuspendActive
        && s_freeMoveAnchor && s_freeMoveAnchor->movement
        && !isOwnInventoryOpen())
    {
        s_fpSuspendedForInv = false;
        enterFirstPerson();
        return;
    }

    if (!s_fpActive && !s_firstPersonActive)
        return;

    // First-person drive: place the detached camera at the anchor's head-bone
    // eye and look along the mouse view.  Runs INSTEAD of the face-cam path.
    if (s_firstPersonActive)
    {
        dcDriveFloorReveal();
        fpDriveFrame(thisptr, uiOpenNow);
        if (s_firstPersonActive) dcSyncRigYaw(thisptr, s_fpYawSm);   // world-map arrow
        {   // detailed log: is the open world map asking the camera for its facing (FP arrow fix)?
            static ULONGLONG s_mapDiagMs = 0;
            ManagementScreen* ms = ManagementScreen::getSingleton();
            ULONGLONG nowM = GetTickCount64();
            if (ms && ms->getVisible() && nowM - s_mapDiagMs >= 3000)
            {
                s_mapDiagMs = nowM;
                char mb[96];
                sprintf_s(mb, sizeof(mb), "[WASDCombat] dc_map_fp_facing_calls=%ld", InterlockedExchange(&s_mapFacingCalls, 0));
                VerbLog(mb);
            }
        }
        return;
    }

    // Anti scroll-zoom: the wheel still reaches the game's camera zoom inside
    // orig (changing altitude → the world name-tags scale).  Hold the altitude
    // at the value saved on enter so scrolling in the inventory zooms nothing.
    thisptr->altitude = s_otsSavedAltitude;

    // Mouse-look — paused while a UI needs the cursor (context menu visible,
    // pause menu, loot/trade/inventory) or the anchor is KO (free the cursor
    // to pick another squad member).
    bool ctxVisible = ou->player->contextMenu.isVisible();
    bool anchorKO   = s_freeMoveAnchor->isUnconcious();
    // Dialogue / bail-out / conversation UIs need the cursor: free it so the
    // player can click the menu instead of it being pinned to the crosshair
    // (field 2026-06-16, bail-NPC-out menu).  inDialogue covers NPC talk +
    // the prisoner/bail dialogue windows.
    bool inDialogue = (gui && gui->inDialogue());
    bool captureOk  = !s_menuSuspendActive && !ctxVisible
                   && !s_lootUiSuspendActive && !anchorKO && !inDialogue
                   && isKenshiForegroundMain();

    if (captureOk)
    {
        HWND fg = GetForegroundWindow();
        RECT rc;
        if (fg && GetClientRect(fg, &rc))
        {
            POINT center;
            center.x = (rc.left + rc.right) / 2
                     + (LONG)((rc.right - rc.left) * s_otsCrosshairOffsetX);
            center.y = (rc.top + rc.bottom) / 2;
            ClientToScreen(fg, &center);
            POINT cur;
            if (GetCursorPos(&cur))
            {
                if (s_fpCursorCaptured)
                {
                    float dx = (float)(cur.x - center.x);
                    float dy = (float)(cur.y - center.y);
                    if (dx != 0.0f || dy != 0.0f)
                    {
                        s_fpYaw   -= dx * FP_RAD_PER_PIXEL * s_fpSensitivity;
                        s_fpPitch -= dy * FP_RAD_PER_PIXEL * s_fpSensitivity;
                        if (s_fpPitch >  FP_PITCH_LIMIT) s_fpPitch =  FP_PITCH_LIMIT;
                        if (s_fpPitch < -FP_PITCH_LIMIT) s_fpPitch = -FP_PITCH_LIMIT;
                    }
                }
                SetCursorPos(center.x, center.y);
                s_fpCursorCaptured = true;
            }
        }
    }
    else
    {
        s_fpCursorCaptured = false;
    }

    CharMovement* mvFP = s_freeMoveAnchor->movement;


    // Own-inventory face-cam (ported from OTS_Project_Shelved, 2026-06-14 for
    // v1.A).  When the player opens their OWN inventory (no trade/loot party on
    // the other side), swing the shoulder camera around to a centered upper-
    // chest shot facing the selected character so worn gear is visible; restore
    // the previous shoulder view when the window closes.  Runs even under the
    // inventory pause (CameraClass::update still ticks) — which is exactly why
    // the OTS detached-node approach frames cleanly where the vanilla camera
    // could not.  Trade/loot keep the normal over-the-shoulder view.
    {
        // Own inventory (incl. a backpack character's 2-window inventory),
        // excluding any shop/loot/corpse trade.  See isOwnInventoryOpen().
        bool ownInv = isOwnInventoryOpen();
        // Face-cam target = the SELECTED (white-highlighted) character, not the
        // DC anchor: clicking portraits while inventory is open changes the
        // selection, never control.
        Character* invTarget = nullptr;
        if (ownInv)
        {
            // PRIMARY = the character whose inventory window is actually open
            // (inventoryWindowCharacter).  At count==1 this is always one of your
            // own squad, including RECRUITED MOD-NPCs (e.g. Wandering Menders'
            // Kumo) that don't report isPlayerCharacter()==true and so used to
            // fall through to the anchor — leaving the camera framing the wrong
            // character (field 2026-06-17).  Fall back to the selected player
            // char, then the DC anchor.
            Character* invChar = gui ? gui->inventoryWindowCharacter.getCharacter() : nullptr;
            if (invChar && invChar->movement)
                invTarget = invChar;
            else if (s_selectedCharacter && s_selectedCharacter->movement
                     && s_selectedCharacter->isPlayerCharacter())
                invTarget = s_selectedCharacter;
            else
                invTarget = s_freeMoveAnchor;
        }

        if (ownInv && (!s_otsInvFaceActive || s_otsInvFaceChar != invTarget))
        {
            if (!s_otsInvFaceActive)
            {
                // First open: remember the player's shoulder view.
                s_otsSavedYaw   = s_fpYaw;
                s_otsSavedPitch = s_fpPitch;
                s_otsSavedDist  = s_otsDistCur;
                DebugLog("[WASDCombat] dc_cam_inventory_face");
            }
            else
            {
                DebugLog("[WASDCombat] dc_cam_inventory_face_retargeted");
            }
            s_otsInvFaceActive = true;
            s_otsInvFaceChar   = invTarget;

            float faceYaw = s_fpYaw + 3.14159265f;   // fallback: spin around
            if (invTarget && invTarget->movement)
            {
                Ogre::Vector3 bd = invTarget->movement->direction;
                bd.y = 0.0f;
                float bl = bd.length();
                if (bl > 0.001f)
                {
                    bd /= bl;
                    faceYaw = atan2f(-bd.x, -bd.z) + 3.14159265f;
                }
            }
            s_fpYaw      = faceYaw;
            s_fpPitch    = -0.02f;                    // near-level at chest height
            s_otsDistCur = 20.0f;                     // pull in for a tight portrait
        }
        else if (!ownInv && s_otsInvFaceActive)
        {
            s_otsInvFaceActive = false;
            s_otsInvFaceChar   = nullptr;
            s_fpYaw      = s_otsSavedYaw;
            s_fpPitch    = s_otsSavedPitch;
            s_otsDistCur = s_otsSavedDist;
            DebugLog("[WASDCombat] dc_cam_inventory_face_restored");
        }
    }

    // Position the DETACHED camera to face the character's FRONT, recomputed
    // EVERY FRAME from the target's current facing — so it always faces the
    // front regardless of which way the character points, and never drifts to
    // the side on a re-open (the old edge-triggered aim was skipped on re-open
    // and left a stale yaw — field 2026-06-16).  Target = the selected (white)
    // character, falling back to the DC anchor.
    // Same target priority as the face-cam swing above: the OPEN inventory
    // window's character first (covers recruited mod-NPCs that fail
    // isPlayerCharacter()), then the selected player char, then the DC anchor.
    Character* tgt = nullptr;
    {
        Character* invChar = gui ? gui->inventoryWindowCharacter.getCharacter() : nullptr;
        if (invChar && invChar->movement)
            tgt = invChar;
        else if (s_selectedCharacter && s_selectedCharacter->movement
                 && s_selectedCharacter->isPlayerCharacter())
            tgt = s_selectedCharacter;
        else
            tgt = s_freeMoveAnchor;
    }
    if (tgt && tgt->movement && s_fpNode)
    {
        CharMovement* mvP = tgt->movement;
        Ogre::Vector3 bd = mvP->direction;
        bd.y = 0.0f;
        float bl = bd.length();
        float faceYaw = (bl > 0.001f) ? atan2f(bd.x, bd.z) : 0.0f;  // forward=-bd=front
        Ogre::Quaternion q =
            Ogre::Quaternion(Ogre::Radian(faceYaw), Ogre::Vector3::UNIT_Y) *
            Ogre::Quaternion(Ogre::Radian(-0.02f),  Ogre::Vector3::UNIT_X);
        Ogre::Vector3 pivot = mvP->pos;
        pivot.y += 12.5f;                                  // upper chest
        // q*(0,0,dist) is along the character's facing (in FRONT of them).
        Ogre::Vector3 eye = pivot + q * Ogre::Vector3(0.0f, 0.5f, 20.0f);
        s_fpNode->setPosition(eye);
        s_fpNode->setOrientation(q);
    }
}

// CameraClass::restrictPosition hook.  This call is what REFRESHES the interior
// floor visibility (data 2026-06-16: floors render iff restrictPosition runs —
// camFloor/centerBuilding are NOT the lever).  We used to skip it entirely under
// OTS because its camera clamp yanks the over-the-shoulder view back to the
// floor — but skipping it meant the storey you climbed onto never rendered until
// P off.  Now we RUN it (so the floor refreshes) and immediately re-apply the
// OTS pose we wrote this frame, so the clamp can't move the view.
static void (*s_restrictPosOrig)(CameraClass* thisptr, lektor<Character*>& objects);
static void restrictPos_hook(CameraClass* thisptr, lektor<Character*>& objects)
{
    // While the detached inventory face-cam OR first-person is active, skip the
    // RTS clamp — it would yank our free camera back to the floor.  (Interior floor
    // reveal in FP is handled separately by driving updateFloorVisibility from the
    // camera hook, since restrictPosition does not run while FP owns the camera.)
    if (s_fpActive || s_firstPersonActive)
        return;
    s_restrictPosOrig(thisptr, objects);
}

// -----------------------------------------------------------------------
// CharMovement::_NV_update hook — WASD priority with animation protection
//
// WASD held:   apply halt+MOVE_DIRECTION before original; instant-stop on release.
// WASD not held: original runs freely; protected animations are untouched.
// -----------------------------------------------------------------------
static void (*s_charMovUpdateOrig)(CharMovement* thisptr, float time);

// -----------------------------------------------------------------------
// isDoorInteractionTask — door-related TaskTypes.  Used only by the
// hold-scoped door suppression in addJob_hook / addOrder_hook.
// MOVE_CUS_ORDERED is deliberately absent (DC's own disengage orders).
// -----------------------------------------------------------------------
static bool isDoorInteractionTask(TaskType t)
{
    switch (t)
    {
    case OPEN_DOOR:
    case CLOSE_DOOR:
    case OPEN_DOOR_HERE:
    case CLOSE_DOOR_HERE:
    case LOCK_DOOR:
    case UNLOCK_DOOR:
    case LOCK_DOOR_HERE:
    case UNLOCK_DOOR_HERE:
    case PICK_LOCK:
    case BASH_DOOR:
    case MOVE_TO_BUILDING_DOOR:
    case MOVE_TO_CURRENT_LOCATION_BUILDING_DOOR:
    case MOVE_TO_BUILDING_DOOR_INSIDEPOS:
    case MOVE_TO_BUILDING_DOOR_OUTSIDEPOS:
    case OPEN_DOOR_FOR_CURRENT_LOCATION:
    case OPEN_DOOR_FOR_DESTINATION:
        return true;
    default:
        return false;
    }
}

// -----------------------------------------------------------------------
// SLAVE OBEDIENCE (v1.4.1, Workshop/Nexus reports 2026-09: "enslaved in
// Rebirth, you can simply walk out with WASD without punishment").
// While the anchor is an OBEYING slave (Character::isSlave()==IS_SLAVE —
// Rebirth, slaver caravans, ...) the vanilla slave-obedience AI job must own
// locomotion: direct MOVE_DIRECTION injection walked the character straight
// out of the camp with no order the guards could see, and the post-WASD hold
// pinned them against the AI's "get back to work" march — so the game never
// saw a disobeying slave and no punishment ever came.  In this state DC
// yields exactly like the other engine-owned states (downed, protected
// clip, furniture):
//   * no hold clamp (computeHoldDecision reason "slave"),
//   * WASD issues REAL player move orders (the very thing a right-click does,
//     which vanilla punishes) instead of injecting motion — applySlaveMovement
//     — except indoors/caged where an order can only bark: direct injection
//     stays there (the AI still owns the body between presses),
//   * the hold-scoped door-task suppression is off (guards march slaves
//     through gates).
// ESCAPING_SLAVE / EX_SLAVE keep normal direct WASD: once the character has
// broken away the obedience job no longer holds them, and the shackle speed
// cap already keeps a chained runner at a shuffle.
//
// HISTORY: 2026-09-13 the yield was briefly scoped to OUTSIDE the town walls
// (WASD free inside Rebirth); 2026-09-14 the user chose VANILLA PARITY instead
// ("I don't want to touch any of the vanilla game's systems"): the yield
// applies wherever isSlave()==IS_SLAVE, so WASD == right-click everywhere.
// -----------------------------------------------------------------------
static bool isObeyingSlave(Character* ch)
{
    return ch && ch->isSlave() == IS_SLAVE;
}

// slaveEscapeZone — obeying slave, ANYWHERE: the state in which DC yields
// (hold exemption, door suppression off, order-driven WASD).  VANILLA PARITY
// (user 2026-09-14): WASD is treated exactly like a right-click move order in
// every slave situation, so the game's own escape rules decide everything.
static bool slaveEscapeZone(Character* ch)
{
    return isObeyingSlave(ch);
}

// -----------------------------------------------------------------------
// computeHoldDecision — single authority decision for the post-WASD hold,
// shared by charMovUpdate_hook (motion gate) and the end-of-mainLoop
// position clamp.  Returns true when vanilla may move the anchor; false
// means the hold keeps the character where WASD parked them.
//
// reason=no_hold is the normal hybrid state: vanilla owns locomotion
// (point-click works) because no WASD release is pending.  The remaining
// exemptions yield the hold to systems that must move or animate the
// character (mirrors the committed-action philosophy; uses
// isProtectedAnimationState, NOT isCommittedAction, because the door
// interaction Tasker owns STARTUP_STATE and would never release the hold).
// -----------------------------------------------------------------------
static bool computeHoldDecision(Character* ch, const char** outReason)
{
    if (!ch)                           { *outReason = "no_character";   return true; }
    if (!s_wasdHoldActive)             { *outReason = "no_hold";        return true; }
    if (slaveEscapeZone(ch))           { *outReason = "slave";          return true; }
    if (s_playerPointClickActive)      { *outReason = "player_click";   return true; }
    if (s_healingJobActive)            { *outReason = "healing";        return true; }
    if (s_cameraLockTurretSuspend)     { *outReason = "turret";         return true; }
    if (s_menuSuspendActive)           { *outReason = "menu";           return true; }
    if (isDownedButMovable(ch))        { *outReason = "downed";         return true; }
    if (isProtectedAnimationState(ch)) { *outReason = "protected_anim"; return true; }
    if (ch->isInCombatMode(true, true)
        && (!s_postWasdGraceActive || s_combatReentryAllowed))
                                       { *outReason = "combat";         return true; }
    *outReason = "hold";
    return false;
}

// True when issuing a playerMoveOrderDefault on this character could trip the
// vanilla "I can't get out of here" pathfinding bark — the character is inside a
// building/interior OR locked in a cage / imprisoned, where the path to ANY
// destination may run through a locked door.  DC then skips its disengage /
// release move-orders and relies on direct injection + the hold-clamp instead.
// Field 2026-06-20: the bark persisted in a "locked building" because
// isInsideBuildingLoadedInterior() returns false unless the interior is loaded/
// rendered — getBuilding()/isIndoors() catch the building regardless, and
// isPrisonerFreeToGo()==false catches cages/shackles.
static bool moveOrderMayBark(Character* ch)
{
    if (!ch) return false;
    CharMovement* mv = ch->movement;
    // Gate ONLY on the indoors signals — they are all false outdoors, so this never
    // regresses the outdoor click-cancel.  isIndoors() is broader than
    // isInsideBuildingLoadedInterior() (the latter is false unless the interior is
    // loaded/rendered).  isPrisonerFreeToGo() is NOT gated on (its return for a
    // free non-prisoner is unverified; gating could skip the disengage everywhere)
    // — it is only LOGGED at the call site for diagnosis.
    return mv && (mv->isInsideBuildingLoadedInterior() || mv->isIndoors());
}

// dcSnapCancelOrder — the standard "cancel any in-flight path order by re-issuing
// a move to the CURRENT position" used by every WASD release/stop path.  Gated on
// moveOrderMayBark: indoors / locked the order pathfinds and fires the vanilla
// "I can't get out of here" bark, so it is SKIPPED — the surrounding halt() +
// motion-zero + the next-frame hold-clamp park the character without it.  Field
// 2026-06-20: the bark was log-proven to come from these ungated snap sites (the
// press-edge disengage was already gated, but the nudge-tap release, the
// structured release-stop, the menu-suspend stop and the heal-start snap were
// NOT) — DC issues NO disengage order yet still barked.
static void dcSnapCancelOrder(Character* ch)
{
    if (ch && ch->movement && !moveOrderMayBark(ch))
    {
        // Build 63 (user 2026-09-30, "180 flip on stop"): a move order to the
        // exact current position gives the pathing a zero-length path and a
        // degenerate heading, and the character can spin to a fixed direction
        // to "arrive".  Order a point a small step AHEAD along the current
        // facing instead: still a stop, and any facing correction is toward
        // where the character already looks.
        Ogre::Vector3 f = ch->movement->direction; f.y = 0.0f;
        Ogre::Vector3 dest = ch->movement->pos;
        if (f.squaredLength() > 1e-6f) dest += f.normalisedCopy() * 0.75f;
        {
            Ogre::Vector3 m = ch->movement->currentMotion; m.y = 0.0f;
            float dot = (f.squaredLength() > 1e-6f && m.squaredLength() > 1e-6f) ? f.normalisedCopy().dotProduct(m.normalisedCopy()) : 2.0f;
            char sb[96];
            sprintf_s(sb, sizeof(sb), "[WASDCombat] snap_order facing_vs_motion=%.2f", dot);   // DIAG: -1 = moving opposite to facing
            VerbLog(sb);
        }
        ch->playerMoveOrderDefault(nullptr, nullptr, dest);
        s_lastModOrderMs = GetTickCount64();   // DIAG
    }
}

// slaveOrderDriven — obeying slave OUTDOORS: WASD becomes move orders and
// direct injection is off.  Indoors/caged (moveOrderMayBark) the order could
// only bark, so direct injection stays (see isObeyingSlave).
static bool slaveOrderDriven(Character* ch)
{
    return slaveEscapeZone(ch) && !moveOrderMayBark(ch);
}

// applySlaveMovement — WASD for an obeying slave = a point-click 10 m ahead.
// Same persistent-order cadence as the (dormant) downed crawl: re-issue on
// first press, direction change, a 1.5 s refresh, or approach of the last
// dest — never per frame (that restarts pathfinding before it moves).  Runs
// pre-AI (step 5) so the obedience job and the guards see the order this
// frame exactly as they would a player's right-click.
static void applySlaveMovement(bool bW, bool bA, bool bS, bool bD)
{
    if (!s_freeMoveAnchor || !s_freeMoveAnchor->movement) return;
    Ogre::Vector3 dir;
    if (!computeWASDDirection(bW, bA, bS, bD, dir)) return;
    float dlen = dir.length();
    if (dlen < 0.001f) return;
    dir /= dlen;
    Ogre::Vector3 posNow = s_freeMoveAnchor->movement->pos;
    ULONGLONG     now    = GetTickCount64();
    const float hopLen     = 100.0f;   // point-click range, 10 m
    const float approachAt = 60.0f;
    if (s_slaveOrderActive
        && dir.dotProduct(s_slaveLastDir) > 0.95f
        && now - s_slaveLastIssueMs < 1500
        && (s_slaveLastDest - posNow).length() > approachAt)
        return;
    s_slaveLastDir     = dir;
    s_slaveLastIssueMs = now;
    Ogre::Vector3 dest = posNow + dir * hopLen;
    s_slaveLastDest    = dest;
    s_freeMoveAnchor->playerMoveOrderDefault(nullptr, nullptr, dest);
    s_lastModOrderMs   = GetTickCount64();   // DIAG
    s_slaveOrderActive = true;
    if (g_log.debugLogging)
    {
        char buf[160];
        sprintf_s(buf, sizeof(buf),
            "[WASDCombat] dc_slave_wasd_order dir=(%.2f,%.2f) dest=(%.1f,%.1f,%.1f)",
            dir.x, dir.z, dest.x, dest.y, dest.z);
        DebugLog(buf);
    }
}

// -----------------------------------------------------------------------
// AI MOTION FEED (v1.3.2) — feed the anchor's REAL velocity to the enemy AI.
//
// Under MOVE_DIRECTION the engine's speed bookkeeping is bypassed: after
// CharMovement::update the anchor's currentMotion/currentSpeed read ZERO even at
// a full sprint (slope-research empirics 2026-08-05, 459 samples).  The enemy
// combat AI reads exactly those fields on its TARGET —
// CombatMovementController::chasingModeCheck (sprint-chase vs combat-stance
// walk), CharStats::chooseAttack(..., opponentIsStationary) and
// CharMovement::predictNextPosition (attack lead) — so every enemy sees a target
// standing perfectly still that keeps being somewhere else: it drops into slow
// stance-walk positioning, aims with zero lead, and never commits a swing while
// WASD is held.  The moment the player stops, its model matches reality and
// attacks resume — the exact exploit in the Workshop report (2026-08-22):
// "enemies go into a combat animation which allows us to escape any situation".
// This is DC STARVING the AI of data vanilla would provide (Rule 4: investigate
// locomotion interference, don't suppress combat) — not the AI misbehaving.
//
// Fix, each driving frame AFTER the original update has moved the body: write
// the real per-frame velocity (pos delta / dt — self-calibrating at any game
// speed) into currentMotion/currentSpeed so AI readers see the truth.  Before
// the NEXT original call both fields are restored to the engine's expected
// post-update state (zero) so the MOVE_DIRECTION ramp integration never sees
// our values (only matters when wasdAccelerationMultiplier==1.0 — the default
// 1.25 pre-charge overwrites currentMotion pre-orig anyway).  The engine NEVER
// writes currentSpeed in this mode, so every non-driving anchor frame tears the
// feed down — else a stale speed would make enemies see a "moving" target while
// the player stands (the inverse bug).  Stationary (hold/stop) therefore reads
// zero = vanilla-correct: attacks on a stopped player work exactly as before.
// INI [Settings] EnemyPursuit=false reverts (read once at plugin load, like the
// other [Settings] keys — takes effect on the next game launch).
//
// Layout inside the hook: fields are RESTORED to zero at the top of EVERY anchor
// frame (so all early-return sub-paths — hold, furniture detach, downed, clip
// buffer — hand the engine and the AI a truthful stationary state), the position
// history is invalidated on every non-driving frame, and the WRITE happens only
// at the end of the inject path after the original has moved the body.
// -----------------------------------------------------------------------
static void charMovUpdate_hook(CharMovement* thisptr, float time)
{
    // Fast path: non-anchor characters exit immediately with no further work.
    // s_anchorMovement is non-volatile; this single pointer comparison is the
    // only cost for every NPC/guard CharMovement::update call.
    if (thisptr != s_anchorMovement || s_loadGuardActive || s_dcShutdownInProgress)
    {
        if (s_dcShutdownInProgress && !s_hookBlockLoggedCharMov) {
            s_hookBlockLoggedCharMov = true;
            DebugLog("[WASDCombat] dc_hooks_blocked_during_loadgame hook=charMovUpdate"); }
        s_charMovUpdateOrig(thisptr, time);
        return;
    }

    // Anchor character only from this point.  Time DC-specific work from here.
    ScopeTimer _tCM(s_prof_charMove);
    // Continuously cache the anchor's REAL speed tier every frame.  A traveling /
    // save-load frees the controlled character before the load path can read it,
    // so that path used to fall back to RUN — which overwrote the player's speed
    // with a sprint after every load ("traveling load forces sprint", user
    // 2026-07-31).  Caching the true tier here lets the reacquire restore the
    // ACTUAL speed instead of guessing.  Guarded to real tiers (< GROUPED).
    // Build 63: NOT while the player pointer is lost (chunk load) - the game
    // resets the tier during the load and the cache picked that up before the
    // reacquire restored it ("speed drops to walk between chunks", user 2026-09-30).
    if (thisptr->speedOrders < GROUPED && !s_dcPtrLossActive)
        s_dcPreservedSpeedMode = thisptr->speedOrders;

    // AI MOTION FEED restore — hand the engine (and any AI reader on a
    // non-driving frame) the expected post-update state (zero) before ANY
    // anchor path runs the original.  The default pre-charge re-seeds
    // currentMotion further down the inject path exactly as it always has.
    if (s_aiFeedWrote)
    {
        thisptr->currentMotion = Ogre::Vector3::ZERO;
        thisptr->currentSpeed  = 0.0f;
        s_aiFeedWrote = false;
    }
    if (s_aiFeedModePublished)
    {
        thisptr->movementMode      = MOVE_DIRECTION;
        thisptr->officiallyStopped = s_aiFeedSavedStopped;
        s_aiFeedModePublished      = false;
    }
    // Use frame-cached values so no volatile reads are needed inside the per-frame anchor logic.
    bool wasdHeld    = s_frameWasdHeld;
    bool inVMode     = (s_frameMode == MODE_FREE_MOVE);
    bool lootSuspend = s_frameLootSuspend;

    // Combat WASD transition bridge: if we were just driving and a real key was
    // held within COMBAT_WASD_BRIDGE_MS, and the anchor is in combat, treat the
    // current no-key gap as still-driving (using the last direction) so a key
    // roll (W->A->S->D) doesn't hand the body to the AI to square up.  Does NOT
    // refresh s_wasdLastHeldMs (see the held branch), so it self-expires.
    bool combatBridge = false;
    if (!wasdHeld && inVMode && !lootSuspend && s_wasdMovementApplied
        && s_prevWasdDir.squaredLength() > 0.0001f)
    {
        Character* chB = thisptr->getCharacter();
        if (chB && chB->isInCombatMode(true, true) && s_wasdLastHeldMs > 0
            && (GetTickCount64() - s_wasdLastHeldMs) < COMBAT_WASD_BRIDGE_MS)
            combatBridge = true;
    }

    if (!inVMode || (!wasdHeld && !combatBridge) || lootSuspend)
    {
        // Not in WASD-drive mode for the anchor.
        if (inVMode && !wasdHeld && !lootSuspend)
        {
            Character*   chR = thisptr->getCharacter();

            // Instant stop: clear residual WASD motion before original runs.
            // Grace window: hold previous motion for wasdInputGraceMs ms after key release
            // so rapid tap/switch doesn't stutter through a stop cycle.
            if (s_wasdMovementApplied)
            {
                ULONGLONG now     = GetTickCount64();
                ULONGLONG graceMs = g_release.wasdReleaseGraceMs;
                bool inGrace      = (graceMs > 0 &&
                                     s_wasdLastHeldMs > 0 &&
                                     (now - s_wasdLastHeldMs) < graceMs);
                if (!inGrace && !isCommittedAction(chR))
                {
                    Ogre::Vector3 velPre = thisptr->currentMotion;
                    thisptr->halt();
                    thisptr->desiredMotion = Ogre::Vector3::ZERO;
                    thisptr->moveLimit     = 0.0f;
                    if (g_loco.wasdDecelerationMultiplier > 1.0f)
                        thisptr->currentMotion = Ogre::Vector3::ZERO;
                    s_wasdMovementApplied = false;
                    s_prevWasdDir         = Ogre::Vector3::ZERO;
                    // The move-to-current-pos snap cancels any in-flight path
                    // order on release.  OUTDOORS it resolves instantly; INDOORS
                    // playerMoveOrderDefault path-walks the interior network even
                    // to the current position, producing a one-frame step before
                    // the hold-clamp engages = "small movement delay when walking
                    // indoors" (field 2026-06-17).  halt()+zeroed motion above and
                    // the X/Z hold-clamp next frame stop the character cleanly, so
                    // indoors we skip the snap.
                    if (s_freeMoveAnchor && !moveOrderMayBark(s_freeMoveAnchor))
                    {
                        s_freeMoveAnchor->playerMoveOrderDefault(nullptr, nullptr, thisptr->pos);
                        VerbLog("[WASDCombat] wasd_release_preorig_anchor_snap");
                    }
                    (void)velPre;
                }
            }

            // Post-WASD hold: keep the character where WASD parked them.
            // Motion is zeroed before the original runs and the X/Z
            // position is clamped after it (indoor routing writes position
            // late in the frame).  Vanilla locomotion resumes the moment
            // the hold clears (click / WASD / V OFF) or an exemption
            // yields (healing, turret, menu, downed, protected anim,
            // combat).
            const char* holdReason = "";
            if (!computeHoldDecision(chR, &holdReason))
            {
                if (!s_holdPosValid)
                {
                    s_holdPos      = thisptr->pos;
                    s_holdPosValid = true;
                }
                thisptr->halt();
                thisptr->movementMode  = MOVE_DIRECTION;   // path-following ignored
                thisptr->desiredMotion = Ogre::Vector3::ZERO;
                thisptr->moveLimit     = 0.0f;
                thisptr->currentMotion = Ogre::Vector3::ZERO;
                if (!s_idleHoldEngaged)
                {
                    s_idleHoldEngaged = true;
                    VerbLog("[WASDCombat] dc_wasd_hold_engaged");
                }
                s_charMovUpdateOrig(thisptr, time);
                thisptr->pos.x         = s_holdPos.x;
                thisptr->pos.z         = s_holdPos.z;
                thisptr->currentMotion = Ogre::Vector3::ZERO;
                return;
            }
            else if (s_idleHoldEngaged)
            {
                s_idleHoldEngaged = false;
                if (g_log.debugLogging)
                    VerbLog("[WASDCombat] dc_wasd_hold_released");
            }
        }
        s_charMovUpdateOrig(thisptr, time);
        return;
    }

    // WASD held — apply movement override.
    // Skip injection while turret-suspended, menu-suspended, or healing job is active.
    if (s_cameraLockTurretSuspend || s_menuSuspendActive)
    {
        s_charMovUpdateOrig(thisptr, time);
        return;
    }
    if (s_healingJobActive)
    {
        if (!s_medicalJobSuppressedThisHold)
        {
            s_medicalJobSuppressedThisHold = true;
            VerbLog("[WASDCombat] medical_job_blocks_wasd");
        }
        s_charMovUpdateOrig(thisptr, time);
        return;
    }
    // SLAVE OBEDIENCE: an obeying slave outdoors is driven by the pre-AI
    // slave move order (step 5) — vanilla owns the body, no halt(), no
    // setDirectMovement, no post-update re-assert (see isObeyingSlave).
    {
        Character* chS = thisptr->getCharacter();
        if (chS && slaveOrderDriven(chS))
        {
            s_wasdMovementApplied = false;
            s_charMovUpdateOrig(thisptr, time);
            return;
        }
    }
    // Downed/crippled characters OUTDOORS use playerMoveOrderDefault (issued
    // in pre-AI section 5) — halt() + setDirectMovement here would cancel
    // that order.  INDOORS they fall through to the direct-steering branch
    // below, same as standing WASD (orders path-walk interiors).
    {
        Character* chC = thisptr->getCharacter();
        if (chC && downedOrderDriven(chC))
        {
            static ULONGLONG s_crippledChMovTick = 0;
            ULONGLONG nowCC = GetTickCount64();
            if (nowCC - s_crippledChMovTick >= 2000) { s_crippledChMovTick = nowCC;
                VerbLog("[WASDCombat] dc_crippled_state_detected");
                VerbLog("[WASDCombat] dc_crippled_can_move=true");
                VerbLog("[WASDCombat] dc_crippled_using_downed_movement_path"); }

            // Same combat-steering conflict as standing WASD (v1.7.5):
            // lingering combat mode after a fight computes its own
            // movement inside update and fights the crawl order — the
            // downed character stutters and goes nowhere.  Flip the flag
            // for the integration step only; CombatClass::go still sees
            // it in the AI phase.  (A downed character cannot be mid-
            // swing, so no CHOP_WEAPON exclusion is needed here.)
            CombatClass* ccD = chC->getCombatClass();
            bool flippedD = false;
            if (ccD && ccD->combatModeActive)
            {
                ccD->combatModeActive = false;
                flippedD = true;
            }
            s_charMovUpdateOrig(thisptr, time);
            if (flippedD)
            {
                ccD->combatModeActive = true;
                static ULONGLONG s_downedSteerLogTick = 0;
                ULONGLONG nowDS = GetTickCount64();
                if (nowDS - s_downedSteerLogTick >= 1000)
                {
                    s_downedSteerLogTick = nowDS;
                    DebugLog("[WASDCombat] dc_downed_combat_steering_overridden");
                }
            }
            return;
        }
    }
    // Only a REAL key press refreshes the last-held timestamp; a bridge frame
    // must let the bridge window expire (otherwise a single tap would drive
    // forever in combat).
    if (!combatBridge)
        s_wasdLastHeldMs = GetTickCount64();
    s_holdPosValid   = false;  // WASD drives — hold anchor recaptured on next hold

    Ogre::Vector3 wasdDir;
    bool dirOk = computeWASDDirection(s_wHeld, s_aHeld, s_sHeld, s_dHeld, wasdDir);
    // Bridge frame: no live key this instant, reuse the last driven direction so
    // movement carries through the key-roll gap instead of stalling.
    if (!dirOk && combatBridge)
    {
        wasdDir = s_prevWasdDir;
        dirOk   = true;
    }

    // GET UP FROM SEAT / BED / MACHINE (user req 2026-06-22): when the character is
    // anchored to a UseableStuff object (chair / throne / bed / workstation) the body
    // is locked to the node, so setDirectMovement only spins the model in place.
    // Issue ONE player move order toward the held direction — the exact path a
    // point-click takes: it (a) gets the character up with the proper animation, and
    // (b) REPLACES the queued "use object" job so the AI doesn't re-grab the furniture
    // and snap them back when they walk past it again (field 2026-06-22).  Edge-gated
    // (once per sit; re-armed when standing).  Direct WASD injection overrides the
    // order the instant they're standing; the on-release stop cancels any remainder.
    if (dirOk)
    {
        Character* chSeat = thisptr->getCharacter();
        if (chSeat && (isAnchoredToFurniture(chSeat) || chSeat->isCurrentlyGettingUp))
        {
            chSeat->playerWantsMeToGetUp = true;
            // SELF-HEALING re-issue: send the get-up move order at most once per window
            // while anchored — NOT a latched once-per-session flag (that got stuck
            // across squad-switches / job re-sits, so a re-selected character could no
            // longer leave the chair, field 2026-06-22).  Throttled so we don't re-path
            // every frame, but always re-arms for a fresh sit / re-selected character.
            static ULONGLONG s_lastFurnitureExitMs = 0;
            ULONGLONG nowF = GetTickCount64();
            if (isAnchoredToFurniture(chSeat) && (nowF - s_lastFurnitureExitMs) > 600)
            {
                Ogre::Vector3 dest = thisptr->pos + wasdDir * 50.0f;   // ~5m ahead (≈10u/m)
                chSeat->playerMoveOrderDefault(nullptr, nullptr, dest);
                s_lastFurnitureExitMs = nowF;
                DebugLog("[WASDCombat] dc_furniture_exit_move_order");
            }
            s_wasdMovementApplied = false;   // not injecting this frame; let the get-up run
            s_prevWasdDir         = wasdDir;
            static ULONGLONG s_seatGetupTick = 0;
            ULONGLONG nowSG = GetTickCount64();
            if (nowSG - s_seatGetupTick >= 1000) { s_seatGetupTick = nowSG;
                DebugLog("[WASDCombat] dc_wasd_getup_from_furniture"); }
            s_charMovUpdateOrig(thisptr, time);
            return;
        }
    }

    // FINISH-THE-CLIP buffer (user req 2026-06-21, broadened 2026-06-22): Kenshi can't
    // abort an animation clip, so cutting one with movement looks broken/stutters.
    // BUFFER movement while a committed combat clip plays — the character's own swing
    // (windup/strike), a stagger from being hit (STUMBLE), or a parry (REACTION_BLOCK)
    // — and let it finish in place, THEN move.  go() is left running for these states
    // (so the clip completes + the state advances), then combatGo_hook suppresses it,
    // so no new attack chains and movement flows the instant the clip ends.  We touch
    // NO combat state here (no flip, no cut) — exactly one system drives the body, so
    // nothing fights.  DECISION/BLOCK/CIRCLE/WAIT/HESITATE still yield to movement so
    // you can always retreat.
    if (dirOk && isCommittedCombatClip(thisptr->getCharacter()))
    {
        s_wasdMovementApplied = false;   // the clip owns the body; do not inject
        static ULONGLONG s_clipBufTick = 0;
        ULONGLONG nowSB = GetTickCount64();
        if (nowSB - s_clipBufTick >= 1000) { s_clipBufTick = nowSB;
            VerbLog("[WASDCombat] dc_wasd_buffered_combat_clip"); }
        s_charMovUpdateOrig(thisptr, time);
        return;
    }

    if (dirOk)
    {
        // Turn responsiveness: detect direction change and boost limit.
        bool prevHasDir = (s_prevWasdDir.squaredLength() > 0.0001f);
        bool turning    = prevHasDir && (wasdDir.dotProduct(s_prevWasdDir) < 0.9f);
        float limit     = wasdMoveLimit(turning);

        s_wasdMovementApplied = true;
        { ULONGLONG t = GetTickCount64();
          if (t - s_movInjLogTick >= 1000) { s_movInjLogTick = t;
              if (g_log.debugVerbose)
                  DebugLog("[WASDCombat] movement_injection_allowed"); } }
        thisptr->halt();
        thisptr->animationOverride = false;
        thisptr->movementMode      = MOVE_DIRECTION;
        thisptr->setDesiredSpeed(thisptr->speedOrders);
        thisptr->setDirectMovement(wasdDir, limit);

        // Pre-charge currentMotion to jump-start acceleration ramp-up.
        // The boost is a FRACTION of desiredSpeed; for a healthy runner
        // (desiredSpeed ~999) even a 20% fraction is fast, but for an
        // injured/crippled character (desiredSpeed clamped to ~55) the same
        // fraction is a ~11-unit crawl.  In a big fight at low FPS the engine
        // often fails to integrate currentMotion up past the pre-charge that
        // same frame, so the body lurches between full speed and that crawl —
        // the "stutter, especially when injured/crippled" (field diag
        // 2026-06-13: spd=55, cur oscillating 55 -> 10 -> 0).  Floor the
        // pre-charge at the FULL desired velocity so a speed-capped character
        // always gets their (already-reduced) full speed, never a fraction.
        float accel = (g_loco.wasdAccelerationMultiplier - 1.0f)
                    * (turning ? g_loco.wasdTurnResponsiveness : 1.0f);
        if (accel > 0.0f)
        {
            float boost = accel < 1.0f ? accel : 1.0f;
            float preSpeed = thisptr->desiredSpeed * boost;
            // Floor at the FULL desired velocity so a (reduced) capped character
            // still gets their whole speed and doesn't stutter — BUT never above the
            // WASD move-limit.  Flooring at raw desiredSpeed was what set currentMotion
            // to ~999 every frame and let shackled/injured runners outrun everything
            // (the setDirectMovement limit alone didn't bind because this velocity
            // write does).  Clamp the pre-charge to `limit` so the cap actually holds.
            float floorSpeed = thisptr->desiredSpeed;
            if (s_settingWasdSpeedCap && floorSpeed > limit) floorSpeed = limit;
            if (preSpeed < floorSpeed) preSpeed = floorSpeed;
            if (s_settingWasdSpeedCap && preSpeed > limit) preSpeed = limit;
            thisptr->currentMotion = wasdDir * preSpeed;
        }

        s_prevWasdDir = wasdDir;

        // NORMAL WALK WHILE DRIVING (user req 2026-06-22; refined via dc_armsdown_diag):
        // WASD movement must ALWAYS use plain walk/run locomotion — never a combat-ready
        // stance.  The diagnostic proved the arms-down appears while the character is
        // TARGETED (isInCombatMode / red portrait): flipping combatModeActive off only
        // for the integration step was NOT enough — RESTORING it true afterward kept the
        // combat animation layer (and our COMBAT_FINISHED cut) active under
        // MOVE_DIRECTION, so the body stayed in the lowered-weapon "arms-down" pose every
        // frame.  Fix: while WASD is driving, leave combatModeActive CLEARED (do not
        // restore).  This is XP-safe — go() is already suppressed while driving
        // (combatGo_hook), so the character isn't attacking/earning combat XP while you
        // reposition anyway — and the AI re-establishes combat the instant you release
        // WASD (go() re-runs and re-engages).  Result: plain walk even with an enemy on
        // you; combat (and its animations) resume the moment you stop driving.
        Character*   chFlip = thisptr->getCharacter();
        CombatClass* ccFlip = chFlip ? chFlip->getCombatClass() : nullptr;
        if (ccFlip && ccFlip->combatModeActive)
            ccFlip->combatModeActive = false;
        s_charMovUpdateOrig(thisptr, time);


        // Post-original re-assert: the combat AI re-enables animationOverride /
        // flips movementMode away from MOVE_DIRECTION when it wants to drive the
        // body (positioning OR an action animation), which would override the
        // player's WASD movement.  Re-assert MOVE_DIRECTION unconditionally so the
        // player always wins (absolute priority, user req 2026-06-20).
        {
            Character*   chPost = thisptr->getCharacter();
            CombatClass* ccPost = chPost ? chPost->getCombatClass() : nullptr;
            if (ccPost)
            {
                // Re-assert MOVE_DIRECTION so the AI can never drag/circle/square the
                // character while keys are held (absolute priority, user req
                // 2026-06-20).  We do NOT cut the combat state here anymore: the
                // earlier per-frame `combatState = COMBAT_FINISHED` write FOUGHT the
                // combat system and produced the broken/stutter look (field
                // 2026-06-22).  Sliding is already prevented two cleaner ways — committed
                // clips (swing/stagger/parry) are BUFFERED above so they never move, and
                // for every other state combatModeActive is cleared this frame so plain
                // walk plays (no combat clip to slide).  One system drives at a time.
                {
                    thisptr->animationOverride = false;
                    thisptr->movementMode      = MOVE_DIRECTION;
                    thisptr->setDirectMovement(wasdDir, wasdMoveLimit(false));
                }
                if (g_log.debugLogging && ccPost && ccPost->combatModeActive)
                {
                    static ULONGLONG s_xpCombatLogTick = 0;
                    ULONGLONG _tcx = GetTickCount64();
                    if (_tcx - s_xpCombatLogTick >= 1000) { s_xpCombatLogTick = _tcx;
                        DebugLog("[WASDCombat] dc_xp_vanilla_combat_allowed skill=Combat"); }
                }
            }
        }

        // AI MOTION FEED publish (v2 — doc at the feed statics): the original
        // has moved the body; publish the frame-level velocity (computed once
        // per render frame in mainLoop — substep-immune) plus the fleeing-
        // character mode so enemy AI readers (chasingModeCheck / chooseAttack /
        // predictNextPosition) see a normally-moving target.  UNCONDITIONAL on
        // every driven update call: substeps each end published; the top-of-
        // frame restore hands the engine clean state before every original.
        if (s_settingEnemyPursuit)
        {
            thisptr->currentMotion     = s_aiPubVel;
            thisptr->currentSpeed      = s_aiPubSpd;
            s_aiFeedWrote              = true;
            s_aiFeedSavedStopped       = thisptr->officiallyStopped;
            thisptr->movementMode      = MOVE_NORMAL;
            thisptr->officiallyStopped = false;
            s_aiFeedModePublished      = true;
            s_aiPubDroveThisFrame      = true;
            if (g_log.debugLogging)
            {
                ULONGLONG nowAF = GetTickCount64();
                if (nowAF - s_aiFeedLogTick >= 1000) { s_aiFeedLogTick = nowAF;
                    char fbuf[96];
                    sprintf_s(fbuf, sizeof(fbuf),
                        "[WASDCombat] dc_ai_motion_feed spd=%.1f", s_aiPubSpd);
                    DebugLog(fbuf); }
            }
        }
    }
    else
    {
        // Race-case stop: snapshot said WASD held but poll thread released keys before
        // direction could be computed.  Zero stale motion fields before vanilla runs.
        if (s_wasdMovementApplied)
        {
            Character* chRace = thisptr->getCharacter();
            if (!isCommittedAction(chRace))
            {
                thisptr->halt();
                thisptr->desiredMotion = Ogre::Vector3::ZERO;
                thisptr->moveLimit     = 0.0f;
                thisptr->currentMotion = Ogre::Vector3::ZERO;
                s_prevWasdDir          = Ogre::Vector3::ZERO;
                s_wasdMovementApplied  = false;
                VerbLog("[WASDCombat] wasd_release_race_preorig_zeroed");
            }
        }
        s_charMovUpdateOrig(thisptr, time);
    }
}

// -----------------------------------------------------------------------
// MANUAL BLOCK (Build 2) - see the state block by s_settingManualBlock.
// -----------------------------------------------------------------------
static bool manualBlockArmed(Character* self)
{
    return s_settingManualBlock && s_blockHeld
        && !s_dcShutdownInProgress && !s_loadGuardActive
        && s_mode == MODE_FREE_MOVE
        && self && self == s_freeMoveAnchor;
}

// Sync the orders-panel BLOCK checkbox when the panel is showing this character
// (same precedent as the sneak toggle driving the panel's own checkbox).
static void manualBlockSyncPanel(Character* ch, bool on)
{
    OrdersPanel* op = (gui && gui->mainbar) ? gui->mainbar->ordersDataPanel : nullptr;
    if (op && op->blocksCheckbox && op->ordersCharacter.getCharacter() == ch)
        op->blocksCheckbox->setStateSelected(on);
}

// Apply / restore the vanilla DEFENSIVE_COMBAT standing order on the anchor.
static void manualBlockOrder(bool on)
{
    if (on)
    {
        if (s_blockOrderApplied || !s_freeMoveAnchor) return;
        bool prev = s_freeMoveAnchor->getStandingOrder(MessageForB::M_SET_ORDER_DEFENSIVE_COMBAT);
        if (!prev)
        {
            s_freeMoveAnchor->setStandingOrder(MessageForB::M_SET_ORDER_DEFENSIVE_COMBAT, true);
            manualBlockSyncPanel(s_freeMoveAnchor, true);
            s_blockOrderApplied = true;
            s_blockOrderChar    = s_freeMoveAnchor;
        }
        char b[96];
        sprintf_s(b, sizeof(b), "[WASDCombat] block_hold vanilla_order=%s", prev ? "already_on" : "applied");
        VerbLog(b);
    }
    else
    {
        if (!s_blockOrderApplied) { VerbLog("[WASDCombat] block_release"); return; }
        // Only touch the pointer while the world is live and it is still the
        // anchor (or the just-switched-from anchor on a live selection change).
        if (!s_dcShutdownInProgress && !s_loadGuardActive && s_blockOrderChar)
        {
            s_blockOrderChar->setStandingOrder(MessageForB::M_SET_ORDER_DEFENSIVE_COMBAT, false);
            manualBlockSyncPanel(s_blockOrderChar, false);
            VerbLog("[WASDCombat] block_release vanilla_order=restored");
        }
        else
            VerbLog("[WASDCombat] block_release vanilla_order=dropped(load)");
        s_blockOrderApplied = false;
        s_blockOrderChar    = nullptr;
    }
}

static bool manualDodgeCommitLive()
{
    return s_dodgeCommitUntilMs != 0 && GetTickCount64() < s_dodgeCommitUntilMs;
}


static bool verifyPatchSiteBytes(intptr_t addr, const unsigned char* expected, size_t len, const char* name);
static void manualDodgeResolvePlayFn()
{
    if (s_dodgePlayChecked) return;
    s_dodgePlayChecked = true;
    intptr_t base = (intptr_t)GetModuleHandleA(NULL);
    intptr_t fn   = base + (intptr_t)DC_DODGE_PLAY_RVA;
    static const unsigned char PROLOGUE[16] = { 0x40,0x53,0x55,0x57,0x41,0x54,0x48,0x81,0xEC,0xC8,0x00,0x00,0x00,0x48,0xC7,0x44 };
    static const unsigned char ISDODGE_CMP[4] = { 0x80,0x7A,0x2D,0x00 };   // cmp byte ptr [rdx+0x2D],0 at +0x58
    if (verifyPatchSiteBytes(fn, PROLOGUE, 16, "dodge technique-play")
        && verifyPatchSiteBytes(fn + 0x58, ISDODGE_CMP, 4, "dodge technique-play (+0x58)"))
    {
        s_dodgePlayFn = (DcTechPlayFn)fn;
        VerbLog("[WASDCombat] dodge_play_fn verified");
    }
    else
        ErrorLog("WASDCombatPlugin: dodge technique-play function not verified on this exe - manual dodge animation disabled");
}

// Build the mod-owned dodge copies from the vanilla pool (one shot, guarded like
// the dump).  A copy = the technique's bytes + its own std::string for the
// animation name; the impact list / event map internals are shared read-only
// with the original (which lives for the whole process) and never freed.
static void manualDodgeBuildCopies()
{
    if (s_dcDodgeBuilt) return;
    s_dcDodgeBuilt = true;
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    const int*  pCount = (const int*)(base + 0x2010F88);
    CombatTechniqueData* const* const* pArr = (CombatTechniqueData* const* const*)(base + 0x2010F90);
    MEMORY_BASIC_INFORMATION mbi;
    auto readable = [&](const void* a, size_t n) -> bool {
        if (!a) return false;
        if (VirtualQuery(a, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_NOACCESS) || (mbi.Protect & PAGE_GUARD)) return false;
        return ((uintptr_t)a + n) <= ((uintptr_t)mbi.BaseAddress + mbi.RegionSize);
    };
    if (!readable(pCount, 4) || !readable(pArr, 8)) { DebugLog("[WASDCombat] dodge_copies_unreadable"); return; }
    int cnt = *pCount;
    CombatTechniqueData* const* arr = *pArr;
    if (cnt <= 0 || cnt > 400 || !readable(arr, (size_t)cnt * 8)) { DebugLog("[WASDCombat] dodge_copies_insane"); return; }
    const size_t objBytes = 0x100;   // > sizeof on 1.0.65 (impactPoints lektor ends ~0xC8)
    for (int i = 0; i < cnt; ++i)
    {
        CombatTechniqueData* t = arr[i];
        if (!readable(t, objBytes)) continue;
        bool isD = t->isDodge, isB = t->isBlock && !t->isDodge;
        if ((isD && s_dcDodgeCount >= 8) || (isB && s_dcBlockCount >= 8) || (!isD && !isB)) continue;
        void* mem = malloc(objBytes + 64);
        if (!mem) break;
        memcpy(mem, t, objBytes);
        CombatTechniqueData* c = (CombatTechniqueData*)mem;
        new (&c->animation) std::string(t->animation);   // own name string (deep copy)
        char b[160];
        if (isD)
        {
            DcDodgeCopy& d = s_dcDodge[s_dcDodgeCount++];
            d.tech = c; d.stumbleOnly = t->stumbleDodge; d.weight = t->chanceMult > 0.0f ? t->chanceMult : 1.0f;
            sprintf_s(b, sizeof(b), "[WASDCombat] dodge_copy[%d] anim=%s stumbleOnly=%d weight=%.2f", s_dcDodgeCount - 1,
                      c->animation.c_str(), d.stumbleOnly ? 1 : 0, d.weight);
        }
        else
        {
            s_dcBlock[s_dcBlockCount++] = c;
            sprintf_s(b, sizeof(b), "[WASDCombat] block_copy[%d] anim=%s dir0=%d", s_dcBlockCount - 1,
                      c->animation.c_str(), c->numImpactPoints() > 0 ? (int)c->impactPoint(0)->direction : -1);
        }
        VerbLog(b);
    }
}

// Pick one of our copies, weighted: the normal dodges when standing, the
// stumble dodges (dodgefly / dodgeback-2) while staggered - same split the AI
// uses (stumbleDodge must match isStumbling in the vanilla chooser).
static CombatTechniqueData* manualDodgePick(bool stumbling)
{
    float total = 0.0f;
    for (int i = 0; i < s_dcDodgeCount; ++i) if (s_dcDodge[i].stumbleOnly == stumbling) total += s_dcDodge[i].weight;
    if (total <= 0.0f) return nullptr;
    float r = UtilityT::random() * total, acc = 0.0f;
    CombatTechniqueData* last = nullptr;
    for (int i = 0; i < s_dcDodgeCount; ++i)
    {
        if (s_dcDodge[i].stumbleOnly != stumbling) continue;
        last = s_dcDodge[i].tech; acc += s_dcDodge[i].weight;
        if (r <= acc) return last;
    }
    return last;
}

// Staggered = the physical states (KO, playing dead, down, getting up) or the
// combat STUMBLE state while combat is live (a stale STUMBLE after a fight is
// not a stagger - see isCommittedAction).
static bool manualStaggered(Character* ch)
{
    if (!ch) return false;
    ProneState prone = ch->getProneState();
    if (prone == PS_KO || prone == PS_PLAYING_DEAD || ch->isDown() || ch->isCurrentlyGettingUp) return true;
    CombatClass* cc = ch->getCombatClass();
    return cc && cc->combatModeActive && cc->getCombatState() == STUMBLE;
}
// Once per frame (from manualBlockTick): stagger edges.  Start = pending
// presses die (nothing buffers across a stagger; a dodge / swing already in
// flight keeps its own bookkeeping).  End = inputs count again; a stagger
// dodge that never found the STUMBLE state is dropped with it.
static void manualStaggerTick()
{
    bool stag = s_mode == MODE_FREE_MOVE && s_freeMoveAnchor && !s_dcShutdownInProgress
             && !s_loadGuardActive && manualStaggered(s_freeMoveAnchor);
    ULONGLONG now = GetTickCount64();
    if (stag && s_staggerRawSinceMs == 0) s_staggerRawSinceMs = now;
    if (!stag) s_staggerRawSinceMs = 0;
    // Debounced: a stagger counts once the signal has held STAGGER_DEBOUNCE_MS
    // (the stumble state flickers for a frame or two on some hits); the long-
    // stagger clock still runs from the real edge.
    if (stag && s_staggerStartMs == 0 && (now - s_staggerRawSinceMs) >= STAGGER_DEBOUNCE_MS)
    {
        s_staggerStartMs   = s_staggerRawSinceMs;
        int dropped = 0;
        if (!s_attackActive && s_attackCommitUntilMs != 0) { s_attackCommitUntilMs = 0; ++dropped; }
        if (!s_dodgeActiveTech && s_dodgeCommitUntilMs != 0) { s_dodgeCommitUntilMs = 0; ++dropped; }
        char b[96];
        sprintf_s(b, sizeof(b), "[WASDCombat] stagger_start pending_dropped=%d", dropped);
        VerbLog(b);
    }
    else if (!stag && s_staggerStartMs != 0)
    {
        char b[96];
        sprintf_s(b, sizeof(b), "[WASDCombat] stagger_end ms=%llu", now - s_staggerStartMs);
        VerbLog(b);
        s_staggerStartMs = 0;
    }
}

// Main thread, once per frame: turn a fresh dodge press into a commit window.
static void manualDodgePressTick()
{
    ULONGLONG press = s_dodgePressMs;
    if (press == 0 || press == s_dodgeSeenPressMs) return;
    s_dodgeSeenPressMs = press;
    manualDodgeBuildCopies();   // one shot
    manualDodgeResolvePlayFn(); // one shot
    ULONGLONG now = GetTickCount64();
    const char* why = nullptr;
    if (!s_settingManualBlock || s_mode != MODE_FREE_MOVE || !s_freeMoveAnchor
        || s_dcShutdownInProgress || s_loadGuardActive)          why = "not_dc";
    else if (!s_manualAttackOn)                                   why = "manual_off";  // Q is a manual-combat control (user 2026-09-29)
    else if (s_staggerStartMs != 0)                               why = "staggered";   // dropped, never buffered
    else if (s_dodgeActiveTech)                                   why = "in_flight";   // one dodge until it finishes
    else if (now < s_dodgeCooldownUntilMs)                        why = "cooldown";
    else if (now < s_dodgeRecoveryUntilMs)                        why = "recovery";    // last dodge's follow-through
    else if (s_dodgeAcceptedPressMs != 0 && press >= s_dodgeAcceptedPressMs
             && press - s_dodgeAcceptedPressMs < (ULONGLONG)s_inputRepeatGuardMs) why = "repeat";  // accidental double-tap
    else if (!s_freeMoveAnchor->isInCombatMode(true, true)
             && (s_lastInCombatMs == 0 || now - s_lastInCombatMs > 1500)) why = "no_combat";
    if (why)
    {
        char b[96];
        sprintf_s(b, sizeof(b), "[WASDCombat] dodge_press_ignored reason=%s", why);
        VerbLog(b);
        return;
    }
    s_dodgeCommitUntilMs   = press + (ULONGLONG)s_dodgeWindowMs;
    s_dodgeCooldownUntilMs = now + (ULONGLONG)s_dodgeCooldownMs;   // a wasted press costs the cooldown too
    s_dodgeAcceptedPressMs = press;
    char b[96];
    sprintf_s(b, sizeof(b), "[WASDCombat] dodge_commit window=%.0f moving=%d",
              s_dodgeWindowMs, (s_wHeld || s_aHeld || s_sHeld || s_dHeld) ? 1 : 0);
    VerbLog(b);
}

// Is this Character* still a live, loaded, hostile target?  (identity pointer -
// never trusted without this check; Rule 5)
// Main thread, once per frame: the Z toggle edge.
static void manualAttackTick()
{
    ULONGLONG mp = s_manualPressMs;
    if (mp != 0 && mp != s_manualSeenPressMs)
    {
        s_manualSeenPressMs = mp;
        if (s_mode == MODE_FREE_MOVE && s_freeMoveAnchor && !s_loadGuardActive)
        {
            s_manualAttackOn = !s_manualAttackOn;
            DebugLog(s_manualAttackOn ? "[WASDCombat] manual_attack_on" : "[WASDCombat] manual_attack_off");
        }
    }
    if (s_manualAttackOn && (s_mode != MODE_FREE_MOVE || !s_freeMoveAnchor))
    {
        s_manualAttackOn = false;
        DebugLog("[WASDCombat] manual_attack_off (dc_off)");
    }
}

// Resolve a KenshiLib export to the REAL game address: the export is a
// `jmp [rip+slot]` stub whose slot KenshiLib fills at start-up for the running
// exe version (the same table GetRealAddress uses) - so this stays version-
// safe without a header declaration.
static intptr_t dcResolveKenshiLibExport(const char* mangled)
{
    HMODULE h = GetModuleHandleA("KenshiLib.dll");
    if (!h) return 0;
    unsigned char* stub = (unsigned char*)GetProcAddress(h, mangled);
    if (!stub) return 0;
    if (stub[0] == 0xFF && stub[1] == 0x25)
    {
        int disp = *(int*)(stub + 2);
        void** slot = (void**)(stub + 6 + disp);
        return (intptr_t)*slot;
    }
    return (intptr_t)stub;
}

// AttackState::_NV_initialise(state) - the ONE place a swing starts (state+8 =
// CombatClass*).  In focus mode, the AI's own swings are denied here: the
// initialiser reports failure exactly as it does when the target is out of
// reach, and no technique is chosen or played.  Our E sets s_attackActive
// first, so its swing passes.  Everything else forwards untouched.
static bool (*s_attackInitOrig)(void* state);
static bool attackInit_hook(void* state)
{
    CombatClass* cc = state ? *(CombatClass**)((char*)state + 8) : nullptr;
    if (cc && s_manualAttackOn && !s_attackActive && s_mode == MODE_FREE_MOVE
        && !s_dcShutdownInProgress && !s_loadGuardActive && cc->me == s_freeMoveAnchor)
    {
        ULONGLONG nowD = GetTickCount64();
        if (nowD - s_manualSuppressLogMs >= 1000)
        {
            s_manualSuppressLogMs = nowD;
            char db[96];
            sprintf_s(db, sizeof(db), "[WASDCombat] manual_ai_attack_denied from_state=%d pending=%d dodging=%d",
                      s_anchorPrevState, manualAttackCommitLive() ? 1 : 0, s_dodgeActiveTech ? 1 : 0);
            VerbLog(db);
        }
        return false;
    }
    return s_attackInitOrig(state);
}

// CombatClass::setCombatState (protected; member pointer via the explicit-
// instantiation loophole) - every state transition the combat code makes goes
// through here.  Second choke point: in focus mode, an attempt to put the
// controlled character into CHOP_WEAPON that is not our own swing becomes
// WAIT_MENACINGLY (the armed ready hold).
template<typename Tag> struct DcRobResult { typedef typename Tag::type type; static type ptr; };
template<typename Tag> typename DcRobResult<Tag>::type DcRobResult<Tag>::ptr;
template<typename Tag, typename Tag::type P> struct DcRob : DcRobResult<Tag>
{
    struct Filler { Filler() { DcRobResult<Tag>::ptr = P; } };
    static Filler filler;
};
template<typename Tag, typename Tag::type P> typename DcRob<Tag, P>::Filler DcRob<Tag, P>::filler;
struct DcSetCombatStateTag { typedef void (CombatClass::*type)(swordStateEnum); };
template struct DcRob<DcSetCombatStateTag, &CombatClass::setCombatState>;

static void (*s_setCombatStateOrig)(CombatClass* cc, swordStateEnum state);
static void setCombatState_hook(CombatClass* cc, swordStateEnum state)
{
    if (state == CHOP_WEAPON && cc && s_manualAttackOn && !s_attackActive && s_mode == MODE_FREE_MOVE
        && !s_dcShutdownInProgress && !s_loadGuardActive && cc->me == s_freeMoveAnchor)
    {
        VerbLog("[WASDCombat] manual_ai_attack_state_denied");
        state = WAIT_MENACINGLY;
    }
    s_setCombatStateOrig(cc, state);
}

// CharStats::chooseBlock - the AI's defence choice.  In manual-attack mode the
// controlled character gets NO automatic block or dodge: the chooser returns
// nothing (exactly what a failed block roll returns) and the hit lands.
// Exceptions: a G block the player asked for (accepted press, within its hold
// or perfect window), and a stagger in progress (the stumble reaction stays
// vanilla).  Our own Q dodge and G pose never use the chooser.
static bool focusTargetValid(Character* t);   // fwd decls (lock-on helpers below)
static bool focusLockLive();
static CombatTechniqueData* (*s_chooseBlockOrig)(CharStats* st, CutDirection dir, float opponentAttackSkill,
                                                 CutOrigination from, Character* opponent);
static ULONGLONG s_manualBlockDeniedLogMs = 0;
static CombatTechniqueData* chooseBlock_hook(CharStats* st, CutDirection dir, float opponentAttackSkill,
                                             CutOrigination from, Character* opponent)
{
    ULONGLONG nowC = GetTickCount64();
    bool blockWanted = s_blockAcceptedPressMs != 0 && s_blockPressMs == s_blockAcceptedPressMs
                    && s_blockHoldStartMs != 0
                    && ((s_blockHeld && (nowC - s_blockHoldStartMs) <= (ULONGLONG)s_blockHoldMs)
                        || (nowC - s_blockHoldStartMs) <= (ULONGLONG)s_perfectWindowMs);
    // AiBlocksInManual (default ON since build 48): the chooser stays vanilla, so
    // the AI still blocks/dodges on its own with Z on; the denial below only runs
    // when the user wants a fully manual defence.  (Log 2026-09-21: 293 denied
    // blocks = 293 aborted block entries - a twitch and a stagger each.)
    if (s_manualAttackOn && !s_settingAiBlocks && !blockWanted && s_mode == MODE_FREE_MOVE
        && st && st->me && st->me == s_freeMoveAnchor && !s_dcShutdownInProgress && !s_loadGuardActive)
    {
        CombatClass* cc = st->me->getCombatClass();
        bool stumbling = cc && cc->combatModeActive && cc->getCombatState() == STUMBLE;
        if (!stumbling)
        {
            if (nowC - s_manualBlockDeniedLogMs >= 1000)
            {
                s_manualBlockDeniedLogMs = nowC;
                VerbLog("[WASDCombat] manual_ai_block_denied");
            }
            return nullptr;
        }
    }
    return s_chooseBlockOrig(st, dir, opponentAttackSkill, from, opponent);
}

// AIM FOCUS feeds the AI's "who": CombatClassAI::chooseAttackTarget (facing,
// approach, swings) answers the aim focus for the controlled character while
// one is live; threats (blocks, dodges, stumble reactions) stay vanilla so an
// attack from anyone still gets its reaction.
static bool focusLockLive()
{
    return s_lockTarget && s_mode == MODE_FREE_MOVE && !s_dcShutdownInProgress && !s_loadGuardActive
        && focusTargetValid(s_lockTarget);
}
static Character* (*s_chooseAttackTargetOrig)(CombatClass* cc);
static Character* chooseAttackTarget_hook(CombatClass* cc)
{
    if (cc && cc->me == s_freeMoveAnchor && focusLockLive()) return s_lockTarget;
    return s_chooseAttackTargetOrig(cc);
}

static bool manualAttackCommitLive()
{
    return s_attackCommitUntilMs != 0 && GetTickCount64() < s_attackCommitUntilMs;
}

// Main thread, once per frame: a fresh Attack press becomes a commit window.
static void manualAttackPressTick()
{
    ULONGLONG press = s_attackPressMs;
    if (press == 0 || press == s_attackSeenPressMs) return;
    s_attackSeenPressMs = press;
    ULONGLONG now = GetTickCount64();
    const char* why = nullptr;
    if (!s_settingManualBlock || s_mode != MODE_FREE_MOVE || !s_freeMoveAnchor
        || s_dcShutdownInProgress || s_loadGuardActive)          why = "not_dc";
    else if (!s_manualAttackOn)                                   why = "manual_off";  // E is a manual-attack control
    else if (s_staggerStartMs != 0)                               why = "staggered";   // dropped, never buffered
    else if (s_attackActive)                                      why = "in_flight";   // one swing until it finishes
    else if (now < s_attackRecoveryUntilMs)                       why = "recovery";    // last swing's follow-through
    else if (s_attackAcceptedPressMs != 0 && press >= s_attackAcceptedPressMs
             && press - s_attackAcceptedPressMs < (ULONGLONG)s_inputRepeatGuardMs) why = "repeat";  // accidental double-tap
    else if (s_dodgeActiveTech)                                 { why = "dodging_buffered"; s_attackAfterDodgeMs = press; }   // dodge-then-strike
    else if (!s_freeMoveAnchor->isInCombatMode(true, true)
             && (s_lastInCombatMs == 0 || now - s_lastInCombatMs > 1500)) why = "no_combat";
    if (why)
    {
        char b[96];
        sprintf_s(b, sizeof(b), "[WASDCombat] attack_press_ignored reason=%s", why);
        VerbLog(b);
        return;
    }
    s_attackCommitUntilMs   = press + (ULONGLONG)s_attackWindowMs;
    s_attackAcceptedPressMs = press;
    char b[96];
    sprintf_s(b, sizeof(b), "[WASDCombat] attack_commit window=%.0f moving=%d",
              s_attackWindowMs, (s_wHeld || s_aHeld || s_sHeld || s_dHeld) ? 1 : 0);
    VerbLog(b);
}

// Once per frame from mainLoop: hold/release edges drive the vanilla order.
static void manualBlockTick()
{
    // WORLD-LIVE GATE (crash 2026-09-21, dump: AV in manualStaggered +0x20 =
    // the virtual getProneState on a FREED anchor, 0.4 s into a quickload).
    // This tick runs at the top of mainLoop, BEFORE its safety gate, so it must
    // never touch the anchor unless the world is live: on a save-load the game
    // nulls ou->player first and frees the characters a few frames later, and
    // the flag may lag behind - both signals are required here.  Nothing in
    // this tick needs to run during a load (the gate + clearAllState reset the
    // manual-combat state) - drop out whole.
    if (!ou || !ou->player || ou->isLoadingFromASaveGame() || s_dcPtrLossActive) return;   // build 74: no anchor deref while ANY load flag is up
    manualAttackTick();
    manualAimTick();
    manualStaggerTick();
    manualDodgePressTick();
    manualAttackPressTick();
    // "Held" = key down OR inside the perfect-block window after a press, so a
    // tap keeps vanilla's BLOCK order (and its visible +20 melee defence) on for
    // exactly the timing window - the HUD shows when the window is live.
    // Key-follow (user 2026-09-18): the BLOCK order (and its "(+20)") is on
    // exactly while the key is down; the perfect window itself is timed from
    // the press in the verdict hook and does not need the key held.
    // Timed block: a fresh press is accepted only off cooldown; the block then
    // lasts while the key is down AND within BlockHoldMs of that press.
    ULONGLONG nowT = GetTickCount64();
    ULONGLONG pressT = s_blockPressMs;
    if (pressT != 0 && pressT != s_blockSeenPressMs)
    {
        s_blockSeenPressMs = pressT;
        // RESUME (input forgiveness): the block is still live (release + press
        // fell inside one frame) or was let go within the repeat guard, and the
        // hold budget is unspent -> the SAME block continues: no cooldown, the
        // budget and the perfect window still run from the first press, and the
        // pose is not re-injected.  A fumbled double-tap no longer drops the
        // guard for BlockCooldownMs (the "got hit while I was blocking" case).
        bool budgetLeft = s_blockHoldStartMs != 0 && pressT >= s_blockHoldStartMs
                       && (pressT - s_blockHoldStartMs) < (ULONGLONG)s_blockHoldMs;
        bool resume = budgetLeft
                   && (s_blockHeldPrev
                       || (s_blockReleaseMs != 0 && pressT >= s_blockReleaseMs
                           && (pressT - s_blockReleaseMs) <= (ULONGLONG)s_inputRepeatGuardMs));
        if (!s_manualAttackOn)
            VerbLog("[WASDCombat] block_press_ignored reason=manual_off");  // G is a manual-combat control (user 2026-09-29)
        else if (s_staggerStartMs != 0)
            VerbLog("[WASDCombat] block_press_ignored reason=staggered");   // dropped, never buffered
        else if (nowT < s_perfectRewardUntilMs)
        {
            // Perfect-block reward: a FRESH block (new hold budget + new perfect
            // window) even inside the resume / cooldown windows.  Spends it.
            s_blockAcceptedPressMs = pressT;
            s_blockHoldStartMs     = pressT;
            s_perfectRewardUntilMs = 0;
            VerbLog("[WASDCombat] block_press_reward");
        }
        else if (resume)
        {
            s_blockAcceptedPressMs = pressT;
            s_blockInjectedPressMs = pressT;    // keep the pose of the first press
            VerbLog("[WASDCombat] block_press_resumed");
        }
        else if (nowT < s_blockCooldownUntilMs)
            VerbLog("[WASDCombat] block_press_ignored reason=cooldown");
        else
        {
            s_blockAcceptedPressMs = pressT;
            s_blockHoldStartMs     = pressT;
        }
        s_blockReleaseMs = 0;
    }
    bool pressLive = s_blockAcceptedPressMs != 0 && s_blockPressMs == s_blockAcceptedPressMs
                  && s_blockHoldStartMs != 0
                  && (nowT - s_blockHoldStartMs) <= (ULONGLONG)s_blockHoldMs;
    bool held = s_settingManualBlock && s_manualAttackOn && s_blockHeld && pressLive && s_mode == MODE_FREE_MOVE
             && !s_dcShutdownInProgress && !s_loadGuardActive && s_freeMoveAnchor;
    // Staggered: the BLOCK order is not applied (anti-cheese) - it comes on
    // the moment the stagger ends if the key is still down.
    if (held)
    {
        CombatClass* ccH = s_freeMoveAnchor->getCombatClass();
        if (ccH && ccH->combatModeActive && ccH->getCombatState() == STUMBLE) held = false;
    }
    // Anchor switched (F) while held: restore the old one, re-apply on the new.
    if (held && s_blockOrderApplied && s_blockOrderChar != s_freeMoveAnchor)
    {
        manualBlockOrder(false);
        s_blockHeldPrev = false;
    }
    if (held != s_blockHeldPrev)
    {
        s_blockHeldPrev = held;
        manualBlockOrder(held);
        if (!held)
        {
            s_blockCooldownUntilMs = (nowT < s_perfectRewardUntilMs) ? 0     // perfect reward: no cooldown
                                   : nowT + (ULONGLONG)s_blockCooldownMs;   // released or expired
            s_blockReleaseMs       = nowT;                                   // a re-press inside the repeat guard resumes
        }
    }
}

// --- MANUAL DODGE helpers ------------------------------------------------
static bool manualDodgeArmed(Character* self)
{
    return s_settingManualBlock
        && !s_dcShutdownInProgress && !s_loadGuardActive
        && s_mode == MODE_FREE_MOVE
        && self && self == s_freeMoveAnchor;
}

// Inject the dodge, once per press, from the combat-AI hook on the first idle
// combat frame inside the commit window.  Writes exactly what the game's own
// changeState + initialiseBlock write; blockState does the rest.
// Pick a mod-owned block copy for the pose.  If the foe is mid-swing, match its
// current impact direction converted into our frame (what the vanilla chooser
// matches a block against); otherwise any block.
static CombatTechniqueData* manualBlockPick(Character* me, Character* foe, int* wantOut)
{
    if (s_dcBlockCount == 0) return nullptr;
    int want = -1;
    CombatClass* fc = foe ? foe->getCombatClass() : nullptr;
    CombatTechniqueData* ft = fc ? fc->currentTechnique : nullptr;
    if (fc && fc->combatModeActive && ft && !ft->isBlock && ft->numImpactPoints() > 0)
    {
        int n = ft->numImpactPoints();
        int sec = fc->currentComboSection; if (sec < 0) sec = 0; if (sec >= n) sec = n - 1;
        CombatTechniqueData::ImpactPoint* ip = ft->impactPoint(sec);
        if (ip) want = (int)me->convertCutDirection(ip->direction, foe);
    }
    if (wantOut) *wantOut = want;
    if (want >= 0)
        for (int i = 0; i < s_dcBlockCount; ++i)
        {
            CombatTechniqueData* c = s_dcBlock[i];
            if (c->numImpactPoints() > 0 && (int)c->impactPoint(0)->direction == want) return c;
        }
    int idx = (int)(UtilityT::random() * (float)s_dcBlockCount); if (idx >= s_dcBlockCount) idx = s_dcBlockCount - 1;
    return s_dcBlock[idx];
}

// --- lock-on + directional aim ------------------------------------------------------
static bool focusTargetLoaded(Character* t)
{
    if (!t || !s_gwFrame) return false;
    auto& chars = s_gwFrame->getCharacterUpdateList();
    for (auto it = chars.begin(); it != chars.end(); ++it) if (*it == t) return true;
    return false;
}
static bool focusTargetValid(Character* t)
{
    return focusTargetLoaded(t) && s_freeMoveAnchor && !t->isDead() && !t->isUnconcious()
        && t->isEnemy(s_freeMoveAnchor, true);
}

// The view yaw the player is aiming with (the mod's own look state - the
// camera object's orientation is not reliable frame to frame).  false = no
// aimed view (plain DC camera).
static bool manualAimYaw(float& yawOut)
{
    if (s_firstPersonActive) { yawOut = s_fpYawSm; return true; }
    if (s_otsCamActive)      { yawOut = s_otsYaw;  return true; }
    return false;
}

// Directional aim: the live hostile nearest to the view ray within reach and
// inside the aim cone (angle first, distance second).  nullptr = nobody aimed at.
static Character* manualAimPick(Character* me, float range)
{
    float yaw;
    if (!me || !me->movement || !s_gwFrame || !manualAimYaw(yaw)) return nullptr;
    const Ogre::Vector3 mp = me->movement->pos;
    const float fx = -sinf(yaw), fz = -cosf(yaw);           // view forward (fwd = (-sin, 0, -cos))
    const float cosCone = cosf(s_aimConeDeg * 0.0174533f);
    const float maxR2 = range * range;
    Character* best = nullptr; float bestScore = 1e9f;
    auto& chars = s_gwFrame->getCharacterUpdateList();
    for (auto it = chars.begin(); it != chars.end(); ++it)
    {
        Character* c = *it;
        if (!c || c == me || !c->movement) continue;
        float dx = c->movement->pos.x - mp.x, dz = c->movement->pos.z - mp.z;
        float d2 = dx * dx + dz * dz;
        if (d2 < 1e-4f || d2 >= maxR2) continue;
        float d = sqrtf(d2), cosA = (dx * fx + dz * fz) / d;
        if (cosA < cosCone) continue;
        if (c->isDead() || c->isUnconcious() || c->isDown() || !c->isEnemy(me, true)) continue;   // downed = not attackable
        float score = (1.0f - cosA) * 100.0f + d * 0.05f;   // angle first, distance second
        if (score < bestScore) { bestScore = score; best = c; }
    }
    return best;
}

// The foe every manual action uses: the LOCKED target, else whoever the view is
// aimed at.  nullptr lets the caller fall back to the AI's own target.
static Character* manualFoeResolve(CombatClass* cc, Character* me)
{
    if (s_lockTarget && focusTargetValid(s_lockTarget)) return s_lockTarget;
    return manualAimPick(me, s_aimRange);
}

// Aim focus with a FREE cursor: the live hostile whose chest projects nearest
// to the mouse cursor, within AimHoverPx on screen and AimRange in the world.
static Character* manualAimHoverPick(Character* me, Ogre::Camera* cam, float radiusPx, float range)
{
    if (!me || !me->movement || !s_gwFrame || !cam) return nullptr;
    MyGUI::InputManager* im = MyGUI::InputManager::getInstancePtr();
    if (!im) return nullptr;
    MyGUI::IntPoint mp = im->getMousePosition();
    const Ogre::Vector3 me3 = me->movement->pos;
    const float maxR2 = range * range;
    Character* best = nullptr; float bestD2 = radiusPx * radiusPx;
    auto& chars = s_gwFrame->getCharacterUpdateList();
    for (auto it = chars.begin(); it != chars.end(); ++it)
    {
        Character* c = *it;
        if (!c || c == me || !c->movement) continue;
        float dx = c->movement->pos.x - me3.x, dz = c->movement->pos.z - me3.z;
        if (dx * dx + dz * dz >= maxR2) continue;
        if (c->isDead() || c->isUnconcious() || c->isDown() || !c->isEnemy(me, true)) continue;
        Ogre::Vector3 chest; int sx, sy;
        if (!focusBonePos(c, "Bip01 Spine1", 11.0f, chest) || !focusProject(cam, chest, 0.0f, sx, sy)) continue;
        float px = (float)(sx - mp.left), py = (float)(sy - mp.top);
        float d2 = px * px + py * py;
        if (d2 < bestD2) { bestD2 = d2; best = c; }
    }
    return best;
}

// Main thread, once per frame (world-live gated by manualBlockTick).  The aim
// focus follows the MANUAL COMBAT toggle (Z): while on, whoever you point at is
// re-asserted as the AI's current target.  Never touches the camera.
static void manualAimTick()
{
    bool want = s_manualAttackOn && s_mode == MODE_FREE_MOVE && s_freeMoveAnchor
             && !s_loadGuardActive && !s_dcShutdownInProgress
             && !s_freeMoveAnchor->isUnconcious() && !s_freeMoveAnchor->isDown();
    ULONGLONG now = GetTickCount64();
    if (!want)
    {
        if (s_lockTarget) { s_lockTarget = nullptr; s_aimLogged = nullptr; VerbLog("[WASDCombat] aim_focus off"); }
        s_aimClicked = false;
        return;
    }
    bool cursorFree = !(s_firstPersonActive || s_otsCamActive) || dcUiWantsCursor();
    Ogre::Camera* cam = (ou && ou->player && ou->player->camera) ? ou->player->camera->camera : nullptr;
    // RIGHT-CLICK: instant focus at any distance (in combat only; outside combat
    // the click stays a plain vanilla order).
    ULONGLONG rc = s_rmbAimPressMs;
    if (rc != 0 && rc != s_rmbAimSeenMs)
    {
        s_rmbAimSeenMs = rc;
        if (s_freeMoveAnchor->isInCombatMode(true, true))
        {
            float radius = s_aimHoverPx < 48.0f ? 48.0f : s_aimHoverPx;
            Character* cpick = cursorFree ? manualAimHoverPick(s_freeMoveAnchor, cam, radius, 600.0f)
                                          : manualAimPick(s_freeMoveAnchor, 600.0f);
            if (cpick)
            {
                s_lockTarget = cpick; s_aimClicked = true;
                VerbLog("[WASDCombat] aim_focus click");
            }
        }
    }
    if (now - s_aimEvalMs >= 50)                       // 20 Hz is plenty for a pointer
    {
        s_aimEvalMs = now;
        Character* pick = nullptr;
        if (cursorFree) pick = manualAimHoverPick(s_freeMoveAnchor, cam, s_aimHoverPx, s_aimRange);
        else if (!s_aimClicked) pick = manualAimPick(s_freeMoveAnchor, s_aimRange);   // the cone never overrides a clicked focus
        if (pick && pick != s_lockTarget) s_aimClicked = false;                       // pointed straight at someone else
        // STICKY (log 2026-09-21: 352 set / 228 none, a change every ~1 s = the AI
        // re-facing and re-pathing all fight): the focus only SWITCHES when a
        // different enemy is actually pointed at; it is kept while the current
        // one is still a live enemy within 1.5x the aim range, and released only
        // when that stops being true.
        if (pick) s_lockTarget = pick;
        else if (s_lockTarget)
        {
            bool keep = focusTargetValid(s_lockTarget) && s_lockTarget->movement && s_freeMoveAnchor->movement;
            if (keep && !s_aimClicked)                 // a clicked focus keeps at any distance
            {
                float dx = s_lockTarget->movement->pos.x - s_freeMoveAnchor->movement->pos.x;
                float dz = s_lockTarget->movement->pos.z - s_freeMoveAnchor->movement->pos.z;
                float lim = s_aimRange * 1.5f;
                keep = dx * dx + dz * dz <= lim * lim;
            }
            if (!keep) { s_lockTarget = nullptr; s_aimClicked = false; }
        }
    }
    if (s_lockTarget && !focusTargetValid(s_lockTarget)) { s_lockTarget = nullptr; s_aimClicked = false; }
    if (s_lockTarget != s_aimLogged)
    {
        s_aimLogged = s_lockTarget;
        DebugLog(s_lockTarget ? "[WASDCombat] aim_focus set" : "[WASDCombat] aim_focus none");
    }
    if (!s_lockTarget) return;
    CombatClass* cc = s_freeMoveAnchor->getCombatClass();
    if (cc && cc->currentTarget != s_lockTarget) cc->currentTarget = s_lockTarget;   // AI target = the aim focus
}

// Block POSE on a G press (user 2026-09-18: a press during the enemy's swing
// blocked without the pose, or the AI's own block came too late).  Same shape
// as the dodge inject: the game's chooser picks the block technique (the
// DEFENSIVE order and the perfect window keep the chooser open for us), then
// the changeState(BLOCK) writes; blockState starts a block technique's clip
// itself (it only pre-checks progress for dodges).  The perfect-block verdict
// still guarantees the block inside the window; outside it, vanilla judges the
// pose by direction and timing exactly as the AI's own block.
static Character* manualAttackFindFoe(CombatClass* cc, Character* me);   // fwd decl
static void manualBlockInject(CombatClass* cc)
{
    ULONGLONG press = s_blockPressMs;
    if (press == 0 || press == s_blockInjectedPressMs || press != s_blockAcceptedPressMs) return;
    ULONGLONG now = GetTickCount64();
    if (now - press > 300) { s_blockInjectedPressMs = press; return; }   // stale press
    if (!cc->combatModeActive) return;                                    // wait for combat (short)
    s_blockInjectedPressMs = press;
    swordStateEnum st = cc->getCombatState();
    if (st == STUMBLE || s_dodgeActiveTech || s_attackActive) return;
    if (st == BLOCK && cc->currentTechnique) return;                      // already blocking
    Character* me = cc->me;
    CharStats* stats = me ? me->getStats() : nullptr;
    if (!stats) return;
    // Build 62: the technique copies were only built on the first Q press of a
    // session (log 2026-09-30: a session with no dodge = every G press
    // "block_inject_no_technique", no pose at all).  Build them here too.
    manualDodgeBuildCopies();
    Character* foe = glintIncomingAttacker();          // build 61: whoever is swinging at us (any direction)
    bool fromSwing = foe != nullptr;
    if (!foe) foe = manualAttackFindFoe(cc, me);       // else the aimed / current / biggest-threat enemy
    if (!foe || !foe->getStats()) { VerbLog("[WASDCombat] block_inject_no_target"); return; }
    // Exactly like the dodge: OUR copies, never the chooser (the chooser needs
    // the exact incoming cut and returned nothing 54 times in the field).
    int want = -1;
    CombatTechniqueData* t = nullptr;
    if (stats->currentWeaponType == SKILL_UNARMED) t = manualDodgePick(false);   // unarmed: no block techniques exist
    else                                            t = manualBlockPick(me, foe, &want);
    if (!t) { VerbLog("[WASDCombat] block_inject_no_technique"); return; }
    cc->currentTechnique = t;
    cc->blockingTarget   = foe;
    bool played = false;
    if (t->isDodge && s_dodgePlayFn && cc->animation)        // (unarmed: a dodge technique needs the clip started)
    {
        std::string suffix;
        s_dodgePlayFn((void*)cc->animation, t, stats->blockSpeed * t->animSpeedMultiplier, &suffix);
        played = true;
    }
    cc->techniqueIntegrityCheckTimer = stats->calculateTechniqueInegrityCheckTimer();
    cc->stateTimer   = 0.2f;
    cc->nextMove     = BLOCK;
    cc->combatState  = BLOCK;
    char b[160];
    sprintf_s(b, sizeof(b), "[WASDCombat] block_inject anim=%s want=%d dodge=%d played=%d state_was=%d press_age=%llu from_swing=%d",
              t->animation.c_str(), want, t->isDodge ? 1 : 0, played ? 1 : 0, (int)st, now - press, fromSwing ? 1 : 0);
    VerbLog(b);
}

static void manualDodgeInject(CombatClass* cc)
{
    ULONGLONG press = s_dodgePressMs;
    if (press == 0 || press == s_dodgeInjectedPressMs || !manualDodgeCommitLive()) return;
    if (!cc->combatModeActive) return;                 // the AI re-engages combat first (1-2 frames)
    swordStateEnum st = cc->getCombatState();
    // HIGH PRIORITY (user 2026-09-18): the dodge overrides whatever the AI is
    // doing - its swing, its block, its waiting.  While STAGGERED (STUMBLE) the
    // normal dodges are off and only the game's stumble dodges are available
    // (the ones the AI itself plays out of a stagger) - anti-cheese.
    bool stumbling = (st == STUMBLE);
    if (s_staggerStartMs != 0 || stumbling) return;    // never out of a stagger (a confirmed one drops the press; a blip clears in a frame)
    Character* me = cc->me;
    CharStats* stats = me ? me->getStats() : nullptr;
    if (!stats) return;
    s_dodgeInjectedPressMs = press;                    // one dodge per press

    // Who are we dodging?  CRASH LESSON (dump 2026-09-18 12:37, AV at
    // chooseBlock+0x5E = `mov rdx,[opponent]`): the chooser dereferences its
    // opponent unconditionally - vanilla only ever calls it with a real
    // attacker.  So: the AI's biggest threat, else its current/attack target,
    // else the nearest live hostile in range; with NO foe there is no dodge.
    Character* foe = manualFoeResolve(cc, me);       // lock, else the enemy you are looking at
    if (!foe) foe = cc->getBiggestThreat(cc->threats, 0.0f);
    if (!foe) foe = cc->currentTarget;
    if (!foe) foe = cc->_getAttackTarget().getCharacter();
    if (!foe && s_gwFrame && me->movement)
    {
        const Ogre::Vector3 mp = me->movement->pos;
        const float maxR2 = 40.0f * 40.0f;
        float best2 = -1.0f;
        auto& chars = s_gwFrame->getCharacterUpdateList();
        for (auto it = chars.begin(); it != chars.end(); ++it)
        {
            Character* c = *it;
            if (!c || c == me || !c->movement) continue;
            float dx = c->movement->pos.x - mp.x, dz = c->movement->pos.z - mp.z;
            float d2 = dx * dx + dz * dz;
            if (d2 >= maxR2 || (best2 >= 0.0f && d2 >= best2)) continue;
            if (c->isDead() || c->isUnconcious() || c->isDown()) continue;   // downed = not attackable
            if (!c->isEnemy(me, true)) continue;
            best2 = d2; foe = c;
        }
    }
    if (!foe || !foe->getStats())
    {
        VerbLog("[WASDCombat] dodge_no_target (no hostile to dodge - press ignored)");
        s_dodgeCommitUntilMs = 0;                      // movement is yours again
        return;
    }
    float versus = foe->getStats()->getMeleeAttack();
    if (versus < 0.0f) versus = 0.0f;                  // animals / no melee stat read as -1

    // PICK one of OUR copies - never the chooser (its null-opponent crash, its
    // unarmed-only dodge roll and its AI weighting are all out of the loop).
    CombatTechniqueData* t = manualDodgePick(stumbling);
    if (!t)
    {
        DebugLog(stumbling ? "[WASDCombat] dodge_no_technique (no stumble dodge copies)"
                           : "[WASDCombat] dodge_no_technique (no dodge copies built)");
        s_dodgeCommitUntilMs = 0;
        return;
    }

    // ROLL honestly: vanilla dodge chance (Dodge +bonus via getDodge) vs the foe.
    float chance = stats->calculateDodgeChance(versus, false);   // reads getDodge(true) -> +bonus
    float roll   = UtilityT::random() * 100.0f;
    s_dodgeRollOk = roll <= chance;

    // INJECT, in initialiseBlock's order: technique + target, START THE CLIP,
    // timers, then the changeState(BLOCK, 0.2) writes.  The clip start is the
    // step blockState cannot do for us: it reads the technique's progress first
    // and an un-started clip reads 1.01 (> 0.98 = finished), which ended every
    // injected dodge on its first frame in builds 11-14.
    cc->currentTechnique = t;
    cc->blockingTarget   = foe;
    bool played = false;
    if (s_dodgePlayFn && cc->animation)
    {
        std::string suffix;                            // dodges take no " override" suffix
        float speed = stats->blockSpeed * t->animSpeedMultiplier;
        s_dodgePlayFn((void*)cc->animation, t, speed, &suffix);
        played = true;
    }
    cc->techniqueIntegrityCheckTimer = stats->calculateTechniqueInegrityCheckTimer();
    cc->stateTimer                   = 0.2f;
    cc->nextMove                     = BLOCK;
    cc->combatState                  = BLOCK;
    s_dodgeActiveTech    = t;
    s_dodgeCommitUntilMs = GetTickCount64() + 1200;    // keep WASD buffered for the clip (bounded)

    char b[192];
    sprintf_s(b, sizeof(b), "[WASDCombat] dodge_inject anim=%s stumbling=%d played=%d foe=%d vsSkill=%.0f chance=%.0f roll=%.0f ok=%d state_was=%d press_age=%llu",
              t->animation.c_str(), stumbling ? 1 : 0, played ? 1 : 0, foe ? 1 : 0, versus, chance, roll, s_dodgeRollOk ? 1 : 0,
              (int)st, GetTickCount64() - press);
    VerbLog(b);
}

// Resolve a foe for the attack (same order as the dodge; the chooser is never
// involved here, but startupState needs a current target or it ends combat).
static Character* manualAttackFindFoe(CombatClass* cc, Character* me)
{
    Character* foe = manualFoeResolve(cc, me);       // lock, else the enemy you are looking at
    if (!foe) foe = cc->currentTarget;                 // the AI's own target (getNearestEnemyInAttackZone is protected)
    if (!foe) foe = cc->getBiggestThreat(cc->threats, 0.0f);
    if (!foe) foe = cc->_getAttackTarget().getCharacter();
    if (!foe && s_gwFrame && me->movement)
    {
        const Ogre::Vector3 mp = me->movement->pos;
        const float maxR2 = 40.0f * 40.0f;
        float best2 = -1.0f;
        auto& chars = s_gwFrame->getCharacterUpdateList();
        for (auto it = chars.begin(); it != chars.end(); ++it)
        {
            Character* c = *it;
            if (!c || c == me || !c->movement) continue;
            float dx = c->movement->pos.x - mp.x, dz = c->movement->pos.z - mp.z;
            float d2 = dx * dx + dz * dz;
            if (d2 >= maxR2 || (best2 >= 0.0f && d2 >= best2)) continue;
            if (c->isDead() || c->isUnconcious() || c->isDown()) continue;   // downed = not attackable
            if (!c->isEnemy(me, true)) continue;
            best2 = d2; foe = c;
        }
    }
    return foe;
}

// Inject the swing, once per press, from the combat-AI hook: the AI's own
// STARTUP -> CHOP_WEAPON entry (checkForNeedBlock's shape with nextMove =
// CHOP_WEAPON); vanilla picks the technique, plays it, and lands it.
static void manualAttackInject(CombatClass* cc)
{
    ULONGLONG press = s_attackPressMs;
    if (press == 0 || press == s_attackInjectedPressMs || press != s_attackAcceptedPressMs
        || !manualAttackCommitLive()) return;
    if (!cc->combatModeActive) return;                 // the AI re-engages combat first (1-2 frames)
    swordStateEnum st = cc->getCombatState();
    if (st == STUMBLE) return;                         // no swing out of a stagger (anti-cheese)
    if (s_dodgeActiveTech) return;                     // let the dodge finish
    Character* me = cc->me;
    if (!me) return;
    s_attackInjectedPressMs = press;                   // one swing per press
    Character* foe = manualAttackFindFoe(cc, me);
    if (!foe)
    {
        VerbLog("[WASDCombat] attack_no_target (no hostile in range - press ignored)");
        s_attackCommitUntilMs = 0;
        return;
    }
    if (cc->currentTarget != foe) cc->currentTarget = foe;   // startupState re-asserts it via setAttackTarget
    cc->stateTimer   = 0.0f;
    cc->nextMove     = CHOP_WEAPON;
    cc->combatState  = STARTUP_STATE;
    s_attackActive        = true;
    s_attackSawChop       = false;
    s_attackActiveSinceMs = GetTickCount64();
    s_attackCommitUntilMs = s_attackActiveSinceMs + 1500;   // keep WASD buffered through the swing (bounded)
    char b[160];
    float distF = (me->movement && foe->movement) ? me->movement->pos.distance(foe->movement->pos) : -1.0f;
    sprintf_s(b, sizeof(b), "[WASDCombat] attack_inject state_was=%d press_age=%llu moving=%d dist=%.0f locked=%d",
              (int)st, s_attackActiveSinceMs - press, (s_wHeld || s_aHeld || s_sHeld || s_dHeld) ? 1 : 0,
              distF, (foe == s_lockTarget) ? 1 : 0);   // locked= aim focus hit
    VerbLog(b);
}

// The swing is over once the machine has left STARTUP/CHOP_WEAPON (or never
// got there within a bounded time - e.g. the target walked out of reach).
static void manualAttackTrack(CombatClass* cc, bool playerMoving)
{
    if (!s_attackActive) return;
    swordStateEnum st = cc->getCombatState();
    if (st == CHOP_WEAPON) s_attackSawChop = true;
    bool swinging = (st == STARTUP_STATE || st == CHOP_WEAPON);
    ULONGLONG now = GetTickCount64();
    // OUT OF REACH: the game's initialiser failed for range (it stepped toward the
    // target and the state fell back).  Vanilla would re-decide and keep closing
    // in; with the AI's own swings refused, we re-arm the same STARTUP+CHOP pair
    // each time it falls back, for up to AttackApproachMs, so the character walks
    // in and swings when in range - unless the player is driving with WASD (then
    // closing the distance is theirs and the press is spent).
    // The failed initialiser CLEARS currentTarget (log 2026-09-21: every retry
    // fizzled with tgt=0 and the approach never ran) - re-resolve the foe here
    // (downed ones are excluded now) so the character actually walks in.
    Character* approachTgt = cc->currentTarget;
    if (!approachTgt && !swinging && !s_attackSawChop && cc->combatModeActive && cc->me)
        approachTgt = manualAttackFindFoe(cc, cc->me);
    if (!swinging && !s_attackSawChop && !playerMoving && cc->combatModeActive && approachTgt
        && st != STUMBLE && !s_dodgeActiveTech
        && (now - s_attackActiveSinceMs) <= (ULONGLONG)s_attackApproachMs)
    {
        // Build 48: the walk-in is the AI's own.  Re-arming STARTUP every frame
        // restarted the startup animation 60x a second while closing in (log:
        // a tenth of all swings ran past 1.6 s, the worst 8.7 s).  While our
        // swing is live (s_attackActive) the initialiser accepts the AI's own
        // attempts and the idle pin stands aside, so vanilla's decision loop
        // approaches, steps in and swings exactly as it does unmanaged; the
        // track below ends the press after that one swing.
        if (cc->currentTarget != approachTgt) cc->currentTarget = approachTgt;
        s_attackCommitUntilMs = now + 250;          // keep the commit alive frame to frame
        if (now - s_attackReapproachLogMs >= 1000)
        {
            s_attackReapproachLogMs = now;
            VerbLog("[WASDCombat] attack_approach (out of reach - AI closes in for this swing)");
        }
        return;
    }
    if (!swinging || now - s_attackActiveSinceMs > 2500 + (ULONGLONG)s_attackApproachMs)
    {
        // FIZZLE RETRY (log 2026-09-21: an inject on the last frame of a stagger
        // recovery fell back in 15 ms with no swing and the press was spent - the
        // player had to press again).  A start that never reached CHOP_WEAPON
        // re-arms the SAME press for the next idle frame, bounded: at most 3
        // retries and never later than AttackWindowMs + 600 ms after the press.
        ULONGLONG press = s_attackAcceptedPressMs;
        if (!s_attackSawChop && press != 0 && now >= press
            && (now - press) <= (ULONGLONG)s_attackWindowMs + 600
            && !(s_attackRetryPressMs == press && s_attackRetries >= 3))
        {
            if (s_attackRetryPressMs != press) { s_attackRetryPressMs = press; s_attackRetries = 0; }
            ++s_attackRetries;
            s_attackActive          = false;
            s_attackInjectedPressMs = 0;                                   // the inject may fire again for this press
            s_attackCommitUntilMs   = press + (ULONGLONG)s_attackWindowMs + 600;
            char b[160];
            sprintf_s(b, sizeof(b), "[WASDCombat] attack_retry n=%d state=%d ms=%llu cm=%d tgt=%d moving=%d",
                      s_attackRetries, (int)st, now - s_attackActiveSinceMs,
                      cc->combatModeActive ? 1 : 0, cc->currentTarget ? 1 : 0, playerMoving ? 1 : 0);
            VerbLog(b);
            return;
        }
        char b[160];
        sprintf_s(b, sizeof(b), "[WASDCombat] attack_done state=%d ms=%llu swung=%d cm=%d tgt=%d moving=%d",
                  (int)st, now - s_attackActiveSinceMs, s_attackSawChop ? 1 : 0,
                  cc->combatModeActive ? 1 : 0, cc->currentTarget ? 1 : 0, playerMoving ? 1 : 0);
        VerbLog(b);
        s_attackActive        = false;
        s_attackCommitUntilMs = 0;      // sharp: movement is yours the frame the swing ends
        if (s_attackSawChop) s_attackRecoveryUntilMs = now + (ULONGLONG)s_attackRecoveryMs;   // follow-through: presses ignored
    }
}

// Once the machine has left BLOCK (or our technique is gone), the dodge is over.
static void manualDodgeTrack(CombatClass* cc)
{
    if (!s_dodgeActiveTech) return;
    if (cc->getCombatState() != BLOCK || cc->currentTechnique != s_dodgeActiveTech)
    {
        s_dodgeActiveTech    = nullptr;
        s_dodgeRollOk        = false;
        s_dodgeCommitUntilMs = 0;      // sharp: movement is yours the frame the dodge ends
        s_dodgeRecoveryUntilMs = GetTickCount64() + (ULONGLONG)s_dodgeRecoveryMs;   // presses ignored while the follow-through plays
        // Dodge-then-strike: an E pressed during the dodge (within the last 600 ms)
        // becomes the swing now, as if pressed the frame the dodge ended.
        if (s_attackAfterDodgeMs != 0)
        {
            ULONGLONG nowE = GetTickCount64();
            if (nowE - s_attackAfterDodgeMs <= 600 && s_manualAttackOn && !s_attackActive)
            {
                s_attackAcceptedPressMs = s_attackAfterDodgeMs;
                s_attackInjectedPressMs = 0;
                s_attackCommitUntilMs   = nowE + (ULONGLONG)s_attackWindowMs;
                VerbLog("[WASDCombat] attack_commit after_dodge");
            }
            s_attackAfterDodgeMs = 0;
        }
    }
}

static bool manualDodgeChar(CharStats* st)
{
    return st && st->me && st->me == s_freeMoveAnchor && s_mode == MODE_FREE_MOVE
        && s_settingManualBlock && !s_dcShutdownInProgress && !s_loadGuardActive;
}
// Bonus visible/active while the key is held, inside the timing window after a
// press, or while our roll is pending (block mode's +20 is likewise "while on").
// The +DodgeSkillBonus is on for the DODGE WINDOW after a press (and while the
// injected dodge is in flight) - a visible "(+20)" that marks the timing window,
// not a hold buff.
static bool manualDodgeBonusActive(CharStats* st)
{
    if (!manualDodgeChar(st) || !s_manualAttackOn) return false;   // the (+20) is a manual-combat effect
    ULONGLONG now = GetTickCount64();
    // Key DOWN and inside the window: releases the moment the key comes up.
    return s_dodgeHeld && s_dodgePressMs > 0 && (now - s_dodgePressMs) <= (ULONGLONG)s_dodgeWindowMs;
}
// CharStats::getDodge - the Dodge skill the roll (and the stats HUD) reads.
static float (*s_getDodgeOrig)(CharStats* st, bool bonuses);
static float getDodge_hook(CharStats* st, bool bonuses)
{
    float v = s_getDodgeOrig(st, bonuses);
    // The stats panel prints "{getDodge(true)} (+{getDodge(true)-getDodge(false)})",
    // so the bonus goes on the with-bonuses read only - it then shows as "(+20)"
    // in the modifier text, exactly like block mode's melee-defence bonus.
    if (bonuses && manualDodgeBonusActive(st))
    {
        v += s_dodgeSkillBonus;
    }
    return v;
}

// CombatClass::_iHitYouAreYouHit - the block VERDICT Character::hitByMeleeAttack
// consults (returns HIT_SWORD = blocked, HIT_MISSED = dodged, else hit).  Runs on
// the game thread from the ATTACKER's impact check.  Inside the perfect window we
// answer HIT_SWORD for the controlled character and skip the original (its only
// entry side effect, the attacker notify, is repeated by the caller anyway; its
// crime-witness path belongs to the "hit landed" outcome).  Everything else -
// including the HELD skill block, which vanilla resolves from the block
// technique currently playing - forwards untouched.
static HitMaterialType (*s_iHitOrig)(CombatClass* cc, CutDirection dir, Damages& damage, Character* who);
static HitMaterialType iHitYouAreYouHit_hook(CombatClass* cc, CutDirection dir, Damages& damage, Character* who)
{
    Character* self = cc ? cc->me : nullptr;
    if (self && self == s_freeMoveAnchor) glintImpact(who);   // weapon glint: flash out at the hit (blocked or not)
    // DIAG (build 56): every hit on the controlled character, with the attacker's
    // technique - to find the two-strike moves and tune the block windows.
    if (self && self == s_freeMoveAnchor)
    {
        static ULONGLONG s_lastHitMs = 0; static Character* s_lastHitWho = nullptr;
        ULONGLONG nowD = GetTickCount64();
        CombatClass* acc = who ? who->getCombatClass() : nullptr;
        CombatTechniqueData* at = acc ? acc->currentTechnique : nullptr;
        char hb[224];
        sprintf_s(hb, sizeof(hb), "[WASDCombat] hit_on_me anim=%s pts=%d dir=%d since_last=%llu same_attacker=%d manual=%d hold=%d",
                  at ? at->animation.c_str() : "?", at ? (int)at->impactPoints.size() : -1, (int)dir,
                  s_lastHitMs ? nowD - s_lastHitMs : 0ULL, (who && who == s_lastHitWho) ? 1 : 0,
                  s_manualAttackOn ? 1 : 0, s_blockOrderApplied ? 1 : 0);
        VerbLog(hb);
        s_lastHitMs = nowD; s_lastHitWho = who;
    }
    // MANUAL DODGE, failed roll: the animation plays but does not protect -
    // hide the technique from the verdict for this hit so vanilla resolves it
    // as a plain hit.  A successful roll is protected only by the technique's
    // own animation window (vanilla), never by us.
    if (self && self == s_freeMoveAnchor && s_dodgeActiveTech && !s_dodgeRollOk
        && cc->currentTechnique == s_dodgeActiveTech)
    {
        CombatTechniqueData* keep = cc->currentTechnique;
        cc->currentTechnique = nullptr;
        HitMaterialType r = s_iHitOrig(cc, dir, damage, who);
        cc->currentTechnique = keep;
        char b[96];
        sprintf_s(b, sizeof(b), "[WASDCombat] dodge_failed_hit dir=%d result=%d", (int)dir, (int)r);
        VerbLog(b);
        return r;
    }
    if (manualBlockArmed(self))
    {
        ULONGLONG now   = GetTickCount64();
        ULONGLONG press = s_blockPressMs;
        ULONGLONG origin = s_blockHoldStartMs;                 // first press of this block (a resume keeps it)
        bool inWindow = s_manualAttackOn && press > 0 && press == s_blockAcceptedPressMs && origin != 0
                     && (now - origin) <= (ULONGLONG)s_perfectWindowMs;
        bool cooling  = now < s_perfectCooldownUntilMs;
        bool rear = (dir == CUT_REAR_DOWNWARD || dir == CUT_REAR_LEFT || dir == CUT_REAR_RIGHT);
        bool front = !rear;
        if (front && who && who->movement && self->movement)
        {
            Ogre::Vector3 f  = self->movement->direction;  f.y  = 0.0f;
            Ogre::Vector3 to = who->movement->pos - self->movement->pos; to.y = 0.0f;
            if (f.squaredLength() > 1e-6f && to.squaredLength() > 1e-6f)
                front = f.normalisedCopy().dotProduct(to.normalisedCopy()) > 0.34f;   // ~140 deg arc
        }
        bool swinging  = cc->combatModeActive && cc->getCombatState() == CHOP_WEAPON;
        bool staggered = cc->combatModeActive && cc->getCombatState() == STUMBLE;   // no block out of a stagger
        // Build 56 (user 2026-09-29: "the follow-up hit always connects"): the
        // cooldown starts at the FIRST perfect hit, and this check refused every
        // later hit as "cooling" - so the second strike of a two-hit technique
        // always fell through to vanilla.  A press that already earned its perfect
        // keeps covering hits for the rest of its window.
        bool sameWindow = (s_perfectUsedPressMs == origin);
        // Build 57: a follow-up strike is covered for PERFECT_FOLLOWUP_MS after the
        // first perfect hit even if the press window has just run out (a late
        // press that parries the first strike at ~900 ms would otherwise leave the
        // second strike, 360 ms later, to the skill roll).
        bool followUp = sameWindow && s_perfectLastHitMs != 0 && (now - s_perfectLastHitMs) <= PERFECT_FOLLOWUP_MS;
        if ((inWindow || followUp) && (sameWindow || !cooling) && front && !swinging && !staggered)
        {
            // One press = one perfect window; the window covers a combo's follow-up
            // hits, and the cooldown starts from the FIRST perfect hit.
            if (s_perfectUsedPressMs != origin)
                s_perfectUsedPressMs     = origin;   // (was never stored: every combo hit restarted the cooldown)
            // REWARD (user 2026-10-08) instead of the old PerfectBlockCooldownMs
            // lockout: no cooldown at all, and the next press is accepted fresh
            // (see manualBlockTick) - chaining perfects is the skill being paid.
            s_perfectCooldownUntilMs = 0;
            s_blockCooldownUntilMs   = 0;
            s_perfectRewardUntilMs   = now + (ULONGLONG)s_perfectRewardMs;
            s_perfectLastHitMs = now;
            char b[128];
            sprintf_s(b, sizeof(b), "[WASDCombat] perfect_block dir=%d press_age=%llu followup=%d",
                      (int)dir, now - origin, sameWindow ? 1 : 0);
            VerbLog(b);
            return HIT_SWORD;   // caller runs the native block branch: pose, parry sound, spark, XP
        }
    }
    HitMaterialType rv = s_iHitOrig(cc, dir, damage, who);
    if (self && self == s_freeMoveAnchor)
    {
        char vb[96];
        sprintf_s(vb, sizeof(vb), "[WASDCombat] hit_vanilla_verdict result=%d", (int)rv);   // 5 = blocked (HIT_SWORD)
        VerbLog(vb);
    }
    return rv;
}

// initCombatMode_hook and youKnowImAttacking_hook removed —
// DC no longer blocks enemy combat entry or attack notifications.
// The combat AI runs freely; DC is a movement overlay only.

// -----------------------------------------------------------------------
// combatGo_hook — CombatClass::_NV_go, the per-frame combat AI decision.
// DC OWNERSHIP-HANDOFF MODEL (user req 2026-06-21): clean, exclusive ownership of
// the controlled character's locomotion, NO per-frame tug-of-war (that was the
// root cause of all the combat stutter/slide).
//   - WASD held (+ a short COMBAT_WASD_BRIDGE_MS grace for key-rolls): MOVEMENT
//     owns the character — SKIP go() so the AI takes no action and never steers,
//     and WASD has uncontested control = smooth, instant, no stutter.  Any clip
//     already mid-play is dropped to the run anim by the charMovUpdate cutoff =
//     no slide.
//   - WASD released: the AI owns the character — go() runs FULLY AUTONOMOUSLY
//     (vanilla combat: block/dodge/attack/position), resuming instantly.
// The handoff happens once at the press/release edge, never per-frame.  Only the
// anchor (thisptr->me == s_freeMoveAnchor, me @0x188) in DC; everything else is
// untouched.  Hooking go() is safe (proven in the Focus-Mode line; the door-era
// crash was setCurrentAction, NOT go).
// -----------------------------------------------------------------------
static void (*s_combatGoOrig)(CombatClass* thisptr, float frameTime);
static void combatGo_hook(CombatClass* thisptr, float frameTime)
{
    if (!s_dcShutdownInProgress && !s_loadGuardActive
        && s_mode == MODE_FREE_MOVE && thisptr && thisptr->me
        && thisptr->me == s_freeMoveAnchor)
    {
        bool wasdHeld     = s_wHeld || s_aHeld || s_sHeld || s_dHeld;
        bool movementOwns = wasdHeld
            || (s_wasdLastHeldMs > 0
                && (GetTickCount64() - s_wasdLastHeldMs) < COMBAT_WASD_BRIDGE_MS);
        // (Manual block: the pose is vanilla's own - the DEFENSIVE_COMBAT standing
        //  order is applied while the key is held; nothing to request here.)
        // Manual dodge: raise the native block intent for a fresh press while the
        // AI is about to run this frame (not while WASD owns the character).
        if (s_settingManualBlock)
        {
            // MANUAL ATTACK mode: a swing intent the AI raised on its own is turned
            // into the armed ready stance before the startup state initialises it.
            // Our E sets the same STARTUP+CHOP pair with s_attackActive.
            if (s_manualAttackOn && !s_attackActive && thisptr->combatModeActive
                && thisptr->getCombatState() == STARTUP_STATE && thisptr->nextMove == CHOP_WEAPON)
            {
                thisptr->nextMove    = WAIT_MENACINGLY;
                thisptr->combatState = WAIT_MENACINGLY;
            }
            // IDLE PIN (build 48; log 2026-09-21: the AI's swing was refused in 289
            // separate seconds = it decided to attack, entered STARTUP, got refused
            // by the initialiser and fell back, several times a second, all fight -
            // a startup-animation twitch that was most of the "stutter").  The
            // decision runs its startup inside the same update, so the STARTUP
            // catch above never sees it: pin the DECISION state itself to the
            // armed ready hold with a short dwell, so the AI never decides at all
            // while nothing of ours is pending.  Untouched: a block on the way
            // (nextMove BLOCK / wantsToBlock), an incoming swing (the AI's own
            // block logic must run - the glint tracker knows), our swing / dodge
            // (pending or in flight), a stagger.
            if (s_manualAttackOn && thisptr->combatModeActive && !s_attackActive && !s_dodgeActiveTech
                && !manualAttackCommitLive() && !manualDodgeCommitLive()
                && thisptr->getCombatState() == DECISION
                && thisptr->nextMove != BLOCK && !thisptr->wantsToBlock
                && !(s_settingAiBlocks && glintIncoming()))   // build 49: WaitingStateAI hands back to DECISION every
                                                                // frame an attack slot is free, and enemy swings are
                                                                // "incoming" most of a melee - with AI blocks OFF the
                                                                // exemption only let the refused-startup twitch back in
            {
                thisptr->nextMove    = WAIT_MENACINGLY;
                thisptr->combatState = WAIT_MENACINGLY;
                thisptr->stateTimer  = 0.4f;
                ULONGLONG nowPin = GetTickCount64();
                if (nowPin - s_manualPinLogMs >= 1000)
                {
                    s_manualPinLogMs = nowPin;
                    VerbLog("[WASDCombat] manual_ai_idle_pinned");
                }
            }
            if (focusLockLive() && thisptr->currentTarget != s_lockTarget)
                thisptr->currentTarget = s_lockTarget;          // the AI thinks about the lock only
            manualDodgeTrack(thisptr);
            manualAttackTrack(thisptr, movementOwns);
            s_anchorPrevState = (int)thisptr->getCombatState();   // DIAG for manual_ai_attack_denied
            if (!movementOwns)
                manualBlockInject(thisptr);
            if (!movementOwns || manualDodgeCommitLive())
                manualDodgeInject(thisptr);
            if (!movementOwns || manualAttackCommitLive())
                manualAttackInject(thisptr);
        }
        // Let go() RUN while a committed combat clip is mid-play (swing / stagger /
        // parry) so it FINISHES and the state advances (the charMovUpdate buffer holds
        // movement meanwhile); suppress it the instant the clip is done so no NEW attack
        // chains and movement takes over.  Suppressing only OUTSIDE the clip also means
        // we never freeze the state machine mid-clip (the buffer would otherwise stick
        // forever — e.g. a stagger that never advances).
        if (movementOwns && !isCommittedCombatClip(thisptr->me))
        {
            s_retreatLockGoSuppressed = true;   // WASD owns locomotion; AI stands down
            return;                             // skip the combat decision entirely
        }
    }
    s_retreatLockGoSuppressed = false;
    s_combatGoOrig(thisptr, frameTime);
}

// -----------------------------------------------------------------------
// Main-thread hook — GameWorld::mainLoop_GPUSensitiveStuff
//
// Execution order:
//   1.  Safety gate (load-guard)
//   2.  Selection tracking
//   3.  V-Mode transition
//   4.  HUD + X speed key
//   5.  Pre-AI WASD
//   6.  s_mainLoopOrig (AI + CharMovement::update + CombatClass::go)
//   7.  Post-AI combat job suppression
//   8.  Periodic squad-threat scan
//   9.  Post-AI WASD re-application + instant stop
// -----------------------------------------------------------------------
static void (*s_mainLoopOrig)(GameWorld* thisptr, float time);

// Build 65: is this character in the game's live update list?  (After a load the
// selection handle can still name the OLD object for a while - it reads as
// unconscious and WASD is refused until the player re-selects.)
static bool dcInUpdateList(GameWorld* gw, Character* c)
{
    if (!gw || !c) return false;
    auto& chars = gw->getCharacterUpdateList();
    for (auto it = chars.begin(); it != chars.end(); ++it) if (*it == c) return true;
    return false;
}

static void mainLoop_hook(GameWorld* thisptr, float time)
{
    s_gwFrame = thisptr;  // manual combat: world for the dodge foe scan (fresh every frame)
    // Build 74 (quickload froze 2026-10-01, 6501 per-frame AVs): the SaveManager
    // signal is set when a load is REQUESTED and consumed before the loading
    // flag goes up, so reading it only while the flag is up (build 67) never saw
    // it and a quickload was classed as a chunk stream.  Latch it every frame;
    // release when the flag drops.  Plus a 4 Hz state line while any flag is up.
    {
        SaveManager* smL = SaveManager::getSingleton();
        int sigL = smL ? smL->signal : 0;
        if (sigL == SaveManager::LOADGAME || sigL == SaveManager::NEWGAME || sigL == SaveManager::IMPORTGAME)
        {
            if (!s_realLoadLatched)
            {
                char lb[64]; sprintf_s(lb, sizeof(lb), "[WASDCombat] dc_real_load_latched signal=%d", sigL); DebugLog(lb);
            }
            s_realLoadLatched = true;
            s_latchSetMs = GetTickCount64();
        }
        bool flagNow = ou && ou->isLoadingFromASaveGame();
        if (s_loadFlagPrev && !flagNow) s_realLoadLatched = false;                 // load finished: release
        if (s_realLoadLatched && !flagNow && GetTickCount64() - s_latchSetMs > 10000) s_realLoadLatched = false;   // signal with no load: expire
        s_loadFlagPrev = flagNow;
        if (flagNow)
        {
            ULONGLONG nowL = GetTickCount64();
            if (nowL - s_loadFlagLogMs >= 250)
            {
                s_loadFlagLogMs = nowL;
                char fb[160];
                sprintf_s(fb, sizeof(fb), "[WASDCombat] dc_load_flag signal=%d latched=%d player=%d anchor=%d in_list=%d",
                          sigL, s_realLoadLatched ? 1 : 0, ou->player ? 1 : 0, s_freeMoveAnchor ? 1 : 0,
                          (s_freeMoveAnchor && dcInUpdateList(thisptr, s_freeMoveAnchor)) ? 1 : 0);
                VerbLog(fb);
            }
        }
    }
    manualBlockTick();    // manual combat: hold/release edges + dodge press processing (self-gated, cheap)
    // Hard-shutdown: all DC hook logic suppressed while true.
    // The six-condition stabilization countdown runs inside this block so it
    // advances even while the shutdown flag is held.
    if (s_dcShutdownInProgress)
    {
        if (!s_hookBlockLoggedMain) {
            s_hookBlockLoggedMain = true;
            DebugLog("[WASDCombat] dc_hooks_blocked_during_loadgame hook=mainLoop"); }

        if (ou && ou->player) s_mainLoopOrig(thisptr, time);

        // Six-condition check: runs once per frame after load finishes.
        // Condition 1: load finished (loadSig=false) — implied by the outer if.
        // Condition 2: player valid.
        // Conditions 3-4: selected character and movement pointers valid.
        // Condition 5: 60-frame window with all above held without lapse.
        // Condition 6: no stale anchor from old save remains.
        if (s_loadGuardActive && ou && ou->player && !dcRealLoad())
        {
            Character* chStab  = ou->player->selectedCharacter.getCharacter();
            bool chValid       = (chStab != nullptr);
            bool mvValid       = chValid && (chStab->movement != nullptr);
            bool noStaleAnchor = (s_freeMoveAnchor == nullptr);
            bool allClear      = chValid && mvValid && noStaleAnchor;

            if (allClear)
            {
                if (s_stabilizationCountdown == 0)
                {
                    s_stabilizationCountdown = 60;
                    DebugLog("[WASDCombat] reload_stabilization_started");
                }
                s_stabilizationCountdown--;
            }
            else
            {
                s_stabilizationCountdown = 0; // reset if any condition lapses
            }

            {
                ULONGLONG _nowW = GetTickCount64();
                if (_nowW - s_shutdownWaitLogTick >= 1000) { s_shutdownWaitLogTick = _nowW;
                    char _wbuf[256];
                    sprintf_s(_wbuf, sizeof(_wbuf),
                        "[WASDCombat] dc_loadgame_waiting_for_safe_reacquire "
                        "player=valid character=%s movement=%s anchor_clear=%s frames=%d/60",
                        chValid ? "valid" : "invalid",
                        mvValid ? "valid" : "invalid",
                        noStaleAnchor ? "yes" : "no",
                        allClear ? (60 - s_stabilizationCountdown) : 0);
                    DebugLog(_wbuf);
                }
            }

            if (allClear && s_stabilizationCountdown == 0)
            {
                s_loadGuardActive        = false;
                s_postLoadReacquire      = true;
                s_stabilizationCountdown = 0;
                DebugLog("[WASDCombat] dc_loadgame_safe_reacquire_complete");
                s_dcShutdownInProgress   = false;
                DebugLog("[WASDCombat] dc_shutdown_flag_cleared_after_safe_reacquire");
                DebugLog("[WASDCombat] reload_complete_reacquire_started");
            }
        }
        return;
    }

    // Profiling: initialize QPC frequency once; time every mainLoop invocation.
    if (!s_profInited)
    {
        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);
        s_profFreq         = freq.QuadPart;
        s_profInited       = true;
        s_prof_windowStart = GetTickCount64();
    }
    ScopeTimer _tML(s_prof_mainLoop);

    // 1. Safety gate.
    {
        bool ouNull  = (ou == nullptr);
        bool loadSig = ouNull || !ou->player || dcRealLoad();
        bool forceReal = false;                   // build 75: set when the anchor vanished mid-"stream"
        // Build 67: chunk STREAM (flag up, no real-load signal, player present):
        // keep control while the anchor is still in the live update list.
        if (!loadSig && ou->isLoadingFromASaveGame())
        {
            if (s_freeMoveAnchor && !dcInUpdateList(thisptr, s_freeMoveAnchor))
            {
                loadSig = true; forceReal = true;                 // anchor gone mid-stream: real-load handling (log 2026-10-01: the branch below re-asked dcRealLoad and took the microload path = freed-anchor deref every frame)
                static ULONGLONG s_vanishLogMs = 0;
                ULONGLONG nowV = GetTickCount64();
                if (nowV - s_vanishLogMs >= 1000) { s_vanishLogMs = nowV; DebugLog("[WASDCombat] dc_stream_anchor_vanished (falling back to the load path)"); }
            }
            else if (!s_streamLogged)
            {
                s_streamLogged = true;
                SaveManager* smS = SaveManager::getSingleton();
                char sb[112];
                sprintf_s(sb, sizeof(sb), "[WASDCombat] dc_stream_load_control_kept signal=%d anchor=%d", smS ? smS->signal : -1, s_freeMoveAnchor ? 1 : 0);
                DebugLog(sb);
            }
        }
        else if (!ou || !ou->isLoadingFromASaveGame()) s_streamLogged = false;

        if (loadSig)
        {
            if (ouNull)
            {
                // Game world gone — full hard shutdown regardless of user intent.
                if (!s_dcShutdownInProgress)
                {
                    s_dcShutdownInProgress = true;
                    DebugLog("[WASDCombat] dc_loadgame_shutdown_begin");
                    if (s_userWantsDC || s_mode == MODE_FREE_MOVE)
                    {
                        DebugLog("[WASDCombat] dc_hard_shutdown_reason=LOADGAME");
                        s_userWantsDC = false;
                    }
                    s_healingJobActive             = false;
                    s_medicalJobSuppressedThisHold = false;
                    s_freeMoveAnchor    = nullptr;
                    s_selectedCharacter = nullptr;
                    DebugLog("[WASDCombat] dc_loadgame_clear_anchor");
                    s_anchorMovement    = nullptr;
                    s_selectedMovement  = nullptr;
                    s_prevAttackTarget  = nullptr;
                    DebugLog("[WASDCombat] dc_loadgame_clear_movement");
                    s_savedFreeCameraMode    = false;
                    s_cameraLockInvSuspend   = false;
                    s_cameraLockTurretSuspend = false;
                    s_savedCamFollowOffY     = 0.0f;
                    DebugLog("[WASDCombat] dc_loadgame_clear_camera");
                    s_dcPtrLossActive = false;
                    s_loadGuardActive = true;
                    clearAllState();
                    s_vHud.label = nullptr;
                    s_vHud.shown = false;
                    s_hudReady   = false;
                    DebugLog("[WASDCombat] dc_loadgame_old_state_cleared");
                    DebugLog("[WASDCombat] load_guard_enabled");
                    DebugLog("[WASDCombat] dc_loadgame_shutdown_complete");
                }
                hudUpdate();
                return;
            }

            // ou is valid from here.
            if (s_userWantsDC)
            {
                // CRASH FIX (2026-06-13): a REAL save-load (isLoadingFromASaveGame)
                // frees the controlled character while ou->player may still be
                // valid.  The old preserve path below dereferences
                // s_freeMoveAnchor->movement (for speedOrders AND in the anchorOk
                // check), which reads freed memory and crashed when reloading
                // mid-capture.  A real save-load is NOT a chunk microload (those
                // only null ou->player without the load flag): drop the runtime
                // anchor IMMEDIATELY without touching it, skip ALL DC processing,
                // and run the game's loop so the load proceeds.  s_userWantsDC is
                // preserved, so the reacquire path restores DC once the load
                // completes.  Never deref the anchor while a save-load is active.
                if (dcRealLoad() || forceReal)
                {
                    if (!s_dcPtrLossActive)
                    {
                        s_dcPtrLossActive      = true;
                        s_dcPtrLossStartedAt   = GetTickCount64();
                        s_dcPtrLossLastLogTick = 0;
                        // Speed is NOT forced here any more.  charMovUpdate_hook
                        // caches the anchor's real speed tier every frame, so the
                        // reacquire restores the player's ACTUAL speed instead of a
                        // hard-coded RUN ("traveling load forces sprint", user
                        // 2026-07-31).  The anchor is freed on a save-load, so we must
                        // rely on that cached value here rather than deref it.
                        DebugLog("[WASDCombat] dc_realload_anchor_dropped_preserving_intent");
                    }
                    s_freeMoveAnchor    = nullptr;
                    s_anchorMovement    = nullptr;
                    s_selectedCharacter = nullptr;
                    s_selectedMovement  = nullptr;
                    hudUpdate();
                    if (ou->player) s_mainLoopOrig(thisptr, time);
                    return;
                }

                // DC intended — preserve through a chunk microload (player null,
                // no save-load flag).  Characters are NOT freed here, so the
                // anchor deref below is safe.
                if (!s_dcPtrLossActive)
                {
                    s_dcPtrLossActive      = true;
                    s_dcPtrLossStartedAt   = GetTickCount64();
                    s_dcPtrLossLastLogTick = 0;
                    if (s_freeMoveAnchor && s_freeMoveAnchor->movement && s_freeMoveAnchor->movement->speedOrders < GROUPED)
                        s_dcPreservedSpeedMode = s_freeMoveAnchor->movement->speedOrders;
                    {
                        char lb[96];
                        sprintf_s(lb, sizeof(lb), "[WASDCombat] dc_pointer_loss_preserving_user_intent speed=%d", (int)s_dcPreservedSpeedMode);
                        VerbLog(lb);
                    }
                    if (!ou->player)
                        DebugLog("[WASDCombat] dc_pointer_loss_preserved_microload");
                    if (ou->player && ou->player->isTrackingCharacter())
                        DebugLog("[WASDCombat] camera_lock_lost_during_long_stream");
                }

                // No timeout — pointer loss of any duration only pauses injection.
                // Reacquire loop runs indefinitely until pointer is valid or a true hard-shutdown fires.

                // Pointer-validity guard: skip injection if anchor or player is invalid.
                bool anchorAlive = (s_freeMoveAnchor != nullptr) && (s_freeMoveAnchor->movement != nullptr);
                bool anchorOk = (ou->player != nullptr) && anchorAlive;
                if (!anchorOk)
                {
                    // Build 63: the characters are NOT freed on a chunk microload,
                    // so the per-character movement hook keeps driving WASD while
                    // the player object is away (before: injection stopped and the
                    // character halted for the whole load).  Only the player-
                    // dependent DC processing below is skipped.
                    s_anchorMovement = anchorAlive ? s_freeMoveAnchor->movement : nullptr;
                    if (g_log.debugLogging)
                    {
                        ULONGLONG nowL = GetTickCount64();
                        if (nowL - s_dcPtrLossLastLogTick >= 500)
                        {
                            s_dcPtrLossLastLogTick = nowL;
                            char dlbuf[80];
                            sprintf_s(dlbuf, sizeof(dlbuf),
                                "[WASDCombat] dc_pointer_loss_duration_ms=%llu",
                                nowL - s_dcPtrLossStartedAt);
                            DebugLog(dlbuf);
                        }
                    }
                    hudUpdate();
                    if (ou->player) s_mainLoopOrig(thisptr, time);
                    return;
                }

                // All pointers valid — restore cache and fall through to DC processing.
                s_anchorMovement = s_freeMoveAnchor->movement;
                // Fall through — injection resumes normally in steps 5 and 9.
            }
            else
            {
                // DC not intended — normal load guard path.
                if (!s_loadGuardActive)
                {
                    if (!s_dcShutdownInProgress)
                    {
                        s_dcShutdownInProgress = true;
                        DebugLog("[WASDCombat] dc_loadgame_shutdown_begin");
                    }
                    s_healingJobActive             = false;
                    s_medicalJobSuppressedThisHold = false;
                    s_freeMoveAnchor               = nullptr;
                    s_anchorMovement               = nullptr;
                    s_selectedCharacter            = nullptr;
                    s_selectedMovement             = nullptr;
                    s_prevAttackTarget             = nullptr;
                    s_savedFreeCameraMode          = false;
                    s_cameraLockInvSuspend         = false;
                    s_cameraLockTurretSuspend      = false;
                    s_savedCamFollowOffY           = 0.0f;
                    s_loadGuardActive = true;
                    clearAllState();
                    s_vHud.label = nullptr;
                    s_vHud.shown = false;
                    s_hudReady   = false;
                    DebugLog("[WASDCombat] dc_loadgame_old_state_cleared");
                    DebugLog("[WASDCombat] load_guard_enabled");
                }
                hudUpdate();
                if (ou && ou->player) s_mainLoopOrig(thisptr, time);
                return;
            }
        }
        else if (s_dcPtrLossActive)
        {
            // loadSig cleared — run reacquire sequence.
            Character* chR = ou->player->selectedCharacter.getCharacter();
            // Build 65 (log 2026-09-30: after a 2.2 s chunk load the reacquired
            // selection read UNCONSCIOUS for 13 s and every WASD press was refused
            // until the user re-selected by double-click = a stale object): only
            // accept a character that is in the live update list; else stay in
            // pointer-loss and retry next frame.
            if (chR && s_userWantsDC && !dcInUpdateList(thisptr, chR))
            {
                ULONGLONG nowDf = GetTickCount64();
                if (nowDf - s_dcPtrLossLastLogTick >= 1000)
                {
                    s_dcPtrLossLastLogTick = nowDf;
                    char db[128];
                    sprintf_s(db, sizeof(db), "[WASDCombat] dc_reacquire_deferred ptr=%p not_in_update_list loss_ms=%llu",
                              (void*)chR, nowDf - s_dcPtrLossStartedAt);
                    DebugLog(db);
                }
                hudUpdate();
                s_mainLoopOrig(thisptr, time);
                return;
            }
            s_dcPtrLossActive = false;
            if (chR && s_userWantsDC)
            {
                s_reacquireMs = GetTickCount64();
                {
                    char rb[192];
                    sprintf_s(rb, sizeof(rb), "[WASDCombat] dc_reacquire_after_long_stream speed_now=%d preserved=%d loss_ms=%llu ptr=%p unc=%d prone=%d dead=%d",
                              chR->movement ? (int)chR->movement->speedOrders : -1, (int)s_dcPreservedSpeedMode,
                              GetTickCount64() - s_dcPtrLossStartedAt, (void*)chR,
                              chR->isUnconcious() ? 1 : 0, (int)chR->getProneState(), chR->isDead() ? 1 : 0);
                    DebugLog(rb);
                }
                s_freeMoveAnchor = chR;
                s_anchorMovement = chR->movement;
                s_dcPtrLossLastLogTick = 0;
                if (chR->movement && s_dcPreservedSpeedMode < GROUPED)
                {
                    chR->movement->setDesiredSpeedOrders(s_dcPreservedSpeedMode);
                    chR->movement->setDesiredSpeed(s_dcPreservedSpeedMode);
                }
                if (s_mode != MODE_FREE_MOVE)
                    s_mode = MODE_FREE_MOVE;
                if (ou->player->isTrackingCharacter())
                {
                    DebugLog("[WASDCombat] camera_lock_restore_skipped_already_locked");
                }
                else
                {
                    ou->player->startTrackCharacter(s_freeMoveAnchor);
                    DebugLog("[WASDCombat] camera_lock_restored_after_long_stream");
                }
                DebugLog("[WASDCombat] dc_restored_after_long_stream");
            }
        }

        // s_loadGuardActive here means the shutdown countdown was handled inside
        // the s_dcShutdownInProgress block above (DC-shutdown path).  If we reach
        // this point with s_loadGuardActive still true it means the flag was set
        // without s_dcShutdownInProgress (should not occur after this fix), so
        // clear it safely to avoid being stuck.
        if (s_loadGuardActive)
        {
            s_loadGuardActive        = false;
            s_postLoadReacquire      = true;
            s_stabilizationCountdown = 0;
            DebugLog("[WASDCombat] reload_complete_reacquire_started");
        }
    }

    // NEWGAME detection — mainLoop signal poll.
    // Reachable only after safety gate confirms ou and ou->player are valid.
    // sm->signal == NEWGAME (0x4) is set by SaveManager::newGame() before any world teardown.
    // LOADGAME/Continue set signal == LOADGAME (0x2) — cannot produce a false positive here.
    // Gated on DC being active: no-op when DC was never enabled this session.
    // Does NOT set s_dcShutdownInProgress or s_loadGuardActive; LOADGAME machinery handles those
    // after world teardown begins normally.
    {
        SaveManager* sm = SaveManager::getSingleton();
        if (sm && sm->signal == SaveManager::NEWGAME
            && (s_mode == MODE_FREE_MOVE || s_userWantsDC || s_freeMoveAnchor != nullptr))
        {
            DebugLog("[WASDCombat] dc_newgame_signal_seen");
            DebugLog("[WASDCombat] dc_newgame_detection_source=mainloop_signal_poll");
            DebugLog("[WASDCombat] dc_newgame_soft_shutdown_begin");

            // Destroy user intent first so no subsequent path can re-enter DC.
            s_userWantsDC = false;
            s_userWantsFP = false;   // new game = hard teardown; never re-enter FP into it

            // Tear down OTS while the anchor + camera are still valid: exitOTS
            // re-attaches the DETACHED camera to the rig (otherwise char
            // creation shows no character — the camera stays orphaned on our
            // node) and restores the view-floor; also drop the face-cam state
            // (it otherwise breaks in the new game).  Field 2026-06-15:
            // new-game-while-in-a-save left the OTS camera/face-cam stranded.
            if (s_fpActive) exitOTS(true);
            if (s_firstPersonActive) exitFirstPerson(true);
            // The DETACHED third-person camera too (2026-09-06 rebuild has its
            // own flag; user 2026-09-14: new game started in OTS showed no
            // characters in creation and a stuck crosshair until DC was
            // toggled).  Re-attach while the camera is valid, drop the CTRL
            // toggle so nothing re-enters, and release the crosshair/pointer.
            if (s_otsCamActive) exitOtsCam(true);
            s_camRotateToggle = false;
            dcUpdateCrosshair(false);
            s_fpSuspendedForInv = false;
            s_otsInvFaceActive = false;
            s_otsInvFaceChar   = nullptr;

            // Stop camera tracking and restore freecam state before clearing pointers.
            if (ou->player)
            {
                ou->player->stopTrackCharacter();
                if (ou->player->camera)
                {
                    ou->player->camera->setFreeCameraMode(s_savedFreeCameraMode);
                    ou->player->camera->objectCurrentlyFollowingOffset.y = s_savedCamFollowOffY;
                }
            }
            // Force mode and tracked-mode to VANILLA together so step-3 exit-transition
            // does not re-run stopTrackCharacter or show "Direct Control Disabled".
            s_mode          = MODE_VANILLA;
            s_fmTrackedMode = MODE_VANILLA;
            DebugLog("[WASDCombat] dc_newgame_forced_vanilla_mode");

            // Clear anchor and selection pointers before world teardown can free them.
            s_freeMoveAnchor    = nullptr;
            s_anchorMovement    = nullptr;
            s_selectedCharacter = nullptr;
            s_selectedMovement  = nullptr;
            s_prevAttackTarget  = nullptr;

            // Clear camera suspension state.
            s_savedFreeCameraMode     = false;
            s_savedCamFollowOffY      = 0.0f;
            s_cameraLockInvSuspend    = false;
            s_cameraLockTurretSuspend = false;
            s_menuSuspendActive       = false;

            // Clear WASD and movement state.
            s_wHeld               = false;
            s_aHeld               = false;
            s_sHeld               = false;
            s_dHeld               = false;
            s_xPressed            = false;
            s_wasdWasActive       = false;
            s_wasdMovementApplied = false;
            s_prevWasdDir         = Ogre::Vector3::ZERO;
            s_wasdTapStartMs      = 0;
            s_wasdLastHeldMs      = 0;
            s_frameMode           = MODE_VANILLA;
            s_frameWasdHeld       = false;

            // Clear loot UI suspension.
            s_lootUiSuspendActive  = false;
            s_lootUiWasPrevOpen    = false;
            s_lootSuspendStartTick = 0;

            // Clear healing and committed-action flags.
            s_healingJobActive             = false;
            s_healingJobPending            = false;
            s_medicalJobSuppressedThisHold = false;

            // Clear micro-load state — NEWGAME is not a micro-load.
            s_dcPtrLossActive      = false;
            s_dcPtrLossStartedAt   = 0;
            s_dcPtrLossLastLogTick = 0;

            DebugLog("[WASDCombat] dc_newgame_old_state_cleared");
            DebugLog("[WASDCombat] dc_newgame_soft_shutdown_complete");
        }
    }

    // Loot UI suspension — detect any open inventory window.
    // s_mode is untouched; all V-Mode processing suspends while any inventory is open.
    // Diagnostic logs identify which specific inventory type was detected.
    {
        bool anyInvOpen      = gui && gui->isAnyInventoryWindowOpen();
        int  numInvOpen      = gui ? gui->getNumOpenInventoryWindows() : 0;
        bool npcFieldOpen    = gui && gui->inventoryWindowNPC.getCharacter()       != nullptr;
        bool charFieldOpen   = gui && gui->inventoryWindowCharacter.getCharacter() != nullptr;
        bool traderFieldOpen = gui && gui->inventoryWindowTrader.getCharacter()    != nullptr;
        bool tradeAOpen      = gui && gui->tradeA.getCharacter()                   != nullptr;
        bool tradeBOpen      = gui && gui->tradeB.getCharacter()                   != nullptr;


        // Pre-open pause memory: pause state from the last frame with NO
        // inventory window open — by definition the state from BEFORE any open
        // edge, where Kenshi's auto-pause cannot have applied yet.  True at an
        // open edge = the pause is the PLAYER'S own (2026-08-31 fix: opening an
        // inventory while manually paused force-unpaused the game — the
        // open-edge grace misread the pre-existing pause as the auto-pause;
        // the latch could never be set because it only latches DURING a
        // move-through session).
        if (ou && gui && !gui->isAnyInventoryWindowOpen())
            s_invPausedBeforeOpen = ou->isPaused();

        // Move-through eligibility (InventoryFaceCam=false): keep DC live + game
        // running instead of suspending.  Evaluated each frame.
        bool moveThrough = invMoveThroughEligible();

        // Suspension trigger: any open inventory window.
        // showTradeWindow_hook may have already set s_lootUiSuspendActive before
        // the window was visible; this block confirms open/close state and manages
        // s_lootUiWasPrevOpen for edge detection.
        if (moveThrough)
        {
            // --- Inventory MOVE-THROUGH: DC stays live, game keeps running ---
            // Lift any early suspend the trade hook set, and un-suspend the camera
            // lock so movement + tracking continue while the inventory UI is open.
            if (s_lootUiSuspendActive)
            {
                s_lootUiSuspendActive  = false;
                s_lootSuspendStartTick = 0;
            }
            if (s_cameraLockInvSuspend)
            {
                s_cameraLockInvSuspend = false;
                if (s_freeMoveAnchor && ou->player)
                    ou->player->startTrackCharacter(s_freeMoveAnchor);
            }
            // Kenshi auto-pauses on inventory open and again when the shown
            // inventory switches to another squad member.  Defeat ONLY those
            // auto-pauses (grace window after each open/switch edge) so injected
            // WASD physically moves the character.  A pause appearing OUTSIDE
            // the grace is the player's own — respect it, including across
            // subsequent inventory switches, until the player unpauses (user
            // req 2026-08-05: manual pause must work in move-through mode).
            Character* mtShownChar = gui->inventoryWindowCharacter.getCharacter();
            bool mtOpenEdge   = !s_invMoveThroughActive;
            bool mtSwitchEdge = s_invMoveThroughActive
                                && mtShownChar != s_invMoveThroughShownChar;
            s_invMoveThroughShownChar = mtShownChar;   // identity only
            // Open edge with a PRE-EXISTING pause: the player paused before the
            // inventory existed, so it cannot be the auto-pause — latch it as
            // theirs up front, which also blocks the grace re-arm below.
            if (mtOpenEdge && s_invPausedBeforeOpen && ou && ou->isPaused())
            {
                s_invMoveThroughPlayerPaused = true;
                DebugLog("[WASDCombat] inv_move_through_preexisting_pause_respected");
            }
            if (mtOpenEdge || mtSwitchEdge)
            {
                // A paused player switching inventories stays paused: no grace
                // re-arm, so the auto-pause (a no-op on a paused game) is never
                // "defeated" out from under them.
                if (!s_invMoveThroughPlayerPaused)
                    s_invMoveThroughEdgeTick = GetTickCount64();
                if (mtSwitchEdge)
                    DebugLog("[WASDCombat] inv_move_through_switch_edge");
            }
            if (ou && ou->isPaused())
            {
                if (!s_invMoveThroughPlayerPaused
                    && s_invMoveThroughEdgeTick != 0
                    && GetTickCount64() - s_invMoveThroughEdgeTick
                           <= INV_MT_AUTOPAUSE_GRACE_MS)
                {
                    ou->userPause(false);              // Kenshi's auto-pause
                    s_invMoveThroughForcedRun = true;
                }
                else if (!s_invMoveThroughPlayerPaused)
                {
                    s_invMoveThroughPlayerPaused = true;
                    DebugLog("[WASDCombat] inv_move_through_player_pause_respected");
                }
            }
            else if (s_invMoveThroughPlayerPaused)
            {
                s_invMoveThroughPlayerPaused = false;  // player unpaused
                DebugLog("[WASDCombat] inv_move_through_player_unpause");
            }
            if (!s_invMoveThroughActive)
            {
                s_invMoveThroughActive = true;
                DebugLog("[WASDCombat] inv_move_through_begin");
            }
            s_lootUiWasPrevOpen = true;   // track open so the close edge is clean

            // Merchant trade window: vanilla won't close it on distance, so close
            // it ourselves once the controlled character has WALKED far enough from
            // where the trade opened.  Corpse/own loot close natively, so only the
            // trader window is handled here.  Skip player-squad trades (trader is
            // your own squadmate) — those must NOT auto-close.
            Character* trader = gui->inventoryWindowTrader.getCharacter();
            if (trader && !trader->isPlayerCharacter() && !s_invTradeCloseRequested)
            {
                Ogre::Vector3 ap = s_freeMoveAnchor->getPosition();
                if (!s_invTradeStartValid)
                {
                    s_invTradeAnchorStart = ap;   // spot where the trade opened
                    s_invTradeStartValid  = true;
                }
                else
                {
                    float dx = ap.x - s_invTradeAnchorStart.x;
                    float dz = ap.z - s_invTradeAnchorStart.z;
                    if (dx * dx + dz * dz > INV_TRADE_AUTOCLOSE_DIST_SQ)
                    {
                        gui->closeTradeWindow();
                        s_invTradeCloseRequested = true;
                        DebugLog("[WASDCombat] inv_trade_autoclosed_distance");
                    }
                }
            }
        }
        else if (s_invMoveThroughActive)
        {
            // --- Move-through session ending: inventory closed, dialogue began,
            //     DC toggled off, or face-cam re-enabled.  Leave the game running
            //     (player owns pause now); just clear the move-through state. ---
            s_invMoveThroughActive    = false;
            s_invMoveThroughForcedRun = false;
            s_invMoveThroughPlayerPaused = false;
            s_invMoveThroughShownChar    = nullptr;
            s_invMoveThroughEdgeTick     = 0;
            s_invTradeCloseRequested  = false;
            s_invTradeStartValid      = false;
            s_tradeWindowActive       = false;
            s_lootUiWasPrevOpen       = false;
            s_lootUiSuspendActive     = false;
            s_lootSuspendStartTick    = 0;
            DebugLog("[WASDCombat] inv_move_through_end");
        }
        else if (anyInvOpen && !s_lootUiWasPrevOpen)
        {
            // Window is now confirmed open.  Hook may have already set suspension;
            // if not (non-trade path such as showInventoryNPC), set it now.
            if (!s_lootUiSuspendActive)
            {
                s_lootUiSuspendActive  = true;
                s_lootSuspendStartTick = GetTickCount64();
            }
            // NOTE: do NOT re-derive s_tradeWindowActive from the npc/trader/
            // tradeA/tradeB fields here — they stay STALE after a trade closes
            // (field 2026-06-17: tradeA=1 tradeB=1 persisted across later own-
            // inventory opens, re-flagging them as trades and killing the face-
            // cam).  showTradeWindow_hook is the authoritative trade latch; it
            // fires for shop AND corpse/loot, and we clear it on the close edge.
            s_lootUiWasPrevOpen = true;
            DebugLog("[WASDCombat] loot_ui_open_suspend_vmode");
            if (s_mode == MODE_FREE_MOVE && s_freeMoveAnchor && !s_cameraLockInvSuspend)
            {
                s_cameraLockInvSuspend = true;
                DebugLog("[WASDCombat] camera_lock_suspended_inventory_open");
            }
        }
        else if (!anyInvOpen && s_lootUiWasPrevOpen)
        {
            // Window closed — trade/loot session is over: clear the trade latch
            // immediately (before the suspension debounce) so the next OWN
            // inventory isn't mis-classified.
            s_tradeWindowActive = false;
            // Window closed — enforce debounce before releasing suspension.
            ULONGLONG elapsed = GetTickCount64() - s_lootSuspendStartTick;
            if (elapsed >= LOOT_SUSPEND_DEBOUNCE_MS)
            {
                s_lootUiSuspendActive = false;
                s_lootUiWasPrevOpen   = false;
                DebugLog("[WASDCombat] loot_ui_closed_restore_vmode");
                if (s_mode == MODE_FREE_MOVE && s_freeMoveAnchor && s_cameraLockInvSuspend && ou->player)
                {
                    s_cameraLockInvSuspend = false;
                    DebugLog("[WASDCombat] camera_lock_restored_after_inventory");
                    ou->player->startTrackCharacter(s_freeMoveAnchor);
                }
            }
            else
            {
            }
        }
        else if (s_lootUiSuspendActive && !s_lootUiWasPrevOpen && s_lootSuspendStartTick > 0)
        {
            // Hook fired but window never appeared (cancelled interaction).
            // Release after 1 s to avoid a stuck suspension.
            ULONGLONG elapsed = GetTickCount64() - s_lootSuspendStartTick;
            if (elapsed >= 1000)
            {
                s_lootUiSuspendActive  = false;
                s_lootSuspendStartTick = 0;
                s_tradeWindowActive    = false;   // cancelled before any window opened
            }
        }
    }

    // DC anchor self-heal: after a reload the anchor can be lost while DC stays
    // on (s_mode FREE_MOVE) until the player manually toggles DC off/on (field
    // 2026-06-15).  Re-grab it from the live selection so DC (and the inventory
    // face-cam, which needs the anchor) stays available without that dance.
    if (s_mode == MODE_FREE_MOVE && !s_lootUiSuspendActive && !s_freeMoveAnchor
        && !s_dcShutdownInProgress && !s_loadGuardActive && ou && ou->player)
    {
        Character* sel = s_selectedCharacter;
        if (sel && sel->movement && sel->isPlayerCharacter())
        {
            s_freeMoveAnchor = sel;
            s_anchorMovement = sel->movement;
            ou->player->startTrackCharacter(s_freeMoveAnchor);
            DebugLog("[WASDCombat] dc_anchor_reacquired_selfheal");
        }
    }

    // Safety: the inventory face-cam manages its own enter/exit in
    // cameraUpdate_hook; if it is somehow left active here with no inventory
    // open, drop straight back to the vanilla camera.
    if (s_fpActive && !isOwnInventoryOpen())
        exitOTS(true);

    // Native command registration happens HERE, not in the loadConfig hook:
    // the game loads its keyboard config before RE_Kenshi loads plugins, so
    // that hook never fires on a normal launch (field finding, log-proven).
    if (!s_nativeCommandsRegistered)
        registerNativeCommands(key);

    // Movement flags come from the poll thread (always — see the native
    // keybind block comment).  Only the toggle/speed persistence watchdog
    // runs here: it saves rebinds even if the options menu never calls
    // saveOptions, and logs whether rebinds actually reach Command::bound.
    if (s_nativeCommandsRegistered)
        watchNativeBindChanges();

    // Frame snapshot — read all volatile input state once before any per-character
    // hooks run inside s_mainLoopOrig.  Hooks use s_frame* instead of re-reading
    // volatiles, eliminating memory fence overhead for 100+ hook calls per frame.
    s_frameMode        = s_mode;
    s_frameWasdHeld    = s_wHeld || s_aHeld || s_sHeld || s_dHeld;
    s_frameLootSuspend = s_lootUiSuspendActive;

    // 2. Selection tracking.
    {
        Character*    ch = ou->player->selectedCharacter.getCharacter();
        CharMovement* mv = ch ? ch->movement : nullptr;
        if (ch != s_selectedCharacter || mv != s_selectedMovement)
        {
            s_selectedCharacter = ch;
            s_selectedMovement  = mv;
        }

        if (s_postLoadReacquire && ch)
        {
            s_postLoadReacquire = false;
            DebugLog("[WASDCombat] reload_reacquire_complete");
            if (s_mode == MODE_FREE_MOVE && s_freeMoveAnchor)
            {
                DebugLog("[WASDCombat] camera_lock_retarget_after_load");
                ou->player->startTrackCharacter(s_freeMoveAnchor);
            }
        }
    }

    // 3. V-Mode transition.
    { LONGLONG _clStart = qpcNow();
    {
        ControlMode curMode = s_mode;
        bool curInVM  = (curMode         == MODE_FREE_MOVE);
        bool prevInVM = (s_fmTrackedMode == MODE_FREE_MOVE);

        if (curInVM && !prevInVM && !s_lootUiSuspendActive)
        {
            s_freeMoveAnchor      = s_selectedCharacter;
            s_anchorMovement      = s_freeMoveAnchor ? s_freeMoveAnchor->movement : nullptr;
            s_wasdWasActive       = false;
            s_combatWASDLogged    = false;
            s_retreatLogged       = false;
            s_squadThreat         = false;
            s_consciousAllyThreat = false;
            s_enemyTargetingLogged = false;
            s_lastScanTick        = 0;
            // Fresh V-mode = full vanilla until the first WASD release; an
            // in-flight point-click order continues normally.
            s_wasdHoldActive         = false;
            s_playerPointClickActive = false;
            s_holdPosValid           = false;
            s_idleHoldEngaged        = false;
            s_camRotateToggle        = false;   // start DC with rotate-toggle off

            if (s_freeMoveAnchor && ou->player->camera)
            {
                s_savedFreeCameraMode = ou->player->camera->isFreeCameraMode();
                DebugLog("[WASDCombat] camera_mode_saved");
                ou->player->camera->setFreeCameraMode(false);
                ou->player->startTrackCharacter(s_freeMoveAnchor);
                s_savedCamFollowOffY = ou->player->camera->objectCurrentlyFollowingOffset.y;
                DebugLog("[WASDCombat] camera_lock_enabled_dc");
            }
            ou->showPlayerAMessage("Direct Control Enabled", false);
        }
        else if (!curInVM && prevInVM && !s_lootUiSuspendActive)
        {
            // DC turned off — drop the OTS camera first (re-attach to the rig
            // while the anchor is still valid), then restore vanilla camera.
            if (s_fpActive) exitOTS(true);
            if (s_firstPersonActive) exitFirstPerson(true);
            // The DETACHED third-person camera too (2026-09-06 rebuild has its
            // own flag; user 2026-09-14: new game started in OTS showed no
            // characters in creation and a stuck crosshair until DC was
            // toggled).  Re-attach while the camera is valid, drop the CTRL
            // toggle so nothing re-enters, and release the crosshair/pointer.
            if (s_otsCamActive) exitOtsCam(true);
            s_camRotateToggle = false;
            dcUpdateCrosshair(false);
            s_fpSuspendedForInv = false;
            ou->player->stopTrackCharacter();
            if (ou->player->camera)
            {
                ou->player->camera->setFreeCameraMode(s_savedFreeCameraMode);
                ou->player->camera->objectCurrentlyFollowingOffset.y = s_savedCamFollowOffY;
            }
            s_savedFreeCameraMode     = false;
            s_savedCamFollowOffY      = 0.0f;
            s_cameraLockInvSuspend    = false;
            s_cameraLockTurretSuspend = false;
            // Restore vanilla hold-to-rotate: drop the toggle and clear the forced
            // rotate flag so the camera doesn't stay rotating in vanilla mode.
            s_camRotateToggle         = false;
            if (key) key->rotate      = false;
            DebugLog("[WASDCombat] camera_lock_disabled_restore_freecam");
            s_freeMoveAnchor        = nullptr;
            s_anchorMovement      = nullptr;
            s_wasdWasActive       = false;
            s_combatWASDLogged    = false;
            s_retreatLogged       = false;
            s_squadThreat         = false;
            s_consciousAllyThreat = false;
            s_enemyTargetingLogged = false;
            s_lastScanTick        = 0;
            s_wasdHoldActive         = false;
            s_playerPointClickActive = false;
            s_holdPosValid           = false;
            s_idleHoldEngaged        = false;
            ou->showPlayerAMessage("Direct Control Disabled", false);
        }
        else if (curInVM && s_freeMoveAnchor && !s_lootUiSuspendActive)
        {
            Character* sel = s_selectedCharacter;
            // Switch WASD control to a different squad member without leaving DC,
            // two ways: DOUBLE-CLICK their portrait (user req 2026-06-20) or press
            // F while they are selected/highlighted (user req 2026-06-28).  A single
            // click only selects (vanilla), so other NPCs can be inspected/ordered
            // without stealing control (Sentient Sands compat).  Honor a poll-thread
            // double-click within the last ~600ms or a pending F edge, then consume
            // so the switch fires exactly once.
            ULONGLONG dcMs       = s_lmbDoubleClickMs;
            bool      dcSwitch   = (dcMs > 0 && (GetTickCount64() - dcMs) <= 600);
            bool      fSwitch    = s_fSelectEdge;
            bool      wantSwitch = dcSwitch || fSwitch;
            if (wantSwitch && sel && sel != s_freeMoveAnchor && sel->isPlayerCharacter())
            {
                s_lmbDoubleClickMs    = 0;   // consumed
                s_fSelectEdge         = false;
                // First-person head/hair hide is PER-CHARACTER: it was applied to the
                // OUTGOING anchor's own skeleton/appearance.  Restore it on the old
                // anchor BEFORE the pointer moves, or that character keeps a shrunk
                // head (user 2026-07-31: "head of the previous controlled character
                // will still be shrunk") and shaved hair.  fpSetHeadBoneHidden always
                // acts on the CURRENT s_freeMoveAnchor, so the order must be
                // restore-old -> switch -> re-hide-new.
                if (s_firstPersonActive && s_fpHeadBoneHidden)
                    fpSetHeadBoneHidden(false);                 // acts on OLD anchor
                if (s_firstPersonActive && (s_fpHideHeadgear || s_fpHideHair))
                    fpSetBeardHidden(false, false);             // OLD anchor's headwear back
                if (s_firstPersonActive && (s_fpHideHead || s_fpHeadPixelHide))
                    fpSetHeadMask(false);                       // OLD anchor's mask re-derived
                if (s_firstPersonActive && s_fpHairHidden)
                {
                    AppearanceBase* apOld = s_freeMoveAnchor->getAppearance();
                    if (apOld) apOld->shaveHead(false);
                    s_fpHairHidden = false;
                    DebugLog("[WASDCombat] dc_fp_hair_restored_on_switch");
                }
                s_freeMoveAnchor      = sel;
                s_anchorMovement      = sel->movement;
                s_combatWASDLogged    = false;
                s_retreatLogged       = false;
                s_squadThreat         = false;
                s_consciousAllyThreat = false;
                s_enemyTargetingLogged = false;
                s_lastScanTick          = 0;
                // A newly controlled character has not been WASD-parked;
                // its vanilla orders continue until the first release.
                s_wasdHoldActive         = false;
                s_playerPointClickActive = false;
                s_holdPosValid           = false;
                s_idleHoldEngaged        = false;
                DebugLog(fSwitch ? "[WASDCombat] camera_lock_retarget_selection_fkey"
                                 : "[WASDCombat] camera_lock_retarget_selection_doubleclick");
                ou->player->startTrackCharacter(s_freeMoveAnchor);
                // Re-apply the FP head/hair hide to the NEW anchor so first-person
                // still hides the now-controlled character's own head/hair.
                if (s_firstPersonActive && (s_fpHideHead || s_fpPixelFallbackOn))
                    fpSetHeadBoneHidden(true);                  // acts on NEW anchor
                if (s_firstPersonActive && (s_fpHideHeadgear || s_fpHideHair))
                    fpSetBeardHidden(true, true);               // NEW anchor's headwear hidden
                if (s_firstPersonActive && (s_fpHideHead || s_fpHeadPixelHide))
                    fpSetHeadMask(true);                        // NEW anchor's pixel mask
                if (s_firstPersonActive && s_fpHideHair)
                {
                    AppearanceBase* apNew = s_freeMoveAnchor->getAppearance();
                    if (apNew) { apNew->shaveHead(true); s_fpHairHidden = true;
                        DebugLog("[WASDCombat] dc_fp_hair_hidden_on_switch"); }
                }
            }
            else if (fSwitch)
            {
                // F pressed with no valid target (same char, non-player, or no
                // selection) — consume the edge so it cannot fire on a later frame.
                s_fSelectEdge = false;
            }
        }
        s_fmTrackedMode = curMode;
    }

    // First-person toggle (P): consume the poll-thread edge here on the GAME
    // thread, where the camera + anchor are valid (the poll thread must never
    // touch the camera).  Enter only in DC with a live anchor and when the
    // inventory face-cam does not already own the camera; exiting is always
    // allowed.  A manual toggle also cancels a pending suspend-for-inventory
    // auto-return so the camera doesn't snap back to first-person on close.
    if (s_fpToggleRequested)
    {
        s_fpToggleRequested = false;
        if (s_firstPersonActive)
        {
            s_userWantsFP = false;         // user turned FP off — clear the persistent intent
            exitFirstPerson(true);
        }
        else if (s_fpSuspendedForInv)
        {
            s_userWantsFP = false;         // cancelling the auto-return = user wants FP off
            s_fpSuspendedForInv = false;   // cancel the pending auto-return
        }
        else if (s_mode == MODE_FREE_MOVE && !s_fpActive && !s_lootUiSuspendActive
                 && s_freeMoveAnchor && s_freeMoveAnchor->movement)
        {
            s_userWantsFP = true;          // user turned FP on — persist through loads
            enterFirstPerson();
        }
    }

    // FP LOAD-PERSISTENCE self-heal: if the user wants first-person but a load/stream
    // dropped it, re-enter once the scene + DC anchor are stable again.  enterFirstPerson
    // self-guards + is idempotent; gated so it never fights the inventory face-cam suspend,
    // the OTS restore, a menu, or a still-loading scene.  This is what makes FP survive
    // chunk loads (user 2026-07-29).
    if (s_userWantsFP && !s_firstPersonActive && !s_fpActive && !s_fpSuspendedForInv
        && !s_otsRestorePending && s_mode == MODE_FREE_MOVE
        && !s_dcShutdownInProgress && !s_loadGuardActive
        && ou && ou->player && ou->player->camera && !dcRealLoad()
        && s_freeMoveAnchor && s_freeMoveAnchor->movement)
    {
        DebugLog("[WASDCombat] fp_reentered_after_load");
        enterFirstPerson();
    }

    // Sneak toggle (Shift+C while first-person, user req 2026-08-01): consume the
    // poll-thread edge here on the game thread.  Drive the game's OWN sneak
    // button handler (OrdersPanel::toggleStealth — the exact path a mouse click
    // on the SNEAK button takes), so the UI checkbox, the standing order, and
    // the character's stealth state all stay in sync (user 2026-08-01: direct
    // setStealthMode toggled sneak but left the SNEAK button visually off).
    // Same precedent as the X speed key driving OrdersPanel::speedNext.  Falls
    // back to the raw state switch if the panel isn't showing the anchor (no UI
    // to sync in that case).  Stealth skill, detection, and XP all run vanilla
    // (Rule 1: DC invokes the system, it does not reimplement it); WASD speed
    // while sneaking is capped to the real stealth speed in wasdMoveLimit.
    // Sneak deliberately persists across FP/DC exit — it is vanilla state the
    // player can also clear via the sneak button.
    if (s_sneakToggleRequested)
    {
        s_sneakToggleRequested = false;
        if ((s_firstPersonActive || s_otsCamActive) && s_freeMoveAnchor && s_freeMoveAnchor->movement)
        {
            OrdersPanel* op = (gui && gui->mainbar) ? gui->mainbar->ordersDataPanel : nullptr;
            if (op && op->stealthCheckBox
                && op->ordersCharacter.getCharacter() == s_freeMoveAnchor)
            {
                op->toggleStealth(op->stealthCheckBox);
            }
            else
            {
                s_freeMoveAnchor->setStealthMode(!s_freeMoveAnchor->isStealthMode());
            }
            char sbuf[80];
            sprintf_s(sbuf, sizeof(sbuf),
                "[WASDCombat] dc_fp_sneak_toggle on=%d viaPanel=%d",
                s_freeMoveAnchor->isStealthMode() ? 1 : 0,
                (op && op->stealthCheckBox) ? 1 : 0);
            DebugLog(sbuf);
        }
    }

    // Enemy body-clip clearance sampling (first-person combat, 2026-08-01): find
    // the nearest LIVE hostile to the anchor each frame; fpDriveFrame turns it
    // into an eye pullback + near-clip tighten so an aggressor pressing into the
    // camera can't slice the view open.  Cost control: skipped entirely unless FP
    // is active AND the 5s threat scan saw enemies (or the anchor is in combat);
    // inside the loop a coarse squared-distance cull runs before any game call,
    // so the per-frame work is a few subtractions per loaded character.
    s_fpEnemyNearestDist = -1.0f;
    if (s_firstPersonActive && s_fpEnemyClearRadius > 0.0f
        && s_mode == MODE_FREE_MOVE && s_freeMoveAnchor && s_freeMoveAnchor->movement
        && (s_nearbyEnemyCount > 0
            || s_freeMoveAnchor->isInCombatMode(true, true)))
    {
        const Ogre::Vector3 anchorPos = s_freeMoveAnchor->movement->pos;
        const float cullR  = s_fpEnemyClearRadius * 2.0f;  // coarse pre-cull ring
        const float cullR2 = cullR * cullR;
        float best2 = -1.0f;
        auto& scanChars = thisptr->getCharacterUpdateList();
        for (auto it = scanChars.begin(); it != scanChars.end(); ++it)
        {
            Character* c = *it;
            if (!c || c == s_freeMoveAnchor || !c->movement) continue;
            float dx = c->movement->pos.x - anchorPos.x;
            float dz = c->movement->pos.z - anchorPos.z;
            float d2 = dx * dx + dz * dz;
            if (d2 >= cullR2) continue;
            if (best2 >= 0.0f && d2 >= best2) continue;
            if (c->isDead() || c->isUnconcious()) continue;   // bodies underfoot: loot freely
            if (!c->isEnemy(s_freeMoveAnchor, true)) continue;
            best2 = d2;
        }
        if (best2 >= 0.0f)
            s_fpEnemyNearestDist = sqrtf(best2);
    }

    // DIAG (2026-09-14): STEEP-WALK telemetry.  User: after minutes of OTS travel
    // the character walks up any mountain (also once at 3x on 09-12); a quick
    // save+load clears it.  Sample the anchor every 250 ms; when the path grade
    // exceeds the engine's own ~0.7 wall while moving, dump the state that could
    // explain a bypassed physics controller.  1 s throttle, DC anchor only.
    if (s_freeMoveAnchor && s_freeMoveAnchor->movement && ou && ou->player)
    {
        static Ogre::Vector3 s_swLast(0.0f, 0.0f, 0.0f);
        static ULONGLONG     s_swLastMs = 0, s_swLogMs = 0;
        static bool          s_swValid = false;
        ULONGLONG nowW = GetTickCount64();
        if (!s_swValid || nowW - s_swLastMs >= 250)
        {
            Ogre::Vector3 pW = s_freeMoveAnchor->movement->pos;
            if (s_swValid)
            {
                float dx = pW.x - s_swLast.x, dz = pW.z - s_swLast.z, dy = pW.y - s_swLast.y;
                float h  = sqrtf(dx * dx + dz * dz);
                if (h > 2.0f && h < 200.0f && dy / h > 0.75f && nowW - s_swLogMs >= 1000)
                {
                    s_swLogMs = nowW;
                    // (getCameraNode() sits at the world origin — its distance was
                    //  just |pos|; measure the ORBIT CENTRE and the real lens instead.)
                    float centerDist = -1.0f, camDist = -1.0f;
                    bool  following  = false;
                    CameraClass* camW = ou->player->camera;
                    if (camW)
                    {
                        centerDist = camW->getCenter().distance(pW);
                        if (camW->camera) camDist = camW->camera->getDerivedPosition().distance(pW);
                        following = camW->getFollowObject().isValid();
                    }
                    CharMovement* mvW = s_freeMoveAnchor->movement;
                    char wb[300];
                    sprintf_s(wb, sizeof(wb),
                        "[WASDCombat] dc_steep_walk grade=%.2f h=%.1f dy=%.1f ots=%d fp=%d indoors=%d bld=%d ragdoll=%d mode=%d stopped=%d loading=%d centerDist=%.0f camDist=%.0f following=%d pursuit=%d sinceTeleMs=%llu wasd=%d",
                        dy / h, h, dy, s_otsCamActive ? 1 : 0, s_firstPersonActive ? 1 : 0,
                        mvW->isIndoors() ? 1 : 0, mvW->building.getBuilding() ? 1 : 0,
                        s_freeMoveAnchor->isRagdoll() ? 1 : 0, (int)mvW->movementMode,
                        mvW->officiallyStopped ? 1 : 0,
                        ou->isLoadingFromASaveGame() ? 1 : 0, centerDist, camDist, following ? 1 : 0,
                        s_settingEnemyPursuit ? 1 : 0,
                        s_lastRigTeleportMs ? (unsigned long long)(nowW - s_lastRigTeleportMs) : 0ULL,
                        s_frameWasdHeld ? 1 : 0);
                    DebugLog(wb);
                }
            }
            s_swLast = pW; s_swLastMs = nowW; s_swValid = true;
        }
    }

    // AI MOTION FEED v2 — frame-level velocity, computed ONCE per render frame
    // from the anchor's mainLoop-to-mainLoop pos delta (CharMovement::update
    // substeps make per-update-call deltas unreliable; this one is not).
    // Consumed by the publish in charMovUpdate_hook and the end-of-frame publish
    // at the bottom of this hook.
    {
        bool drivingAF = (s_mode == MODE_FREE_MOVE) && s_freeMoveAnchor
                       && s_freeMoveAnchor->movement && s_wasdMovementApplied
                       && s_settingEnemyPursuit;
        if (drivingAF)
        {
            const Ogre::Vector3 pNow = s_freeMoveAnchor->movement->pos;
            if (s_aiPubValid && time > 0.0001f)
            {
                Ogre::Vector3 v = (pNow - s_aiPubLastPos) / time;
                float sp = v.length();
                if (sp < 400.0f) { s_aiPubVel = v; s_aiPubSpd = sp; }
                else if (g_log.debugLogging)
                {
                    // Gated + throttled (was unconditional per-frame: 3,019
                    // lines in one run-heavy OTS session, 09-12).  Numbers
                    // included so a debug log can settle the `time` units.
                    static ULONGLONG s_aiDiscLogTick = 0;
                    ULONGLONG nowD = GetTickCount64();
                    if (nowD - s_aiDiscLogTick >= 1000) { s_aiDiscLogTick = nowD;
                        char dbuf[128];
                        sprintf_s(dbuf, sizeof(dbuf),
                            "[WASDCombat] dc_ai_motion_feed_reseed_discontinuity sp=%.1f dt=%.5f",
                            sp, time);
                        DebugLog(dbuf); }
                }
            }
            s_aiPubLastPos = pNow;
            s_aiPubValid   = true;
        }
        else
        {
            s_aiPubValid = false;
            s_aiPubSpd   = 0.0f;
            s_aiPubVel   = Ogre::Vector3::ZERO;
        }
    }

    // Turret/mounted-use camera-lock suspension.
    // Detects enter/exit of isUsingStationaryTurret to suspend movement injection
    // and camera-lock updates while mounted, then restores exactly once on exit.
    if (s_mode == MODE_FREE_MOVE && s_freeMoveAnchor && !s_lootUiSuspendActive && ou->player)
    {
        bool atTurret = isUsingStationaryTurret(s_freeMoveAnchor);
        if (atTurret && !s_cameraLockTurretSuspend)
        {
            s_cameraLockTurretSuspend = true;
            DebugLog("[WASDCombat] camera_lock_suspended_turret_enter");
        }
        else if (!atTurret && s_cameraLockTurretSuspend)
        {
            s_cameraLockTurretSuspend = false;
            DebugLog("[WASDCombat] camera_lock_restored_after_turret_exit");
            ou->player->startTrackCharacter(s_freeMoveAnchor);
        }
    }
    // Menu/pause suspension — movement injection only; camera lock and anchor preserved throughout.
    // Detects transition into/out of engine pause (escape menu, options, save, load).
    // Explicitly excludes inventory-triggered auto-pause: loot UI suspension owns that path.
    // On false→true: one-time movement stop using the same fields as the structured release-stop path.
    // No stop on subsequent frames while suspended; no camera change on either edge.
    if (s_mode == MODE_FREE_MOVE && s_freeMoveAnchor)
    {
        bool inventoryPausing = s_lootUiSuspendActive || (gui && gui->isAnyInventoryWindowOpen());
        bool menuPausedNow    = ou->isPaused() && !inventoryPausing;
        if (menuPausedNow && !s_menuSuspendActive)
        {
            s_menuSuspendActive = true;
            DebugLog("[WASDCombat] dc_menu_suspend_begin");
            CharMovement* mvM = s_freeMoveAnchor->movement;
            // Only kill momentum / cancel the path order if the character was
            // actively WASD-driving.  A character standing on a player-issued move
            // order has NO WASD momentum, and pausing must leave that order intact
            // — wiping it here was the "move order gets removed when the game
            // pauses while direct controlled" bug (user 2026-07-31).  WASD driving
            // owns no player order (it snaps to current pos on release), so stopping
            // it loses nothing.
            bool hadWasdMomentum = s_wasdMovementApplied
                                 || s_prevWasdDir.squaredLength() > 0.0001f;
            if (mvM && hadWasdMomentum && !isDownedButMovable(s_freeMoveAnchor))
            {
                s_prevWasdDir = Ogre::Vector3::ZERO;
                dcSnapCancelOrder(s_freeMoveAnchor);
                mvM->halt();
                mvM->desiredMotion = Ogre::Vector3::ZERO;
                mvM->moveLimit     = 0.0f;
                if (g_release.wasdReleaseDecelerationMultiplier > 1.0f)
                    mvM->currentMotion = Ogre::Vector3::ZERO;
                s_wasdMovementApplied = false;
                VerbLog("[WASDCombat] dc_menu_suspend_stop_applied");
            }
            else
            {
                VerbLog("[WASDCombat] dc_menu_suspend_stop_skipped_no_wasd_momentum");
            }
        }
        else if (!menuPausedNow && s_menuSuspendActive)
        {
            s_menuSuspendActive = false;
            DebugLog("[WASDCombat] dc_menu_suspend_end");
        }
        // Diagnostic: ou->isPaused() is true but inventory auto-pause is the cause.
        // Fires once per inventory-pause event; resets when the condition clears.
        { static bool s_skipLogged = false;
          if (ou->isPaused() && inventoryPausing && !s_menuSuspendActive)
          { if (!s_skipLogged) { s_skipLogged = true;
                DebugLog("[WASDCombat] dc_menu_suspend_skipped_inventory_pause"); } }
          else { s_skipLogged = false; } }
    }
    // Per-frame DC camera focus offset — raises focus toward chest at close zoom, tapers to zero at medium/far.
    // Skipped while the detached OTS camera owns the view (!s_otsCamActive) —
    // OTS runs its own camera, so this vanilla-follow-offset write is moot then.
    if (s_mode == MODE_FREE_MOVE && g_dcCam.dcCameraCloseZoomChestOffset && !s_fpActive && !s_firstPersonActive &&
        !s_otsCamActive &&
        s_freeMoveAnchor && !s_lootUiSuspendActive && ou->player && ou->player->camera)
    {
        CameraClass*  cam    = ou->player->camera;
        Ogre::Vector3 camPos = cam->getCameraPos();
        Ogre::Vector3 ctr    = cam->getCenter();
        float dist = (camPos - ctr).length();
        // Full effect at/inside typical min-zoom distance (~15 units), gone by 5.5 m.
        const float zoomClose = 15.0f, zoomFar = 55.0f;
        float t   = (dist - zoomClose) / (zoomFar - zoomClose);
        float scl = 1.0f - (t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t));
        cam->objectCurrentlyFollowingOffset.y = s_savedCamFollowOffY + g_dcCam.dcCameraFocusOffsetY * scl;
    }
    s_prof_cameraLock += qpcNow() - _clStart; }   // end camera-lock timer

    // 4. HUD + X speed key.
    hudUpdate();

    // SPEEDSYNC-TWEAK: s_xPressed = the legacy X path (cycles the panel THEN
    // applies).  s_syncSpeedToAnchor = vanilla cycle_run_speed already cycled
    // the panel (orig ran in processKeys), so only APPLY to the anchor — no
    // second speedNext (that would skip a step).  Both end at the same apply.
    if ((s_xPressed || s_syncSpeedToAnchor) && !s_lootUiSuspendActive)
    {
        bool doCycle = s_xPressed;
        s_xPressed          = false;
        s_syncSpeedToAnchor = false;
        if (s_mode == MODE_FREE_MOVE && s_freeMoveAnchor && s_freeMoveAnchor->movement)
        {
            OrdersPanel* op = (gui && gui->mainbar) ? gui->mainbar->ordersDataPanel : nullptr;
            if (op)
            {
                if (doCycle) op->speedNext(nullptr);
                MoveSpeed ns = MoveSpeed(int((unsigned char)op->speedImageNamesIdx));
                if (ns < GROUPED)
                {
                    s_freeMoveAnchor->movement->setDesiredSpeedOrders(ns);
                    s_freeMoveAnchor->movement->setDesiredSpeed(ns);
                }
                DebugLog(ns == WALK ? "[WASDCombat] speed_walk"
                       : ns == JOG  ? "[WASDCombat] speed_jog"
                                    : "[WASDCombat] speed_run");
            }
        }
    }

    // 5. Pre-AI WASD application.
    if (s_mode == MODE_FREE_MOVE && s_freeMoveAnchor && s_freeMoveAnchor->movement && !s_lootUiSuspendActive)
    {
        // Promote deferred healing job once WASD is released.
        if (s_healingJobPending && !(s_wHeld || s_aHeld || s_sHeld || s_dHeld))
        {
            s_healingJobPending = false;
            s_healingJobActive  = true;
            VerbLog("[WASDCombat] dc_heal_resumed_after_wasd_release");
        }
        // Mirror: an ACTIVE heal yields to a held WASD, exactly as a vanilla
        // point-click moves the patient out of treatment (medic re-paths and
        // resumes on release via the promotion above).  This is also the cure
        // for a STUCK heal flag: the auto-heal job stays queued through a
        // knockdown/KO so removeJob never fires to clear s_healingJobActive
        // (field 2026-06-13: ~90 s of medical_job_blocks_wasd after the
        // character went unconscious, WASD pinned to a single disengage step).
        // Demoting to pending on any WASD hold guarantees the player can
        // always move, and the heal resumes the instant they stop.
        else if (s_healingJobActive && (s_wHeld || s_aHeld || s_sHeld || s_dHeld))
        {
            s_healingJobActive  = false;
            s_healingJobPending = true;
            VerbLog("[WASDCombat] dc_heal_yielded_to_wasd");
        }

        bool bW = s_wHeld, bA = s_aHeld, bS = s_sHeld, bD = s_dHeld;
        if (!(bW || bA || bS || bD))
        {
            s_playDeadExitDone       = false;
        }
        if (!(bW || bA || bS || bD) && s_slaveOrderActive)
        {
            // Slave WASD order live with keys up: the step-9 release snap
            // (dcSnapCancelOrder) has already re-ordered a move to the current
            // position = stop; just drop our bookkeeping so the next press
            // re-issues from scratch.
            s_slaveOrderActive = false;
            s_slaveModeLogged  = false;
        }
        if (!(bW || bA || bS || bD) && s_wasdDownedMovementActive)
        {
            // Keys are up but a persistent crawl order is live — cancel it
            // at the current position NOW (pre-AI), before pathfinding can
            // advance it another frame.  Level-triggered: catches any
            // release the step-9 edge stop might miss.  Crawl = keys held,
            // release = stay put.
            CharMovement* mvDC = s_freeMoveAnchor->movement;
            s_freeMoveAnchor->playerMoveOrderDefault(nullptr, nullptr, mvDC->pos);
            mvDC->halt();
            s_wasdDownedMovementActive = false;
            DebugLog("[WASDCombat] dc_downed_crawl_cancelled_on_release");
        }
        if (bW || bA || bS || bD)
        {
            // WASD breaks playing-dead exactly like a vanilla move order:
            // drop the prone state once per press and let the game decide
            // whether the character can actually stand — if not, they stay
            // crippled/downed and the crawl order below carries them.
            if (!s_playDeadExitDone
                && s_freeMoveAnchor->getProneState() == PS_PLAYING_DEAD)
            {
                s_freeMoveAnchor->setProneState(PS_NORMAL);
                s_playDeadExitDone = true;
                DebugLog("[WASDCombat] dc_play_dead_exit_on_wasd");
            }
            if (slaveOrderDriven(s_freeMoveAnchor))
            {
                // SLAVE OBEDIENCE: WASD = real move orders (see isObeyingSlave).
                if (!s_slaveModeLogged)
                {
                    s_slaveModeLogged = true;
                    DebugLog("[WASDCombat] dc_slave_wasd_order_mode");
                }
                applySlaveMovement(bW, bA, bS, bD);
            }
            else if (isDownedButMovable(s_freeMoveAnchor) && !downedOrderDriven(s_freeMoveAnchor))
            {
                // Downed — direct injection (charMovUpdate steering + step-9)
                // handles it like standing WASD; everywhere since v1.7.17.
                // Only job here: cancel a leftover crawl order from the
                // dormant order mode (flag can't be set anymore — safety).
                if (s_wasdDownedMovementActive)
                {
                    s_freeMoveAnchor->playerMoveOrderDefault(
                        nullptr, nullptr, s_freeMoveAnchor->movement->pos);
                    s_wasdDownedMovementActive = false;
                    DebugLog("[WASDCombat] dc_downed_crawl_switch_to_direct_indoors");
                }
            }
            else if (isDownedButMovable(s_freeMoveAnchor))
            {
                applyDownedMovement(bW, bA, bS, bD);
                s_wasdDownedMovementActive = true;
                static ULONGLONG s_downMoveTick = 0;
                ULONGLONG t = GetTickCount64();
                if (t - s_downMoveTick >= 2000) { s_downMoveTick = t;
                    ProneState prone = s_freeMoveAnchor->getProneState();
                    if (prone == PS_PLAYING_DEAD)   DebugLog("[WASDCombat] playing_dead_state_detected");
                    else if (prone == PS_CRIPPLED)  DebugLog("[WASDCombat] crippled_state_detected");
                    else                            DebugLog("[WASDCombat] downed_state_detected");
                    VerbLog("[WASDCombat] dc_crippled_state_detected");
                    VerbLog("[WASDCombat] dc_crippled_can_move=true");
                    VerbLog("[WASDCombat] dc_crippled_using_downed_movement_path");
                    DebugLog("[WASDCombat] wasd_downed_movement_attempt");
                    DebugLog("[WASDCombat] pointclick_movement_allowed_while_downed");
                    DebugLog("[WASDCombat] wasd_downed_movement_uses_pointclick_path");
                    DebugLog("[WASDCombat] wasd_downed_destination_created");
                    DebugLog("[WASDCombat] wasd_downed_destination_source_wasd");
                    if (s_freeMoveAnchor->isCrippled())
                    {
                        DebugLog("[WASDCombat] wasd_crippled_movement_allowed");
                        DebugLog("[WASDCombat] wasd_crippled_movement_success");
                    } }
            }
            else if (s_freeMoveAnchor->isUnconcious())
            {
                static ULONGLONG s_koTick = 0;
                ULONGLONG t = GetTickCount64();
                // Build 65: within 10 s of a post-load reacquire an "unconscious"
                // anchor may be the stale object - re-read the selection and switch
                // when it names a different, loaded, conscious character.
                if (s_reacquireMs != 0 && t - s_reacquireMs <= 10000 && ou && ou->player)
                {
                    Character* sel = ou->player->selectedCharacter.getCharacter();
                    if (sel && sel != s_freeMoveAnchor && sel->movement && sel->isPlayerCharacter()
                        && dcInUpdateList(thisptr, sel) && !sel->isUnconcious())
                    {
                        char rb[128];
                        sprintf_s(rb, sizeof(rb), "[WASDCombat] dc_reacquire_anchor_replaced old=%p new=%p", (void*)s_freeMoveAnchor, (void*)sel);
                        DebugLog(rb);
                        s_freeMoveAnchor = sel;
                        s_anchorMovement = sel->movement;
                        ou->player->startTrackCharacter(sel);
                    }
                }
                if (t - s_koTick >= 2000) { s_koTick = t;
                    char kb[192];
                    sprintf_s(kb, sizeof(kb), "[WASDCombat] wasd_true_unconscious_movement_blocked ptr=%p prone=%d dead=%d in_list=%d since_reacquire_ms=%llu",
                              (void*)s_freeMoveAnchor, (int)s_freeMoveAnchor->getProneState(), s_freeMoveAnchor->isDead() ? 1 : 0,
                              dcInUpdateList(thisptr, s_freeMoveAnchor) ? 1 : 0, s_reacquireMs ? t - s_reacquireMs : 0ULL);
                    VerbLog(kb);
                    VerbLog("[WASDCombat] dc_crippled_can_move=false");
                    VerbLog("[WASDCombat] dc_crippled_move_blocked_reason=UNCONSCIOUS"); }
            }
            else if (!s_cameraLockTurretSuspend && !s_menuSuspendActive)
            {
                if (s_healingJobActive)
                {
                    if (!s_medicalJobSuppressedThisHold)
                    {
                        s_medicalJobSuppressedThisHold = true;
                        VerbLog("[WASDCombat] medical_job_blocks_wasd");
                    }
                }
                else
                {
                    { LONGLONG _wi = qpcNow();
                      applyPlayerMovement(bW, bA, bS, bD);
                      s_prof_wasdInject += qpcNow() - _wi; }
                    { ULONGLONG t = GetTickCount64();
                      if (t - s_movInjLogTick >= 1000) { s_movInjLogTick = t;
                          if (g_log.debugVerbose)
                              DebugLog("[WASDCombat] movement_injection_allowed"); } }
                }
            }
        }
    }

    // 6. Run original game loop — AI + CharMovement::update + CombatClass::go run here.
    // Combat is untouched at the AI level (DC is locomotion-only); WASD priority
    // over combat locomotion is enforced inside charMovUpdate_hook.
    s_retreatLockGoSuppressed = false;

    s_mainLoopOrig(thisptr, time);

    // Post-loop load guard.
    if (!ou || !ou->player || dcRealLoad())
    {
        // Distinguish true LOADGAME teardown from temporary micro-load pointer loss.
        //   Hard shutdown only when:
        //     (a) ou == null          — game world destroyed (absolute teardown)
        //     (b) !ou->player AND isLoadingFromASaveGame() — player freed during confirmed load
        //   !ou->player WITHOUT a load signal = micro-load travel; preserve DC, do not shutdown.
        bool absoluteHard      = (!ou);
        bool confirmedLoadGame = (!absoluteHard) && (!ou->player) && ou->isLoadingFromASaveGame();
        bool hardLoss          = absoluteHard || confirmedLoadGame;

        if (hardLoss || !s_userWantsDC)
        {
            // True LOADGAME (hardLoss): full shutdown with s_dcShutdownInProgress.
            // DC not intended (!s_userWantsDC): normal load guard only, no shutdown flag.
            if (hardLoss && !s_dcShutdownInProgress)
            {
                s_dcShutdownInProgress = true;
                DebugLog("[WASDCombat] dc_loadgame_shutdown_begin");
                DebugLog("[WASDCombat] dc_shutdown_triggered_by_true_load");
                if (s_userWantsDC || s_mode == MODE_FREE_MOVE)
                {
                    DebugLog("[WASDCombat] dc_hard_shutdown_reason=LOADGAME");
                    s_userWantsDC = false;
                }
                s_healingJobActive             = false;
                s_medicalJobSuppressedThisHold = false;
                s_freeMoveAnchor    = nullptr;
                s_selectedCharacter = nullptr;
                DebugLog("[WASDCombat] dc_loadgame_clear_anchor");
                s_anchorMovement    = nullptr;
                s_selectedMovement  = nullptr;
                s_prevAttackTarget  = nullptr;
                DebugLog("[WASDCombat] dc_loadgame_clear_movement");
                s_savedFreeCameraMode    = false;
                s_cameraLockInvSuspend   = false;
                s_cameraLockTurretSuspend = false;
                s_savedCamFollowOffY     = 0.0f;
                DebugLog("[WASDCombat] dc_loadgame_clear_camera");
                DebugLog("[WASDCombat] dc_loadgame_old_state_cleared");
            }
            if (!s_loadGuardActive)
            {
                DebugLog("[WASDCombat] loadgame_signal_hard_shutdown");
                s_loadGuardActive = true;
                clearAllState();
                s_vHud.label = nullptr;
                s_vHud.shown = false;
                s_hudReady   = false;
                if (hardLoss)
                    DebugLog("[WASDCombat] dc_loadgame_shutdown_complete");
            }
            return;
        }

        // s_userWantsDC = true and no confirmed LOADGAME signal:
        // Pointer temporarily invalid during chunk travel — preserve DC, skip post-AI steps.
        {
            static ULONGLONG s_skipLogTick = 0;
            ULONGLONG nowSk = GetTickCount64();
            if (nowSk - s_skipLogTick >= 2000) { s_skipLogTick = nowSk;
                VerbLog("[WASDCombat] dc_shutdown_skipped_microload"); }
        }
        return;
    }

    // Skip all DC post-processing if a hard shutdown is in progress.
    if (s_dcShutdownInProgress)
        return;


    // 7b. Auto-off when the controlled character is knocked out (or dead) and
    // there is NOBODY else to switch to (user req 2026-09-21).  With a squad
    // the player simply selects someone else and DC follows the selection; a
    // lone character has no such out, and DC would sit on an unconscious body
    // (camera locked, WASD dead, FP/OTS still engaged) until V is pressed.
    // Takes the same path as V OFF so the camera/FP/OTS teardown is identical.
    // PS_KO/isDead only (the same "truly out" test as isDownedButMovable);
    // crippled / playing-dead / conscious-downed keep DC (they can crawl).
    // Debounced 1 s so the knockdown-to-KO transition does not flap.
    if (s_mode == MODE_FREE_MOVE && s_freeMoveAnchor && ou && ou->player)
    {
        static ULONGLONG s_koAloneSinceMs = 0;
        bool anchorOut = s_freeMoveAnchor->isDead()
                      || s_freeMoveAnchor->getProneState() == PS_KO;
        bool alone = false;
        if (anchorOut)
        {
            alone = true;
            const lektor<Character*>& pcs = ou->player->playerCharacters;
            for (auto it = pcs.begin(); it != pcs.end(); ++it)
            {
                Character* c = *it;
                if (!c || c == s_freeMoveAnchor || !c->movement) continue;
                if (c->isDead() || c->getProneState() == PS_KO) continue;
                alone = false;   // someone conscious remains -> the player can switch
                break;
            }
        }
        if (anchorOut && alone)
        {
            ULONGLONG nowKo = GetTickCount64();
            if (s_koAloneSinceMs == 0) s_koAloneSinceMs = nowKo;
            else if (nowKo - s_koAloneSinceMs >= 1000)
            {
                s_koAloneSinceMs = 0;
                s_userWantsDC = false;
                s_userWantsFP = false;
                setMode(MODE_VANILLA);
                DebugLog(s_freeMoveAnchor->isDead()
                    ? "[WASDCombat] dc_auto_off_anchor_dead_alone"
                    : "[WASDCombat] dc_auto_off_anchor_ko_alone");
            }
        }
        else s_koAloneSinceMs = 0;
    }

    // 8. Periodic squad-threat scan.
    if (s_mode == MODE_FREE_MOVE && s_freeMoveAnchor)
    {
        ULONGLONG nowMs   = GetTickCount64();
        bool inCombat     = s_freeMoveAnchor->isInCombatMode(true, true);

        if (nowMs - s_lastScanTick >= SCAN_INTERVAL_MS)
        {
            s_lastScanTick = nowMs;
            LONGLONG _tsStart = qpcNow();

            bool foundEnemyTargetingAnchor     = false;
            bool foundEnemyTargetingSquad      = false;
            bool foundConsciousAllyUnderAttack = false;
            int  enemiesTargetingAnchor        = 0;
            int  nearbyEnemies                 = 0;
            int  pathfindingEnemies            = 0;

            auto& allChars = thisptr->getCharacterUpdateList();
            for (auto it = allChars.begin(); it != allChars.end(); ++it)
            {
                Character* c = *it;
                if (!c || c == s_freeMoveAnchor || !c->movement) continue;
                if (!c->isEnemy(s_freeMoveAnchor, true)) continue;
                nearbyEnemies++;
                {
                    CombatClass* ccc = c->getCombatClass();
                    if (ccc)
                    {
                        swordStateEnum cs = ccc->getCombatState();
                        if (cs == TARGET_PATHFINDING || cs == TARGET_PATHFINDING_STARTUP)
                            pathfindingEnemies++;
                    }
                }

                Character* attacked = c->getAttackTarget().getCharacter();
                if (!attacked) continue;

                if (attacked == s_freeMoveAnchor)
                {
                    foundEnemyTargetingAnchor = true;
                    enemiesTargetingAnchor++;
                }
                else if (attacked->isPlayerCharacter())
                {
                    foundEnemyTargetingSquad = true;
                    if (!attacked->isUnconcious() && !attacked->isDead())
                        foundConsciousAllyUnderAttack = true;
                }
            }
            s_lastKnownEnemyCount   = enemiesTargetingAnchor;
            s_nearbyEnemyCount      = nearbyEnemies;
            s_pathfindingEnemyCount = pathfindingEnemies;
            s_prof_threatScan      += qpcNow() - _tsStart;

            {
                bool wasdNow = s_wHeld || s_aHeld || s_sHeld || s_dHeld;
                if (foundEnemyTargetingAnchor && !s_enemyTargetingLogged)
                {
                    if (wasdNow && s_retreatLockGoSuppressed)
                    {
                    }
                    else
                    {
                        if (g_log.debugVerbose)
                            DebugLog("[WASDCombat] enemy_targeting_player");
                        s_enemyTargetingLogged = true;
                    }
                }
                if (!inCombat) s_enemyTargetingLogged = false;

            }

            s_squadThreat         = foundEnemyTargetingSquad;
            s_consciousAllyThreat = foundConsciousAllyUnderAttack;

            // Combat engagement tracking — target acquired/lost, range entry/exit.
            Character* curTgt = s_freeMoveAnchor->getAttackTarget().getCharacter();
            if (curTgt != s_prevAttackTarget)
            {
                if (curTgt && !s_prevAttackTarget)
                    VerbLog("[WASDCombat] combat_target_acquired");
                else if (!curTgt && s_prevAttackTarget)
                    VerbLog("[WASDCombat] combat_target_lost");
                s_prevAttackTarget = curTgt;
            }

            bool curInRange = false;
            if (curTgt && curTgt->movement)
            {
                Ogre::Vector3 toTgt = curTgt->movement->pos - s_freeMoveAnchor->movement->pos;
                toTgt.y = 0.0f;
                float tgtDist = toTgt.length();
                curInRange = (tgtDist <= ATTACK_RANGE);

                if (curInRange && !s_prevTargetInRange)
                {
                    char buf[128];
                    sprintf_s(buf, sizeof(buf),
                        "[WASDCombat] enemy_entered_attack_range dist=%.0f", tgtDist);
                    VerbLog(buf);
                }
                else if (!curInRange && s_prevTargetInRange)
                {
                    char buf[128];
                    sprintf_s(buf, sizeof(buf),
                        "[WASDCombat] enemy_left_attack_range dist=%.0f", tgtDist);
                    DebugLog(buf);
                }
            }
            s_prevTargetInRange = curInRange;
        }
    }

    // ----------------------------------------------------------------
    // 9. Post-AI WASD re-application + instant stop.
    // ----------------------------------------------------------------
    if (!(s_mode == MODE_FREE_MOVE && s_freeMoveAnchor && s_freeMoveAnchor->movement) || s_lootUiSuspendActive)
    {
        // A live WASD crawl order must not keep moving the character into
        // a menu/suspend — cancel it at the current position.
        if (s_wasdDownedMovementActive
            && s_freeMoveAnchor && s_freeMoveAnchor->movement)
        {
            s_freeMoveAnchor->playerMoveOrderDefault(
                nullptr, nullptr, s_freeMoveAnchor->movement->pos);
            s_freeMoveAnchor->movement->halt();
            s_wasdDownedMovementActive = false;
            DebugLog("[WASDCombat] dc_downed_crawl_cancelled_on_suspend");
        }
        s_wasdWasActive    = false;
        s_combatWASDLogged = false;
        s_retreatLogged    = false;
        s_rmbPressedEdge   = false;  // discard stale click edges outside V-mode
        return;
    }

    bool bW = s_wHeld, bA = s_aHeld, bS = s_sHeld, bD = s_dHeld;
    bool wasdActive = bW || bA || bS || bD;
    bool inCombat   = s_freeMoveAnchor->isInCombatMode(true, true);

    // DIAG (2026-09-14): slave-state edges with the DC context at that instant —
    // settles what flips IS_SLAVE -> ESCAPING when the user right-clicks inside
    // Rebirth with DC on (user says it's safe with DC off).
    {
        int stS = (int)s_freeMoveAnchor->isSlave();
        if (stS != s_lastSlaveState)
        {
            ULONGLONG nowS = GetTickCount64();
            char sb[224];
            sprintf_s(sb, sizeof(sb),
                "[WASDCombat] dc_slave_state from=%d to=%d walls=%d hold=%d click=%d wasd=%d combat=%d sinceModOrderMs=%llu chained=%d",
                s_lastSlaveState, stS, s_freeMoveAnchor->amInsideTownWalls(),
                s_wasdHoldActive ? 1 : 0, s_playerPointClickActive ? 1 : 0, wasdActive ? 1 : 0,
                inCombat ? 1 : 0,
                s_lastModOrderMs ? (unsigned long long)(nowS - s_lastModOrderMs) : 0ULL,
                s_freeMoveAnchor->isChainedMode() ? 1 : 0);
            VerbLog(sb);
            s_lastSlaveState = stS;
        }
    }

    // Earliest reliable player-click signal: RMB press edge from the poll
    // thread.  Input-level — cannot be blocked by any game-side dispatch
    // (field finding 2026-06-10: ground clicks during the hold never
    // reached PlayerInterface::playerMove, so the dispatcher hook alone
    // could not release the hold).  A real click while the hold is active
    // releases it immediately; the click's own order then proceeds under
    // vanilla control.  The playerMove and addOrder paths remain as
    // backup/diagnostics.
    if (s_rmbPressedEdge)
    {
        s_rmbPressedEdge = false;
        VerbLog("[WASDCombat] dc_click_input_seen");
        if (s_wasdHoldActive)
        {
            s_wasdHoldActive         = false;
            s_playerPointClickActive = true;
            s_holdPosValid           = false;
            VerbLog("[WASDCombat] dc_wasd_hold_cleared_by_click_input");
        }
    }

    // Combat mode transition — suppress log and detect flicker during WASD retreat.
    if (inCombat && !s_wasPrevInCombat)
    {
        s_chaseFlapsCount++;
        if (!wasdActive)
            VerbLog("[WASDCombat] combat_entered");
    }
    else if (!inCombat && s_wasPrevInCombat)
    {
        s_chaseFlapsCount++;
        if (!wasdActive)
            VerbLog("[WASDCombat] combat_exited");
    }
    s_wasPrevInCombat = inCombat;

    if (wasdActive)
    {
        if (!s_wasdWasActive)
        {
            s_combatWASDLogged     = false;
            s_retreatLogged        = false;
            s_postWasdGraceActive  = false;
            s_combatReentryAllowed = false;

            // WASD always wins: release the post-WASD hold while keys are
            // held (it re-engages at the next release edge), and clear the
            // active point-click — the disengage order below replaces the
            // clicked order itself.  Capture whether a point-click was actually
            // pending BEFORE clearing it: the disengage move-order is ONLY needed
            // to cancel such a click.  When the player is merely navigating with
            // WASD (no pending click) there is nothing to cancel, so we must NOT
            // issue a move order — that order pathfinds, and inside a locked
            // building (esp. at the locked door/threshold, where isIndoors() reads
            // FALSE) the path-out fails and the vanilla "I can't get out of here"
            // bark fires (field 2026-06-20, log-proven: dc_disengage_order_issued
            // fired with indoors=0 at the threshold).
            bool hadPendingClick     = s_playerPointClickActive;
            s_wasdHoldActive         = false;
            s_playerPointClickActive = false;
            s_holdPosValid           = false;
            s_idleHoldEngaged        = false;

            if (isUsingStationaryTurret(s_freeMoveAnchor))
                DebugLog("[WASDCombat] stationary_crossbow_cancelled_by_wasd");

            // First WASD press: cancel any in-flight point-click/path order with a
            // disengage move order — but ONLY OUTDOORS.  playerMoveOrderDefault
            // ALWAYS issues a MOVE_CUS_ORDERED (task 29) that runs pathfinding, and
            // when the character is locked inside a building the path out runs
            // through a locked door -> the pathfinder fails -> the vanilla "I can't
            // get out of here" bark fires on every WASD press (field 2026-06-20;
            // confirmed it barked even with dest = current pos, because the move
            // order itself is the trigger, not the destination).  So skip the order
            // entirely indoors (same condition the release-snap already uses): there
            // setDirectMovement drives movement and the hold-clamp parks the
            // character on release, so no explicit order-cancel is needed.  Also
            // skipped while the crawl order owns movement (downed outdoors).
            // Only issue the disengage when a point-click was actually pending
            // (hadPendingClick) AND it won't bark (moveOrderMayBark) AND not downed.
            if (s_freeMoveAnchor->movement
                && hadPendingClick
                && !downedOrderDriven(s_freeMoveAnchor)
                && !moveOrderMayBark(s_freeMoveAnchor))
            {
                Ogre::Vector3 cancelDir;
                if (computeWASDDirection(bW, bA, bS, bD, cancelDir))
                {
                    Ogre::Vector3 dest = s_freeMoveAnchor->movement->pos;
                    s_freeMoveAnchor->playerMoveOrderDefault(nullptr, nullptr, dest);
                }
            }
        }

        if (!s_cameraLockTurretSuspend && !s_menuSuspendActive
            && !slaveOrderDriven(s_freeMoveAnchor)      // obeying slave: order-driven (step 5)
            && !downedOrderDriven(s_freeMoveAnchor))    // outdoor downed crawl is
                                                        // order-driven; halt() here
                                                        // was killing it every frame.
                                                        // Indoors-downed = direct
                                                        // injection, must run.
        {
            if (s_healingJobActive)
            {
                if (!s_medicalJobSuppressedThisHold)
                {
                    s_medicalJobSuppressedThisHold = true;
                    VerbLog("[WASDCombat] medical_job_blocks_wasd");
                }
            }
            else
            {
                { LONGLONG _wi = qpcNow();
                  applyPlayerMovement(bW, bA, bS, bD);
                  s_prof_wasdInject += qpcNow() - _wi; }
                { ULONGLONG t = GetTickCount64();
                  if (t - s_movInjLogTick >= 1000) { s_movInjLogTick = t;
                      if (g_log.debugVerbose)
                          DebugLog("[WASDCombat] movement_injection_allowed"); } }
            }
        }
        // Note: downed/crippled movement is handled in step 5 (pre-AI) only —
        // playerMoveOrderDefault persists through the AI loop, no re-issue needed.

        // Retreat detection.
        if (inCombat && !s_retreatLogged && ou->player->camera)
        {
            Character* aiTarget = s_freeMoveAnchor->getAttackTarget().getCharacter();
            if (aiTarget && aiTarget->movement)
            {
                Ogre::Vector3 camFwd = ou->player->camera->getFacingDirection();
                camFwd.y = 0.0f;
                float cflen = camFwd.length();
                if (cflen > 0.001f)
                {
                    camFwd /= cflen;
                    Ogre::Vector3 camRight(-camFwd.z, 0.0f, camFwd.x);
                    Ogre::Vector3 mv = Ogre::Vector3::ZERO;
                    if (bW) mv += camFwd; if (bS) mv -= camFwd;
                    if (bD) mv += camRight; if (bA) mv -= camRight;
                    float mlen = mv.length();
                    if (mlen > 0.001f)
                    {
                        mv /= mlen;
                        Ogre::Vector3 toEnemy = aiTarget->movement->getPosition()
                                              - s_freeMoveAnchor->movement->getPosition();
                        toEnemy.y = 0.0f;
                        float elen = toEnemy.length();
                        if (elen > 0.001f && mv.dotProduct(toEnemy / elen) < -0.3f)
                            s_retreatLogged = true;
                    }
                }
            }
        }

        // Athletics XP bridge — CharMovement::periodicUpdate skips xpRunning when
        // movementMode == MOVE_DIRECTION; award manually during DC WASD movement.
        {
            static const float     WALK_THRESHOLD           = 0.1f;
            static const ULONGLONG ATHLETICS_XP_INTERVAL_MS = 1000;

            float currentSpd = s_freeMoveAnchor->movement->desiredSpeed;

            if (!s_dcShutdownInProgress
                && !s_cameraLockTurretSuspend
                && !s_healingJobActive
                && !isProtectedAnimationState(s_freeMoveAnchor)
                && currentSpd > WALK_THRESHOLD)
            {
                ULONGLONG nowXP   = GetTickCount64();
                ULONGLONG elapsed = nowXP - s_athleticsXpLastTick;

                if (s_athleticsXpLastTick == 0)
                {
                    // First movement frame — arm timer, do not award.
                    s_athleticsXpLastTick = nowXP;
                }
                else if (elapsed >= ATHLETICS_XP_INTERVAL_MS)
                {
                    CharStats* stXP = s_freeMoveAnchor->getStats();
                    if (stXP)
                    {
                        float deltaTime = elapsed / 1000.0f;
                        stXP->xpRunning(deltaTime, currentSpd);
                        if (g_log.debugLogging)
                        {
                            MoveSpeed tier = s_freeMoveAnchor->movement->speedOrders;
                            const char* tierName = (tier == WALK) ? "walk"
                                                 : (tier == JOG)  ? "jog"
                                                 :                  "run";
                            char xpBuf[96];
                            sprintf_s(xpBuf, sizeof(xpBuf),
                                "[WASDCombat] dc_xp_movement_bridge_tick skill=Athletics speed=%s",
                                tierName);
                            DebugLog(xpBuf);
                        }
                    }
                    s_athleticsXpLastTick = nowXP;
                }
            }
            else
            {
                // Guard failed or speed too low — reset so the next movement hold re-arms.
                s_athleticsXpLastTick = 0;
            }
        }
    }
    else
    {
        if (s_wasdWasActive)
        {
            s_combatWASDLogged     = false;
            s_retreatLogged        = false;
            s_wasdReleasedTick     = GetTickCount64();
            s_postWasdGraceActive  = true;
            s_postWasdGraceStart   = GetTickCount64();
            s_combatReentryAllowed = false;

            // Nudge-tap check — consume tap-start timestamp and decide release path.
            ULONGLONG tapMs      = s_wasdTapStartMs;
            ULONGLONG tapElapsed = (tapMs > 0) ? (GetTickCount64() - tapMs) : ~0ULL;
            bool isNudgeTap      = (tapElapsed <= g_loco.wasdNudgeTapWindowMs);
            s_wasdTapStartMs     = 0;

            if (isNudgeTap && !isDownedButMovable(s_freeMoveAnchor))
            {
                // Nudge-safe release: zero velocity, clear stale dir, snap anchor — no facing correction.
                VerbLog("[WASDCombat] wasd_nudge_tap_detected");
                CharMovement* mvN = s_freeMoveAnchor ? s_freeMoveAnchor->movement : nullptr;
                if (mvN)
                {
                    mvN->halt();
                    mvN->desiredMotion    = Ogre::Vector3::ZERO;
                    mvN->moveLimit        = 0.0f;
                    mvN->currentMotion    = Ogre::Vector3::ZERO;
                    s_wasdMovementApplied = false;
                    VerbLog("[WASDCombat] wasd_nudge_stop_no_turnaround");
                    s_prevWasdDir = Ogre::Vector3::ZERO;
                    VerbLog("[WASDCombat] stale_movement_vector_cleared");
                    dcSnapCancelOrder(s_freeMoveAnchor);   // gated: no bark indoors/locked
                    VerbLog("[WASDCombat] anchor_snapped_no_facing_change");
                }
            }
            else
            {
                // Structured release-stop sequence — fires once on full WASD release.
                if (g_release.wasdStopOnRelease && s_freeMoveAnchor && s_freeMoveAnchor->movement)
                {
                    if (g_log.debugLogging) DebugLog("[WASDCombat] wasd_release_detected");
                    CharMovement* mvR = s_freeMoveAnchor->movement;

                    // STARTUP_STATE (attack windup) alone must not block release-stop.
                    // Compute whether a *real* committed action is blocking: re-use
                    // isCommittedAction for the full check, then subtract STARTUP_STATE.
                    // A HEAL JOB is not a committed action for the STOP (build 51; log
                    // 2026-09-22: all 45 skipped release-stops were the frame the yielded
                    // heal job resumed - step 5 promotes it BEFORE this check, so the
                    // momentum was never zeroed and the character walked on for a
                    // second or two after the key came up).  The medic re-paths on its
                    // own; the WASD vector must still be zeroed.
                    bool releaseCommitted = false;
                    bool healSaved = s_healingJobActive;
                    s_healingJobActive = false;                       // evaluate without the heal reason
                    bool committedNoHeal = isCommittedAction(s_freeMoveAnchor);
                    s_healingJobActive = healSaved;
                    if (committedNoHeal)
                    {
                        CombatClass*   ccRel = s_freeMoveAnchor->getCombatClass();
                        swordStateEnum stRel = ccRel ? ccRel->getCombatState() : COMBAT_FINISHED;
                        bool onlyStartup = (stRel == STARTUP_STATE)
                                        && !isProtectedAnimationState(s_freeMoveAnchor);
                        releaseCommitted = !onlyStartup;
                    }

                    if (releaseCommitted)
                    {
                        VerbLog("[WASDCombat] wasd_release_stop_skipped_committed_action");
                    }
                    else if (!downedOrderDriven(s_freeMoveAnchor))
                    {
                        // Standing AND indoors-downed (direct injection) both
                        // stop here; outdoor downed crawl is order-driven and
                        // stops in the downed block below.
                        // 1. Zero DC movement vector.
                        s_prevWasdDir = Ogre::Vector3::ZERO;
                        VerbLog("[WASDCombat] wasd_release_vector_zeroed");

                        // 2. Snap anchor — cancel any pending pathfind destination.
                        if (g_release.wasdAnchorSnapOnRelease)
                        {
                            dcSnapCancelOrder(s_freeMoveAnchor);   // gated: no bark indoors/locked
                            if (g_log.debugLogging) DebugLog("[WASDCombat] wasd_release_anchor_snapped");
                        }

                        // 3. Zero injected velocity — halt + force-zero all motion fields.
                        if (g_release.wasdZeroVelocityOnRelease)
                        {
                            mvR->halt();
                            mvR->desiredMotion = Ogre::Vector3::ZERO;
                            mvR->moveLimit     = 0.0f;
                            if (g_release.wasdReleaseDecelerationMultiplier > 1.0f)
                                mvR->currentMotion = Ogre::Vector3::ZERO;
                            s_wasdMovementApplied = false;
                            if (g_log.debugLogging) DebugLog("[WASDCombat] wasd_release_velocity_zeroed");
                        }
                    }
                    // else: downed — handled by existing downed-stop block below.
                }
            }

            // Downed/crippled instant stop.
            // Gate on s_wasdDownedMovementActive alone — NOT isDownedButMovable.
            // isDownedButMovable may flip false mid-release (e.g. isCurrentlyGettingUp
            // becomes true inside the AI loop), yet the WASD-issued destination is
            // still pending and must be cancelled.
            if (s_wasdDownedMovementActive)
            {
                CharMovement* mvDown = s_freeMoveAnchor ? s_freeMoveAnchor->movement : nullptr;
                if (mvDown)
                {
                    // playerMoveOrderDefault at current pos cancels the MOVE job entirely,
                    // not just the CharMovement destination field.
                    s_freeMoveAnchor->playerMoveOrderDefault(nullptr, nullptr, mvDown->pos);
                    mvDown->halt();
                }
                ProneState proneStop = s_freeMoveAnchor ? s_freeMoveAnchor->getProneState()
                                                        : PS_NORMAL;
                DebugLog("[WASDCombat] wasd_downed_key_released");
                DebugLog("[WASDCombat] wasd_downed_destination_cleared");
                DebugLog("[WASDCombat] wasd_downed_cached_direction_cleared");
                DebugLog("[WASDCombat] wasd_downed_movement_stopped");
                if (proneStop == PS_CRIPPLED ||
                    (s_freeMoveAnchor && s_freeMoveAnchor->isCrippled()))
                    DebugLog("[WASDCombat] wasd_crippled_instant_stop_applied");
                DebugLog("[WASDCombat] wasd_downed_pointclick_destination_not_persisted");
            }
            else if (s_freeMoveAnchor && isDownedButMovable(s_freeMoveAnchor))
            {
                // Destination was not WASD-created — likely a real player point-click.
                DebugLog("[WASDCombat] vanilla_pointclick_downed_destination_preserved");
            }
            s_wasdDownedMovementActive    = false;
            s_retreatLockEverActive       = false;
            s_retreatSessionCacheCount    = 0;  // clear session cache on WASD release
            s_retreatTargetsProcessed     = 0;
            s_retreatTargetsCachedSkipped = 0;
            s_retreatBlockedAttackerCount    = 0;
            s_medicalJobSuppressedThisHold   = false;
            // Standing instant stop after AI loop (fallback — fires only if the structured
            // release-stop sequence above did not already clear s_wasdMovementApplied).
            CharMovement* mvStop = s_freeMoveAnchor->movement;
            if (mvStop && s_wasdMovementApplied &&
                !isCommittedAction(s_freeMoveAnchor) &&
                !isDownedButMovable(s_freeMoveAnchor))
            {
                Ogre::Vector3 velPre = mvStop->currentMotion;
                mvStop->halt();
                mvStop->desiredMotion = Ogre::Vector3::ZERO;
                mvStop->moveLimit     = 0.0f;
                if (g_loco.wasdDecelerationMultiplier > 1.0f)
                    mvStop->currentMotion = Ogre::Vector3::ZERO;
                s_wasdMovementApplied = false;
                s_prevWasdDir         = Ogre::Vector3::ZERO;

                char buf[192];
                sprintf_s(buf, sizeof(buf),
                    "[WASDCombat] wasd_released instant_stop vel_pre=(%.1f,%.1f,%.1f)",
                    velPre.x, velPre.y, velPre.z);
                DebugLog(buf);
            }

            // Engage the post-WASD hold: WASD left the character here —
            // keep them here until a new point-click, the next WASD press,
            // or V OFF.  Any click made during the drive was already
            // cancelled by the release anchor-snap above, so its flag is
            // cleared too.  (Enforcement + engage log live in the
            // charMovUpdate hold branch and the end-of-frame clamp.)
            s_wasdHoldActive         = true;
            s_playerPointClickActive = false;
        }
    }

    s_wasdWasActive = wasdActive;

    // Post-WASD grace period: suppress combat re-entry unless enemy is close and actively targeting.
    if (!wasdActive)
    {
        if (s_postWasdGraceActive)
        {
            ULONGLONG elapsed = GetTickCount64() - s_postWasdGraceStart;
            if (elapsed >= POST_WASD_GRACE_MS && !isCommittedAction(s_freeMoveAnchor))
            {
                s_postWasdGraceActive  = false;
                s_combatReentryAllowed = true;
                VerbLog("[WASDCombat] combat_state_restored_after_wasd_release");
            }
            else
            {
                // Allow re-engagement only if the old target is within close range and attacking us.
                bool enemyCloseAndActive = false;
                Character* tgt = s_freeMoveAnchor->getAttackTarget().getCharacter();
                if (tgt && tgt->movement)
                {
                    float dist = (tgt->movement->pos - s_freeMoveAnchor->movement->pos).length();
                    bool tgtAttackingUs = (tgt->getAttackTarget().getCharacter() == s_freeMoveAnchor);
                    if (dist <= POST_WASD_REENGAGEMENT_RANGE && tgtAttackingUs)
                        enemyCloseAndActive = true;
                }
                s_combatReentryAllowed = enemyCloseAndActive;
            }
        }
    }
    else
    {
        s_combatReentryAllowed = false;
    }

    // Retreat state tracking — WASD held while combat AI has been suppressed.
    // s_retreatLockEverActive is sticky: set on the first frame go() is suppressed
    // during this WASD hold, cleared on release.  This prevents log bursts on frames
    // where Kenshi skips calling go() (no enemy nearby) while WASD is still held.
    {
        bool curRetreat = wasdActive && s_retreatLockEverActive;
        if (curRetreat && !s_wasdRetreatActive)
        {
            s_wasdRetreatActive      = true;
            s_retreatActiveStartTick = GetTickCount64();
            s_retreatCleanLogged     = false;
            s_combatFlickerLogged    = false;
        }
        else if (!curRetreat && s_wasdRetreatActive)
        {
            s_wasdRetreatActive = false;
        }
        if (s_wasdRetreatActive && !s_retreatCleanLogged &&
            GetTickCount64() - s_retreatActiveStartTick >= 1000)
        {
            s_retreatCleanLogged = true;
            DebugLog("[WASDCombat] retreat_clean_locomotion_confirmed");
            if (s_lastKnownEnemyCount > 1)
                DebugLog("[WASDCombat] retreat_locomotion_stable_under_multi_chase");
            DebugLog("[WASDCombat] retreat_perf_optimized");
            char scanBuf[128];
            sprintf_s(scanBuf, sizeof(scanBuf),
                "[WASDCombat] retreat_scan_interval_ms %llu cache_size=%d processed=%d skipped=%d",
                JOB_REMOVAL_INTERVAL_MS, s_retreatSessionCacheCount,
                s_retreatTargetsProcessed, s_retreatTargetsCachedSkipped);
            DebugLog(scanBuf);
        }
    }

    { LONGLONG _ctStart = qpcNow();
    // Protected animation state transition tracking.
    if (s_mode == MODE_FREE_MOVE && s_freeMoveAnchor)
    {
        bool nowProtected = isProtectedAnimationState(s_freeMoveAnchor);

        if (nowProtected && !s_wasProtectedState)
        {
            if (s_freeMoveAnchor->isCurrentlyGettingUp)
                VerbLog("[WASDCombat] getup_animation_started");
            else
                VerbLog("[WASDCombat] knockdown_or_stagger_started");
        }
        else if (!nowProtected && s_wasProtectedState)
        {
            VerbLog("[WASDCombat] protected_animation_completed");
            VerbLog("[WASDCombat] vmode_runtime_resumed");
        }

        s_wasProtectedState = nowProtected;
    }
    s_prof_combatTarget += qpcNow() - _ctStart; }  // end combat-target timer

    // ----------------------------------------------------------------
    // Final hold clamp — LAST DC-controlled write point in the frame.
    // Everything (AI, Taskers, indoor routing, pathing) has already run.
    // While the post-WASD hold is enforcing, restore the anchor's X/Z and
    // zero motion so no system that moved the character mid-frame keeps
    // the displacement.  Y is left free for gravity/ramp settling.
    // ----------------------------------------------------------------
    if (!wasdActive)
    {
        const char* gateReason = "";
        bool allowFC = computeHoldDecision(s_freeMoveAnchor, &gateReason);
        if (!allowFC)
        {
            CharMovement* mvFC = s_freeMoveAnchor->movement;
            if (mvFC)
            {
                if (!s_holdPosValid)
                {
                    s_holdPos      = mvFC->pos;
                    s_holdPosValid = true;
                }
                mvFC->pos.x         = s_holdPos.x;
                mvFC->pos.z         = s_holdPos.z;
                mvFC->desiredMotion = Ogre::Vector3::ZERO;
                mvFC->currentMotion = Ogre::Vector3::ZERO;
                mvFC->moveLimit     = 0.0f;
            }
        }
        else
        {
            s_holdPosValid = false;
        }

        // dc_authority_gate diagnostic — on state/reason change + 1 s heartbeat.
        ULONGLONG nowAG = GetTickCount64();
        bool agChanged = (allowFC != s_authGateLastAllow)
                      || (strcmp(gateReason, s_authGateLastReason) != 0);
        if (agChanged || nowAG - s_authGateLogTick >= 1000)
        {
            s_authGateLogTick   = nowAG;
            s_authGateLastAllow = allowFC;
            strcpy_s(s_authGateLastReason, sizeof(s_authGateLastReason), gateReason);
            if (g_log.debugLogging)
            {
                char gbuf[128];
                sprintf_s(gbuf, sizeof(gbuf),
                    "[WASDCombat] dc_authority_gate allowVanillaMove=%d reason=%s",
                    (int)allowFC, gateReason);
                VerbLog(gbuf);
            }
        }
    }
    else
    {
        s_holdPosValid = false;

        // Gate diagnostic during WASD drive: WASD owns locomotion.
        ULONGLONG nowAG = GetTickCount64();
        bool agChanged = s_authGateLastAllow
                      || (strcmp("wasd", s_authGateLastReason) != 0);
        if (agChanged || nowAG - s_authGateLogTick >= 1000)
        {
            s_authGateLogTick   = nowAG;
            s_authGateLastAllow = false;
            strcpy_s(s_authGateLastReason, sizeof(s_authGateLastReason), "wasd");
            if (g_log.debugLogging)
                VerbLog("[WASDCombat] dc_authority_gate allowVanillaMove=0 reason=wasd");
        }
    }

    // dc_perf: emit aggregate profiling line once per second.
    {
        ULONGLONG nowMs = GetTickCount64();
        if (nowMs - s_prof_windowStart >= 1000)
        {
            // Gated (v1.4.1): the emit ran unconditionally — 2+ log lines/sec
            // in every release session.  Accumulators still reset below so the
            // window stays coherent when diagnostics are re-enabled.
            if (g_log.debugLogging)
            {
            float f = (float)s_profFreq / 1000.0f;  // ticks → ms divisor
            char perfBuf[512];
            sprintf_s(perfBuf, sizeof(perfBuf),
                "[WASDCombat] dc_perf mainLoop_ms=%.2f charMove_ms=%.2f"
                " playerControl_ms=%.2f committedAction_ms=%.2f"
                " threatScan_ms=%.2f cameraLock_ms=%.2f"
                " wasdInject_ms=%.2f combatTarget_ms=%.2f"
                " enemyCount=%d nearbyCombatantCount=%d dcMode=%d wasdHeld=%d",
                s_prof_mainLoop      / f,
                s_prof_charMove      / f,
                s_prof_playerControl / f,
                s_prof_committedAct  / f,
                s_prof_threatScan    / f,
                s_prof_cameraLock    / f,
                s_prof_wasdInject    / f,
                s_prof_combatTarget  / f,
                s_lastKnownEnemyCount,
                s_nearbyEnemyCount,
                (int)(s_mode == MODE_FREE_MOVE),
                (int)(s_wHeld || s_aHeld || s_sHeld || s_dHeld));
            DebugLog(perfBuf);

            // Chase diagnostics — emitted once per second alongside dc_perf.
            {
                bool wasdNowC    = s_wHeld || s_aHeld || s_sHeld || s_dHeld;
                bool enemiesChasing = (s_lastKnownEnemyCount > 0);
                char chaseBuf[256];
                sprintf_s(chaseBuf, sizeof(chaseBuf),
                    "[WASDCombat] dc_chase_perf enemyCount=%d nearbyCombatantCount=%d"
                    " combatStateFlaps=%d pathRecalcSuspected=%d",
                    s_lastKnownEnemyCount, s_nearbyEnemyCount,
                    s_chaseFlapsCount, (int)(s_pathfindingEnemyCount > 0));
                DebugLog(chaseBuf);
                if (enemiesChasing)
                {
                    if (wasdNowC)
                        VerbLog("[WASDCombat] dc_wasd_chase_active");
                    else
                        VerbLog("[WASDCombat] dc_point_click_chase_compare");
                }
                s_chaseFlapsCount = 0;
            }
            }
            else
                s_chaseFlapsCount = 0;   // window bookkeeping even when silent

            // Reset accumulators for the next window.
            s_prof_mainLoop      = 0;
            s_prof_charMove      = 0;
            s_prof_playerControl = 0;
            s_prof_committedAct  = 0;
            s_prof_threatScan    = 0;
            s_prof_cameraLock    = 0;
            s_prof_wasdInject    = 0;
            s_prof_combatTarget  = 0;
            s_prof_windowStart   = nowMs;
        }
    }

    // FP BEARD RE-ASSERT (1 Hz) — appearance rebuilds (clothing/injury/reload)
    // recreate the beard entity VISIBLE while FP still hides the head; the
    // cheap re-walk heals it within a second.  Quiet when nothing changed
    // (already-hidden beards are skipped before any log line).
    // (2026-08-31 note: a per-frame upgrade was tried and reverted the same
    // day with the zero-offset camera test — the engine re-shows the beard
    // per frame, so 1 Hz loses the visibility fight; the visible hide comes
    // from the head-bone shrink, this walk is the rebuild-heal backstop.)
    if (s_firstPersonActive && s_freeMoveAnchor
        && !s_loadGuardActive && !s_dcShutdownInProgress)
    {
        if (s_fpHideHeadgear || s_fpHideHair)
        {
            // Headwear walk: PER-FRAME once anything is on record (engine
            // re-shows facial attachments every frame — the 1 Hz logs proved
            // it); discovery stays 1 Hz so bare-headed anchors skip the walk.
            static ULONGLONG s_fpBeardTick = 0;
            ULONGLONG nowBrd = GetTickCount64();
            if (s_fpHiddenBeardCount > 0 || nowBrd - s_fpBeardTick >= 1000)
            {
                s_fpBeardTick = nowBrd;
                fpSetBeardHidden(true, false);
            }
        }
        if (s_fpHideHead || s_fpHeadPixelHide)
        {
            static ULONGLONG s_fpMaskTick = 0;   // shader constant: 1 Hz heal
            ULONGLONG nowMk = GetTickCount64();
            if (nowMk - s_fpMaskTick >= 1000)
            {
                s_fpMaskTick = nowMk;
                fpSetHeadMask(true);
            }
        }
    }

    // FP INJURY-OVERLAY SUPPRESSION (v1.3.2) — POST-orig so the disables
    // survive to the render (pre-orig placement was undone by the engine's own
    // update re-enabling overlays from injury status — the feature never
    // visibly worked there).  List fixed 2026-08-24: the chest-clutch clip is
    // named `hand2chest` at runtime (the log's enabled-state dump), not the
    // skeleton-file `handtowounds` family — keep both.  Ogre access rules:
    // enabled-iterator + getAnimationName + setEnabled ONLY (uint-hash map;
    // String-keyed lookups don't exist in OgreMain — plugin fails to load).
    if (s_firstPersonActive && s_freeMoveAnchor && s_fpHideInjuryOverlay
        && !s_loadGuardActive && !s_dcShutdownInProgress)
    {
        AppearanceBase* apS = s_freeMoveAnchor->getAppearance();
        Ogre::Entity* bodyS = apS ? apS->getBody() : nullptr;
        Ogre::AnimationStateSet* setS = bodyS ? bodyS->getAllAnimationStates() : nullptr;
        if (setS)
        {
            static const char* const FP_SUPPRESS_ANIMS[] =
                { "hand2head", "hand2head-R", "hand2chest",
                  "handtowounds", "handtowoundsR" };
            Ogre::ConstEnabledAnimationStateIterator itS =
                setS->getEnabledAnimationStateIterator();
            while (itS.hasMoreElements())
            {
                Ogre::AnimationState* stS = itS.getNext();
                const Ogre::String& nmS = stS->getAnimationName();
                for (int i = 0; i < 5; ++i)
                {
                    if (nmS == FP_SUPPRESS_ANIMS[i])
                    {
                        stS->setEnabled(false);
                        static ULONGLONG s_fpSupLogTick = 0;
                        ULONGLONG nowSup = GetTickCount64();
                        if (nowSup - s_fpSupLogTick >= 1000) { s_fpSupLogTick = nowSup;
                            char supBuf[128];
                            sprintf_s(supBuf, sizeof(supBuf),
                                "[WASDCombat] dc_fp_injury_overlay_suppressed anim=%s",
                                nmS.c_str());
                            DebugLog(supBuf); }
                        break;
                    }
                }
            }
        }
    }


    // AI MOTION FEED v2 end-of-frame publish — the frame's LAST word on the
    // anchor's bookkeeping, so every AI reader (whatever its update order or
    // engine substep) sees the published fleeing-character state from here until
    // the anchor's next update restores → re-publishes.  Gated on the inject
    // path having ACTUALLY driven this frame (see s_aiPubDroveThisFrame) — never
    // on s_wasdMovementApplied, which survives a committed-action release and
    // made a released character keep walking.
    if (s_aiPubDroveThisFrame)
    {
        s_aiPubDroveThisFrame = false;
        if (s_mode == MODE_FREE_MOVE && s_freeMoveAnchor && s_freeMoveAnchor->movement
            && s_settingEnemyPursuit
            && !s_loadGuardActive && !s_dcShutdownInProgress)
        {
            CharMovement* mvPub = s_freeMoveAnchor->movement;
            mvPub->currentMotion     = s_aiPubVel;
            mvPub->currentSpeed      = s_aiPubSpd;
            mvPub->movementMode      = MOVE_NORMAL;
            mvPub->officiallyStopped = false;
            s_aiFeedWrote            = true;
            s_aiFeedModePublished    = true;
        }
    }

    // Native settings tab: detect UI-driven setting changes → unit
    // conversions, FP camera re-apply, immediate INI persistence.  Touches
    // only our own statics (no GUI calls, no character state).
    dcOptionsWatch();
}


// -----------------------------------------------------------------------
// PlayerInterface::playerControl hook
// -----------------------------------------------------------------------
static void (*s_playerControlOrig)(PlayerInterface*, InputHandler&);

static void playerControl_hook(PlayerInterface* thisptr, InputHandler& k)
{
    if (s_dcShutdownInProgress) {
        if (!s_hookBlockLoggedPCtrl) {
            s_hookBlockLoggedPCtrl = true;
            DebugLog("[WASDCombat] dc_hooks_blocked_during_loadgame hook=playerControl"); }
        s_playerControlOrig(thisptr, k); return; }
    ScopeTimer _tPC(s_prof_playerControl);
    if (!ou || !ou->player || dcRealLoad())
    {
        s_playerControlOrig(thisptr, k);
        return;
    }
    if (s_mode == MODE_FREE_MOVE && !s_lootUiSuspendActive)
    {
        bool wasdHeld = s_wHeld || s_aHeld || s_sHeld || s_dHeld;
        bool atTurret = isUsingStationaryTurret(s_freeMoveAnchor);

        if (atTurret && !wasdHeld)
        {
            // Manning a turret: preserve directional inputs for turret aiming.
            static ULONGLONG s_turretProtTick = 0;
            ULONGLONG t = GetTickCount64();
            if (t - s_turretProtTick >= 2000) { s_turretProtTick = t;
                DebugLog("[WASDCombat] stationary_crossbow_action_detected");
                DebugLog("[WASDCombat] stationary_action_protected");
                DebugLog("[WASDCombat] vmode_suppression_skipped_stationary_crossbow"); }
        }
        else
        {
            // Suppress camera pan inputs while DC is active.
            // Zeroing k.up/down/left/right prevents keyboard scroll from detaching
            // the camera lock.  During WASD hold this also blocks AI facing override.
            k.up    = false;
            k.down  = false;
            k.left  = false;
            k.right = false;
            k.pgup  = false;
            k.pgdn  = false;
            static ULONGLONG s_panSupLogTick = 0;
            ULONGLONG t = GetTickCount64();
            if (t - s_panSupLogTick >= 2000) { s_panSupLogTick = t;
                if (g_log.debugLogging && g_log.verboseMovementLogs)
                    DebugLog("[WASDCombat] free_camera_input_suppressed_dc"); }
        }
    }
    s_playerControlOrig(thisptr, k);

    if (s_mode == MODE_FREE_MOVE
        && !s_fpActive && !s_firstPersonActive   // OTS/FP own a DETACHED camera node — never
        && !s_otsCamActive                       // (detached OTS flag — was missing: 5,169
                                                 //  re-tracks in one 5-min OTS run, 09-12)
                                     // re-track it here (this fired every frame
                                     // during OTS, fighting the detached camera
                                     // and breaking it after a reload).
        && !s_lootUiSuspendActive
        && !s_cameraLockTurretSuspend
        && !s_dcPtrLossActive
        && s_freeMoveAnchor != nullptr
        && !ou->player->isTrackingCharacter())
    {
        ou->player->startTrackCharacter(s_freeMoveAnchor);
        DebugLog("[WASDCombat] camera_restored_after_external_detach");
    }
}

// -----------------------------------------------------------------------
// taskTypeName — used by removeJob_hook
// -----------------------------------------------------------------------
static const char* taskTypeName(TaskType t)
{
    switch (t)
    {
        case MELEE_ATTACK:               return "MELEE_ATTACK";
        case FOCUSED_MELEE_ATTACK:       return "FOCUSED_MELEE_ATTACK";
        case CHOOSE_ENEMY_AND_ATTACK:    return "CHOOSE_ENEMY_AND_ATTACK";
        case CHOOSE_ATTACKER_OF_ALLY:    return "CHOOSE_ATTACKER_OF_ALLY";
        case ATTACK_CHARACTERS_ATTACKER: return "ATTACK_CHARACTERS_ATTACKER";
        case ATTACK_ATTACKERS_OF:        return "ATTACK_ATTACKERS_OF";
        case PROTECT_ALLIES:             return "PROTECT_ALLIES";
        case ATTACK_ENEMIES:             return "ATTACK_ENEMIES";
        case JOB_MEDIC:                  return "JOB_MEDIC";
        case FIRST_AID_ORDER:            return "FIRST_AID_ORDER";
        case FIRST_AID_ROBOT:            return "FIRST_AID_ROBOT";
        case JOB_REPAIR_ROBOT:           return "JOB_REPAIR_ROBOT";
        case SPLINT_ORDER:               return "SPLINT_ORDER";
        case SPLINT_JOB:                 return "SPLINT_JOB";
        case HEAL_MY_LEGS:               return "HEAL_MY_LEGS";
        default: { static char buf[32]; sprintf_s(buf, sizeof(buf), "TASK_%d", (int)t); return buf; }
    }
}

// -----------------------------------------------------------------------
// Character::removeJob hook — misclassification guard
// -----------------------------------------------------------------------
static void (*s_removeJobOrig)(Character* thisptr, TaskType t);

static void removeJob_hook(Character* thisptr, TaskType t)
{
    if (s_dcShutdownInProgress) {
        if (!s_hookBlockLoggedRemJob) {
            s_hookBlockLoggedRemJob = true;
            DebugLog("[WASDCombat] dc_hooks_blocked_during_loadgame hook=removeJob"); }
        s_removeJobOrig(thisptr, t); return; }
    if (!s_loadGuardActive && s_mode == MODE_FREE_MOVE && thisptr == s_freeMoveAnchor)
    {
        bool isMedicalJob = (t == JOB_MEDIC       || t == FIRST_AID_ORDER  ||
                              t == FIRST_AID_ROBOT || t == JOB_REPAIR_ROBOT ||
                              t == SPLINT_ORDER    || t == SPLINT_JOB       ||
                              t == HEAL_MY_LEGS);

        // Clear healing job flags — DC movement injection resumes.
        if (isMedicalJob && (s_healingJobActive || s_healingJobPending))
        {
            s_healingJobActive  = false;
            s_healingJobPending = false;
            DebugLog("[WASDCombat] medical_job_ended_restore_dc");
            DebugLog("[WASDCombat] healing_action_completed");
        }

    }
    s_removeJobOrig(thisptr, t);
}

// -----------------------------------------------------------------------
// Character::addJob hook — log attack job creation
// -----------------------------------------------------------------------

// -----------------------------------------------------------------------
// Character::addOrder hook — door suppression on the player-order channel
// (separate from the job queue; proven hookable via GetRealAddress).  Same
// rule as the addJob gate: door-type orders on the anchor are swallowed
// ONLY while the post-WASD hold is active.  Everything else — including
// MOVE_CUS_ORDERED, which DC's own disengage orders use — passes through.
// -----------------------------------------------------------------------
static void (*s_addOrderOrig)(Character* thisptr, Building* dest, TaskType t,
                              RootObject* subject, bool shift, bool clear,
                              const Ogre::Vector3& location);

static void addOrder_hook(Character* thisptr, Building* dest, TaskType t,
                          RootObject* subject, bool shift, bool clear,
                          const Ogre::Vector3& location)
{
    // ANY MOVE while an inventory window is open — swallow it (matches the
    // addJob_hook rule).  subject==nullptr ⇒ a move (to a position OR a
    // building/door); loot/attack/interact/equip carry a subject and pass
    // through.  This is INDEPENDENT of s_fpActive (the face-cam): movement is
    // blocked whenever inventory is open, so a point-click can't walk the
    // character even if the face-cam isn't engaged (field 2026-06-17: moves with
    // a non-null dest leaked through because the old gate required dest==null OR
    // s_fpActive, and s_fpActive is false whenever the face-cam is off/broken).
    // `|| s_fpActive` additionally blocks subject-bearing clicks during the
    // face-cam.  Any PLAYER character, not just the anchor (the face-cam can lock
    // onto the SELECTED character).
    if (!s_dcShutdownInProgress && !s_loadGuardActive
        && s_mode == MODE_FREE_MOVE && thisptr && thisptr->isPlayerCharacter()
        && (subject == nullptr || s_fpActive)
        && gui && gui->isAnyInventoryWindowOpen())
    {
        static ULONGLONG s_invOrdTick = 0;
        ULONGLONG nowIO = GetTickCount64();
        if (nowIO - s_invOrdTick >= 1000) { s_invOrdTick = nowIO;
            char b[120]; sprintf_s(b, sizeof(b),
                "[WASDCombat] dc_order_suppressed_inventory hook=addOrder task=%d", (int)t);
            DebugLog(b); }
        return;
    }

    // DIAG (2026-09-14): every order reaching a SLAVE anchor (any state) —
    // who issues what around the IS_SLAVE -> ESCAPING flip.  1 s throttle.
    if (g_log.debugLogging && !s_dcShutdownInProgress && !s_loadGuardActive
        && thisptr == s_freeMoveAnchor && thisptr->isSlave() != NOT_SLAVE)
    {
        static ULONGLONG s_slvOrdTick = 0;
        ULONGLONG nowSO = GetTickCount64();
        if (nowSO - s_slvOrdTick >= 1000) { s_slvOrdTick = nowSO;
            char ob[160]; sprintf_s(ob, sizeof(ob),
                "[WASDCombat] dc_slave_addorder task=%d clear=%d subject=%d dest=%d state=%d",
                (int)t, (int)clear, subject ? 1 : 0, dest ? 1 : 0, (int)thisptr->isSlave());
            DebugLog(ob); }
    }

    // Diagnostic: every order reaching the anchor while the hold is active.
    // The clear flag is the candidate discriminator between a fresh player
    // click's order and a stale automatic re-issue — field data from this
    // log decides whether addOrder can ever clear the hold safely.
    if (!s_dcShutdownInProgress && !s_loadGuardActive
        && s_mode == MODE_FREE_MOVE && thisptr == s_freeMoveAnchor
        && s_wasdHoldActive)
    {
        ULONGLONG nowAD = GetTickCount64();
        if (nowAD - s_addOrderDiagLogTick >= 1000)
        {
            s_addOrderDiagLogTick = nowAD;
            char dbuf[160];
            sprintf_s(dbuf, sizeof(dbuf),
                "[WASDCombat] dc_addorder_during_hold task=%d clear=%d dest=%p",
                (int)t, (int)clear, (void*)dest);
            VerbLog(dbuf);
        }
    }

    if (!s_dcShutdownInProgress && !s_loadGuardActive
        && s_mode == MODE_FREE_MOVE && thisptr == s_freeMoveAnchor
        && s_wasdHoldActive && !s_playerPointClickActive
        && !s_lootUiSuspendActive && !s_cameraLockTurretSuspend
        && !s_menuSuspendActive
        && !slaveEscapeZone(thisptr)         // escaping slave: the AI's door tasks must run
        && isDoorInteractionTask(t))
    {
        ULONGLONG nowDO = GetTickCount64();
        if (nowDO - s_doorSuppressLogTick >= 1000)
        {
            s_doorSuppressLogTick = nowDO;
            char obuf[128];
            sprintf_s(obuf, sizeof(obuf),
                "[WASDCombat] dc_door_addorder_suppressed task=%d", (int)t);
            DebugLog(obuf);
        }
        return;  // swallowed — the order never enters the queue
    }

    s_addOrderOrig(thisptr, dest, t, subject, shift, clear, location);
}

static void (*s_addJobOrig)(Character* thisptr, TaskType t, RootObject* subject,
                             bool shift, bool addDontClear, const Ogre::Vector3& location);

static void addJob_hook(Character* thisptr, TaskType t, RootObject* subject,
                         bool shift, bool addDontClear, const Ogre::Vector3& location)
{
    if (s_dcShutdownInProgress) {
        if (!s_hookBlockLoggedAddJob) {
            s_hookBlockLoggedAddJob = true;
            DebugLog("[WASDCombat] dc_hooks_blocked_during_loadgame hook=addJob"); }
        s_addJobOrig(thisptr, t, subject, shift, addDontClear, location); return; }

    // Ground-click MOVE while an inventory window is open: swallow it so the
    // freed cursor can't walk the controlled character (the OTS "no point-click
    // while in inventory" feel).  The order reaches the anchor here, NOT through
    // playerMove (field log 2026-06-15: playerMove suppression never fired).
    // subject==nullptr ⇒ a pure position move; loot/attack/interact carry a
    // subject, so equipping/looting/attacking are unaffected.  gui->isAny... is
    // authoritative (s_lootUiSuspendActive can lag a frame).
    if (!s_loadGuardActive && s_mode == MODE_FREE_MOVE
        && thisptr && thisptr->isPlayerCharacter()
        && (subject == nullptr || s_fpActive)   // s_fpActive = own-inventory face-cam: block ALL
        && gui && gui->isAnyInventoryWindowOpen())
    {
        static ULONGLONG s_invJobTick = 0;
        ULONGLONG nowIJ = GetTickCount64();
        if (nowIJ - s_invJobTick >= 1000) { s_invJobTick = nowIJ;
            char b[120]; sprintf_s(b, sizeof(b),
                "[WASDCombat] dc_order_suppressed_inventory hook=addJob task=%d", (int)t);
            DebugLog(b); }
        return;
    }

    if (!s_loadGuardActive && s_mode == MODE_FREE_MOVE && thisptr == s_freeMoveAnchor)
    {
        // Door suppression — ONLY while the post-WASD hold is active: that
        // is the window where no player intent exists and stale indoor
        // door tasks used to auto-fire.  Vanilla door behavior everywhere
        // else (point-clicks, fresh V-mode, suspends, V OFF).
        if (s_wasdHoldActive && !s_playerPointClickActive
            && !s_lootUiSuspendActive && !s_cameraLockTurretSuspend
            && !s_menuSuspendActive
            && !slaveEscapeZone(thisptr)     // escaping slave: the AI's door tasks must run
            && isDoorInteractionTask(t))
        {
            ULONGLONG nowDJ = GetTickCount64();
            if (nowDJ - s_doorSuppressLogTick >= 1000)
            {
                s_doorSuppressLogTick = nowDJ;
                char jbuf[128];
                sprintf_s(jbuf, sizeof(jbuf),
                    "[WASDCombat] dc_door_addjob_suppressed task=%d", (int)t);
                DebugLog(jbuf);
            }
            return;  // swallowed — the job never enters the queue
        }

        bool isAttackJob  = (t == MELEE_ATTACK            || t == FOCUSED_MELEE_ATTACK      ||
                              t == CHOOSE_ENEMY_AND_ATTACK || t == ATTACK_CHARACTERS_ATTACKER ||
                              t == ATTACK_ENEMIES);
        bool isMedicalJob = (t == JOB_MEDIC        || t == FIRST_AID_ORDER  ||
                              t == FIRST_AID_ROBOT  || t == JOB_REPAIR_ROBOT ||
                              t == SPLINT_ORDER     || t == SPLINT_JOB       ||
                              t == HEAL_MY_LEGS);

        // Healing job detection with WASD-aware deferral.
        if (isMedicalJob)
        {
            if (s_healingJobActive)
            {
                // Already committed — DC has already yielded; do not re-interrupt.
                DebugLog("[WASDCombat] dc_heal_committed_action_preserved");
            }
            else if (!s_healingJobPending)
            {
                DebugLog("[WASDCombat] dc_auto_heal_job_detected");
                bool wasdNow = s_wHeld || s_aHeld || s_sHeld || s_dHeld;
                if (wasdNow)
                {
                    // Defer: WASD active — preserve locomotion until keys release.
                    s_healingJobPending = true;
                    DebugLog("[WASDCombat] dc_auto_heal_deferred_due_to_wasd");
                }
                else
                {
                    // WASD not held: activate immediately and zero stale motion.
                    s_healingJobActive = true;
                    if (s_freeMoveAnchor && s_freeMoveAnchor->movement && !isDownedButMovable(s_freeMoveAnchor))
                    {
                        CharMovement* mvH = s_freeMoveAnchor->movement;
                        mvH->halt();
                        mvH->desiredMotion    = Ogre::Vector3::ZERO;
                        mvH->moveLimit        = 0.0f;
                        mvH->currentMotion    = Ogre::Vector3::ZERO;
                        s_wasdMovementApplied = false;
                        s_prevWasdDir         = Ogre::Vector3::ZERO;
                        dcSnapCancelOrder(s_freeMoveAnchor);   // gated: no bark indoors/locked
                        DebugLog("[WASDCombat] medical_job_started_anchor");
                        DebugLog("[WASDCombat] medical_job_movement_zeroed");
                    }
                    DebugLog("[WASDCombat] dc_manual_heal_allowed");
                    DebugLog("[WASDCombat] healing_action_detected");
                }
            }
        }

        if (isAttackJob)
        {
            char buf[128];
            sprintf_s(buf, sizeof(buf), "[WASDCombat] attack_job_created type=%s", taskTypeName(t));
            DebugLog(buf);
        }
        if (g_log.debugLogging && !isAttackJob && !isMedicalJob)
        {
            char axbuf[128];
            sprintf_s(axbuf, sizeof(axbuf),
                "[WASDCombat] dc_xp_vanilla_action_allowed skill=%s", taskTypeName(t));
            DebugLog(axbuf);
        }
    }
    s_addJobOrig(thisptr, t, subject, shift, addDontClear, location);
}

// -----------------------------------------------------------------------
// Native Controls-menu keybind hooks (v1.7, KEP pattern)
// -----------------------------------------------------------------------

// InputHandler::loadConfig — register Direct Control commands BEFORE the
// original runs so the game's keyboard config applies any user-saved
// bindings on top of the defaults, and the game persists rebinds itself.
static bool s_processKeysHookOk = false;  // set at install; native registration
                                          // requires the event reader too

// Belt-and-braces persistence: the game saves bound plugin commands to
// controls.cfg (proven by KEP's toggle_devtools=F12), but our own INI copy
// guards against any case where the command ends up unbound at save time.
// The value is the raw bound int (OIS code | modifier masks), round-
// tripped verbatim through InputHandler::bind.
// SPEEDSYNC-TWEAK: dc_speed_cycle REMOVED (v1.4.0, user req 2026-09-03) — it
// duplicated Kenshi's own cycle_run_speed command (both drive OrdersPanel::
// speedNext).  DC now listens to the vanilla cycle_run_speed key in every
// mode and the DC-tab row edits that same vanilla bind (see processKeys_hook
// + the tab injector).  Only dc_toggle remains a plugin-owned native command.
static const int DC_CMD_COUNT = 1;
static const char* const DC_CMD_NAMES[DC_CMD_COUNT] =
{
    "dc_toggle"
};

// [NativeBinds] format version.  v1 (no "version=" key) WROTE Command::bound
// (0x40) — which is NOT the keycode (field 2026-06-20: bind(name,1) then read
// bound = 2, MISMATCH), so it persisted garbage ("=1") and menu rebinds never
// survived a restart.  v2 persists the real keycode via getBoundKeys().  On a
// version mismatch the old per-command values are IGNORED (defaults stand) and
// the file is re-stamped, so corrupted v1 INIs self-heal instead of binding the
// command to key 1.
static const int DC_NATIVE_BIND_FORMAT_VERSION = 2;

// readBoundKey — the keycode the dc_ command is CURRENTLY bound to (its first
// bound key) via the public InputHandler::getBoundKeys API.  Returns INT_MIN when
// the command is unknown or unbound.  Do NOT read Command::bound (0x40): it is an
// internal value, not the keycode.
static int readBoundKey(const char* name)
{
    if (!key) return INT_MIN;
    lektor<int> keys = key->getBoundKeys(name);
    if (keys.size() == 0) return INT_MIN;   // command unbound
    return keys[0];
}

// readChangeToken — a CHEAP, non-allocating value that merely CHANGES when the
// command's binding changes, used only for change DETECTION (getBoundKeys returns
// a heap lektor, too costly to poll every few seconds).  Command::bound is not
// the keycode but it does differ per binding, so it is a valid change token.
static int readChangeToken(const char* name)
{
    if (!key) return INT_MIN;
    auto it = key->commands.find(name);
    if (it == key->commands.end()) return INT_MIN;
    return it->second.bound;
}

static void saveNativeBindsToIni(const char* reason)
{
    if (!key) return;
    char path[MAX_PATH];
    getConfigPath(path, sizeof(path));
    // Stamp the format version first so a partially-written file is still
    // recognised as v2 (and never re-applies the v1 garbage).
    {
        char vbuf[16];
        sprintf_s(vbuf, sizeof(vbuf), "%d", DC_NATIVE_BIND_FORMAT_VERSION);
        WritePrivateProfileStringA("NativeBinds", "version", vbuf, path);
    }
    for (int i = 0; i < DC_CMD_COUNT; ++i)
    {
        int bound = readBoundKey(DC_CMD_NAMES[i]);
        if (bound == INT_MIN)
        {
            char ebuf[96];
            sprintf_s(ebuf, sizeof(ebuf),
                "[WASDCombat] dc_native_bind_save_failed name=%s reason=unbound_or_not_found",
                DC_CMD_NAMES[i]);
            DebugLog(ebuf);
            continue;
        }
        char val[16];
        sprintf_s(val, sizeof(val), "%d", bound);
        BOOL ok = WritePrivateProfileStringA("NativeBinds", DC_CMD_NAMES[i], val, path);
        char sbuf[160];
        sprintf_s(sbuf, sizeof(sbuf),
            "[WASDCombat] dc_native_bind_saved name=%s bound=%d write_ok=%d",
            DC_CMD_NAMES[i], bound, (int)ok);
        DebugLog(sbuf);
    }
    char rbuf[MAX_PATH + 96];
    sprintf_s(rbuf, sizeof(rbuf),
        "[WASDCombat] dc_native_binds_saved reason=%s path=%s", reason, path);
    DebugLog(rbuf);
}

// Periodic change detector — persistence must not depend on the options
// menu calling saveOptions (and its logs reveal whether a menu rebind even
// updates Command::bound).  Runs from mainLoop every BIND_WATCH_INTERVAL_MS
// once native bindings are registered; first pass only snapshots.
static int       s_bindSnapshot[DC_CMD_COUNT] = { 0 };
static bool      s_bindSnapshotValid    = false;
static ULONGLONG s_bindWatchTick        = 0;
static const ULONGLONG BIND_WATCH_INTERVAL_MS = 3000;

static void watchNativeBindChanges()
{
    ULONGLONG nowBW = GetTickCount64();
    if (nowBW - s_bindWatchTick < BIND_WATCH_INTERVAL_MS) return;
    s_bindWatchTick = nowBW;

    int cur[DC_CMD_COUNT];
    for (int i = 0; i < DC_CMD_COUNT; ++i)
        cur[i] = readChangeToken(DC_CMD_NAMES[i]);   // cheap change-detection token

    if (!s_bindSnapshotValid)
    {
        for (int i = 0; i < DC_CMD_COUNT; ++i) s_bindSnapshot[i] = cur[i];
        s_bindSnapshotValid = true;
        return;
    }

    bool changed = false;
    for (int i = 0; i < DC_CMD_COUNT; ++i)
    {
        if (cur[i] != s_bindSnapshot[i])
        {
            char cbuf[160];
            sprintf_s(cbuf, sizeof(cbuf),
                "[WASDCombat] dc_native_bind_change_detected name=%s old=%d new=%d",
                DC_CMD_NAMES[i], s_bindSnapshot[i], cur[i]);
            DebugLog(cbuf);
            s_bindSnapshot[i] = cur[i];
            changed = true;
        }
    }
    if (changed)
        saveNativeBindsToIni("change_detected");
}

static void applyNativeBindsFromIni(InputHandler* self)
{
    char path[MAX_PATH];
    getConfigPath(path, sizeof(path));

    // Migration guard: only apply stored binds written by the CURRENT format.
    // A v1 file (no "version=" key, or < current) stored Command::bound garbage
    // (e.g. "=1"); applying it would bind the command to key 1.  Ignore those,
    // leave the addCommand defaults (V / X) in place, and re-stamp the file so it
    // self-heals to v2 going forward.
    int ver = (int)GetPrivateProfileIntA("NativeBinds", "version", 0, path);
    if (ver < DC_NATIVE_BIND_FORMAT_VERSION)
    {
        char mbuf[MAX_PATH + 96];
        sprintf_s(mbuf, sizeof(mbuf),
            "[WASDCombat] dc_native_binds_migrated old_ver=%d -> v%d (defaults kept) path=%s",
            ver, DC_NATIVE_BIND_FORMAT_VERSION, path);
        DebugLog(mbuf);
        saveNativeBindsToIni("format_migration");   // re-stamp version + correct keycodes
        return;
    }

    int applied = 0;
    for (int i = 0; i < DC_CMD_COUNT; ++i)
    {
        int v = (int)GetPrivateProfileIntA("NativeBinds", DC_CMD_NAMES[i], -1, path);
        if (v > 0)
        {
            // bind() ADDS a key, it does not replace — so the addCommand default
            // (V / X) would remain ALONGSIDE the saved key and BOTH would fire
            // (field 2026-06-20: default + new bind both activated after restart).
            // Unbind the command first so exactly the saved key remains.  This
            // also cleans up any leftover double-binding from the old format.
            self->unbind(std::string(DC_CMD_NAMES[i]));
            self->bind(DC_CMD_NAMES[i], v);
            int after = readBoundKey(DC_CMD_NAMES[i]);
            char abuf[160];
            sprintf_s(abuf, sizeof(abuf),
                "[WASDCombat] dc_native_bind_applied name=%s ini=%d bound_after=%d%s",
                DC_CMD_NAMES[i], v, after,
                (after == v) ? "" : " MISMATCH");
            DebugLog(abuf);
            ++applied;
        }
    }
    char cbuf[MAX_PATH + 96];
    sprintf_s(cbuf, sizeof(cbuf),
        "[WASDCombat] dc_native_binds_applied count=%d path=%s", applied, path);
    DebugLog(cbuf);
}

// iniSavedNativeBind — the keycode a dc_ command was rebound to in the v2
// [NativeBinds] INI, or -1 if the user never rebound it (no file, pre-v2
// format, or value absent).  Mirrors applyNativeBindsFromIni's version gate
// exactly.  Used at registration to decide whether to claim the V/X default
// at all: if the user already moved the command (e.g. to Ctrl+V), registering
// plain V/X would let Kenshi's one-command-per-key rule STEAL V/X from any
// vanilla command (camera tilt, zoom-out) the player bound there, wiping it
// every session (field report 2026-06-23).
static int iniSavedNativeBind(const char* name)
{
    char path[MAX_PATH];
    getConfigPath(path, sizeof(path));
    int ver = (int)GetPrivateProfileIntA("NativeBinds", "version", 0, path);
    if (ver < DC_NATIVE_BIND_FORMAT_VERSION) return -1;
    int v = (int)GetPrivateProfileIntA("NativeBinds", name, -1, path);
    return (v > 0) ? v : -1;
}

// registerNativeCommands — register dc_toggle / dc_speed_cycle and apply
// INI-persisted bindings.  FIELD FINDING (2026-06-10 log): the game runs
// InputHandler::loadConfig BEFORE RE_Kenshi loads plugins, so a loadConfig
// hook alone never fires.  This is therefore called from the first
// mainLoop pass (key global valid, main thread) — the loadConfig hook
// remains only as a re-registration path if the game ever reloads its
// keyboard config.  Toggle + speed ONLY: movement keys must never be
// registered (one command per key; vanilla camera owns W/S/A/D).
static void registerNativeCommands(InputHandler* self)
{
    if (s_nativeCommandsRegistered || !self) return;
    if (!s_processKeysHookOk)
    {
        // Without the event reader, toggle/speed presses would be lost —
        // stay on the INI/poll fallback entirely.
        DebugLog("[WASDCombat] dc_native_keybinds_skipped_no_event_reader");
        return;
    }
    // Claim the V/X default ONLY when the user has not rebound the command.
    // If a saved rebind exists (e.g. Ctrl+V), register with NO physical key so
    // loadConfig never steals plain V/X from a vanilla camera binding; the
    // saved key is restored by applyNativeBindsFromIni immediately below.
    const int savedToggle = iniSavedNativeBind("dc_toggle");
    self->addCommand("dc_toggle",        0,
                     (savedToggle > 0) ? OIS::KC_UNASSIGNED : OIS::KC_V,
                     OIS::KC_UNASSIGNED, InputHandler::NONE_MASK, InputHandler::GLOBAL);
    // SPEEDSYNC-TWEAK: no dc_speed_cycle command — cycle_run_speed (vanilla)
    // is used instead (processKeys_hook syncs the anchor's speed to it).
    char rnbuf[128];
    sprintf_s(rnbuf, sizeof(rnbuf),
        "[WASDCombat] dc_native_register defaults toggle=%s",
        (savedToggle > 0) ? "deferred(rebound)" : "V");
    DebugLog(rnbuf);
    applyNativeBindsFromIni(self);       // our INI is the real persistence
    s_nativeCommandsRegistered = true;   // poll-thread toggle/speed stand down
    DebugLog("[WASDCombat] dc_native_keybinds_registered");
}

static void (*s_inputLoadConfigOrig)(InputHandler*);
static void inputLoadConfig_hook(InputHandler* self)
{
    registerNativeCommands(self);
    s_inputLoadConfigOrig(self);
    if (s_nativeCommandsRegistered)
        applyNativeBindsFromIni(self);   // re-assert ours over any cfg reload
}

// OptionsWindow::saveOptions — the game just saved every binding it knows
// about to controls.cfg, which excludes plugin commands; persist ours.
static void (*s_optionsSaveOrig)(OptionsWindow*);
static void optionsSave_hook(OptionsWindow* self)
{
    s_optionsSaveOrig(self);
    if (s_nativeCommandsRegistered)
        saveNativeBindsToIni("save_options");
}

// OptionsWindow::create — after the original builds the options UI, find
// the Controls tab (category 0x19) and append the Direct Control rows.
// Rebinding then uses the game's own press-a-key flow and conflict
// handling; nothing custom is drawn.
static void (*s_optionsCreateOrig)(OptionsWindow*);
static void optionsCreate_hook(OptionsWindow* self)
{
    s_optionsCreateOrig(self);

    // v1.5: ALL Direct Control rows now live on the mod's own settings page
    // (sliders + checkboxes + keybinds) — see the NATIVE SETTINGS TAB block.
    // The dc_toggle/dc_speed_cycle KeyConfig rows moved there from the
    // Controls tab (user request 2026-09-02); their registration and
    // saveOptions persistence are unchanged.
    dcInjectSettingsTab(self);
}

// GameWorld::processKeys — toggle/speed press events arrive in key->events
// for exactly one processKeys cycle; consume them here on the main thread.
// No pause gate: the V toggle has always worked while paused (poll-thread
// behavior preserved).  Loading screens are skipped.
static void (*s_processKeysOrig)(GameWorld* thisptr);
static void processKeys_hook(GameWorld* thisptr)
{
    // MANUAL BLOCK: while DC is on, our Block key must not also flip vanilla's
    // orders-panel block MODE.  Orig never clears key->events, so erasing the
    // command BEFORE orig reliably stops the dispatch (v1.9.2 pattern).
    if (s_settingManualBlock && s_mode == MODE_FREE_MOVE && !s_dcShutdownInProgress && key)
    {
        // Build 53 (user 2026-09-23: "F2 stops working"): these vanilla commands
        // are dropped ONLY while the mod key that collides with them is physically
        // down.  The old rule dropped every speed_1 / speed_3 / toggle_block event
        // by NAME, so F2 (speed_1 in controls.cfg), F4 (speed_3) and NUM0 (block
        // toggle) were eaten for the whole time DC was on, not just for Q / E / G.
        bool dodgeDown  = (GetAsyncKeyState(s_bindVk[KR_DODGE])  & 0x8000) != 0;
        bool attackDown = (GetAsyncKeyState(s_bindVk[KR_ATTACK]) & 0x8000) != 0;
        bool blockDown  = (GetAsyncKeyState(s_bindVk[KR_BLOCK])  & 0x8000) != 0;
        for (auto it = key->events.begin(); it != key->events.end(); )
        {
            const std::string& en = (*it)->name;
            // v1.5: Block sits on F = vanilla focus_char (camera re-center) -
            // a block press in a fight must not also snap the camera.  Only
            // while manual combat is on; F is plain vanilla focus otherwise.
            bool drop = (en == "toggle_block" && blockDown)
                     || (en == "focus_char"   && blockDown && s_manualAttackOn)
                     || (en == "speed_1"      && dodgeDown)
                     || (en == "speed_3"      && attackDown);
            if (drop) it = key->events.erase(it);
            else ++it;
        }
    }
    s_processKeysOrig(thisptr);
    if (s_dcShutdownInProgress || !s_nativeCommandsRegistered)
        return;
    if (gui && gui->isLoadingMessageVisible())
        return;
    for (auto it = key->events.begin(); it != key->events.end(); ++it)
    {
        const std::string& n = (*it)->name;
        if (n == "dc_toggle")             handleTogglePress();
        // SPEEDSYNC-TWEAK: vanilla cycle_run_speed drives the speed in DC too.
        // Orig already ran speedNext (cycled the orders panel) this frame, so
        // do NOT cycle again — just flag the anchor's desired speed to be
        // re-synced to the panel in mainLoop (the DC anchor runs under
        // MOVE_DIRECTION, where the panel change doesn't reach its movement
        // on its own — the same reason the old X path set it explicitly).
        else if (n == "cycle_run_speed" && s_mode == MODE_FREE_MOVE)
            s_syncSpeedToAnchor = true;
    }
}

// -----------------------------------------------------------------------
// verifyPatchSiteBytes — REQUIRED gate for any hook installed by raw RVA.
//
// KenshiLib::GetRealAddress hooks are symbol-based and survive exe changes;
// raw-RVA hooks do not.  RE_Kenshi regenerates its patched Kenshi_x64.exe
// on its own updates (last: 2026-06-21), silently shifting all code — a
// stale RVA then patches the middle of an unrelated instruction and MinHook
// still reports SUCCESS.  That shipped two landmines in v1.3.0 (playerMove
// RVA → navmesh crash, showTradeWindow RVA → corrupted conversion helper;
// see the retired install blocks in startPlugin).
//
// Usage: record the first `len` bytes at the target RVA from the SAME exe
// the RVA was derived on, and only AddHook when they still match.  On
// mismatch the hook is skipped (feature degrades, nothing corrupts) and the
// actual bytes are logged for re-derivation.
// -----------------------------------------------------------------------
static bool verifyPatchSiteBytes(intptr_t addr, const unsigned char* expected,
                                 size_t len, const char* name)
{
    if (len > 16) len = 16;
    if (memcmp((const void*)addr, expected, len) == 0)
        return true;
    char hex[3 * 16 + 1] = { 0 };
    for (size_t i = 0; i < len; ++i)
        sprintf_s(hex + 3 * i, sizeof(hex) - 3 * i, "%02X ",
                  ((const unsigned char*)addr)[i]);
    char buf[224];
    sprintf_s(buf, sizeof(buf),
        "WASDCombatPlugin: %s RVA hook SKIPPED — patch-site bytes changed (exe updated?), got: %s",
        name, hex);
    ErrorLog(buf);
    return false;
}

// -----------------------------------------------------------------------
// DllMain / startPlugin
// -----------------------------------------------------------------------
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
        s_thisModule = hModule;   // for the keybind INI path (DLL directory)
    if (reason == DLL_PROCESS_DETACH)
        DebugLog("WASDCombatPlugin: unloaded");
    return TRUE;
}

__declspec(dllexport) void startPlugin()
{
    // SINGLE-INSTANCE GUARD (build 54; field 2026-09-26): the Workshop release
    // and this test mod were both enabled (a Workshop update re-appeared in the
    // mod list), so TWO copies of the plugin hooked the game - every hook ran
    // twice and two first-person drivers fought over one camera (view upside
    // down, mouse look dead).  The first copy to start owns the process-wide
    // mutex; any later copy logs the conflict and installs NOTHING.
    {
        HANDLE mtx = CreateMutexA(NULL, FALSE, "Local\\WASDCombatPlugin_DirectControl_SingleInstance");
        if (mtx && GetLastError() == ERROR_ALREADY_EXISTS)
        {
            ErrorLog("WASDCombatPlugin: ANOTHER COPY of Direct Control is already loaded (two DC mods enabled?) - this copy is disabled. Enable only one.");
            CloseHandle(mtx);
            return;
        }
        // (handle intentionally kept for the life of the process)
    }
    DebugLog("WASDCombatPlugin v1.5.0 — Direct Control: manual combat, aim focus, weapon glint, realistic first person");

    // INI keybinds remain the FALLBACK: loaded unconditionally so the poll
    // thread works from frame one and keeps working if the native command
    // registration never fires (load-order or hook failure).  Once
    // dc_native_keybinds_registered appears, the INI is inert.
    loadKeybinds();

    // Native Controls-menu keybinds (KEP pattern) — all three hooks are
    // DIAG (2026-09-14): real RVAs of the functions behind two open questions
    // (CameraClass::teleport side effects; what flips a slave to ESCAPING) so the
    // current RE exe can be disassembled offline — header RVAs are stale.
    {
        intptr_t baseR = (intptr_t)GetModuleHandleA(NULL);
        char rb[320];
        sprintf_s(rb, sizeof(rb),
            "[WASDCombat] dc_rva base=%p teleport=%llX stopFollowing=%llX startTrack=%llX isSlave=%llX setSlaveAIJob=%llX pmoDefault=%llX checkOrder=%llX addOrder=%llX addJob=%llX",
            (void*)baseR,
            (unsigned long long)(KenshiLib::GetRealAddress(&CameraClass::teleport) - baseR),
            (unsigned long long)(KenshiLib::GetRealAddress(&CameraClass::stopFollowing) - baseR),
            (unsigned long long)(KenshiLib::GetRealAddress(&PlayerInterface::startTrackCharacter) - baseR),
            (unsigned long long)(KenshiLib::GetRealAddress(&Character::isSlave) - baseR),
            (unsigned long long)(KenshiLib::GetRealAddress(&Character::setSlaveAIJob) - baseR),
            (unsigned long long)(KenshiLib::GetRealAddress(&Character::_NV_playerMoveOrderDefault) - baseR),
            (unsigned long long)(KenshiLib::GetRealAddress(&Character::checkPlayerOrderForProblems) - baseR),
            (unsigned long long)(KenshiLib::GetRealAddress(&Character::addOrder) - baseR),
            (unsigned long long)(KenshiLib::GetRealAddress(&Character::addJob) - baseR));
        DebugLog(rb);
    }
    // header-declared members resolved via GetRealAddress; no raw-RVA hooks.
    if (KenshiLib::SUCCESS != KenshiLib::AddHook(
            KenshiLib::GetRealAddress(&InputHandler::loadConfig),
            &inputLoadConfig_hook, &s_inputLoadConfigOrig))
        ErrorLog("WASDCombatPlugin: InputHandler::loadConfig hook FAILED — using INI keybind fallback");
    else
        DebugLog("WASDCombatPlugin: InputHandler::loadConfig hook OK");

    if (KenshiLib::SUCCESS != KenshiLib::AddHook(
            KenshiLib::GetRealAddress(&OptionsWindow::create),
            &optionsCreate_hook, &s_optionsCreateOrig))
        ErrorLog("WASDCombatPlugin: OptionsWindow::create hook FAILED — Controls-menu rows unavailable");
    else
        DebugLog("WASDCombatPlugin: OptionsWindow::create hook OK");

    if (KenshiLib::SUCCESS != KenshiLib::AddHook(
            KenshiLib::GetRealAddress(&OptionsWindow::saveOptions),
            &optionsSave_hook, &s_optionsSaveOrig))
        ErrorLog("WASDCombatPlugin: OptionsWindow::saveOptions hook FAILED — rebinds will not persist across restarts");
    else
        DebugLog("WASDCombatPlugin: saveOptions hook OK");

    if (KenshiLib::SUCCESS != KenshiLib::AddHook(
            KenshiLib::GetRealAddress(&GameWorld::processKeys),
            &processKeys_hook, &s_processKeysOrig))
        ErrorLog("WASDCombatPlugin: GameWorld::processKeys hook FAILED — using INI keybind fallback");
    else
    {
        s_processKeysHookOk = true;
        DebugLog("WASDCombatPlugin: processKeys hook OK");
    }

    if (KenshiLib::SUCCESS != KenshiLib::AddHook(
            KenshiLib::GetRealAddress(&GameWorld::_NV_mainLoop_GPUSensitiveStuff),
            &mainLoop_hook, &s_mainLoopOrig))
        ErrorLog("WASDCombatPlugin: mainLoop hook FAILED");
    else
        DebugLog("WASDCombatPlugin: mainLoop hook OK");

    if (KenshiLib::SUCCESS != KenshiLib::AddHook(
            KenshiLib::GetRealAddress(&CharMovement::_NV_update),
            &charMovUpdate_hook, &s_charMovUpdateOrig))
        ErrorLog("WASDCombatPlugin: charMovUpdate hook FAILED");
    else
        DebugLog("WASDCombatPlugin: charMovUpdate hook OK");

    // OTS action camera — drive (runs after the game's camera update, before
    // render) and the RTS-clamp bypass.  If either fails, OTS is unavailable
    // but the rest of DC is unaffected.
    if (KenshiLib::SUCCESS != KenshiLib::AddHook(
            KenshiLib::GetRealAddress(&CameraClass::update),
            &cameraUpdate_hook, &s_cameraUpdateOrig))
        ErrorLog("WASDCombatPlugin: CameraClass::update hook FAILED — OTS camera unavailable");
    else
        DebugLog("WASDCombatPlugin: cameraUpdate hook OK");

    if (KenshiLib::SUCCESS != KenshiLib::AddHook(
            KenshiLib::GetRealAddress(&CameraClass::getFacingDirection),
            &getFacing_hook, &s_getFacingOrig))
        ErrorLog("WASDCombatPlugin: getFacingDirection hook FAILED — FP map arrow may be inverted");
    else
        DebugLog("WASDCombatPlugin: getFacingDirection hook OK");

    if (KenshiLib::SUCCESS != KenshiLib::AddHook(
            KenshiLib::GetRealAddress(&CameraClass::restrictPosition),
            &restrictPos_hook, &s_restrictPosOrig))
        ErrorLog("WASDCombatPlugin: restrictPosition hook FAILED — OTS camera may clamp to floor");
    else
        DebugLog("WASDCombatPlugin: restrictPosition hook OK");

    if (KenshiLib::SUCCESS != KenshiLib::AddHook(
            KenshiLib::GetRealAddress(&PlayerInterface::playerControl),
            &playerControl_hook, &s_playerControlOrig))
        ErrorLog("WASDCombatPlugin: playerControl hook FAILED");
    else
        DebugLog("WASDCombatPlugin: playerControl hook OK");

    if (KenshiLib::SUCCESS != KenshiLib::AddHook(
            KenshiLib::GetRealAddress(&Character::removeJob),
            &removeJob_hook, &s_removeJobOrig))
        ErrorLog("WASDCombatPlugin: removeJob hook FAILED");
    else
        DebugLog("WASDCombatPlugin: removeJob hook OK");

    if (KenshiLib::SUCCESS != KenshiLib::AddHook(
            KenshiLib::GetRealAddress(&Character::addJob),
            &addJob_hook, &s_addJobOrig))
        ErrorLog("WASDCombatPlugin: addJob hook FAILED");
    else
        DebugLog("WASDCombatPlugin: addJob hook OK");

    // addOrder — door suppression on the player-order channel while the
    // post-WASD hold is active.
    if (KenshiLib::SUCCESS != KenshiLib::AddHook(
            KenshiLib::GetRealAddress(&Character::addOrder),
            &addOrder_hook, &s_addOrderOrig))
        ErrorLog("WASDCombatPlugin: addOrder hook FAILED — door suppression partial (addJob only)");
    else
        DebugLog("WASDCombatPlugin: addOrder hook OK");

    // PlayerInterface::playerMove — NOT INSTALLED (2026-08-05, crash-dump
    // verified).  RVA 0x7F95F0 was derived from the pre-2026-06-21 RE_Kenshi
    // exe; RE_Kenshi regenerated its patched Kenshi_x64.exe on 6/21 and all
    // code shifted.  On the current exe 0x7F95F0 is MID-INSTRUCTION (+0x2ED
    // into a NavMesh-path function, one byte into a 5-byte call at 0x7F95EF):
    // the MinHook E9 byte became that call's displacement low byte, and any
    // pathfind reaching the rare branch at 0x7F95EF jumped into unmapped
    // memory — the "pack bull + right-click inside hive home" crash.  The
    // hook never fired on this exe (0 log lines across full sessions); its
    // job is fully covered by the RMB press-edge poller + addOrder/addJob
    // gates, so nothing replaces it.  To re-enable: re-derive the RVA on the
    // CURRENT exe and install through verifyPatchSiteBytes().

    // combatGo — DC passive-combat model: suppress the controlled character's
    // combat AI unless engaged (manual attack / meleed) and not moving.
    if (KenshiLib::SUCCESS != KenshiLib::AddHook(
            KenshiLib::GetRealAddress(&CombatClass::_NV_go),
            &combatGo_hook, &s_combatGoOrig))
        ErrorLog("WASDCombatPlugin: combatGo hook FAILED — passive-combat model inactive");
    else
        DebugLog("WASDCombatPlugin: combatGo hook OK");

    // _iHitYouAreYouHit - perfect-block verdict (Build 4).  Header-declared,
    // resolved via GetRealAddress like every other hook here (no raw RVA).
    if (KenshiLib::SUCCESS != KenshiLib::AddHook(
            KenshiLib::GetRealAddress(&CombatClass::_iHitYouAreYouHit),
            &iHitYouAreYouHit_hook, &s_iHitOrig))
        ErrorLog("WASDCombatPlugin: iHitYouAreYouHit hook FAILED - perfect block inactive");
    else
        DebugLog("WASDCombatPlugin: iHitYouAreYouHit hook OK");

    if (KenshiLib::SUCCESS != KenshiLib::AddHook(
            KenshiLib::GetRealAddress(&CharStats::getDodge),
            &getDodge_hook, &s_getDodgeOrig))
        ErrorLog("WASDCombatPlugin: getDodge hook FAILED - dodge skill bonus inactive");
    else
        DebugLog("WASDCombatPlugin: getDodge hook OK");

    // AttackState::_NV_initialise - manual-attack mode denies the AI's own swings.
    {
        intptr_t ai = dcResolveKenshiLibExport("?_NV_initialise@AttackState@@QEAA_NXZ");
        if (!ai)
            ErrorLog("WASDCombatPlugin: AttackState::_NV_initialise export not found - manual attack mode cannot silence AI attacks");
        else if (KenshiLib::SUCCESS != KenshiLib::AddHook(ai, &attackInit_hook, &s_attackInitOrig))
            ErrorLog("WASDCombatPlugin: AttackState::_NV_initialise hook FAILED - manual attack mode cannot silence AI attacks");
        else
            DebugLog("WASDCombatPlugin: AttackState::_NV_initialise hook OK");
    }
    // Focus: the AI's target / threat queries answer with the lock.
    {
        intptr_t cat = dcResolveKenshiLibExport("?chooseAttackTarget@CombatClassAI@@QEAAPEAVCharacter@@XZ");
        if (!cat)
            ErrorLog("WASDCombatPlugin: CombatClassAI::chooseAttackTarget export not found - focus cannot pin the AI target");
        else if (KenshiLib::SUCCESS != KenshiLib::AddHook(cat, &chooseAttackTarget_hook, &s_chooseAttackTargetOrig))
            ErrorLog("WASDCombatPlugin: chooseAttackTarget hook FAILED - focus cannot pin the AI target");
        else
            DebugLog("WASDCombatPlugin: chooseAttackTarget hook OK");
    }
    if (KenshiLib::SUCCESS != KenshiLib::AddHook(
            KenshiLib::GetRealAddress(&Character::attackingYou),
            &attackingYou_hook, &s_attackingYouOrig))
        ErrorLog("WASDCombatPlugin: attackingYou hook FAILED - weapon glint off");
    else
        DebugLog("WASDCombatPlugin: attackingYou hook OK");
    if (KenshiLib::SUCCESS != KenshiLib::AddHook(
            KenshiLib::GetRealAddress(&CharStats::chooseBlock),
            &chooseBlock_hook, &s_chooseBlockOrig))
        ErrorLog("WASDCombatPlugin: chooseBlock hook FAILED - manual attack mode cannot silence AI blocks");
    else
        DebugLog("WASDCombatPlugin: chooseBlock hook OK");
    if (KenshiLib::SUCCESS != KenshiLib::AddHook(
            KenshiLib::GetRealAddress(DcRobResult<DcSetCombatStateTag>::ptr),
            &setCombatState_hook, &s_setCombatStateOrig))
        ErrorLog("WASDCombatPlugin: setCombatState hook FAILED - manual attack mode second choke point inactive");
    else
        DebugLog("WASDCombatPlugin: setCombatState hook OK");

    // initCombatMode and youKnowImAttacking hooks remain removed — DC does not
    // block combat ENTRY or attack notifications; only the per-frame go() decision
    // is gated (passive-combat model).

    // XHAIR-INDICATOR: observe cursor-context changes at the source.  These
    // are MyGUIEngine_x64.dll EXPORTS resolved by GetProcAddress — no RVAs to
    // go stale.  Record-and-forward only; if any resolve/hook fails the
    // indicator stays inert and the crosshair behaves exactly like v1.4.0.
    {
        HMODULE mg = GetModuleHandleA("MyGUIEngine_x64.dll");
        void* pSet  = mg ? (void*)GetProcAddress(mg,
            "?setPointer@PointerManager@MyGUI@@QEAAXAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@Z") : nullptr;
        void* pSetW = mg ? (void*)GetProcAddress(mg,
            "?setPointer@PointerManager@MyGUI@@AEAAXAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@PEAVWidget@2@@Z") : nullptr;
        void* pRst  = mg ? (void*)GetProcAddress(mg,
            "?resetToDefaultPointer@PointerManager@MyGUI@@QEAAXXZ") : nullptr;
        if (pSet && KenshiLib::SUCCESS == KenshiLib::AddHook(
                pSet, &xhSetPointer_hook, (void**)&s_xhSetPtrOrig))
            DebugLog("WASDCombatPlugin: PointerManager::setPointer hook OK");
        else
            ErrorLog("WASDCombatPlugin: PointerManager::setPointer hook FAILED — crosshair indicator inactive");
        if (pSetW && KenshiLib::SUCCESS == KenshiLib::AddHook(
                pSetW, &xhSetPointerW_hook, (void**)&s_xhSetPtrWOrig))
            DebugLog("WASDCombatPlugin: PointerManager::setPointer(owner) hook OK");
        if (pRst && KenshiLib::SUCCESS == KenshiLib::AddHook(
                pRst, &xhResetPointer_hook, (void**)&s_xhResetOrig))
            DebugLog("WASDCombatPlugin: PointerManager::resetToDefaultPointer hook OK");
    }

    // showTradeWindow — NOT INSTALLED (2026-08-05, same stale-RVA disease as
    // playerMove above).  On the current exe 0x7905D0 is MID-INSTRUCTION
    // (+0x50 into a double→int64 conversion helper at 0x790580, inside a
    // 10-byte movabs), so the patch was corrupting that helper's COMMON path
    // — wrong return values + dirty MMX state on every call — while the hook
    // itself never fired (the function is not showTradeWindow).  Loot/trade
    // detection has been carried entirely by the mainLoop GUI poll
    // (isAnyInventoryWindowOpen → s_lootUiSuspendActive) since 6/21; every
    // tested-good build ran that way, so nothing replaces this either.  To
    // re-enable: re-derive the RVA and install through verifyPatchSiteBytes().

    HANDLE h = CreateThread(nullptr, 0, PollThread, nullptr, 0, nullptr);
    if (!h)
    {
        char buf[64];
        sprintf_s(buf, sizeof(buf), "WASDCombatPlugin: poll thread FAILED err=%lu", GetLastError());
        ErrorLog(buf);
    }
    else
    {
        CloseHandle(h);
        DebugLog("WASDCombatPlugin: poll thread OK");
    }
}
