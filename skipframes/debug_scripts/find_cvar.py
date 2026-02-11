import struct
import os

def find_offsets():
    try:
        if not os.path.exists("hw_dump.dll"):
            print("Error: hw_dump.dll not found")
            return

        with open("hw_dump.dll", "rb") as f:
            data = f.read()

        RUNTIME_BASE = 0x3840000 
        print(f"Using Runtime Base: 0x{RUNTIME_BASE:X}")
        
        # 1. Find "fps_override" string (newer CVar)
        cvar_name = b"fps_override\0"
        sens_offset = data.find(cvar_name)
        if sens_offset == -1:
            print(f"Could not find '{cvar_name.decode()}' string")
            return
            
        print(f"'{cvar_name.decode()}' string at offset: 0x{sens_offset:X}")
        
        # Address of string in memory
        sens_addr = RUNTIME_BASE + sens_offset
        print(f"Searching for push 0x{sens_addr:X}...")
        
        packed_addr = struct.pack("<I", sens_addr)
        
        # The struct will contain this pointer. 
        # But RegisterVariable takes a pointer to the struct, NOT the string.
        # So:
        # 1. Find where string address is written (initialization of struct)
        # 2. Find who pushes the STRUCT address
        
        # Search for the string address in DATA section (struct initialization)
        print(f"Searching for reference to string 0x{sens_addr:X}...")
        struct_offset = data.find(packed_addr)
        if struct_offset == -1:
            print("Could not find struct initialization")
             # Maybe it's pushed directly? checking push
            push_pattern = b"\x68" + packed_addr
            push_offset = data.find(push_pattern)
            if push_offset != -1:
                 print(f"Found PUSH of string at 0x{push_offset:X}. This might be direct Cmd_AddCommand?")
            return

        print(f"Found struct at offset: 0x{struct_offset:X} (This is likely 'name' field of cvar_t)")
        
        # Struct address in memory
        struct_addr = RUNTIME_BASE + struct_offset
        print(f"Struct address: 0x{struct_addr:X}")
        
        # Now find who pushes THIS struct address
        print(f"Searching for push struct 0x{struct_addr:X}...")
        packed_struct = struct.pack("<I", struct_addr)
        push_struct_pattern = b"\x68" + packed_struct
        
        push_struct_offset = data.find(push_struct_pattern)
        if push_struct_offset == -1:
            print("Could not find push of struct")
            return
            
        print(f"Found PUSH of struct at 0x{push_struct_offset:X}")
        
        # 3. Look for the CALL after the push (Cvar_RegisterVariable)
        cvar_reg_offset = 0
        for i in range(push_struct_offset, push_struct_offset + 50):
            if data[i] == 0xE8: # CALL
                rel = struct.unpack("<i", data[i+1:i+5])[0] 
                dest = RUNTIME_BASE + i + 5 + rel
                dest_rva = dest - RUNTIME_BASE
                print(f"Found CALL at offset 0x{i:X} -> Target RVA: 0x{dest_rva:X}")
                cvar_reg_offset = dest_rva
                break

        
        if cvar_reg_offset:
            print(f"Cvar_RegisterVariable Offset: 0x{cvar_reg_offset:X}")
            
        # VERIFY with known Cmd_AddCommand
        print("\nVerifying Cmd_AddCommand...")
        echo_offset = data.find(b"echo\0")
        if echo_offset != -1:
             print(f"'echo' string at: 0x{echo_offset:X}")
             echo_addr = RUNTIME_BASE + echo_offset
             pack_echo = struct.pack("<I", echo_addr)
             push_echo = b"\x68" + pack_echo
             
             push_echo_off = data.find(push_echo)
             if push_echo_off != -1:
                 print(f"Found push 'echo' at 0x{push_echo_off:X}")
                 # Look for call AddCommand
                 for i in range(push_echo_off, push_echo_off + 50):
                    if data[i] == 0xE8:
                        rel = struct.unpack("<i", data[i+1:i+5])[0]
                        dest = RUNTIME_BASE + i + 5 + rel
                        dest_rva = dest - RUNTIME_BASE
                        print(f"Found CALL (Cmd_AddCommand?) at 0x{i:X} -> RVA: 0x{dest_rva:X}")
                        break
            
    except Exception as e:
        print(f"Error: {e}")

if __name__ == "__main__":
    find_offsets()
