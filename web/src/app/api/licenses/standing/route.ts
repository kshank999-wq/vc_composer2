import { NextResponse } from 'next/server';
import { deskStandingFrom, licenseGrantSchema, licenseStatusSchema } from '@/domain';
import { serverClient } from '@/lib/supabase';

export const runtime = 'nodejs';
export const dynamic = 'force-dynamic';

/**
 * This account's licence standing, for the desktop app to write down.
 *
 * A machine that cannot reach the internet must not refuse somebody their own
 * project, so the app records this answer with the day it got it and does the
 * reading itself (`writingStanding` in `src/domain/subscription.ts` is the
 * reference). **Nothing here says what a lapse means**: the status and the
 * date are facts.
 *
 * **No admin client**: a customer reads their own licence rows under row level
 * security, so this is their own session asking its own question and cannot
 * see anybody else's. Authenticated by bearer token or cookie, since
 * `serverClient` reads both.
 */
export async function GET(): Promise<Response> {
  const db = serverClient();
  const { data: auth } = await db.auth.getUser();
  if (!auth.user) return NextResponse.json({ error: 'Sign in to check your licence' }, { status: 401 });

  const { data, error } = await db.from('licenses').select('status, expires_at, grant_kind');
  if (error) return NextResponse.json({ error: 'The licence could not be read' }, { status: 502 });

  const licenses = (data ?? []).flatMap((row) => {
    // A status this build cannot name is left out rather than read as lapsed:
    // nothing uncertain takes editing away.
    const status = licenseStatusSchema.safeParse(row.status);
    if (!status.success) return [];
    // How it came to be, so an academic licence that ends is not told to renew.
    const grant = licenseGrantSchema.catch('purchase').parse(row.grant_kind);
    return [{ status: status.data, expiresAt: (row.expires_at as string | null) ?? null, grant }];
  });

  return NextResponse.json({ standing: deskStandingFrom(licenses, new Date()) });
}
