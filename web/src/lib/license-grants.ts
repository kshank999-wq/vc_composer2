import type { SupabaseClient } from '@supabase/supabase-js';
import {
  licenseGrantRefusal,
  licenseGrantSchema,
  licenseGrantTerms,
  licenseSeatsPhrase,
  type LicenseGrant,
  type LicenseGrantRequest,
  type LicenseStatus,
} from '@/domain';
import { generateSerial } from './license';
import { adminClient } from './supabase';

/**
 * Giving a licence: an academic giveaway or the owner's master licence, issued
 * from the admin console.
 *
 * It is written the way a licence issued by hand has always been kept — a $0
 * order and a licence row beside it — so every gate that reads a licence
 * reads this one without a second path. The account is found by email, and
 * made where there is none, exactly as a checkout without a sign-in does
 * (`fulfillment.ts`): the recipient signs in with an emailed link and the
 * licence is already there.
 */

export interface GivenLicenseRow {
  id: string;
  serial: string;
  email: string | null;
  grant: LicenseGrant;
  grantedTo: string;
  status: LicenseStatus;
  seats: string;
  inUse: number;
  expiresAt: string | null;
  createdAt: string;
}

export type IssueResult =
  | { ok: true; license: GivenLicenseRow; accountCreated: boolean; userId: string }
  | { ok: false; error: string };

const findByEmail = async (client: SupabaseClient, email: string): Promise<string | null> => {
  const { data } = await client.from('profiles').select('id').ilike('email', email).maybeSingle();
  return (data?.id as string | undefined) ?? null;
};

const accountFor = async (client: SupabaseClient, email: string): Promise<{ id: string; created: boolean } | null> => {
  const existing = await findByEmail(client, email);
  if (existing) return { id: existing, created: false };
  const { data } = await client.auth.admin.createUser({
    email,
    email_confirm: true,
    user_metadata: { source: 'license_grant' },
  });
  if (data?.user) return { id: data.user.id, created: true };
  // Made a moment ago by something else: read it.
  const raced = await findByEmail(client, email);
  return raced ? { id: raced, created: false } : null;
};

export const issueLicense = async (
  request: LicenseGrantRequest,
  adminId: string,
  options: { client?: SupabaseClient; newSerial?: () => string; now?: Date } = {},
): Promise<IssueResult> => {
  const client = options.client ?? adminClient();
  const newSerial = options.newSerial ?? generateSerial;
  const now = options.now ?? new Date();

  // Asked again here, so the route cannot get past the reading by not reading it.
  const refusal = licenseGrantRefusal(request, now);
  if (refusal) return { ok: false, error: refusal };

  const email = request.email.trim().toLowerCase();
  const account = await accountFor(client, email);
  if (!account) return { ok: false, error: `No account could be found or made for ${email}.` };

  const terms = licenseGrantTerms(request);
  const nowIso = now.toISOString();
  const { data: order, error: orderError } = await client
    .from('orders')
    .insert({ user_id: account.id, status: 'paid', amount_cents: 0, currency: 'usd', paid_at: nowIso })
    .select('id')
    .single();
  if (orderError || !order) return { ok: false, error: `The licence could not be recorded: ${orderError?.message ?? 'no order'}` };

  const row = {
    user_id: account.id,
    order_id: order.id as string,
    status: 'active',
    entitled_platforms: ['windows', 'macos'],
    max_activations: terms.maxActivations,
    expires_at: terms.expiresAt,
    grant_kind: terms.grant,
    granted_to: terms.grantedTo,
    granted_by: adminId,
  };
  // A serial is random over 32^20; a collision is a retry, not an error.
  let inserted: { id: string; serial: string; created_at: string } | null = null;
  let lastError = '';
  for (let attempt = 0; attempt < 3 && !inserted; attempt += 1) {
    const { data, error } = await client.from('licenses').insert({ ...row, serial: newSerial() }).select('id, serial, created_at').single();
    if (data) inserted = data as { id: string; serial: string; created_at: string };
    else lastError = error?.message ?? 'unknown error';
  }
  if (!inserted) {
    await client.from('orders').delete().eq('id', order.id);
    return { ok: false, error: `The licence could not be issued: ${lastError}` };
  }

  return {
    ok: true,
    userId: account.id,
    accountCreated: account.created,
    license: {
      id: inserted.id,
      serial: inserted.serial,
      email,
      grant: terms.grant,
      grantedTo: terms.grantedTo,
      status: 'active',
      seats: licenseSeatsPhrase({ grant: terms.grant, maxActivations: terms.maxActivations }),
      inUse: 0,
      expiresAt: terms.expiresAt,
      createdAt: inserted.created_at,
    },
  };
};

