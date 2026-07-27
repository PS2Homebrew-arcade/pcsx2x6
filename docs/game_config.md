# game config

Since the execution of arcade games consists on the interaction with several files (a main executable, a disc/HDD image and a memory card image). the system2x6 games will be executed via a special per-game settings file

this files provide all the information the emulator needs to setup the enviroment and run that game

the file uses the INI format, and the extension must be `.acgame`

here's an example of how it looks like in the inside

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
game    | [name](#name)  | string         | cosmetic game title for error messages or game list
game    | [gameid](#gameid)| string         | Sony official gameID, used for artwork and automatic patches if we ever need such thing
game    | [platform](#platform) | string  | value must be `246`, `256` or (`super256` for TimeCrisis4).
data    | [elf](#elf)   | string         | elf filename to be executed _(must be at the same location than config file)_
subdir  | [subdir](#subdir) | string         | subfolder to store all relevant files excluding the dongle images
data    | [dongle](#dongle) | string         | security dongle filename _(must be at the `memcards` folder of PCSX2x6)_
data    | [card](#card)  | string         | secondary memory card filename, only useful for SoulCalibur2 Conquest mode _(file must be at the `memcards` folder of PCSX2x6)_
data    | [mediasrc](#mediasrc) | string       | filename of the media file _(must be at the same location than config file, or subdir if used)_
data    | [media](#media) | string         | media type of the file from `mediasrc`, value must be `CD`, `DVD` or `HDD`.
data    | [256Region](#256region) | string     | system256 region override, used by emulator to fake regional signature, saves you from the hassle of having several NVRAM files for the same bios
data    | [sram](#sram)  | string  | filename for the SRAM settings. if not found, defaults to `sram.bin`
data    | [jvsmode](#jvsmode) | string | input settings for the game.

### `name`

purely cosmetic. used for error messages, game list name, and discord rich presence. if not declared it gets auto resolved based on game database

### `gameid`
The most important entry of the config file, it's the only one that cannot be skipped under any circumstance


### `platform`
this command tells the emulator if it should emulate the overclock of the System256 (and SUPER256). if not declared it gets auto resolved based on game database

### `elf`
the name of the elf file to be executed to kickstart the game

defaults to `boot.elf`

### `subdir`
a subdirectory to store all the files for that game excluding the dongle image. if not declared it defaults to the game ID

### `dongle`
the filename of the security dongle to be mounted before launching the game. if not declared it defaults to `GAMEID.ps2`, it has to be stored on the `memcards` folder of PCSX2x6

### `card`
the filename of the memory card image to be mounted on left port before launching the game, only useful for soulcalibur2 conquest card. there's no default value for this one, if not declared, the second port gets cleared out. file has to be stored on the `memcards` folder of PCSX2x6
### `mediasrc`
the filename of the media image for the game. if not declared, it defaults to `GAMEID.chd`
### `media`
tells the emulator the correct sector size for data transfers. if not declared it gets auto resolved based on game database

### `256Region`
make the emulator feed custom payloads into the mechacon commands used to read the iLink ID from ps2 NVRAM.

this is used by the taiko drum games to identify the region of the system256 unit.

valid values: [`ASIA4`, `ASIA5`, `JAPAN`]

### `jvsmode`
manual JVS input parameters.
This is mostly for messing around with settings

starting with **v0.0.33** the appropiate input system is automatically detected from the gameID

dont include this entry if you don't know what youre doing

### `sram`
name of the file that will store the contents of the sram that stores settings on the system2x6 units

defaults to `sram.bin`

## game library template

You feel a bit lost? yeah, might be too much to process

however, PCSX2x6 needs more information than regular PCSX2 to run games, since system246 is more complex than retail PS2s

to ease your confusion, the emulator comes bundled with a python script (`utils/arcade_game_template.py`) that will download and setup a template library for each game, providing you with a generic config file for each game and their bootloader

hope it helps out a bit with the confussion

if you don't know how (or want) to setup python for running the script, you may download a [pregenerated package](https://github.com/PS2Homebrew-arcade/pcsx2x6/releases/download/v0.0.10/system2x6_template_gamelibrary.7z) that gets updated from time to time

## Settings simplification
starting with [v0.2.20](https://github.com/PS2Homebrew-arcade/pcsx2x6/releases/tag/v0.2.20), PCSX2x6 can auto resolve a LOT of config values for you!

the only mandatory setting is the gameID, the rest will be auto resolved one way or another

as an example: lets take dragonballZ

lets say I make a config file for the game in 
```
C:\GAMES\SYSTEM2x6\DBZ.acgame
```

and inside the file I only declare
```ini
[game]
gameid=NM00027
```
you would only need to:

- put the disc image in `C:\GAMES\SYSTEM2x6\NM00027\NM00027.chd`
- put the dongle on the memcards folder of PCSX2x6, named as `NM00027.ps2`

and it should work out of the box