import { describe, expect, it } from 'vitest';
import {
  customerSummaries,
  dashboardFromRows,
  formatMoney,
  formatRevenue,
  revenueInWindow,
  stripeUrlFor,
  type ActivationRow,
  type BuildRow,
  type EmailEventRow,
  type LicenseRow,
  type OrderRow,
  type ProfileRow,
} from '../admin-console';

/**
 * Every number on the dashboard has a definition. These pin each one to rows,
 * so a tile that drifts from its definition fails here rather than being
 * noticed by whoever is reading it.
 */

const NOW = new Date('2026-09-07T12:00:00Z');
const daysAgo = (days: number, hours = 0): string =>
  new Date(NOW.getTime() - days * 86_400_000 - hours * 3_600_000).toISOString();

const order = (over: Partial<OrderRow>): OrderRow => ({
  id: over.id ?? crypto.randomUUID(),
  user_id: 'u1',
  status: 'paid',
  amount_cents: 24_995,
  currency: 'usd',
  selected_platform: 'windows',
  stripe_checkout_session_id: null,
  stripe_payment_intent_id: null,
  paid_at: daysAgo(1),
  created_at: daysAgo(1),
  updated_at: daysAgo(1),
  ...over,
});

describe('revenueInWindow', () => {
  it('sums paid orders inside the window, per currency, and nothing else', () => {
    const orders = [
      order({ paid_at: daysAgo(2) }),
      order({ paid_at: daysAgo(6), currency: 'EUR', amount_cents: 1_000 }),
      order({ paid_at: daysAgo(8) }), // outside 7 days
      order({ paid_at: daysAgo(1), status: 'refunded' }), // not paid
      order({ paid_at: null, status: 'paid' }), // paid with no timestamp: excluded, not a crash
    ];
    expect(revenueInWindow(orders, NOW, 7)).toEqual({ usd: 24_995, eur: 1_000 });
    expect(revenueInWindow(orders, NOW, 30)).toEqual({ usd: 49_990, eur: 1_000 });
  });

  it('is empty when nothing sold', () => {
    expect(revenueInWindow([], NOW, 30)).toEqual({});
  });
});

describe('dashboardFromRows', () => {
  const licenses: LicenseRow[] = [
    { id: 'l1', user_id: 'u1', status: 'active', created_at: daysAgo(10) },
    { id: 'l2', user_id: 'u2', status: 'revoked', created_at: daysAgo(10) },
    { id: 'l3', user_id: 'u3', status: 'active', created_at: daysAgo(10) },
  ];
  const activations: ActivationRow[] = [
    { id: 'a1', license_id: 'l1', platform: 'windows', deactivated_at: null },
    { id: 'a2', license_id: 'l1', platform: 'macos', deactivated_at: null },
    { id: 'a3', license_id: 'l3', platform: 'windows', deactivated_at: daysAgo(1) }, // freed
  ];
  const emails: EmailEventRow[] = [
    { id: 'e1', user_id: 'u1', template: 'purchase_confirmation', status: 'failed', error: 'x', provider_message_id: null, created_at: daysAgo(1) },
    { id: 'e2', user_id: 'u1', template: 'purchase_confirmation', status: 'sent', error: null, provider_message_id: 'm', created_at: daysAgo(1) },
    { id: 'e3', user_id: 'u2', template: 'license_reminder', status: 'failed', error: 'x', provider_message_id: null, created_at: daysAgo(9) }, // too old
  ];
  const builds: BuildRow[] = [
    { platform: 'windows', version: '1.2.0', channel: 'stable', active: true, published_at: daysAgo(3) },
    { platform: 'macos', version: '1.3.0-beta', channel: 'beta', active: true, published_at: daysAgo(1) }, // not stable
    { platform: 'windows', version: '0.9.0', channel: 'preview', active: true, published_at: daysAgo(2) }, // the free preview
    { platform: 'macos', version: '1.1.0', channel: 'stable', active: false, published_at: daysAgo(30) }, // retired
  ];

  it('computes every tile from its definition', () => {
    const metrics = dashboardFromRows(
      {
        orders: [
          order({ paid_at: daysAgo(1) }),
          order({ paid_at: daysAgo(20) }),
          order({ status: 'refunded', updated_at: daysAgo(2) }),
          order({ status: 'disputed', updated_at: daysAgo(40) }), // outside 30 days
          order({ status: 'pending', paid_at: null }),
        ],
        licenses,
        activations,
        emails,
        builds,
      },
      NOW,
    );

    expect(metrics.revenue30).toEqual({ usd: 49_990 });
    expect(metrics.revenue7).toEqual({ usd: 24_995 });
    expect(metrics.paidOrders30).toBe(2);
    expect(metrics.refundsAndDisputes30).toBe(1);
    expect(metrics.activeLicenses).toBe(2);
    expect(metrics.activeDevices).toEqual({ windows: 1, macos: 1 });
    expect(metrics.failedEmails7).toBe(1);
    expect(metrics.currentBuilds).toEqual({ windows: '1.2.0', macos: null });
    expect(metrics.previewBuilds).toEqual({ windows: '0.9.0', macos: null });
  });

  it('is all zeros and dashes on an empty database, not NaN', () => {
    const metrics = dashboardFromRows({ orders: [], licenses: [], activations: [], emails: [], builds: [] }, NOW);
    expect(metrics.paidOrders30).toBe(0);
    expect(metrics.activeDevices).toEqual({ windows: 0, macos: 0 });
    expect(metrics.currentBuilds).toEqual({ windows: null, macos: null });
    expect(metrics.previewBuilds).toEqual({ windows: null, macos: null });
    expect(formatRevenue(metrics.revenue30)).toBe('—');
  });
});

