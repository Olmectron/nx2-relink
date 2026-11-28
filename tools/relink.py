"""The relinker: a game's NSO modules made into objects that link into the port's own executable.

A Switch 2 homebrew program (kaiHEN) can't map code at run time the way the console's loader does, and an NRO has one executable
segment and one writable one. So each module's code and read-only data go, as they are, into the port's .text, and its data and bss
into the port's .data. What depended on the distance between the two is rewritten here, at build time:

  adrp xN, <page in data>     relocated by the linker to where the data now is (the low 12 bits that follow stay: both parts keep
                              their page alignment)
  adr  xN, <address in data>  out of adr's reach now: a branch to a thunk that makes the address with adrp + add
  svc  #n                     a branch to a thunk that asks the runtime (nx2_svc) first: it answers itself, or lets the call through

Code and read-only data stay together because compilers reach one from the other with fixed distances (jump tables, exception
tables). Pointers stored in data are the loader's to fix (runtime/loader.c), from the modules' own relocation tables.

  python tools/relink.py <out dir> <name>=<file.nso> [...]     in load order: main, subsdk0.., sdk (rtld is not used: the loader is its stand-in)
      --whole              every module kept whole: its image as it is, for the loader to map at run time (runtime/whole.c)
      --whole=main,sdk     those modules only, the others relinked. kaiHEN's loader service maps eight images for a program, the
                           program's own among them, and a game that loads modules of its own as it runs needs some of the eight
      --whole=auto         a Switch 2 game's default (tools/build.py): all of them, as on the console, unless the game loads
                           modules of its own as it runs; then main, sdk and any with thread-local storage of its own (below)
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nso import Nso  # noqa: E402

PAGE = 0x1000
# The one system call left as the game has it, the runtime not asked: SynchronizePreemptionState. The SDK counts, in a thread's own
# area (its TLS, at 0x100), the stretches where the thread isn't to be taken off its core; a thread the kernel wanted to take off in
# one is "pinned" instead, and told (a flag beside the count); at the stretch's end it makes this call, and until it has, the kernel
# lets it make no other: any other ends the program ("undefined system call", error 2168-0008 in the console's reports). The
# runtime's way in (svc_entry.s, svc.c) writes a line in the log at a call's first use, which is a system call: a game,
# whose SDK does this in a hundred places, was ended by it at a different moment each run, 6 to 10 seconds in (2026-11-24).
NOT_ASKED = 0x36


def sx(v, bits):
    return v - (1 << bits) if v >> (bits - 1) & 1 else v


def relink(name, nso, out):
    img = bytearray(nso.image())
    data_off = nso.data_off
    end = (nso.end + PAGE - 1) & ~(PAGE - 1)
    text_len = len(nso.text)
    # One game's own instruction changed, where what is there is that game's (the words and the text checked first).
    # A game 4.0.1: its heap "UIHeap_Sys" is 7 MB, and in a port for a Switch 2 it is full, 16 KB short, when the stack
    # of "UILoadThread" is asked of it; the thread is then made with no stack and the game ends (log, 2026-11-23). Which of this
    # port's answers makes it fuller than on a Switch isn't known. 7 MB and 128 KB: a way around, not the cause mended.
    # (Not 8 MB: the heap it is made in, "TaskHeap", is 288 MB, and "UIHeap_Root", made next in the same, is 280.75; with 8 MB
    # taken that one wasn't made at all, and the game put an 80 MB heap's contents into this one; log, 2026-11-23. There are 256 KB
    # between the three.)
    for module, at, old, new, text_at, text in (('main', 0x3fec34, 0x320c0be0, 0x52a00e40, 0xee7505, b'UIHeap_Sys\0'),):
        if name == module and at + 4 <= text_len and struct.unpack_from('<I', img, at)[0] == old and img[text_at:text_at + len(text)] == text:
            struct.pack_into('<I', img, at, new)
            print(f'{name}: the instruction at {at:#x} changed ({old:08x} to {new:08x}), for "{text[:-1].decode()}"')
    words = struct.unpack_from(f'<{text_len // 4}I', img, 0)
    relocs, thunks = [], []
    stats = {'adrp': 0, 'adr': 0, 'svc': 0, 'literal': 0}
    svc_seen = {}
    for i, w in enumerate(words):
        a = i * 4
        if w & 0x9F000000 == 0x90000000:  # adrp
            target = (a & ~(PAGE - 1)) + (sx((w >> 5 & 0x7FFFF) << 2 | (w >> 29 & 3), 21) << 12)
            if data_off <= target < end:
                relocs.append(f'    .reloc g_{name}_text+{a:#x}, R_AARCH64_ADR_PREL_PG_HI21, g_{name}_data+{target - data_off:#x}')
                stats['adrp'] += 1
        elif w & 0x9F000000 == 0x10000000:  # adr
            target = a + sx((w >> 5 & 0x7FFFF) << 2 | (w >> 29 & 3), 21)
            if data_off <= target < end:
                k, r = len(thunks), w & 31
                sym = f'g_{name}_data+{target - data_off:#x}'
                thunks.append(f'.Lt_{name}_{k}:\n    adrp x{r}, {sym}\n    add x{r}, x{r}, :lo12:{sym}\n    b g_{name}_text+{a + 4:#x}')
                struct.pack_into('<I', img, a, 0x14000000)
                relocs.append(f'    .reloc g_{name}_text+{a:#x}, R_AARCH64_JUMP26, .Lt_{name}_{k}')
                stats['adr'] += 1
        elif w & 0xFFE0001F == 0xD4000001 and w >> 5 & 0xFFFF != NOT_ASKED:  # svc
            n = w >> 5 & 0xFFFF
            k = len(thunks)
            thunks.append(f'.Lt_{name}_{k}:\n    stp x16, x30, [sp, #-16]!\n    mov w16, #{n:#x}\n    bl nx2_svc_entry\n'
                          f'    ldp x16, x30, [sp], #16\n    b g_{name}_text+{a + 4:#x}')
            struct.pack_into('<I', img, a, 0x14000000)
            relocs.append(f'    .reloc g_{name}_text+{a:#x}, R_AARCH64_JUMP26, .Lt_{name}_{k}')
            stats['svc'] += 1
            svc_seen[n] = svc_seen.get(n, 0) + 1
        elif w & 0x3B000000 == 0x18000000:  # ldr (literal), all its forms
            target = a + (sx(w >> 5 & 0x7FFFF, 19) << 2)
            if data_off <= target < end:
                stats['literal'] += 1
    open(f'{out}/{name}_tr.bin', 'wb').write(img[:data_off])
    open(f'{out}/{name}_data.bin', 'wb').write(img[data_off:])
    bss = end - len(img)
    nl = '\n'
    s = f'''// {name}: made by tools/relink.py from {os.path.basename(nso.path)}. Not to be edited.
    .section .text.g_{name}, "ax", %progbits
    .balign {PAGE:#x}
    .global g_{name}_text
g_{name}_text:
    .incbin "{name}_tr.bin"
    .balign 4
{nl.join(thunks)}
{nl.join(relocs)}

    .section .data.g_{name}, "aw", %progbits
    .balign {PAGE:#x}
    .global g_{name}_data
g_{name}_data:
    .incbin "{name}_data.bin"
    .space {bss:#x}
    .space {PAGE:#x}    // the page after a module is where the console's loader puts the program's arguments: none (zeros)
'''
    open(f'{out}/{name}.s', 'w', newline='\n').write(s)
    print(f'{name}: code and read-only data {data_off:#x} bytes, data and bss {end - data_off:#x}; {stats["adrp"]} adrp and {stats["adr"]} adr '
          f'into the data moved, {stats["svc"]} system calls ({", ".join(f"{k:02x}" for k in sorted(svc_seen))})')
    if stats['literal']:
        print(f'  WARNING: {stats["literal"]} literal loads reach into the data: not handled yet, they will read the wrong place')
    return end


def whole(name, nso, out):
    """A module kept whole (--whole): its image as it is in the NSO, for the loader to put in memory of its own at run time, code and
    data together and at the distances they were built with (runtime/loader.c, nx2_map_modules). Nothing is rewritten here; the
    places of its system calls are listed, for the loader to turn into branches to thunks it makes beside the module."""
    img = bytes(nso.image())
    end = (nso.end + PAGE - 1) & ~(PAGE - 1)
    text_len = len(nso.text)
    words = struct.unpack_from(f'<{text_len // 4}I', img, 0)
    svcs = [(i * 4, w >> 5 & 0xFFFF) for i, w in enumerate(words) if w & 0xFFE0001F == 0xD4000001]
    # (kept packed, one Zstandard frame: the runtime unpacks it once, into the memory the loader service maps, whole.c. As it was,
    # the program held each image and the runtime a copy of it: a game's modules twice, some 95 MB that the game then lacked.
    # NX2_RAW_MODULES=1 in the environment keeps them as they are.)
    # (And the packed frames are not in the program either, but after it in its file, packed.bin, which tools/build.py puts there:
    # the homebrew loader reads into memory the program alone, as its header gives its size, and the frames are read from the card
    # as the modules are mapped. In the program they were some 28 MB that stayed in memory, of no use once unpacked. 2026-11-27.
    # NX2_PACKED_INSIDE=1 keeps them in the program.)
    packed = 0
    data = img
    if not os.environ.get('NX2_RAW_MODULES'):
        import zstandard
        data = zstandard.ZstdCompressor(level=19, threads=-1, write_checksum=True).compress(img)
        packed = len(data)
    at = -1
    if packed and not os.environ.get('NX2_PACKED_INSIDE'):
        at = sum(len(b) for b in OUTSIDE)
        OUTSIDE.append(data)
        data = b'\0' * 8  # (the image's symbol stays, a mark that the module is kept whole)
    open(f'{out}/{name}_img.bin', 'wb').write(data)
    open(f'{out}/{name}.s', 'w', newline='\n').write(f'''// {name}: made by tools/relink.py --whole from {os.path.basename(nso.path)}. Not to be edited.
    .section .rodata.g_{name}, "a", %progbits
    .balign {PAGE:#x}
    .global g_{name}_image
g_{name}_image:
    .incbin "{name}_img.bin"
''')
    print(f'{name}: kept whole, {len(img):#x} bytes{f", {packed >> 10} KB packed" if packed else ""}{" (after the program in its file)" if at >= 0 else ""} (code {text_len:#x}, read-only data from {nso.ro_off:#x}, '
          f'data from {nso.data_off:#x}, ends at {end:#x}); {len(svcs)} system calls ({", ".join(f"{k:02x}" for k in sorted(set(n for _, n in svcs)))})')
    return end, svcs, packed, at


OUTSIDE = []  # (the packed images that go after the program in its file, in order: packed.bin)


def has_own_tls(nso):
    """Whether a module has thread-local storage that is found at run time (a relocation of the TLS kinds: TLSDESC and its kin)."""
    img = nso.image()
    tags = nso.dyn_entries()
    for table, size_tag in ((7, 8), (23, 2)):  # (DT_RELA and its size; DT_JMPREL and its)
        if table in tags and size_tag in tags:
            for k in range(tags[size_tag] // 24):
                if 1028 <= struct.unpack_from('<Q', img, tags[table] + k * 24 + 8)[0] & 0xFFFFFFFF <= 1031:
                    return True
    return False


def loads_modules(nso):
    """Whether a module asks for the SDK's function that loads a module as the game runs (nn::ro::LoadModule)."""
    return any(s[0].startswith('_ZN2nn2ro10LoadModule') and not s[1] for s in nso.symbols())


