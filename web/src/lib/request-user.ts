import { adminClient, currentUser } from '@/lib/supabase';

/**
 * Who is calling: the desktop app's bearer token (a Supabase access token)
 * or, from the site, the session cookie. Null when neither identifies a user.
 */
export const requestUserId = async (request: Request): Promise<string | null> => {
  const header = request.headers.get('authorization');
  if (header?.toLowerCase().startsWith('bearer ')) {
    const { data } = await adminClient().auth.getUser(header.slice(7).trim());
    return data.user?.id ?? null;
  }
  const user = await currentUser();
  return user?.id ?? null;
};
