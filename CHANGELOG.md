# Changelog

## Unreleased

- **iPhone: the whole screen of a foldable iPhone, folded out too.** The app used to be shown
  in a box the size of the outer screen; it now fills whichever screen it is on.
- **iPhone: controllers work in the full-screen app.** A Bluetooth controller (a Backbone Pro,
  for one) is read in the game list and in the game again.
- **SWAP A/B AND X/Y** is the new name of BUTTONS (in CONTROLS): ON puts the DS's A on the right
  button and B on the bottom one, for Xbox-style controllers.
- **iPhone: QUIT no longer leaves a black screen.** It saves and closes the app (iOS lets no
  app start itself again); open it again for the game list.
- **New: iPhone and iPad.** A sideloaded app (`pokemon-ds-wide-ios.ipa`, iOS 14 or newer): the
  same game list and games as on Android, with the ROMs, saves and settings in the Files app
  (On My iPhone > Pokemon DS Widescreen).
- **KEYS AND BUTTONS in the game list too:** its settings page (CONTROLS) changes the keys and
  buttons before you play, as the pause menu does; on the Switch as well (any controller it
  takes, sys-con and MissionControl ones included), and its list follows the buttons changed.
- **Cheats: CHEATS in the pause menu.** Action Replay codes, run by melonDS's own engine, from a
  `cheats` folder: the R4 / DeSmuME database (`usrcheat.dat`), a text file of your own codes named
  after the game's code (`IPKE.txt` for HeartGold, ...) or a melonDS `.mch` file. Each turns on and
  off as you play, and stays as you left it. Long lists scroll, and the chosen cheat's note from
  the database (and its whole name, when the menu has to shorten it) shows under the menu.
- **SoulSilver and HeartGold: the Running Shoes button beside the X menu.** The game greys it out
  while its menu is open, and the single-screen view only shows the touch screen then; now the
  button stands next to the menu, ON or OFF, and Y (or a touch on it) turns running on or off.
- **SoulSilver and HeartGold: the title screen on both screens**, the logo beside the legendary
  bird, as the other games show theirs; the opening movie shows its two screens one above the
  other (the moon and the sea were on the hidden one).
- **README: a trailer at the top**, every game on every platform.
- **The pause menu and the settings page in groups:** the pause menu is RESUME, SAVE STATE, LOAD
  STATE, FAST FORWARD, then DISPLAY, GRAPHICS and CONTROLS (each a page of its own), RESET GAME and
  QUIT; the game list's settings page opens on DISPLAY, GRAPHICS, CONTROLS, SHORTCUTS and HELP.
- **Game list: SETTINGS can be chosen with the D-pad** (up from the top row, then A), as well as
  with its button; on the settings page, up from the top reaches BACK.
- **Button prompts name your controller's buttons**: PlayStation's symbols (cross, circle, square,
  triangle), Nintendo's and Xbox's, and the buttons you gave in KEYS AND BUTTONS.
- **Remappable controls: CONTROLS > KEYS AND BUTTONS in the pause menu.** Every DS button, the pause menu, fast
  forward and swapping the screens can be given another key on the keyboard and another button
  on a controller (triggers too). Choose one and press the key or button for it; giving it one
  that something else had swaps the two, and RESET ALL puts everything back. On the PC, Mac and
  Android game list, A, B, Y and the directions follow the ones you change.
- **Mac: Pokemon DS Widescreen.app** for Apple silicon and Intel Macs (macOS 11 or newer), built on
  GitHub's Macs (`tools/mac/build_all.sh`, `.github/workflows/mac.yml`). Its ROMs, saves and
  settings go in `~/Library/Application Support/Pokemon DS Widescreen`; the world is drawn by the
  processor (a Mac's OpenGL stops at 4.1), at 720P to start; a MacBook Air M1 holds 60 FPS at
  1080P. Not notarised: the first time, Open Anyway in System Settings > Privacy & Security.
- **Linux: an AppImage** (`pokemon-ds-wide-pc-linux-x86_64.AppImage`), the program as one file, for
  Gear Lever and other AppImage managers, which also update it from the release. Its ROMs, saves
  and settings go in `~/.local/share/pokemon-ds-wide` (O in the game list opens its roms folder);
  its desktop shortcuts start the AppImage.
- **Switch: HOME MENU ICONS could fail with THIS BUILD HAS NO FORWARDER** when the game list had no
  game that needed the program's built-in tables (only SoulSilver and HeartGold, or games with their
  own funcs.txt). Fixed.
- **LOWER DETAIL WHEN BUSY (LOWER RES WHEN BUSY on a PC or phone) is off to start**, on every
  platform, also for players who had it on from before; turn it on in the settings if you want it.
- **Android: the first public release** (`pokemon-ds-wide-android.apk`), all seven games in one
  app, with on-screen controls and controller support. README: an Android section.
- **START opens the pause menu** (+ on the Switch, a controller's START on a PC or phone). The
  games hardly use the DS's START; START BUTTON in the settings puts it back (then + and − together,
  or START and SELECT, open the menu as before).
- **Android: a proper app icon** that fills the launcher's circle, instead of a square picture
  inside a white circle.
- **README: Use your own save**, near the top: where each platform keeps the saves, each game's
  save name, and which save files work.
- **One download per platform:** the all-in-one for the Switch and the PC download, each with all
  seven games. The one-game Switch downloads are no longer published; their folders keep working
  from the all-in-one's list.
