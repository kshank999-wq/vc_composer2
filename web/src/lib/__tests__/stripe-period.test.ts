import { describe, expect, it, vi } from 'vitest';

vi.mock('../env', () => ({ env: {} }));

import { subscriptionPeriodEnd } from '../stripe';

/** Where a subscription's period end lives depends on the Stripe API version. */
describe('subscriptionPeriodEnd', () => {
  const at = (seconds: number) => new Date(seconds * 1000).toISOString();

  it('reads the subscription’s own field (older API versions)', () => {
    expect(subscriptionPeriodEnd({ current_period_end: 1794355200, items: { data: [] } } as never)).toBe(at(1794355200));
  });

  it('reads the items’ field when the subscription has none (basil and later)', () => {
    expect(
      subscriptionPeriodEnd({ items: { data: [{ current_period_end: 1794355200 }, { current_period_end: 1796947200 }] } } as never),
    ).toBe(at(1796947200));
  });

  it('gives null rather than an invalid date when there is none at all', () => {
    expect(subscriptionPeriodEnd({ items: { data: [] } } as never)).toBeNull();
  });
});
