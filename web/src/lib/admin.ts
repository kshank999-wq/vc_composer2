import { adminClient, currentUser } from './supabase';

/**
 * Who may use the admin console.
 *
 * Admin is a flag on the profile, checked here on every request rather than
 * carried in a token — revoking it takes effect immediately, and there is no
 * stale claim to reason about.
 */

export interface AdminIdentity {
  userId: string;
  email: string | null;
}

export const currentAdmin = async (): Promise<AdminIdentity | null> => {
  // Any failure (no session, Supabase unreachable or not configured) answers
  // "not an administrator": the safe direction to be wrong in.
  try {
    const user = await currentUser();
    if (!user) return null;

    const { data } = await adminClient().from('profiles').select('is_admin').eq('id', user.id).maybeSingle();
    if (!data?.is_admin) return null;

    return { userId: user.id, email: user.email ?? null };
  } catch {
    return null;
  }
};

/** Throws with a message suitable for returning as a 403 body. */
export const requireAdmin = async (): Promise<AdminIdentity> => {
  const admin = await currentAdmin();
  if (!admin) throw new Error('This area is for administrators.');
  return admin;
};
