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

// Draws each Mermaid fence in docs/ to <hash>.svg in this folder, using a local
// Chrome. The site ships these drawings instead of the Mermaid runtime. Colors are
// drawn as placeholders and swapped for theme variables, so one drawing serves the
// light and dark themes.

import { chromium } from 'playwright-core';
import { createHash } from 'node:crypto';
import { readFileSync, readdirSync, rmSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const docs = join(here, '..', '..', 'docs');

// Each placeholder stands for one theme token
const INK = '#010101';
const INK2 = '#020202';
const LINE = '#030303';
const RULE = '#040404';
const SOFT = '#050505';
const PAGE = '#060606';
const ACCENT = '#070707';
const TOKENS = new Map([
  [INK, 'var(--fg)'],
  [INK2, 'var(--fg2)'],
  [LINE, 'var(--fg3)'],
  [RULE, 'var(--rule-diagram)'],
  [SOFT, 'var(--soft)'],
  [PAGE, 'var(--bg)'],
  [ACCENT, 'var(--sel)'],
]);

const NOTICE = `<!-- Copyright 2026 Feralthedogg
     SPDX-License-Identifier: Apache-2.0
     Licensed under the Apache License, Version 2.0. See LICENSE. -->
`;

const THEME = {
  fontFamily: 'Inter, system-ui, sans-serif',
  fontSize: '14px',
  darkMode: false,
  background: PAGE,
  primaryColor: SOFT,
  primaryTextColor: INK,
  primaryBorderColor: RULE,
  secondaryColor: SOFT,
  secondaryTextColor: INK,
  secondaryBorderColor: RULE,
  tertiaryColor: PAGE,
  tertiaryTextColor: INK,
  tertiaryBorderColor: RULE,
  lineColor: LINE,
  textColor: INK,
  mainBkg: SOFT,
  nodeBorder: RULE,
  nodeTextColor: INK,
  clusterBkg: PAGE,
  clusterBorder: RULE,
  titleColor: INK2,
  defaultLinkColor: LINE,
  edgeLabelBackground: PAGE,
  actorBkg: SOFT,
  actorBorder: RULE,
  actorTextColor: INK,
  actorLineColor: RULE,
  signalColor: LINE,
  signalTextColor: INK,
  labelBoxBkgColor: SOFT,
  labelBoxBorderColor: RULE,
  labelTextColor: INK,
  loopTextColor: INK2,
  noteBkgColor: ACCENT,
  noteBorderColor: ACCENT,
  noteTextColor: INK,
  activationBkgColor: ACCENT,
  activationBorderColor: LINE,
  sequenceNumberColor: PAGE,
};

function markdownFiles(folder) {
  return readdirSync(folder, { withFileTypes: true }).flatMap((entry) => {
    if (entry.isDirectory()) return markdownFiles(join(folder, entry.name));
    return entry.name.endsWith('.md') ? [join(folder, entry.name)] : [];
  });
}

const sources = markdownFiles(docs).flatMap((file) =>
  [...readFileSync(file, 'utf8').matchAll(/^```mermaid\n([\s\S]*?)^```/gm)].map((match) => match[1].trim()));

for (const file of readdirSync(here)) {
  if (file.endsWith('.svg')) rmSync(join(here, file));
}

const font = readFileSync(join(here, '..', 'theme', 'assets', 'vendor', 'fonts', 'inter.woff2')).toString('base64');
const browser = await chromium.launch({ channel: 'chrome', headless: true });
const page = await browser.newPage();
await page.setContent('<!doctype html><title>diagrams</title><body></body>');
await page.addStyleTag({ content: `@font-face { font-family: Inter; src: url(data:font/woff2;base64,${font}); font-weight: 100 900; }` });
await page.addScriptTag({ path: join(here, 'node_modules', 'mermaid', 'dist', 'mermaid.min.js') });
await page.evaluate(() => document.fonts.load('14px Inter'));

for (const source of sources) {
  const key = createHash('sha256').update(source).digest('hex').slice(0, 16);
  let svg = await page.evaluate(async ([text, id, themeVariables]) => {
    window.mermaid.initialize({
      startOnLoad: false,
      securityLevel: 'loose',
      theme: 'base',
      themeVariables,
      flowchart: { htmlLabels: false, nodeSpacing: 16, rankSpacing: 40, wrappingWidth: 170, padding: 8, subGraphTitleMargin: { top: 6, bottom: 8 } },
      sequence: { useMaxWidth: true, actorMargin: 24, width: 104, height: 44, messageMargin: 28, boxMargin: 6, diagramMarginX: 8, diagramMarginY: 8 },
    });
    return (await window.mermaid.render(id, text)).svg;
  }, [source, `d-${key}`, THEME]);
  for (const [placeholder, token] of TOKENS) svg = svg.replaceAll(placeholder, token);
  writeFileSync(join(here, `${key}.svg`), `${NOTICE}${svg}\n`);
  console.log(`${key}.svg`, svg.length);
}
await browser.close();
