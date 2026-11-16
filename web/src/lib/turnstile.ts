/**
 * The sign-in page's CAPTCHA: Cloudflare Turnstile.
 *
 * The emailed sign-in link and the reset link go through
 * `/api/auth/email-link`, which asks Turnstile before Supabase sends anything,
 * so the form cannot be used by bots to make accounts for addresses that were
 * never theirs. (Supabase's own CAPTCHA was not used: it would cover every way
 * in, including the desktop app's sign-in, which cannot show a Turnstile box.)
 *
 * Both keys come from the Cloudflare dashboard and live in Vercel, never here:
 * `NEXT_PUBLIC_TURNSTILE_SITE_KEY` (public, drawn on the page) and
 * `TURNSTILE_SECRET_KEY` (server only). **Until the secret is set the check is
 * skipped**, so the form keeps working while the keys are being made; the
 * per-address rate limit applies either way.
 */

export const TURNSTILE_SCRIPT = 'https://challenges.cloudflare.com/turnstile/v0/api.js?render=explicit';
const VERIFY = 'https://challenges.cloudflare.com/turnstile/v0/siteverify';

export type TurnstileVerdict = { ok: true; skipped: boolean } | { ok: false; reason: string };

export const verifyTurnstile = async (
  token: string | null | undefined,
  remoteIp: string | null,
  secret: string | undefined = process.env.TURNSTILE_SECRET_KEY,
  post: typeof fetch = fetch,
): Promise<TurnstileVerdict> => {
  if (!secret) return { ok: true, skipped: true };
  if (!token) return { ok: false, reason: 'missing' };
  const form = new URLSearchParams({ secret, response: token });
  if (remoteIp && remoteIp !== 'unknown') form.set('remoteip', remoteIp);
  try {
    const response = await post(VERIFY, { method: 'POST', body: form, cache: 'no-store' });
    const body = (await response.json()) as { success?: boolean; 'error-codes'?: string[] };
    return body.success ? { ok: true, skipped: false } : { ok: false, reason: (body['error-codes'] ?? []).join(',') || 'rejected' };
  } catch {
    // Cloudflare unreachable: refuse rather than wave everything through, and
    // say so, so a person can try again.
    return { ok: false, reason: 'unreachable' };
  }
};
