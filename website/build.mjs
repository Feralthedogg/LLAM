/*
 * Copyright 2026 Feralthedogg
 * SPDX-License-Identifier: Apache-2.0
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * See LICENSE.
 */

// Builds what the theme loads from npm packages and third_party/ into
// theme/assets/vendor, which is not committed: the bundles, the fonts, and the
// license of everything in them. A site built without this step still works, in
// system fonts and without the demo, the glass, or skeletons.

import { build } from 'esbuild';
import { copyFileSync, mkdirSync, rmSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const vendor = join(here, 'theme', 'assets', 'vendor');
const modules = join(here, 'node_modules');

rmSync(vendor, { recursive: true, force: true });
mkdirSync(join(vendor, 'fonts'), { recursive: true });
mkdirSync(join(vendor, 'licenses'), { recursive: true });

await build({
  absWorkingDir: here,
  entryPoints: { skullet: 'src/skullet.js', glass: 'src/glass.js', park: 'src/park/mount.tsx' },
  outdir: vendor,
  nodePaths: [modules],
  bundle: true,
  format: 'esm',
  minify: true,
  legalComments: 'none',
  target: ['es2020', 'safari15'],
  jsx: 'automatic',
  define: { 'process.env.NODE_ENV': '"production"' },
  banner: {
    js: '/* Third-party code in this file is licensed as listed in ./licenses/. */',
    css: '/* Third-party code in this file is licensed as listed in ./licenses/. */',
  },
  logLevel: 'info',
});

const fonts = {
  'inter.woff2': '@fontsource-variable/inter/files/inter-latin-wght-normal.woff2',
  'jetbrains-mono.woff2': '@fontsource-variable/jetbrains-mono/files/jetbrains-mono-latin-wght-normal.woff2',
};
for (const [name, source] of Object.entries(fonts)) copyFileSync(join(modules, source), join(vendor, 'fonts', name));

const licenses = {
  'skullet.txt': join(modules, 'skullet/LICENSE'),
  'glassrender.txt': join(modules, 'glassrender/LICENSE'),
  'react.txt': join(modules, 'react/LICENSE'),
  'react-dom.txt': join(modules, 'react-dom/LICENSE'),
  'scheduler.txt': join(modules, 'scheduler/LICENSE'),
  'stageshow.txt': join(here, '..', 'third_party', 'stageshow', 'LICENSE'),
  'inter.txt': join(modules, '@fontsource-variable/inter/LICENSE'),
  'jetbrains-mono.txt': join(modules, '@fontsource-variable/jetbrains-mono/LICENSE'),
};
for (const [name, source] of Object.entries(licenses)) copyFileSync(source, join(vendor, 'licenses', name));
