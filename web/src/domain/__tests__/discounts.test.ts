import { describe, expect, it } from 'vitest';
import {
  advertisedLink,
  codeRefusal,
  describeNewDiscount,
  DISCOUNT_PARAM,
  describeOff,
  describeStanding,
  type DiscountOffer,
  DISCOUNT_REFUSAL,
  isRedeemable,
  newDiscountRefusal,
  normaliseCode,
  priceWith,
  purchaseSettled,
} from '../index';

/**
 * Discount codes.
 *
 * Stripe holds the money, so what is testable here is everything that is
 * **not** the money: the one spelling of a code, what the offer says in words,
 * what a reader is charged, the link an advertisement carries, and what is
 * refused before anybody presses.
 */

const offer = (over: Partial<DiscountOffer> = {}): DiscountOffer => ({
  code: 'LAUNCH20',
  off: { kind: 'percent', percent: 20 },
  promotionCodeId: 'promo_1',
  redeemed: 0,
  maxRedemptions: null,
  expiresAt: null,
  active: true,
  ...over,
});

const NOW = new Date('2026-10-03T12:00:00.000Z');

describe('the one spelling of a code', () => {
  it('reads a code off paper however it was typed', () => {
    // Read off an advertisement, said aloud, typed with a thumb: one code.
    expect(normaliseCode('launch20')).toBe('LAUNCH20');
    expect(normaliseCode('  Launch 20 ')).toBe('LAUNCH20');
    expect(normaliseCode('LAUNCH-20')).toBe('LAUNCH-20');
  });

  it('says what is wrong with it while somebody is still typing', () => {
    expect(codeRefusal('')).toBe('Type the code.');
    expect(codeRefusal('ab')).toBe('A code is at least three characters.');
    expect(codeRefusal('A'.repeat(41))).toBe('A code is at most forty characters.');
    expect(codeRefusal('LAUNCH 20%')).toBe('Letters, numbers and dashes only.');
    expect(codeRefusal('LAUNCH20')).toBe(null);
  });
});

describe('what the offer says', () => {
  it('says what comes off, in the format the reader sees prices in', () => {
    expect(describeOff({ kind: 'percent', percent: 20 })).toBe('20% off');
    expect(describeOff({ kind: 'amount', amountCents: 1000, currency: 'usd' })).toBe('$10 off');
    expect(describeOff({ kind: 'amount', amountCents: 1050, currency: 'usd' })).toBe('$10.50 off');
  });

  /**
   * The figure on the page has to be the figure at the till, so this follows
   * Stripe's own arithmetic rather than inventing rounding — and **answers
   * null rather than guessing** where it cannot be certain.
   */
  it('works out what is owed the way the till does', () => {
    expect(priceWith(24995, 'usd', { kind: 'percent', percent: 20 })).toBe(19996);
    expect(priceWith(24995, 'usd', { kind: 'amount', amountCents: 5000, currency: 'usd' })).toBe(19995);
    // Never below nothing.
    expect(priceWith(1000, 'usd', { kind: 'amount', amountCents: 5000, currency: 'usd' })).toBe(0);
    expect(priceWith(1000, 'usd', { kind: 'percent', percent: 100 })).toBe(0);
    // A fixed amount in another currency is not converted by a shop front.
    expect(priceWith(24995, 'usd', { kind: 'amount', amountCents: 5000, currency: 'eur' })).toBe(null);
  });
});

describe('the link an advertisement carries', () => {
  it('lands on the buying page with the code already said', () => {
    expect(advertisedLink('https://vc-composer.com', 'launch20')).toBe(
      'https://vc-composer.com/download?discount=LAUNCH20',
    );
    // A trailing slash on the site URL is the one thing that would otherwise
    // put two in the middle of an advertised link.
    expect(advertisedLink('https://vc-composer.com/', 'LAUNCH20')).toBe(
      'https://vc-composer.com/download?discount=LAUNCH20',
    );
  });

  /**
   * The finding this parameter exists for: `?code=` is the site's sign-in
   * parameter, so an advertisement built on it took every reader to *your link
   * has expired*. Named here rather than only in a comment, because the next
   * person to shorten the parameter needs the test to stop them.
   */
  it('is not swallowed by the sign-in redirect', () => {
    expect(DISCOUNT_PARAM).not.toBe('code');
    expect(advertisedLink('https://vc-composer.com', 'LAUNCH20')).not.toContain('?code=');
  });
});

