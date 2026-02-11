
import os

HW_DLL_NAME = "hw_dump.dll"
OFF_SCR_UpdateScreen = 0x4C2A0

def dump_scr():
    if not os.path.exists(HW_DLL_NAME):
        print("hw_dump.dll not found")
        return
        
    with open(HW_DLL_NAME, "rb") as f:
        data = f.read()
        
    start = OFF_SCR_UpdateScreen
    end = start + 32
    bytes_data = data[start:end]
    print(f"SCR_BYTES: {bytes_data.hex().upper()}")

if __name__ == "__main__":
    dump_scr()
