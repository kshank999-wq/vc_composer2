-- VC Composer 2: rate limiting for the routes anyone on the internet can call
-- (checkout, activation, discount checks, emailed sign-in links).
--
-- Postgres rather than a separate store: the traffic is small, the table stays
-- tiny, and it adds no service to run or pay for. Consolidated from VC
-- Writer's 0010.

create table public.rate_limits (
  -- `<rule>:<hashed client address>`; never a raw address.
  key text not null,
  window_start timestamptz not null,
  count integer not null default 0,
  primary key (key, window_start)
);

alter table public.rate_limits enable row level security;
-- No policies on purpose: only the service role, from the route handlers.

-- Count one hit against a fixed window and say whether it was within the
-- limit. Atomic: the upsert is the whole decision, so two requests racing on
-- the last permitted slot cannot both be told yes.
create or replace function public.consume_rate_limit(
  p_key text,
  p_limit integer,
  p_window_seconds integer
)
returns table (allowed boolean, remaining integer, retry_after_seconds integer)
language plpgsql
security definer
set search_path = public
as $$
declare
  v_window timestamptz := to_timestamp(
    floor(extract(epoch from now()) / p_window_seconds) * p_window_seconds
  );
  v_count integer;
begin
  insert into public.rate_limits (key, window_start, count)
  values (p_key, v_window, 1)
  on conflict (key, window_start)
  do update set count = public.rate_limits.count + 1
  returning public.rate_limits.count into v_count;

  -- Sweep windows long gone, on the first hit of a new window, so the cost is
  -- paid once per window rather than once per request.
  if v_count = 1 then
    delete from public.rate_limits
    where window_start < now() - make_interval(secs => p_window_seconds * 2);
  end if;

  return query
  select
    v_count <= p_limit,
    greatest(p_limit - v_count, 0),
    case
      when v_count <= p_limit then 0
      else greatest(
        1,
        ceil(extract(epoch from (v_window + make_interval(secs => p_window_seconds) - now())))::integer
      )
    end;
end;
$$;

-- Callable by the service role only. A signed-in user must not be able to
-- burn someone else's allowance, or read it.
revoke execute on function public.consume_rate_limit(text, integer, integer) from public, anon, authenticated;
