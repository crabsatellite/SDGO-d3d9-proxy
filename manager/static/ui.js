// SDGO Mecha UI: helpers for the library in ui.css (icons, the brand mark, client artwork and
// a few builders). Pages that load it as a module get data-ui-* attributes filled in:
//   data-ui-icon="mods"   an icon prepended to the element
//   data-ui-mark          the V-fin brand mark prepended
//   data-ui-art="hangar"  the client picture set as the element's --art (class has-art)

const SVG = 'http://www.w3.org/2000/svg';

// 24 x 24 line icons, angular like the plates. Drawn for this UI.
const ICONS = {
  mods: 'M4 7l8-4 8 4v10l-8 4-8-4z M4 7l8 4 8-4 M12 11v10',
  services: 'M3 12h4l2-5 4 10 2-5h6',
  archive: 'M3 4h18v5H3z M5 9v11h14V9 M10 13h4',
  shop: 'M3 4h3l2.5 11h10L21 7H7 M10 20h.01 M17 20h.01',
  operators: 'M6 20v-3a6 6 0 0 1 12 0v3 M8 9a4 4 0 1 0 8 0 4 4 0 0 0-8 0z M5 9h2 M17 9h2',
  capsules: 'M8.5 3.5l12 12-5 5-12-12z M6 11l7-7 M11 13l2 2',
  collection_capsule: 'M3 8l9-5 9 5v8l-9 5-9-5z M3 8l9 5 9-5 M12 13v8 M7.5 5.5l9 5',
  rewards: 'M8 3h8l-1 6H9z M12 9v3 M7 16a5 5 0 1 0 10 0 5 5 0 0 0-10 0z M12 14v4',
  frp: 'M12 3v4 M5 21l3-6h8l3 6 M8 15V9h8v6 M3 9h3 M18 9h3 M10 12h4',
  ai_catalog: 'M7 7h10v10H7z M10 3v4 M14 3v4 M10 17v4 M14 17v4 M3 10h4 M3 14h4 M17 10h4 M17 14h4 M10 10h4v4h-4z',
  developer: 'M8 7l-5 5 5 5 M16 7l5 5-5 5 M14 4l-4 16',
  exit: 'M12 3v8 M6.3 6.3a8 8 0 1 0 11.4 0',
  add: 'M12 5v14 M5 12h14',
  host: 'M3 4h18v12H3z M8 20h8 M12 16v4 M7 8h6',
  back: 'M19 12H5 M11 6l-6 6 6 6',
  copy: 'M8 8h12v12H8z M4 16V4h12',
  bug: 'M9 7a3 3 0 0 1 6 0 M7 10h10v6a5 5 0 0 1-10 0z M3 13h4 M17 13h4 M4 8l3 2 M20 8l-3 2 M4 19l3-2 M20 19l-3-2 M12 10v11',
  settings: 'M4 7h10 M18 7h2 M4 17h4 M12 17h8 M14 4v6 M8 14v6',
  import: 'M12 3v12 M7 10l5 5 5-5 M4 17v3h16v-3',
  save: 'M4 4h13l3 3v13H4z M8 4v5h8V4 M8 20v-6h8v6',
  check: 'M4 12l5 5L20 6',
  warning: 'M12 3l10 18H2z M12 10v5 M12 18h.01',
  play: 'M7 4l13 8-13 8z',
  refresh: 'M20 11a8 8 0 1 0-2.3 5.7 M20 4v7h-7',
  search: 'M10.5 17a6.5 6.5 0 1 0 0-13 6.5 6.5 0 0 0 0 13z M15.5 15.5L21 21',
};

export function icon(name, cls = 'ui-icon') {
  const svg = document.createElementNS(SVG, 'svg');
  svg.setAttribute('viewBox', '0 0 24 24');
  svg.setAttribute('aria-hidden', 'true');
  svg.setAttribute('class', cls);
  const path = document.createElementNS(SVG, 'path');
  path.setAttribute('d', ICONS[name] || ICONS.mods);
  for (const [key, value] of [['fill', 'none'], ['stroke', 'currentColor'], ['stroke-width', '1.8'], ['stroke-linejoin', 'miter'], ['stroke-linecap', 'square']])
    path.setAttribute(key, value);
  svg.append(path);
  return svg;
}

// The brand mark: a gold V-fin over a red crest and a white face plate with cyan eyes.
export function mark(cls = 'ui-mark') {
  const svg = document.createElementNS(SVG, 'svg');
  svg.setAttribute('viewBox', '0 0 64 64');
  svg.setAttribute('aria-hidden', 'true');
  svg.setAttribute('class', cls);
  svg.innerHTML =
    '<defs><linearGradient id="uiFin" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#ffe07a"/><stop offset=".55" stop-color="#ffc531"/><stop offset="1" stop-color="#e39a0e"/></linearGradient>' +
    '<linearGradient id="uiFace" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#ffffff"/><stop offset="1" stop-color="#b9c6dc"/></linearGradient></defs>' +
    '<path d="M32 37 3 7l10-1 19 21L51 6l10 1z" fill="url(#uiFin)"/>' +
    '<path d="M28 26l4-8 4 8-4 9z" fill="#ff3b4e"/>' +
    '<path d="M19 36l13 7 13-7-3 18-10 7-10-7z" fill="url(#uiFace)" stroke="#2a3d5c" stroke-width="1.2"/>' +
    '<path d="M23 42l7 3v2.4l-7-2.6zM41 42l-7 3v2.4l7-2.6z" fill="#39e1ff"/>' +
    '<path d="M28 52h8l-1 4h-6z" fill="#ff3b4e"/>';
  return svg;
}

