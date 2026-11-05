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

// The landing page: install command, sample tabs, the scheduler demo, the latest
// release, and the glass route tiles. Everything here loads after first paint,
// and the page stands without any of it.

import { copyText, loadSkullet, read, reducedMotion, setOS, write } from './site.js';

const $ = (selector, root = document) => root.querySelector(selector);
const $$ = (selector, root = document) => [...root.querySelectorAll(selector)];
const asset = (path) => new URL(path, import.meta.url);
const COPIED_FOR = 1400;
const COPY_ICON = '<svg class="i s" viewBox="0 0 24 24" aria-hidden="true"><rect width="14" height="14" x="8" y="8" rx="2" ry="2"></rect><path d="M4 16c-1.1 0-2-.9-2-2V4c0-1.1.9-2 2-2h10c1.1 0 2 .9 2 2"></path></svg>';
const CHECK_ICON = '<svg class="i s" viewBox="0 0 24 24" aria-hidden="true"><path d="M20 6 9 17l-5-5"></path></svg>';

// Announcement

function initAnnouncement() {
  const bar = $('.ann');
  $('[data-ann-close]')?.addEventListener('click', () => {
    write('llam-ann', bar.dataset.key);
    bar.remove();
  });
}

// Install command

const isWindows = () => document.documentElement.classList.contains('os-win');

function paintOS() {
  const win = isWindows();
  $$('[data-os-pick]').forEach((tab) => {
    const on = (tab.dataset.osPick === 'win') === win;
    tab.classList.toggle('on', on);
    tab.setAttribute('aria-selected', String(on));
  });
}

function pickOS(win) {
  document.documentElement.classList.toggle('os-win', win);
  setOS(win ? 'Windows' : detectPosix());
  paintOS();
}

function detectPosix() {
  return /mac/i.test(navigator.userAgentData?.platform || navigator.platform || '') ? 'macOS' : 'Linux';
}

function installText() {
  const block = $(isWindows() ? '.cmdbox .only-win' : '.cmdbox .only-posix');
  const lines = $$('[data-cmd]', block).map((line) => line.textContent);
  // The POSIX command is one pipeline; PowerShell is two commands
  return isWindows() ? lines.join('\n') : lines.map((line) => line.trim()).join(' ');
}

function initInstall() {
  paintOS();
  $$('[data-os-pick]').forEach((tab) => tab.addEventListener('click', () => pickOS(tab.dataset.osPick === 'win')));
  $('[data-os-toggle]')?.addEventListener('click', () => pickOS(!isWindows()));
  $$('[data-copy-install]').forEach((button) => {
    const icon = $('.cp', button) || button;
    button.addEventListener('click', () => {
      copyText(installText()).then(() => {
        button.dataset.done = '1';
        icon.innerHTML = CHECK_ICON;
        setTimeout(() => {
          delete button.dataset.done;
          icon.innerHTML = COPY_ICON;
        }, COPIED_FOR);
      }, () => {});
    });
  });
}

// Sample tabs

function initPanel() {
  const tabs = $$('#why [role="tab"]');
  const openExample = $('#open-example');
  const play = $('#play');
  const copy = $('#copy-code');
  const caption = $('#why-cap');
  const pick = (tab) => {
    for (const other of tabs) {
      const on = other === tab;
      other.classList.toggle('on', on);
      other.setAttribute('aria-selected', String(on));
      other.tabIndex = on ? 0 : -1;
      $(`#${other.getAttribute('aria-controls')}`).hidden = !on;
    }
    const io = tab.id === 'tab-io';
    openExample.href = tab.dataset.to;
    play.hidden = !io;
    copy.hidden = io;
    caption.textContent = io ? caption.dataset.io : caption.dataset.code;
    demo.onTab(io);
  };
  tabs.forEach((tab, index) => {
    tab.addEventListener('click', () => pick(tab));
    tab.addEventListener('keydown', (event) => {
      const step = { ArrowRight: 1, ArrowLeft: -1 }[event.key];
      if (!step) return;
      event.preventDefault();
      const next = tabs[(index + step + tabs.length) % tabs.length];
      next.focus();
      pick(next);
    });
  });
  $$('#why .fold').forEach((fold) => {
    fold.addEventListener('click', () => {
      fold.nextElementSibling.hidden = false;
      fold.remove();
    });
  });
  copy.addEventListener('click', () => {
    const pane = $('#why .pane:not([hidden])');
    const lines = $$('.ln', pane).map((line) => line.lastElementChild.textContent);
    // A blank line separates what starts folded from what stays open
    const tail = $('.tail', pane);
    const head = tail ? lines.length - $$('.ln', tail).length : lines.length;
    if (tail) lines.splice(head, 0, '');
    const label = $('.lbl', copy);
    copyText(`${lines.join('\n')}\n`).then(() => {
      label.textContent = 'Copied';
      setTimeout(() => { label.textContent = 'Copy'; }, COPIED_FOR);
    }, () => {});
  });
}

