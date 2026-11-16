-- VC Composer: collaboration (Main Spec §15), its own subscription.
--
-- Shared projects with the spec's five roles, project-wide; push/pull with
-- merge, a push naming the revision it was made from (optimistic
-- concurrency: a stale push is refused and the desktop merges first); track
-- locks; an audit trail of who did what and when.
--
-- Who pays: the owner's collaboration plan carries editor seats. Editors and
-- arrangers take a seat; reviewers and view-only people join free
-- (src/domain/collaboration.ts holds the rules).
--
-- As with licences, nothing here is writable by a signed-in user: every
-- write goes through the server routes (service role) that enforce roles and
-- seats. Members read their own projects. Revision files live in the private
-- `collab` bucket with no storage policy, read and written only through
-- short-lived signed URLs the routes mint after the membership check.

create type public.collab_role as enum ('owner', 'editor', 'arranger', 'reviewer', 'viewer');

-- ---------------------------------------------------------------------------
-- The owner's plan
-- ---------------------------------------------------------------------------

create table public.collab_plans (
  owner_id uuid primary key references public.profiles (id) on delete cascade,
  -- Editor seats paid for (the Stripe subscription's quantity).
  editor_seats integer not null default 0 check (editor_seats >= 0),
  status text not null default 'active' check (status in ('active', 'past_due', 'canceled')),
  stripe_subscription_id text unique,
  current_period_end timestamptz,
  created_at timestamptz not null default now(),
  updated_at timestamptz not null default now()
);

comment on table public.collab_plans is
  'A project owner''s collaboration subscription: how many editor seats. Written only by the Stripe webhook (service role).';

-- ---------------------------------------------------------------------------
-- Shared projects
-- ---------------------------------------------------------------------------

create table public.collab_projects (
  id uuid primary key default gen_random_uuid(),
  owner_id uuid not null references public.profiles (id) on delete cascade,
  name text not null check (char_length(name) between 1 and 200),
  -- The latest revision number; 0 before the first push.
  head_revision integer not null default 0 check (head_revision >= 0),
  archived_at timestamptz,
  created_at timestamptz not null default now(),
  updated_at timestamptz not null default now()
);

create index collab_projects_owner_idx on public.collab_projects (owner_id);

create table public.collab_members (
  id uuid primary key default gen_random_uuid(),
  project_id uuid not null references public.collab_projects (id) on delete cascade,
  -- Null until the invitation is accepted.
  user_id uuid references public.profiles (id) on delete cascade,
  invited_email text not null,
  role public.collab_role not null,
  -- The secret in the invitation link. Cleared once accepted.
  invite_token uuid unique default gen_random_uuid(),
  invited_by uuid references public.profiles (id) on delete set null,
  accepted_at timestamptz,
  created_at timestamptz not null default now(),
  updated_at timestamptz not null default now()
);

create unique index collab_members_user_idx on public.collab_members (project_id, user_id) where user_id is not null;
create unique index collab_members_email_idx on public.collab_members (project_id, lower(invited_email));
create index collab_members_user_lookup_idx on public.collab_members (user_id) where user_id is not null;

-- ---------------------------------------------------------------------------
-- Revisions: the project as each push left it
-- ---------------------------------------------------------------------------

create table public.collab_revisions (
  project_id uuid not null references public.collab_projects (id) on delete cascade,
  number integer not null check (number >= 1),
  author_id uuid references public.profiles (id) on delete set null,
  message text not null default '',
  -- In the collab bucket: <project id>/<uuid>.vcc2
  storage_path text not null,
  size_bytes bigint not null check (size_bytes > 0),
  sha256 text not null check (sha256 ~ '^[0-9a-f]{64}$'),
  -- The pushing installation's id site (collision-free ids).
  site integer not null,
  created_at timestamptz not null default now(),
  primary key (project_id, number)
);

create index collab_revisions_author_idx on public.collab_revisions (author_id) where author_id is not null;

-- ---------------------------------------------------------------------------
-- Track locks: one writer per part at a time
-- ---------------------------------------------------------------------------

create table public.collab_locks (
  project_id uuid not null references public.collab_projects (id) on delete cascade,
  track_id text not null check (char_length(track_id) between 1 and 80),
  user_id uuid not null references public.profiles (id) on delete cascade,
  expires_at timestamptz not null,
  created_at timestamptz not null default now(),
  primary key (project_id, track_id)
);

create index collab_locks_user_idx on public.collab_locks (user_id);

-- ---------------------------------------------------------------------------
-- Audit: who did what, and when (spec: "maintain audit history")
-- ---------------------------------------------------------------------------

create table public.collab_events (
  id bigint generated always as identity primary key,
  project_id uuid not null references public.collab_projects (id) on delete cascade,
  user_id uuid references public.profiles (id) on delete set null,
  kind text not null check (kind in ('created', 'pushed', 'invited', 'joined', 'role_changed', 'removed', 'locked', 'unlocked', 'renamed', 'archived')),
  detail jsonb not null default '{}'::jsonb,
  created_at timestamptz not null default now()
);

create index collab_events_project_idx on public.collab_events (project_id, created_at desc);
create index collab_events_user_idx on public.collab_events (user_id) where user_id is not null;

-- ---------------------------------------------------------------------------
-- A push, atomically: the head moves only if it is where the pusher
-- started from. Service role only (the route has checked the role).
-- ---------------------------------------------------------------------------

create or replace function public.collab_commit_revision(
  p_project uuid,
  p_base integer,
  p_author uuid,
  p_message text,
  p_path text,
  p_size bigint,
  p_sha256 text,
  p_site integer
) returns integer
language plpgsql
security definer
set search_path = ''
as $$
declare
  v_next integer;
begin
  update public.collab_projects
     set head_revision = head_revision + 1, updated_at = now()
   where id = p_project and head_revision = p_base and archived_at is null
   returning head_revision into v_next;
  if v_next is null then
    -- Stale (somebody pushed since p_base) or no such project: the caller
    -- reads the head and answers 409.
    return null;
  end if;
  insert into public.collab_revisions (project_id, number, author_id, message, storage_path, size_bytes, sha256, site)
  values (p_project, v_next, p_author, p_message, p_path, p_size, p_sha256, p_site);
  insert into public.collab_events (project_id, user_id, kind, detail)
  values (p_project, p_author, 'pushed', jsonb_build_object('revision', v_next, 'message', p_message));
  return v_next;
end;
$$;

revoke all on function public.collab_commit_revision(uuid, integer, uuid, text, text, bigint, text, integer) from public, anon, authenticated;

-- Whether the signed-in user is an accepted member of a project. Security
-- definer so the policies below can ask without recursing into themselves.
create or replace function public.is_collab_member(p_project uuid)
returns boolean
language sql
stable
security definer
set search_path = ''
as $$
  select exists (
    select 1 from public.collab_members m
    where m.project_id = p_project and m.user_id = (select auth.uid()) and m.accepted_at is not null
  );
$$;

-- ---------------------------------------------------------------------------
-- Row level security: members read; nobody writes but the service role.
-- ---------------------------------------------------------------------------

alter table public.collab_plans enable row level security;
alter table public.collab_projects enable row level security;
alter table public.collab_members enable row level security;
alter table public.collab_revisions enable row level security;
alter table public.collab_locks enable row level security;
alter table public.collab_events enable row level security;

create policy "owners read their own plan" on public.collab_plans
  for select using (owner_id = (select auth.uid()));

create policy "members read their projects" on public.collab_projects
  for select using (owner_id = (select auth.uid()) or public.is_collab_member(id));

create policy "members read who else is in their projects" on public.collab_members
  for select using (public.is_collab_member(project_id)
                    or exists (select 1 from public.collab_projects p where p.id = project_id and p.owner_id = (select auth.uid())));

create policy "members read revisions" on public.collab_revisions
  for select using (public.is_collab_member(project_id)
                    or exists (select 1 from public.collab_projects p where p.id = project_id and p.owner_id = (select auth.uid())));

create policy "members read locks" on public.collab_locks
  for select using (public.is_collab_member(project_id)
                    or exists (select 1 from public.collab_projects p where p.id = project_id and p.owner_id = (select auth.uid())));

create policy "members read the history" on public.collab_events
  for select using (public.is_collab_member(project_id)
                    or exists (select 1 from public.collab_projects p where p.id = project_id and p.owner_id = (select auth.uid())));

-- ---------------------------------------------------------------------------
-- The private bucket revision files live in (no storage policy: signed URLs only)
-- ---------------------------------------------------------------------------

insert into storage.buckets (id, name, public)
values ('collab', 'collab', false)
on conflict (id) do nothing;

-- Only signed-in users ask (the policies above run as them); it answers
-- only about the caller's own membership.
revoke execute on function public.is_collab_member(uuid) from public, anon;
grant execute on function public.is_collab_member(uuid) to authenticated;
