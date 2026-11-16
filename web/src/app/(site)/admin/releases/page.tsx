import type { Metadata } from 'next';
import Link from 'next/link';
import { currentAdmin } from '@/lib/admin';
import { adminClient } from '@/lib/supabase';
import { ReleaseManager, type ReleaseBuildRow } from './release-manager';

export const metadata: Metadata = { title: 'Releases' };
export const dynamic = 'force-dynamic';

/**
 * Release management.
 *
 * Deliberately plain: publishing a build is an operational act, not a place to
 * be clever. What matters is that the operator can see which build each
 * platform is serving right now, and that activating one never disturbs the
 * other.
 */
export default async function ReleasesPage() {
  const admin = await currentAdmin();

  if (!admin) {
    return (
      <>
        <div className="hero">
          <h1>Releases</h1>
          <p>This area is for administrators.</p>
        </div>
        <Link href="/signin?next=/admin/releases" className="button">
          Sign in
        </Link>
      </>
    );
  }

  const { data } = await adminClient()
    .from('release_builds')
    .select('*')
    .order('created_at', { ascending: false })
    .limit(100);

  return (
    <>
      <div className="hero">
        <h1>Releases</h1>
        <p>
          Signed in as {admin.email}. Windows and macOS, stable and preview, are published independently: activating one never
          touches the other.
        </p>
      </div>
      <ReleaseManager initialBuilds={(data ?? []) as ReleaseBuildRow[]} />
    </>
  );
}
