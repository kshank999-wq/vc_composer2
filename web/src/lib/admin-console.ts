import { formatMoney } from '@/domain';
import { adminClient } from './supabase';

/**
 * The administration console's reads.
 *
 * Split in two on purpose. The functions at the top take plain rows and
 * return plain numbers, and the tests pin each definition. The loaders at the
 * bottom fetch bounded row sets through the service-role client and hand them
 * up. A number on the dashboard is therefore reproducible from the rows it was
 * computed over.
 *
 * The row types below are the whole surface the console can see.
 */

// ---------------------------------------------------------------------------
// Rows, as Supabase returns them
// ---------------------------------------------------------------------------

export interface OrderRow {
  id: string;
  /**
   * Null once the buyer has deleted their account: the row stays as the
   * financial record and stops being about a person. The orders list shows it
   * with no email, which is what it now is.
   */
  user_id: string | null;
  status: 'pending' | 'paid' | 'refunded' | 'failed' | 'disputed';
  amount_cents: number;
  currency: string;
  selected_platform: 'windows' | 'macos' | null;
  stripe_checkout_session_id: string | null;
  stripe_payment_intent_id: string | null;
  paid_at: string | null;
  created_at: string;
  updated_at: string;
}

export interface LicenseRow {
  id: string;
  user_id: string;
  status: 'active' | 'suspended' | 'revoked' | 'expired';
  created_at: string;
}

export interface ActivationRow {
  id: string;
  license_id: string;
  platform: 'windows' | 'macos';
  deactivated_at: string | null;
}

export interface EmailEventRow {
  id: string;
  user_id: string | null;
  template: string;
  status: string;
  error: string | null;
  provider_message_id: string | null;
  created_at: string;
}

export interface BuildRow {
  platform: 'windows' | 'macos';
  version: string;
  channel: 'stable' | 'preview' | 'beta' | 'internal';
  active: boolean;
  published_at: string | null;
}

export interface ProfileRow {
  id: string;
  email: string | null;
  display_name: string;
  is_admin: boolean;
  created_at: string;
}

// ---------------------------------------------------------------------------
// Pure: the definitions
// ---------------------------------------------------------------------------

const DAY = 24 * 60 * 60 * 1000;

const since = (now: Date, days: number): number => now.getTime() - days * DAY;

const at = (value: string | null): number => (value ? new Date(value).getTime() : Number.NaN);

/** Paid revenue in a trailing window, per currency, in minor units. */
export const revenueInWindow = (
  orders: readonly OrderRow[],
  now: Date,
  days: number,
): Record<string, number> => {
  const floor = since(now, days);
  const totals: Record<string, number> = {};
  for (const order of orders) {
    if (order.status !== 'paid') continue;
    const paid = at(order.paid_at);
    if (Number.isNaN(paid) || paid < floor || paid > now.getTime()) continue;
    const currency = order.currency.toLowerCase();
    totals[currency] = (totals[currency] ?? 0) + order.amount_cents;
  }
  return totals;
};

export interface DashboardMetrics {
  revenue30: Record<string, number>;
  revenue7: Record<string, number>;
  paidOrders30: number;
  refundsAndDisputes30: number;
  activeLicenses: number;
  activeDevices: { windows: number; macos: number };
  failedEmails7: number;
  /** The live stable build per platform: what licence holders download. */
  currentBuilds: { windows: string | null; macos: string | null };
  /** The live preview build per platform: what any signed-in account downloads. */
  previewBuilds: { windows: string | null; macos: string | null };
}

export interface DashboardRows {
  /** Orders created or updated in the last 30 days — the loader bounds this. */
  orders: readonly OrderRow[];
  licenses: readonly LicenseRow[];
  activations: readonly ActivationRow[];
  /** Email events from the last 7 days. */
  emails: readonly EmailEventRow[];
  builds: readonly BuildRow[];
}

/** Every tile on the dashboard, from rows. */
export const dashboardFromRows = (rows: DashboardRows, now: Date): DashboardMetrics => {
  const floor30 = since(now, 30);
  const floor7 = since(now, 7);

  const paidOrders30 = rows.orders.filter((order) => {
    const paid = at(order.paid_at);
    return order.status === 'paid' && !Number.isNaN(paid) && paid >= floor30;
  }).length;

  const refundsAndDisputes30 = rows.orders.filter(
    (order) =>
      (order.status === 'refunded' || order.status === 'disputed') && at(order.updated_at) >= floor30,
  ).length;

  const activeDevices = { windows: 0, macos: 0 };
  for (const activation of rows.activations) {
    if (activation.deactivated_at === null) activeDevices[activation.platform] += 1;
  }

  const currentBuilds: DashboardMetrics['currentBuilds'] = { windows: null, macos: null };
  const previewBuilds: DashboardMetrics['previewBuilds'] = { windows: null, macos: null };
  for (const build of rows.builds) {
    if (!build.active) continue;
    if (build.channel === 'stable') currentBuilds[build.platform] = build.version;
    if (build.channel === 'preview') previewBuilds[build.platform] = build.version;
  }

  return {
    revenue30: revenueInWindow(rows.orders, now, 30),
    revenue7: revenueInWindow(rows.orders, now, 7),
    paidOrders30,
    refundsAndDisputes30,
    activeLicenses: rows.licenses.filter((license) => license.status === 'active').length,
    activeDevices,
    failedEmails7: rows.emails.filter((email) => email.status === 'failed' && at(email.created_at) >= floor7).length,
    currentBuilds,
    previewBuilds,
  };
};

