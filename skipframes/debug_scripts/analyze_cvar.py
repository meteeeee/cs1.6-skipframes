
import os
import re

HW_DLL_NAME = "hw_dump.dll"
HW_DLL_BASE = 0x3940000 
OFF_SCR_UpdateScreen = 0x4C2A0

def analyze():
    if not os.path.exists(HW_DLL_NAME):
        print(f"Error: {HW_DLL_NAME} not found.")
        return

    with open(HW_DLL_NAME, "rb") as f:
        data = f.read()

    print(f"Loaded {HW_DLL_NAME} ({len(data)} bytes)")

    # 1. Dump SCR_UpdateScreen Bytes
    print(f"\nAnalyzing SCR_UpdateScreen at +0x{OFF_SCR_UpdateScreen:X}...")
    start_scr = OFF_SCR_UpdateScreen
    end_scr = start_scr + 32
    print(f"SCR_BYTES: {data[start_scr:end_scr].hex().upper()}")

    # 2. Check for A1 (Safety Flag)
    # A1 1C 33 F8 03
    # Check offset +6
    if data[start_scr+6] == 0xA1:
        addr = int.from_bytes(data[start_scr+7:start_scr+11], byteorder='little')
        print(f"Found Safety Flag Ptr: 0x{addr:X}")
    else:
        print("Safety Flag (A1) NOT found at expected offset!")

    # 3. Find cls.state
    # Pattern: 83 3D <4 bytes> 05
    # This means CMP DWORD PTR [addr], 5
    print("\nScanning for 'CMP [cls.state], 5'...")
    
    # We scan the ENTIRE file for this pattern
    # It usually appears in SCR_UpdateScreen, CL_Connect, etc.
    
    candidates = {}
    
    # Iterate all matches
    pattern = b'\x83\x3D....\x05'
    for match in re.finditer(pattern, data):
        addr_bytes = match.group(0)[2:6]
        addr = int.from_bytes(addr_bytes, byteorder='little')
        
        # Address sanity check
        # Must be within data segment range?
        # HW base is 0x3940000. Data is usually after code.
        # Max reasonable address < Base + Size
        if addr >= HW_DLL_BASE and addr < HW_DLL_BASE + len(data):
            candidates[addr] = candidates.get(addr, 0) + 1
            
            # Print context if inside SCR_UpdateScreen (approx range)
            # SCR is at 0x4C2A0. Let's look around 0x4C000 - 0x4D000
            if OFF_SCR_UpdateScreen <= match.start() <= OFF_SCR_UpdateScreen + 0x200:
                print(f"  MATCH INSIDE SCR_UpdateScreen! Offset +0x{match.start():X} -> Addr 0x{addr:X}")
    
    print("\nTop Candidates for cls.state:")
    for addr, count in sorted(candidates.items(), key=lambda x: x[1], reverse=True):
        print(f"  0x{addr:X} (Count: {count})")
        if count > 5:
            print("    -> HIGH CONFIDENCE")

if __name__ == "__main__":
    analyze()
