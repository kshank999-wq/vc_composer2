import type { SupabaseClient } from '@supabase/supabase-js';
import {
  downloadGate,
  licenseSchema,
  PLATFORM_LABEL,
  type License,
  type Platform,
  type PublicChannel,
} from '@/domain';
import { SITE_NAME } from './site';

/**
 * Issuing an installer download.
 *
 * The installer is never a permanent public URL. Every request re-checks the
 * caller and mints a short-lived signed URL, so a link that leaks is useless
 * within minutes and a revoked licence stops working immediately.
 *
 * Who may download what is `downloadGate` in the domain: stable needs a live
 * licence covering the platform; the free preview needs only a signed-in
 * account. This module reads the rows the gate needs and signs the URL. It
 * takes its client as an argument so the sequencing is testable without
 * Supabase.
 */

export interface DownloadRequest {
  platform: Platform;
  channel: PublicChannel;
  /** The signed-in account, or null. */
  userId: string | null;
}

export interface DownloadDeps {
  client: SupabaseClient;
  bucket: string;
  ttlSeconds: number;
  now?: Date;
}

export type DownloadAnswer =
  | {
      status: 200;
      body: {
        url: string;
        version: string;
        sha256: string;
        minimumOsVersion: string;
        expiresInSeconds: number;
      };
    }
  | { status: 401 | 403 | 404 | 500; body: { error: string } };

const LICENSE_COLUMNS =
  'id, user_id, order_id, serial, status, entitled_platforms, max_activations, expires_at, grant_kind, created_at, updated_at';

const licenseFromRow = (row: Record<string, unknown>): License | null => {
  const parsed = licenseSchema.safeParse({
    id: row['id'],
    userId: row['user_id'],
    orderId: row['order_id'],
    serial: row['serial'],
    status: row['status'],
    entitledPlatforms: row['entitled_platforms'],
    maxActivations: row['max_activations'],
    expiresAt: row['expires_at'] ?? null,
    grant: row['grant_kind'] ?? undefined,
    createdAt: row['created_at'],
    updatedAt: row['updated_at'],
  });
  return parsed.success ? parsed.data : null;
};

export const issueDownload = async (request: DownloadRequest, deps: DownloadDeps): Promise<DownloadAnswer> => {
  const { client } = deps;
  const platformName = PLATFORM_LABEL[request.platform];

  if (!request.userId) {
    return {
      status: 401,
      body: {
        error:
          request.channel === 'preview'
            ? `Sign in to download the ${SITE_NAME} preview`
            : `Sign in to download ${SITE_NAME}`,
      },
    };
  }

  // The preview needs no licence, so the licence rows are not even read for it.
  let licenses: License[] = [];
  if (request.channel === 'stable') {
    const { data: rows, error } = await client
      .from('licenses')
      .select(LICENSE_COLUMNS)
      .eq('user_id', request.userId)
      .eq('status', 'active');
    if (error) return { status: 500, body: { error: 'Could not read your licences' } };
    licenses = ((rows ?? []) as Record<string, unknown>[])
      .map(licenseFromRow)
      .filter((one): one is License => one !== null);
  }

  const gate = downloadGate({
    channel: request.channel,
    platform: request.platform,
    signedIn: true,
    licenses,
    ...(deps.now ? { now: deps.now } : {}),
  });
  if (!gate.allowed) {
    return {
      status: 403,
      body: { error: `Your account does not have a ${platformName} download. Subscribe, or try the free preview.` },
    };
  }

  const { data: build, error: buildError } = await client
    .from('release_builds')
    .select('version, artifact_key, sha256, minimum_os_version')
    .eq('platform', request.platform)
    .eq('channel', request.channel)
    .eq('active', true)
    .maybeSingle();
  if (buildError) return { status: 500, body: { error: 'Could not read the release catalogue' } };
  if (!build) {
    return {
      status: 404,
      body: {
        error:
          request.channel === 'preview'
            ? `No preview build for ${platformName} is available right now`
            : `No published build for ${platformName} yet`,
      },
    };
  }

  const { data: signed, error: signError } = await client.storage
    .from(deps.bucket)
    .createSignedUrl(build.artifact_key as string, deps.ttlSeconds);
  if (signError || !signed) return { status: 500, body: { error: 'Could not prepare the download' } };

  return {
    status: 200,
    body: {
      url: signed.signedUrl,
      version: build.version as string,
      sha256: (build.sha256 as string) ?? '',
      minimumOsVersion: (build.minimum_os_version as string) ?? '',
      expiresInSeconds: deps.ttlSeconds,
    },
  };
};
