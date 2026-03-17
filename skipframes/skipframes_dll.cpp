#include "sayi_bilen.h"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <tlhelp32.h>
#include <windows.h>
//
//
#define OFF_CVAR_REG 0x2E960
#define OFF_CMD_PATCH 0x2809D
#define OFF_SCR_UPDATE 0x4C2A0

DWORD g_HwBase = 0;
#define IN_ATTACK (1 << 0)
#define IN_JUMP (1 << 1)
#define IN_DUCK (1 << 2)
#define IN_FORWARD (1 << 3)
#define IN_BACK (1 << 4)
#define IN_USE (1 << 5)
#define IN_CANCEL (1 << 6)
#define IN_LEFT (1 << 7)
#define IN_RIGHT (1 << 8)
#define IN_MOVELEFT (1 << 9)
#define IN_MOVERIGHT (1 << 10)
#define IN_ATTACK2 (1 << 11)
#define IN_RUN (1 << 12)
#define IN_RELOAD (1 << 13)
#define IN_ALT1 (1 << 14)
#define IN_SCORE (1 << 15)

#define FL_ONGROUND (1 << 9)
#define FL_DUCKING (1 << 1)
#define M_PI_F 3.14159265358979323846f
DWORD g_SCR = 0;
DWORD g_CreateInterface = 0;
DWORD g_SteamInternal = 0;
DWORD g_GetVolumeInfo = 0;
DWORD g_JmpTarget = 0;

// [NEW] Engine Functions
typedef void (*ClientCmd_t)(const char *);
extern ClientCmd_t g_pfnClientCmd;
typedef void (*ServerCmd_t)(const char *);
extern ServerCmd_t g_pfnServerCmd;

// [NEW] Command Gateway Hook (Catches ALL commands: console, server, UI)
typedef void (*Cbuf_AddText_t)(const char *);
extern Cbuf_AddText_t g_pfnCbuf_AddText;

// CVAR SYSTEM
typedef struct cvar_s {
  const char *name;
  const char *string;
  int flags;
  float value;
  struct cvar_s *next;
} cvar_t;

typedef void (*Cvar_RegisterVariable_t)(cvar_t *variable);
extern Cvar_RegisterVariable_t g_RegisterCvar;

// UTILS
typedef void (*ConPrintf_t)(const char *fmt, ...);
extern ConPrintf_t g_pfnConPrintf;

// ANTI-SS STATE
extern cvar_t g_cvar_cl_antiss;
extern DWORD g_AntiSS_EndTime; 
extern int g_AntiSS_PendingSnapshot; 
void TriggerAntiSS();

inline bool ShouldHideVisuals() {
  return (g_cvar_cl_antiss.value != 0.0f && (GetTickCount() < g_AntiSS_EndTime || g_AntiSS_PendingSnapshot > 0));
}

void Hook_Cbuf_AddText(const char *szText);
extern BYTE g_Orig_Cbuf[12];

void Hook_Cbuf_AddText(const char *szText) {
  // 1. Logic (BEFORE unhooking) - Block if it's a snapshot
  bool block = false;
  if (szText && g_cvar_cl_antiss.value != 0.0f) {
    if (strstr(szText, "snapshot") || strstr(szText, "screenshot") || strstr(szText, "screendump")) {
        TriggerAntiSS();
        g_AntiSS_PendingSnapshot = 15; // Wait 15 frames (very safe)
        block = true;
    }
  }

  if (block) return; // DON'T call original

  // 2. Unhook
  if (g_pfnCbuf_AddText)
    memcpy((void *)g_pfnCbuf_AddText, g_Orig_Cbuf, 6);

  // 3. Call Original
  if (g_pfnCbuf_AddText)
    g_pfnCbuf_AddText(szText);

  // 4. Rehook
  if (g_pfnCbuf_AddText) {
    BYTE patch[5] = {0xE9, 0, 0, 0, 0};
    *(DWORD *)(patch + 1) = (DWORD)Hook_Cbuf_AddText - (DWORD)g_pfnCbuf_AddText - 5;
    memcpy((void *)g_pfnCbuf_AddText, patch, 5);
    *(BYTE *)((DWORD)g_pfnCbuf_AddText + 5) = 0x90;
  }
}


bool g_HooksActive = false;
bool g_StealthMode = false;

// STORAGE
BYTE g_Orig_SCR[6];
BYTE g_Orig_CI[5];
BYTE g_Orig_SI[5];
BYTE g_Orig_Vol[5];

// LOGGING
void Log(const char *fmt, ...) {
#ifndef NDEBUG
  char buf[4096];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
  va_end(ap);
  buf[sizeof(buf) - 1] = 0;
  OutputDebugStringA(buf);
#endif
}
#define LogDebug Log

// -------------------------------------------------------------
// ESP UTILS
// -------------------------------------------------------------
#define ESP_ORIGIN_OFFSET 704

inline void ReadOrigin(void *ent, int offset, float &x, float &y, float &z) {
  if (!ent) return;
  float *fp = (float *)((char *)ent + offset);
  x = fp[0];
  y = fp[1];
  z = fp[2];
}

// Identify team based on model name strings (Pure C-style for small DLL size)
inline int GetTeamFromModel(const char *model) {
  if (!model) return 0;
  char s[64];
  strncpy(s, model, 63);
  s[63] = 0;
  for (int i = 0; s[i]; i++) {
    if (s[i] >= 'A' && s[i] <= 'Z') s[i] += 32;
  }

  // CT Models
  if (strstr(s, "urban") || strstr(s, "gsg9") || strstr(s, "sas") || 
      strstr(s, "gign") || strstr(s, "ct_") || strstr(s, "spetsnaz"))
    return 2;

  // T Models
  if (strstr(s, "terror") || strstr(s, "leet") || strstr(s, "arctic") || 
      strstr(s, "guerilla") || strstr(s, "militia") || strstr(s, "te_"))
    return 1;

  return 0;
}

// -------------------------------------------------------------
// CVARS
// -------------------------------------------------------------
Cvar_RegisterVariable_t g_RegisterCvar = nullptr;

cvar_t g_cvar_frame_skip = {"frame_skip", "0", 0, 0.0f, nullptr};
cvar_t g_cvar_change_id = {"change_id", "0", 0, 0.0f, nullptr};
cvar_t g_cvar_sb = {"sb", "0", 0, 0.0f, nullptr};
cvar_t g_cvar_sb_delay = {"sb_delay", "5", 0, 0.0f, nullptr};
cvar_t g_cvar_sb_range = {"sb_range", "100", 0, 0.0f, nullptr};
cvar_t g_cvar_no_smoke = {"no_smoke", "1", 0, 1.0f, nullptr};
cvar_t g_cvar_speedometer = {"speedometer", "1", 0, 1.0f, nullptr};
cvar_t g_cvar_ct_esp = {"esp_ct", "0", 0, 0.0f, nullptr};
cvar_t g_cvar_t_esp = {"esp_t", "0", 0, 0.0f, nullptr};
cvar_t g_cvar_esp_type = {"esp_type", "3", 0, 0.0f,
                          nullptr}; // 1=Glow, 2=Box, 3=Both
cvar_t g_cvar_esp_label = {"esp_label", "0", 0, 0.0f, nullptr}; // 1=show labels
cvar_t g_cvar_showfps = {"showfps", "1", 0, 0.0f, nullptr};
// [NEW] Custom Crosshair
cvar_t g_cvar_ch = {"ch", "1", 0, 0.0f, nullptr};
cvar_t g_cvar_ch_color = {"ch_color", "0 255 255", 0, 0.0f, nullptr};
cvar_t g_cvar_ch_length = {"ch_length", "6", 0, 0.0f, nullptr};
cvar_t g_cvar_ch_offset = {"ch_offset", "4", 0, 0.0f, nullptr};
cvar_t g_cvar_ch_thickness = {"ch_thickness", "1", 0, 0.0f, nullptr};
cvar_t g_cvar_ch_import = {"ch_import", "", 0, 0.0f, nullptr};
cvar_t g_cvar_ch_export = {"ch_export", "", 0, 0.0f, nullptr};
// [NEW] Help Command
cvar_t g_cvar_speedometer_color = {"speedometer_color", "0 255 255", 0, 0.0f,
                                   nullptr};
cvar_t g_cvar_hide_knife = {"hide_knife", "0", 0, 0.0f, nullptr};
cvar_t g_cvar_hide_entities = {"hide_entities", "0", 0, 0.0f, nullptr};
cvar_t g_cvar_anti_drug = {"anti_drug", "1", 0, 1.0f, nullptr};
cvar_t g_cvar_strafe_helper = {"strafe_helper", "0", 0, 0.0f, nullptr};
cvar_t g_cvar_sgs = {"sgs", "0", 0, 0.0f, nullptr};
cvar_t g_cvar_cl_antiss = {"anti_ss", "1", 0, 1.0f, nullptr};
cvar_t g_cvar_null_canceling_movement = {"null_canceling_movement", "1", 0, 0.0f, nullptr};
cvar_t g_cvar_qs = {"quick_scope", "1", 0, 0.0f, nullptr};
cvar_t g_cvar_no_scope = {"no_scope", "1", 0, 0.0f, nullptr};
cvar_t *g_pCvar_SideSpeed = nullptr;
cvar_t *g_pCvar_ForwardSpeed = nullptr;
cvar_t *g_pCvar_BackSpeed = nullptr;
void Cmd_ShowHelp() {
  if (g_pfnConPrintf) {
    ((void (*)(const char *, ...))g_pfnConPrintf)("--- SkipFrames Commands ---\n");
    ((void (*)(const char *, ...))g_pfnConPrintf)(
        "  frame_skip <value>  - Skip frames (0=Off)\n");
    ((void (*)(const char *, ...))g_pfnConPrintf)(
        "  change_id <0/1>     - Toggle ID Changer\n");
    ((void (*)(const char *, ...))g_pfnConPrintf)(
        "  sf_help             - Show Help Menu\n");
    ((void (*)(const char *, ...))g_pfnConPrintf)(
        "  sb <0/1>            - Toggle SayiBilen (1=Ready)\n");
    ((void (*)(const char *, ...))g_pfnConPrintf)(
        "  sb_range <N>        - Guess Range (Default: 100)\n");
    ((void (*)(const char *, ...))g_pfnConPrintf)(
        "  sb_delay <MS>       - Delay in ms (Default: 5)\n");
    ((void (*)(const char *, ...))g_pfnConPrintf)(
        "  esp_ct <0/1>        - Toggle CT ESP\n");
    ((void (*)(const char *, ...))g_pfnConPrintf)(
        "  esp_t <0/1>         - Toggle T ESP\n");
    ((void (*)(const char *, ...))g_pfnConPrintf)(
        "  esp_type <1/2/3>    - 1=Glow 2=Box 3=Both\n");
    ((void (*)(const char *, ...))g_pfnConPrintf)(
        "  esp_label <0/1>     - Toggle Name+Distance Labels\n");
    ((void (*)(const char *, ...))g_pfnConPrintf)(
        "  no_smoke <0/1>      - Toggle Smoke Removal\n");
    ((void (*)(const char *, ...))g_pfnConPrintf)(
        "  ch <0/1>            - Toggle Custom Crosshair\n");
    ((void (*)(const char *, ...))g_pfnConPrintf)(
        "  ch_color            - RGB color (e.g., \"0 255 0\")\n");
    ((void (*)(const char *, ...))g_pfnConPrintf)(
        "  ch_length           - Crosshair line length\n");
    ((void (*)(const char *, ...))g_pfnConPrintf)(
        "  ch_offset           - Crosshair center offset\n");
    ((void (*)(const char *, ...))g_pfnConPrintf)(
        "  ch_thickness        - Crosshair line thickness\n");
    ((void (*)(const char *, ...))g_pfnConPrintf)(
        "  ch_import <name>    - Import cstrike/name.txt profile\n");
    ((void (*)(const char *, ...))g_pfnConPrintf)(
        "  ch_export <name>    - Export cstrike/name.txt profile\n");
    ((void (*)(const char *, ...))g_pfnConPrintf)(
        "  speedometer <0/1>   - Toggle Speedometer\n");
    ((void (*)(const char *, ...))g_pfnConPrintf)(
        "  speedometer_color   - RGB color (e.g., \"0 255 255\")\n");
    ((void (*)(const char *, ...))g_pfnConPrintf)(
        "  hide_knife <0/1>    - Hide Knife Viewmodel\n");
    ((void (*)(const char *, ...))g_pfnConPrintf)(
        "  hide_entities <0/1> - Hide Non Solid Map Entities\n");
    ((void (*)(const char *, ...))g_pfnConPrintf)(
        "  showfps <0/1>       - Toggle Real FPS Counter\n");
    ((void (*)(const char *, ...))g_pfnConPrintf)(
        "  anti_drug <0/1>     - Block server drug effects (FOV > 90)\n");
    ((void (*)(const char *, ...))g_pfnConPrintf)(
        "  +strafe_boost       - Hold to auto-perfect air acceleration\n");
    ((void (*)(const char *, ...))g_pfnConPrintf)(
        "  +auto_bhop          - Hold to auto crouch-jump on landing\n");
    ((void (*)(const char *, ...))g_pfnConPrintf)(
        "  +sgs                - Hold to auto Ground-Strafe (sgs 1/2)\n");
    ((void (*)(const char *, ...))g_pfnConPrintf)(
        "  sgs <0/1/2>         - 0=Off 1=Legit 2=Rage (with boost)\n");
    ((void (*)(const char *, ...))g_pfnConPrintf)(
        "  quick_scope <0/1>   - Auto scope-then-shoot with QQ switch\n");
    ((void (*)(const char *, ...))g_pfnConPrintf)(
        "  anti_ss <0/1>       - Hide visuals during screenshots\n");
    ((void (*)(const char *, ...))g_pfnConPrintf)(
        "  null_canceling_movement <0/1> - Prevents standing still (Snap Tap)\n");
    ((void (*)(const char *, ...))g_pfnConPrintf)(
        "  F11                 - Toggle Stealth (Freeze-Patch-Thaw)\n");
    ((void (*)(const char *, ...))g_pfnConPrintf)(
        "  +strafe_helper      - Hold to auto-strafe/detect keys\n");
    ((void (*)(const char *, ...))g_pfnConPrintf)(
        "  strafe_helper <0/1/2> - 0=Off 1=Legit 2=Rage\n");
    ((void (*)(const char *, ...))g_pfnConPrintf)(
        "  no_scope <0/1>      - Force crosshair for snipers while unscoped\n");
    ((void (*)(const char *, ...))g_pfnConPrintf)(
        "--------------------\n");
  }
}

// [NEW] +strafe_boost Engine Command
typedef int (*AddCommand_t)(char *, void (*)());
AddCommand_t g_pfnAddCommand = nullptr;
bool g_StrafeBoostActive = false;

void Cmd_StrafeBoost_On() { g_StrafeBoostActive = true; }
void Cmd_StrafeBoost_Off() { g_StrafeBoostActive = false; }

// [NEW] +auto_bhop Engine Command
bool g_AutoBhopActive = false;
void Cmd_AutoBhop_On() { g_AutoBhopActive = true; }
void Cmd_AutoBhop_Off() { g_AutoBhopActive = false; }

// [NEW] +sgs Engine Command
bool g_SGSActive = false;
void Cmd_SGS_On() { g_SGSActive = true; }
void Cmd_SGS_Off() { g_SGSActive = false; }
bool g_StrafeHelperActive = false;
void Cmd_StrafeHelper_On() { g_StrafeHelperActive = true; }
void Cmd_StrafeHelper_Off() { g_StrafeHelperActive = false; }

// CL_CreateMove hook globals
typedef void (__cdecl *HUD_CL_CreateMove_t)(float, void *, int);
HUD_CL_CreateMove_t g_Original_CL_CreateMove = nullptr;
DWORD g_CL_CreateMove_Addr = 0;
BYTE g_Orig_CL_CreateMove[6] = {0};

// Player flags from playermove_s (for ground detection)
int g_PlayerFlags = 0;
int g_PlayerTeam[33] = {0}; // 0=Unknown, 1=T, 2=CT

// TriAPI WorldToScreen (built-in engine W2S)
typedef int (*TriAPI_WorldToScreen_t)(float *world, float *screen);
TriAPI_WorldToScreen_t g_pfnTriWorldToScreen = nullptr;

// -------------------------------------------------------------
// PATTERN SCANNING
// -------------------------------------------------------------
bool Compare(const char *pData, const char *pPattern, const char *pMask) {
  for (; *pMask; ++pMask, ++pData, ++pPattern) {
    if (*pMask == 'x' && *pData != *pPattern)
      return false;
  }
  return (*pMask) == 0;
}

DWORD FindPattern(DWORD start, DWORD size, const char *pattern,
                  const char *mask) {
  for (DWORD i = 0; i < size; i++) {
    if (Compare((char *)(start + i), pattern, mask)) {
      return start + i;
    }
  }
  return 0;
}

// Helper: Find string in memory (with null terminator check)
DWORD FindStringRef(DWORD start, DWORD size, const char *str) {
  int len = strlen(str);
  DWORD strAddr = 0;
  LogDebug("[FindStringRef] Searching for '%s' (len=%d) in range 0x%X - 0x%X\n",
           str, len, start, start + size);

  // 1. Find Data String (MUST include null terminator to avoid partial matches
  // like SayText2)
  for (DWORD i = 0; i < size - len; i++) {
    // Optim: Check readability every 4KB page
    if ((i % 4096) == 0) {
      if (IsBadReadPtr((void *)(start + i), 4096)) {
        i += 4095;
        continue;
      }
    }

    // Fast scan for first char
    if (*(char *)(start + i) != str[0])
      continue;

    // Check full string + NULL terminator
    if (memcmp((void *)(start + i), str, len) == 0 &&
        *(char *)(start + i + len) == 0) {
      strAddr = start + i;
      LogDebug("[FindStringRef] Found string '%s\\0' at 0x%X\n", str, strAddr);
      break;
    }
  }

  if (!strAddr) {
    LogDebug("[FindStringRef] String '%s' NOT FOUND!\n", str);
    return 0;
  }

  // 2. Find PUSH strAddr (68 XX XX XX XX)
  LogDebug("[FindStringRef] Looking for PUSH 0x%X in code section...\n",
           strAddr);

  // Scan smaller range for code (usually .text is first) or just scan all
  // To be safe, we re-check readability
  for (DWORD i = 0; i < size - 5; i++) {
    if ((i % 4096) == 0) {
      if (IsBadReadPtr((void *)(start + i), 4096)) {
        i += 4095;
        continue;
      }
    }

    if (*(BYTE *)(start + i) == 0x68) {
      if (*(DWORD *)(start + i + 1) == strAddr) {
        LogDebug("[FindStringRef] SUCCESS! Found PUSH at 0x%X\n", start + i);
        return start + i;
      }
    }
  }

  LogDebug("[FindStringRef] PUSH 0x%X NOT FOUND!\n", strAddr);
  return 0;
}

// -------------------------------------------------------------
// HOOK TYPEDEFS
// -------------------------------------------------------------
void InstallHooks(); // Forward declaration
typedef int(__fastcall *InitiateGameConnection_t)(void *, int, void *, int,
                                                  uint64_t, uint32_t, uint16_t,
                                                  bool);
typedef int(__cdecl *pfnUserMsgHook)(const char *pszName, int iSize,
                                     void *pbuf);
typedef int(__cdecl *pfnHookUserMsg_t)(char *szMsgName, pfnUserMsgHook pfn);
pfnHookUserMsg_t g_pfnHookUserMsg = nullptr;

// --- Weapon Tracking (for hide_knife) ---
int g_CurrentWeaponID = 0;
pfnUserMsgHook g_Original_CurWeapon = nullptr;
cvar_t *g_pCvar_drawviewmodel = nullptr; // Cached pointer - scanned once

