'use client';

import Link from 'next/link';
import { useState } from 'react';

export function AcceptInvite({ token }: { token: string }) {
  const [state, setState] = useState<'idle' | 'working' | 'done'>('idle');
  const [error, setError] = useState<string | null>(null);

  const accept = async () => {
    setState('working');
    setError(null);
    const response = await fetch(`/api/collab/invites/${token}`, { method: 'POST' });
    const payload = (await response.json().catch(() => ({}))) as { error?: string };
    if (!response.ok) {
      setError(payload.error ?? 'The invitation could not be accepted');
      setState('idle');
      return;
    }
    setState('done');
  };

  if (state === 'done')
    return (
      <p className="notice">
        You are in. Open it from VC Composer with File &gt; Open Shared Project, or see it under{' '}
        <Link href="/account/collaboration">My shared projects</Link>.
      </p>
    );
  return (
    <>
      <button type="button" className="button" onClick={() => void accept()} disabled={state === 'working'}>
        {state === 'working' ? 'Accepting…' : 'Accept the invitation'}
      </button>
      {error ? (
        <p className="error" role="alert" style={{ marginTop: 12 }}>
          {error}
        </p>
      ) : null}
    </>
  );
}
