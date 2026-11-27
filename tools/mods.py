"""Mods of the games of a card's ports: kept on the card, in /mods, and put on (or taken off) a port.

  python nxrelink.py mods ports  --sd <drive> | --ftp <host>            the ports on the card, and how many mods each has on
  python nxrelink.py mods list   [<port>] --sd <drive> | --ftp <host>   the mods on the card; with a port: which are for its
                                                                        game, which are on, which overlap
  python nxrelink.py mods send   [<mod> ...] --sd | --ftp               mods of this computer's mods folder copied to the card's
                                                                        /mods (all of them, or the ones named)
  python nxrelink.py mods apply  <port> <mod> [<mod> ...] --sd | --ftp  these put on, added to the ones that are on
  python nxrelink.py mods remove <port> <mod> [...] | --all  --sd | --ftp   these (or all) taken off
  python nxrelink.py mods set    <port> [<mod> ...] --sd | --ftp        exactly these on, in this order: the rest taken off
  python nxrelink.py mods delete <mod> [...] --sd | --ftp               mods removed from the card's /mods (taken off first)
      --mods <folder>     this computer's mods folder (default: mods beside the tool)

A mod is a folder, on the card in /mods and, before it is sent there, in this computer's mods folder:
    /mods/<mod>/mod.json     {"name": "...", "game": {"title_id": "0100...", "title": "..."}, "version": "1.0",
                              "author": "...", "description": "...", "requires": ["<another mod's folder>"]}
    /mods/<mod>/files/...    its files, each at the path it has in the game and in the form the game expects (its packing too)
"game" says which game the mod is for: a port is shown the mods of its own game (by title id; the port's is known from what was
kept when it was built on this computer, or from port.json in its folder on the card, which this writes when it knows).

The card's /mods is where mods are kept, whether on or not: nothing reads them there. Putting a mod on copies its files to where
the port looks for them, /<tool>/<port>/mods/<path>: a file there is what the game gets in place of its own. Which mods are on,
in which order, and which files each put there is kept in /<tool>/<port>/mods.json, so a mod is taken off by removing its own
files and no others; it stays in /mods, to be put on again. Several mods can be on at once. Two that have the same file OVERLAP:
the game can get only one, and it is the one of the mod put on last (later in the order); this says so for every such file, and
when the winning mod is taken off the other's file is put back. Files in a port's mods folder that no mod put there (copied by
hand) are left alone, and said.
A file is known to be on the card already by its size, so a mod changed without any of its files changing size is sent
or put on again with --again (send, apply, set: every file of the mods named is copied whether it looks the same or not).
Over FTP a file can't be copied from one place of the card to another: it is sent from this computer's mods folder when the same
mod is there (the same file, by its size), and fetched from the card and sent back when it isn't.
A mod of this computer's mods folder can also be put on straight from here, without being sent to the card's /mods first: its
files go to the port's folder all the same. It is then on for that port and not kept on the card, so only this computer can put
it on again; "mods send" keeps it there too. Where a mod of the same name is in both places, the card's is the one used.
"""
import io
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__))).replace('\\', '/')
sys.path.insert(0, f'{ROOT}/tools')
from edition import EDITION  # noqa: E402
from install import Card, Ftp, option  # noqa: E402
from progress import progress  # noqa: E402

MODS = f'{ROOT}/mods'
SHARED = '/mods'  # (on the card)
STATE = 'mods.json'


def tool_folder(target):
    return 'nx-relink' if target == 'switch' else 'nx2-relink'


# ---------------------------------------------------------------- the card: a few things install.py's two kinds don't have
def card_read(card, path, most=1 << 20):
    """A small file of the card's, or None if it isn't there."""
    if isinstance(card, Card):
        try:
            with open(card.root + path.lstrip('/'), 'rb') as f:
                return f.read(most)
        except OSError:
            return None
    out = io.BytesIO()
    try:
        card.ftp.retrbinary('RETR ' + path, out.write)
    except Exception:  # noqa: BLE001  (not there, or not readable: the same to the caller)
        return None
    return out.getvalue()[:most]


