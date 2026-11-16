import type Stripe from 'stripe';
import { licenseStatusForSubscription } from '@/domain';
import { adminClient } from './supabase';
import { subscriptionPeriodEnd } from './stripe';

/**
 * Carrying Stripe's word about a desktop subscription onto the licence.
 *
 * **This is the whole of what a renewal does.** A subscription renews every
 * month or every year, and if each renewal made an order and a licence a
 * composer would collect twelve serials a year. So a renewal finds the licence
 * it already has by `stripe_subscription_id` and moves two fields: the status,
 * and the date it is paid up to.
 *
 * Both are Stripe's answer rather than arithmetic done here. A plan changed
 * from monthly to yearly in the Customer Portal needs no case of its own:
 * Stripe sends an update and the new date is simply what it says.
 */
export const recordDesktopSubscription = async (subscription: Stripe.Subscription): Promise<void> => {
  const db = adminClient();

  // A period end Stripe did not give is left as it was, never cleared: an
  // empty expiry on a licence would read as one that never ends.
  const end = subscriptionPeriodEnd(subscription);
  const patch = {
    status: licenseStatusForSubscription(subscription.status),
    ...(end ? { expires_at: end } : {}),
  };

  // No row is not a failure. **The first event can beat the checkout** —
  // Stripe may send `customer.subscription.created` before
  // `checkout.session.completed`, and there is then no licence to move, which
  // the session's own event fixes a moment later by writing the subscription
  // id and the period end itself. Making a licence from a subscription alone
  // would be a second way for an entitlement to be born, and two of those is
  // how somebody ends up with two.
  await db
    .from('licenses')
    .update(patch)
    .eq('stripe_subscription_id', subscription.id);
};
