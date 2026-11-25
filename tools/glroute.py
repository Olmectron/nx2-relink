"""Writes runtime/glvk_route.h: for each OpenGL ES function the port's NVN side (runtime/nvn2.c) calls, the declaration of the
port's own function of that name over Vulkan (vk_glX, in runtime/glvk.c), and a macro by which a call of glX goes to it when the
port draws with Vulkan (nx2_vulkan) and to the host's OpenGL when it doesn't.

    python tools/glroute.py
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import build  # noqa: E402  (where devkitPro is)

DKP = build.DKP
source = open(f'{ROOT}/runtime/nvn2.c', encoding='utf-8').read()
used = sorted(set(re.findall(r'\bgl[A-Z][A-Za-z0-9]*\b', source)))
header = open(f'{DKP}/portlibs/switch2/include/GLES3/gl32.h', encoding='utf-8').read()
out = ['// Written by tools/glroute.py: the OpenGL ES functions runtime/nvn2.c calls, each with the port\'s own over Vulkan (runtime/glvk.c).',
       '#pragma once', 'extern int nx2_vulkan;']
missing = []
for name in used:
    m = re.search(r'GL_APICALL\s+(.+?)\s*GL_APIENTRY\s+' + name + r'\s*\(([^;]*)\);', header)
    if not m:
        missing.append(name)
        continue
    ret, args = m.group(1).strip(), ' '.join(m.group(2).split())
    out.append(f'{ret} vk_{name}({args});')
    out.append(f'#define {name}(...) (nx2_vulkan ? vk_{name}(__VA_ARGS__) : {name}(__VA_ARGS__))')
open(f'{ROOT}/runtime/glvk_route.h', 'w', newline='\n').write('\n'.join(out) + '\n')
print(f'{len(used) - len(missing)} functions routed; not in gl32.h (asked of EGL by name, or an extension): {" ".join(missing)}')
