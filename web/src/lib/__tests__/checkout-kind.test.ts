import { describe, expect, it } from 'vitest';
import { checkoutKind, OWN_PRODUCT, subscriptionKind } from '../checkout-kind';

/**
 * The Stripe account is shared with VC Writer and every other VC Software
 * product, and Stripe sends every product's events to every webhook. VC
 * Composer fulfils only what it stamped itself.
 */
describe('checkout kind', () => {
  it('is the product name the checkout route stamps', () => {
    expect(OWN_PRODUCT).toBe('vc-composer');
  });

  it("is a desktop purchase for VC Composer's own checkouts", () => {
    expect(checkoutKind({ metadata: { product: 'vc-composer', platform: 'windows', supabase_user_id: 'u1', plan: 'monthly' } })).toBe(
      'desktop',
    );
  });

  it("ignores every other product's checkout", () => {
    for (const product of ['vc-writer', 'vc-game-studio', 'vc-dit', 'VC-COMPOSER', 'vc-composer ', 'something-new']) {
      expect(checkoutKind({ metadata: { product, platform: 'macos' } })).toBe('other-product');
    }
  });

  it("ignores an unstamped checkout, which is VC Writer's oldest kind", () => {
    expect(checkoutKind({ metadata: { platform: 'windows', supabase_user_id: 'u1', plan: 'monthly' } })).toBe('other-product');
    expect(checkoutKind({ metadata: {} })).toBe('other-product');
    expect(checkoutKind({ metadata: null })).toBe('other-product');
    expect(checkoutKind({})).toBe('other-product');
  });

  it('ignores a Writers Room seat checkout', () => {
    expect(checkoutKind({ metadata: { room_id: 'r1' } })).toBe('other-product');
    expect(checkoutKind({ metadata: { product: 'vc-writer', room_id: 'r1' } })).toBe('other-product');
  });
});

describe('subscription kind', () => {
  it('is a desktop subscription where VC Composer stamped it', () => {
    expect(subscriptionKind({ metadata: { product: 'vc-composer', kind: 'desktop', plan: 'yearly' } })).toBe('desktop');
    expect(subscriptionKind({ metadata: { product: 'vc-composer' } })).toBe('desktop');
  });

  it("leaves another product's subscription, or a hand-made one, alone", () => {
    expect(subscriptionKind({ metadata: { product: 'vc-writer', kind: 'desktop' } })).toBe('other-product');
    expect(subscriptionKind({ metadata: { room_id: 'r1' } })).toBe('other-product');
    expect(subscriptionKind({ metadata: {} })).toBe('other-product');
    expect(subscriptionKind({ metadata: null })).toBe('other-product');
  });

  it('does not mistake a future VC Composer subscription of another kind for the desktop licence', () => {
    expect(subscriptionKind({ metadata: { product: 'vc-composer', kind: 'collaboration' } })).toBe('other-product');
  });
});

describe('the collaboration plan', () => {
  it('is its own kind, never a desktop licence', () => {
    expect(checkoutKind({ metadata: { product: OWN_PRODUCT, kind: 'collab', supabase_user_id: 'u1' } })).toBe('collab');
    expect(subscriptionKind({ metadata: { product: OWN_PRODUCT, kind: 'collab' } })).toBe('collab');
  });
  it('leaves another product stamped collab alone', () => {
    expect(checkoutKind({ metadata: { product: 'vc-writer', kind: 'collab' } })).toBe('other-product');
  });
});
