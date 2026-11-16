import { describe, expect, it } from 'vitest';
import { downloadGate, licenseSchema, parsePublicChannel, type License } from '../index';

/**
 * The download gate: stable is licence-gated exactly as before, and the free
 * preview channel is open to every signed-in account.
 */

const NOW = new Date('2026-11-16T12:00:00.000Z');

const licence = (over: Partial<License> = {}): License =>
  licenseSchema.parse({
    id: '00000000-0000-4000-8000-000000000001',
    userId: '00000000-0000-4000-8000-000000000002',
    orderId: '00000000-0000-4000-8000-000000000003',
    serial: 'VCC-AAAAA-BBBBB-CCCCC-DDDDD',
    createdAt: '2026-10-01T00:00:00.000Z',
    updatedAt: '2026-10-01T00:00:00.000Z',
    ...over,
  });

describe('which channel was asked for', () => {
  it('is stable when nothing is said, so older callers are unchanged', () => {
    expect(parsePublicChannel(null)).toBe('stable');
    expect(parsePublicChannel(undefined)).toBe('stable');
    expect(parsePublicChannel('')).toBe('stable');
  });

  it('accepts the two customer channels', () => {
    expect(parsePublicChannel('stable')).toBe('stable');
    expect(parsePublicChannel('preview')).toBe('preview');
  });

  it('refuses the staging channels and anything unknown', () => {
    expect(parsePublicChannel('beta')).toBeNull();
    expect(parsePublicChannel('internal')).toBeNull();
    expect(parsePublicChannel('PREVIEW')).toBeNull();
    expect(parsePublicChannel('nightly')).toBeNull();
  });
});

describe('the preview channel', () => {
  it('is open to any signed-in account, with no licence at all', () => {
    expect(downloadGate({ channel: 'preview', platform: 'windows', signedIn: true, licenses: [], now: NOW })).toEqual({
      allowed: true,
    });
    expect(downloadGate({ channel: 'preview', platform: 'macos', signedIn: true, licenses: [], now: NOW })).toEqual({
      allowed: true,
    });
  });

  it('is open to an account whose licence has lapsed or been revoked', () => {
    const lapsed = [licence({ expiresAt: '2026-01-01T00:00:00.000Z' }), licence({ status: 'revoked' })];
    expect(downloadGate({ channel: 'preview', platform: 'windows', signedIn: true, licenses: lapsed, now: NOW }).allowed).toBe(
      true,
    );
  });

  it('still asks a stranger to sign in', () => {
    expect(downloadGate({ channel: 'preview', platform: 'windows', signedIn: false, licenses: [], now: NOW })).toEqual({
      allowed: false,
      reason: 'sign_in',
    });
  });
});

describe('the stable channel', () => {
  it('needs a live licence covering the platform', () => {
    expect(downloadGate({ channel: 'stable', platform: 'windows', signedIn: true, licenses: [licence()], now: NOW })).toEqual({
      allowed: true,
    });
  });

  it('refuses a signed-in account with no licence', () => {
    expect(downloadGate({ channel: 'stable', platform: 'windows', signedIn: true, licenses: [], now: NOW })).toEqual({
      allowed: false,
      reason: 'no_licence',
    });
  });

  it('refuses a lapsed, revoked or wrong-platform licence', () => {
    for (const held of [
      licence({ expiresAt: '2026-11-01T00:00:00.000Z' }),
      licence({ status: 'revoked' }),
      licence({ status: 'expired' }),
      licence({ entitledPlatforms: ['macos'] }),
    ]) {
      expect(downloadGate({ channel: 'stable', platform: 'windows', signedIn: true, licenses: [held], now: NOW })).toEqual({
        allowed: false,
        reason: 'no_licence',
      });
    }
  });

  it('asks a stranger to sign in before anything else', () => {
    expect(downloadGate({ channel: 'stable', platform: 'windows', signedIn: false, licenses: [licence()], now: NOW })).toEqual({
      allowed: false,
      reason: 'sign_in',
    });
  });
});