def card_write(card, path, data):
    if isinstance(card, Card):
        os.makedirs(os.path.dirname(card.root + path.lstrip('/')), exist_ok=True)
        with open(card.root + path.lstrip('/'), 'wb') as f:
            f.write(data)
    else:
        card.ftp.storbinary('STOR ' + path, io.BytesIO(data))


def card_dirs(card, d):
    """The folders in a folder of the card's."""
    if isinstance(card, Card):
        base = card.root + d.lstrip('/')
        return sorted(x for x in card.listing(d) if os.path.isdir(os.path.join(base, x)))
    lines = []
    try:
        card.ftp.dir(d, lines.append)
    except Exception:  # noqa: BLE001
        return []
    return sorted(l.split(None, 8)[-1] for l in lines if l[:1] == 'd' and l.split(None, 8)[-1] not in ('.', '..'))


def card_walk(card, d):
    """Every file under a folder of the card's: {path under it: size}."""
    out = {}
    if isinstance(card, Card):
        base = card.root + d.lstrip('/')
        for here, _, files in os.walk(base):
            for f in files:
                p = os.path.join(here, f)
                out[os.path.relpath(p, base).replace('\\', '/')] = os.path.getsize(p)
        return out

    def walk(at, rel):
        lines = []
        try:
            card.ftp.dir(at, lines.append)
        except Exception:  # noqa: BLE001
            return
        for l in lines:
            parts = l.split(None, 8)
            if len(parts) < 9 or parts[8] in ('.', '..'):
                continue
            if l[:1] == 'd':
                walk(f'{at}/{parts[8]}', rel + parts[8] + '/')
            else:
                out[rel + parts[8]] = int(parts[4]) if parts[4].isdigit() else -1
    walk(d.rstrip('/'), '')
    return out


def card_mkdirs(card, path):
    """Every folder a file's path goes through, made."""
    parts = path.strip('/').split('/')[:-1]
    for k in range(1, len(parts) + 1):
        card.mkdir('/' + '/'.join(parts[:k]))


def card_prune(card, top, path):
    """The folders a removed file was in, removed where they are left empty (never top itself)."""
    d = os.path.dirname(path)
    while d and d != top and d.startswith(top):
        if card.listing(d):
            break
        card.rmdir(d)
        d = os.path.dirname(d)


# ---------------------------------------------------------------- the mods: on the card, and on this computer
def runs_code(m):
    """Whether a mod carries code that runs in the game: a hook file (Code/<name>.nxh, which a port maps and calls as it starts).
    The switch files beside them in Code/ are data."""
    return any(p.replace('\\', '/').lstrip('/').lower().startswith('code/') and p.lower().endswith('.nxh') for p in m['files'])


def describe(d, raw, files):
    """A mod as this keeps it, from its mod.json's bytes (None: there is none) and its files {path: size}."""
    m = {'id': d, 'name': d, 'game': {}, 'version': '', 'author': '', 'description': '', 'requires': [], 'files': files, 'bad': ''}
    if raw is None:
        m['bad'] = 'no mod.json'
    else:
        try:
            info = json.loads(raw.decode('utf-8-sig'))
            for k in ('name', 'version', 'author', 'description'):
                m[k] = str(info.get(k) or m[k])
            game = info.get('game') or {}
            m['game'] = {'title_id': str(game.get('title_id') or '').lower(), 'title': str(game.get('title') or '')} if isinstance(game, dict) else \
                        {'title_id': '', 'title': str(game)}
            m['requires'] = [str(x) for x in info.get('requires') or []]
        except (ValueError, AttributeError) as e:
            m['bad'] = f'mod.json can\'t be read ({e})'
    if not m['files'] and not m['bad']:
        m['bad'] = 'no files (they go in its folder "files", at the paths they have in the game)'
    return m


