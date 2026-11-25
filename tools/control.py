"""A port's name, version and icon, for a game whose dump doesn't have them: taken from the console, or given here.

  python tools/control.py <name> --ftp <host[:port]>                       from the console that has the game
  python tools/control.py <name> --title "..." [--author "..."] [--version "..."] [--icon <picture>]      given by hand
                                         (python nxrelink.py identity <name> ...  does either, and builds and installs the port again)

A Switch game's NSP has its control data (tools/nsp.py reads it), and so has a Switch 2 game's NUP container of the newer kind. The
older kind of container and a folder of loose files have only the program and its files, and the port then shows the dump's file
name and no icon in the homebrew menu. Two ways to give it its own:

From the console. It keeps a game's name, version and icon for every game it has (installed, or its card in it), and the port asks
it (runtime/control.c). In two steps, because the asking is done on the console:
  1. Run this with --ftp: it leaves the file "control" in the port's folder on the card, with the game's number. Open the port
     once from the homebrew menu: it asks the console, writes control-<number>.bin there and goes back to the menu by itself.
  2. Run this again: it fetches that file, writes what it says into build/<name>_nsp (info.json, icon.jpg) and takes both files off
     the card.
Nothing of the console's is changed: its data is only read. A console that doesn't have the game says so in the port's log, and
the second step finds nothing.

By hand. --title, --author, --version as they are to be shown; --icon any picture (a screenshot of the game's title will do): its
middle is made the 256 by 256 JPEG the menu shows (tools/icon.py).

Exit code: 0 the port's information was updated (it is then built again); 2 the console's first step was done, the port is to be
opened once; 1 it couldn't be done.
"""
import ftplib
import io
import json
import os
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__))).replace('\\', '/')
sys.path.insert(0, f'{ROOT}/tools')
import icon as icons  # noqa: E402


def entries(nacp):
    """The control data's names by language: [(name, maker)], sixteen of them as they are, or thirty-two packed (DEFLATE, a newer
    console's way: the first two bytes say how many packed bytes follow)."""
    table = nacp[:0x3000]
    plain = [(table[k * 0x300:k * 0x300 + 0x200], table[k * 0x300 + 0x200:k * 0x300 + 0x300]) for k in range(16)]
    looks_plain = any(n.split(b'\0')[0] and all(c >= 0x20 or c in (9, 10, 13) for c in n.split(b'\0')[0]) for n, _ in plain) and nacp[0] >= 0x20
    if not looks_plain:
        size = int.from_bytes(nacp[:2], 'little')
        try:
            table = zlib.decompressobj(-15).decompress(nacp[2:2 + size])
            plain = [(table[k * 0x300:k * 0x300 + 0x200], table[k * 0x300 + 0x200:k * 0x300 + 0x300]) for k in range(len(table) // 0x300)]
        except zlib.error:
            pass
    return [(n.split(b'\0')[0].decode('utf-8', 'replace'), a.split(b'\0')[0].decode('utf-8', 'replace')) for n, a in plain]


def read(data):
    """(name, maker, version, the icon's bytes) from what the console gave: the control data's 0x4000 bytes, then a JPEG."""
    nacp, icon = data[:0x4000], data[0x4000:]
    name, maker = next(((n, a) for n, a in entries(nacp) if n), ('', ''))
    version = nacp[0x3060:0x3070].split(b'\0')[0].decode('utf-8', 'replace')
    end = icon.rfind(bytes([0xFF, 0xD9]))  # (the JPEG's end: what follows is the buffer's padding)
    return name, maker, version, icon[:end + 2] if icon[:2] == bytes([0xFF, 0xD8]) and end > 0 else b''


def option(args, name):
    if name in args:
        i = args.index(name)
        v = args[i + 1]
        del args[i:i + 2]
        return v
    return None


def main():
    args = sys.argv[1:]
    where = option(args, '--ftp')
    given = {k: option(args, f'--{k}') for k in ('title', 'author', 'version', 'icon')}
    by_hand = any(v is not None for v in given.values())
    if len(args) != 1 or not (where or by_hand):
        sys.exit(__doc__)
    name = args[0]
    folder = f'{ROOT}/build/{name}_nsp'
    if not os.path.isfile(f'{folder}/info.json'):
        sys.exit(f'{folder}/info.json isn\'t there: the port is built first (python nxrelink.py make ...).')
    info = json.load(open(f'{folder}/info.json', encoding='utf-8'))
    picture = None
    if by_hand:
        for k in ('title', 'author', 'version'):
            if given[k] is not None:
                info[k] = given[k]
        if given['icon']:
            if not os.path.isfile(given['icon']):
                sys.exit(f'{given["icon"]} isn\'t there.')
            picture = icons.fit(given['icon'])
    else:
        number = (info.get('title_id') or '').lower()
        if len(number) != 16 or not int(number, 16):
            sys.exit(f'{name}: the game\'s number isn\'t known (title_id in {folder}/info.json): the console can\'t be asked for it.')
        host, _, port = where.partition(':')
        ftp = ftplib.FTP()
        try:
            ftp.connect(host, int(port or 5000), timeout=60)
            ftp.login()
        except (OSError, ftplib.all_errors) as e:
            sys.exit(f'No FTP server answers at {where} ({e}). Is it open on the console?')
        home = f'/{"nx2-relink" if info.get("switch2") else "nx-relink"}/{name}'  # (the port's folder: runtime/nx2.h)
        there = [n.split('/')[-1] for n in ftp.nlst(home)]
        answer = f'control-{number}.bin'
        if answer not in there:
            ftp.storbinary(f'STOR {home}/control', io.BytesIO(number.encode()))
            print(f'{name}: the console is to be asked for the name, version and icon of game {number}.\n'
                  f'  Open the port once from the homebrew menu ("{info.get("title") or name}"): it comes back to the menu by itself.\n'
                  f'  Then run this again. (If the console doesn\'t have that game, nothing comes: give them with --title, --version, --icon.)')
            sys.exit(2)
        data = bytearray()
        ftp.retrbinary(f'RETR {home}/{answer}', data.extend)
        title, maker, version, icon = read(bytes(data))
        for leftover in (answer, 'control'):
            if leftover in there:
                ftp.delete(f'{home}/{leftover}')
        if not title:
            sys.exit(f'{name}: what the console gave has no name in it ({len(data)} bytes): nothing changed.')
        info['title'], info['author'] = title, maker or info.get('author', '')
        info['version'] = version or info.get('version', '')
        picture = icons.fit(icon) if icon else None
    if picture:
        open(f'{folder}/icon.jpg', 'wb').write(picture)
        info['icon'] = 'icon.jpg'
    info['identity'] = 'by hand' if by_hand else 'from the console'  # (kept when the port is made again: tools/game2.py)
    json.dump(info, open(f'{folder}/info.json', 'w', encoding='utf-8'), ensure_ascii=False, indent=1)
    print(f'{name}: "{info.get("title")}" {info.get("version")}, by "{info.get("author")}"' + (f'; its icon ({len(picture)} bytes)' if picture else '; the icon as it was')
          + f': written to {folder}')


if __name__ == '__main__':
    main()
