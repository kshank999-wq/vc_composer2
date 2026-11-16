-- VC Composer: collaboration media (audio and picture files of a shared project).
--
-- Files are stored once per project by content: <project id>/media/<sha256>
-- in the private `collab` bucket, however many revisions use them. Each
-- revision records which file each media path in its project is (its
-- `media` manifest: [{path, sha256, sizeBytes}]), so a pull fetches only
-- what a computer does not already have.
--
-- As before, nothing here is writable by a signed-in user: the server routes
-- register a file (service role) after checking the role and that the
-- upload really is in storage.

create table public.collab_media (
  project_id uuid not null references public.collab_projects (id) on delete cascade,
  sha256 text not null check (sha256 ~ '^[0-9a-f]{64}$'),
  size_bytes bigint not null check (size_bytes > 0),
  uploaded_by uuid references public.profiles (id) on delete set null,
  created_at timestamptz not null default now(),
  primary key (project_id, sha256)
);

create index collab_media_uploaded_by_idx on public.collab_media (uploaded_by) where uploaded_by is not null;

comment on table public.collab_media is
  'A shared project''s media files, by content hash. Written only by the collaboration routes (service role).';

alter table public.collab_media enable row level security;

create policy "members read the media list" on public.collab_media
  for select using (public.is_collab_member(project_id)
                    or exists (select 1 from public.collab_projects p where p.id = project_id and p.owner_id = (select auth.uid())));

alter table public.collab_revisions add column media jsonb not null default '[]'::jsonb;

-- The push now records the revision's media manifest.
drop function public.collab_commit_revision(uuid, integer, uuid, text, text, bigint, text, integer);

create or replace function public.collab_commit_revision(
  p_project uuid,
  p_base integer,
  p_author uuid,
  p_message text,
  p_path text,
  p_size bigint,
  p_sha256 text,
  p_site integer,
  p_media jsonb
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
    return null;
  end if;
  insert into public.collab_revisions (project_id, number, author_id, message, storage_path, size_bytes, sha256, site, media)
  values (p_project, v_next, p_author, p_message, p_path, p_size, p_sha256, p_site, coalesce(p_media, '[]'::jsonb));
  insert into public.collab_events (project_id, user_id, kind, detail)
  values (p_project, p_author, 'pushed', jsonb_build_object('revision', v_next, 'message', p_message,
                                                            'media', jsonb_array_length(coalesce(p_media, '[]'::jsonb))));
  return v_next;
end;
$$;

revoke all on function public.collab_commit_revision(uuid, integer, uuid, text, text, bigint, text, integer, jsonb)
  from public, anon, authenticated;