def choose_whole(modules):
    """--whole=auto: which of a Switch 2 game's modules are kept whole, and why, in words. modules: [(name, Nso)].

    Whole is how a module is on its console, and nothing has to be found and rewritten in it. What stands against keeping all of
    them whole is that kaiHEN's loader service maps eight images for a program (measured, 2026-11-25): the program's own, and
    seven more, which the game's modules kept whole and the modules the game loads as it runs (nn::ro) share.
      - A game that loads none: all of its modules, if they are seven or fewer.
      - A game that loads some: those that have to be whole, and the rest relinked, to leave room. Three kinds have to be, each met
        with a game (logs, 2026-11-25): a module with thread-local storage of its own (its subsdk2, "dlss":
        relinked, the game ended in its initializers with a data abort, whichever others were whole); main and sdk, because the
        game throws C++ exceptions, and the SDK's unwinder finds a module by where its code begins in the memory map, which a
        relinked module's does not say (the throw is in main, the unwinder and the first frames it meets are in sdk: with either
        relinked the game ended at its first throw).
    """
    names = [n for n, _ in modules]
    if not any(loads_modules(nso) for _, nso in modules) and len(modules) <= 7:
        return set(names), 'the game loads no modules as it runs: all kept whole, as on its console'
    keep = {n for n in names if n in ('main', 'sdk')} | {n for n, nso in modules if has_own_tls(nso)}
    return keep, (f'the game loads modules as it runs, and the loader service maps eight images in all: {", ".join(n for n in names if n in keep)} kept whole '
                  f'(main and sdk for C++ exceptions, the others for thread-local storage of their own), the rest relinked; {7 - len(keep)} left for the game\'s own')


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    args = sys.argv[1:]
    # (which modules are kept whole: all with --whole, those named with --whole=a,b)
    choice = next((a for a in args if a == '--whole' or a.startswith('--whole=')), None)
    if choice:
        args.remove(choice)
    named = set(choice[8:].split(',')) if choice and '=' in choice else None
    if named == {'auto'}:
        named, why = choose_whole([(a.split('=', 1)[0], Nso(a.split('=', 1)[1])) for a in args[1:]])
        print(f'whole: {why}')
    out = args[0]
    os.makedirs(out, exist_ok=True)
    rows, externs, images, tables = [], [], [], []
    for arg in args[1:]:
        name, path = arg.split('=', 1)
        nso = Nso(path)
        if choice and (named is None or name in named):
            end, svcs, packed, at = whole(name, nso, out)
            externs.append(f'extern const unsigned char g_{name}_image[];')
            tables.append(f'static const u32 svcs_{name}[] = {{{", ".join(f"{a:#x}, {n:#x}" for a, n in svcs) or "0"}}};')
            images.append(f'    {{g_{name}_image, {nso.data_off + len(nso.data):#x}, {len(nso.text):#x}, {nso.ro_off:#x}, {len(nso.ro):#x}, svcs_{name}, {len(svcs)}, {packed:#x}, {max(at, 0):#x}, {int(at >= 0)}}},')
            text, data = '0', '0'
        else:
            end = relink(name, nso, out)
            externs.append(f'extern unsigned char g_{name}_text[], g_{name}_data[];')
            text, data = f'g_{name}_text', f'g_{name}_data'
            images.append('    {0},')
        rows.append(f'    {{"{name}", {text}, {data}, {nso.data_off:#x}, {end:#x}, {nso.mod0:#x}, {nso.dynamic:#x}, '
                    f'{nso.symtab:#x}, {nso.nsyms}, {nso.strtab:#x}, {nso.strsz:#x}, '
                    f'{nso.module_object:#x}}},')
    nl = '\n'
    open(f'{out}/modules.c', 'w', newline='\n').write(f'''// made by tools/relink.py: the game's modules, in load order
#include "nx2.h"
{nl.join(externs)}
{nl.join(tables)}
Nx2Module nx2_modules[] = {{
{nl.join(rows)}
}};
const unsigned nx2_module_count = {len(rows)};
// (the modules kept whole, for the loader to map at run time, one row for each module in the order above: a row of zeros is a
// module that was relinked. nx2_whole: how many are kept whole)
const Nx2Image nx2_images[] = {{
{nl.join(images)}
}};
const int nx2_whole = {sum(1 for i in images if i.strip() != '{0},')};
// (how many bytes of packed images follow the program in its file: tools/build.py puts them there, packed.bin, then a mark)
const unsigned long long nx2_outside_size = {sum(len(b) for b in OUTSIDE):#x};
''')
    # (written every time, empty if there are none: tools/build.py puts it after the program as it is)
    open(f'{out}/packed.bin', 'wb').write(b''.join(OUTSIDE))


if __name__ == '__main__':
    main()
