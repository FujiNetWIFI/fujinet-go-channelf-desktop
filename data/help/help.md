This is the text of FujiNet Go Channel F's built-in Help (Help ▸ FujiNet Go
Channel F Help, or Ctrl+Shift+H; ⌘? on a Mac). The app shows each part only
to the frontend it is about; here you see them all. Every topic below is one
page in the app.

# Getting started

FujiNet Go Channel F is a Fairchild Channel F with a FujiNet cartridge in its
slot. The console is MAME's: the F3850 (F8) processor, the 128 × 64 video
RAM, the beeper, and the two hand controllers. The FujiNet runs inside the
app, so CONFIG comes up at once and can mount games and programs from the
network.

1. **Start the app.** The console powers on and runs FujiNet's CONFIG. It
   needs no BIOS: the app has a small stand-in of its own that runs CONFIG
   and every FujiNet app. See [BIOS and ROMs](#bios-and-roms).
2. **Pick something in CONFIG.** Choose a host (the SD folder or a server on
   the network) and a program on it. FujiNet sends it to the cartridge and
   it starts. See [FujiNet](#fujinet).
3. **Or open a cartridge of your own.** **Open Cartridge…**
   (<kbd>Ctrl+O</kbd>) takes a Videocart image: a `.bin`, a `.chf`, or a
   zip of one. The Videocarts need the console's own BIOS, which you import
   (**Import BIOS…**). See [Cartridges](#cartridges).
4. **Play.** The keys, the gamepads and the Controllers window
   (<kbd>F9</kbd>) work the two hand controllers and the console's buttons.
   See [Controllers and keys](#controllers-and-keys).
5. **Change the console.** [Preferences](#preferences) picks the machine,
   the BIOS, the picture, the controls and FujiNet.

## This Help

The list on the left has a topic for each part of the app; the search box
above it finds every section that has all the words you type. A **Help**
button in Preferences, the Debugger, the Controllers window, the FujiNet
console log and the first page opens the topic for what it shows, and
<kbd>Ctrl+Shift+H</kbd> does the same for the window you are in.

<!-- only: gnome kde windows -->

In Preferences, the Debugger and the console log, <kbd>F1</kbd> does too.
(In the main window <kbd>F1</kbd> is the console's RESET.)

<!-- end -->

The same text is on GitHub, as
[data/help/help.md](https://github.com/FujiNetWIFI/fujinet-go-channelf-desktop/blob/main/data/help/help.md).

<!-- only: macos -->

On a Mac, the Help menu's search field also finds Help topics.

<!-- end -->

<!-- context: main -->
# The window

The console's picture fills the window. The menus have:

- **Open Cartridge…**, **Eject Cartridge**, **Import Cartridge to SD…** and
  **Open SD Folder**: see [Cartridges](#cartridges).
- **Import BIOS…** and **Import BIOS from a Folder…**: see
  [BIOS and ROMs](#bios-and-roms).
- **Pause**, **Reset** and **Reset to CONFIG**.
- **Controllers** (<kbd>F9</kbd>) and **Debugger** (<kbd>F12</kbd>).
- **TV Aspect**, **Smooth Scaling** and **Fullscreen** (<kbd>F11</kbd>).
- **FujiNet Web UI** and **Console Log**.
- **Preferences** (<kbd>Ctrl+,</kbd>), **Help**, **About** and **Quit**
  (<kbd>Ctrl+Q</kbd>).

<!-- only: gnome -->

On GNOME, everything is in the main menu (the button at the right of the
header bar).

<!-- end -->

<!-- only: kde windows -->

The menu bar has Machine, View, FujiNet and Help. The Machine menu has the
cartridge and BIOS items, Pause, Reset, Reset to CONFIG, Preferences and
Quit (Exit on Windows).

<!-- end -->

<!-- only: macos -->

On a Mac, Preferences and Quit are in the app menu, and the cartridge and
BIOS items are in the Machine menu. Controllers and the Debugger are also
⌘J and ⌘D.

<!-- end -->

The status shows the cartridge and FujiNet's state:

| Status | Means |
|---|---|
| connected | The cartridge is talking to FujiNet. |
| link down | FujiNet is off or not answering yet; CONFIG tries again. |
| loading 42% | FujiNet is sending a program to the cartridge. |
| game running; mailbox closed | A game that does not use FujiNet is running. |

A **Needs BIOS** message (with an **Import BIOS…** button) appears when
something needs the console's own BIOS and has none. See
[Needing the BIOS](#needing-the-bios).

Messages (a cartridge opened, a gamepad plugged in, FujiNet restarted) show
for a few seconds over the picture.

## Reset and power

- **Reset** presses the console's RESET button (<kbd>F1</kbd>): the
  processor starts again from the BIOS. The cartridge keeps what it has
  (a FujiNet app starts over with its memory intact), and so does the
  video RAM.
- **Reset to CONFIG** (<kbd>Ctrl+R</kbd>, or <kbd>Escape</kbd>) is a power cycle:
  it ejects an opened cartridge and boots CONFIG, exactly as at power-on.
  The machine and BIOS you chose in Preferences take effect here.
- **Eject Cartridge** does the same; a FujiNet cartridge is always in the
  slot, and CONFIG is its own program.
- **Pause** stops the console and its sound; the Debugger's Stop is
  separate.

## The picture

The console draws 102 × 58 cells, two pixels wide and four lines tall. A
television shows them 4:3; **TV Aspect** switches between that and square
pixels. The NTSC machines run at 60.5 frames a second and the PAL ones at
50.

**Smooth Scaling** blends the pixels when the picture is stretched, and
leaves them sharp when it is off.

# Controllers and keys

The Channel F has no keyboard. Its console has four numbered buttons and a
RESET, and each of its two hand controllers is one knob with four axes:

- **Stick**: tilt the knob any of eight ways (forward, back, left, right,
  and the diagonals).
- **Twist**: turn it counter-clockwise or clockwise.
- **Pull**: lift the knob up.
- **Push**: press the knob down, like a plunger.

All of them can be held at once. The **right controller is Player 1** and
the **left controller is Player 2**.

The console's buttons are **1 TIME**, **2 MODE**, **3 HOLD** and
**4 START**, and **RESET**.

## Default keys

Keys go by where they are on the keyboard, whatever your layout.

| Does | Player 1 | Player 2 |
|---|---|---|
| Stick forward | <kbd>Up</kbd> | <kbd>Keypad 8</kbd> |
| Stick back | <kbd>Down</kbd> | <kbd>Keypad 2</kbd> |
| Stick left | <kbd>Left</kbd> | <kbd>Keypad 4</kbd> |
| Stick right | <kbd>Right</kbd> | <kbd>Keypad 6</kbd> |
| Twist counter-clockwise | <kbd>Z</kbd> | <kbd>Keypad 7</kbd> |
| Twist clockwise | <kbd>X</kbd> | <kbd>Keypad 9</kbd> |
| Pull | <kbd>C</kbd> | <kbd>Keypad .</kbd> |
| Push | <kbd>Space</kbd> | <kbd>Keypad 0</kbd> |

| Console | Key |
|---|---|
| 1 TIME | <kbd>1</kbd> |
| 2 MODE | <kbd>2</kbd> |
| 3 HOLD | <kbd>3</kbd> |
| 4 START | <kbd>4</kbd> |
| RESET | <kbd>F1</kbd> |
| Reset to CONFIG | <kbd>Escape</kbd> |

A key does one thing: giving a key to a new job takes it from the old one.
Opposite contacts held together cancel, so the stick, the twist and the
pull and push never hold both ways.

The app keeps these keys for itself:

| Shortcut | Does |
|---|---|
| <kbd>Ctrl+O</kbd> | Open Cartridge |
| <kbd>Ctrl+R</kbd> | Reset to CONFIG |
| <kbd>Ctrl+,</kbd> | Preferences |
| <kbd>Ctrl+Q</kbd> | Quit |
| <kbd>Ctrl+Shift+H</kbd> | Help |
| <kbd>F9</kbd> | Controllers |
| <kbd>F11</kbd> | Fullscreen |
| <kbd>F12</kbd> | Debugger |

<!-- only: gnome kde windows -->

Keys with <kbd>Ctrl</kbd> never reach the console.

<!-- end -->

<!-- only: macos -->

Keys with ⌘ never reach the console.

<!-- end -->

<!-- only: linux -->

Key combinations with the Super (Windows) key are the desktop's.

<!-- end -->

## Gamepads

Gamepads are picked up as they are plugged in, and can come and go at any
time. The first is Player 1, the second Player 2; Preferences ▸ Input can
fix which is which, and a pad plugged back in gets its player back.

| Gamepad | Does |
|---|---|
| D-pad, or left stick | The stick |
| Left and right shoulder | Twist counter-clockwise, clockwise |
| East (B) | Pull |
| South (A) | Push |
| West (X) | 1 TIME |
| Back | 2 MODE |
| North (Y) | 3 HOLD |
| Start | 4 START |

The left stick is an eight-way switch: it engages at half deflection and
lets go below a third. With **Right stick works the knob** on (the
default), the right stick is the other two axes: left and right twist, up
pulls and down pushes. A gamepad with no standard layout (a plain USB
adapter) uses its first stick or hat and its buttons, which can be given
jobs in the same way.

## Changing the controls

<!-- only: gnome kde windows -->

Preferences ▸ Input has every control the console has (23 in all: both
controllers' eight contacts, the four buttons, RESET and the app's own
actions) with its key and its gamepad button. Choose one, then press the
key or the gamepad button to give it. **Clear** takes the job off a key or
button, and **Reset Controls** puts the defaults back. The map is kept in
the settings, and only what differs from the defaults is written.

<!-- end -->

<!-- only: macos -->

On a Mac the controls are changed in the Controllers window
(<kbd>F9</kbd>) rather than in Settings: click **Map**, then click a
control on the picture or choose one (all 23 are in the list: both
controllers' eight contacts, the four buttons, RESET and the app's own
actions), then press the key or the gamepad button to give it. **Show
bindings** writes each control's key on it. **Reset Controls** (there and in
Settings ▸ Input) puts the defaults back. The map is kept in the settings,
and only what differs from the defaults is written.

<!-- end -->

<!-- context: controllers -->
# Controllers window

The Controllers window (<kbd>F9</kbd>) draws the console as a player sees
it: the buttons across the top, the left controller (Player 2) on the left
and the right controller (Player 1) on the right. It floats on its own at
a fixed size. The controls that are held light up in the accent colour,
whoever holds them: the keyboard, a gamepad or the mouse.

Each controller is shown twice:

- **From above**: the knob in a ring of eight direction wedges, with the
  twist arrows either side. The knob shifts as it is tilted and turns a
  little as it is twisted.
- **From the side**: the handle, with PULL above and PUSH below. The knob
  rises and sinks with them.

| With the mouse | Does |
|---|---|
| Drag from the knob | Tilts the stick (eight ways, nothing near the middle) |
| Press on a wedge | Holds that direction |
| Press on the knob | Pushes it |
| Right-press on the knob | Pulls it |
| Wheel over a controller | A notch twists it, clockwise one way and counter-clockwise the other |
| Press a twist/PULL/PUSH arrow | Holds it |
| Press a console button | Holds it |

<!-- only: macos -->

On a Mac, Control-click is the right-press.

<!-- end -->

A wheel notch is held for six frames so that a program polling once a
frame sees it. Switching to another window lets go of everything the
mouse holds. Keys typed in the Controllers window go to the console.

# Machines

Preferences ▸ Machine picks the machine, MAME's four Channel F consoles:

- **Fairchild Channel F (NTSC)**: the 1976 console. BIOS: sl31253 and
  sl31254.
- **Fairchild Channel F System II (NTSC)**: the 1978 revision. BIOS: sl90025
  and sl31254.
- **Saba Videoplay (PAL)**: the German version, also sold as the Luxor
  Video Entertainment System. BIOS: sl31253 and sl31254.
- **Saba Videoplay 2 (PAL)**: the 1978 revision, also sold by Luxor, ITT
  and Ingelen. BIOS: sl90025 and sl31254. Its sound is half the level of the
  others, as in MAME.

The NTSC machines run the F3850 at 1.79 MHz and draw 264 lines at 60.5
frames a second; the PAL ones run it at 2 MHz and draw 312 lines at 50. A
new machine takes effect at the next power cycle (Reset to CONFIG).

# BIOS and ROMs

The console's BIOS is copyrighted, so none comes with the app. It is two 1
KB ROMs:

| ROM | Is |
|---|---|
| `sl31253.rom` | First half, in the Channel F and the Videoplay |
| `sl90025.rom` | First half, in the System II and the Videoplay 2 |
| `sl31254.rom` | Second half, in every console |

The first half is at $0000 and the second at $0400. Either first half runs
on any machine: with the Automatic choice the machine uses its own and falls
back to the other, if only that one is imported.

## The stand-in

Without the BIOS the app runs a small program of its own in its place. It
has the four BIOS routines that FujiNet's CONFIG and the FujiNet apps use
(RESET at $0000, CLRSCRN at $00D0, PUSHK at $0107 and POPK at $011E) and
boots a cartridge that starts with the $55 signature. That is enough for
CONFIG and every FujiNet app. A Videocart calls the real BIOS's other
routines, so it needs the real BIOS (see [Needing the BIOS](#needing-the-bios)).

## Importing it

- **Import BIOS…** (in the Machine menu, on the ROMs page of Preferences, and
  in the Needs BIOS message) takes a file or a zip. MAME's `channelf.zip`
  has all three ROMs.
- **Import BIOS from a Folder…** takes a whole folder, searched eight
  levels down, and the zips in it.
- Dropping a BIOS file or zip on the window imports it too.

Files are recognised by their size and checksum (CRC-32), whatever they are
called; anything else in a folder or zip is left alone. A 2 KB file that is
a known first half followed by `sl31254` is split in two. A 1 KB file whose
checksum is not one of the three is **not** taken, because it could be
either first half. The message says what was imported and what is still
missing. Imported files are copied into the app's `roms` folder (see
[Files and folders](#files-and-folders)), under MAME's names, so the
originals can go. A new BIOS takes effect at the next power cycle (Reset to
CONFIG).

<!-- context: nobios -->
## Needing the BIOS

A cartridge that is not a FujiNet app is a game, and a game calls the real
BIOS. If the BIOS is not imported, or Preferences ▸ Machine ▸ **BIOS** is
set to the stand-in, **Open Cartridge** refuses it and says why, and the
window shows a **Needs BIOS** message. Import the two ROMs, then open the
cartridge again. If a FujiNet app calls a routine the stand-in does not
have, the console stops at that call with the same message.

<!-- context: prefs/ROMs -->
## The ROMs page

Preferences ▸ ROMs lists the three ROMs, *Imported* or *Not imported*, with
**Import…** and **Import a Folder…** buttons, and says where they are
kept.

- **Console:**
  - Channel F BIOS, first half (sl31253.rom): the Channel F and the
    Videoplay.
  - Channel F System II BIOS, first half (sl90025.rom): the System II and
    the Videoplay 2.
  - Channel F BIOS, second half (sl31254.rom): every console.

# Cartridges

A cartridge is a file: a `.bin` or a `.chf`, or a `.zip` that holds exactly
one. The app works out what it is from its contents.

- **A FujiNet app** is a 16 KB image (or 16 KB plus whole 8 KB pages) with
  `FUJI` at $47FC. It runs in the FujiNet cartridge with the mailbox live:
  it can talk to the network, and it needs no BIOS beyond the stand-in.
- **A Videocart** is a game. It runs in the slot as its own cartridge, with
  no FujiNet behind it, and needs the console's BIOS.

The Videocarts are the standard ones (ROM from $0800, 2 KB to 6 KB) and
four with extra hardware. The app knows them by the checksum of the image
against MAME's software list:

- **Maze** and **Hangman** carry a 2102 RAM, read and written through I/O
  ports.
- **Chess** (Schach) has 2 KB of RAM at $2800.
- **The multi-game cartridges** (two versions) are 256 KB, in banks.

An image the list does not name runs as a plain Videocart with 2 KB of RAM
at $2800 (a 256 KB one as the multi-game cartridge), which is how MAME
runs an image opened by its path.

## Open Cartridge

**Open Cartridge…** (<kbd>Ctrl+O</kbd>) powers the console off and on with
the cartridge in the slot. The cartridge is remembered and starts again
next time; **Eject Cartridge** or **Reset to CONFIG** forgets it. Dropping a
cartridge file on the window opens it. A file the console cannot take is
refused with the reason, and what was running goes on.

<!-- context: import-sd -->
## Import Cartridge to SD

**Import Cartridge to SD…** copies a cartridge into FujiNet's SD folder, so
CONFIG can list it and boot it through the cartridge (a zip of one is
unpacked). The FujiNet cartridge serves a flat ROM of up to 16 KB that
starts with $55, or a FujiNet app, and it refuses the rest, with the reason:

- Maze and Hangman need a 2102 RAM on the cartridge.
- Chess needs 2 KB of RAM at $2800.
- The multi-game cartridges are banked.
- An image over 16 KB that is not an app, an empty one, one without the $55
  signature, and an app with pages but no `FUJI` claim.

A game that boots from the SD folder still needs the console's BIOS.
**Open SD Folder** shows the folder in the file manager.

# FujiNet

The FujiNet is FujiNet's own firmware, running inside the app, and the
cartridge in the slot is its connection to the console. On the real console
the FujiNet cartridge is a board on the cartridge connector; here it is
emulated, and talks to FujiNet over a local network socket. FujiNet
listens; the cartridge dials in, and keeps trying again if the link drops.

## CONFIG

CONFIG is FujiNet's program for choosing what to run:

- **Hosts** are where programs come from: the SD folder, and servers on the
  network.
- Pick a host, a program on it, and start it. FujiNet sends the image to the
  cartridge, which swaps it in and restarts the console on it. A FujiNet
  app can then ask FujiNet for more (pages, files, the network).
- With the BIOS imported, CONFIG runs on the real BIOS; without it, on the
  stand-in.

## The FujiNet menu

- **FujiNet Web UI** opens FujiNet's settings page in your browser, at
  `http://127.0.0.1:11527/`. Only this computer can reach it.
- **Console Log** shows what FujiNet prints. See
  [The console log](#the-console-log).

<!-- context: fujilog -->
## The console log

The FujiNet Console Log window shows FujiNet's own messages as they come,
following the end while you are at the bottom. It is also kept in
`fujinet-console.log` in the app's `fujinet` folder.

## Turning it off

Preferences ▸ FujiNet ▸ **Enable FujiNet** turns the FujiNet off. The
cartridge then reports *link down*, CONFIG runs without hosts, and nothing
is dialled. The Videocarts are not affected.

## An external FujiNet

Preferences ▸ FujiNet ▸ **Use external FujiNet** skips the built-in FujiNet
and dials another FujiNet instance at the address you give, for example
`192.168.1.20:11526`. Leave the field empty for `127.0.0.1:11526`. Set
that instance's `[BOIP]` section to listen there (`enabled=1`, `host`,
`port`). The cartridge dials out, so the other FujiNet can start before or
after it. FujiNet Web UI and Console Log belong to the built-in FujiNet, so
they say the FujiNet is external; use the other instance's own. The
Debugger's FujiNet tab still shows the traffic.

## Behind the scenes

- The cartridge and FujiNet talk over TCP port 11526 on 127.0.0.1 (the
  BoIP port), and the web UI is on port 11527. Nothing from outside can
  reach either.
- FujiNet's settings are `fnconfig.ini` in the app's `fujinet` folder. It is
  written once and then left for FujiNet and you.
- A game that does not claim the mailbox runs with FujiNet unreachable until
  the next power cycle.

# Preferences

Preferences (<kbd>Ctrl+,</kbd>) set up the console. The machine, BIOS and
FujiNet settings apply with a power cycle: the app does it when
Preferences closes. The picture, the volume and the controls apply at once.
The settings are one file, shared by every Channel F app on the computer.

<!-- only: macos -->

On a Mac this window is called **Settings** (in the app menu), and its
pages are tabs.

<!-- end -->

<!-- context: prefs/Machine -->
## Machine settings

- **Machine**: which console (see [Machines](#machines)).
- **BIOS**: which BIOS runs.
  - *Automatic* takes the machine's own first half, else the other one.
  - *Channel F (sl31253)* and *Channel F System II (sl90025)* take that
    first half.
  - *The app's stand-in* always uses the app's own (CONFIG and FujiNet
    apps only).

  When the chosen half, or sl31254, is not imported, the stand-in runs.

<!-- context: prefs/Display -->
## Display and audio settings

- **Aspect**: *Television (4:3)* or *Square pixels* (setting `aspect`).
- **Smooth scaling**: blend the pixels when the picture is stretched
  (setting `smooth`).
- **Enable audio**: open the system audio device.
- **Volume**: the output level, 0 to 100.

<!-- context: prefs/Input -->
## Input settings

- **Enable gamepads**: poll USB and Bluetooth gamepads.
- **Right stick works the knob**: the right analog stick twists, pulls and
  pushes (setting `right_stick`).
- **Gamepads**: each one plugged in, and which player it drives. A new
  gamepad drives Player 1, the next Player 2; choose *Player 1* or
  *Player 2* to fix it, or *Automatic*.
- **Controls**: every control with its key and gamepad button (see
  [Changing the controls](#changing-the-controls)).

<!-- context: prefs/FujiNet -->
## FujiNet settings

- **Enable FujiNet**: run the built-in FujiNet.
- **Use external FujiNet**: dial another instance instead.
- **Address**: its `host:port`, or just a port (see
  [An external FujiNet](#an-external-fujinet)).
- **Web UI**: the address of the built-in FujiNet's settings page.

## The settings file

The settings file is `settings.ini` (see [Files and folders](#files-and-folders)).
The keys the Preferences pages set:

| Key | Is |
|---|---|
| `machine` | 0 to 3: Channel F, System II, Videoplay, Videoplay 2 |
| `bios` | 0 Automatic, 1 sl31253, 2 sl90025, 3 the stand-in |
| `right_stick` | 1 (the default) or 0 |
| `enable_fujinet` | 1 or 0 |
| `fujinet_external` | 1 or 0 |
| `fujinet_external_addr` | `host:port` |
| `enable_audio` | 1 or 0 |
| `enable_gamepad` | 1 or 0 |
| `volume` | 0 to 100 |
| `aspect` | 0 the television's 4:3, 1 square pixels |
| `smooth` | 1 blends the pixels when stretched, 0 keeps them sharp |
| `cart` | the opened cartridge, empty for CONFIG |

# Debugger

The Debugger (<kbd>F12</kbd>) stops the console and shows what it is doing.
It has eight tabs; closing it lets the console go on as it was.

| Button | Does |
|---|---|
| Stop or Run (<kbd>F5</kbd>) | Stops before the next instruction, or runs on |
| Step (<kbd>F7</kbd>) | One instruction |
| Step Over (<kbd>F8</kbd>) | Over a PI or PK, to the instruction after it |
| Step Out (<kbd>Shift+F8</kbd>) | On until the routine returns (POP) |
| Frame | On to the next frame |
| Scanline | On to the next line of the picture |
| Run to Cursor | On to the line selected in the Disassembly |

Beside them, the Debugger says why the console stopped: a step, a
breakpoint and its address, a VRAM write, a port write.

The F8 has no address registers of its own. Each chip on the bus keeps its
own program counter PC0, one return address PC1, and data counters DC0 and
DC1, and updates them from the ROMC code of each cycle. The CPU cannot read
the video RAM: it draws a cell by putting the colour on port 1, the column on
port 4 and the row on port 5, then strobing port 0.

<!-- context: debugger/prompt -->
## Prompt

The Prompt takes commands, with completion on <kbd>Tab</kbd> for commands,
registers and labels. Numbers are `$0800`, `0x800`, `800h`, `2048`,
`%1010` or `o17` (octal). `help` lists the commands.

| Command | Does |
|---|---|
| `step` | Runs `step n` instructions |
| `over`, `out` | Step over, step out |
| `frame`, `scanline` | Run `n` frames or lines |
| `run`, `stop` | Run on, or stop |
| `runto` | Runs to an address |
| `break` | An execute breakpoint, `break addr[-end] [cond]` |
| `bpr`, `bpw` | Data reads or writes of an address range |
| `bpi`, `bpo` | Port reads or writes, `bpi port[-end] [cond]` |
| `bpv` | VRAM writes to a cell range, `bpv row,col[-row,col]` |
| `bps` | Scratchpad writes, `bps reg[-end]` |
| `delete`, `enable`, `disable` | A breakpoint by id |
| `clear`, `breaks` | Clear all, or list them |
| `print` | Evaluates an expression |
| `regs`, `set` | The registers; `set reg expr` |
| `mem` | Memory in a space: `mem space addr [len]` |
| `poke` | Writes bytes to a space |
| `scratch`, `isar` | The 64-byte scratchpad, and ISAR |
| `kstack` | The BIOS's K stack |
| `vram`, `pal`, `video` | The video RAM, palettes and ports |
| `ports`, `sound`, `input` | The ports, the beeper and the controllers |
| `cart`, `mailbox` | The cartridge, and FujiNet's mailbox |
| `bios` | Which BIOS is running |
| `romc` | The ROMC trace: `romc [on\|off\|n]` |
| `label`, `labels` | Name an address; load a symbol file |
| `disasm` | Disassembles `disasm [addr] [n]` |
| `trace` | The instruction trace: `trace [on\|off\|clear\|n]` |
| `save` | Saves `vram`, `scratch`, `screen` and more to a file |

The spaces are `prog` (the CPU's view of $0000 to $FFFF), `scratch`, `vram`,
`cart` and `io`. An expression has the registers (`a w is isar pc pc0 pc1
dc0 dc1 j h k q`, the flags `icb o z c s`, and `r0` to `r63`), the byte at
`[addr]`, `p[addr]`, `s[reg]`, `v[cell]`, `c[off]` and `io[port]`, labels,
and `vpos`, `hpos`, `frame` and `cycles`. In a breakpoint condition `value`
and `addr` are the access (and `row` and `col` for `bpv`, `port` for `bpi`
and `bpo`). The operators are C's.

<!-- context: debugger/cpu -->
## CPU and RAM

The registers: A, W (the flags I C O Z S, with S set meaning the result was
positive), ISAR in octal, PC0, PC1, DC0, DC1, and J, H, K and Q. Below
them the 64-byte scratchpad as an 8 × 8 grid with the register names, the
cell ISAR points at outlined and the cells that just changed in the accent
colour. The K stack panel shows the BIOS's call stack. Click a value to
edit it while stopped. The hex view reads any of the five spaces (PROG,
scratchpad, VRAM, the cartridge and the I/O ports) and can edit them.

<!-- context: debugger/disassembly -->
## Disassembly

The F8 disassembled at the PC, with the opcode bytes, labels and the
breakpoints in the margin. Double-click a line for an execute breakpoint.
Follow PC keeps the current line in view. **Symbols** loads the labels of a
`.map` or listing from the AS assembler, or a plain list of addresses and
names. The BIOS, the cartridge header and FujiNet's mailbox registers have
labels of their own. The APPBANK window ($4800 to $67FF) says which page
it shows.

<!-- context: debugger/video -->
## Video

The picture is a 128 × 64 video RAM of two-bit values. Each row picks one of
four palettes from the values in its columns 125 and 126. The TV shows
columns 4 to 105 and rows 4 to 61; the rest is hidden, and programs keep data
there. The frame is drawn from the whole RAM at the vertical blank.

The views:

- **Frame**: the last frame, as the window shows it.
- **Live**: the video RAM as it is now.
- **Beam**: what a TV would show, row by row as the beam passed it.
- **VRAM**: all 128 × 64 cells, the hidden ones hatched.
- **Values**: the raw values, in false colour.
- **Palettes**: the four palettes.

The overlays mark the visible area, the cell grid, the palette columns, the
beam line, the cell the next write will hit, write heat, cells that changed,
and rows whose palette changed. Hover a cell for its row, column, value,
palette, and the last write (PC, frame and line); click to pin it;
double-click to break on writes to it. The row strip lists all 64 rows'
palettes, and a table has the palette colours and how many rows use each.
The write log lists every write with its frame, line, cycle and PC.

<!-- context: debugger/io -->
## Sound and I/O

The ports 0, 1, 4 and 5 as the CPU last wrote them (raw, complemented and
decoded: the write strobe, the controllers' enable, the panel latch, the
colour, column and row), and the value the CPU would read now. The
controllers and the console buttons, decoded and active-low as the CPU sees
them. The beeper's mode (off, 1 kHz, 500 Hz or 120 Hz), its level and a
timeline of mode changes. The I/O log lists every port access. For a Maze or
Hangman cartridge the 2102's address and bit.

<!-- context: debugger/breakpoints -->
## Breakpoints

The list of breakpoints with their ids, kinds, ranges, conditions and hit
counts. A breakpoint can be an execute, a data read or write, a port read
or write, a VRAM cell range, or a scratchpad range, each with an optional
condition. **Add**, **Remove**, and the check box to turn one off.

<!-- context: debugger/cart -->
## Cart

The cartridge in the slot. For a Videocart: its type, its ROM size and
checksum, its RAM, and the name MAME's software list gives it. For the
FujiNet cartridge: the live image and the one staged for the next swap
(size and checksum), the page at $4800, the status page at $FC00 to $FC0F
decoded (the tick, the boot state and the last error), the mailbox, and the
events (register writes, bytes sent, page selects and swaps).

<!-- context: debugger/fujinet -->
## FujiNet packets

Everything between the cartridge and FujiNet as FujiBus packets, from the
start of the session: the direction, the device, the command and the
length. A repeat of the same exchange shows once with a count. **Clear**
empties the list.

# Files and folders

The app keeps its settings in one file, and everything else in its data
folder:

<!-- only: linux -->

On Linux, the settings are `~/.config/fujinet-go-channelf/settings.ini` and
the data is in `~/.local/share/fujinet-go-channelf` (or under
`$XDG_CONFIG_HOME` and `$XDG_DATA_HOME`). The GNOME and KDE apps share them.
As a Flatpak, they are under `~/.var/app/`, in the app's own folder.

<!-- end -->

<!-- only: windows -->

On Windows, the settings are `%APPDATA%\fujinet-go-channelf\settings.ini` and
the data is in `%LOCALAPPDATA%\fujinet-go-channelf`.

<!-- end -->

<!-- only: macos -->

On a Mac, the settings are `~/.config/fujinet-go-channelf/settings.ini` and
the data is in `~/.local/share/fujinet-go-channelf`.

<!-- end -->

In the data folder:

| Folder | Has |
|---|---|
| `roms` | the imported BIOS ROMs |
| `carts` | cartridges dropped on the window |
| `fujinet` | `fnconfig.ini`, the SD folder (`SD`), and the console log |

# Troubleshooting

- **This cartridge needs the console's BIOS**: it is a game, and no BIOS is
  imported (or the stand-in is chosen). See
  [Needing the BIOS](#needing-the-bios).
- **The console stops with Needs BIOS**: a FujiNet app called a BIOS
  routine the stand-in does not have. Import the BIOS.
- **An imported file was not taken**: a 1 KB file whose checksum is not one
  of MAME's is refused on purpose; get the ROMs from MAME's `channelf.zip`.
- **link down**: FujiNet is off, not started yet, or the external address
  is wrong. CONFIG tries again; check Preferences ▸ FujiNet.
- **The cartridge was refused**: the message says why: too big, empty, or
  not a cartridge image. A zip must hold exactly one image.
- **Import Cartridge to SD refused a game**: Maze, Hangman, Chess and the
  multi-game cartridges need hardware the FujiNet cartridge does not have;
  open them with Open Cartridge instead.
- **Keys don't reach the console**: a dialog or another window is in front,
  or the key is one the app keeps (see
  [Controllers and keys](#controllers-and-keys)).
- **A gamepad does nothing**: it may not be on the player you expect (see
  [Gamepads](#gamepads)), or its buttons may be given other jobs.
- **No sound**: the audio device could not be opened, or Pause is on.
- **A port is taken**: another program is using 11526 or 11527. Close the
  other FujiNet, or point the app at it with **Use external FujiNet**.

# About and licences

FujiNet Go Channel F is free software under the GNU General Public License,
version 3 or later. Copyright © 2026 Thomas Cherryhomes.

- **MAME** (the Fairchild Channel F driver, GPL-2.0-or-later, and the F8
  processor and cartridge slot, BSD-3-Clause) is what the console is
  modelled on.
- **The FujiNet firmware** (GPL-3.0-or-later) runs inside the app, and the
  cartridge's own sources are the firmware's.
- No BIOS or other copyrighted ROM is part of the app; you import your own.

[fujinet.online](https://fujinet.online/) ·
[Report an issue](https://github.com/FujiNetWIFI/fujinet-go-channelf-desktop/issues)
