import { NextResponse } from 'next/server';
import { z } from 'zod';
import { platformSchema, releaseChannelSchema } from '@/domain';
import { requireAdmin } from '@/lib/admin';
import { env } from '@/lib/env';
import { adminClient } from '@/lib/supabase';

export const runtime = 'nodejs';
export const dynamic = 'force-dynamic';

/**
 * Publishing a build.
 *
 * Two things this route exists to guarantee:
 *
 *  - Windows and macOS are independent. Publishing one never touches the other,
 *    because activation is scoped to a platform and channel.
 *  - An installer is uploaded before its row exists, and the row is only
 *    activated deliberately. A build that is present but not active is
 *    invisible to customers, which is what makes a staged release possible.
 */

const uploadRequestSchema = z.object({
  platform: platformSchema,
  version: z.string().min(1).max(40).regex(/^[\w.\-+]+$/, 'Version may only contain word characters, . - and +'),
  fileName: z.string().min(1).max(200),
});

const publishSchema = z.object({
  platform: platformSchema,
  version: z.string().min(1).max(40),
  channel: releaseChannelSchema.default('internal'),
  minimumOsVersion: z.string().max(40).default(''),
  releaseNotes: z.string().max(20_000).default(''),
  artifactKey: z.string().min(1),
  artifactSizeBytes: z.number().int().nonnegative().default(0),
  sha256: z.string().max(128).default(''),
});

const forbidden = () =>
  NextResponse.json({ error: 'This area is for administrators.' }, { status: 403 });

/** List every build, newest first. Admins see retired and pre-release ones too. */
export async function GET(): Promise<Response> {
  try {
    await requireAdmin();
  } catch {
    return forbidden();
  }

  const { data, error } = await adminClient()
    .from('release_builds')
    .select('*')
    .order('created_at', { ascending: false })
    .limit(100);

  if (error) return NextResponse.json({ error: 'The release catalogue could not be read' }, { status: 500 });
  return NextResponse.json({ builds: data ?? [] });
}

/**
 * Record a build whose artifact is already in the private bucket (uploaded
 * from the browser below, or by `scripts/publish-release.mjs`).
 *
 * Installers run to a hundred megabytes and more, which is far past what a
 * serverless request body can carry — so the file goes straight from the
 * browser to storage with a signed upload URL (see PUT below) and this route
 * only writes the row.
 */
export async function POST(request: Request): Promise<Response> {
  try {
    await requireAdmin();
  } catch {
    return forbidden();
  }

  const parsed = publishSchema.safeParse(await request.json().catch(() => null));
  if (!parsed.success) {
    return NextResponse.json({ error: parsed.error.issues[0]?.message ?? 'Invalid build' }, { status: 400 });
  }
  const build = parsed.data;

  // Replacing the bytes under a live row would hand customers a file whose
  // checksum the updater no longer believes, so a live version is refused.
  const { data: existing } = await adminClient()
    .from('release_builds')
    .select('active')
    .eq('platform', build.platform)
    .eq('channel', build.channel)
    .eq('version', build.version)
    .maybeSingle();
  if (existing?.active) {
    return NextResponse.json(
      { error: `${build.platform} ${build.version} is already live on ${build.channel}. Publish a new version instead.` },
      { status: 409 },
    );
  }

  const { data, error } = await adminClient()
    .from('release_builds')
    .upsert(
      {
        platform: build.platform,
        version: build.version,
        channel: build.channel,
        minimum_os_version: build.minimumOsVersion,
        release_notes: build.releaseNotes,
        artifact_key: build.artifactKey,
        artifact_size_bytes: build.artifactSizeBytes,
        sha256: build.sha256,
        // Never active on arrival: publishing is a separate, deliberate act.
        active: false,
      },
      { onConflict: 'platform,channel,version' },
    )
    .select('id')
    .single();

  if (error || !data) {
    return NextResponse.json({ error: error?.message ?? 'The build could not be recorded' }, { status: 500 });
  }
  return NextResponse.json({ id: data.id });
}

/** Mint a signed URL so the browser can upload the installer directly. */
export async function PUT(request: Request): Promise<Response> {
  try {
    await requireAdmin();
  } catch {
    return forbidden();
  }

  const parsed = uploadRequestSchema.safeParse(await request.json().catch(() => null));
  if (!parsed.success) {
    return NextResponse.json({ error: parsed.error.issues[0]?.message ?? 'Invalid upload' }, { status: 400 });
  }

  const { platform, version, fileName } = parsed.data;
  const safeName = fileName.replace(/[^\w.\-]+/g, '_');
  const artifactKey = `${platform}/${version}/${safeName}`;

  const { data, error } = await adminClient()
    .storage.from(env.releaseBucket)
    .createSignedUploadUrl(artifactKey, { upsert: true });

  if (error || !data) {
    return NextResponse.json({ error: error?.message ?? 'The upload could not be prepared' }, { status: 500 });
  }

  return NextResponse.json({ artifactKey, signedUrl: data.signedUrl, token: data.token });
}
