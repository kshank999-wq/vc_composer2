import { describe, expect, it } from 'vitest';
import type { LicenseGrantRequest } from '@/domain';
import { changeGivenLicense, issueLicense } from '../license-grants';
import { createFakeSupabase } from './fake-supabase';

/**
 * Giving a licence: a $0 order and a licence beside it, to an
 * account found by email or made for it.
 */

const NOW = new Date('2026-10-25T12:00:00.000Z');
const ADMIN = '00000000-0000-4000-8000-00000000a0a0';

const academic = (overrides: Partial<LicenseGrantRequest> = {}): LicenseGrantRequest => ({
  kind: 'academic',
  email: 'Teacher@School.edu',
  grantedTo: 'Hillside High — Film Scoring',
  seats: 25,
  expiresAt: '2027-06-30T23:59:59.000Z',
  ...overrides,
});

let serial = 0;
const options = (fake: ReturnType<typeof createFakeSupabase>) => ({
  client: fake.client,
  now: NOW,
  newSerial: () => `VCC-GIVEN-${(serial += 1)}`,
});

describe('giving an academic licence', () => {
  it('makes an account for a new address, a $0 order, and the licence with its seats and end', async () => {
    const fake = createFakeSupabase();
    const issued = await issueLicense(academic(), ADMIN, options(fake));
    expect(issued.ok).toBe(true);
    if (!issued.ok) return;
    expect(issued.accountCreated).toBe(true);
    expect(fake.state.authUsers[0]?.email).toBe('teacher@school.edu');
    expect(fake.state.orders[0]).toMatchObject({ amount_cents: 0, status: 'paid', user_id: issued.userId });
    expect(fake.state.licenses[0]).toMatchObject({
      grant_kind: 'academic',
      granted_to: 'Hillside High — Film Scoring',
      granted_by: ADMIN,
      max_activations: 25,
      expires_at: '2027-06-30T23:59:59.000Z',
      status: 'active',
    });
    expect(issued.license.seats).toBe('25 computers');
  });

  it('goes to the account already behind an address', async () => {
    const fake = createFakeSupabase({ profiles: [{ id: 'existing', email: 'teacher@school.edu' }] });
    const issued = await issueLicense(academic(), ADMIN, options(fake));
    expect(issued).toMatchObject({ ok: true, accountCreated: false, userId: 'existing' });
    expect(fake.state.authUsers).toHaveLength(0);
  });

  it('refuses what the domain refuses, and writes nothing', async () => {
    const fake = createFakeSupabase();
    const issued = await issueLicense(academic({ grantedTo: '' }), ADMIN, options(fake));
    expect(issued.ok).toBe(false);
    expect(fake.state.writes).toHaveLength(0);
  });
});

describe('giving the master licence', () => {
  it('never ends and covers any number of computers', async () => {
    const fake = createFakeSupabase({ profiles: [{ id: 'owner', email: 'owner@example.com' }] });
    const issued = await issueLicense(
      { kind: 'master', email: 'owner@example.com', grantedTo: '', seats: 0, expiresAt: '2027-01-01T00:00:00.000Z' },
      ADMIN,
      options(fake),
    );
    expect(issued.ok).toBe(true);
    if (!issued.ok) return;
    expect(fake.state.licenses[0]).toMatchObject({ grant_kind: 'master', expires_at: null });
    expect(issued.license.seats).toBe('any number of computers');
  });
});

describe('changing a given licence', () => {
  it('suspends one and moves an academic one’s end, but leaves bought and master ones their own', async () => {
    const fake = createFakeSupabase({
      licenses: [
        { id: 'given', grant_kind: 'academic', status: 'active', expires_at: '2027-01-01T00:00:00.000Z' },
        { id: 'bought', grant_kind: 'purchase', status: 'active' },
        { id: 'master', grant_kind: 'master', status: 'active', expires_at: null },
      ],
    });
    expect(await changeGivenLicense({ licenseId: 'given', status: 'suspended' }, fake.client, NOW)).toEqual({ ok: true, error: null });
    expect(fake.state.licenses[0]?.['status']).toBe('suspended');
    expect((await changeGivenLicense({ licenseId: 'given', expiresAt: '2028-01-01T00:00:00.000Z' }, fake.client, NOW)).ok).toBe(true);
    expect(fake.state.licenses[0]?.['expires_at']).toBe('2028-01-01T00:00:00.000Z');
    expect((await changeGivenLicense({ licenseId: 'given', expiresAt: '2026-01-01T00:00:00.000Z' }, fake.client, NOW)).ok).toBe(false);
    expect((await changeGivenLicense({ licenseId: 'bought', status: 'revoked' }, fake.client, NOW)).ok).toBe(false);
    expect((await changeGivenLicense({ licenseId: 'master', expiresAt: '2028-01-01T00:00:00.000Z' }, fake.client, NOW)).ok).toBe(false);
    expect(fake.state.licenses[1]?.['status']).toBe('active');
  });
});
