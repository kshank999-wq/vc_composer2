-- VC Composer 2: commerce, licensing and release delivery.
--
-- Entitlement is server-authoritative. Nothing in this file is writable by a
-- signed-in user: orders, licences, builds and activations are written only by
-- the service role from the Stripe webhook and the server routes, and
-- customers get read-only visibility into their own rows. The webhook and
-- email logs are service-role only.
--
-- Consolidated from VC Writer's 0002 (commerce and releases), 0008 (admins
-- read every build), 0011 (initplan policies, FK indexes), 0062 (account
-- deletion), 0066 (desktop subscription) and 0069 (licence grants), with one
-- addition: the `preview` release channel for the free preview build.

create type public.platform as enum ('windows', 'macos');
create type public.order_status as enum ('pending', 'paid', 'refunded', 'failed', 'disputed');
create type public.license_status as enum ('active', 'suspended', 'revoked', 'expired');
-- stable: what licence holders download. preview: the free, time-limited build
-- any signed-in account may download. beta and internal: administrators only.
create type public.release_channel as enum ('stable', 'preview', 'beta', 'internal');

-- ---------------------------------------------------------------------------
-- Orders
-- ---------------------------------------------------------------------------

create table public.orders (
  id uuid primary key default gen_random_uuid(),
  -- Who bought it, until they delete their account. The order outlives the
  -- account as the financial record, with nobody attached.
  user_id uuid references public.profiles (id) on delete set null,
  status public.order_status not null default 'pending',
  -- Platform picked at checkout. Recorded for analytics and support; it does
  -- not restrict later downloads on its own.
  selected_platform public.platform,
  amount_cents integer not null check (amount_cents >= 0),
  currency text not null default 'usd',
  -- The idempotency key: a replayed webhook upserts the same order.
  stripe_checkout_session_id text unique,
  stripe_payment_intent_id text,
  stripe_customer_id text,
  paid_at timestamptz,
  created_at timestamptz not null default now(),
  updated_at timestamptz not null default now()
);

comment on column public.orders.user_id is
  'Who bought it, until they delete their account. Null means the buyer is gone and this row is the financial record alone.';

create index orders_user_idx on public.orders (user_id, created_at desc);
-- A refund or dispute finds its order by payment intent.
create index orders_payment_intent_idx on public.orders (stripe_payment_intent_id)
  where stripe_payment_intent_id is not null;

-- ---------------------------------------------------------------------------
-- Licences
-- ---------------------------------------------------------------------------

create table public.licenses (
  id uuid primary key default gen_random_uuid(),
  -- A licence is an entitlement and goes with the account.
  user_id uuid not null references public.profiles (id) on delete cascade,
  -- One licence per order. This unique constraint is what makes a retried
  -- Stripe webhook idempotent: exactly one licence per purchase.
  order_id uuid not null unique references public.orders (id) on delete restrict,
  -- VCC-XXXXX-XXXXX-XXXXX-XXXXX. Identifies the row; not a secret.
  serial text not null unique,
  status public.license_status not null default 'active',
  -- Data, not a hard-coded rule: which installers this licence may download.
  entitled_platforms public.platform[] not null default array['windows', 'macos']::public.platform[],
  max_activations integer not null default 2 check (max_activations > 0),
  -- What a subscription is paid up to. Null never lapses (a given licence).
  expires_at timestamptz,
  -- The Stripe subscription that renews this licence. A renewal extends the
  -- licence it already has rather than issuing a new serial each month.
  stripe_subscription_id text,
  -- How the licence came to be. Text with a check rather than an enum, so a
  -- value a newer build writes is not refused at the door of an older one.
  grant_kind text not null default 'purchase',
  -- Who or what a given licence is for. Empty on a bought one.
  granted_to text not null default '',
  -- The administrator who issued a given licence.
  granted_by uuid references public.profiles (id) on delete set null,
  created_at timestamptz not null default now(),
  updated_at timestamptz not null default now(),
  constraint licenses_grant_kind_check check (grant_kind in ('purchase', 'academic', 'master'))
);

comment on column public.licenses.stripe_subscription_id is
  'The Stripe subscription that renews this licence. Null for a licence that does not renew. Written only by the service role from the Stripe webhook.';
comment on column public.licenses.grant_kind is
  'purchase (by subscription), academic (a giveaway with a set number of computers, which may end on a date) or master (the owner''s own: any number of computers, never ends). Written only by the service role.';

create index licenses_user_idx on public.licenses (user_id);
-- Unique so a replayed customer.subscription.updated cannot fan out across
-- rows; partial so licences without a subscription do not collide on null.
create unique index licenses_subscription_idx on public.licenses (stripe_subscription_id)
  where stripe_subscription_id is not null;