// The scheduler demo. It costs nothing until it is seen, and pausing it removes
// it, so the poster stays on screen and the page has no running clock.

const demo = {
  slot: null,
  button: null,
  paused: false,
  visible: false,
  active: true,
  unmount: null,
  mounting: false,

  init() {
    this.slot = $('#park');
    this.button = $('#play');
    this.paused = reducedMotion();
    this.paint();
    this.button.addEventListener('click', () => {
      this.paused = !this.paused;
      this.paint();
      this.sync();
    });
    new IntersectionObserver((entries) => {
      this.visible = entries.at(-1).isIntersecting;
      if (this.visible) this.sync();
    }, { rootMargin: '200px' }).observe(this.slot);
  },

  paint() {
    this.button.setAttribute('aria-pressed', String(this.paused));
    $('.lbl', this.button).textContent = this.paused ? 'Play' : 'Pause';
    $('.ic-pause', this.button).hidden = this.paused;
    $('.ic-play', this.button).hidden = !this.paused;
  },

  onTab(active) {
    this.active = active;
    this.sync();
  },

  async sync() {
    const want = this.active && !this.paused;
    if (!want) {
      this.unmount?.();
      this.unmount = null;
      this.slot.classList.remove('live');
      return;
    }
    if (this.unmount || this.mounting || !this.visible) return;
    this.mounting = true;
    try {
      await loadStyles(asset('../vendor/park.css'));
      const { mount } = await import('../vendor/park.js');
      // The reader may have paused or moved on while this loaded
      if (this.active && !this.paused) {
        this.unmount = mount($('.park-root', this.slot));
        this.slot.classList.add('live');
      }
    } catch {
      // The poster stays. The panel still tells the story.
    } finally {
      this.mounting = false;
    }
  },
};

function loadStyles(href) {
  return new Promise((resolve, reject) => {
    const link = document.createElement('link');
    link.rel = 'stylesheet';
    link.href = href;
    link.onload = resolve;
    link.onerror = reject;
    document.head.append(link);
  });
}

// The release matching this documentation

const RELEASE_TTL = 3600 * 1000;

function releaseNode({ tag, date }) {
  const node = document.createElement('span');
  node.className = 'rel-in';
  const version = document.createElement('b');
  version.textContent = tag;
  node.append(version, ` ${new Intl.DateTimeFormat('en-GB', { day: 'numeric', month: 'short', year: 'numeric', timeZone: 'UTC' }).format(new Date(date))}`);
  return node;
}

async function initRelease() {
  const line = $('#release');
  const slot = $('#release-v');
  const tag = line.dataset.releaseTag;
  const key = `llam-release:${tag}`;
  let cached = null;
  try { cached = JSON.parse(read(key) || 'null'); } catch { cached = null; }
  if (cached?.tag === tag && Date.now() - cached.at < RELEASE_TTL) {
    slot.replaceChildren(releaseNode(cached));
    return;
  }
  let reveal = null;
  try {
    const { Reveal } = await loadSkullet();
    if (!slot.firstChild) {
      reveal = new Reveal(slot, { id: 'docs-release', label: `Loading release ${tag}` });
      reveal.load(releaseNode({ tag, date: '2026-01-01' }));
    }
  } catch {
    // No skeleton: the line fills in or goes away all the same
  }
  try {
    const response = await fetch(`https://api.github.com/repos/Feralthedogg/LLAM/releases/tags/${encodeURIComponent(tag)}`, { headers: { Accept: 'application/vnd.github+json' } });
    if (!response.ok) throw new Error('No release');
    const release = await response.json();
    if (release.tag_name !== tag) throw new Error('Wrong release');
    const value = { tag: release.tag_name, date: release.published_at, at: Date.now() };
    write(key, JSON.stringify(value));
    if (reveal) reveal.show(releaseNode(value));
    else slot.replaceChildren(releaseNode(value));
  } catch {
    reveal?.destroy();
    if (cached?.tag === tag) slot.replaceChildren(releaseNode(cached));
    else line.hidden = true;
  }
}

