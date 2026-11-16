'use client';

import { useEffect, useState } from 'react';
import Link from 'next/link';
import { browserClient } from '@/lib/supabase-browser';
import { SHORTEST, checkNewPassword, refusalFor } from '@/lib/password-words';
import { SUPPORT_EMAIL } from '@/lib/site';

/**
 * Setting a password after a reset link.
 *
 * **The link is the proof, so nothing else is asked for.** Somebody arrives
 * here because they have forgotten their password, so this screen sends
 * `updateUser({ password })` and never a current password.
 *
 * It needs no token of its own: `/auth/callback` exchanges the emailed code for
 * the session cookie and sends the reader on to here. A reader who opened the
 * link in another browser arrives signed out, and that is the failure to
 * explain rather than a form to draw.
 */
export default function ResetPage() {
  const [ready, setReady] = useState(false);
  const [signedIn, setSignedIn] = useState(false);
  const [email, setEmail] = useState<string | null>(null);

  const [password, setPassword] = useState('');
  const [again, setAgain] = useState('');
  const [status, setStatus] = useState<'idle' | 'working' | 'done'>('idle');
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    let alive = true;
    void (async () => {
      const { data } = await browserClient().auth.getSession();
      if (!alive) return;
      setSignedIn(Boolean(data.session));
      setEmail(data.session?.user.email ?? null);
      setReady(true);
    })();
    return () => {
      alive = false;
    };
  }, []);

  const save = async (event: React.FormEvent) => {
    event.preventDefault();
    const said = checkNewPassword(password, again);
    if (said) {
      setError(said);
      return;
    }
    setError(null);
    setStatus('working');

    const { error: failure } = await browserClient().auth.updateUser({ password });
    if (failure) {
      setError(
        refusalFor(
          failure,
          // This screen sends no current password, so a refusal naming one is
          // the project refusing the reset itself: not fixable by typing.
          `This link could not set a password on its own. Write to ${SUPPORT_EMAIL} and a person will sort it out.`,
        ),
      );
      setStatus('idle');
      return;
    }
    setPassword('');
    setAgain('');
    setStatus('done');
  };

  if (!ready) return null;

  if (status === 'done') {
    return (
      <>
        <div className="hero hero-compact">
          <h1>Password set</h1>
        </div>
        <section>
          <p className="notice" role="status">
            You are signed in, and that password works everywhere now, including from the desktop app.
          </p>
          <Link href="/account" className="button">
            My account
          </Link>
        </section>
      </>
    );
  }

  if (!signedIn) {
    return (
      <>
        <div className="hero hero-compact">
          <h1>That link did not open</h1>
        </div>
        <section>
          <p className="notice">
            A reset link only works in the browser that asked for it, and it works once.{' '}
            <strong>If your mail app opened it itself, copy the link and paste it into your own browser</strong> —
            or ask for a new one.
          </p>
          <Link href="/signin" className="button">
            Ask for a new link
          </Link>
        </section>
      </>
    );
  }

  return (
    <>
      <div className="hero hero-compact">
        <h1>Set a new password</h1>
        <p>{email ?? 'Your account'}</p>
      </div>
      <section>
        <p className="muted small">
          The link you opened is what proves this is you, so there is nothing else to confirm — pick a new
          password and it takes effect at once, on every device.
        </p>
        <form onSubmit={save} style={{ display: 'grid', gap: 16, maxWidth: 340 }}>
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
            {status === 'working' ? 'Saving…' : 'Set it'}
          </button>
        </form>
        {error ? (
          <p className="error" role="alert" style={{ marginTop: 16 }}>
            {error}
          </p>
        ) : null}
      </section>
    </>
  );
}
