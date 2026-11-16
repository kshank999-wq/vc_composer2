import type { SupabaseClient } from '@supabase/supabase-js';
import { platformSchema, type Platform } from '@/domain';
import { adminClient } from './supabase';
import { generateSerial } from './license';

/**
 * Turning a paid Stripe checkout into an entitlement.
 *
 * Stripe delivers webhooks at least once, so this whole path is written to be
 * idempotent: a retry of the same event must end with exactly one order and
 * exactly one license. Two database constraints do the real work —
 * `orders.stripe_checkout_session_id` is unique and `licenses.order_id` is
 * unique — so concurrent retries collide in Postgres rather than racing in
 * application code.
 *
 * Everything here takes its database client as an argument so the money path
 * can be tested against a fake rather than only in production.
 */

export interface FulfillmentInput {
  checkoutSessionId: string;
  paymentIntentId: string | null;
  stripeCustomerId: string | null;
  customerEmail: string;
  amountCents: number;
  currency: string;
  /** Platform chosen at checkout; recorded for support and analytics. */
  selectedPlatform: Platform | null;
  /** Present when the buyer was signed in when they started checkout. */
  userId: string | null;
  /**
   * The subscription this licence renews on, and the period it is paid to.
   * Both null for a licence that does not renew; `licenseLive` reads a null
   * expiry as *never lapses*.
   */
  subscriptionId?: string | null;
  expiresAt?: string | null;
}

export interface FulfillmentResult {
  userId: string;
  orderId: string;
  licenseId: string;
  serial: string;
  /** False when a retry found the license already present. */
  created: boolean;
}

export interface FulfillmentDeps {
  client: SupabaseClient;
  newSerial: () => string;
}

const defaultDeps = (): FulfillmentDeps => ({ client: adminClient(), newSerial: generateSerial });

export const parsePlatform = (value: unknown): Platform | null => {
  const result = platformSchema.safeParse(value);
  return result.success ? result.data : null;
};

const findByEmail = async (client: SupabaseClient, email: string): Promise<string | null> => {
  const { data } = await client.from('profiles').select('id').ilike('email', email).maybeSingle();
  return data?.id ?? null;
};

/**
 * Resolve the buyer to an account.
 *
 * The lookup comes first and the creation second, deliberately. A returning
 * customer is the common case, and resolving them must not depend on an error
 * path — nor on the auth admin API's paginated user list, which silently
 * misses anyone past the first page and would strand a purchase that had
 * already been paid for.
 *
 * A purchase is never stranded because the buyer checked out without signing
 * in: an account is created for that email, and the confirmation email points
 * them at the account page, where an emailed sign-in link gets them in.
 */
const resolveUserId = async (client: SupabaseClient, input: FulfillmentInput): Promise<string> => {
  if (input.userId) return input.userId;

  const email = input.customerEmail.trim().toLowerCase();

  const existing = await findByEmail(client, email);
  if (existing) return existing;

  const { data: created, error } = await client.auth.admin.createUser({
    email,
    email_confirm: true,
    user_metadata: { source: 'stripe_checkout' },
  });
  if (created?.user) return created.user.id;

  // Lost a race with a concurrent delivery that created the same account.
  const raced = await findByEmail(client, email);
  if (raced) return raced;

  throw new Error(`Could not create or find an account for ${email}: ${error?.message ?? 'unknown error'}`);
};

export const fulfillCheckout = async (
  input: FulfillmentInput,
  deps: FulfillmentDeps = defaultDeps(),
): Promise<FulfillmentResult> => {
  const { client, newSerial } = deps;
  const userId = await resolveUserId(client, input);

  // Upsert on the unique checkout session id: a replayed webhook updates the
  // same row instead of inserting a duplicate order.
  const { data: order, error: orderError } = await client
    .from('orders')
    .upsert(
      {
        user_id: userId,
        status: 'paid',
        selected_platform: input.selectedPlatform,
        amount_cents: input.amountCents,
        currency: input.currency,
        stripe_checkout_session_id: input.checkoutSessionId,
        stripe_payment_intent_id: input.paymentIntentId,
        stripe_customer_id: input.stripeCustomerId,
        paid_at: new Date().toISOString(),
      },
      { onConflict: 'stripe_checkout_session_id' },
    )
    .select('id')
    .single();
  if (orderError || !order) {
    throw new Error(`Could not record order for session ${input.checkoutSessionId}: ${orderError?.message}`);
  }

  const existing = await client.from('licenses').select('id, serial').eq('order_id', order.id).maybeSingle();
  if (existing.data) {
    // A replay, not a renewal — but if the first delivery landed before the
    // subscription was known, this is where it catches up. Writing it again is
    // harmless and writing it never is a licence that can only lapse.
    if (input.subscriptionId) {
      await client
        .from('licenses')
        .update({ stripe_subscription_id: input.subscriptionId, expires_at: input.expiresAt ?? null })
        .eq('id', existing.data.id);
    }
    return {
      userId,
      orderId: order.id,
      licenseId: existing.data.id,
      serial: existing.data.serial,
      created: false,
    };
  }

  const { data: license, error: licenseError } = await client
    .from('licenses')
    .insert({
      user_id: userId,
      order_id: order.id,
      serial: newSerial(),
      status: 'active',
      // Data, not a rule: today one purchase covers both installers.
      entitled_platforms: ['windows', 'macos'],
      stripe_subscription_id: input.subscriptionId ?? null,
      expires_at: input.expiresAt ?? null,
    })
    .select('id, serial')
    .single();

  if (licenseError) {
    // A concurrent retry won the unique(order_id) race — read its license.
    const { data: raced } = await client
      .from('licenses')
      .select('id, serial')
      .eq('order_id', order.id)
      .maybeSingle();
    if (raced) {
      return { userId, orderId: order.id, licenseId: raced.id, serial: raced.serial, created: false };
    }
    throw new Error(`Could not issue license for order ${order.id}: ${licenseError.message}`);
  }

  return { userId, orderId: order.id, licenseId: license.id, serial: license.serial, created: true };
};
