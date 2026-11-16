import { NextResponse } from 'next/server';
import type Stripe from 'stripe';
import type { SupabaseClient } from '@supabase/supabase-js';
import { purchaseSettled } from '@/domain';
import { env } from '@/lib/env';
import { stripe, subscriptionPeriodEnd } from '@/lib/stripe';
import { adminClient } from '@/lib/supabase';
import { fulfillCheckout, parsePlatform } from '@/lib/fulfillment';
import { sendPurchaseEmail } from '@/lib/email';
import { recordDesktopSubscription } from '@/lib/desktop-subscription';
import { checkoutKind, subscriptionKind } from '@/lib/checkout-kind';

export const runtime = 'nodejs';
export const dynamic = 'force-dynamic';

/**
 * When the subscription is paid up to, asked of Stripe rather than guessed.
 *
 * The checkout session says which subscription was made and not when it ends.
 * A failure answers null: a licence with no expiry never lapses
 * (`licenseLive`), which is the safe way to be wrong. Somebody who has just
 * paid keeps working, and the first subscription event writes the real date.
 */
const periodEnd = async (subscriptionId: string): Promise<string | null> => {
  try {
    const subscription = await stripe().subscriptions.retrieve(subscriptionId);
    return subscriptionPeriodEnd(subscription);
  } catch {
    return null;
  }
};

/**
 * The order a refunded or disputed charge paid for, if it is one of ours.
 *
 * A subscription checkout has no payment intent of its own (the first charge
 * belongs to the subscription's first invoice), so the payment intent alone
 * rarely matches. The charge's invoice names the subscription, and the
 * subscription names the licence and through it the order. Either way the
 * lookup can only ever find VC Composer's own rows, so another product's
 * refund on the shared Stripe account changes nothing here.
 */
const orderForCharge = async (client: SupabaseClient, charge: Stripe.Charge): Promise<string | null> => {
  const paymentIntentId = typeof charge.payment_intent === 'string' ? charge.payment_intent : charge.payment_intent?.id;
  if (paymentIntentId) {
    const { data } = await client.from('orders').select('id').eq('stripe_payment_intent_id', paymentIntentId).maybeSingle();
    if (data?.id) return data.id as string;
  }

  const invoiceId = typeof charge.invoice === 'string' ? charge.invoice : charge.invoice?.id;
  if (!invoiceId) return null;
  let subscriptionId: string | null = null;
  try {
    const invoice = await stripe().invoices.retrieve(invoiceId);
    subscriptionId = typeof invoice.subscription === 'string' ? invoice.subscription : (invoice.subscription?.id ?? null);
  } catch {
    return null;
  }
  if (!subscriptionId) return null;
  const { data: license } = await client
    .from('licenses')
    .select('order_id')
    .eq('stripe_subscription_id', subscriptionId)
    .maybeSingle();
  return (license?.order_id as string | undefined) ?? null;
};

/**
 * Stripe webhook: the only place a paid entitlement is created.
 *
 * The Stripe account is shared with other VC Software products, so every
 * handler below first asks whether the event is VC Composer's
 * (`lib/checkout-kind.ts`) and does nothing for anyone else's.
 *
 * Order of operations matters:
 *  1. verify the signature, so only Stripe can trigger fulfilment;
 *  2. claim the event id, so a redelivery of an event already handled exits
 *     immediately (one purchase, one licence, however many retries);
 *  3. fulfil, which is itself idempotent through unique constraints;
 *  4. email, whose failure is logged but never fails the webhook: a 500 here
 *     would make Stripe retry a purchase that has already been fulfilled.
 */
