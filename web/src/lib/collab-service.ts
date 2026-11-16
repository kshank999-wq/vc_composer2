import type { SupabaseClient } from '@supabase/supabase-js';
import {
  canLock,
  canManage,
  canPush,
  checkSeat,
  type CollabRole,
  decideLock,
  LOCK_MINUTES,
  seatsUsed,
  takesSeat,
} from '@/domain';

/**
 * Collaboration on the server (Main Spec §15). Every write here is made with
 * the service role after the caller's role was checked against the rules in
 * src/domain/collaboration.ts; members only ever read through row security.
 *
 * Errors come back as an `Answer` with an HTTP status and words for the
 * person, never as a thrown database message.
 */

export const COLLAB_BUCKET = 'collab';
/** How long a signed revision link lasts: long enough for a large project on a slow line. */
const LINK_SECONDS = 15 * 60;

export type Answer<T> = { ok: true; value: T } | { ok: false; status: number; error: string };
const fail = (status: number, error: string): { ok: false; status: number; error: string } => ({ ok: false, status, error });

export interface ProjectSummary {
  id: string;
  name: string;
  role: CollabRole;
  head: number;
  ownerName: string;
  updatedAt: string;
}

/** The caller's role in a project, or null when they are not an accepted member. */
export const roleIn = async (db: SupabaseClient, projectId: string, userId: string): Promise<CollabRole | null> => {
  const { data } = await db
    .from('collab_members')
    .select('role')
    .eq('project_id', projectId)
    .eq('user_id', userId)
    .not('accepted_at', 'is', null)
    .maybeSingle();
  return (data?.role as CollabRole | undefined) ?? null;
};

const record = async (db: SupabaseClient, projectId: string, userId: string | null, kind: string, detail: object = {}) => {
  await db.from('collab_events').insert({ project_id: projectId, user_id: userId, kind, detail });
};

const planOf = async (db: SupabaseClient, ownerId: string) => {
  const { data } = await db.from('collab_plans').select('editor_seats,status').eq('owner_id', ownerId).maybeSingle();
  return { seats: Number(data?.editor_seats ?? 0), active: data?.status === 'active' };
};

// --- Projects -------------------------------------------------------------------

export const listProjects = async (db: SupabaseClient, userId: string): Promise<ProjectSummary[]> => {
  const { data } = await db
    .from('collab_members')
    .select('role, collab_projects!inner(id,name,head_revision,updated_at,archived_at,owner:profiles!collab_projects_owner_id_fkey(display_name,email))')
    .eq('user_id', userId)
    .not('accepted_at', 'is', null);
  return (data ?? [])
    .map((row) => {
      const p = row.collab_projects as unknown as {
        id: string;
        name: string;
        head_revision: number;
        updated_at: string;
        archived_at: string | null;
        owner: { display_name: string; email: string | null } | null;
      };
      return {
        id: p.id,
        name: p.name,
        role: row.role as CollabRole,
        head: p.head_revision,
        ownerName: p.owner?.display_name || p.owner?.email || '',
        updatedAt: p.updated_at,
        archived: p.archived_at !== null,
      };
    })
    .filter((p) => !p.archived)
    .map(({ archived: _a, ...p }) => p)
    .sort((a, b) => b.updatedAt.localeCompare(a.updatedAt));
};

/** Sharing a project: anyone signed in may own one; inviting editors needs the plan. */
export const createProject = async (db: SupabaseClient, userId: string, name: string): Promise<Answer<{ id: string }>> => {
  const { data: profile } = await db.from('profiles').select('email').eq('id', userId).maybeSingle();
  const { data, error } = await db.from('collab_projects').insert({ owner_id: userId, name }).select('id').single();
  if (error || !data) return fail(500, 'The project could not be shared. Try again.');
  await db.from('collab_members').insert({
    project_id: data.id,
    user_id: userId,
    invited_email: profile?.email ?? '',
    role: 'owner',
    invite_token: null,
    accepted_at: new Date().toISOString(),
  });
  await record(db, data.id, userId, 'created', { name });
  return { ok: true, value: { id: data.id as string } };
};

export const projectDetail = async (db: SupabaseClient, projectId: string, userId: string) => {
  const role = await roleIn(db, projectId, userId);
  if (!role) return fail(404, 'No such shared project, or you are not in it');
  const [{ data: project }, { data: members }, { data: revisions }, { data: locks }] = await Promise.all([
    db.from('collab_projects').select('id,name,head_revision,owner_id,updated_at').eq('id', projectId).single(),
    db
      .from('collab_members')
      .select('id,role,invited_email,accepted_at,user_id,profile:profiles!collab_members_user_id_fkey(display_name)')
      .eq('project_id', projectId)
      .order('created_at'),
    db
      .from('collab_revisions')
      .select('number,message,created_at,size_bytes,author:profiles!collab_revisions_author_id_fkey(display_name)')
      .eq('project_id', projectId)
      .order('number', { ascending: false })
      .limit(50),
    db
      .from('collab_locks')
      .select('track_id,expires_at,user_id,holder:profiles!collab_locks_user_id_fkey(display_name)')
      .eq('project_id', projectId)
      .gt('expires_at', new Date().toISOString()),
  ]);
  return { ok: true as const, value: { role, me: userId, project, members: members ?? [], revisions: revisions ?? [], locks: locks ?? [] } };
};