describe('what a code is still worth', () => {
  it('is redeemable until it is not, and says which', () => {
    expect(isRedeemable(offer(), NOW)).toBe(true);
    expect(isRedeemable(offer({ active: false }), NOW)).toBe(false);
    expect(isRedeemable(offer({ maxRedemptions: 50, redeemed: 50 }), NOW)).toBe(false);
    expect(isRedeemable(offer({ expiresAt: '2026-01-01T00:00:00.000Z' }), NOW)).toBe(false);
    expect(isRedeemable(offer({ expiresAt: '2027-01-01T00:00:00.000Z' }), NOW)).toBe(true);
  });

  it('describes the standing for whoever is deciding whether to advertise it', () => {
    expect(describeStanding(offer({ redeemed: 7 }), NOW)).toBe('7 used');
    expect(describeStanding(offer({ redeemed: 7, maxRedemptions: 50 }), NOW)).toBe('7 of 50 used');
    expect(describeStanding(offer({ maxRedemptions: 50, redeemed: 50 }), NOW)).toContain('used up');
    expect(describeStanding(offer({ expiresAt: '2026-01-01T00:00:00.000Z' }), NOW)).toContain('expired');
    expect(describeStanding(offer({ active: false }), NOW)).toContain('switched off');
  });

  /**
   * **One refusal for every way a code can fail.** The sentence that told them
   * apart would be the one telling a stranger which codes exist, and a buyer
   * does the same thing in all five cases.
   */
  it('refuses in one sentence', () => {
    expect(DISCOUNT_REFUSAL).toBe('That code is not valid.');
  });
});

describe('making one', () => {
  const plan = {
    code: 'LAUNCH20',
    off: { kind: 'percent', percent: 20 } as const,
    maxRedemptions: null,
    expiresAt: null,
  };

  it('says what a press would do, before it can be asked for', () => {
    expect(describeNewDiscount(plan)).toBe('LAUNCH20 takes 20% off.');
    expect(describeNewDiscount({ ...plan, maxRedemptions: 50 })).toBe(
      'LAUNCH20 takes 20% off, for the first 50.',
    );
    expect(
      describeNewDiscount({
        ...plan,
        off: { kind: 'amount', amountCents: 5000, currency: 'usd' },
        expiresAt: '2026-12-31',
      }),
    ).toBe('LAUNCH20 takes $50 off, until 2026-12-31.');
  });

  it('refuses a plan that cannot be made', () => {
    // Not `a b` — spaces are stripped, so that is a two-character code and
    // the length refusal is the true one. An underscore survives normalising
    // and is the thing the shape actually rejects.
    expect(newDiscountRefusal({ ...plan, code: 'a b' }, NOW)).toBe('A code is at least three characters.');
    expect(newDiscountRefusal({ ...plan, code: 'LAUNCH_20' }, NOW)).toBe('Letters, numbers and dashes only.');
    expect(newDiscountRefusal({ ...plan, off: { kind: 'percent', percent: 0 } }, NOW)).toBe(
      'Take something off.',
    );
    expect(newDiscountRefusal({ ...plan, off: { kind: 'percent', percent: 120 } }, NOW)).toBe(
      'Nothing over 100% off.',
    );
    expect(newDiscountRefusal({ ...plan, maxRedemptions: 0 }, NOW)).toBe(
      'A limit is one or more, or none at all.',
    );
    // An end date in the past makes a code that never worked.
    expect(newDiscountRefusal({ ...plan, expiresAt: '2020-01-01T00:00:00.000Z' }, NOW)).toBe(
      'An end date is in the future.',
    );
    expect(newDiscountRefusal(plan, NOW)).toBe(null);
  });
});

/**
 * The free purchase.
 *
 * The one case a discount can produce that the rest of the shop never sees:
 * Stripe answers `no_payment_required` where the total is zero, and a webhook
 * that asked only for `paid` would fulfil nothing, silently.
 */
describe('a purchase that cost nothing', () => {
  it('is settled, and so is one that was paid for', () => {
    expect(purchaseSettled('paid')).toBe(true);
    expect(purchaseSettled('no_payment_required')).toBe(true);
  });

  it('does not settle what Stripe says is unpaid', () => {
    expect(purchaseSettled('unpaid')).toBe(false);
  });

  it('does not settle a word this build has never heard of', () => {
    // An answer we do not know is not a reason to hand anything over.
    expect(purchaseSettled('pending')).toBe(false);
    expect(purchaseSettled('')).toBe(false);
    expect(purchaseSettled('PAID')).toBe(false);
  });

  it('says what a code taking everything off will do', () => {
    const everything = {
      code: 'REVIEW',
      off: { kind: 'percent' as const, percent: 100 },
      maxRedemptions: 1,
      expiresAt: null,
    };
    expect(describeNewDiscount(everything)).toBe(
      'REVIEW takes 100% off, for the first 1. Nothing is charged and no card is asked for, and the licence is still issued.',
    );
    // Said only where it is certain: this module does not hold the price, so a
    // fixed amount may or may not clear it and says nothing either way.
    expect(
      describeNewDiscount({ ...everything, off: { kind: 'amount', amountCents: 99900, currency: 'usd' } }),
    ).toBe('REVIEW takes $999 off, for the first 1.');
    expect(describeNewDiscount({ ...everything, off: { kind: 'percent', percent: 99 } })).toBe(
      'REVIEW takes 99% off, for the first 1.',
    );
  });

  it('is a plan the admin screen may make', () => {
    // 100% is the ceiling rather than over it, which is what makes a review
    // copy possible at all.
    expect(
      newDiscountRefusal(
        { code: 'REVIEW', off: { kind: 'percent', percent: 100 }, maxRedemptions: 1, expiresAt: null },
        NOW,
      ),
    ).toBe(null);
    expect(priceWith(24900, 'usd', { kind: 'percent', percent: 100 })).toBe(0);
  });
});
