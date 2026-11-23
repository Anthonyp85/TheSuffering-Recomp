# Third-Party Licenses & Notices

This project's original wrapper, launcher, Dear ImGui settings overlay, hooks, icon generator, resource definitions, build scripts, and verification tools are licensed under the **GNU General Public License v2.0 or later (`GPL-2.0-or-later`)** — see [LICENSE](LICENSE).

**Copyright & Asset Notice:** This repository and its release packages contain **zero** copyrighted game assets, Xbox disc images (`.iso` / `.xiso.iso`), Xbox executables (`.xbe`), or game-derived recompiled code (`suffering.dll` / `libsuffering.so` / `generated/`). Users must supply their own legally acquired Original Xbox copy of *The Suffering (USA)*.

All open-source third-party components linked into or bundled with this project are compatible with `GPL-2.0-or-later`. Their respective license texts and notices are preserved below.

---

## 1. XboxXBERecomp SDK (Pinned Commit `cc9edda3a821a4204dc28783f429b88a27ade4f9`)

- **Upstream:** https://github.com/TGP482/XboxXBERecomp
- **License:** MIT License (`XboxXBERecomp/LICENSE`, shipped as `SDK-LICENSE.txt` in release packages)

```text
MIT License

Copyright (c) 2026 TGP482

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

---

## 2. Zydis (`v4.1.1`)

- **Upstream:** https://github.com/zyantific/zydis
- **Usage:** Statically linked into `compiler\xbe_recomp.exe` and `xbe_symbols.exe`
- **License:** MIT License

```text
The MIT License (MIT)

Copyright (c) 2014-2024 Florian Bernd
Copyright (c) 2014-2024 Joel Höner

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

---

## 3. Zycore (Bundled with Zydis `v4.1.1`)

- **Upstream:** https://github.com/zyantific/zycore-c
- **Usage:** Statically linked with Zydis in `compiler\xbe_recomp.exe` and `xbe_symbols.exe`
- **License:** MIT License

```text
The MIT License (MIT)

Copyright (c) 2018-2024 Florian Bernd
Copyright (c) 2018-2024 Joel Höner

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

---

## 4. toml++ (`v3.4.0`)

- **Upstream:** https://github.com/marzer/tomlplusplus
- **Usage:** Header-only TOML parser/serializer in `suffering.exe`, `compiler\xbe_recomp.exe`, and `xbe_symbols.exe`
- **License:** MIT License

```text
MIT License

Copyright (c) Mark Gillard <mark.gillard@outlook.com.au>

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
documentation files (the "Software"), to deal in the Software without restriction, including without limitation the
rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the
Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

---

## 5. Dear ImGui (`v1.91.0`)

- **Upstream:** https://github.com/ocornut/imgui
- **Usage:** Statically compiled into `suffering.exe` for the pre-game graphical launcher, in-game settings overlay, and FPS HUD
- **License:** MIT License

```text
The MIT License (MIT)

Copyright (c) 2014-2024 Omar Cornut

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

---

## 6. SDL3 (`release-3.4.16`)

- **Upstream:** https://github.com/libsdl-org/SDL
- **Usage:** Statically linked into `suffering.exe` (Windows: HIDAPI + WGI gamepad input) and `suffering` (Native Linux `x86_64` / Steam Deck: X11/Wayland/Gamescope windowing, Vulkan surface creation, `SDL_Renderer` pre-game launcher, PipeWire/PulseAudio/ALSA audio, evdev + HIDAPI gamepad input, and relative mouse input)
- **License:** zlib License

```text
Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

