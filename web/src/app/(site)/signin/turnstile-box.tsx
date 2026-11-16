'use client';

import { useEffect, useRef } from 'react';
import { TURNSTILE_SCRIPT } from '@/lib/turnstile';

/**
 * The Turnstile box. Draws nothing until
 * `NEXT_PUBLIC_TURNSTILE_SITE_KEY` is set, and the server skips its check
 * until the secret is, so the form works before the keys exist.
 *
 * The script is added from this bundle rather than a tag in the page, so the
 * site's `strict-dynamic` policy trusts it without a nonce.
 */

interface TurnstileApi {
  render(element: HTMLElement, options: Record<string, unknown>): string;
  reset(id?: string): void;
  remove(id: string): void;
}

declare global {
  interface Window {
    turnstile?: TurnstileApi;
  }
}

export const TURNSTILE_SITE_KEY = process.env.NEXT_PUBLIC_TURNSTILE_SITE_KEY ?? '';

let loading: Promise<TurnstileApi> | null = null;
const loadTurnstile = (): Promise<TurnstileApi> => {
  if (window.turnstile) return Promise.resolve(window.turnstile);
  loading ??= new Promise((resolve, reject) => {
    const script = document.createElement('script');
    script.src = TURNSTILE_SCRIPT;
    script.async = true;
    script.onload = () => (window.turnstile ? resolve(window.turnstile) : reject(new Error('no turnstile')));
    script.onerror = () => {
      loading = null;
      reject(new Error('turnstile did not load'));
    };
    document.head.appendChild(script);
  });
  return loading;
};

/** `round` changes after each try, so a used token is never sent twice. */
export function TurnstileBox({ onToken, round }: { onToken(token: string): void; round: number }) {
  const holder = useRef<HTMLDivElement>(null);
  const said = useRef(onToken);
  said.current = onToken;

  useEffect(() => {
    if (!TURNSTILE_SITE_KEY || !holder.current) return;
    let id: string | null = null;
    let gone = false;
    said.current('');
    void loadTurnstile()
      .then((api) => {
        if (gone || !holder.current) return;
        id = api.render(holder.current, {
          sitekey: TURNSTILE_SITE_KEY,
          theme: 'dark',
          callback: (token: string) => said.current(token),
          'expired-callback': () => said.current(''),
          'error-callback': () => said.current(''),
        });
      })
      .catch(() => undefined);
    return () => {
      gone = true;
      if (id && window.turnstile) window.turnstile.remove(id);
    };
  }, [round]);

  if (!TURNSTILE_SITE_KEY) return null;
  return <div ref={holder} style={{ minHeight: 65 }} />;
}