// --- Revisions: push and pull ---------------------------------------------------------------

/** A place to upload the next revision's file (the push follows once it is there). */
export const uploadSlot = async (
  db: SupabaseClient,
  projectId: string,
  userId: string,
): Promise<Answer<{ path: string; url: string; token: string; head: number }>> => {
  const role = await roleIn(db, projectId, userId);
  if (!role) return fail(404, 'No such shared project, or you are not in it');
  if (!canPush(role)) return fail(403, 'Reviewers and viewers open revisions; only editors, arrangers and the owner sync changes');
  const { data: project } = await db.from('collab_projects').select('head_revision').eq('id', projectId).single();
  const path = `${projectId}/${crypto.randomUUID()}.vcc2`;
  const { data, error } = await db.storage.from(COLLAB_BUCKET).createSignedUploadUrl(path);
  if (error || !data) return fail(500, 'No upload slot right now. Try again.');
  return { ok: true, value: { path, url: data.signedUrl, token: data.token, head: Number(project?.head_revision ?? 0) } };
};

/**
 * The push: the revision uploaded at `path` becomes the next one, provided
 * nobody pushed since `base`. Stale: 409 with the head to pull and merge.
 */
export const pushRevision = async (
  db: SupabaseClient,
  projectId: string,
  userId: string,
  push: { base: number; message: string; sizeBytes: number; sha256: string; site: number; path: string },
): Promise<Answer<{ revision: number }> | { ok: false; status: 409; error: string; head: number }> => {
  const role = await roleIn(db, projectId, userId);
  if (!role) return fail(404, 'No such shared project, or you are not in it');
  if (!canPush(role)) return fail(403, 'Only editors, arrangers and the owner sync changes');
  if (!push.path.startsWith(`${projectId}/`)) return fail(400, 'That file is not this project\'s');
  const { data: revision, error } = await db.rpc('collab_commit_revision', {
    p_project: projectId,
    p_base: push.base,
    p_author: userId,
    p_message: push.message,
    p_path: push.path,
    p_size: push.sizeBytes,
    p_sha256: push.sha256,
    p_site: push.site,
  });
  if (error) return fail(500, 'The change could not be saved. Try again.');
  if (revision === null || revision === undefined) {
    const { data: project } = await db.from('collab_projects').select('head_revision').eq('id', projectId).single();
    return {
      ok: false,
      status: 409,
      error: 'Somebody synced since you last did: get their changes first (VC Composer merges them), then sync again',
      head: Number(project?.head_revision ?? 0),
    };
  }
  return { ok: true, value: { revision: Number(revision) } };
};

/** A link to download revision `number` (every member may). */
export const revisionLink = async (
  db: SupabaseClient,
  projectId: string,
  userId: string,
  number: number,
): Promise<Answer<{ url: string; sha256: string; sizeBytes: number; number: number }>> => {
  const role = await roleIn(db, projectId, userId);
  if (!role) return fail(404, 'No such shared project, or you are not in it');
  const { data: rev } = await db
    .from('collab_revisions')
    .select('storage_path,sha256,size_bytes,number')
    .eq('project_id', projectId)
    .eq('number', number)
    .maybeSingle();
  if (!rev) return fail(404, `There is no revision ${number}`);
  const { data, error } = await db.storage.from(COLLAB_BUCKET).createSignedUrl(rev.storage_path as string, LINK_SECONDS);
  if (error || !data) return fail(500, 'No download link right now. Try again.');
  return { ok: true, value: { url: data.signedUrl, sha256: rev.sha256 as string, sizeBytes: Number(rev.size_bytes), number } };
};

// --- People -------------------------------------------------------------------------

