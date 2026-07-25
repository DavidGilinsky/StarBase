// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          web/js/app.js
// Purpose:       StarBase web UI. Dashboard, browse, the query builder (filter
//                AST) with saved queries and an actions bar that stages / hands
//                off to WBPP / exports / runs filesystem ops on the result set,
//                the jobs ledger, and the frame detail drawer (header,
//                calibration match, sidecars). Vanilla JS, no build step.
// Created:       2026-07-24
// Last Modified: 2026-07-25
// Version:       0.2.0
// License:       GPL-3.0-or-later
// ---------------------------------------------------------------------------
'use strict';

// ---- API + DOM helpers -----------------------------------------------------

const token = () => localStorage.getItem('sb_token') || '';
const authHeaders = (h = {}) => { const t = token(); if (t) h['Authorization'] = 'Bearer ' + t; return h; };
const asError = (r) => r.json().then(e => { throw new Error(e.error || r.statusText); },
                                    () => { throw new Error(r.statusText); });

const api = (path) => fetch('/api/v1' + path, { headers: authHeaders() })
  .then(r => r.ok ? r.json() : asError(r));
const apiPost = (path, body) => fetch('/api/v1' + path, {
  method: 'POST', headers: authHeaders({ 'Content-Type': 'application/json' }),
  body: JSON.stringify(body)
}).then(r => r.ok ? r.json() : asError(r));
const apiPostText = (path, body) => fetch('/api/v1' + path, {
  method: 'POST', headers: authHeaders({ 'Content-Type': 'application/json' }),
  body: JSON.stringify(body)
}).then(r => r.ok ? r.text() : asError(r));
const apiPatch = (path, body) => fetch('/api/v1' + path, {
  method: 'PATCH', headers: authHeaders({ 'Content-Type': 'application/json' }),
  body: JSON.stringify(body)
}).then(r => r.ok ? r.json() : asError(r));
const apiDelete = (path) => fetch('/api/v1' + path, { method: 'DELETE', headers: authHeaders() })
  .then(r => r.ok ? r.json() : asError(r));
const apiDeleteBody = (path, body) => fetch('/api/v1' + path, {
  method: 'DELETE', headers: authHeaders({ 'Content-Type': 'application/json' }),
  body: JSON.stringify(body)
}).then(r => r.ok ? r.json() : asError(r));

const el = (tag, attrs = {}, ...kids) => {
  const n = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (k === 'class') n.className = v;
    else if (k === 'onclick') n.onclick = v;
    else if (k === 'onchange') n.onchange = v;
    else if (k === 'html') n.innerHTML = v;
    else if (v != null && v !== false) n.setAttribute(k, v === true ? '' : v);
  }
  for (const kid of kids) if (kid != null && kid !== false) n.append(kid.nodeType ? kid : String(kid));
  return n;
};
const app = document.getElementById('app');
const drawer = document.getElementById('detail');
const fmt = (v, d = '-') => (v == null || v === '') ? d : v;
const num = (v, dp = 0) => v == null || v === '' ? '-' : Number(v).toFixed(dp);
const truthy = (v) => v === true || v === 1 || v === '1';

function download(filename, content, type) {
  const blob = new Blob([content], { type: type || 'text/plain' });
  const url = URL.createObjectURL(blob);
  const a = el('a', { href: url, download: filename });
  document.body.append(a); a.click(); a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}
async function copy(text, btn) {
  try { await navigator.clipboard.writeText(text); if (btn) { const t = btn.textContent; btn.textContent = 'copied'; setTimeout(() => btn.textContent = t, 1200); } }
  catch (_e) { /* clipboard may be blocked on http; ignore */ }
}

// ---- queryable fields (mirror of query.cpp fields()) -----------------------

const FIELDS = {
  image_type: 'str', object: 'str', filter: 'str', rig: 'str', camera: 'str',
  site: 'str', bucket: 'str', file_status: 'str', root_label: 'str',
  pier_side: 'str', row_order: 'str',
  session_night: 'date', date_obs_utc: 'date',
  exposure_s: 'num', gain: 'num', offset_adu: 'num', binx: 'num', biny: 'num',
  ccd_temp_c: 'num', set_temp_c: 'num', airmass: 'num', sqm_mag_arcsec2: 'num',
  ra_deg: 'num', dec_deg: 'num', focus_pos: 'num', rotator_deg: 'num',
  guide_rms_arcsec: 'num', naxis1: 'num', naxis2: 'num',
};
const OP_LABEL = {
  eq: '=', ne: '≠', lt: '<', lte: '≤', gt: '>', gte: '≥', like: 'matches',
  in: 'in list', between: 'between', isnull: 'is empty', notnull: 'is set',
};
const OPS_FOR = (type) => {
  if (type === 'str') return ['eq', 'ne', 'like', 'in', 'isnull', 'notnull'];
  if (type === 'date') return ['eq', 'ne', 'lt', 'lte', 'gt', 'gte', 'between', 'isnull', 'notnull'];
  return ['eq', 'ne', 'lt', 'lte', 'gt', 'gte', 'between', 'in', 'isnull', 'notnull'];
};
const noValue = (op) => op === 'isnull' || op === 'notnull';

// Curation lists (tags, collections), cached and refreshed on demand so the
// query builder and actions bar can offer them without a fetch per render.
let gTags = [], gCollections = [];
async function loadCuration() {
  try { const [t, c] = await Promise.all([api('/tags'), api('/collections')]); gTags = t; gCollections = c; }
  catch (_e) { /* keep whatever we had */ }
}

// ---- Dashboard -------------------------------------------------------------

async function dashboard() {
  app.replaceChildren(el('div', { class: 'muted' }, 'Loading...'));
  try {
    const s = await api('/status');
    const tiles = el('div', { class: 'tiles' });
    const tile = (n, l, cls) => tiles.append(
      el('div', { class: 'tile ' + (cls || '') },
        el('div', { class: 'n' }, Number(n).toLocaleString()), el('div', { class: 'l' }, l)));
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
        el('tr', { onclick: () => go('query', { object: label }) },
          el('td', {}, fmt(label)), el('td', { class: 'num' }, o.frames.toLocaleString()),
          el('td', { class: 'num' }, o.hours.toFixed(1)),
          el('td', {}, [...o.types].join(', '))))));

    app.replaceChildren(tiles,
      el('h2', {}, 'Targets (click to query)'), el('div', { class: 'wrap' }, tbl));
    setConn(`${s.server} · schema v${s.schema_version} · StarBase ${s.version}`);
  } catch (e) { showError(e); }
}

