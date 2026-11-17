import { NextResponse } from 'next/server';
import { z } from 'zod';
import { safeNextPath } from '@/lib/auth-redirect';
import { RULES, clientAddress, rateLimit } from '@/lib/rate-limit';
import { adminClient, serverClient } from '@/lib/supabase';
import { verifyTurnstile } from '@/lib/turnstile';

/**
 * Sends the sign-in link or the password-reset link, after the CAPTCHA and a
 * rate limit. Called by `/signin` only.
 *
 * Asked for from the server's own Supabase client, which keeps the PKCE
 * verifier in this browser's cookie, so the link still has to be opened in
 * the browser that asked for it.
 *
 * A new address gets an account, which is how somebody tries the free
 * preview. If sign-ups are switched off in Supabase (closing the door the
 * public anon key would otherwise leave open), the account is made here with
 * the service role, and only after the CAPTCHA.
 */

const Body = z.object({
  kind: z.enum(['link', 'reset']),
  email: z.string().trim().toLowerCase().email().max(320),
  next: z.string().max(500).optional(),
  token: z.string().max(4096).optional(),
});

const signupsOff = (message: string) => /signups? not allowed|signup.*disabled/i.test(message);

export async function POST(request: Request) {
  const limited = await rateLimit(request, RULES.emailLink);
  if (limited) return limited;

  const parsed = Body.safeParse(await request.json().catch(() => null));
  if (!parsed.success) return NextResponse.json({ error: 'Enter your email address.' }, { status: 400 });
  const { kind, email, token } = parsed.data;

  const verdict = await verifyTurnstile(token, clientAddress(request));
  if (!verdict.ok) {
    // Cloudflare's reason, in the server log: a secret that does not belong
    // with the page's site key otherwise looks exactly like an unticked box.
    console.warn(`email-link: person check refused (${verdict.reason})`);
    const misconfigured = /invalid-input-secret|missing-input-secret/.test(verdict.reason);
    return NextResponse.json(
      {
        error: misconfigured
          ? 'Sign-in by email is not working on our side right now. Please use your password, or try again later.'
          : verdict.reason === 'unreachable'
            ? 'The check that you are a person could not be reached. Try again in a moment.'
            : 'Please tick the box to show you are a person, then try again.',
      },
      { status: misconfigured ? 503 : 400 },
    );
  }

  const origin = new URL(request.url).origin;
  const supabase = serverClient();

  if (kind === 'reset') {
    const { error } = await supabase.auth.resetPasswordForEmail(email, {
      redirectTo: `${origin}/auth/callback?next=${encodeURIComponent('/reset')}`,
    });
    // Said the same whether or not the address has an account.
    if (error && !/not found/i.test(error.message)) {
      console.warn(`email-link: Supabase refused the reset link (${error.message})`);
      return NextResponse.json({ error: error.message }, { status: 400 });
    }
    return NextResponse.json({ ok: true });
  }

  const emailRedirectTo = `${origin}/auth/callback?next=${encodeURIComponent(safeNextPath(parsed.data.next ?? null))}`;
  let { error } = await supabase.auth.signInWithOtp({ email, options: { emailRedirectTo } });
  if (error && signupsOff(error.message)) {
    const made = await adminClient().auth.admin.createUser({ email });
    if (made.error && !/already/i.test(made.error.message)) return NextResponse.json({ error: made.error.message }, { status: 400 });
    ({ error } = await supabase.auth.signInWithOtp({ email, options: { emailRedirectTo, shouldCreateUser: false } }));
  }
  if (error) {
    console.warn(`email-link: Supabase refused the sign-in link (${error.message})`);
    return NextResponse.json({ error: error.message }, { status: 400 });
  }
  return NextResponse.json({ ok: true });
}
