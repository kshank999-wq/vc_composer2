import { describe, expect, it } from 'vitest';
import { fulfillCheckout, parsePlatform, type FulfillmentInput } from '../fulfillment';
import { createFakeSupabase } from './fake-supabase';

/**
 * The money path.
 *
 * Stripe delivers at least once, so the acceptance criterion is blunt: a
 * successful purchase creates exactly one valid license however many times the
 * webhook arrives. These run against a fake that enforces the same unique
 * constraints Postgres does, so a passing test here is testing the real rule.
 */

const purchase = (overrides: Partial<FulfillmentInput> = {}): FulfillmentInput => ({
  checkoutSessionId: 'cs_test_123',
  paymentIntentId: 'pi_test_123',
  stripeCustomerId: 'cus_test_123',
  customerEmail: 'Buyer@Example.com',
  amountCents: 9900,
  currency: 'usd',
  selectedPlatform: 'windows',
  userId: null,
  ...overrides,
});

let serialCounter = 0;
const deps = (fake: ReturnType<typeof createFakeSupabase>) => ({
  client: fake.client,
  newSerial: () => {
    serialCounter += 1;
    return `VCC-TEST${serialCounter}-AAAAA-BBBBB-CCCCC`;
  },
});

describe('fulfilling a purchase', () => {
  it('creates an account, an order and a license for a first-time buyer', async () => {
    const fake = createFakeSupabase();

    const result = await fulfillCheckout(purchase(), deps(fake));

    expect(result.created).toBe(true);
    expect(fake.state.orders).toHaveLength(1);
    expect(fake.state.licenses).toHaveLength(1);
    expect(fake.state.licenses[0]?.['entitled_platforms']).toEqual(['windows', 'macos']);
    expect(fake.state.orders[0]?.['status']).toBe('paid');
    expect(fake.state.orders[0]?.['selected_platform']).toBe('windows');
  });

  it('creates exactly one license however many times the webhook arrives', async () => {
    const fake = createFakeSupabase();

    const first = await fulfillCheckout(purchase(), deps(fake));
    const second = await fulfillCheckout(purchase(), deps(fake));
    const third = await fulfillCheckout(purchase(), deps(fake));

    expect(fake.state.orders).toHaveLength(1);
    expect(fake.state.licenses).toHaveLength(1);
    expect(second.created).toBe(false);
    expect(third.created).toBe(false);
    expect(second.serial).toBe(first.serial);
    expect(second.licenseId).toBe(first.licenseId);
  });

  it('survives two deliveries racing on the same order', async () => {
    const fake = createFakeSupabase();

    const [a, b] = await Promise.all([
      fulfillCheckout(purchase(), deps(fake)),
      fulfillCheckout(purchase(), deps(fake)),
    ]);

    expect(fake.state.licenses).toHaveLength(1);
    expect(a.licenseId).toBe(b.licenseId);
    // Exactly one of the two won the insert.
    expect([a.created, b.created].filter(Boolean)).toHaveLength(1);
  });

  it('finds a returning customer by email instead of creating a second account', async () => {
    const fake = createFakeSupabase({
      profiles: [{ id: '11111111-1111-4111-8111-111111111111', email: 'buyer@example.com' }],
      authUsers: [{ id: '11111111-1111-4111-8111-111111111111', email: 'buyer@example.com' }],
    });

    const result = await fulfillCheckout(purchase(), deps(fake));

    expect(result.userId).toBe('11111111-1111-4111-8111-111111111111');
    expect(fake.state.authUsers).toHaveLength(1);
    expect(fake.state.profiles).toHaveLength(1);
  });

  it('matches a returning customer whatever case they typed their email in', async () => {
    const fake = createFakeSupabase({
      profiles: [{ id: '22222222-2222-4222-8222-222222222222', email: 'buyer@example.com' }],
      authUsers: [{ id: '22222222-2222-4222-8222-222222222222', email: 'buyer@example.com' }],
    });

    const result = await fulfillCheckout(purchase({ customerEmail: 'BUYER@EXAMPLE.COM' }), deps(fake));

    expect(result.userId).toBe('22222222-2222-4222-8222-222222222222');
    expect(fake.state.profiles).toHaveLength(1);
  });

  it('uses the signed-in account when the buyer was signed in at checkout', async () => {
    const fake = createFakeSupabase();

    const result = await fulfillCheckout(purchase({ userId: 'signed-in-user' }), deps(fake));

    expect(result.userId).toBe('signed-in-user');
    // No account creation attempted at all.
    expect(fake.state.authUsers).toHaveLength(0);
  });

  it('records the platform chosen at checkout without restricting the license to it', async () => {
    const fake = createFakeSupabase();

    await fulfillCheckout(purchase({ selectedPlatform: 'macos' }), deps(fake));

    expect(fake.state.orders[0]?.['selected_platform']).toBe('macos');
    // One purchase covering both installers is data, not a hard rule.
    expect(fake.state.licenses[0]?.['entitled_platforms']).toEqual(['windows', 'macos']);
  });

  it('keeps separate purchases separate', async () => {
    const fake = createFakeSupabase();

    await fulfillCheckout(purchase({ checkoutSessionId: 'cs_a' }), deps(fake));
    await fulfillCheckout(purchase({ checkoutSessionId: 'cs_b', customerEmail: 'other@example.com' }), deps(fake));

    expect(fake.state.orders).toHaveLength(2);
    expect(fake.state.licenses).toHaveLength(2);
    expect(fake.state.licenses[0]?.['serial']).not.toBe(fake.state.licenses[1]?.['serial']);
  });
});