describe('customerSummaries', () => {
  const profiles: ProfileRow[] = [
    { id: 'u1', email: 'a@example.com', display_name: 'A', is_admin: true, created_at: daysAgo(10) },
    { id: 'u2', email: 'b@example.com', display_name: '', is_admin: false, created_at: daysAgo(5) },
    { id: 'u3', email: null, display_name: 'C', is_admin: false, created_at: daysAgo(1) },
  ];

  it('counts orders and live devices per customer and picks the licence that matters', () => {
    const rows = customerSummaries(
      profiles,
      [order({ user_id: 'u1' }), order({ user_id: 'u1', status: 'refunded' }), order({ user_id: 'u2' })],
      [
        { id: 'l1', user_id: 'u1', status: 'revoked', created_at: daysAgo(9) },
        { id: 'l2', user_id: 'u1', status: 'active', created_at: daysAgo(8) }, // active wins over revoked
        { id: 'l3', user_id: 'u2', status: 'suspended', created_at: daysAgo(4) },
      ],
      [
        { id: 'a1', license_id: 'l2', platform: 'windows', deactivated_at: null },
        { id: 'a2', license_id: 'l1', platform: 'macos', deactivated_at: null }, // on the revoked licence, still a live seat
        { id: 'a3', license_id: 'l3', platform: 'macos', deactivated_at: daysAgo(1) }, // freed
      ],
    );

    expect(rows.map((row) => [row.email, row.orders, row.licenseStatus, row.activeDevices, row.isAdmin])).toEqual([
      ['a@example.com', 2, 'active', 2, true],
      ['b@example.com', 1, 'suspended', 0, false],
      [null, 0, null, 0, false],
    ]);
  });

  it('keeps the profiles in the order given', () => {
    const rows = customerSummaries(profiles, [], [], []);
    expect(rows.map((row) => row.id)).toEqual(['u1', 'u2', 'u3']);
  });
});

describe('money', () => {
  it('formats minor units in the currency', () => {
    expect(formatMoney(24_995, 'usd')).toBe('$249.95');
    expect(formatMoney(1_000, 'EUR')).toBe('€10.00');
  });

  it('joins currencies on the revenue tile', () => {
    expect(formatRevenue({ usd: 49_990, eur: 1_000 })).toBe('$499.90 · €10.00');
  });
});

describe('stripeUrlFor', () => {
  it('opens the payment when there is one', () => {
    expect(stripeUrlFor({ stripe_payment_intent_id: 'pi_live1', stripe_checkout_session_id: 'cs_live1' })).toBe(
      'https://dashboard.stripe.com/payments/pi_live1',
    );
  });

  it('falls back to a search for the session, and knows test mode', () => {
    expect(stripeUrlFor({ stripe_payment_intent_id: null, stripe_checkout_session_id: 'cs_test_abc' })).toBe(
      'https://dashboard.stripe.com/test/search?query=cs_test_abc',
    );
  });

  it('is null when Stripe never saw the order', () => {
    expect(stripeUrlFor({ stripe_payment_intent_id: null, stripe_checkout_session_id: null })).toBeNull();
  });
});
