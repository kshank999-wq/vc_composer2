import type { Metadata } from 'next';
import { currentAdmin } from '@/lib/admin';
import { loadOrders, type OrderRow } from '@/lib/admin-console';
import { AdminGate } from '../_components/gate';
import { OrdersTable } from '../_components/tables';

export const metadata: Metadata = { title: 'Orders' };
export const dynamic = 'force-dynamic';

const STATUSES: OrderRow['status'][] = ['paid', 'pending', 'refunded', 'disputed', 'failed'];

const isStatus = (value: string | undefined): value is OrderRow['status'] =>
  value !== undefined && (STATUSES as string[]).includes(value);

/**
 * Every order. Nothing here changes one: a refund is done in
 * Stripe and arrives through the webhook.
 */
export default async function OrdersPage({ searchParams }: { searchParams: { status?: string } }) {
  const admin = await currentAdmin();
  if (!admin) return <AdminGate title="Orders" next="/admin/orders" />;

  const status = isStatus(searchParams.status) ? searchParams.status : undefined;
  const orders = await loadOrders({ status });

  return (
    <>
      <div className="hero">
        <h1>Orders</h1>
        <p>Newest first. Open an order in Stripe for the money side.</p>
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

      <OrdersTable orders={orders} />
    </>
  );
}
