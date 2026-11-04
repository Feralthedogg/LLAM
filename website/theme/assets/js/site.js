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

// Behavior for every page: theme, platform, copy buttons, the page list, page
// filters, the checklist, instant page changes, and the search palette. Pages
// read correctly without it; this only makes them quicker to use.

const BASE = new URL('../../', import.meta.url);
const OS_NAMES = ['Linux', 'macOS', 'Windows'];
const SKELETON_AFTER = 150;
const COPIED_FOR = 1400;
const PREFETCH_AFTER = 65;
// The header takes the top 64px, so reading position starts below it
const SPY_LINE = 224;

const $ = (selector, root = document) => root.querySelector(selector);
const $$ = (selector, root = document) => [...root.querySelectorAll(selector)];

export function read(key) {
  try { return localStorage.getItem(key); } catch { return null; }
}

export function write(key, value) {
  // A private window can refuse this; the page keeps working without it
  try { localStorage.setItem(key, value); } catch { /* not remembered */ }
}

export const reducedMotion = () => matchMedia('(prefers-reduced-motion: reduce)').matches;

let skullet = null;
export function loadSkullet() {
  skullet ??= import('../vendor/skullet.js').catch((error) => { skullet = null; throw error; });
  return skullet;
}

export function copyText(text) {
  return navigator.clipboard ? navigator.clipboard.writeText(text) : Promise.reject(new Error('No clipboard'));
}

// Theme

function isDark() {
  const { classList } = document.documentElement;
  if (classList.contains('t-dark')) return true;
  if (classList.contains('t-light')) return false;
  return matchMedia('(prefers-color-scheme: dark)').matches;
}

function labelThemeButtons() {
  const label = isDark() ? 'Use light theme' : 'Use dark theme';
  $$('[data-theme-toggle]').forEach((button) => {
    button.setAttribute('aria-label', label);
    const text = $('.lbl', button);
    if (text) text.textContent = label;
  });
}

function toggleTheme() {
  const next = isDark() ? 'light' : 'dark';
  document.documentElement.classList.remove('t-light', 't-dark');
  document.documentElement.classList.add(`t-${next}`);
  write('llam-theme', next);
  labelThemeButtons();
}

// Platform: one choice drives every platform tab on the site

export function detectOS() {
  const saved = read('llam-os');
  if (OS_NAMES.includes(saved)) return saved;
  const platform = navigator.userAgentData?.platform || navigator.platform || '';
  if (/mac/i.test(platform)) return 'macOS';
  if (/win/i.test(platform)) return 'Windows';
  return 'Linux';
}

let os = detectOS();

const osTokens = (label) => label.textContent.split(/[,/]/).map((name) => name.trim());

function applyPlatform() {
  $$('[data-platforms] button').forEach((button) => {
    const on = button.dataset.os === os;
    button.classList.toggle('on', on);
    button.setAttribute('aria-pressed', String(on));
  });
  $$('.tabbed-set').forEach((set) => {
    const label = $$(':scope > .tabbed-labels > label', set).find((item) => osTokens(item).includes(os));
    const input = label && document.getElementById(label.htmlFor);
    if (input) input.checked = true;
  });
}

export function setOS(next) {
  os = next;
  write('llam-os', next);
  applyPlatform();
}

// Blocks above the one in view change height; keep what is being read in place
function setOSKeeping(next, anchor) {
  const before = anchor.getBoundingClientRect().top;
  setOS(next);
  window.scrollBy(0, anchor.getBoundingClientRect().top - before);
}

function onTabChange(input) {
  const label = $(`.tabbed-labels > label[for="${input.id}"]`, input.parentElement);
  if (!label) return;
  const names = osTokens(label).filter((name) => OS_NAMES.includes(name));
  if (names.length && !names.includes(os)) setOSKeeping(names[0], label);
}

// Copy

function showToast(text) {
  let toast = $('#toast');
  if (!toast) {
    toast = document.createElement('div');
    toast.id = 'toast';
    toast.className = 'toast';
    toast.setAttribute('role', 'status');
    document.body.append(toast);
  }
  toast.textContent = text;
  toast.hidden = false;
  clearTimeout(showToast.timer);
  showToast.timer = setTimeout(() => { toast.hidden = true; }, COPIED_FOR);
}

function copyLink(anchor) {
  copyText(anchor.href).then(() => showToast('Link copied'), () => {});
}

