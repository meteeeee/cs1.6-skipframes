#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>

// =============================================================
// SKIPFRAMES + VALVE_ID FORCE (RELEASE BUILD)
// =============================================================

#define OFF_CVAR_REG    0x2E960
#define OFF_CMD_PATCH   0x2809D
#define OFF_SCR_UPDATE  0x4C2A0

DWORD g_HwBase = 0;
DWORD g_SCR = 0;
DWORD g_CreateInterface = 0;
DWORD g_SteamInternal = 0;
DWORD g_GetVolumeInfo = 0;
DWORD g_JmpTarget = 0;

bool g_HooksActive = false;
bool g_StealthMode = false;

// STORAGE
BYTE g_Orig_SCR[6];
BYTE g_Orig_CI[5];
BYTE g_Orig_SI[5];
BYTE g_Orig_Vol[5]; // Kept for consistency, though VolInfo hook is minimal

// LOGGING (SILENT)
void Log(const char* fmt, ...) {
    // Release Mode: No Logs
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

// We only keep frame_skip and change_id
cvar_t g_cvar_frame_skip = { "frame_skip", "0", 0, 0.0f, nullptr };
cvar_t g_cvar_change_id  = { "change_id", "0", 0, 0.0f, nullptr }; // 0=Real, 1=ValveID

// -------------------------------------------------------------
// HOOK TYPEDEFS
// -------------------------------------------------------------
typedef int (__fastcall *InitiateGameConnection_t)(void*, int, void*, int, uint64_t, uint32_t, uint16_t, bool);
InitiateGameConnection_t g_Original_Initiate = nullptr;

// -------------------------------------------------------------
// HOOKS
// -------------------------------------------------------------

// InitiateGameConnection Hook (Index 3)
// This is where we BLOCK the ticket to get VALVE_ID (IP-Based).
int __fastcall Hook_LegacyAuth(void* thisptr, int edx, void* pBlob, int cbMax, uint64_t steamIDGS, uint32_t ip, uint16_t port, bool secure) {
    // Dynamic Toggle:
    // If change_id is "1", we BLOCK -> VALVE_ID.
    // If change_id is "0", we PASS  -> REAL_ID.
    
    int val = atoi(g_cvar_change_id.string);
    if (val == 1) {
        memset(pBlob, 0, cbMax);
        return 0; // Blocked
    }
    
    // Pass-through to Original to get Real ID
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
                 // Index 3: InitiateGameConnection (BLOCKER)
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
    // We don't care about Serial anymore for Blocking method, but let's keep it consistent
    if (lpVolSerial) *lpVolSerial = 123456; 
    if (lpMaxComp) *lpMaxComp = 255;
    if (lpFlags) *lpFlags = 0;
    if (lpFSName && nFSNameSz > 0) strcpy(lpFSName, "NTFS");
    return TRUE; 
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
    
    PatchByte(g_HwBase + OFF_CMD_PATCH, 0xEB);
    
    if(g_SCR) WriteJMP(g_SCR, (DWORD)Hook_SCR_Trampoline, g_Orig_SCR, 6);
    if(g_CreateInterface) WriteJMP(g_CreateInterface, (DWORD)Hook_CreateInterface, g_Orig_CI, 5);
    if(g_SteamInternal) WriteJMP(g_SteamInternal, (DWORD)Hook_SteamInternal, g_Orig_SI, 5);
    if(g_GetVolumeInfo) WriteJMP(g_GetVolumeInfo, (DWORD)Hook_GetVolInfo, g_Orig_Vol, 5);

    g_HooksActive = true;
}

void RemoveHooks() {
    if(!g_HooksActive) return;

    if(g_SCR) Restore(g_SCR, g_Orig_SCR, 6);
    if(g_CreateInterface) Restore(g_CreateInterface, g_Orig_CI, 5);
    if(g_SteamInternal) Restore(g_SteamInternal, g_Orig_SI, 5);
    if(g_GetVolumeInfo) Restore(g_GetVolumeInfo, g_Orig_Vol, 5);
    
    PatchByte(g_HwBase + OFF_CMD_PATCH, 0x74);
    g_HooksActive = false;
}

void ToggleStealth() {
    g_StealthMode = !g_StealthMode;
    if(g_StealthMode) RemoveHooks();
    else InstallHooks();
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

DWORD WINAPI MainThread(LPVOID) {
    while(!(g_HwBase = (DWORD)GetModuleHandleA("hw.dll"))) Sleep(100);
    g_SCR = g_HwBase + OFF_SCR_UPDATE;
    g_JmpTarget = g_SCR + 6;

    HMODULE hKernel = GetModuleHandleA("kernel32.dll");
    if(hKernel) g_GetVolumeInfo = (DWORD)GetProcAddress(hKernel, "GetVolumeInformationA");
    
    // Scan for steam_api.dll (optional delay)
    HMODULE hSteam = NULL;
    for (int i = 0; i < 50; i++) { // Wait up to 5s
        hSteam = GetModuleHandleA("steam_api.dll");
        if (hSteam) break;
        Sleep(100);
    }
    
    // Proactive Hooking for SteamUser
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
    }

    InstallHooks();

    if(g_CreateInterface) {
        FindAndHookSteamUser();
    }

    while(true) {
        if(GetAsyncKeyState(VK_F11)&1) ToggleStealth();
        if(g_HooksActive) {
            if (g_cvar_frame_skip.string) {
                g_SkipVal = atoi(g_cvar_frame_skip.string);
            }
        }
        Sleep(50);
    }
    return 0;
}

BOOL APIENTRY DllMain(HMODULE h, DWORD r, LPVOID) {
    if(r==DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        CreateThread(0,0,MainThread,0,0,0);
    }
    return TRUE;
}
