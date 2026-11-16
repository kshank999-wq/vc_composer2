'use client';

import { useState } from 'react';

/**
 * **Manage the subscription**: change the card, switch between monthly and
 * yearly, cancel, see receipts. All of it is Stripe's Customer Portal, which
 * this opens.
 */
export function ManageSubscription() {
  const [opening, setOpening] = useState(false);
  const [error, setError] = useState<string | null>(null);

  const open = async () => {
    setOpening(true);
    setError(null);
    const response = await fetch('/api/account/billing', { method: 'POST' });
    const payload = (await response.json().catch(() => ({}))) as { url?: string; error?: string };
    if (!response.ok || !payload.url) {
      setError(payload.error ?? 'Billing could not be opened.');
      setOpening(false);
      return;
    }
    window.location.assign(payload.url);
  };

  return (
    <>
      <button type="button" className="button secondary" onClick={() => void open()} disabled={opening}>
        {opening ? 'Opening…' : 'Manage subscription'}
      </button>
      {error ? (
        <p className="error" role="alert" style={{ marginTop: 12 }}>
          {error}
        </p>
      ) : null}
    </>
  );
}
