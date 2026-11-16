import type { Metadata } from 'next';
import Link from 'next/link';

export const metadata: Metadata = {
  title: 'Collaboration',
  description: 'Collaboration in VC Composer is coming, as a separate subscription.',
};

/**
 * Collaboration: coming soon.
 *
 * It will be a subscription of its own, separate from the desktop licence, the
 * way VC Writer's Writers Room is. There is no billing for it yet: when it
 * ships, its checkout stamps its own `kind` on the subscription
 * (`lib/checkout-kind.ts`) so the webhook can never confuse it with the
 * desktop licence.
 *
 * What the page says is limited to what is true today: the app already has the
 * local half (versions, snapshots, director review, comments, packages); sync
 * and roles, which need a server, are not built.
 */
export default function CollaborationPage() {
  return (
    <>
      <div className="hero">
        <span className="eyebrow">Coming soon</span>
        <h1>Collaboration</h1>
        <p>
          Working on one VC Composer project with your orchestrator, your music editor and your director is coming.
          It will be a separate subscription from the desktop licence, so you only pay for it if your team uses it.
        </p>
      </div>

      <section className="grid">
        <article className="card">
          <h3>What you can do today</h3>
          <p className="lede">
            The review tools are already in the app: track versions and snapshots, director review with approval
            locks, timecode comments with replies, review mixes, and Collect and Save Package to hand a project to
            another machine.
          </p>
        </article>
        <article className="card">
          <h3>What is coming</h3>
          <p className="lede">
            Sharing a project between people, with roles, without passing packages back and forth. It needs a
            server, and it will be offered as its own subscription.
          </p>
        </article>
        <article className="card">
          <h3>Your subscription is unaffected</h3>
          <p className="lede">
            The desktop subscription stays what it is. Collaboration will be an addition, never something taken out of
            it.
          </p>
        </article>
      </section>

      <div className="actions">
        <Link href="/download" className="button">
          Subscribe to VC Composer
        </Link>
        <Link href="/download#preview" className="button secondary">
          Try the free preview
        </Link>
      </div>
    </>
  );
}
