import struct

def find_cmd_functions():
    with open("hw_dump.dll", "rb") as f:
        data = f.read()
    
    # Base addresses to try
    bases = [0x1D00000, 0x1900000, 0x3840000, 0x0] 
    
    targets = ["Cbuf_InsertText", "Cmd_ExecuteString", "Cbuf_AddText"]
    
    for target in targets:
        print(f"\n--- Searching for {target} ---")
        str_off = data.find(target.encode('utf-8'))
        
        if str_off == -1:
            print("String not found.")
            continue
            
        print(f"String at offset: 0x{str_off:X}")
        
        found = False
        for base in bases:
            str_addr = base + str_off
            packed = struct.pack("<I", str_addr)
            
            # PUSH (68 XXXXXXXX)
            push_pattern = b"\x68" + packed
            push_off = data.find(push_pattern)
            
            if push_off != -1:
                print(f"[Base 0x{base:X}] PUSH at 0x{push_off:X}")
                
                # Scan backwards for function start
                start = push_off
                while start > push_off - 300:
                    if data[start] == 0x55 and data[start+1] == 0x8B and data[start+2] == 0xEC:
                         print(f"Function Start: 0x{start:X}")
                         found = True
                         break
                    start -= 1
        
        if not found:
            print("No references found for this string.")

if __name__ == "__main__":
    find_cmd_functions()
