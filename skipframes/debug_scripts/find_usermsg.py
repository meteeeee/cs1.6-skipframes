import struct

def find_usermsg():
    with open("hw_dump.dll", "rb") as f:
        data = f.read()

    # 1. Find string "User Msg"
    # This string is usually passed to Alert/Con_Printf when a msg is not found.
    # The function containing this is DispatchUserMsg.
    target_str = b"User Msg"
    str_off = data.find(target_str)
    
    if str_off == -1:
        print("String 'User Msg' not found.")
        return

    print(f"String found at: 0x{str_off:X}")
    
    # 2. Find reference (PUSH address)
    # The dump is likely from memory base. We need to know the base.
    # User said 0x3840000 might be base? Or 0x01D00000.
    # Let's search for the *relative offset* if we can, or just brute force common bases.
    
    bases = [0x3840000, 0x1D00000, 0x1900000, 0x0] 
    
    found_func = False
    
    for base in bases:
        str_addr = base + str_off
        packed = struct.pack("<I", str_addr)
        
        # Search for PUSH (0x68 + address)
        push_pattern = b"\x68" + packed
        push_off = data.find(push_pattern)
        
        if push_off != -1:
            print(f"Match with Base 0x{base:X}: PUSH at 0x{push_off:X}")
            
            # 3. Scan backwards for Function Start (55 8B EC)
            start = push_off
            while start > push_off - 500:
                if data[start] == 0x55 and data[start+1] == 0x8B and data[start+2] == 0xEC:
                    print(f"Function Start found at: 0x{start:X}")
                    print(f"HARDCODE THIS OFFSET: 0x{start:X}")
                    found_func = True
                    break
                start -= 1
            if found_func: break
            
    if not found_func:
        print("Could not find reference to string. Dump might be raw section or relocated.")

if __name__ == "__main__":
    find_usermsg()
