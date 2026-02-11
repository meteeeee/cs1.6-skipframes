import struct

filename = "hw_dump.dll"
base_addr = 0x03940000

with open(filename, "rb") as f:
    data = f.read()

# Find "fps_max" string
fps_str = b"fps_max\x00"
try:
    str_off = data.index(fps_str)
    print(f"[String] 'fps_max' at 0x{str_off:X}")
    
    # In HL, Cvars are often registered like:
    # cvar_t fps_max = {"fps_max", "72", 0};
    # Cvar_RegisterVariable(&fps_max);
    # So "fps_max" string is pointed to by a Struct.
    # The Struct is passed to the function.
    
    # Find pointer to 'fps_max' string
    str_va = base_addr + str_off
    # Search for the pointer in data segment (The cvar_t struct)
    str_ptr = struct.pack('<I', str_va)
    struct_offsets = []
    
    # Scan raw data for pointer to string
    off = -1
    while True:
        try:
            off = data.index(str_ptr, off+1)
            struct_offsets.append(off)
        except ValueError:
            break
            
    print(f"[Struct] Found {len(struct_offsets)} pointers to string.")
    
    # For each struct candidate, see if it's passed to a function
    for s_off in struct_offsets:
        s_va = base_addr + s_off
        print(f"  Candidate Struct at 0x{s_off:X} (VA 0x{s_va:X})")
        
        # Look for PUSH Struct_VA (68 XX XX XX XX)
        push_struct = b'\x68' + struct.pack('<I', s_va)
        try:
            ref = data.index(push_struct)
            print(f"    [Ref] Passed to function at 0x{ref:X}")
            # PUSH Struct
            # CALL Cvar_RegisterVariable
            # Look for CALL after PUSH
            snippet = data[ref:ref+10] # PUSH(5) + CALL(5)
            print(f"    Bytes: {snippet.hex(' ').upper()}")
            
            if snippet[5] == 0xE8:
                rel = struct.unpack('<i', snippet[6:10])[0]
                func_off = ref + 5 + 5 + rel # offset from file start? No.
                # ref is file offset.
                # CALL is at ref+5.
                # Next instr is ref+10.
                # Target = (ref+10) + rel ?? No. REL is relative to next IP.
                # ref is offset. We need to be careful with VA mapping.
                # Assuming raw file mapping is linear (it usually is for .text).
                target = (ref + 10) + rel
                print(f"    [Potential] Cvar_RegisterVariable at 0x{target:X}")
                
        except ValueError:
            pass

except ValueError:
    print("[-] fps_max string not found")