// ---- results grid (shared by Browse and Query) -----------------------------

function resultsTable(frames) {
  return el('table', {},
    el('thead', {}, el('tr', {},
      ...['Type', 'Target', 'Filter', 'Night', 'Exp', 'Gain', 'Rig', 'File'].map(h => el('th', {}, h)))),
    el('tbody', {}, ...frames.map(fr =>
      el('tr', { onclick: () => detail(fr.frame_id) },
        el('td', {}, el('span', { class: 'pill ' + fr.image_type }, fr.image_type)),
        el('td', {}, fmt(fr.object)), el('td', {}, fmt(fr.filter)),
        el('td', {}, fmt(fr.session_night)), el('td', { class: 'num' }, num(fr.exposure_s, 0) + 's'),
        el('td', { class: 'num' }, fmt(fr.gain)), el('td', {}, fmt(fr.rig)),
        el('td', { class: 'muted' }, fr.filename)))));
}

// ---- Browse (simple filter bar over GET /frames) ---------------------------

const bstate = { filters: {}, offset: 0, limit: 50, total: 0 };

async function browse(preset) {
  if (preset) { bstate.filters = { ...preset }; bstate.offset = 0; }
  const f = bstate.filters;
  const input = (name, ph) => el('input', {
    name, placeholder: ph, value: f[name] || '',
    onchange: (e) => { if (e.target.value) f[name] = e.target.value; else delete f[name]; bstate.offset = 0; browse(); }
  });
  const typeSel = el('select', { onchange: (e) => { if (e.target.value) f.image_type = e.target.value; else delete f.image_type; bstate.offset = 0; browse(); } },
    ...['', 'light', 'dark', 'flat', 'bias', 'darkflat', 'master', 'unknown'].map(t =>
      el('option', { value: t, selected: (f.image_type || '') === t }, t || 'any type')));

  const bar = el('div', { class: 'filters' },
    input('object', 'target'), typeSel, input('filter', 'filter'),
    input('night', 'night YYYY-MM-DD'), input('rig', 'rig'),
    el('button', { class: 'btn', onclick: () => { bstate.filters = {}; bstate.offset = 0; browse(); } }, 'Clear'),
    el('button', { class: 'btn primary', onclick: () => go('query', { ...f }) }, 'Open in Query →'));

  const grid = el('div', { class: 'wrap' }, el('div', { class: 'muted' }, 'Loading...'));
  const pager = el('div', { class: 'pager' });
  app.replaceChildren(bar, grid, pager);
  try {
    const q = new URLSearchParams({ limit: bstate.limit, offset: bstate.offset, ...f }).toString();
    const data = await api('/frames?' + q);
    bstate.total = data.total;
    grid.replaceChildren(resultsTable(data.frames));
    const from = bstate.total ? bstate.offset + 1 : 0;
    const to = Math.min(bstate.offset + bstate.limit, bstate.total);
    pager.replaceChildren(
      el('button', { class: 'btn', onclick: () => { if (bstate.offset > 0) { bstate.offset -= bstate.limit; browse(); } } }, '← Prev'),
      el('span', {}, `${from}–${to} of ${bstate.total.toLocaleString()}`),
      el('button', { class: 'btn', onclick: () => { if (bstate.offset + bstate.limit < bstate.total) { bstate.offset += bstate.limit; browse(); } } }, 'Next →'));
  } catch (e) { showError(e); }
}

// ---- Query builder ---------------------------------------------------------

const qb = {
  combiner: 'and',
  conds: [],
  cone: { on: false, ra: '', dec: '', radius: '' },
  raw: false, rawText: '',
  sortField: 'date_obs_utc', sortDir: 'desc',
  limit: 100, offset: 0, total: 0, lastCount: null,
};

// Preset from Browse/Dashboard: map simple {object,image_type,filter,night,rig}.
function presetToConds(p) {
  const conds = [];
  if (p.__tag) conds.push({ field: 'tag', op: 'tagged', value: p.__tag });
  if (p.__collection) conds.push({ field: 'collection', op: 'in_collection', value: p.__collection });
  const m = { object: 'object', image_type: 'image_type', filter: 'filter', night: 'session_night', rig: 'rig' };
  for (const [k, field] of Object.entries(m))
    if (p[k]) conds.push({ field, op: field === 'object' || field === 'filter' || field === 'rig' ? 'like' : 'eq', value: p[k] });
  return conds;
}

// Build the filter AST from the visual builder (or return the raw JSON).
function buildAST() {
  if (qb.raw) return qb.rawText.trim() ? JSON.parse(qb.rawText) : {};
  const clauses = [];
  for (const c of qb.conds) {
    if (!c.field) continue;
    if (c.field === 'tag') {
      if (!c.value) continue;
      clauses.push({ op: c.op === 'untagged' ? 'untagged' : 'tagged', value: c.value });
      continue;
    }
    if (c.field === 'collection') {
      if (!c.value) continue;
      clauses.push({ op: 'in_collection', value: c.value });
      continue;
    }
    if (noValue(c.op)) { clauses.push({ field: c.field, op: c.op }); continue; }
    if (c.op === 'between') {
      if (c.value === '' || c.value2 === '') continue;
      clauses.push({ field: c.field, op: 'between', value: [c.value, c.value2] });
    } else if (c.op === 'in') {
      const list = String(c.value).split(',').map(s => s.trim()).filter(Boolean);
      if (list.length) clauses.push({ field: c.field, op: 'in', value: list });
    } else if (c.op === 'like') {
      if (c.value === '' || c.value == null) continue;
      // "matches" is a contains-search unless the user supplies their own
      // wildcard: bare NGC becomes %NGC%, while NGC%_L keeps its pattern.
      let v = String(c.value);
      if (!v.includes('%') && !v.includes('_')) v = '%' + v + '%';
      clauses.push({ field: c.field, op: 'like', value: v });
    } else {
      if (c.value === '' || c.value == null) continue;
      clauses.push({ field: c.field, op: c.op, value: c.value });
    }
  }
  if (qb.cone.on && qb.cone.ra !== '' && qb.cone.dec !== '' && qb.cone.radius !== '')
    clauses.push({ op: 'cone', ra: Number(qb.cone.ra), dec: Number(qb.cone.dec), radius_deg: Number(qb.cone.radius) });
  if (clauses.length === 0) return {};
  if (clauses.length === 1) return clauses[0];
  return { op: qb.combiner, clauses };
}

