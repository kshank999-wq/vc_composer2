import type { Metadata } from 'next';
import { currentAdmin } from '@/lib/admin';
import { AdminGate } from '../_components/gate';
import { SupportConsole } from './support-console';

export const metadata: Metadata = { title: 'Customer record' };
export const dynamic = 'force-dynamic';

/**
 * One customer: what they bought, which
 * machines hold their seats, whether their emails arrived — and the actions
 * on all three, so nobody opens the database to answer a ticket.
 *
 * Reached from the customers list with the email in the query, or directly
 * with the search box.
 */
export default async function SupportPage({ searchParams }: { searchParams: { email?: string } }) {
  const admin = await currentAdmin();
  if (!admin) return <AdminGate title="Customer record" next="/admin/support" />;

  return (
    <>
      <div className="hero">
        <h1>Customer record</h1>
        <p>Look up a customer by the email address on their account.</p>
      </div>
      <SupportConsole initialEmail={searchParams.email ?? ''} />
    </>
  );
}
