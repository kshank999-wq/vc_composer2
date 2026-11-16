import { describe, expect, it } from 'vitest';
import type Stripe from 'stripe';
import { collabPlanFrom } from '../collab-plan';

const sub = (over: Partial<Stripe.Subscription>): Stripe.Subscription =>
  ({
    id: 'sub_1',
    status: 'active',
    metadata: { product: 'vc-composer', kind: 'collab', supabase_user_id: 'owner-1' },
    items: { data: [{ quantity: 4, current_period_end: 1800000000 }] },
    ...over,
  }) as unknown as Stripe.Subscription;

describe('the collaboration plan from a subscription', () => {
  it('reads the owner, the editor seats (the quantity) and the period end', () => {
    const plan = collabPlanFrom(sub({}));
    expect(plan).toMatchObject({ ownerId: 'owner-1', editorSeats: 4, status: 'active', subscriptionId: 'sub_1' });
    expect(plan?.currentPeriodEnd).toBe(new Date(1800000000 * 1000).toISOString());
  });
  it('says past due and canceled as Stripe does', () => {
    expect(collabPlanFrom(sub({ status: 'past_due' }))?.status).toBe('past_due');
    expect(collabPlanFrom(sub({ status: 'unpaid' }))?.status).toBe('past_due');
    expect(collabPlanFrom(sub({ status: 'canceled' }))?.status).toBe('canceled');
  });
  it('is nothing without an account to attach it to', () => {
    expect(collabPlanFrom(sub({ metadata: { product: 'vc-composer', kind: 'collab' } }))).toBeNull();
  });
});