// Best-effort inverse: load a saved AST back into the visual builder. Falls
// back to the raw JSON editor for shapes the flat builder can't represent.
function loadAST(ast) {
  qb.conds = []; qb.cone = { on: false, ra: '', dec: '', radius: '' }; qb.raw = false;
  const asCond = (n) => {
    if (n.op === 'cone') { qb.cone = { on: true, ra: n.ra, dec: n.dec, radius: n.radius_deg }; return true; }
    if (n.op === 'tagged' || n.op === 'untagged') { qb.conds.push({ field: 'tag', op: n.op, value: n.value }); return true; }
    if (n.op === 'in_collection') { qb.conds.push({ field: 'collection', op: 'in_collection', value: n.value }); return true; }
    if (!n.field || (n.op && ['and', 'or', 'not'].includes(n.op))) return false;
    const op = n.op || 'eq';
    const c = { field: n.field, op };
    if (op === 'between' && Array.isArray(n.value)) { c.value = n.value[0]; c.value2 = n.value[1]; }
    else if (op === 'in' && Array.isArray(n.value)) c.value = n.value.join(', ');
    else if (!noValue(op)) c.value = n.value;
    qb.conds.push(c); return true;
  };
  try {
    if (!ast || (typeof ast === 'object' && Object.keys(ast).length === 0)) { /* empty: all */ }
    else if (ast.op === 'and' || ast.op === 'or') {
      qb.combiner = ast.op;
      for (const cl of ast.clauses) if (!asCond(cl)) throw 0;
    } else if (!asCond(ast)) throw 0;
  } catch (_e) {
    qb.raw = true; qb.rawText = JSON.stringify(ast, null, 2);
  }
}

function condRow(c, i) {
  const fieldSel = el('select', { onchange: (e) => {
      c.field = e.target.value;
      if (c.field === 'tag') c.op = 'tagged';
      else if (c.field === 'collection') c.op = 'in_collection';
      else { const ops = OPS_FOR(FIELDS[c.field]); if (!ops.includes(c.op)) c.op = ops[0]; }
      c.value = ''; renderQuery();
    } },
    el('option', { value: '', selected: !c.field }, 'field…'),
    ...Object.keys(FIELDS).map(f => el('option', { value: f, selected: c.field === f }, f)),
    el('optgroup', { label: 'membership' },
      el('option', { value: 'tag', selected: c.field === 'tag' }, '🏷 tag'),
      el('option', { value: 'collection', selected: c.field === 'collection' }, '📁 collection')));
  const parts = [fieldSel];

  if (c.field === 'tag' || c.field === 'collection') {
    const isTag = c.field === 'tag';
    const opSel = el('select', { onchange: (e) => c.op = e.target.value },
      ...(isTag ? [['tagged', 'is'], ['untagged', 'is not']] : [['in_collection', 'is']])
        .map(([v, l]) => el('option', { value: v, selected: c.op === v }, l)));
    const names = (isTag ? gTags : gCollections).map(x => x.name);
    const valSel = el('select', { onchange: (e) => c.value = e.target.value },
      el('option', { value: '', selected: !c.value }, isTag ? '(pick a tag)' : '(pick a collection)'),
      ...names.map(n => el('option', { value: n, selected: c.value === n }, n)));
    parts.push(opSel, valSel);
  } else {
    const type = FIELDS[c.field] || 'str';
    const opSel = el('select', { onchange: (e) => { c.op = e.target.value; renderQuery(); } },
      ...OPS_FOR(type).map(o => el('option', { value: o, selected: c.op === o }, OP_LABEL[o])));
    parts.push(opSel);
    if (!noValue(c.op)) {
      parts.push(el('input', { placeholder: c.op === 'in' ? 'a, b, c' : 'value', value: c.value != null ? c.value : '', onchange: (e) => c.value = e.target.value }));
      if (c.op === 'between')
        parts.push(el('span', { class: 'muted' }, 'and'),
          el('input', { placeholder: 'value', value: c.value2 != null ? c.value2 : '', onchange: (e) => c.value2 = e.target.value }));
    }
  }
  parts.push(el('button', { class: 'btn ghost', title: 'remove', onclick: () => { qb.conds.splice(i, 1); renderQuery(); } }, '✕'));
  return el('div', { class: 'cond' }, ...parts);
}

function queryBuilder() {
  const combiner = el('select', { onchange: (e) => qb.combiner = e.target.value },
    el('option', { value: 'and', selected: qb.combiner === 'and' }, 'match ALL (AND)'),
    el('option', { value: 'or', selected: qb.combiner === 'or' }, 'match ANY (OR)'));

  const conds = el('div', { class: 'conds' }, ...qb.conds.map((c, i) => condRow(c, i)));

  const coneBox = el('div', { class: 'cone' + (qb.cone.on ? '' : ' off') },
    el('label', {}, el('input', { type: 'checkbox', checked: qb.cone.on, onchange: (e) => { qb.cone.on = e.target.checked; renderQuery(); } }), ' Cone search'),
    qb.cone.on && el('span', { class: 'cone-in' },
      el('input', { placeholder: 'RA°', value: qb.cone.ra, onchange: (e) => qb.cone.ra = e.target.value }),
      el('input', { placeholder: 'Dec°', value: qb.cone.dec, onchange: (e) => qb.cone.dec = e.target.value }),
      el('input', { placeholder: 'radius°', value: qb.cone.radius, onchange: (e) => qb.cone.radius = e.target.value })));

  const visual = el('div', { class: 'builder' + (qb.raw ? ' hidden' : '') },
    el('div', { class: 'row' }, combiner,
      el('button', { class: 'btn', onclick: () => { qb.conds.push({ field: 'object', op: 'like', value: '' }); renderQuery(); } }, '+ condition')),
    conds, coneBox);

  const raw = el('div', { class: 'rawbox' + (qb.raw ? '' : ' hidden') },
    el('textarea', { rows: 8, placeholder: '{ "field": "image_type", "op": "eq", "value": "light" }', onchange: (e) => qb.rawText = e.target.value }, qb.rawText));

  const toggle = el('label', { class: 'adv' },
    el('input', { type: 'checkbox', checked: qb.raw, onchange: (e) => { if (e.target.checked) { try { qb.rawText = JSON.stringify(buildAST(), null, 2); } catch (_e) {} } else { try { loadAST(JSON.parse(qb.rawText || '{}')); } catch (_e) {} } qb.raw = e.target.checked; renderQuery(); } }),
    ' Advanced (raw JSON AST)');

  const sortSel = el('select', { onchange: (e) => qb.sortField = e.target.value },
    ...Object.keys(FIELDS).map(f => el('option', { value: f, selected: qb.sortField === f }, f)));
  const dirSel = el('select', { onchange: (e) => qb.sortDir = e.target.value },
    el('option', { value: 'desc', selected: qb.sortDir === 'desc' }, 'desc'),
    el('option', { value: 'asc', selected: qb.sortDir === 'asc' }, 'asc'));

  return el('div', { class: 'qbuild card' },
    visual, raw,
    el('div', { class: 'row wrap-row' }, toggle,
      el('span', { class: 'sp' }), el('span', { class: 'muted' }, 'sort'), sortSel, dirSel,
      el('button', { class: 'btn primary', onclick: () => { qb.offset = 0; runQuery(); } }, 'Run query'),
      el('button', { class: 'btn', onclick: () => saveDialog() }, 'Save…')));
}

