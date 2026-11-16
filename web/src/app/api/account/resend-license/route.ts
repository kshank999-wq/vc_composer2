import { NextResponse } from 'next/server';
import { adminClient, currentUser } from '@/lib/supabase';
import { sendLicenseReminder } from '@/lib/email';

export const runtime = 'nodejs';
export const dynamic = 'force-dynamic';

/** No more than one reminder every few minutes per account. */
const COOLDOWN_MS = 5 * 60 * 1000;

/**
 * Send a customer their own licence again.
 *
 * It only ever repeats something the caller already owns, to the address on
 * their own account — there is no recipient field — so the worst a misfire can
 * do is send a duplicate. A short cooldown keeps it from becoming a way to
 * bombard an inbox.
 */
export async function POST(): Promise<Response> {
  const user = await currentUser();
  if (!user?.email) {
    return NextResponse.json({ error: 'Sign in to have your licence re-sent' }, { status: 401 });
  }

  const client = adminClient();

  const { data: license, error } = await client
    .from('licenses')
    .select('serial')
    .eq('user_id', user.id)
    .eq('status', 'active')
    .order('created_at', { ascending: false })
    .limit(1)
    .maybeSingle();

  if (error) return NextResponse.json({ error: 'Your licence could not be read' }, { status: 500 });
  if (!license) return NextResponse.json({ error: 'There is no active licence on this account' }, { status: 404 });

  const { data: recent } = await client
    .from('email_events')
    .select('created_at')
    .eq('user_id', user.id)
    .like('template', 'license_reminder@%')
    .order('created_at', { ascending: false })
    .limit(1)
    .maybeSingle();

  if (recent?.created_at && Date.now() - Date.parse(recent.created_at as string) < COOLDOWN_MS) {
    return NextResponse.json(
      { error: 'A reminder was sent very recently. Check your inbox, then try again in a few minutes.' },
      { status: 429 },
    );
  }

  const result = await sendLicenseReminder({ to: user.email, userId: user.id, serial: license.serial });
  if (!result.sent) {
    return NextResponse.json({ error: result.error ?? 'The email could not be sent' }, { status: 502 });
  }

  return NextResponse.json({ sent: true });
}