// UserMsg Handlers
int __cdecl MsgFunc_SayText(const char *pszName, int iSize, void *pbuf);
int __cdecl MsgFunc_ScreenFade(const char *pszName, int iSize, void *pbuf);
InitiateGameConnection_t g_Original_Initiate = nullptr;
// -------------------------------------------------------------
// ENGINE TABLE
// -------------------------------------------------------------
// Global pointer to the engine table (array of void*)
void **g_EngineTable = nullptr;

// Half-Life SDK cl_enginefunc_t indices (from cdll_int.h):
// [11] pfnFillRGBA       [12] pfnGetScreenInfo
// [14] pfnRegisterVariable  [18] pfnHookUserMsg
// [20] pfnClientCmd      [27] pfnDrawConsoleString
// [28] pfnDrawSetTextColor  [40] Con_Printf
// [51] GetLocalPlayer    [53] GetEntityByIndex
// [82] pTriAPI (struct with WorldToScreen at offset 48)
// We will use a raw pointer table.

// Helpers to call engine functions
typedef void (*DrawSetTextColor_t)(float r, float g, float b);
typedef int (*DrawConsoleString_t)(int x, int y, const char *string);
typedef void *(*GetLocalPlayer_t)();
typedef void (*FillRGBA_t)(int x, int y, int w, int h, int r, int g, int b,
                           int a);
typedef void *(*GetEntityByIndex_t)(int idx);

FillRGBA_t g_pfnFillRGBA = nullptr;
GetEntityByIndex_t g_pfnGetEntityByIndex = nullptr;
DrawSetTextColor_t g_pfnDrawSetTextColor = nullptr;
DrawConsoleString_t g_pfnDrawConsoleString = nullptr;
GetLocalPlayer_t g_pfnGetLocalPlayer = nullptr;

// SCREENINFO for screen resolution
struct SCREENINFO {
  int iSize;
  int iWidth;
  int iHeight;
  int iFlags;
  int iCharHeight;
  short charWidths[256];
};
typedef void (*GetScreenInfo_t)(SCREENINFO *pscrinfo);
GetScreenInfo_t g_pfnGetScreenInfo = nullptr;


// [NEW] FPS Counter Globals
extern "C" volatile int g_SkipVal;
float g_RealFPS = 0.0f;
int g_FPSFrameCount = 0;
DWORD g_LastFPSUpdateTime = 0;

// Player info for ESP names
struct hud_player_info_t {
  char *name;
  short ping;
  unsigned char thisplayer;
  unsigned char spectator;
  unsigned char packetloss;
  char *model;
  short topcolor;
  short bottomcolor;
};
typedef void (*GetPlayerInfo_t)(int ent_num, hud_player_info_t *pinfo);
GetPlayerInfo_t g_pfnGetPlayerInfo = nullptr;

// Minimal entity state for reading velocity
struct entity_state_t {
  int entityType;
  int number;
  float msg_time;
  int messagenum;
  float origin[3];
  float angles[3];
  int modelindex;
  int sequence;
  float frame;
  int colormap;
  short skin;
  short solid;
  int effects;
  float scale;
  BYTE eflags;
  int rendermode;
  int renderamt;
  int rendercolor;
  int renderfx;
  int movetype;
  float animtime;
  float framerate;
  int body;
  BYTE controller[4];
  BYTE blending[4];
  float velocity[3];
};

struct cl_entity_t {
  int index;
  BYTE bUnknown[20]; // internal padding prefix
  void *model;
  void *epuzzle;
  float baseline[10];      // approximate size
  entity_state_t curstate; // Velocity is at the end of this struct
};

// -------------------------------------------------------------
// ENGINE GLOBAL DEFINITIONS
// -------------------------------------------------------------
ClientCmd_t g_pfnClientCmd = nullptr;
ServerCmd_t g_pfnServerCmd = nullptr;
Cbuf_AddText_t g_pfnCbuf_AddText = nullptr;
BYTE g_Orig_Cbuf[12] = {0};

// -------------------------------------------------------------
// HOOK TYPEDEFS
// -------------------------------------------------------------

// [NEW] UserMsg Hook (Blind Dump + Keyword Scan)
typedef void (*UserMsg_t)(void *, void *, void *);
UserMsg_t g_pfnUserMsg = nullptr;
BYTE g_Orig_UserMsg[10];

// [NEW] Con_Printf Hook
typedef void (*ConPrintf_t)(const char *fmt, ...);
ConPrintf_t g_pfnConPrintf = nullptr;
BYTE g_Orig_ConPrintf[10];

// [NEW] HUD_PlayerMove Hook (for perfect instantaneous velocity)
typedef void (*HUD_PlayerMove_t)(void *ppmove, int server);
HUD_PlayerMove_t g_Original_HUD_PlayerMove = nullptr;
DWORD g_HUD_PlayerMove_Addr = 0;
BYTE g_Orig_HUD_PlayerMove[10];
float g_TrueEngineVelocity[3] = {0.0f, 0.0f, 0.0f};
DWORD g_LastVelocityUpdateTime = 0;

int g_QuickScopeState = 0;
int g_QS_WaitTicks = 0;

// [NEW] Anti-Screenshot (Anti-SS) State
DWORD g_AntiSS_EndTime = 0;
int g_AntiSS_PendingSnapshot = 0;

void TriggerAntiSS() {
    g_AntiSS_EndTime = GetTickCount() + 1500; // Total 1.5s protection
}

void Hook_ClientCmd(const char *szCmdString) {
  if (szCmdString && g_cvar_cl_antiss.value != 0.0f) {
    if (strstr(szCmdString, "snapshot") || strstr(szCmdString, "screenshot") || strstr(szCmdString, "screendump")) {
        TriggerAntiSS();
        g_AntiSS_PendingSnapshot = 10;
        return; // Block
    }
  }
  if (g_pfnClientCmd)
    g_pfnClientCmd(szCmdString);
}

void Hook_ServerCmd(const char *szCmdString) {
  if (szCmdString && g_cvar_cl_antiss.value != 0.0f) {
    if (strstr(szCmdString, "snapshot") || strstr(szCmdString, "screenshot") || strstr(szCmdString, "screendump")) {
        TriggerAntiSS();
        g_AntiSS_PendingSnapshot = 10;
        return; // Block
    }
  }
  if (g_pfnServerCmd)
    g_pfnServerCmd(szCmdString);
}

// --- FindCvarByName: Walk engine cvar linked list (cached, scan once) ---
cvar_t *FindCvarByName(const char *name) {
  if (!name) return nullptr;
  // Walk the engine's cvar linked list starting from our registered cvar
  cvar_t *current = g_cvar_frame_skip.next;
  int limit = 10000;
  while (current && limit-- > 0) {
    if (!IsBadReadPtr(current, sizeof(cvar_t)) && current->name &&
        !IsBadReadPtr(current->name, 1) && !strcmp(current->name, name)) {
      return current;
    }
    current = current->next;
  }
  return nullptr;
}

// --- HUD_AddEntity Hook (Hide Knife + Hide Entities) ---
typedef int(__cdecl *HUD_AddEntity_t)(int type, void *ent, const char *modelname);
DWORD g_HUD_AddEntity_Addr = 0;
BYTE g_Orig_HUD_AddEntity[10];

int __cdecl Hook_HUD_AddEntity(int type, void *ent, const char *modelname) {
  // --- HIDE KNIFE: Direct memory write to r_drawviewmodel ---
  if (g_cvar_hide_knife.value > 0.0f) {
    // Search ONCE only (avoid 10K IsBadReadPtr walk every frame)
    static bool s_drawvmSearched = false;
    if (!s_drawvmSearched && !g_pCvar_drawviewmodel) {
      s_drawvmSearched = true;
      g_pCvar_drawviewmodel = FindCvarByName("r_drawviewmodel");
    }
    if (g_pCvar_drawviewmodel) {
      float desired = (g_CurrentWeaponID == 29) ? 0.0f : 1.0f;
      if (g_pCvar_drawviewmodel->value != desired)
        g_pCvar_drawviewmodel->value = desired;
    }
  }
  // --- HIDE ENTITIES: Block non-player entities (except weapons + SOLID brushes) ---
  #define ENT_SOLID_OFFSET 746
  if (g_HooksActive && g_cvar_hide_entities.value > 0.0f && ent) {
    int index = *(int *)ent;
    if (index > 32) {
      if (modelname && strstr(modelname, "/w_")) {
        // Dropped weapon — let it render
      }
      else if (modelname && modelname[0] == '*') {
        short solid = *(short *)((char *)ent + ENT_SOLID_OFFSET);
        if (solid <= 0) return 0; // Non-solid brush — HIDE
      }
      else {
        return 0; // Non-brush entity — HIDE
      }
    }
  }

  // Unhook -> call original -> rehook (memory kept permanently unlocked)
  memcpy((void *)g_HUD_AddEntity_Addr, g_Orig_HUD_AddEntity, 6);
  int result = ((HUD_AddEntity_t)g_HUD_AddEntity_Addr)(type, ent, modelname);
  BYTE patch[5] = {0xE9, 0, 0, 0, 0};
  *(DWORD *)(patch + 1) =
      (DWORD)Hook_HUD_AddEntity - (DWORD)g_HUD_AddEntity_Addr - 5;
  memcpy((void *)g_HUD_AddEntity_Addr, patch, 5);
  *(BYTE *)((DWORD)g_HUD_AddEntity_Addr + 5) = 0x90;

  return result;
}

// --- CurWeapon UserMsg Handler ---
int __cdecl MsgFunc_CurWeapon(const char *pszName, int iSize, void *pbuf) {
  if (iSize >= 3 && pbuf) {
    BYTE *data = (BYTE *)pbuf;
    int state = data[0]; // 1 = active weapon
    int wpnID = data[1]; // Weapon ID (Knife = 29)
    if (state == 1) {
      g_CurrentWeaponID = wpnID;
    }
  }
  if (g_Original_CurWeapon)
    return g_Original_CurWeapon(pszName, iSize, pbuf);
  return 0;
}

void Hook_HUD_PlayerMove(void *ppmove, int server) {
  // Fast-Patch: Memory is kept unlocked. Swap bytes instantly.
  memcpy((void *)g_HUD_PlayerMove_Addr, g_Orig_HUD_PlayerMove, 6);

  ((HUD_PlayerMove_t)g_HUD_PlayerMove_Addr)(ppmove, server);

  if (ppmove && !IsBadReadPtr(ppmove, 128)) {
    g_TrueEngineVelocity[0] = *(float *)((char *)ppmove + 92);
    g_TrueEngineVelocity[1] = *(float *)((char *)ppmove + 96);
    g_TrueEngineVelocity[2] = *(float *)((char *)ppmove + 100);
    g_LastVelocityUpdateTime = GetTickCount();

    g_PlayerFlags = *(int *)((char *)ppmove + 184); // flags (FL_ONGROUND etc.)
  }

  // Hook back IN
  BYTE patch[5] = {0xE9, 0, 0, 0, 0};
  *(DWORD *)(patch + 1) =
      (DWORD)Hook_HUD_PlayerMove - (DWORD)g_HUD_PlayerMove_Addr - 5;
  memcpy((void *)g_HUD_PlayerMove_Addr, patch, 5);
  *(BYTE *)((DWORD)g_HUD_PlayerMove_Addr + 5) = 0x90;
}

void Hook_ConPrintf(const char *fmt, void *a1, void *a2, void *a3) {
  // 1. Unhook (Fast-Patch: Memory is kept permanently unlocked)
  memcpy((void *)g_pfnConPrintf, g_Orig_ConPrintf, 6);

  // 2. Only process if SayiBilen is active (skip all work otherwise)
  if (g_SayiBilenActive && fmt) {
    SayiBilen_OnMessage(fmt);
  }

  // 3. Call Original
  ((void (*)(const char *, ...))g_pfnConPrintf)(fmt, a1, a2, a3, 0, 0, 0, 0, 0);

  // 4. Rehook
  BYTE patch[5] = {0xE9, 0, 0, 0, 0};
  *(DWORD *)(patch + 1) = (DWORD)Hook_ConPrintf - (DWORD)g_pfnConPrintf - 5;
  memcpy((void *)g_pfnConPrintf, patch, 5);
  *(BYTE *)((DWORD)g_pfnConPrintf + 5) = 0x90;
}

// -------------------------------------------------------------
// USERMSG HANDLERS
// -------------------------------------------------------------
pfnUserMsgHook g_Original_SayText = nullptr;
pfnUserMsgHook g_Original_ScreenFade = nullptr;
pfnUserMsgHook g_Original_SetFOV = nullptr;
pfnUserMsgHook g_Original_HideWeapon = nullptr;

bool g_NoFlash = true; // Block ScreenFade
bool g_NoSmoke = true; // Block smoke sprites

int g_CurrentFOV = 90;
byte g_ServerHideWeaponFlags = 0;
int g_CrosshairsSpriteID = -1;
int g_CrosshairSpriteID = -1;
int g_SniperScopeSpriteID = -1;

// No-Smoke: Core Engine Sprite Hooks
// We hook the actual sprite rendering functions in the engine table.
// This is robust because ALL sprites (temp ents, env_sprite, etc.) use these.

void WriteJMP(DWORD from, DWORD to, BYTE *storage, int len);
void Restore(DWORD from, BYTE *storage, int len);

typedef void(__cdecl *SPR_Set_t)(int hPic, int r, int g, int b);
typedef void(__cdecl *SPR_Draw_t)(int frame, int x, int y, const void *prc);
typedef void(__cdecl *SPR_DrawHoles_t)(int frame, int x, int y,
                                       const void *prc);
typedef void(__cdecl *SPR_DrawAdditive_t)(int frame, int x, int y,
                                          const void *prc);

SPR_Set_t g_Original_SPR_Set = nullptr;
SPR_Draw_t g_Original_SPR_Draw = nullptr;
SPR_Draw_t g_Original_SPR_DrawHoles = nullptr;
SPR_Draw_t g_Original_SPR_DrawAdditive = nullptr;

BYTE g_Orig_SPR_Set[10] = {0};
BYTE g_Orig_SPR_Draw[10] = {0};
BYTE g_Orig_SPR_DrawHoles[10] = {0};
BYTE g_Orig_SPR_DrawAdditive[10] = {0};

// State to track if the current sprite operation should be blocked
bool g_BlockCurrentSprite = false;

typedef int(__cdecl *SPR_Load_t)(const char *pTextureName);
SPR_Load_t g_pfnSPR_Load = nullptr;
SPR_Load_t g_pfnSPR_Load_Original = nullptr;
bool g_Original_SPR_Load_Hook = false;

// Hook for SPR_Load (Index 0)
int __cdecl Hook_SPR_Load(const char *name) {
  // LogDebug("[SPR_Load] %s\n", name);
  if (g_NoSmoke && name &&
      (strstr(name, "gas_puff") || strstr(name, "smoke") ||
       strstr(name, "black_smoke"))) {
    LogDebug("[NoSmoke] Blocked loading of %s via SPR_Load Hook!\n", name);
    return 0; // Block it
  }
  if (g_pfnSPR_Load_Original)
    return g_pfnSPR_Load_Original(name);
  return 0;
}

// Multiple smoke model indices (different CS builds use different sprites)
#define MAX_SMOKE_MODELS 5
int g_SmokeModelIndices[MAX_SMOKE_MODELS] = {-1, -1, -1, -1, -1};
int g_NumSmokeModels = 0;

// No-Smoke: Smoke creation function patch (legacy, kept for F11 stealth)
DWORD g_SmokeFuncAddr = 0;
BYTE g_SmokeFuncOrigByte = 0;

// =============================================================
// EfxAPI HOOKS (Indices 49, 50, 54, 55, 58)
// To catch ALL possible smoke creation methods!
// =============================================================
void *g_pEfxAPI = nullptr;

// Helper: Lazy-load smoke sprites
void EnsureSmokeModelsLoaded() {
  // LogDebug("[NoSmoke] EnsureSmokeModelsLoaded() called (Count=%d)\n",
  // g_NumSmokeModels);
  if (g_NumSmokeModels <= 0 && g_pfnSPR_Load) {
    static bool triedLoading = false;
    if (!triedLoading) {
      triedLoading = true;
      const char *smokeSprites[] = {
          "sprites/gas_puff_01.spr",  "sprites/smokepuff.spr",
          "sprites/smoke.spr",        "sprites/black_smoke1.spr",
          "sprites/black_smoke4.spr", "sprites/steam1.spr"};
      for (int i = 0; i < 6 && g_NumSmokeModels < MAX_SMOKE_MODELS; i++) {
        int idx = g_pfnSPR_Load(smokeSprites[i]);
        if (idx > 0) {
          g_SmokeModelIndices[g_NumSmokeModels++] = idx;
          LogDebug("[NoSmoke] Loaded %s -> handle %d\n", smokeSprites[i], idx);
        }
      }
      
      // Load crosshair sprites for blocking
      g_CrosshairsSpriteID = g_pfnSPR_Load("sprites/crosshairs.spr");
      g_CrosshairSpriteID = g_pfnSPR_Load("sprites/crosshair.spr");
      g_SniperScopeSpriteID = g_pfnSPR_Load("sprites/sniper_scope.spr");
    }
  }
}

// --- Index 49: R_Sprite_Explosion ---
BYTE g_Orig_R_Sprite_Explosion[10] = {0};
typedef void(__cdecl *R_Sprite_Explosion_t)(float *pos, int modelIndex,
                                            int count);
R_Sprite_Explosion_t g_Original_R_Sprite_Explosion = nullptr;

void __cdecl Hook_R_Sprite_Explosion(float *pos, int modelIndex, int count) {
  EnsureSmokeModelsLoaded();

  static int logCount = 0;
  if (logCount < 10) {
    LogDebug("[R_Sprite_Explosion] modelIndex=%d count=%d\n", modelIndex,
             count);
    logCount++;
  }

  // Filter
  if (g_NoSmoke && g_NumSmokeModels > 0) {
    for (int i = 0; i < g_NumSmokeModels; i++) {
      if (modelIndex == g_SmokeModelIndices[i])
        return; // Block
    }
  }

  // Table hooked: Call original directly
  if (g_Original_R_Sprite_Explosion)
    g_Original_R_Sprite_Explosion(pos, modelIndex, count);
}

// --- Index 50: R_TempSprite (Existing) ---
BYTE g_Orig_R_TempSprite[10] = {0};
typedef void *(__cdecl *R_TempSprite_t)(float *pos, float *dir, float scale,
                                        int modelIndex, int rendermode,
                                        int renderfx, float a, float life,
                                        int flags);
R_TempSprite_t g_Original_R_TempSprite = nullptr;

// --- Index 55: R_ParticleExplosion ---
BYTE g_Orig_R_ParticleExplosion[10] = {0};
typedef void(__cdecl *R_ParticleExplosion_t)(float *org, int r, int g, int b,
                                             int style);
R_ParticleExplosion_t g_Original_R_ParticleExplosion = nullptr;

void __cdecl Hook_R_ParticleExplosion(float *org, int r, int g, int b,
                                      int style) {
  static int logCount = 0;
  if (logCount < 10) {
    LogDebug("[R_ParticleExplosion] RGB=%d,%d,%d style=%d\n", r, g, b, style);
    logCount++;
  }

  if (g_Original_R_ParticleExplosion)
    g_Original_R_ParticleExplosion(org, r, g, b, style);
}

// --- Index 58: R_RocketTrail ---
BYTE g_Orig_R_RocketTrail[10] = {0};
typedef void(__cdecl *R_RocketTrail_t)(float *start, float *end, int type);
R_RocketTrail_t g_Original_R_RocketTrail = nullptr;