async function runQuery() {
  const host = document.getElementById('qresult');
  if (host) host.replaceChildren(el('div', { class: 'muted' }, 'Running...'));
  let filter;
  try { filter = buildAST(); } catch (e) { if (host) host.replaceChildren(el('div', { class: 'err-msg' }, 'Bad JSON: ' + e.message)); return; }
  try {
    const data = await apiPost('/query', {
      filter, sort: [{ field: qb.sortField, dir: qb.sortDir }], limit: qb.limit, offset: qb.offset,
    });
    qb.total = data.total; qb.lastCount = data.total;
    renderQuery();
    const grid = document.getElementById('qgrid');
    if (grid) {
      grid.replaceChildren(resultsTable(data.frames));
      const from = data.total ? qb.offset + 1 : 0;
      const to = Math.min(qb.offset + qb.limit, data.total);
      document.getElementById('qpager').replaceChildren(
        el('button', { class: 'btn', onclick: () => { if (qb.offset > 0) { qb.offset -= qb.limit; runQuery(); } } }, '← Prev'),
        el('span', {}, `${from}–${to} of ${data.total.toLocaleString()}`),
        el('button', { class: 'btn', onclick: () => { if (qb.offset + qb.limit < data.total) { qb.offset += qb.limit; runQuery(); } } }, 'Next →'),
        el('span', { class: 'muted mono', style: 'margin-left:auto' }, data.where));
    }
  } catch (e) {
    const g = document.getElementById('qgrid');
    if (g) g.replaceChildren(el('div', { class: 'err-msg' }, String(e.message || e)));
  }
}

// ---- Actions bar (acts on the current query's result set) ------------------

function actionsBar() {
  const status = el('div', { class: 'act-status' });
  const setStatus = (...kids) => status.replaceChildren(...kids);
  const actLimit = el('input', { type: 'number', value: 5000, title: 'max frames to act on', style: 'width:6em' });
  const filterOrThrow = () => { try { return buildAST(); } catch (e) { setStatus(el('div', { class: 'err-msg' }, 'Bad JSON: ' + e.message)); throw e; } };
  const busy = (l) => setStatus(el('div', { class: 'muted' }, l));

  // Stage
  const linkMode = el('select', {}, ...['symlink', 'hardlink', 'copy'].map(m => el('option', { value: m }, m)));
  const stageBtn = el('button', { class: 'btn', onclick: async () => {
    let filter; try { filter = filterOrThrow(); } catch (_e) { return; }
    busy('Staging…');
    try { const r = await apiPost('/actions', { op: 'stage', filter, limit: Number(actLimit.value), link_mode: linkMode.value }); setStatus(jobResult(r)); }
    catch (e) { setStatus(el('div', { class: 'err-msg' }, String(e.message || e))); }
  } }, 'Stage');

  // WBPP
  const loadOnly = el('input', { type: 'checkbox' });
  const outDir = el('input', { placeholder: 'output dir (optional)', style: 'width:16em' });
  const kw = el('input', { placeholder: 'grouping keywords', value: 'FILTER prepost', style: 'width:12em' });
  const wbppBtn = el('button', { class: 'btn', onclick: async () => {
    let filter; try { filter = filterOrThrow(); } catch (_e) { return; }
    busy('Building WBPP handoff…');
    try {
      const r = await apiPost('/actions', { op: 'wbpp', filter, limit: Number(actLimit.value), load_only: loadOnly.checked, profile: { output_dir: outDir.value, keywords: kw.value } });
      setStatus(wbppResult(r));
    } catch (e) { setStatus(el('div', { class: 'err-msg' }, String(e.message || e))); }
  } }, 'WBPP handoff');

  // Export
  const fmtSel = el('select', {}, ...['csv', 'json', 'paths'].map(f => el('option', { value: f }, f)));
  const exportBtn = el('button', { class: 'btn', onclick: async () => {
    let filter; try { filter = filterOrThrow(); } catch (_e) { return; }
    busy('Exporting…');
    try {
      const text = await apiPostText('/actions', { op: 'export', format: fmtSel.value, filter, limit: Number(actLimit.value) });
      const ext = fmtSel.value === 'paths' ? 'txt' : fmtSel.value;
      download('starbase-export.' + ext, text, fmtSel.value === 'json' ? 'application/json' : 'text/plain');
      setStatus(el('div', { class: 'ok-msg' }, 'Exported ' + (text.split('\n').filter(Boolean).length) + ' line(s).'));
    } catch (e) { setStatus(el('div', { class: 'err-msg' }, String(e.message || e))); }
  } }, 'Export');

  // Filesystem ops (destructive; dry-run default)
  const fsOp = el('select', {}, ...['copy', 'symlink', 'move', 'trash'].map(o => el('option', { value: o }, o)));
  const fsTarget = el('input', { placeholder: 'target dir (copy/move/symlink)', style: 'width:16em' });
  const dry = el('input', { type: 'checkbox', checked: true });
  const fsBtn = el('button', { class: 'btn danger', onclick: async () => {
    let filter; try { filter = filterOrThrow(); } catch (_e) { return; }
    const op = fsOp.value;
    if (!dry.checked && (op === 'move' || op === 'trash') &&
        !confirm(`Really ${op} the matching frames? This moves files on disk.`)) return;
    busy(op + (dry.checked ? ' (dry run)…' : '…'));
    try {
      const r = await apiPost('/actions', { op, filter, limit: Number(actLimit.value), target: fsTarget.value, dry_run: dry.checked });
      setStatus(jobResult(r));
    } catch (e) { setStatus(el('div', { class: 'err-msg' }, String(e.message || e))); }
  } }, 'Run');

  // Tag / untag the whole matching set (not bounded by act-limit: curation is
  // cheap, reversible metadata).
  const tagSel = el('select', {}, gTags.length
    ? gTags.map(t => el('option', { value: t.id }, t.name))
    : [el('option', { value: '' }, '(create a tag first)')]);
  const tagBtn = el('button', { class: 'btn', onclick: async () => {
    if (!tagSel.value) return; let filter; try { filter = filterOrThrow(); } catch (_e) { return; }
    busy('Tagging…');
    try { const r = await apiPost('/tags/' + tagSel.value + '/members', { filter }); setStatus(el('div', { class: 'ok-msg' }, `tagged ${r.added} frame(s)`)); loadCuration(); }
    catch (e) { setStatus(el('div', { class: 'err-msg' }, String(e.message || e))); }
  } }, 'Tag');
  const untagBtn = el('button', { class: 'btn ghost', onclick: async () => {
    if (!tagSel.value) return; let filter; try { filter = filterOrThrow(); } catch (_e) { return; }
    busy('Untagging…');
    try { const r = await apiDeleteBody('/tags/' + tagSel.value + '/members', { filter }); setStatus(el('div', { class: 'ok-msg' }, `untagged ${r.removed} frame(s)`)); loadCuration(); }
    catch (e) { setStatus(el('div', { class: 'err-msg' }, String(e.message || e))); }
  } }, 'Untag');

  const collSel = el('select', {}, gCollections.length
    ? gCollections.map(c => el('option', { value: c.id }, c.name))
    : [el('option', { value: '' }, '(create a collection first)')]);
  const collBtn = el('button', { class: 'btn', onclick: async () => {
    if (!collSel.value) return; let filter; try { filter = filterOrThrow(); } catch (_e) { return; }
    busy('Adding to collection…');
    try { const r = await apiPost('/collections/' + collSel.value + '/members', { filter }); setStatus(el('div', { class: 'ok-msg' }, `added ${r.added} frame(s)`)); loadCuration(); }
    catch (e) { setStatus(el('div', { class: 'err-msg' }, String(e.message || e))); }
  } }, 'Add');

  return el('div', { class: 'actions card' },
    el('h3', {}, 'Act on this result set'),
    el('div', { class: 'act-row' }, el('b', {}, 'Stage'), linkMode, stageBtn,
      el('span', { class: 'sp' }), el('span', { class: 'muted' }, 'act on ≤'), actLimit, el('span', { class: 'muted' }, 'frames')),
    el('div', { class: 'act-row' }, el('b', {}, 'WBPP'), el('label', {}, loadOnly, ' loadOnly'), kw, outDir, wbppBtn),
    el('div', { class: 'act-row' }, el('b', {}, 'Export'), fmtSel, exportBtn),
    el('div', { class: 'act-row' }, el('b', {}, 'Tag'), tagSel, tagBtn, untagBtn,
      el('span', { class: 'sp' }), el('b', {}, 'Collection'), collSel, collBtn),
    el('div', { class: 'act-row danger-row' }, el('b', {}, 'Filesystem'), fsOp, fsTarget, el('label', {}, dry, ' dry run'), fsBtn),
    status);
}