// Client artwork (manager/art.py). The picture is set only once it has loaded, so a page
// without a client keeps the drawn look instead of a broken image.
const artCache = new Map();
export function artUrl(key) { return '/api/art/' + encodeURIComponent(key); }
export function applyArt(element, key, property = '--art') {
  if (!element || !key) return Promise.resolve(false);
  let loading = artCache.get(key);
  if (!loading) {
    loading = new Promise(resolve => {
      const probe = new Image();
      probe.onload = () => resolve(true);
      probe.onerror = () => resolve(false);
      probe.src = artUrl(key);
    });
    artCache.set(key, loading);
  }
  return loading.then(ok => {
    if (ok) {
      element.style.setProperty(property, 'url("' + artUrl(key) + '")');
      element.classList.add('has-art');
    }
    return ok;
  });
}

export const HERO_ART = ['hero-unicorn', 'hero-strike', 'hero-maoh', 'hero-awaken', 'hero-space'];
export function pick(list) { return list[Math.floor(Math.random() * list.length)]; }

function node(tag, text, cls) {
  const element = document.createElement(tag);
  if (text !== undefined && text !== null) element.textContent = text;
  if (cls) element.className = cls;
  return element;
}

export function chip(text, tone = '') { return node('span', text, 'ui-chip' + (tone ? ' is-' + tone : '')); }
export function led(state = 'off') { return node('i', undefined, 'ui-led is-' + state); }
export function status(state, text) { const box = node('span', undefined, 'ui-status'); box.append(led(state), node('span', text)); return box; }
export function stat(value, label, tone = '') {
  const box = node('div', undefined, 'ui-stat' + (tone ? ' is-' + tone : ''));
  box.append(node('b', String(value)), node('span', label));
  return box;
}
export function gauge(label, value, max, tone = '') {
  const box = node('div', undefined, 'ui-gauge' + (tone ? ' is-' + tone : ''));
  const head = node('div', undefined, 'ui-gauge-label');
  head.append(node('span', label), node('span', value + ' / ' + max));
  const track = node('div', undefined, 'ui-gauge-track');
  track.style.setProperty('--value', (max > 0 ? Math.max(0, Math.min(100, value / max * 100)) : 0) + '%');
  box.append(head, track);
  return box;
}

// A wide banner with a random client illustration on the right.
export function hero({ eyebrow, title, text, stats = [], art = pick(HERO_ART), tag = 'MOBILE SUIT HANGAR' }) {
  const section = node('section', undefined, 'ui-hero ui-boot');
  const copy = node('div', undefined, 'ui-hero-copy');
  if (eyebrow) copy.append(node('span', eyebrow, 'ui-eyebrow'));
  copy.append(node('h2', title, 'ui-title'));
  if (text) copy.append(node('p', text));
  const row = node('div', undefined, 'ui-hero-stats');
  for (const item of stats) row.append(stat(item.value, item.label, item.tone));
  section.append(node('span', tag, 'ui-hero-tag'), node('i', undefined, 'ui-hero-scan'), copy, row);
  applyArt(section, art);
  return section;
}

// Behaviours by attribute: data-ui-open="<dialog id>" opens that dialog, data-ui-close closes
// the dialog it is in, and a click on a .ui-tab marks it active in its .ui-tabs group.
document.addEventListener('click', event => {
  const target = event.target instanceof Element ? event.target : null;
  const opener = target?.closest('[data-ui-open]');
  if (opener) document.getElementById(opener.dataset.uiOpen)?.showModal();
  const closer = target?.closest('[data-ui-close]');
  if (closer) closer.closest('dialog')?.close();
  const tab = target?.closest('.ui-tabs .ui-tab');
  if (tab) for (const other of tab.parentElement.querySelectorAll('.ui-tab')) {
    other.classList.toggle('is-active', other === tab);
    other.setAttribute('aria-selected', String(other === tab));
  }
});

export function enhance(root = document) {
  for (const element of root.querySelectorAll('[data-ui-icon]')) {
    if (!element.querySelector(':scope > svg.ui-icon')) element.prepend(icon(element.dataset.uiIcon));
  }
  for (const element of root.querySelectorAll('[data-ui-mark]')) {
    if (!element.querySelector(':scope > svg.ui-mark')) element.prepend(mark());
  }
  for (const element of root.querySelectorAll('[data-ui-art]')) applyArt(element, element.dataset.uiArt);
}

enhance();