export interface CustomerSummary {
  id: string;
  email: string | null;
  displayName: string;
  isAdmin: boolean;
  createdAt: string;
  orders: number;
  /** The licence that matters: active beats suspended beats revoked beats expired. */
  licenseStatus: LicenseRow['status'] | null;
  activeDevices: number;
}

const LICENSE_PRECEDENCE: LicenseRow['status'][] = ['active', 'suspended', 'revoked', 'expired'];

/** One row per profile, with the counts the list shows. */
export const customerSummaries = (
  profiles: readonly ProfileRow[],
  orders: readonly OrderRow[],
  licenses: readonly LicenseRow[],
  activations: readonly ActivationRow[],
): CustomerSummary[] => {
  const ordersByUser = new Map<string, number>();
  for (const order of orders) {
    if (!order.user_id) continue;
    ordersByUser.set(order.user_id, (ordersByUser.get(order.user_id) ?? 0) + 1);
  }

  const licenseOwner = new Map<string, string>();
  const licensesByUser = new Map<string, LicenseRow[]>();
  for (const license of licenses) {
    licenseOwner.set(license.id, license.user_id);
    licensesByUser.set(license.user_id, [...(licensesByUser.get(license.user_id) ?? []), license]);
  }

  const devicesByUser = new Map<string, number>();
  for (const activation of activations) {
    if (activation.deactivated_at !== null) continue;
    const owner = licenseOwner.get(activation.license_id);
    if (owner) devicesByUser.set(owner, (devicesByUser.get(owner) ?? 0) + 1);
  }

  return profiles.map((profile) => {
    const owned = licensesByUser.get(profile.id) ?? [];
    const licenseStatus =
      LICENSE_PRECEDENCE.find((status) => owned.some((license) => license.status === status)) ?? null;
    return {
      id: profile.id,
      email: profile.email,
      displayName: profile.display_name,
      isAdmin: profile.is_admin,
      createdAt: profile.created_at,
      orders: ordersByUser.get(profile.id) ?? 0,
      licenseStatus,
      activeDevices: devicesByUser.get(profile.id) ?? 0,
    };
  });
};

/** The domain's, re-exported: there is one way to write a figure down. */
export { formatMoney };

/** The dashboard revenue tile: one line per currency, or "—" for none. */
export const formatRevenue = (totals: Record<string, number>): string => {
  const entries = Object.entries(totals);
  if (entries.length === 0) return '—';
  return entries.map(([currency, cents]) => formatMoney(cents, currency)).join(' · ');
};

/**
 * Where to open this order in Stripe. The payment when there is one, else a
 * search for the session — Stripe's search understands either id. Test-mode
 * objects live under /test/, and their ids say so.
 */
export const stripeUrlFor = (order: Pick<OrderRow, 'stripe_payment_intent_id' | 'stripe_checkout_session_id'>): string | null => {
  const id = order.stripe_payment_intent_id ?? order.stripe_checkout_session_id;
  if (!id) return null;
  const mode = id.includes('_test_') ? '/test' : '';
  return order.stripe_payment_intent_id
    ? `https://dashboard.stripe.com${mode}/payments/${encodeURIComponent(id)}`
    : `https://dashboard.stripe.com${mode}/search?query=${encodeURIComponent(id)}`;
};

// ---------------------------------------------------------------------------
// Loaders: bounded reads through the service role
// ---------------------------------------------------------------------------

const iso = (ms: number): string => new Date(ms).toISOString();

export interface DashboardData {
  metrics: DashboardMetrics;
  recentOrders: Array<OrderRow & { email: string | null }>;
  recentFailedEmails: Array<EmailEventRow & { email: string | null }>;
}

const emailsFor = async (userIds: readonly (string | null)[]): Promise<Map<string, string | null>> => {
  const ids = [...new Set(userIds.filter((id): id is string => id !== null))];
  if (ids.length === 0) return new Map();
  const { data } = await adminClient().from('profiles').select('id, email').in('id', ids);
  return new Map((data ?? []).map((row) => [row.id as string, (row.email as string | null) ?? null]));
};