This software is provided 'as-is', without any express or implied
warranty.  In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required.
2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.
```

---

## 7. LLVM / Clang / LLD (`clang.exe`, `lld-link.exe` on Windows; `clang`, `ld.lld` & `compiler/lib/*.so` on Linux)

- **Upstream:** https://llvm.org (LLVM Project)
- **Usage:** Unmodified LLVM `clang` and `lld` binaries bundled in `compiler/` of end-user Windows and Linux release packages for local first-launch compilation of the player's own disc executable into `suffering.dll` (Windows) or `libsuffering.so` (Linux). On Linux, non-LSB shared dependencies (`libLLVM-*.so*`, `libclang-cpp.so*`, etc.) and their permissive Ubuntu 22.04 runtime support libraries (`libxml2` [MIT], `icu` [Unicode/ICU], `liblzma` [Public Domain / 0BSD], `libedit` [BSD-3-Clause], `libtinfo` [MIT/X11], `libffi` [MIT], `libbsd` [BSD], `libmd` [BSD], `zlib` [zlib]) are bundled in `compiler/lib/` with `$ORIGIN:$ORIGIN/lib` RPATH for self-contained execution on stock SteamOS 3.x.
- **License:** **Apache License v2.0 with LLVM Exceptions (`Apache-2.0 WITH LLVM-exception`)** — includes the GPLv2 linking compatibility exception. The full LLVM license text is bundled in `compiler/LLVM-LICENSE.txt` (`XboxXBERecomp/tools/LLVM-LICENSE.txt`), and the complete verbatim copyright and license notices for every bundled `compiler/lib/*.so` package are automatically included in `compiler/lib/THIRD_PARTY_LIBS.txt` inside the Linux release archive.

---

## 8. Microsoft Visual C++ Runtime Libraries

- **Provider:** Microsoft Corporation (Visual Studio Redistributable CRT)
- **Usage:** App-local `msvcp140.dll`, `vcruntime140.dll`, and `vcruntime140_1.dll` bundled in end-user release packages alongside `suffering.exe` and `compiler\xbe_recomp.exe` under Microsoft's Visual Studio redistributable terms (and the GPLv2 §3 System Library exception).
- **Alternative:** Users may also delete these app-local CRT DLLs if Microsoft's official **Visual C++ 2015–2022 Redistributable (x64)** is installed system-wide.

---

## 9. AMD FidelityFX Super Resolution 1.0 (FSR 1.0 EASU + RCAS) & FidelityFX CAS

- **Upstream:** https://github.com/GPUOpen-Effects/FidelityFX-FSR and https://github.com/GPUOpen-Effects/FidelityFX-CAS
- **Usage:** Built-in zero-DLL HLSL compute/pixel upscaling and sharpening algorithms implemented in `src/upscale/upscaler_spatial.cpp`
- **License:** MIT License

```text
MIT License

Copyright (c) 2017-2024 Advanced Micro Devices, Inc. All rights reserved.

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
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

---

## 10. AMD FidelityFX API (`ffx_api`) C-ABI Structures (SDK `v2.4.0`)

- **Upstream:** https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK
- **Usage:** Permissive C-ABI header definitions (`src/upscale/ffx_abi.h`) used for dynamic `LoadLibraryExW` integration with AMD FidelityFX SDK `v2.4.0` signed runtime DLLs (`plugins\amd_fidelityfx_loader_dx12.dll` + `plugins\amd_fidelityfx_upscaler_dx12.dll`). See **Section 13.2** below for the AMD Binary Redistribution License governing the signed runtime DLLs.
- **License (`ffx_api` headers):** MIT License

```text
MIT License

Copyright (c) 2024 Advanced Micro Devices, Inc. All rights reserved.

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
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

---

## 11. NVIDIA Image Scaling (`NIS` v1.0.3)

- **Upstream:** https://github.com/NVIDIAGameWorks/NVIDIAImageScaling
- **Usage:** Built-in zero-DLL 6-tap directional scaling and unsharp-mask sharpening shader implemented in `src/upscale/upscaler_spatial.cpp`
- **License:** MIT License

```text
The MIT License(MIT)

Copyright(c) 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files(the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and / or sell copies of
the Software, and to permit persons to whom the Software is furnished to do so,
subject to the following conditions :

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

---

## 12. Intel XeSS C-ABI Interoperability Declarations (`xess_abi.h`)

- **Upstream Reference:** https://github.com/intel/xess
- **Usage:** Clean-room C-ABI interoperability declarations (`src/upscale/xess_abi.h`, licensed under `GPL-2.0-or-later`) used for dynamic `LoadLibraryExW` / `GetProcAddress` integration with Intel's signed `plugins\libxess.dll` runtime library. Zero proprietary Intel SDK source headers are copied into this repository. See **Section 13.3** below for the Intel Simplified Software License governing `plugins\libxess.dll`.

---

## 13. Bundled & Dynamically Loaded Vendor Temporal Upscaler Plugins (`plugins\`)

- **Repository vs. End-User Release Policy:** **Zero** `.dll` binaries are stored in this Git source repository or in `source.zip`. End-user Win64 binary packages (`win64-launcher.zip`, built by `release.bat` via `tools/fetch_upscalers.py`) bundle the four unmodified, Authenticode-signed, watermark-free official vendor plugin DLLs and their complete vendor license/notice texts inside `plugins\` under the **Runtime Upscaler Plugin Exception** in [LICENSE](LICENSE).
- **Dynamic Runtime Loading & Automatic Fallback:** `suffering.exe` loads vendor plugin DLLs exclusively at runtime via `LoadLibraryExW` / `GetProcAddress`. Users may update or delete any DLL in `plugins\` at any time. If a vendor DLL is absent or unsupported on the active GPU, the engine automatically falls back to built-in **AMD FSR 1.0 (EASU + RCAS)**.

### 13.1 NVIDIA DLSS Super Resolution (`v310.9.16`, `plugins\nvngx_dlss.dll`)

- **Upstream:** https://github.com/NVIDIA/DLSS (tag `v310.9.16`, `lib/Windows_x86_64/rel/nvngx_dlss.dll`)
- **License:** NVIDIA RTX SDKs License Agreement & Supplement (version March 14, 2024) — full license text bundled in `plugins\NVNGX-DLSS-LICENSE.txt`.
- **Clean-Room Host Interop & NGX Driver Fallback:** `src/upscale/ngx_abi.h` and `src/upscale/upscaler_dlss.cpp` use clean-room C-ABI interoperability declarations with zero NVIDIA SDK source code modified or distributed. On NVIDIA RTX systems where `plugins\nvngx_dlss.dll` is absent (such as local source builds), `suffering.exe` can also automatically locate and stage an Authenticode-verified, watermark-free production DLSS model from the user's installed NVIDIA NGX driver cache (`%ProgramData%\NVIDIA\NGX\models\dlss\`) into `cache\ngx\nvngx_dlss.dll`.
- **Trademark Attribution:** NVIDIA, GeForce RTX, and DLSS are trademarks and/or registered trademarks of NVIDIA Corporation in the U.S. and other countries.

### 13.2 AMD FidelityFX SDK Prebuilt Signed Libraries (`v2.4.0`, `plugins\amd_fidelityfx_loader_dx12.dll` `v2.3.0.4555` & `plugins\amd_fidelityfx_upscaler_dx12.dll` `v4.1.2.4555`)

- **Upstream:** https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK (tag `v2.4.0`, `Kits/FidelityFX/signedbin/`)
- **Full License File:** Bundled in `plugins\AMD-FIDELITYFX-LICENSE.md` (AMD Binary Redistribution License for signed binaries + MIT License for `ffx_api` source/headers).

```text
Copyright (C)  Advanced Micro Devices, Inc. All rights reserved. 
 
REDISTRIBUTION: Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the “Software”), to install, reproduce, copy and distribute copies of the Software, in binary form only, and to permit persons to whom the Software is provided to do the same, provided that the following conditions are met: 
 
No reverse engineering, decompilation, or disassembly of this Software is permitted. 
 
Redistributions must reproduce the above copyright notice, this permission notice, and the following disclaimers and notices in the Software documentation and/or other materials provided with the Software. 
 
DISCLAIMER: THE USE OF THE SOFTWARE IS AT YOUR SOLE RISK.  THE SOFTWARE 
IS PROVIDED "AS IS" AND WITHOUT WARRANTY OF ANY KIND AND COPYRIGHT 
HOLDER AND ITS LICENSORS EXPRESSLY DISCLAIM ALL WARRANTIES, EXPRESS AND 
IMPLIED, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF 
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. 
COPYRIGHT HOLDER AND ITS LICENSORS DO NOT WARRANT THAT THE SOFTWARE WILL 
MEET YOUR REQUIREMENTS, OR THAT THE OPERATION OF THE SOFTWARE WILL BE 
UNINTERRUPTED OR ERROR-FREE.  THE ENTIRE RISK ASSOCIATED WITH THE USE OF 
THE SOFTWARE IS ASSUMED BY YOU.  FURTHERMORE, COPYRIGHT HOLDER AND ITS 
LICENSORS DO NOT WARRANT OR MAKE ANY REPRESENTATIONS REGARDING THE USE 
OR THE RESULTS OF THE USE OF THE SOFTWARE IN TERMS OF ITS CORRECTNESS, 
ACCURACY, RELIABILITY, CURRENTNESS, OR OTHERWISE. 
 
DISCLAIMER: UNDER NO CIRCUMSTANCES INCLUDING NEGLIGENCE, SHALL COPYRIGHT HOLDER AND ITS LICENSORS OR ITS DIRECTORS, OFFICERS, EMPLOYEES OR AGENTS ("AUTHORIZED REPRESENTATIVES") BE LIABLE FOR ANY INCIDENTAL, INDIRECT, 
SPECIAL OR CONSEQUENTIAL DAMAGES (INCLUDING DAMAGES FOR LOSS OF BUSINESS 
PROFITS, BUSINESS INTERRUPTION, LOSS OF BUSINESS INFORMATION, AND THE 
LIKE) ARISING OUT OF THE USE, MISUSE OR INABILITY TO USE THE SOFTWARE, 
BREACH OR DEFAULT, INCLUDING THOSE ARISING FROM INFRINGEMENT OR ALLEGED 
INFRINGEMENT OF ANY PATENT, TRADEMARK, COPYRIGHT OR OTHER INTELLECTUAL 
PROPERTY RIGHT EVEN IF COPYRIGHT HOLDER AND ITS AUTHORIZED 
REPRESENTATIVES HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGES.  IN 
NO EVENT SHALL COPYRIGHT HOLDER OR ITS AUTHORIZED REPRESENTATIVES TOTAL 
LIABILITY FOR ALL DAMAGES, LOSSES, AND CAUSES OF ACTION (WHETHER IN 
CONTRACT, TORT (INCLUDING NEGLIGENCE) OR OTHERWISE) EXCEED THE AMOUNT OF 
US$10.  
```

### 13.3 Intel(R) Xe Super Sampling (`XeSS SDK v3.0.2`, `plugins\libxess.dll` `v2.0.2.68`)

- **Upstream:** https://github.com/intel/xess (release `v3.0.2`, `XeSS_SDK_3.0.2.zip`)
- **Full License & Third-Party Notice Files:** Bundled in `plugins\INTEL-XESS-LICENSE.txt` and `plugins\INTEL-XESS-THIRD-PARTY.txt`.

```text
Intel Simplified Software License (Version October 2022)

Intel(R) Xe Super Sampling (XeSS) SDK: Copyright (C) 2025 Intel Corporation

Use and Redistribution. You may use and redistribute the software, which is provided in binary form only, (the "Software"), without modification, provided the following conditions are met:

* 	Redistributions must reproduce the above copyright notice and these terms of use in the Software and in the documentation and/or other materials provided with the distribution.
* 	Neither the name of Intel nor the names of its suppliers may be used to endorse or promote products derived from this Software without specific prior written permission.
* 	No reverse engineering, decompilation, or disassembly of the Software is permitted, nor any modification or alteration of the Software or its operation at any time, including during execution.

No other licenses. Except as provided in the preceding section, Intel grants no licenses or other rights by implication, estoppel or otherwise to, patent, copyright, trademark, trade name, service mark or other intellectual property licenses or rights of Intel.

Third party software.  "Third Party Software" means the files (if any) listed in the "third-party-software.txt" or other similarly-named text file that may be included with the Software. Third Party Software, even if included with the distribution of the Software, may be governed by separate license terms, including without limitation, third party license terms, open source software notices and terms, and/or other Intel software license terms. These separate license terms solely govern Your use of the Third Party Software.

DISCLAIMER. THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, AND NON-INFRINGEMENT ARE DISCLAIMED. THIS SOFTWARE IS NOT INTENDED FOR USE IN SYSTEMS OR APPLICATIONS WHERE FAILURE OF THE SOFTWARE MAY CAUSE PERSONAL INJURY OR DEATH AND YOU AGREE THAT YOU ARE FULLY RESPONSIBLE FOR ANY CLAIMS, COSTS, DAMAGES, EXPENSES, AND ATTORNEYS' FEES ARISING OUT OF ANY SUCH USE, EVEN IF ANY CLAIM ALLEGES THAT INTEL WAS NEGLIGENT REGARDING THE DESIGN OR MANUFACTURE OF THE SOFTWARE.

LIMITATION OF LIABILITY. IN NO EVENT WILL INTEL BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

No support.  Intel may make changes to the Software, at any time without notice, and is not obligated to support, update or provide training for the Software.

Termination. Your right to use the Software is terminated in the event of your breach of this license.

Feedback.  Should you provide Intel with comments, modifications, corrections, enhancements or other input (“Feedback”) related to the Software, Intel will be free to use, disclose, reproduce, license or otherwise distribute or exploit the Feedback in its sole discretion without any obligations or restrictions of any kind, including without limitation, intellectual property rights or licensing obligations.

Compliance with laws.  You agree to comply with all relevant laws and regulations governing your use, transfer, import or export (or prohibition thereof) of the Software.

Governing law.  All disputes will be governed by the laws of the United States of America and the State of Delaware without reference to conflict of law principles and subject to the exclusive jurisdiction of the state or federal courts sitting in the State of Delaware, and each party agrees that it submits to the personal jurisdiction and venue of those courts and waives any objections. THE UNITED NATIONS CONVENTION ON CONTRACTS FOR THE INTERNATIONAL SALE OF GOODS (1980) IS SPECIFICALLY EXCLUDED AND WILL NOT APPLY TO THE SOFTWARE.
```

---

## 14. Native Linux `x86_64` Vulkan & Compression Components (`v1.3.0`)

### 14.1 Khronos Vulkan Headers & `glslang` / `SPIRV-Tools`
- **Upstream:** https://github.com/KhronosGroup/Vulkan-Headers, https://github.com/KhronosGroup/glslang, https://github.com/KhronosGroup/SPIRV-Tools
- **Usage:** Dynamically loaded Vulkan 1.2 entry points (`VK_NO_PROTOTYPES` via `SDL_Vulkan_GetVkGetInstanceProcAddr`) and statically linked `glslang` + `SPIRV-Tools` libraries in the Native Linux `x86_64` `./suffering` binary for runtime HLSL-to-SPIR-V shader translation (Xbox fixed-function/NV2A shaders and all 6 built-in spatial upscalers).
- **License:** **Apache License v2.0** (Vulkan Headers, SPIRV-Tools) and **BSD-3-Clause / Apache-2.0 / Khronos permissive licenses** (`glslang`).

### 14.2 zlib
- **Upstream:** https://zlib.net
- **Usage:** Statically linked into the Native Linux `x86_64` `./suffering` binary for PNG icon decoding (`suffering.png`) and custom texture dump/replacement PNG encoding/decoding.
- **License:** **zlib License** (permissive, GPL-compatible).
