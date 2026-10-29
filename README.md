# CustomStartMenu 0.1.2

Replaces the Forza Horizon 2 main-menu music (`HZ2_PressStartFINAL_01`, stored in `MasterBank.bank`).

## Install
Copy the contents of `fh2-mods` into your Azimuth `fh2-mods` folder. Requires FH2 Mod Platform 2.8.0 or newer.

## Use
Mods menu > **Main menu music**: Default (original song), Language, ... , Custom (always last).
A new choice applies the next time the menu music starts (e.g. restart the game).

- **Custom**: put one `.wav` or `.mp3` in `fh2-mods\CustomStartMenu\Custom`; the first file by name is used.
- `CustomStartMenu.ini`: `Gain` (0-4, default 1.0) and `FadeMs` (fade-in, default 600) apply to bundled and custom tracks.
- Bundled tracks live in `fh2-mods\CustomStartMenu\CustomStartMenu.bank` (FSB5, PCM16 stereo 44.1 kHz); more will be added in later versions.

## How it works (for modders)
The menu song is started by the game's own FMOD Studio event, which calls FMOD Core `System::playSound` (game+0x199B0F0, paused). The mod hooks that call, recognises the sound by its length (13976064 samples) and passes its own sound (created with `System::createSound`, game+0x1997740, 2D, looped) instead. The event still controls start, volume and fades. The loop point is moved past the fade-in.