function jobResult(r) {
  return el('div', { class: 'jobcard' },
    el('div', {}, el('b', {}, (r.type || 'job') + (r.dry_run ? ' (dry run)' : '')),
      ` · job #${r.job_id || '—'} · ${r.done} done, ${r.failed} failed of ${r.total}`),
    r.root && el('div', { class: 'muted mono' }, r.root),
    r.job_id ? el('button', { class: 'btn ghost', onclick: () => { go('jobs'); setTimeout(() => jobDetail(r.job_id), 50); } }, 'view job →') : null);
}

function wbppResult(r) {
  const box = el('div', { class: 'jobcard' },
    el('div', {}, el('b', {}, 'WBPP ' + r.mode), ` · job #${r.job_id} · staged ${r.done}/${r.total}`),
    ...(r.warnings || []).map(w => el('div', { class: 'warn-msg' }, '⚠ ' + w)),
    el('div', { class: 'muted' }, 'Output: ' + fmt(r.output_dir)),
    el('label', { class: 'muted' }, 'Command (run from your desktop session):'),
    el('pre', { class: 'code' }, r.command),
    el('div', { class: 'row' },
      el('button', { class: 'btn', onclick: (e) => copy(r.command, e.target) }, 'Copy command'),
      el('a', { class: 'btn', href: '/api/v1/jobs/' + r.job_id + '/launcher' }, '↓ Download launcher.sh')));
  return box;
}

// ---- Saved queries ---------------------------------------------------------

async function savedQueriesPanel() {
  const host = el('div', { class: 'saved card' }, el('div', { class: 'muted' }, 'Loading saved queries…'));
  try {
    const list = await api('/queries');
    const rows = list.length ? list.map(q => el('div', { class: 'saved-row' },
      el('button', { class: 'link', onclick: () => { loadAST(JSON.parse(q.filter_json || '{}')); qb.offset = 0; renderQuery(); runQuery(); } }, q.name),
      q.last_count != null ? el('span', { class: 'muted' }, `${Number(q.last_count).toLocaleString()} frames`) : el('span', {}),
      el('span', { class: 'muted', title: 'REST pull for the PJSR helper' },
        el('a', { class: 'mono', href: '/api/v1/queries/' + q.id + '/paths', target: '_blank' }, 'paths')),
      el('button', { class: 'btn ghost', title: 'delete', onclick: async () => { if (confirm('Delete saved query "' + q.name + '"?')) { await apiDelete('/queries/' + q.id); renderQuery(); } } }, '✕')))
      : [el('div', { class: 'muted' }, 'No saved queries yet.')];
    host.replaceChildren(el('h3', {}, 'Saved queries'), ...rows);
  } catch (e) { host.replaceChildren(el('div', { class: 'err-msg' }, String(e.message || e))); }
  return host;
}

async function saveDialog() {
  const name = prompt('Save query as:');
  if (!name) return;
  let filter; try { filter = buildAST(); } catch (e) { alert('Bad JSON: ' + e.message); return; }
  try { await apiPost('/queries', { name, filter, sort: [{ field: qb.sortField, dir: qb.sortDir }] }); renderQuery(); }
  catch (e) { alert('Save failed: ' + (e.message || e)); }
}

// ---- Query view assembly ---------------------------------------------------

let queryMounted = false;
function renderQuery() {
  // Re-render only the builder + actions + saved panel; keep the grid host stable.
  const b = document.getElementById('qbuilder');
  if (b) b.replaceChildren(queryBuilder());
  const a = document.getElementById('qactions');
  if (a) a.replaceChildren(actionsBar());
  const s = document.getElementById('qsaved');
  if (s) savedQueriesPanel().then(p => s.replaceChildren(p));
  const c = document.getElementById('qcount');
  if (c) c.textContent = qb.lastCount != null ? `${Number(qb.lastCount).toLocaleString()} frames match` : '';
}