function copyBlock(button) {
  // A folded program copies whole, including the part that starts closed
  const parts = $$('pre.code', button.closest('.cb'));
  if (!parts.length) return;
  copyText(parts.map((part) => part.textContent).join('').replace(/\n$/, '')).then(() => {
    button.textContent = 'Copied';
    button.dataset.done = '1';
    setTimeout(() => {
      button.textContent = 'Copy';
      delete button.dataset.done;
    }, COPIED_FOR);
  }, () => {});
}

// The page list follows what is being read

let spyHeads = [];
let spyHoldUntil = 0;
let spyFrame = 0;

function setTocCurrent(link) {
  $$('.toc a.on').forEach((other) => { if (other !== link) other.classList.remove('on'); });
  if (!link || link.classList.contains('on')) return;
  link.classList.add('on');
  const toc = link.closest('.toc');
  // Only the list scrolls here, never the page
  if (toc.scrollHeight > toc.clientHeight) {
    const offset = link.offsetTop - toc.offsetTop;
    if (offset < toc.scrollTop || offset > toc.scrollTop + toc.clientHeight - 48) toc.scrollTop = offset - toc.clientHeight / 2;
  }
}

function spy() {
  spyFrame = 0;
  // A click on the page list already said which section is being read
  if (!spyHeads.length || Date.now() < spyHoldUntil) return;
  let current = spyHeads[0];
  for (const entry of spyHeads) {
    if (entry.head.getBoundingClientRect().top < SPY_LINE) current = entry;
  }
  if (window.innerHeight + window.scrollY >= document.documentElement.scrollHeight - 4) current = spyHeads[spyHeads.length - 1];
  setTocCurrent(current.link);
}

function initToc() {
  spyHeads = $$('.toc a').map((link) => ({ link, head: document.getElementById(decodeURIComponent(link.hash.slice(1))) }))
    .filter((entry) => entry.head);
  spy();
}

// A link to a symbol or a version opens it

function revealHash() {
  const id = decodeURIComponent(location.hash.slice(1));
  const target = id && document.getElementById(id);
  if (!target) return;
  const holder = target.closest('details');
  if (holder && !holder.open) holder.open = true;
  if (holder) holder.scrollIntoView({ block: 'start' });
  $$('.hit-now').forEach((row) => row.classList.remove('hit-now'));
  if (target.classList.contains('sym')) target.classList.add('hit-now');
}

// Page filter

function sectionsOf(prose) {
  const sections = [];
  let current = { head: null, nodes: [] };
  for (const node of prose.children) {
    if (/^H[23]$/.test(node.tagName)) {
      sections.push(current);
      current = { head: node, nodes: [] };
    } else {
      current.nodes.push(node);
    }
  }
  sections.push(current);
  return sections;
}

function initFilter() {
  const article = $('.art-in[data-filter]');
  if (!article) return;
  const prose = $('.prose', article);
  const label = article.dataset.filter;
  const rows = $$('.sym, .t-wrap tbody tr', prose);
  const lead = $('.lead', prose);
  if (!rows.length) return;
  const noun = $('.sym', prose) ? 'symbols' : 'entries';
  const box = document.createElement('label');
  box.className = 'filter';
  box.innerHTML = '<svg class="i s" viewBox="0 0 24 24" aria-hidden="true"><circle cx="11" cy="11" r="8"></circle><path d="m21 21-4.3-4.3"></path></svg><input type="text" autocomplete="off" spellcheck="false"><small></small>';
  const input = $('input', box);
  input.placeholder = label;
  input.setAttribute('aria-label', label);
  const count = $('small', box);
  count.textContent = `${rows.length} ${noun}`;
  const empty = document.createElement('div');
  empty.className = 'syms';
  empty.hidden = true;
  empty.innerHTML = `<p class="empty" style="margin: 0">Nothing on this page matches. Press <kbd>${isMac() ? '⌘K' : 'Ctrl K'}</kbd> to search all the docs.</p>`;
  (lead || prose.firstElementChild).after(box);
  box.after(empty);
  const text = new Map(rows.map((row) => [row, ($('.sym-h', row) || row).textContent.toLowerCase()]));
  const sections = sectionsOf(prose);

  input.addEventListener('input', () => {
    const query = input.value.trim().toLowerCase();
    let shown = 0;
    for (const row of rows) {
      const match = !query || text.get(row).includes(query);
      row.classList.toggle('filter-hide', !match);
      if (match) shown++;
    }
    for (const section of sections) {
      const nodes = section.nodes.filter((node) => node !== box && node !== empty && !(node === lead));
      const inside = nodes.flatMap((node) => rows.filter((row) => node.contains(row)));
      const anyShown = inside.some((row) => !row.classList.contains('filter-hide'));
      // While filtering, only rows and the headings above them stay
      const keep = !query || anyShown;
      section.head?.classList.toggle('filter-hide', !keep);
      for (const node of nodes) {
        const hasRows = inside.some((row) => node.contains(row));
        node.classList.toggle('filter-hide', Boolean(query) && (!hasRows || !inside.some((row) => node.contains(row) && !row.classList.contains('filter-hide'))));
      }
    }
    count.textContent = query ? `${shown} of ${rows.length}` : `${rows.length} ${noun}`;
    empty.hidden = shown > 0;
  });
}

