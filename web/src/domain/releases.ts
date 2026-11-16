import { z } from 'zod';
import { canDownloadPlatform, type License, type ReleaseChannel } from './commerce';
import type { Platform } from './common';

/**
 * Who may download which channel.
 *
 * Two channels reach customers, and they are gated differently:
 *
 * - **stable** is what a licence pays for. It needs a live licence that covers
 *   the platform, exactly as it always has.
 * - **preview** is the free, time-limited build. **Any signed-in account** may
 *   download it, licence or not; signing in is what lets us tell a preview
 *   user when a newer preview is out, and keeps the link from being a public
 *   file. A preview stops saving after its own end date (the desktop build
 *   carries that date) but keeps opening and exporting.
 *
 * `beta` and `internal` are never served here: administrators reach those
 * through the admin release manager.
 */

/** The channels a customer-facing route accepts. Anything else is refused. */
export const PUBLIC_CHANNELS = ['stable', 'preview'] as const satisfies readonly ReleaseChannel[];
export const publicChannelSchema = z.enum(PUBLIC_CHANNELS);
export type PublicChannel = z.infer<typeof publicChannelSchema>;

/**
 * Read a `?channel=` value. Absent means stable, which is what every caller
 * written before the preview channel expects; anything unknown is null and
 * the route answers 400.
 */
export const parsePublicChannel = (said: string | null | undefined): PublicChannel | null => {
  if (said === null || said === undefined || said === '') return 'stable';
  const parsed = publicChannelSchema.safeParse(said);
  return parsed.success ? parsed.data : null;
};

export type DownloadGate =
  | { allowed: true }
  | { allowed: false; reason: 'sign_in' | 'no_licence' };

/**
 * Whether this caller may download this platform's build on this channel.
 *
 * Pure, so the rule is pinned by tests rather than by a route nobody can run
 * without Supabase.
 */
export const downloadGate = (input: {
  channel: PublicChannel;
  platform: Platform;
  signedIn: boolean;
  licenses: readonly License[];
  now?: Date;
}): DownloadGate => {
  if (!input.signedIn) return { allowed: false, reason: 'sign_in' };
  if (input.channel === 'preview') return { allowed: true };
  const now = input.now ?? new Date();
  return input.licenses.some((license) => canDownloadPlatform(license, input.platform, now))
    ? { allowed: true }
    : { allowed: false, reason: 'no_licence' };
};
