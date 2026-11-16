/**
 * Money, written out.
 *
 * Two formatters because they answer different questions: a price is
 * advertised and a figure is accounted for. Kept free of any server client so
 * a browser component can format a price without pulling Stripe in with it.
 */

/** `$249.95`, `€10.00`: a figure in a ledger, where the cents always show. */
export const formatMoney = (cents: number, currency: string): string =>
  new Intl.NumberFormat('en-US', { style: 'currency', currency: currency.toUpperCase() }).format(cents / 100);

/** `$249`, `$249.95`: a price on a page, without the two zeros nobody is asked for. */
export const formatPrice = (cents: number, currency: string): string =>
  new Intl.NumberFormat('en-US', {
    style: 'currency',
    currency: currency.toUpperCase(),
    minimumFractionDigits: cents % 100 === 0 ? 0 : 2,
  }).format(cents / 100);