def read_mods(folder):
    """{mod's folder name: the mod} of every mod in this computer's mods folder; what is wrong with one is in its 'bad'."""
    out = {}
    if not os.path.isdir(folder):
        return out
    for d in sorted(os.listdir(folder)):
        base = f'{folder}/{d}'
        if not os.path.isdir(base) or d.startswith('.'):
            continue
        files = {}
        for here, _, names in os.walk(f'{base}/files'):
            for f in names:
                p = os.path.join(here, f)
                files[os.path.relpath(p, f'{base}/files').replace('\\', '/')] = os.path.getsize(p)
        try:
            raw = open(f'{base}/mod.json', 'rb').read()
        except OSError:
            raw = None
        out[d] = describe(d, raw, files)
    return out


def read_card_mods(card):
    """The same of the card's /mods."""
    out = {}
    for d in card_dirs(card, SHARED):
        if d.startswith('.'):
            continue
        out[d] = describe(d, card_read(card, f'{SHARED}/{d}/mod.json'), card_walk(card, f'{SHARED}/{d}/files'))
    return out


def card_fetch(card, path, to):
    """A file of the card's copied to this computer."""
    if isinstance(card, Card):
        import shutil
        shutil.copyfile(card.root + path.lstrip('/'), to)
    else:
        with open(to, 'wb') as f:
            card.ftp.retrbinary('RETR ' + path, f.write, blocksize=1 << 18)


def local_identity(port):
    """What was kept of a port when it was built here: {'title', 'title_id'}, or {}."""
    try:
        info = json.load(open(f'{ROOT}/build/{port}_nsp/info.json', encoding='utf-8'))
        return {'title': info.get('title') or '', 'title_id': str(info.get('title_id') or '').lower()}
    except (OSError, ValueError):
        return {}


def port_identity(card, top, port):
    raw = card_read(card, f'/{top}/{port}/port.json')
    if raw:
        try:
            got = json.loads(raw.decode('utf-8'))
            return {'title': got.get('title') or '', 'title_id': str(got.get('title_id') or '').lower()}
        except ValueError:
            pass
    return local_identity(port)


def read_state(card, top, port):
    raw = card_read(card, f'/{top}/{port}/{STATE}')
    if raw:
        try:
            got = json.loads(raw.decode('utf-8'))
            if isinstance(got.get('mods'), list):
                return got
        except ValueError:
            pass
    return {'version': 1, 'mods': []}


def write_state(card, top, port, state):
    card_write(card, f'/{top}/{port}/{STATE}', json.dumps(state, indent=1).encode('utf-8'))


def fits(mod, identity):
    """Whether a mod is for a port's game: True, False, or None when one of the two doesn't say."""
    a, b = mod['game'].get('title_id'), identity.get('title_id')
    if a and b:
        return a == b
    a, b = mod['game'].get('title', '').strip().lower(), identity.get('title', '').strip().lower()
    return (a == b) if a and b else None


def overlaps(order, mods):
    """{path: [mods that have it, in order]} for the paths more than one of these mods has: the last named is the one the game gets."""
    seen = {}
    for mid in order:
        for path in mods[mid]['files']:
            seen.setdefault(path, []).append(mid)
    return {p: who for p, who in seen.items() if len(who) > 1}


# ---------------------------------------------------------------- the commands
def cmd_ports(card, top):
    ports = card_dirs(card, f'/{top}')
    if not ports:
        print(f'No port is on this card (nothing in /{top}/).')
    for port in ports:
        ident, state = port_identity(card, top, port), read_state(card, top, port)
        # (which game it is, left on the card where this computer knows and the card doesn't say: for a mod manager on the console)
        if ident and card_read(card, f'/{top}/{port}/port.json') is None:
            card_write(card, f'/{top}/{port}/port.json', json.dumps(ident, indent=1).encode('utf-8'))
        if os.environ.get('NX2_GUI'):
            print(f'@@port|{port}|{ident.get("title", "")}|{ident.get("title_id", "")}|{len(state["mods"])}', flush=True)
        print(f'{port}: {ident.get("title") or "(game not known: not built on this computer)"}'
              f'{" [" + ident["title_id"] + "]" if ident.get("title_id") else ""}; {len(state["mods"])} mod(s) on')


def not_sent(mods, local):
    """This computer's mods that the card hasn't, or has differently: {mod: 'new' | 'changed'}."""
    return {mid: 'new' if mid not in mods else 'changed' for mid, m in local.items()
            if not m['bad'] and (mid not in mods or mods[mid]['files'] != m['files'])}


