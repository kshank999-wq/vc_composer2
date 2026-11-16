import type { SupabaseClient } from '@supabase/supabase-js';
import { describe, expect, it } from 'vitest';
import { issueDownload } from '../downloads';

/**
 * The download gate, end to end through the row reads.
 *
 * Stable is licence-gated exactly as it always was. The free preview channel
 * serves the active preview build to any signed-in account, licence or not,
 * and never serves anything to a stranger.
 */

type Row = Record<string, unknown>;

interface Catalogue {
  licenses: Row[];
  release_builds: Row[];
}

/** Just enough of the client: filtered selects, `maybeSingle`, and signing. */
const stubClient = (catalogue: Catalogue) => {
  const reads: string[] = [];
  const signed: string[] = [];
  const query = (table: keyof Catalogue) => {
    const filters: Array<[string, unknown]> = [];
    const rows = () => catalogue[table].filter((row) => filters.every(([column, value]) => row[column] === value));
    const builder = {
      select: () => builder,
      eq: (column: string, value: unknown) => {
        filters.push([column, value]);
        return builder;
      },
      maybeSingle: async () => ({ data: rows()[0] ?? null, error: null }),
      then: <T>(resolve: (value: { data: Row[]; error: null }) => T) => resolve({ data: rows(), error: null }),
    };
    return builder;
  };
  const client = {
    from: (table: keyof Catalogue) => {
      reads.push(table);
      return query(table);
    },
    storage: {
      from: (bucket: string) => ({
        createSignedUrl: async (key: string, ttl: number) => {
          signed.push(`${bucket}/${key}`);
          return { data: { signedUrl: `https://storage.test/${bucket}/${key}?ttl=${ttl}` }, error: null };
        },
      }),
    },
  } as unknown as SupabaseClient;
  return { client, reads, signed };
};

const NOW = new Date('2026-11-16T12:00:00.000Z');
const USER = '00000000-0000-4000-8000-000000000002';

const licence = (over: Row = {}): Row => ({
  id: '00000000-0000-4000-8000-000000000001',
  user_id: USER,
  order_id: '00000000-0000-4000-8000-000000000003',
  serial: 'VCC-AAAAA-BBBBB-CCCCC-DDDDD',
  status: 'active',
  entitled_platforms: ['windows', 'macos'],
  max_activations: 2,
  expires_at: '2026-12-16T00:00:00.000Z',
  grant_kind: 'purchase',
  created_at: '2026-10-01T00:00:00.000Z',
  updated_at: '2026-10-01T00:00:00.000Z',
  ...over,
});

const builds: Row[] = [
  {
    platform: 'windows',
    channel: 'stable',
    active: true,
    version: '2.0.0',
    artifact_key: 'windows/2.0.0/VC-Composer-2-Setup.exe',
    sha256: 'aa',
    minimum_os_version: '10.0.17763',
  },
  {
    platform: 'windows',
    channel: 'preview',
    active: true,
    version: '0.1.0',
    artifact_key: 'windows/0.1.0/VC-Composer-2-Preview-Setup.exe',
    sha256: 'bb',
    minimum_os_version: '10.0.17763',
  },
  {
    platform: 'windows',
    channel: 'preview',
    active: false,
    version: '0.0.9',
    artifact_key: 'windows/0.0.9/old.exe',
    sha256: 'cc',
    minimum_os_version: '',
  },
  {
    platform: 'macos',
    channel: 'beta',
    active: true,
    version: '2.1.0-beta.1',
    artifact_key: 'macos/2.1.0-beta.1/beta.dmg',
    sha256: 'dd',
    minimum_os_version: '11.0',
  },
];

const deps = (client: SupabaseClient) => ({ client, bucket: 'releases', ttlSeconds: 900, now: NOW });

describe('the free preview channel', () => {
  it('serves the active preview build to a signed-in account with no licence', async () => {
    const stub = stubClient({ licenses: [], release_builds: builds });
    const answer = await issueDownload({ platform: 'windows', channel: 'preview', userId: USER }, deps(stub.client));
    expect(answer).toEqual({
      status: 200,
      body: {
        url: 'https://storage.test/releases/windows/0.1.0/VC-Composer-2-Preview-Setup.exe?ttl=900',
        version: '0.1.0',
        sha256: 'bb',
        minimumOsVersion: '10.0.17763',
        expiresInSeconds: 900,
      },
    });
    // No licence is consulted for a preview.
    expect(stub.reads).not.toContain('licenses');
  });

  it('serves it to an account whose licence was revoked', async () => {
    const stub = stubClient({ licenses: [licence({ status: 'revoked' })], release_builds: builds });
    const answer = await issueDownload({ platform: 'windows', channel: 'preview', userId: USER }, deps(stub.client));
    expect(answer.status).toBe(200);
  });

  it('refuses a stranger before reading anything', async () => {
    const stub = stubClient({ licenses: [], release_builds: builds });
    const answer = await issueDownload({ platform: 'windows', channel: 'preview', userId: null }, deps(stub.client));
    expect(answer.status).toBe(401);
    expect(stub.reads).toEqual([]);
    expect(stub.signed).toEqual([]);
  });

  it('says so when no preview is live for the platform', async () => {
    const stub = stubClient({ licenses: [], release_builds: builds });
    const answer = await issueDownload({ platform: 'macos', channel: 'preview', userId: USER }, deps(stub.client));
    expect(answer.status).toBe(404);
    expect(stub.signed).toEqual([]);
  });

  it('never hands out the stable build in place of a preview', async () => {
    const stub = stubClient({ licenses: [], release_builds: builds });
    await issueDownload({ platform: 'windows', channel: 'preview', userId: USER }, deps(stub.client));
    expect(stub.signed).toEqual(['releases/windows/0.1.0/VC-Composer-2-Preview-Setup.exe']);
  });
});

describe('the stable channel', () => {
  it('serves a live licence holder', async () => {
    const stub = stubClient({ licenses: [licence()], release_builds: builds });
    const answer = await issueDownload({ platform: 'windows', channel: 'stable', userId: USER }, deps(stub.client));
    expect(answer.status).toBe(200);
    expect(stub.signed).toEqual(['releases/windows/2.0.0/VC-Composer-2-Setup.exe']);
  });

  it('refuses a signed-in account with no licence, and signs nothing', async () => {
    const stub = stubClient({ licenses: [], release_builds: builds });
    const answer = await issueDownload({ platform: 'windows', channel: 'stable', userId: USER }, deps(stub.client));
    expect(answer.status).toBe(403);
    expect(stub.signed).toEqual([]);
  });

  it('refuses a licence whose paid period has run out, even where the status still says active', async () => {
    const stub = stubClient({ licenses: [licence({ expires_at: '2026-11-01T00:00:00.000Z' })], release_builds: builds });
    const answer = await issueDownload({ platform: 'windows', channel: 'stable', userId: USER }, deps(stub.client));
    expect(answer.status).toBe(403);
  });

  it('refuses a stranger', async () => {
    const stub = stubClient({ licenses: [licence()], release_builds: builds });
    const answer = await issueDownload({ platform: 'windows', channel: 'stable', userId: null }, deps(stub.client));
    expect(answer.status).toBe(401);
  });

  it('only reads the caller’s own licences', async () => {
    const stub = stubClient({ licenses: [licence({ user_id: 'somebody-else' })], release_builds: builds });
    const answer = await issueDownload({ platform: 'windows', channel: 'stable', userId: USER }, deps(stub.client));
    expect(answer.status).toBe(403);
  });
});
