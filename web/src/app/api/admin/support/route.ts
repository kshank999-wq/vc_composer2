import { NextResponse } from 'next/server';
import { z } from 'zod';
import { describeDevice, deviceActivationSchema } from '@/domain';
import { requireAdmin } from '@/lib/admin';
import { adminClient } from '@/lib/supabase';
import { sendLicenseReminder } from '@/lib/email';

export const runtime = 'nodejs';
export const dynamic = 'force-dynamic';

/**
 * Support tooling, so support never requires a manual database edit.
 *
 * Everything support is likely to be asked for — what did this person buy,
 * which machines are using their seats, why did they not get the email, send it
 * again, free a seat, revoke a license after a chargeback — is here, so nobody
 * ends up writing UPDATE statements against production to answer a ticket.
 *
 * Projects never reach this server, so nothing here can show one.
 */

const forbidden = () =>
  NextResponse.json({ error: 'This area is for administrators.' }, { status: 403 });

export async function GET(request: Request): Promise<Response> {
  try {
    await requireAdmin();
  } catch {
    return forbidden();
  }

  const email = new URL(request.url).searchParams.get('email')?.trim();
  if (!email) return NextResponse.json({ error: 'An email address is required' }, { status: 400 });

  const client = adminClient();
  const { data: profile } = await client
    .from('profiles')
    .select('id, email, display_name, is_admin, created_at')
    .ilike('email', email)
    .maybeSingle();

  if (!profile) return NextResponse.json({ found: false });

  const [{ data: licenses }, { data: orders }, { data: emails }] = await Promise.all([
    client.from('licenses').select('*').eq('user_id', profile.id).order('created_at', { ascending: false }),
    client.from('orders').select('*').eq('user_id', profile.id).order('created_at', { ascending: false }).limit(20),
    client
      .from('email_events')
      .select('template, status, error, created_at')
      .eq('user_id', profile.id)
      .order('created_at', { ascending: false })
      .limit(20),
  ]);

  const licenseIds = (licenses ?? []).map((license) => license.id as string);
  const { data: activations } = licenseIds.length
    ? await client.from('device_activations').select('*').in('license_id', licenseIds)
    : { data: [] };

  return NextResponse.json({
    found: true,
    customer: profile,
    licenses: licenses ?? [],
    orders: orders ?? [],
    emails: emails ?? [],
    devices: (activations ?? []).map((row) => {
      const activation = deviceActivationSchema.parse({
        id: row.id,
        licenseId: row.license_id,
        deviceFingerprint: row.device_fingerprint,
        deviceName: row.device_name,
        platform: row.platform,
        appVersion: row.app_version,
        activatedAt: row.activated_at,
        lastSeenAt: row.last_seen_at,
        deactivatedAt: row.deactivated_at,
      });
      return {
        id: activation.id,
        licenseId: activation.licenseId,
        label: describeDevice(activation),
        activatedAt: activation.activatedAt,
        deactivatedAt: activation.deactivatedAt,
      };
    }),
  });
}

const actionSchema = z.discriminatedUnion('action', [
  z.object({ action: z.literal('resend_license'), licenseId: z.string().uuid() }),
  z.object({ action: z.literal('set_license_status'), licenseId: z.string().uuid(), status: z.enum(['active', 'suspended', 'revoked']) }),
  z.object({ action: z.literal('free_device'), activationId: z.string().uuid() }),
]);

export async function POST(request: Request): Promise<Response> {
  try {
    await requireAdmin();
  } catch {
    return forbidden();
  }

  const parsed = actionSchema.safeParse(await request.json().catch(() => null));
  if (!parsed.success) return NextResponse.json({ error: 'Unknown action' }, { status: 400 });

  const client = adminClient();

  if (parsed.data.action === 'free_device') {
    const { error } = await client
      .from('device_activations')
      .update({ deactivated_at: new Date().toISOString() })
      .eq('id', parsed.data.activationId);
    return error
      ? NextResponse.json({ error: error.message }, { status: 500 })
      : NextResponse.json({ ok: true });
  }

  const { data: license } = await client
    .from('licenses')
    .select('id, serial, user_id')
    .eq('id', parsed.data.licenseId)
    .maybeSingle();
  if (!license) return NextResponse.json({ error: 'No such license' }, { status: 404 });

  if (parsed.data.action === 'set_license_status') {
    const { error } = await client
      .from('licenses')
      .update({ status: parsed.data.status })
      .eq('id', parsed.data.licenseId);
    return error
      ? NextResponse.json({ error: error.message }, { status: 500 })
      : NextResponse.json({ ok: true });
  }

  const { data: profile } = await client
    .from('profiles')
    .select('email')
    .eq('id', license.user_id)
    .maybeSingle();
  if (!profile?.email) return NextResponse.json({ error: 'That account has no email on file' }, { status: 400 });

  const result = await sendLicenseReminder({
    to: profile.email as string,
    userId: license.user_id as string,
    serial: license.serial as string,
  });

  return result.sent
    ? NextResponse.json({ ok: true })
    : NextResponse.json({ error: result.error ?? 'The email could not be sent' }, { status: 502 });
}