create index licenses_expiry_idx on public.licenses (expires_at) where expires_at is not null;
create index licenses_grant_kind_idx on public.licenses (grant_kind) where grant_kind <> 'purchase';
create index licenses_granted_by_idx on public.licenses (granted_by) where granted_by is not null;

-- ---------------------------------------------------------------------------
-- Release builds
-- ---------------------------------------------------------------------------

-- Windows and macOS artifacts are published independently: shipping a new
-- Windows build must never replace the Mac artifact.
create table public.release_builds (
  id uuid primary key default gen_random_uuid(),
  platform public.platform not null,
  version text not null,
  channel public.release_channel not null default 'stable',
  minimum_os_version text not null default '',
  -- Storage object key in the private `releases` bucket, never a public URL.
  -- Downloads are authorised per request and served as short-lived signed URLs.
  artifact_key text not null,
  artifact_size_bytes bigint not null default 0,
  sha256 text not null default '',
  release_notes text not null default '',
  -- Never active on arrival: publishing is a separate, deliberate act.
  active boolean not null default false,
  published_at timestamptz,
  created_at timestamptz not null default now(),
  updated_at timestamptz not null default now(),
  constraint release_builds_version_unique unique (platform, channel, version)
);

-- At most one active build per platform and channel, so "current build" is
-- unambiguous on stable and on preview alike.
create unique index release_builds_active_idx on public.release_builds (platform, channel) where active;

-- ---------------------------------------------------------------------------
-- Device activations
-- ---------------------------------------------------------------------------

create table public.device_activations (
  id uuid primary key default gen_random_uuid(),
  license_id uuid not null references public.licenses (id) on delete cascade,
  device_fingerprint text not null,
  device_name text not null default '',
  platform public.platform not null,
  app_version text not null default '',
  activated_at timestamptz not null default now(),
  last_seen_at timestamptz,
  -- Deactivation is bookkeeping, not a delete, so support can see history.
  deactivated_at timestamptz,
  constraint device_activations_unique unique (license_id, device_fingerprint)
);

create index device_activations_license_idx on public.device_activations (license_id) where deactivated_at is null;

-- ---------------------------------------------------------------------------
-- Webhook and email logs (service role only)
-- ---------------------------------------------------------------------------

-- Stripe delivers webhooks at least once. Recording the event id before
-- processing makes a replay a no-op.
create table public.stripe_webhook_events (
  id text primary key,
  type text not null,
  received_at timestamptz not null default now(),
  processed_at timestamptz,
  error text,
  payload jsonb
);

-- Resend delivery outcomes for support, as `template@version`, without
-- storing message bodies.
create table public.email_events (
  id uuid primary key default gen_random_uuid(),
  user_id uuid references public.profiles (id) on delete set null,
  template text not null,
  provider_message_id text,
  status text not null default 'queued',
  error text,
  created_at timestamptz not null default now(),
  updated_at timestamptz not null default now()
);

create index email_events_user_idx on public.email_events (user_id);

create trigger orders_touch_updated_at before update on public.orders
  for each row execute function public.touch_updated_at();
create trigger licenses_touch_updated_at before update on public.licenses
  for each row execute function public.touch_updated_at();
create trigger release_builds_touch_updated_at before update on public.release_builds
  for each row execute function public.touch_updated_at();
create trigger email_events_touch_updated_at before update on public.email_events
  for each row execute function public.touch_updated_at();

-- ---------------------------------------------------------------------------
-- Row level security: customers read their own commerce rows and write none.
-- `(select auth.uid())` is the InitPlan form: evaluated once per statement.
-- ---------------------------------------------------------------------------

alter table public.orders enable row level security;
alter table public.licenses enable row level security;
alter table public.release_builds enable row level security;
alter table public.device_activations enable row level security;
alter table public.stripe_webhook_events enable row level security;
alter table public.email_events enable row level security;

create policy "customers read their own orders" on public.orders
  for select
  using (user_id = (select auth.uid()));

create policy "customers read their own licenses" on public.licenses
  for select
  using (user_id = (select auth.uid()));

create policy "customers read their own activations" on public.device_activations
  for select
  using (
    exists (
      select 1 from public.licenses l
      where l.id = device_activations.license_id
        and l.user_id = (select auth.uid())
    )
  );

-- Signed-in users may see which customer-facing builds are live (version,
-- notes, minimum OS). The artifact itself still requires a server-issued
-- signed URL, which checks the licence for stable.
create policy "signed-in users read active customer builds" on public.release_builds
  for select to authenticated
  using (active and channel in ('stable', 'preview'));

-- Admins can see every build, including retired, beta and internal ones.
-- Writes stay service-role only even for admins: publishing goes through the
-- server, where the storage object and the row are kept consistent.
create policy "admins read every release build" on public.release_builds
  for select to authenticated
  using ((select public.is_admin()));

-- stripe_webhook_events and email_events get no policies at all: service role only.
