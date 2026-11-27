"""The tool's window: the steps of nxrelink.py, with buttons. It builds the same command lines and runs them: whatever is done here
can be done from a terminal, and the window shows the command it ran (under "Show details").

  python nxrelink.py          (or: python tools/gui.py)

Five pages: a new port (the game, what the homebrew menu shows, the card, one button), the ports already built, the mods of a
port's game (tools/mods.py: the ones kept in the card's /mods, put on a port and taken off it), the settings that are seldom touched, and the advanced
options, which are behind a warning. Under them, always in view, what is being done: the step
of how many, a bar, how much is left.
Like the command line, it shows the options of its own console alone (tools/edition.py): a Switch's in nx-relink, a Switch 2's in
nx2-relink.
"""
import os
import queue
import string
import subprocess
import sys
import threading
import time
import tkinter as tk
import webbrowser
from tkinter import filedialog, messagebox, ttk

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__))).replace('\\', '/')
sys.path.insert(0, ROOT)
import nxrelink as nx  # noqa: E402
from nxrelink import EDITION, MEMORY, SWITCH, SWITCH2, TOOL, VERSION  # noqa: E402

DEV = EDITION == 'dev'
ALL = ('All files', '*.*')
NSP, NUP = ('Switch game (NSP)', '*.nsp'), ('Game dump (NUP)', '*.nup')
GAMES = [('Game (NSP or NUP)', '*.nsp *.nup'), ALL] if DEV else [NSP if SWITCH else NUP, ALL]
PICTURES = [('Pictures', '*.jpg *.jpeg *.png *.bmp *.webp'), ALL]
BG, CARD, INK, SOFT, LINE = '#F3F4F8', '#FFFFFF', '#1F2330', '#667085', '#E3E6EE'
PRIMARY, PRIMARY_DARK, PRIMARY_SOFT = '#3F51B5', '#303F9F', '#E8EAF6'
GOOD, BAD, WARN, WARN_BG, GOOD_BG = '#2E7D32', '#C62828', '#B26A00', '#FFF4DC', '#E8F5E9'
RISK, RISK_LINE, RISK_BG = '#B3261E', '#EBA9A4', '#FDF1F0'  # (the notice of risk: a light red, to be read and not to shout)
FONT = 'Segoe UI' if os.name == 'nt' else 'Helvetica'
KINDS = {'extract': 'Extracting the game\'s files', 'aoc': 'Extracting the DLC', 'relink': 'Preparing the game\'s code',
         'compile': 'Compiling', 'link': 'Linking', 'copy': 'Copying', 'verify': 'Checking what was copied'}
TARGETS = {'switch': 'Switch', 'switch2': 'Switch 2', 'both': 'both'}
# the steps of each thing done, as nxrelink.py counts them: a Switch game's, and a Switch 2 game's (which is built from its dump in one)
STEPS = {'make+install': ['Extract', 'Build', 'Copy and verify', 'Done'], 'make': ['Extract', 'Build', 'Done'],
         'make2+install': ['Build', 'Copy and verify', 'Done'], 'make2': ['Build', 'Done'],
         'install': ['Copy and verify', 'Done'], 'check': ['Extract the code', 'Check'], 'setup': ['Check what is needed'],
         'remake': ['Build again', 'Done'], 'mods': ['Copy and verify', 'Done']}
FOLDER = {'switch': 'nx-relink', 'switch2': 'nx2-relink', 'dev': 'nx-relink (nx2-relink on a Switch 2)'}[EDITION]
NOT_YET = 'NOT SUPPORTED YET'  # (how the tools begin a refusal that isn't a fault: tools/game2.py)


def is_switch2_game(path):
    return path.lower().endswith('.nup') or os.path.isdir(path)


def removable_drives():
    """The drives a card may be: (path, label) of each removable one (Windows), most likely first."""
    out = []
    if os.name != 'nt':
        return out
    import ctypes
    k = ctypes.windll.kernel32
    for letter in string.ascii_uppercase:
        root = f'{letter}:\\'
        if k.GetDriveTypeW(root) != 2:  # DRIVE_REMOVABLE
            continue
        name, fs = ctypes.create_unicode_buffer(64), ctypes.create_unicode_buffer(64)
        if not k.GetVolumeInformationW(root, name, 64, None, None, None, fs, 64):
            continue
        free = ctypes.c_ulonglong(0)
        k.GetDiskFreeSpaceExW(root, None, None, ctypes.byref(free))
        looks = os.path.isdir(f'{root}switch') or os.path.isdir(f'{root}atmosphere')
        out.append((looks, f'{letter}:', f'{letter}:  {name.value or "no name"}  ·  {fs.value}  ·  {free.value / (1 << 30):.1f} GB free'
                    + ('' if looks else '  ·  doesn\'t look like a console\'s')))
    return [(p, label) for _, p, label in sorted(out, key=lambda x: not x[0])]


def amount(n):
    return f'{n / (1 << 30):.2f} GB' if n >= 1 << 30 else f'{n / (1 << 20):.0f} MB'


def clock(seconds):
    seconds = int(seconds)
    return f'{seconds // 60} min {seconds % 60:02d} s' if seconds >= 60 else f'{seconds} s'


class Segmented(tk.Frame):
    """One choice of a few, as a row of flat buttons: the chosen one filled."""

    def __init__(self, parent, var, choices, command=None):
        super().__init__(parent, bg=LINE, padx=1, pady=1)
        self.var, self.command, self.buttons = var, command, {}
        for value, text in choices:
            b = tk.Label(self, text=text, font=(FONT, 10), padx=16, pady=6, cursor='hand2')
            b.pack(side='left', padx=(0, 1))
            b.bind('<Button-1>', lambda e, v=value: self.choose(v))
            self.buttons[value] = b
        var.trace_add('write', lambda *a: self.paint())
        self.paint()

    def choose(self, value):
        if self.buttons[value]['state'] == 'disabled':
            return
        self.var.set(value)
        if self.command:
            self.command()

    def disable(self, value):
        self.buttons[value].configure(state='disabled', cursor='arrow')
        self.paint()

    def paint(self):
        for value, b in self.buttons.items():
            on = self.var.get() == value
            off_ink = '#B0B5C3' if b['state'] == 'disabled' else INK
            b.configure(bg=PRIMARY if on else CARD, fg='white' if on else off_ink, disabledforeground='#B0B5C3')


