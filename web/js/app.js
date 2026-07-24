// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          web/js/app.js
// Purpose:       StarBase web UI: dashboard, browse/query grid, and frame detail,
//                driven entirely by the /api/v1 API. Vanilla JS, no build step.
// Created:       2026-07-24
// Last Modified: 2026-07-24
// Version:       0.1.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
'use strict';

const api = (path) => fetch('/api/v1' + path).then(r => {
  if (!r.ok) return r.json().then(e => { throw new Error(e.error || r.statusText); });
  return r.json();
});
const el = (tag, attrs = {}, ...kids) => {
  const n = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (k === 'class') n.className = v;
    else if (k === 'onclick') n.onclick = v;
    else if (v != null) n.setAttribute(k, v);
  }
  for (const kid of kids) if (kid != null) n.append(kid.nodeType ? kid : String(kid));
  return n;
};
const app = document.getElementById('app');
const fmt = (v, d = '-') => (v == null || v === '') ? d : v;
const num = (v, dp = 0) => v == null ? '-' : Number(v).toFixed(dp);

// ---- Dashboard ----
async function dashboard() {
  app.replaceChildren(el('div', { class: 'muted' }, 'Loading...'));
  try {
    const s = await api('/status');
    const tiles = el('div', { class: 'tiles' });
    const tile = (n, l, cls) => tiles.append(
      el('div', { class: 'tile ' + (cls || '') },
        el('div', { class: 'n' }, n.toLocaleString()), el('div', { class: 'l' }, l)));
    tile(s.frames, 'Frames'); tile(s.objects, 'Targets'); tile(s.nights, 'Nights');
    tile(s.files, 'Files'); tile(s.roots, 'Roots');
    if (s.errors > 0) tile(s.errors, 'Errors', 'warn');

    const summary = await api('/summary?by=object');
    const rollup = new Map();
    for (const r of summary) {
      const o = rollup.get(r.label) || { frames: 0, hours: 0, types: new Set() };
      o.frames += Number(r.count); o.hours += Number(r.hours || 0); o.types.add(r.image_type);
      rollup.set(r.label, o);
    }
    const rows = [...rollup.entries()].sort((a, b) => b[1].frames - a[1].frames);
    const tbl = el('table', {},
      el('thead', {}, el('tr', {},
        el('th', {}, 'Target'), el('th', { class: 'num' }, 'Frames'),
        el('th', { class: 'num' }, 'Hours'), el('th', {}, 'Types'))),
      el('tbody', {}, ...rows.map(([label, o]) =>
        el('tr', { onclick: () => go('browse', { object: label }) },
          el('td', {}, fmt(label)), el('td', { class: 'num' }, o.frames.toLocaleString()),
          el('td', { class: 'num' }, o.hours.toFixed(1)),
          el('td', {}, [...o.types].join(', '))))));

    app.replaceChildren(tiles,
      el('h2', {}, 'Targets (click to browse)'), el('div', { class: 'wrap' }, tbl));
    setConn(`${s.server} · schema v${s.schema_version} · StarBase ${s.version}`);
  } catch (e) { showError(e); }
}

// ---- Browse ----
const state = { filters: {}, offset: 0, limit: 50, total: 0 };

