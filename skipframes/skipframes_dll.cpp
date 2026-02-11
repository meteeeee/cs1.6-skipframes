#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>
#include <tlhelp32.h>
#include "sayi_bilen.h"

// =============================================================
// SKIPFRAMES + VALVE_ID FORCE (RELEASE BUILD)
// =============================================================

#define OFF_CVAR_REG    0x2E960
#define OFF_CMD_PATCH   0x2809D
#define OFF_SCR_UPDATE  0x4C2A0

DWORD g_HwBase = 0;
DWORD g_ClientBase = 0;  // client.dll base address
DWORD g_SCR = 0;
DWORD g_CreateInterface = 0;
DWORD g_SteamInternal = 0;
DWORD g_GetVolumeInfo = 0;
DWORD g_JmpTarget = 0;

// [NEW] Engine Functions
typedef void (*ClientCmd_t)(const char*);
ClientCmd_t g_pfnClientCmd = nullptr;

bool g_HooksActive = false;
bool g_StealthMode = false;

// STORAGE
BYTE g_Orig_SCR[6];
BYTE g_Orig_CI[5];
BYTE g_Orig_SI[5];
BYTE g_Orig_Vol[5];

// LOGGING (OutputDebugStringA)
void Log(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf)-1, fmt, ap);
    va_end(ap);
    buf[sizeof(buf)-1] = 0;
    OutputDebugStringA(buf);
}

void LogDebug(const char* fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf)-1, fmt, ap);
    va_end(ap);
    buf[sizeof(buf)-1] = 0;
    OutputDebugStringA(buf);
}

// -------------------------------------------------------------
// CVARS
// -------------------------------------------------------------
typedef struct cvar_s {
    const char* name;
    const char* string;
    int flags;
    float value;
    struct cvar_s* next;
} cvar_t;

typedef void (*Cvar_RegisterVariable_t)(cvar_t* variable);
Cvar_RegisterVariable_t g_RegisterCvar = nullptr;

cvar_t g_cvar_frame_skip = { "frame_skip", "0", 0, 0.0f, nullptr };
cvar_t g_cvar_change_id  = { "change_id", "0", 0, 0.0f, nullptr };
cvar_t g_cvar_sb         = { "sb", "0", 0, 0.0f, nullptr };
cvar_t g_cvar_sb_delay   = { "sb_delay", "5", 0, 0.0f, nullptr };
cvar_t g_cvar_sb_range   = { "sb_range", "100", 0, 0.0f, nullptr };

// -------------------------------------------------------------
// PATTERN SCANNING
// -------------------------------------------------------------
bool Compare(const char* pData, const char* pPattern, const char* pMask) {
    for (; *pMask; ++pMask, ++pData, ++pPattern) {
        if (*pMask == 'x' && *pData != *pPattern) return false;
    }
    return (*pMask) == 0;
}

DWORD FindPattern(DWORD start, DWORD size, const char* pattern, const char* mask) {
    for (DWORD i = 0; i < size; i++) {
        if (Compare((char*)(start + i), pattern, mask)) {
            return start + i;
        }
    }
    return 0;
}