void __cdecl Hook_R_RocketTrail(float *start, float *end, int type) {
  static int logCount = 0;
  if (logCount < 10) {
    LogDebug("[R_RocketTrail] type=%d\n", type);
    logCount++;
  }

  if (g_Original_R_RocketTrail)
    g_Original_R_RocketTrail(start, end, type);
}

void *__cdecl Hook_R_TempSprite(float *pos, float *dir, float scale,
                                int modelIndex, int rendermode, int renderfx,
                                float a, float life, int flags) {
  // Lazy-load smoke sprites if not loaded yet (e.g. if InstallHooks ran
  // before map load)
  EnsureSmokeModelsLoaded();

  // Debug Logging: Log first few calls to see what's happening
  static int debugLogCount = 0;
  if (debugLogCount < 50) {
    // Create a small cache of logged indices to avoid spamming the same one
    static int loggedIndices[50];
    bool seen = false;
    for (int k = 0; k < debugLogCount; k++)
      if (loggedIndices[k] == modelIndex)
        seen = true;
    if (!seen) {
      loggedIndices[debugLogCount++] = modelIndex;
      LogDebug("[R_TempSprite] Called with modelIndex=%d flags=%d\n",
               modelIndex, flags);
    }
  }

  // Check if this model is smoke
  if (g_NoSmoke && g_NumSmokeModels > 0) {
    for (int i = 0; i < g_NumSmokeModels; i++) {
      if (modelIndex == g_SmokeModelIndices[i]) {
        // LogDebug("[NoSmoke] Blocked R_TempSprite smoke (idx %d)\n",
        // modelIndex);
        return nullptr; // Block creation! (Return NULL = temp ent not
                        // created)
      }
    }
  }

  // Table hooked: Call original directly
  if (g_Original_R_TempSprite)
    return g_Original_R_TempSprite(pos, dir, scale, modelIndex, rendermode,
                                   renderfx, a, life, flags);
  return nullptr;
}

int __cdecl MsgFunc_SayText(const char *pszName, int iSize, void *pbuf) {
  // (SayText received, size=%d)

  if (iSize > 2 && pbuf) {
    // Simple heuristic: The chat message is usually at the end of the buffer
    // standard SayText: [byte client] [string msg] ...
    // We will scan the buffer for printable strings
    char *pData = (char *)pbuf;
    for (int i = 0; i < iSize - 1; i++) {
      if (pData[i] >= 32 && pData[i] < 127) {
        // Potential string start
        int len = strlen(&pData[i]);
        if (len > 3 && len < iSize - i + 1) {
          char temp[256];
          strncpy(temp, &pData[i], 255);
          temp[255] = 0;

          // Send to SayiBilen
          SayiBilen_OnMessage(temp);

          // Move index to skip this string
          i += len;
        }
      }
    }
  }

  // Call original handler so chat stays VISIBLE!
  if (g_Original_SayText) {
    return g_Original_SayText(pszName, iSize, pbuf);
  } else {
    // Fallback: Print to console if we couldn't find original handler
    // SayText format: [byte client] [string msg]
    if (iSize > 2 && g_pfnConPrintf) {
      char *pData = (char *)pbuf;
      // Skip client byte
      ((void (*)(const char *, ...))g_pfnConPrintf)("[ConsoleLog] Chat: '%s'\n",
                                                    pData + 1);
    }
  }
  return 0;
}

int __cdecl MsgFunc_ScreenFade(const char *pszName, int iSize, void *pbuf) {
  if (g_NoFlash) {
    LogDebug("[NoFlash] Blocked ScreenFade!\n");
    return 0;
  }
  if (g_Original_ScreenFade) {
    return g_Original_ScreenFade(pszName, iSize, pbuf);
  }
  return 0;
}

// Globals for storing original function bytes (trampoline)

// No-Smoke: Hook SPR_Set (Engine Index 4) - TABLE HOOK
void __cdecl Hook_SPR_Set(int hPic, int r, int g, int b) {
  g_BlockCurrentSprite = false;

  // Lazy-load smoke sprites on first use
  EnsureSmokeModelsLoaded();

  // Debug Log
  static int sprSetLogCount = 0;
  if (sprSetLogCount < 50) {
    LogDebug("[SPR_Set] hPic=%d\n", hPic);
    sprSetLogCount++;
  }

  // Filter
  if (g_NoSmoke && g_NumSmokeModels > 0) {
    for (int i = 0; i < g_NumSmokeModels; i++) {
      if (hPic == g_SmokeModelIndices[i]) {
        g_BlockCurrentSprite = true;
        break;
      }
    }
  }
  
  if (g_cvar_ch.value != 0.0f) {
    if (hPic > 0 && (hPic == g_CrosshairsSpriteID || hPic == g_CrosshairSpriteID || hPic == g_SniperScopeSpriteID)) {
      g_BlockCurrentSprite = true;
    }
  }

  // Call Original (Via global function pointer)
  if (g_Original_SPR_Set)
    g_Original_SPR_Set(hPic, r, g, b);
}

// No-Smoke: Hook SPR_Draw (Engine Index 5) - TABLE HOOK
void __cdecl Hook_SPR_Draw(int frame, int x, int y, const void *prc) {
  if (!g_BlockCurrentSprite) {
    if (g_Original_SPR_Draw)
      g_Original_SPR_Draw(frame, x, y, prc);
  }
}

// No-Smoke: Hook SPR_DrawHoles (Engine Index 6) - TABLE HOOK
void __cdecl Hook_SPR_DrawHoles(int frame, int x, int y, const void *prc) {
  if (!g_BlockCurrentSprite) {
    if (g_Original_SPR_DrawHoles)
      g_Original_SPR_DrawHoles(frame, x, y, prc);
  }
}

// No-Smoke: Hook SPR_DrawAdditive (Engine Index 7) - TABLE HOOK
void __cdecl Hook_SPR_DrawAdditive(int frame, int x, int y, const void *prc) {
  if (!g_BlockCurrentSprite) {
    if (g_Original_SPR_DrawAdditive)
      g_Original_SPR_DrawAdditive(frame, x, y, prc);
  }
}

// -------------------------------------------------------------
// ESP: TEAMINFO HOOK
// -------------------------------------------------------------
pfnUserMsgHook g_Original_TeamInfo = nullptr;
int __cdecl MsgFunc_TeamInfo(const char *pszName, int iSize, void *pbuf) {
  if (iSize > 1 && pbuf) {
    BYTE *data = (BYTE *)pbuf;
    int playerIdx = data[0];
    if (playerIdx >= 1 && playerIdx <= 32) {
      char *teamStr = (char *)(data + 1);
      if (strcmp(teamStr, "TERRORIST") == 0)
        g_PlayerTeam[playerIdx] = 1;
      else if (strcmp(teamStr, "CT") == 0)
        g_PlayerTeam[playerIdx] = 2;
      else if (strcmp(teamStr, "SPECTATOR") == 0 ||
               strcmp(teamStr, "UNASSIGNED") == 0)
        g_PlayerTeam[playerIdx] = 0;
      LogDebug("[ESP] TeamInfo: Player %d -> %s (team=%d)\n", playerIdx,
               teamStr, g_PlayerTeam[playerIdx]);
    }
  }
  if (g_Original_TeamInfo)
    return g_Original_TeamInfo(pszName, iSize, pbuf);
  return 0;
}

int __cdecl MsgFunc_SetFOV(const char *pszName, int iSize, void *pbuf) {
  if (iSize >= 1 && pbuf) {
    BYTE serverFOV = *(BYTE *)pbuf;
    if (serverFOV == 0) // 0 means reset to default (90)
      serverFOV = 90;
      
    // Store original requested FOV for crosshair scoping checks
    g_CurrentFOV = serverFOV;

    int isAntiDrug = 1;
    if (g_cvar_anti_drug.string) isAntiDrug = atoi(g_cvar_anti_drug.string);
    
    // If Anti-Drug is ON and the server tries to set FOV above 90 (drug effect),
    // we block the drug by forcing the engine to stay at the default 90 FOV.
    if (isAntiDrug > 0 && serverFOV > 90) {
      *(BYTE *)pbuf = 90;
    }

    LogDebug("[FOV] SetFOV intercepted: server=%d anti_drug=%d final=%d\n", serverFOV, isAntiDrug, *(BYTE *)pbuf);
  }
  if (g_Original_SetFOV)
    return g_Original_SetFOV(pszName, iSize, pbuf);
  return 0;
}

int __cdecl MsgFunc_HideWeapon(const char *pszName, int iSize, void *pbuf) {
  if (iSize == 1 && pbuf) {
    g_ServerHideWeaponFlags = *(byte *)pbuf;
    byte flags = g_ServerHideWeaponFlags;
    
    // Add our custom hides
    if (g_cvar_ch.value != 0.0f) {
      flags |= (1 << 6); // HIDEHUD_MISCSTATUS (blanks crosshair)
    }
    
    if (g_Original_HideWeapon)
      return g_Original_HideWeapon(pszName, iSize, &flags);
    return 0;
  }
  if (g_Original_HideWeapon)
    return g_Original_HideWeapon(pszName, iSize, pbuf);
  return 0;
}

// SCANNER IMPLEMENTATION - PRO SCANNER via client.dll Initialize export
void FindEngineFunctions() {
  HMODULE hClient = GetModuleHandleA("client.dll");
  if (!hClient) {
    LogDebug("[ProScanner] ERROR: client.dll not loaded!\n");
    return;
  }
  LogDebug("[ProScanner] client.dll base: 0x%X\n", (DWORD)hClient);

  // Step 1: Find the exported "Initialize" function
  FARPROC pInitialize = GetProcAddress(hClient, "Initialize");
  if (!pInitialize) {
    LogDebug(
        "[ProScanner] ERROR: Initialize export not found in client.dll!\n");
    return;
  }
  LogDebug("[ProScanner] Found Initialize at 0x%X\n", (DWORD)pInitialize);

  // Step 2: Scan Initialize prologue for the pattern that stores pEnginefuncs
  // In Build 8684, Initialize uses REP MOVSD (memcpy) to copy the table:
  //   MOV ESI, [ESP+X]     ; source = pEnginefuncs parameter
  //   MOV EDI, 0xXXXXXXXX  ; destination = global buffer (BF XX XX XX XX)
  //   REP MOVSD             ; memcpy (F3 A5)
  BYTE *pCode = (BYTE *)pInitialize;
  DWORD engTableDirect = 0; // Direct table address (memcpy destination)
  bool isMemcpy = false;

  // Dump first 48 bytes on one line for debugging
  char hexDump[256] = {0};
  int pos = 0;
  for (int i = 0; i < 48 && !IsBadReadPtr(pCode + i, 1); i++) {
    pos += sprintf(hexDump + pos, "%02X ", pCode[i]);
  }
  LogDebug("[ProScanner] Initialize bytes: %s\n", hexDump);

  // PRIORITY 1: Look for BF XX XX XX XX followed by F3 A5 (MOV EDI, imm32 +
  // REP MOVSD) This is how Build 8684 copies the engine table via memcpy!
  for (int i = 0; i < 128; i++) {
    if (IsBadReadPtr(pCode + i, 7))
      break;

    if (pCode[i] == 0xBF) { // MOV EDI, imm32
      DWORD candidate = *(DWORD *)(pCode + i + 1);
      // Check if F3 A5 (REP MOVSD) follows within next 4 bytes
      for (int j = 5; j < 8; j++) {
        if (pCode[i + j] == 0xF3 && pCode[i + j + 1] == 0xA5) {
          // FOUND IT! EDI = destination = the global engine table
          if (candidate > 0x10000 && !IsBadReadPtr((void *)candidate, 540)) {
            engTableDirect = candidate;
            isMemcpy = true;
            LogDebug("[ProScanner] Found MEMCPY pattern! MOV EDI, 0x%X + REP "
                     "MOVSD at offset +%d\n",
                     candidate, i);
            break;
          }
        }
      }
      if (engTableDirect)
        break;
    }
  }

  // PRIORITY 2: Fallback to A3/89 patterns (simple pointer store)
  DWORD engTableGlobal = 0;
  if (!engTableDirect) {
    for (int i = 0; i < 128; i++) {
      if (IsBadReadPtr(pCode + i, 6))
        break;

      if (pCode[i] == 0xA3) {
        DWORD candidate = *(DWORD *)(pCode + i + 1);
        if (candidate > 0x10000 && !IsBadReadPtr((void *)candidate, 4)) {
          engTableGlobal = candidate;
          LogDebug(
              "[ProScanner] Fallback: Found MOV [0x%X], EAX at offset +%d\n",
              candidate, i);
          break;
        }
      }
      if (pCode[i] == 0x89 && (pCode[i + 1] == 0x05 || pCode[i + 1] == 0x0D ||
                               pCode[i + 1] == 0x15)) {
        DWORD candidate = *(DWORD *)(pCode + i + 2);
        if (candidate > 0x10000 && !IsBadReadPtr((void *)candidate, 4)) {
          engTableGlobal = candidate;
          LogDebug(
              "[ProScanner] Fallback: Found MOV [0x%X], reg at offset +%d\n",
              candidate, i);
          break;
        }
      }
    }
  }

  if (!engTableDirect && !engTableGlobal) {
    LogDebug(
        "[ProScanner] FAILED to find pEnginefuncs storage in Initialize!\n");
    return;
  }

  // Step 3: Set up the engine table pointer
  void **engineTable = NULL;

  if (engTableDirect) {
    // MEMCPY case: the address IS the table directly (Build 8684)
    engineTable = (void **)engTableDirect;
    LogDebug(
        "[ProScanner] cl_enginefunc_t at 0x%X (direct memcpy destination)\n",
        engTableDirect);
  } else {
    // Pointer store case: dereference to get table
    DWORD storedValue = *(DWORD *)engTableGlobal;
    if (storedValue > 0x10000 && !IsBadReadPtr((void *)storedValue, 400)) {
      engineTable = (void **)storedValue;
      LogDebug("[ProScanner] cl_enginefunc_t* at 0x%X (ptr at 0x%X)\n",
               storedValue, engTableGlobal);
    } else {
      engineTable = (void **)engTableGlobal;
      LogDebug("[ProScanner] cl_enginefunc_t at 0x%X (direct global)\n",
               engTableGlobal);
    }
  }

  if (IsBadReadPtr(engineTable, 100 * sizeof(void *))) {
    LogDebug("[ProScanner] ERROR: engineTable at 0x%X not readable!\n",
             engineTable);
    return;
  }

  // Log first 25 entries for debugging
  LogDebug("[ProScanner] Table entries:\n");
  for (int i = 0; i < 25; i++) {
    LogDebug("  [%d] = 0x%X\n", i, (DWORD)engineTable[i]);
  }

  // Step 4: Extract functions (indices from Half-Life SDK cdll_int.h)
  g_EngineTable = engineTable;

  g_pfnHookUserMsg = (pfnHookUserMsg_t)engineTable[18];
  LogDebug("[ProScanner] pfnHookUserMsg [18] = 0x%X\n",
           (DWORD)g_pfnHookUserMsg);

  g_pfnServerCmd = (ServerCmd_t)engineTable[19];
  engineTable[19] = (void *)Hook_ServerCmd;
  LogDebug("[ProScanner] pfnServerCmd [19] hooked (0x%X -> 0x%X)\n", (DWORD)g_pfnServerCmd, (DWORD)Hook_ServerCmd);

  g_pfnClientCmd = (ClientCmd_t)engineTable[20];
  
  // Find Cbuf_AddText by tracing pfnClientCmd (index 20)
  if (g_pfnClientCmd) {
    BYTE* code = (BYTE*)g_pfnClientCmd;
    for (int i=0; i<32; i++) {
        if (code[i] == 0xE8) { // CALL rel32
            DWORD offset = *(DWORD*)(code + i + 1);
            g_pfnCbuf_AddText = (Cbuf_AddText_t)((DWORD)g_pfnClientCmd + i + 5 + offset);
            LogDebug("[ProScanner] Found Cbuf_AddText at 0x%X via CALL\n", (DWORD)g_pfnCbuf_AddText);
            break;
        }
        if (code[i] == 0xE9) { // JMP rel32
            DWORD offset = *(DWORD*)(code + i + 1);
            g_pfnCbuf_AddText = (Cbuf_AddText_t)((DWORD)g_pfnClientCmd + i + 5 + offset);
            LogDebug("[ProScanner] Found Cbuf_AddText at 0x%X via JMP\n", (DWORD)g_pfnCbuf_AddText);
            break;
        }
    }
  }

  engineTable[20] = (void *)Hook_ClientCmd; // Hook it
  LogDebug("[ProScanner] pfnClientCmd [20] hooked (0x%X -> 0x%X)\n", (DWORD)g_pfnClientCmd, (DWORD)Hook_ClientCmd);

  // Apply Cbuf_AddText gateway hook (STUBBORN protection)
  if (g_pfnCbuf_AddText) {
      DWORD old;
      VirtualProtect((void *)g_pfnCbuf_AddText, 10, PAGE_EXECUTE_READWRITE, &old);
      memcpy(g_Orig_Cbuf, (void *)g_pfnCbuf_AddText, 6);
      
      BYTE patch[5] = {0xE9, 0, 0, 0, 0};
      *(DWORD *)(patch + 1) = (DWORD)Hook_Cbuf_AddText - (DWORD)g_pfnCbuf_AddText - 5;
      memcpy((void *)g_pfnCbuf_AddText, patch, 5);
      *(BYTE *)((DWORD)g_pfnCbuf_AddText + 5) = 0x90;
      LogDebug("[ProScanner] Cbuf_AddText HOOKED!\n");
  }

  g_RegisterCvar = (Cvar_RegisterVariable_t)engineTable[14];
  LogDebug("[ProScanner] pfnRegisterVariable [14] = 0x%X\n",
           (DWORD)g_RegisterCvar);

  g_pfnConPrintf = (ConPrintf_t)engineTable[40];
  LogDebug("[ProScanner] Con_Printf [40] = 0x%X\n", (DWORD)g_pfnConPrintf);

  if (!g_pfnHookUserMsg || IsBadReadPtr((void *)g_pfnHookUserMsg, 4)) {
    LogDebug("[ProScanner] ERROR: pfnHookUserMsg is invalid!\n");
    return;
  }

  // Step 5: Find original handlers by scanning CLIENT.DLL code
  // client.dll's init calls: gEngfuncs.pfnHookUserMsg("SayText",
  // __MsgFunc_SayText) In cdecl, 2nd arg (handler) is PUSHed BEFORE 1st arg
  // ("SayText"):
  //   PUSH __MsgFunc_SayText   ; 68 XX XX XX XX
  //   PUSH "SayText"           ; 68 YY YY YY YY  <-- FindStringRef finds this
  //   CALL [gEngfuncs+72]
  // So we find PUSH "SayText", then look backwards for PUSH handler

  LogDebug(
      "[ProScanner] Scanning client.dll for original SayText handler...\n");
  DWORD clientBase = (DWORD)hClient;
  DWORD stRef = FindStringRef(clientBase, 0x800000, "SayText");
  if (stRef) {
    LogDebug("[ProScanner] Found PUSH 'SayText' in client.dll at 0x%X\n",
             stRef);
    // Look backwards for PUSH imm32 (68 XX XX XX XX) - the handler function
    for (int k = 5; k < 40; k++) {
      if (*(BYTE *)(stRef - k) == 0x68) {
        DWORD funcPtr = *(DWORD *)(stRef - k + 1);
        // Must be a valid function pointer within client.dll range
        if (funcPtr > clientBase && funcPtr < clientBase + 0x800000) {
          if (!IsBadReadPtr((void *)funcPtr, 4)) {
            g_Original_SayText = (pfnUserMsgHook)funcPtr;
            LogDebug("[ProScanner] Found original SayText handler at 0x%X "
                     "(PUSH at 0x%X)\n",
                     funcPtr, stRef - k);
            break;
          }
        }
      }
    }
  } else {
    LogDebug(
        "[ProScanner] Could not find 'SayText' reference in client.dll!\n");
  }

  // Also find ScreenFade handler the same way
  DWORD sfRef = FindStringRef(clientBase, 0x800000, "ScreenFade");
  if (sfRef) {
    for (int k = 5; k < 40; k++) {
      if (*(BYTE *)(sfRef - k) == 0x68) {
        DWORD funcPtr = *(DWORD *)(sfRef - k + 1);
        if (funcPtr > clientBase && funcPtr < clientBase + 0x800000 &&
            !IsBadReadPtr((void *)funcPtr, 4)) {
          g_Original_ScreenFade = (pfnUserMsgHook)funcPtr;
          LogDebug("[ProScanner] Found original ScreenFade handler at 0x%X\n",
                   funcPtr);
          break;
        }
      }
    }
  }

  if (!g_Original_SayText) {
    LogDebug("[ProScanner] WARNING: Could not find original SayText handler! "
             "Chat may be invisible.\n");
  }

  // Step 6: NOW hook via official API
  LogDebug("[ProScanner] Hooking SayText via official pfnHookUserMsg...\n");
  g_pfnHookUserMsg((char *)"SayText", MsgFunc_SayText);
  LogDebug("[ProScanner] SayText hooked! Original handler: 0x%X\n",
           (DWORD)g_Original_SayText);

  // Hook ScreenFade for NoFlash
  g_pfnHookUserMsg((char *)"ScreenFade", MsgFunc_ScreenFade);
  LogDebug("[ProScanner] ScreenFade hooked! Original handler: 0x%X\n",
           (DWORD)g_Original_ScreenFade);

  // Hook SetFOV
  DWORD fovRef = FindStringRef(clientBase, 0x800000, "SetFOV");
  if (fovRef) {
    for (int k = 5; k < 40; k++) {
      if (*(BYTE *)(fovRef - k) == 0x68) {
        DWORD funcPtr = *(DWORD *)(fovRef - k + 1);
        if (funcPtr > clientBase && funcPtr < clientBase + 0x800000 &&
            !IsBadReadPtr((void *)funcPtr, 4)) {
          g_Original_SetFOV = (pfnUserMsgHook)funcPtr;
          LogDebug("[FOV] Found original SetFOV handler at 0x%X\n", funcPtr);
          break;
        }
      }
    }
  }
  g_pfnHookUserMsg((char *)"SetFOV", MsgFunc_SetFOV);
  LogDebug("[FOV] SetFOV hooked!\n");

  // Hook HideWeapon
  DWORD hideRef = FindStringRef(clientBase, 0x800000, "HideWeapon");
  if (hideRef) {
    for (int k = 5; k < 40; k++) {
      if (*(BYTE *)(hideRef - k) == 0x68) {
        DWORD funcPtr = *(DWORD *)(hideRef - k + 1);
        if (funcPtr > clientBase && funcPtr < clientBase + 0x800000 &&
            !IsBadReadPtr((void *)funcPtr, 4)) {
          g_Original_HideWeapon = (pfnUserMsgHook)funcPtr;
          LogDebug("[HideWeapon] Found original handler at 0x%X\n", funcPtr);
          break;
        }
      }
    }
  }
  g_pfnHookUserMsg((char *)"HideWeapon", MsgFunc_HideWeapon);
  LogDebug("[HideWeapon] HideWeapon hooked!\n");

  // Hook TeamInfo for ESP team tracking
  DWORD tiRef = FindStringRef(clientBase, 0x800000, "TeamInfo");
  if (tiRef) {
    for (int k = 5; k < 40; k++) {
      if (*(BYTE *)(tiRef - k) == 0x68) {
        DWORD funcPtr = *(DWORD *)(tiRef - k + 1);
        if (funcPtr > clientBase && funcPtr < clientBase + 0x800000 &&
            !IsBadReadPtr((void *)funcPtr, 4)) {
          g_Original_TeamInfo = (pfnUserMsgHook)funcPtr;
          LogDebug("[ESP] Found original TeamInfo handler at 0x%X\n", funcPtr);
          break;
        }
      }
    }
  }
  g_pfnHookUserMsg((char *)"TeamInfo", MsgFunc_TeamInfo);
  LogDebug("[ESP] TeamInfo hooked for team tracking!\n");

  // Hook CurWeapon for weapon ID tracking (hide_knife)
  DWORD cwRef = FindStringRef(clientBase, 0x800000, "CurWeapon");
  if (cwRef) {
    for (int k = 5; k < 40; k++) {
      if (*(BYTE *)(cwRef - k) == 0x68) {
        DWORD funcPtr = *(DWORD *)(cwRef - k + 1);
        if (funcPtr > clientBase && funcPtr < clientBase + 0x800000 &&
            !IsBadReadPtr((void *)funcPtr, 4)) {
          g_Original_CurWeapon = (pfnUserMsgHook)funcPtr;
          LogDebug("[HideKnife] Found original CurWeapon handler at 0x%X\n",
                   funcPtr);
          break;
        }
      }
    }
  }
  g_pfnHookUserMsg((char *)"CurWeapon", MsgFunc_CurWeapon);
  LogDebug("[HideKnife] CurWeapon hooked for weapon ID tracking!\n");

  // Extract ESP functions from engine table (Half-Life SDK indices)
  g_pfnFillRGBA = (FillRGBA_t)engineTable[11];
  g_pfnGetScreenInfo = (GetScreenInfo_t)engineTable[12];
  g_pfnGetPlayerInfo = (GetPlayerInfo_t)engineTable[21];
  g_pfnDrawConsoleString = (DrawConsoleString_t)engineTable[27];
  g_pfnDrawSetTextColor = (DrawSetTextColor_t)engineTable[28];
  g_pfnGetLocalPlayer = (GetLocalPlayer_t)engineTable[51];
  g_pfnGetEntityByIndex = (GetEntityByIndex_t)engineTable[53];
  LogDebug("[ESP] FillRGBA[11]=0x%X GetEntityByIndex[53]=0x%X\n",
           (DWORD)g_pfnFillRGBA, (DWORD)g_pfnGetEntityByIndex);
  LogDebug("[ESP] GetLocalPlayer[51]=0x%X GetPlayerInfo[21]=0x%X\n",
           (DWORD)g_pfnGetLocalPlayer, (DWORD)g_pfnGetPlayerInfo);

  // Extract pTriAPI (index 82) and get WorldToScreen from it
  void *pTriAPI = engineTable[82];
  if (pTriAPI && !IsBadReadPtr(pTriAPI, 64)) {
    // triangleapi_s layout: [int version, then function pointers...]
    // WorldToScreen is at index 12 in the struct (offset 48 bytes)
    void **triVtable = (void **)pTriAPI;
    g_pfnTriWorldToScreen = (TriAPI_WorldToScreen_t)triVtable[12];
    LogDebug("[ESP] pTriAPI=0x%X WorldToScreen=0x%X\n", (DWORD)pTriAPI,
             (DWORD)g_pfnTriWorldToScreen);
  } else {
    LogDebug("[ESP] WARNING: pTriAPI at index 82 is invalid (0x%X)!\n",
             (DWORD)pTriAPI);
  }

  // --- NO SMOKE: INLINE PATCH R_Sprite_Smoke in hw.dll ---
  // The engine calls R_Sprite_Smoke DIRECTLY (not through a vtable).
  // So we must patch the ACTUAL FUNCTION CODE in hw.dll, not a table entry.
  // We get the real function address from pEfxAPI (engine effects API).
  g_pEfxAPI = engineTable[83]; // Store globally for hooking R_TempSprite
  void *pEfxAPI = g_pEfxAPI;
  if (pEfxAPI && !IsBadReadPtr(pEfxAPI, 200)) {
    void **efxTable = (void **)pEfxAPI;

    // R_Sprite_Smoke is at EfxAPI index 38 (per GoldSrc SDK)
    DWORD smokeFunc = (DWORD)efxTable[38];
    LogDebug("[NoSmoke] pEfxAPI=0x%X R_Sprite_Smoke=0x%X\n", (DWORD)pEfxAPI,
             smokeFunc);

    // Also try R_TempSprite at index 50 as backup info
    LogDebug("[NoSmoke] R_TempSprite[50]=0x%X\n", (DWORD)efxTable[50]);

    if (smokeFunc && !IsBadReadPtr((void *)smokeFunc, 1)) {
      g_SmokeFuncAddr = smokeFunc;
      g_SmokeFuncOrigByte = *(BYTE *)smokeFunc;

      // Patch: write RET (0xC3) at the entry point
      DWORD old;
      VirtualProtect((void *)smokeFunc, 1, PAGE_EXECUTE_READWRITE, &old);
      *(BYTE *)smokeFunc = 0xC3; // RET = function does nothing
      VirtualProtect((void *)smokeFunc, 1, old, &old);

      LogDebug("[NoSmoke] INLINE PATCHED R_Sprite_Smoke @ 0x%X! "
               "(orig byte=0x%02X -> 0xC3)\n",
               smokeFunc, g_SmokeFuncOrigByte);
    } else {
      LogDebug("[NoSmoke] WARNING: R_Sprite_Smoke address invalid!\n");
    }
  } else {
    LogDebug("[NoSmoke] WARNING: pEfxAPI[83]=0x%X invalid!\n", (DWORD)pEfxAPI);
  }

  // Extract SPR_Load for potential future use
  g_pfnSPR_Load = (SPR_Load_t)engineTable[0];
  LogDebug("[NoSmoke] SPR_Load[0]=0x%X\n", (DWORD)g_pfnSPR_Load);

  // Extract pfnAddCommand (index 17) for +strafe_boost
  g_pfnAddCommand = (AddCommand_t)engineTable[17];
  LogDebug("[StrafeBoost] pfnAddCommand[17]=0x%X\n", (DWORD)g_pfnAddCommand);

  LogDebug("[ProScanner] === ALL DONE! Engine table found! ===\n");
}

