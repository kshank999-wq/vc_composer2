import { NextResponse, type NextRequest } from 'next/server';
import { createServerClient, type CookieOptions } from '@supabase/ssr';
import { strayAuthRedirect } from '@/lib/auth-redirect';

/**
 * Refreshes the Supabase session cookie on navigation, so a signed-in customer
 * is not bounced to sign-in mid-download, and sets the content security policy.
 *
 * The policy is nonce-based rather than `unsafe-inline`. This site has no
 * external scripts besides the sign-in page's Turnstile box (loaded from the
 * bundle, so `strict-dynamic` trusts it) and no third-party embeds, so the
 * strict version costs nothing and is worth having on pages where somebody
 * signs in and pays. Next reads the nonce back off the request header and
 * stamps it onto its own inline scripts.
 *
 * The trade: a page carrying a per-request nonce cannot be statically cached,
 * so every page renders per request (see the root layout).
 */
const contentSecurityPolicy = (nonce: string): string => {
  const supabase = process.env.NEXT_PUBLIC_SUPABASE_URL ?? '';
  // The browser client talks to Supabase over https and, for realtime, wss.
  const supabaseSocket = supabase.replace(/^https:/, 'wss:');

  return [
    "default-src 'self'",
    // `strict-dynamic` lets Next's bootstrap load its own chunks while still
    // refusing anything an injection introduces.
    `script-src 'self' 'nonce-${nonce}' 'strict-dynamic' 'unsafe-eval'`,
    // React and Next set inline style attributes; there is no nonce for those.
    "style-src 'self' 'unsafe-inline'",
    "img-src 'self' data: blob:",
    "font-src 'self' data:",
    `connect-src 'self' ${supabase} ${supabaseSocket}`.trim(),
    // The sign-in page's CAPTCHA box is Cloudflare's frame.
    'frame-src https://challenges.cloudflare.com',
    "form-action 'self'",
    "frame-ancestors 'none'",
    "base-uri 'self'",
    "object-src 'none'",
    'upgrade-insecure-requests',
  ].join('; ');
};

export async function middleware(request: NextRequest) {
  // A sign-in code that landed on the wrong page (see auth-redirect.ts).
  // Decided first: a redirect renders nothing, so it needs no policy.
  const stray = strayAuthRedirect(request.nextUrl);
  if (stray) return NextResponse.redirect(stray);

  const nonce = Buffer.from(crypto.randomUUID()).toString('base64');
  const policy = contentSecurityPolicy(nonce);

  const requestHeaders = new Headers(request.headers);
  requestHeaders.set('x-nonce', nonce);
  // Next reads the nonce from this header to stamp its own inline scripts.
  requestHeaders.set('content-security-policy', policy);

  const response = NextResponse.next({ request: { headers: requestHeaders } });
  response.headers.set('content-security-policy', policy);

  const url = process.env.NEXT_PUBLIC_SUPABASE_URL;
  const anonKey = process.env.NEXT_PUBLIC_SUPABASE_ANON_KEY;
  // A deployment without Supabase still serves its pages; there is simply no
  // session to refresh.
  if (!url || !anonKey) return response;

  const supabase = createServerClient(url, anonKey, {
    cookies: {
      get: (name: string) => request.cookies.get(name)?.value,
      set: (name: string, value: string, options: CookieOptions) => {
        response.cookies.set({ name, value, ...options });
      },
      remove: (name: string, options: CookieOptions) => {
        response.cookies.set({ name, value: '', ...options });
      },
    },
  });

  try {
    await supabase.auth.getUser();
  } catch {
    // An unreachable auth server must not take the page down with it.
  }
  return response;
}

export const config = {
  // Everything except static assets and the Stripe webhook, which
  // authenticates with a signature rather than a session.
  matcher: ['/((?!_next/static|_next/image|favicon.ico|icon.png|apple-icon.png|api/stripe).*)'],
};
