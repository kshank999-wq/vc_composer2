/**
 * Where to send a request that has landed on the wrong page carrying
 * Supabase auth parameters.
 *
 * A sign-in link points wherever Supabase's Site URL says. The site asks for
 * `/auth/callback`, but if that request is refused — the allow-list is stale,
 * the link was sent from the Supabase dashboard, the config was saved after
 * the email went out — Supabase falls back to the bare Site URL and the user
 * arrives at `/?code=…`. Nothing on the home page exchanges a code, so they
 * are left signed out looking at the landing page, with a link that has now
 * been spent. This happened on the first real sign-in.
 *
 * So: a `code` anywhere but the callback is forwarded to the callback, with
 * the page it arrived on as the place to continue to afterwards. Supabase's
 * error parameters (an expired link) go to the sign-in page rather than
 * decorating whatever page they fell on.
 *
 * Pure so it can be tested without a request; the middleware calls it.
 */
export const CALLBACK_PATH = '/auth/callback';

export const strayAuthRedirect = (url: URL): URL | null => {
  const { pathname, searchParams } = url;

  // The callback is where these belong; API routes never render a page.
  if (pathname === CALLBACK_PATH || pathname.startsWith('/api/')) return null;

  const code = searchParams.get('code');
  if (code) {
    const remaining = new URLSearchParams(searchParams);
    remaining.delete('code');
    const suffix = remaining.toString();
    const next = suffix.length > 0 ? `${pathname}?${suffix}` : pathname;

    const target = new URL(CALLBACK_PATH, url.origin);
    target.searchParams.set('code', code);
    // The landing page is a poor place to end up after signing in; the
    // account page is what the link was for.
    target.searchParams.set('next', pathname === '/' ? '/account' : next);
    return target;
  }

  // Supabase reports a dead link as `?error=access_denied&error_code=otp_expired`.
  if (searchParams.has('error_code') || searchParams.get('error') === 'access_denied') {
    return new URL('/signin?error=link_expired', url.origin);
  }

  return null;
};

/**
 * A `next` parameter is a place on this site to continue to after sign-in,
 * and nothing else: an absolute URL or a protocol-relative one would send
 * the customer off-site with a fresh session. Anything doubtful becomes the
 * account page.
 */
export const safeNextPath = (value: string | null | undefined, fallback = '/account'): string =>
  value && value.startsWith('/') && !value.startsWith('//') && !value.includes('\\') ? value : fallback;
