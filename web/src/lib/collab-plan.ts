import type Stripe from 'stripe';
import { subscriptionPeriodEnd } from '@/lib/stripe';
import { adminClient } from '@/lib/supabase';

/**
 * The owner's collaboration plan, as a Stripe subscription says it is: how
 * many editor seats (the quantity), and whether it is paid up.
 */
export interface CollabPlan {
  ownerId: string;
  editorSeats: number;
  status: 'active' | 'past_due' | 'canceled';
  subscriptionId: string;
  currentPeriodEnd: string | null;
}

export const collabPlanFrom = (subscription: Stripe.Subscription): CollabPlan | null => {
  const ownerId = subscription.metadata?.['supabase_user_id'];
  if (!ownerId) return null;
  const seats = subscription.items?.data?.reduce((n, item) => n + (item.quantity ?? 0), 0) ?? 0;
  const s = subscription.status;
  const status = s === 'active' || s === 'trialing' ? 'active' : s === 'past_due' || s === 'unpaid' ? 'past_due' : 'canceled';
  return { ownerId, editorSeats: seats, status, subscriptionId: subscription.id, currentPeriodEnd: subscriptionPeriodEnd(subscription) };
};

/** Records (creates or updates) the owner's plan from their subscription. */
export const recordCollabPlan = async (subscription: Stripe.Subscription): Promise<void> => {
  const plan = collabPlanFrom(subscription);
  if (!plan) throw new Error(`Collaboration subscription ${subscription.id} names no account`);
  const { error } = await adminClient()
    .from('collab_plans')
    .upsert(
      {
        owner_id: plan.ownerId,
        editor_seats: plan.editorSeats,
        status: plan.status,
        stripe_subscription_id: plan.subscriptionId,
        current_period_end: plan.currentPeriodEnd,
        updated_at: new Date().toISOString(),
      },
      { onConflict: 'owner_id' },
    );
  if (error) throw new Error(`collab_plans: ${error.message}`);
};