// Checklist

function initChecklist() {
  const list = $('[data-checklist]');
  if (!list) return;
  const items = $$(':scope > li', list);
  const key = 'llam-checklist';
  let saved = {};
  try { saved = JSON.parse(read(key) || '{}'); } catch { saved = {}; }
  const progress = document.createElement('div');
  progress.className = 'gh-prog';
  progress.innerHTML = '<span></span><i><u></u></i>';
  list.before(progress);
  const paint = () => {
    const done = items.filter((item) => item.dataset.on === '1').length;
    $('span', progress).textContent = `${done} of ${items.length} checked`;
    $('u', progress).style.width = `${(done / items.length) * 100}%`;
  };
  for (const item of items) {
    const box = $('input', item);
    const name = ($('.tx', item) || item).textContent.trim();
    if (!box) continue;
    box.checked = Boolean(saved[name]);
    item.dataset.on = box.checked ? '1' : '0';
    box.addEventListener('change', () => {
      item.dataset.on = box.checked ? '1' : '0';
      if (box.checked) saved[name] = 1; else delete saved[name];
      write(key, JSON.stringify(saved));
      paint();
    });
  }
  paint();
}

function initPage() {
  labelThemeButtons();
  applyPlatform();
  initFilter();
  initChecklist();
  initToc();
  revealHash();
}

// Page changes: fetch the next page and swap the article, so the sidebar, its
// scroll and its open groups stay where they are. Anything unexpected falls
// back to an ordinary navigation.

const pages = new Map();
const positions = new Map();
let navigation = 0;
let shownPath = location.pathname;
let lastScrollY = window.scrollY;

function fetchPage(url) {
  const key = url.split('#')[0];
  if (!pages.has(key)) {
    pages.set(key, fetch(key, { credentials: 'same-origin' }).then((response) => {
      const type = response.headers.get('content-type') || '';
      if (!response.ok || !type.includes('html')) throw new Error(`Cannot swap in ${key}`);
      return response.text();
    }).then((html) => new DOMParser().parseFromString(html, 'text/html')).catch((error) => {
      pages.delete(key);
      throw error;
    }));
  }
  return pages.get(key);
}

function sitePageURL(anchor) {
  if (!anchor || anchor.target || anchor.hasAttribute('download')) return null;
  const url = new URL(anchor.href, location.href);
  if (url.origin !== location.origin || !url.pathname.startsWith(BASE.pathname)) return null;
  if (!url.pathname.endsWith('/')) return null;
  // The home has its own page shell
  if (url.pathname === BASE.pathname) return null;
  return url;
}

// Links in the parts that stay on screen must survive the address changing
function pinLinks(root) {
  $$('a[href]', root).forEach((anchor) => anchor.setAttribute('href', anchor.href));
}

function pageShape(title) {
  const shape = document.createElement('div');
  shape.className = 'page-shape';
  shape.innerHTML = `<p class="s-crumb">Docs / Concepts / Page title</p><h1 class="s-h1"></h1>
    <div class="meta"><span>Edit page</span><span>Last modified: 03 November 2026</span></div>
    <p class="lead">A page answers one question. This line says what it covers, and who it is for in a sentence or two.</p>
    <h2 class="d2">The first section of the page</h2>
    <p>Most sections open with a short paragraph that says what the reader can rely on. It runs for two or three lines at this width, and then gives way to a table, a list, or a block of code.</p>
    <p>A second paragraph explains the exception to the rule above, and links to the page that covers it in depth.</p>
    <h2 class="d2">A second section</h2>
    <p>Then the same rhythm again: a sentence or two, and then the details that a reader came here to find.</p>`;
  $('.s-h1', shape).textContent = title;
  return shape;
}

