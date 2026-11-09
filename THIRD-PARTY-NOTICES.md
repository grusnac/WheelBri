# Third-party notices

WheelBri is free software under the GNU General Public License version 3 or later (see [LICENSE](LICENSE)). It
contains or is derived from the following works under their own licences, whose notices are kept here as they ask.

## spdlog (compiled into WheelBri's programs)

<https://github.com/gabime/spdlog>, version 1.15.3.

```text
The MIT License (MIT)

Copyright (c) 2016 Gabi Melman.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

## {fmt} (bundled in spdlog)

<https://github.com/fmtlib/fmt>. Copyright (c) 2012 - present, Victor Zverovich and {fmt} contributors. MIT licence
(the text above), with its exception that portions embedded in compiled programs need no notice; named here anyway.

## HIDMaestro (the virtual wheel's HID descriptor)

<https://github.com/hifihedgehog/HIDMaestro>. The HID report descriptor in `core/src/HidWheelDevice.cpp` was generated
with its `HidDescriptorBuilder` and has been changed since.

```text
MIT License

Copyright (c) 2026 HIDMaestro Contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Windows-driver-samples (the virtual wheel driver's structure)

<https://github.com/microsoft/Windows-driver-samples>. `driver/Driver.cpp` follows the layout of its `vhidmini2` UMDF 2
sample (HID class requests as IOCTLs on a parallel queue, reads waiting on a manual queue).

```text
MIT License

Copyright (c) Microsoft Corporation.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Microsoft's Update for Universal C Runtime in Windows, KB2999226 (Windows installer package only)

`Setup.exe` and the `.zip` carry Microsoft's own, unmodified and signed update packages for 64-bit Windows 7 SP1, 8
and 8.1 (`ucrt\Windows6.1-KB2999226-x64.msu`, `ucrt\Windows8-RT-KB2999226-x64.msu`,
`ucrt\Windows8.1-KB2999226-x64.msu`), downloaded from Microsoft's Download Center (ids 49093, 49082, 49081). The
installer runs the one for that Windows with Windows' own `wusa.exe` when the Universal C Runtime is missing. They
are Microsoft's software under Microsoft's terms, not WheelBri's, and not under the GPL. Copyright (c) Microsoft
Corporation.

## GoogleTest (tests only, not shipped)

<https://github.com/google/googletest>, version 1.15.2, BSD 3-Clause licence. Used to build and run the tests; no part
of it is in a release package.