def cmd_list(card, top, port, mods, folder, local, kept=()):
    """mods: every mod there is (the card's, and this computer's that the card hasn't); kept: the ones in the card's /mods."""
    if not mods:
        print(f'No mods on the card (in {SHARED}/).' if card else f'No mods in {folder}.', 'A mod is a folder with mod.json and its files under files/ '
              '(see the top of tools/mods.py); "mods send" copies this computer\'s to the card.')
    if card:
        ident = port_identity(card, top, port) if port else {}
        for mid, how in not_sent({m: mods[m] for m in kept}, local).items():
            if os.environ.get('NX2_GUI'):
                m = local[mid]
                fit = {True: 'yes', False: 'no', None: 'unknown'}[fits(m, ident) if port else None]
                print('@@local|' + '|'.join([mid, m['name'].replace('|', '/'), how, m['version'], fit, str(len(m['files'])), str(sum(m['files'].values())),
                                             m['description'].replace('|', '/').replace('\n', ' ')]), flush=True)
            print(f'on this computer and {"not in the card\'s " + SHARED if how == "new" else "different on the card"}: {mid} ("mods send" copies it there)')
    ident, state, on_card = {}, {'mods': []}, None
    if card and port:
        ident, state = port_identity(card, top, port), read_state(card, top, port)
        on_card = card_walk(card, f'/{top}/{port}/mods')
    on = [m['id'] for m in state['mods']]
    over = overlaps([m for m in on if m in mods], mods)
    gui = bool(os.environ.get('NX2_GUI'))
    # (a mod that runs code in the game is listed apart, after those that only replace files)
    order = sorted(mods, key=lambda x: runs_code(mods[x]))
    if not gui and any(runs_code(m) for m in mods.values()) and not all(runs_code(m) for m in mods.values()):
        print('Mods that replace files of the game:')
    for mid in order:
        m = mods[mid]
        if not gui and runs_code(m) and mid == next(x for x in order if runs_code(mods[x])):
            print('Mods that RUN CODE IN THE GAME (a hook file each, made for one version of one game; the port says in its log whether it used it):')
        fit = fits(m, ident) if port else None
        where = on.index(mid) + 1 if mid in on else 0
        shared = sorted({o for p, who in overlaps([x for x in mods if not mods[x]['bad']], mods).items() if mid in who for o in who if o != mid
                         and (not port or fits(mods[o], ident) is not False)})
        lost = [p for p, who in over.items() if mid in who and who[-1] != mid]
        size = sum(m['files'].values())
        if gui:
            print('@@mod|' + '|'.join([mid, m['name'].replace('|', '/'), m['version'], m['game'].get('title', '').replace('|', '/'), m['game'].get('title_id', ''),
                                       {True: 'yes', False: 'no', None: 'unknown'}[fit], str(where), str(len(m['files'])), str(size), ','.join(shared),
                                       ','.join(m['requires']), m['bad'].replace('|', '/'), m['description'].replace('|', '/').replace('\n', ' '),
                                       'only' if mid not in kept else ('same' if local[mid]['files'] == m['files'] else 'different') if mid in local else 'no',
                                       'code' if runs_code(m) else '']),
                  flush=True)
        line = f'{"[" + str(where) + "]" if where else "[ ]"} {mid}: {m["name"]} {m["version"]}'.rstrip() + f'  ({len(m["files"])} files, {size / (1 << 20):.1f} MB)'
        line += f'  for {m["game"].get("title") or m["game"].get("title_id") or "a game it doesn\'t name"}'
        if port and fit is False:
            line += '  -- NOT for this port\'s game'
        if runs_code(m):
            line += '  -- runs code in the game'
        if m['bad']:
            line += f'  -- CAN\'T BE USED: {m["bad"]}'
        if card and mid not in kept:
            line += '  -- on this computer only'
        print(line)
        if shared:
            print(f'      overlaps with {", ".join(shared)}: they have files in common, and the game gets those of the one put on last')
        if lost:
            print(f'      {len(lost)} of its files are not the ones in use now: a mod put on after it has the same')
        missing = [r for r in m['requires'] if where and r not in on]
        if missing:
            print(f'      NEEDS {", ".join(missing)}, which isn\'t on')
    if on_card is not None:
        owned = {p for m in state['mods'] for p in m['files']}
        loose = sorted(p for p in on_card if p not in owned)
        gone = [m['id'] for m in state['mods'] if m['id'] not in mods]
        if gone:
            print(f'On for this port and no longer in {SHARED} nor on this computer: {", ".join(gone)} (they can still be taken off)')
        if loose:
            if gui:
                print(f'@@loose|{len(loose)}|{", ".join(loose[:6])}', flush=True)
            print(f'{len(loose)} file(s) in the port\'s mods folder that no mod put there (left alone): ' + ', '.join(loose[:6]) + (' ...' if len(loose) > 6 else ''))


