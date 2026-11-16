import { NextResponse } from 'next/server';
import { parsePublicChannel, platformSchema } from '@/domain';
import { issueDownload } from '@/lib/downloads';
import { env } from '@/lib/env';
import { adminClient, currentUser } from '@/lib/supabase';

export const runtime = 'nodejs';
export const dynamic = 'force-dynamic';

/**
 * Issue a download for the current build of one platform.
 *
 * `?channel=stable` (the default) is licence-gated; `?channel=preview` serves
 * the active free preview build to any signed-in account. The installer is
 * never a permanent public URL: every request re-checks the caller and mints
 * a short-lived signed URL (`lib/downloads.ts`).
 *
 * Authenticated by session cookie or, from the desktop app, by
 * `Authorization: Bearer <Supabase access token>`.
 */
export async function GET(request: Request, { params }: { params: { platform: string } }): Promise<Response> {
  const platform = platformSchema.safeParse(params.platform);
  if (!platform.success) {
    return NextResponse.json({ error: 'Unknown platform' }, { status: 404 });
  }

  const channel = parsePublicChannel(new URL(request.url).searchParams.get('channel'));
  if (!channel) {
    return NextResponse.json({ error: 'Unknown channel. Use "stable" or "preview".' }, { status: 400 });
  }

  const user = await currentUser();
  const answer = await issueDownload(
    { platform: platform.data, channel, userId: user?.id ?? null },
    { client: adminClient(), bucket: env.releaseBucket, ttlSeconds: env.releaseDownloadTtlSeconds },
  );
  return NextResponse.json(answer.body, { status: answer.status });
}
