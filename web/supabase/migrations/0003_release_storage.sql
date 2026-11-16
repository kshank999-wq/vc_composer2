-- VC Composer: the private bucket installers live in.
--
-- Consolidated from VC Writer's 0004 (storage), keeping only `releases`: this
-- site stores no project assets and no voice captures.
--
-- The bucket is private and deliberately gets no storage policy at all:
-- installers are readable only through short-lived signed URLs minted
-- server-side after the download gate (src/lib/downloads.ts), and written only
-- by the service role (the admin release manager and
-- scripts/publish-release.mjs).
--
-- If RELEASE_BUCKET is set to another name, create that bucket the same way.

insert into storage.buckets (id, name, public)
values ('releases', 'releases', false)
on conflict (id) do nothing;