// Waits under the threshold show nothing. Past it, a skeleton is drawn over the
// old article, in the same place, and leaves with it.
function startPageSkeleton(title) {
  const article = $('.art-in');
  let host = null;
  let reveal = null;
  let cancelled = false;
  const timer = setTimeout(() => {
    loadSkullet().then(({ Reveal }) => {
      if (cancelled) return;
      host = document.createElement('div');
      host.className = 'page-reveal';
      article.append(host);
      reveal = new Reveal(host, { id: 'doc-page', label: 'Loading page', reveal: 'together' });
      reveal.load(pageShape(title));
    }, () => {});
  }, 0);
  return () => {
    cancelled = true;
    clearTimeout(timer);
    reveal?.destroy();
    host?.remove();
    return Boolean(host);
  };
}

function announce(text) {
  let region = $('#page-status');
  if (!region) {
    region = document.createElement('div');
    region.id = 'page-status';
    region.className = 'sr-only';
    region.setAttribute('aria-live', 'polite');
    document.body.append(region);
  }
  region.textContent = text;
}

function swapPage(doc, url, { restore }) {
  const next = {
    article: $('.art-in', doc),
    toc: $('.toc', doc),
    main: $('.main', doc),
    side: $('.side', doc),
    nav: $('.nav', doc),
  };
  const fresh = document.importNode(next.article, true);
  $('.art-in').replaceWith(fresh);
  $('.toc').replaceWith(document.importNode(next.toc, true));
  const main = $('.main');
  if (main.dataset.tab !== next.main.dataset.tab) {
    main.dataset.tab = next.main.dataset.tab;
    const side = document.importNode(next.side, true);
    $('.side').replaceWith(side);
    pinLinks(side);
    const nav = document.importNode(next.nav, true);
    $('.nav').replaceWith(nav);
    pinLinks(nav);
  } else {
    updateSide(url);
  }
  setMenu(false);
  document.title = doc.title;
  initPage();
  if (restore !== undefined) window.scrollTo(0, restore);
  else if (url.hash) document.getElementById(decodeURIComponent(url.hash.slice(1)))?.scrollIntoView();
  else window.scrollTo(0, 0);
  const heading = $('h1', fresh);
  if (heading) announce(heading.textContent);
}

// Only the sidebar scrolls here, never the page
function showCurrentSideLink() {
  const link = $('.side a.side-l.on');
  const box = link?.closest('.side-in');
  if (!box) return;
  const rect = link.getBoundingClientRect();
  const within = box.getBoundingClientRect();
  if (rect.top < within.top || rect.bottom > within.bottom) box.scrollTop += rect.top - within.top - within.height / 2;
}

function updateSide(url) {
  $$('.side a.side-l').forEach((link) => {
    const on = link.pathname === url.pathname;
    link.classList.toggle('on', on);
    if (on) {
      link.setAttribute('aria-current', 'page');
      const group = link.closest('details');
      if (group) group.open = true;
    } else {
      link.removeAttribute('aria-current');
    }
  });
  showCurrentSideLink();
}

async function go(url, options = {}) {
  const seq = ++navigation;
  const stop = startPageSkeletonLater(options.title || 'Page');
  const leftAt = lastScrollY;
  let doc;
  try {
    doc = await fetchPage(url.href);
  } catch {
    stop();
    location.href = url.href;
    return;
  }
  stop();
  if (seq !== navigation) return;
  if (!$('.art-in', doc) || !$('.side', doc)) {
    location.href = url.href;
    return;
  }
  positions.set(shownPath, leftAt);
  if (options.push !== false) history.pushState({}, '', url.href);
  shownPath = url.pathname;
  swapPage(doc, url, { restore: options.push === false ? positions.get(url.pathname) ?? 0 : undefined });
}

// The first timer is the wait worth showing; Skullet then fades in on its own
function startPageSkeletonLater(title) {
  let stop = null;
  const timer = setTimeout(() => { stop = startPageSkeleton(title); }, SKELETON_AFTER);
  return () => {
    clearTimeout(timer);
    stop?.();
  };
}

