# Third-party notices

raya itself is licensed under the GNU Lesser General Public License, version 2.1 or (at your option) any later version. See [LICENSE](LICENSE).

The repository contains only raya's own source. `make deps` downloads the libraries below into `deps/`. The web build (`build/index.html`, `index.js`, `index.wasm`) is compiled from raya together with these libraries, so anyone distributing that build distributes them too. Their licenses and notices follow.

## shine: MP3 encoder (LGPL)

- Source: https://github.com/toots/shine, commit `ab5e352`, used unmodified.
- License: GNU Library General Public License, version 2. The full text is in `deps/shine/COPYING` and at https://www.gnu.org/licenses/old-licenses/lgpl-2.0.txt. Because shine does not state a version number, the license lets you choose any version published by the Free Software Foundation, so it can be combined with raya under LGPL-2.1.
- How the LGPL is honoured: shine is compiled as a separate static library (`deps/shine/libshine.a`) and linked into the web build. raya's complete source and build files are in this repository, so anyone can rebuild raya with a modified version of shine: change `deps/shine/src/lib/` and run `make`.

## raylib: zlib/libpng license

```
Copyright (c) 2013-2024 Ramon Santamaria (@raysan5)

This software is provided "as-is", without any express or implied warranty. In no event
will the authors be held liable for any damages arising from the use of this software.

Permission is granted to anyone to use this software for any purpose, including commercial
applications, and to alter it and redistribute it freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not claim that you
  wrote the original software. If you use this software in a product, an acknowledgment
  in the product documentation would be appreciated but is not required.

  2. Altered source versions must be plainly marked as such, and must not be misrepresented
  as being the original software.

  3. This notice may not be removed or altered from any source distribution.
```

raylib 5.5 is used as released. It is built with FLAC decoding switched on (`-DSUPPORT_FILEFORMAT_FLAC=1`), a build option rather than a source change.

### Libraries bundled inside raylib

These come with raylib in `deps/raylib/src/external/`. Each file carries its full license text.

| Library | Author | License |
| --- | --- | --- |
| miniaudio, dr_wav, dr_mp3, dr_flac | David Reid | Public domain (Unlicense) or MIT No Attribution, your choice |
| stb_vorbis, stb_image, stb_image_resize2, stb_image_write, stb_truetype, stb_rect_pack, stb_perlin | Sean Barrett and contributors | Public domain or MIT, your choice |
| qoa, qoi | Copyright (c) 2023 / 2021 Dominic Szablewski | MIT |
| jar_mod | Joshua Reisenauer | Public domain |
| jar_xm | Romain Dalmaso, Dan Spencer, Joshua Reisenauer | WTFPL version 2 |
| cgltf | Copyright (c) 2018-2021 Johannes Kuhlmann (bundled jsmn: Copyright (c) 2010 Serge A. Zaitsev) | MIT |
| tinyobj_loader_c | Copyright (c) 2016-2019 Syoyo Fujita and many contributors | MIT |
| par_shapes | Copyright (c) 2019 Philip Rideout | MIT |
| m3d | Copyright (C) 2020 bzt | MIT |
| vox_loader | Copyright (c) 2021 Johann Nadalutti | MIT |
| msf_gif | Copyright (c) 2021 Miles Fogle | MIT or public domain, your choice |
| sdefl, sinfl | Copyright (c) 2020-2023 Micha Mettke | MIT or public domain, your choice |
| rprand, rl_gputex | Copyright (c) Ramon Santamaria | zlib/libpng |

The MIT License, as it applies to each MIT entry above with that entry's copyright line:

```
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

## Emscripten: MIT (or University of Illinois/NCSA)

The web build contains Emscripten's JavaScript runtime and its GLFW and file-system support (`index.js`).

```
Copyright (c) 2010-2014 Emscripten authors, see AUTHORS file.

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

## Trademarks

Names of drum machines and their makers appear only to describe what a kit is loosely inspired by. raya is not affiliated with, endorsed by or connected to any of these manufacturers, and all trademarks belong to their respective owners.
