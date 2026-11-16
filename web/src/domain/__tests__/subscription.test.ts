import { describe, expect, it } from 'vitest';
import {
  decideActivation,
  isPlan,
  licenseLive,
  licenseStatusForSubscription,
  PLANS,
  planWords,
  subscriptionEntitles,
  yearlySaving,
} from '../index';
import type { DeviceActivation, License } from '../commerce';

/**
 * The desktop subscription.
 *
 * What is worth pinning is not that two plans exist but that a lapse reaches
 * what it reaches **through the licence**, rather than through a second kind
 * of entitlement.
 */

const NOW = new Date('2026-06-01T00:00:00.000Z');

const licence = (over: Partial<License> = {}): License =>
  ({
    id: 'lic_1',
    userId: 'user_1',
    orderId: 'ord_1',
    serial: 'VCC-AAAAA-BBBBB-CCCCC-DDDDD',
    status: 'active',
    entitledPlatforms: ['windows', 'macos'],
    maxActivations: 2,
    expiresAt: null,
    createdAt: NOW.toISOString(),
    updatedAt: NOW.toISOString(),
    ...over,
  }) as License;

describe('the two plans', () => {
  it('are a word a client may say', () => {
    expect([...PLANS]).toEqual(['monthly', 'yearly']);
    expect(isPlan('monthly')).toBe(true);
    expect(isPlan('yearly')).toBe(true);
    expect(isPlan('lifetime')).toBe(false);
    expect(isPlan('')).toBe(false);
  });

  it('are named where a composer chooses one', () => {
    expect(planWords('monthly')).toEqual({ label: 'Monthly', per: 'per month' });
    expect(planWords('yearly')).toEqual({ label: 'Yearly', per: 'per year' });
  });
});

describe('what the yearly plan saves', () => {
  it('is read off the two prices rather than typed', () => {
    // Example figures only: the real prices live in Stripe.
    expect(yearlySaving(1999, 19999, 'usd', 'usd')).toBe(17);
  });

  it('says nothing where there is nothing to say', () => {
    expect(yearlySaving(1999, 23988, 'usd', 'usd')).toBe(null);
    expect(yearlySaving(1999, 30000, 'usd', 'usd')).toBe(null);
    expect(yearlySaving(0, 19999, 'usd', 'usd')).toBe(null);
  });

  it('refuses to subtract two currencies', () => {
    // A page that invented an exchange rate would quote a figure nobody charges.
    expect(yearlySaving(1999, 19999, 'usd', 'gbp')).toBe(null);
  });
});

describe("Stripe's subscription words", () => {
  it('entitle while the subscription is live', () => {
    expect(subscriptionEntitles('active')).toBe(true);
    expect(subscriptionEntitles('trialing')).toBe(true);
  });

  it('entitle through a failed retry, deliberately', () => {
    // An expired card is not somebody leaving, and Stripe is still trying.
    expect(subscriptionEntitles('past_due')).toBe(true);
    expect(licenseStatusForSubscription('past_due')).toBe('active');
  });

  it('do not entitle once it is over', () => {
    for (const over of ['canceled', 'unpaid', 'paused', 'incomplete', 'incomplete_expired']) {
      expect(subscriptionEntitles(over)).toBe(false);
      expect(licenseStatusForSubscription(over)).toBe('expired');
    }
  });

  it('do not entitle on a word this build has never heard of', () => {
    // An answer we do not know is not a reason to hand anything over.
    expect(subscriptionEntitles('refunded_pending_review')).toBe(false);
    expect(subscriptionEntitles('')).toBe(false);
    expect(licenseStatusForSubscription('something_new')).toBe('expired');
  });
});

describe('whether a licence is live', () => {
  it('is live while it is paid up', () => {
    expect(licenseLive(licence({ expiresAt: '2026-07-01T00:00:00.000Z' }), NOW)).toBe(true);
  });

  it('is not live once the paid period has run out', () => {
    expect(licenseLive(licence({ expiresAt: '2026-05-01T00:00:00.000Z' }), NOW)).toBe(false);
  });

  it('reads the date even where the status was never updated', () => {
    // The half that matters: a webhook is a message that may not arrive, and a
    // date already in hand beats one that never came.
    const stale = licence({ status: 'active', expiresAt: '2026-01-01T00:00:00.000Z' });
    expect(stale.status).toBe('active');
    expect(licenseLive(stale, NOW)).toBe(false);
  });

  it('never lapses where there is no expiry', () => {
    // A given licence with no end date is this one.
    expect(licenseLive(licence({ expiresAt: null }), NOW)).toBe(true);
  });

  it('is not live where the status says otherwise, whatever the date', () => {
    expect(licenseLive(licence({ status: 'revoked', expiresAt: '2026-07-01T00:00:00.000Z' }), NOW)).toBe(false);
  });
});

describe('what a lapse reaches', () => {
  const activated: DeviceActivation = {
    id: 'act_1',
    licenseId: 'lic_1',
    deviceFingerprint: 'laptop',
    deviceName: 'Laptop',
    platform: 'macos',
    appVersion: '1.0.0',
    activatedAt: NOW.toISOString(),
    lastSeenAt: null,
    deactivatedAt: null,
  } as DeviceActivation;

  it('refuses a new machine', () => {
    const outcome = decideActivation({
      license: licence({ expiresAt: '2026-05-01T00:00:00.000Z' }),
      activations: [],
      deviceFingerprint: 'new-desk',
      platform: 'windows',
      now: NOW,
    });
    expect(outcome).toEqual({ result: 'refused', reason: 'license_inactive' });
  });

  it('lets a new machine in while it is paid up', () => {
    const outcome = decideActivation({
      license: licence({ expiresAt: '2026-07-01T00:00:00.000Z' }),
      activations: [],
      deviceFingerprint: 'new-desk',
      platform: 'windows',
      now: NOW,
    });
    expect(outcome).toEqual({ result: 'activated', reason: 'new_device' });
  });

  it('refuses a lapsed licence even to a machine that already held a seat', () => {
    // This decides activation only; editing is the desk standing's question.
    const outcome = decideActivation({
      license: licence({ expiresAt: '2026-05-01T00:00:00.000Z' }),
      activations: [activated],
      deviceFingerprint: 'laptop',
      platform: 'macos',
      now: NOW,
    });
    expect(outcome).toEqual({ result: 'refused', reason: 'license_inactive' });
  });

  it('leaves a licence with no expiry exactly as it was', () => {
    const outcome = decideActivation({
      license: licence({ expiresAt: null }),
      activations: [],
      deviceFingerprint: 'new-desk',
      platform: 'windows',
      now: NOW,
    });
    expect(outcome).toEqual({ result: 'activated', reason: 'new_device' });
  });
});
