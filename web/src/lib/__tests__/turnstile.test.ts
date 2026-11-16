import { describe, expect, it, vi } from 'vitest';
import { verifyTurnstile } from '../turnstile';

/** The sign-in CAPTCHA's server check. */
describe('verifyTurnstile', () => {
  const answer = (body: unknown) => vi.fn(async () => new Response(JSON.stringify(body))) as unknown as typeof fetch;

  it('is skipped until the secret is set, so the form keeps working', async () => {
    expect(await verifyTurnstile(undefined, null, undefined)).toEqual({ ok: true, skipped: true });
  });

  it('refuses a missing token once the secret is set', async () => {
    expect(await verifyTurnstile('', '1.2.3.4', 'secret', answer({ success: true }))).toEqual({ ok: false, reason: 'missing' });
  });

  it('passes what Cloudflare passes, and refuses what it refuses', async () => {
    expect(await verifyTurnstile('t', '1.2.3.4', 'secret', answer({ success: true }))).toEqual({ ok: true, skipped: false });
    expect(await verifyTurnstile('t', null, 'secret', answer({ success: false, 'error-codes': ['invalid-input-response'] }))).toEqual({
      ok: false,
      reason: 'invalid-input-response',
    });
  });

  it('refuses when Cloudflare cannot be reached', async () => {
    const down = vi.fn(async () => {
      throw new Error('down');
    }) as unknown as typeof fetch;
    expect(await verifyTurnstile('t', null, 'secret', down)).toEqual({ ok: false, reason: 'unreachable' });
  });
});