/** Invites someone by email. Editors and arrangers need a free seat on the owner's plan. */
export const invite = async (
  db: SupabaseClient,
  projectId: string,
  userId: string,
  email: string,
  role: CollabRole,
): Promise<Answer<{ token: string; projectName: string }>> => {
  const mine = await roleIn(db, projectId, userId);
  if (!mine) return fail(404, 'No such shared project, or you are not in it');
  if (!canManage(mine)) return fail(403, 'Only the owner invites people');
  const { data: project } = await db.from('collab_projects').select('owner_id,name').eq('id', projectId).single();
  if (!project) return fail(404, 'No such shared project');
  if (takesSeat(role)) {
    const plan = await planOf(db, project.owner_id as string);
    const { data: seated } = await db
      .from('collab_members')
      .select('user_id, invited_email, role, collab_projects!inner(owner_id)')
      .eq('collab_projects.owner_id', project.owner_id)
      .in('role', ['editor', 'arranger']);
    const people = (seated ?? []).map((m) => ({ userId: m.user_id as string | null, email: m.invited_email as string, role: m.role as CollabRole }));
    const already = people.some((p) => p.email.toLowerCase() === email.toLowerCase());
    const check = checkSeat({ role, planActive: plan.active, seats: plan.seats, used: seatsUsed(people), alreadySeated: already });
    if (!check.ok) return fail(402, check.reason);
  }
  const { data, error } = await db
    .from('collab_members')
    .insert({ project_id: projectId, invited_email: email, role, invited_by: userId })
    .select('invite_token')
    .single();
  if (error?.code === '23505') return fail(409, 'That person is already in this project or invited');
  if (error || !data) return fail(500, 'The invitation could not be made. Try again.');
  await record(db, projectId, userId, 'invited', { email, role });
  return { ok: true, value: { token: data.invite_token as string, projectName: project.name as string } };
};

/** Accepting an invitation: binds it to the signed-in account. */
export const acceptInvite = async (db: SupabaseClient, token: string, userId: string): Promise<Answer<{ projectId: string }>> => {
  const { data: m } = await db.from('collab_members').select('id,project_id,accepted_at').eq('invite_token', token).maybeSingle();
  if (!m) return fail(404, 'That invitation is no longer valid. Ask the project owner for a new one.');
  const { error } = await db
    .from('collab_members')
    .update({ user_id: userId, accepted_at: new Date().toISOString(), invite_token: null, updated_at: new Date().toISOString() })
    .eq('id', m.id);
  if (error?.code === '23505') return fail(409, 'You are already in this project');
  if (error) return fail(500, 'The invitation could not be accepted. Try again.');
  await record(db, m.project_id as string, userId, 'joined');
  return { ok: true, value: { projectId: m.project_id as string } };
};

export const removeMember = async (db: SupabaseClient, projectId: string, userId: string, memberId: string): Promise<Answer<null>> => {
  const mine = await roleIn(db, projectId, userId);
  if (!mine || !canManage(mine)) return fail(403, 'Only the owner removes people');
  const { data: m } = await db.from('collab_members').select('role,invited_email').eq('id', memberId).eq('project_id', projectId).maybeSingle();
  if (!m) return fail(404, 'No such member');
  if (m.role === 'owner') return fail(400, 'The owner cannot be removed');
  await db.from('collab_members').delete().eq('id', memberId);
  await record(db, projectId, userId, 'removed', { email: m.invited_email });
  return { ok: true, value: null };
};

// --- Track locks --------------------------------------------------------------------------

export const lockTrack = async (
  db: SupabaseClient,
  projectId: string,
  userId: string,
  trackId: string,
): Promise<Answer<{ until: string }>> => {
  const role = await roleIn(db, projectId, userId);
  if (!role) return fail(404, 'No such shared project, or you are not in it');
  if (!canLock(role)) return fail(403, 'Only editors, arrangers and the owner lock parts');
  const { data: existing } = await db
    .from('collab_locks')
    .select('user_id,expires_at,holder:profiles!collab_locks_user_id_fkey(display_name)')
    .eq('project_id', projectId)
    .eq('track_id', trackId)
    .maybeSingle();
  const holder = existing
    ? {
        userId: existing.user_id as string,
        name: ((existing.holder as unknown as { display_name?: string } | null)?.display_name || 'someone') as string,
        expiresAt: existing.expires_at as string,
      }
    : null;
  const decision = decideLock(holder, userId, new Date());
  if (!decision.ok) return fail(409, `${decision.heldBy} is writing this part (locked until ${decision.until})`);
  const until = new Date(Date.now() + LOCK_MINUTES * 60_000).toISOString();
  const { error } = await db
    .from('collab_locks')
    .upsert({ project_id: projectId, track_id: trackId, user_id: userId, expires_at: until }, { onConflict: 'project_id,track_id' });
  if (error) return fail(500, 'The part could not be locked. Try again.');
  await record(db, projectId, userId, 'locked', { track: trackId });
  return { ok: true, value: { until } };
};

export const unlockTrack = async (db: SupabaseClient, projectId: string, userId: string, trackId: string): Promise<Answer<null>> => {
  const role = await roleIn(db, projectId, userId);
  if (!role) return fail(404, 'No such shared project, or you are not in it');
  // Your own lock, or any lock when you own the project.
  let q = db.from('collab_locks').delete().eq('project_id', projectId).eq('track_id', trackId);
  if (!canManage(role)) q = q.eq('user_id', userId);
  await q;
  await record(db, projectId, userId, 'unlocked', { track: trackId });
  return { ok: true, value: null };
};