export async function POST(request: Request): Promise<Response> {
  const signature = request.headers.get('stripe-signature');
  if (!signature) {
    return NextResponse.json({ error: 'Missing stripe-signature header' }, { status: 400 });
  }

  const payload = await request.text();
  let event: Stripe.Event;
  try {
    event = stripe().webhooks.constructEvent(payload, signature, env.stripeWebhookSecret);
  } catch (cause) {
    const message = cause instanceof Error ? cause.message : 'Signature verification failed';
    // Said in the logs: a wrong STRIPE_WEBHOOK_SECRET (e.g. another endpoint's,
    // or live vs sandbox) otherwise fails silently and nothing is fulfilled.
    console.warn(`stripe webhook refused: ${message}`);
    return NextResponse.json({ error: message }, { status: 400 });
  }

  const client = adminClient();

  // Claim the event. If the insert loses to an earlier delivery this is a
  // redelivery: skip it when that delivery finished, and let it through when
  // the earlier attempt failed, which is exactly what Stripe's retry is for.
  const { error: claimError } = await client.from('stripe_webhook_events').insert({ id: event.id, type: event.type });
  if (claimError) {
    const { data: seen } = await client
      .from('stripe_webhook_events')
      .select('processed_at')
      .eq('id', event.id)
      .maybeSingle();
    if (seen?.processed_at) {
      return NextResponse.json({ received: true, duplicate: true });
    }
  }

  try {
    if (event.type === 'checkout.session.completed') {
      const session = event.data.object as Stripe.Checkout.Session;
      // **Only VC Composer's own checkouts make a licence.** Another product's
      // sale on the shared account is that product's webhook's to fulfil.
      //
      // And **not `=== 'paid'`**: Stripe answers `no_payment_required` where a
      // 100%-off code took the total to zero, and that purchase is owed its
      // licence too. `purchaseSettled` is the one reading of Stripe's words.
      if (checkoutKind(session) === 'desktop' && purchaseSettled(session.payment_status)) {
        const email = session.customer_details?.email ?? session.customer_email;
        if (!email) throw new Error(`Checkout session ${session.id} has no customer email`);

        // The subscription the licence will renew on. Read here rather than
        // waited for, because the two events race and this one always knows
        // who bought it.
        const subscriptionId =
          typeof session.subscription === 'string' ? session.subscription : (session.subscription?.id ?? null);
        const expiresAt = subscriptionId ? await periodEnd(subscriptionId) : null;

        const result = await fulfillCheckout({
          checkoutSessionId: session.id,
          subscriptionId,
          expiresAt,
          paymentIntentId: typeof session.payment_intent === 'string' ? session.payment_intent : null,
          stripeCustomerId: typeof session.customer === 'string' ? session.customer : null,
          customerEmail: email,
          amountCents: session.amount_total ?? 0,
          currency: session.currency ?? 'usd',
          selectedPlatform: parsePlatform(session.metadata?.['platform']),
          userId: session.metadata?.['supabase_user_id'] || null,
        });

        // Only the first successful fulfilment sends mail; retries stay quiet.
        if (result.created) {
          await sendPurchaseEmail({
            to: email,
            userId: result.userId,
            serial: result.serial,
            platform: parsePlatform(session.metadata?.['platform']),
          });
        }
      }
    } else if (
      event.type === 'customer.subscription.created' ||
      event.type === 'customer.subscription.updated' ||
      event.type === 'customer.subscription.deleted'
    ) {
      // One handler for all three, because `status` says everything the
      // licence needs: `deleted` arrives as `canceled` and needs no special
      // case. Routed by the metadata the checkout stamped; anything else (a
      // VC Writer room, a subscription made by hand) is left alone.
      const subscription = event.data.object as Stripe.Subscription;
      if (subscriptionKind(subscription) === 'desktop') await recordDesktopSubscription(subscription);
    } else if (event.type === 'charge.refunded' || event.type === 'charge.dispute.created') {
      // Entitlement follows the money: a refund or a dispute revokes the licence.
      const charge =
        event.type === 'charge.refunded'
          ? (event.data.object as Stripe.Charge)
          : await (async () => {
              const dispute = event.data.object as Stripe.Dispute;
              return typeof dispute.charge === 'string' ? await stripe().charges.retrieve(dispute.charge) : dispute.charge;
            })();
      const orderId = await orderForCharge(client, charge);
      if (orderId) {
        const status = event.type === 'charge.refunded' ? 'refunded' : 'disputed';
        await client.from('orders').update({ status }).eq('id', orderId);
        await client.from('licenses').update({ status: 'revoked' }).eq('order_id', orderId);
      }
    }

    await client
      .from('stripe_webhook_events')
      .update({ processed_at: new Date().toISOString(), error: null })
      .eq('id', event.id);

    return NextResponse.json({ received: true });
  } catch (cause) {
    const message = cause instanceof Error ? cause.message : 'Fulfilment failed';
    // Record why and leave `processed_at` null, so Stripe's retry re-runs the
    // (idempotent) fulfilment rather than being turned away as a duplicate.
    await client.from('stripe_webhook_events').update({ error: message }).eq('id', event.id);
    return NextResponse.json({ error: message }, { status: 500 });
  }
}
