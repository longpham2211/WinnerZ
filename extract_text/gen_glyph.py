import os

def generate():
    m = {}
    with open('/mnt/e/thuvien_winnerz/my-lib/agl-aglfn-master/glyphlist.txt', 'r') as f:
        for line in f:
            line = line.strip()
            if line and not line.startswith('#'):
                parts = line.split(';')
                if len(parts) >= 2:
                    name = parts[0].strip()
                    code_part = parts[1].strip().split(' ')[0] # just take the first code pointer
                    m[name] = int(code_part, 16)
    
    with open('/mnt/e/thuvien_winnerz/my-lib/extract_text/adobe_glyph_list.hpp', 'w') as out:
        out.write('#pragma once\n')
        out.write('#include <string>\n')
        out.write('#include <unordered_map>\n\n')
        out.write('namespace WinExtract {\n')
        out.write('inline const std::unordered_map<std::string, int>& get_adobe_glyph_map() {\n')
        out.write('    static const std::unordered_map<std::string, int> glyph_map = {\n')
        for name, code in m.items():
            out.write(f'        {{"{name}", {code}}},\n')
        out.write('    };\n')
        out.write('    return glyph_map;\n')
        out.write('}\n}\n')

if __name__ == '__main__':
    generate()