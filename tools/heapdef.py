"""What a game's own table of heaps says of the memory it keeps apart from its heap.

Some games size themselves by the console's whole memory: asked how much there is, they take as their heap what is left of it
after a stretch of address space they map into as they go, and their fixed heaps only fit in what a console's full figure leaves.
Told of less (what a port really has), such a game is short at once: one ended at its start, its graphics given a heap of no
size, when told of 7800 MB, and of 8400, and ran when told of the console's own 8992 (found on a Switch 2, 2026-11-25).
A game that takes all it is told of as its heap is the opposite case: told of more than there is, it is refused. Nothing a game
asks of the system tells the two apart before it has sized itself, so the game's own table is read when its port is made.

The table is a file of the game's, System/HeapSize/<...>.HeapDef.byml (packed, with the dictionary the game keeps in
Pack/ZsDic.pack.zs): a BYML map "Heaps" of name -> {Size (KB), Parent}. The heap "VirtualAddressMemory" is that stretch.

    apart(files) -> MB, or None when the game has no such table or no such heap
    python tools/heapdef.py <dump>        the table, printed

files: {path: (size, a function giving the file's bytes in pieces)}, as tools/game2.py has a dump's.
"""
import struct
import sys

CONSOLE = 8992  # MB a Switch 2 game has on its console, as it sees it: 9120 less the 128 said to be in use (runtime/svc.c)


def whole(files, name):
    size, pieces = files[name]
    return b''.join(bytes(x) for x in pieces())


def archive(b):
    """A SARC archive's files: {name: bytes}."""
    if b[:4] != b'SARC':
        return {}
    e = '<' if struct.unpack_from('<H', b, 6)[0] == 0xFEFF else '>'
    data = struct.unpack_from(e + 'I', b, 0xC)[0]
    count = struct.unpack_from(e + 'H', b, 0x1A)[0]
    names = 0x20 + count * 16 + 8
    out = {}
    for k in range(count):
        _, attr, start, end = struct.unpack_from(e + '4I', b, 0x20 + k * 16)
        at = names + (attr & 0xFFFFFF) * 4
        out[b[at:b.find(b'\0', at)].decode('utf-8', 'replace')] = b[data + start:data + end]
    return out


def unpacked(files, raw):
    """A .zs file's bytes: Zstandard, with one of the game's dictionaries or with none."""
    import zstandard
    dictionaries = [None]
    if 'Pack/ZsDic.pack.zs' in files:
        try:
            inner = zstandard.ZstdDecompressor().decompress(whole(files, 'Pack/ZsDic.pack.zs'), max_output_size=64 << 20)
            dictionaries = [zstandard.ZstdCompressionDict(v) for v in archive(inner).values()] + [None]
        except zstandard.ZstdError:
            pass
    for d in dictionaries:
        try:
            z = zstandard.ZstdDecompressor(dict_data=d) if d else zstandard.ZstdDecompressor()
            return z.decompress(raw, max_output_size=64 << 20)
        except zstandard.ZstdError:
            continue
    raise ValueError('not unpacked with any of the game\'s dictionaries')


class Byml:
    """A BYML file's tree, as far as a table of heaps needs it: maps, arrays, strings and numbers."""

    def __init__(self, b):
        self.b, self.e = b, '<' if b[:2] == b'YB' else '>'
        if b[:2] not in (b'YB', b'BY'):
            raise ValueError('not a BYML file')
        _, keys, strings, root = struct.unpack_from(self.e + 'H3I', b, 2)
        self.keys = self.strings_at(keys) if keys else []
        self.strs = self.strings_at(strings) if strings else []
        self.root = self.node(b[root], root) if root else None

    def strings_at(self, at):
        n = int.from_bytes(self.b[at + 1:at + 4], 'little' if self.e == '<' else 'big')
        offs = struct.unpack_from(f'{self.e}{n + 1}I', self.b, at + 4)
        return [self.b[at + o:self.b.find(b'\0', at + o)].decode('utf-8', 'replace') for o in offs[:n]]

    def value(self, kind, v):
        b, e = self.b, self.e
        if kind == 0xA0:
            return self.strs[v]
        if kind in (0xC0, 0xC1):
            return self.node(kind, v)
        if kind == 0xD0:
            return bool(v)
        if kind == 0xD1:
            return struct.unpack(e + 'i', struct.pack(e + 'I', v))[0]
        if kind == 0xD2:
            return struct.unpack(e + 'f', struct.pack(e + 'I', v))[0]
        if kind == 0xD3:
            return v
        if kind in (0xD4, 0xD5, 0xD6):
            return struct.unpack_from(e + {0xD4: 'q', 0xD5: 'Q', 0xD6: 'd'}[kind], b, v)[0]
        return None

    def node(self, kind, at):
        b, e = self.b, self.e
        n = int.from_bytes(b[at + 1:at + 4], 'little' if e == '<' else 'big')
        if kind == 0xC0:
            kinds = b[at + 4:at + 4 + n]
            base = at + 4 + (n + 3 & ~3)
            return [self.value(kinds[k], struct.unpack_from(e + 'I', b, base + 4 * k)[0]) for k in range(n)]
        out = {}
        for k in range(n):
            w = struct.unpack_from(e + 'I', b, at + 4 + 8 * k)[0]
            key, kd = (w & 0xFFFFFF, w >> 24) if e == '<' else (w >> 8, w & 0xFF)
            out[self.keys[key]] = self.value(kd, struct.unpack_from(e + 'I', b, at + 8 + 8 * k)[0])
        return out


def table(files):
    """The game's heaps, {name: {'Size': KB, 'Parent': name}}, or None: of the Switch 2's table when there are several."""
    names = sorted(n for n in files if n.startswith('System/HeapSize/') and '.HeapDef.byml' in n)
    names = [n for n in names if 'Ounce' in n] or names
    names.sort(key=lambda n: 'Product' not in n)  # (the table of the game as sold, before a default one)
    for name in names:
        try:
            raw = whole(files, name)
            tree = Byml(unpacked(files, raw) if name.endswith('.zs') else raw).root
        except Exception:  # noqa: BLE001  (a table this can't read is as good as none: the port is made as for any game)
            continue
        if isinstance(tree, dict) and isinstance(tree.get('Heaps'), dict):
            return tree['Heaps']
    return None


def apart(files):
    """MB of address space the game keeps apart from its heap, by its own table; None when it has none."""
    try:
        heaps = table(files)
    except ImportError:
        return None  # (no zstandard here: the table isn't read)
    entry = (heaps or {}).get('VirtualAddressMemory')
    size = entry.get('Size') if isinstance(entry, dict) else None
    return size // 1024 if isinstance(size, int) and size >= 1024 else None


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    import game2
    files = game2.Dump(sys.argv[1]).files()
    heaps = table(files)
    if not heaps:
        sys.exit('The game has no table of heaps that this reads.')
    for name in sorted(heaps):
        h = heaps[name]
        print(f'{name.encode("ascii", "replace").decode():36s} {h.get("Size")!s:>10} KB' + (f'   in {str(h["Parent"]).encode("ascii", "replace").decode()}' if h.get('Parent') else ''))
    print(f'apart from the heap: {apart(files)} MB')


if __name__ == '__main__':
    main()
