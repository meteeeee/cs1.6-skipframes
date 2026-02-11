import struct
import os

def find_offsets():
    try:
        if not os.path.exists("hw_dump.dll"):
            print("Error: hw_dump.dll not found")
            return

        with open("hw_dump.dll", "rb") as f:
            data = f.read()

        RUNTIME_BASE = 0x3840000 
        print(f"Using Runtime Base: 0x{RUNTIME_BASE:X}")
        
        # ---------------------------------------------------------
        # FIND ClientCmd (Cmd_ExecuteString)
        # ---------------------------------------------------------
        # Strategy: Look for "exec <filename>" string which is used in Cmd_Exec_f
        # Or look for "say" command registration?
        # Actually, "Cmd_AddCommand" is usually exported.
        # "Cmd_ExecuteString" might be at a known offset relative to something.
        
        # Best bet: Look for the function that handles "say" command?
        # 1. Find string "say"
        # 2. Find PUSH of "say"
        # 3. Find call to Cmd_AddCommand( "say", Cmd_Say_f )
        # 4. Cmd_Say_f calls ClientCmd or similar? No, standard say goes to server.
        
        # Alternative: "ClientCmd" function usually references "Cmd_ExecuteString" string? No.
        # It handles "stufftext".
        
        # Let's try to find "Cmd_ExecuteString" in exports first (if user has exports)
        # But this is a raw dump, imports/exports might be messy to parse without PE lib.
        
        # Let's look for known string usage in `Cbuf_AddText` (often used to send commands)
        # Or look for "text %s"
        
        # Let's try to find "quota %d" string which is used in Cbuf_AddText in some versions
        # Or just look for "Host_Error: Cbuf_AddText: buffer overflow\n"
        
        cbuf_overflow = b"Cbuf_AddText: buffer overflow"
        off = data.find(cbuf_overflow)
        if off != -1:
             print(f"Found 'Cbuf_AddText: buffer overflow' string at 0x{off:X}")
             # This string is pushed in Cbuf_AddText.
             # Find push of this string.
             str_addr = RUNTIME_BASE + off
             packed = struct.pack("<I", str_addr)
             push_pattern = b"\x68" + packed
             push_off = data.find(push_pattern)
             if push_off != -1:
                 print(f"Found function Cbuf_AddText candidate near 0x{push_off:X} (RVA: 0x{push_off:X})")
                 # Cbuf_AddText is basically ClientCmd for local.
                 # Function start search backwards from push_off?
                 # prologue is usually 55 8B EC.
                 start = push_off
                 while start > push_off - 200:
                     if data[start] == 0x55 and data[start+1] == 0x8B and data[start+2] == 0xEC:
                         print(f"Candidate Cbuf_AddText Start: 0x{start:X}")
                         # This IS ClientCmd typically.
                         break
                     start -= 1
        
        # ---------------------------------------------------------
        # FIND UserMsg Hook (pfnHookUserMsg)
        # ---------------------------------------------------------
        # Used by client.dll to hook messages.
        # It's usually passed in `HUD_Init`.
        # But we are in `hw.dll`.
        # `hw.dll` EXPORTS `HookUserMsg` usually? Or it's `EngMsg_HookUserMsg`.
        
        # Look for error string: "User Msg '%s' not found on server\n"
        # Or "User Msg %s declared with size %d, but server... "
        
        # Better: Look for "SayText" string? No, that's in client.
        
        # Look for "User Msg" string.
        msg_err = b"User Msg"
        off = data.find(msg_err)
        if off != -1:
             print(f"Found 'User Msg' string at 0x{off:X}")
             
    except Exception as e:
        print(f"Error: {e}")

if __name__ == "__main__":
    find_offsets()
