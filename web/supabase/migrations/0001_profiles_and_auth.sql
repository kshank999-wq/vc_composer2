-- VC Composer 2: accounts.
--
-- One profile row per Supabase auth user, created by a trigger on signup so a
-- signup that drops its follow-up request can never leave an account without
-- one. auth.users holds the credentials; this table holds what the site needs:
-- the email (to find a returning buyer in one indexed query), the admin flag,
-- and when a password was last set.
--
-- Consolidated from VC Writer's 0001 (profiles only), 0004 (signup trigger),
-- 0005 (function hardening), 0007 (profile email), 0008 (admin flag and
-- is_admin()) and 0065 (password_set_at).

-- ---------------------------------------------------------------------------
-- updated_at, kept by the database rather than remembered by every caller.
-- ---------------------------------------------------------------------------

create or replace function public.touch_updated_at()
returns trigger
language plpgsql
-- Pinned so the trigger cannot be redirected by a caller-set search path.
set search_path = public
as $$
begin
  new.updated_at = now();
  return new;
end;
$$;

-- ---------------------------------------------------------------------------
-- Profiles
-- ---------------------------------------------------------------------------

create table public.profiles (
  id uuid primary key references auth.users (id) on delete cascade,
  display_name text not null default '',
  marketing_opt_in boolean not null default false,
  -- Mirror of auth.users.email, kept by the signup trigger, so fulfilment can
  -- resolve a buyer by email without paging through the auth admin API (which
  -- silently misses anyone past the first page and would strand a purchase
  -- that had already been paid for).
  email text,
  -- Grants the admin console. Set deliberately with SQL; never granted by
  -- signup, and never writable by the user (see the policies below).
  is_admin boolean not null default false,
  -- When this person last set a password. Null means no record of one, which
  -- is not proof there is none; the account page words it that way.
  password_set_at timestamptz,
  created_at timestamptz not null default now(),
  updated_at timestamptz not null default now()
);

comment on table public.profiles is 'Application-level account record; auth.users holds the credentials.';
comment on column public.profiles.email is
  'Mirror of auth.users.email, kept by the signup trigger so fulfilment can resolve a buyer by email.';
comment on column public.profiles.is_admin is
  'Grants the admin console. Set deliberately (update profiles set is_admin = true ...); never granted by signup.';
comment on column public.profiles.password_set_at is
  'When this person last set a password. Null means no record of one, which is not the same as proof there is none.';

create unique index profiles_email_key on public.profiles (lower(email)) where email is not null;

create trigger profiles_touch_updated_at before update on public.profiles
  for each row execute function public.touch_updated_at();

-- ---------------------------------------------------------------------------
-- Signup: make the profile, with the email.
-- ---------------------------------------------------------------------------

create or replace function public.handle_new_user()
returns trigger
language plpgsql
security definer
set search_path = public
as $$
begin
  insert into public.profiles (id, display_name, email)
  values (
    new.id,
    coalesce(new.raw_user_meta_data ->> 'display_name', ''),
    new.email
  )
  on conflict (id) do update set email = excluded.email;
  return new;
end;
$$;

-- Only ever invoked by the auth trigger. Nobody may call it through PostgREST.
revoke execute on function public.handle_new_user() from public, anon, authenticated;

create trigger on_auth_user_created
  after insert on auth.users
  for each row execute function public.handle_new_user();

-- ---------------------------------------------------------------------------
-- When a password was set.
--
-- Supabase writes a password hash into auth.users.encrypted_password for every
-- account at signup, including one that only ever uses an emailed link, so
-- that column cannot say whether somebody chose a password. The record is
-- made where the password is: a trigger on a real change of the hash, so no
-- client can claim a password it has not got and no screen can forget.
-- ---------------------------------------------------------------------------

create or replace function public.note_password_set()
returns trigger
language plpgsql
security definer
set search_path = public
as $$
begin
  update public.profiles
     set password_set_at = now(),
         updated_at = now()
   where id = new.id;
  return new;
end;
$$;

-- A security definer trigger function left with the default grant would be
-- callable at /rest/v1/rpc/note_password_set by anybody.
revoke execute on function public.note_password_set() from public, anon, authenticated;

-- `update of encrypted_password` with the WHEN clause, so it fires on a real
-- change and never on a sign-in or confirmation. Signing up is an INSERT, so
-- the hash written there does not stamp.
create trigger on_auth_password_set
  after update of encrypted_password on auth.users
  for each row
  when (old.encrypted_password is distinct from new.encrypted_password)
  execute function public.note_password_set();

-- ---------------------------------------------------------------------------
-- Administrators
-- ---------------------------------------------------------------------------

-- A flag on the profile rather than a role in the JWT, so granting or revoking
-- it is one visible row change with no token to re-issue. The server checks it
-- on every admin request through the service role; policies use this.
create or replace function public.is_admin()
returns boolean
language sql
stable
security definer
set search_path = public
as $$
  select coalesce((select p.is_admin from public.profiles p where p.id = (select auth.uid())), false);
$$;

-- Executable by signed-in users because policies call it as the invoking role.
-- It only says whether the caller is an admin, which they can read anyway.
revoke execute on function public.is_admin() from public, anon;
grant execute on function public.is_admin() to authenticated;

-- ---------------------------------------------------------------------------
-- Row level security
--
-- A user reads their own profile and writes none of it. VC Writer's policy was
-- `for all` on the owner's row, which would let a signed-in user set their own
-- `is_admin` (or `password_set_at`) through the REST API; nothing on this site
-- writes a profile from the browser, so here the user gets select only. The
-- signup trigger, the password trigger and the service role do every write.
-- ---------------------------------------------------------------------------

alter table public.profiles enable row level security;

create policy "users read their own profile" on public.profiles
  for select
  using (id = (select auth.uid()));
