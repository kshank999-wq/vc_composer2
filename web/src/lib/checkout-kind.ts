/**
 * Whose Stripe event this is, read from the metadata it was stamped with.
 *
 * Every VC Software product sells through one Stripe account, and Stripe sends
 * every product's events to every product's webhook. So this webhook acts
 * **only** on what VC Composer stamped itself: `metadata.product ===
 * 'vc-composer'` on the checkout session and on the subscription it creates.
 *
 * Unlike VC Writer's reading, an unstamped event is **not** ours: VC Writer's
 * oldest checkouts carry no stamp at all, and every VC Composer checkout has
 * carried one from the first sale. Anything else (another product, no stamp,
 * a subscription made by hand in the dashboard) is left alone.
 *
 * Collaboration is a separate subscription with its own `kind: 'collab'`, so
 * the desktop licence is never confused with it.
 */
export const OWN_PRODUCT = 'vc-composer';

export type CheckoutKind = 'desktop' | 'collab' | 'other-product';

type Stamped = { metadata?: Record<string, string> | null };

export const checkoutKind = (session: Stamped): CheckoutKind => {
  if (session.metadata?.['product'] !== OWN_PRODUCT) return 'other-product';
  return session.metadata?.['kind'] === 'collab' ? 'collab' : 'desktop';
};

/**
 * The same reading for `customer.subscription.*` events. A desktop
 * subscription carries the product stamp and `kind: 'desktop'`; an older
 * subscription stamped with the product alone still reads as desktop, since
 * that is the only kind VC Composer sells today.
 */
export type SubscriptionKind = 'desktop' | 'collab' | 'other-product';

export const subscriptionKind = (subscription: Stamped): SubscriptionKind => {
  if (subscription.metadata?.['product'] !== OWN_PRODUCT) return 'other-product';
  const kind = subscription.metadata?.['kind'];
  if (kind === 'collab') return 'collab';
  return kind === undefined || kind === '' || kind === 'desktop' ? 'desktop' : 'other-product';
};