function onLinkClick(event) {
  if (event.defaultPrevented || event.button !== 0 || event.metaKey || event.ctrlKey || event.shiftKey || event.altKey) return;
  const anchor = event.target.closest?.('a[href]');
  if (!anchor) return;
  const dialog = anchor.closest('dialog');
  const url = sitePageURL(anchor);
  if (anchor.closest('.side, #m-menu')) setMenu(false);
  anchor.closest('.toc-m')?.removeAttribute('open');
  const samePage = url && url.pathname === location.pathname;
  if (dialog) dialog.close();
  if (samePage) {
    if (url.hash) {
      spyHoldUntil = Date.now() + 300;
      if (anchor.closest('.toc')) setTocCurrent(anchor);
    } else {
      event.preventDefault();
      window.scrollTo(0, 0);
    }
    return;
  }
  // Only article pages can swap in place
  if (!url || !$('.art-in')) return;
  event.preventDefault();
  go(url, { title: anchor.textContent.trim().slice(0, 40) || 'Page' });
}

let prefetchTimer = 0;

function prefetchLink(event) {
  clearTimeout(prefetchTimer);
  const anchor = event.target.closest?.('a[href]');
  if (!anchor || !$('.art-in') || navigator.connection?.saveData) return;
  const url = sitePageURL(anchor);
  if (!url || url.pathname === location.pathname) return;
  // A pointer passing over the sidebar should not fetch every page it crosses
  prefetchTimer = setTimeout(() => fetchPage(url.href).catch(() => {}), event.type === 'touchstart' ? 0 : PREFETCH_AFTER);
}

// Small screens

const menuIsOpen = () => $('[data-menu]')?.getAttribute('aria-expanded') === 'true';

// Docs pages open the sidebar as the menu; the home and 404 have their own list
function setMenu(open) {
  const side = $('.side');
  const menu = $('#m-menu');
  const button = $('[data-menu]');
  if (side) side.dataset.open = open ? '1' : '0';
  if (menu) menu.hidden = !open;
  button?.setAttribute('aria-expanded', String(open));
  button?.setAttribute('aria-label', open ? 'Close menu' : 'Menu');
  document.documentElement.classList.toggle('menu-open', open);
  if (open) $('.side a.side-l.on')?.scrollIntoView({ block: 'nearest' });
}

// Search

const isMac = () => /mac/i.test(navigator.userAgentData?.platform || navigator.platform || '');

const STARTERS = [
  { n: 'Quickstart', t: 'Install LLAM, link it from CMake, and run one task.', w: 'Get started', u: 'start/quickstart/' },
  { n: 'Examples', t: 'Pick a focused task, channel, I/O, embedding, or blocking-work snippet.', w: 'Examples', u: 'examples/' },
  { n: 'Public API', t: 'Find the right API group, then check options and ABI rules.', w: 'Reference', u: 'reference/api/' },
  { n: 'Debug a hang', t: 'Capture stats, runtime dumps, wait ownership, and timeout artifacts.', w: 'Guides', u: 'guides/diagnostics/' },
];
const PER_GROUP = 7;

let searchIndex = null;
let searchLoad = null;
let searchActive = 0;

const decoder = document.createElement('textarea');
function plain(text) {
  decoder.innerHTML = text.replace(/<[^>]+>/g, ' ');
  return decoder.value.replace(/\s+/g, ' ').trim();
}

function buildIndex(search, site) {
  const entries = site.symbols.map((symbol) => ({
    g: 'API', n: symbol.n, t: symbol.t, w: symbol.w, u: symbol.u, m: true, page: true,
    name: symbol.n.toLowerCase(), text: symbol.t.toLowerCase(), full: symbol.t,
  }));
  for (const doc of search.docs) {
    const [path, hash] = doc.location.split('#');
    const page = site.pages[path];
    // The home is a landing page, not a search result
    if (!page?.tab) continue;
    const title = plain(doc.title);
    const full = plain(doc.text);
    entries.push({
      g: page.tab === 'Examples' ? 'Examples' : 'Docs',
      n: title,
      t: full,
      w: hash ? `${page.w} / ${page.t}` : page.w,
      u: doc.location,
      m: false,
      page: !hash,
      name: title.toLowerCase(),
      text: full.toLowerCase(),
      full,
    });
  }
  return entries;
}

