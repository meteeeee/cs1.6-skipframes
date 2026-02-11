import struct

def find_usermsg_v2():
    with open("hw_dump.dll", "rb") as f:
        data = f.read()

    # Found in dump: "spatchUserMsg..."
    # The full string is likely "DispatchUserMsg: User Msg %s/%d sent too much data"
    target_str = b"DispatchUserMsg"
    str_off = data.find(target_str)
    
    if str_off == -1:
        print("String 'User Msg' not found.")
        return

    print(f"String found at: 0x{str_off:X}")
    
    # Bases to try
    bases = [0x1D00000, 0x1900000, 0x3840000, 0x0] 
    
    found = False
    
    for base in bases:
        str_addr = base + str_off
        packed = struct.pack("<I", str_addr)
        
        # 1. PUSH (68 XXXXXXXX)
        push_pattern = b"\x68" + packed
        push_off = data.find(push_pattern)
        
        if push_off != -1:
            print(f"[Base 0x{base:X}] match via PUSH at 0x{push_off:X}")
            found = True
            scan_func(data, push_off)
            
        # 2. MOV reg, address (B8..BF)
        # B8+r32 (EAX..EDI)
        for reg in range(8):
            opcode = 0xB8 + reg
            pattern = struct.pack("B", opcode) + packed
            mov_off = data.find(pattern)
            if mov_off != -1:
                print(f"[Base 0x{base:X}] match via MOV (reg {reg}) at 0x{mov_off:X}")
                found = True
                scan_func(data, mov_off)

    if not found:
        print("No PUSH/MOV reference found.")
        print("Dumping bytes around string just in case:")
        start = max(0, str_off - 16)
        end = min(len(data), str_off + 32)
        print(f"Dump {start:X}-{end:X}:")
        for i in range(start, end):
            print(f"{data[i]:02X} ", end="")
        print("")
        
        # Search for "TextMsg" string as fallback
        print("\nSearching for 'TextMsg' string...")
        tm_off = data.find(b"TextMsg\0")
        if tm_off != -1:
            print(f"'TextMsg' found at 0x{tm_off:X}")
            # Do similar ref check for TextMsg
            for base in bases:
                 p2 = struct.pack("<I", base + tm_off)
                 if data.find(b"\x68" + p2) != -1:
                      print(f"Reference to 'TextMsg' found with Base 0x{base:X}")

def scan_func(data, ref_offset):
    # Same as before
    # Scan backwards for 55 8B EC
    start = ref_offset
    while start > ref_offset - 500:
        if data[start] == 0x55 and data[start+1] == 0x8B and data[start+2] == 0xEC:
            print(f"Function Start found at: 0x{start:X}")
            return
        start -= 1
    print("Function start not found nearby.")

if __name__ == "__main__":
    find_usermsg_v2()
