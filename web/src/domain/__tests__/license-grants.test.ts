import { describe, expect, it } from 'vitest';
import {
  ACADEMIC_MAX_SEATS,
  academicDefaultEnd,
  decideActivation,
  deskStandingFrom,
  licenseGrantRefusal,
  licenseGrantTerms,
  licenseSchema,
  licenseSeatsPhrase,
  noteStanding,
  seatLimit,
  summariseActivations,
  writingStanding,
  type DeviceActivation,
  type License,
  type LicenseGrantRequest,
} from '../index';

/** Licences given rather than bought: academic and master. */

const NOW = new Date('2026-10-25T12:00:00.000Z');

const licence = (overrides: Partial<License> = {}): License =>
  licenseSchema.parse({
    id: '00000000-0000-4000-8000-000000000001',
    userId: '00000000-0000-4000-8000-000000000002',
    orderId: '00000000-0000-4000-8000-000000000003',
    serial: 'VCC-AAAAA-BBBBB-CCCCC-DDDDD',
    createdAt: '2026-10-01T00:00:00.000Z',
    updatedAt: '2026-10-01T00:00:00.000Z',
    ...overrides,
  });

const machines = (license: License, count: number): DeviceActivation[] =>
  Array.from({ length: count }, (_, at) => ({
    id: `00000000-0000-4000-8000-0000000001${String(at).padStart(2, '0')}`,
    licenseId: license.id,
    deviceFingerprint: `machine-${at}`,
    deviceName: `Machine ${at}`,
    platform: 'macos' as const,
    appVersion: '1.0.0',
    activatedAt: '2026-10-02T00:00:00.000Z',
    lastSeenAt: null,
    deactivatedAt: null,
  })) as DeviceActivation[];

const request = (overrides: Partial<LicenseGrantRequest> = {}): LicenseGrantRequest => ({
  kind: 'academic',
  email: 'teacher@school.edu',
  grantedTo: 'Hillside High — Film Scoring 101',
  seats: 2,
  expiresAt: academicDefaultEnd(NOW),
  ...overrides,
});

describe('a licence as it was', () => {
  it('reads as bought, covering its seat count', () => {
    const bought = licence();
    expect(bought.grant).toBe('purchase');
    expect(seatLimit(bought)).toBe(2);
    expect(licenseSeatsPhrase(bought)).toBe('2 computers');
  });
});

describe('the master licence', () => {
  it('activates on any number of computers', () => {
    const master = licence({ grant: 'master' });
    expect(seatLimit(master)).toBeNull();
    expect(
      decideActivation({ license: master, activations: machines(master, 40), deviceFingerprint: 'machine-new', platform: 'windows', now: NOW }),
    ).toEqual({ result: 'activated', reason: 'new_device' });
    const summary = summariseActivations(master, machines(master, 40));
    expect(summary).toMatchObject({ inUse: 40, limit: null, slotsFree: null });
    expect(licenseSeatsPhrase(master)).toBe('any number of computers');
  });

  it('still stops when it is revoked', () => {
    const master = licence({ grant: 'master', status: 'revoked' });
    expect(decideActivation({ license: master, activations: [], deviceFingerprint: 'x', platform: 'macos', now: NOW }).result).toBe('refused');
  });

  it('never ends, whatever the request said', () => {
    expect(licenseGrantTerms(request({ kind: 'master', expiresAt: '2027-01-01T00:00:00.000Z', seats: 1 }))).toMatchObject({
      grant: 'master',
      expiresAt: null,
    });
  });
});

describe('an academic licence', () => {
  it('covers the computers it was given for, and no more', () => {
    const academic = licence({ grant: 'academic', maxActivations: 3, expiresAt: academicDefaultEnd(NOW) });
    expect(decideActivation({ license: academic, activations: machines(academic, 2), deviceFingerprint: 'n', platform: 'macos', now: NOW }).result).toBe(
      'activated',
    );
    expect(decideActivation({ license: academic, activations: machines(academic, 3), deviceFingerprint: 'n', platform: 'macos', now: NOW })).toMatchObject({
      result: 'refused',
      reason: 'no_slots',
      limit: 3,
    });
  });

  it('runs a year by default, to the end of the day', () => {
    expect(academicDefaultEnd(NOW)).toBe('2027-10-25T23:59:59.000Z');
  });

  it('says what is wrong with a request before it is issued', () => {
    expect(licenseGrantRefusal(request(), NOW)).toBeNull();
    expect(licenseGrantRefusal(request({ expiresAt: null }), NOW)).toBeNull();
    expect(licenseGrantRefusal(request({ email: 'nobody' }), NOW)).toMatch(/email/);
    expect(licenseGrantRefusal(request({ grantedTo: ' ' }), NOW)).toMatch(/who/);
    expect(licenseGrantRefusal(request({ seats: 0 }), NOW)).toMatch(/1 to/);
    expect(licenseGrantRefusal(request({ seats: ACADEMIC_MAX_SEATS + 1 }), NOW)).toMatch(/1 to/);
    expect(licenseGrantRefusal(request({ expiresAt: '2026-01-01T00:00:00.000Z' }), NOW)).toMatch(/after today/);
    // A master licence needs only the address.
    expect(licenseGrantRefusal(request({ kind: 'master', grantedTo: '', seats: 0 }), NOW)).toBeNull();
  });

  it('when it ends, the desk says so without telling anyone to renew a subscription', () => {
    const ended = licence({ grant: 'academic', expiresAt: '2026-09-01T00:00:00.000Z' });
    const claim = deskStandingFrom([ended], NOW)!;
    expect(claim.grant).toBe('academic');
    const standing = noteStanding(null, claim, NOW);
    const words = writingStanding(standing, NOW).notice ?? '';
    expect(words).toContain('academic licence ended on 2026-09-01');
    expect(words).not.toContain('subscription');
  });
});

describe('an account with more than one', () => {
  it('is heard as its master licence, which never ends', () => {
    const subscription = licence({ expiresAt: '2026-11-25T00:00:00.000Z' });
    const master = licence({ grant: 'master', id: '00000000-0000-4000-8000-000000000009' as License['id'] });
    expect(deskStandingFrom([subscription, master], NOW)).toEqual({ status: 'active', expiresAt: null, grant: 'master' });
    // A bought licence's claim says nothing about how it came to be.
    expect(deskStandingFrom([subscription], NOW)).toEqual({ status: 'active', expiresAt: '2026-11-25T00:00:00.000Z' });
  });
});
