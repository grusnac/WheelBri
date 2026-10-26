# tests/

Loaded when working in `tests/`; the root CLAUDE.md still applies.

- GoogleTest with fakes (see `PipelineTest.cpp`) + `wheelbri_ui_tests` (scripted window).
- Windows-only code: tested in the Windows binary, or under Wine from Linux.
- Fuzz targets (`tests/fuzz/FuzzTargets.cpp`): `RobustnessTest` feeds fixed inputs every build, `wheelbri_fuzz`
  searches longer. `WE_BUILD_FUZZ` is Clang only (`CXX=clang++`); the whole build gets address + UB checks. Run:

```bash
build/tests/wheelbri_fuzz -max_total_time=600 <corpus folder>
```

- Tests running a `wheelbri-uhid` take `UhidTestLock` (`ctest -j` must not run two at once).
