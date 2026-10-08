
# Perfect Dark PS4 port

A port of [this](https://github.com/perfect-dark-pc-port/perfect_dark) to the PS4!.

## Install

NOTE: you also need the two Piglet modules. 
- `libScePigletv2VSH.sprx`
- `libSceShaccVSH.sprx`
if you have Super Mario 64 installed then you already have them.

1.) install the FPKG via Hen
2.) place your rom file to data/perfectdark/pd.ntsc-final.z64
3.) enjoy

Data and Logs are also stored in that folder aswell.

## Building
Step by step, for Ubuntu 24.04 (WSL on Windows).

## 1. Install the tools

```bash
sudo apt update
sudo apt install -y clang-18 lld-18 llvm-18 cmake ninja-build git python3 curl

# OpenSSL 1.1, needed by the packaging tool
curl -LO http://archive.ubuntu.com/ubuntu/pool/main/o/openssl/libssl1.1_1.1.1f-1ubuntu2_amd64.deb
sudo dpkg -i libssl1.1_1.1.1f-1ubuntu2_amd64.deb
```

The build uses Clang 18 from `/usr/lib/llvm-18/bin` automatically, even if another Clang version is
installed as well.

## 2. Download the code

```bash
cd ~
git -c core.autocrlf=false clone https://github.com/patomario/pd-ps4
cd pd-ps4
```

On WSL, build in your Linux home folder (`~`): it is much faster than `/mnt/c` or `/mnt/d`.
Always clone with `-c core.autocrlf=false`; Windows line endings break the build.

## 3. Download the PS4 toolchain

```bash
ps4/setup.sh
```

Downloads OpenOrbis (about 160 MB) into `ps4/tools/`.

## 4. Build the libraries

```bash
ps4/build-deps.sh
```

Downloads and builds zlib and SDL2 into `ps4/prefix/`. Takes a few minutes.

## 5. Build the game

```bash
ps4/build.sh
```

For the PAL or Japanese version: `ROMID=pal-final ps4/build.sh` or `ROMID=jpn-final ps4/build.sh`.

## 6. Make the package

```bash
ps4/package.sh
```

## 7. Updating

```bash
cd ~/pd-ps4
git pull
ps4/build.sh && ps4/package.sh
```

On the PS4, uninstall the old version before installing the new `.pkg`; installing over it with
the same version number can silently keep the old one. Saves and settings in `/data/perfectdark/`
are kept.

## If something goes wrong

| Error | Fix |
| --- | --- |
| `cmake: command not found` | Run the `apt install` line from step 1 again |
| `python3\r: No such file or directory` | The code has Windows line endings: delete the folder and repeat step 2 exactly |
| `cannot find linker script ... link.x` | Toolchain download incomplete: `rm -rf ps4/tools/OpenOrbis`, then `ps4/setup.sh` |
| `The C compiler ... is not able to compile a simple test program` | Check that `/usr/lib/llvm-18/bin/clang` exists (step 1), then `rm -rf build/ps4` and build again |
| `No usable version of libssl was found` | Install OpenSSL 1.1 (second part of step 1) |
| The PS4 still runs the old version | Uninstall the game on the PS4, then install the new `.pkg` |

# OpenSSL 1.1, needed by the packaging tool
curl -LO http://archive.ubuntu.com/ubuntu/pool/main/o/openssl/libssl1.1_1.1.1f-1ubuntu2_amd64.deb
sudo dpkg -i libssl1.1_1.1.1f-1ubuntu2_amd64.deb

# tell the build where Clang 18 is (also for future terminals)
echo 'export PS4_LLVM_BIN=/usr/lib/llvm-18/bin' >> ~/.bashrc
source ~/.bashrc

## Credits

* the original [decompilation project](https://github.com/n64decomp/perfect_dark) authors;
* Ryan Dwyer for the above, additional help, and `pd-extract`;
* doomhack for the only other publicly available [PD porting effort](https://github.com/doomhack/perfect_dark) I could find;
* [sm64-port](https://github.com/sm64-port/sm64-port) authors for the audio mixer and some other changes;
* [Ship of Harkinian team](https://github.com/Kenix3/libultraship/tree/main/src/fast), Emill and MaikelChan for the libultraship version of fast3d that this port uses;
* lieff for [minimp3](https://github.com/lieff/minimp3);
* Mouse Injector and 1964GEPD authors for some of the 60FPS- and mouselook-related fixes;
* Raf for the 64-bit port;
* NicNamSam for the icon;
* everyone who has submitted pull requests and issues to this repository and tested the port;
* probably more I'm forgetting.
