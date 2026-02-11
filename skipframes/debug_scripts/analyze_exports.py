import struct

def scan_exports():
    try:
        with open("hw_dump.dll", "rb") as f:
            data = f.read()
            
        print("Scanning Exports...")
        # PE Header
        pe_offset = struct.unpack("<I", data[0x3C:0x40])[0]
        # Export Dir RVA
        export_rva = struct.unpack("<I", data[pe_offset + 0x78 : pe_offset + 0x7C])[0]
        
        if export_rva == 0:
            print("No Export Directory?")
            return

        # Map RVA to Offset (Assuming raw dump 1:1 if loaded, but this is file dump?)
        # If it's a raw memory dump, RVA = Offset usually (if base 0).
        # Users dump is likely just 'hw.dll' file or memory dump.
        # User said "hw.dll" from game folder? No, "hw_dump.dll" suggests memory dump.
        
        # Let's assume RVA = Offset for now suitable for memory dump.
        export_off = export_rva
        
        if export_off >= len(data):
            print("Export offset out of bounds.")
            return

        num_funcs = struct.unpack("<I", data[export_off+20:export_off+24])[0]
        num_names = struct.unpack("<I", data[export_off+24:export_off+28])[0]
        func_rva_off = struct.unpack("<I", data[export_off+28:export_off+32])[0]
        name_rva_off = struct.unpack("<I", data[export_off+32:export_off+36])[0]
        ord_rva_off  = struct.unpack("<I", data[export_off+36:export_off+40])[0]
        
        print(f"Functions: {num_funcs}, Names: {num_names}")
        
        for i in range(num_names):
            name_rva = struct.unpack("<I", data[name_rva_off + i*4 : name_rva_off + i*4 + 4])[0]
            # Read name
            if name_rva < len(data):
                 end = data.find(b'\0', name_rva)
                 if end == -1: end = len(data)
                 name_bytes = data[name_rva:end]
                 try:
                    name = name_bytes.decode('utf-8')
                 except:
                    name = str(name_bytes)
                 
                 # Get Ordinal
                 ordinal = struct.unpack("<H", data[ord_rva_off + i*2 : ord_rva_off + i*2 + 2])[0]
                 # Get Func RVA
                 func_rva = struct.unpack("<I", data[func_rva_off + ordinal*4 : func_rva_off + ordinal*4 + 4])[0]
                 
                 # Write to file
                 with open("exports.txt", "a", encoding="utf-8") as out:
                     out.write(f"{name} @ 0x{func_rva:X}\n")
                 
                 # Still print if matches filter
                 lower_name = name.lower()
                 if any(x in lower_name for x in ["cmd", "cbuf", "msg", "text", "execute", "console", "print", "gl"]):
                     print(f"EXPORT: {name} @ 0x{func_rva:X}")

    except Exception as e:
        print(f"Error: {e}")

if __name__ == "__main__":
    scan_exports()