async function query(preset) {
  await loadCuration();
  if (preset) { qb.conds = presetToConds(preset); qb.raw = false; qb.offset = 0; }
  app.replaceChildren(
    el('div', { id: 'qbuilder' }, queryBuilder()),
    el('div', { id: 'qactions' }, actionsBar()),
    el('div', { class: 'row section' }, el('h2', {}, 'Results'), el('span', { id: 'qcount', class: 'muted' })),
    el('div', { id: 'qgrid', class: 'wrap' }, el('div', { class: 'muted' }, 'Run a query to see frames.')),
    el('div', { id: 'qpager', class: 'pager' }),
    el('div', { id: 'qsaved' }));
  queryMounted = true;
  savedQueriesPanel().then(p => { const s = document.getElementById('qsaved'); if (s) s.replaceChildren(p); });
  if (preset) runQuery();
}

// ---- Jobs ------------------------------------------------------------------

async function jobs() {
  app.replaceChildren(el('div', { class: 'muted' }, 'Loading jobs…'));
  try {
    const list = await api('/jobs');
    const tbl = el('table', {},
      el('thead', {}, el('tr', {}, ...['#', 'Type', 'Status', 'Dry', 'Total', 'Done', 'Failed', 'Created'].map(h => el('th', {}, h)))),
      el('tbody', {}, ...list.map(j =>
        el('tr', { onclick: () => jobDetail(j.id) },
          el('td', {}, j.id), el('td', {}, el('span', { class: 'pill ' + j.type }, j.type)),
          el('td', {}, el('span', { class: 'stat ' + j.status }, j.status)),
          el('td', {}, j.dry_run === '1' || j.dry_run === 1 ? 'yes' : ''),
          el('td', { class: 'num' }, j.total), el('td', { class: 'num' }, j.done),
          el('td', { class: 'num' }, Number(j.failed) > 0 ? el('b', { class: 'warn' }, j.failed) : '0'),
          el('td', { class: 'muted' }, fmt(j.created_at))))));
    app.replaceChildren(el('h2', {}, 'Jobs'),
      list.length ? el('div', { class: 'wrap' }, tbl) : el('div', { class: 'muted' }, 'No jobs yet.'));
  } catch (e) { showError(e); }
}

async function jobDetail(id) {
  drawer.classList.remove('hidden');
  drawer.replaceChildren(el('div', { class: 'muted' }, 'Loading…'));
  try {
    const j = await api('/jobs/' + id);
    const items = el('table', {}, el('thead', {}, el('tr', {}, ...['Frame', 'Action', 'Status', 'Src → Dst / detail'].map(h => el('th', {}, h)))),
      el('tbody', {}, ...(j.items || []).map(it => el('tr', {},
        el('td', {}, fmt(it.frame_id)), el('td', {}, it.action),
        el('td', {}, el('span', { class: 'stat ' + it.status }, it.status)),
        el('td', { class: 'muted mono' }, it.detail || `${fmt(it.src, '')} → ${fmt(it.dst, '')}`)))));
    drawer.replaceChildren(
      el('button', { class: 'btn close', onclick: () => drawer.classList.add('hidden') }, '✕'),
      el('h3', {}, `Job #${j.id} · ${j.type}`),
      el('div', { class: 'muted' }, `${j.status} · ${j.done}/${j.total} done, ${j.failed} failed`),
      j.type === 'wbpp' ? el('a', { class: 'btn', href: '/api/v1/jobs/' + j.id + '/launcher' }, '↓ Download launcher.sh') : null,
      el('h2', {}, `Items (${(j.items || []).length})`),
      el('div', { class: 'hdr' }, items));
  } catch (e) { drawer.replaceChildren(el('div', { class: 'err-msg' }, String(e.message || e))); }
}

// ---- Frame detail drawer ---------------------------------------------------

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

    const parts = [
      el('button', { class: 'btn close', onclick: () => drawer.classList.add('hidden') }, '✕'),
      el('h3', {}, fr.filename),
      el('div', { class: 'muted', style: 'word-break:break-all' }, fr.abs_path),
      kv];

    // Tags and collection membership (chips).
    if ((fr.tags && fr.tags.length) || (fr.collections && fr.collections.length)) {
      const chips = el('div', { class: 'chips' });
      for (const t of (fr.tags || []))
        chips.append(el('span', { class: 'tagchip', style: t.color ? `border-color:${t.color};color:${t.color}` : '' }, '🏷 ' + t.name));
      for (const c of (fr.collections || []))
        chips.append(el('span', { class: 'tagchip coll', onclick: () => { drawer.classList.add('hidden'); go('query', { __collection: c.name }); } }, '📁 ' + c.name));
      parts.push(chips);
    }

    // Sidecars (M9)
    if (fr.artifacts && fr.artifacts.length) {
      parts.push(el('h2', {}, `Sidecars (${fr.artifacts.length})`),
        el('div', { class: 'wrap' }, el('table', {}, el('tbody', {}, ...fr.artifacts.map(a =>
          el('tr', {}, el('td', {}, el('span', { class: 'pill ' + a.kind }, a.kind)),
            el('td', { class: 'muted mono' }, a.rel_path)))))));
    }

    // Calibration match (M6), for lights, loaded on demand.
    if (fr.image_type === 'light') {
      const calBox = el('div', {});
      parts.push(el('h2', {}, 'Calibration'),
        el('button', { class: 'btn', onclick: async () => {
          calBox.replaceChildren(el('div', { class: 'muted' }, 'Matching…'));
          try {
            const groups = await api('/frames/' + id + '/calibration?limit=5');
            if (!groups.length) { calBox.replaceChildren(el('div', { class: 'muted' }, 'No calibration rules matched.')); return; }
            calBox.replaceChildren(...groups.map(g => el('div', { class: 'calgroup' },
              el('div', {}, el('b', {}, g.target_type), ` · ${g.total} candidate(s) · rule ${g.rule}`),
              g.warning ? el('div', { class: 'warn-msg' }, '⚠ ' + g.warning) : null,
              el('table', {}, el('tbody', {}, ...g.candidates.slice(0, 5).map(c => el('tr', {},
                el('td', {}, c.is_master ? el('span', { class: 'pill master' }, 'master') : ''),
                el('td', { class: 'muted mono' }, c.filename),
                el('td', { class: 'muted' }, c.reason))))))));
          } catch (e) { calBox.replaceChildren(el('div', { class: 'err-msg' }, String(e.message || e))); }
        } }, 'Find calibration'), calBox);
    }

    parts.push(el('h2', {}, `Header (${(fr.keywords || []).length} cards)`),
      el('div', { class: 'hdr' }, el('table', {}, el('tbody', {}, ...(fr.keywords || []).map(k =>
        el('tr', {}, el('td', {}, k.keyword), el('td', {}, fmt(k.value)),
          el('td', { class: 'muted' }, fmt(k.comment, ''))))))));
    drawer.replaceChildren(...parts);
  } catch (e) { drawer.replaceChildren(el('div', { class: 'err-msg' }, String(e.message || e))); }
}

