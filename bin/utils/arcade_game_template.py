#!/usr/bin/env python3


import io
import zipfile
import requests
import yaml
from pathlib import Path

GAMEINDEX_URL = "https://github.com/PS2Homebrew-arcade/pcsx2x6/raw/refs/heads/master/bin/resources/GameIndex.yaml"
PROVERB_URL = "https://github.com/PS2Homebrew-arcade/proverb/releases/download/nightly/proverb.zip"

OUTPUT_DIR = Path("system2x6_games")
if OUTPUT_DIR.exists():
    raise SystemExit(f"Output folder '{OUTPUT_DIR}' already exists. skipping download to avoid files overwrite")


def get_platform(region: str) -> str:
    region = region.lower()

    if "super256" in region:
        return "super256"

    if "256" in region:
        return "256"

    return "246"


print(
f"""
        PCSX2x6 game libary template generator
        This script will now prepare you a bootloader and game settings file for each game registered in our database
        you will only need to add your dongle and game media image to the appropiate folders
        EXAMPLE: for TEKKEN4 (gameID NM00004) you will need to put
          - dongle: on the memcards folder of PCSX2x6 named as 'NM00004.ps2'
          - game image: {OUTPUT_DIR}/NM00004/NM00004.chd
"""
)

print("Downloading GameIndex.yaml...")
yaml_data = requests.get(GAMEINDEX_URL)
yaml_data.raise_for_status()

game_db = yaml.safe_load(yaml_data.text)

print("Downloading proverb.zip...")
zip_data = requests.get(PROVERB_URL)
zip_data.raise_for_status()

print("Extracting...")
with zipfile.ZipFile(io.BytesIO(zip_data.content)) as z:
    for member in z.infolist():
        name = member.filename

        if name.startswith("bin/NM"):
            member.filename = name[4:]
            z.extract(member, OUTPUT_DIR)
#with zipfile.ZipFile(io.BytesIO(zip_data.content)) as z:
#    z.extractall(OUTPUT_DIR)

OUTPUT_DIR = OUTPUT_DIR

if not OUTPUT_DIR.exists():
    raise RuntimeError(f"BIN directory not found: {OUTPUT_DIR}")

created = 0

for gameid, info in game_db.items():

    game_dir = OUTPUT_DIR / gameid

    if not game_dir.is_dir():
        print(f"[FAIL] '{game_dir}' not a folder. skipping")
        continue

    name = info.get("name", gameid)
    media = info.get("media", "???")
    platform = get_platform(info.get("region", ""))
    if media == "???":
        print(f"[FAIL] '{acgame_path}' no media type inside. skipping")
        continue
    
    acgame = f"""[game]
name={name}
gameid={gameid}
platform={platform}

[data]
subdir={gameid}
elf=boot.elf
dongle={gameid}.ps2
mediasrc={gameid}.chd
media={media}
"""

    acgame_path = OUTPUT_DIR / f"{gameid}.acgame"

    with open(acgame_path, "w", encoding="utf-8", newline="\n") as fp:
        fp.write(acgame)

    created += 1
    print(f"[CREATE] '{acgame_path}'", end='\r')

print(f"\nDone. Generated {created} .acgame files.")