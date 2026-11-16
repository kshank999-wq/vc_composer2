import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { describe, expect, it } from 'vitest';
import { z } from 'zod';
import { advertisedLink, PLANS, platformSchema } from '@/domain';
import { strayAuthRedirect } from '@/lib/auth-redirect';
import { isOwnCode } from '@/lib/discounts';

/**
 * What a client may say about a discount.
 *
 * **The shape is the permission**: a browser may name a *code* and a *plan*,
 * and may not name what either is worth. So this is the checkout body's own
 * schema, handed everything a client might try to decide for itself, with the
 * assertion that all of it falls off.
 *
 * It is a copy of the route's schema rather than an import because a route
 * module pulls in Stripe and the Next request machinery, so the first test
 * **reads the route's own source** and holds the copy to it.
 */
const bodySchema = z.object({
  platform: platformSchema,
  email: z.string().email().optional(),
  code: z.string().max(64).optional(),
  plan: z.enum(PLANS).default('monthly'),
});

const read = (relative: string): string => readFileSync(fileURLToPath(new URL(relative, import.meta.url)), 'utf8');

const ROUTE = read('../../app/api/checkout/route.ts');
const WEBHOOK = read('../../app/api/stripe/webhook/route.ts');

/** Source lines that are code, not commentary about the code. */
const code = (source: string): string[] =>
  source.split('\n').filter((line) => !line.trim().startsWith('//') && !line.trim().startsWith('*'));

describe('what a client may say about a discount', () => {
  it('is the schema the route actually declares', () => {
    const body = ROUTE.slice(ROUTE.indexOf('const bodySchema'), ROUTE.indexOf('});', ROUTE.indexOf('const bodySchema')));
    expect(body).toContain('platform: platformSchema');
    expect(body).toContain('email:');
    expect(body).toContain('code: z.string().max(64).optional()');
    expect(body).toContain("plan: z.enum(PLANS).default('monthly')");
    for (const money of ['percent', 'amount', 'discount', 'coupon', 'price', 'total', 'interval']) {
      expect(code(body).join('\n').toLowerCase()).not.toContain(`${money}:`);
    }
  });

  it('takes the word and nothing about the money', () => {
    const parsed = bodySchema.parse({
      platform: 'windows',
      code: 'LAUNCH20',
      plan: 'yearly',
      percentOff: 100,
      amountOff: 24995,
      discounts: [{ coupon: 'free_forever' }],
      promotionCodeId: 'promo_mine',
      priceId: 'price_cheap',
      total: 0,
    } as never);

    expect(parsed).toEqual({ platform: 'windows', code: 'LAUNCH20', plan: 'yearly' });
  });

  it('refuses a code long enough to be an attack rather than a word, and a plan that does not exist', () => {
    expect(bodySchema.safeParse({ platform: 'macos', code: 'A'.repeat(65) }).success).toBe(false);
    expect(bodySchema.safeParse({ platform: 'macos', plan: 'lifetime' }).success).toBe(false);
  });

  it('is still a checkout without one, on the monthly plan', () => {
    expect(bodySchema.parse({ platform: 'macos' })).toEqual({ platform: 'macos', plan: 'monthly' });
  });
});

describe('a shared Stripe account', () => {
  it('stamps every checkout and its subscription as VC Composer’s', () => {
    expect(ROUTE).toContain('product: OWN_PRODUCT');
    expect(ROUTE.match(/product: OWN_PRODUCT/g)?.length).toBe(2);
    expect(ROUTE).toContain("kind: 'desktop'");
  });

  it('never offers Stripe’s own promotion-code box, which would take another product’s codes', () => {
    expect(code(ROUTE).filter((line) => line.includes('allow_promotion_codes'))).toEqual([]);
  });

  it('honours only codes stamped for this product', () => {
    expect(isOwnCode({ metadata: { product: 'vc-composer' }, coupon: { metadata: {} } })).toBe(true);
    expect(isOwnCode({ metadata: {}, coupon: { metadata: { product: 'vc-composer' } } })).toBe(true);
    expect(isOwnCode({ metadata: { product: 'vc-writer' }, coupon: { metadata: { product: 'vc-writer' } } })).toBe(false);
    expect(isOwnCode({ metadata: {}, coupon: { metadata: {} } })).toBe(false);
    expect(isOwnCode({ metadata: null, coupon: 'coupon_id' })).toBe(false);
  });

  it('routes every webhook branch through the product reading', () => {
    expect(WEBHOOK).toContain("checkoutKind(session) === 'desktop'");
    expect(WEBHOOK).toContain("subscriptionKind(subscription) === 'desktop'");
  });
});

/**
 * The advertised link, against this site's own sign-in redirect: a link built
 * on `?code=` would be forwarded to the auth callback and the discount lost.
 */
describe('an advertised link survives the site it points at', () => {
  it('is not mistaken for a sign-in code', () => {
    const url = new URL(advertisedLink('https://vc-composer.com', 'LAUNCH20'));
    expect(strayAuthRedirect(url)).toBe(null);
  });

  it('and `?code=` really would have been', () => {
    const wrong = new URL('https://vc-composer.com/download?code=LAUNCH20');
    expect(strayAuthRedirect(wrong)?.pathname).toBe('/auth/callback');
  });
});

/**
 * The webhook's gate on a free purchase, read off the source because the route
 * cannot run without a Stripe signature: a zero-total session answers
 * `no_payment_required`, so any literal comparison here is the bug.
 */
describe('the webhook gates fulfilment on the domain reading', () => {
  it('asks purchaseSettled', () => {
    expect(WEBHOOK).toContain("import { purchaseSettled } from '@/domain'");
    expect(WEBHOOK).toContain('purchaseSettled(session.payment_status)');
  });

  it('does not compare the status to a literal', () => {
    expect(WEBHOOK).not.toMatch(/payment_status\s*===/);
  });

  it('handles the six events the deployment guide subscribes', () => {
    for (const type of [
      'checkout.session.completed',
      'customer.subscription.created',
      'customer.subscription.updated',
      'customer.subscription.deleted',
      'charge.refunded',
      'charge.dispute.created',
    ]) {
      expect(WEBHOOK).toContain(`'${type}'`);
    }
  });
});

/**
 * What a subscription needs before Stripe can tax it. Read off the source for
 * the webhook gate's reason: an address Checkout decided not to collect leaves
 * every renewal invoice stuck in draft, silently.
 */
describe('what Stripe Tax is given to work from', () => {
  it('collects an address on the subscription', () => {
    expect(ROUTE).toContain('automatic_tax: { enabled: true }');
    expect(ROUTE).toContain("billing_address_collection: 'required'");
  });

  it('never sends customer_update, since the session names no customer', () => {
    expect(code(ROUTE).filter((line) => line.includes('customer_update'))).toEqual([]);
  });
});
