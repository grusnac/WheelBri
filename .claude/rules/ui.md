---
paths:
  - "service/src/Ui*"
  - "service/src/*View*"
  - "service/src/SessionUi.hpp"
  - "service/src/PaneWindow.hpp"
  - "service/src/App.cpp"
  - "core/**/ShapeSpans*"
  - "tests/ui/**"
  - "tests/Ui*"
  - "tests/*View*"
  - "tests/ShapeSpansTest.cpp"
---

# UI

- UI via `UiWindow.hpp` (`UiWindowWin32.cpp` / `UiWindowX11.cpp`). Views lay out from `UiWindow::width()` /
  `height()` every frame, never 960 / 748.
- Colours only from `style::` (`UiStyle.hpp`); tests compare to `style::`, not literals.
- Shapes (`fillPolygon`, `fillEllipse`, `fillRing`) are rows of `fill()` (core `ShapeSpans`), so every window and fake
  draws them. The Shadow view (`ShadowView.cpp`, the app's Live tab) draws the target with them; every target has a
  picture (`ShadowView::hasPicture`) drawn from Logitech's photos of the real wheel.
- Labels ≤ 20 characters.
- The app's editor and live view draw into `PaneWindow`s (`App::placePanes`): lay out from the window you were given,
  never the app's; clicks reach a view in its own coordinates.
