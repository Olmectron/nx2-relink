"""Hook files: code of your own that a port loads from the card as it starts, with nothing built again.

  python nxrelink.py hooks build <file.c> [<file.c> ...] -o <name>.nxh  --port <name>
                                                                        (or  --title <16 hex digits> --game-version <text>)
  python nxrelink.py hooks info <name>.nxh

A source is the same one that can be built into a port (make --also): it includes runtime/nx2_hook.h and names its function
with NX2_HOOKS(fn). Built here it becomes one file, <name>.nxh, to be put in the port's folder on the card under mods/Code/
(a mod carries it as files/Code/<name>.nxh): the port maps it and calls its functions before the game starts.

A hook file is made for ONE version of ONE game, since everything in it is a place in that game's code: --port takes the
game's number and version from a port built on this computer, and the file says both. A port of another game, or of
another version of it, refuses the file and says so in its log. In such a file there is no C library: memcpy, memset,
memcmp, strlen, strcmp and math.h's functions are there, and nx2_hook.h's; nothing that opens a file or prints.
"""
import json
import os
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__))).replace('\\', '/')
sys.path.insert(0, f'{ROOT}/tools')
import build  # noqa: E402  (the compiler and its flags, as a port's own code is built with)

MAGIC, FORMAT, HEADER, MOST = b'NXH1', 1, 0x50, 4 << 20
R_AARCH64_RELATIVE = 1027


def option(args, name, default=None):
    if name in args:
        i = args.index(name)
        if i + 1 >= len(args):
            sys.exit(f'{name} needs a value.')
        v = args[i + 1]
        del args[i:i + 2]
        return v
    return default


def elf_parts(path):
    """What of the linked file a port needs: its three stretches, its symbols of the layout, its relocations."""
    d = open(path, 'rb').read()
    if d[:4] != b'\x7fELF' or d[4] != 2:
        sys.exit(f'{path}: not what the linker should have written.')
    shoff, shentsize, shnum, shstrndx = struct.unpack_from('<Q', d, 0x28)[0], *struct.unpack_from('<HHH', d, 0x3A)
    sections = []
    for k in range(shnum):
        name, kind, flags, addr, off, size, link, info, align, entsize = struct.unpack_from('<IIQQQQIIQQ', d, shoff + k * shentsize)
        sections.append({'name': name, 'type': kind, 'addr': addr, 'off': off, 'size': size, 'link': link, 'entsize': entsize})
    names = d[sections[shstrndx]['off']:sections[shstrndx]['off'] + sections[shstrndx]['size']]
    for s in sections:
        s['name'] = names[s['name']:names.index(b'\0', s['name'])].decode()
    symbols = {}
    for s in sections:
        if s['type'] == 2:  # SHT_SYMTAB
            strings = sections[s['link']]
            for at in range(s['off'], s['off'] + s['size'], 24):
                name, _, _, _, value, _ = struct.unpack_from('<IBBHQQ', d, at)
                end = d.index(b'\0', strings['off'] + name)
                symbols[d[strings['off'] + name:end].decode()] = value
    relocs = []
    for s in sections:
        if s['type'] == 4 and s['size']:  # SHT_RELA
            for at in range(s['off'], s['off'] + s['size'], 24):
                offset, info, addend = struct.unpack_from('<QQq', d, at)
                if info & 0xFFFFFFFF == 0:
                    continue
                if info & 0xFFFFFFFF != R_AARCH64_RELATIVE:
                    sys.exit(f'{path}: a relocation of a kind a port doesn\'t do ({info & 0xFFFFFFFF}, at {offset:#x}): the source uses something '
                             'that isn\'t in the file itself (a function of the C library that a hook file hasn\'t?).')
                relocs.append((offset, addend))
    image = bytearray(symbols['__nxh_bss'] - 0x1000)
    for s in sections:
        if s['type'] == 1 and s['addr'] >= 0x1000 and s['size'] and s['addr'] + s['size'] <= symbols['__nxh_bss']:  # SHT_PROGBITS, loaded
            image[s['addr'] - 0x1000:s['addr'] - 0x1000 + s['size']] = d[s['off']:s['off'] + s['size']]
    return image, symbols, sorted(set(relocs))