- **Switch: the touch screen works** in handheld mode. Taps did not reach the game, so
  questions answered only by touch (Platinum's first YES / NO with Professor Rowan) could not be
  answered. Docked (or handheld), the right stick moves a cursor and ZL touches; pressing the
  right stick in shows the other screen.
- **Platinum: the introduction on one screen.** Professor Rowan's screen fills the view; for his
  two questions that take a touch, the YES / NO buttons or the Poke Ball stand at the right edge
  (touchable). A and B answer them too (A for YES, B for NO, A presses the Poke Ball's button),
  which the game itself only takes as a touch.
- **Switch: HOME MENU ICONS** in the all-in-one's settings (Y in the list): an icon on the HOME
  Menu for each game, with its box art, that starts the game straight away. Made on the console
  (no PC or keys file); like any forwarder they need sigpatches. The list shows box art for every
  game too.
- **PC: desktop shortcuts for each game**: DESKTOP SHORTCUTS on the game list's settings page
  makes them, with the box art as their icons; each starts its game without the list
  (`--game IREO` on a command line does the same).
- **PC: FAST FORWARD LOW RES**, a new setting, off to start: your 3D resolution now stays while
  you fast forward; turn it on for more FPS in fast forward (the world at 360P while it is held).
- **PC: box art in the game list**, built into the program, as on the Switch (your own pictures in
  `icons/` still take its place).
- **Android: one app for all seven games** with the game list,
  built by `tools/android/build_all.sh`. The first time, tap the screen, allow access to files
  and pick the folder with your ROMs (saves go in a `saves` folder inside it). On touch screens
  the game list, its settings page and the pause menu take taps (drag to scroll the settings), and
  the on-screen D-pad has a dark edge so that it shows on light scenery.
- **Settings in the game list**: press Y (on a PC also H or F1) to set the pause menu's options
  before you play, each with what it does written under it.
- **Switch: a shorter pause menu.** WORLD ON GPU and STICK TOUCH CURSOR are gone from it: the
  world is always drawn by the graphics chip (if it ever fails to keep going, the processor
  draws it for that start only), and the Switch has its touch screen.
- **PC: the Linux download brings its own SDL2**, so nothing needs installing first, and it
  runs on the Steam Deck (SteamOS): see the Steam Deck steps in the README.
- **PC: a first start in borderless fullscreen at 1080P with 60 FPS on** (F11 or the settings
  for a window; LOWER RES WHEN BUSY brings the 3D resolution down on a PC that cannot keep up).
- **PC: the Windows program is now `Pokemon DS Launcher.exe`**, with its own icon, and the
  menu's FULLSCREEN is called BORDERLESS FULLSCREEN (which is what it always was) and WORLD DETAIL
  is 3D RESOLUTION, shown as 720P, 1080P, 1440P (2K) or 2160P (4K). The game list
  can show your own pictures of the games: see `icons/` in the PC README.
- **Fast forward is on the right trigger in every game** (ZR on the Switch, RT on a PC
  controller): hold it for up to 4x speed. It was the left trigger. With STICK TOUCH CURSOR on (PC),
  the left trigger now presses the touch screen instead of the right.
- **Fast forward goes faster and looks smoother.** Of the frames it runs at once only the last
  is shown, so the others are no longer drawn (their 2D and widescreen world); the 60 FPS
  in-between frames pause and the widescreen world is drawn at 360 lines while it is held (back
  to your WORLD DETAIL as soon as you let go); and where the console cannot run 4 frames at once
  any faster than 3 (the Switch), it runs 3 and shows more of them.
- **Switch: fixed a crash when LOWER DETAIL WHEN BUSY brought the world down to 423 lines**
  (in heavy scenes, and nearly always while fast forwarding): at that size the world is now
  drawn the way it is at the other sizes the detail goes through.
- **Switch: smoother in heavy scenes.** The game now runs one frame ahead of what is on screen
  and shows each frame at an even time, so a frame that takes a little too long no longer
  makes its picture late when the frames around it are quick. At a 1581 MHz CPU clock, frames
  shown unevenly (over 17.5 ms after the last): White 2's Aspertia City from 39% to 17%, a
  Black 2 battle from 29% to 19%, Platinum's Snowpoint City from 30% to 10%, a HeartGold route from 19% to 2%. The cost is one
  more frame (17 ms) between pressing a button and seeing it, which in these turn-based games
  is hard to notice.
- **Switch: fewer late frames in towns and battles.** Preparing the 3D pictures for the
  graphics chip, sending them and collecting them now happens on the Switch's spare cores
  instead of between the game's own work, about 1 ms less a frame. Measured from save states,
  frames running late: White 2's Aspertia City from 4.5% to 0.9%, Black 2's Castelia City from
  1.6% to 0.1%, a Black 2 battle from 6% to 2.5%, Platinum's Snowpoint City from 4.8% to 1.7%.
  The picture is the same as before.
- **Switch: a game picked from the game list runs as smoothly as one started on its own.** The
  list left the Switch's graphics slower for the game played after it (HeartGold took 12 ms a
  frame to put its picture together instead of 4, and one frame in ten ran late). The program
  now starts itself again for the game you pick (a second of black screen, as QUIT already
  has), and the game runs at full speed.
- **Switch: smoother in towns, 60 FPS where it dropped to the mid-50s** (White 2's Aspertia City,
  Black 2's Castelia City). Where a game draws its 3D every other frame, each new 3D frame
  meant building the wide view's polygon lists and the 60 FPS in-between frames, about 8 ms of
  emulation every second frame. That work now runs on a spare core of the Switch, and the
  in-between frame's matching of polygons is worked out once instead of twice. Measured from
  save states: Aspertia from 56 to 60 FPS (frames running late from half to 4%), Castelia
  walking from 35% to under 2% late. The picture is the same as before.
- **Switch: fixed a rare freeze a few seconds into a game** (black screen, only holding POWER
  got out of it). The game asked the console for its clock speeds and temperatures for its log,
  and with sys-clk installed that could crash the console's power service. A player's log now
  goes without them.
- **Switch: smoother battles while you choose a command** (SoulSilver, HeartGold, Platinum):
  the buttons beside the battle are only sent to the graphics chip when they change, instead of
  every frame; and the chosen button has its white outline on the Switch too.
- **SoulSilver, HeartGold and Platinum battles:** the message box beside the buttons ends
  cleanly again on the Switch (its closing edge was drawn the wrong way round, a dark line
  beside the text); the party's Poke Balls sit in two straight rows with room between them,
  without the grey ring around each.
- **A new look for the game list** (Switch and PC): black with white text and red details, and
  four games across when there are more than six, so all seven fit on one screen.
- **PC all-games download (new, Windows and Linux):** unpack it, put your ROMs in its roms
  folder and pick a game from the list (keyboard, controller or mouse); QUIT in the menu goes
  back to the list. Saves go in its saves folder, one per game.
- **PC: the world on the graphics card, up to 4K.** WORLD ON GPU (on by default in the PC
  download) draws the widescreen world with the graphics card instead of the processor, and
  WORLD DETAIL now goes up to 2160 lines (4K).
- **Platinum battles:** "Use next Pokemon?" after a faint shows its two buttons (Use next
  Pokemon and Flee) instead of a cut-off red bar.
- **Black, White, Black 2 and White 2 (playtest notes):** no more trees drawn as thin slivers at
  the edge of the wide view (Castelia City's plaza); calm side bars instead of streaks while
  a door's zoom, a key item's cut-in or Fly covers the world; Black and White's start menu
  with no saved game shows its list properly; the X menu no longer opens as a grid of black
  boxes; going back from the Bag to the X menu goes straight to the wide view.
- **SoulSilver and HeartGold menus:** the Bag shows the chosen item's description above the
  items; no black box where the X menu was while it closes into the save or a menu; the
  Pokemon summary no longer flashes empty frames while it fades; a fade to white (choosing
  your starter, whiting out) fills the whole view instead of a white square with black bars.
- **SoulSilver and HeartGold: smoother while a battle waits for your command.** The battle
  buttons were drawn from hundreds of small pieces, each sent to the Switch's graphics chip
  every frame; each button is now a few pieces.
- **No more stretched trees at the edges of the wide view:** a tree, house or flower bed that
  ended just short of the screen's edge was sometimes stretched out to it (SoulSilver and
  HeartGold by New Bark Town, Black 2). It now stays its own size, and SoulSilver, HeartGold and
  Platinum no longer have anything continued to the edges at all (only the Black games' skies
  and lookouts need it).
- **No more dotted black lines in the wide world (WORLD ON GPU):** pixels the graphics chip left
  out where two polygons meet (along the ground's seams in SoulSilver and HeartGold) are filled
  from beside them.
- **Switch: the log of the game you just played is kept.** QUIT back to the game list used to
  replace `game.log` with the list's few lines; the previous one is now kept as
  `game-previous.log`.
- **Battle buttons are easier to read:** the chosen command or move has a thick white
  outline and the others are shaded, so you can always see what A will press (the red outline
  was lost around the red FIGHT button).
- **One program for all six games (Switch):** the all-games download
  (`pokemon-ds-wide-all-switch.zip`) is now a single program that shows a grid of your games when it starts, each with the icon from its ROM (or
  your own picture in `icons/`, or the HOME Menu icon of a one-game program beside it), and
  **QUIT** in the pause menu goes back to it. It also lists the games in the other
  `/switch/` folders (the one-game downloads), each played with the save in its folder, so
  nothing needs moving. The download comes with an empty `roms` folder for your ROMs. The one-game downloads stay as they are.
- **Black and White battles: catching and levelling up.** A thrown Poke Ball is shown whole
  as it lands and shakes (it was cut in two, its other half at the view's right edge: it lands
  where the player's HP box is, whose place was still being moved while the box was put away),
  and the level-up window with the new stats moves with the player's box as one piece instead
  of being cut through (Black 2 already did this; Black's own box places overrode it).
- **Black and White: the wide view's edges in towns and on routes** (trees popping in and
  stretching at the sides, as in Nuvema Town) load the wider map as Black 2 and White 2 do:
  the map's pieces are loaded further out, with the extra memory of the DSi mode
  (`bw2map.h` now knows Black's and White's addresses). Save states made before this need
  `SSR_B2_MAP=0`.
- **Black and White's bag with an empty pocket** is shown as the bag screen alone, without an
  empty card beside it ("It's empty." with nothing to describe).
- **WIDE MAP LOADING (pause menu, Black, White, Black 2, White 2):** ON (the default) loads
  more of the map around the player so the wide view's sides are not empty; OFF leaves the
  game's own map loading alone, if it ever causes trouble. The game sets its memory up for it
  as it starts, so a change takes effect on the next start (the menu says "NEXT START" until
  then). Kept in the settings as `wide_map`.
- **Pause menu:** no game name at the top (it said "POKEMON SOULSILVER" in every game).
- **Trainer battles' start (Black 2, White 2, Black, White): the bars of Poke Balls** that slide
  in for each side's party are shown whole: the HP boxes' places stay put while they are up,
  instead of the bars being cut where the boxes' pieces are moved to the view's edges.
- **Start menus redone for the wide screen.** Black 2, White 2, Black and White: the list and
  the saved game's cards side by side across the whole view on the menu's dark grey, the
  buttons and cards without the Poke Ball picture behind them (it showed cut up and blended
  into them), the lower card closed off cleanly, and the whole list always shown with the
  highlight moving down it (Black's list scrolled the card away). SoulSilver and HeartGold:
  the touch screen's menu at full height with its blue carried to the edges instead of black
  bars. The screens are drawn again from the game's backgrounds for this
  (`belowscreen.h`, `Scene::redrawSkip`, `Scene::redrawUnscroll`).
- **The opening movie of Black and White (and Black 2 and White 2) has plain black bars** beside
  it instead of streaks of its edge colours.
- **Black and White's Pokédex list:** the SEEN / OBTAINED counts above the list, large,
  instead of both screens as a thin column.
- **SoulSilver and HeartGold: SAVE** shows its screen whole from the start (for a second or
  two it was cut to the start menu's panel at the right edge).
- **Switch: new names, without "ssrecomp".** The games' folders and programs are now
  `/switch/pokemon-<game>-wide/pokemon-<game>-wide.nro` (black2, white2, black, white,
  soulsilver, heartgold) and the downloads `pokemon-…-wide-switch.zip`. The settings file beside
  the game is `settings.cfg` (an old `ssrecomp.cfg` there is renamed to it), and the program's
  messages say "Pokemon DS widescreen" and mention starting it from a forwarder.
- **Pokémon White (English, US/Europe).** The same program and layouts as Black: White is the
  same game with its code a few bytes apart, so its function table is Black's at White's
  addresses (`tools/bw1/white_shifts.txt`, found by `tools/bw1/match_white.py`;
  `tools/bw1/funcmap.py --white` makes the table). On the Switch it gets `white.sav`, and the
  messages for a missing or unknown game list it.
- **Black: double, triple and rotation battles laid out for the wide view.** The foes' HP boxes
  stay down the left edge of the view and yours down the right edge, where Black draws them
  (they were cut by the single battle's places), and the battle buttons sit in the message
  box's band so they don't cover your boxes. Choosing a move's target shows the target buttons
  there too (it showed none). Black's table now also knows the target menu, the level-up
  window and the evolution scene by Black 2's names. Headless tests: `--state-at` counts from
  the loaded state under `--relative`, like the other frame options.
- **SoulSilver and HeartGold (English, US) as a download, on the fast JIT.** The program holds
  none of the games' code: its tables have addresses only, and the overlay code the game
  watch compares is read from your ROM when the game starts. HeartGold is told apart by its
  game code; its few moved functions come from `tools/soulsilver/heartgold-us.json`. All of
  SoulSilver's layouts work without the recompiled code, about 1.7 times as fast. The new
  game's first menu is skipped again (also in HeartGold). Battles' widened picture is made
  beside the next frame's emulation. A bot for long test runs: `tools/soulsilver/bot/`.
  On the Switch, HeartGold gets `heartgold.sav` and the JIT like SoulSilver, and the message
  for a game the program doesn't know lists both.
- **The bars beside an evolution or a battle take the screen's outermost colour that isn't
  black** (from its last four columns), so a scene that leaves a black column at the screen's
  edge (SoulSilver's evolution) no longer gets a black bar on that side.
- **3D drawing commands are no longer lost (Black 2, White 2 and the other games on the fast
  JIT).** When the console's 3D command queue was full, a burst of commands from the fast JIT
  could overflow the small overflow store melonDS keeps, and from then on every command read
  its neighbour's numbers. Fixed: White 2's overworld not drawn after a trainer battle (so
  White 2 now has the wider map too), triple and double battles' backgrounds going black for
  a few seconds after a Pokémon faints, and the summary screen's Pokémon sometimes missing.
  Save states made before keep working and fall back in step at once.
- **Black 2 and White 2: double, triple and rotation battles' HP boxes against the edges of the
  wide view,** the foes' in the upper left and yours in the lower right, as in single battles
  (they kept the DS's places in the middle). The boxes stay put while the screen goes black
  into an evolution, instead of the next scene showing through where they were.
- **Switch: a shorter pause menu.** The game always shows the one wide screen, so SCREEN LAYOUT
  and LARGE SCREEN are gone (an older saved layout is ignored). CPU BOOST is gone: the system
  kept taking its boost mode away and the game dropped frames; sys-clk sets the clocks
  instead (see README). EMULATION is gone: FAST JIT always, still falling back by itself for
  one start if a start with it fails, and the interpreter while L is held. BUTTONS, which
  had nothing to choose on the Switch, is gone too.
- **Pokemon White 2 (US/Europe, English):** plays like Black 2, with the same layouts, wide
  view and battle screens (its function table comes from the pokebw2 decompilation's White 2
  symbols), and the wider map.
- **Switch, Black, Black 2 and White 2 start at WORLD DETAIL 720 with 60 FPS on** (with WORLD
  ON GPU, about 16 ms a frame), instead of 288 lines and 60 FPS off, which suited the world
  drawn by the processor. Settings you have already changed are kept.
- **Switch, Black 2 and White 2 without building:** the program can carry the function tables
  for Black 2, White 2 and Black itself and read the few bytes it needs from your ROM when
  the game starts, so one ready-made `.nro` plays any English copy of those games
  (`tools/switch/build_game.sh bw`). A ROM it doesn't know (another language) gets a message
  saying which versions it plays.
- **Switch: one command per game.** `tools/switch/build_game.sh soulsilver|black2|black
  your-rom.nds` does every step once the tools are installed: the function tables, the
  build (with devkitPro's Docker image when devkitPro is not installed) and a folder laid out
  like the SD card (see README, "Nintendo Switch").
- **SoulSilver on the Switch's JIT:** built without the recompiled code
  (`tools/soulsilver/tables_only.py`), it runs on melonDS's JIT, about 1.7 times as fast on
  the Switch, and builds in minutes; its layouts learn which functions and overlays run from
  the emulator instead. In a wide battle the bottom band shows the message box on its left
  and the touch screen's buttons on its right.

- **Nintendo Switch (experimental):** a homebrew `.nro` for consoles with custom
  firmware, built on the PC from your ROM with `tools/switch/build.sh` (see README,
  "Nintendo Switch"). The pause menu's FULLSCREEN is CPU BOOST there, the
  widescreen world starts at 1.5 times the DS's sharpness (WORLD DETAIL 288) and
  60 FPS starts off. The buttons follow the Switch's own labels (A is the right
  button). Quitting saves and closes cleanly (each step is written to `game.log`).
  CPU BOOST now sets the speeds itself, as sys-clk does, instead of asking for the
  system's boost mode (which the system took away for 10-20 seconds every minute or
  so): processor 1785 MHz, memory 1600 MHz (its docked speed), checked five times a
  second, and put back to normal when it is turned off or the game ends.
  Games without recompiled code (the experimental Black 2 build) run on melonDS's
  JIT on the Switch (EMULATION in the pause menu; hold L while starting for the
  interpreter, which is also used by itself after a start where the JIT failed).

- **WORLD ON GPU (Switch, pause menu):** the widescreen world drawn by the Switch's
  graphics chip through deko3D, its own graphics API, instead of the processor, into the
  same picture. On the Skyarrow Bridge at WORLD DETAIL 720 a frame went from about 34 ms
  (30 fps) to about 16 ms (60 fps). On by default; if the game does not get 600 frames
  into it, it is off at the next start. With it, the DS's own 3D picture is drawn by the graphics chip
  too (`ds3d_gpu=0` keeps that on the processor), and the texture memory is only sent to
  the graphics chip when it has changed. On a PC the same
  drawing runs through OpenGL in headless tests (`--wide-gpu`), to compare with the
  processor's picture: they differ only along the edges of polygons.
  LOWER DETAIL WHEN BUSY works with it too: where the graphics chip spends more than 11 ms
  of each frame on the world (a battle at 720 lines), the world is drawn with fewer lines
  until it fits, and back up when there is room.
  Where the game draws its world every other frame (Black 2's cities) and the graphics
  chip has not finished a picture in time, the last one stays on show for a frame instead
  of the game waiting for it; and the graphics chip shades fewer hidden pixels.
  The graphics chip now keeps its pictures compressed and draws the world straight into
  the frame it shows, and each DS texture is decoded once (when the game changes it)
  rather than again for every pixel (that decoding was over half of its drawing time).
- SoulSilver: a new game's first menu, which is on the touch screen alone ("Please touch any
  topic you'd like to know more about"), is answered NO INFO NEEDED by itself as it comes up;
  pressing A had chosen the controls explanation again and again.
- Black 2 battles: the level-up window (the list of stats) moves with your HP box instead of
  being cut through by it; in double, triple and rotation battles the buttons sit in the
  message box's band (as BATTLE BUTTONS' PANEL), below all your HP boxes instead of over the
  lowest one, and choosing a move's target, which was not shown at all, is.
- Black 2: with the widescreen world on, the game loads more of its map beside you (3 of its
  32-tile chunks across instead of 2), so the wide view no longer comes to the end of the
  land where the DS's own screen could not see (beside the cliff at Aspertia's lookout). The
  DS has no room for that in its 4 MB, so Black 2 now gets 8 MB, as the DS's development units
  have, with the game's memory moved into the upper half (as it is given more on a DSi). Save
  states made before keep the game's own map until the game is next started.
- Black 2: where a sky or a distant layer of the view (Castelia zoomed out; the sky,
  mountains, haze and far trees from a lookout) ends a little short of the wide view's edge
  with nothing beside it, it is continued to the edge, its picture carried on, instead of
  leaving a strip of flat colour. The start menu's grey
  circle no longer runs on below the screen as a straight band.

- Switch: quitting no longer hangs when closing the sound never returns (seen at
  "[exit] audio"): after three seconds the program ends without it.
- Black 2: the world no longer flashes up for a frame between CONTINUE and the season's
  screen (the widescreen world is left out while the game shows no 3D). The start menu is
  drawn with sharp pixels and fills the height of a wide view: its list and the saved
  game's card go on below the screens, drawn from what the game has ready there, so the
  last entry is no longer cut in half.
- **Pokemon Black (experimental):** the same program runs Black (US/Europe) like Black 2, from
  a function table made from the aletheia decompilation (`tools/bw1/`), with Black's own
  layouts where its screens differ (`frontend/bw1.h`): the start menu (the saved game's card
  beside the whole list, drawn on below the screen), the X menu, single battles' HP boxes, the
  Badge Case, the Pokedex registration after a catch and the save screen (as Black 2's).
- Black 2: in a single battle on the wide view the HP boxes are back where the game has them,
  the opponent's in the upper left and yours in the lower right (just above the battle's
  buttons), each against the edge of the view, instead of both along the top.
- **BATTLE BUTTONS (pause menu, Black and Black 2):** how a battle's touch-screen buttons are
  shown beside the wide view: CORNER (the panel in the lower right corner, as before; Black 2's
  default), PANEL (the touch screen's panel as tall as the message box) or, in Black, STRIP
  (its default: the buttons large in the message box's band, the one the cursor is on framed).
- Black 2: saving from the X menu has a layout of its own. The question, YES / NO and the
  messages that follow are in a message box along the bottom of the overworld, and the
  saved game's card (with the progress of the save) is a panel in the upper right corner.
  Before, all of that was on the hidden touch screen, and while the save was being
  written both screens popped up side by side for a second or two.
- **Test runs on the Switch:** a `test.txt` in the game's folder lists runs (a snapshot to
  start from, how many frames, which frames to take pictures of, an input script, settings
  for the run only). Each start of the game does the next run, writes the pictures (BMP), a
  line of timings per frame (`timing.csv`) and its `game.log` to `test-results/<run>/`, and
  quits; it then starts again by itself for the next one. The save and the settings are left
  alone. When all are done, `test.txt` becomes `test-done.txt`. On the PC the same options
  exist: `--relative`, `--timing <file.csv>`, `--set key=value`, `--keep-settings`. See
  `frontend/testrun.h` for the format.

- **3D ON GPU** (pause menu, Linux; "3D ON GPU (TEST)" on the Switch): the 3D scene, the
  widescreen world included, is drawn by the graphics chip through OpenGL instead of the
  processor, on the thread that shows the frames. Off by default. If it was on and the game did
  not get 600 frames into it (it hung a Switch once), it is off at the next start.
- When the game runs slower than a DS (a slow device), the sound is stretched to fill the
  time, at its own pitch, instead of breaking up into crackles. At full speed it plays
  exactly as before.
- Fixed: a zoomed or rotated background could drift and smear into streaks when parts
  of it stayed the same from one frame to the next.
- The setup program for macOS can now check for updates and update itself from
  the setup window and with `--update`, matching Linux and Windows.
- A single-screen build of the decompilation (the one-screen game) is recognised when it
  starts and shown as the screen the game itself has chosen, with no merging or
  widescreen changes. The choice is not saved in the settings.

## v0.4.2 — Faster widescreen

**Downloads:** the same setup programs as before; update from the setup window
and run the setup again to rebuild the game (your save is kept). Android
players: press **Build for Android** again and install the new app over the old
one.

- **The widescreen 3D picture takes about half the processor time it did.** It is
  drawn several pixels at a time (SIMD: AVX2 or SSE on PCs, NEON on phones), and
  its working memory now stays in the processor's cache. On PCs it also uses fewer
  threads (about one per core), which leaves more room for the game itself. The
  picture is the same, apart from the odd pixel along the edge of a polygon.
- The widescreen battle view looks for the stats window in one pass instead of four.

## v0.4.1 — Android controllers and update notice

**Downloads:** the same setup programs as before; update from the setup window
and run the setup again to rebuild the game (your save is kept). Android
players: press **Build for Android** again and install the new app over the old
one.

- **Android: controllers such as the Backbone One work.** SDL's own USB drivers
  took over some pads (the Backbone One, PlayStation pads) from Android's
  input, and lost them when they could not open them themselves. All controllers
  now come through Android, which knows them.
- **Android: the app says when there is a new version.** At start it asks
  GitHub for the newest release; when that is newer, a message at the top of the
  screen says to build the app again on the PC (the app holds your own game, so
  it cannot download itself). It is the only thing the app uses the internet for.
- **Lower world detail for slower devices.** WORLD DETAIL in the pause menu now
  also offers 384, 288 and 192 (the DS's own sharpness) as well as 540 to 1440;
  fewer lines draw faster. Left and right go down and up through the choices.
- **FPS counter.** FPS COUNTER in the pause menu shows the frames shown per
  second in the top right corner.

## v0.4.0 — Faster again, cleaner menus

**Downloads:** the setup programs for Linux (x86_64 and arm64), Windows and now
macOS (Apple Silicon and Intel; the Mac app is unsigned, so open it with
right-click → Open the first time). Use the setup window's update button or
download the new one, then run the setup again to rebuild the game (your save
is kept).

- **The bag shows just the bag.** The item description box from the other
  screen is no longer drawn below it.
- **A tidier Pokémon summary.** The Pokémon's name, item and markings sit in
  their own column and the information from both screens is shown side by side, each
  in a rounded card with its contents centred, and no borders cut off.
- **Menus stay on one layout.** Opening the bag, party, Pokégear and the other
  menus from the start menu no longer shows the top screen and then both screens
  before the menu's own view, and closing them no longer falls back to the plain
  bottom screen while they fade out. The view now follows each menu from the
  moment the game starts loading it until it has closed (the game's start and
  end functions of each menu are watched as well as its main one).
- **Faster again: about 30% in the overworld and 20% in battles** (measured
  headless on a 6-core desktop; slower machines such as the Steam Deck and
  phones gain the most). The bottom screen and anything that does not move are
  no longer redrawn every frame, the CPUs switch less often, waits for the game
  card and the touch screen are skipped, and more instructions are recompiled.
  Nothing else changes: in testing, pictures, sound and memory came out the
  same, apart from a one-line difference in a few frames of battle transitions.
- `run.sh` writes `game.log` next to it, as the setup window's Play button does.
- The log lists every controller SDL sees (name, USB IDs, whether its layout is
  known), the SDL version, and the Steam and container settings that can hide a
  controller, for reports like #8.

## v0.3.0 — Faster, updates, logs

**Downloads:** the same three setup programs; use these instead of the earlier
ones and run the setup again to rebuild the game (your save is kept). From this
version on, the setup window can update itself.

### Faster

- **The recompiled game code is about 6–9% faster than melonDS's interpreter**
  (before it was about 16% slower). The common instructions are now plain C++
  instead of calls into the interpreter, and memory accesses and jumps take a
  shorter path. Every instruction was checked against the interpreter
  (`--lockstep`) over 16,000 frames of booting, walking and battling.

### Fixed

- **White strips beside the screen as a battle opens.** The widescreen view showed
  the half of the opening's background, and the 3D pieces, that the game keeps off
  the screen; a row the screen shows in one colour is now that colour beside it.
- **The trainer's legs over the black bar** as he slides out after throwing the
  Poké Ball: beside the screen he is now behind the message box, as on the screen.
- **Fades at the start and end of a battle** reached the sides of the view a frame
  before the screen, so the picture faded unevenly. The sides now use the palettes
  and brightness the screen was drawn with.
- **The stats after a level up** were cut in two, the top half moved to the right
  with the wild Pokémon's side. Windows like it now stay where the game puts them.

### Added

- **Logs for bug reports.** Started from the setup window, the game's messages now
  go to `game.log` (new each time) and a build's output to `build.log`; **Open logs
  folder** opens the folder with both. On Android `log.txt` moved to
  `Android/data/io.github.clickingmaster.ssrecomp/files`, where a file manager or a
  PC can reach it. The renderer in use is now always in the log.

- **Updates from the setup window.** At start it checks for a newer release (shown
  next to the version number); **Update** downloads the setup program for your
  system, checks it against the release's `SHA256SUMS.txt`, puts it in place of
  the old one and restarts. Then press **Build** once to update the game; the
  save and settings are kept. (This version still has to be downloaded by hand.)
- **Issue forms** on GitHub ask for the version, system and log.

- **Portable mode** (#10): with a file `portable.txt` next to the setup program,
  everything is kept in a folder `ssrecomp-data` beside it instead of
  `%LOCALAPPDATA%` (Windows) or `~/.local/share` (Linux). The ROM's location is
  remembered relative to that folder, so it still works on another PC or drive
  letter.

## v0.2.6 — Controller fixes (prerelease)

**Downloads:** the same three setup programs; use these instead of the earlier
ones and run the setup again to rebuild the game.

### Fixed

- **Controllers not working** (#8). Only the first controller found was used;
  on Windows that is often a virtual or idle device (from Steam, DS4Windows,
  vJoy, a racing wheel...), and then nothing pressed on any controller worked.
  Every connected controller now works at once.
- **Generic USB pads** that SDL does not know the layout of were ignored. They
  are now read with the usual layout: the first buttons as A, B, X, Y, then L,
  R, Select, Start, with the D-pad and the left stick. A device whose stick
  rests at one end (some keyboards and mice report themselves as joysticks) has
  its stick ignored, so the player does not walk on their own.
- The controller now also works while the game window is not focused, as when
  it was started from the setup window.

### Tested

- On Linux with a real Xbox Series controller and virtual ones (an Xbox 360
  pad, a generic USB pad, one with a stuck stick). Compiled for Windows, not yet
  tried with a controller there.

## v0.2.5 — Widescreen battles

**Downloads:** the same three setup programs; use these instead of the earlier
ones and run the setup again to rebuild the game (and the APK, on Android).

### New

- **Battles fill the wide screen** instead of having stretched sides. The
  battle scene is drawn wider than the DS screen: the background and the
  platforms continue past its edges, the HP boxes move out to the sides of the
  view, and the wild Pokémon slides over to the right as your own Pokémon is on
  the left. The trainer throwing the Poké Ball walks all the way off screen.
- Move effects stay with the Pokémon they belong to, and the message box is
  left where it is.

### Fixed

- **The battle strip vanished for a moment** between choosing FIGHT and the
  moves appearing, showing the message box under it.

### Known limitations

- Tried on 16:9 only; wider phone screens (20:9) are untested in battles.
- A move effect crossing the gap between the two Pokémon can look stretched
  there for a moment.

## v0.2.4 — Colour fixes

### Fixed

- **Washed-out colours on Windows** (#5): the game asked SDL for its OpenGL ES
  renderer, which on Windows (seen with AMD graphics) can convert every pixel
  as if it were linear light, so the whole picture looked pale. Windows now uses
  SDL's default, Direct3D.
- **The game was always at night.** The DS clock started at midnight on
  1 January 2000 on every launch, so the overworld always had night lighting.
  It now follows the computer's clock, as on a real DS, including after loading
  a quick save.

## v0.2.3 — Android (experimental)

**Downloads:** the same three setup programs; use these instead of the earlier
ones. There is no APK to download: the Android version contains the game's code,
so it is built from your own ROM on your PC, like the PC version.

### New

- **Build for Android** in the setup program (`--android` without a window).
  It downloads Google's Android NDK and SDL2 (once, checked against their
  published checksums), compiles the game for 64-bit ARM Android 7.0+, and makes
  a signed APK to copy to the phone or handheld and install. No Java or Android
  Studio is needed: the APK is packed and signed by `tools/android/apk.py`.
- **On Android:** full screen in landscape; on-screen buttons (D-pad, A/B/X/Y,
  L/R, Start, Select, Menu), hidden while a controller is being used and back at
  a touch; fingers on the picture touch the game's touch screen, several at
  once; Back opens the pause menu; the save is written when the app goes to the
  background; messages and frame timings go to `log.txt` in the app's storage.
- **Tested in Google's Android emulator** (Android 14): installs, updates over
  itself, loads a save, plays with the on-screen buttons and touch, pause menu.
  **Not yet tried on a real phone or handheld**: reports welcome, with
  `log.txt` if something is wrong.

## v0.2.2 — Windows fixes

**Downloads:** the same three setup programs as before; use these instead of the
earlier ones. Already built? Start the new setup program and press **Build**
once: only the changed files are recompiled and your save is kept.

### Fixed

- **Windows: progress was not saved** (#2). Only the first save ever reached
  `soulsilver.sav`; later ones were left in `soulsilver.sav.tmp`, because
  Windows does not let a file be renamed over an existing one. Saves now replace
  the file. If you lost progress this way, see the note below.
- **Windows: the build stopped with "junk at end of line"** in `rc_images.S`
  (#3). The generated assembly ended with a Linux-only line.
- **Windows: "Could not find a package configuration file provided by SDL2"**
  (#3). The setup program now uses MSYS2's CMake ahead of any other copy on the
  PATH, tells CMake where MSYS2's libraries are, and says when SDL2, libpng or
  zlib are missing.
- **Screen jumping around while saving** (#1). During "Saving a lot of
  data…" the layout fell back to the X menu's (the save card beside the
  overworld) and the picture flipped between widescreen and 4:3, because the
  game hardly runs anything else while it writes. The save screen now stays as
  it is until the save is written.
- **Clearer install instructions** in the setup program (#4): it says to open a
  terminal, has a button to copy the commands, and on SteamOS (Steam Deck)
  gives the container steps, as there programs cannot be installed directly.

### Getting back lost progress on Windows (#2)

Close the game, open `%LOCALAPPDATA%\ssrecomp`, rename `soulsilver.sav` to
`soulsilver.sav.old`, then rename `soulsilver.sav.tmp` to `soulsilver.sav`. The
`.tmp` file holds your latest save.

### Tested

- The whole game cross-compiled for Windows with MinGW and run under Wine:
  identical to the Linux build frame for frame, and saving in the game replaces
  the save file. Still not tried on a real Windows PC.

## v0.2.1 — 64-bit ARM Linux

**Downloads:** `ssrecomp-setup-linux-x86_64`,
`ssrecomp-setup-linux-aarch64.AppImage` (64-bit ARM Linux: Raspberry Pi 4/5
with a 64-bit OS, other arm64 boards and laptops) and
`ssrecomp-setup-windows-x86_64.exe` are setup programs, not the game: start
one, choose your own Pokémon SoulSilver (US) ROM, press Build, then Play. Use
these instead of the v0.2.0 ones.

### New

- **Setup program for 64-bit ARM Linux**, as an AppImage
  (`tools/make_appimage.sh`). The game compiles for arm64; cross-compiled and
  run under emulation it is identical to the x86 build frame for frame. Not yet
  tried on real ARM hardware.

### Fixed

- **The game would not compile on ARM, and would crash on older x86 CPUs**: the
  widescreen renderer was always built with AVX2 instructions. They are now
  used only on x86 CPUs that have them; nothing changes on those.
- The recompiler failed when its output folder was on another drive.

### Known limitations

- Windows and ARM builds not yet tried on real hardware (Windows under Wine,
  ARM under emulation).
- See v0.2.0 and v0.1.0 for the rest.

## v0.2.0 — build from your own ROM

**Downloads:** `ssrecomp-setup-linux-x86_64` and
`ssrecomp-setup-windows-x86_64.exe` are setup programs, not the game: start
one, choose your own Pokémon SoulSilver (US) ROM, press Build, then Play. A
compiler and a few libraries are needed; the program says which and how to get
them. A ready-built game cannot be offered, as it would contain the game's code.

### New

- **Setup programs for Linux and Windows**: one file each, made from this
  release's source (`tools/package_setup.py`). They carry the project's source
  and build the game in a folder of their own.
- **Setup window** (`ssrecomp_setup.py`): pick your ROM, press Build, press
  Play. It checks the ROM is the right game, says what is missing to compile
  and how to install it, picks the number of compile jobs from your memory, and
  shows the progress. `python3 ssrecomp_setup.py --rom <file>` does the same
  without a window.
- **No decompilation needed to play.** The code is recompiled straight from
  your ROM (`tools/ndsrom.py` reads and decompresses it); what the
  decompilation used to add comes from `layouts/soulsilver-us.json`, which holds
  addresses and checksums only. The result is identical to building from the
  decompilation. `setup.sh` remains for working on the game's source.
- **Windows (untested):** the code compiles with MinGW, and the setup window
  supports MSYS2 and copies the needed DLLs next to the game. Not yet tried on
  a Windows machine.

### Known limitations

- Tested on Linux only (Ubuntu 24.04 in a container on Bazzite).
- Only Pokémon SoulSilver (US).
- See v0.1.0 for the rest.

## v0.1.0 — first preview

The first public version. Playable from the start of a new game through the
early routes, towns and gyms; the rest of the game has not been played through
with it yet.

**No ready-made download.** A built program would contain the game's code, so
releases are source only: run `./setup.sh` with your own copy of Pokémon
SoulSilver (US) as described in the README.

### What's in it

- **Static recompilation** of the game's ARM9 code from pret/pokeheartgold's
  build output, running on the melonDS core. Checked against melonDS's
  interpreter instruction by instruction (`--lockstep`).
- **One screen, no switching.** Each situation has its own layout that takes the
  useful parts of both DS screens: the overworld with dialogue, the X menu beside
  the overworld, the bag, party, Pokédex, Pokégear, PC, shops, the intro and the
  starter choice.
- **Widescreen overworld:** the 3D world is drawn across the whole 16:9 view at
  a higher resolution (540–1440 lines, "WORLD DETAIL" in the pause menu); trees
  and buildings at the sides are drawn instead of popping in.
- **Battles across the whole screen**, with the touch-screen buttons as a strip
  along the bottom: FIGHT / BAG / POKéMON / RUN and the four moves, chosen with
  left and right, A to pick, B to go back.
- **60 fps:** in-between frames for the 3D scene, which the game updates 30
  times a second.
- **Controller first:** everything is reachable with buttons; a pause menu (Guide
  button or Esc) holds the settings, save states and fast forward.
- **Runs well:** about 9 ms of a 16.7 ms frame at 2560×1440 on a 6-core desktop
  CPU; the picture is composed on the graphics card.
- **`setup.sh`** installs what is missing, builds the game with the
  decompilation, checks it against the retail ROM's SHA-1 and compiles
  everything in one step.

### Known limitations

- Linux only so far (tested on Ubuntu 24.04 in a container on Bazzite).
  Windows, macOS and Android builds do not exist yet.
- Only Pokémon SoulSilver (US). HeartGold and other regions are not supported.
- Later parts of the game, and some screens (radio, contests, the Pokéathlon,
  wireless features), have not been checked and may show a generic two-screen
  layout.
- The widescreen picture has no edge-marking outlines; battles, menus and the
  intro stay 4:3 apart from the battle sides.
- The recompiled code is not yet faster than melonDS's own interpreter; making
  it so is future work.