// ---- Roots & Settings ------------------------------------------------------

async function runScan(label, statusEl) {
  statusEl.replaceChildren(el('span', { class: 'muted' }, label ? `scanning ${label}…` : 'scanning all roots…'));
  try {
    const q = label ? '?root=' + encodeURIComponent(label) : '';
    const results = await apiPost('/scan' + q, {});
    const parts = results.map(x => `${x.root}: ${x.added} added, ${x.updated} updated, ${x.unchanged} same, ${x.frames} frames, ${x.sidecars} sidecars (${x.ms} ms)`);
    statusEl.replaceChildren(el('span', { class: 'ok-msg' }, parts.join(' · ') || 'nothing to scan'));
  } catch (e) { statusEl.replaceChildren(el('span', { class: 'err-msg' }, String(e.message || e))); }
}

function addRootForm() {
  const label = el('input', { placeholder: 'label, e.g. lights', style: 'width:10em' });
  const path = el('input', { placeholder: '/absolute/path', style: 'flex:1;min-width:16em' });
  const writable = el('input', { type: 'checkbox' });
  const status = el('span', {});
  return el('div', { class: 'card' },
    el('h3', {}, 'Add a root'),
    el('div', { class: 'row' }, label, path,
      el('label', {}, writable, ' writable'),
      el('button', { class: 'btn primary', onclick: async () => {
        if (!label.value.trim() || !path.value.trim()) { status.replaceChildren(el('span', { class: 'err-msg' }, 'label and path are required')); return; }
        status.replaceChildren(el('span', { class: 'muted' }, 'probing filesystem…'));
        try {
          const r = await apiPost('/roots', { label: label.value.trim(), path: path.value.trim(), writable: writable.checked });
          status.replaceChildren(el('span', { class: 'ok-msg' }, `added #${r.id}: ${r.fs_type}, ${r.watchable ? 'inotify' : 'poll'}, ${truthy(r.case_sensitive) ? 'case-sensitive' : 'case-folding'}`));
          setTimeout(roots, 800);
        } catch (e) { status.replaceChildren(el('span', { class: 'err-msg' }, String(e.message || e))); }
      } }, 'Add')),
    el('div', { class: 'muted', style: 'margin-top:.4rem' }, 'Registering probes the filesystem for type, case-folding, and inotify support, then indexes on the next scan.'),
    status);
}

function rootCard(r) {
  const enabled = el('input', { type: 'checkbox', checked: truthy(r.enabled) });
  const writable = el('input', { type: 'checkbox', checked: truthy(r.writable) });
  const watch = el('select', {}, ...['auto', 'inotify', 'poll', 'off'].map(m => el('option', { value: m, selected: r.watch_mode === m }, m)));
  const interval = el('input', { type: 'number', value: r.scan_interval_s, style: 'width:7em' });
  const settle = el('input', { type: 'number', value: r.settle_seconds, style: 'width:6em' });
  const globs = el('textarea', { rows: 2, placeholder: '_gsdata_\n*.tmp\n*.part' }, fmt(r.ignore_globs, ''));
  const status = el('span', {});
  return el('div', { class: 'card rootcard' },
    el('div', { class: 'row section' },
      el('div', {}, el('b', {}, r.label), ' ', el('span', { class: 'stat ' + r.last_scan_status }, r.last_scan_status),
        r.last_scan_end ? el('span', { class: 'muted' }, ' · last ' + r.last_scan_end) : null),
      el('div', { class: 'muted' }, `${Number(r.file_count).toLocaleString()} files${r.fs_type ? ' · ' + r.fs_type : ''}`)),
    el('div', { class: 'muted mono', style: 'word-break:break-all;margin:.2rem 0' }, r.path),
    r.last_scan_error ? el('div', { class: 'warn-msg' }, '⚠ ' + r.last_scan_error) : null,
    el('div', { class: 'rootgrid' },
      el('label', {}, enabled, ' enabled'),
      el('label', {}, writable, ' writable (allow filesystem ops)'),
      el('span', {}, 'watch ', watch),
      el('span', {}, 'scan every ', interval, ' s'),
      el('span', {}, 'settle ', settle, ' s')),
    el('label', { class: 'muted', style: 'display:block;margin-top:.5rem' }, 'ignore globs (one per line)'),
    globs,
    el('div', { class: 'row', style: 'margin-top:.5rem' },
      el('button', { class: 'btn primary', onclick: async () => {
        status.replaceChildren(el('span', { class: 'muted' }, 'saving…'));
        try {
          await apiPatch('/roots/' + encodeURIComponent(r.label), {
            enabled: enabled.checked, writable: writable.checked, watch_mode: watch.value,
            scan_interval_s: Number(interval.value), settle_seconds: Number(settle.value),
            ignore_globs: globs.value,
          });
          status.replaceChildren(el('span', { class: 'ok-msg' }, 'saved'));
        } catch (e) { status.replaceChildren(el('span', { class: 'err-msg' }, String(e.message || e))); }
      } }, 'Save'),
      el('button', { class: 'btn', onclick: () => runScan(r.label, status) }, 'Scan now'),
      el('button', { class: 'btn danger', onclick: async () => {
        if (!confirm(`Remove root "${r.label}"? Its indexed rows are deleted (files on disk are untouched).`)) return;
        try { await apiDelete('/roots/' + encodeURIComponent(r.label)); roots(); }
        catch (e) { status.replaceChildren(el('span', { class: 'err-msg' }, String(e.message || e))); }
      } }, 'Remove'),
      status));
}

async function roots() {
  app.replaceChildren(el('div', { class: 'muted' }, 'Loading roots…'));
  try {
    const list = await api('/roots');
    const scanStatus = el('span', {});
    const host = el('div', {},
      el('div', { class: 'row section' }, el('h2', {}, 'Roots & Settings'),
        el('div', { class: 'row' }, el('button', { class: 'btn primary', onclick: () => runScan(null, scanStatus) }, 'Scan all'), scanStatus)),
      addRootForm());
    if (!list.length) host.append(el('div', { class: 'muted' }, 'No roots registered. Add one above.'));
    for (const r of list) host.append(rootCard(r));
    app.replaceChildren(host);
  } catch (e) { showError(e); }
}

