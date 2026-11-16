import { NextResponse } from 'next/server';
import { env } from '@/lib/env';
import { stripe } from '@/lib/stripe';
import { adminClient, currentUser } from '@/lib/supabase';

export const runtime = 'nodejs';
export const dynamic = 'force-dynamic';

/**
 * **The subscription's billing page.**
 *
 * Changing the card, moving between monthly and yearly, cancelling and the
 * receipts are Stripe's Customer Portal. This route only finds whose customer
 * the signed-in account is and asks Stripe for a portal session.
 *
 * The customer is read from the account's own paid orders, which fulfilment
 * writes; nothing is taken from the request, so a customer can only ever open
 * their own billing.
 */
export async function POST(): Promise<Response> {
  const user = await currentUser();
  if (!user) return NextResponse.json({ error: 'Sign in first.' }, { status: 401 });

  const { data: order, error } = await adminClient()
    .from('orders')
    .select('stripe_customer_id')
    .eq('user_id', user.id)
    .eq('status', 'paid')
    .not('stripe_customer_id', 'is', null)
    .order('created_at', { ascending: false })
    .limit(1)
    .maybeSingle();

  if (error) return NextResponse.json({ error: 'Your orders could not be read.' }, { status: 500 });
  const customer = order?.stripe_customer_id as string | null | undefined;
  if (!customer) {
    return NextResponse.json({ error: 'There is no subscription on this account to manage.' }, { status: 404 });
  }

  try {
    const session = await stripe().billingPortal.sessions.create({
      customer,
      return_url: `${env.siteUrl}/account`,
    });
    return NextResponse.json({ url: session.url });
  } catch (cause) {
    const message = cause instanceof Error ? cause.message : 'Billing could not be opened.';
    return NextResponse.json({ error: message }, { status: 502 });
  }
}
