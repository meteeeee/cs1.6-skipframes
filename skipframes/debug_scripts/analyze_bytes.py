import struct

def analyze_27390():
    with open("hw_dump.dll", "rb") as f:
        data = f.read()
    
    # Dump 0x27390
    start = 0x27390
    end = 0x273E0
    print(f"Dumping 0x{start:X}:")
    for i in range(start, end):
        print(f"{data[i]:02X} ", end="")
        if (i - start + 1) % 16 == 0:
            print("")
    print("")

if __name__ == "__main__":
    analyze_27390()
