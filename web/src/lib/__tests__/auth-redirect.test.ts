import { describe, expect, it } from 'vitest';
import { strayAuthRedirect } from '../auth-redirect';

const at = (path: string) => new URL(path, 'https://vc-composer.com');

describe('strayAuthRedirect', () => {
  it('forwards a code that landed on the home page to the callback, bound for the account', () => {
    // The real first sign-in: Supabase fell back to its Site URL.
    const target = strayAuthRedirect(at('/?code=a4df99a6-b75d-4c5c-8265-1fc5d7d9fb2c'));
    expect(target?.pathname).toBe('/auth/callback');
    expect(target?.searchParams.get('code')).toBe('a4df99a6-b75d-4c5c-8265-1fc5d7d9fb2c');
    expect(target?.searchParams.get('next')).toBe('/account');
  });

  it('keeps the page the code arrived on as the continuation, other params intact', () => {
    const target = strayAuthRedirect(at('/download?code=abc&cancelled=1'));
    expect(target?.searchParams.get('next')).toBe('/download?cancelled=1');
  });

  it('stays on the same origin whatever the code contains', () => {
    const target = strayAuthRedirect(at('/?code=https://evil.example'));
    expect(target?.origin).toBe('https://vc-composer.com');
    expect(target?.pathname).toBe('/auth/callback');
  });

  it('leaves the callback itself alone', () => {
    expect(strayAuthRedirect(at('/auth/callback?code=abc&next=/account'))).toBeNull();
  });

  it('leaves API routes alone', () => {
    expect(strayAuthRedirect(at('/api/checkout?code=abc'))).toBeNull();
  });

  it('sends an expired-link error to sign-in', () => {
    const target = strayAuthRedirect(at('/?error=access_denied&error_code=otp_expired&error_description=Email+link+is+invalid'));
    expect(target?.pathname).toBe('/signin');
    expect(target?.searchParams.get('error')).toBe('link_expired');
  });

  it('does nothing to an ordinary request', () => {
    expect(strayAuthRedirect(at('/'))).toBeNull();
    expect(strayAuthRedirect(at('/download?cancelled=1'))).toBeNull();
    expect(strayAuthRedirect(at('/purchase/complete?session_id=cs_123'))).toBeNull();
  });
});

describe('the place to continue to after sign-in', () => {
  it('keeps a path on this site and refuses anything that would leave it', async () => {
    const { safeNextPath } = await import('../auth-redirect');
    expect(safeNextPath('/download#preview')).toBe('/download#preview');
    expect(safeNextPath('/admin/orders?status=paid')).toBe('/admin/orders?status=paid');
    expect(safeNextPath(null)).toBe('/account');
    expect(safeNextPath('https://evil.example/')).toBe('/account');
    expect(safeNextPath('//evil.example')).toBe('/account');
    expect(safeNextPath('account')).toBe('/account');
  });
});
