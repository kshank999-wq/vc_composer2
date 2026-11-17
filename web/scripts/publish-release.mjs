#!/usr/bin/env node
/**
 * Publish an installer to the release catalogue, inactive.
 *
 *   node scripts/publish-release.mjs --platform windows --channel preview \
 *     --version 0.1.0 --min-os 10.0.17763 "VC Composer Setup.exe"
 *
 * What it does, in order:
 *   1. hashes the file (SHA-256) and measures it;
 *   2. refuses if that platform/channel/version is already **live**: replacing
 *      the bytes under a live row would hand customers a file whose checksum
 *      the updater no longer believes. Publish a new version instead;
 *   3. uploads it to the private Storage bucket at `${platform}/${version}/${file}`
 *      with Supabase's resumable (TUS) upload, in 6 MB pieces, straight to the
 *      storage host, which is Supabase's own route for large files;
 *   4. upserts an **inactive** `release_builds` row with its size, checksum and
 *      minimum OS.
 *
 * Nobody can download it yet. An administrator installs it from
 * /admin/releases, then presses "Make live". This is the same job VC Writer's
 * desktop-release workflow does, as a script a CI job or a person can run.
 *
 * Reads from the environment (never from the command line, so a key does not
 * end up in shell history):
 *   SUPABASE_URL               https://<project-ref>.supabase.co
 *   SUPABASE_SERVICE_ROLE_KEY  the service-role key (server-side secret)
 *   RELEASE_BUCKET             optional, default "releases"
 *   SUPABASE_STORAGE_URL       optional; default derived from SUPABASE_URL
 *
 * Options:
 *   --platform windows|macos      required
 *   --version <x.y.z>             required; letters, digits, . - + _
 *   --channel stable|preview|beta|internal   default internal
 *   --min-os <version>            default ""
 *   --notes <text>                release notes, default ""
 *   --make-live                   then make it the live build for its platform and
 *                                 channel (what "Make live" in /admin/releases does:
 *                                 the previous live build is retired, not deleted)
 *   --dry-run                     hash and print the plan; touch nothing
 *
 * Node 20 or later; no dependencies.
 */

import { createHash } from 'node:crypto';
import { createReadStream } from 'node:fs';
import { open, stat } from 'node:fs/promises';
import { basename } from 'node:path';
import { parseArgs } from 'node:util';

const PLATFORMS = ['windows', 'macos'];
const CHANNELS = ['stable', 'preview', 'beta', 'internal'];
const VERSION = /^[\w.\-+]{1,40}$/;
const CHUNK = 6 * 1024 * 1024;

const fail = (message) => {
  console.error(`publish-release: ${message}`);
  process.exit(1);
};

const { values, positionals } = (() => {
  try {
    return parseArgs({
      allowPositionals: true,
      options: {
        platform: { type: 'string' },
        channel: { type: 'string', default: 'internal' },
        version: { type: 'string' },
        'min-os': { type: 'string', default: '' },
        notes: { type: 'string', default: '' },
        'dry-run': { type: 'boolean', default: false },
        'make-live': { type: 'boolean', default: false },
        help: { type: 'boolean', default: false },
      },
    });
  } catch (cause) {
    fail(cause.message);
  }
})();

if (values.help) {
  console.log(
    'Usage: node scripts/publish-release.mjs --platform windows|macos --channel stable|preview|beta|internal --version X.Y.Z [--min-os V] [--notes TEXT] [--make-live] [--dry-run] <installer>',
  );
  process.exit(0);
}

const platform = values.platform;
const channel = values.channel;
const version = values.version?.replace(/^v/, '');
const minimumOs = values['min-os'];
const file = positionals[0];

if (!PLATFORMS.includes(platform)) fail(`--platform must be one of ${PLATFORMS.join(', ')}`);
if (!CHANNELS.includes(channel)) fail(`--channel must be one of ${CHANNELS.join(', ')}`);
if (!version || !VERSION.test(version)) fail('--version is required: letters, digits, and . - + _ only');
if (!file) fail('the installer file is required as the last argument');
if (positionals.length > 1) fail('give exactly one installer file');

const info = await stat(file).catch(() => null);
if (!info?.isFile()) fail(`${file} is not a file`);
const size = info.size;
if (size === 0) fail(`${file} is empty`);

// The same key the admin release manager writes, with the same file-name rule.
const name = basename(file).replace(/[^\w.\-]+/g, '_');
const key = `${platform}/${version}/${name}`;

const sha256 = await new Promise((resolve, reject) => {
  const hash = createHash('sha256');
  createReadStream(file)
    .on('data', (piece) => hash.update(piece))
    .on('end', () => resolve(hash.digest('hex')))
    .on('error', reject);
});

const bucket = process.env.RELEASE_BUCKET || 'releases';
const plan = { platform, channel, version, minimumOs, key, bucket, size, sha256 };

if (values['dry-run']) {
  console.log(JSON.stringify({ dryRun: true, ...plan }, null, 2));
  process.exit(0);
}

const supabaseUrl = process.env.SUPABASE_URL?.replace(/\/+$/, '');
const serviceKey = process.env.SUPABASE_SERVICE_ROLE_KEY;
if (!supabaseUrl || !serviceKey) fail('SUPABASE_URL and SUPABASE_SERVICE_ROLE_KEY must be set in the environment');

