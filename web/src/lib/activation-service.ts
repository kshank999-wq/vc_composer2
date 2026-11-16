import type { SupabaseClient } from '@supabase/supabase-js';
import {
  decideActivation,
  deviceActivationSchema,
  explainRefusal,
  licenseLive,
  licenseSchema,
  seatLimit,
  type ActivationOutcome,
  type DeviceActivation,
  type License,
  type Platform,
} from '@/domain';
import { env } from './env';
import { PRODUCT_SHORT_NAME } from './site';
import { adminClient } from './supabase';

/**
 * Activating and freeing device seats.
 *
 * The decision itself lives in the domain and is tested there; this module is
 * the part that reads and writes rows. It takes its client as an argument so
 * the sequencing can be tested against a fake, for the same reason fulfillment
 * does: a license that will not activate is a customer who paid and cannot
 * work.
 */

export interface ActivateInput {
  userId: string;
  /** Absent: try the account's own licences, liveliest first. */
  serial?: string | undefined;
  deviceFingerprint: string;
  deviceName: string;
  platform: Platform;
  appVersion: string;
}

export interface ActivateResult {
  outcome: ActivationOutcome;
  message: string | null;
  /** `maxActivations` is null where the licence covers any number of computers. */
  license: { serial: string; maxActivations: number | null } | null;
}

const licenseRowToDomain = (row: Record<string, unknown>): License =>
  licenseSchema.parse({
    id: row['id'],
    userId: row['user_id'],
    orderId: row['order_id'],
    serial: row['serial'],
    status: row['status'],
    entitledPlatforms: row['entitled_platforms'],
    maxActivations: row['max_activations'],
    expiresAt: row['expires_at'],
    grant: row['grant_kind'] ?? undefined,
    grantedTo: row['granted_to'] ?? undefined,
    createdAt: row['created_at'],
    updatedAt: row['updated_at'],
  });

const activationRowToDomain = (row: Record<string, unknown>): DeviceActivation =>
  deviceActivationSchema.parse({
    id: row['id'],
    licenseId: row['license_id'],
    deviceFingerprint: row['device_fingerprint'],
    deviceName: row['device_name'],
    platform: row['platform'],
    appVersion: row['app_version'],
    activatedAt: row['activated_at'],
    lastSeenAt: row['last_seen_at'],
    deactivatedAt: row['deactivated_at'],
  });

export const activateDevice = async (
  input: ActivateInput,
  client: SupabaseClient = adminClient(),
): Promise<ActivateResult> => {
  const serial = input.serial?.trim().toUpperCase() ?? '';

  // Scoped to the caller's own licenses: a serial is not a bearer token, so
  // knowing one is not enough to activate against someone else's account.
  let query = client.from('licenses').select('*').eq('user_id', input.userId);
  if (serial) query = query.eq('serial', serial);
  const { data: licenseRows } = await query;
  const now = new Date();
  // Without a serial, every licence on the account is a candidate, live ones
  // first, so a customer holding a lapsed licence and a live one activates on
  // the one that is paid for.
  const candidates = ((licenseRows ?? []) as Record<string, unknown>[])
    .map(licenseRowToDomain)
    .sort((a, b) => Number(licenseLive(b, now)) - Number(licenseLive(a, now)));

  if (candidates.length === 0) {
    return {
      outcome: { result: 'refused', reason: 'license_inactive' },
      message: serial
        ? 'That license is not on this account.'
        : `There is no ${PRODUCT_SHORT_NAME} licence on this account. Subscribe at ${new URL(env.siteUrl).host}, then activate.`,
      license: null,
    };
  }

  let chosen: { license: License; outcome: ActivationOutcome } | null = null;
  let firstRefusal: ActivationOutcome | null = null;
  for (const license of candidates) {
    const { data: activationRows } = await client
      .from('device_activations')
      .select('*')
      .eq('license_id', license.id);
    const activations = ((activationRows ?? []) as Record<string, unknown>[]).map(activationRowToDomain);
    const decided = decideActivation({
      license,
      activations,
      deviceFingerprint: input.deviceFingerprint,
      platform: input.platform,
    });
    if (decided.result !== 'refused') {
      chosen = { license, outcome: decided };
      break;
    }
    firstRefusal ??= decided;
  }

  if (!chosen) {
    const outcome = firstRefusal!;
    return { outcome, message: explainRefusal(outcome), license: null };
  }
  const { license, outcome } = chosen;

  const nowIso = new Date().toISOString();
  // Unique on (license_id, device_fingerprint), so a device that already has a
  // record reclaims it rather than creating a second one.
  const { error } = await client.from('device_activations').upsert(
    {
      license_id: license.id,
      device_fingerprint: input.deviceFingerprint,
      device_name: input.deviceName,
      platform: input.platform,
      app_version: input.appVersion,
      activated_at: nowIso,
      last_seen_at: nowIso,
      deactivated_at: null,
    },
    { onConflict: 'license_id,device_fingerprint' },
  );

  if (error) {
    return {
      outcome: { result: 'refused', reason: 'license_inactive' },
      message: 'The activation could not be recorded. Try again in a moment.',
      license: null,
    };
  }

  return {
    outcome,
    message: null,
    license: { serial: license.serial, maxActivations: seatLimit(license) },
  };
};

/**
 * Free a seat. This is the lost-device replacement flow: the customer does it
 * themselves, and the record is kept rather than deleted so support can still
 * see what happened.
 */
export const deactivateDevice = async (
  input: { userId: string; activationId: string },
  client: SupabaseClient = adminClient(),
): Promise<{ ok: boolean; error: string | null }> => {
  const { data: row } = await client
    .from('device_activations')
    .select('id, license_id')
    .eq('id', input.activationId)
    .maybeSingle();

  if (!row) return { ok: false, error: 'No such device.' };

  const { data: owned } = await client
    .from('licenses')
    .select('id')
    .eq('id', row.license_id)
    .eq('user_id', input.userId)
    .maybeSingle();

  if (!owned) return { ok: false, error: 'That device is not on a license you own.' };

  const { error } = await client
    .from('device_activations')
    .update({ deactivated_at: new Date().toISOString() })
    .eq('id', input.activationId);

  return error ? { ok: false, error: error.message } : { ok: true, error: null };
};

/** Devices on every license this account owns, for the account page. */
export const listDevices = async (
  userId: string,
  client: SupabaseClient = adminClient(),
): Promise<Array<{ activation: DeviceActivation; serial: string; maxActivations: number | null }>> => {
  const { data: licenseRows } = await client.from('licenses').select('*').eq('user_id', userId);
  const licenses = ((licenseRows ?? []) as Record<string, unknown>[]).map(licenseRowToDomain);
  if (licenses.length === 0) return [];

  const { data: activationRows } = await client
    .from('device_activations')
    .select('*')
    .in(
      'license_id',
      licenses.map((license) => license.id),
    );

  return ((activationRows ?? []) as Record<string, unknown>[])
    .map(activationRowToDomain)
    .map((activation) => {
      const license = licenses.find((candidate) => candidate.id === activation.licenseId);
      return license
        ? { activation, serial: license.serial, maxActivations: seatLimit(license) }
        : null;
    })
    .filter((entry): entry is { activation: DeviceActivation; serial: string; maxActivations: number | null } =>
      entry !== null,
    );
};
