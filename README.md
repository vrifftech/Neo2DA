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

## Build

This repository consumes shared code from the separate `neoshared` repository. Clone the repositories as siblings:

```text
workspace/
  neoshared/
  Neo2DA/
```

CMake automatically detects `../neoshared`. For another layout, pass `--neoshared-root /path/to/neoshared` to `build.sh`, `-NeoSharedRoot C:\path\to\neoshared` to `build.ps1`, or set `NEOSHARED_ROOT` directly.


Linux GUI build:

```sh
./scripts/build.sh --wx ON --require-wx ON --jobs "$(nproc)"
```

Linux CLI/core-only build:

```sh
./scripts/build.sh --wx OFF --jobs "$(nproc)"
```

Windows GUI build with the shared, pinned wxWidgets 3.3.3 overlay:

```powershell
& ..\neoshared\scripts\install-wxwidgets.ps1 `
  -VcpkgRoot C:\vcpkg `
  -Triplet x64-windows-static `
  -CleanAfterBuild

.\scripts\build.ps1 `
  -Wx ON `
  -RequireWx ON `
  -VcpkgRoot C:\vcpkg `
  -VcpkgTriplet x64-windows-static `
  -Parallel ([Environment]::ProcessorCount)
```

Use `-Wx OFF` on Windows for a CLI/core-only build. The default build directory is `build/`.

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