int __fastcall Hook_LegacyAuth(void *thisptr, int edx, void *pBlob, int cbMax,
                               uint64_t steamIDGS, uint32_t ip, uint16_t port,
                               bool secure) {
  int val = atoi(g_cvar_change_id.string);
  if (val == 1) {
    memset(pBlob, 0, cbMax);
    return 0; // Blocked
  }
  if (g_Original_Initiate) {
    return g_Original_Initiate(thisptr, edx, pBlob, cbMax, steamIDGS, ip, port,
                               secure);
  }
  return 0;
}

// -------------------------------------------------------------
// VTABLE HOOK APPLICATION
// -------------------------------------------------------------
void ApplyVTableHooks(void *iface, const char *pName) {
  if (!iface || (DWORD)iface < 0x10000)
    return;

  if (strstr(pName, "SteamUser")) {
    DWORD *obj = (DWORD *)iface;
    DWORD vptr = obj[0];
    if (!IsBadReadPtr((void *)vptr, 4)) {
      DWORD *vtable = (DWORD *)vptr;
      DWORD oldV;
      if (VirtualProtect(vtable, 256, PAGE_EXECUTE_READWRITE, &oldV)) {
        if (vtable[3] != (DWORD)Hook_LegacyAuth) {
          g_Original_Initiate = (InitiateGameConnection_t)vtable[3];
          vtable[3] = (DWORD)Hook_LegacyAuth;
        }
        VirtualProtect(vtable, 256, oldV, &oldV);
      }
    }
  }
}

// -------------------------------------------------------------
// CREATEINTERFACE HOOKS
// -------------------------------------------------------------
typedef void *(*CreateInterface_t)(const char *pName, int *pReturnCode);
void *__cdecl Hook_CreateInterface(const char *pName,
                                   int *pReturnCode); // Forward decl

void *__cdecl Hook_CreateInterface(const char *pName, int *pReturnCode) {
  DWORD old;
  VirtualProtect((void *)g_CreateInterface, 5, PAGE_EXECUTE_READWRITE, &old);
  memcpy((void *)g_CreateInterface, g_Orig_CI, 5);
  VirtualProtect((void *)g_CreateInterface, 5, old, &old);

  CreateInterface_t fn = (CreateInterface_t)g_CreateInterface;
  void *iface = fn(pName, pReturnCode);

  VirtualProtect((void *)g_CreateInterface, 5, PAGE_EXECUTE_READWRITE, &old);
  BYTE patch[5] = {0xE9, 0, 0, 0, 0};
  *(DWORD *)(patch + 1) = (DWORD)Hook_CreateInterface - g_CreateInterface - 5;
  memcpy((void *)g_CreateInterface, patch, 5);
  VirtualProtect((void *)g_CreateInterface, 5, old, &old);

  if (iface && pName)
    ApplyVTableHooks(iface, pName);
  return iface;
}

void *__cdecl Hook_SteamInternal(const char *pName, int *pReturnCode) {
  DWORD old;
  VirtualProtect((void *)g_SteamInternal, 5, PAGE_EXECUTE_READWRITE, &old);
  memcpy((void *)g_SteamInternal, g_Orig_SI, 5);
  VirtualProtect((void *)g_SteamInternal, 5, old, &old);

  CreateInterface_t fn = (CreateInterface_t)g_SteamInternal;
  void *iface = fn(pName, pReturnCode);

  VirtualProtect((void *)g_SteamInternal, 5, PAGE_EXECUTE_READWRITE, &old);
  BYTE patch[5] = {0xE9, 0, 0, 0, 0};
  *(DWORD *)(patch + 1) = (DWORD)Hook_SteamInternal - g_SteamInternal - 5;
  memcpy((void *)g_SteamInternal, patch, 5);
  VirtualProtect((void *)g_SteamInternal, 5, old, &old);

  if (iface && pName)
    ApplyVTableHooks(iface, pName);
  return iface;
}

// -------------------------------------------------------------
// HWID HOOK (Dummy/Minimal)
// -------------------------------------------------------------
BOOL WINAPI Hook_GetVolInfo(LPCSTR lpRoot, LPSTR lpVolName, DWORD nVolNameSz,
                            LPDWORD lpVolSerial, LPDWORD lpMaxComp,
                            LPDWORD lpFlags, LPSTR lpFSName, DWORD nFSNameSz) {
  if (lpVolName && nVolNameSz > 0)
    strcpy(lpVolName, "C_DRIVE");
  if (lpVolSerial)
    *lpVolSerial = 123456;
  if (lpMaxComp)
    *lpMaxComp = 255;
  if (lpFlags)
    *lpFlags = 0;
  if (lpFSName && nFSNameSz > 0)
    strcpy(lpFSName, "NTFS");
  return TRUE;
}

// -------------------------------------------------------------
// DRAW ENGINE HOOK (Speedometer)
// -------------------------------------------------------------
typedef int(__cdecl *DrawEngine_t)();
DrawEngine_t g_Original_DrawEngine = nullptr;
DWORD g_DrawEngineAddr = 0;
BYTE g_Orig_DrawEngine_CallOffset[4] = {0}; // Original E8 offset bytes

// HUD_Redraw hook (for ESP drawing - fires AFTER 3D world renders)
typedef int(__cdecl *HUD_Redraw_t)(float time, int intermission);
HUD_Redraw_t g_pfnHUD_Redraw = nullptr;
DWORD g_HUD_Redraw_Addr = 0;
BYTE g_Orig_HUD_Redraw[6] = {0};

// -------------------------------------------------------------
// GLOW ESP (Chams) - OpenGL + StudioDrawPlayer hook
// -------------------------------------------------------------
// OpenGL constants
#define MY_GL_DEPTH_TEST 0x0B71
#define MY_GL_BLEND 0x0BE2
#define MY_GL_SRC_ALPHA 0x0302
#define MY_GL_ONE_MINUS_SRC_ALPHA 0x0303
#define MY_GL_ONE 0x1
#define MY_GL_MODELVIEW 0x1700
#define MY_GL_TEXTURE_2D 0x0DE1
#define MY_GL_LIGHTING 0x0B50
#define MY_GL_FLAT 0x1D00
#define MY_GL_SMOOTH 0x1D01
#define MY_GL_FRONT_AND_BACK 0x0408
#define MY_GL_LINE 0x1B01
#define MY_GL_FILL 0x1B02
#define MY_GL_CULL_FACE 0x0B44
#define MY_GL_FRONT 0x0404
#define MY_GL_BACK 0x0405
#define MY_GL_PROJECTION 0x1701
#define MY_GL_TEXTURE_2D 0x0DE1
#define MY_GL_BLEND 0x0BE2

// OpenGL function typedefs
typedef void(__stdcall *glEnable_t)(unsigned int cap);
typedef void(__stdcall *glDisable_t)(unsigned int cap);
typedef void(__stdcall *glDepthMask_t)(unsigned char flag);
typedef void(__stdcall *glColor4f_t)(float r, float g, float b, float a);
typedef void(__stdcall *glBlendFunc_t)(unsigned int sfactor,
                                       unsigned int dfactor);
typedef void(__stdcall *glShadeModel_t)(unsigned int mode);
typedef void(__stdcall *glDepthFunc_t)(unsigned int func);
typedef void(__stdcall *glPolygonMode_t)(unsigned int face, unsigned int mode);
typedef void(__stdcall *glLineWidth_t)(float width);
typedef void(__stdcall *glCullFace_t)(unsigned int mode);
typedef void(__stdcall *glMatrixMode_t)(unsigned int mode);
typedef void(__stdcall *glPushMatrix_t)(void);
typedef void(__stdcall *glPopMatrix_t)(void);
typedef void(__stdcall *glScalef_t)(float x, float y, float z);

// OpenGL function pointers
glEnable_t g_glEnable = nullptr;
glDisable_t g_glDisable = nullptr;
glDepthMask_t g_glDepthMask = nullptr;
glColor4f_t g_glColor4f = nullptr;
glBlendFunc_t g_glBlendFunc = nullptr;
glPolygonMode_t g_glPolygonMode = nullptr;
glLineWidth_t g_glLineWidth = nullptr;
glCullFace_t g_glCullFace = nullptr;
glMatrixMode_t g_glMatrixMode = nullptr;
glPushMatrix_t g_glPushMatrix = nullptr;
glPopMatrix_t g_glPopMatrix = nullptr;
glScalef_t g_glScalef = nullptr;

// glPushAttrib/glPopAttrib for complete GL state save/restore
typedef void(__stdcall *glPushAttrib_t)(unsigned int mask);
typedef void(__stdcall *glPopAttrib_t)(void);
glPushAttrib_t g_glPushAttrib = nullptr;
glPopAttrib_t g_glPopAttrib = nullptr;
#define MY_GL_ALL_ATTRIB_BITS 0x000FFFFF

// Studio renderer hook
// r_studio_interface_s: { int version; StudioDrawModel; StudioDrawPlayer; }
typedef int(__cdecl *StudioDrawPlayer_t)(int flags, void *pplayer);
StudioDrawPlayer_t g_Original_StudioDrawPlayer = nullptr;
void **g_pStudioInterface = nullptr; // Pointer to the interface vtable
bool g_GlowESP_Ready = false;

// Current player being drawn (set by our hook, used for team coloring)
int g_CurrentDrawingPlayerIndex = -1;

