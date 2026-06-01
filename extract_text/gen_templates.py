import freetype
import os

fonts = [
    'C:/Windows/Fonts/arial.ttf',
    'C:/Windows/Fonts/times.ttf',
    'C:/Windows/Fonts/cour.ttf',
    'C:/Windows/Fonts/tahoma.ttf',
    'C:/Windows/Fonts/segoeui.ttf'
]

chars_ascii = '0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ!"#$%&\'()*+,-./:;<=>?@[\\]^_`{|}~ '
chars_vi = "áàảãạâấầẩẫậăắằẳẵặđéèẻẽẹêếềểễệíìỉĩịóòỏõọôốồổỗộơớờởỡợúùủũụưứừửữựýỳỷỹỵÁÀẢÃẠÂẤẦẨẪẬĂẮẰẲẴẶĐÉÈẺẼẸÊẾỀỂỄỆÍÌỈĨỊÓÒỎÕỌÔỐỒỔỖỘƠỚỜỞỠỢÚÙỦŨỤƯỨỪỬỮỰÝỲỶỸỴ"
chars_cyrillic = "АБВГДЕЁЖЗИЙКЛМНОПРСТУФХЦЧШЩЪЫЬЭЮЯабвгдеёжзийклмнопрстуфхцчшщъыьэюя"
chars_greek = "ΑΒΓΔΕΖΗΘΙΚΛΜΝΞΟΠΡΣΤΥΦΧΨΩαβγδεζηθικλμνξοπρστυφχψω"
chars_latin_ext = "ÄÖÜäöüßÇçÑñÉéÈèÊêËëÎîÏïÔôŒœÙùÛûŸÿÆæ"
chars_thai = "กขฃคฅฆงจฉชซฌญฎฏฐฑฒณดตถทธนบปผฝพฟภมยรลวศษสหฬอฮะัาำิีึืุูเแโใไๆ็่้๊๋์"

chars = chars_ascii + chars_vi
chars = "".join(dict.fromkeys(chars))

templates = []
for font_path in fonts:
    if not os.path.exists(font_path): continue
    try:
        face = freetype.Face(font_path)
        face.set_pixel_sizes(0, 32)
        for c in chars:
            face.load_char(c, freetype.FT_LOAD_RENDER | freetype.FT_LOAD_TARGET_MONO)
            bitmap = face.glyph.bitmap
            if bitmap.width == 0 or bitmap.rows == 0: continue
            
            blocks = [0]*16
            for y in range(32):
                for x in range(32):
                    u = (x + 0.5) / 32.0
                    v = (y + 0.5) / 32.0
                    bx = int(u * bitmap.width)
                    by = int(v * bitmap.rows)
                    bx = min(max(bx, 0), bitmap.width - 1)
                    by = min(max(by, 0), bitmap.rows - 1)
                    
                    byte_idx = by * bitmap.pitch + (bx >> 3)
                    bit = (bitmap.buffer[byte_idx] >> (7 - (bx & 7))) & 1
                    
                    if bit:
                        pixel_idx = y * 32 + x
                        block_idx = pixel_idx // 64
                        bit_idx = pixel_idx % 64
                        blocks[block_idx] |= (1 << bit_idx)
            
            contours = face.glyph.outline.n_contours
            templates.append((c.encode('utf-8'), contours, blocks))
    except Exception as e:
        print("Error with", font_path, e)

# Add special test font if exists
try:
    if os.path.exists('../font45.cff'):
        face = freetype.Face('../font45.cff')
        face.set_pixel_sizes(0, 32)
        gid_map = {'gid00136': '5', 'gid00131': '0', 'gid00133': '2'}
        for gname, c in gid_map.items():
            gid = face.get_name_index(gname.encode('ascii'))
            face.load_glyph(gid, freetype.FT_LOAD_RENDER | freetype.FT_LOAD_TARGET_MONO)
            bitmap = face.glyph.bitmap
            blocks = [0]*16
            for y in range(32):
                for x in range(32):
                    u = (x + 0.5) / 32.0
                    v = (y + 0.5) / 32.0
                    bx = int(u * bitmap.width)
                    by = int(v * bitmap.rows)
                    bx = min(max(bx, 0), bitmap.width - 1)
                    by = min(max(by, 0), bitmap.rows - 1)
                    byte_idx = by * bitmap.pitch + (bx >> 3)
                    bit = (bitmap.buffer[byte_idx] >> (7 - (bx & 7))) & 1
                    if bit:
                        pixel_idx = y * 32 + x
                        block_idx = pixel_idx // 64
                        bit_idx = pixel_idx % 64
                        blocks[block_idx] |= (1 << bit_idx)
            contours = face.glyph.outline.n_contours
            templates.append((c.encode('utf-8'), contours, blocks))
except Exception as e:
    pass

with open(os.path.join(os.path.dirname(__file__), 'micro_ocr_templates.hpp'), 'w', encoding='utf-8') as f:
    f.write('#pragma once\n#include <stdint.h>\n\n')
    f.write('namespace WinExtract {\n\n')
    f.write('struct MicroOcrTemplate {\n    char utf8_char[8];\n    int contours;\n    uint64_t pixels[16];\n};\n\n')
    f.write(f'const int NUM_MICRO_OCR_TEMPLATES = {len(templates)};\n\n')
    f.write('const MicroOcrTemplate MICRO_OCR_TEMPLATES[] = {\n')
    for c_bytes, contours, blocks in templates:
        b0,b1,b2,b3,b4,b5,b6,b7,b8,b9,b10,b11,b12,b13,b14,b15 = blocks
        c_esc = '"' + "".join(f"\\x{b:02x}" for b in c_bytes) + '"'
        f.write(f'    {{ {c_esc}, {contours}, {{ {b0}ULL,{b1}ULL,{b2}ULL,{b3}ULL,{b4}ULL,{b5}ULL,{b6}ULL,{b7}ULL,{b8}ULL,{b9}ULL,{b10}ULL,{b11}ULL,{b12}ULL,{b13}ULL,{b14}ULL,{b15}ULL }} }},\n')
    f.write('};\n\n} // namespace WinExtract\n')

print(f"Generated {len(templates)} templates with bitwise packing.")
