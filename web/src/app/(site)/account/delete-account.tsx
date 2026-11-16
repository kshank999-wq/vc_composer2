'use client';

import { useState } from 'react';
import { browserClient } from '@/lib/supabase-browser';

/**
 * Deleting the account, from the account page.
 *
 * **What goes is said before the press and not after it**, on both sides: what
 * goes, and the one thing that does not (the receipt, which tax law wants and
 * which keeps no name). The confirmation is the address typed out, because this
 * is the only control on the site that cannot be undone.
 */
export function DeleteAccount({ email }: { email: string }) {
  const [open, setOpen] = useState(false);
  const [typed, setTyped] = useState('');
  const [status, setStatus] = useState<'idle' | 'working' | 'done'>('idle');
  const [error, setError] = useState<string | null>(null);

  const remove = async (event: React.FormEvent) => {
    event.preventDefault();
    setError(null);
    setStatus('working');

    const response = await fetch('/api/account/delete', {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ confirm: typed }),
    }).catch(() => null);

    const said = (await response?.json().catch(() => null)) as { error?: string } | null;
    if (!response?.ok) {
      setError(said?.error ?? 'Your account could not be deleted. Try again in a moment.');
      setStatus('idle');
      return;
    }

    // The account is gone, so the session in this browser is a key to nothing;
    // signing out is what makes the screen agree with that.
    await browserClient().auth.signOut().catch(() => undefined);
    setStatus('done');
  };

  if (status === 'done') {
    return (
      <section>
        <h2>Account deleted</h2>
        <p className="notice" role="status">
          Your account and your licence are gone. The projects on your own computer are untouched: those are
          yours, and we cannot reach them.
        </p>
      </section>
    );
  }

  return (
    <section>
      <h2>Delete this account</h2>
      <p className="muted small">
        Permanent, immediate, and not undoable. It takes your licence, the computers activated on it, and your
        sign-in.
      </p>
      <p className="muted small">
        One thing stays: the record that a purchase happened remains as a financial record with your name taken
        off it, because accounting law requires it. <strong>Cancel your subscription first</strong> (Manage
        subscription, above) so you are not charged again.
      </p>
      <p className="muted small">Projects saved on your own computer are not touched.</p>

      {open ? (
        <form onSubmit={remove} style={{ display: 'grid', gap: 16, maxWidth: 340, marginTop: 16 }}>
          <label className="muted small" htmlFor="delete-confirm">
            Type <strong>{email}</strong> to confirm.
          </label>
          <input
            id="delete-confirm"
            type="email"
            required
            autoComplete="off"
            value={typed}
            onChange={(event) => setTyped(event.target.value)}
            placeholder={email}
          />
          <div style={{ display: 'flex', gap: 12 }}>
            <button type="submit" className="button danger" disabled={status === 'working'}>
              {status === 'working' ? 'Deleting…' : 'Delete my account'}
            </button>
            <button
              type="button"
              className="button secondary"
              onClick={() => {
                setOpen(false);
                setTyped('');
                setError(null);
              }}
            >
              Keep it
            </button>
          </div>
        </form>
      ) : (
        <button type="button" className="button secondary" onClick={() => setOpen(true)} style={{ marginTop: 16 }}>
          Delete this account…
        </button>
      )}

      {error ? (
        <p className="error" role="alert" style={{ marginTop: 16 }}>
          {error}
        </p>
      ) : null}
    </section>
  );
}
