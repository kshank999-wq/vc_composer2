import { AdminSubnav } from './_components/subnav';

/**
 * The sub-navigation every administration page shares
 *.
 *
 * Deliberately no access check here. Each page refuses a non-administrator
 * for itself, so the layout deciding nothing means there is nothing for it
 * to get wrong; the nav is just links.
 */
export default function AdminLayout({ children }: { children: React.ReactNode }) {
  return (
    <>
      <AdminSubnav />
      {children}
    </>
  );
}