// ---- Collections & Tags ----------------------------------------------------

function tagsCard() {
  const name = el('input', { placeholder: 'tag name', style: 'width:10em' });
  const color = el('input', { type: 'color', value: '#58a6ff', title: 'color' });
  const desc = el('input', { placeholder: 'description (optional)', style: 'flex:1;min-width:12em' });
  const status = el('span', {});
  const rows = gTags.length ? gTags.map(t => el('div', { class: 'curow' },
    el('span', { class: 'tagchip', style: t.color ? `border-color:${t.color};color:${t.color}` : '' }, t.name),
    el('span', { class: 'muted' }, `${Number(t.count || 0).toLocaleString()} frames`),
    el('button', { class: 'link', onclick: () => go('query', { __tag: t.name }) }, 'query →'),
    el('button', { class: 'btn ghost', title: 'delete tag', onclick: async () => {
      if (!confirm(`Delete tag "${t.name}"? Frames keep their files; only the tag is removed.`)) return;
      try { await apiDelete('/tags/' + t.id); tags(); } catch (e) { status.replaceChildren(el('span', { class: 'err-msg' }, String(e.message || e))); }
    } }, '✕')))
    : [el('div', { class: 'muted' }, 'No tags yet. Create one, then tag a query result from the Query tab.')];
  return el('div', { class: 'card' }, el('h3', {}, 'Tags'),
    el('div', { class: 'row' }, name, color, desc,
      el('button', { class: 'btn primary', onclick: async () => {
        if (!name.value.trim()) { status.replaceChildren(el('span', { class: 'err-msg' }, 'name is required')); return; }
        try { await apiPost('/tags', { name: name.value.trim(), color: color.value, description: desc.value }); tags(); }
        catch (e) { status.replaceChildren(el('span', { class: 'err-msg' }, String(e.message || e))); }
      } }, 'Create tag'), status),
    el('div', { class: 'culist' }, ...rows));
}

function collectionsCard() {
  const name = el('input', { placeholder: 'collection name', style: 'width:14em' });
  const desc = el('input', { placeholder: 'description (optional)', style: 'flex:1;min-width:12em' });
  const status = el('span', {});
  const rows = gCollections.length ? gCollections.map(c => el('div', { class: 'curow' },
    el('b', {}, c.name),
    el('span', { class: 'muted' }, `${Number(c.count || 0).toLocaleString()} frames`),
    el('button', { class: 'link', onclick: () => collectionDetail(c.id) }, 'view'),
    el('button', { class: 'link', onclick: () => go('query', { __collection: c.name }) }, 'query →'),
    el('button', { class: 'btn ghost', title: 'delete collection', onclick: async () => {
      if (!confirm(`Delete collection "${c.name}"? Frames keep their files.`)) return;
      try { await apiDelete('/collections/' + c.id); tags(); } catch (e) { status.replaceChildren(el('span', { class: 'err-msg' }, String(e.message || e))); }
    } }, '✕')))
    : [el('div', { class: 'muted' }, 'No collections yet. Create one, then add a query result from the Query tab.')];
  return el('div', { class: 'card' }, el('h3', {}, 'Collections'),
    el('div', { class: 'row' }, name, desc,
      el('button', { class: 'btn primary', onclick: async () => {
        if (!name.value.trim()) { status.replaceChildren(el('span', { class: 'err-msg' }, 'name is required')); return; }
        try { await apiPost('/collections', { name: name.value.trim(), description: desc.value }); tags(); }
        catch (e) { status.replaceChildren(el('span', { class: 'err-msg' }, String(e.message || e))); }
      } }, 'Create collection'), status),
    el('div', { class: 'culist' }, ...rows));
}

async function collectionDetail(id) {
  drawer.classList.remove('hidden');
  drawer.replaceChildren(el('div', { class: 'muted' }, 'Loading…'));
  try {
    const c = await api('/collections/' + id);
    drawer.replaceChildren(
      el('button', { class: 'btn close', onclick: () => drawer.classList.add('hidden') }, '✕'),
      el('h3', {}, '📁 ' + c.name),
      c.description ? el('div', { class: 'muted' }, c.description) : null,
      el('div', { class: 'row', style: 'margin:.5rem 0' },
        el('button', { class: 'btn', onclick: () => { drawer.classList.add('hidden'); go('query', { __collection: c.name }); } }, 'Open in Query'),
        el('span', { class: 'muted' }, `${c.frames.length} frames`)),
      el('div', { class: 'hdr' }, resultsTable(c.frames)));
  } catch (e) { drawer.replaceChildren(el('div', { class: 'err-msg' }, String(e.message || e))); }
}

async function tags() {
  app.replaceChildren(el('div', { class: 'muted' }, 'Loading…'));
  try {
    await loadCuration();
    app.replaceChildren(el('h2', {}, 'Collections & Tags'), tagsCard(), collectionsCard());
  } catch (e) { showError(e); }
}

// ---- shell / router --------------------------------------------------------

function setConn(t) { const c = document.getElementById('conn'); c.textContent = t; c.classList.remove('err'); }
function showError(e) {
  app.replaceChildren(el('div', { class: 'err-msg' }, 'Error: ' + (e.message || e)));
  const c = document.getElementById('conn'); c.textContent = 'disconnected'; c.classList.add('err');
}

const VIEWS = { dashboard, browse, query, jobs, roots, tags };
let current = 'dashboard';
function go(view, preset) {
  if (!VIEWS[view]) view = 'dashboard';
  current = view;
  if (location.hash.slice(1) !== view) history.replaceState(null, '', '#' + view);
  document.querySelectorAll('nav button').forEach(b => b.classList.toggle('active', b.dataset.view === view));
  drawer.classList.add('hidden');
  VIEWS[view](preset);
}
document.querySelectorAll('nav button').forEach(b => b.onclick = () => go(b.dataset.view));
document.getElementById('tokenBtn').onclick = () => {
  const t = prompt('API token (blank to clear). Only needed if the server sets SB_API_TOKEN.', token());
  if (t === null) return;
  if (t) localStorage.setItem('sb_token', t); else localStorage.removeItem('sb_token');
};
window.addEventListener('hashchange', () => { const v = location.hash.slice(1); if (v && v !== current) go(v); });
go(location.hash.slice(1) || 'dashboard');
