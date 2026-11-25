# The robco command

`robco` is the command line of RobCo Studio. It installs SDK packages, writes a project from a template, builds a tape, runs it in QEMU and runs a project's scenarios. This spec fixes what crosses between `robco` and whatever calls it: the command lines, the two output streams, the JSON lines of `--json`, the exit codes, the card image of a run, what ends a run, and the scenario files of `robco test`.

State today: `robco` prints its version and nothing else. No command of this spec exists yet. [STUDIO_PROJECT.md](./STUDIO_PROJECT.md) fixes the project files and [SDK_PACKAGE.md](./SDK_PACKAGE.md) the packages. This spec is version 1 of every format it defines.

## Commands

| Command | Does | Needs a project |
|---|---|---|
| `robco sdk list [--index LOCATION]` | Lists the packages and tools in the cache, and with `--index` those the index offers | No |
| `robco sdk install PACKAGE[@VERSION] --index LOCATION` | Installs a package, and for `pipos` the two tools it names | No |
| `robco new TEMPLATE NAME` | Writes a new project folder from a template | No |
| `robco update --index LOCATION` | Resolves again and writes `robco.lock`. [STUDIO_PROJECT.md](./STUDIO_PROJECT.md#robco-update) defines it. | Yes |
| `robco build [--index LOCATION]` | Builds `build/NAME.HTP` | Yes |
| `robco run --headless [--expect TEXT] [--timeout SECONDS] [--board BOARD] [--index LOCATION]` | Builds, then boots the tape in QEMU and prints its serial lines | Yes |
| `robco test [SCENARIO...] [--update-golden] [--board BOARD] [--index LOCATION]` | Builds, then runs the project's scenarios in QEMU | Yes |

Every command also takes `--json`. `robco --version` and `robco version` print the version and are not part of this spec.

### Options

- An option that takes a value is written as two arguments, `--index LOCATION`, or as one, `--index=LOCATION`.
- Options and the other arguments may come in any order after the command words `sdk list`, `sdk install`, `new`, `update`, `build`, `run` and `test`.
- An unknown command, an unknown option, an option given twice, a missing value, a value outside its limits, a missing argument or an extra argument fails with `usage`. The command does nothing else.
- `--index` follows [Where the index is](./SDK_PACKAGE.md#where-the-index-is).

### The project folder

A command that needs a project runs in the project root. It looks for `robco.toml` in the current folder only, as [A project folder](./STUDIO_PROJECT.md#a-project-folder) says. Without that file it fails with `project: none`.

### Output streams

| Mode | Standard output | Standard error |
|---|---|---|
| Without `--json` | Text for a person. Its wording is not a contract. `robco run --headless` is the exception: it writes the serial lines and nothing else, as [Serial lines](#serial-lines) says. | First the diagnostics of the compiler and the linker, as those programs print them. Then, when the command fails, one error line. It is the last line. |
| With `--json` | The JSON lines of [JSON lines](#json-lines), and nothing else | Nothing |

A command that fails without `--json` prints exactly one error line:

```
<code>: <detail>
```

`<code>` is a code of [Error codes](#error-codes), such as `manifest: name`. `<detail>` is text for a person, on the same line. A tool acts on the code and on the exit code, never on the detail.

## Exit codes

Every command ends with one of these codes. A caller can act on the code alone.

| Code | Name | Meaning | Commands |
|---|---|---|---|
| 0 | `ok` | The command did what it was asked, and every check it made held | All |
| 1 | `failed` | The command ran to its end, and a check on the running tape did not hold | `run`, `test` |
| 2 | `usage` | The command line is wrong | All |
| 3 | `project` | The project's files, or a value given for the project, are refused | All but `sdk list` and `sdk install` |
| 4 | `sdk` | A package, a tool, the index or the cache is refused | All |
| 5 | `build` | A compile, the link, `objcopy` or the seal failed | `build`, `run`, `test` |
| 6 | `timeout` | The time limit of a run passed before the run ended | `run` |
| 7 | `simulator` | QEMU did not start, died or stopped answering | `run`, `test` |
| 8 | `guest` | The machine failed outside the tape: a kernel fault, or a runner ROM that did not report | `run` |
| 9 | `io` | The host refused to read, write, create or rename a file | All |
| 10 | `internal` | A fault of the tool itself. The message asks for a report. | All |

A command that cannot tell which case holds never ends with 0.

`robco run` gives one code per outcome. [What ends a run](#what-ends-a-run) has the rules.

| Outcome of `robco run --headless --expect TEXT` | Code |
|---|---|
| A line held `TEXT` | 0 |
| The tape ended and no line held `TEXT` | 1 |
| The time limit passed | 6 |
| QEMU did not start, or died | 7 |
| A line held an `ev=exception` or `ev=panic` marker | 8 |
| The runner ROM did not report, or could not read its settings | 8 |

`robco test` ends with 0 when every scenario passed, with 7 when QEMU failed in at least one scenario, and with 1 when a scenario failed and QEMU failed in none. A fault of the machine inside a scenario fails that scenario, so it gives 1.

### Error codes

Every failure has one code. This table maps each code to its exit code, including the codes that [STUDIO_PROJECT.md](./STUDIO_PROJECT.md#errors) and [SDK_PACKAGE.md](./SDK_PACKAGE.md) define.

| Code | Exit | Defined in | When |
|---|---|---|---|
| `usage` | 2 | [Options](#options) | The command line is wrong |
| `project: none` | 3 | [The project folder](#the-project-folder) | The current folder holds no `robco.toml` |
| `manifest: syntax`, `manifest: format`, `manifest: unknown key`, `manifest: type`, `manifest: missing`, `manifest: name`, `manifest: version`, `manifest: title`, `manifest: kind`, `manifest: subtype`, `manifest: platform`, `manifest: abi`, `manifest: path`, `manifest: icon`, `manifest: sources` | 3 | [Errors](./STUDIO_PROJECT.md#errors) | `robco.toml` is refused |
| `lock: syntax`, `lock: format`, `lock: unknown key`, `lock: stale`, `lock: hash`, `lock: missing`, `lock: tool` | 3 | [robco.lock](./STUDIO_PROJECT.md#robcolock) | `robco.lock` is refused, or no longer matches what it pins |
| `new: name`, `new: template`, `new: exists` | 3 | [robco new](#robco-new) | The name, the template or the folder is refused |
| `run: board` | 3 | [Starting QEMU](#starting-qemu) | The package has no such board |
| `test: none`, `test: name`, `test: syntax`, `test: format`, `test: unknown key`, `test: type`, `test: missing`, `test: step`, `test: value`, `test: golden` | 3 | [Scenario files](#scenario-files) | A scenario file or a golden image is refused |
| `package: archive`, `package: format`, `package: manifest`, `package: hash` | 4 | [SDK_PACKAGE.md](./SDK_PACKAGE.md#the-archive) | A package is refused |
| `index: location`, `index: read`, `index: format`, `index: entry`, `index: duplicate`, `index: host` | 4 | [The index](./SDK_PACKAGE.md#the-index) | The index is refused, or has no archive for this host |
| `sdk: no package`, `sdk: no tool` | 4 | [Version and ABI rules](./SDK_PACKAGE.md#version-and-abi-rules) | Nothing in the index fits |
| `tool: hash` | 4 | [Hashes](./SDK_PACKAGE.md#hashes) | A tool archive fails its check |
| `tool: program` | 4 | [robco build](#robco-build) | An installed tool folder lacks a program the command must start |
| `firmware: hash` | 4 | [Firmware pins](./SDK_PACKAGE.md#firmware-pins) | A firmware file fails its check |
| `cache: path` | 4 | [The cache folder](./SDK_PACKAGE.md#the-cache-folder) | `ROBCO_CACHE` is not an absolute path |
| `templates: none`, `templates: format`, `templates: manifest`, `templates: token` | 4 | [robco new](#robco-new), [templates.json](./SDK_PACKAGE.md#templatesjson) | No `templates` package is installed, or the installed one is refused |
| `build: compile`, `build: link`, `build: objcopy`, `build: seal` | 5 | [robco build](#robco-build) | A build step failed |
| `run: unmatched`, `run: tape` | 1 | [What ends a run](#what-ends-a-run) | The tape ended without the awaited result |
| `run: timeout` | 6 | [What ends a run](#what-ends-a-run) | The time limit passed |
| `sim: start`, `sim: died`, `sim: qmp` | 7 | [Starting QEMU](#starting-qemu) | QEMU did not start, died or did not answer |
| `guest: fault`, `guest: runner` | 8 | [What ends a run](#what-ends-a-run) | The machine failed outside the tape |
| `test: failed` | 1 | [robco test](#robco-test) | At least one scenario failed |
| `io: read`, `io: write` | 9 | This table | The host refused a file operation. The detail names the path. |
| `internal` | 10 | This table | A fault of the tool itself |

`test: failed` gives exit code 7 in place of 1 when QEMU failed in a scenario, as [Exit codes](#exit-codes) says.

## JSON lines

With `--json` a command writes one JSON object per line on standard output.

| Rule | Detail |
|---|---|
| Encoding | UTF-8, no byte order mark. Each line is one object with no line break inside, ended by one line feed, on every system. |
| Order of keys | `v`, then `kind`, then the keys of the kind in the order of its table |
| `v` | The integer `1` for this spec, on every line |
| Numbers | Integers only |
| Paths | `/` as the separator on every system. A path inside the project is relative to the project root. |
| The last line | Exactly one `result` line, and it is the last line of every run |
| Errors | A run that ends with a code other than 0 writes exactly one `error` line, right before the `result` line. A run that ends with 0 writes none. |
| A run with no `result` line | The command was killed. A caller treats it as failed. |

Rules for a newer version:

- A new key on an existing kind, and a new kind, do not raise `v`. A caller ignores a key and a kind it does not know.
- A key that is removed, renamed, retyped or given another meaning raises `v`. So does a kind that is removed.
- A caller that reads a `v` larger than it knows stops and reports it. It does not guess.

### Kinds

| `kind` | Written by | Meaning |
|---|---|---|
| `step` | `sdk install`, `update`, `build`, `run`, `test` | A unit of work started, ended or was skipped |
| `diagnostic` | `build`, `run`, `test` | One line that a program started by the tool printed |
| `item` | `sdk list` | One package or tool |
| `serial` | `run`, `test` | One line of the machine's PL011 |
| `scenario` | `test` | A scenario started or ended |
| `check` | `test` | One step of a scenario ended |
| `error` | All | The one failure of the run |
| `result` | All | The end of the run |

### step

| Key | Type | Value |
|---|---|---|
| `step` | string | `resolve`, `download`, `verify`, `install`, `compile`, `link`, `objcopy`, `seal`, `card` or `boot` |
| `item` | string | What the step works on. Empty when the step has one subject only. |
| `state` | string | `start`, `done` or `skip` |

A `start` line is followed by a `done` line with the same `step` and `item`, unless the command fails inside the step. A `skip` line stands alone: the work was not needed. Version 1 runs one step at a time, so no two steps are open at once.

| `step` | `item` | The work |
|---|---|---|
| `resolve` | Empty | Reading the index and picking packages and tools |
| `download` | `pipos@1.4.0` for a package, `gcc@15.2.1-1.1/linux-x64` for a tool | Fetching an archive |
| `verify` | As `download` | The hash checks of [Hashes](./SDK_PACKAGE.md#hashes) |
| `install` | As `download` | Extracting, and moving the folder into the cache. `skip` when the cache already holds it. |
| `compile` | `crt0` for the startup code, else the source path | Step 1 or 2 of [Build order](./SDK_PACKAGE.md#build-order) |
| `link`, `objcopy`, `seal` | Empty | Steps 3, 4 and 5 |
| `card` | Empty | Writing [the simulator card](#the-simulator-card) |
| `boot` | The board | From starting QEMU to the `cont` command |

An item that is already in the cache writes one `install` line with `skip` and no `download` or `verify` line.

### diagnostic

| Key | Type | Value |
|---|---|---|
| `program` | string | `gcc`, `objcopy` or `qemu` |
| `text` | string | One line of what the program printed, without its line end |

### serial

| Key | Type | Value |
|---|---|---|
| `t_ms` | integer | Milliseconds on the host's clock since the tool sent `cont` |
| `text` | string | One line of the PL011, by the rules of [Serial lines](#serial-lines) |

### error

| Key | Type | Value |
|---|---|---|
| `code` | string | A code of [Error codes](#error-codes) |
| `detail` | string | Text for a person, on one line |
| `exit` | integer | The exit code of the run |

### result

Every `result` line starts with these keys:

| Key | Type | Value |
|---|---|---|
| `command` | string | `sdk list`, `sdk install`, `new`, `update`, `build`, `run` or `test` |
| `ok` | boolean | `true` when `exit` is 0 |
| `exit` | integer | The exit code of the run |

The keys that follow depend on the command. Each command's section lists them. They are present when the table says "Always", and otherwise only when `ok` is `true`.

## robco sdk list

`robco sdk list [--index LOCATION] [--json]` writes one `item` line per package version and per tool archive: every one in the cache, and with `--index` also every one the index lists. It changes nothing.

| Key | Type | Value |
|---|---|---|
| `type` | string | `package` or `tool` |
| `name` | string | The name |
| `version` | string | The version |
| `host` | string | The host of a tool archive, such as `linux-x64`. Empty for a package. |
| `installed` | boolean | The cache holds it |
| `offered` | boolean | The index lists it. `false` without `--index`. |

Packages come first, then tools. Each group is sorted by `name`, then by `version`: as numbers for a package, as bytes for a tool, then by `host`. The `result` line adds one key:

| Key | Type | Present | Value |
|---|---|---|---|
| `count` | integer | When `ok` | The number of `item` lines |

## robco sdk install

`robco sdk install PACKAGE[@VERSION] --index LOCATION [--json]` installs one package into the cache.

| Argument | Picks |
|---|---|
| `PACKAGE` | The highest version of that package in the index |
| `PACKAGE@X.Y` | The highest version whose first two parts are `X.Y`, such as `pipos@1.4` |
| `PACKAGE@X.Y.Z` | That version |

Any other form after `@` fails with `usage`. A package that the index does not list at a version that matches fails with `sdk: no package`. `gcc` and `qemu` are tools, not packages, so `robco sdk install gcc` fails with `sdk: no package`.

The command then does this, in order:

1. It resolves the package and each package it `requires`.
2. For `pipos`, it adds the two tools the package names, for the running host, as [Tools a package names](./SDK_PACKAGE.md#tools-a-package-names) says.
3. For each of them that the cache does not hold, it downloads, checks and installs, by the rules of [SDK_PACKAGE.md](./SDK_PACKAGE.md#hashes). One that the cache holds is skipped.

It reads no `robco.toml` and writes no `robco.lock`. A second run with the same arguments skips everything and ends with 0. A failure leaves nothing of the failed item behind and leaves the items installed before it in place.

The `result` line adds:

| Key | Type | Present | Value |
|---|---|---|---|
| `packages` | array of objects | When `ok` | `name`, `version` and `state` of each package, in the order installed |
| `tools` | array of objects | When `ok` | `name`, `version`, `host` and `state` of each tool, `gcc` first |

`state` is `installed` when this run put it into the cache and `present` when the cache already held it.

## robco new

`robco new TEMPLATE NAME [--json]` writes a new project into the folder `NAME` of the current folder. Everything it writes comes from the installed `templates` package of [The templates package](./SDK_PACKAGE.md#the-templates-package). It reads no index and installs nothing.

It makes these checks in order and stops at the first that fails. It writes nothing until all have passed.

| Order | Check | Error |
|---|---|---|
| 1 | `NAME` follows the [name rules](./STUDIO_PROJECT.md#name-rules), as given. The command does not change its case. | `new: name`, with the reason |
| 2 | The cache holds a `templates` package. The command takes the highest version in `<cache>/packages/templates/`. | `templates: none`. The detail ends with `run robco sdk install templates`. |
| 3 | The tool knows every format in `formats` of that package's `sdk.json` | `package: format` |
| 4 | `templates.json` passes the checks of [templates.json](./SDK_PACKAGE.md#templatesjson) | `templates: format` or `templates: manifest` |
| 5 | `templates.json` lists a template whose `id` is `TEMPLATE` | `new: template`. The detail lists the ids. |
| 6 | No file in a `substitute` file holds a token other than `@NAME@` | `templates: token` |
| 7 | `NAME` in the current folder does not exist, or is an empty folder | `new: exists` |

Then it writes:

1. It creates a new folder beside the target, in the current folder, with a name of its own choice that starts with a dot.
2. It writes each file of the template there, at its `path`, with the folders the path needs. A `substitute` file gets every `@NAME@` replaced by `NAME`. Every other byte is the package's byte.
3. It removes an empty `NAME` folder if there is one, and renames the new folder to `NAME`.

A failure at any step removes the new folder and reports `io: write`. So the command leaves either the whole project or nothing. It writes no `robco.lock` and no `build/`: the first `robco build` resolves and writes the lock.

The `result` line adds:

| Key | Type | Present | Value |
|---|---|---|---|
| `template` | string | When `ok` | The template's `id` |
| `name` | string | When `ok` | `NAME` |
| `path` | string | When `ok` | The folder written, relative to the current folder: `NAME` |
| `files` | array of strings | When `ok` | The paths written, relative to `path`, sorted in byte order |

## robco build

`robco build [--index LOCATION] [--json]` builds the tape of the project in the current folder. It does these in order and stops at the first failure:

| Order | Work | Errors |
|---|---|---|
| 1 | Read `robco.toml` | `project: none`, `manifest: ...` |
| 2 | Read `robco.lock`, or resolve and write it when there is none, by [When the lock changes](./STUDIO_PROJECT.md#when-the-lock-changes) | `lock: ...`, `index: ...`, `sdk: ...` |
| 3 | Install each locked package, and the locked `gcc` tool, where the cache lacks them. A build never downloads `qemu`. | `package: ...`, `tool: hash`, `index: ...`, `lock: hash`, `lock: missing`, `lock: tool` |
| 4 | Find `arm-none-eabi-gcc` and `arm-none-eabi-objcopy` in `bin/` of the locked `gcc` tool, with `.exe` on Windows | `tool: program` |
| 5 | Compile the startup code and each source, steps 1 and 2 of [Build order](./SDK_PACKAGE.md#build-order), in the order of the [source list](./STUDIO_PROJECT.md#source-order) | `build: compile`, with the source |
| 6 | Link, step 3 | `build: link` |
| 7 | `objcopy`, step 4 | `build: objcopy` |
| 8 | Seal and validate, by [Sealing and packing](./IMAGE_FORMAT.md#sealing-and-packing) | `build: seal`, with the `@RC` name of the code, such as `size` |
| 9 | Write `build/NAME.HTP` and `build/NAME.elf` | `io: write` |

A program that exits with a code other than 0 fails its step. Each line a program prints becomes a `diagnostic` line with `--json`. [What a holotape build produces](./STUDIO_PROJECT.md#what-a-holotape-build-produces) says what is left after a failure.

The `result` line adds:

| Key | Type | Present | Value |
|---|---|---|---|
| `tape` | string | When `ok` | `build/NAME.HTP` |
| `elf` | string | When `ok` | `build/NAME.elf` |
| `size` | integer | When `ok` | The size of the tape in bytes |
| `sha256` | string | When `ok` | The SHA-256 of the tape, 64 lower case hex digits |
| `abi` | string | When `ok` | `target.abi`, such as `1.3` |
| `gcc` | string | When `ok` | The version of the locked `gcc` tool, such as `15.2.1-1.1` |
| `sources` | array of strings | When `ok` | The [source list](./STUDIO_PROJECT.md#source-order), in link order |

`robco update` writes `step` lines for `resolve`, `download`, `verify` and `install`, and a `result` line with no added key.

## The simulator card

`robco run` and `robco test` boot the machine from a card image that the tool builds for each run. It is the card of a real device, less the firmware files, with the runner ROM in place of Deitrix.

| Property | Value |
|---|---|
| Size | 16 MiB, 16,777,216 bytes |
| Sector 0 | An MBR with one partition entry at offset `0x1BE`: status `0x00`, first CHS `FE FF FF`, type `0x0E`, last CHS `FE FF FF`, first sector 2048, 30,720 sectors. Bytes 510 and 511 are `55 AA`. Every other byte of sectors 0 to 2047 is 0. |
| Volume | FAT16, made by the suite's own FatFs: `fs_mkfs()` with `FS_MKFS_FAT` and a cluster of 512 bytes |
| Dates | The library's fixed time, 2026-01-01 00:00:00, on every file and folder. The tool sets no clock. |
| Entries | Created in the order of the table below, which fixes the order inside each folder |

The files, in the order the tool creates them:

| Order | Path on the card | Bytes from | Read by |
|---|---|---|---|
| 1 | `/PIPOS.IMG` | The package, `boot/PIPOS.IMG` | Nothing in QEMU. A real device's firmware loads it. |
| 2 | `/config.txt` | The package, `boot/config.txt` | Nothing in QEMU |
| 3 | `/PIPBOY/` | A folder | |
| 4 | `/PIPBOY/PIPOS.CFG` | The tool: the text below | The kernel |
| 5 | `/PIPBOY/ROM1.ROM` | The package, `boot/RUNNER.ROM` | The kernel |
| 6 | `/PIPBOY/RUNNER.CFG` | The tool: the text of [RUNNER.CFG](./SDK_PACKAGE.md#runnercfg) | The runner ROM |
| 7 | `/PIPBOY/HOLO/` | A folder | |
| 8 | `/PIPBOY/HOLO/NAME.HTP` | The project, `build/NAME.HTP` | The kernel, for the runner ROM |

`NAME` is `project.name`. The card holds nothing else: no firmware file, no save folder and no other tape. The kernel creates `/PIPBOY/SAVE` when the tape first saves.

`/PIPBOY/PIPOS.CFG` is exactly this text: 11 lines, each ended by one line feed, 113 bytes in all.

```
[display]
scheme = green
line_delay_ms = 0
backend = hdmi

[audio]
volume = 100
beep = no

[holotape]
slot = off
```

| Key | Value | Why |
|---|---|---|
| `display.scheme` | `green` | The default colours of a tape follow the scheme. A fixed scheme keeps golden images right. |
| `display.line_delay_ms` | `0` | No pause between the boot lines, so the tape starts as soon as the kernel is up. The package's own `boot/PIPOS.CFG` pauses 150 ms per line. |
| `display.backend` | `hdmi` | QEMU has no SPI panel. The screen a capture reads is the HDMI one. |
| `audio.volume` | `100` | A fixed level in the audio trace |
| `audio.beep` | `no` | No power-on beep in the audio trace, so every sound in it is the tape's |
| `holotape.slot` | `off` | QEMU has no card on SPI0. The tape is on the boot card. |

The tool writes all six keys even where the value is the kernel's default, so a later kernel with other defaults gives the same run.

The same package and the same tape give the same card image, byte for byte, on every system. The files always fit: the kernel, the ROM and the tape are each at most 4 MiB. Each run, and each scenario of `robco test`, starts from a fresh copy of the card. What a tape saves is gone when the run ends.

Left out on purpose: FAT32, a card for a real device, a card with Deitrix, and a card that keeps its saves between runs.

## robco run

`robco run --headless [--expect TEXT] [--timeout SECONDS] [--board BOARD] [--index LOCATION] [--json]` builds the tape, boots it in QEMU on the runner ROM and reads the machine's serial lines until the run ends.

| Option | Value | Default | Meaning |
|---|---|---|---|
| `--headless` | None | | Required in version 1. Without it the command fails with `usage`: the run in a window comes later. |
| `--expect` | `TEXT`, 1 to 200 bytes, with no line feed | None | The text to wait for. See [What --expect matches](#what---expect-matches). |
| `--timeout` | Whole seconds, 1 to 3600 | 60 | The time limit of the run |
| `--board` | A board name | `raspi0` | The QEMU machine |

The command first installs the locked `qemu` tool where the cache lacks it, by the rules of step 3 of [robco build](#robco-build), and then does everything `robco build` does. A failure there ends the command with that failure's code, and no QEMU starts.

### Starting QEMU

The tool boots the machine from the `pipos` package that the lock names, `PKG`, and the `qemu` tool that the lock names.

1. It reads `PKG/boot.json`, by the rules of [boot.json](./TESTING.md#bootjson). A `--board` that the file does not list fails with `run: board`, and the detail lists the boards. The mode is the board's `default_mode`.
2. The program is the board's `qemu` value in `bin/` of the `qemu` tool folder, with `.exe` on Windows. A missing program fails with `tool: program`.
3. It makes a new, empty folder for the run in the system's temporary folder, and writes [the simulator card](#the-simulator-card) there as `sd.img`.
4. It builds the command line in the order of [boot.json](./TESTING.md#bootjson): `{stub}` is `PKG/boot/fwstub.bin`, `{image}` is `PKG/boot/PIPOS.IMG` and `{sd}` is the card. It attaches QMP once, no qtest, both serial ports and the card, and no other device.
5. QMP and both serial ports use TCP on `127.0.0.1`, on every system.
6. It starts QEMU so that QEMU dies when the tool dies, connects to QMP and to both serial ports, and only then sends QMP `cont`. The time limit of the run starts at `cont`.
7. While the machine runs, it sends QMP `query-status` every 20 ms. [QEMU keep-alive](./TESTING.md#qemu-keep-alive) says why: without it QEMU 10.1 at times leaves a `raspi0` or `raspi1ap` machine halted for ever.

| Failure | Error |
|---|---|
| QEMU does not start, or a connection is not made within 10 s | `sim: start` |
| QEMU exits, or one of its three connections closes, before the run ends | `sim: died` |
| QMP answers a command with an error, or does not answer within 10 s | `sim: qmp` |

A read that waits on a silent machine is not a closed connection. A machine that prints nothing for a while is still running.

When the run ends, the tool stops QEMU and removes the run's folder before it exits. No QEMU process outlives the command, however the command ends.

### Serial lines

The tool reads the PL011, the first serial port, as lines.

| Rule | Detail |
|---|---|
| A line | The bytes up to a line feed, without the line feed and without one carriage return before it |
| A long line | Cut at 4096 bytes. The rest starts a new line. |
| An unfinished line | It is not a line until its line feed comes. When the run ends, the bytes still waiting are written as one last line and are never matched. |
| As text | UTF-8, with U+FFFD in place of each byte that is not valid |

Without `--json` the command writes each line on standard output as it comes, ended by one line feed. With `--json` it writes one `serial` line per line. The mini UART carries the script channel and is never printed.

### What --expect matches

`TEXT` matches a line when the bytes of `TEXT` occur in the bytes of the line. The match is exact and case-sensitive. `TEXT` is plain text and never a pattern: no character in it is special. Only whole lines of the PL011 are searched.

Left out on purpose: regular expressions, a match across two lines, and a match on the script channel.

### What ends a run

The tool handles each whole line in this order, and the first rule that holds ends the run:

| Order | Rule | Outcome | Exit | Error |
|---|---|---|---|---|
| 1 | The line is written out, as [Serial lines](#serial-lines) says | | | |
| 2 | `--expect` is given and `TEXT` matches the line | `matched` | 0 | None |
| 3 | The line holds a marker with `ev=exception` or `ev=panic` | `guest` | 8 | `guest: fault`, with the line |
| 4 | The line holds the runner's `done` marker with `result=config` | `guest` | 8 | `guest: runner`, with the line |
| 5 | The line holds the runner's `done` marker with another result, and `--expect` is given | `ended` | 1 | `run: unmatched` |
| 6 | The same, without `--expect`, and the result is `exit` | `ended` | 0 | None |
| 7 | The same, without `--expect`, and the result is not `exit` | `ended` | 1 | `run: tape`, with the result |
| 8 | The line holds the kernel's `ev=ready` marker, and no `done` marker came before | `guest` | 8 | `guest: runner`, with the line |

Between lines, two more things end the run:

| Event | Outcome | Exit | Error |
|---|---|---|---|
| The time limit passes | `timeout` | 6 | `run: timeout` |
| QEMU exits or a connection closes | `simulator` | 7 | `sim: died` |

[When a run is over](./SDK_PACKAGE.md#when-a-run-is-over) says how to find a marker in a line. Rule 2 comes before the others, so `--expect` can wait for any line, a marker included. The exit status of the tape is not the exit code of the command: it is `tape_rc` in the `result` line.

```mermaid
flowchart TB
    L["A whole line"] --> P["Write it out"]
    P --> M{"--expect matches?"}
    M -- Yes --> OK["matched, 0"]
    M -- No --> F{"exception or panic?"}
    F -- Yes --> G["guest, 8"]
    F -- No --> D{"runner done?"}
    D -- "result=config" --> G
    D -- "other result" --> E["ended: 1 with --expect,<br/>else 0 for exit, 1 otherwise"]
    D -- No --> R{"kernel ready,<br/>no done before?"}
    R -- Yes --> G
    R -- No --> N["Next line, or the time limit, or QEMU gone"]
```

The `result` line adds:

| Key | Type | Present | Value |
|---|---|---|---|
| `outcome` | string | Always | `matched`, `ended`, `timeout`, `simulator` or `guest`. `not_run` when the command failed before `cont`. |
| `board` | string | Always | The board |
| `line` | string | Always | The line that ended the run, by rules 2 to 8. Empty for `timeout`, `simulator` and `not_run`. |
| `tape_result` | string | Always | The `result` of the runner's `done` marker when one came, else empty |
| `tape_rc` | integer | Always | The `rc` of that marker when one came, else 0 |
| `ms` | integer | Always | Milliseconds from `cont` to the end of the run. 0 for `not_run`. |

## Examples

Each block is the whole standard output of one command with `--json`. Hashes are placeholders.

`robco sdk install pipos@1.4 --index index.json --json`, on Linux, with `gcc` already in the cache:

```json
{"v":1,"kind":"step","step":"resolve","item":"","state":"start"}
{"v":1,"kind":"step","step":"resolve","item":"","state":"done"}
{"v":1,"kind":"step","step":"download","item":"pipos@1.4.0","state":"start"}
{"v":1,"kind":"step","step":"download","item":"pipos@1.4.0","state":"done"}
{"v":1,"kind":"step","step":"verify","item":"pipos@1.4.0","state":"start"}
{"v":1,"kind":"step","step":"verify","item":"pipos@1.4.0","state":"done"}
{"v":1,"kind":"step","step":"install","item":"pipos@1.4.0","state":"start"}
{"v":1,"kind":"step","step":"install","item":"pipos@1.4.0","state":"done"}
{"v":1,"kind":"step","step":"install","item":"gcc@15.2.1-1.1/linux-x64","state":"skip"}
{"v":1,"kind":"step","step":"download","item":"qemu@10.1.2/linux-x64","state":"start"}
{"v":1,"kind":"step","step":"download","item":"qemu@10.1.2/linux-x64","state":"done"}
{"v":1,"kind":"step","step":"verify","item":"qemu@10.1.2/linux-x64","state":"start"}
{"v":1,"kind":"step","step":"verify","item":"qemu@10.1.2/linux-x64","state":"done"}
{"v":1,"kind":"step","step":"install","item":"qemu@10.1.2/linux-x64","state":"start"}
{"v":1,"kind":"step","step":"install","item":"qemu@10.1.2/linux-x64","state":"done"}
{"v":1,"kind":"result","command":"sdk install","ok":true,"exit":0,"packages":[{"name":"pipos","version":"1.4.0","state":"installed"}],"tools":[{"name":"gcc","version":"15.2.1-1.1","host":"linux-x64","state":"present"},{"name":"qemu","version":"10.1.2","host":"linux-x64","state":"installed"}]}
```

`robco sdk list --json`, after that:

```json
{"v":1,"kind":"item","type":"package","name":"pipos","version":"1.4.0","host":"","installed":true,"offered":false}
{"v":1,"kind":"item","type":"tool","name":"gcc","version":"15.2.1-1.1","host":"linux-x64","installed":true,"offered":false}
{"v":1,"kind":"item","type":"tool","name":"qemu","version":"10.1.2","host":"linux-x64","installed":true,"offered":false}
{"v":1,"kind":"result","command":"sdk list","ok":true,"exit":0,"count":3}
```

`robco new holotape-c PIPES --json`:

```json
{"v":1,"kind":"result","command":"new","ok":true,"exit":0,"template":"holotape-c","name":"PIPES","path":"PIPES","files":["icon.pbm","robco.toml","src/main.c","tests/golden/start/ready.png","tests/start.toml"]}
```

`robco new holotape-c pipes --json`, which fails:

```json
{"v":1,"kind":"error","code":"new: name","detail":"\"pipes\" holds lower case letters: use A to Z, 0 to 9, _ and -","exit":3}
{"v":1,"kind":"result","command":"new","ok":false,"exit":3}
```

`robco build --json` in that project, with a lock and every package in the cache:

```json
{"v":1,"kind":"step","step":"install","item":"pipos@1.4.0","state":"skip"}
{"v":1,"kind":"step","step":"install","item":"gcc@15.2.1-1.1/linux-x64","state":"skip"}
{"v":1,"kind":"step","step":"compile","item":"crt0","state":"start"}
{"v":1,"kind":"step","step":"compile","item":"crt0","state":"done"}
{"v":1,"kind":"step","step":"compile","item":"src/main.c","state":"start"}
{"v":1,"kind":"step","step":"compile","item":"src/main.c","state":"done"}
{"v":1,"kind":"step","step":"link","item":"","state":"start"}
{"v":1,"kind":"step","step":"link","item":"","state":"done"}
{"v":1,"kind":"step","step":"objcopy","item":"","state":"start"}
{"v":1,"kind":"step","step":"objcopy","item":"","state":"done"}
{"v":1,"kind":"step","step":"seal","item":"","state":"start"}
{"v":1,"kind":"step","step":"seal","item":"","state":"done"}
{"v":1,"kind":"result","command":"build","ok":true,"exit":0,"tape":"build/PIPES.HTP","elf":"build/PIPES.elf","size":2304,"sha256":"0000000000000000000000000000000000000000000000000000000000000001","abi":"1.4","gcc":"15.2.1-1.1","sources":["src/main.c"]}
```

`robco build --json` with a mistake in `src/main.c`:

```json
{"v":1,"kind":"step","step":"install","item":"pipos@1.4.0","state":"skip"}
{"v":1,"kind":"step","step":"install","item":"gcc@15.2.1-1.1/linux-x64","state":"skip"}
{"v":1,"kind":"step","step":"compile","item":"crt0","state":"start"}
{"v":1,"kind":"step","step":"compile","item":"crt0","state":"done"}
{"v":1,"kind":"step","step":"compile","item":"src/main.c","state":"start"}
{"v":1,"kind":"diagnostic","program":"gcc","text":"src/main.c:12:5: error: 'x' undeclared (first use in this function)"}
{"v":1,"kind":"error","code":"build: compile","detail":"src/main.c","exit":5}
{"v":1,"kind":"result","command":"build","ok":false,"exit":5}
```

`robco run --headless --expect "tape: ready" --json`. The lines of the install and of the build are cut here, and so are the serial lines between the first and the last:

```json
{"v":1,"kind":"step","step":"card","item":"","state":"start"}
{"v":1,"kind":"step","step":"card","item":"","state":"done"}
{"v":1,"kind":"step","step":"boot","item":"raspi0","state":"start"}
{"v":1,"kind":"step","step":"boot","item":"raspi0","state":"done"}
{"v":1,"kind":"serial","t_ms":41,"text":"@RC stage=pipos ev=boot midr=0x410fb767 soc=bcm2835 base=0x20000000 board_rev=0x00900092 mem=0x1c000000 entry_mode=svc"}
{"v":1,"kind":"serial","t_ms":902,"text":"@RC stage=runner ev=run tape=PIPES.HTP"}
{"v":1,"kind":"serial","t_ms":931,"text":"tape: ready"}
{"v":1,"kind":"result","command":"run","ok":true,"exit":0,"outcome":"matched","board":"raspi0","line":"tape: ready","tape_result":"","tape_rc":0,"ms":931}
```

`robco run --headless --expect "never" --json`, where the tape returns 3. Only the end is shown:

```json
{"v":1,"kind":"serial","t_ms":2210,"text":"@RC stage=pipos ev=holo_exit status=3"}
{"v":1,"kind":"serial","t_ms":2211,"text":"@RC stage=runner ev=done result=exit rc=3"}
{"v":1,"kind":"error","code":"run: unmatched","detail":"the tape ended and no line held \"never\"","exit":1}
{"v":1,"kind":"result","command":"run","ok":false,"exit":1,"outcome":"ended","board":"raspi0","line":"@RC stage=runner ev=done result=exit rc=3","tape_result":"exit","tape_rc":3,"ms":2211}
```

## Left out of version 1

One line each, so nobody adds them by accident:

- `robco run` without `--headless`, and any window.
- Keys on the command line of `robco run`.
- `robco sdk remove` and `robco sdk doctor`, and an index URL built into the tool.
- Byte counts inside a download, and steps that run at the same time.
- A JSON line for the script channel, and parsed markers as JSON. A caller parses the `text` of a `serial` line.
- Sound, the holotape slot, GPIO lines, a second machine.