async function browse(preset) {
  if (preset) { state.filters = { ...preset }; state.offset = 0; }
  const f = state.filters;

  const input = (name, ph) => el('input', {
    name, placeholder: ph, value: f[name] || '',
    onchange: (e) => { setFilter(name, e.target.value); }
  });
  const typeSel = el('select', { name: 'image_type', onchange: (e) => setFilter('image_type', e.target.value) },
    ...['', 'light', 'dark', 'flat', 'bias', 'darkflat', 'master', 'unknown'].map(t =>
      el('option', { value: t, selected: (f.image_type || '') === t ? '' : null }, t || 'any type')));

  const bar = el('div', { class: 'filters' },
    input('object', 'target'), typeSel, input('filter', 'filter'),
    input('night', 'night YYYY-MM-DD'), input('rig', 'rig'),
    el('button', { class: 'btn', onclick: () => { state.filters = {}; state.offset = 0; render(); } }, 'Clear'));

  const grid = el('div', { class: 'wrap' }, el('div', { class: 'muted' }, 'Loading...'));
  const pager = el('div', { class: 'pager' });
  app.replaceChildren(bar, grid, pager);

  try {
    const q = new URLSearchParams({ limit: state.limit, offset: state.offset, ...f }).toString();
    const data = await api('/frames?' + q);
    state.total = data.total;

    const tbl = el('table', {},
      el('thead', {}, el('tr', {},
        ...['Type', 'Target', 'Filter', 'Night', 'Exp', 'Gain', 'Rig', 'File'].map(h =>
          el('th', {}, h)))),
      el('tbody', {}, ...data.frames.map(fr =>
        el('tr', { onclick: () => detail(fr.frame_id) },
          el('td', {}, el('span', { class: 'pill ' + fr.image_type }, fr.image_type)),
          el('td', {}, fmt(fr.object)), el('td', {}, fmt(fr.filter)),
          el('td', {}, fmt(fr.session_night)), el('td', { class: 'num' }, num(fr.exposure_s, 0) + 's'),
          el('td', { class: 'num' }, fmt(fr.gain)), el('td', {}, fmt(fr.rig)),
          el('td', { class: 'muted' }, fr.filename)))));
    grid.replaceChildren(tbl);

    const from = state.total ? state.offset + 1 : 0;
    const to = Math.min(state.offset + state.limit, state.total);
    pager.replaceChildren(
      el('button', { class: 'btn', onclick: () => page(-1) }, '← Prev'),
      el('span', {}, `${from}–${to} of ${state.total.toLocaleString()}`),
      el('button', { class: 'btn', onclick: () => page(1) }, 'Next →'));
  } catch (e) { showError(e); }
}
function setFilter(k, v) { if (v) state.filters[k] = v; else delete state.filters[k]; state.offset = 0; browse(); }
function page(d) {
  const off = state.offset + d * state.limit;
  if (off < 0 || off >= state.total) return;
  state.offset = off; browse();
}

// ---- Frame detail drawer ----
const drawer = document.getElementById('detail');
async function detail(id) {
  drawer.classList.remove('hidden');
  drawer.replaceChildren(el('div', { class: 'muted' }, 'Loading...'));
  try {
    const fr = await api('/frames/' + id);
    const kv = el('div', { class: 'kv' });
    const row = (k, v) => { kv.append(el('div', { class: 'k' }, k), el('div', { class: 'v' }, fmt(v))); };
    row('Type', fr.image_type); row('Target', fr.object);
    row('Filter', fr.filter); row('Night', fr.session_night);
    row('Date (UTC)', fr.date_obs_utc); row('Exposure', fr.exposure_s && fr.exposure_s + ' s');
    row('Gain / Offset', `${fmt(fr.gain)} / ${fmt(fr.offset_adu)}`);
    row('Camera', fr.camera); row('Rig', fr.rig); row('Site', fr.site);
    row('Binning', fr.binx && `${fr.binx}×${fr.biny}`);
    row('CCD temp', fr.ccd_temp_c != null && fr.ccd_temp_c + ' °C');
    row('RA / Dec', (fr.ra_deg != null) ? `${num(fr.ra_deg, 4)}, ${num(fr.dec_deg, 4)}` : null);
    row('SQM', fr.sqm_mag_arcsec2);

    const hdr = el('table', {}, el('tbody', {}, ...(fr.keywords || []).map(k =>
      el('tr', {}, el('td', {}, k.keyword), el('td', {}, fmt(k.value)),
        el('td', { class: 'muted' }, fmt(k.comment, ''))))));

    drawer.replaceChildren(
      el('button', { class: 'btn close', onclick: () => drawer.classList.add('hidden') }, '✕'),
      el('h3', {}, fr.filename),
      el('div', { class: 'muted', style: 'word-break:break-all' }, fr.abs_path),
      kv,
      el('h2', {}, `Header (${(fr.keywords || []).length} cards)`),
      el('div', { class: 'hdr' }, hdr));
  } catch (e) { drawer.replaceChildren(el('div', { class: 'err-msg' }, String(e.message || e))); }
}

// ---- shell ----
function setConn(t) { const c = document.getElementById('conn'); c.textContent = t; c.classList.remove('err'); }
function showError(e) {
  app.replaceChildren(el('div', { class: 'err-msg' }, 'Error: ' + (e.message || e)));
  const c = document.getElementById('conn'); c.textContent = 'disconnected'; c.classList.add('err');
}
let current = 'dashboard';
function render() { current === 'dashboard' ? dashboard() : browse(); }
function go(view, preset) {
  current = view;
  document.querySelectorAll('nav button').forEach(b => b.classList.toggle('active', b.dataset.view === view));
  view === 'dashboard' ? dashboard() : browse(preset);
}
document.querySelectorAll('nav button').forEach(b => b.onclick = () => go(b.dataset.view));
go('dashboard');