// Helper: Find string in memory (with null terminator check)
DWORD FindStringRef(DWORD start, DWORD size, const char* str) {
    int len = strlen(str);
    DWORD strAddr = 0;
    LogDebug("[FindStringRef] Searching for '%s' (len=%d) in range 0x%X - 0x%X\n", str, len, start, start+size);
    
    // 1. Find Data String (MUST include null terminator to avoid partial matches like SayText2)
    for(DWORD i=0; i < size - len; i++) {
        // Optim: Check readability every 4KB page
        if ((i % 4096) == 0) {
           if (IsBadReadPtr((void*)(start+i), 4096)) {
               i += 4095; 
               continue;
           }
        }
        
        // Fast scan for first char
        if (*(char*)(start+i) != str[0]) continue;
        
        // Check full string + NULL terminator
        if (memcmp((void*)(start+i), str, len) == 0 && *(char*)(start+i+len) == 0) {
            strAddr = start + i;
            LogDebug("[FindStringRef] Found string '%s\\0' at 0x%X\n", str, strAddr);
            break;
        }
    }
    
    if(!strAddr) {
        LogDebug("[FindStringRef] String '%s' NOT FOUND!\n", str);
        return 0;
    }
    
    // 2. Find PUSH strAddr (68 XX XX XX XX)
    LogDebug("[FindStringRef] Looking for PUSH 0x%X in code section...\n", strAddr);
    
    // Scan smaller range for code (usually .text is first) or just scan all
    // To be safe, we re-check readability
    for(DWORD i=0; i < size - 5; i++) {
        if ((i % 4096) == 0) {
           if (IsBadReadPtr((void*)(start+i), 4096)) {
               i += 4095; 
               continue;
           }
        }
        
        if(*(BYTE*)(start+i) == 0x68) {
            if(*(DWORD*)(start+i+1) == strAddr) {
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
typedef int (__fastcall *InitiateGameConnection_t)(void*, int, void*, int, uint64_t, uint32_t, uint16_t, bool);
typedef int (__cdecl *pfnUserMsgHook)(const char *pszName, int iSize, void *pbuf);
typedef int (__cdecl *pfnHookUserMsg_t)(char *szMsgName, pfnUserMsgHook pfn);
pfnHookUserMsg_t g_pfnHookUserMsg = nullptr;

// UserMsg Handlers
int __cdecl MsgFunc_SayText(const char *pszName, int iSize, void *pbuf);
int __cdecl MsgFunc_ScreenFade(const char *pszName, int iSize, void *pbuf);
InitiateGameConnection_t g_Original_Initiate = nullptr;
// -------------------------------------------------------------
// ENGINE TABLE
// -------------------------------------------------------------
typedef struct cl_enginefuncs_s {
    // 0-5
    void* pfnSPR_Load;
    void* pfnSPR_Frames;
    void* pfnSPR_Height;
    void* pfnSPR_Width;
    void* pfnSPR_Set;
    void* pfnSPR_Draw;
    // 6
    void* pfnSPR_DrawHoles;
    void* pfnDrawGeneric; // Index 7? Wait, ClientCmd is 6?
    // Let's use void* array for safety and cast later
    // Standard SDK: ClientCmd is index 6?
    // Let's define it as array of void* 
} cl_enginefuncs_t;

// We need the offsets.
// Index 6: pfnClientCmd
// Index 34: pfnHookUserMsg
// Index 40: pfnGetLocalPlayer (approx)
// We will use a raw pointer table.
void** g_peengfuncs = nullptr; 

// Helpers to call engine functions
typedef void (*DrawSetTextColor_t)(float r, float g, float b);
typedef int (*DrawConsoleString_t)(int x, int y, const char *string);
typedef void* (*GetLocalPlayer_t)();

// -------------------------------------------------------------
// HOOK TYPEDEFS
// -------------------------------------------------------------
// Helper to check if buffer contains string safely
bool MemContains(void* ptr, int size, const char* str) {
    if (!ptr || IsBadReadPtr(ptr, size)) return false;
    int len = strlen(str);
    for(int i=0; i<size-len; i++) {
        if (memcmp((char*)ptr+i, str, len) == 0) return true;
    }
    return false;
}

// [NEW] UserMsg Hook (Blind Dump + Keyword Scan)
typedef void (*UserMsg_t)(void*, void*, void*);
UserMsg_t g_pfnUserMsg = nullptr;
BYTE g_Orig_UserMsg[10];

// Helper to normalize string (düşük -> dusuk) for keyword matching
// Handles ANSI (CP1254) and UTF-8
void QuickNormalize(char* str) {
    if(!str) return;
    int w = 0; // Write index
    for(int r=0; str[r]; r++) {
        unsigned char c = (unsigned char)str[r];
        unsigned char next = (unsigned char)str[r+1];
        
        // UTF-8 Handling
        if(c == 0xC3) { // UTF-8 Block 1
            if(next == 0xBC || next == 0x9C) str[w++] = 'u'; // ü, Ü
            else if(next == 0xA7 || next == 0x87) str[w++] = 'c'; // ç, Ç
            else if(next == 0xB6 || next == 0x96) str[w++] = 'o'; // ö, Ö
            else str[w++] = '?';
            r++; // Skip next
        }
        else if(c == 0xC4) { // UTF-8 Block 2
            if(next == 0xB1 || next == 0xB0) str[w++] = 'i'; // ı, İ (dotless i used as i)
            else if(next == 0x9F || next == 0x9E) str[w++] = 'g'; // ğ, Ğ
            else str[w++] = '?';
            r++; // Skip next
        }
        else if(c == 0xC5) { // UTF-8 Block 3
            if(next == 0x9F || next == 0x9E) str[w++] = 's'; // ş, Ş
            else str[w++] = '?';
            r++; // Skip next
        }
        // ANSI / Standard Handling
        else if(c >= 'A' && c <= 'Z') str[w++] = c + ('a' - 'A'); // Lowercase
        else if(c == 0xDC || c == 0xFC) str[w++] = 'u'; // Ü, ü
        else if(c == 0xC7 || c == 0xE7) str[w++] = 'c'; // Ç, ç
        else if(c == 0xD6 || c == 0xF6) str[w++] = 'o'; // Ö, ö
        else if(c == 0xDE || c == 0xFE) str[w++] = 's'; // Ş, ş
        else if(c == 0xD0 || c == 0xF0) str[w++] = 'g'; // Ğ, ğ
        else if(c == 0xDD || c == 0xFD) str[w++] = 'i'; // İ, ı
        else str[w++] = (char)c;
    }
    str[w] = 0; // Null terminate new length
}

// [NEW] Con_Printf Hook
typedef void (*ConPrintf_t)(const char* fmt, ...);
ConPrintf_t g_pfnConPrintf = nullptr;
BYTE g_Orig_ConPrintf[10];

void Hook_ConPrintf(const char* fmt, void* a1, void* a2, void* a3) {
    // 1. Unhook
    DWORD old;
    VirtualProtect((void*)g_pfnConPrintf, 6, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void*)g_pfnConPrintf, g_Orig_ConPrintf, 6);
    VirtualProtect((void*)g_pfnConPrintf, 6, old, &old);

    // 2. Copy fmt to temp buffer for analysis
    char temp[512] = {0};
    if (fmt && !IsBadReadPtr((void*)fmt, 1)) {
        int j=0;
        while(j<510 && !IsBadReadPtr((void*)(fmt+j), 1) && fmt[j] != 0) {
            temp[j] = fmt[j];
            j++;
        }
        temp[j] = 0;
    }
    
    int len = strlen(temp);
    
    // 3. Log "interesting" messages (>30 chars or contains player brackets)
    if (len > 30 || strstr(temp, " : ")) {
        LogDebug("[ConPrintf] INTERESTING: '%s'\n", temp);
    }
    
    // 4. Normalize and check keywords
    char normalized[512];
    strcpy(normalized, temp);
    QuickNormalize(normalized);
    
    if (strstr(normalized, "yuksek") || strstr(normalized, "dusuk") || strstr(normalized, "sayitest") ||
        strstr(normalized, "asagi") || strstr(normalized, "yukari") || strstr(normalized, "kucuk") || strstr(normalized, "buyuk")) {
        
        LogDebug("[ConPrintf HIT!] Original: '%s' Normalized: '%s'\n", temp, normalized);
        // RE-ENABLED: ConPrintf fallback since UserMsg hook isn't being called
        SayiBilen_OnMessage(fmt); 
    }

    // 5. Call Original
    ((void (*)(const char*,...))g_pfnConPrintf)(fmt, a1, a2, a3, 0,0,0,0,0);

    // 6. Rehook
    BYTE patch[5] = {0xE9, 0,0,0,0};
    *(DWORD*)(patch+1) = (DWORD)Hook_ConPrintf - (DWORD)g_pfnConPrintf - 5;
    VirtualProtect((void*)g_pfnConPrintf, 6, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void*)g_pfnConPrintf, patch, 5);
    *(BYTE*)((DWORD)g_pfnConPrintf+5) = 0x90; 
    VirtualProtect((void*)g_pfnConPrintf, 6, old, &old);
}

// -------------------------------------------------------------
// USERMSG HANDLERS
// -------------------------------------------------------------
pfnUserMsgHook g_Original_SayText = nullptr;
pfnUserMsgHook g_Original_ScreenFade = nullptr;

bool g_NoFlash = true; // Use simple bool for now

int __cdecl MsgFunc_SayText(const char *pszName, int iSize, void *pbuf) {
    LogDebug("[MsgFunc_SayText] CALLED! Name=%s Size=%d\n", pszName ? pszName : "NULL", iSize);
    
    if (iSize > 2 && pbuf) {
        // Simple heuristic: The chat message is usually at the end of the buffer
        // standard SayText: [byte client] [string msg] ...
        // We will scan the buffer for printable strings
        char* pData = (char*)pbuf;
        for(int i=0; i < iSize-1; i++) {
             if (pData[i] >= 32 && pData[i] < 127) {
                 // Potential string start
                 int len = strlen(&pData[i]);
                 if (len > 3 && len < iSize - i + 1) {
                     char temp[256];
                     strncpy(temp, &pData[i], 255);
                     temp[255] = 0;
                     
                     LogDebug("[MsgFunc_SayText] Found string: '%s'\n", temp);
                     
                     // Send to SayiBilen
                     SayiBilen_OnMessage(temp); // This is INSTANT!
                     
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
             char* pData = (char*)pbuf;
             // Skip client byte
             ((void (*)(const char*,...))g_pfnConPrintf)("[ConsoleLog] Chat: '%s'\n", pData+1);
        }
    }
    return 0;
}

int __cdecl MsgFunc_ScreenFade(const char *pszName, int iSize, void *pbuf) {
    LogDebug("[ScreenFade] Blocked Flash!\n"); // DEBUG LOG
    if (g_NoFlash) {
        return 0; // BLOCK IT!
    }
    if (g_Original_ScreenFade) {
        return g_Original_ScreenFade(pszName, iSize, pbuf);
    }
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
        LogDebug("[ProScanner] ERROR: Initialize export not found in client.dll!\n");
        return;
    }
    LogDebug("[ProScanner] Found Initialize at 0x%X\n", (DWORD)pInitialize);

    // Step 2: Scan Initialize prologue for the pattern that stores pEnginefuncs
    // In Build 8684, Initialize uses REP MOVSD (memcpy) to copy the table:
    //   MOV ESI, [ESP+X]     ; source = pEnginefuncs parameter
    //   MOV EDI, 0xXXXXXXXX  ; destination = global buffer (BF XX XX XX XX)
    //   REP MOVSD             ; memcpy (F3 A5)
    BYTE* pCode = (BYTE*)pInitialize;
    DWORD engTableDirect = 0;  // Direct table address (memcpy destination)
    bool isMemcpy = false;
    
    // Dump first 48 bytes on one line for debugging
    char hexDump[256] = {0};
    int pos = 0;
    for (int i = 0; i < 48 && !IsBadReadPtr(pCode+i, 1); i++) {
        pos += sprintf(hexDump + pos, "%02X ", pCode[i]);
    }
    LogDebug("[ProScanner] Initialize bytes: %s\n", hexDump);
    
    // PRIORITY 1: Look for BF XX XX XX XX followed by F3 A5 (MOV EDI, imm32 + REP MOVSD)
    // This is how Build 8684 copies the engine table via memcpy!
    for (int i = 0; i < 128; i++) {
        if (IsBadReadPtr(pCode + i, 7)) break;
        
        if (pCode[i] == 0xBF) {  // MOV EDI, imm32
            DWORD candidate = *(DWORD*)(pCode + i + 1);
            // Check if F3 A5 (REP MOVSD) follows within next 4 bytes
            for (int j = 5; j < 8; j++) {
                if (pCode[i+j] == 0xF3 && pCode[i+j+1] == 0xA5) {
                    // FOUND IT! EDI = destination = the global engine table
                    if (candidate > 0x10000 && !IsBadReadPtr((void*)candidate, 540)) {
                        engTableDirect = candidate;
                        isMemcpy = true;
                        LogDebug("[ProScanner] Found MEMCPY pattern! MOV EDI, 0x%X + REP MOVSD at offset +%d\n", candidate, i);
                        break;
                    }
                }
            }
            if (engTableDirect) break;
        }
    }
    
    // PRIORITY 2: Fallback to A3/89 patterns (simple pointer store)
    DWORD engTableGlobal = 0;
    if (!engTableDirect) {
        for (int i = 0; i < 128; i++) {
            if (IsBadReadPtr(pCode + i, 6)) break;
            
            if (pCode[i] == 0xA3) {
                DWORD candidate = *(DWORD*)(pCode + i + 1);
                if (candidate > 0x10000 && !IsBadReadPtr((void*)candidate, 4)) {
                    engTableGlobal = candidate;
                    LogDebug("[ProScanner] Fallback: Found MOV [0x%X], EAX at offset +%d\n", candidate, i);
                    break;
                }
            }
            if (pCode[i] == 0x89 && (pCode[i+1] == 0x05 || pCode[i+1] == 0x0D || pCode[i+1] == 0x15)) {
                DWORD candidate = *(DWORD*)(pCode + i + 2);
                if (candidate > 0x10000 && !IsBadReadPtr((void*)candidate, 4)) {
                    engTableGlobal = candidate;
                    LogDebug("[ProScanner] Fallback: Found MOV [0x%X], reg at offset +%d\n", candidate, i);
                    break;
                }
            }
        }
    }
    
    if (!engTableDirect && !engTableGlobal) {
        LogDebug("[ProScanner] FAILED to find pEnginefuncs storage in Initialize!\n");
        return;
    }
    
    // Step 3: Set up the engine table pointer
    void** engineTable = NULL;
    
    if (engTableDirect) {
        // MEMCPY case: the address IS the table directly (Build 8684)
        engineTable = (void**)engTableDirect;
        LogDebug("[ProScanner] cl_enginefunc_t at 0x%X (direct memcpy destination)\n", engTableDirect);
    } else {
        // Pointer store case: dereference to get table
        DWORD storedValue = *(DWORD*)engTableGlobal;
        if (storedValue > 0x10000 && !IsBadReadPtr((void*)storedValue, 400)) {
            engineTable = (void**)storedValue;
            LogDebug("[ProScanner] cl_enginefunc_t* at 0x%X (ptr at 0x%X)\n", storedValue, engTableGlobal);
        } else {
            engineTable = (void**)engTableGlobal;
            LogDebug("[ProScanner] cl_enginefunc_t at 0x%X (direct global)\n", engTableGlobal);
        }
    }
    
    if (IsBadReadPtr(engineTable, 100 * sizeof(void*))) {
        LogDebug("[ProScanner] ERROR: engineTable at 0x%X not readable!\n", engineTable);
        return;
    }
    
    // Log first 25 entries for debugging
    LogDebug("[ProScanner] Table entries:\n");
    for (int i = 0; i < 25; i++) {
        LogDebug("  [%d] = 0x%X\n", i, (DWORD)engineTable[i]);
    }
    
    // Step 4: Extract functions (indices from Half-Life SDK cdll_int.h)
    g_peengfuncs = engineTable;
    
    g_pfnHookUserMsg = (pfnHookUserMsg_t)engineTable[18];
    LogDebug("[ProScanner] pfnHookUserMsg [18] = 0x%X\n", (DWORD)g_pfnHookUserMsg);
    
    g_pfnClientCmd = (ClientCmd_t)engineTable[20];
    LogDebug("[ProScanner] pfnClientCmd [20] = 0x%X\n", (DWORD)g_pfnClientCmd);
    
    g_RegisterCvar = (Cvar_RegisterVariable_t)engineTable[14];
    LogDebug("[ProScanner] pfnRegisterVariable [14] = 0x%X\n", (DWORD)g_RegisterCvar);
    
    g_pfnConPrintf = (ConPrintf_t)engineTable[40];
    LogDebug("[ProScanner] Con_Printf [40] = 0x%X\n", (DWORD)g_pfnConPrintf);
    
    if (!g_pfnHookUserMsg || IsBadReadPtr((void*)g_pfnHookUserMsg, 4)) {
        LogDebug("[ProScanner] ERROR: pfnHookUserMsg is invalid!\n");
        return;
    }
    
    // Step 5: Find original handlers by scanning CLIENT.DLL code
    // client.dll's init calls: gEngfuncs.pfnHookUserMsg("SayText", __MsgFunc_SayText)
    // In cdecl, 2nd arg (handler) is PUSHed BEFORE 1st arg ("SayText"):
    //   PUSH __MsgFunc_SayText   ; 68 XX XX XX XX
    //   PUSH "SayText"           ; 68 YY YY YY YY  <-- FindStringRef finds this
    //   CALL [gEngfuncs+72]
    // So we find PUSH "SayText", then look backwards for PUSH handler
    
    LogDebug("[ProScanner] Scanning client.dll for original SayText handler...\n");
    DWORD clientBase = (DWORD)hClient;
    DWORD stRef = FindStringRef(clientBase, 0x800000, "SayText");
    if (stRef) {
        LogDebug("[ProScanner] Found PUSH 'SayText' in client.dll at 0x%X\n", stRef);
        // Look backwards for PUSH imm32 (68 XX XX XX XX) - the handler function
        for (int k = 5; k < 40; k++) {
            if (*(BYTE*)(stRef - k) == 0x68) {
                DWORD funcPtr = *(DWORD*)(stRef - k + 1);
                // Must be a valid function pointer within client.dll range
                if (funcPtr > clientBase && funcPtr < clientBase + 0x800000) {
                    if (!IsBadReadPtr((void*)funcPtr, 4)) {
                        g_Original_SayText = (pfnUserMsgHook)funcPtr;
                        LogDebug("[ProScanner] Found original SayText handler at 0x%X (PUSH at 0x%X)\n", funcPtr, stRef - k);
                        break;
                    }
                }
            }
        }
    } else {
        LogDebug("[ProScanner] Could not find 'SayText' reference in client.dll!\n");
    }
    
    // Also find ScreenFade handler the same way
    DWORD sfRef = FindStringRef(clientBase, 0x800000, "ScreenFade");
    if (sfRef) {
        for (int k = 5; k < 40; k++) {
            if (*(BYTE*)(sfRef - k) == 0x68) {
                DWORD funcPtr = *(DWORD*)(sfRef - k + 1);
                if (funcPtr > clientBase && funcPtr < clientBase + 0x800000 && !IsBadReadPtr((void*)funcPtr, 4)) {
                    g_Original_ScreenFade = (pfnUserMsgHook)funcPtr;
                    LogDebug("[ProScanner] Found original ScreenFade handler at 0x%X\n", funcPtr);
                    break;
                }
            }
        }
    }
    
    if (!g_Original_SayText) {
        LogDebug("[ProScanner] WARNING: Could not find original SayText handler! Chat may be invisible.\n");
    }
    
    // Step 6: NOW hook via official API
    LogDebug("[ProScanner] Hooking SayText via official pfnHookUserMsg...\n");
    g_pfnHookUserMsg("SayText", MsgFunc_SayText);
    LogDebug("[ProScanner] SayText hooked! Original handler: 0x%X\n", (DWORD)g_Original_SayText);
    
    // Hook ScreenFade for NoFlash
    g_pfnHookUserMsg("ScreenFade", MsgFunc_ScreenFade);
    LogDebug("[ProScanner] ScreenFade hooked! Original handler: 0x%X\n", (DWORD)g_Original_ScreenFade);
    
    LogDebug("[ProScanner] === ALL DONE! Engine table found! ===\n");
}

int __fastcall Hook_LegacyAuth(void* thisptr, int edx, void* pBlob, int cbMax, uint64_t steamIDGS, uint32_t ip, uint16_t port, bool secure) {
    int val = atoi(g_cvar_change_id.string);
    if (val == 1) {
        memset(pBlob, 0, cbMax);
        return 0; // Blocked
    }
    if (g_Original_Initiate) {
        return g_Original_Initiate(thisptr, edx, pBlob, cbMax, steamIDGS, ip, port, secure);
    }
    return 0; 
}

// -------------------------------------------------------------
// VTABLE HOOK APPLICATION
// -------------------------------------------------------------
void ApplyVTableHooks(void* iface, const char* pName) {
    if (!iface || (DWORD)iface < 0x10000) return;
    
    if (strstr(pName, "SteamUser")) {
        DWORD* obj = (DWORD*)iface;
        DWORD vptr = obj[0];
        if (!IsBadReadPtr((void*)vptr, 4)) {
            DWORD* vtable = (DWORD*)vptr;
            DWORD oldV;
            if(VirtualProtect(vtable, 256, PAGE_EXECUTE_READWRITE, &oldV)) {
                 if(vtable[3] != (DWORD)Hook_LegacyAuth) {
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
typedef void* (*CreateInterface_t)(const char* pName, int* pReturnCode);
void* __cdecl Hook_CreateInterface(const char* pName, int* pReturnCode); // Forward decl

void* __cdecl Hook_CreateInterface(const char* pName, int* pReturnCode) {
    DWORD old;
    VirtualProtect((void*)g_CreateInterface, 5, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void*)g_CreateInterface, g_Orig_CI, 5);
    VirtualProtect((void*)g_CreateInterface, 5, old, &old);

    CreateInterface_t fn = (CreateInterface_t)g_CreateInterface;
    void* iface = fn(pName, pReturnCode);

    VirtualProtect((void*)g_CreateInterface, 5, PAGE_EXECUTE_READWRITE, &old);
    BYTE patch[5] = {0xE9, 0,0,0,0};
    *(DWORD*)(patch+1) = (DWORD)Hook_CreateInterface - g_CreateInterface - 5;
    memcpy((void*)g_CreateInterface, patch, 5);
    VirtualProtect((void*)g_CreateInterface, 5, old, &old);

    if (iface && pName) ApplyVTableHooks(iface, pName);
    return iface;
}

void* __cdecl Hook_SteamInternal(const char* pName, int* pReturnCode) {
    DWORD old;
    VirtualProtect((void*)g_SteamInternal, 5, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void*)g_SteamInternal, g_Orig_SI, 5);
    VirtualProtect((void*)g_SteamInternal, 5, old, &old);

    CreateInterface_t fn = (CreateInterface_t)g_SteamInternal;
    void* iface = fn(pName, pReturnCode);

    VirtualProtect((void*)g_SteamInternal, 5, PAGE_EXECUTE_READWRITE, &old);
    BYTE patch[5] = {0xE9, 0,0,0,0};
    *(DWORD*)(patch+1) = (DWORD)Hook_SteamInternal - g_SteamInternal - 5;
    memcpy((void*)g_SteamInternal, patch, 5);
    VirtualProtect((void*)g_SteamInternal, 5, old, &old);

    if (iface && pName) ApplyVTableHooks(iface, pName);
    return iface;
}

// -------------------------------------------------------------
// HWID HOOK (Dummy/Minimal)
// -------------------------------------------------------------
BOOL WINAPI Hook_GetVolInfo(
    LPCSTR lpRoot, LPSTR lpVolName, DWORD nVolNameSz,
    LPDWORD lpVolSerial, LPDWORD lpMaxComp, LPDWORD lpFlags,
    LPSTR lpFSName, DWORD nFSNameSz) 
{
    if (lpVolName && nVolNameSz > 0) strcpy(lpVolName, "C_DRIVE");
    if (lpVolSerial) *lpVolSerial = 123456; 
    if (lpMaxComp) *lpMaxComp = 255;
    if (lpFlags) *lpFlags = 0;
    if (lpFSName && nFSNameSz > 0) strcpy(lpFSName, "NTFS");
    return TRUE; 
}

// -------------------------------------------------------------
// DRAW ENGINE HOOK (Speedometer)
// -------------------------------------------------------------
typedef int (__cdecl *DrawEngine_t)();
DrawEngine_t g_Original_DrawEngine = nullptr;
DWORD g_DrawEngineAddr = 0;
BYTE g_Orig_DrawEngine_CallOffset[4] = {0}; // Original E8 offset bytes

void DrawSpeedometer() {
    if (!g_peengfuncs) return;
    
    // FUNCTION MAP (HL1 Standard)
    // 26: pfnDrawSetTextColor(r, g, b)
    // 41: pfnGetLocalPlayer() -> cl_entity_t*
    // 48: pfnDrawConsoleString(x, y, str)
    
    void* pfnSetColor = g_peengfuncs[26];
    void* pfnDrawString = g_peengfuncs[48];
    void* pfnGetPlayer = g_peengfuncs[41];
    
    // Safety Checks
    if (IsBadCodePtr((FARPROC)pfnSetColor) || IsBadCodePtr((FARPROC)pfnDrawString)) return;
    
    // Draw Speedometer Text
    DrawSetTextColor_t SetColor = (DrawSetTextColor_t)pfnSetColor;
    DrawConsoleString_t DrawStr = (DrawConsoleString_t)pfnDrawString;
    
    // Set Color (Green/Cyan)
    SetColor(0.0f, 1.0f, 1.0f);
    
    // Get Speed (Placeholder for now)
    char buf[64];
    sprintf(buf, "Speed: ???");
    
    if (!IsBadCodePtr((FARPROC)pfnGetPlayer)) {
        GetLocalPlayer_t GetPlayer = (GetLocalPlayer_t)pfnGetPlayer;
        void* pPlayer = GetPlayer();
        if (pPlayer && !IsBadReadPtr(pPlayer, 0x300)) {
            // Try to find velocity.
            // In vanilla HL, cl_entity_t has curstate at offset ~0x8 or higher.
            // Let's assume origin at 0x200?
            // Actually, we can just print the address of player to debug log once.
            static bool loggedP = false;
            if(!loggedP) {
                LogDebug("[Speedometer] LocalPlayer Struct at 0x%X\n", pPlayer);
                loggedP = true;
            }
            
            // Temporary: Show pointer address on screen to verify
            sprintf(buf, "Player: %X", pPlayer);
        }
    }

    // Draw at specific position (e.g. Center Bottom)
    // Screen resolution? We assume 640x480 minimum.
    // Center ~320, Bottom ~400.
    DrawStr(300, 400, buf);
}

int __cdecl Hook_DrawEngine() {
    // Call Original FIRST (Draws Server HUD behind us)
    int ret = 0;
    
    // Global toggle to hide HUD?
    static bool HideHUD = false; // Set to true to hide server HUD
    
    if (!HideHUD && g_Original_DrawEngine) {
        ret = g_Original_DrawEngine();
    }
    
    // Draw Custom HUD ON TOP
    // DrawSpeedometer(); // DISABLED: Focusing on SayiBilen fixes first!
    
    // [CRITICAL] Run SayiBilen update every frame for INSTANT response!
    SayiBilen_Update();
    
    return ret;
}

DWORD FindEngineDraw() {
    // Pattern from user: 90 E8 ...
    // "\x90\xE8\x00\x00\x00\x00\x85\xC0\x74\x00\xE8", "xx????xxx?x"
    // Wait, FindPattern takes signature and mask.
    const char* sig = "\x90\xE8\x00\x00\x00\x00\x85\xC0\x74\x00\xE8";
    const char* mask = "xx????xxx?x";
    
    DWORD addr = FindPattern(g_HwBase, 0x1200000, sig, mask);
    if(addr) {
        return addr + 1; // +1 to skip NOP? 90 is NOP. E8 is CALL.
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
__asm__ (
    ".intel_syntax noprefix\n"
    ".globl _Hook_SCR_Trampoline\n"
    "_Hook_SCR_Trampoline:\n"
    "pushad\n"
    "pushfd\n"
    "call _SayiBilen_Update\n"  
    "mov eax, [_g_SkipVal]\n"
    "test eax, eax\n"
    "jz .L_run\n"
    "mov ecx, [_g_FrameCount]\n"
    "dec ecx\n"
    "mov [_g_FrameCount], ecx\n"
    "jg .L_skip\n"
    "mov [_g_FrameCount], eax\n"
    ".L_run:\n"
    "popfd\n"
    "popad\n"
    "push ebp\n"
    "mov ebp, esp\n"
    "sub esp, 0x10\n"
    "jmp DWORD PTR [_g_JmpTarget]\n"
    ".L_skip:\n"
    "popfd\n"
    "popad\n"
    "ret\n"
    ".att_syntax\n"
);

// -------------------------------------------------------------
// INSTALL/REMOVE
// -------------------------------------------------------------
void PatchByte(DWORD addr, BYTE val) {
    DWORD old;
    VirtualProtect((void*)addr, 1, PAGE_EXECUTE_READWRITE, &old);
    *(BYTE*)addr = val;
    VirtualProtect((void*)addr, 1, old, &old);
}

void WriteJMP(DWORD from, DWORD to, BYTE* storage, int len) {
    DWORD old;
    VirtualProtect((void*)from, len, PAGE_EXECUTE_READWRITE, &old);
    bool empty=true; for(int i=0;i<len;i++) if(storage[i]) empty=false;
    if(empty) memcpy(storage, (void*)from, len);
    BYTE patch[5] = {0xE9, 0,0,0,0};
    *(DWORD*)(patch+1) = to - from - 5;
    memcpy((void*)from, patch, 5);
    if(len>5) memset((void*)(from+5), 0x90, len-5);
    VirtualProtect((void*)from, len, old, &old);
}

void Restore(DWORD from, BYTE* storage, int len) {
    DWORD old;
    VirtualProtect((void*)from, len, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void*)from, storage, len);
    VirtualProtect((void*)from, len, old, &old);
}

void InstallHooks() {
    if(g_HooksActive) return;
    
    LogDebug("[Hooks] InstallHooks() called.\n");
    
    PatchByte(g_HwBase + OFF_CMD_PATCH, 0xEB);
    
    // Install DrawEngine Hook (cached - only scan once)
    if (!g_DrawEngineAddr) {
        g_DrawEngineAddr = FindEngineDraw();
        if (g_DrawEngineAddr) {
            DWORD offset = *(DWORD*)(g_DrawEngineAddr + 1);
            g_Original_DrawEngine = (DrawEngine_t)(g_DrawEngineAddr + 5 + offset);
            // Save original CALL offset bytes for restore
            memcpy(g_Orig_DrawEngine_CallOffset, (void*)(g_DrawEngineAddr + 1), 4);
            LogDebug("[Hooks] Found DrawEngine Call at 0x%X, Target: 0x%X\n", g_DrawEngineAddr, g_Original_DrawEngine);
        } else {
            LogDebug("[Hooks] FAILED to find DrawEngine pattern.\n");
        }
    }
    if (g_DrawEngineAddr) {
        DWORD newOffset = (DWORD)Hook_DrawEngine - (g_DrawEngineAddr + 5);
        DWORD old;
        VirtualProtect((void*)(g_DrawEngineAddr + 1), 4, PAGE_EXECUTE_READWRITE, &old);
        *(DWORD*)(g_DrawEngineAddr + 1) = newOffset;
        VirtualProtect((void*)(g_DrawEngineAddr + 1), 4, old, &old);
    }

    if(g_SCR) WriteJMP(g_SCR, (DWORD)Hook_SCR_Trampoline, g_Orig_SCR, 6);
    if(g_CreateInterface) WriteJMP(g_CreateInterface, (DWORD)Hook_CreateInterface, g_Orig_CI, 5);
    if(g_SteamInternal) WriteJMP(g_SteamInternal, (DWORD)Hook_SteamInternal, g_Orig_SI, 5);
    if(g_GetVolumeInfo) WriteJMP(g_GetVolumeInfo, (DWORD)Hook_GetVolInfo, g_Orig_Vol, 5);
    
    if(g_pfnConPrintf) {
        LogDebug("[Hooks] Installing ConPrintf hook at 0x%X -> 0x%X\n", (DWORD)g_pfnConPrintf, (DWORD)Hook_ConPrintf);
        WriteJMP((DWORD)g_pfnConPrintf, (DWORD)Hook_ConPrintf, g_Orig_ConPrintf, 6);
    } else {
        LogDebug("[Hooks] WARNING: g_pfnConPrintf is NULL, skipping hook.\n");
    }

    g_HooksActive = true;
}

void RemoveHooks() {
    if(!g_HooksActive) return;

    if(g_SCR) Restore(g_SCR, g_Orig_SCR, 6);
    if(g_CreateInterface) Restore(g_CreateInterface, g_Orig_CI, 5);
    if(g_SteamInternal) Restore(g_SteamInternal, g_Orig_SI, 5);
    if(g_GetVolumeInfo) Restore(g_GetVolumeInfo, g_Orig_Vol, 5);
    if(g_pfnConPrintf) Restore((DWORD)g_pfnConPrintf, g_Orig_ConPrintf, 6);
    
    // Restore DrawEngine CALL offset (CRITICAL - was missing before!)
    if (g_DrawEngineAddr) {
        DWORD old;
        VirtualProtect((void*)(g_DrawEngineAddr + 1), 4, PAGE_EXECUTE_READWRITE, &old);
        memcpy((void*)(g_DrawEngineAddr + 1), g_Orig_DrawEngine_CallOffset, 4);
        VirtualProtect((void*)(g_DrawEngineAddr + 1), 4, old, &old);
    }
    
    // Restore UserMsg Hooks
    if (g_pfnHookUserMsg) {
        if (g_Original_SayText) g_pfnHookUserMsg("SayText", g_Original_SayText);
        if (g_Original_ScreenFade) g_pfnHookUserMsg("ScreenFade", g_Original_ScreenFade);
    }

    PatchByte(g_HwBase + OFF_CMD_PATCH, 0x74);
    g_HooksActive = false;
}

bool g_SafePaused = false;
int g_SkipVal_Backup = 0;

// Freeze all game threads (safe patching)
void SuspendOtherThreads() {
    DWORD myTid = GetCurrentThreadId();
    DWORD pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te;
    te.dwSize = sizeof(te);
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID == pid && te.th32ThreadID != myTid) {
                HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
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
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te;
    te.dwSize = sizeof(te);
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID == pid && te.th32ThreadID != myTid) {
                HANDLE h = OpenThread(THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
                if (h) { ResumeThread(h); CloseHandle(h); }
            }
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
}

void ToggleStealth() {
    // FREEZE game -> Patch -> THAW game (Crash-Safe + Undetected)
    SuspendOtherThreads();
    
    if(g_HooksActive) {
        RemoveHooks();
        g_SafePaused = true;
    } else {
        InstallHooks();
        g_SafePaused = false;
    }
    
    ResumeOtherThreads();
    
    // Stop bot after thaw (uses ClientCmd which needs engine running)
    if (g_SafePaused) SayiBilen_Stop();
}

// -------------------------------------------------------------
// MAIN THREAD
// -------------------------------------------------------------
void FindAndHookSteamUser() {
    if (!g_CreateInterface && !g_SteamInternal) return;

    CreateInterface_t factory = (CreateInterface_t)(g_CreateInterface ? g_CreateInterface : g_SteamInternal);
    
    const char* versions[] = { 
        "SteamUser023", "SteamUser022", "SteamUser021", "SteamUser020",
        "SteamUser019", "SteamUser018", "SteamUser017", "SteamUser016", 
        "SteamUser015", "SteamUser014", "SteamUser013", "SteamUser012",
        "SteamUser010", "SteamUser009", "SteamUser003" 
    };

    for(const char* ver : versions) {
        int err = 0;
        void* iface = factory(ver, &err);
        if (iface) {
            ApplyVTableHooks(iface, ver);
        }
    }
}

// SCANNING HELPER REMOVED (Duplicate)
DWORD WINAPI MainThread(LPVOID) {
    while(!(g_HwBase = (DWORD)GetModuleHandleA("hw.dll"))) Sleep(100);
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
        if (hSteam) break;
        Sleep(100);
    }
    
    if(hSteam) {
        CreateInterface_t fnSteamUser = (CreateInterface_t)GetProcAddress(hSteam, "SteamUser");
        if (fnSteamUser) {
             typedef void* (*SteamUserFn)();
             SteamUserFn fn = (SteamUserFn)fnSteamUser;
             void* iface = fn();
             if (iface) ApplyVTableHooks(iface, "SteamUser");
        }
    }

    PatchByte(g_HwBase + OFF_CMD_PATCH, 0xEB);
    g_RegisterCvar = (Cvar_RegisterVariable_t)(g_HwBase + OFF_CVAR_REG);
    if(g_RegisterCvar) {
        g_RegisterCvar(&g_cvar_frame_skip);
        g_RegisterCvar(&g_cvar_change_id);
        g_RegisterCvar(&g_cvar_sb); 
        g_RegisterCvar(&g_cvar_sb_delay);
        g_RegisterCvar(&g_cvar_sb_range);
    }

    InstallHooks();

    if(g_CreateInterface) {
        FindAndHookSteamUser();
    }

    while(true) {
        if(GetAsyncKeyState(VK_F11)&1) ToggleStealth();
        
        if (g_SafePaused) {
            Sleep(100);
            continue;
        }

        if(g_HooksActive) {
            if (g_cvar_frame_skip.string) {
                g_SkipVal = atoi(g_cvar_frame_skip.string);
            }
            
            // Update delay dynamically
            if (g_cvar_sb_delay.string) {
                int d = atoi(g_cvar_sb_delay.string);
                if (d >= 0) g_SayiBilenDelay = d;
            }
            
            if (g_cvar_sb.string) {
                static char lastVal[64] = "";
                if (strcmp(lastVal, g_cvar_sb.string) != 0) {
                     strcpy(lastVal, g_cvar_sb.string);
                     int mn, mx;
                     int count = sscanf(lastVal, "%d %d", &mn, &mx);
                     
                     if (count == 2) {
                         if (mn >= 0 && mx >= 0) SayiBilen_Start(mn, mx);
                     } else if (count == 1) {
                         if (mn == 0) SayiBilen_Stop(); 
                         else if (mn == 1) {
                             // "sb 1" means START using sb_range
                             int r = 100;
                             if (g_cvar_sb_range.string) r = atoi(g_cvar_sb_range.string);
                             if (r < 1) r = 100;
                             SayiBilen_Start(0, r);
                         }
                         else if (mn > 1) {
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
    if(r==DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        CreateThread(0,0,MainThread,0,0,0);
    }
    return TRUE;
}
