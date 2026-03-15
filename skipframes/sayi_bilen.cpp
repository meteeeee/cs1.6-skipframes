#include "sayi_bilen.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <windows.h>

// ---------------------------------------------------------
// STATE
// ---------------------------------------------------------
bool g_SayiBilenActive = false;
static bool g_WaitingForStart = false;
static int g_Min = 0;
static int g_Max = 0;
static int g_LastGuess = 0;
static DWORD g_NextGuessTime = 0;

int g_SayiBilenDelay = 5; // Default 5ms (User requested configurable)

// Engine pointer
typedef void (*ClientCmd_t)(const char *);
extern ClientCmd_t g_pfnClientCmd;

void SendChat(int number) {
  if (!g_pfnClientCmd)
    return;

  char cmd[64];
  sprintf(cmd, "say %d", number);
  g_pfnClientCmd(cmd);
}

void SayiBilen_Init() { srand(GetTickCount()); }

void SayiBilen_Start(int min, int max) {
  if (min > max) {
    int temp = min;
    min = max;
    max = temp;
  }

  g_Min = min;
  g_Max = max;
  g_SayiBilenActive = true;
  g_WaitingForStart = true;
  g_LastGuess = -99999;
  g_NextGuessTime = 0;
}

void SayiBilen_Stop() {
  g_SayiBilenActive = false;
  // Auto-reset cvar to 0 so "sb 100" works again next time
  if (g_pfnClientCmd)
    g_pfnClientCmd("sb 0");
}

void NormalizeString(const char *input, char *output, int maxLen) {
  if (!input || !output || maxLen <= 0) return;
  int w = 0;
  for (int r = 0; input[r] && w < maxLen - 1; r++) {
    unsigned char c = (unsigned char)input[r];
    if (c >= 'A' && c <= 'Z') {
      output[w++] = c + 32; // Optimized 'a' - 'A'
    } else {
      output[w++] = (char)c;
    }
  }
  output[w] = 0;
}

void SayiBilen_OnMessage(const char *msg) {
  if (!g_SayiBilenActive)
    return;
  if (!msg || !*msg)
    return;

  char cleanMsg[256];
  NormalizeString(msg, cleanMsg, 256);

  if (g_WaitingForStart) {
    // "Sayiyi Bilen Kazanir oyununu baslatti" OR just "basla"
    if (strstr(cleanMsg, "sayiyi bilen kazanir oyununu baslatti") ||
        strstr(cleanMsg, "basla")) {
      g_WaitingForStart = false;
      g_NextGuessTime = 0;
    }
    return;
  }

  // Stop command: ONLY "oyununu kazandi"
  if (strstr(cleanMsg, "oyununu kazandi")) {
    SayiBilen_Stop();
    return;
  }

  bool isLow = (strstr(cleanMsg, "dusuk") || strstr(cleanMsg, "kucuk") ||
                strstr(cleanMsg, "asagi"));
  bool isHigh = (strstr(cleanMsg, "yuksek") || strstr(cleanMsg, "buyuk") ||
                 strstr(cleanMsg, "yukari") || strstr(cleanMsg, "cik"));

  // Ignore ambiguous messages containing both directions
  if (isLow && isHigh)
    return;

  // Prevent glitch: Ignore feedback if the bot hasn't made its first guess yet
  if (g_LastGuess == -99999)
    return;

  // Cooldown
  static DWORD lastFeedbackTime = 0;
  if (GetTickCount() - lastFeedbackTime < (DWORD)g_SayiBilenDelay)
    return;

  if (isLow) {
    g_Max = g_LastGuess - 1;
    if (g_Max < g_Min)
      g_Max = g_Min;
    g_NextGuessTime = GetTickCount() + g_SayiBilenDelay;
    lastFeedbackTime = GetTickCount();
  } else if (isHigh) {
    g_Min = g_LastGuess + 1;
    if (g_Min > g_Max)
      g_Min = g_Max;
    g_NextGuessTime = GetTickCount() + g_SayiBilenDelay;
    lastFeedbackTime = GetTickCount();
  }
}

extern "C" void SayiBilen_Update() {
  if (!g_SayiBilenActive)
    return;
  if (g_WaitingForStart)
    return;

  if (GetTickCount() >= g_NextGuessTime) {
    int guess = (g_Min + g_Max) / 2;

    if (guess == g_LastGuess)
      return;

    g_LastGuess = guess;
    SendChat(guess);

    // Use configurable delay
    g_NextGuessTime = GetTickCount() + g_SayiBilenDelay;
  }
}
