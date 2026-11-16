import type { Metadata } from 'next';
import Link from 'next/link';
import { type CollabRole, roleWords, seatsUsed, takesSeat } from '@/domain';
import { listProjects } from '@/lib/collab-service';
import { adminClient, currentUserOrNull } from '@/lib/supabase';
import { BuyPlan, InviteForm, RemoveMember } from './controls';

export const metadata: Metadata = { title: 'My shared projects' };
export const dynamic = 'force-dynamic';

/**
 * Collaboration on the account: the owner's plan (editor seats), the shared
 * projects the account is in, and, for the projects it owns, who is in them.
 * Projects are shared and synced from VC Composer itself.
 */
export default async function CollaborationAccountPage({ searchParams }: { searchParams: { started?: string } }) {
  const user = await currentUserOrNull();
  if (!user) {
    return (
      <>
        <div className="hero">
          <h1>My shared projects</h1>
          <p>Sign in to see the projects you share.</p>
        </div>
        <Link href="/signin?next=%2Faccount%2Fcollaboration" className="button">
          Sign in
        </Link>
      </>
    );
  }

  const db = adminClient();
  const [{ data: plan }, projects] = await Promise.all([
    db.from('collab_plans').select('editor_seats,status,current_period_end').eq('owner_id', user.id).maybeSingle(),
    listProjects(db, user.id),
  ]);
  const owned = projects.filter((p) => p.role === 'owner');
  const { data: members } = owned.length
    ? await db
        .from('collab_members')
        .select('id,project_id,role,invited_email,accepted_at,user_id')
        .in(
          'project_id',
          owned.map((p) => p.id),
        )
        .order('created_at')
    : { data: [] as never[] };
  const used = seatsUsed(
    (members ?? []).map((m) => ({ userId: m.user_id as string | null, email: m.invited_email as string, role: m.role as CollabRole })),
  );
  const seats = Number(plan?.editor_seats ?? 0);
  const active = plan?.status === 'active';

  return (
    <>
      <div className="hero">
        <span className="eyebrow">Collaboration</span>
        <h1>My shared projects</h1>
        <p>
          Share a project from VC Composer (File &gt; Share Project), then invite people here. Editors and arrangers take a
          seat on your plan; reviewers and view-only people are free.
        </p>
      </div>
      {searchParams.started ? <p className="notice">Your collaboration plan is starting. It shows here within a minute.</p> : null}

      <section>
        <h2>Your plan</h2>
        {plan ? (
          <article className="card">
            <h3>
              {seats} editor seat{seats === 1 ? '' : 's'} · {active ? 'active' : plan.status === 'past_due' ? 'payment due' : 'ended'}
            </h3>
            <p className="lede">
              {used} of {seats} in use. Change the number of seats, or cancel, from{' '}
              <Link href="/account">My account &gt; Manage subscription</Link>.
            </p>
          </article>
        ) : (
          <article className="card">
            <h3>No plan yet</h3>
            <p className="lede">
              You can share projects and invite reviewers and viewers without one. A plan adds editor seats, for the
              people who write music with you.
            </p>
            <BuyPlan />
          </article>
        )}
      </section>

      <section>
        <h2>Projects</h2>
        {projects.length === 0 ? (
          <p className="lede">None yet. Share one from VC Composer with File &gt; Share Project.</p>
        ) : (
          <div className="grid">
            {projects.map((p) => (
              <article key={p.id} className="card">
                <h3>{p.name}</h3>
                <p className="lede">
                  {roleWords(p.role).label}
                  {p.role === 'owner' ? '' : ` · shared by ${p.ownerName || 'its owner'}`} · revision {p.head}
                </p>
                {p.role === 'owner' ? (
                  <>
                    <ul className="member-list">
                      {(members ?? [])
                        .filter((m) => m.project_id === p.id)
                        .map((m) => (
                          <li key={m.id as string}>
                            <span>{m.invited_email as string}</span>{' '}
                            <span className="muted">
                              {roleWords(m.role as CollabRole).label}
                              {takesSeat(m.role as CollabRole) ? ' · seat' : ''}
                              {m.accepted_at ? '' : ' · invited'}
                            </span>{' '}
                            {m.role !== 'owner' ? <RemoveMember projectId={p.id} memberId={m.id as string} /> : null}
                          </li>
                        ))}
                    </ul>
                    <InviteForm projectId={p.id} />
                  </>
                ) : null}
              </article>
            ))}
          </div>
        )}
      </section>
    </>
  );
}
