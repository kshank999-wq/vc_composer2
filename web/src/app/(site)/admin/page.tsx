import type { Metadata } from 'next';
import Link from 'next/link';
import { currentAdmin } from '@/lib/admin';
import { loadDashboard } from '@/lib/admin-console';
import { AdminGate } from './_components/gate';
import { StatTiles } from './_components/tiles';
import { EmailsTable, OrdersTable } from './_components/tables';

export const metadata: Metadata = { title: 'Admin' };
export const dynamic = 'force-dynamic';

/**
 * The dashboard: the numbers
 * that matter, and where anything is wrong. A marked tile is the console's
 * only alerting.
 */
export default async function AdminDashboardPage() {
  const admin = await currentAdmin();
  if (!admin) return <AdminGate title="Admin" next="/admin" />;

  const { metrics, recentOrders, recentFailedEmails } = await loadDashboard();

  return (
    <>
      <div className="hero">
        <h1>Dashboard</h1>
        <p>Signed in as {admin.email ?? 'an administrator'}. Money lives in Stripe; everything else is here.</p>
      </div>

      <StatTiles metrics={metrics} />

      <section>
        <h2>Recent orders</h2>
        <p className="lede">
          The last ten. <Link href="/admin/orders">All orders</Link>
        </p>
        <OrdersTable orders={recentOrders} />
      </section>

      <section>
        <h2>Recent failed emails</h2>
        <p className="lede">
          Delivery failures, newest first. <Link href="/admin/emails">All email</Link>
        </p>
        <EmailsTable events={recentFailedEmails} />
      </section>
    </>
  );
}