// Glass route tiles. CSS tiles are the default; these replace them only where
// WebGL2 runs on a real GPU and the reader has not asked for less transparency.
// Software renderers draw the lens with a visible crease, and slowly.

const NAVY = [0.04, 0.05, 0.16, 0.34];
const ACCENT = [0.30, 0.36, 1, 0.42];

function softwareRenderer() {
  const gl = document.createElement('canvas').getContext('webgl2');
  if (!gl) return true;
  const info = gl.getExtension('WEBGL_debug_renderer_info');
  const name = info ? gl.getParameter(info.UNMASKED_RENDERER_WEBGL) : '';
  gl.getExtension('WEBGL_lose_context')?.loseContext();
  return /swiftshader|llvmpipe|softpipe|software|basic render/i.test(name);
}

async function startGlass() {
  if (softwareRenderer()) return;
  const root = document.documentElement;
  const box = $('.hero-top');
  const hero = $('.hero');
  const canvas = $('#glass');
  const wide = matchMedia('(min-width: 1120px)');
  const plain = matchMedia('(prefers-reduced-transparency: reduce)');
  const art = new Image();
  art.src = asset('../img/hero.webp');
  await art.decode();
  const { createGlass } = await import('../vendor/glass.js');

  // The glass reads a 2D canvas painted exactly like the CSS background beneath
  // it, so nothing jumps when the WebGL canvas fades in.
  const backdrop = document.createElement('canvas');
  const paintBackdrop = () => {
    const rect = box.getBoundingClientRect();
    const dpr = Math.min(window.devicePixelRatio, 2);
    backdrop.width = Math.round(rect.width * dpr);
    backdrop.height = Math.round(rect.height * dpr);
    const context = backdrop.getContext('2d');
    context.fillStyle = '#000';
    context.fillRect(0, 0, backdrop.width, backdrop.height);
    context.imageSmoothingQuality = 'high';
    const style = getComputedStyle(hero);
    const width = parseFloat(style.backgroundSize) * dpr;
    const top = parseFloat(style.backgroundPositionY) * dpr;
    context.drawImage(art, Math.round((backdrop.width - width) / 2), top, width, width * art.naturalHeight / art.naturalWidth);
  };
  paintBackdrop();

  const made = createGlass(canvas, { backdrop, fit: 'stretch', appearance: 'dark', stacking: 'flat' });
  if (!made.ok) return;
  const glass = made.value;
  const shapes = $$('.tile').map((tile) => {
    const shape = glass
      .add({ x: 0, y: 0, width: 0, height: 0, radius: 12, preset: 'standard', appearance: 'dark', tint: tile.classList.contains('hi') ? ACCENT : NAVY })
      .follow(tile);
    tile.addEventListener('pointerdown', () => shape.press(true));
    for (const type of ['pointerup', 'pointercancel', 'pointerleave']) tile.addEventListener(type, () => shape.press(false));
    return shape;
  });
  const apply = () => {
    const on = wide.matches && !plain.matches;
    root.classList.toggle('glass-on', on);
    for (const shape of shapes) {
      if (on) shape.show(); else shape.hide();
    }
  };
  wide.addEventListener('change', apply);
  plain.addEventListener('change', apply);
  new ResizeObserver(() => {
    paintBackdrop();
    glass.setBackdrop(backdrop, 'stretch');
  }).observe(box);
  window.addEventListener('pagehide', () => glass.destroy(), { once: true });
  apply();
}

function initGlass() {
  const wide = matchMedia('(min-width: 1120px)');
  const plain = matchMedia('(prefers-reduced-transparency: reduce)');
  let started = false;
  const begin = () => {
    if (started || !wide.matches || plain.matches) return;
    started = true;
    startGlass().catch(() => {});
  };
  const whenIdle = window.requestIdleCallback || ((work) => setTimeout(work, 200));
  const afterLoad = () => whenIdle(begin);
  if (document.readyState === 'complete') afterLoad();
  else window.addEventListener('load', afterLoad, { once: true });
  wide.addEventListener('change', begin);
}

function start() {
  initAnnouncement();
  initInstall();
  demo.init();
  initPanel();
  demo.sync();
  initRelease();
  initGlass();
}

start();
