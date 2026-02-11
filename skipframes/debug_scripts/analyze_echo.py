import struct

filename = "hw_dump.dll"
base_addr = 0x03940000

with open(filename, "rb") as f:
    data = f.read()

# Cmd_Echo location: 0x27B00 (From previous manual analysis of 'echo' string ref)
# Let's verify it matches "PUSH ESI; MOV ESI, 1"
echo_off = 0x27B00
print(f"[Cmd_Echo] Offset 0x{echo_off:X}")
snippet = data[echo_off:echo_off+64]
print("Bytes: " + snippet.hex(' ').upper())

# Parsing:
# 0: 56 (PUSH ESI)
# 1: BE 01 00 00 00 (MOV ESI, 1) -> Loop start
# 6: E8 D5 03 00 00 (CALL Cmd_Argc) -> 0x27B06 + 5 + 3D5 = 27EE0.
# 11: 3B C6 (CMP EAX, ESI) -> Compare i < argc? No.
# Argc result in EAX. CMP EAX, ESI.
# 13: 7E 1E (JLE 0x1E) -> Exit loop?
# 15: 56 (PUSH ESI) -> i
# 16: E8 DB 03 00 00 (CALL Cmd_Argv)  <-- FOUND IT.
#     Address: 0x27B10 + 1 + 5 + 03DB = 0x27EF0.
#     0x27B11 + 0x3DB = 0x27EEC? No. 
#     Call at 0x27B10+1 = 0x27B11.
#     Next instruction: 0x27B16.
#     Target: 0x27B16 + 0x03DB = 0x27EF1?
#     Let's check snippet:
#     16: E8 DB 03 00 00.
#     Offset 16 (relative to start 27B00) -> 27B10.
#     Wait, 0x10 is 16.
#     So E8 is at 27B10.
#     Next instruction at 27B15.
#     Rel: 03DB.
#     27B15 + 3DB = 27EF0.
#     So Cmd_Argv is at 0x27EF0.
#     Matches my manual suspicion.

# Conclusion:
# Cmd_Echo sig: 56 BE 01 00 00 00 E8
# Cmd_Argc Call Offset: +7 (DWORD at +7)
# Cmd_Argv Call Offset: +17 (DWORD at +17) (Wait, 16 is E8. Offset value is at 17?)
# E8 at 16. Value at 17,18,19,20.
# So Argv Call Rel = *(DWORD*)(echo + 17).
# Correct.

# I will print offsets to verify uniqueness of "56 BE 01 00 00 00 E8".
pat = b"\x56\xBE\x01\x00\x00\x00\xE8"
mask = "xxxxxxx"
# count matches
import analyze_dump # Self, but simpler loop
count = 0
for i in range(len(data)-len(pat)):
    if data[i:i+len(pat)] == pat:
        count += 1
        print(f"Match found at 0x{i:X}")

print(f"Total Echo matches: {count}")
