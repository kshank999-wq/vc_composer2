import { z } from 'zod';

/**
 * Collaboration (Main Spec §15): shared projects, sold as its own
 * subscription.
 *
 * - **Who pays**: the project owner's collaboration plan carries a number of
 *   editor seats. People who write music in a project (editors and
 *   arrangers) take a seat; reviewers (the director, the music supervisor)
 *   and view-only people join free.
 * - **How it syncs**: push and pull with merge. Everyone works on their own
 *   copy; a push names the revision it was made from and is refused if
 *   somebody pushed since (optimistic concurrency) - the desktop then pulls,
 *   merges (asking only about real conflicts) and pushes again.
 * - **Roles**: the spec's five, project-wide, with track locks so two people
 *   do not write the same part at once.
 *
 * Pure rules only; the routes and the database enforce them.
 */

export const ROLES = ['owner', 'editor', 'arranger', 'reviewer', 'viewer'] as const;
export type CollabRole = (typeof ROLES)[number];
export const roleSchema = z.enum(ROLES);

/** What each role is called, and what it may do, in words for the team page. */
export const roleWords = (role: CollabRole): { label: string; does: string } => {
  switch (role) {
    case 'owner':
      return { label: 'Owner', does: 'Everything, including inviting people and the plan' };
    case 'editor':
      return { label: 'Editor / composer', does: 'Writes and edits any part, syncs changes' };
    case 'arranger':
      return { label: 'Arranger / orchestrator', does: 'Writes and edits any part, syncs changes' };
    case 'reviewer':
      return { label: 'Reviewer / director', does: 'Opens every revision, plays it and comments' };
    case 'viewer':
      return { label: 'View only', does: 'Opens and plays revisions' };
  }
};

/** People who write music take an editor seat; reviewers and viewers are free. */
export const takesSeat = (role: CollabRole): boolean => role === 'editor' || role === 'arranger';

/** May push revisions (write the shared project). */
export const canPush = (role: CollabRole): boolean => role === 'owner' || takesSeat(role);

/** May lock a track for writing. */
export const canLock = canPush;

/** May invite, change roles and remove people. */
export const canManage = (role: CollabRole): boolean => role === 'owner';

/** May download revisions. Every member. */
export const canRead = (_role: CollabRole): boolean => true;

/** The owner role is not given by invitation. */
export const invitableRoles = ROLES.filter((r) => r !== 'owner');

export type SeatCheck = { ok: true } | { ok: false; reason: string };

/**
 * Whether the owner may give someone `role`, with `used` seats already taken
 * across their projects (each person counted once) and `seats` on their plan.
 * `alreadySeated` - that person already holds a seat in another of the
 * owner's projects, so this takes no new one.
 */
export const checkSeat = (args: {
  role: CollabRole;
  planActive: boolean;
  seats: number;
  used: number;
  alreadySeated: boolean;
}): SeatCheck => {
  if (args.role === 'owner') return { ok: false, reason: 'A project has one owner' };
  if (!takesSeat(args.role)) return { ok: true };
  if (!args.planActive) return { ok: false, reason: 'Editors and arrangers need an active collaboration plan' };
  if (args.alreadySeated) return { ok: true };
  if (args.used >= args.seats)
    return {
      ok: false,
      reason: `All ${args.seats} editor seat${args.seats === 1 ? ' is' : 's are'} in use: add a seat, or invite them as a reviewer (free)`,
    };
  return { ok: true };
};

/** The seats an owner uses: distinct people holding an editor or arranger role in any of their projects. */
export const seatsUsed = (members: ReadonlyArray<{ userId: string | null; email: string; role: CollabRole }>): number => {
  const people = new Set<string>();
  for (const m of members) if (takesSeat(m.role)) people.add(m.userId ?? `email:${m.email.toLowerCase()}`);
  return people.size;
};

/**
 * A push's answer: accepted as the next revision, or stale - somebody pushed
 * since `base` - with the head to pull and merge first.
 */
export type PushDecision = { accepted: true; revision: number } | { accepted: false; head: number };

export const decidePush = (base: number, head: number): PushDecision =>
  base === head ? { accepted: true, revision: head + 1 } : { accepted: false, head };

/** Track locks last this long unless renewed; a forgotten lock frees itself. */
export const LOCK_MINUTES = 120;

export type LockDecision = { ok: true } | { ok: false; heldBy: string; until: string };

/** A lock may be taken when nobody holds it, it has run out, or the asker already holds it. */
export const decideLock = (
  existing: { userId: string; name: string; expiresAt: string } | null,
  userId: string,
  now: Date,
): LockDecision => {
  if (!existing) return { ok: true };
  if (existing.userId === userId) return { ok: true };
  if (new Date(existing.expiresAt).getTime() <= now.getTime()) return { ok: true };
  return { ok: false, heldBy: existing.name, until: existing.expiresAt };
};

/** The largest media file a shared project takes (the desktop holds one in memory to upload it). */
export const MEDIA_MAX_BYTES = 2 * 1024 * 1024 * 1024;

/**
 * A media path as a shared project stores it: relative to the project's
 * folder, inside it. Another computer writes the file there, so nothing
 * that could climb out ("..", absolute, a drive) is accepted.
 */
export const isSafeMediaPath = (path: string): boolean => {
  if (path.length === 0 || path.length > 400) return false;
  if (path.startsWith('/') || path.includes('\\') || /^[a-zA-Z]:/.test(path) || path.includes('\0')) return false;
  return path.split('/').every((part) => part.length > 0 && part !== '.' && part !== '..');
};

const sha256Schema = z.string().regex(/^[0-9a-f]{64}$/);
export const mediaEntrySchema = z.object({
  path: z.string().refine(isSafeMediaPath, 'A media path stays inside the project folder'),
  sha256: sha256Schema,
  sizeBytes: z.number().int().min(1).max(MEDIA_MAX_BYTES),
});
export type MediaEntry = z.infer<typeof mediaEntrySchema>;

/** Which of these files the server still needs (before a push). */
export const mediaQuerySchema = z.object({
  files: z.array(z.object({ sha256: sha256Schema, sizeBytes: z.number().int().min(1).max(MEDIA_MAX_BYTES) })).min(1).max(1000),
});

export const projectNameSchema = z.string().trim().min(1, 'Name the project').max(200);
export const pushSchema = z.object({
  base: z.number().int().min(0),
  message: z.string().max(500).default(''),
  sizeBytes: z.number().int().min(1).max(512 * 1024 * 1024),
  sha256: z.string().regex(/^[0-9a-f]{64}$/),
  site: z.number().int().min(1).max(0xffffff),
  path: z.string().regex(/^[0-9a-f-]{36}\/[0-9a-f-]{36}\.vcc2$/),
  media: z
    .array(mediaEntrySchema)
    .max(5000)
    .default([])
    .refine((m) => new Set(m.map((e) => e.path)).size === m.length, 'Each media path once'),
});
export const inviteSchema = z.object({
  email: z.string().trim().toLowerCase().email().max(254),
  role: z.enum(['editor', 'arranger', 'reviewer', 'viewer']),
});
