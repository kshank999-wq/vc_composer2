import type { Metadata } from 'next';
import { currentAdmin } from '@/lib/admin';
import { loadEmailEvents } from '@/lib/admin-console';
import { AdminGate } from '../_components/gate';
import { EmailsTable } from '../_components/tables';

export const metadata: Metadata = { title: 'Email' };
export const dynamic = 'force-dynamic';

const STATUSES = ['sent', 'queued', 'failed'] as const;

/**
 * Every transactional email. For the pattern across customers
 * — a provider outage, a template that stopped rendering. Resending one
 * customer's licence email is on their record.
 */
export default async function EmailsPage({ searchParams }: { searchParams: { status?: string } }) {
  const admin = await currentAdmin();
  if (!admin) return <AdminGate title="Email" next="/admin/emails" />;

  const status = (STATUSES as readonly string[]).includes(searchParams.status ?? '') ? searchParams.status : undefined;
  const events = await loadEmailEvents({ status });

  return (
    <>
      <div className="hero">
        <h1>Email</h1>
        <p>Everything the system sent, newest first.</p>
      </div>

      <form method="get" className="filters">
        <label htmlFor="status">Status</label>
        <select id="status" name="status" defaultValue={status ?? ''}>
          <option value="">All</option>
          {STATUSES.map((value) => (
            <option key={value} value={value}>
              {value}
            </option>
          ))}
        </select>
        <button type="submit" className="button secondary">
          Filter
        </button>
      </form>

      <EmailsTable events={events} />
    </>
  );
}