// Glow ESP hook: intercepts player model rendering
int __cdecl Hook_StudioDrawPlayer(int flags, void *pplayer) {
  if (!g_Original_StudioDrawPlayer)
    return 0;

  if (ShouldHideVisuals()) {
    return g_Original_StudioDrawPlayer(flags, pplayer);
  }

  // [Failsafe] If stealth mode is active (hooks removed), stop rendering glow
  // immediately
  if (!g_HooksActive)
    return g_Original_StudioDrawPlayer(flags, pplayer);

  // Get player index from entity_state_s.number (offset 4 in entity_state_t)
  int playerIndex = -1;
  if (pplayer) {
    playerIndex = *(int *)((char *)pplayer + 4); // entity_state_t.number
  }

  // Check if glow ESP is active for this player's team
  bool drawCT = (g_cvar_ct_esp.value != 0.0f);
  bool drawT = (g_cvar_t_esp.value != 0.0f);

  int team = 0;
  if (playerIndex >= 1 && playerIndex <= 32)
    team = g_PlayerTeam[playerIndex];

  // Fallback: Model-based detection if TeamInfo was missed
  if (team == 0 && playerIndex != -1 && g_pfnGetPlayerInfo) {
      hud_player_info_t info;
      memset(&info, 0, sizeof(info));
      g_pfnGetPlayerInfo(playerIndex, &info);
      if (info.model) {
          team = GetTeamFromModel(info.model);
          if (team != 0) g_PlayerTeam[playerIndex] = team; // Cache it
      }
  }

  // 1. SKIP DEAD Players (Fixes dead body ESP)
  // sequence > 100 typically means dead/spectating in GoldSrc
  int seq = *(int *)((char *)pplayer + 44);
  if (seq > 100)
    return g_Original_StudioDrawPlayer(flags, pplayer);

  // 2. SKIP SELF Glow (Fixes projection issues at zero distance)
  int localIdx = -1;
  if (g_pfnGetLocalPlayer) {
    void *local = g_pfnGetLocalPlayer();
    if (local) localIdx = *(int*)local; // Corrected offset from +4 to +0
  }
  if (playerIndex != -1 && playerIndex == localIdx)
    return g_Original_StudioDrawPlayer(flags, pplayer);

  // 2. DISTANCE GUARD for Glow (Skip if closer than 100 units)
  float lX=0, lY=0, lZ=0;
  if (g_pfnGetLocalPlayer) {
    void *local = g_pfnGetLocalPlayer();
    if (local) {
        ReadOrigin(local, ESP_ORIGIN_OFFSET, lX, lY, lZ);
    }
  }
  if (pplayer) {
      float *eOrig = (float *)((char *)pplayer + 16); // entity_state_t.origin is at offset 16
      float dx = eOrig[0] - lX, dy = eOrig[1] - lY, dz = eOrig[2] - lZ;
      float d = sqrtf(dx*dx + dy*dy + dz*dz);
      if (d > 0.1f && d < 100.0f) {
          return g_Original_StudioDrawPlayer(flags, pplayer);
      }
  }

  bool shouldGlow = false;
  if (team == 1 && drawT)
    shouldGlow = true;
  if (team == 2 && drawCT)
    shouldGlow = true;

  // Check esp_type (Glow enabled if bit 0 set: type 1 or 3)
  int espType = (int)g_cvar_esp_type.value;
  if (espType == 0)
    espType = 3;
  if (!(espType & 1))
    shouldGlow = false;

  if (!shouldGlow || !g_glEnable || !g_glDisable || !g_glColor4f ||
      !g_glDepthMask || !g_glBlendFunc) {
    // No glow — just draw normally
    return g_Original_StudioDrawPlayer(flags, pplayer);
  }

  // ===== GLOW PASS: Solid color silhouette through walls =====
  // Minimal state changes to avoid "blinking"
  g_glDisable(MY_GL_DEPTH_TEST);
  g_glDepthMask(0);
  g_glEnable(MY_GL_BLEND);
  g_glBlendFunc(MY_GL_SRC_ALPHA, MY_GL_ONE);
  g_glDisable(MY_GL_TEXTURE_2D);

  if (team == 1)
    g_glColor4f(1.0f, 0.1f, 0.1f, 0.6f);
  else
    g_glColor4f(0.1f, 0.3f, 1.0f, 0.6f);

  g_Original_StudioDrawPlayer(flags, pplayer);

  // ===== RESTORE MINIMAL STATE =====
  g_glEnable(MY_GL_DEPTH_TEST);
  g_glDepthMask(1);
  g_glDisable(MY_GL_BLEND);
  g_glEnable(MY_GL_TEXTURE_2D);
  g_glColor4f(1.0f, 1.0f, 1.0f, 1.0f);

  // Draw the player normally (with original GL state restored)
  int ret = g_Original_StudioDrawPlayer(flags, pplayer);

  return ret;
}

// Initialize OpenGL function pointers
void InitGlowESP() {
  static bool s_Scanned = false;

  // --- PHASE 1: ONE-TIME SCANNING ---
  if (!s_Scanned) {
    HMODULE hGL = GetModuleHandleA("opengl32.dll");
    if (!hGL) {
      LogDebug("[Glow] opengl32.dll not found!\n");
      return;
    }

    g_glEnable = (glEnable_t)GetProcAddress(hGL, "glEnable");
    g_glDisable = (glDisable_t)GetProcAddress(hGL, "glDisable");
    g_glDepthMask = (glDepthMask_t)GetProcAddress(hGL, "glDepthMask");
    g_glColor4f = (glColor4f_t)GetProcAddress(hGL, "glColor4f");
    g_glBlendFunc = (glBlendFunc_t)GetProcAddress(hGL, "glBlendFunc");
    g_glPolygonMode = (glPolygonMode_t)GetProcAddress(hGL, "glPolygonMode");
    g_glLineWidth = (glLineWidth_t)GetProcAddress(hGL, "glLineWidth");
    g_glCullFace = (glCullFace_t)GetProcAddress(hGL, "glCullFace");
    g_glMatrixMode = (glMatrixMode_t)GetProcAddress(hGL, "glMatrixMode");
    g_glPushMatrix = (glPushMatrix_t)GetProcAddress(hGL, "glPushMatrix");
    g_glPopMatrix = (glPopMatrix_t)GetProcAddress(hGL, "glPopMatrix");
    g_glScalef = (glScalef_t)GetProcAddress(hGL, "glScalef");

    LogDebug("[Glow] OpenGL: glEnable=0x%X glScalef=0x%X\n", (DWORD)g_glEnable,
             (DWORD)g_glScalef);

    // Find client.dll's studio interface
    HMODULE hClient = GetModuleHandleA("client.dll");
    if (!hClient) {
      LogDebug("[Glow] client.dll not found!\n");
      return;
    }

    // HUD_GetStudioModelInterface export
    typedef int(__cdecl * GetStudioAPI_t)(int version, void **ppInterface,
                                          void *pStudio);
    GetStudioAPI_t pfnGetStudio = nullptr;

    // Try different export names
    pfnGetStudio =
        (GetStudioAPI_t)GetProcAddress(hClient, "HUD_GetStudioModelInterface");
    if (!pfnGetStudio)
      pfnGetStudio = (GetStudioAPI_t)GetProcAddress(
          hClient, "_HUD_GetStudioModelInterface@12");

    if (!pfnGetStudio) {
      LogDebug("[Glow] HUD_GetStudioModelInterface not found!\n");
      return;
    }

    LogDebug("[Glow] HUD_GetStudioModelInterface at 0x%X\n",
             (DWORD)pfnGetStudio);

    BYTE *code = (BYTE *)pfnGetStudio;
    if (IsBadReadPtr(code, 80)) {
      LogDebug("[Glow] Cannot read GetStudioModelInterface code!\n");
      return;
    }

    // Hex dump first 80 bytes of the function for diagnostics
    LogDebug("[Glow] Function bytes:\n");
    for (int d = 0; d < 80; d += 16) {
      char hex[128];
      int pos = 0;
      for (int j = 0; j < 16 && (d + j) < 80; j++) {
        pos += sprintf(hex + pos, "%02X ", code[d + j]);
      }
      LogDebug("[Glow]  +%02X: %s\n", d, hex);
    }

    // Helper lambda to validate a candidate vtable address
    // r_studio_interface_s: { int version=1; fn StudioDrawModel; fn
    // StudioDrawPlayer; }
    void **pInterface = nullptr;

    auto tryCandidate = [&](DWORD candidate, int off,
                            const char *pattern) -> bool {
      if (candidate < 0x10000 || IsBadReadPtr((void *)candidate, 12))
        return false;
      int *pCandidate = (int *)candidate;
      if (pCandidate[0] == 1) { // version == 1
        DWORD fn1 = (DWORD)pCandidate[1];
        DWORD fn2 = (DWORD)pCandidate[2];
        if (fn1 > 0x10000 && fn2 > 0x10000 && !IsBadReadPtr((void *)fn1, 1) &&
            !IsBadReadPtr((void *)fn2, 1)) {
          pInterface = (void **)candidate;
          LogDebug("[Glow] Found r_studio_interface via %s at +%d: 0x%X "
                   "(v=%d Draw=0x%X Player=0x%X)\n",
                   pattern, off, candidate, pCandidate[0], fn1, fn2);
          return true;
        }
      }
      return false;
    };

    for (int off = 0; off < 80 && !pInterface; off++) {
      if (IsBadReadPtr(code + off, 7))
        break;

      BYTE b0 = code[off];

      // Pattern 1: MOV reg, imm32 (B8-BF)
      if (b0 >= 0xB8 && b0 <= 0xBF) {
        DWORD candidate = *(DWORD *)(code + off + 1);
        if (tryCandidate(candidate, off, "MOV reg,imm"))
          break;
      }

      // Pattern 2: C7 00-07 XX XX XX XX  = MOV [reg], imm32
      if (b0 == 0xC7 && code[off + 1] <= 0x07) {
        DWORD candidate = *(DWORD *)(code + off + 2);
        if (tryCandidate(candidate, off, "MOV [reg],imm"))
          break;
      }

      // Pattern 3: C7 05 addr imm32 = MOV [mem32], imm32 (10 bytes)
      if (b0 == 0xC7 && code[off + 1] == 0x05 &&
          !IsBadReadPtr(code + off, 10)) {
        DWORD candidate = *(DWORD *)(code + off + 6);
        if (tryCandidate(candidate, off, "MOV [mem],imm"))
          break;
      }

      // Pattern 4: C7 45 XX YY YY YY YY = MOV [EBP+disp8], imm32
      if (b0 == 0xC7 && code[off + 1] == 0x45) {
        DWORD candidate = *(DWORD *)(code + off + 3);
        if (tryCandidate(candidate, off, "MOV [ebp+d8],imm"))
          break;
      }

      // Pattern 5: C7 85 XX XX XX XX YY YY YY YY = MOV [EBP+disp32], imm32
      if (b0 == 0xC7 && code[off + 1] == 0x85 &&
          !IsBadReadPtr(code + off, 10)) {
        DWORD candidate = *(DWORD *)(code + off + 6);
        if (tryCandidate(candidate, off, "MOV [ebp+d32],imm"))
          break;
      }

      // Pattern 6: 8D XX (LEA reg, [addr]) - 8D 05 = LEA EAX, [mem32]
      if (b0 == 0x8D && (code[off + 1] & 0xC7) == 0x05) {
        DWORD candidate = *(DWORD *)(code + off + 2);
        if (tryCandidate(candidate, off, "LEA reg,[mem]"))
          break;
      }

      // Pattern 7: 68 XX XX XX XX = PUSH imm32
      if (b0 == 0x68) {
        DWORD candidate = *(DWORD *)(code + off + 1);
        if (tryCandidate(candidate, off, "PUSH imm"))
          break;
      }

      // Pattern 8: A1/A3 addr = MOV EAX,[mem] / MOV [mem],EAX
      if (b0 == 0xA1 || b0 == 0xA3) {
        // The address itself points to a pointer that might contain the vtable
        DWORD memAddr = *(DWORD *)(code + off + 1);
        if (memAddr > 0x10000 && !IsBadReadPtr((void *)memAddr, 4)) {
          DWORD candidate = *(DWORD *)memAddr;
          if (tryCandidate(candidate, off, "MOV EAX<->mem (deref)"))
            break;
        }
      }
    }

    if (!pInterface) {
      LogDebug("[Glow] Could not find r_studio_interface vtable!\n");
      LogDebug("[Glow] Will try brute-force scan of any 4-byte value...\n");

      // Brute force: try every 4-byte aligned value in the function as a
      // potential address
      for (int off = 0; off < 76 && !pInterface; off++) {
        DWORD candidate = *(DWORD *)(code + off);
        if (tryCandidate(candidate, off, "BRUTE"))
          break;
      }
    }

    if (!pInterface) {
      LogDebug("[Glow] FAILED: No r_studio_interface vtable found!\n");
      return;
    }

    // Only set these ONCE
    g_pStudioInterface = pInterface;
    g_Original_StudioDrawPlayer = (StudioDrawPlayer_t)pInterface[2];
    s_Scanned = true; // Mark as scanned

    LogDebug("[Glow] Original StudioDrawPlayer = 0x%X\n",
             (DWORD)g_Original_StudioDrawPlayer);
  }

  // --- PHASE 2: INSTALLATION ---
  // Always run this part if scan was successful
  if (s_Scanned && g_pStudioInterface && g_Original_StudioDrawPlayer) {
    DWORD old;
    VirtualProtect(&g_pStudioInterface[2], 4, PAGE_READWRITE, &old);
    g_pStudioInterface[2] = (void *)Hook_StudioDrawPlayer;
    VirtualProtect(&g_pStudioInterface[2], 4, old, &old);

    g_GlowESP_Ready = true;
    LogDebug("[Glow] === StudioDrawPlayer hooked! Glow ESP active! ===\n");
  }
}


// ESP: Draw outlined box
void DrawBox(int x, int y, int w, int h, int r, int g, int b, int a,
             int thickness) {
  if (!g_pfnFillRGBA)
    return;
  g_pfnFillRGBA(x, y, w, thickness, r, g, b, a);                 // Top
  g_pfnFillRGBA(x, y + h - thickness, w, thickness, r, g, b, a); // Bottom
  g_pfnFillRGBA(x, y, thickness, h, r, g, b, a);                 // Left
  g_pfnFillRGBA(x + w - thickness, y, thickness, h, r, g, b, a); // Right
}

// ESP: World-to-Screen using engine's TriAPI
bool W2S(float *origin, float &screenX, float &screenY, int scrW, int scrH) {
  if (!g_pfnTriWorldToScreen)
    return false;
  float screen[2];
  // TriAPI WorldToScreen returns 1 if behind camera, 0 if visible
  if (g_pfnTriWorldToScreen(origin, screen))
    return false;
  // screen[] is in NDC: -1..1 range
  screenX = (1.0f + screen[0]) * scrW * 0.5f;
  screenY = (1.0f - screen[1]) * scrH * 0.5f;
  return true;
}

int __cdecl Hook_DrawEngine() {
  // Call Original (Draws Server HUD)
  int ret = 0;
  if (g_Original_DrawEngine) {
    ret = g_Original_DrawEngine();
  }
  // ESP drawing moved to Hook_HUD_Redraw (fires after 3D world renders)
  return ret;
}

