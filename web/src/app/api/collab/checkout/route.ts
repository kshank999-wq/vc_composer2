import { NextResponse } from 'next/server';
import { z } from 'zod';
import { PLANS } from '@/domain';
import { OWN_PRODUCT } from '@/lib/checkout-kind';
import { env } from '@/lib/env';
import { RULES, rateLimit } from '@/lib/rate-limit';
import { CHECKOUT_API_VERSION, stripe } from '@/lib/stripe';
import { currentUserOrNull } from '@/lib/supabase';

export const runtime = 'nodejs';
export const dynamic = 'force-dynamic';

const bodySchema = z.object({
  /** Editor seats: people who write music in the owner's shared projects. */
  seats: z.number().int().min(1).max(100),
  plan: z.enum(PLANS).default('monthly'),
});

/**
 * Start a checkout for a collaboration plan: the owner pays per editor seat
 * (reviewers and view-only people are free). Signed in, because the plan
 * belongs to the account that owns the projects. The seats are recorded by
 * the webhook from the subscription's quantity; changing them later is done
 * in the billing portal.
 */
export async function POST(request: Request): Promise<Response> {
  const limited = await rateLimit(request, RULES.checkout);
  if (limited) return limited;
  const user = await currentUserOrNull();
  if (!user) return NextResponse.json({ error: 'Sign in to start a collaboration plan' }, { status: 401 });
  const parsed = bodySchema.safeParse(await request.json().catch(() => null));
  if (!parsed.success) return NextResponse.json({ error: 'Choose between 1 and 100 editor seats' }, { status: 400 });

  const price = parsed.data.plan === 'yearly' ? env.stripeCollabSeatYearlyPriceId : env.stripeCollabSeatMonthlyPriceId;
  if (!price) return NextResponse.json({ error: 'Collaboration plans are not on sale yet' }, { status: 503 });

  const metadata = { product: OWN_PRODUCT, kind: 'collab', supabase_user_id: user.id, plan: parsed.data.plan };
  try {
    const session = await stripe().checkout.sessions.create(
      {
        mode: 'subscription',
        line_items: [
          { price, quantity: parsed.data.seats, adjustable_quantity: { enabled: true, minimum: 1, maximum: 100 } },
        ],
        success_url: `${env.siteUrl}/account/collaboration?started=1`,
        cancel_url: `${env.siteUrl}/collaboration?cancelled=1`,
        ...(user.email ? { customer_email: user.email } : {}),
        metadata,
        // The subscription says what it is for, so its events reach the
        // collaboration plan and never the desktop licence.
        subscription_data: { metadata },
        automatic_tax: { enabled: env.stripeAutomaticTax },
        billing_address_collection: 'required',
      },
      { apiVersion: CHECKOUT_API_VERSION },
    );
    if (!session.url) return NextResponse.json({ error: 'Stripe did not return a checkout URL' }, { status: 502 });
    return NextResponse.json({ url: session.url });
  } catch (cause) {
    return NextResponse.json({ error: cause instanceof Error ? cause.message : 'Checkout could not be started' }, { status: 500 });
  }
}