function loadIndex() {
  searchLoad ??= Promise.all([
    fetch(new URL('search/search_index.json', BASE)).then((response) => { if (!response.ok) throw new Error('search index'); return response.json(); }),
    fetch(new URL('search/site.json', BASE)).then((response) => { if (!response.ok) throw new Error('site index'); return response.json(); }),
  ]).then(([search, site]) => {
    searchIndex = buildIndex(search, site);
  }).catch((error) => {
    searchLoad = null;
    throw error;
  });
  return searchLoad;
}

function excerpt(entry, terms) {
  const text = entry.full;
  if (!text) return '';
  if (entry.g === 'API') return text;
  const at = terms.map((term) => entry.text.indexOf(term)).filter((index) => index >= 0).sort((a, b) => a - b)[0];
  if (at === undefined || at < 80) return text.length > 130 ? `${text.slice(0, 130).replace(/\s+\S*$/, '')}…` : text;
  const start = text.indexOf(' ', at - 40) + 1;
  return `…${text.slice(start, start + 130).replace(/\s+\S*$/, '')}…`;
}

function runSearch(query) {
  const needle = query.trim().toLowerCase().replace(/^llam_/, '');
  if (!needle) return [];
  const terms = needle.split(/\s+/);
  const scored = [];
  for (const entry of searchIndex) {
    const at = entry.name.indexOf(needle);
    const inName = at >= 0;
    if (!inName && !terms.every((term) => entry.name.includes(term) || entry.text.includes(term))) continue;
    const bare = entry.name.replace(/^llam_/, '');
    const rank = bare === needle ? 0 : bare.startsWith(needle) ? 1 : inName ? 2 : terms.every((term) => entry.name.includes(term)) ? 3 : entry.page ? 5 : 4;
    scored.push({ entry, at, len: needle.length, terms, score: rank * 100 + Math.min(bare.length, 99), rank });
  }
  scored.sort((a, b) => a.score - b.score);
  const seen = new Set();
  const counts = {};
  const results = [];
  for (const hit of scored) {
    const path = hit.entry.u.split('#')[0];
    // A page whose only tie to the query is text its sections already show
    if (hit.rank === 5 && seen.has(path)) continue;
    seen.add(path);
    counts[hit.entry.g] = (counts[hit.entry.g] || 0) + 1;
    if (counts[hit.entry.g] <= PER_GROUP) results.push(hit);
  }
  return results;
}

function appendMarked(parent, text, terms) {
  const words = (terms || []).filter(Boolean).map((term) => term.replace(/[.*+?^${}()|[\]\\]/g, '\\$&'));
  if (!words.length) {
    parent.append(text);
    return;
  }
  const pattern = new RegExp(words.join('|'), 'ig');
  let last = 0;
  for (const match of text.matchAll(pattern)) {
    parent.append(text.slice(last, match.index));
    const mark = document.createElement('mark');
    mark.textContent = match[0];
    parent.append(mark);
    last = match.index + match[0].length;
  }
  parent.append(text.slice(last));
}

function hitRow(hit, index) {
  const row = document.createElement('a');
  row.className = 'hit';
  row.id = `hit-${index}`;
  row.setAttribute('role', 'option');
  row.href = new URL(hit.entry.u, BASE).href;
  const name = document.createElement('b');
  if (hit.entry.m) name.className = 'm';
  const title = hit.entry.n;
  if (hit.at >= 0) {
    name.append(title.slice(0, hit.at));
    const mark = document.createElement('mark');
    mark.textContent = title.slice(hit.at, hit.at + hit.len);
    name.append(mark, title.slice(hit.at + hit.len));
  } else {
    appendMarked(name, title, hit.terms);
  }
  const where = document.createElement('i');
  where.textContent = hit.entry.w;
  const text = document.createElement('span');
  if (hit.terms) appendMarked(text, excerpt(hit.entry, hit.terms), hit.terms);
  else text.textContent = hit.entry.t;
  row.append(name, where, text);
  return row;
}

