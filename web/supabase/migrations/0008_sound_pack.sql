-- The built-in sounds VC Composer downloads on first launch (Studio > Built-in
-- Sounds): a public, read-only bucket. Anyone may read it (the files are
-- open-licensed: Salamander Grand Piano, CC-BY 3.0; MuseScore General, MIT);
-- only the service role (the sound-pack workflow) writes, as for any bucket
-- without insert/update policies.
insert into storage.buckets (id, name, public, file_size_limit)
values ('sounds', 'sounds', true, 104857600)
on conflict (id) do update set public = true, file_size_limit = excluded.file_size_limit;
