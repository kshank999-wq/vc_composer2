import type { AuthError } from '@supabase/supabase-js';

/**
 * What the two password screens say.
 *
 * There are two places a password is set — the account page, where somebody
 * who knows theirs changes it, and the reset page, where somebody who has
 * forgotten it arrives from a link — and they must agree about what a refusal
 * means. So the mapping lives here and each screen asks it, rather than each
 * translating Supabase's wording for itself and drifting the first time one of
 * them is edited.
 *
 * What does **not** live here is the one sentence that is about the screen: the
 * account page can be told the current password is wrong, and the reset page
 * cannot, because it never sends one. Each supplies its own for that case.
 */

/** Supabase's own floor is six; this says so rather than letting the server refuse. */
export const SHORTEST = 8;

/**
 * Both refusals a screen can give before asking the server, because the answer
 * to each is a keystroke away. Null means there is nothing to say yet.
 */
export const checkNewPassword = (password: string, again: string): string | null => {
  if (password.length < SHORTEST) return `Use at least ${SHORTEST} characters.`;
  if (password !== again) return 'Those two do not match.';
  return null;
};

/**
 * What to say about a refusal from the server.
 *
 * Supabase's wording is written for a developer reading a stack trace, and the
 * three failures worth naming are the three a customer can do something about.
 * Anything else is **passed through rather than paraphrased**: a guess at what
 * an unknown code meant would be worse than the server's own words.
 *
 * `credentials` is the caller's own sentence for the one case that differs
 * between the two screens.
 */
export const refusalFor = (failure: AuthError, credentials: string): string => {
  const code = failure.code ?? '';
  const said = failure.message.toLowerCase();

  if (code === 'same_password') return 'That is the password you already have.';
  if (code.startsWith('reauthentication')) {
    // The project's *Secure password change* switch. Nothing here runs the
    // nonce flow, so the honest answer is the one that works: sign in again.
    return 'For safety this account needs a fresh sign-in before the password changes. Sign out, sign back in, and try again.';
  }
  if (said.includes('current password') || said.includes('invalid login credentials')) {
    return credentials;
  }
  return failure.message;
};
