import json
import base64
from PIL import Image
import struct

def convert_rgb565_to_rgb888(data):
    rgb888 = bytearray()
    for i in range(0, len(data), 2):
        # RGB565: RRRRRGGG GGGBBBBB
        pixel = struct.unpack('<H', data[i:i+2])[0]
        r = (pixel >> 11) & 0x1F
        g = (pixel >> 5) & 0x3F
        b = pixel & 0x1F
        
        # Scale to 8-bit
        r = (r * 255) // 31
        g = (g * 255) // 63
        b = (b * 255) // 31
        
        rgb888.extend([r, g, b])
    return bytes(rgb888)

def main():
    with open('screenshot_raw.json', 'r') as f:
        resp = json.load(f)
    
    b64_data = resp['result']['data']
    raw_data = base64.b64decode(b64_data)
    
    width = resp['result']['width']
    height = resp['result']['height']
    
    rgb888_data = convert_rgb565_to_rgb888(raw_data)
    
    img = Image.frombytes('RGB', (width, height), rgb888_data)
    img.save('screenshot.png')
    print(f"Saved screenshot.png ({width}x{height})")

if __name__ == '__main__':
    main()
