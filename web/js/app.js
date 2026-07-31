// ---------------------------------------------------------------------------
// Author:        David Gilinsky
// File:          web/js/app.js
// Purpose:       StarBase web UI. Dashboard, browse, the query builder (filter
//                AST) with saved queries and an actions bar that stages / hands
//                off to WBPP / exports / runs filesystem ops on the result set,
//                the jobs ledger, and the frame detail drawer (header,
//                calibration match, sidecars). Vanilla JS, no build step.
// Created:       2026-07-24
// Last Modified: 2026-07-31
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
const apiPut = (path, body) => fetch('/api/v1' + path, {
  method: 'PUT', headers: authHeaders({ 'Content-Type': 'application/json' }),
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

// Distinct values per facet (rig, camera, filter, object) for the Browse
// dropdowns. Cached; a page reload picks up values discovered by a later scan.
let gFacets = { rig: [], camera: [], filter: [], object: [] };
async function loadFacets() {
  const fields = ['rig', 'camera', 'filter', 'object'];
  try {
    const lists = await Promise.all(fields.map(f => api('/facets/' + f)));
    fields.forEach((f, i) => { gFacets[f] = lists[i]; });
  } catch (_e) { /* keep whatever we had */ }
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

// Fields already shown by a fixed column, so a query on one adds no new column.
const BASE_RESULT_FIELDS = new Set(['image_type', 'object', 'filter', 'session_night', 'exposure_s', 'gain', 'rig']);

// `extraFields` (the fields a query filtered on) become extra columns between Rig
// and File, so the results reflect exactly what was queried.
function resultsTable(frames, extraFields) {
  const extra = (extraFields || []).filter(f => !BASE_RESULT_FIELDS.has(f));
  const headers = ['Type', 'Target', 'Filter', 'Night', 'Exp', 'Gain', 'Rig', ...extra, 'File'];
  return el('table', {},
    el('thead', {}, el('tr', {}, ...headers.map(h => el('th', {}, h)))),
    el('tbody', {}, ...frames.map(fr =>
      el('tr', { class: fr.file_status === 'missing' ? 'missing' : '', onclick: () => detail(fr.frame_id) },
        el('td', {}, el('span', { class: 'pill ' + fr.image_type }, fr.image_type)),
        el('td', {}, fmt(fr.object)), el('td', {}, fmt(fr.filter)),
        el('td', {}, fmt(fr.session_night)), el('td', { class: 'num' }, num(fr.exposure_s, 0) + 's'),
        el('td', { class: 'num' }, fmt(fr.gain)), el('td', {}, fmt(fr.rig)),
        ...extra.map(f => el('td', { class: FIELDS[f] === 'num' ? 'num' : '' }, fmt(fr[f]))),
        el('td', { class: 'muted' },
          fr.file_status === 'missing' ? el('span', { class: 'pill missing' }, 'missing') : null,
          fr.filename)))));
}

// Permanently remove the index rows for files the rescan soft-deleted as
// 'missing' (already gone from disk). Dry-runs and confirms first. Returns the
// prune result, {empty:true} if there was nothing to prune, or null if the
// operator cancelled at the confirm.
async function pruneMissing(days = 0) {
  const dry = await apiPost('/db/prune-missing', { older_than_days: days, dry_run: true });
  if (!dry.files) return { empty: true };
  if (!confirm(`Delete ${Number(dry.files).toLocaleString()} missing file(s) and ${Number(dry.frames).toLocaleString()} frame(s)${days ? ' not seen in ' + days + '+ days' : ''}?\n\nThese files are already gone from disk; this removes their index rows, frames, and tag/collection memberships. Not reversible.`)) return null;
  return apiPost('/db/prune-missing', { older_than_days: days, dry_run: false });
}

// ---- Browse (simple filter bar over GET /frames) ---------------------------

const bstate = { filters: {}, offset: 0, limit: 50, total: 0, showMissing: false, missing: 0 };

async function browse(preset) {
  if (preset) { bstate.filters = { ...preset }; bstate.offset = 0; }
  if (!gFacets.rig.length && !gFacets.object.length) await loadFacets();
  const f = bstate.filters;
  const setF = (name, v) => { if (v) f[name] = v; else delete f[name]; bstate.offset = 0; browse(); };
  const input = (name, ph) => el('input', {
    name, placeholder: ph, value: f[name] || '', onchange: (e) => setF(name, e.target.value)
  });
  const select = (name, anyLabel, opts) => {
    const values = opts.slice();
    if (f[name] && !values.includes(f[name])) values.unshift(f[name]);  // keep an unknown preset visible
    return el('select', { onchange: (e) => setF(name, e.target.value) },
      el('option', { value: '', selected: !f[name] }, anyLabel),
      ...values.map(v => el('option', { value: v, selected: f[name] === v }, v)));
  };
  // A typeable dropdown: an input backed by a datalist, so a value can be picked
  // or typed. Used for object, which has more distinct values than a plain menu.
  const combo = (name, ph, opts) => {
    const listId = 'dl_' + name;
    return el('span', { class: 'combo' },
      el('input', { name, placeholder: ph, value: f[name] || '', list: listId, onchange: (e) => setF(name, e.target.value) }),
      el('datalist', { id: listId }, ...opts.map(v => el('option', { value: v }))));
  };

  const bar = el('div', { class: 'filters' },
    combo('object', 'target', gFacets.object),
    select('image_type', 'any type', ['light', 'dark', 'flat', 'bias', 'darkflat', 'master', 'unknown']),
    select('filter', 'any filter', gFacets.filter),
    input('night', 'night YYYY-MM-DD'),
    select('camera', 'any camera', gFacets.camera),
    select('rig', 'any rig', gFacets.rig),
    el('label', { class: 'chk', title: 'Include frames whose file is gone from disk' },
      el('input', { type: 'checkbox', checked: bstate.showMissing,
        onchange: (e) => { bstate.showMissing = e.target.checked; bstate.offset = 0; browse(); } }),
      ' show missing'),
    el('button', { class: 'btn', onclick: () => { bstate.filters = {}; bstate.offset = 0; browse(); } }, 'Clear'),
    el('button', { class: 'btn primary', onclick: () => go('query', { ...f }) }, 'Open in Query →'));

  const grid = el('div', { class: 'wrap' }, el('div', { class: 'muted' }, 'Loading...'));
  const pager = el('div', { class: 'pager' });
  app.replaceChildren(bar, grid, pager);
  try {
    const params = { limit: bstate.limit, offset: bstate.offset, ...f };
    if (bstate.showMissing) params.include_missing = 1;
    const data = await api('/frames?' + new URLSearchParams(params).toString());
    bstate.total = data.total;
    bstate.missing = data.missing_hidden || 0;
    grid.replaceChildren(resultsTable(data.frames));
    const from = bstate.total ? bstate.offset + 1 : 0;
    const to = Math.min(bstate.offset + bstate.limit, bstate.total);
    const kids = [
      el('button', { class: 'btn', onclick: () => { if (bstate.offset > 0) { bstate.offset -= bstate.limit; browse(); } } }, '← Prev'),
      el('span', {}, `${from}–${to} of ${bstate.total.toLocaleString()}`),
      el('button', { class: 'btn', onclick: () => { if (bstate.offset + bstate.limit < bstate.total) { bstate.offset += bstate.limit; browse(); } } }, 'Next →')];
    // Deleted-but-not-pruned files are hidden by default; surface the count so a
    // delete-then-scan is legible, with one-click reveal and (admin) prune.
    if (bstate.missing > 0 && !bstate.showMissing) {
      const note = el('span', { class: 'missing-note' },
        `· ${bstate.missing.toLocaleString()} missing on disk · `,
        el('a', { href: '#', onclick: (e) => { e.preventDefault(); bstate.showMissing = true; bstate.offset = 0; browse(); } }, 'show'));
      if (me && me.role === 'admin')
        note.append(' · ', el('a', { href: '#', onclick: async (e) => {
          e.preventDefault();
          const r = await pruneMissing(0);
          if (r && !r.empty) browse();
        } }, 'prune…'));
      kids.push(note);
    }
    pager.replaceChildren(...kids);
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
  const m = { object: 'object', image_type: 'image_type', filter: 'filter', night: 'session_night', rig: 'rig', site: 'site', file_status: 'file_status' };
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
      grid.replaceChildren(resultsTable(data.frames, data.query_fields));
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

// ---- Server-side directory picker (modal) ----------------------------------

// Opens a modal that browses the daemon's filesystem; onPick(path) fires with
// the chosen directory. Navigate by clicking folders or "..", type a path and
// press Enter to jump, or edit the box directly before choosing.
function pathBrowser(onPick, start) {
  const overlay = el('div', { class: 'modal-overlay' });
  const close = () => overlay.remove();
  const cur = el('input', { class: 'mono', value: start || '/', style: 'flex:1;min-width:18em' });
  const listEl = el('div', { class: 'fslist' });
  const status = el('div', { class: 'muted' });

  async function load(path) {
    status.textContent = 'loading…'; listEl.replaceChildren();
    try {
      const d = await api('/fs/list?path=' + encodeURIComponent(path || '/'));
      cur.value = d.path;
      const rows = [];
      if (d.parent != null) rows.push(el('div', { class: 'fsrow up', onclick: () => load(d.parent) }, '⤴  ..'));
      for (const e of d.entries) rows.push(el('div', { class: 'fsrow', onclick: () => load(e.path) }, '📁  ' + e.name));
      listEl.replaceChildren(...(rows.length ? rows : [el('div', { class: 'muted', style: 'padding:.4rem' }, '(no subdirectories)')]));
      status.textContent = '';
    } catch (e) { status.textContent = String(e.message || e); }
  }
  cur.addEventListener('keydown', (ev) => { if (ev.key === 'Enter') load(cur.value.trim()); });

  overlay.append(el('div', { class: 'modal' },
    el('div', { class: 'row', style: 'justify-content:space-between' },
      el('b', {}, 'Choose a directory'),
      el('button', { class: 'btn ghost', onclick: close }, '✕')),
    el('div', { class: 'row' }, cur, el('button', { class: 'btn', onclick: () => load(cur.value.trim()) }, 'Go')),
    listEl, status,
    el('div', { class: 'row', style: 'justify-content:flex-end;margin-top:.4rem' },
      el('button', { class: 'btn', onclick: close }, 'Cancel'),
      el('button', { class: 'btn primary', onclick: () => { onPick(cur.value.trim()); close(); } }, 'Use this directory'))));
  overlay.onclick = (ev) => { if (ev.target === overlay) close(); };
  document.body.append(overlay);
  load(start || '/');
}

// ---- Roots & Settings ------------------------------------------------------

// ---- live scan progress (shared by Roots and Database) ---------------------

let scanPoll = null;
function stopScanPoll() { if (scanPoll) { clearInterval(scanPoll); scanPoll = null; } }

const scanCount = (label, v, cls) =>
  el('span', { class: 'scancount ' + (cls || '') }, el('b', {}, Number(v || 0).toLocaleString()), ' ' + label);

function scanStatusView(s) {
  if (!s.active && !s.finished) return el('div', { class: 'muted' }, 'No scan running.');
  const parts = [];
  if (s.active) {
    parts.push(el('div', { class: 'row' },
      el('span', { class: 'spinner' }),
      el('b', {}, `Scanning ${fmt(s.root, '')}`),
      s.total_roots > 1 ? el('span', { class: 'muted' }, `· root ${s.done_roots + 1} of ${s.total_roots}`) : null,
      el('span', { class: 'muted' }, `· ${Math.round((s.elapsed_ms || 0) / 1000)}s`)));
    parts.push(el('div', { class: 'scancounts' },
      scanCount('seen', s.seen), scanCount('added', s.added), scanCount('updated', s.updated),
      scanCount('unchanged', s.unchanged), scanCount('frames', s.frames), scanCount('sidecars', s.sidecars),
      Number(s.error) ? scanCount('errors', s.error, 'warn') : null));
  } else {
    parts.push(el('div', { class: 'ok-msg' }, 'Scan complete.'));
  }
  if (s.results && s.results.length) {
    parts.push(el('div', { class: 'wrap', style: 'margin-top:.5rem' }, el('table', {},
      el('thead', {}, el('tr', {}, ...['Root', 'Added', 'Updated', 'Unchanged', 'Frames', 'Sidecars', 'ms'].map(h => el('th', {}, h)))),
      el('tbody', {}, ...s.results.map(r => r.failed
        ? el('tr', {}, el('td', {}, r.root), el('td', { class: 'err-msg', colspan: 6 }, r.failed))
        : el('tr', {}, el('td', {}, r.root), el('td', { class: 'num' }, r.added), el('td', { class: 'num' }, r.updated),
            el('td', { class: 'num' }, r.unchanged), el('td', { class: 'num' }, r.frames),
            el('td', { class: 'num' }, r.sidecars), el('td', { class: 'num muted' }, r.ms)))))));
  }
  return el('div', {}, ...parts);
}

// Poll /scan/status into `host` until the scan is idle. onDone(status) optional.
function watchScan(host, onDone) {
  stopScanPoll();
  const tick = async () => {
    try {
      const s = await api('/scan/status');
      host.replaceChildren(scanStatusView(s));
      if (!s.active) { stopScanPoll(); if (onDone) onDone(s); }
    } catch (e) { host.replaceChildren(el('div', { class: 'err-msg' }, String(e.message || e))); stopScanPoll(); }
  };
  tick();
  scanPoll = setInterval(tick, 1000);
}

// Start a scan (one root or all) and show its live progress in `host`.
async function runScan(label, host) {
  host.replaceChildren(el('span', { class: 'muted' }, 'starting…'));
  try {
    await apiPost('/scan' + (label ? '?root=' + encodeURIComponent(label) : ''), {});
    watchScan(host);
  } catch (e) { host.replaceChildren(el('span', { class: 'err-msg' }, String(e.message || e))); }
}

function addRootForm() {
  const label = el('input', { placeholder: 'label, e.g. lights', style: 'width:10em' });
  const path = el('input', { placeholder: '/absolute/path', style: 'flex:1;min-width:16em' });
  const writable = el('input', { type: 'checkbox' });
  const status = el('span', {});
  return el('div', { class: 'card' },
    el('h3', {}, 'Add a root'),
    el('div', { class: 'row' }, label, path,
      el('button', { class: 'btn', onclick: () => pathBrowser((picked) => { path.value = picked; }, path.value.trim() || '/') }, 'Browse…'),
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

// ---- Database --------------------------------------------------------------

async function database() {
  app.replaceChildren(el('div', { class: 'muted' }, 'Loading…'));
  try {
    const j = await api('/db');

    const info = el('div', { class: 'card' }, el('h3', {}, 'Database'),
      el('div', { class: 'kv' },
        el('div', { class: 'k' }, 'Name'), el('div', { class: 'v' }, fmt(j.database)),
        el('div', { class: 'k' }, 'Schema version'), el('div', { class: 'v' }, fmt(j.schema_version)),
        el('div', { class: 'k' }, 'Server'), el('div', { class: 'v' }, fmt(j.server)),
        el('div', { class: 'k' }, 'Views'), el('div', { class: 'v' }, (j.views || []).join(', ')),
        el('div', { class: 'k' }, 'Total rows'), el('div', { class: 'v' }, Number(j.total_rows).toLocaleString())));

    const err = (j.file_status || []).find(f => f.status === 'error');
    const statusCard = el('div', { class: 'card' }, el('h3', {}, 'Files by status'),
      el('div', { class: 'row', style: 'gap:.8rem' }, ...(j.file_status || []).map(f =>
        el('span', { class: 'stat ' + (f.status === 'error' ? 'failed' : f.status === 'ok' ? 'ok' : 'skipped') },
          `${f.status}: ${Number(f.count).toLocaleString()}`))),
      err ? el('button', { class: 'link', style: 'margin-top:.5rem', onclick: () => go('query', { file_status: 'error' }) }, 'browse errored files →') : null);

    const rowsSorted = [...j.tables].sort((a, b) => b.rows - a.rows);
    const tbl = el('table', {},
      el('thead', {}, el('tr', {}, el('th', {}, 'Table'), el('th', { class: 'num' }, 'Rows'),
        el('th', { class: 'num' }, 'Data MB'), el('th', { class: 'num' }, 'Index MB'))),
      el('tbody', {}, ...rowsSorted.map(t => el('tr', {},
        el('td', { class: 'mono' }, t.name), el('td', { class: 'num' }, Number(t.rows).toLocaleString()),
        el('td', { class: 'num' }, num(t.data_mb, 2)), el('td', { class: 'num' }, num(t.index_mb, 2))))));
    const tablesCard = el('div', { class: 'card' }, el('h3', {}, `Tables (${j.tables.length})`),
      el('div', { class: 'wrap' }, tbl));

    const scanHost = el('div', { class: 'scanhost' });
    const scanCard = el('div', { class: 'card' },
      el('div', { class: 'row section' }, el('h3', {}, 'Scanning'),
        el('button', { class: 'btn primary', onclick: () => runScan(null, scanHost) }, 'Scan all roots')),
      scanHost);

    const mstatus = el('span', {});
    // Prune: permanently remove files the rescan soft-deleted as 'missing'.
    const missingCount = Number(((j.file_status || []).find(f => f.status === 'missing') || {}).count || 0);
    const daysIn = el('input', { type: 'number', min: '0', placeholder: 'days (0=all)', style: 'width:8em' });
    const pruneBtn = (me && me.role === 'admin') ? el('button', { class: 'btn ghost', onclick: async () => {
      const days = Math.max(0, Number(daysIn.value) || 0);
      mstatus.replaceChildren(el('span', { class: 'muted' }, 'checking…'));
      try {
        const r = await pruneMissing(days);
        if (!r) { mstatus.replaceChildren(); return; }                        // cancelled
        if (r.empty) { mstatus.replaceChildren(el('span', { class: 'ok-msg' }, 'no missing files to prune')); return; }
        mstatus.replaceChildren(el('span', { class: 'ok-msg' }, `pruned ${Number(r.pruned).toLocaleString()} file(s), ${Number(r.frames).toLocaleString()} frame(s)`));
        database();
      } catch (e) { mstatus.replaceChildren(el('span', { class: 'err-msg' }, String(e.message || e))); }
    } }, `Prune missing…${missingCount ? ' (' + missingCount.toLocaleString() + ')' : ''}`) : null;
    const maint = el('div', { class: 'card' }, el('h3', {}, 'Maintenance'),
      el('div', { class: 'row' },
        el('button', { class: 'btn', onclick: async () => {
          mstatus.replaceChildren(el('span', { class: 'muted' }, 're-seeding…'));
          try { const r = await apiPost('/db/seed', {}); mstatus.replaceChildren(el('span', { class: 'ok-msg' }, `re-seeded header mapping (${r.statements} statements)`)); }
          catch (e) { mstatus.replaceChildren(el('span', { class: 'err-msg' }, String(e.message || e))); }
        } }, 'Re-seed header mapping'),
        me && me.role === 'admin' ? el('button', { class: 'btn', onclick: () => go('targets') }, 'Review target names →') : null,
        me && me.role === 'admin' ? el('button', { class: 'btn', onclick: () => go('equipment') }, 'Manage equipment →') : null,
        pruneBtn ? daysIn : null, pruneBtn,
        mstatus),
      el('div', { class: 'muted', style: 'margin-top:.4rem' }, 'Re-seeding re-applies the default header mapping (idempotent). Target-name normalization canonicalizes OBJECT values (M101 → “M 101”). Pruning permanently removes files the rescan marked “missing” (gone from disk); leave days at 0 for all, or set a minimum age. Scanning runs in the background; watch it above.'));

    app.replaceChildren(el('h2', {}, 'Database'), info, statusCard, scanCard, tablesCard, maint);
    watchScan(scanHost);  // reflect any scan already in progress (or show idle)
  } catch (e) { showError(e); }
}

// ---- Equipment (admin): build rigs from camera + focal-length combos --------

let siteDist = 250;  // metres; the user-adjustable site clustering distance
let flatGap = 45;    // minutes; the gap that separates one flat run from the next

async function equipment() {
  app.replaceChildren(el('div', { class: 'muted' }, 'Loading…'));
  try {
    const j = await api('/equipment');
    let sug = { clusters: [] };
    try { sug = await api('/equipment/site-suggestions?distance=' + siteDist); } catch (_e) { /* needs frames */ }
    let fsug = { clusters: [] };
    try { fsug = await api('/equipment/flat-set-suggestions?gap_minutes=' + flatGap); } catch (_e) { /* needs flats */ }
    const rnd = (v) => Math.round(Number(v));
    const parts = [el('h2', {}, 'Equipment')];

    // Suggested rigs (uncovered camera + focal clusters).
    const sstatus = el('div', {});
    if (j.suggestions.length) {
      const rows = j.suggestions.map(s => {
        const name = el('input', { value: `${s.camera}-${rnd((s.focal_min + s.focal_max) / 2)}mm`, style: 'width:12em' });
        const tel = el('input', { placeholder: 'telescope (optional)', style: 'width:11em' });
        const fmin = el('input', { type: 'number', value: s.suggest_min, style: 'width:6em' });
        const fmax = el('input', { type: 'number', value: s.suggest_max, style: 'width:6em' });
        const site = el('select', {}, el('option', { value: '' }, '(no site)'),
          ...j.sites.map(x => el('option', { value: x.id }, x.name)));
        return el('div', { class: 'rigrow' },
          el('div', { style: 'min-width:14em' }, el('b', {}, s.camera),
            el('span', { class: 'muted' }, ` · ${rnd(s.focal_min)}–${rnd(s.focal_max)} mm · ${Number(s.frames).toLocaleString()} frames`)),
          name, tel, el('span', { class: 'muted' }, 'range'), fmin, el('span', { class: 'muted' }, '–'), fmax, site,
          el('button', { class: 'btn primary', onclick: async () => {
            if (!name.value.trim()) { sstatus.replaceChildren(el('span', { class: 'err-msg' }, 'name required')); return; }
            try {
              const r = await apiPost('/equipment/rigs', { name: name.value.trim(), camera_id: s.camera_id, telescope: tel.value.trim(), focal_min_mm: Number(fmin.value), focal_max_mm: Number(fmax.value), site_id: site.value ? Number(site.value) : 0 });
              sstatus.replaceChildren(el('span', { class: 'ok-msg' }, `created ${r.name}; assigned ${Number(r.assigned).toLocaleString()} frame(s)`));
              if (r.warning) alert('Note: ' + r.warning + '.');
              equipment();
            } catch (e) { sstatus.replaceChildren(el('span', { class: 'err-msg' }, String(e.message || e))); }
          } }, 'Create rig'));
      });
      parts.push(el('div', { class: 'card' }, el('h3', {}, 'Suggested rigs'),
        el('div', { class: 'muted' }, 'Camera + focal-length combinations in your frames that no rig covers yet. Name each and create it; matching frames are assigned immediately.'),
        el('div', { class: 'culist' }, ...rows), sstatus));
    }

    // Existing rigs, editable: name, camera, telescope, site, focal range.
    const rrows = j.rigs.length ? j.rigs.map(r => {
      const nm = el('input', { value: r.name, style: 'width:11em' });
      const cam = el('select', {}, ...j.cameras.map(c =>
        el('option', { value: c.id, selected: Number(c.id) === Number(r.camera_id) }, c.model)));
      const tel = el('input', { value: r.telescope || '', placeholder: 'telescope', style: 'width:10em' });
      const site = el('select', {}, el('option', { value: '' }, '(no site)'),
        ...j.sites.map(x => el('option', { value: x.id, selected: Number(x.id) === Number(r.site_id) }, x.name)));
      const fmin = el('input', { type: 'number', value: rnd(r.focal_min_mm), style: 'width:5.5em' });
      const fmax = el('input', { type: 'number', value: rnd(r.focal_max_mm), style: 'width:5.5em' });
      const curSite = r.site_id ? Number(r.site_id) : 0;
      return el('div', { class: 'rigrow' },
        nm, cam, tel, site,
        el('span', { class: 'muted' }, 'range'), fmin, el('span', { class: 'muted' }, '–'), fmax,
        el('span', { class: 'muted' }, `${Number(r.frames).toLocaleString()} frames`),
        el('button', { class: 'btn', onclick: async () => {
          // Send only what changed, so frame membership is recomputed only when
          // camera or focal range actually moves.
          const patch = {};
          if (nm.value.trim() !== r.name) patch.name = nm.value.trim();
          if (Number(cam.value) !== Number(r.camera_id)) patch.camera_id = Number(cam.value);
          if (tel.value.trim() !== (r.telescope || '')) patch.telescope = tel.value.trim();
          const newSite = site.value ? Number(site.value) : 0;
          if (newSite !== curSite) patch.site_id = newSite;
          if (Number(fmin.value) !== rnd(r.focal_min_mm)) patch.focal_min_mm = Number(fmin.value);
          if (Number(fmax.value) !== rnd(r.focal_max_mm)) patch.focal_max_mm = Number(fmax.value);
          if (!Object.keys(patch).length) { alert('No changes.'); return; }
          try {
            const x = await apiPatch('/equipment/rigs/' + r.id, patch);
            if (x.reassigned) alert(`Saved; ${Number(x.reassigned).toLocaleString()} frame(s) reassigned.`);
            if (x.warning) alert('Note: ' + x.warning + '.');
            equipment();
          } catch (e) { alert(String(e.message || e)); }
        } }, 'Save'),
        el('button', { class: 'link', onclick: () => go('query', { rig: r.name }) }, 'query →'),
        el('button', { class: 'btn ghost', title: 'delete rig', onclick: async () => {
          if (!confirm(`Delete rig "${r.name}"? Frames keep their files; their rig assignment is cleared.`)) return;
          try { await apiDelete('/equipment/rigs/' + r.id); equipment(); } catch (e) { alert(String(e.message || e)); }
        } }, '✕'));
    }) : [el('div', { class: 'muted' }, 'No rigs yet. Create them from the suggestions above.')];
    parts.push(el('div', { class: 'card' }, el('h3', {}, 'Rigs'), el('div', { class: 'culist' }, ...rrows)));

    // Sites: existing (editable) + location-clustered suggestions.
    const sstatus2 = el('div', {});
    const srrows = j.sites.length ? j.sites.map(s => {
      const nm = el('input', { value: s.name, style: 'width:11em' });
      const lat0 = num(s.latitude, 6), lon0 = num(s.longitude, 6);
      const lat = el('input', { type: 'number', step: 'any', value: lat0, style: 'width:8em' });
      const lon = el('input', { type: 'number', step: 'any', value: lon0, style: 'width:8em' });
      const tz = el('input', { value: s.timezone || '', placeholder: 'timezone', style: 'width:11em' });
      return el('div', { class: 'rigrow' },
        nm, lat, lon, tz,
        el('span', { class: 'muted' }, `${Number(s.frames).toLocaleString()} frames`),
        el('button', { class: 'btn', onclick: async () => {
          const patch = {};
          if (nm.value.trim() !== s.name) patch.name = nm.value.trim();
          if (lat.value !== lat0) patch.latitude = Number(lat.value);
          if (lon.value !== lon0) patch.longitude = Number(lon.value);
          if (tz.value.trim() !== (s.timezone || '')) patch.timezone = tz.value.trim();
          if (!Object.keys(patch).length) { alert('No changes.'); return; }
          try { await apiPatch('/equipment/sites/' + s.id, patch); equipment(); }
          catch (e) { alert(String(e.message || e)); }
        } }, 'Save'),
        el('button', { class: 'link', onclick: () => go('query', { site: s.name }) }, 'query →'),
        el('button', { class: 'btn ghost', title: 'delete site', onclick: async () => {
          if (!confirm(`Delete site "${s.name}"? Frames keep their files; their site is cleared.`)) return;
          try { await apiDelete('/equipment/sites/' + s.id); equipment(); } catch (e) { alert(String(e.message || e)); }
        } }, '✕'));
    }) : [el('div', { class: 'muted' }, 'No sites yet. Create them from the location clusters below.')];

    const distInput = el('input', { type: 'number', value: siteDist, style: 'width:7em' });
    const clusterRows = (sug.clusters || []).map(c => {
      if (c.covered_by) return el('div', { class: 'rigrow muted' },
        `${num(c.latitude, 5)}, ${num(c.longitude, 5)} · ${Number(c.frames).toLocaleString()} frames`,
        el('span', { class: 'tagchip' }, '📍 ' + c.covered_by));
      const name = el('input', { placeholder: 'site name', style: 'width:12em' });
      const tz = el('input', { placeholder: 'timezone (optional)', value: 'America/Phoenix', style: 'width:12em' });
      return el('div', { class: 'rigrow' },
        el('span', { class: 'muted', style: 'min-width:16em' }, `${num(c.latitude, 5)}, ${num(c.longitude, 5)} · ${Number(c.frames).toLocaleString()} frames`),
        name, tz,
        el('button', { class: 'btn primary', onclick: async () => {
          if (!name.value.trim()) { sstatus2.replaceChildren(el('span', { class: 'err-msg' }, 'name required')); return; }
          try {
            const r = await apiPost('/equipment/sites', { name: name.value.trim(), latitude: c.latitude, longitude: c.longitude, distance_m: siteDist, timezone: tz.value.trim() });
            sstatus2.replaceChildren(el('span', { class: 'ok-msg' }, `created ${r.name}; assigned ${Number(r.assigned).toLocaleString()} frame(s)`));
            equipment();
          } catch (e) { sstatus2.replaceChildren(el('span', { class: 'err-msg' }, String(e.message || e))); }
        } }, 'Create site'));
    });

    parts.push(el('div', { class: 'card' }, el('h3', {}, 'Sites'),
      el('div', { class: 'muted', style: 'margin-bottom:.3rem' }, 'Edit corrects the label; changing coordinates does not re-cluster or re-assign frames.'),
      el('div', { class: 'culist' }, ...srrows)));
    parts.push(el('div', { class: 'card' }, el('h3', {}, 'Location clusters'),
      el('div', { class: 'row' }, el('span', { class: 'muted' }, 'Group frames within'), distInput, el('span', { class: 'muted' }, 'metres of each other'),
        el('button', { class: 'btn', onclick: () => { siteDist = Math.max(1, Number(distInput.value) || 250); equipment(); } }, 'Re-cluster')),
      el('div', { class: 'muted', style: 'margin:.3rem 0' }, 'Observatory locations found in your frame headers. Name a cluster to create a site and assign its frames.'),
      el('div', { class: 'culist' }, ...(clusterRows.length ? clusterRows : [el('div', { class: 'muted' }, 'No located frames (need OBSGEO/SITELAT headers, and a scan).')])), sstatus2));

    // Cameras (auto-detected, for context).
    // Flat sets: bind a specific group of flats (or a master) to lights.
    const fsStatus = el('div', {});
    const fsRows = (j.flat_sets || []).length ? j.flat_sets.map(s => {
      const isSite = truthy(s.is_site);
      const isActive = truthy(s.is_active);
      const label = el('input', { value: s.label, style: 'width:14em' });
      const vf = el('input', { type: 'date', value: s.valid_from || '', title: 'valid from', style: 'width:9.5em' });
      const vt = el('input', { type: 'date', value: s.valid_to || '', title: 'valid to', style: 'width:9.5em' });
      const row = [
        el('b', { style: 'min-width:9em' }, s.rig), label,
        el('span', { class: 'muted' }, `${s.filter || 'all filters'} · ${s.source} · ${s.captured_night || '—'} · ${Number(s.frames).toLocaleString()} flats`),
      ];
      if (isSite) {
        // Fixed rig: sticky set, with an optional explicit validity window and
        // an active pointer.
        row.push(el('span', { class: 'muted' }, 'valid'), vf, el('span', { class: 'muted' }, '→'), vt);
        row.push(isActive ? el('span', { class: 'tagchip' }, '★ active')
          : el('button', { class: 'btn', onclick: async () => {
              try { await apiPatch('/equipment/rigs/' + s.rig_id, { active_flat_set_id: Number(s.id) }); equipment(); }
              catch (e) { alert(String(e.message || e)); } } }, 'Make active'));
      } else {
        row.push(el('span', { class: 'muted' }, 'night-matched (mobile rig)'));
      }
      row.push(el('button', { class: 'btn', onclick: async () => {
        const patch = {};
        if (label.value.trim() !== s.label) patch.label = label.value.trim();
        if (isSite) {
          if ((vf.value || '') !== (s.valid_from || '')) patch.valid_from = vf.value || null;
          if ((vt.value || '') !== (s.valid_to || '')) patch.valid_to = vt.value || null;
        }
        if (!Object.keys(patch).length) { alert('No changes.'); return; }
        try { await apiPatch('/equipment/flat-sets/' + s.id, patch); equipment(); }
        catch (e) { alert(String(e.message || e)); }
      } }, 'Save'));
      row.push(el('button', { class: 'btn ghost', title: 'delete flat set', onclick: async () => {
        if (!confirm(`Delete flat set "${s.label}"? Its flats keep their files; only the flat-set assignment is cleared.`)) return;
        try { await apiDelete('/equipment/flat-sets/' + s.id); equipment(); } catch (e) { alert(String(e.message || e)); }
      } }, '✕'));
      return el('div', { class: 'rigrow' }, ...row);
    }) : [el('div', { class: 'muted' }, 'No flat sets yet. Create them from the detected clusters below.')];

    // Explicit pins (rig + night -> set), which override the rules.
    const pinRows = (j.flat_pins || []).filter(p => p.scope === 'rig_night').map(p =>
      el('div', { class: 'rigrow' },
        el('span', { style: 'min-width:16em' }, `${p.rig || ('rig ' + p.rig_id)} · ${p.session_night}`),
        el('span', { class: 'muted' }, '→ ' + p.flat_set),
        el('button', { class: 'btn ghost', title: 'remove pin', onclick: async () => {
          try { await apiDelete('/equipment/flat-pins/' + p.id); equipment(); } catch (e) { alert(String(e.message || e)); }
        } }, '✕')));
    const pinSet = el('select', {}, el('option', { value: '' }, '(flat set)'),
      ...(j.flat_sets || []).map(s => el('option', { value: s.id }, `${s.rig} · ${s.label}`)));
    const pinRig = el('select', {}, el('option', { value: '' }, '(rig)'),
      ...j.rigs.map(r => el('option', { value: r.id }, r.name)));
    const pinNight = el('input', { type: 'date', style: 'width:10em' });
    const pinAdd = el('button', { class: 'btn', onclick: async () => {
      if (!pinSet.value || !pinRig.value || !pinNight.value) { alert('Pick a set, a rig, and a night.'); return; }
      try { await apiPost('/equipment/flat-pins', { flat_set_id: Number(pinSet.value), scope: 'rig_night', rig_id: Number(pinRig.value), session_night: pinNight.value }); equipment(); }
      catch (e) { alert(String(e.message || e)); }
    } }, 'Pin');

    parts.push(el('div', { class: 'card' }, el('h3', {}, 'Flat sets'),
      el('div', { class: 'muted', style: 'margin-bottom:.3rem' }, 'Bind a specific group of flats (or a master) to lights. Mobile rigs (no site) match flats by night; fixed rigs (with a site) keep an active set until you shoot a new one. Pins override both.'),
      el('div', { class: 'culist' }, ...fsRows),
      el('h4', { style: 'margin:.6rem 0 .2rem' }, 'Pins (rig + night → set)'),
      el('div', { class: 'culist' }, ...(pinRows.length ? pinRows : [el('div', { class: 'muted' }, 'No pins.')])),
      el('div', { class: 'row', style: 'margin-top:.3rem' }, pinSet, pinRig, pinNight, pinAdd),
      fsStatus));

    // Detect flat sets: cluster not-yet-assigned flats by DATE-OBS gap.
    const fsugStatus = el('div', {});
    const gapInput = el('input', { type: 'number', value: flatGap, style: 'width:6em' });
    const clRows = (fsug.clusters || []).map(c => {
      const rigObj = j.rigs.find(r => Number(r.id) === Number(c.rig_id));
      const isSite = !!(rigObj && rigObj.site_id);
      const filt = c.filter || '';
      const nm = el('input', { value: `${c.rig}-${filt ? filt + '-' : ''}${(c.session_night || c.start_utc || '').slice(0, 10)}`, style: 'width:16em' });
      const active = el('input', { type: 'checkbox' });
      const row = [
        el('span', { class: 'muted', style: 'min-width:22em' },
          `${c.rig} · ${filt || '(no filter)'} · ${(c.start_utc || '').slice(0, 16)} → ${(c.end_utc || '').slice(11, 16)} · ${Number(c.frames).toLocaleString()} flats`),
        nm,
      ];
      if (isSite) row.push(el('label', { class: 'muted' }, active, ' active'));
      row.push(el('button', { class: 'btn primary', onclick: async () => {
        if (!nm.value.trim()) { fsugStatus.replaceChildren(el('span', { class: 'err-msg' }, 'name required')); return; }
        try {
          const r = await apiPost('/equipment/flat-sets', { rig_id: Number(c.rig_id), filter_id: c.filter_id || 0, label: nm.value.trim(), start_utc: c.start_utc, end_utc: c.end_utc, set_active: isSite ? active.checked : false });
          fsugStatus.replaceChildren(el('span', { class: 'ok-msg' }, `created ${r.label}; assigned ${Number(r.assigned).toLocaleString()} flat(s)`));
          equipment();
        } catch (e) { fsugStatus.replaceChildren(el('span', { class: 'err-msg' }, String(e.message || e))); }
      } }, 'Create set'));
      return el('div', { class: 'rigrow' }, ...row);
    });
    parts.push(el('div', { class: 'card' }, el('h3', {}, 'Detect flat sets'),
      el('div', { class: 'row' }, el('span', { class: 'muted' }, 'Group flats within'), gapInput, el('span', { class: 'muted' }, 'minutes of each other'),
        el('button', { class: 'btn', onclick: () => { flatGap = Math.max(1, Number(gapInput.value) || 45); equipment(); } }, 'Re-scan')),
      el('div', { class: 'muted', style: 'margin:.3rem 0' }, 'Contiguous runs of not-yet-assigned flats, per rig. Name one to create a flat set and assign its flats.'),
      el('div', { class: 'culist' }, ...(clRows.length ? clRows : [el('div', { class: 'muted' }, 'No unassigned flats (need scanned flats with a rig and DATE-OBS).')])), fsugStatus));

    parts.push(el('div', { class: 'card' }, el('h3', {}, 'Cameras (auto-detected on scan)'),
      el('div', { class: 'chips' }, ...(j.cameras.length
        ? j.cameras.map(c => el('span', { class: 'tagchip' }, `${c.model} · ${Number(c.frames).toLocaleString()}`))
        : [el('span', { class: 'muted' }, 'none yet — scan a root first')]))));

    app.replaceChildren(...parts);
  } catch (e) { showError(e); }
}

// ---- Users (admin) ---------------------------------------------------------

async function users() {
  app.replaceChildren(el('div', { class: 'muted' }, 'Loading…'));
  try {
    const list = await api('/users');

    const nu = el('input', { placeholder: 'username', style: 'width:10em' });
    const np = el('input', { type: 'password', placeholder: 'password', style: 'width:12em' });
    const nr = el('select', {}, ...['user', 'admin', 'readonly'].map(r => el('option', { value: r }, r)));
    const cstatus = el('span', {});
    const createCard = el('div', { class: 'card' }, el('h3', {}, 'Add a user'),
      el('div', { class: 'row' }, nu, np, nr,
        el('button', { class: 'btn primary', onclick: async () => {
          if (!nu.value.trim() || !np.value) { cstatus.replaceChildren(el('span', { class: 'err-msg' }, 'username and password required')); return; }
          try { await apiPost('/users', { username: nu.value.trim(), password: np.value, role: nr.value, must_change_password: true }); users(); }
          catch (e) { cstatus.replaceChildren(el('span', { class: 'err-msg' }, String(e.message || e))); }
        } }, 'Create user'), cstatus),
      el('div', { class: 'muted', style: 'margin-top:.4rem' }, 'New users must change their password on first login. Roles: admin (full), user (writes), readonly (browse only).'));

    const rows = list.map(u => {
      const roleSel = el('select', { onchange: async (e) => {
        try { await apiPatch('/users/' + encodeURIComponent(u.username), { role: e.target.value }); }
        catch (err) { alert(String(err.message || err)); users(); }
      } }, ...['admin', 'user', 'readonly'].map(r => el('option', { value: r, selected: u.role === r }, r)));
      const enabled = el('input', { type: 'checkbox', checked: truthy(u.enabled), onchange: async (e) => {
        try { await apiPatch('/users/' + encodeURIComponent(u.username), { enabled: e.target.checked }); }
        catch (err) { alert(String(err.message || err)); users(); }
      } });
      return el('tr', {},
        el('td', {}, u.username, u.username === (me && me.username) ? el('span', { class: 'muted' }, ' (you)') : null),
        el('td', {}, roleSel),
        el('td', {}, el('label', {}, enabled, ' enabled')),
        el('td', { class: 'muted' }, truthy(u.must_change_password) ? 'must change pw' : ''),
        el('td', { class: 'muted' }, fmt(u.last_login_at, 'never')),
        el('td', {},
          el('button', { class: 'link', onclick: async () => {
            const p = prompt('New password for ' + u.username + ':');
            if (!p) return;
            try { await apiPatch('/users/' + encodeURIComponent(u.username), { password: p }); alert('password reset; user must change it on next login'); }
            catch (err) { alert(String(err.message || err)); }
          } }, 'reset pw'),
          el('span', {}, ' '),
          el('button', { class: 'btn ghost', title: 'delete user', onclick: async () => {
            if (!confirm('Delete user "' + u.username + '"?')) return;
            try { await apiDelete('/users/' + encodeURIComponent(u.username)); users(); }
            catch (err) { alert(String(err.message || err)); }
          } }, '✕')));
    });
    const tbl = el('table', {},
      el('thead', {}, el('tr', {}, ...['User', 'Role', 'Status', '', 'Last login', 'Actions'].map(h => el('th', {}, h)))),
      el('tbody', {}, ...rows));
    app.replaceChildren(el('h2', {}, 'Users'), createCard, el('div', { class: 'card' }, el('div', { class: 'wrap' }, tbl)));
  } catch (e) { showError(e); }
}

// ---- Target names (review & normalize) -------------------------------------

async function targets() {
  app.replaceChildren(el('div', { class: 'muted' }, 'Loading…'));
  try {
    const data = await api('/objects');
    const list = data.objects || [];
    const sesameOn = !!data.sesame_enabled;
    const gstatus = el('span', {});

    // Each row keeps an editable target, pre-filled with the best proposal
    // (Sesame identity if resolved, else the offline catalog form).
    const inputs = new Map();
    const rowEls = list.map(o => {
      const best = (o.sesame && o.sesame.name) ? o.sesame.name : o.offline;
      const inp = el('input', { value: o.placeholder ? '' : best, disabled: o.placeholder, style: 'width:12em' });
      inputs.set(o.raw, { inp, o });
      const ses = o.sesame && o.sesame.name
        ? el('td', { class: 'muted' }, `${o.sesame.name}${o.sesame.otype ? ' · ' + o.sesame.otype : ''}${o.sesame.ra_deg != null ? ` · ${num(o.sesame.ra_deg, 3)}, ${num(o.sesame.dec_deg, 3)}` : ''}`)
        : el('td', { class: 'muted' }, o.placeholder ? '' : '—');
      return el('tr', { class: o.placeholder ? 'muted' : '' },
        el('td', { class: 'mono' }, o.raw),
        el('td', { class: 'num' }, Number(o.count).toLocaleString()),
        el('td', {}, o.placeholder ? el('span', { class: 'pill' }, 'placeholder') : fmt(o.current, '—')),
        el('td', { class: o.changed ? 'warn' : 'muted' }, o.offline),
        ses,
        el('td', {}, inp));
    });

    const tbl = el('table', {},
      el('thead', {}, el('tr', {}, ...['Raw OBJECT', 'Frames', 'Current', 'Offline', 'Sesame (SIMBAD)', 'Apply as'].map(h => el('th', {}, h)))),
      el('tbody', {}, ...rowEls));

    const collectMapping = () => {
      const m = {};
      for (const [raw, { inp, o }] of inputs) {
        const v = inp.value.trim();
        if (!o.placeholder && v && v !== (o.current || '')) m[raw] = v;
      }
      return m;
    };
    const apply = async () => {
      const mapping = collectMapping();
      if (!Object.keys(mapping).length) { gstatus.replaceChildren(el('span', { class: 'muted' }, 'nothing to change')); return; }
      gstatus.replaceChildren(el('span', { class: 'muted' }, 'previewing…'));
      try {
        const dry = await apiPost('/objects/apply', { dry_run: true, mapping });
        if (!confirm(`Set object_canonical on ${dry.total_frames.toLocaleString()} frame(s) across ${dry.changes.length} target name(s)?\n\nThe raw OBJECT card is preserved.`)) { gstatus.replaceChildren(); return; }
        const r = await apiPost('/objects/apply', { dry_run: false, mapping });
        gstatus.replaceChildren(el('span', { class: 'ok-msg' }, `Normalized ${r.total_frames.toLocaleString()} frame(s).`));
        targets();
      } catch (e) { gstatus.replaceChildren(el('span', { class: 'err-msg' }, String(e.message || e))); }
    };
    const resolve = async () => {
      gstatus.replaceChildren(el('span', { class: 'muted' }, 'resolving via CDS Sesame… (this reaches the network)'));
      try {
        const names = list.filter(o => !o.placeholder).map(o => o.raw);
        await apiPost('/objects/resolve', { names });
        gstatus.replaceChildren(el('span', { class: 'ok-msg' }, 'resolved; review the SIMBAD column'));
        targets();
      } catch (e) { gstatus.replaceChildren(el('span', { class: 'err-msg' }, String(e.message || e))); }
    };

    app.replaceChildren(el('h2', {}, 'Target names'),
      el('div', { class: 'card' },
        el('div', { class: 'muted' }, 'FITS prescribes no format for OBJECT. StarBase canonicalizes catalog names to the CDS/IAU form (M101 → “M 101”), stored in object_canonical; the raw OBJECT card is kept. Edit any “Apply as” value before applying.'),
        el('div', { class: 'row', style: 'margin-top:.6rem' },
          el('button', { class: 'btn primary', onclick: apply }, 'Apply changes'),
          sesameOn
            ? el('button', { class: 'btn', onclick: resolve }, 'Resolve all via Sesame')
            : el('span', { class: 'muted' }, 'Online Sesame resolution is off (enable [names] sesame_enabled in the config).'),
          gstatus)),
      el('div', { class: 'card' }, el('div', { class: 'wrap' }, tbl)));
  } catch (e) { showError(e); }
}

// ---- Server (admin: change the listening interface) ------------------------

async function server() {
  app.replaceChildren(el('div', { class: 'muted' }, 'Loading…'));
  try {
    const [s, ifaces] = await Promise.all([api('/settings'), api('/interfaces')]);
    const run = s.running, conf = s.configured;
    const scheme = (t) => (t ? 'https' : 'http');

    const bindInput = el('input', { value: conf.bind, list: 'dl_bind', style: 'width:16em' });
    const dl = el('datalist', { id: 'dl_bind' }, ...ifaces.map(i => el('option', { value: i.address }, i.name)));
    const portInput = el('input', { type: 'number', value: conf.port, style: 'width:8em' });
    const tlsInput = el('input', { type: 'checkbox', checked: conf.tls });
    const status = el('div', {});
    const collect = () => ({ bind: bindInput.value.trim(), port: Number(portInput.value), tls: tlsInput.checked });

    const save = async () => {
      status.replaceChildren(el('span', { class: 'muted' }, 'saving…'));
      try {
        const r = await apiPut('/settings', collect());
        status.replaceChildren(el('span', { class: r.restart_required ? 'warn-msg' : 'ok-msg' },
          r.restart_required ? `Saved. Use “Restart & apply” to start listening on ${scheme(r.configured.tls)}://${r.configured.bind}:${r.configured.port}.` : 'Saved.'));
      } catch (e) { status.replaceChildren(el('span', { class: 'err-msg' }, String(e.message || e))); }
    };
    const applyNow = async () => {
      const c = collect();
      if (!confirm(`Rebind the API to ${scheme(c.tls)}://${c.bind}:${c.port}?\n\nIf the address or port changes, this page will stop responding and you must reconnect at the new URL.`)) return;
      status.replaceChildren(el('span', { class: 'muted' }, 'applying…'));
      try {
        await apiPut('/settings', c);
        await apiPost('/settings/apply', {});
        status.replaceChildren(el('span', { class: 'ok-msg' },
          `Applying now. If this page stops responding, reconnect at ${scheme(c.tls)}://${c.bind}:${c.port}.`));
      } catch (e) { status.replaceChildren(el('span', { class: 'err-msg' }, String(e.message || e))); }
    };

    app.replaceChildren(el('h2', {}, 'Server'),
      el('div', { class: 'card' },
        el('div', {}, 'Currently listening on ', el('b', {}, `${scheme(run.tls)}://${run.bind}:${run.port}`)),
        s.restart_required ? el('div', { class: 'warn-msg' }, `⚠ Pending change: configured ${scheme(conf.tls)}://${conf.bind}:${conf.port}. Restart & apply, or restart the daemon, to take effect.`) : null,
        !s.can_apply ? el('div', { class: 'muted' }, 'Live apply is unavailable; changes take effect on the next daemon restart.') : null),
      el('div', { class: 'card' }, el('h3', {}, 'Listening interface'),
        el('div', { class: 'kv' },
          el('div', { class: 'k' }, 'Bind address'), el('div', { class: 'v' }, el('span', { class: 'combo' }, bindInput, dl)),
          el('div', { class: 'k' }, 'Port'), el('div', { class: 'v' }, portInput),
          el('div', { class: 'k' }, 'TLS (HTTPS)'), el('div', { class: 'v' }, el('label', {}, tlsInput, ' enabled'))),
        el('div', { class: 'muted', style: 'margin:.5rem 0' }, '0.0.0.0 = all interfaces · 127.0.0.1 = localhost only · or pick a specific interface address.'),
        el('div', { class: 'row' },
          el('button', { class: 'btn primary', onclick: save }, 'Save'),
          el('button', { class: 'btn', onclick: applyNow }, 'Restart & apply')),
        status));
  } catch (e) { showError(e); }
}

// ---- shell / router --------------------------------------------------------

function setConn(t) { const c = document.getElementById('conn'); c.textContent = t; c.classList.remove('err'); }
function showError(e) {
  app.replaceChildren(el('div', { class: 'err-msg' }, 'Error: ' + (e.message || e)));
  const c = document.getElementById('conn'); c.textContent = 'disconnected'; c.classList.add('err');
}

const VIEWS = { dashboard, browse, query, jobs, roots, tags, database, users, server, targets, equipment };
const ADMIN_VIEWS = ['users', 'server', 'targets', 'equipment'];
let current = 'dashboard';
function go(view, preset) {
  if (!VIEWS[view]) view = 'dashboard';
  if (ADMIN_VIEWS.includes(view) && !(me && me.role === 'admin')) view = 'dashboard';
  current = view;
  if (location.hash.slice(1) !== view) history.replaceState(null, '', '#' + view);
  document.querySelectorAll('nav button').forEach(b => b.classList.toggle('active', b.dataset.view === view));
  drawer.classList.add('hidden');
  stopScanPoll();  // a view still watching a scan re-establishes its own poll
  VIEWS[view](preset);
}
document.querySelectorAll('nav button').forEach(b => b.onclick = () => go(b.dataset.view));
window.addEventListener('hashchange', () => { const v = location.hash.slice(1); if (v && v !== current) go(v); });

// ---- auth shell ------------------------------------------------------------

let me = null;  // { username, role } when logged in

function chrome(on) {
  document.querySelector('nav').style.visibility = on ? 'visible' : 'hidden';
  const admin = on && me && me.role === 'admin';
  document.querySelectorAll('nav button[data-view="equipment"], nav button[data-view="users"], nav button[data-view="server"]')
    .forEach(b => { b.style.display = admin ? '' : 'none'; });
  const acc = document.getElementById('account');
  if (!on || !me) { acc.replaceChildren(); return; }
  acc.replaceChildren(
    el('span', { class: 'muted' }, `${me.username} · ${me.role}`),
    el('button', { class: 'btn ghost', onclick: () => changePasswordScreen(false) }, 'Password'),
    el('button', { class: 'btn ghost', onclick: async () => { try { await apiPost('/logout', {}); } catch (_e) { /* ignore */ } me = null; boot(); } }, 'Logout'));
}

function authScreen(title, fields, onSubmit, extra) {
  chrome(false);
  const status = el('div', { class: 'err-msg' });
  const submit = () => onSubmit(status);
  for (const f of fields) f.addEventListener('keydown', e => { if (e.key === 'Enter') submit(); });
  app.replaceChildren(el('div', { class: 'login' },
    el('h1', {}, 'Star', el('span', {}, 'Base')),
    el('div', { class: 'card loginbox' }, el('h3', {}, title),
      extra || null, ...fields.map(f => el('div', { class: 'field' }, f)),
      status, el('button', { class: 'btn primary', onclick: submit }, title))));
  setTimeout(() => fields[0].focus(), 50);
}

function loginScreen(msg) {
  const u = el('input', { placeholder: 'username' });
  const p = el('input', { type: 'password', placeholder: 'password' });
  authScreen('Log in', [u, p], async (status) => {
    status.className = 'err-msg'; status.textContent = '';
    try {
      const r = await apiPost('/login', { username: u.value, password: p.value });
      if (r.must_change_password) { changePasswordScreen(true); return; }
      await boot();
    } catch (e) { status.textContent = e.message || String(e); }
  }, msg ? el('div', { class: 'muted' }, msg) : null);
}

function changePasswordScreen(forced) {
  const cur = el('input', { type: 'password', placeholder: 'current password' });
  const nw = el('input', { type: 'password', placeholder: 'new password' });
  const cf = el('input', { type: 'password', placeholder: 'confirm new password' });
  authScreen(forced ? 'Set a new password' : 'Change password', [cur, nw, cf], async (status) => {
    status.className = 'err-msg';
    if (nw.value !== cf.value) { status.textContent = 'passwords do not match'; return; }
    try {
      await apiPost('/me/password', { current_password: cur.value, new_password: nw.value });
      status.className = 'ok-msg'; status.textContent = 'password changed';
      setTimeout(boot, 700);
    } catch (e) { status.textContent = e.message || String(e); }
  }, forced ? el('div', { class: 'muted' }, 'This account must set a new password before continuing.') : null);
}

async function boot() {
  try {
    const m = await api('/me');
    if (m.authenticated) { me = { username: m.username, role: m.role }; chrome(true); go(location.hash.slice(1) || 'dashboard'); }
    else if (m.auth_required) { me = null; loginScreen(); }
    else { me = null; chrome(true); go(location.hash.slice(1) || 'dashboard'); }  // open install
  } catch (e) { showError(e); }
}
boot();

