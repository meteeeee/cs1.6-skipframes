import struct

def inspect_func():
    try:
        with open("hw_dump.dll", "rb") as f:
            data = f.read()
            
        CVAR_OFFSET = 0x4C2A0
        print(f"Inspecting bytes at offset 0x{CVAR_OFFSET:X}...")
        
        # Read 32 bytes
        bytes = data[CVAR_OFFSET:CVAR_OFFSET+32]
        hex_idx = " ".join([f"{b:02X}" for b in bytes])
        print(f"Bytes: {hex_idx}")
        
    except Exception as e:
        print(f"Error: {e}")

if __name__ == "__main__":
    inspect_func()
