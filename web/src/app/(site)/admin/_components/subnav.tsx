'use client';

import Link from 'next/link';
import { usePathname } from 'next/navigation';

const ITEMS = [
  { href: '/admin', label: 'Dashboard' },
  { href: '/admin/customers', label: 'Customers' },
  { href: '/admin/orders', label: 'Orders' },
  { href: '/admin/emails', label: 'Email' },
  { href: '/admin/discounts', label: 'Discounts' },
  { href: '/admin/licenses', label: 'Licenses' },
  { href: '/admin/releases', label: 'Releases' },
] as const;

/** Client-side only for the current-page marker; the links are static. */
export function AdminSubnav() {
  const pathname = usePathname();
  const current = (href: string): boolean =>
    href === '/admin' ? pathname === '/admin' : pathname === href || pathname.startsWith(`${href}/`);

  return (
    <nav className="subnav" aria-label="Administration">
      {ITEMS.map((item) => (
        <Link key={item.href} href={item.href} aria-current={current(item.href) ? 'page' : undefined}>
          {item.label}
        </Link>
      ))}
      {/* The customer record has no top-level entry: it is reached from a customer. */}
      {pathname.startsWith('/admin/support') ? (
        <Link href="/admin/support" aria-current="page">
          Customer record
        </Link>
      ) : null}
    </nav>
  );
}
