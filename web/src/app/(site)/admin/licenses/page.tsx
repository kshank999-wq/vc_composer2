import type { Metadata } from 'next';
import Link from 'next/link';
import { currentAdmin } from '@/lib/admin';
import { listGivenLicenses } from '@/lib/license-grants';
import { LicenseGiver } from './license-giver';

export const metadata: Metadata = { title: 'Licenses' };
export const dynamic = 'force-dynamic';

/**
 * Licences given rather than bought: academic giveaways, and the
 * master licence that covers any number of computers.
 */
export default async function LicensesPage() {
  const admin = await currentAdmin();

  if (!admin) {
    return (
      <>
        <div className="hero">
          <h1>Licenses</h1>
          <p>This area is for administrators.</p>
        </div>
        <Link href="/signin?next=/admin/licenses" className="button">
          Sign in
        </Link>
      </>
    );
  }

  const licenses = await listGivenLicenses();

  return (
    <>
      <div className="hero">
        <h1>Licenses</h1>
        <p>
          Signed in as {admin.email}. Give an academic license to a school, a class or a student, or a master
          license that runs on any number of computers. Either works the moment it is issued: the recipient signs
          in on this site with that address and downloads.
        </p>
      </div>
      <LicenseGiver initialLicenses={licenses} adminEmail={admin.email} />
    </>
  );
}