// ===== HUD_REDRAW HOOK (ESP drawing happens here, after 3D world) =====
int __cdecl Hook_HUD_Redraw(float time, int intermission) {
  // Anti-Screenshot: Re-issue logic
  if (g_AntiSS_PendingSnapshot > 0) {
      g_AntiSS_PendingSnapshot--;
      if (g_AntiSS_PendingSnapshot == 5) {
          // Call UNHOOKED buffer to avoid another block
          if (g_pfnCbuf_AddText) {
              memcpy((void *)g_pfnCbuf_AddText, g_Orig_Cbuf, 6);
              g_pfnCbuf_AddText("snapshot\n");
              // Rehook
              BYTE patch[5] = {0xE9, 0, 0, 0, 0};
              *(DWORD *)(patch + 1) = (DWORD)Hook_Cbuf_AddText - (DWORD)g_pfnCbuf_AddText - 5;
              memcpy((void *)g_pfnCbuf_AddText, patch, 5);
              *(BYTE *)((DWORD)g_pfnCbuf_AddText + 5) = 0x90;
          }
      }
  }

  // 1. Unhook
  memcpy((void *)g_HUD_Redraw_Addr, g_Orig_HUD_Redraw, 6);

  // 2. Ensure GL functions are loaded (needed for state management)
  static bool glLoaded = false;
  if (!glLoaded) {
    HMODULE hGL = GetModuleHandleA("opengl32.dll");
    if (hGL) {
      if (!g_glEnable)
        g_glEnable = (glEnable_t)GetProcAddress(hGL, "glEnable");
      if (!g_glDisable)
        g_glDisable = (glDisable_t)GetProcAddress(hGL, "glDisable");
      if (!g_glColor4f)
        g_glColor4f = (glColor4f_t)GetProcAddress(hGL, "glColor4f");
      if (!g_glPushAttrib)
        g_glPushAttrib = (glPushAttrib_t)GetProcAddress(hGL, "glPushAttrib");
      if (!g_glPopAttrib)
        g_glPopAttrib = (glPopAttrib_t)GetProcAddress(hGL, "glPopAttrib");
      glLoaded = true;
    }
  }

  // 3. Restore GL state BEFORE original HUD draws
  // StudioDrawPlayer (Glow ESP) may have left dirty state from the 3D scene
  if (g_glEnable)
    g_glEnable(MY_GL_TEXTURE_2D);
  if (g_glDisable)
    g_glDisable(MY_GL_BLEND);
  if (g_glColor4f)
    g_glColor4f(1.0f, 1.0f, 1.0f, 1.0f);

  // 4. Call Original (Draw engine HUD / Sniper Overlays)
  int res = ((HUD_Redraw_t)g_HUD_Redraw_Addr)(time, intermission);

  // 5. Rehook
  BYTE patch[5] = {0xE9, 0, 0, 0, 0};
  *(DWORD *)(patch + 1) = (DWORD)Hook_HUD_Redraw - (DWORD)g_HUD_Redraw_Addr - 5;
  memcpy((void *)g_HUD_Redraw_Addr, patch, 5);
  *(BYTE *)((DWORD)g_HUD_Redraw_Addr + 5) = 0x90;

  // 6. Check Anti-SS (Only run our visuals if screen is NOT being captured)
  if (ShouldHideVisuals()) {
      return res;
  }

  // 7. Update HideWeapon dynamically if 'ch' is toggled
  static bool lastCrosshairState = false;
  bool currentCrosshairState = (g_cvar_ch.value != 0.0f);
  if (lastCrosshairState != currentCrosshairState) {
    lastCrosshairState = currentCrosshairState;
    if (g_Original_HideWeapon) {
      byte flags = g_ServerHideWeaponFlags;
      if (currentCrosshairState) {
        flags |= (1 << 6); // HIDEHUD_MISCSTATUS
      }
      g_Original_HideWeapon("HideWeapon", 1, &flags);
    }
  }

  // 8. Visual Calculations (FPS etc)
  DWORD curTime = GetTickCount();
  if (g_LastFPSUpdateTime == 0) g_LastFPSUpdateTime = curTime;
  DWORD elapsed = curTime - g_LastFPSUpdateTime;
  if (elapsed >= 1000) {
    g_RealFPS = (float)g_FPSFrameCount * 1000.0f / (float)elapsed;
    g_FPSFrameCount = 0;
    g_LastFPSUpdateTime = curTime;
  }

  // 9. Rehook Safeties (Fast-Hook maintenance)
  if (g_HooksActive) {
    if (g_HUD_PlayerMove_Addr && *(BYTE *)g_HUD_PlayerMove_Addr != 0xE9) {
      *(DWORD *)(patch + 1) = (DWORD)Hook_HUD_PlayerMove - (DWORD)g_HUD_PlayerMove_Addr - 5;
      memcpy((void *)g_HUD_PlayerMove_Addr, patch, 5);
      *(BYTE *)((DWORD)g_HUD_PlayerMove_Addr + 5) = 0x90;
    }
  }

  // 10. Draw Cheat Visuals
  if (g_glPushAttrib)
    g_glPushAttrib(MY_GL_ALL_ATTRIB_BITS);

  if (g_HooksActive) {
    bool drawCT = (g_cvar_ct_esp.value != 0.0f);
    bool drawT = (g_cvar_t_esp.value != 0.0f);
    int espType = (int)g_cvar_esp_type.value;
    if (espType == 0) espType = 3;
    bool drawBox = (espType & 2);

    if (drawBox && (drawCT || drawT) && g_pfnFillRGBA && g_pfnTriWorldToScreen && g_pfnGetEntityByIndex) {
      int scrW = 800, scrH = 600;
      if (g_pfnGetScreenInfo) {
        static SCREENINFO scr;
        scr.iSize = sizeof(SCREENINFO);
        g_pfnGetScreenInfo(&scr);
        if (scr.iWidth > 0) { scrW = scr.iWidth; scrH = scr.iHeight; }
      }

      void *localEnt = g_pfnGetLocalPlayer ? g_pfnGetLocalPlayer() : nullptr;
      float lx=0, ly=0, lz=0;
      if (localEnt) ReadOrigin(localEnt, ESP_ORIGIN_OFFSET, lx, ly, lz);
      bool showLabels = (g_cvar_esp_label.value != 0.0f);

      for (int i = 1; i <= 32; i++) {
        int team = g_PlayerTeam[i];
        if (team == 0 && g_pfnGetPlayerInfo) {
            hud_player_info_t info; memset(&info, 0, sizeof(info));
            g_pfnGetPlayerInfo(i, &info);
            if (info.model) { team = GetTeamFromModel(info.model); if (team != 0) g_PlayerTeam[i] = team; }
        }
        if (team == 0) continue;
        if (team == 1 && !drawT) continue;
        if (team == 2 && !drawCT) continue;

        void *ent = g_pfnGetEntityByIndex(i);
        if (!ent || ent == localEnt) continue;

        int modelIdx = *(int *)((char *)ent + ESP_ORIGIN_OFFSET + 24);
        if (modelIdx == 0) continue;

        char playerName[32] = {0};
        if (g_pfnGetPlayerInfo) {
          hud_player_info_t info; memset(&info, 0, sizeof(info));
          g_pfnGetPlayerInfo(i, &info);
          if (!info.name || !info.name[0]) continue;
          strncpy(playerName, info.name, 31);
        }

        int seq = *(int *)((char *)ent + ESP_ORIGIN_OFFSET + 28);
        if (seq > 100) continue;

        float entMsgTime = *(float *)((char *)ent + ESP_ORIGIN_OFFSET - 8);
        if (entMsgTime > 0.0f && (time - entMsgTime) > 1.0f) continue;

        float ox, oy, oz;
        ReadOrigin(ent, ESP_ORIGIN_OFFSET, ox, oy, oz);
        
        float feetOrigin[3] = {ox, oy, oz - 36.0f};
        float headOrigin[3] = {ox, oy, oz + 36.0f}; 
        float fx, fy, hx, hy;
        if (W2S(feetOrigin, fx, fy, scrW, scrH) && W2S(headOrigin, hx, hy, scrW, scrH)) {
           float bh = fy - hy;
           if (bh > 4.0f && bh < scrH) {
             float bw = bh * 0.5f;
             int bx = (int)(hx - bw / 2.0f);
             int cr = (team == 1 ? 255 : 50), cg = (team == 1 ? 50 : 100), cb = (team == 1 ? 50 : 255);
             DrawBox(bx, (int)hy, (int)bw, (int)bh, cr, cg, cb, 200, 2);

             if (showLabels && bh > 20.0f && g_pfnDrawConsoleString) {
                float dist = 0;
                if (localEnt) { float dx = ox - lx, dy = oy - ly, dz = oz - lz; dist = sqrt(dx*dx + dy*dy + dz*dz); }
                char label[96]; sprintf(label, "%s [%.0fm]", playerName, dist / 40.0f);
                g_pfnDrawSetTextColor((float)cr/255.0f, (float)cg/255.0f, (float)cb/255.0f);
                
                if (g_glMatrixMode && g_glPushMatrix) {
                   float scale = (bh < 100.0f) ? (0.5f + (bh / 200.0f)) : 1.0f;
                   if (scale < 0.5f) scale = 0.5f;
                   g_glMatrixMode(MY_GL_PROJECTION); g_glPushMatrix(); g_glScalef(scale, scale, 1.0f);
                   g_pfnDrawConsoleString((int)((float)bx/scale), (int)((hy-12.0f)/scale), label);
                   g_glPopMatrix();
                } else {
                   g_pfnDrawConsoleString(bx, (int)(hy - 12), label);
                }
             }
           }
        }
      }
    }

    // Speedometer
    if (g_cvar_speedometer.value != 0.0f && g_pfnGetLocalPlayer && g_pfnDrawConsoleString) {
        float engineSpeed = sqrtf((g_TrueEngineVelocity[0] * g_TrueEngineVelocity[0]) + (g_TrueEngineVelocity[1] * g_TrueEngineVelocity[1]));
        
        // Fix: If hasn't updated in 0.5s (Alt-Tabbed), zero it out so it doesn't stay stuck on old speed
        if (GetTickCount() - g_LastVelocityUpdateTime > 500) engineSpeed = 0.0f;

        static float dispSpeed = 0.0f;
        static float lastUpd = 0.0f;
        // Fix: Use 0.1s update freq but handle engine time resets/wraps properly
        if (time < lastUpd || time - lastUpd >= 0.1f) { dispSpeed = engineSpeed; lastUpd = time; }
        
        if (dispSpeed >= 0.0f && dispSpeed < 4000.0f) {
           char speedText[32]; sprintf(speedText, "Speed: %.2f", dispSpeed);
           int cr = 0, cg = 255, cb = 255;
           if (g_cvar_speedometer_color.string) sscanf(g_cvar_speedometer_color.string, "%d %d %d", &cr, &cg, &cb);
           g_pfnDrawSetTextColor((float)cr/255.0f, (float)cg/255.0f, (float)cb/255.0f);
           
           int dx = 400, dy = 500;
           if (g_pfnGetScreenInfo) {
              static SCREENINFO sinfo; sinfo.iSize = sizeof(SCREENINFO); g_pfnGetScreenInfo(&sinfo);
              if (sinfo.iWidth > 0) { dx = sinfo.iWidth/2 - 40; dy = sinfo.iHeight - 100; }
           }
           g_pfnDrawConsoleString(dx, dy, speedText);
        }
    }

    // FPS
    if (g_cvar_showfps.value != 0.0f && g_pfnDrawConsoleString) {
        char fpsText[32]; sprintf(fpsText, "FPS: %d", (int)g_RealFPS);
        g_pfnDrawSetTextColor(0.5f, 1.0f, 0.0f); g_pfnDrawConsoleString(10, 10, fpsText);
    }

    // Crosshair
    bool isScoped = (g_CurrentFOV < 90 && g_CurrentFOV > 0);
    bool isSniper = (g_CurrentWeaponID == 18 || g_CurrentWeaponID == 3);
    bool forceCH = (g_cvar_no_scope.value != 0.0f && isSniper);

    if ((g_cvar_ch.value != 0.0f || forceCH) && !isScoped && g_pfnFillRGBA) {
       SCREENINFO scr; scr.iSize = sizeof(SCREENINFO); g_pfnGetScreenInfo(&scr);
       if (scr.iWidth > 0) {
         int cx = scr.iWidth/2, cy = scr.iHeight/2;
         int r=0, g=255, b=0, a=255;
         if (g_cvar_ch_color.string) sscanf(g_cvar_ch_color.string, "%d %d %d", &r, &g, &b);
         int len=5, off=5, thk=2;
         if (g_cvar_ch_length.string) len = atoi(g_cvar_ch_length.string);
         if (g_cvar_ch_offset.string) off = atoi(g_cvar_ch_offset.string);
         if (g_cvar_ch_thickness.string) thk = atoi(g_cvar_ch_thickness.string);
         
         g_pfnFillRGBA(cx-(thk/2), cy-off-len, thk, len, r, g, b, a); // Top
         g_pfnFillRGBA(cx-(thk/2), cy+off, thk, len, r, g, b, a);     // Bottom
         g_pfnFillRGBA(cx-off-len, cy-(thk/2), len, thk, r, g, b, a); // Left
         g_pfnFillRGBA(cx+off, cy-(thk/2), len, thk, r, g, b, a);     // Right
       }
    }
  }

  if (g_glPopAttrib)
    g_glPopAttrib();

  return res;
}

DWORD FindEngineDraw() {
  const char *sig = "\x90\xE8\x00\x00\x00\x00\x85\xC0\x74\x00\xE8";
  const char *mask = "xx????xxx?x";

  DWORD addr = FindPattern(g_HwBase, 0x1200000, sig, mask);
  if (addr) {
    return addr + 1;
  }
  return 0;
}

// -------------------------------------------------------------
// ASM TRAMPOLINE
// -------------------------------------------------------------
extern "C" {
volatile int g_SkipVal = 0;
int g_FrameCount = 0;
}

extern "C" void Hook_SCR_Trampoline();
__asm__(".intel_syntax noprefix\n"
        ".globl _Hook_SCR_Trampoline\n"
        "_Hook_SCR_Trampoline:\n"
        "inc dword ptr [_g_FPSFrameCount]\n"
        "push eax\n"
        "push ecx\n"
        "mov eax, [_g_SkipVal]\n"
        "test eax, eax\n"
        "jz .L_run\n"
        "mov ecx, [_g_FrameCount]\n"
        "dec ecx\n"
        "mov [_g_FrameCount], ecx\n"
        "jg .L_skip\n"
        "mov [_g_FrameCount], eax\n"
        ".L_run:\n"
        "pop ecx\n"
        "pop eax\n"
        "pushad\n"
        "pushfd\n"
        "call _SayiBilen_Update\n"
        "popfd\n"
        "popad\n"
        "push ebp\n"
        "mov ebp, esp\n"
        "sub esp, 0x10\n"
        "jmp DWORD PTR [_g_JmpTarget]\n"
        ".L_skip:\n"
        "pop ecx\n"
        "pop eax\n"
        "ret\n"
        ".att_syntax\n");

// -------------------------------------------------------------
// INSTALL/REMOVE
// -------------------------------------------------------------
void PatchByte(DWORD addr, BYTE val) {
  DWORD old;
  VirtualProtect((void *)addr, 1, PAGE_EXECUTE_READWRITE, &old);
  *(BYTE *)addr = val;
  VirtualProtect((void *)addr, 1, old, &old);
}

void WriteJMP(DWORD from, DWORD to, BYTE *storage, int len) {
  // Assume memory is ALREADY unprotected by caller if needed for persistent
  // fast-patching
  bool empty = true;
  for (int i = 0; i < len; i++)
    if (storage[i])
      empty = false;
  if (empty)
    memcpy(storage, (void *)from, len);
  BYTE patch[5] = {0xE9, 0, 0, 0, 0};
  *(DWORD *)(patch + 1) = to - from - 5;
  memcpy((void *)from, patch, 5);
  if (len > 5)
    memset((void *)(from + 5), 0x90, len - 5);
}

// Allocates executable memory, copies original bytes, and adds a JMP back
void *CreateTrampoline(DWORD srcAddr, BYTE *origBytes, int len) {
  // Allocate executable memory
  void *tramp = VirtualAlloc(NULL, len + 5, MEM_COMMIT | MEM_RESERVE,
                             PAGE_EXECUTE_READWRITE);
  if (!tramp)
    return nullptr;

  // Copy stolen bytes to trampoline
  memcpy(tramp, origBytes, len);

  // Write JMP back to original function + len
  BYTE jmp[5] = {0xE9, 0, 0, 0, 0};
  *(DWORD *)(jmp + 1) = (srcAddr + len) - ((DWORD)tramp + len) - 5;
  memcpy((BYTE *)tramp + len, jmp, 5);

  return tramp;
}

void Restore(DWORD from, BYTE *storage, int len) {
  DWORD old;
  VirtualProtect((void *)from, len, PAGE_EXECUTE_READWRITE, &old);
  memcpy((void *)from, storage, len);
  VirtualProtect((void *)from, len, old, &old);
}


// -------------------------------------------------------------
// +strafe_boost: Air Strafe Optimizer
// -------------------------------------------------------------
// Mathematically optimal air-strafe boost.
// GoldSrc air acceleration caps wishspeed at ~30 units, so input
// MAGNITUDE doesn't affect speed gain — only DIRECTION matters.
// This fully overrides the direction on every frame for maximum speed gain.
static void ApplyStrafeHelper(void *cmd) {
  if (!cmd) return;

  float *pViewAngles = (float *)((char *)cmd + 4);
  float *pForward    = (float *)((char *)cmd + 16);
  float *pSideMove   = (float *)((char *)cmd + 20);

  float vx = g_TrueEngineVelocity[0], vy = g_TrueEngineVelocity[1];
  float speed2D = sqrtf(vx * vx + vy * vy);
  float yaw_rad = pViewAngles[1] * (M_PI_F / 180.0f);

  // If nearly standing still, just push sideways (alternating) to start gaining speed
  if (speed2D < 15.0f) {
    static bool sway = true;
    static int swayCount = 0;
    swayCount++;
    if (swayCount >= 2) { swayCount = 0; sway = !sway; }
    *pForward = 0;
    *pSideMove = sway ? 400.0f : -400.0f;
    return;
  }

  // Calculate optimal strafe direction
  float vel_angle = atan2f(vy, vx);
  float angle_diff = vel_angle - yaw_rad;
  while (angle_diff > M_PI_F) angle_diff -= 2.0f * M_PI_F;
  while (angle_diff < -M_PI_F) angle_diff += 2.0f * M_PI_F;

  float sin_diff = sinf(angle_diff);
  float optimalDir = (sin_diff >= 0.0f) ? 1.0f : -1.0f;

  // Calculate the mathematically optimal movement values (magnitude = 400)
  float optSideMove = optimalDir * 400.0f;

  float osin = sinf(yaw_rad);
  float ocos = cosf(yaw_rad);
  float nangle = yaw_rad * 2.0f - vel_angle;
  float nsin = sinf(nangle);
  float ncos = cosf(nangle);
  float optForward = optSideMove * (osin * ncos - ocos * nsin);

  // Fully override manual input with optimal input (100% boost)
  *pForward  = optForward;
  *pSideMove = optSideMove;
}

// -------------------------------------------------------------
// Mode 1: Legit (Mouse turn -> A/D)
// Mode 2: Hybrid (Legit + Rage Boost)
// -------------------------------------------------------------
static void ApplyStrafeHelperLegit(void *cmd, float deltaYaw, int mode) {
    if (!cmd) return;

    unsigned short *pButtons = (unsigned short *)((char *)cmd + 30);
    float *pSideMove = (float *)((char *)cmd + 20);

    if (mode == 1) {
        // LEGIT: precise button + sidemove sync
        if (fabsf(deltaYaw) < 0.1f) return;

        if (deltaYaw < 0.0f) {
            *pButtons &= ~IN_MOVELEFT;
            *pButtons |= IN_MOVERIGHT;
            *pSideMove = 400.0f;
        } else {
            *pButtons &= ~IN_MOVERIGHT;
            *pButtons |= IN_MOVELEFT;
            *pSideMove = -400.0f;
        }
    }
    else if (mode == 2) {
        // RAGE: smoothed yaw + optimal strafe boost on top
        if (fabsf(deltaYaw) < 0.1f) return;

        static float s_smoothedYaw = 0.0f;
        s_smoothedYaw = s_smoothedYaw * 0.6f + deltaYaw * 0.4f;

        float mouseDir = (s_smoothedYaw < 0.0f) ? 1.0f : -1.0f;
        *pSideMove = mouseDir * 400.0f;

        ApplyStrafeHelper(cmd);
    }
}

