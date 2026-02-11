import psutil
import ctypes
import sys

# Windows API constants
PROCESS_VM_READ = 0x0010
PROCESS_QUERY_INFORMATION = 0x0400

def dump_module(process_name, module_name, output_file):
    print(f"Looking for process: {process_name}")
    pid = 0
    proc = None
    
    # Find process
    for p in psutil.process_iter(['pid', 'name']):
        if p.info['name'].lower() == process_name.lower():
            pid = p.info['pid']
            proc = p
            break
            
    if pid == 0:
        print(f"Error: Process '{process_name}' not found!")
        return False
        
    print(f"Found {process_name} (PID: {pid})")
    
    # Find module using psutil
    base_addr = 0
    size = 0
    found = False
    
    try:
        for map in proc.memory_maps(grouped=False):
            if module_name.lower() in map.path.lower():
                base_addr = int(map.addr, 16)
                # We need the full size. psutil gives regions. 
                # Let's just take the base address and find the size from PE header or try to read a large chunk
                print(f"Found module at: {map.path} (Base: 0x{base_addr:X})")
                found = True
                break
    except Exception as e:
        print(f"Error listing modules: {e}")
        # Fallback to pure ctypes if access denied?
        pass

    if not found:
        print(f"Error: Module '{module_name}' not found in process!")
        return False

    # Read memory
    kernel32 = ctypes.WinDLL('kernel32')
    h_process = kernel32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, False, pid)
    
    if not h_process:
        print("Error: Failed to open process handle!")
        return False

    # Read PE header to get SizeOfImage
    header_buf = ctypes.create_string_buffer(4096) # Read first page
    bytes_read = ctypes.c_size_t(0)
    
    if kernel32.ReadProcessMemory(h_process, ctypes.c_void_p(base_addr), header_buf, 4096, ctypes.byref(bytes_read)):
        # Parse SizeOfImage from Optional Header
        # DOS Header: e_lfanew at offset 0x3C (4 bytes)
        e_lfanew = int.from_bytes(header_buf[0x3C:0x40], 'little')
        # Optional Header Signature is 4 bytes "PE\0\0"
        # File Header is 20 bytes
        # Optional Header starts at e_lfanew + 4 + 20
        # SizeOfImage is at offset 56 (0x38) inside Optional Header (32-bit)
        size_of_image_offset = e_lfanew + 4 + 20 + 56
        size = int.from_bytes(header_buf[size_of_image_offset:size_of_image_offset+4], 'little')
        print(f"Read SizeOfImage from header: 0x{size:X}")
    else:
        print("Error: Failed to read PE header! Defaulting to 10MB.")
        size = 10 * 1024 * 1024

    # Read full module
    full_buf = ctypes.create_string_buffer(size)
    if kernel32.ReadProcessMemory(h_process, ctypes.c_void_p(base_addr), full_buf, size, ctypes.byref(bytes_read)):
        with open(output_file, "wb") as f:
            f.write(full_buf)
        print(f"Successfully dumped module to {output_file}")
        kernel32.CloseHandle(h_process)
        return True
    else:
        print(f"Error: Failed to dump module! (SysErr: {ctypes.get_last_error()})")
        kernel32.CloseHandle(h_process)
        return False

if __name__ == "__main__":
    dump_module("hl.exe", "hw.dll", "hw_dump.dll")
