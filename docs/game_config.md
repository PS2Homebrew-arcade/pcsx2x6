# game config

Since the execution of arcade games consists on the interaction with several files (a main executable, a disc/HDD image and a memory card image). the system2x6 games will be executed via a special per-game settings file

this files provide all the information the emulator needs to setup the enviroment and run that game

the file uses the INI format, and the extension must be `.acgame`

here's an example of how it should look like in the inside

```ini
[game]
name=SoulCalibur II (SC21 vA10 + DVD0D)
gameid=NM00007
platform=246
[data]
elf=boot.elf 
dongle=NM00007 SC21, Ver.A10 a025781148773a.bin
card=ConquestCard0.bin
mediasrc=dvd0.img
media=DVD
```

## items Breakdown

> mandatory entries will have their name in bold
> if needed, details for some of the entries may appear below

section | entry | value expected | description
------- | ----- | -------------- | ------------
game    | **name**  | string         | cosmetic game title for error messages or game list
game    | **gameid**| string         | Sony official gameID, used for artwork and automatic patches if we ever need such thing
game    | **platform** | string  | value must be `246`, `256` or (`super256` for TimeCrisis4).
data    | **elf**   | string         | elf filename to be executed _(must be at the same location than config file)_
data    | **dongle**| string         | security dongle filename _(must be at the `memcards` folder of PCSX2x6)_
data    | card  | string         | secondary memory card filename, only useful for SoulCalibur2 Conquest mode _(file must be at the `memcards` folder of PCSX2x6)_
data    | **mediasrc**| string       | filename of the media file _(must be at the same location than config file, or subdir if used)_
data    | **media** | string         | media type of the file from `mediasrc`, value must be `CD`, `DVD` or `HDD`.
data    | 256Region | string     | system256 region override, used by emulator to fake regional signature, saves you from the hassle of having several NVRAM files for the same bios
data    | sram  | string  | filename for the SRAM settings. if not found, defaults to `sram.bin`
data    | jvsmode | string | input settings for the game. [see below](#jvsmode)


### `platform`
this command tells the emulator if it should emulate the overclock of the System256 (and SUPER256)
so don't forget to include it on games from those platforms

### `elf`
the elf file mentioned on the `elf` entry must be either the decrypted boot.bin file from the security dongle contents, or the open source bootloader: [Proverb](https://github.com/PS2Homebrew-arcade/proverb/)
the [latest release of proverb](https://github.com/PS2Homebrew-arcade/proverb/releases/download/nightly/proverb.zip) already has a well organized structure so you can download and grab the binaries you need for each game

### `256Region`
NOT IMPLEMENTED YET

### `jvsmode`
manual JVS input parameters.
This is mostly for messing around

starting with **v0.0.33** the appropiate input system is automatically detected from the gameID

### `media`
tells the emulator the correct sector size for data transfers (VERY IMPORTANT)


## game library template

You feel a bit lost? yeah, might be too much to process

however, PCSX2x6 needs more information than regular PCSX2 to run games, since system246 is more complex than retail PS2s

to ease your confusion, the emulator comes bundled with a python script (`utils/arcade_game_template.py`) that will download and setup a template library for each game, providing you with a generic config file for each game and their bootloader

hope it helps out a bit with the confussion

if you don't know how (or want) to setup python for running the script, you may download a [pregenerated package](https://github.com/PS2Homebrew-arcade/pcsx2x6/releases/download/v0.0.10/system2x6_template_gamelibrary.7z) that gets updated from time to time