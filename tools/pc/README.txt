POKEMON DS WIDESCREEN FOR PC
============================

The Pokemon DS games on one widescreen display, with the two DS screens merged into one view,
the world drawn out to the sides of a 16:9 screen, 60 fps and controller support.
It plays your own copies of:

  Pokemon Black, White, Black 2 and White 2 (English, US or Europe)
  Pokemon SoulSilver and HeartGold (English, US or Europe)
  Pokemon Platinum (English, US or Europe)

Nothing of the games comes with this download: you need your own ROMs (.nds files).


GETTING STARTED
---------------

1. Unpack the download anywhere you like (not inside Program Files).
2. Put your ROMs in the roms folder. Any file names are fine.
3. Start Pokemon Fulldex.exe (Windows) or pokemon-ds-wide (Linux).
4. Pick a game from the list: arrow keys or a controller's D-pad, then Enter or A.
   You can also double-click a game. Press O in the list to open the roms folder, and H (or
   a controller's Y) for the settings: the Esc menu's options, each explained.
   The list shows every game under its console's heading; the tabs at the top (Q and E, a
   controller's shoulder buttons, or a click) show one console's games.

Windows may warn that the program is from an unknown publisher the first time: choose
"More info", then "Run anyway".
On Linux there is nothing else to install: SDL2 comes with it (libSDL2-2.0.so.0).


STEAM DECK
----------

1. In Desktop Mode, unpack the Linux download (for example into your Games folder) and put
   your ROMs in its roms folder.
2. In Steam: Games > Add a Non-Steam Game to My Library > Browse, set the file type to
   All Files and choose pokemon-ds-wide.
3. Back in Gaming Mode, start it from your library with a gamepad controller layout.
4. The Deck's screen is 1280x800: set 3D RESOLUTION to 720P in the settings (Y in the game
   list), full sharpness there and easier on the battery.


PICTURE SETTINGS (Esc menu)
---------------------------

  WORLD ON GPU       ON: the widescreen world is drawn by your graphics card (OpenGL 4.3 or
                     newer). Turn it OFF if the world looks wrong or the game will not start.
  3D RESOLUTION      how sharp the widescreen 3D world is: 1080P to start, or 720P,
                     1440P (2K), 2160P (4K) and lower for slow PCs. The 2D sprites and
                     text stay the DS's own pixels. With WORLD ON GPU, 1080P or 1440P
                     suits most PCs; 2160P needs a good graphics card.
  LOWER RES WHEN BUSY      off to start. ON: lowers the 3D RESOLUTION for a moment when the PC
                     cannot keep up, instead of slowing down.
  FAST FORWARD LOW RES     off to start. ON: the 3D world drops to 360P while you fast forward,
                     for more FPS (a faster fast forward if your PC cannot keep up).
  BORDERLESS FULLSCREEN    on to start: fills your screen at its own resolution as a
                     borderless window, so switching to other windows is instant. F11
                     switches between it and a window.
  DESKTOP SHORTCUTS        (settings page only) CREATE puts a shortcut for each game in the list
                     on your desktop (Linux: also in your apps menu), with its picture. It
                     starts that game straight away; QUIT in the pause menu still comes back
                     to the list. The same from a command line:
                     "Pokemon Fulldex.exe" --game IREO (the game codes are under icons/).


WHERE THINGS ARE KEPT
---------------------

  roms/           your ROMs
  icons/          pictures for the game list, if you want them: a JPEG or PNG named by the
                  game's code (IREO.jpg Black 2, IRDO White 2, IRBO Black, IRAO White,
                  IPKE HeartGold, IPGE SoulSilver, CPUE Platinum), in place of the box art
                  the list shows. A picture named like the ROM (Black 2.jpg beside Black 2.nds)
                  works too.
  saves/          your saved games, one per game (black2.sav, soulsilver.sav, ...)
  settings.cfg    your settings
  game.log        what happened in the last session (send it with a bug report)

To carry on a game from another emulator, put its .sav file in the saves folder with the name
above (for example saves/platinum.sav). Back your saves up before you do.


CONTROLS
--------

Controller (Xbox, PlayStation, Switch Pro and others): plays as a DS, with the touch screen's
buttons shown in the view. Hold the right trigger to fast forward (up to 4x).
Its START (Menu on Xbox, Options on PlayStation) opens the menu; START BUTTON in the settings
makes it the DS's START instead.

Keyboard:
  Arrow keys       D-pad
  X / Z            A / B
  S / A            X / Y
  Q / W            L / R
  Enter            START
  Backspace        SELECT
  Mouse            touch screen (click to touch)
  Esc              menu (settings, save state, QUIT back to the game list)
  F11              fullscreen
  Tab              next screen layout
  F1               show the other DS screen
  Left Shift       fast forward while held

Changing them: CONTROLS > KEYS AND BUTTONS in the menu (Esc) lists the DS's buttons, the menu, fast forward and
swapping the screens, with the key or button each has. Choose one and press the key or button
to give it (Esc, or waiting 5 seconds, leaves it as it was); CHANGING at the top switches
between the keyboard and a controller. Giving one a key or button another one had swaps the
two. RESET ALL puts them back. The game list follows the ones you change (A plays, B goes back,
Y opens the settings).


SOURCE AND LICENCE
------------------

This program is free software under the GNU General Public License version 3 (LICENSE.txt),
built on melonDS. Its source code:
  https://github.com/mamesj/pokemon-ds-widescreen
SDL2 (SDL2.dll on Windows, libSDL2-2.0.so.0 on Linux) is under the zlib licence
(LICENSE-SDL2.txt).
Pokemon is a trademark of Nintendo, Creatures and GAME FREAK. This project is not affiliated
with or endorsed by them. The box art in the game list belongs to Nintendo, Creatures,
GAME FREAK and The Pokemon Company and is shown only to tell the games apart; if you hold the
rights and want it removed, open an issue on the GitHub page and it will be taken out.
