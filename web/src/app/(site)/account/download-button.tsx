'use client';

import { useState } from 'react';
import type { Platform, PublicChannel } from '@/domain';

/**
 * Asks the server for a fresh signed installer URL each time it is clicked.
 * The URL is short-lived by design, so it is never rendered into the page or
 * bookmarked.
 */
export function DownloadButton({
  platform,
  channel = 'stable',
  label = 'Download',
}: {
  platform: Platform;
  channel?: PublicChannel;
  label?: string;
}) {
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);

  const download = async () => {
    setBusy(true);
    setError(null);
    try {
      const response = await fetch(`/api/downloads/${platform}?channel=${channel}`);
      const payload = (await response.json()) as { url?: string; error?: string };
      if (!response.ok || !payload.url) throw new Error(payload.error ?? 'Download could not be prepared');
      window.location.assign(payload.url);
    } catch (cause) {
      setError(cause instanceof Error ? cause.message : 'Download could not be prepared');
    } finally {
      setBusy(false);
    }
  };

  return (
    <>
      <button type="button" className={channel === 'preview' ? 'button warm' : 'button'} onClick={download} disabled={busy}>
        {busy ? 'Preparing…' : label}
      </button>
      {error ? (
        <span className="error" role="alert" style={{ display: 'block', marginTop: 8 }}>
          {error}
        </span>
      ) : null}
    </>
  );
}
