import { describe, expect, it } from 'vitest';
import {
  canDownloadPlatform,
  deviceActivationSchema,
  isActivationSlotAvailable,
  licenseSchema,
  orderSchema,
  releaseBuildSchema,
  type DeviceActivation,
  type License,
} from '../commerce';
import { newId, type DeviceActivationId, type LicenseId, type OrderId, type UserId } from '../common';

/**
 * Entitlement rules.
 *
 * These decide who may download what, so they are worth pinning even though
 * they are short: a wrong answer here either hands the product to someone who
 * did not buy it, or refuses a customer who did.
 */

const now = new Date().toISOString();

const license = (overrides: Partial<License> = {}): License =>
  licenseSchema.parse({
    id: newId<LicenseId>(),
    userId: newId<UserId>(),
    orderId: newId<OrderId>(),
    serial: 'VCC-ABCDE-FGHJK-LMNPQ-RSTUV',
    createdAt: now,
    updatedAt: now,
    ...overrides,
  });

const activation = (overrides: Partial<DeviceActivation> = {}): DeviceActivation =>
  deviceActivationSchema.parse({
    id: newId<DeviceActivationId>(),
    licenseId: newId<LicenseId>(),
    deviceFingerprint: `device-${Math.random()}`,
    platform: 'windows',
    activatedAt: now,
    ...overrides,
  });

describe('who may download', () => {
  it('lets an active license download the platforms it covers', () => {
    const active = license();
    expect(canDownloadPlatform(active, 'windows')).toBe(true);
    expect(canDownloadPlatform(active, 'macos')).toBe(true);
  });

  it('refuses a platform the license does not cover', () => {
    // Whether one purchase grants both installers is data, so a licence
    // restricted to one platform has to be honoured.
    const windowsOnly = license({ entitledPlatforms: ['windows'] });
    expect(canDownloadPlatform(windowsOnly, 'windows')).toBe(true);
    expect(canDownloadPlatform(windowsOnly, 'macos')).toBe(false);
  });

  it('refuses a revoked, suspended or expired license', () => {
    for (const status of ['revoked', 'suspended', 'expired'] as const) {
      expect(canDownloadPlatform(license({ status }), 'windows')).toBe(false);
    }
  });

  it('refuses a refunded purchase the moment the license is revoked', () => {
    // The webhook revokes on refund; this is the check that makes that bite.
    const revoked = license({ status: 'revoked' });
    expect(canDownloadPlatform(revoked, 'windows')).toBe(false);
    expect(canDownloadPlatform(revoked, 'macos')).toBe(false);
  });
});

describe('device activation slots', () => {
  it('allows activations up to the license limit', () => {
    const twoDevices = license({ maxActivations: 2 });
    expect(isActivationSlotAvailable(twoDevices, [])).toBe(true);
    expect(isActivationSlotAvailable(twoDevices, [activation()])).toBe(true);
    expect(isActivationSlotAvailable(twoDevices, [activation(), activation()])).toBe(false);
  });

  it('frees a slot when a device is deactivated rather than deleted', () => {
    // Lost-device replacement works without a manual database edit, and
    // deactivation remains visible history.
    const twoDevices = license({ maxActivations: 2 });
    const used = [activation(), activation({ deactivatedAt: now })];

    expect(isActivationSlotAvailable(twoDevices, used)).toBe(true);
    expect(used).toHaveLength(2);
  });
});

describe('commerce records', () => {
  it('defaults a new license to both platforms and two devices', () => {
    const fresh = license();
    expect(fresh.entitledPlatforms).toEqual(['windows', 'macos']);
    expect(fresh.maxActivations).toBe(2);
    expect(fresh.status).toBe('active');
  });

  it('carries the checkout session id, which is what makes a replay idempotent', () => {
    const order = orderSchema.parse({
      id: newId<OrderId>(),
      userId: newId<UserId>(),
      amountCents: 9900,
      stripeCheckoutSessionId: 'cs_test_123',
      createdAt: now,
      updatedAt: now,
    });

    expect(order.stripeCheckoutSessionId).toBe('cs_test_123');
    expect(order.status).toBe('pending');
    // No second idempotency key: one identifier cannot disagree with itself.
    expect(order).not.toHaveProperty('idempotencyKey');
  });

  it('starts a release build inactive, so publishing is a deliberate act', () => {
    const build = releaseBuildSchema.parse({
      id: newId(),
      platform: 'windows',
      version: '1.0.0',
      artifactKey: 'windows/1.0.0/VCComposer2-Setup.exe',
      createdAt: now,
      updatedAt: now,
    });

    expect(build.active).toBe(false);
    expect(build.channel).toBe('stable');
  });
});