describe('platform metadata', () => {
  it('accepts the two platforms and rejects anything else', () => {
    expect(parsePlatform('windows')).toBe('windows');
    expect(parsePlatform('macos')).toBe('macos');
    expect(parsePlatform('linux')).toBeNull();
    expect(parsePlatform(undefined)).toBeNull();
    expect(parsePlatform('')).toBeNull();
  });
});

/**
 * The free purchase.
 *
 * A 100%-off code makes a session with no payment intent and a total of
 * nothing. The webhook's gate was the bug; this is the other half — that what
 * it hands over goes through unchanged, so a review copy gets its licence.
 */
describe('fulfilling a purchase that cost nothing', () => {
  it('records the order and issues the licence with no payment intent', async () => {
    const fake = createFakeSupabase();

    const result = await fulfillCheckout(
      purchase({ checkoutSessionId: 'cs_test_free', paymentIntentId: null, amountCents: 0 }),
      deps(fake),
    );

    expect(result.created).toBe(true);
    expect(fake.state.orders).toHaveLength(1);
    expect(fake.state.orders[0]?.['amount_cents']).toBe(0);
    expect(fake.state.orders[0]?.['stripe_payment_intent_id']).toBe(null);
    // The receipt is for nothing and the entitlement is the same entitlement.
    expect(fake.state.orders[0]?.['status']).toBe('paid');
    expect(fake.state.licenses).toHaveLength(1);
    expect(fake.state.licenses[0]?.['status']).toBe('active');
    expect(fake.state.licenses[0]?.['entitled_platforms']).toEqual(['windows', 'macos']);
  });

  it('is still only ever fulfilled once', async () => {
    const fake = createFakeSupabase();
    const free = purchase({ checkoutSessionId: 'cs_test_free2', paymentIntentId: null, amountCents: 0 });

    const first = await fulfillCheckout(free, deps(fake));
    const again = await fulfillCheckout(free, deps(fake));

    expect(fake.state.licenses).toHaveLength(1);
    expect(again.created).toBe(false);
    expect(again.serial).toBe(first.serial);
  });
});

/**
 * The subscription.
 *
 * The licence is born exactly as it always was — what is new is that it now
 * carries the subscription that renews it and the date it is paid to, so a
 * renewal has something to find and `licenseLive` has something to read.
 */
describe('fulfilling a subscription', () => {
  it('records the subscription and the period on the licence', async () => {
    const fake = createFakeSupabase();

    await fulfillCheckout(
      purchase({
        checkoutSessionId: 'cs_test_sub',
        subscriptionId: 'sub_test_1',
        expiresAt: '2026-07-01T00:00:00.000Z',
        amountCents: 1999,
      }),
      deps(fake),
    );

    expect(fake.state.licenses).toHaveLength(1);
    expect(fake.state.licenses[0]?.['stripe_subscription_id']).toBe('sub_test_1');
    expect(fake.state.licenses[0]?.['expires_at']).toBe('2026-07-01T00:00:00.000Z');
    expect(fake.state.licenses[0]?.['status']).toBe('active');
  });

  it('leaves a licence that does not renew with no expiry at all', async () => {
    // Which `licenseLive` reads as *never lapses* — the shape of every row
    // written before the subscription, and the safe way to be wrong.
    const fake = createFakeSupabase();

    await fulfillCheckout(purchase({ checkoutSessionId: 'cs_test_plain' }), deps(fake));

    expect(fake.state.licenses[0]?.['stripe_subscription_id']).toBe(null);
    expect(fake.state.licenses[0]?.['expires_at']).toBe(null);
  });

  it('still issues exactly one licence however many times the webhook arrives', async () => {
    const fake = createFakeSupabase();
    const sub = purchase({
      checkoutSessionId: 'cs_test_sub2',
      subscriptionId: 'sub_test_2',
      expiresAt: '2026-07-01T00:00:00.000Z',
    });

    const first = await fulfillCheckout(sub, deps(fake));
    const again = await fulfillCheckout(sub, deps(fake));

    expect(fake.state.licenses).toHaveLength(1);
    expect(again.created).toBe(false);
    expect(again.serial).toBe(first.serial);
  });
});
