import { NextResponse } from 'next/server';
import { DISCOUNT_REFUSAL, describeOff, formatPrice, isPlan, isRedeemable, priceWith } from '@/domain';
import { findDiscount } from '@/lib/discounts';
import { fetchPlanPrices } from '@/lib/pricing';
import { RULES, rateLimit } from '@/lib/rate-limit';

export const runtime = 'nodejs';
export const dynamic = 'force-dynamic';

/**
 * What a discount code is worth, before anybody presses.
 *
 * **Public on purpose**: a code is printed in advertisements, so a reader who
 * follows the link must be told what it takes off on the page they land on.
 * It is rate-limited for the one thing that is not public about it (guessing
 * codes nobody published), and the refusal is **one sentence for every way it
 * can fail**, which makes guessing worthless.
 *
 * It says what comes off and what is owed, and **grants nothing**: nothing is
 * reserved and the answer is not carried to checkout, which resolves the word
 * again for itself.
 */
export async function GET(request: Request): Promise<Response> {
  const limited = await rateLimit(request, RULES.discount);
  if (limited) return limited;

  const said = new URL(request.url).searchParams.get('code')?.trim() ?? '';
  if (said.length === 0) {
    return NextResponse.json({ error: 'A code is required' }, { status: 400 });
  }

  const offer = await findDiscount(said);
  if (!offer || !isRedeemable(offer, new Date())) {
    return NextResponse.json({ ok: false, error: DISCOUNT_REFUSAL });
  }

  // What is owed depends on which plan, so the page says which one it is
  // asking about. Monthly where nothing is said, the plan the page opens on.
  const wanted = new URL(request.url).searchParams.get('plan') ?? 'monthly';
  const plan = isPlan(wanted) ? wanted : 'monthly';
  const prices = await fetchPlanPrices();
  const price = prices.find((one) => one.plan === plan) ?? null;
  const nowCents = price ? priceWith(price.amountCents, price.currency, offer.off) : null;

  return NextResponse.json({
    ok: true,
    code: offer.code,
    // What it takes off, always; what is then owed only where the shop can say
    // so for certain (a fixed amount in another currency cannot be).
    takes: describeOff(offer.off),
    was: price?.formatted ?? null,
    now: nowCents === null || !price ? null : formatPrice(nowCents, price.currency),
  });
}