function renderHits(list, hits) {
  const order = [];
  const groups = {};
  for (const hit of hits) {
    if (!groups[hit.entry.g]) {
      groups[hit.entry.g] = [];
      order.push(hit.entry.g);
    }
    groups[hit.entry.g].push(hit);
  }
  // API stays first: a symbol search should land on the symbol
  order.sort((a, b) => (a === 'API' ? -1 : b === 'API' ? 1 : 0));
  const out = document.createDocumentFragment();
  let index = 0;
  for (const name of order) {
    const group = document.createElement('div');
    group.setAttribute('role', 'group');
    const label = document.createElement('div');
    label.className = 'pal-g';
    label.textContent = name;
    group.append(label);
    for (const hit of groups[name]) group.append(hitRow(hit, index++));
    out.append(group);
  }
  list.replaceChildren(out);
  setActiveHit(0);
}

function setActiveHit(index) {
  const rows = $$('#search-list .hit');
  const input = $('#search-q');
  searchActive = rows.length ? Math.max(0, Math.min(index, rows.length - 1)) : 0;
  rows.forEach((row, i) => {
    const on = i === searchActive;
    row.classList.toggle('on', on);
    row.setAttribute('aria-selected', String(on));
    if (on) {
      input.setAttribute('aria-activedescendant', row.id);
      row.scrollIntoView({ block: 'nearest' });
    }
  });
  if (!rows.length) input.removeAttribute('aria-activedescendant');
}

function searchSample() {
  const sample = document.createElement('div');
  const row = (name, where, text) => `<div class="hit"><b>${name}</b><i>${where}</i><span>${text}</span></div>`;
  sample.innerHTML = `<div class="pal-g">API</div>
    ${row('llam_runtime_spawn_ex', 'Public API / Tasks', 'Spawn into an explicit runtime handle.')}
    ${row('llam_spawn_ex', 'Public API / Tasks', 'Spawn into the selected current or default runtime.')}
    ${row('llam_spawn', 'Public API / Tasks', 'Convenience spawn wrapper.')}
    <div class="pal-g">Docs</div>
    ${row('Spawn and join', 'Examples / Tasks and timers', 'Start with this shape when one task needs to create work.')}
    ${row('Spawn options', 'Reference / Options', 'Stack size, scheduler class, and placement for a new task.')}`;
  return sample;
}

let searchSkeleton = null;

function clearSearchSkeleton() {
  searchSkeleton?.reveal?.destroy();
  searchSkeleton = null;
}

function showSearchSkeleton(list) {
  if (searchSkeleton) return;
  const state = { reveal: null };
  searchSkeleton = state;
  loadSkullet().then(({ Reveal }) => {
    if (searchSkeleton !== state || searchIndex) return;
    const host = document.createElement('div');
    host.className = 'pal-loader';
    list.replaceChildren(host);
    state.reveal = new Reveal(host, { id: 'search-first-run', label: 'Loading search' });
    state.reveal.load(searchSample());
  }, () => {});
}

function renderSearchFailure(list) {
  list.innerHTML = '<div class="empty"><p>Search could not load</p><p>Check your connection and try again.</p><button class="pal-retry" type="button">Retry</button></div>';
  $('.pal-retry', list).addEventListener('click', () => {
    updateSearch();
    $('#search-q').focus();
  });
}

let searchRun = 0;

function updateSearch() {
  const list = $('#search-list');
  const input = $('#search-q');
  const query = input.value;
  $('#search-clear').hidden = !query;
  if (!query.trim()) {
    clearSearchSkeleton();
    renderHits(list, STARTERS.map((entry) => ({ entry: { ...entry, g: 'Start here', m: false }, at: -1, len: 0 })));
    return;
  }
  if (searchIndex) {
    clearSearchSkeleton();
    const hits = runSearch(query);
    if (hits.length) {
      renderHits(list, hits);
    } else {
      const box = document.createElement('div');
      box.className = 'empty';
      const title = document.createElement('p');
      title.textContent = `No results for “${query.trim()}”`;
      const hint = document.createElement('p');
      hint.innerHTML = 'Try a symbol name without the llam_ prefix, or browse the <a></a>.';
      const link = $('a', hint);
      link.href = new URL('reference/api/', BASE).href;
      link.textContent = 'Public API';
      box.append(title, hint);
      list.replaceChildren(box);
      setActiveHit(0);
    }
    return;
  }
  // The first search fetches the index. Later ones are instant.
  const run = ++searchRun;
  const timer = setTimeout(() => { if (!searchIndex) showSearchSkeleton(list); }, SKELETON_AFTER);
  loadIndex().then(() => {
    clearTimeout(timer);
    if (run === searchRun) updateSearch();
  }, () => {
    clearTimeout(timer);
    clearSearchSkeleton();
    if (run === searchRun) renderSearchFailure(list);
  });
}

