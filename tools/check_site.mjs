#!/usr/bin/env node
// Audit a site tree BEFORE it is published. Exit 0 only if everything the installer will fetch is present and consistent.
//   node tools/check_site.mjs <site-dir> [--require-vendor]
// Checks: versions.json (same rules the page enforces, via site/lib.js), every manifest and image exists, sizes and
// SHA-256 match, the manifest says what the index says, offsets, file modes, and the page files are there.
import { readFileSync, statSync, existsSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const { validateVersions, BOARDS, parseVersion } = await import(join(here, '..', 'site', 'lib.js'));

const args = process.argv.slice(2);
const requireVendor = args.includes('--require-vendor');
const dir = args.find((a) => !a.startsWith('--'));
if (!dir) { console.error('usage: check_site.mjs <site-dir> [--require-vendor]'); process.exit(2); }

const problems = [];
const bad = (m) => problems.push(m);
const root = resolve(dir);
const inside = (p) => { const r = resolve(root, p); return r.startsWith(root + '/') ? r : null; };

for (const f of ['index.html', 'app.js', 'lib.js', 'style.css', 'versions.json']) if (!existsSync(join(root, f))) bad(`missing ${f}`);
if (requireVendor && !existsSync(join(root, 'vendor/esp-web-tools/install-button.js'))) bad('missing vendor/esp-web-tools/install-button.js (the install button would not load)');

let doc = null;
try { doc = JSON.parse(readFileSync(join(root, 'versions.json'), 'utf8')); } catch (e) { bad(`versions.json unreadable: ${e.message}`); }
if (doc) {
  const v = validateVersions(doc);
  if (!v.ok) bad(`versions.json: ${v.error}`);
  const seen = new Set();
  for (const b of doc.builds || []) {
    const id = `${b.board}/${b.channel}/${b.version}`;
    if (seen.has(id)) bad(`${id}: listed twice`);
    seen.add(id);
    if (!BOARDS.some((x) => x.id === b.board)) { bad(`${id}: unknown board`); continue; }
    const mPath = inside(b.manifest || ''), fPath = inside(b.file || '');
    if (!mPath || !fPath) { bad(`${id}: path escapes the site`); continue; }
    if (!existsSync(mPath)) { bad(`${id}: manifest missing`); continue; }
    if (!existsSync(fPath)) { bad(`${id}: image missing`); continue; }
    const data = readFileSync(fPath);
    if (data.length !== b.size) bad(`${id}: size ${data.length} != index ${b.size}`);
    if (createHash('sha256').update(data).digest('hex') !== b.sha256) bad(`${id}: sha256 does not match the index`);
    if (data[0] !== 0xe9) bad(`${id}: image does not start with the ESP image magic`);
    for (const p of [mPath, fPath]) if ((statSync(p).mode & 0o044) !== 0o044) bad(`${id}: ${p.slice(root.length + 1)} is not world-readable`);
    let m;
    try { m = JSON.parse(readFileSync(mPath, 'utf8')); } catch (e) { bad(`${id}: manifest unreadable: ${e.message}`); continue; }
    if (m.version !== b.version) bad(`${id}: manifest version ${m.version} != index`);
    if (m.new_install_prompt_erase !== true) bad(`${id}: manifest must set new_install_prompt_erase (a reinstall would silently wipe settings)`);
    const parts = m.builds?.[0]?.parts || [];
    if (parts.length !== 1 || parts[0].offset !== 0) bad(`${id}: expected one part at offset 0`);
    else if (resolve(dirname(mPath), parts[0].path) !== fPath) bad(`${id}: manifest part does not point at the indexed image`);
  }
  // each board/channel needs a newest build the page can pick
  for (const b of BOARDS) for (const ch of ['stable', 'nightly']) {
    const list = (doc.builds || []).filter((x) => x.board === b.id && x.channel === ch);
    for (const x of list) if (!parseVersion(x.version)) bad(`${x.version}: not a version`);
  }
}

if (problems.length) { console.error(`site check FAILED (${problems.length}):`); for (const p of problems) console.error('  - ' + p); process.exit(1); }
console.log(`site check ok: ${doc.builds.length} build(s)`);
