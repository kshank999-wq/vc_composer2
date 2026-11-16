import { NextResponse } from 'next/server';
import { z } from 'zod';
import { DISCOUNT_REFUSAL, isRedeemable, PLANS, platformSchema } from '@/domain';
import { env } from '@/lib/env';
import { findDiscount } from '@/lib/discounts';
import { priceIdFor } from '@/lib/pricing';
import { CHECKOUT_API_VERSION, stripe } from '@/lib/stripe';
import { currentUserOrNull } from '@/lib/supabase';
import { RULES, rateLimit } from '@/lib/rate-limit';
import { OWN_PRODUCT } from '@/lib/checkout-kind';

export const runtime = 'nodejs';
export const dynamic = 'force-dynamic';

const bodySchema = z.object({
  /** Windows or Mac, chosen before payment and recorded on the order. */
  platform: platformSchema,
  email: z.string().email().optional(),
  /**
   * A discount code, typed or carried in from an advertisement's link.
   * **The shape is the permission**: this is the *word*, and there is no field
   * for a percentage, an amount, a coupon or a promotion-code id, so a client
   * that decided its own discount has nowhere to put it. The server asks
   * Stripe what the word is worth.
   */
  code: z.string().max(64).optional(),
  /**
   * Which plan: the *word* `monthly` or `yearly`. There is no field for a
   * price id, an amount or an interval; the server turns the word into a price.
   */
  plan: z.enum(PLANS).default('monthly'),
});

/**
 * Start a Stripe Checkout session for a VC Composer subscription.
 *
 * Price and entitlement are decided server-side; the client only names the
 * plan and platform. Nothing here grants anything: the licence is issued by
 * the webhook once Stripe confirms payment.
 */
export async function POST(request: Request): Promise<Response> {
  const limited = await rateLimit(request, RULES.checkout);
  if (limited) return limited;

  const parsed = bodySchema.safeParse(await request.json().catch(() => null));
  if (!parsed.success) {
    return NextResponse.json({ error: 'A platform of "windows" or "macos" is required' }, { status: 400 });
  }

  const user = await currentUserOrNull();
  const email = user?.email ?? parsed.data.email;

  // Resolved here and never trusted from the client. A code that is not
  // redeemable is refused rather than quietly dropped: somebody who followed
  // an advertisement and is charged full price without being told has been
  // overcharged as far as they are concerned.
  const offer = parsed.data.code ? await findDiscount(parsed.data.code) : null;
  if (parsed.data.code && (!offer || !isRedeemable(offer, new Date()))) {
    return NextResponse.json({ error: DISCOUNT_REFUSAL }, { status: 400 });
  }

  try {
    const session = await stripe().checkout.sessions.create(
      {
        // The licence is issued by the webhook on `checkout.session.completed`;
        // renewals arrive afterwards as `customer.subscription.*` events.
        mode: 'subscription',
        line_items: [{ price: priceIdFor(parsed.data.plan), quantity: 1 }],
        success_url: `${env.siteUrl}/purchase/complete?session_id={CHECKOUT_SESSION_ID}`,
        cancel_url: `${env.siteUrl}/download?cancelled=1`,
        ...(email ? { customer_email: email } : {}),
        // Read back by the webhook. `product` is what makes this sale VC
        // Composer's on a Stripe account shared with other products
        // (lib/checkout-kind.ts); the platform is recorded on the order.
        metadata: {
          product: OWN_PRODUCT,
          platform: parsed.data.platform,
          supabase_user_id: user?.id ?? '',
          plan: parsed.data.plan,
        },
        // **A subscription says what it is for**, so its own events can be told
        // from every other product's, and from the collaboration subscription
        // that will come later.
        subscription_data: {
          metadata: {
            product: OWN_PRODUCT,
            kind: 'desktop',
            supabase_user_id: user?.id ?? '',
            plan: parsed.data.plan,
          },
        },
        // Only a code this site has checked is applied. Stripe's own
        // promotion-code box is deliberately not offered: on an account shared
        // with other products it would accept their codes too.
        ...(offer ? { discounts: [{ promotion_code: offer.promotionCodeId }] } : {}),
        // Stripe Tax works out VAT and sales tax wherever the customer is.
        automatic_tax: { enabled: true },
        // **Required rather than `auto`, because this is a subscription.** A
        // renewal is re-rated from the address saved on the Customer, and one
        // saved without an address leaves every renewal invoice stuck in
        // draft. No `customer_update` here on purpose: this session names an
        // email rather than an existing customer, so Checkout makes the
        // Customer and saves the collected address on it, and Stripe refuses
        // the field without a `customer`.
        billing_address_collection: 'required',
      },
      { apiVersion: CHECKOUT_API_VERSION },
    );

    if (!session.url) {
      return NextResponse.json({ error: 'Stripe did not return a checkout URL' }, { status: 502 });
    }
    return NextResponse.json({ url: session.url });
  } catch (cause) {
    const message = cause instanceof Error ? cause.message : 'Checkout could not be started';
    return NextResponse.json({ error: message }, { status: 500 });
  }
}
