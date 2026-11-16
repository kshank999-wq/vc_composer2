'use client';

import { useRouter } from 'next/navigation';
import { useState } from 'react';

const post = async (url: string, body: unknown, method = 'POST') => {
  const response = await fetch(url, { method, headers: { 'content-type': 'application/json' }, body: JSON.stringify(body) });
  const payload = (await response.json().catch(() => ({}))) as { error?: string; url?: string };
  return { ok: response.ok, ...payload };
};

/** Start a collaboration plan: how many editor seats, monthly or yearly. */
export function BuyPlan() {
  const [seats, setSeats] = useState(1);
  const [plan, setPlan] = useState<'monthly' | 'yearly'>('monthly');
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const start = async () => {
    setBusy(true);
    setError(null);
    const r = await post('/api/collab/checkout', { seats, plan });
    if (r.ok && r.url) window.location.assign(r.url);
    else {
      setError(r.error ?? 'Checkout could not be started');
      setBusy(false);
    }
  };
  return (
    <div className="actions">
      <label>
        Editor seats{' '}
        <input type="number" min={1} max={100} value={seats} onChange={(e) => setSeats(Math.max(1, Number(e.target.value) || 1))} />
      </label>
      <select value={plan} onChange={(e) => setPlan(e.target.value as 'monthly' | 'yearly')} aria-label="Billing">
        <option value="monthly">Monthly</option>
        <option value="yearly">Yearly</option>
      </select>
      <button type="button" className="button" onClick={() => void start()} disabled={busy}>
        {busy ? 'Starting…' : 'Start a collaboration plan'}
      </button>
      {error ? (
        <p className="error" role="alert">
          {error}
        </p>
      ) : null}
    </div>
  );
}

/** Invite someone to a project by email, with a role. */
export function InviteForm({ projectId }: { projectId: string }) {
  const router = useRouter();
  const [email, setEmail] = useState('');
  const [role, setRole] = useState('reviewer');
  const [message, setMessage] = useState<{ ok: boolean; text: string } | null>(null);
  const invite = async () => {
    setMessage(null);
    const r = await post(`/api/collab/projects/${projectId}/members`, { email, role });
    if (r.ok) {
      setMessage({ ok: true, text: `Invitation sent to ${email}` });
      setEmail('');
      router.refresh();
    } else setMessage({ ok: false, text: r.error ?? 'The invitation could not be sent' });
  };
  return (
    <form
      className="actions"
      onSubmit={(e) => {
        e.preventDefault();
        void invite();
      }}
    >
      <input type="email" required placeholder="name@example.com" value={email} onChange={(e) => setEmail(e.target.value)} aria-label="Email" />
      <select value={role} onChange={(e) => setRole(e.target.value)} aria-label="Role">
        <option value="editor">Editor / composer (seat)</option>
        <option value="arranger">Arranger / orchestrator (seat)</option>
        <option value="reviewer">Reviewer / director (free)</option>
        <option value="viewer">View only (free)</option>
      </select>
      <button type="submit" className="button secondary">
        Invite
      </button>
      {message ? <p className={message.ok ? 'notice' : 'error'}>{message.text}</p> : null}
    </form>
  );
}

export function RemoveMember({ projectId, memberId }: { projectId: string; memberId: string }) {
  const router = useRouter();
  const [busy, setBusy] = useState(false);
  return (
    <button
      type="button"
      className="link-button"
      disabled={busy}
      onClick={async () => {
        if (!window.confirm('Remove this person from the project?')) return;
        setBusy(true);
        await fetch(`/api/collab/projects/${projectId}/members/${memberId}`, { method: 'DELETE' });
        router.refresh();
      }}
    >
      Remove
    </button>
  );
}