function openSearch() {
  const dialog = $('#search');
  if (!dialog || dialog.open) return;
  const input = $('#search-q');
  setMenu(false);
  dialog.showModal();
  input.value = '';
  updateSearch();
  input.focus();
  loadIndex().catch(() => {});
}

function initSearch() {
  const dialog = $('#search');
  if (!dialog) return;
  const input = $('#search-q');
  input.addEventListener('input', () => {
    searchRun++;
    updateSearch();
  });
  input.addEventListener('keydown', (event) => {
    if (event.key === 'ArrowDown' || event.key === 'ArrowUp') {
      event.preventDefault();
      setActiveHit(searchActive + (event.key === 'ArrowDown' ? 1 : -1));
    } else if (event.key === 'Enter') {
      event.preventDefault();
      $$('#search-list .hit')[searchActive]?.click();
    }
  });
  $('#search-clear').addEventListener('click', () => {
    input.value = '';
    updateSearch();
    input.focus();
  });
  dialog.addEventListener('click', (event) => {
    if (event.target === dialog) dialog.close();
  });
  document.addEventListener('keydown', (event) => {
    const typing = /^(INPUT|TEXTAREA|SELECT)$/.test(event.target.tagName) || event.target.isContentEditable;
    if ((event.key === 'k' || event.key === 'K') && (event.metaKey || event.ctrlKey)) {
      event.preventDefault();
      if (dialog.open) dialog.close(); else openSearch();
    } else if (event.key === '/' && !typing && !event.metaKey && !event.ctrlKey && !event.altKey) {
      event.preventDefault();
      openSearch();
    }
  });
}

// Wiring

function onClick(event) {
  const target = event.target.closest?.('button, a, summary');
  if (target?.matches('.toc a')) {
    spyHoldUntil = Date.now() + 300;
    setTocCurrent(target);
  }
  if (target?.matches('[data-search]')) openSearch();
  else if (target?.matches('[data-theme-toggle]')) toggleTheme();
  else if (target?.matches('[data-menu]')) setMenu(!menuIsOpen());
  else if (target?.matches('.headerlink')) copyLink(target);
  else if (target?.matches('[data-copy-link]')) {
    // The row is already in view; only the address changes
    event.preventDefault();
    history.replaceState(null, '', target.href);
    copyLink(target);
  }
  else if (target?.matches('.cb-copy')) copyBlock(target);
  else if (target?.matches('[data-platforms] button')) setOSKeeping(target.dataset.os, target);
  onLinkClick(event);
}

function start() {
  if (!isMac()) $$('[data-mod]').forEach((key) => { key.textContent = 'Ctrl K'; });
  pinLinks($('.hd') || document.createElement('i'));
  ['.side', '.ft'].forEach((selector) => { const part = $(selector); if (part) pinLinks(part); });
  document.addEventListener('click', onClick);
  document.addEventListener('change', (event) => {
    if (event.target.matches?.('.tabbed-set > input')) onTabChange(event.target);
  });
  document.addEventListener('pointerover', prefetchLink, { passive: true });
  document.addEventListener('touchstart', prefetchLink, { passive: true });
  document.addEventListener('focusin', prefetchLink);
  window.addEventListener('scroll', () => {
    lastScrollY = window.scrollY;
    spyFrame ||= requestAnimationFrame(spy);
  }, { passive: true });
  window.addEventListener('hashchange', revealHash);
  window.addEventListener('popstate', () => {
    // A link to a section fires this too; the browser handles those itself
    if ($('.art-in') && location.pathname !== shownPath) go(new URL(location.href), { push: false });
  });
  matchMedia('(prefers-color-scheme: dark)').addEventListener('change', labelThemeButtons);
  document.addEventListener('keydown', (event) => {
    if (event.key === 'Escape' && menuIsOpen()) {
      setMenu(false);
      $('[data-menu]')?.focus();
    }
  });
  matchMedia('(min-width: 961px)').addEventListener('change', (event) => { if (event.matches) setMenu(false); });
  initSearch();
  initPage();
  showCurrentSideLink();
  // Skeletons need this only on a slow wait, and then it must already be here
  const idle = window.requestIdleCallback || ((work) => setTimeout(work, 1200));
  if ($('.art-in')) idle(() => loadSkullet().catch(() => {}));
}

start();
