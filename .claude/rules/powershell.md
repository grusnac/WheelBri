---
paths:
  - "**/*.ps1"
  - "installer/**"
---

# PowerShell and the installer

- Target PowerShell 5.1: wrap function results in `@(...)` before `.Count`.
- Windows package: `scripts/make/package.ps1` builds it in `dist/windows/` (IExpress).
