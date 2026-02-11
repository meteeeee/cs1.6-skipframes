import struct

def analyze_1A6EF():
    with open("hw_dump.dll", "rb") as f:
        data = f.read()
    
    # 0x1A6EF is where PUSH of string happens.
    # We want to see instructions BEFORE it to see PUSH of Name and Size.
    start = 0x1A6D0
    end = 0x1A710
    
    print(f"Dumping 0x{start:X}-0x{end:X}")
    for i in range(start, end):
        print(f"{data[i]:02X} ", end="")
        if (i - start + 1) % 16 == 0:
            print("")
    print("")

if __name__ == "__main__":
    analyze_1A6EF()
