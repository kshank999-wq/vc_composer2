import { cookies, headers } from 'next/headers';
import { createServerClient, type CookieOptions } from '@supabase/ssr';
import { createClient, type SupabaseClient } from '@supabase/supabase-js';
import { env } from './env';

/**
 * Two clients, deliberately distinct.
 *
 * `serverClient()` acts as the signed-in visitor and is bound by row level
 * security. `adminClient()` uses the service role, bypasses RLS, and is the
 * only way to write orders, licences and release rows: commerce and download
 * authorisation stay server-side.
 */

/**
 * The bearer token the desktop app sent, where it sent one.
 *
 * A browser proves who it is with a cookie; the desktop app cannot, so it sends
 * its Supabase access token as `Authorization: Bearer …`. Read here so *who is
 * calling* has one answer for every route. A browser sends no Authorization
 * header unless asked to, so browser requests are unaffected.
 */
const bearerToken = (): string | null => {
  try {
    const said = headers().get('authorization') ?? '';
    const [scheme, token] = said.split(' ');
    return scheme?.toLowerCase() === 'bearer' && token ? token : null;
  } catch {
    // `headers()` is unavailable outside a request.
    return null;
  }
};

export const serverClient = () => {
  // The app's token stands in for the cookie and is bound by exactly the same
  // row level security: it *is* that person's session, not a way past it.
  const token = bearerToken();
  if (token) {
    return createClient(env.supabaseUrl, env.supabaseAnonKey, {
      auth: { persistSession: false, autoRefreshToken: false },
      global: { headers: { Authorization: `Bearer ${token}` } },
    });
  }

  const cookieStore = cookies();
  return createServerClient(env.supabaseUrl, env.supabaseAnonKey, {
    cookies: {
      get(name: string) {
        return cookieStore.get(name)?.value;
      },
      set(name: string, value: string, options: CookieOptions) {
        try {
          cookieStore.set({ name, value, ...options });
        } catch {
          // Called from a Server Component, where cookies are read-only. The
          // middleware refresh path handles rotation instead.
        }
      },
      remove(name: string, options: CookieOptions) {
        try {
          cookieStore.set({ name, value: '', ...options });
        } catch {
          // See above.
        }
      },
    },
  });
};

let cachedAdmin: SupabaseClient | null = null;

/** Service-role client. Never import this from a client component. */
export const adminClient = (): SupabaseClient => {
  if (!cachedAdmin) {
    cachedAdmin = createClient(env.supabaseUrl, env.supabaseServiceRoleKey, {
      auth: { persistSession: false, autoRefreshToken: false },
      // Never through Next's data cache: a service-role read is the catalogue,
      // licences and orders as they are now, not as they were when first read.
      global: { fetch: (input, init) => fetch(input, { ...init, cache: 'no-store' }) },
    });
  }
  return cachedAdmin;
};

export const currentUser = async () => {
  const { data } = await serverClient().auth.getUser();
  return data.user ?? null;
};

/**
 * The signed-in visitor, or null where there is none **or Supabase is not
 * configured**. For public pages (home, download) that must render on a
 * deployment without keys; account and admin pages use `currentUser`.
 */
export const currentUserOrNull = async () => {
  try {
    return await currentUser();
  } catch {
    return null;
  }
};
