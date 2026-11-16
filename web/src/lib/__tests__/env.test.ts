import { afterEach, describe, expect, it, vi } from 'vitest';

/**
 * `env` reads `process.env` through getters, so each case can set the variable
 * and import the module fresh without any reset dance.
 */
const siteUrlWith = async (value: string | undefined): Promise<string> => {
  vi.resetModules();
  if (value === undefined) delete process.env['NEXT_PUBLIC_SITE_URL'];
  else process.env['NEXT_PUBLIC_SITE_URL'] = value;
  const { env } = await import('../env');
  return env.siteUrl;
};

afterEach(() => {
  delete process.env['NEXT_PUBLIC_SITE_URL'];
  vi.restoreAllMocks();
});

describe('env.siteUrl', () => {
  it('uses the production origin when nothing is set', async () => {
    await expect(siteUrlWith(undefined)).resolves.toBe('https://vc-composer.com');
  });

  it('accepts a configured origin', async () => {
    await expect(siteUrlWith('https://staging.vc-composer.com')).resolves.toBe(
      'https://staging.vc-composer.com',
    );
  });

  it('drops a trailing slash, so appended paths do not double it', async () => {
    // `${env.siteUrl}/account` is how every emailed link and Stripe redirect is
    // built; a trailing slash there produces `https://vc-composer.com//account`.
    await expect(siteUrlWith('https://vc-composer.com/')).resolves.toBe('https://vc-composer.com');
  });

  it('falls back rather than throwing when the value is not a URL', async () => {
    // The real failure this guards: the variable's *name* was pasted into the
    // value field in Vercel, and `new URL(...)` in the root layout's
    // `metadataBase` threw during "Collecting page data", failing the build.
    const warn = vi.spyOn(console, 'warn').mockImplementation(() => undefined);
    await expect(siteUrlWith('NEXT_PUBLIC_SITE_URL')).resolves.toBe('https://vc-composer.com');
    expect(warn).toHaveBeenCalledOnce();
    expect(warn.mock.calls[0]?.[0]).toContain('NEXT_PUBLIC_SITE_URL');
  });

  it('refuses a bare hostname instead of guessing a scheme', async () => {
    // Guessing would turn the mistake above into `https://NEXT_PUBLIC_SITE_URL`,
    // which parses fine and would ship a site linking to a host that does not
    // exist. Warning and using production is the safer failure.
    vi.spyOn(console, 'warn').mockImplementation(() => undefined);
    await expect(siteUrlWith('vc-composer.com')).resolves.toBe('https://vc-composer.com');
  });

  it('refuses a non-http scheme', async () => {
    vi.spyOn(console, 'warn').mockImplementation(() => undefined);
    await expect(siteUrlWith('javascript:alert(1)')).resolves.toBe('https://vc-composer.com');
  });

  it('does not throw when handed to new URL, whatever was configured', async () => {
    vi.spyOn(console, 'warn').mockImplementation(() => undefined);
    for (const value of ['NEXT_PUBLIC_SITE_URL', '', '   ', 'vc-composer.com', 'https://a.test/']) {
      const resolved = await siteUrlWith(value);
      expect(() => new URL(resolved)).not.toThrow();
    }
  });
});
