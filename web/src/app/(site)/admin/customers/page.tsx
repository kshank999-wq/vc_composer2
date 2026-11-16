import type { Metadata } from 'next';
import { currentAdmin } from '@/lib/admin';
import { loadCustomers } from '@/lib/admin-console';
import { AdminGate } from '../_components/gate';
import { CustomersTable } from '../_components/tables';

export const metadata: Metadata = { title: 'Customers' };
export const dynamic = 'force-dynamic';

/** Every account, searchable. A row opens the customer's record. */
export default async function CustomersPage({ searchParams }: { searchParams: { q?: string } }) {
  const admin = await currentAdmin();
  if (!admin) return <AdminGate title="Customers" next="/admin/customers" />;

  const query = (searchParams.q ?? '').trim();
  const customers = await loadCustomers({ query });

  return (
    <>
      <div className="hero">
        <h1>Customers</h1>
        <p>Every account, newest first. Open one to see purchases, licence, devices and email.</p>
      </div>

      <form method="get" className="filters" role="search">
        <input type="search" name="q" defaultValue={query} placeholder="Email or name" aria-label="Search customers" />
        <button type="submit" className="button secondary">
          Search
        </button>
      </form>

      <CustomersTable customers={customers} />
    </>
  );
}
