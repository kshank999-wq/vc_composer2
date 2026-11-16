import type { Metadata } from 'next';
import Link from 'next/link';

export const metadata: Metadata = {
  title: 'Collaboration',
  description:
    'Share a VC Composer project, audio and picture included, with your orchestrator, music editor and director: sync with merge, roles, and part locks.',
};

/**
 * Collaboration (Main Spec §15).
 *
 * What the page says is limited to what is true: sharing, sync with merge,
 * the five roles and part locks are built (desktop File > Shared Projects,
 * the /api/collab routes, My shared projects). The per-editor-seat plan is
 * sold once its Stripe prices are configured; until then checkout answers
 * "not on sale yet", so the page does not quote a price.
 */
export default function CollaborationPage() {
  return (
    <>
      <div className="hero">
        <span className="eyebrow">Collaboration</span>
        <h1>One project, the whole team</h1>
        <p>
          Share a project from the desktop app and invite your orchestrator, music editor and director. Everyone works
          on their own copy; Sync brings in what the others did and sends yours. Separate work combines by itself, and
          you are asked only when two people changed the same thing.
        </p>
      </div>

      <section className="grid">
        <article className="card">
          <h3>Sync with merge</h3>
          <p className="lede">
            File &gt; Shared Projects in the desktop app. A sync merges note by note and track by track; a real conflict
            shows both versions side by side, and you keep yours or theirs. Every sync is one undo step.
          </p>
        </article>
        <article className="card">
          <h3>Roles and part locks</h3>
          <p className="lede">
            Owner, editor, arranger, reviewer and view-only. Lock a part while you write it and nobody else can change
            it until you unlock it; a forgotten lock frees itself after two hours.
          </p>
        </article>
        <article className="card">
          <h3>Who pays</h3>
          <p className="lede">
            The project owner. Editors and arrangers each take a seat on the owner&apos;s collaboration plan; reviewers
            and view-only people join free. It is separate from the desktop licence, so your licence is unaffected.
          </p>
        </article>
      </section>

      <section className="grid">
        <article className="card">
          <h3>What travels</h3>
          <p className="lede">
            The project and its media: tracks, notes, versions, mix, markers and comments, and the audio and picture
            files it uses. Each file uploads once and downloads only to the people who don&apos;t have it yet; if two
            people record a take with the same name, both are kept.
          </p>
        </article>
        <article className="card">
          <h3>Every revision kept</h3>
          <p className="lede">
            Each sync is a numbered revision with who made it and when, and the project&apos;s history shows who joined,
            changed roles and locked parts.
          </p>
        </article>
        <article className="card">
          <h3>Inviting people</h3>
          <p className="lede">
            From My shared projects in your account: invite by email with a role, change roles, or remove someone.
          </p>
        </article>
      </section>

      <div className="actions">
        <Link href="/account/collaboration" className="button">
          My shared projects
        </Link>
        <Link href="/download" className="button secondary">
          Get VC Composer
        </Link>
      </div>
    </>
  );
}
