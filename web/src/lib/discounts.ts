import type Stripe from 'stripe';
import {
  type DiscountOff,
  type DiscountOffer,
  type NewDiscount,
  newDiscountRefusal,
  normaliseCode,
} from '@/domain';
import { OWN_PRODUCT } from './checkout-kind';
import { stripe } from './stripe';

/**
 * Discount codes, asked of Stripe.
 *
 * The till holds them, so this file is a **reader and a maker and keeps
 * nothing**: no table, no cache, no copy of a percentage.
 *
 * **A coupon is the discount and a promotion code is the word you say**,
 * which is Stripe's own split: one coupon (20% off) can carry several codes,
 * each counting its own redemptions, which tells you which advertisement
 * worked. Making one from the admin screen makes both in a single act.
 *
 * **Only VC Composer's own codes count.** Promotion codes are account-wide in
 * Stripe, and the account is shared with other VC Software products, so a
 * code is honoured here only where it (or its coupon) carries
 * `metadata.product = 'vc-composer'`. Codes made from the admin screen are
 * stamped automatically; a code made in the Stripe dashboard needs that
 * metadata added by hand. For the same reason checkout never shows Stripe's
 * own promotion-code box, which would accept any product's code.
 */

/** What Stripe's coupon says it takes off, in the domain's words. */
const offOf = (coupon: Stripe.Coupon): DiscountOff | null => {
  if (coupon.percent_off) return { kind: 'percent', percent: coupon.percent_off };
  if (coupon.amount_off && coupon.currency) {
    return { kind: 'amount', amountCents: coupon.amount_off, currency: coupon.currency };
  }
  return null;
};

/** Whether a code was made for this product, on the code or on its coupon. */
export const isOwnCode = (promotion: {
  metadata?: Record<string, string> | null;
  coupon?: { metadata?: Record<string, string> | null } | string | null;
}): boolean =>
  promotion.metadata?.['product'] === OWN_PRODUCT ||
  (typeof promotion.coupon === 'object' && promotion.coupon?.metadata?.['product'] === OWN_PRODUCT);

const offerOf = (promotion: Stripe.PromotionCode): DiscountOffer | null => {
  const coupon = promotion.coupon;
  if (!coupon || typeof coupon === 'string') return null;
  if (!isOwnCode(promotion)) return null;
  const off = offOf(coupon);
  if (!off) return null;

  return {
    code: promotion.code,
    off,
    promotionCodeId: promotion.id,
    redeemed: promotion.times_redeemed,
    maxRedemptions: promotion.max_redemptions ?? null,
    expiresAt: promotion.expires_at ? new Date(promotion.expires_at * 1000).toISOString() : null,
    // Stripe switches a promotion code off itself when its coupon is deleted
    // or its ceiling is reached, so this is read rather than worked out.
    active: promotion.active && coupon.valid,
  };
};

/**
 * One code, by the word somebody typed or followed a link with.
 *
 * Returns null for every way it can fail, which is `DISCOUNT_REFUSAL`'s
 * argument in the data layer: the caller has one answer to give and so needs
 * one answer back. Stripe not being configured answers null too — a shop with
 * no keys has no discounts, and the page says the same thing it says about a
 * code that was never made.
 */
export const findDiscount = async (said: string): Promise<DiscountOffer | null> => {
  const code = normaliseCode(said);
  if (code.length === 0) return null;

  try {
    const found = await stripe().promotionCodes.list({ code, limit: 1, expand: ['data.coupon'] });
    const promotion = found.data[0];
    return promotion ? offerOf(promotion) : null;
  } catch {
    return null;
  }
};

/**
 * Every code, newest first, for the screen that manages them.
 *
 * Inactive ones are listed too: a campaign that has run is the thing an
 * operator most wants to look at afterwards, and a list that hid it would be
 * a list that answers *how did LAUNCH20 do* with nothing.
 */
export const listDiscounts = async (limit = 100): Promise<DiscountOffer[]> => {
  try {
    const found = await stripe().promotionCodes.list({ limit, expand: ['data.coupon'] });
    return found.data.map(offerOf).filter((one): one is DiscountOffer => one !== null);
  } catch {
    return [];
  }
};

export type MadeDiscount = { ok: true; offer: DiscountOffer } | { ok: false; error: string };

/**
 * Make one: a coupon for what comes off, and the code that names it.
 *
 * The refusal is **asked again here** rather than trusted from the screen
 * (`trackRemoval`'s shape), and the two Stripe records are made in order, the
 * coupon first — a coupon with no code is invisible and harmless, where a code
 * pointing at nothing could not exist at all.
 */
export const makeDiscount = async (plan: NewDiscount, now = new Date()): Promise<MadeDiscount> => {
  const refusal = newDiscountRefusal(plan, now);
  if (refusal) return { ok: false, error: refusal };

  const code = normaliseCode(plan.code);

  try {
    const already = await stripe().promotionCodes.list({ code, limit: 1 });
    if (already.data.length > 0) return { ok: false, error: `${code} is already a code.` };

    const coupon = await stripe().coupons.create({
      name: code,
      // `once`: on a subscription the discount applies to the first invoice
      // only, and renewals are charged at the plan's price. A code meant to
      // discount every renewal is a decision for the Stripe dashboard.
      duration: 'once',
      metadata: { product: OWN_PRODUCT },
      ...(plan.off.kind === 'percent'
        ? { percent_off: plan.off.percent }
        : { amount_off: plan.off.amountCents, currency: plan.off.currency.toLowerCase() }),
    });

    const promotion = await stripe().promotionCodes.create({
      coupon: coupon.id,
      code,
      metadata: { product: OWN_PRODUCT },
      ...(plan.maxRedemptions === null ? {} : { max_redemptions: plan.maxRedemptions }),
      ...(plan.expiresAt === null
        ? {}
        : { expires_at: Math.floor(new Date(plan.expiresAt).getTime() / 1000) }),
    });

    const offer = offerOf({ ...promotion, coupon });
    return offer ? { ok: true, offer } : { ok: false, error: 'Stripe made something this shop cannot read.' };
  } catch (cause) {
    const message = cause instanceof Error ? cause.message : 'The code could not be made.';
    return { ok: false, error: message };
  }
};

/**
 * Stop one working, without deleting what it did. Switched off rather than
 * destroyed: an operator asking *how did LAUNCH20 do* a month later must
 * still get an answer.
 */
export const switchDiscount = async (promotionCodeId: string, active: boolean): Promise<boolean> => {
  try {
    await stripe().promotionCodes.update(promotionCodeId, { active });
    return true;
  } catch {
    return false;
  }
};
