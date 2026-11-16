/**
 * Whether this account has a password, and what the account page says about it.
 *
 * Supabase writes a password hash into `auth.users` for **every** account at
 * signup, including one that has only ever used an emailed link, so that
 * column cannot answer the question. What can is the site's own record of the
 * act: `profiles.password_set_at`, stamped by a trigger whenever the password
 * actually changes (migration 0001).
 *
 * **No record is not proof of no password**, so the wording says "no password
 * set" as a statement about what we recorded, and the note offers rather than
 * warns.
 */

export type PasswordStanding = 'set' | 'unrecorded';

export const passwordStanding = (passwordSetAt: string | null | undefined): PasswordStanding =>
  passwordSetAt ? 'set' : 'unrecorded';

export const PASSWORD_STANDING_WORDS: Record<PasswordStanding, string> = {
  set: 'Password set',
  unrecorded: 'No password set',
};

export const describePasswordStanding = (standing: PasswordStanding): string =>
  standing === 'set'
    ? 'You can sign in with your email and password on any device, including from the VC Composer 2 app.'
    : 'You sign in with an emailed link. Set a password to sign in with your email and password anywhere, which is easier from the desktop app and from a phone.';
