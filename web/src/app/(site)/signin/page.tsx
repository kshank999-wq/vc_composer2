'use client';

import { Suspense, useState } from 'react';
import { useRouter, useSearchParams } from 'next/navigation';
import { browserClient } from '@/lib/supabase-browser';
import { safeNextPath } from '@/lib/auth-redirect';
import { TURNSTILE_SITE_KEY, TurnstileBox } from './turnstile-box';

/**
 * Signing in, two ways.
 *
 * **An emailed link is bound to the browser that asked for it** (the PKCE
 * verifier is in that browser's storage), and a mail app often opens links in
 * a browser of its own, so the exchange fails and the form comes back. A
 * password has no handoff between browsers, so it works wherever it is typed.
 * The link stays, because it is the only way in for somebody who has never
 * set a password, which is everybody arriving for the free preview.
 */
export default function SignInPage() {
  return (
    <Suspense fallback={null}>
      <SignInForm />
    </Suspense>
  );
}

/**
 * Which way in is showing. The password is first, being the one that always
 * works. **Forgetting is not a third way in**, so it is not a third tab: it is
 * a repair reached from the password form, with its own way back.
 */
type Way = 'password' | 'link' | 'forgot';

function SignInForm() {
  // Where to land after: the page that sent the customer here (the download
  // page's preview section, the admin console), or the account page.
  const params = useSearchParams();
  const router = useRouter();
  const next = safeNextPath(params.get('next'));
  /**
   * Why the last link did not work, where the callback said so. Said here,
   * because a form that silently reappears reads as a loop — and now the
   * notice can offer the thing that would stop it happening again.
   */
  const failed = params.get('error') === 'link_expired';

  const [way, setWay] = useState<Way>('password');
  const [email, setEmail] = useState('');
  const [password, setPassword] = useState('');
  const [status, setStatus] = useState<'idle' | 'working' | 'sent' | 'reset-sent'>('idle');
  const [error, setError] = useState<string | null>(null);
  // The CAPTCHA's answer for the link and reset forms, and a counter that
  // draws a fresh box after each try.
  const [human, setHuman] = useState('');
  const [round, setRound] = useState(0);
  const needsHuman = TURNSTILE_SITE_KEY !== '' && human === '';

  /** Both emailed links go through the server, which checks the CAPTCHA first. */
  const askServer = async (kind: 'link' | 'reset'): Promise<string | null> => {
    const response = await fetch('/api/auth/email-link', {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ kind, email, next, token: human }),
    }).catch(() => null);
    setHuman('');
    setRound((n) => n + 1);
    if (!response) return 'That could not be sent. Check your connection and try again.';
    if (response.ok) return null;
    const body = (await response.json().catch(() => ({}))) as { error?: string };
    return body.error ?? 'That could not be sent. Try again in a moment.';
  };

  const signIn = async (event: React.FormEvent) => {
    event.preventDefault();
    setStatus('working');
    setError(null);
    const { error: failure } = await browserClient().auth.signInWithPassword({ email, password });
    if (failure) {
      // Supabase says *Invalid login credentials* for a wrong password and for
      // an account that has never set one, which are different problems with
      // different answers — so the sentence names both and points at the link.
      setError(
        failure.message === 'Invalid login credentials'
          ? 'That email and password do not match. If you have never set a password, ask for a link instead and set one on your account page.'
          : failure.message,
      );
      setStatus('idle');
      return;
    }
    // A full navigation rather than a push: the session is a cookie the server
    // components read, and only a real request hands it to them.
    window.location.assign(next);
  };

  const sendLink = async (event: React.FormEvent) => {
    event.preventDefault();
    setStatus('working');
    setError(null);
    const failure = await askServer('link');
    if (failure) {
      setError(failure);
      setStatus('idle');
      return;
    }
    setStatus('sent');
  };

  /**
   * The reset link. It lands on `/auth/callback` exactly as the sign-in link
   * does, so the code becomes the session cookie before `/reset` draws.
   */
  const sendReset = async (event: React.FormEvent) => {
    event.preventDefault();
    setStatus('working');
    setError(null);
    const failure = await askServer('reset');
    if (failure) {
      setError(failure);
      setStatus('idle');
      return;
    }
    setStatus('reset-sent');
  };

  if (status === 'reset-sent') {
    return (
      <>
        <div className="hero hero-compact">
          <h1>Check your email</h1>
        </div>
        <section>
          <p className="notice">
            If {email} has an account, a link to set a new password is on its way.{' '}
            <strong>Open it in this browser</strong> — the link only works where it was asked for, and it works
            once.
          </p>
          <p className="muted small">
            You will not be asked for your old password: the link is what proves it is you.
          </p>
          <button type="button" className="button secondary" onClick={() => setStatus('idle')}>
            Back
          </button>
        </section>
      </>
    );
  }

  if (status === 'sent') {
    return (
      <>
        <div className="hero hero-compact">
          <h1>Check your email</h1>
        </div>
        <section>
          <p className="notice">
            A sign-in link is on its way to {email}. <strong>Open it in this browser</strong> — the link only
            works where it was asked for. If your mail app opens links in a browser of its own, copy the link
            and paste it here instead.
          </p>
          <p className="muted small">
            Once you are in, set a password on your account page and you will not need a link again.
          </p>
          <button type="button" className="button secondary" onClick={() => setStatus('idle')}>
            Back
          </button>
        </section>
      </>
    );
  }

  return (
    <>
      <div className="hero hero-compact">
        <h1>Sign in</h1>
        <p>Use the address you subscribed with, or any address to try the free preview.</p>
      </div>
      <section>
        <div className="signin-ways" role="tablist" aria-label="How to sign in">
          <button
            type="button"
            role="tab"
            aria-selected={way !== 'link'}
            className={way !== 'link' ? 'button' : 'button secondary'}
            onClick={() => {
              setWay('password');
              setError(null);
            }}
          >
            With a password
          </button>
          <button
            type="button"
            role="tab"
            aria-selected={way === 'link'}
            className={way === 'link' ? 'button' : 'button secondary'}
            onClick={() => {
              setWay('link');
              setError(null);
            }}
          >
            Email me a link
          </button>
        </div>

        {way === 'password' ? (
          <form onSubmit={signIn} style={{ display: 'grid', gap: 16, maxWidth: 340 }}>
            <input
              type="email"
              required
              autoComplete="username"
              value={email}
              onChange={(event) => setEmail(event.target.value)}
              placeholder="you@example.com"
              aria-label="Email address"
            />
            <input
              type="password"
              required
              autoComplete="current-password"
              value={password}
              onChange={(event) => setPassword(event.target.value)}
              placeholder="Password"
              aria-label="Password"
            />
            <button type="submit" className="button" disabled={status === 'working'}>
              {status === 'working' ? 'Signing in…' : 'Sign in'}
            </button>
            <p className="muted small">
              No password yet? Ask for a link, then set one on your account page. After that this is all you
              need, on any device and in the desktop app.
            </p>
            <button
              type="button"
              className="button secondary"
              onClick={() => {
                setWay('forgot');
                setError(null);
              }}
            >
              Forgotten it?
            </button>
          </form>
        ) : way === 'forgot' ? (
          <form onSubmit={sendReset} style={{ display: 'grid', gap: 16, maxWidth: 340 }}>
            <input
              type="email"
              required
              autoComplete="username"
              value={email}
              onChange={(event) => setEmail(event.target.value)}
              placeholder="you@example.com"
              aria-label="Email address"
            />
            <TurnstileBox key="reset" round={round} onToken={setHuman} />
            <button type="submit" className="button" disabled={status === 'working' || needsHuman}>
              {status === 'working' ? 'Sending…' : 'Email me a reset link'}
            </button>
            <p className="muted small">
              You will set a new one straight from the link. Your old password is not needed — that is the
              point.
            </p>
            <button
              type="button"
              className="button secondary"
              onClick={() => {
                setWay('password');
                setError(null);
              }}
            >
              Back to signing in
            </button>
          </form>
        ) : (
          <form onSubmit={sendLink} style={{ display: 'grid', gap: 16, maxWidth: 340 }}>
            <input
              type="email"
              required
              autoComplete="username"
              value={email}
              onChange={(event) => setEmail(event.target.value)}
              placeholder="you@example.com"
              aria-label="Email address"
            />
            <TurnstileBox key="link" round={round} onToken={setHuman} />
            <button type="submit" className="button" disabled={status === 'working' || needsHuman}>
              {status === 'working' ? 'Sending…' : 'Email me a link'}
            </button>
            <p className="muted small">
              Open the link in this same browser. On a phone that often means copying it out of your mail app.
            </p>
          </form>
        )}

        {error ? (
          <p className="error" role="alert" style={{ marginTop: 16 }}>
            {error}
          </p>
        ) : null}

        {failed ? (
          <p className="notice" role="status">
            That link could not sign you in: it had already been used, or it was opened in a different browser
            from the one that asked for it, which is what happens when a mail app opens links itself.{' '}
            <strong>A password works in any browser</strong>, so setting one on your account page is the way to
            stop this happening again.
          </p>
        ) : null}


      </section>
    </>
  );
}