/** Every given licence, newest first, with whose it is and how many computers are on it. */
export const listGivenLicenses = async (client: SupabaseClient = adminClient()): Promise<GivenLicenseRow[]> => {
  const { data: rows } = await client
    .from('licenses')
    .select('id, user_id, serial, status, max_activations, expires_at, grant_kind, granted_to, created_at')
    .neq('grant_kind', 'purchase')
    .order('created_at', { ascending: false })
    .limit(500);
  const licenses = (rows ?? []) as Record<string, unknown>[];
  if (licenses.length === 0) return [];

  const [{ data: profiles }, { data: activations }] = await Promise.all([
    client.from('profiles').select('id, email').in('id', [...new Set(licenses.map((one) => one['user_id'] as string))]),
    client
      .from('device_activations')
      .select('license_id')
      .is('deactivated_at', null)
      .in('license_id', licenses.map((one) => one['id'] as string)),
  ]);
  const emailOf = new Map(((profiles ?? []) as { id: string; email: string | null }[]).map((one) => [one.id, one.email]));
  const inUse = new Map<string, number>();
  for (const one of (activations ?? []) as { license_id: string }[]) inUse.set(one.license_id, (inUse.get(one.license_id) ?? 0) + 1);

  return licenses.map((one) => {
    const grant = licenseGrantSchema.catch('purchase').parse(one['grant_kind']);
    return {
      id: one['id'] as string,
      serial: one['serial'] as string,
      email: emailOf.get(one['user_id'] as string) ?? null,
      grant,
      grantedTo: (one['granted_to'] as string) ?? '',
      status: one['status'] as LicenseStatus,
      seats: licenseSeatsPhrase({ grant, maxActivations: one['max_activations'] as number }),
      inUse: inUse.get(one['id'] as string) ?? 0,
      expiresAt: (one['expires_at'] as string | null) ?? null,
      createdAt: one['created_at'] as string,
    };
  });
};

/**
 * Change a given licence: suspend or restore it, or move an academic one's end.
 * Never a bought one — those are Stripe's to change, and support's tools say so.
 */
export const changeGivenLicense = async (
  change: { licenseId: string; status?: 'active' | 'suspended' | 'revoked'; expiresAt?: string | null },
  client: SupabaseClient = adminClient(),
  now: Date = new Date(),
): Promise<{ ok: boolean; error: string | null }> => {
  const { data: found } = await client.from('licenses').select('id, grant_kind').eq('id', change.licenseId).maybeSingle();
  if (!found || found.grant_kind === 'purchase') return { ok: false, error: 'No such given licence.' };
  const update: Record<string, unknown> = {};
  if (change.status) update['status'] = change.status;
  if (change.expiresAt !== undefined) {
    if (found.grant_kind === 'master') return { ok: false, error: 'A master licence does not end.' };
    if (change.expiresAt !== null && !(new Date(change.expiresAt).getTime() > now.getTime())) {
      return { ok: false, error: 'An academic licence has to end after today.' };
    }
    update['expires_at'] = change.expiresAt;
  }
  if (Object.keys(update).length === 0) return { ok: false, error: 'Nothing to change.' };
  const { error } = await client.from('licenses').update(update).eq('id', change.licenseId);
  return error ? { ok: false, error: error.message } : { ok: true, error: null };
};
