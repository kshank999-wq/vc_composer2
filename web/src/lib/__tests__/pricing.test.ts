import { describe, expect, it } from 'vitest';
import { formatPrice } from '../pricing';

describe('price formatting', () => {
  it('drops the trailing zeros on a whole amount', () => {
    expect(formatPrice(9900, 'usd')).toBe('$99');
    expect(formatPrice(12000, 'usd')).toBe('$120');
  });

  it('keeps the cents when there are any', () => {
    expect(formatPrice(9950, 'usd')).toBe('$99.50');
  });

  it('respects the currency Stripe reports', () => {
    expect(formatPrice(8900, 'gbp')).toContain('89');
    expect(formatPrice(8900, 'eur')).toContain('89');
  });
});