def put_file(card, src, path, said):
    card_mkdirs(card, path)
    n = os.path.getsize(src)
    for attempt in (1, 2):
        try:
            if card.put(src, 0, n, path):
                return True
        except Exception as e:  # noqa: BLE001  (a connection that dropped: said, and tried once more)
            print(f'  {said}: {e}')
    return False


def cmd_send(card, names, local, folder, again=False):
    """Mods of this computer's folder copied to the card's /mods: the files the card hasn't, or has with another size."""
    if not local:
        sys.exit(f'No mods in {folder}.')
    for mid in names:
        if mid not in local:
            sys.exit(f'No mod "{mid}" in {folder}.')
    there = read_card_mods(card)
    failed = 0
    for mid in names or list(local):
        m = local[mid]
        if m['bad']:
            print(f'{mid}: not sent: {m["bad"]}')
            continue
        has = there.get(mid, {'files': {}})['files']
        send = sorted(p for p, n in m['files'].items() if again or has.get(p) != n)
        total, done = sum(m['files'][p] for p in send), 0
        card_mkdirs(card, f'{SHARED}/{mid}/x')
        card_write(card, f'{SHARED}/{mid}/mod.json', open(f'{folder}/{mid}/mod.json', 'rb').read())
        for path in sorted(p for p in has if p not in m['files']):  # (a file the mod no longer has)
            card.remove(f'{SHARED}/{mid}/files/{path}')
            card_prune(card, f'{SHARED}/{mid}/files', f'{SHARED}/{mid}/files/{path}')
        for k, path in enumerate(send, 1):
            if not put_file(card, f'{folder}/{mid}/files/{path}', f'{SHARED}/{mid}/files/{path}', path):
                failed += 1
                print(f'  {path} did NOT arrive whole')
            done += m['files'][path]
            progress('copy', f'{mid}: {k} of {len(send)}', done, max(total, 1))
        print(f'{mid}: {len(send)} file(s) sent ({total / (1 << 20):.1f} MB), {len(m["files"]) - len(send)} there already')
    if failed:
        sys.exit(f'{failed} file(s) failed: run it again.')
    print(f'Done: they are in {SHARED} on the card. A mod that was on for a port already is put on again for its change to be used.')


def cmd_delete(card, top, names, mods):
    for mid in names:
        if mid not in mods:
            print(f'{mid}: not in {SHARED} on the card')
            continue
        for port in card_dirs(card, f'/{top}'):
            if mid in [m['id'] for m in read_state(card, top, port)['mods']]:
                sys.exit(f'{mid} is on for the port "{port}": take it off first ("mods remove {port} {mid}").')
        for path in mods[mid]['files']:
            card.remove(f'{SHARED}/{mid}/files/{path}')
            card_prune(card, f'{SHARED}/{mid}', f'{SHARED}/{mid}/files/{path}')
        if card_read(card, f'{SHARED}/{mid}/mod.json') is not None:
            card.remove(f'{SHARED}/{mid}/mod.json')
        card.rmdir(f'{SHARED}/{mid}/files')
        card.rmdir(f'{SHARED}/{mid}')
        print(f'{mid}: removed from the card')


