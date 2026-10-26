---
paths:
  - "shim/**"
  - "logi/**"
  - "cmake/toolchain-mingw32.cmake"
  - "scripts/make/shim*.sh"
  - "scripts/make/logi.sh"
---

# Shim and logi (inside the game process)

- `shim/` proxies `IDirectInput8A/W`, templates over `AnsiTraits` / `WideTraits`. Physical wheel shown as the target
  via `EmulatedDevice<T>` (state from `StateFeed`); game effects → service via `FfbForwarder`; other devices stay
  `ProxyDevice<T>`.
- `DllMain` only `DisableThreadLibraryCalls` + the detach hook; logging starts lazily; no threads; every line flushed;
  nothing throws into the game (`guarded()` / `guardedOr()`); singletons never destroyed; link statically, never link
  `dinput8`.

32-bit shim (needs `pacman -S mingw-w64-i686-gcc` once):

```bash
PATH="/mingw32/bin:$PATH" cmake -S . -B build-x86 -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw32.cmake -DWE_BUILD_SERVICE=OFF -DWE_BUILD_TESTS=OFF -DWE_BUILD_LOGI=OFF
PATH="/mingw32/bin:$PATH" cmake --build build-x86 --target wheelbri_shim
```
