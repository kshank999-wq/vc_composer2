import { beforeEach, describe, expect, it, vi } from 'vitest';

/**
 * The download route's own reading of the request: the platform, the
 * `?channel=` parameter and who is calling. What the gate decides is pinned in
 * `lib/__tests__/downloads.test.ts`; this pins that the route hands it the
 * right question.
 */
const issueDownload = vi.fn();
const currentUser = vi.fn();

vi.mock('@/lib/downloads', () => ({ issueDownload: (...args: unknown[]) => issueDownload(...args) }));
vi.mock('@/lib/supabase', () => ({ adminClient: () => ({}), currentUser: () => currentUser() }));
vi.mock('@/lib/env', () => ({ env: { releaseBucket: 'releases', releaseDownloadTtlSeconds: 900 } }));

const { GET } = await import('../route');
const ask = (platform: string, query = '') =>
  GET(new Request(`https://vc-composer.com/api/downloads/${platform}${query}`), { params: { platform } });

describe('GET /api/downloads/[platform]', () => {
  beforeEach(() => {
    vi.clearAllMocks();
    currentUser.mockResolvedValue({ id: 'user-1' });
    issueDownload.mockResolvedValue({ status: 200, body: { url: 'https://signed', version: '0.1.0' } });
  });

  it('asks for the stable build when no channel is named', async () => {
    expect((await ask('windows')).status).toBe(200);
    expect(issueDownload.mock.calls[0]?.[0]).toEqual({ platform: 'windows', channel: 'stable', userId: 'user-1' });
  });

  it('asks for the preview build with ?channel=preview', async () => {
    await ask('macos', '?channel=preview');
    expect(issueDownload.mock.calls[0]?.[0]).toEqual({ platform: 'macos', channel: 'preview', userId: 'user-1' });
  });

  it('passes a stranger through as nobody, for the gate to refuse', async () => {
    currentUser.mockResolvedValue(null);
    issueDownload.mockResolvedValue({ status: 401, body: { error: 'Sign in' } });
    const response = await ask('windows', '?channel=preview');
    expect(response.status).toBe(401);
    expect(issueDownload.mock.calls[0]?.[0]).toMatchObject({ userId: null });
  });

  it('refuses the staging channels and anything unknown, before asking anything', async () => {
    for (const channel of ['beta', 'internal', 'nightly']) {
      expect((await ask('windows', `?channel=${channel}`)).status).toBe(400);
    }
    expect(issueDownload).not.toHaveBeenCalled();
  });

  it('refuses a platform that does not exist', async () => {
    expect((await ask('linux')).status).toBe(404);
    expect(issueDownload).not.toHaveBeenCalled();
  });
});