def copy_on_card(card, mid, path, to, size, local, folder, kept_on_card=True):
    """A mod's file copied from the card's /mods to a port's folder. With the card in this computer, a copy; over FTP, sent from
    this computer's mods folder if the same file is there, and otherwise fetched from the card first."""
    src = f'{SHARED}/{mid}/files/{path}'
    mine = f'{folder}/{mid}/files/{path}'
    if not kept_on_card:  # (a mod of this computer's alone: straight from here)
        return put_file(card, mine, to, path)
    if isinstance(card, Card):
        return put_file(card, card.root + src.lstrip('/'), to, path)
    if mid in local and local[mid]['files'].get(path) == size and os.path.isfile(mine):
        return put_file(card, mine, to, path)
    import tempfile
    with tempfile.TemporaryDirectory() as tmp:
        got = os.path.join(tmp, 'f')
        try:
            card_fetch(card, src, got)
        except Exception as e:  # noqa: BLE001
            print(f'  {path}: couldn\'t be read from {SHARED} ({e})')
            return False
        return os.path.getsize(got) == size and put_file(card, got, to, path)


def cmd_set(card, top, port, want, mods, folder, local, kept=(), again=()):
    """The mods that are on made exactly `want`, in that order."""
    ident, state = port_identity(card, top, port), read_state(card, top, port)
    if port not in card_dirs(card, f'/{top}'):
        sys.exit(f'No port "{port}" on this card (/{top}/{port}/ isn\'t there). "mods ports" lists the ones that are.')
    for mid in want:
        if mid not in mods:
            sys.exit(f'No mod "{mid}": not in {SHARED} on the card, nor in {folder}.')
        if mods[mid]['bad']:
            sys.exit(f'The mod "{mid}" can\'t be used: {mods[mid]["bad"]}.')
        if fits(mods[mid], ident) is False:
            sys.exit(f'The mod "{mid}" is for {mods[mid]["game"].get("title") or mods[mid]["game"].get("title_id")}, and the port "{port}" is '
                     f'{ident.get("title") or ident.get("title_id")}: not put on. (Its mod.json says which game it is for.)')
    base = f'/{top}/{port}/mods'
    was = {m['id']: m for m in state['mods']}
    on_card = card_walk(card, base)
    # what each path should be, and from which mod: the last mod of the order that has it
    owner = {}
    for mid in want:
        for path in mods[mid]['files']:
            owner[path] = mid
    had = {}  # (what the card has now, by the record: path -> (mod, size))
    for m in state['mods']:
        for path, size in m['files'].items():
            had[path] = (m['id'], size)
    over = overlaps(want, mods)
    for path, who in sorted(over.items()):
        print(f'OVERLAP: {path} is in {", ".join(who)}: the game gets {who[-1]}\'s')
    for mid in want:
        missing = [r for r in mods[mid]['requires'] if r not in want]
        if missing:
            print(f'NOTE: {mid} needs {", ".join(missing)}, which {"isn\'t" if len(missing) == 1 else "aren\'t"} among the mods put on')
    loose_hit = sorted(p for p in owner if p in on_card and p not in had)
    if loose_hit:
        print(f'NOTE: {len(loose_hit)} file(s) that were in the port\'s mods folder already (put there by hand) are replaced by a mod\'s: ' + ', '.join(loose_hit[:5])
              + (' ...' if len(loose_hit) > 5 else ''))
    remove = sorted(p for p in had if p not in owner)
    send = sorted(p for p, mid in owner.items()
                  if mid in again or had.get(p, (None, -1)) != (mid, mods[mid]['files'][p]) or on_card.get(p, -2) != mods[mid]['files'][p])
    total, done, failed = sum(mods[owner[p]]['files'][p] for p in send), 0, []
    print(f'{port}: {len(want)} mod(s) on ({", ".join(want) or "none"}); {len(send)} file(s) to copy ({total / (1 << 20):.1f} MB), {len(remove)} to remove')
    for path in remove:
        if path in on_card:
            try:
                card.remove(f'{base}/{path}')
            except Exception as e:  # noqa: BLE001
                print(f'  {path} couldn\'t be removed ({e})')
                failed.append(path)
                continue
        card_prune(card, base, f'{base}/{path}')
    for k, path in enumerate(send, 1):
        mid = owner[path]
        if not copy_on_card(card, mid, path, f'{base}/{path}', mods[mid]['files'][path], local, folder, mid in kept):
            failed.append(path)
            print(f'  {path} did NOT arrive whole')
        done += mods[mid]['files'][path]
        progress('copy', f'{k} of {len(send)}: {os.path.basename(path)}', done, max(total, 1))
    # the record: every mod that is on, with the files of it that are the ones in use
    state = {'version': 1, 'mods': [{'id': mid, 'name': mods[mid]['name'], 'version': mods[mid]['version'],
                                     'files': {p: s for p, s in mods[mid]['files'].items() if owner[p] == mid and p not in failed}} for mid in want]}
    write_state(card, top, port, state)
    if ident and not card_read(card, f'/{top}/{port}/port.json'):
        card_write(card, f'/{top}/{port}/port.json', json.dumps(ident, indent=1).encode('utf-8'))
    gone = [m for m in was if m not in want]
    if gone:
        print('taken off: ' + ', '.join(gone))
    if failed:
        sys.exit(f'{len(failed)} file(s) failed: run it again. Until then the port may have a mod half on.')
    print('Done: the port uses these from its next start.' if want else 'Done: no mod is on; the game\'s own files are used.')