export const loadDashboard = async (now = new Date()): Promise<DashboardData> => {
  const client = adminClient();
  const floor30 = iso(since(now, 30));
  const floor7 = iso(since(now, 7));

  const [orders, licenses, activations, emails, builds, recentOrders, recentFailed] = await Promise.all([
    client.from('orders').select('*').or(`paid_at.gte.${floor30},updated_at.gte.${floor30}`).limit(5000),
    client.from('licenses').select('id, user_id, status, created_at').limit(50_000),
    client.from('device_activations').select('id, license_id, platform, deactivated_at').is('deactivated_at', null).limit(50_000),
    client.from('email_events').select('id, user_id, template, status, error, provider_message_id, created_at').gte('created_at', floor7).limit(5000),
    client.from('release_builds').select('platform, version, channel, active, published_at').eq('active', true),
    client.from('orders').select('*').order('created_at', { ascending: false }).limit(10),
    client
      .from('email_events')
      .select('id, user_id, template, status, error, provider_message_id, created_at')
      .eq('status', 'failed')
      .order('created_at', { ascending: false })
      .limit(10),
  ]);

  const metrics = dashboardFromRows(
    {
      orders: (orders.data ?? []) as OrderRow[],
      licenses: (licenses.data ?? []) as LicenseRow[],
      activations: (activations.data ?? []) as ActivationRow[],
      emails: (emails.data ?? []) as EmailEventRow[],
      builds: (builds.data ?? []) as BuildRow[],
    },
    now,
  );

  const recentOrderRows = (recentOrders.data ?? []) as OrderRow[];
  const recentFailedRows = (recentFailed.data ?? []) as EmailEventRow[];
  const emailMap = await emailsFor([...recentOrderRows.map((o) => o.user_id), ...recentFailedRows.map((e) => e.user_id)]);

  return {
    metrics,
    recentOrders: recentOrderRows.map((order) => ({
      ...order,
      email: order.user_id ? (emailMap.get(order.user_id) ?? null) : null,
    })),
    recentFailedEmails: recentFailedRows.map((event) => ({
      ...event,
      email: event.user_id ? (emailMap.get(event.user_id) ?? null) : null,
    })),
  };
};

export const loadCustomers = async (options: { query?: string; limit?: number } = {}): Promise<CustomerSummary[]> => {
  const client = adminClient();
  const limit = options.limit ?? 100;
  const query = options.query?.trim();

  let profiles = client.from('profiles').select('id, email, display_name, is_admin, created_at').order('created_at', { ascending: false }).limit(limit);
  if (query) {
    // PostgREST's `or` takes a comma-separated filter list; commas in the
    // term would split it, so they are dropped rather than escaped.
    const term = `%${query.replace(/[,%]/g, '')}%`;
    profiles = profiles.or(`email.ilike.${term},display_name.ilike.${term}`);
  }

  const { data: profileRows } = await profiles;
  const rows = (profileRows ?? []) as ProfileRow[];
  if (rows.length === 0) return [];

  const ids = rows.map((row) => row.id);
  const [orders, licenses] = await Promise.all([
    client.from('orders').select('id, user_id, status, amount_cents, currency, selected_platform, stripe_checkout_session_id, stripe_payment_intent_id, paid_at, created_at, updated_at').in('user_id', ids),
    client.from('licenses').select('id, user_id, status, created_at').in('user_id', ids),
  ]);
  const licenseRows = (licenses.data ?? []) as LicenseRow[];
  const { data: activationRows } = licenseRows.length
    ? await client
        .from('device_activations')
        .select('id, license_id, platform, deactivated_at')
        .in('license_id', licenseRows.map((license) => license.id))
    : { data: [] };

  return customerSummaries(rows, (orders.data ?? []) as OrderRow[], licenseRows, (activationRows ?? []) as ActivationRow[]);
};

export type OrderWithEmail = OrderRow & { email: string | null };

export const loadOrders = async (options: { status?: OrderRow['status']; limit?: number } = {}): Promise<OrderWithEmail[]> => {
  const client = adminClient();
  let query = client.from('orders').select('*').order('created_at', { ascending: false }).limit(options.limit ?? 200);
  if (options.status) query = query.eq('status', options.status);
  const { data } = await query;
  const rows = (data ?? []) as OrderRow[];
  const emailMap = await emailsFor(rows.map((row) => row.user_id));
  return rows.map((row) => ({ ...row, email: row.user_id ? (emailMap.get(row.user_id) ?? null) : null }));
};

export type EmailEventWithEmail = EmailEventRow & { email: string | null };

export const loadEmailEvents = async (options: { status?: string; limit?: number } = {}): Promise<EmailEventWithEmail[]> => {
  const client = adminClient();
  let query = client
    .from('email_events')
    .select('id, user_id, template, status, error, provider_message_id, created_at')
    .order('created_at', { ascending: false })
    .limit(options.limit ?? 200);
  if (options.status) query = query.eq('status', options.status);
  const { data } = await query;
  const rows = (data ?? []) as EmailEventRow[];
  const emailMap = await emailsFor(rows.map((row) => row.user_id));
  return rows.map((row) => ({ ...row, email: row.user_id ? (emailMap.get(row.user_id) ?? null) : null }));
};