// https://<ref>.supabase.co → https://<ref>.storage.supabase.co, which Supabase
// recommends for large uploads. Anything else (a local stack) is used as is.
const storageUrl =
  process.env.SUPABASE_STORAGE_URL?.replace(/\/+$/, '') ||
  supabaseUrl.replace(/^https:\/\/([^.]+)\.supabase\.co$/, 'https://$1.storage.supabase.co');

const auth = { apikey: serviceKey, authorization: `Bearer ${serviceKey}` };
const b64 = (text) => Buffer.from(text, 'utf8').toString('base64');

// 2. Refuse a version that is already live.
{
  const query = new URLSearchParams({
    select: 'id,active',
    platform: `eq.${platform}`,
    channel: `eq.${channel}`,
    version: `eq.${version}`,
  });
  const response = await fetch(`${supabaseUrl}/rest/v1/release_builds?${query}`, { headers: auth });
  if (!response.ok) fail(`could not read the release catalogue (HTTP ${response.status}): ${await response.text()}`);
  const rows = await response.json();
  if (rows.some((row) => row.active)) {
    fail(`${platform} ${version} is already live on ${channel}. Publish a new version instead of replacing a live build.`);
  }
}

// 3. Upload, resumable, in pieces.
console.log(`Uploading ${name} (${size} bytes) to ${bucket}/${key}`);
const created = await fetch(`${storageUrl}/storage/v1/upload/resumable`, {
  method: 'POST',
  headers: {
    ...auth,
    'tus-resumable': '1.0.0',
    'x-upsert': 'true',
    'upload-length': String(size),
    'upload-metadata': [
      `bucketName ${b64(bucket)}`,
      `objectName ${b64(key)}`,
      `contentType ${b64('application/octet-stream')}`,
      `cacheControl ${b64('3600')}`,
    ].join(','),
  },
});
if (created.status !== 201) {
  const body = await created.text();
  if (created.status === 413 || /EntityTooLarge|exceeded the maximum/i.test(body)) {
    fail(`Storage refused ${name} as larger than the project's upload limit. Raise it in Supabase: Storage, Settings, global file size limit.`);
  }
  fail(`Storage would not start the upload (HTTP ${created.status}): ${body}`);
}
const location = created.headers.get('location');
if (!location) fail('Storage did not say where to send the upload');
const target = /^https?:/.test(location) ? location : `${storageUrl}${location}`;

const handle = await open(file, 'r');
try {
  let offset = 0;
  while (offset < size) {
    const length = Math.min(CHUNK, size - offset);
    const piece = Buffer.alloc(length);
    const { bytesRead } = await handle.read(piece, 0, length, offset);
    if (bytesRead !== length) fail(`short read at byte ${offset}`);
    const sent = await fetch(target, {
      method: 'PATCH',
      headers: {
        ...auth,
        'tus-resumable': '1.0.0',
        'upload-offset': String(offset),
        'content-type': 'application/offset+octet-stream',
      },
      body: piece,
    });
    if (sent.status !== 204) fail(`upload stopped at byte ${offset} (HTTP ${sent.status}): ${await sent.text()}`);
    const next = Number.parseInt(sent.headers.get('upload-offset') ?? '', 10);
    offset = Number.isFinite(next) ? next : offset + length;
    process.stdout.write(`  ${Math.round((offset / size) * 100)}%\r`);
  }
} finally {
  await handle.close();
}
console.log(`Uploaded ${name}.`);

// 4. Record it, inactive.
const recorded = await fetch(`${supabaseUrl}/rest/v1/release_builds?on_conflict=platform,channel,version`, {
  method: 'POST',
  headers: {
    ...auth,
    'content-type': 'application/json',
    prefer: 'resolution=merge-duplicates,return=minimal',
  },
  body: JSON.stringify({
    platform,
    version,
    channel,
    minimum_os_version: minimumOs,
    release_notes: values.notes,
    artifact_key: key,
    artifact_size_bytes: size,
    sha256,
    active: false,
  }),
});
if (!recorded.ok) fail(`the build uploaded but could not be recorded (HTTP ${recorded.status}): ${await recorded.text()}`);

console.log(`Recorded ${platform} ${version} on ${channel}, inactive, sha256 ${sha256}.`);

if (!values['make-live']) {
  console.log('Install it from /admin/releases, then press "Make live".');
  process.exit(0);
}

// 5. Make it live: retire the current live build of this platform and channel
// (at most one may be live), then activate this one. The other platform is
// never touched.
const scope = new URLSearchParams({ platform: `eq.${platform}`, channel: `eq.${channel}`, active: 'eq.true' });
const retired = await fetch(`${supabaseUrl}/rest/v1/release_builds?${scope}`, {
  method: 'PATCH',
  headers: { ...auth, 'content-type': 'application/json', prefer: 'return=minimal' },
  body: JSON.stringify({ active: false, updated_at: new Date().toISOString() }),
});
if (!retired.ok) fail(`could not retire the current live build (HTTP ${retired.status}): ${await retired.text()}`);
const mine = new URLSearchParams({ platform: `eq.${platform}`, channel: `eq.${channel}`, version: `eq.${version}` });
const now = new Date().toISOString();
const live = await fetch(`${supabaseUrl}/rest/v1/release_builds?${mine}`, {
  method: 'PATCH',
  headers: { ...auth, 'content-type': 'application/json', prefer: 'return=minimal' },
  body: JSON.stringify({ active: true, published_at: now, updated_at: now }),
});
if (!live.ok) fail(`recorded, but could not be made live (HTTP ${live.status}): ${await live.text()}`);
console.log(`${platform} ${version} is now the live ${channel} build.`);
