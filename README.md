# SkipFrames

A low-level systems engineering utility for the GoldSrc engine, featuring a custom **Manual Map Injector**, **x86 Assembly Hooks**, and an **OpenGL Graphics Layer**.

## Key Features
- **Stealth Manual Mapping**: Injects the DLL directly into process memory without a disk footprint.
- **Volatile Memory Restoration (F11)**: A real-time "Stealth Toggle" that restores original instructions to bypass anti-cheat scans.
- **Graphics Enhancement (OpenGL Chams)**: Intercepts the rendering pipeline for target highlighting.
- **Mathematical Movement Optimizer**: Vector-based air acceleration logic for perfect Strafe-Boosting, BHop, and SGS.
- **Automated Game Solver**: Built-in Binary Search bot for chat-based games.

## Performance & Security
- **Atomic State Transitions**: Thread suspension ensures safe memory patching without crashes.
- **Dynamic Signature Scanning**: Automatically finds engine functions.

## Building & Installation
### Prerequisites
- **MinGW-w64** (32-bit GCC)

### Compilation
1. Open a terminal in the project directory.
2. Run:
   ```cmd
   cd skipframes
   compile.bat
   ```

---
Developed by [meteeeee](https://github.com/meteeeee)
