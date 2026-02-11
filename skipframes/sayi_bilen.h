#ifndef SAYI_BILEN_H
#define SAYI_BILEN_H

#include <windows.h>

// Initialize the module
void LogDebug(const char* fmt, ...);



void SayiBilen_Init();

// Set the guessing range (starts the bot)
// min < target < max
void SayiBilen_Start(int min, int max);

// Stop the bot manually
// Stop the bot manually
void SayiBilen_Stop();

// Global delay setting (ms)
extern int g_SayiBilenDelay;

// Process incoming chat messages
// Returns true if the message was handled/recognized
void SayiBilen_OnMessage(const char* msg);

// Main update loop (call this periodically, e.g. in a thread)
extern "C" void SayiBilen_Update();



extern bool g_SayiBilenActive;

#endif
