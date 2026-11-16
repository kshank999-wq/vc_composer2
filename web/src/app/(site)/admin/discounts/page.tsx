import type { Metadata } from 'next';
import Link from 'next/link';
import { currentAdmin } from '@/lib/admin';
import { listDiscounts } from '@/lib/discounts';
import { env } from '@/lib/env';
import { DiscountManager } from './discount-manager';

export const metadata: Metadata = { title: 'Discounts' };
export const dynamic = 'force-dynamic';

/**
 * Discount codes.
 *
 * The list is read from Stripe on every render rather than from a table here,
 * so a code made or withdrawn in the Stripe dashboard shows up without
 * anything being synchronised — there is one set of codes and this is a window
 * onto it.
 */
export default async function DiscountsPage() {
  const admin = await currentAdmin();

  if (!admin) {
    return (
      <>
        <div className="hero">
          <h1>Discounts</h1>
          <p>This area is for administrators.</p>
        </div>
        <Link href="/signin?next=/admin/discounts" className="button">
          Sign in
        </Link>
      </>
    );
  }

  const offers = await listDiscounts();

  return (
    <>
      <div className="hero">
        <h1>Discounts</h1>
        <p>
          Signed in as {admin.email}. Codes live in Stripe, which is what charges the customer — so what is
          listed here is what the till will honour, and withdrawing one stops it working at once.
        </p>
      </div>
      <DiscountManager initialOffers={offers} siteUrl={env.siteUrl} />
    </>
  );
}
