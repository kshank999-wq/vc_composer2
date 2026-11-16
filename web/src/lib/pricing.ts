import { formatPrice, PLANS, planWords, type SubscriptionPlan, yearlySaving } from '@/domain';
import { env } from './env';
import { stripe } from './stripe';

/**
 * The prices, read from Stripe rather than repeated in the code.
 *
 * Stripe is where the price actually lives (it is what the customer is
 * charged), so the store reads it from there. **No price appears anywhere in
 * this repository**: changing a plan's price is a minute in the Stripe
 * dashboard and nothing here moves.
 */

export interface DisplayPrice {
  amountCents: number;
  currency: string;
  formatted: string;
  /** True for a subscription. Both desktop plans are. */
  recurring: boolean;
}

export interface PlanPrice extends DisplayPrice {
  plan: SubscriptionPlan;
  label: string;
  per: string;
  /** Whole percent off against twelve months, read rather than typed. */
  savingPercent: number | null;
}

/** Re-exported: the formatter is the domain's, so a browser screen can have it without Stripe. */
export { formatPrice };

export const priceIdFor = (plan: SubscriptionPlan): string =>
  plan === 'monthly' ? env.stripeMonthlyPriceId : env.stripeYearlyPriceId;

const readOne = async (plan: SubscriptionPlan): Promise<DisplayPrice | null> => {
  const price = await stripe().prices.retrieve(priceIdFor(plan));
  if (!price.unit_amount || !price.currency) return null;
  return {
    amountCents: price.unit_amount,
    currency: price.currency,
    formatted: formatPrice(price.unit_amount, price.currency),
    recurring: price.type === 'recurring',
  };
};

/**
 * Both plans, or an empty list.
 *
 * Returns nothing rather than throwing when Stripe is not configured, so the
 * pages still render on a deployment without keys and the price simply
 * appears at checkout instead. **A plan Stripe cannot answer for is left out**
 * rather than drawn with a figure the page made up.
 */
export const fetchPlanPrices = async (): Promise<PlanPrice[]> => {
  let monthly: DisplayPrice | null = null;
  let yearly: DisplayPrice | null = null;
  try {
    [monthly, yearly] = await Promise.all([readOne('monthly'), readOne('yearly')]);
  } catch {
    return [];
  }

  const found: Record<SubscriptionPlan, DisplayPrice | null> = { monthly, yearly };
  const saving =
    monthly && yearly ? yearlySaving(monthly.amountCents, yearly.amountCents, monthly.currency, yearly.currency) : null;

  return PLANS.flatMap((plan) => {
    const price = found[plan];
    if (!price) return [];
    const words = planWords(plan);
    return [{ ...price, plan, label: words.label, per: words.per, savingPercent: plan === 'yearly' ? saving : null }];
  });
};