def cmd_build(args):
    out, port = option(args, '-o'), option(args, '--port')
    title, version = option(args, '--title'), option(args, '--game-version')
    sources = [os.path.abspath(a).replace('\\', '/') for a in args]
    if not sources or not out or not out.lower().endswith('.nxh') or any(not os.path.isfile(s) for s in sources):
        sys.exit(__doc__)
    if port:
        beside = f'{ROOT}/build/{port}_nsp/info.json'
        if not os.path.isfile(beside):
            sys.exit(f'{port}: no port of that name is built here. Give the game\'s own instead: --title <16 hex digits> --game-version <text>.')
        info = json.load(open(beside, encoding='utf-8'))
        title, version = title or info.get('title_id'), version if version is not None else str(info.get('version') or '')
    try:
        number = int(title or '', 16)
    except ValueError:
        number = 0
    if not number:
        sys.exit('Which game is this for? --port <a port built here>, or --title <16 hex digits> --game-version <text>: a hook file is '
                 'made for one version of one game, and a port of any other refuses it.')
    version = (version or '').encode('utf-8')
    if len(version) > 31:
        sys.exit('--game-version is at most 31 characters.')
    work = f'{ROOT}/build/nxh/{os.path.basename(out)[:-4]}'
    os.makedirs(work, exist_ok=True)
    flags = ['-O2', '-Wall', '-ffunction-sections', '-fdata-sections', '-fPIC', '-fno-plt', '-fvisibility=hidden', '-ffreestanding', '-fno-builtin-memset',
             '-fno-math-errno', '-fno-stack-protector', '-fno-asynchronous-unwind-tables', '-march=armv8-a+crc+crypto', '-mtune=cortex-a57',
             '-DNXH', f'-I{ROOT}/runtime']
    objects = []
    for k, s in enumerate([f'{ROOT}/tools/nxh_start.c'] + sources):
        o = f'{work}/{k}_{os.path.basename(s)}.o'
        build.run([build.GCC, '-c', *flags, s, '-o', o])
        objects.append(o)
    elf = f'{work}/linked.elf'
    build.run([build.GCC, '-nostdlib', '-nostartfiles', '-pie', '-Wl,--no-dynamic-linker', '-Wl,-z,norelro', '-Wl,--gc-sections', '-Wl,--no-undefined',
               '-Wl,-T', f'-Wl,{ROOT}/tools/nxh.ld', '-o', elf, *objects, '-lm', '-lgcc', f'-Wl,-Map,{work}/linked.map'])
    image, sym, relocs = elf_parts(elf)
    ro, data, bss, end, entry = (sym[n] for n in ('__nxh_rodata', '__nxh_data', '__nxh_bss', '__nxh_end', 'nxh_install'))
    for offset, addend in relocs:
        if not (data <= offset and offset + 8 <= bss and 0x1000 <= addend < end):
            sys.exit(f'{out}: an address to be settled outside the file\'s data ({offset:#x} -> {addend:#x}): not a file a port would take.')
    table = b''.join(struct.pack('<II', offset, addend) for offset, addend in relocs)
    # the header: what a port checks before it does anything with the file (runtime/hook.c reads the same)
    size = HEADER + len(image) + len(table)
    head = struct.pack('<4sIQ32sIIIIIIII', MAGIC, FORMAT, number, version, size, len(image), ro, data, end - bss, entry, len(relocs), 0)
    assert len(head) == HEADER
    if size > MOST:
        sys.exit(f'{out}: {size >> 10} KB, and a port takes a hook file of 4 MB at most.')
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    open(out, 'wb').write(head + bytes(image) + table)
    print(f'{out}: {size} bytes ({ro - 0x1000} of code, {end - bss} of bss, {len(relocs)} addresses to settle), from {len(sources)} source(s); for the game '
          f'{number:016x}{", version " + version.decode() if version else ""}. It goes in the port\'s folder on the card, under mods/Code/ '
          '(in a mod: files/Code/).')


def read_header(d):
    """A hook file's header, or the reason it isn't one."""
    if len(d) < HEADER or d[:4] != MAGIC:
        return None, 'not a hook file'
    magic, fmt, number, version, size, image, ro, data, bss, entry, relocs, _ = struct.unpack_from('<4sIQ32sIIIIIIII', d, 0)
    if fmt != FORMAT:
        return None, f'made in a form this doesn\'t know ({fmt})'
    if size != len(d) or size != HEADER + image + relocs * 8:
        return None, 'its size isn\'t what it says'
    return {'title_id': f'{number:016x}', 'version': version.rstrip(b'\0').decode('utf-8', 'replace'), 'size': size, 'code': ro - 0x1000, 'bss': bss,
            'relocs': relocs}, None


def cmd_info(args):
    if len(args) != 1 or not os.path.isfile(args[0]):
        sys.exit(__doc__)
    head, why = read_header(open(args[0], 'rb').read())
    if not head:
        sys.exit(f'{args[0]}: {why}.')
    print(f'{args[0]}: for the game {head["title_id"]}{", version " + head["version"] if head["version"] else ""}; {head["size"]} bytes, '
          f'{head["code"]} of code, {head["bss"]} of bss, {head["relocs"]} addresses to settle')


def main(args):
    if args[:1] == ['build']:
        return cmd_build(args[1:])
    if args[:1] == ['info']:
        return cmd_info(args[1:])
    sys.exit(__doc__)


if __name__ == '__main__':
    main(sys.argv[1:])
