# Anatomica Menu (SKSE Menu Framework 3 plugin)

In-game settings page for **Anatomica** under *SKSE Menu Framework -> Anatomica*:

* **Gape** - every setting of the after-sex gape script (enable, gape after/during sex,
  seconds to dilate, hold, close, per-partner multiplier, detection radius, idle check
  interval). Changes apply instantly (they write the AnatomicaGape.esp globals) and are
  stored in `Data/SKSE/Plugins/AnatomicaMenu.ini`, so they carry over to every save.
* **Jiggle** - chest / belly / butt strength multipliers for the Anatomica CBPC option.
  *Apply* rewrites `CBPConfig_Anatomica_Male*.txt` from the base files in
  `SKSE/Plugins/AnatomicaMenu/JiggleBase` (CBPC reads them on game start).

Requires SKSE, SKSE Menu Framework 3, Address Library, and AnatomicaGape.esp for the gape page.

Based on the MIT-licensed SKSE-Menu-Framework-3-Example template by Thiago Kaique.

## Building (Windows)

1. Install Visual Studio 2022 with *Desktop development with C++*, plus CMake and Ninja
   (both come with VS), and Git.
2. Install vcpkg and set the `VCPKG_ROOT` environment variable to its folder:
   ```
   git clone https://github.com/microsoft/vcpkg C:\vcpkg
   C:\vcpkg\bootstrap-vcpkg.bat
   setx VCPKG_ROOT C:\vcpkg
   ```
3. Get CommonLibSSE-NG and point `COMMONLIB_SSE_FOLDER` at it:
   ```
   git clone --recursive https://github.com/alandtse/CommonLibVR C:\dev\CommonLibVR
   cd C:\dev\CommonLibVR && git checkout ng
   setx COMMONLIB_SSE_FOLDER C:\dev\CommonLibVR
   ```
4. Open a **x64 Native Tools Command Prompt for VS 2022** in this folder and run:
   ```
   cmake --preset release
   cmake --build build/release
   ```
   (Optional: set `SKYRIM_MODS_FOLDER` to your MO2 `mods` folder first and the build is
   copied into a mod folder automatically.)
5. The DLL is `build/release/AnatomicaMenu.dll`. Ship it as
   `SKSE/Plugins/AnatomicaMenu.dll` together with the `public/SKSE` folder contents.