// -------------------------------------------------------------
// +strafe_boost: CL_CreateMove Hook
// -------------------------------------------------------------
void __cdecl Hook_CL_CreateMove(float frametime, void *cmd, int active) {
  if (!g_CL_CreateMove_Addr) return;

  // Unhook, call original
  DWORD old;
  VirtualProtect((void *)g_CL_CreateMove_Addr, 10, PAGE_EXECUTE_READWRITE, &old);
  memcpy((void *)g_CL_CreateMove_Addr, g_Orig_CL_CreateMove, 6);
  VirtualProtect((void *)g_CL_CreateMove_Addr, 10, old, &old);

  g_Original_CL_CreateMove(frametime, cmd, active);
  
  if (cmd && active) {
    unsigned short *pButtons = (unsigned short *)((char *)cmd + 30);
    float *pViewAngles = (float *)((char *)cmd + 4);

    // Track Mouse Delta
    static float s_lastYaw = 0.0f;
    float currentYaw = pViewAngles[1];
    float deltaYaw = currentYaw - s_lastYaw;
    while (deltaYaw > 180.0f) deltaYaw -= 360.0f;
    while (deltaYaw < -180.0f) deltaYaw += 360.0f;
    s_lastYaw = currentYaw;

    // --- Auto Bunny Hop (Crouch-Bhop Style) ---
    if (g_AutoBhopActive) {
      bool onGround = (g_PlayerFlags & FL_ONGROUND) != 0;

      if (onGround) {
        // Landing frame: JUMP + release duck
        *pButtons |= IN_JUMP;
        *pButtons &= ~IN_DUCK;
      } else {
        // Airborne: hold DUCK (pull legs up) + release JUMP
        *pButtons |= IN_DUCK;
        *pButtons &= ~IN_JUMP;
      }
    }

    // --- Strafe Boost (+strafe_boost) ---
    if (g_StrafeBoostActive && g_cvar_strafe_helper.value < 1.0f) {
      float vz = g_TrueEngineVelocity[2];
      float vx = g_TrueEngineVelocity[0];
      float vy = g_TrueEngineVelocity[1];
      float speed2D = sqrtf(vx * vx + vy * vy);
      bool isInAir = (vz > 1.0f || vz < -1.0f) || speed2D > 100.0f;

      if (isInAir) {
        ApplyStrafeHelper(cmd);
      }
    }

    // --- Strafe Helper (strafe_helper 1/2) ---
    if (g_StrafeHelperActive && g_cvar_strafe_helper.value >= 1.0f) {
      float vz = g_TrueEngineVelocity[2];
      float speed2D = sqrtf(g_TrueEngineVelocity[0] * g_TrueEngineVelocity[0] + 
                            g_TrueEngineVelocity[1] * g_TrueEngineVelocity[1]);
      bool isInAir = (vz > 1.0f || vz < -1.0f) || speed2D > 100.0f;

      if (isInAir) {
        ApplyStrafeHelperLegit(cmd, deltaYaw, (int)g_cvar_strafe_helper.value);
      }
    }

    // --- Null Cancelling Movement (Snap Tap) ---
    if (g_cvar_null_canceling_movement.value != 0.0f) {
        static bool s_wasA = false;
        static bool s_wasD = false;
        static bool s_wasW = false;
        static bool s_wasS = false;
        static int s_lastSide = 0; // 0=None, 1=Left, 2=Right
        static int s_lastForward = 0; // 0=None, 1=Forward, 2=Back

        float *pForwardMove = (float *)((char *)cmd + 16);
        float *pSideMove = (float *)((char *)cmd + 20);

        bool isA = ((*pButtons) & IN_MOVELEFT) != 0;
        bool isD = ((*pButtons) & IN_MOVERIGHT) != 0;
        bool isW = ((*pButtons) & IN_FORWARD) != 0;
        bool isS = ((*pButtons) & IN_BACK) != 0;

        // Lazy load speed cvars
        if (!g_pCvar_SideSpeed) g_pCvar_SideSpeed = FindCvarByName("cl_sidespeed");
        if (!g_pCvar_ForwardSpeed) g_pCvar_ForwardSpeed = FindCvarByName("cl_forwardspeed");
        if (!g_pCvar_BackSpeed) g_pCvar_BackSpeed = FindCvarByName("cl_backspeed");

        float sideSpeed = g_pCvar_SideSpeed ? g_pCvar_SideSpeed->value : 400.0f;
        float forwardSpeed = g_pCvar_ForwardSpeed ? g_pCvar_ForwardSpeed->value : 400.0f;
        float backSpeed = g_pCvar_BackSpeed ? g_pCvar_BackSpeed->value : 400.0f;

        // Side Axis
        if (isA && !s_wasA) s_lastSide = 1;
        if (isD && !s_wasD) s_lastSide = 2;
        if (isA && isD) {
            if (s_lastSide == 1) {
                *pButtons &= ~IN_MOVERIGHT;
                *pSideMove = -sideSpeed;
            } else if (s_lastSide == 2) {
                *pButtons &= ~IN_MOVELEFT;
                *pSideMove = sideSpeed;
            }
        }
        s_wasA = isA;
        s_wasD = isD;

        // Forward Axis
        if (isW && !s_wasW) s_lastForward = 1;
        if (isS && !s_wasS) s_lastForward = 2;
        if (isW && isS) {
            if (s_lastForward == 1) {
                *pButtons &= ~IN_BACK;
                *pForwardMove = forwardSpeed;
            } else if (s_lastForward == 2) {
                *pButtons &= ~IN_FORWARD;
                *pForwardMove = -backSpeed;
            }
        }
        s_wasW = isW;
        s_wasS = isS;
    }

    // --- Ground Strafe (SGS) ---
    if (g_SGSActive && g_cvar_sgs.value >= 1.0f) {
      static bool s_did_duck = false;
      bool onGround = (g_PlayerFlags & FL_ONGROUND) != 0;

      if (onGround && !s_did_duck) {
        *pButtons |= IN_DUCK;
        s_did_duck = true;
      } else {
        if (s_did_duck) {
          *pButtons &= ~IN_DUCK;
          s_did_duck = false;
        }
      }

      // RAGE MODE (sgs 2): Add Air Strafe Boost
      if (g_cvar_sgs.value >= 2.0f) {
        ApplyStrafeHelper(cmd);
      }
    }

    // --- Quick Scope (AWP/Scout) with QQ ---
    if (g_cvar_qs.value != 0.0f) {
        // AWP = 18, Scout = 3 or 21/22? User confirmed 18 for AWP, and mentioned 22.
        bool isSniper = (g_CurrentWeaponID == 18 || g_CurrentWeaponID == 3);
        if (isSniper) {
            bool isScoped = (g_CurrentFOV < 90 && g_CurrentFOV > 0);
            
            if (g_QuickScopeState == 1) {
                // State 1: Wait for scope to open
                *pButtons &= ~IN_ATTACK;
                static int s_scopeTimeout = 0;
                if (isScoped) {
                    g_QS_WaitTicks = 3; 
                    g_QuickScopeState = 2;
                    s_scopeTimeout = 0;
                } else if (++s_scopeTimeout > 30) {
                    g_QuickScopeState = 0;
                    s_scopeTimeout = 0;
                }
            }
            else if (g_QuickScopeState == 2) {
                // State 2: Accuracy delay
                *pButtons &= ~IN_ATTACK;
                if (g_QS_WaitTicks > 0) {
                    g_QS_WaitTicks--;
                } else {
                    g_QuickScopeState = 3;
                }
            }
            else if (g_QuickScopeState == 3) {
                // State 3: Fire!
                *pButtons |= IN_ATTACK;
                g_QS_WaitTicks = 10; // Wait 10 frames for server registration
                g_QuickScopeState = 4;
            }
            else if (g_QuickScopeState == 4) {
                // State 4: Fire persistence (ensure server sees shot/ammo depletion)
                *pButtons |= IN_ATTACK;
                if (g_QS_WaitTicks > 0) {
                    g_QS_WaitTicks--;
                } else {
                    g_QuickScopeState = 5;
                }
            }
            else if (g_QuickScopeState == 5) {
                // State 5: Switch back to sniper
                if (g_pfnClientCmd) {
                    g_pfnClientCmd("lastinv; lastinv"); // QQ switch without menu interference
                }
                g_QuickScopeState = 0;
            }
            else if ((*pButtons) & IN_ATTACK) {
                if (!isScoped) {
                    // Start Quick Scope sequence (Zoom -> Fire -> QQ)
                    *pButtons &= ~IN_ATTACK;
                    *pButtons |= IN_ATTACK2;
                    g_QuickScopeState = 1;
                } else {
                    // Manual fire while scoped -> Trigger QQ sequence
                    g_QuickScopeState = 3;
                }
            }
        } else {
            g_QuickScopeState = 0;
        }
    }
  }

  // Re-hook
  VirtualProtect((void *)g_CL_CreateMove_Addr, 10, PAGE_EXECUTE_READWRITE, &old);
  BYTE patch[5] = {0xE9, 0, 0, 0, 0};
  *(DWORD *)(patch + 1) = (DWORD)Hook_CL_CreateMove - g_CL_CreateMove_Addr - 5;
  memcpy((void *)g_CL_CreateMove_Addr, patch, 5);
  *(BYTE *)(g_CL_CreateMove_Addr + 5) = 0x90;
  VirtualProtect((void *)g_CL_CreateMove_Addr, 10, old, &old);
}

void InstallHooks() {
  if (g_HooksActive)
    return;

  LogDebug("[Hooks] InstallHooks() called.\n");

  PatchByte(g_HwBase + OFF_CMD_PATCH, 0xEB);

  // Locate HUD_PlayerMove (once)
  if (!g_HUD_PlayerMove_Addr) {
    HMODULE hClient = GetModuleHandleA("client.dll");
    if (hClient) {
      HUD_PlayerMove_t pfnPMove =
          (HUD_PlayerMove_t)GetProcAddress(hClient, "HUD_PlayerMove");
      if (!pfnPMove)
        pfnPMove =
            (HUD_PlayerMove_t)GetProcAddress(hClient, "_HUD_PlayerMove@8");
      if (pfnPMove) {
        g_HUD_PlayerMove_Addr = (DWORD)pfnPMove;
        LogDebug("[Hooks] Found HUD_PlayerMove at 0x%X\n", g_HUD_PlayerMove_Addr);
        DWORD old;
        VirtualProtect((void *)g_HUD_PlayerMove_Addr, 6, PAGE_EXECUTE_READWRITE, &old);
        memcpy(g_Orig_HUD_PlayerMove, (void *)g_HUD_PlayerMove_Addr, 6);
      }
    }
  }
  // ALWAYS re-apply JMP patch (critical for F11 toggle)
  if (g_HUD_PlayerMove_Addr) {
    DWORD old;
    VirtualProtect((void *)g_HUD_PlayerMove_Addr, 6, PAGE_EXECUTE_READWRITE, &old);
    BYTE patch[5] = {0xE9, 0, 0, 0, 0};
    *(DWORD *)(patch + 1) =
        (DWORD)Hook_HUD_PlayerMove - (DWORD)g_HUD_PlayerMove_Addr - 5;
    memcpy((void *)g_HUD_PlayerMove_Addr, patch, 5);
    *(BYTE *)((DWORD)g_HUD_PlayerMove_Addr + 5) = 0x90;
    LogDebug("[Hooks] HUD_PlayerMove patched (0x%X)\n", g_HUD_PlayerMove_Addr);
  }

  // Install DrawEngine Hook (cached - only scan once)
  if (!g_DrawEngineAddr) {
    g_DrawEngineAddr = FindEngineDraw();
    if (g_DrawEngineAddr) {
      DWORD offset = *(DWORD *)(g_DrawEngineAddr + 1);
      g_Original_DrawEngine = (DrawEngine_t)(g_DrawEngineAddr + 5 + offset);
      // Save original CALL offset bytes for restore
      memcpy(g_Orig_DrawEngine_CallOffset, (void *)(g_DrawEngineAddr + 1), 4);
      LogDebug("[Hooks] Found DrawEngine Call at 0x%X, Target: 0x%X\n",
               g_DrawEngineAddr, g_Original_DrawEngine);
    } else {
      LogDebug("[Hooks] FAILED to find DrawEngine pattern.\n");
    }
  }
  if (g_DrawEngineAddr) {
    DWORD newOffset = (DWORD)Hook_DrawEngine - (g_DrawEngineAddr + 5);
    DWORD old;
    VirtualProtect((void *)(g_DrawEngineAddr + 1), 4, PAGE_EXECUTE_READWRITE,
                   &old);
    *(DWORD *)(g_DrawEngineAddr + 1) = newOffset;
    VirtualProtect((void *)(g_DrawEngineAddr + 1), 4, old, &old);
  }

  if (g_SCR) {
    DWORD old;
    VirtualProtect((void *)g_SCR, 6, PAGE_EXECUTE_READWRITE, &old);
    WriteJMP(g_SCR, (DWORD)Hook_SCR_Trampoline, g_Orig_SCR, 6);
    VirtualProtect((void *)g_SCR, 6, old, &old);
  }
  if (g_CreateInterface) {
    DWORD old;
    VirtualProtect((void *)g_CreateInterface, 5, PAGE_EXECUTE_READWRITE, &old);
    WriteJMP(g_CreateInterface, (DWORD)Hook_CreateInterface, g_Orig_CI, 5);
    VirtualProtect((void *)g_CreateInterface, 5, old, &old);
  }
  if (g_SteamInternal) {
    DWORD old;
    VirtualProtect((void *)g_SteamInternal, 5, PAGE_EXECUTE_READWRITE, &old);
    WriteJMP(g_SteamInternal, (DWORD)Hook_SteamInternal, g_Orig_SI, 5);
    VirtualProtect((void *)g_SteamInternal, 5, old, &old);
  }
  if (g_GetVolumeInfo) {
    DWORD old;
    VirtualProtect((void *)g_GetVolumeInfo, 5, PAGE_EXECUTE_READWRITE, &old);
    WriteJMP(g_GetVolumeInfo, (DWORD)Hook_GetVolInfo, g_Orig_Vol, 5);
    VirtualProtect((void *)g_GetVolumeInfo, 5, old, &old);
  }

  if (g_pfnConPrintf) {
    LogDebug("[Hooks] Installing ConPrintf hook at 0x%X -> 0x%X\n",
             (DWORD)g_pfnConPrintf, (DWORD)Hook_ConPrintf);
    // PERMANENT UNLOCK for Fast-Patching zero-overhead
    DWORD old;
    VirtualProtect((void *)g_pfnConPrintf, 6, PAGE_EXECUTE_READWRITE, &old);
    WriteJMP((DWORD)g_pfnConPrintf, (DWORD)Hook_ConPrintf, g_Orig_ConPrintf, 6);
    LogDebug("[Hooks] ConPrintf Hooked successfully (Memory unlocked)!\n");
  } else {
    LogDebug("[Hooks] WARNING: g_pfnConPrintf is NULL, skipping hook.\n");
  }

  // Install HUD_Redraw hook (for ESP drawing after 3D scene)
  if (!g_HUD_Redraw_Addr) {
    HMODULE hClient = GetModuleHandleA("client.dll");
    if (hClient) {
      g_pfnHUD_Redraw = (HUD_Redraw_t)GetProcAddress(hClient, "_HUD_Redraw@8");
      if (!g_pfnHUD_Redraw)
        g_pfnHUD_Redraw = (HUD_Redraw_t)GetProcAddress(hClient, "HUD_Redraw");
      if (g_pfnHUD_Redraw) {
        g_HUD_Redraw_Addr = (DWORD)g_pfnHUD_Redraw;
        LogDebug("[Hooks] Found HUD_Redraw at 0x%X\n", g_HUD_Redraw_Addr);
      } else {
        LogDebug("[Hooks] WARNING: HUD_Redraw not found in client.dll!\n");
      }
    }
  }
  if (g_HUD_Redraw_Addr) {
    DWORD old;
    // PERMANENT UNLOCK for Fast-Patching zero-overhead
    VirtualProtect((void *)g_HUD_Redraw_Addr, 6, PAGE_EXECUTE_READWRITE, &old);
    WriteJMP(g_HUD_Redraw_Addr, (DWORD)Hook_HUD_Redraw, g_Orig_HUD_Redraw, 6);
    LogDebug("[Hooks] HUD_Redraw hooked at 0x%X -> 0x%X (Memory unlocked)\n",
             g_HUD_Redraw_Addr, (DWORD)Hook_HUD_Redraw);
  }

  // Initialize Glow ESP (StudioDrawPlayer hook)
  InitGlowESP();

  // Detour Core Sprite Engine functions (Inline Hook)
  // TABLE HOOK Core Sprite Engine functions
  if (g_EngineTable) {
    LogDebug("[Hooks] Table Hooking SPR_Set, SPR_Draw...\n");
    DWORD old;
    if (VirtualProtect(g_EngineTable, 100 * 4, PAGE_READWRITE, &old)) {
      g_Original_SPR_Set = (SPR_Set_t)g_EngineTable[4];
      g_EngineTable[4] = (void *)Hook_SPR_Set;

      g_Original_SPR_Draw = (SPR_Draw_t)g_EngineTable[5];
      g_EngineTable[5] = (void *)Hook_SPR_Draw;

      g_Original_SPR_DrawHoles = (SPR_DrawHoles_t)g_EngineTable[6];
      g_EngineTable[6] = (void *)Hook_SPR_DrawHoles;

      g_Original_SPR_DrawAdditive = (SPR_DrawAdditive_t)g_EngineTable[7];
      g_EngineTable[7] = (void *)Hook_SPR_DrawAdditive;

      VirtualProtect(g_EngineTable, 100 * 4, old, &old);
      LogDebug("[NoSmoke] Core Sprite functions Table Hooked!\n");
    }
  }

  // Pre-load smoke sprites and Detour R_TempSprite (Index 50)
  // (Redundant lazy-load block removed)

  // Force load smoke sprites NOW (don't wait for hooks)
  EnsureSmokeModelsLoaded();

  if (g_pEfxAPI) {
    void **efxTable = (void **)g_pEfxAPI;

    DWORD oldEfx;
    if (VirtualProtect(efxTable, 100 * 4, PAGE_READWRITE, &oldEfx)) {
      // Index 50: R_TempSprite
      if (efxTable[50]) {
        g_Original_R_TempSprite = (R_TempSprite_t)efxTable[50];
        efxTable[50] = (void *)Hook_R_TempSprite;
        LogDebug("[NoSmoke] Table Hooked R_TempSprite\n");
      }

      // Index 49: R_Sprite_Explosion
      if (efxTable[49]) {
        g_Original_R_Sprite_Explosion = (R_Sprite_Explosion_t)efxTable[49];
        efxTable[49] = (void *)Hook_R_Sprite_Explosion;
        LogDebug("[NoSmoke] Table Hooked R_Sprite_Explosion\n");
      }

      // Index 55: R_ParticleExplosion
      if (efxTable[55]) {
        g_Original_R_ParticleExplosion = (R_ParticleExplosion_t)efxTable[55];
        efxTable[55] = (void *)Hook_R_ParticleExplosion;
        LogDebug("[NoSmoke] Table Hooked R_ParticleExplosion\n");
      }

      // Index 58: R_RocketTrail
      if (efxTable[58]) {
        g_Original_R_RocketTrail = (R_RocketTrail_t)efxTable[58];
        efxTable[58] = (void *)Hook_R_RocketTrail;
        LogDebug("[NoSmoke] Table Hooked R_RocketTrail\n");
      }
      VirtualProtect(efxTable, 100 * 4, oldEfx, &oldEfx);
    }
  }

  // Hook SPR_Load (Table Hook) - Index 0
  if (g_EngineTable && !g_Original_SPR_Load_Hook) {
    g_pfnSPR_Load_Original = (SPR_Load_t)g_EngineTable[0];
    DWORD old;
    VirtualProtect(&g_EngineTable[0], 4, PAGE_READWRITE, &old);
    g_EngineTable[0] = (void *)Hook_SPR_Load;
    VirtualProtect(&g_EngineTable[0], 4, old, &old);
    LogDebug("[NoSmoke] Table Hooked SPR_Load at index 0\n");
    g_Original_SPR_Load_Hook = true;
  }

  // Re-patch smoke creation function (for F11 toggle back)
  if (g_SmokeFuncAddr && g_SmokeFuncOrigByte) {
    DWORD old;
    VirtualProtect((void *)g_SmokeFuncAddr, 1, PAGE_EXECUTE_READWRITE, &old);
    *(BYTE *)g_SmokeFuncAddr = 0xC3; // RET
    VirtualProtect((void *)g_SmokeFuncAddr, 1, old, &old);
    LogDebug("[NoSmoke] Smoke function re-patched -> RET!\n");
  }

  // Locate HUD_AddEntity (once)
  if (!g_HUD_AddEntity_Addr) {
    HMODULE hClient = GetModuleHandleA("client.dll");
    if (hClient) {
      HUD_AddEntity_t pfnAdd =
          (HUD_AddEntity_t)GetProcAddress(hClient, "HUD_AddEntity");
      if (!pfnAdd)
        pfnAdd = (HUD_AddEntity_t)GetProcAddress(hClient, "_HUD_AddEntity@12");
      if (pfnAdd) {
        g_HUD_AddEntity_Addr = (DWORD)pfnAdd;
        LogDebug("[Hooks] Found HUD_AddEntity at 0x%X\n", g_HUD_AddEntity_Addr);
        DWORD old;
        VirtualProtect((void *)g_HUD_AddEntity_Addr, 6, PAGE_EXECUTE_READWRITE, &old);
        memcpy(g_Orig_HUD_AddEntity, (void *)g_HUD_AddEntity_Addr, 6);
      }
    }
  }
  // ALWAYS re-apply JMP patch (critical for F11 toggle)
  if (g_HUD_AddEntity_Addr) {
    DWORD old;
    VirtualProtect((void *)g_HUD_AddEntity_Addr, 6, PAGE_EXECUTE_READWRITE, &old);
    BYTE patch[5] = {0xE9, 0, 0, 0, 0};
    *(DWORD *)(patch + 1) =
        (DWORD)Hook_HUD_AddEntity - (DWORD)g_HUD_AddEntity_Addr - 5;
    memcpy((void *)g_HUD_AddEntity_Addr, patch, 5);
    *(BYTE *)((DWORD)g_HUD_AddEntity_Addr + 5) = 0x90;
    LogDebug("[Hooks] HUD_AddEntity patched (0x%X)\n", g_HUD_AddEntity_Addr);
  }

  // CL_CreateMove Hook (Strafe Boost)
  if (!g_CL_CreateMove_Addr) {
    HMODULE hClient = GetModuleHandleA("client.dll");
    if (hClient) {
      const char *names[] = {"CL_CreateMove", "_CL_CreateMove",
                             "HUD_CL_CreateMove", "_HUD_CL_CreateMove", nullptr};
      for (int i = 0; names[i]; i++) {
        g_Original_CL_CreateMove =
            (HUD_CL_CreateMove_t)GetProcAddress(hClient, names[i]);
        if (g_Original_CL_CreateMove) {
          g_CL_CreateMove_Addr = (DWORD)g_Original_CL_CreateMove;
          LogDebug("[Hooks] Found CL_CreateMove at 0x%X (%s)\n",
                   g_CL_CreateMove_Addr, names[i]);
          break;
        }
      }
    }
  }
  if (g_CL_CreateMove_Addr) {
    DWORD old;
    VirtualProtect((void *)g_CL_CreateMove_Addr, 10, PAGE_EXECUTE_READWRITE, &old);
    memcpy(g_Orig_CL_CreateMove, (void *)g_CL_CreateMove_Addr, 6);
    BYTE patch[5] = {0xE9, 0, 0, 0, 0};
    *(DWORD *)(patch + 1) =
        (DWORD)Hook_CL_CreateMove - g_CL_CreateMove_Addr - 5;
    memcpy((void *)g_CL_CreateMove_Addr, patch, 5);
    *(BYTE *)(g_CL_CreateMove_Addr + 5) = 0x90;
    VirtualProtect((void *)g_CL_CreateMove_Addr, 10, old, &old);
    LogDebug("[Hooks] CL_CreateMove hooked (Strafe Boost ready)\n");
  }

  // HUD_PlayerMove Hook (for velocity reading)
  if (!g_HUD_PlayerMove_Addr) {
    HMODULE hClient = GetModuleHandleA("client.dll");
    if (hClient) {
      HUD_PlayerMove_t pfnPMove =
          (HUD_PlayerMove_t)GetProcAddress(hClient, "HUD_PlayerMove");
      if (!pfnPMove)
        pfnPMove =
            (HUD_PlayerMove_t)GetProcAddress(hClient, "_HUD_PlayerMove@8");
      if (pfnPMove) {
        g_HUD_PlayerMove_Addr = (DWORD)pfnPMove;
        LogDebug("[Hooks] Found HUD_PlayerMove at 0x%X\n", g_HUD_PlayerMove_Addr);
        DWORD old;
        VirtualProtect((void *)g_HUD_PlayerMove_Addr, 6, PAGE_EXECUTE_READWRITE, &old);
        memcpy(g_Orig_HUD_PlayerMove, (void *)g_HUD_PlayerMove_Addr, 6);
      }
    }
  }
  if (g_HUD_PlayerMove_Addr) {
    DWORD old;
    VirtualProtect((void *)g_HUD_PlayerMove_Addr, 6, PAGE_EXECUTE_READWRITE, &old);
    BYTE patch[5] = {0xE9, 0, 0, 0, 0};
    *(DWORD *)(patch + 1) =
        (DWORD)Hook_HUD_PlayerMove - (DWORD)g_HUD_PlayerMove_Addr - 5;
    memcpy((void *)g_HUD_PlayerMove_Addr, patch, 5);
    *(BYTE *)((DWORD)g_HUD_PlayerMove_Addr + 5) = 0x90;
    LogDebug("[Hooks] HUD_PlayerMove patched (0x%X)\n", g_HUD_PlayerMove_Addr);
  }

  // (Command registration moved to MainThread)


  g_HooksActive = true;
}

