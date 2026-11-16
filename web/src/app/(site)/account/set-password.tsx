'use client';

import { useState } from 'react';
import {
  PASSWORD_STANDING_WORDS,
  describePasswordStanding,
  passwordStanding,
} from '@/domain';
import { browserClient } from '@/lib/supabase-browser';
import { SHORTEST, checkNewPassword, refusalFor } from '@/lib/password-words';

/**
 * Setting a password on an account that has only ever used an emailed link,
 * and changing the one you have.
 *
 * An emailed link is bound to the browser that asked for it, which is awkward
 * from a mail app's own browser; a password works anywhere, including when
 * signing in from the desktop app. Only somebody already signed in may set
 * one: `updateUser` writes to the session's own account.
 *
 * **The current password is asked for and not required**: a form that
 * demanded the old one would lock a link-only customer out of ever having a
 * first. The server is still the thing that refuses.
 *
 * The standing line reads `profiles.password_set_at`, which a database trigger
 * stamps whenever the password really changes (migration 0001).
 */

export function SetPassword({ setAt }: { setAt: string | null }) {
  const standing = passwordStanding(setAt);
  const [current, setCurrent] = useState('');
  const [password, setPassword] = useState('');
  const [again, setAgain] = useState('');
  const [status, setStatus] = useState<'idle' | 'working' | 'done'>('idle');
  const [error, setError] = useState<string | null>(null);

  const save = async (event: React.FormEvent) => {
    event.preventDefault();
    setError(null);

    const said = checkNewPassword(password, again);
    if (said) {
      setError(said);
      return;
    }

    setStatus('working');
    // **Sent only when it was typed.** An empty `current_password` is a
    // different request from one that was never made, and the account with no
    // password to prove is exactly the one that cannot fill the box.
    const { error: failure } = await browserClient().auth.updateUser(
      current.length > 0 ? { password, current_password: current } : { password },
    );
    if (failure) {
      setError(
        refusalFor(
          failure,
          'That is not your current password. If you have never set one, leave that box empty — and if you have forgotten it, ask for a reset link on the sign-in page.',
        ),
      );
      setStatus('idle');
      return;
    }
    setCurrent('');
    setPassword('');
    setAgain('');
    setStatus('done');
  };

  return (
    <section>
      <h2>Password</h2>
      <p className={standing === 'set' ? 'password-standing set' : 'password-standing'} role="status">
        <span aria-hidden="true">{standing === 'set' ? '●' : '○'}</span>{' '}
        <strong>{PASSWORD_STANDING_WORDS[standing]}</strong>
        {standing === 'set' && setAt ? (
          <span className="password-when">
            {' '}
            · {new Date(setAt).toLocaleDateString(undefined, { year: 'numeric', month: 'long', day: 'numeric' })}
          </span>
        ) : null}
      </p>
      <p className="password-standing-note">{describePasswordStanding(standing)}</p>
      {status === 'done' ? (
        <p className="notice" role="status">
          Saved. You can sign in with it anywhere now, including from the desktop app. Reload this page and it
          will say when.
        </p>
      ) : null}
      <form onSubmit={save} style={{ display: 'grid', gap: 16, maxWidth: 340 }}>
        <div style={{ display: 'grid', gap: 6 }}>
          <input
            type="password"
            autoComplete="current-password"
            value={current}
            onChange={(event) => setCurrent(event.target.value)}
            placeholder="Current password"
            aria-label="Current password"
            aria-describedby="current-password-note"
          />
          <span id="current-password-note" className="field-note">
            {standing === 'set'
              ? 'The one you have now.'
              : 'Leave this empty if you have never set one.'}{' '}
            Forgotten it? <a href="/signin">Ask for a reset link</a>.
          </span>
        </div>
        <input
          type="password"
          required
          autoComplete="new-password"
          value={password}
          onChange={(event) => setPassword(event.target.value)}
          placeholder={`New password (${SHORTEST} characters or more)`}
          aria-label="New password"
        />
        <input
          type="password"
          required
          autoComplete="new-password"
          value={again}
          onChange={(event) => setAgain(event.target.value)}
          placeholder="And again"
          aria-label="Repeat the new password"
        />
        <button type="submit" className="button" disabled={status === 'working'}>
          {status === 'working' ? 'Saving…' : 'Save it'}
        </button>
      </form>
      {error ? (
        <p className="error" role="alert" style={{ marginTop: 16 }}>
          {error}
        </p>
      ) : null}
    </section>
  );
}