class App:
    def __init__(self, root):
        self.root = root
        root.title(f'{TOOL} {VERSION}')
        root.configure(bg=BG)
        root.minsize(900, 850)
        self.proc, self.lines, self.kind = None, queue.Queue(), None
        self.styles()

        head = tk.Frame(root, bg=PRIMARY)
        head.pack(fill='x')
        tk.Label(head, text=TOOL, bg=PRIMARY, fg='white', font=(FONT, 16, 'bold')).pack(side='left', padx=(20, 10), pady=12)
        tk.Label(head, text=VERSION, bg=PRIMARY, fg='#C5CAE9', font=(FONT, 9)).pack(side='right', padx=20)
        self.tabs_bar = tk.Frame(root, bg=PRIMARY_DARK)
        self.tabs_bar.pack(fill='x')
        self.pages, self.tab_labels = {}, {}
        self.body = tk.Frame(root, bg=BG)

        self.game, self.update, self.keys = tk.StringVar(), tk.StringVar(), tk.StringVar(value=(nx.default_keys() or '') if SWITCH else '')
        self.name, self.title, self.version, self.icon = tk.StringVar(), tk.StringVar(), tk.StringVar(), tk.StringVar()
        self.target = tk.StringVar(value=('both' if nx.has_switch2() else 'switch') if DEV else EDITION)
        self.where, self.sd, self.ftp = tk.StringVar(value='sd'), tk.StringVar(), tk.StringVar()
        self.fmt, self.install_target, self.romfs = tk.StringVar(value='auto'), tk.StringVar(value='switch2' if EDITION == 'switch2' else 'switch'), tk.BooleanVar(value=True)
        self.nro_only = tk.BooleanVar(value=False)
        self.memory, self.whole, self.long_log = tk.StringVar(value=''), tk.StringVar(value='auto'), tk.BooleanVar(value=False)
        self.aocs = []
        self.also = []  # (sources of the user's own built into a port: the advanced page)
        self.also_text = tk.StringVar(value='none')
        self.mods_again = tk.BooleanVar(value=False)
        self.mods_dir, self.mods_port = tk.StringVar(value=f'{ROOT}/mods'), tk.StringVar()
        self.mods_ports, self.mods_rows, self.mods_on, self.mods_asked, self.mods_loose, self.mods_said = {}, {}, [], 0, '', ''

        self.status_panel()  # (packed at the bottom first, so the pages take what is left)
        self.body.pack(fill='both', expand=True)
        for key, text, build in (('new', 'New port', self.page_new), ('ports', 'My ports', self.page_ports), ('mods', 'Mods', self.page_mods),
                                 ('settings', 'Settings', self.page_settings),
                                 ('advanced', 'Advanced', self.page_advanced)):
            lab = tk.Label(self.tabs_bar, text=text, bg=PRIMARY_DARK, fg='#C5CAE9', font=(FONT, 10, 'bold'), padx=18, pady=9, cursor='hand2')
            lab.pack(side='left')
            lab.bind('<Button-1>', lambda e, k=key: self.show(k))
            self.tab_labels[key] = lab
            page = tk.Frame(self.body, bg=BG)
            self.pages[key] = page
            build(page)
        self.deps_fix = None
        self.find_drives()
        self.refresh_ports()
        self.sync()
        self.check_deps()
        self.show('new')
        self.idle('Choose your game and press "Build and install".')
        root.after(80, self.pump)

    # ---- looks
    def styles(self):
        st = ttk.Style()
        st.theme_use('clam')
        st.configure('.', font=(FONT, 10), background=CARD, foreground=INK)
        st.configure('TEntry', fieldbackground='white', bordercolor=LINE, lightcolor=LINE, darkcolor=LINE, padding=5)
        st.configure('TSpinbox', fieldbackground='white', bordercolor=LINE, lightcolor=LINE, darkcolor=LINE, padding=5, arrowsize=14)
        st.configure('TCombobox', fieldbackground='white', bordercolor=LINE, lightcolor=LINE, darkcolor=LINE, padding=5, arrowsize=14)
        st.map('TCombobox', fieldbackground=[('readonly', 'white')])
        st.configure('Primary.TButton', background=PRIMARY, foreground='white', font=(FONT, 11, 'bold'), padding=(22, 10), borderwidth=0, focuscolor=PRIMARY)
        st.map('Primary.TButton', background=[('disabled', '#B7BEDF'), ('active', PRIMARY_DARK)], foreground=[('disabled', 'white')])
        st.configure('Soft.TButton', background=PRIMARY_SOFT, foreground=PRIMARY_DARK, font=(FONT, 10), padding=(14, 7), borderwidth=0, focuscolor=PRIMARY_SOFT)
        st.map('Soft.TButton', background=[('disabled', '#EEF0F6'), ('active', '#D5D9F0')], foreground=[('disabled', '#A9AEC0')])
        st.configure('Text.TButton', background=CARD, foreground=PRIMARY, font=(FONT, 10), padding=(8, 5), borderwidth=0, focuscolor=CARD)
        st.map('Text.TButton', background=[('active', PRIMARY_SOFT)], foreground=[('disabled', '#A9AEC0')])
        st.configure('Bar.Horizontal.TProgressbar', troughcolor='#E3E6EE', background=PRIMARY, bordercolor='#E3E6EE', lightcolor=PRIMARY, darkcolor=PRIMARY, thickness=12)
        st.configure('Good.Horizontal.TProgressbar', troughcolor='#E3E6EE', background=GOOD, bordercolor='#E3E6EE', lightcolor=GOOD, darkcolor=GOOD, thickness=12)
        st.configure('Bad.Horizontal.TProgressbar', troughcolor='#E3E6EE', background=BAD, bordercolor='#E3E6EE', lightcolor=BAD, darkcolor=BAD, thickness=12)
        st.configure('TCheckbutton', background=CARD)
        st.configure('TRadiobutton', background=CARD)
        st.configure('Treeview', rowheight=30, fieldbackground='white', bordercolor=LINE, font=(FONT, 10))
        st.configure('Treeview.Heading', font=(FONT, 9, 'bold'), background='#F3F4F8', foreground=SOFT, relief='flat')
        st.map('Treeview', background=[('selected', PRIMARY_SOFT)], foreground=[('selected', INK)])

    def card(self, parent, number, title, hint=None):
        """A white panel with a numbered heading; returns its inside."""
        outer = tk.Frame(parent, bg=LINE, padx=1, pady=1)
        outer.pack(fill='x', padx=20, pady=(12, 0))
        inner = tk.Frame(outer, bg=CARD, padx=16, pady=12)
        inner.pack(fill='x')
        head = tk.Frame(inner, bg=CARD)
        head.pack(fill='x', pady=(0, 8))
        if number:
            tk.Label(head, text=str(number), bg=PRIMARY, fg='white', font=(FONT, 9, 'bold'), width=2).pack(side='left', padx=(0, 8))
        tk.Label(head, text=title, bg=CARD, fg=INK, font=(FONT, 11, 'bold')).pack(side='left')
        if hint:
            tk.Label(head, text=hint, bg=CARD, fg=SOFT, font=(FONT, 9)).pack(side='left', padx=10)
        return inner

    def link(self, parent, text, url, bg=CARD):
        """A line of text that opens an address in the browser."""
        lab = tk.Label(parent, text=text + '  ↗', bg=bg, fg=PRIMARY, font=(FONT, 9, 'underline'), cursor='hand2')
        lab.bind('<Button-1>', lambda e: webbrowser.open(url))
        return lab

    def label(self, parent, text, soft=False, **kw):
        return tk.Label(parent, text=text, bg=CARD, fg=SOFT if soft else INK, font=(FONT, 9 if soft else 10), **kw)

    def show(self, key):
        for k, page in self.pages.items():
            page.pack_forget()
            self.tab_labels[k].configure(bg=PRIMARY_DARK, fg='#C5CAE9')
        self.pages[key].pack(fill='both', expand=True)
        self.tab_labels[key].configure(bg=BG, fg=PRIMARY_DARK)
        if key == 'ports':
            self.refresh_ports()
        if key == 'mods' and not self.mods_rows:
            self.mods_list()

    # ---- the pages
    def page_new(self, page):
        self.deps_box = tk.Frame(page, bg=BG)  # (what the tool lacks, said first: empty, and unseen, when it lacks nothing)
        self.deps_box.pack(fill='x')
        box = tk.Frame(page, bg=GOOD, padx=1, pady=1)
        box.pack(fill='x', padx=20, pady=(12, 0))
        inner = tk.Frame(box, bg=GOOD_BG, padx=14, pady=7)
        inner.pack(fill='x')
        tk.Label(inner, text='✔  Use your own backups only.', bg=GOOD_BG, fg=GOOD, font=(FONT, 10, 'bold')).pack(side='left')
        tk.Label(inner, text='No games or keys come with this tool: build only from dumps of games you own, made from your own console.',
                 bg=GOOD_BG, fg=INK, font=(FONT, 9)).pack(side='left', padx=(8, 0))
        box = tk.Frame(page, bg=RISK_LINE, padx=1, pady=1)
        box.pack(fill='x', padx=20, pady=(6, 0))
        inner = tk.Frame(box, bg=RISK_BG, padx=14, pady=7)
        inner.pack(fill='x')
        tk.Label(inner, text='⚠  Use it at your own risk.', bg=RISK_BG, fg=RISK, font=(FONT, 10, 'bold')).pack(side='left')
        tk.Label(inner, text='Experimental, with no warranty: the authors take no responsibility for any damage or loss (console, card, saves, account).',
                 bg=RISK_BG, fg=INK, font=(FONT, 9)).pack(side='left', padx=(8, 0))
        n = 1
        c = self.card(page, n, 'The game', 'your own files; they don\'t leave this computer')
        row = tk.Frame(c, bg=CARD)
        row.pack(fill='x')
        what = 'NSP or NUP' if DEV else 'NSP' if SWITCH else 'NUP'
        ttk.Button(row, text=f'Choose the game ({what})...', style='Soft.TButton', command=self.pick_game).pack(side='left')
        if SWITCH2:
            # (a Switch 2 game dumped as loose files: a folder with base/ and updates/, or exefs/ and romfs/)
            ttk.Button(row, text='or a folder of its files...', style='Text.TButton', command=self.pick_game_folder).pack(side='left', padx=(4, 0))
        self.game_label = self.label(row, 'No game chosen', soft=True)
        self.game_label.pack(side='left', padx=12)
        row = tk.Frame(c, bg=CARD)
        row.pack(fill='x', pady=(8, 0))
        ttk.Button(row, text='+ Update', style='Text.TButton', command=self.pick_update).pack(side='left')
        if SWITCH2:
            ttk.Button(row, text='+ Update (folder)', style='Text.TButton', command=self.pick_update_folder).pack(side='left', padx=(4, 0))
        if SWITCH:
            ttk.Button(row, text='+ DLC', style='Text.TButton', command=self.pick_aoc).pack(side='left', padx=(4, 0))
        self.no_extras = 'No update or DLC (optional)' if SWITCH else 'No separate update (optional; those inside the dump are applied by themselves)'
        self.extras = self.label(row, self.no_extras, soft=True)
        self.extras.pack(side='left', padx=12)
        self.clear_extras = ttk.Button(row, text='Remove', style='Text.TButton', command=self.drop_extras)

        if DEV:
            n += 1
            c = self.card(page, n, 'The console')
            row = tk.Frame(c, bg=CARD)
            row.pack(fill='x')
            self.target_pick = Segmented(row, self.target, [('switch', 'Switch'), ('switch2', 'Switch 2'), ('both', 'Both')], self.sync)
            self.target_pick.pack(side='left')
            self.target_note = self.label(row, 'for a Switch game; a Switch 2 game is for a Switch 2 alone', soft=True)
            self.target_note.pack(side='left', padx=12)
            if not nx.has_switch2():
                self.target_pick.disable('switch2')
                self.target_pick.disable('both')
                self.target_note.configure(text='The Switch 2 kit (ounce-devkit) isn\'t installed.', fg=WARN)

        n += 1
        c = self.card(page, n, 'In the homebrew menu', 'optional: left empty, the game\'s own name, version and icon are used if its files have them')
        row = tk.Frame(c, bg=CARD)
        row.pack(fill='x')
        self.label(row, 'Name:').pack(side='left')
        ttk.Entry(row, textvariable=self.title, width=30).pack(side='left', padx=(6, 14))
        self.label(row, 'Version:').pack(side='left')
        ttk.Entry(row, textvariable=self.version, width=10).pack(side='left', padx=(6, 14))
        ttk.Button(row, text='Icon...', style='Text.TButton', command=self.pick_icon).pack(side='left')
        self.icon_label = self.label(row, 'the game\'s own', soft=True)
        self.icon_label.pack(side='left', padx=6)

        n += 1
        self.card_step = n
        c = self.card(page, n, 'The console\'s SD card')
        row = tk.Frame(c, bg=CARD)
        row.pack(fill='x')
        Segmented(row, self.where, [('sd', 'In this computer'), ('ftp', 'Over the network (FTP)')], self.sync).pack(side='left')
        self.sd_row = tk.Frame(c, bg=CARD)
        self.sd_box = ttk.Combobox(self.sd_row, textvariable=self.sd)
        self.sd_box.pack(side='left', fill='x', expand=True)
        ttk.Button(self.sd_row, text='Refresh', style='Text.TButton', command=self.find_drives).pack(side='left', padx=(6, 0))
        ttk.Button(self.sd_row, text='Folder...', style='Text.TButton', command=lambda: self.sd.set(filedialog.askdirectory() or self.sd.get())).pack(side='left')
        self.ftp_row = tk.Frame(c, bg=CARD)
        row = tk.Frame(self.ftp_row, bg=CARD)
        row.pack(fill='x')
        ttk.Entry(row, textvariable=self.ftp, width=22).pack(side='left')
        self.label(row, 'the console\'s address, for example 192.168.1.50  (open the FTP server on it first; port 5000)', soft=True).pack(side='left', padx=10)
        if SWITCH2:
            ttk.Checkbutton(c, text='Copy only the program (.nro): the game\'s files are on the card already (a port built again)',
                            variable=self.nro_only).pack(anchor='w', pady=(10, 0), side='bottom')
        self.install_row = tk.Frame(c, bg=CARD)
        self.label(self.install_row, 'The build that goes on the card:').pack(side='left', padx=(0, 10))
        Segmented(self.install_row, self.install_target, [('switch', 'Switch'), ('switch2', 'Switch 2')]).pack(side='left')

        row = tk.Frame(page, bg=BG)
        row.pack(fill='x', padx=20, pady=14)
        self.go = ttk.Button(row, text='BUILD AND INSTALL', style='Primary.TButton', command=lambda: self.do_make(True))
        self.go.pack(side='left')
        self.only = ttk.Button(row, text='Build only', style='Soft.TButton', command=lambda: self.do_make(False))
        self.only.pack(side='left', padx=10)
        self.work_buttons = [self.go, self.only]
        if SWITCH:
            self.review = ttk.Button(row, text='Check the game first', style='Soft.TButton', command=self.do_check)
            self.review.pack(side='left')
            self.work_buttons.append(self.review)
            tk.Label(page, text='"Check" says what the game asks of a port and how likely it is to work, without building anything.', bg=BG, fg=SOFT,
                     font=(FONT, 9)).pack(anchor='w', padx=20)

    def page_ports(self, page):
        c = self.card(page, None, 'Ports already built', 'choose one to put it on a card again')
        cols = [('title', 'Game', 420 if not DEV else 380), ('version', 'Version', 120)] + ([('for', 'Built for', 200)] if DEV else [])
        self.tree = ttk.Treeview(c, columns=[k for k, _, _ in cols], show='headings', height=8, selectmode='browse')
        for col, text, width in cols:
            self.tree.heading(col, text=text, anchor='w')
            self.tree.column(col, width=width, anchor='w')
        self.tree.pack(fill='x')
        row = tk.Frame(c, bg=CARD)
        row.pack(fill='x', pady=(12, 0))
        self.install_btn = ttk.Button(row, text='INSTALL ON THE CARD', style='Primary.TButton', command=lambda: self.do_install(False))
        self.install_btn.pack(side='left')
        self.nro_btn = ttk.Button(row, text='The program only (the game\'s files are there already)', style='Soft.TButton', command=lambda: self.do_install(True))
        self.nro_btn.pack(side='left', padx=10)
        self.work_buttons += [self.install_btn, self.nro_btn]
        if EDITION != 'switch':  # (after the tool is updated: every port built again with it, from what was kept of each)
            again = tk.Frame(c, bg=CARD)
            again.pack(fill='x', pady=(10, 0))
            self.remake_btn = ttk.Button(again, text='Build ALL again with this version, and copy the programs', style='Soft.TButton', command=self.do_remake)
            self.remake_btn.pack(side='left')
            self.label(again, 'for after the tool is updated: no dump is needed, and the game\'s files on the card stay', soft=True).pack(side='left', padx=10)
            self.work_buttons.append(self.remake_btn)
        self.label(c, f'Uses the card chosen in "New port" (step {self.card_step}).', soft=True).pack(anchor='w', pady=(10, 0))

        c = self.card(page, None, 'On the card', 'where a port keeps its things')
        self.label(c, f'A port\'s program is /switch/{FOLDER}/<name>.nro. Everything else of it is in its folder at the card\'s root, /{FOLDER}/<name>/:\n'
                      '   save/          the game\'s saved data, as loose files. Copy this folder to back it up; it is apart from the console\'s own saves.\n'
                      '   crash.txt     written when a run ended badly: what happened, and the end of that run\'s log. Send this file with a report.\n'
                      '   log.txt         the last run\'s log (log.prev.txt: the one before).\n'
                      '   romfs.bin    the game\'s files.\n'
                      '   mods/          optional: a file put here, at the path it has in the game, is used in place of the game\'s own.',
                   justify='left').pack(anchor='w')

    def page_mods(self, page):
        c = self.card(page, 1, 'The game', f'a port on the card chosen in "New port" (step {self.card_step})')
        row = tk.Frame(c, bg=CARD)
        row.pack(fill='x')
        self.mods_read_btn = ttk.Button(row, text='Read the card', style='Soft.TButton', command=self.mods_read)
        self.mods_read_btn.pack(side='left')
        self.mods_box = ttk.Combobox(row, textvariable=self.mods_port, state='readonly', width=58)
        self.mods_box.pack(side='left', padx=10)
        self.mods_box.bind('<<ComboboxSelected>>', lambda e: self.mods_list())
        self.mods_note = self.label(c, 'Press "Read the card" to see the ports that are on it.', soft=True, justify='left')
        self.mods_note.pack(anchor='w', pady=(8, 0))

        c = self.card(page, 2, 'Mods for this game', 'tick the ones to have on (click a row): those kept on the card, in /mods, and those of this computer')
        cols = [('on', 'On', 40), ('name', 'Mod', 250), ('where', 'Where it is', 190), ('version', 'Version', 60), ('size', 'Size', 70), ('note', 'Notes', 230)]
        self.mods_tree = ttk.Treeview(c, columns=[k for k, _, _ in cols], show='headings', height=7, selectmode='browse')
        for col, text, width in cols:
            self.mods_tree.heading(col, text=text, anchor='w')
            self.mods_tree.column(col, width=width, anchor='w')
        self.mods_tree.pack(fill='x')
        self.mods_tree.tag_configure('off', foreground=SOFT)
        self.mods_tree.tag_configure('warn', foreground=WARN)
        self.mods_tree.bind('<Button-1>', self.mods_click)
        self.mods_about = self.label(c, '', soft=True, justify='left', wraplength=820)
        self.mods_about.pack(anchor='w', pady=(8, 0))
        self.mods_warn = tk.Label(c, text='', bg=CARD, fg=WARN, font=(FONT, 9), justify='left', wraplength=820)
        self.mods_warn.pack(anchor='w', pady=(4, 0))
        row = tk.Frame(c, bg=CARD)
        row.pack(fill='x', pady=(10, 0))
        self.mods_apply_btn = ttk.Button(row, text='APPLY TO THE CARD', style='Primary.TButton', command=self.mods_apply)
        self.mods_apply_btn.pack(side='left')
        self.mods_off_btn = ttk.Button(row, text='Take all off', style='Soft.TButton', command=lambda: self.mods_apply(none=True))
        self.mods_off_btn.pack(side='left', padx=10)
        # (a file is known to be on the card by its size: a mod changed with no file's size changing needs every file copied again)
        ttk.Checkbutton(row, text='Copy every file again', variable=self.mods_again).pack(side='left', padx=(0, 10))
        self.label(row, 'Of two that overlap, the lower one wins:', soft=True).pack(side='left', padx=(10, 0))
        ttk.Button(row, text='▲ up', style='Text.TButton', command=lambda: self.mods_move(-1)).pack(side='left')
        ttk.Button(row, text='▼ down', style='Text.TButton', command=lambda: self.mods_move(1)).pack(side='left')
        self.work_buttons += [self.mods_apply_btn, self.mods_off_btn, self.mods_read_btn]

        c = self.card(page, None, 'Mods on this computer', 'a folder of mods to send to the card: one folder for each mod')
        row = tk.Frame(c, bg=CARD)
        row.pack(fill='x')
        ttk.Entry(row, textvariable=self.mods_dir).pack(side='left', fill='x', expand=True)
        ttk.Button(row, text='Choose...', style='Text.TButton', command=self.mods_pick).pack(side='left', padx=(6, 0))
        ttk.Button(row, text='Open', style='Text.TButton', command=self.mods_open).pack(side='left')
        ttk.Button(row, text='Refresh', style='Text.TButton', command=self.mods_list).pack(side='left')
        row = tk.Frame(c, bg=CARD)
        row.pack(fill='x', pady=(8, 0))
        self.mods_send_btn = ttk.Button(row, text='Send all to the card', style='Soft.TButton', command=self.mods_send)
        self.mods_send_btn.pack(side='left')
        self.mods_send1_btn = ttk.Button(row, text='Send the selected one', style='Soft.TButton', command=lambda: self.mods_send(one=True))
        self.mods_send1_btn.pack(side='left', padx=(8, 0))
        self.work_buttons.append(self.mods_send1_btn)
        self.mods_local = self.label(row, '', soft=True)
        self.mods_local.pack(side='left', padx=10)
        self.work_buttons.append(self.mods_send_btn)
        self.label(c, 'A mod of this computer can be ticked and applied as it is: its files go straight to the game\'s folder on the card.\n'
                      'Sending puts a mod on the card without putting it on for any game: it is there for later, from this window or from the console.\n'
                      'Mods are kept on the card, in the folder "mods" at its root: a folder copied there by hand is a mod too. Putting one on copies its files\n'
                      'to the port\'s own mods folder, where the game reads them; taking it off removes them, and the mod stays in "mods" for the next time.\n'
                      'A mod is a folder with mod.json (its name, and the game it is for) and its files under "files", each at the path it has in the game.\n'
                      'Two mods that have the same file overlap: the game can get only one, and this page says which.',
                   soft=True, justify='left').pack(anchor='w', pady=(8, 0))

    # ---- the mods page: asking tools/mods.py, and showing what it says
    def card_where(self, quiet=False):
        """[--sd x] or [--ftp host] of the card chosen in "New port"; None (said, unless quiet) if it isn't filled in."""
        if self.where.get() == 'sd':
            sd = self.sd.get().split('  ')[0].strip()
            if sd:
                return ['--sd', sd]
        elif self.ftp.get().strip():
            return ['--ftp', self.ftp.get().strip()]
        if not quiet:
            messagebox.showwarning('No card', f'Choose the console\'s card first: "New port", step {self.card_step} (the card in this computer, or the console\'s address).')
        return None

    def mods_ask(self, args, then):
        """A question to tools/mods.py, asked apart from the window's thread; then(lines) is called with what it printed."""
        self.mods_asked += 1
        mine = self.mods_asked
        cmd = [sys.executable, '-u', f'{ROOT}/nxrelink.py', 'mods'] + args + ['--mods', self.mods_dir.get()]

        def work():
            flags = subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0
            try:
                out = subprocess.run(cmd, capture_output=True, text=True, encoding='utf-8', errors='replace', cwd=ROOT, creationflags=flags, timeout=120,
                                     env=dict(os.environ, NX2_GUI='1', PYTHONIOENCODING='utf-8'))
                lines = (out.stdout + out.stderr).splitlines()
            except (OSError, subprocess.SubprocessError) as e:
                lines = [str(e)]
            self.lines.put(('asked', mine, then, lines))  # (answered on the window's own thread: pump)
        threading.Thread(target=work, daemon=True).start()

    def mods_read(self):
        where = self.card_where()
        if not where:
            return
        self.mods_note.configure(text='Reading the card...', fg=SOFT)

        def got(lines):
            keep = self.mods_chosen()
            self.mods_ports = {}
            for line in lines:
                if line.startswith('@@port|'):
                    _, port, title, title_id, on = line.split('|')
                    self.mods_ports[port] = (title, title_id, int(on))
            shown = [f'{title or "game not known"}   ·   {port}' + (f'   ·   {on} mod(s) on' if on else '') for port, (title, _, on) in self.mods_ports.items()]
            self.mods_box['values'] = shown
            if not self.mods_ports:
                said = [x for x in lines if x.strip() and not x.startswith('@@')]
                self.mods_note.configure(text=(said[-1] if said else 'No port is on this card.')[:200], fg=WARN)
                self.mods_port.set('')
                return
            self.mods_port.set(shown[list(self.mods_ports).index(keep)] if keep in self.mods_ports else shown[0])
            self.mods_note.configure(text=f'{len(self.mods_ports)} port(s) on the card. A port whose game is "not known" was built on another computer: '
                                          'every mod is offered for it.', fg=SOFT)
            self.mods_list()
        self.mods_ask(['ports'] + where, got)

    def mods_chosen(self):
        shown = self.mods_port.get()
        return shown.split('   ·   ')[1] if '   ·   ' in shown else ''

    def mods_pick(self):
        self.mods_dir.set(filedialog.askdirectory() or self.mods_dir.get())
        self.mods_list()

    def mods_list(self):
        port, where = self.mods_chosen(), self.card_where(quiet=True)

        def got(lines):
            self.mods_rows, on, loose, said, here = {}, {}, None, [], []
            for line in lines:
                if line.startswith('@@mod|'):
                    f = line.split('|', 15)[1:]
                    self.mods_rows[f[0]] = dict(name=f[1], version=f[2], game=f[3] or f[4], fit=f[5], files=int(f[7]), size=int(f[8]),
                                                shared=[x for x in f[9].split(',') if x], needs=[x for x in f[10].split(',') if x], bad=f[11], about=f[12],
                                                here=f[13] if len(f) > 13 else 'no', code=len(f) > 14 and f[14] == 'code')
                    if int(f[6]):
                        on[int(f[6])] = f[0]
                elif line.startswith('@@loose|'):
                    loose = line.split('|', 2)
                elif line.startswith('@@local|'):
                    here.append(line.split('|', 8)[2])
                elif line.startswith(('On the card and no longer', 'No mods in', 'No FTP server')):
                    said.append(line)
            self.mods_on = [on[k] for k in sorted(on)]
            self.mods_loose = (f'{loose[1]} file(s) in the port\'s mods folder were put there by hand ({loose[2]}'
                               f'{" ..." if int(loose[1]) > 6 else ""}): they are left alone.') if loose else ''
            self.mods_said = ' '.join(said)
            self.mods_local.configure(text=(f'{len(here)} not on the card yet, or different there: ' + ', '.join(here)[:70]) if here else
                                      'The card has every mod of this folder.' if port and where else '', fg=WARN if here else SOFT)
            self.mods_paint()
        self.mods_ask(['list'] + ([port] + where if port and where else []), got)

    def mods_name(self, mid):
        return self.mods_rows[mid]['name'] if mid in self.mods_rows else mid

    def mods_paint(self):
        keep = self.mods_tree.selection()
        self.mods_tree.delete(*self.mods_tree.get_children())
        # (those that are on first, in their order: the lower of two that overlap is the one the game gets)
        order = self.mods_on + [m for m in self.mods_rows if m not in self.mods_on]
        notes = []
        for mid in order:
            m = self.mods_rows.get(mid)
            if not m or m['fit'] == 'no':
                continue
            on = mid in self.mods_on
            note = []
            if m.get('code'):
                note.append('runs code in the game')
            if m['bad']:
                note.append('can\'t be used: ' + m['bad'])
            clash = [o for o in m['shared'] if on and o in self.mods_on]
            if clash:
                wins = max(clash + [mid], key=self.mods_on.index)
                note.append('overlaps with ' + ', '.join(self.mods_name(o) for o in clash) + (': this one wins' if wins == mid else ': the other wins'))
                if wins == mid:
                    notes.append(f'"{m["name"]}" and ' + ', '.join(f'"{self.mods_name(o)}"' for o in clash) + ' have files in common: '
                                 f'the game gets those of "{m["name"]}", the lower one.')
            elif m['shared']:
                note.append('would overlap with ' + ', '.join(self.mods_name(o) for o in m['shared']))
            lacks = [n for n in m['needs'] if on and n not in self.mods_on]
            if lacks:
                note.append('needs ' + ', '.join(self.mods_name(n) for n in lacks))
                notes.append(f'"{m["name"]}" needs ' + ', '.join(f'"{self.mods_name(n)}"' for n in lacks) + ', which isn\'t ticked.')
            size = amount(m['size']) if m['size'] >= 1 << 20 else f'{m["size"] // 1024} KB'
            where = {'only': 'this computer only', 'same': 'SD card, and this computer', 'different': 'SD card (this computer\'s differs)',
                     'no': 'SD card'}[m['here']]
            self.mods_tree.insert('', 'end', iid=mid, values=('☑' if on else '☐', m['name'], where, m['version'], size, '; '.join(note)),
                                  tags=(('warn',) if lacks or m['bad'] else () if on else ('off',)))
        if keep and self.mods_tree.exists(keep[0]):
            self.mods_tree.selection_set(keep[0])
        hidden = sum(1 for m in self.mods_rows.values() if m['fit'] == 'no')
        if not self.mods_tree.get_children():
            notes.append((f'No mod for this game in /mods on the card' + (f' ({hidden} for other games)' if hidden else '') + '.') if self.mods_chosen() else
                         'Read the card and choose a port to see the mods for its game.')
        self.mods_warn.configure(text='\n'.join(notes + [x for x in (self.mods_loose, self.mods_said) if x]))
        self.mods_paint_about()

    def mods_paint_about(self):
        sel = self.mods_tree.selection()
        m = self.mods_rows.get(sel[0]) if sel else None
        self.mods_about.configure(text=f'{m["name"]}: {m["about"] or "no description"}   ({m["files"]} files)' if m else '')

    def mods_click(self, event):
        mid = self.mods_tree.identify_row(event.y)
        if not mid or self.proc or self.mods_rows[mid]['bad']:
            return None
        if mid in self.mods_on:
            self.mods_on.remove(mid)
        else:
            self.mods_on.append(mid)
        self.mods_paint()
        self.mods_tree.selection_set(mid)
        self.mods_paint_about()
        return 'break'

    def mods_move(self, by):
        sel = self.mods_tree.selection()
        if not sel or sel[0] not in self.mods_on:
            return
        i = self.mods_on.index(sel[0])
        self.mods_on.insert(min(max(i + by, 0), len(self.mods_on) - 1), self.mods_on.pop(i))
        self.mods_paint()

    def mods_open(self):
        d = self.mods_dir.get()
        os.makedirs(d, exist_ok=True)
        if os.name == 'nt':
            os.startfile(d)  # noqa: S606
        else:
            subprocess.Popen(['open' if sys.platform == 'darwin' else 'xdg-open', d])

    def mods_send(self, one=False):
        """This computer's mods copied to the card's /mods, where they are kept: none is put on by this."""
        where = self.card_where()
        if not where:
            return
        sel = self.mods_tree.selection()
        if one and (not sel or self.mods_rows[sel[0]]['here'] not in ('only', 'different')):
            messagebox.showinfo('Send', 'Select in the list a mod that is on this computer and not on the card (or different there).')
            return
        self.start('mods', ['mods', 'send'] + ([sel[0]] if one else []) + where + ['--mods', self.mods_dir.get()] +
                   (['--again'] if self.mods_again.get() else []))

    def mods_apply(self, none=False):
        port, where = self.mods_chosen(), self.card_where()
        if not where:
            return
        if not port:
            messagebox.showwarning('No port', 'Press "Read the card" and choose the port first.')
            return
        want = [] if none else [m for m in self.mods_on if m in self.mods_rows]
        if none and not messagebox.askyesno('Take all off', f'Take every mod off "{port}"? The game\'s own files are used again. '
                                                            'Files put in its mods folder by hand stay.'):
            return
        self.start('mods', ['mods', 'set', port] + want + where + ['--mods', self.mods_dir.get()] + (['--again'] if self.mods_again.get() else []))

    def page_settings(self, page):
        if SWITCH:
            c = self.card(page, None, 'Your console\'s keys', 'prod.keys, to open your games; never shown or sent anywhere')
            row = tk.Frame(c, bg=CARD)
            row.pack(fill='x')
            ttk.Entry(row, textvariable=self.keys).pack(side='left', fill='x', expand=True)
            ttk.Button(row, text='Choose...', style='Text.TButton',
                       command=lambda: self.keys.set(filedialog.askopenfilename(filetypes=[('prod.keys', '*.keys'), ALL]) or self.keys.get())).pack(side='left', padx=(6, 0))
        c = self.card(page, None, 'The SD card\'s format')
        Segmented(c, self.fmt, [('auto', 'Detect'), ('fat32', 'FAT32'), ('exfat', 'exFAT')]).pack(anchor='w')
        self.label(c, 'FAT32 can\'t hold a file of 4 GB: a large game\'s files go in parts of 2 GB. On exFAT they go in one file.\n'
                      'Over the network it can\'t be detected: parts are used, which work on both.', soft=True, justify='left').pack(anchor='w', pady=(8, 0))
        c = self.card(page, None, 'When building')
        row = tk.Frame(c, bg=CARD)
        row.pack(fill='x')
        self.label(row, 'Short name:').pack(side='left')
        ttk.Entry(row, textvariable=self.name, width=24).pack(side='left', padx=8)
        self.label(row, 'the name of the port\'s folder and files on the card. Empty: made from the game\'s name.', soft=True).pack(side='left')
        text = ('Extract the game\'s files (untick it if they were extracted already under the same short name: saves time)' if EDITION == 'switch' else
                'Send the game\'s files to the card (untick it if they are on the card already: saves time)' if EDITION == 'switch2' else
                'Extract the game\'s files (Switch) / send them to the card (Switch 2): untick it if that was done already')
        if EDITION != 'switch2':  # (a Switch 2 game's files aren't extracted: "Copy only the program" in "New port" is its choice)
            ttk.Checkbutton(c, text=text, variable=self.romfs).pack(anchor='w', pady=(8, 0))
        c = self.card(page, None, 'What this tool needs')
        self.deps_btn = ttk.Button(c, text='Check and install what is needed', style='Soft.TButton', command=lambda: self.start('setup', ['setup']))
        self.deps_btn.pack(anchor='w')
        self.work_buttons.append(self.deps_btn)
        self.label(c, 'Installs the Python packages that are missing, and says where the rest is had:', soft=True).pack(anchor='w', pady=(8, 0))
        row = tk.Frame(c, bg=CARD)
        row.pack(anchor='w', pady=(4, 0))
        for text, url in nx.links('devkitA64') + (nx.links('ounce-devkit') if SWITCH2 else []):
            self.link(row, text, url).pack(side='left', padx=(0, 14))

    def page_advanced(self, page):
        box = tk.Frame(page, bg=WARN, padx=1, pady=1)
        box.pack(fill='x', padx=20, pady=(12, 0))
        inner = tk.Frame(box, bg=WARN_BG, padx=16, pady=12)
        inner.pack(fill='x')
        tk.Label(inner, text='⚠  Advanced options: use with care', bg=WARN_BG, fg=WARN, font=(FONT, 11, 'bold')).pack(anchor='w')
        tk.Label(inner, bg=WARN_BG, fg=INK, font=(FONT, 9), justify='left', wraplength=800,
                 text='These change what the game is given when it runs. A wrong value can make a game that worked stop at its start or while it '
                      'loads, or behave in ways nobody else sees. The defaults are what every tested game runs with: leave them alone unless a '
                      'game needs something else, change one thing at a time, and say what you changed if you report a problem. '
                      'They are used at your own risk.').pack(anchor='w', pady=(6, 0))
        if SWITCH2:
            c = self.card(page, None, 'Memory the game is told of', 'Switch 2 games' if DEV else None)
            row = tk.Frame(c, bg=CARD)
            row.pack(fill='x')
            ttk.Spinbox(row, textvariable=self.memory, from_=MEMORY[0], to=MEMORY[1], increment=100, width=8).pack(side='left')
            self.label(row, f'MB   ({MEMORY[0]} to {MEMORY[1]}). Empty: decided for the game when its port is made').pack(side='left', padx=8)
            self.label(c, 'Nothing is set aside: the game takes memory from the console as it asks for it, and this is only the most it is told there is.\n'
                          f'Left empty it is {MEMORY[2]} MB, or the console\'s own {MEMORY[1]} for a game whose own table of heaps asks for that (read from its files).\n'
                          'A game that sizes itself by it takes less with a smaller number. More than there is is cut to what there is.',
                       soft=True, justify='left').pack(anchor='w', pady=(8, 0))
            c = self.card(page, None, 'Modules kept whole', 'Switch 2 games' if DEV else None)
            row = tk.Frame(c, bg=CARD)
            row.pack(fill='x')
            ttk.Combobox(row, textvariable=self.whole, values=['auto', 'all', 'none', 'main,sdk'], width=24).pack(side='left')
            self.label(row, 'auto (default), all, none, or modules\' names with commas between them').pack(side='left', padx=8)
            self.label(c, 'A whole module is mapped as on its console; the others are relinked into the port. The console\'s loader maps eight images\n'
                          'for a program: "all" leaves a game that loads modules of its own as it runs no room for them. "auto" decides this by the game.',
                       soft=True, justify='left').pack(anchor='w', pady=(8, 0))
        if SWITCH2:
            c = self.card(page, None, 'Sources of your own built into the port', 'Switch 2 games' if DEV else None)
            row = tk.Frame(c, bg=CARD)
            row.pack(fill='x')
            ttk.Button(row, text='Add...', style='Soft.TButton', command=self.pick_also).pack(side='left')
            ttk.Button(row, text='Clear', style='Text.TButton', command=self.clear_also).pack(side='left', padx=6)
            self.label(row, '', textvariable=self.also_text).pack(side='left', padx=8)
            self.label(c, 'C files with code that runs in the game ("hooks"): each names a function that is called once before the game starts and may\n'
                          'change which function a table of the game\'s points to. runtime/nx2_hook.h says what such a file may use. A file is written for\n'
                          'one version of one game. They are kept with the port, and "Build again" builds them in again.',
                       soft=True, justify='left').pack(anchor='w', pady=(8, 0))
        c = self.card(page, None, 'Detailed log')
        ttk.Checkbutton(c, text='When installing, turn on the port\'s detailed log (the file "debug" in its folder on the card)', variable=self.long_log).pack(anchor='w')
        self.label(c, 'The port then writes every call the game makes to log.txt: much slower, and a large file. Delete "debug" from the port\'s folder to turn it off.',
                   soft=True, justify='left').pack(anchor='w', pady=(8, 0))
        row = tk.Frame(page, bg=BG)
        row.pack(fill='x', padx=20, pady=14)
        ttk.Button(row, text='Restore the defaults', style='Soft.TButton', command=self.defaults).pack(side='left')

    def check_deps(self):
        """The first page's notice of what the tool needs and hasn't got: there only while something is missing."""
        for w in self.deps_box.winfo_children():
            w.destroy()
        self.deps_box.configure(height=1)
        try:
            missing = nx.setup(install=False, quiet=True)
        except Exception:  # noqa: BLE001
            missing = []
        # (the notice takes room of the page: the window is made no shorter than what shows the buttons under it)
        self.root.minsize(900, min(850 + (70 + 42 * len(missing) if missing else 0), self.root.winfo_screenheight() - 80))
        if not missing:
            return
        box = tk.Frame(self.deps_box, bg=WARN, padx=1, pady=1)
        box.pack(fill='x', padx=20, pady=(12, 0))
        inner = tk.Frame(box, bg=WARN_BG, padx=16, pady=10)
        inner.pack(fill='x')
        fix = ttk.Button(inner, text='Check and install', style='Soft.TButton', command=lambda: self.start('setup', ['setup']))
        fix.pack(side='right', padx=(12, 0))
        self.deps_fix = fix
        tk.Label(inner, text=f'⚠  This tool is missing {len(missing)} thing{"s" if len(missing) > 1 else ""} it needs: a port can\'t be built until '
                             f'{"they are" if len(missing) > 1 else "it is"} here', bg=WARN_BG, fg=WARN, font=(FONT, 10, 'bold')).pack(anchor='w')
        for what, why, how in missing:
            row = tk.Frame(inner, bg=WARN_BG)
            row.pack(anchor='w', pady=(4, 0))
            tk.Label(row, text=f'•  {what}  (for {why})', bg=WARN_BG, fg=INK, font=(FONT, 9, 'bold')).pack(side='left')
            for text, url in nx.links(what):
                self.link(row, text, url, WARN_BG).pack(side='left', padx=(10, 0))
            tk.Label(inner, text='     ' + how, bg=WARN_BG, fg=SOFT, font=(FONT, 9), justify='left').pack(anchor='w')
        tk.Label(inner, text='"Check and install" installs the Python packages by itself; the rest is installed from the links above.',
                 bg=WARN_BG, fg=INK, font=(FONT, 9), justify='left').pack(anchor='w', pady=(6, 0))

    def defaults(self):
        self.memory.set('')
        self.whole.set('auto')
        self.long_log.set(False)
        self.clear_also()

    def pick_also(self):
        self.also += [p for p in filedialog.askopenfilenames(filetypes=[('C source', '*.c'), ALL]) if p not in self.also]
        self.also_text.set(', '.join(os.path.basename(p) for p in self.also) or 'none')

    def clear_also(self):
        self.also = []
        self.also_text.set('none')

    def advanced_args(self, switch2_game):
        """The advanced options that aren't at their defaults, as arguments; None (said) if one is wrong."""
        args = []
        if switch2_game:
            memory = self.memory.get().strip()
            if memory and (not memory.isdigit() or not MEMORY[0] <= int(memory) <= MEMORY[1]):
                messagebox.showwarning('Advanced: memory', f'The memory is a number of MB from {MEMORY[0]} to {MEMORY[1]}, or empty.')
                return None
            whole = self.whole.get().strip().replace(' ', '') or 'auto'
            if memory:
                args += ['--memory', memory]
            if whole != 'auto':
                args += ['--whole', whole]
            for path in self.also:
                args += ['--also', path]
            if args and not messagebox.askyesno('Advanced options', 'This port will be built with advanced options changed:\n\n   ' + ' '.join(args) +
                                                '\n\nThe game may not start, or may stop while it loads. Build it this way?', icon='warning'):
                return None
        return args

    # ---- what is being done, always in view
    def status_panel(self):
        outer = tk.Frame(self.root, bg=LINE, pady=1)
        outer.pack(side='bottom', fill='x')
        p = tk.Frame(outer, bg=CARD, padx=20, pady=12)
        p.pack(fill='x')
        self.steps_row = tk.Frame(p, bg=CARD)
        self.steps_row.pack(fill='x')
        row = tk.Frame(p, bg=CARD)
        row.pack(fill='x', pady=(8, 2))
        self.headline = tk.Label(row, text='', bg=CARD, fg=INK, font=(FONT, 12, 'bold'), anchor='w')
        self.headline.pack(side='left')
        self.percent = tk.Label(row, text='', bg=CARD, fg=PRIMARY, font=(FONT, 12, 'bold'))
        self.percent.pack(side='right')
        self.bar = ttk.Progressbar(p, style='Bar.Horizontal.TProgressbar', maximum=1000)
        self.bar.pack(fill='x', pady=(2, 4))
        row = tk.Frame(p, bg=CARD)
        row.pack(fill='x')
        self.detail = tk.Label(row, text='', bg=CARD, fg=SOFT, font=(FONT, 9), anchor='w', justify='left', wraplength=700)
        self.detail.pack(side='left')
        self.stop = ttk.Button(row, text='Stop', style='Text.TButton', command=self.halt)
        self.more = ttk.Button(row, text='Show details', style='Text.TButton', command=self.toggle_log)
        self.more.pack(side='right')
        self.log_frame = tk.Frame(p, bg=CARD)
        self.text = tk.Text(self.log_frame, height=11, wrap='word', state='disabled', font=('Consolas', 9), bg='#FAFBFD', fg=INK, relief='flat',
                            highlightthickness=1, highlightbackground=LINE, padx=8, pady=6)
        sb = ttk.Scrollbar(self.log_frame, command=self.text.yview)
        self.text.configure(yscrollcommand=sb.set)
        sb.pack(side='right', fill='y')
        self.text.pack(side='left', fill='both', expand=True)
        self.log_open = False

    def toggle_log(self, want=None):
        self.log_open = (not self.log_open) if want is None else want
        if self.log_open:
            self.log_frame.pack(fill='both', expand=True, pady=(8, 0))
            self.more.configure(text='Hide details')
        else:
            self.log_frame.pack_forget()
            self.more.configure(text='Show details')

    def draw_steps(self, names, current, failed=False):
        for w in self.steps_row.winfo_children():
            w.destroy()
        for i, name in enumerate(names, 1):
            done, now = i < current, i == current
            color = (BAD if failed else PRIMARY) if now else (GOOD if done else '#B0B5C3')
            tk.Label(self.steps_row, text='✓' if done else str(i), bg=color, fg='white', font=(FONT, 8, 'bold'), width=2).pack(side='left')
            tk.Label(self.steps_row, text=name, bg=CARD, fg=INK if now or done else '#9AA1B2', font=(FONT, 9, 'bold' if now else 'normal')).pack(side='left', padx=(5, 0))
            if i < len(names):
                tk.Label(self.steps_row, text='―――', bg=CARD, fg=GOOD if done else LINE, font=(FONT, 8)).pack(side='left', padx=6)

    def idle(self, text):
        self.draw_steps([], 0)
        self.headline.configure(text=text, fg=INK)
        self.percent.configure(text='')
        self.detail.configure(text='')
        self.bar.stop()
        self.bar.configure(style='Bar.Horizontal.TProgressbar', mode='determinate', value=0)

    def say(self, s):
        self.text.configure(state='normal')
        self.text.insert('end', s)
        self.text.see('end')
        self.text.configure(state='disabled')

    # ---- the form
    def set_game(self, p, shown):
        self.game.set(p)
        self.game_label.configure(text=shown, fg=INK)
        self.name.set(nx.slug(nx.title_of(p)))

    def pick_game(self):
        p = filedialog.askopenfilename(filetypes=GAMES)
        if p:
            self.set_game(p, os.path.basename(p))

    def pick_game_folder(self):
        p = filedialog.askdirectory()
        if p:
            self.set_game(p, os.path.basename(p.rstrip('/')) + '/')

    def pick_update(self):
        p = filedialog.askopenfilename(filetypes=GAMES)
        if p:
            self.update.set(p)
            self.paint_extras()

    def pick_update_folder(self):
        p = filedialog.askdirectory()
        if p:
            self.update.set(p)
            self.paint_extras()

    def pick_aoc(self):
        self.aocs += [p for p in filedialog.askopenfilenames(filetypes=[NSP, ALL]) if p not in self.aocs]
        self.paint_extras()

    def pick_icon(self):
        p = filedialog.askopenfilename(filetypes=PICTURES)
        self.icon.set(p or '')
        self.icon_label.configure(text=os.path.basename(p) if p else 'the game\'s own', fg=INK if p else SOFT)

    def drop_extras(self):
        self.update.set('')
        self.aocs = []
        self.paint_extras()

    def paint_extras(self):
        parts = ([f'Update: {os.path.basename(self.update.get().rstrip("/"))}'] if self.update.get() else []) + \
                ([f'{len(self.aocs)} DLC'] if self.aocs else [])
        self.extras.configure(text='   ·   '.join(parts) or self.no_extras, fg=INK if parts else SOFT)
        if parts:
            self.clear_extras.pack(side='left')
        else:
            self.clear_extras.pack_forget()

    def find_drives(self):
        drives = removable_drives()
        self.sd_box['values'] = [label for _, label in drives]
        if drives and (not self.sd.get() or '·' in self.sd.get()):
            self.sd.set(drives[0][1])

    def refresh_ports(self):
        self.ports = nx.built()
        keep = self.tree.selection()
        self.tree.delete(*self.tree.get_children())
        for name, (title, version, targets) in self.ports.items():
            self.tree.insert('', 'end', iid=name, values=(title, version) + ((' and '.join(TARGETS[t] for t in targets),) if DEV else ()))
        if keep and keep[0] in self.ports:
            self.tree.selection_set(keep[0])
        elif self.ports:
            self.tree.selection_set(next(iter(self.ports)))

    def sync(self):
        """What is shown follows what is chosen: the card's row, and the build for the card when both are built."""
        t = self.target.get()
        if t != 'both':
            self.install_target.set(t)
        for w in (self.sd_row, self.ftp_row, self.install_row):
            w.pack_forget()
        (self.sd_row if self.where.get() == 'sd' else self.ftp_row).pack(fill='x', pady=(10, 0))
        if t == 'both':
            self.install_row.pack(fill='x', pady=(10, 0))

    def card_args(self, switch2_port):
        """The arguments that say where the card is, or None (said) if that isn't filled in."""
        if self.where.get() == 'sd':
            sd = self.sd.get().split('  ')[0].strip()
            if not sd:
                messagebox.showwarning('No card', 'Put the console\'s SD card in this computer and press "Refresh", or choose a folder.')
                return None
            where = ['--sd', sd]
        else:
            host = self.ftp.get().strip()
            if not host:
                messagebox.showwarning('No address', 'Type the console\'s address. The FTP server has to be open on it.')
                return None
            where = ['--ftp', host]
        return where + ['--sd-format', self.fmt.get()] + (['--long-log'] if self.long_log.get() else [])

    def game_args(self, with_dlc=True):
        game = self.game.get().strip()
        if not game or not os.path.exists(game):
            messagebox.showwarning('No game', 'Choose the game first (step 1).')
            return None
        if not DEV and is_switch2_game(game) != SWITCH2:
            messagebox.showwarning('Not this tool\'s kind of game',
                                   f'{TOOL} takes a NUP container, or a folder of a game\'s loose files. A Switch game (NSP) is nx-relink\'s.' if SWITCH2 else
                                   f'{TOOL} takes a Switch game\'s NSP. A NUP container or a folder is nx2-relink\'s.')
            return None
        args = [game]
        if self.update.get().strip():
            args += ['--update', self.update.get().strip()]
        if not is_switch2_game(game):
            if with_dlc:
                for p in self.aocs:
                    args += ['--aoc', p]
            if self.keys.get().strip():
                args += ['--keys', self.keys.get().strip()]
        return args

    def do_check(self):
        args = self.game_args(with_dlc=False)
        if args and is_switch2_game(args[0]):
            messagebox.showinfo('Check', 'The check is for NSP games: a NUP or folder dump is checked as it is built.')
        elif args:
            self.start('check', ['check'] + args)

    def do_make(self, install):
        args = self.game_args()
        if not args:
            return
        two = is_switch2_game(args[0])
        if self.name.get().strip():
            args += ['--name', self.name.get().strip()]
        for option, var in (('--title', self.title), ('--version', self.version), ('--icon', self.icon)):
            if var.get().strip():
                args += [option, var.get().strip()]
        if DEV and not two:
            args += ['--target', self.target.get()]
        if not self.romfs.get() or (two and install and self.nro_only.get()):
            args.append('--no-romfs')
        extra = self.advanced_args(two)
        if extra is None:
            return
        args += extra
        if install:
            card = self.card_args(two)
            if not card:
                return
            args += card + (['--install-target', self.install_target.get()] if DEV and not two else [])
        self.start(('make2' if two else 'make') + ('+install' if install else ''), ['make'] + args)

    def do_install(self, nro_only):
        chosen = self.tree.selection()
        if not chosen:
            messagebox.showwarning('No port', 'No port is built yet: use "New port" first.')
            return
        name = chosen[0]
        target = self.install_target.get() if DEV else EDITION
        if target not in self.ports[name][2]:
            if len(self.ports[name][2]) != 1:
                messagebox.showwarning('Not built', f'"{self.ports[name][0]}" isn\'t built for {TARGETS[target]}. Change the build in "New port", or build it again.')
                return
            target = self.ports[name][2][0]
        card = self.card_args(target == 'switch2')
        if not card:
            return
        self.start('install', ['install', name] + card + ['--target', target] + (['--no-romfs'] if nro_only else []))

    def do_remake(self):
        if not self.ports:
            messagebox.showwarning('No port', 'No port is built yet: use "New port" first.')
            return
        card = self.card_args(True)
        if not card:
            return
        card = [a for a in card if a != '--long-log']
        if messagebox.askokcancel('Build all again', f'{len(self.ports)} port(s) will be built again with this version, one after the other, and each one\'s '
                                                     'program copied to the card. The game\'s files on the card are not touched.'):
            self.start('remake', ['remake'] + card)

    # ---- running a command, and following it
    def busy(self, on):
        for b in self.work_buttons + ([self.deps_fix] if self.deps_fix and self.deps_fix.winfo_exists() else []):
            b.configure(state='disabled' if on else 'normal')
        if on:
            self.stop.pack(side='right', padx=(0, 6))
        else:
            self.stop.pack_forget()

    def start(self, kind, args):
        if self.proc:
            return
        self.kind, self.step, self.began, self.track, self.last_line, self.refused = kind, 1, time.time(), None, '', ''
        self.say('\n> python nxrelink.py ' + ' '.join(f'"{a}"' if ' ' in a else a for a in args) + '\n')
        self.busy(True)
        self.draw_steps(STEPS[kind], 1)
        self.headline.configure(text=STEPS[kind][0] + '...', fg=INK)
        self.percent.configure(text='')
        self.detail.configure(text='Starting...')
        self.bar.configure(style='Bar.Horizontal.TProgressbar', mode='indeterminate')
        self.bar.start(12)
        flags = subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0
        self.proc = subprocess.Popen([sys.executable, '-u', f'{ROOT}/nxrelink.py'] + args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                     encoding='utf-8', errors='replace', cwd=ROOT, creationflags=flags,
                                     env=dict(os.environ, NX2_GUI='1', PYTHONIOENCODING='utf-8'))

        def read(p):
            for line in p.stdout:
                self.lines.put(line)
            self.lines.put(('done', p.wait()))
        threading.Thread(target=read, args=(self.proc,), daemon=True).start()

    def halt(self):
        if self.proc and messagebox.askyesno('Stop', 'Stop what is being done? What was half done stays incomplete.'):
            self.stopped = True
            self.proc.kill()

    def on_phase(self, i, n):
        names = STEPS[self.kind]
        self.step = min(i, len(names))
        self.track = None
        if self.step < len(names) or self.kind in ('check', 'setup'):
            self.draw_steps(names, self.step)
            self.headline.configure(text=names[self.step - 1] + '...')
            self.percent.configure(text='')
            self.detail.configure(text='')
            self.bar.configure(mode='indeterminate')
            self.bar.start(12)

    def on_progress(self, kind, detail, done, total):
        now = time.time()
        if not self.track or self.track[0] != (kind, detail):
            self.track = ((kind, detail), now, done)
        frac = 1.0 if total <= 0 else done / total
        self.bar.stop()
        self.bar.configure(mode='determinate', value=int(frac * 1000))
        self.percent.configure(text=f'{int(frac * 100)} %')
        what = KINDS.get(kind, kind)
        if kind in ('compile', 'relink', 'link'):
            self.headline.configure(text=what + (f' for {TARGETS.get(detail, detail)}' if DEV else ''))
            self.detail.configure(text=f'{done} of {total} files' if kind == 'compile' else '')
            return
        self.headline.configure(text=what + (f': {os.path.basename(detail)}' if detail and kind in ('copy', 'verify') else ''))
        elapsed, moved = now - self.track[1], done - self.track[2]
        text = f'{amount(done)} of {amount(total)}'
        if elapsed > 1.5 and moved > 0 and done < total:
            speed = moved / elapsed
            text += f'   ·   {speed / (1 << 20):.0f} MB/s   ·   about {clock((total - done) / speed)} left'
        self.detail.configure(text=text)

    def finish(self, code):
        self.proc = None
        self.bar.stop()
        self.busy(False)
        took = clock(time.time() - self.began)
        names = STEPS[self.kind]
        if code == 0:
            self.draw_steps(names, len(names) + 1)
            on_card = 'Done: it is on the card. Open it from the homebrew menu, started over a game.'
            done = {'make+install': on_card, 'make2+install': on_card,
                    'install': 'Done: it is on the card, and what arrived was checked against what was sent.',
                    'make': 'Done: the port is built. It can be installed from "My ports".', 'make2': 'Done: the port is built. It can be installed from "My ports".',
                    'check': 'Checked: the result is in the details.', 'setup': 'Checked: the result is in the details.',
                    'remake': 'Done: every port is built again, and its program is on the card.',
                    'mods': 'Done: the card is as asked. A port uses the mods that are on from its next start.'}
            self.headline.configure(text=done[self.kind], fg=GOOD)
            self.bar.configure(style='Good.Horizontal.TProgressbar', mode='determinate', value=1000)
            self.percent.configure(text='')
            self.detail.configure(text=f'It took {took}.')
            if self.kind in ('check', 'setup'):
                self.toggle_log(True)
        else:
            self.draw_steps(names, self.step, failed=True)
            stopped = getattr(self, 'stopped', False)
            self.stopped = False
            self.bar.configure(style='Bad.Horizontal.TProgressbar', mode='determinate')
            self.percent.configure(text='')
            if self.refused and not stopped:
                # (not a fault: a game this release can't open yet. Said as that, with the tool's own words for why)
                self.headline.configure(text='This game isn\'t supported yet.', fg=WARN)
                self.detail.configure(text=self.refused[:400])
            else:
                self.headline.configure(text='Stopped. It was left incomplete.' if stopped else f'It couldn\'t be finished: "{names[self.step - 1]}" failed.', fg=BAD)
                self.detail.configure(text='' if stopped else (self.last_line[:150] or 'See the details.'))
            if not stopped:
                self.toggle_log(True)
        self.refresh_ports()
        if self.kind == 'setup':
            self.check_deps()
        if self.kind == 'mods':
            self.mods_read()

    def pump(self):
        try:
            for _ in range(400):
                item = self.lines.get_nowait()
                if isinstance(item, tuple) and item[0] == 'asked':
                    if item[1] == self.mods_asked:  # (not an answer to a question asked before the last one)
                        item[2](item[3])
                elif isinstance(item, tuple):
                    self.finish(item[1])
                elif item.startswith('@@phase|'):
                    _, i, n, _ = item.rstrip('\n').split('|', 3)
                    self.on_phase(int(i), int(n))
                elif item.startswith('@@progress|'):
                    _, kind, detail, done, total = item.rstrip('\n').split('|')
                    self.on_progress(kind, detail, int(done), int(total))
                else:
                    if item.strip():
                        self.last_line = item.strip()
                    if NOT_YET in item:
                        self.refused = item[item.index(NOT_YET) + len(NOT_YET):].lstrip(': ').strip()
                    elif self.refused and item.startswith('  ') and len(self.refused) < 400:
                        self.refused += ' ' + item.strip()  # (the lines under it: which file, and why)
                    self.say(item)
        except queue.Empty:
            pass
        self.root.after(80, self.pump)


def main():
    root = tk.Tk()
    App(root)
    root.mainloop()


if __name__ == '__main__':
    main()
