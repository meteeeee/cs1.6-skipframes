#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>

// =============================================================
// SKIPFRAMES - STEALTH TOGGLE + CVAR STYLE
// F11 = toggle stealth mode
// =============================================================

struct cvar_t {
    char* name;
    char* string;
    int flags;
    float value;
    cvar_t* next;
};

typedef void(*Cvar_RegisterVariable_t)(cvar_t*);
typedef void(*Cmd_AddCommand_t)(const char*, void(*)());

Cvar_RegisterVariable_t g_CvarRegister = nullptr;
Cmd_AddCommand_t g_AddCmd = nullptr;

cvar_t g_cvar_frame_skip = {
    (char*)"frame_skip",
    (char*)"0",
    0, 
    0.0f,
    nullptr
};

DWORD g_HwBase = 0;
DWORD g_SCR = 0;

// Global variables exposed for ASM direct access
extern "C" {
    volatile int g_skip_frames = 0;
    int g_FrameCount = 0;
    DWORD g_JmpBackAddr = 0;
}

BYTE g_OriginalBytes[6];
bool g_HookInstalled = false;
bool g_StealthMode = false;

// LOGGING - Disabled for Release Build to save size/speed
// Uncomment the function and comment the macro to enable logs
#ifdef _DEBUG
void Log(const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    OutputDebugStringA(buf);
}
#else
// Compiled out completely - strings won't appear in binary
#define Log(...) ((void)0)
#endif

// Assembly hook
__asm__ (
    ".intel_syntax noprefix\n"
    ".text\n"
    ".globl _Hook_SCR\n"
    "_Hook_SCR:\n"
    "pushad\n"
    "pushfd\n"
    
    // Check if skipping is disabled (skip_frames == 0)
    // This is "Common Case A" (Normal play)
    "mov eax, [_g_skip_frames]\n"
    "test eax, eax\n"
    "jz .Lrender\n"
    
    // "Common Case B" (Skipping enabled)
    // We use a DOWN COUNTER to avoid reading _g_skip_frames every frame!
    // Optimization: 'dec' is faster/smaller than 'cmp [mem]'
    
    "mov ecx, [_g_FrameCount]\n"
    "dec ecx\n"
    "mov [_g_FrameCount], ecx\n"
    
    "jg .Lskip\n" // If count > 0, SKIP. (Common Case for skipping)
    
    // If count <= 0, Time to RENDER.
    // Reset counter to skip_frames
    // We already have skip_frames in EAX from earlier check
    "mov [_g_FrameCount], eax\n"
    
    ".Lrender:\n"
    "popfd\n"
    "popad\n"
    
    // Restore overwritten instructions (6 bytes):
    // 55          push ebp
    // 8B EC       mov ebp, esp
    // 83 EC 10    sub esp, 0x10
    "push ebp\n"
    "mov ebp, esp\n"
    "sub esp, 0x10\n" 
    
    "jmp dword ptr [_g_JmpBackAddr]\n"
    
    ".Lskip:\n"
    "popfd\n"
    "popad\n"
    "ret\n"
    ".att_syntax\n"
);

extern "C" void Hook_SCR();

bool WriteBytes(DWORD addr, void* bytes, int len) {
    DWORD old;
    if (!VirtualProtect((void*)addr, len, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy((void*)addr, bytes, len);
    VirtualProtect((void*)addr, len, old, &old);
    return true;
}

bool InstallHook() {
    if (g_HookInstalled) return true;
    
    BYTE hookBytes[6];
    hookBytes[0] = 0xE9;
    *(DWORD*)(hookBytes + 1) = (DWORD)Hook_SCR - g_SCR - 5;
    hookBytes[5] = 0x90;
    
    if (!WriteBytes(g_SCR, hookBytes, 6)) return false;
    
    DWORD cmdAddr = g_HwBase + 0x2809D;
    BYTE patch = 0xEB;
    WriteBytes(cmdAddr, &patch, 1);
    
    g_HookInstalled = true;
    g_StealthMode = false;
    return true;
}

bool RemoveHook() {
    if (!g_HookInstalled) return true;
    
    if (!WriteBytes(g_SCR, g_OriginalBytes, 6)) return false;
    
    DWORD cmdAddr = g_HwBase + 0x2809D;
    BYTE original = 0x74;
    WriteBytes(cmdAddr, &original, 1);
    
    g_HookInstalled = false;
    g_StealthMode = true;
    return true;
}

void ToggleStealth() {
    if (g_StealthMode) {
        if (InstallHook()) Log("[SF] STEALTH OFF");
    } else {
        if (RemoveHook()) Log("[SF] STEALTH ON");
    }
}

bool BypassCmdAdd() {
    DWORD addr = g_HwBase + 0x2809D;
    if (*(BYTE*)addr == 0x74) {
        BYTE patch = 0xEB;
        WriteBytes(addr, &patch, 1);
    }
    return true;
}

DWORD WINAPI HotkeyThread(LPVOID) {
    while (true) {
        if (GetAsyncKeyState(VK_F11) & 0x8000) {
            ToggleStealth();
            Sleep(500);
        }
        
        if (g_HookInstalled) {
            g_skip_frames = (int)g_cvar_frame_skip.value;
        }
        
        Sleep(50);
    }
    return 0;
}

void Init(HMODULE) {
    Sleep(500);
    Log("=== SkipFrames Stealth + CVar ===");
    
    HMODULE hw = GetModuleHandleA("hw.dll");
    if (!hw) { Log("[SF] No hw.dll"); return; }
    
    g_HwBase = (DWORD)hw;
    
    g_CvarRegister = (Cvar_RegisterVariable_t)(g_HwBase + 0x2E960);
    g_AddCmd = (Cmd_AddCommand_t)(g_HwBase + 0x281A0);
    g_SCR = g_HwBase + 0x4C2A0;
    
    g_JmpBackAddr = g_SCR + 6;
    
    // Check for expected bytes
    BYTE* p = (BYTE*)g_SCR;
    if (p[0] != 0x55 || p[1] != 0x8B || p[2] != 0xEC || p[3] != 0x83 || p[4] != 0xEC || p[5] != 0x10) {
        Log("[SF] ERROR: Unexpected instructions at SCR! hook might crash.");
        Log("[SF] Found: %02X %02X %02X %02X %02X %02X", p[0], p[1], p[2], p[3], p[4], p[5]);
        return;
    }
    
    memcpy(g_OriginalBytes, (void*)g_SCR, 6);
    
    if (InstallHook()) {
        Log("[SF] Hook installed");
        
        BypassCmdAdd();
        
        if (g_CvarRegister) {
            g_CvarRegister(&g_cvar_frame_skip);
            Log("[SF] CVar registered");
        }
        
        CreateThread(NULL, 0, HotkeyThread, NULL, 0, NULL);
        Log("[SF] Ready! F11=stealth");
    }
}

BOOL APIENTRY DllMain(HMODULE m, DWORD r, LPVOID) {
    if (r == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(m);
        CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)Init, m, 0, NULL);
    }
    else if (r == DLL_PROCESS_DETACH) {
        RemoveHook();
    }
    return TRUE;
}
