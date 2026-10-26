# Development

Run all commands from the repository root.

Run local checks with:

```sh
pre-commit run --all-files
```

Run the unit tests (native C++ decoder tests, protocol tests, and — with
`esphome` installed — config validation) with:

```sh
pytest tests/unit
```

Compile the minimal fixture (nRF24/HM) and the HMS one (CMT2300A, no `spi:`
bus) with:

```sh
esphome compile tests/components/hoymiles_dtu/test.minimal.yaml
esphome compile tests/components/hoymiles_dtu/test.hms-cmt.yaml
```

Format the C++ sources, and lint them with clang-tidy using ESPHome's
`.clang-tidy`:

```sh
clang-format -i components/*/*.{h,cpp}
scripts/clang-tidy.py          # add --fix to apply fix-its, --no-compile to reuse builds
```

`scripts/clang-tidy.py` compiles the ESP-IDF fixtures (`test.esp32-idf.yaml`
and `test.hms-cmt-idf.yaml`) to get real compiler flags, then runs clang-tidy
on the repo sources. It needs `esphome` and clang-tidy 22 (the version ESPHome
pins) on the `PATH`. It uses `clang-tidy-22` if present, otherwise
`clang-tidy` with a warning when that is another version. `--clang-tidy PATH`
overrides it.

Hardware-in-the-loop checklists live in `tests/manual/`:
[`hm4_readonly/`](../tests/manual/hm4_readonly/README.md) for HM telemetry and
[`hms_cmt/`](../tests/manual/hms_cmt/README.md) for the unverified HMS path.