def main(args):
    if not args or args[0] in ('-h', '--help', 'help'):
        print(__doc__)
        return
    cmd = args.pop(0)
    folder = (option(args, '--mods') or MODS).replace('\\', '/').rstrip('/')
    sd, ftp = option(args, '--sd'), option(args, '--ftp')
    target = option(args, '--target', 'switch' if EDITION == 'switch' else 'switch2')
    everything, again = '--all' in args, '--again' in args
    args = [a for a in args if a not in ('--all', '--again')]
    top = tool_folder(target)
    card = Card(sd) if sd else Ftp(ftp) if ftp else None
    local = read_mods(folder)
    if cmd == 'list' and not card:
        return cmd_list(None, top, None, local, folder, local, local)
    if not card:
        sys.exit('Say where the card is: --sd <drive or folder> or --ftp <host[:port]>.')
    if cmd == 'ports':
        return cmd_ports(card, top)
    if cmd == 'send':
        return cmd_send(card, args, local, folder, again)
    kept = read_card_mods(card)
    if cmd == 'delete' and args:
        return cmd_delete(card, top, args, kept)
    mods = dict({mid: m for mid, m in local.items() if mid not in kept}, **kept)  # (the card's, and this computer's that it hasn't)
    if cmd == 'list':
        return cmd_list(card, top, args[0] if args else None, mods, folder, local, kept)
    if cmd not in ('apply', 'remove', 'set') or not args:
        sys.exit(__doc__)
    port, named = args[0], args[1:]
    on = [m['id'] for m in read_state(card, top, port)['mods']]
    if cmd == 'apply':
        want = [m for m in on if m not in named] + named  # (one named again goes last: it wins the files it shares)
    elif cmd == 'remove':
        want = [] if everything else [m for m in on if m not in named]
        for m in named:
            if m not in on:
                print(f'{m} isn\'t on: nothing to take off')
    else:
        want = named
    # (a mod that is on and no longer in the mods folder can be taken off, not kept: its files aren't here to be told from another's)
    lost = [m for m in want if m not in mods and m in on]
    if lost:
        print(f'{", ".join(lost)}: on for this port but no longer in {SHARED} on the card, nor on this computer: taken off with this')
        want = [m for m in want if m not in lost]
    straight = [m for m in dict.fromkeys(want) if m not in kept]
    if straight:
        print(f'put on straight from this computer, not kept in {SHARED} on the card: {", ".join(straight)}')
    cmd_set(card, top, port, list(dict.fromkeys(want)), mods, folder, local, kept, named if again and cmd != 'remove' else ())


if __name__ == '__main__':
    main(sys.argv[1:])
