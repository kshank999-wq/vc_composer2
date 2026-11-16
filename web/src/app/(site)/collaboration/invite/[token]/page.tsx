import type { Metadata } from 'next';
import Link from 'next/link';
import { currentUserOrNull } from '@/lib/supabase';
import { AcceptInvite } from './accept';

export const metadata: Metadata = { title: 'Invitation to a shared project' };
export const dynamic = 'force-dynamic';

/** The link in an invitation email: sign in (or create a free account), then accept. */
export default async function InvitePage({ params }: { params: { token: string } }) {
  const user = await currentUserOrNull();
  const here = `/collaboration/invite/${params.token}`;
  return (
    <>
      <div className="hero">
        <span className="eyebrow">Collaboration</span>
        <h1>You are invited to a shared project</h1>
        <p>
          Accept with your VC Composer account, then open the project from VC Composer (File &gt; Open Shared Project).
          Reviewers and viewers need no subscription; editors are covered by the project owner&apos;s plan.
        </p>
      </div>
      {user ? (
        <AcceptInvite token={params.token} />
      ) : (
        <div className="actions">
          <Link href={`/signin?next=${encodeURIComponent(here)}`} className="button">
            Sign in to accept
          </Link>
        </div>
      )}
    </>
  );
}
