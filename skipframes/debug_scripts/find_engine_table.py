import os

# ClientCmd is at offset 0x272E0 in the file (based on previous findings)
# We want to find where this offset (or Base+Offset) is stored in the data section.

def scan_for_refs():
    try:
        dll_path = "../hw.dll"
        if not os.path.exists(dll_path):
            print(f"ERROR: {dll_path} not found")
            return

        with open(dll_path, "rb") as f:
            data = f.read()

        print(f"File size: {len(data)}")
        
        # We don't know the exact base address used in the file's relocation table
        # But commonly pointers in the table might be relative or absolute.
        # Let's search for the raw offset first: 00 02 72 E0 (Big Endian) -> E0 72 02 00 (Little Endian)
        
        target_bytes = b'\xE0\x72\x02\x00'
        
        print(f"Scanning for {target_bytes.hex()}...")
        
        count = 0
        for i in range(len(data) - 4):
            if data[i:i+4] == target_bytes:
                print(f"Found raw offset match at file offset: 0x{i:X}")
                # Check surrounding bytes to see if it looks like a table
                # (Many similar pointers nearby)
                context = data[i-16:i+16]
                print(f"Context: {context.hex()}")
                count += 1
                
        if count == 0:
            print("No raw offset matches found. Trying with default base 0x01D00000...")
            # 0x01D00000 + 0x272E0 = 0x01D272E0 -> E0 72 D2 01
            target_va = b'\xE0\x72\xD2\x01'
            print(f"Scanning for {target_va.hex()}...")
            
            for i in range(len(data) - 4):
                if data[i:i+4] == target_va:
                    print(f"Found VA match at file offset: 0x{i:X}")
                    context = data[i-32:i+32]
                    print(f"Context: {context.hex()}")
                    count += 1

    except Exception as e:
        print(f"Error: {e}")

scan_for_refs()
