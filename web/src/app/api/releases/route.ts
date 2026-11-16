import { NextResponse } from 'next/server';
import { parsePublicChannel } from '@/domain';
import { adminClient } from '@/lib/supabase';

export const runtime = 'nodejs';
export const dynamic = 'force-dynamic';

/**
 * Release metadata for the desktop updater.
 *
 * The app asks "is there a newer build for my platform?" and never holds a
 * hard-coded download URL: the artifact itself comes from
 * `/api/downloads/[platform]`, which checks the caller and signs a short-lived
 * URL. This endpoint exposes version information only, and needs no sign-in.
 *
 * `?channel=preview` lists the live preview builds instead of the stable ones;
 * with no channel it is stable, as it always was.
 */
export async function GET(request: Request): Promise<Response> {
  const channel = parsePublicChannel(new URL(request.url).searchParams.get('channel'));
  if (!channel) {
    return NextResponse.json({ error: 'Unknown channel. Use "stable" or "preview".' }, { status: 400 });
  }

  const { data, error } = await adminClient()
    .from('release_builds')
    .select('platform, version, minimum_os_version, release_notes, published_at, sha256')
    .eq('channel', channel)
    .eq('active', true);

  if (error) {
    return NextResponse.json({ error: 'Could not read the release catalogue' }, { status: 500 });
  }

  return NextResponse.json({
    builds: (data ?? []).map((build) => ({
      platform: build.platform,
      version: build.version,
      minimumOsVersion: build.minimum_os_version,
      releaseNotes: build.release_notes,
      publishedAt: build.published_at,
      sha256: build.sha256,
    })),
  });
}
