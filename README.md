# Neo2DA

Neo2DA v1.1.0 is a standalone C++17 editor for BioWare `2DA` tables and Dragon Age `GDA` / `G2DA` tables. It includes a command-line tool and an optional wxWidgets desktop GUI.


Compatible Games:
```
Neverwinter Nights 
Neverwinter Nights Enhanced Edition
Neverwinter Nights 2
Neverwinter Nights 2 Enhanced Edition
Star Wars: Knights of the Old Republic
Star Wars: Knights of the Old Republic 2: The Sith Lords
Jade Empire
The Witcher 1
The Witcher 1 Enhanced Edition
Jade Empire
Dragon Age: Origins
Dragon Age
```


## Format support

- Binary `2DA V2.b`.
- Text `2DA V2.0`, including tab-delimited and whitespace/fixed-width variants.
- Dragon Age `GDA` / `G2DA` files stored as `GFF V4.0PC` with `G2DA V0.1` or `G2DA V0.2` data.
- Native save preserves the physical format: text 2DAs save as text, binary 2DAs save as binary, and GDAs save as GFF4/G2DA.
- CSV and TSV import/export for spreadsheet workflows.
- TSLPatcher/HoloPatcher diff output for representable 2DA/GDA changes.

## Requirements

CLI-only builds require:

- CMake 3.16 or newer.
- A C++17 compiler.

GUI builds also require wxWidgets with `core`, `base`, and `adv` components.

On Debian/Ubuntu-style Linux systems:

```sh
sudo apt update
sudo apt install -y build-essential cmake ninja-build
sudo apt install -y pkg-config libwxgtk3.2-dev   # GUI builds only
```

On Windows, use Visual Studio 2022 Build Tools or newer. For the GUI, install wxWidgets yourself or through vcpkg, for example:

```powershell
C:\vcpkg\vcpkg.exe install wxwidgets:x64-windows-static
```

## Build on Linux

From the Neo2DA directory:

```sh
./scripts/build.sh --wx OFF --jobs "$(nproc)"
```

Build the GUI when wxWidgets is installed:

```sh
./scripts/build.sh --wx ON --require-wx ON --jobs "$(nproc)"
```

Clean and rebuild:

```sh
./scripts/build.sh --clean --wx OFF --jobs "$(nproc)"
```

The default output directory is:

```text
build/
```

## Build on Windows

From the Neo2DA directory:

```powershell
.\scripts\build.ps1 -Wx OFF -Parallel ([Environment]::ProcessorCount)
```

Build the GUI with vcpkg wxWidgets:

```powershell
.\scripts\build.ps1 `
  -Wx ON `
  -RequireWx ON `
  -VcpkgRoot C:\vcpkg `
  -VcpkgTriplet x64-windows-static `
  -Parallel ([Environment]::ProcessorCount)
```

Clean and rebuild:

```powershell
.\scripts\build.ps1 -Clean -Wx OFF -Parallel ([Environment]::ProcessorCount)
```

## Common CLI commands

```sh
neo2da-cli --version
neo2da-cli info appearance.2da
neo2da-cli dump appearance.2da
neo2da-cli export appearance.2da csv appearance.csv
neo2da-cli import appearance.csv csv appearance.2da
neo2da-cli set-cell appearance.2da appearance.2da row:0 col:label Soldier
```

Dragon Age GDA examples:

```sh
neo2da-cli info table.gda
neo2da-cli dump table.gda
```

## Dragon Age GDA column-name hashes

Dragon Age `G2DA V0.2` tables often store column labels as 32-bit hashes instead of readable strings. Neo2DA resolves labels in this order:

1. Names embedded directly in `G2DA V0.1` columns.
2. Built-in common labels.
3. Dictionary files named `gda_column_names.tsv` or `gda_column_names.csv` beside the `.gda`.
4. Dictionary files in the current working directory, `resources/gda_column_names.tsv`, or paths listed in `NEO2DA_GDA_COLUMN_NAMES` separated by semicolons.
5. A stable fallback label such as `hash_0F642429`.

Dictionary file entries can use tabs, commas, or whitespace:

```text
0x66FBC936    ID
hash_F333FE09 script
B22D0123      name
```

Generate entries from known column names:

```sh
neo2da-cli gda-hash ID COUNT script label worksheet
```

Neo2DA preserves unresolved column hashes on save unless the user explicitly renames the column.