void RemoveHooks() {
  if (!g_HooksActive)
    return;

  if (g_SCR)
    Restore(g_SCR, g_Orig_SCR, 6);
  if (g_CreateInterface)
    Restore(g_CreateInterface, g_Orig_CI, 5);
  if (g_SteamInternal)
    Restore(g_SteamInternal, g_Orig_SI, 5);
  if (g_GetVolumeInfo)
    Restore(g_GetVolumeInfo, g_Orig_Vol, 5);
  if (g_pfnConPrintf)
    Restore((DWORD)g_pfnConPrintf, g_Orig_ConPrintf, 6);

  // Restore Core Sprite Engine functions
  // Restore Core Sprite Engine functions
  if (g_EngineTable) {
    DWORD old;
    if (VirtualProtect(g_EngineTable, 100 * 4, PAGE_READWRITE, &old)) {
      if (g_Original_SPR_Set)
        g_EngineTable[4] = (void *)g_Original_SPR_Set;
      if (g_Original_SPR_Draw)
        g_EngineTable[5] = (void *)g_Original_SPR_Draw;
      if (g_Original_SPR_DrawHoles)
        g_EngineTable[6] = (void *)g_Original_SPR_DrawHoles;
      if (g_Original_SPR_DrawAdditive)
        g_EngineTable[7] = (void *)g_Original_SPR_DrawAdditive;
      VirtualProtect(g_EngineTable, 100 * 4, old, &old);
      LogDebug("[NoSmoke] Core Sprite functions Restored\n");
    }
  }

  if (g_pEfxAPI) {
    void **efxTable = (void **)g_pEfxAPI;
    DWORD oldEfx;
    if (VirtualProtect(efxTable, 100 * 4, PAGE_READWRITE, &oldEfx)) {
      if (g_Original_R_TempSprite)
        efxTable[50] = (void *)g_Original_R_TempSprite;
      if (g_Original_R_Sprite_Explosion)
        efxTable[49] = (void *)g_Original_R_Sprite_Explosion;
      if (g_Original_R_ParticleExplosion)
        efxTable[55] = (void *)g_Original_R_ParticleExplosion;
      if (g_Original_R_RocketTrail)
        efxTable[58] = (void *)g_Original_R_RocketTrail;
      VirtualProtect(efxTable, 100 * 4, oldEfx, &oldEfx);
    }
    LogDebug("[NoSmoke] Restored EfxAPI table hooks\n");
  }

  // Restore HUD_PlayerMove
  if (g_HUD_PlayerMove_Addr) {
    Restore(g_HUD_PlayerMove_Addr, g_Orig_HUD_PlayerMove, 6);
  }

  // Restore CL_CreateMove (Strafe Boost)
  if (g_CL_CreateMove_Addr) {
    Restore(g_CL_CreateMove_Addr, g_Orig_CL_CreateMove, 6);
  }

  // Reset strafe boost state
  g_StrafeBoostActive = false;

  // Restore DrawEngine CALL offset (CRITICAL - was missing before!)
  if (g_HUD_Redraw_Addr && g_Orig_HUD_Redraw[0] != 0) {
    Restore(g_HUD_Redraw_Addr, g_Orig_HUD_Redraw, 6);
  }

  if (g_DrawEngineAddr) {
    DWORD old;
    VirtualProtect((void *)(g_DrawEngineAddr + 1), 4, PAGE_EXECUTE_READWRITE,
                   &old);
    memcpy((void *)(g_DrawEngineAddr + 1), g_Orig_DrawEngine_CallOffset, 4);
    VirtualProtect((void *)(g_DrawEngineAddr + 1), 4, old, &old);
  }

  // Restore UserMsg Hooks
  if (g_pfnHookUserMsg) {
    if (g_Original_SayText)
      g_pfnHookUserMsg((char *)"SayText", g_Original_SayText);
    if (g_Original_ScreenFade)
      g_pfnHookUserMsg((char *)"ScreenFade", g_Original_ScreenFade);
    if (g_Original_CurWeapon)
      g_pfnHookUserMsg((char *)"CurWeapon", g_Original_CurWeapon);
    if (g_Original_SetFOV)
      g_pfnHookUserMsg((char *)"SetFOV", g_Original_SetFOV);
    if (g_Original_HideWeapon) {
      // Restore standard HUD elements before unhooking
      g_Original_HideWeapon("HideWeapon", 1, &g_ServerHideWeaponFlags);
      g_pfnHookUserMsg((char *)"HideWeapon", g_Original_HideWeapon);
    }
  }

  // Restore HUD_AddEntity
  if (g_HUD_AddEntity_Addr) {
    Restore(g_HUD_AddEntity_Addr, g_Orig_HUD_AddEntity, 6);
  }

  // Restore r_drawviewmodel to visible
  if (g_pCvar_drawviewmodel) {
    g_pCvar_drawviewmodel->value = 1.0f;
  }

  PatchByte(g_HwBase + OFF_CMD_PATCH, 0x74);

  // Restore StudioDrawPlayer (Glow ESP)
  if (g_pStudioInterface && g_Original_StudioDrawPlayer) {
    DWORD old;
    VirtualProtect(&g_pStudioInterface[2], 4, PAGE_READWRITE, &old);
    g_pStudioInterface[2] = (void *)g_Original_StudioDrawPlayer;
    VirtualProtect(&g_pStudioInterface[2], 4, old, &old);
    g_GlowESP_Ready = false;
    LogDebug("[Glow] StudioDrawPlayer restored\n");
  }

  // Restore smoke creation function (no-smoke)
  if (g_SmokeFuncAddr && g_SmokeFuncOrigByte) {
    DWORD old;
    VirtualProtect((void *)g_SmokeFuncAddr, 1, PAGE_EXECUTE_READWRITE, &old);
    *(BYTE *)g_SmokeFuncAddr = g_SmokeFuncOrigByte;
    VirtualProtect((void *)g_SmokeFuncAddr, 1, old, &old);
    LogDebug("[NoSmoke] Smoke function restored\n");
  }


  g_HooksActive = false;
}

bool g_SafePaused = false;
int g_SkipVal_Backup = 0;

// Freeze all game threads (safe patching)
void SuspendOtherThreads() {
  DWORD myTid = GetCurrentThreadId();
  DWORD pid = GetCurrentProcessId();
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap == INVALID_HANDLE_VALUE)
    return;
  THREADENTRY32 te;
  te.dwSize = sizeof(te);
  if (Thread32First(snap, &te)) {
    do {
      if (te.th32OwnerProcessID == pid && te.th32ThreadID != myTid) {
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE,
                              te.th32ThreadID);
        if (h) {
          SuspendThread(h);
          // Force full suspension by reading context
          CONTEXT ctx;
          ctx.ContextFlags = CONTEXT_FULL;
          GetThreadContext(h, &ctx);
          CloseHandle(h);
        }
      }
    } while (Thread32Next(snap, &te));
  }
  CloseHandle(snap);
}

// Thaw all game threads
void ResumeOtherThreads() {
  DWORD myTid = GetCurrentThreadId();
  DWORD pid = GetCurrentProcessId();
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap == INVALID_HANDLE_VALUE)
    return;
  THREADENTRY32 te;
  te.dwSize = sizeof(te);
  if (Thread32First(snap, &te)) {
    do {
      if (te.th32OwnerProcessID == pid && te.th32ThreadID != myTid) {
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
        if (h) {
          ResumeThread(h);
          CloseHandle(h);
        }
      }
    } while (Thread32Next(snap, &te));
  }
  CloseHandle(snap);
}

void ToggleStealth() {
  // FREEZE game -> Patch -> THAW game (Crash-Safe + Undetected)
  SuspendOtherThreads();

  if (g_HooksActive) {
    RemoveHooks();
    g_SafePaused = true;
  } else {
    InstallHooks();
    g_SafePaused = false;
  }

  ResumeOtherThreads();

  // Stop bot after thaw (uses ClientCmd which needs engine running)
  if (g_SafePaused)
    SayiBilen_Stop();
}

// -------------------------------------------------------------
// MAIN THREAD
// -------------------------------------------------------------
void FindAndHookSteamUser() {
  if (!g_CreateInterface && !g_SteamInternal)
    return;

  CreateInterface_t factory =
      (CreateInterface_t)(g_CreateInterface ? g_CreateInterface
                                            : g_SteamInternal);

  const char *versions[] = {"SteamUser023", "SteamUser022", "SteamUser021",
                            "SteamUser020", "SteamUser019", "SteamUser018",
                            "SteamUser017", "SteamUser016", "SteamUser015",
                            "SteamUser014", "SteamUser013", "SteamUser012",
                            "SteamUser010", "SteamUser009", "SteamUser003"};

  for (const char *ver : versions) {
    int err = 0;
    void *iface = factory(ver, &err);
    if (iface) {
      ApplyVTableHooks(iface, ver);
    }
  }
}

// SCANNING HELPER REMOVED (Duplicate)
DWORD WINAPI MainThread(LPVOID) {
  while (!(g_HwBase = (DWORD)GetModuleHandleA("hw.dll")))
    Sleep(100);
  g_SCR = g_HwBase + OFF_SCR_UPDATE;
  g_JmpTarget = g_SCR + 6;

  FindEngineFunctions();

  if (g_pfnClientCmd) {
    SayiBilen_Init();
  }

  HMODULE hKernel = GetModuleHandleA("kernel32.dll");

  HMODULE hSteam = NULL;
  for (int i = 0; i < 5; i++) {
    hSteam = GetModuleHandleA("steam_api.dll");
    if (hSteam)
      break;
    Sleep(100);
  }

  if (hSteam) {
    CreateInterface_t fnSteamUser =
        (CreateInterface_t)GetProcAddress(hSteam, "SteamUser");
    if (fnSteamUser) {
      typedef void *(*SteamUserFn)();
      SteamUserFn fn = (SteamUserFn)fnSteamUser;
      void *iface = fn();
      if (iface)
        ApplyVTableHooks(iface, "SteamUser");
    }
  }

  PatchByte(g_HwBase + OFF_CMD_PATCH, 0xEB);
  g_RegisterCvar = (Cvar_RegisterVariable_t)(g_HwBase + OFF_CVAR_REG);
  if (g_RegisterCvar) {
    g_RegisterCvar(&g_cvar_frame_skip);
    g_RegisterCvar(&g_cvar_change_id);
    g_RegisterCvar(&g_cvar_sb);
    g_RegisterCvar(&g_cvar_sb_delay);
    g_RegisterCvar(&g_cvar_sb_range);
    g_RegisterCvar(&g_cvar_ct_esp);
    g_RegisterCvar(&g_cvar_t_esp);
    g_RegisterCvar(&g_cvar_esp_type);
    g_RegisterCvar(&g_cvar_esp_label);
    g_RegisterCvar(&g_cvar_ch);
    g_RegisterCvar(&g_cvar_ch_color);
    g_RegisterCvar(&g_cvar_ch_length);
    g_RegisterCvar(&g_cvar_ch_offset);
    g_RegisterCvar(&g_cvar_ch_thickness);
    g_RegisterCvar(&g_cvar_ch_import);
     g_RegisterCvar(&g_cvar_ch_export);
     g_RegisterCvar(&g_cvar_no_smoke);
     g_RegisterCvar(&g_cvar_speedometer);
     g_RegisterCvar(&g_cvar_speedometer_color);
     g_RegisterCvar(&g_cvar_hide_knife);
     g_RegisterCvar(&g_cvar_hide_entities);
     g_RegisterCvar(&g_cvar_showfps);
     g_RegisterCvar(&g_cvar_anti_drug);
     g_RegisterCvar(&g_cvar_strafe_helper);
     g_RegisterCvar(&g_cvar_sgs);
     g_RegisterCvar(&g_cvar_cl_antiss);
     g_RegisterCvar(&g_cvar_null_canceling_movement);
     g_RegisterCvar(&g_cvar_qs);
     g_RegisterCvar(&g_cvar_no_scope);
   }
 
   // Register +strafe_boost / -strafe_boost commands
   if (g_pfnAddCommand) {
     g_pfnAddCommand((char *)"sf_help", Cmd_ShowHelp);
     g_pfnAddCommand((char *)"+strafe_boost", Cmd_StrafeBoost_On);
    g_pfnAddCommand((char *)"-strafe_boost", Cmd_StrafeBoost_Off);
    g_pfnAddCommand((char *)"+auto_bhop", Cmd_AutoBhop_On);
    g_pfnAddCommand((char *)"-auto_bhop", Cmd_AutoBhop_Off);
    g_pfnAddCommand((char *)"+sgs", Cmd_SGS_On);
    g_pfnAddCommand((char *)"-sgs", Cmd_SGS_Off);
    g_pfnAddCommand((char *)"+strafe_helper", Cmd_StrafeHelper_On);
    g_pfnAddCommand((char *)"-strafe_helper", Cmd_StrafeHelper_Off);
    LogDebug("[Commands] +strafe_boost, +auto_bhop, +sgs, and -sgs registered!\n");
  }

  InstallHooks();

  if (g_CreateInterface) {
    FindAndHookSteamUser();
  }

  while (true) {
    if (GetAsyncKeyState(VK_F11) & 1)
      ToggleStealth();

    if (g_SafePaused) {
      Sleep(100);
      continue;
    }

    if (g_HooksActive) {
      // Sync no_smoke cvar to global bool
      if (g_cvar_no_smoke.string) {
        g_NoSmoke = (atoi(g_cvar_no_smoke.string) > 0);
      }

      if (g_cvar_frame_skip.string) {
        g_SkipVal = atoi(g_cvar_frame_skip.string);
      }

      // Handle Crosshair Export
      static char lastExport[64] = "";
      if (g_cvar_ch_export.string && g_cvar_ch_export.string[0] && strcmp(lastExport, g_cvar_ch_export.string) != 0) {
        strcpy(lastExport, g_cvar_ch_export.string);
        char path[256];
        sprintf(path, "cstrike/%s.txt", g_cvar_ch_export.string);
        FILE *f = fopen(path, "w");
        if (f) {
          fprintf(f, "%s\n%s\n%s\n%s\n", 
            g_cvar_ch_color.string ? g_cvar_ch_color.string : "0 255 0",
            g_cvar_ch_length.string ? g_cvar_ch_length.string : "5",
            g_cvar_ch_offset.string ? g_cvar_ch_offset.string : "5",
            g_cvar_ch_thickness.string ? g_cvar_ch_thickness.string : "2");
          fclose(f);
          if (g_pfnConPrintf) {
            ((void (*)(const char *, ...))g_pfnConPrintf)("[SF] Crosshair exported to %s\n", path);
          }
        }
      }

      // Handle Crosshair Import
      static char lastImport[64] = "";
      if (g_cvar_ch_import.string && g_cvar_ch_import.string[0] && strcmp(lastImport, g_cvar_ch_import.string) != 0) {
        strcpy(lastImport, g_cvar_ch_import.string);
        char path[256];
        sprintf(path, "cstrike/%s.txt", g_cvar_ch_import.string);
        FILE *f = fopen(path, "r");
        if (f) {
          char col[64]={0}, len[64]={0}, off[64]={0}, thk[64]={0};
          if (fgets(col, 64, f) && fgets(len, 64, f) && fgets(off, 64, f) && fgets(thk, 64, f)) {
            col[strcspn(col, "\r\n")] = 0;
            len[strcspn(len, "\r\n")] = 0;
            off[strcspn(off, "\r\n")] = 0;
            thk[strcspn(thk, "\r\n")] = 0;
            
            char cmd[512];
            sprintf(cmd, "ch_color \"%s\"; ch_length \"%s\"; ch_offset \"%s\"; ch_thickness \"%s\"\n", col, len, off, thk);
            if (g_pfnClientCmd) {
              g_pfnClientCmd(cmd);
              if (g_pfnConPrintf) {
                ((void (*)(const char *, ...))g_pfnConPrintf)("[SF] Crosshair imported from %s\n", path);
              }
            }
          }
          fclose(f);
        } else {
          if (g_pfnConPrintf) {
            ((void (*)(const char *, ...))g_pfnConPrintf)("[SF] Failed to load %s\n", path);
          }
        }
      }

      // Update delay dynamically
      if (g_cvar_sb_delay.string) {
        int d = atoi(g_cvar_sb_delay.string);
        if (d >= 0)
          g_SayiBilenDelay = d;
      }

      if (g_cvar_sb.string) {
        static char lastVal[64] = "";
        if (strcmp(lastVal, g_cvar_sb.string) != 0) {
          strcpy(lastVal, g_cvar_sb.string);
          int mn, mx;
          int count = sscanf(lastVal, "%d %d", &mn, &mx);

          if (count == 2) {
            if (mn >= 0 && mx >= 0)
              SayiBilen_Start(mn, mx);
          } else if (count == 1) {
            if (mn == 0)
              SayiBilen_Stop();
            else if (mn == 1) {
              // "sb 1" means START using sb_range
              int r = 100;
              if (g_cvar_sb_range.string)
                r = atoi(g_cvar_sb_range.string);
              if (r < 1)
                r = 100;
              SayiBilen_Start(0, r);
            } else if (mn > 1) {
              // "sb 500" means custom max
              SayiBilen_Start(0, mn);
            }
          } else {
            SayiBilen_Stop();
          }
        }
      }
    }
    Sleep(50);
  }
  return 0;
}

// (Moved to top)

BOOL APIENTRY DllMain(HMODULE h, DWORD r, LPVOID) {
  if (r == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(h);
    CreateThread(0, 0, MainThread, 0, 0, 0);
  }
  return TRUE;
}