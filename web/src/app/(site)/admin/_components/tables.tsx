import Link from 'next/link';
import {
  formatMoney,
  stripeUrlFor,
  type CustomerSummary,
  type EmailEventWithEmail,
  type OrderWithEmail,
} from '@/lib/admin-console';
import { customerHref, emailTone, formatDate, licenseTone, orderTone, type PillTone } from './format';

/** Plain tables over plain rows. Every customer email links to their record. */

function Pill({ tone, children }: { tone: PillTone; children: React.ReactNode }) {
  return <span className={tone === 'neutral' ? 'pill' : `pill ${tone}`}>{children}</span>;
}

function Customer({ email }: { email: string | null }) {
  return email ? <Link href={customerHref(email)}>{email}</Link> : <span>—</span>;
}

function Empty({ children, columns }: { children: React.ReactNode; columns: number }) {
  return (
    <tr>
      <td className="empty" colSpan={columns}>
        {children}
      </td>
    </tr>
  );
}

export function OrdersTable({ orders }: { orders: readonly OrderWithEmail[] }) {
  return (
    <div className="data-table">
      <table>
        <thead>
          <tr>
            <th>Date</th>
            <th>Customer</th>
            <th>Amount</th>
            <th>Status</th>
            <th>Platform</th>
            <th>Stripe</th>
          </tr>
        </thead>
        <tbody>
          {orders.length === 0 ? (
            <Empty columns={6}>No orders yet.</Empty>
          ) : (
            orders.map((order) => {
              const stripe = stripeUrlFor(order);
              return (
                <tr key={order.id}>
                  <td>{formatDate(order.paid_at ?? order.created_at)}</td>
                  <td>
                    <Customer email={order.email} />
                  </td>
                  <td className="num">{formatMoney(order.amount_cents, order.currency)}</td>
                  <td>
                    <Pill tone={orderTone(order.status)}>{order.status}</Pill>
                  </td>
                  <td>{order.selected_platform ?? '—'}</td>
                  <td>
                    {stripe ? (
                      <a href={stripe} target="_blank" rel="noreferrer">
                        Open ↗
                      </a>
                    ) : (
                      '—'
                    )}
                  </td>
                </tr>
              );
            })
          )}
        </tbody>
      </table>
    </div>
  );
}

export function EmailsTable({ events }: { events: readonly EmailEventWithEmail[] }) {
  return (
    <div className="data-table">
      <table>
        <thead>
          <tr>
            <th>Date</th>
            <th>Customer</th>
            <th>Template</th>
            <th>Status</th>
            <th>Message id</th>
            <th>Error</th>
          </tr>
        </thead>
        <tbody>
          {events.length === 0 ? (
            <Empty columns={6}>Nothing sent yet.</Empty>
          ) : (
            events.map((event) => (
              <tr key={event.id}>
                <td>{formatDate(event.created_at)}</td>
                <td>
                  <Customer email={event.email} />
                </td>
                <td>{event.template}</td>
                <td>
                  <Pill tone={emailTone(event.status)}>{event.status}</Pill>
                </td>
                <td>{event.provider_message_id ?? '—'}</td>
                <td className="wrap">{event.error ?? ''}</td>
              </tr>
            ))
          )}
        </tbody>
      </table>
    </div>
  );
}

export function CustomersTable({ customers }: { customers: readonly CustomerSummary[] }) {
  return (
    <div className="data-table">
      <table>
        <thead>
          <tr>
            <th>Email</th>
            <th>Name</th>
            <th>Joined</th>
            <th>Orders</th>
            <th>Licence</th>
            <th>Devices</th>
            <th>Role</th>
          </tr>
        </thead>
        <tbody>
          {customers.length === 0 ? (
            <Empty columns={7}>No accounts match.</Empty>
          ) : (
            customers.map((customer) => (
              <tr key={customer.id}>
                <td>
                  <Customer email={customer.email} />
                </td>
                <td>{customer.displayName || '—'}</td>
                <td>{formatDate(customer.createdAt)}</td>
                <td className="num">{customer.orders}</td>
                <td>
                  {customer.licenseStatus ? (
                    <Pill tone={licenseTone(customer.licenseStatus)}>{customer.licenseStatus}</Pill>
                  ) : (
                    '—'
                  )}
                </td>
                <td className="num">{customer.activeDevices}</td>
                <td>{customer.isAdmin ? <Pill tone="neutral">admin</Pill> : ''}</td>
              </tr>
            ))
          )}
        </tbody>
      </table>
    </div>
  );
}
